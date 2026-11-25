#include "FreeRTOS.h"
#include "bsp_board.h"
#include "dev_husb238.h"
#include "dev_ina226.h"
#include "fault_blackbox.h"
#include "main.h"
#include "pd_sink_policy.h"
#include "power_pid.h"
#include "prot_ovp_ocp.h"
#include "semphr.h"
#include "task.h"
#include "tft_hud.h"
#include <string.h>

#define APP_SHUNT_MOHM       100U   /* R100 (0.1 ohm) on INA219/INA226 module */
#define APP_HW_ALERT_OCP_MA  780U   /* Hardware ALE limit inside 819mA range */
#define APP_SIM_LOAD_OHM     35U    /* 10+10+15 ohm golden resistor series */

/* Set to 1 ONLY for offline desk simulation without INA219/INA226 attached;
 * default 0 keeps PA0 PWM forced to 0% whenever the sensor is absent/disconnected. */
#ifndef APP_ALLOW_SIM_PWM_OUTPUT
#define APP_ALLOW_SIM_PWM_OUTPUT 0
#endif

#define CTRL_TASK_STACK_WORDS 160U
#define PD_TASK_STACK_WORDS   160U
#define LOG_TASK_STACK_WORDS  192U

#define WDG_BIT_CTRL  (1U << 0)
#define WDG_BIT_PD    (1U << 1)
#define WDG_BIT_LOG   (1U << 2)
#define WDG_ALL_BITS  (WDG_BIT_CTRL | WDG_BIT_PD | WDG_BIT_LOG)

static StaticTask_t s_ctrl_tcb;
static StackType_t s_ctrl_stack[CTRL_TASK_STACK_WORDS];
static TaskHandle_t s_ctrl_handle;

static StaticTask_t s_pd_tcb;
static StackType_t s_pd_stack[PD_TASK_STACK_WORDS];
static TaskHandle_t s_pd_handle;

static StaticTask_t s_log_tcb;
static StackType_t s_log_stack[LOG_TASK_STACK_WORDS];
static TaskHandle_t s_log_handle;

static StaticSemaphore_t s_state_mutex_buf;
static SemaphoreHandle_t s_state_mutex;

static StaticSemaphore_t s_i2c_mutex_buf;
static SemaphoreHandle_t s_i2c_mutex;

static StaticSemaphore_t s_bbox_mutex_buf;
static SemaphoreHandle_t s_bbox_mutex;

static bbox_ctx_t s_bbox;
static volatile uint8_t s_wdg_alive_mask;
static volatile bool s_wdg_stall_test;

typedef struct {
    uint16_t desired_pd_mv;
    uint32_t target_power_mw;
    uint16_t pd_mv;
    uint16_t vbus_mv;
    uint16_t ibus_ma;
    uint32_t meas_power_mw;
    uint16_t pid_duty_permille;
    uint16_t vdda_mv;
    int16_t mcu_temp_c;
    uint8_t pd_state;
    uint8_t prot_fault;
    uint8_t sim_ovp_ticks;
    uint8_t sim_otp_ticks;
    bool clear_latch_req;
    bool auto_pd_enable;
    bool manual_v_pending;
    uint16_t auto_boost_notice_mv;
    uint8_t safe_5v_notice;
    bool husb_online;
    bool ina_online;
    bool pending_bb_valid;
    bbox_record_t pending_bb_rec;
} app_shared_t;

static app_shared_t s_shared = {
    .desired_pd_mv = 5000U,
    .target_power_mw = 0U,
    .pd_mv = 5000U,
    .auto_pd_enable = true
};

static uint8_t wdg_mark_alive(uint8_t bit) {
    uint8_t mask;
    taskENTER_CRITICAL();
    s_wdg_alive_mask = (uint8_t)(s_wdg_alive_mask | bit);
    mask = s_wdg_alive_mask;
    taskEXIT_CRITICAL();
    return mask;
}

static uint8_t prot_fault_to_bbox_evt(prot_fault_t fault) {
    switch (fault) {
    case PROT_FAULT_UVP:
        return (uint8_t)BBOX_EVT_UVP;
    case PROT_FAULT_OVP:
        return (uint8_t)BBOX_EVT_OVP;
    case PROT_FAULT_OCP_SOFT:
        return (uint8_t)BBOX_EVT_OCP_SOFT;
    case PROT_FAULT_OCP_HARD:
        return (uint8_t)BBOX_EVT_OCP_HARD;
    case PROT_FAULT_OTP:
        return (uint8_t)BBOX_EVT_OTP;
    case PROT_FAULT_SENSOR_LOSS:
        return (uint8_t)BBOX_EVT_SENSOR_LOSS;
    default:
        return (uint8_t)BBOX_EVT_NONE;
    }
}

static void print_reset_cause_str(uint8_t mask) {
    if (mask == 0U) {
        bsp_uart_puts("UNK");
        return;
    }
    if ((mask & BSP_RST_IWDG) != 0U) {
        bsp_uart_puts("IWDG ");
    }
    if ((mask & BSP_RST_WWDG) != 0U) {
        bsp_uart_puts("WWDG ");
    }
    if ((mask & BSP_RST_SFT) != 0U) {
        bsp_uart_puts("SFT ");
    }
    if ((mask & BSP_RST_PWR) != 0U) {
        bsp_uart_puts("PWR ");
    }
    if ((mask & BSP_RST_PIN) != 0U) {
        bsp_uart_puts("PIN");
    }
}

static void print_blackbox_dump(void) {
    bbox_ctx_t snap;
    memset(&snap, 0, sizeof(snap));

    if (xSemaphoreTake(s_bbox_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
        snap = s_bbox;
        xSemaphoreGive(s_bbox_mutex);
    }

    bsp_uart_puts("[BBOX] SEQ=");
    bsp_uart_put_u32(snap.state.seq);
    bsp_uart_puts(" SLOT=");
    bsp_uart_put_u32(snap.state.active_slot);
    bsp_uart_puts(" BOOT=");
    bsp_uart_put_u32(snap.state.boot_count);
    bsp_uart_puts(" IWDG_RST=");
    bsp_uart_put_u32(snap.state.iwdg_reset_count);
    bsp_uart_puts(" TOTAL=");
    bsp_uart_put_u32(snap.state.fault_total_count);
    bsp_uart_puts(" VALID=");
    bsp_uart_put_u32(snap.state.valid_count);
    bsp_uart_puts(" LAST_RST=");
    print_reset_cause_str(snap.state.last_reset_cause);
    bsp_uart_puts("\r\n");

    for (uint8_t i = 0U; i < snap.state.valid_count; i++) {
        bbox_record_t rec;
        if (bbox_get_latest(&snap, i, &rec)) {
            bsp_uart_puts("  #");
            bsp_uart_put_u32(i);
            bsp_uart_puts(" T=");
            bsp_uart_put_u32(rec.uptime_ms);
            bsp_uart_puts("ms EVT=");
            bsp_uart_puts(bbox_event_name(rec.event_type));
            bsp_uart_puts(" V=");
            bsp_uart_put_u32(rec.vbus_mv);
            bsp_uart_puts("mV I=");
            bsp_uart_put_u32(rec.ibus_ma);
            bsp_uart_puts("mA P=");
            bsp_uart_put_u32(rec.power_mw);
            bsp_uart_puts("mW D=");
            bsp_uart_put_u32(rec.duty_permille);
            bsp_uart_puts("pm FLG=");
            bsp_uart_put_u32(rec.flags);
            bsp_uart_puts(" EXT=");
            bsp_uart_put_u32(rec.extra);
            bsp_uart_puts("\r\n");
        }
    }
}

void vApplicationStackOverflowHook(TaskHandle_t xTask, char *pcTaskName) {
    (void)xTask;
    (void)pcTaskName;
    __disable_irq();
    if ((RCC->APBENR2 & RCC_APBENR2_TIM1EN) != 0U) {
        TIM1->CCR3 = 0U;
        TIM1->BDTR = (TIM1->BDTR & ~TIM_BDTR_MOE) | TIM_BDTR_OSSI;
    }
    bsp_crumb_update((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                     0U, 0U, 0U, (uint8_t)s_wdg_alive_mask, 0x80U);
    bsp_uart_puts("\r\n[ERR] RTOS_STACK_OVERFLOW\r\n");
    for (;;) {
    }
}

void APP_ErrorHandler(void) {
    __disable_irq();
    if ((RCC->APBENR2 & RCC_APBENR2_TIM1EN) != 0U) {
        TIM1->CCR3 = 0U;
        TIM1->BDTR = (TIM1->BDTR & ~TIM_BDTR_MOE) | TIM_BDTR_OSSI;
    }
    while (1) {
    }
}

static uint32_t parse_u32_decimal(const char *s) {
    uint32_t val = 0U;
    while (*s == ' ') {
        s++;
    }
    while ((*s >= '0') && (*s <= '9')) {
        val = (val * 10U) + (uint32_t)(*s - '0');
        s++;
    }
    return val;
}

static void handle_uart_cmd(const char *cmd) {
    char op = cmd[0];
    if ((op >= 'a') && (op <= 'z')) {
        op = (char)(op - ('a' - 'A'));
    }

    if (op == 'V') {
        uint32_t mv = parse_u32_decimal(&cmd[1]);
        if ((mv >= 5000U) && (mv <= 20000U)) {
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
                s_shared.desired_pd_mv = (uint16_t)mv;
                s_shared.manual_v_pending = true;
                xSemaphoreGive(s_state_mutex);
            }
            bsp_uart_puts("[CMD] SET_PD=");
            bsp_uart_put_u32(mv);
            bsp_uart_puts("mV\r\n");
        }
    } else if (op == 'P') {
        uint32_t mw = parse_u32_decimal(&cmd[1]);
        if (mw <= 20000U) {
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
                s_shared.target_power_mw = mw;
                xSemaphoreGive(s_state_mutex);
            }
            bsp_uart_puts("[CMD] SET_PWR=");
            bsp_uart_put_u32(mw);
            bsp_uart_puts("mW\r\n");
        }
    } else if (op == 'C') {
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
            s_shared.clear_latch_req = true;
            xSemaphoreGive(s_state_mutex);
        }
        bsp_uart_puts("[CMD] CLEAR_LATCH_REQ\r\n");
    } else if (op == 'L') {
        print_blackbox_dump();
    } else if (op == 'E') {
        bool ok = false;
        if (xSemaphoreTake(s_bbox_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
            ok = bbox_clear(&s_bbox, bsp_bbox_flash_ops());
            xSemaphoreGive(s_bbox_mutex);
        }
        bsp_uart_puts(ok ? "[CMD] BBOX_CLEARED\r\n" : "[ERR] BBOX_CLEAR_FAIL\r\n");
    } else if (op == 'T') {
        bool live = true;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
            live = s_shared.ina_online;
            if (!live) {
                s_shared.sim_ovp_ticks = 4U;
            }
            xSemaphoreGive(s_state_mutex);
        }
        bsp_uart_puts(live ? "[ERR] TEST_BLOCKED_IN_LIVE\r\n"
                           : "[TEST] SIM_OVP_4TICKS_INJECTED\r\n");
    } else if (op == 'O') {
        bool live = true;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
            live = s_shared.ina_online;
            if (!live) {
                s_shared.sim_otp_ticks = 4U;
            }
            xSemaphoreGive(s_state_mutex);
        }
        bsp_uart_puts(live ? "[ERR] TEST_BLOCKED_IN_LIVE\r\n"
                           : "[TEST] SIM_OTP_90C_4TICKS_INJECTED\r\n");
    } else if (op == 'A') {
        bool now_en = true;
        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
            s_shared.auto_pd_enable = !s_shared.auto_pd_enable;
            now_en = s_shared.auto_pd_enable;
            xSemaphoreGive(s_state_mutex);
        }
        bsp_uart_puts(now_en ? "[CMD] AUTO_PD=ON\r\n" : "[CMD] AUTO_PD=OFF\r\n");
    } else if (op == 'W') {
        s_wdg_stall_test = true;
        bsp_uart_puts("[TEST] WDG_STALL_INJECTED (~2s reset)\r\n");
    } else if (op == 'H') {
        bsp_uart_puts("[HELP] V <mv> | P <mw> | A (auto-pd) | C (clr) | L (bbox) | E (erase) | T/O/W\r\n");
    }
}

static void task_pd_entry(void *arg) {
    (void)arg;
    const i2c_bus_ops_t *bus = bsp_i2c1_bus();
    pd_sink_fsm_t pd_fsm;
    pd_auto_boost_t auto_boost;
    pd_safe_5v_t safe_5v;
    pd_caps_t mock_caps = {0};
    TickType_t last_wake = xTaskGetTickCount();

    mock_caps.attached = true;
    mock_caps.supported[PD_PROFILE_5V] = true;
    mock_caps.max_current_ma[PD_PROFILE_5V] = 3000U;
    mock_caps.supported[PD_PROFILE_9V] = true;
    mock_caps.max_current_ma[PD_PROFILE_9V] = 3000U;
    mock_caps.supported[PD_PROFILE_12V] = true;
    mock_caps.max_current_ma[PD_PROFILE_12V] = 3000U;
    mock_caps.supported[PD_PROFILE_20V] = true;
    mock_caps.max_current_ma[PD_PROFILE_20V] = 3250U;

    pd_sink_init(&pd_fsm, 5000U);
    pd_auto_boost_init(&auto_boost);
    pd_safe_5v_init(&safe_5v);

    for (;;) {
        uint16_t desired_mv = 5000U;
        uint16_t live_vbus_mv = 0U;
        uint16_t ibus_ma = 0U;
        uint16_t duty_pm = 0U;
        uint32_t target_mw = 0U;
        uint32_t meas_mw = 0U;
        uint8_t prot_fault = 0U;
        bool auto_pd_en = true;
        bool manual_v = false;
        bool ina_online = false;

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5U)) != pdTRUE) {
            (void)wdg_mark_alive(WDG_BIT_PD);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50U));
            continue;
        }
        desired_mv = s_shared.desired_pd_mv;
        live_vbus_mv = s_shared.vbus_mv;
        ibus_ma = s_shared.ibus_ma;
        duty_pm = s_shared.pid_duty_permille;
        target_mw = s_shared.target_power_mw;
        meas_mw = s_shared.meas_power_mw;
        prot_fault = s_shared.prot_fault;
        auto_pd_en = s_shared.auto_pd_enable;
        manual_v = s_shared.manual_v_pending;
        s_shared.manual_v_pending = false;
        ina_online = s_shared.ina_online;
        xSemaphoreGive(s_state_mutex);

        if (manual_v) {
            pd_safe_5v_notify_manual_v(&safe_5v);
            pd_auto_boost_init(&auto_boost);
        }

        pd_caps_t hw_caps = {0};
        uint16_t contract_mv = 0U;
        bool husb_ok = false;

        if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(15U)) == pdTRUE) {
            husb_ok = husb238_read_caps(bus, &hw_caps, &contract_mv);
            xSemaphoreGive(s_i2c_mutex);
        }

        pd_sink_input_t in;
        pd_sink_output_t out;
        const pd_caps_t *active_caps;
        memset(&in, 0, sizeof(in));
        memset(&out, 0, sizeof(out));

        if (husb_ok) {
            active_caps = &hw_caps;
            in.link_ok = true;
            in.attached = hw_caps.attached;
            in.vbus_mv = ina_online ? live_vbus_mv : contract_mv;
            in.desired_mv = desired_mv;
            in.caps = &hw_caps;
            pd_sink_step(&pd_fsm, &in, &out);

            if (out.should_write_request) {
                if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(15U)) == pdTRUE) {
                    (void)husb238_request_profile(bus, out.request_profile);
                    xSemaphoreGive(s_i2c_mutex);
                }
            }
        } else {
            /* Safe desk simulation mode before HUSB238 hardware is attached. */
            active_caps = &mock_caps;
            in.link_ok = true;
            in.attached = true;
            in.vbus_mv = pd_sink_profile_mv(pd_sink_select_profile(&mock_caps, desired_mv));
            in.desired_mv = desired_mv;
            in.caps = &mock_caps;
            pd_sink_step(&pd_fsm, &in, &out);
        }

        /* vSafe5V fallback: drop to 5V immediately on latched fault, or 500ms after P=0 stop. */
        pd_safe_drop_reason_t safe_drop;
        uint16_t rail_mv = (out.active_mv > desired_mv) ? out.active_mv : desired_mv;
        safe_drop = pd_safe_5v_step(&safe_5v, rail_mv, target_mw, (prot_fault != 0U));

        /* Auto step-up one supported PD voltage gear if current rail saturates below target power. */
        pd_auto_boost_input_t ab_in;
        uint16_t boost_mv = 0U;
        if (safe_drop == PD_SAFE_DROP_NONE) {
            ab_in.enabled = auto_pd_en;
            ab_in.power_ready = out.power_ready;
            ab_in.fault_active = (prot_fault != 0U);
            ab_in.current_mv = out.active_mv;
            ab_in.ibus_ma = ibus_ma;
            ab_in.duty_permille = duty_pm;
            ab_in.target_power_mw = target_mw;
            ab_in.meas_power_mw = meas_mw;
            ab_in.caps = active_caps;
            boost_mv = pd_auto_boost_step(&auto_boost, &ab_in);
        } else {
            pd_auto_boost_init(&auto_boost);
        }

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5U)) == pdTRUE) {
            s_shared.husb_online = husb_ok;
            s_shared.pd_mv = out.active_mv;
            s_shared.pd_state = (uint8_t)out.state;
            if ((safe_drop != PD_SAFE_DROP_NONE) && (s_shared.desired_pd_mv == desired_mv)) {
                s_shared.desired_pd_mv = 5000U;
                s_shared.safe_5v_notice = (uint8_t)safe_drop;
            } else if ((boost_mv > 0U) && (s_shared.desired_pd_mv == desired_mv)) {
                s_shared.desired_pd_mv = boost_mv;
                s_shared.auto_boost_notice_mv = boost_mv;
            }
            xSemaphoreGive(s_state_mutex);
        }

        (void)wdg_mark_alive(WDG_BIT_PD);
        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(50U));
    }
}

static void task_ctrl_entry(void *arg) {
    (void)arg;
    const i2c_bus_ops_t *bus = bsp_i2c1_bus();
    power_pid_t pid;
    prot_fsm_t prot;
    const power_pid_cfg_t pid_cfg = {
        .kp_milli = 25,
        .ki_milli = 15,
        .kd_milli = 2,
        .ramp_step_mw = 200U,
        .max_duty_permille = 950U
    };
    const prot_cfg_t prot_cfg = {
        .uvp_mv = 4300U,
        .uvp_hyst_mv = 400U,
        .ovp_mv = 21500U,
        .ovp_hyst_mv = 1000U,
        .ocp_soft_ma = 750U,
        .ocp_hard_ma = 810U,
        .uvp_debounce_ticks = 3U,
        .ovp_debounce_ticks = 3U,
        .ocp_debounce_ticks = 2U,
        .otp_derate_c = 70,
        .otp_shutdown_c = 85,
        .otp_hyst_c = 5,
        .otp_debounce_ticks = 3U,
        .sensor_loss_debounce_ticks = 2U
    };
    bool ina_configured = false;
    bool prev_ina_ok = false;
    bool husb_online = false;
    uint32_t sim_meas_mw = 0U;
    uint16_t last_duty = 0U;
    TickType_t last_wake = xTaskGetTickCount();

    power_pid_init(&pid, &pid_cfg);
    prot_init(&prot, &prot_cfg);

    for (;;) {
        uint32_t target_mw = 0U;
        uint16_t pd_mv = 5000U;
        uint8_t sim_ovp_ticks = 0U;
        uint8_t sim_otp_ticks = 0U;
        bool clear_req = false;

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5U)) != pdTRUE) {
            /* Fail-safe: never compute PID or protection from uninitialized defaults. */
            bsp_pwm_set_duty_permille(0U);
            (void)wdg_mark_alive(WDG_BIT_CTRL);
            vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20U));
            continue;
        }
        target_mw = s_shared.target_power_mw;
        pd_mv = s_shared.pd_mv;
        husb_online = s_shared.husb_online;
        clear_req = s_shared.clear_latch_req;
        s_shared.clear_latch_req = false;
        if (s_shared.sim_ovp_ticks > 0U) {
            sim_ovp_ticks = s_shared.sim_ovp_ticks;
            s_shared.sim_ovp_ticks--;
        }
        if (s_shared.sim_otp_ticks > 0U) {
            sim_otp_ticks = s_shared.sim_otp_ticks;
            s_shared.sim_otp_ticks--;
        }
        xSemaphoreGive(s_state_mutex);

        ina226_sample_t sample;
        memset(&sample, 0, sizeof(sample));
        bool ina_ok = false;

        if (xSemaphoreTake(s_i2c_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
            if (!ina_configured) {
                ina_configured = ina226_init(bus, APP_SHUNT_MOHM, APP_HW_ALERT_OCP_MA);
            }
            if (ina_configured) {
                ina_ok = ina226_read_sample(bus, APP_SHUNT_MOHM, &sample);
                if (!ina_ok) {
                    ina_configured = false;
                } else if (clear_req) {
                    (void)ina226_clear_alert(bus);
                    bsp_alert_clear_latch();
                }
            }
            xSemaphoreGive(s_i2c_mutex);
        }

        uint16_t vbus_mv;
        uint16_t ibus_ma;
        uint32_t meas_mw;
        bool hw_alert = false;

        if (ina_ok) {
            vbus_mv = sample.vbus_mv;
            ibus_ma = sample.ibus_ma;
            meas_mw = sample.power_mw;
            if (ina226_detected_chip() == INA_CHIP_219) {
                /* INA219 (+-320mV / R100 = 3.2A full-scale) has no ALERT pin. */
                bsp_alert_exti_set_enabled(false);
                prot.cfg.ocp_soft_ma = 2000U;
                prot.cfg.ocp_hard_ma = 2500U;
                hw_alert = false;
            } else {
                bsp_alert_exti_set_enabled(true);
                prot.cfg.ocp_soft_ma = prot_cfg.ocp_soft_ma;
                prot.cfg.ocp_hard_ma = prot_cfg.ocp_hard_ma;
                hw_alert = bsp_alert_is_active();
            }
        } else {
            bsp_alert_exti_set_enabled(false);
            prot.cfg.ocp_soft_ma = prot_cfg.ocp_soft_ma;
            prot.cfg.ocp_hard_ma = prot_cfg.ocp_hard_ma;
            /* Preserve any latched EXTI4 hardware alert across transient I2C dropouts. */
            hw_alert = bsp_alert_is_active();
            /* Safe simulation model (PA0 physical PWM output is gated unless APP_ALLOW_SIM_PWM_OUTPUT=1). */
            vbus_mv = (pd_mv >= 5000U) ? pd_mv : 5000U;
            if (sim_ovp_ticks > 0U) {
                vbus_mv = 22000U;
            }
            meas_mw = sim_meas_mw;
            ibus_ma = (uint16_t)((meas_mw * 1000UL) / vbus_mv);
        }

        uint16_t vdda_mv = 3300U;
        int16_t mcu_temp_c = 25;
        (void)bsp_adc_read_telemetry(&vdda_mv, &mcu_temp_c);
        if (!ina_ok && (sim_otp_ticks > 0U)) {
            mcu_temp_c = 90;
        }

        if (clear_req && (ina_ok || !prot.sensor_ever_online)) {
            (void)prot_clear_latch_with_temp(&prot, vbus_mv, ibus_ma, hw_alert, mcu_temp_c);
        }

        prot_fault_t prev_fault = prot.latched_fault;
        /* Evaluate hardware ALERT / electrical limits first so EMI-induced I2C glitches
         * during an EXTI4 trip still record OCP_H instead of SENSOR_LOSS. */
        bool allow = prot_step(&prot, vbus_mv, ibus_ma, hw_alert);
        allow = prot_step_sensor(&prot, ina_ok) && allow;
        allow = prot_step_thermal(&prot, mcu_temp_c) && allow;
        uint16_t duty = 0U;

        if (allow) {
            if (ina_ok && !prev_ina_ok) {
                /* Discard any offline-simulation PID integrator/slew state when entering live control. */
                power_pid_reset(&pid);
                sim_meas_mw = 0U;
            }
            uint32_t eff_target_mw = prot_thermal_limit_mw(target_mw,
                                                           mcu_temp_c,
                                                           prot_cfg.otp_derate_c,
                                                           prot_cfg.otp_shutdown_c);
            duty = power_pid_step(&pid, eff_target_mw, meas_mw);
            if (!ina_ok) {
                uint64_t p_full_mw = ((uint64_t)vbus_mv * vbus_mv) /
                                     ((uint64_t)APP_SIM_LOAD_OHM * 1000UL);
                sim_meas_mw = (uint32_t)((p_full_mw * duty) / 1000UL);
                meas_mw = sim_meas_mw;
#if !APP_ALLOW_SIM_PWM_OUTPUT
                /* Never drive the physical MOSFET gate without a verified live current sensor. */
                duty = 0U;
#endif
            }
        } else {
            power_pid_reset(&pid);
            sim_meas_mw = 0U;
        }

        bsp_pwm_set_duty_permille(duty);

        uint8_t flags = ina_ok ? BBOX_FLAG_INA_ONLINE : BBOX_FLAG_SIM_MODE;
        if (husb_online) {
            flags |= BBOX_FLAG_HUSB_ONLINE;
        }
        if (hw_alert) {
            flags |= BBOX_FLAG_HW_ALERT;
        }

        if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5U)) == pdTRUE) {
            s_shared.ina_online = ina_ok;
            s_shared.vbus_mv = vbus_mv;
            s_shared.ibus_ma = ibus_ma;
            s_shared.meas_power_mw = meas_mw;
            s_shared.pid_duty_permille = duty;
            s_shared.vdda_mv = vdda_mv;
            s_shared.mcu_temp_c = mcu_temp_c;
            s_shared.prot_fault = (uint8_t)prot.latched_fault;
            bool notify_bbox = false;
            if ((prev_fault == PROT_FAULT_NONE) &&
                (prot.latched_fault != PROT_FAULT_NONE) &&
                (!s_shared.pending_bb_valid)) {
                s_shared.pending_bb_rec.uptime_ms =
                    (uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS);
                s_shared.pending_bb_rec.vbus_mv = vbus_mv;
                s_shared.pending_bb_rec.ibus_ma = ibus_ma;
                s_shared.pending_bb_rec.power_mw =
                    (meas_mw > UINT16_MAX) ? UINT16_MAX : (uint16_t)meas_mw;
                s_shared.pending_bb_rec.duty_permille = last_duty;
                s_shared.pending_bb_rec.event_type =
                    prot_fault_to_bbox_evt(prot.latched_fault);
                s_shared.pending_bb_rec.flags = flags;
                s_shared.pending_bb_rec.extra = (uint16_t)prot.latched_fault;
                s_shared.pending_bb_valid = true;
                notify_bbox = true;
            }
            xSemaphoreGive(s_state_mutex);
            if (notify_bbox && (s_log_handle != NULL)) {
                (void)xTaskNotifyGive(s_log_handle);
            }
        }

        last_duty = duty;
        prev_ina_ok = ina_ok;
        {
            uint8_t mask = wdg_mark_alive(WDG_BIT_CTRL);
            bsp_crumb_update((uint32_t)(xTaskGetTickCount() * portTICK_PERIOD_MS),
                             vbus_mv, ibus_ma, duty, mask, flags);
        }

        vTaskDelayUntil(&last_wake, pdMS_TO_TICKS(20U));
    }
}

static void task_log_entry(void *arg) {
    (void)arg;
    char cmd_buf[32];
    uint8_t tick_div = 0U;
    uint8_t hud_div = 3U; /* Refresh HUD immediately on first 50ms tick, then every 200ms */
    TickType_t last_wake;

    bsp_uart_puts("\r\n[BOOT] PY32F003 PD+INA226+RTOS Ready RST=");
    print_reset_cause_str(bsp_get_reset_cause_mask());
    bsp_uart_puts(" BBOX=");
    bsp_uart_put_u32(s_bbox.state.valid_count);
    bsp_uart_puts("/");
    bsp_uart_put_u32(s_bbox.state.fault_total_count);
    bsp_uart_puts(" (H for help)\r\n");

    tft_hud_init();
    bsp_iwdg_feed();
    last_wake = xTaskGetTickCount();

    for (;;) {
        while (bsp_uart_pop_line(cmd_buf, (uint8_t)sizeof(cmd_buf))) {
            handle_uart_cmd(cmd_buf);
        }

        /* Flush newly latched fault from Task_Ctrl to Flash in low-priority logger task. */
        {
            bbox_record_t pending;
            bool has_pending = false;
            uint16_t boost_mv = 0U;
            uint8_t safe_5v_evt = 0U;

            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5U)) == pdTRUE) {
                if (s_shared.pending_bb_valid) {
                    pending = s_shared.pending_bb_rec;
                    s_shared.pending_bb_valid = false;
                    has_pending = true;
                }
                if (s_shared.auto_boost_notice_mv > 0U) {
                    boost_mv = s_shared.auto_boost_notice_mv;
                    s_shared.auto_boost_notice_mv = 0U;
                }
                if (s_shared.safe_5v_notice != 0U) {
                    safe_5v_evt = s_shared.safe_5v_notice;
                    s_shared.safe_5v_notice = 0U;
                }
                xSemaphoreGive(s_state_mutex);
            }

            if (boost_mv > 0U) {
                bsp_uart_puts("[AUTO-PD] BOOST -> ");
                bsp_uart_put_u32(boost_mv);
                bsp_uart_puts("mV\r\n");
            }
            if (safe_5v_evt == (uint8_t)PD_SAFE_DROP_IDLE) {
                bsp_uart_puts("[SAFE-PD] IDLE -> 5000mV (vSafe5V)\r\n");
            } else if (safe_5v_evt == (uint8_t)PD_SAFE_DROP_FAULT) {
                bsp_uart_puts("[SAFE-PD] FAULT -> 5000mV (vSafe5V)\r\n");
            }

            if (has_pending) {
                bool saved = false;
                if (xSemaphoreTake(s_bbox_mutex, pdMS_TO_TICKS(50U)) == pdTRUE) {
                    saved = bbox_append_fault(&s_bbox, bsp_bbox_flash_ops(), &pending);
                    xSemaphoreGive(s_bbox_mutex);
                }
                bsp_uart_puts(saved ? "[BBOX] SAVED EVT=" : "[BBOX] SAVE_ERR EVT=");
                bsp_uart_puts(bbox_event_name(pending.event_type));
                bsp_uart_puts(" V=");
                bsp_uart_put_u32(pending.vbus_mv);
                bsp_uart_puts("mV I=");
                bsp_uart_put_u32(pending.ibus_ma);
                bsp_uart_puts("mA D=");
                bsp_uart_put_u32(pending.duty_permille);
                bsp_uart_puts("pm\r\n");
            }
        }

        hud_div++;
        if (hud_div >= 4U) { /* 50ms * 4 = 200ms (5 FPS) incremental dirty-rect HUD refresh */
            app_shared_t hsnap;
            tft_hud_state_t hud;
            hud_div = 0U;
            bool has_snap = false;
            memset(&hsnap, 0, sizeof(hsnap));
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(5U)) == pdTRUE) {
                hsnap = s_shared;
                has_snap = true;
                xSemaphoreGive(s_state_mutex);
            }
            if (has_snap) {
                hud.live_mode = hsnap.ina_online;
                hud.pd_target_mv = (hsnap.pd_mv > 0U) ? hsnap.pd_mv : hsnap.desired_pd_mv;
                hud.vbus_mv = hsnap.vbus_mv;
                hud.ibus_ma = hsnap.ibus_ma;
                hud.power_mw = hsnap.meas_power_mw;
                hud.duty_permille = hsnap.pid_duty_permille;
                hud.mcu_temp_c = hsnap.mcu_temp_c;
                hud.prot_fault = hsnap.prot_fault;
                tft_hud_update(&hud);
            }
        }

        tick_div++;
        if (tick_div >= 10U) { /* 50ms * 10 = 500ms telemetry heartbeat */
            app_shared_t snap;
            bool has_snap = false;
            tick_div = 0U;
            bsp_led_toggle();

            memset(&snap, 0, sizeof(snap));
            if (xSemaphoreTake(s_state_mutex, pdMS_TO_TICKS(10U)) == pdTRUE) {
                snap = s_shared;
                has_snap = true;
                xSemaphoreGive(s_state_mutex);
            }

            if (has_snap) {
                if (!snap.ina_online) {
                    bsp_uart_puts("[SIM]  ");
                } else if (ina226_detected_chip() == INA_CHIP_219) {
                    bsp_uart_puts("[LIVE-219] ");
                } else {
                    bsp_uart_puts("[LIVE-226] ");
                }
                bsp_uart_puts("HUSB=");
                bsp_uart_put_u32(snap.husb_online ? 1U : 0U);
                bsp_uart_puts(" VBUS=");
                bsp_uart_put_u32(snap.vbus_mv);
                bsp_uart_puts("mV I=");
                bsp_uart_put_u32(snap.ibus_ma);
                bsp_uart_puts("mA P=");
                bsp_uart_put_u32(snap.meas_power_mw);
                bsp_uart_puts("/");
                bsp_uart_put_u32(snap.target_power_mw);
                bsp_uart_puts("mW DUTY=");
                bsp_uart_put_u32(snap.pid_duty_permille);
                bsp_uart_puts("pm T=");
                bsp_uart_put_i32((int32_t)snap.mcu_temp_c);
                bsp_uart_puts("C VDDA=");
                bsp_uart_put_u32(snap.vdda_mv);
                bsp_uart_puts("mV FLT=");
                bsp_uart_put_u32(snap.prot_fault);
                bsp_uart_puts(" | HWM=");
                bsp_uart_put_u32((uint32_t)uxTaskGetStackHighWaterMark(s_ctrl_handle));
                bsp_uart_puts("/");
                bsp_uart_put_u32((uint32_t)uxTaskGetStackHighWaterMark(s_pd_handle));
                bsp_uart_puts("/");
                bsp_uart_put_u32((uint32_t)uxTaskGetStackHighWaterMark(s_log_handle));
                bsp_uart_puts("\r\n");
            }
        }

        /* Cooperative watchdog: feed IWDG ONLY when all 3 tasks checked in cleanly. */
        if (!s_wdg_stall_test) {
            bool all_alive = false;
            taskENTER_CRITICAL();
            s_wdg_alive_mask = (uint8_t)(s_wdg_alive_mask | WDG_BIT_LOG);
            if ((s_wdg_alive_mask & WDG_ALL_BITS) == WDG_ALL_BITS) {
                s_wdg_alive_mask = 0U;
                all_alive = true;
            }
            taskEXIT_CRITICAL();

            if (all_alive) {
                bsp_iwdg_feed();
            }
        }

        /* Wait up to 50ms periodic boundary, or wake immediately on fault notification from Task_Ctrl. */
        {
            TickType_t now = xTaskGetTickCount();
            TickType_t elapsed = now - last_wake;
            if (elapsed < pdMS_TO_TICKS(50U)) {
                (void)ulTaskNotifyTake(pdTRUE, pdMS_TO_TICKS(50U) - elapsed);
            }
            last_wake = xTaskGetTickCount();
        }
    }
}

int main(void) {
    uint8_t rst_mask;
    bool is_iwdg_rst;
    bsp_wdg_crumb_t crumb;
    bbox_record_t bb_crumb;
    bool has_crumb;

    bsp_board_init();

    rst_mask = bsp_get_reset_cause_mask();
    is_iwdg_rst = ((rst_mask & BSP_RST_IWDG) != 0U);
    has_crumb = bsp_crumb_consume(&crumb);

    bbox_init(&s_bbox, bsp_bbox_flash_ops());
    if (has_crumb) {
        uint32_t p_mw = ((uint32_t)crumb.vbus_mv * crumb.ibus_ma) / 1000UL;
        memset(&bb_crumb, 0, sizeof(bb_crumb));
        bb_crumb.uptime_ms = crumb.uptime_ms;
        bb_crumb.vbus_mv = crumb.vbus_mv;
        bb_crumb.ibus_ma = crumb.ibus_ma;
        bb_crumb.power_mw = (p_mw > UINT16_MAX) ? UINT16_MAX : (uint16_t)p_mw;
        bb_crumb.duty_permille = crumb.duty_permille;
        bb_crumb.flags = crumb.flags;
        bb_crumb.extra = crumb.alive_mask;
    }
    (void)bbox_record_boot(&s_bbox, bsp_bbox_flash_ops(), rst_mask, is_iwdg_rst,
                           has_crumb ? &bb_crumb : NULL);

    bsp_iwdg_init();

    s_state_mutex = xSemaphoreCreateMutexStatic(&s_state_mutex_buf);
    s_i2c_mutex = xSemaphoreCreateMutexStatic(&s_i2c_mutex_buf);
    s_bbox_mutex = xSemaphoreCreateMutexStatic(&s_bbox_mutex_buf);
    configASSERT(s_state_mutex != NULL);
    configASSERT(s_i2c_mutex != NULL);
    configASSERT(s_bbox_mutex != NULL);

    s_ctrl_handle = xTaskCreateStatic(task_ctrl_entry, "CTRL", CTRL_TASK_STACK_WORDS,
                                      NULL, 3U, s_ctrl_stack, &s_ctrl_tcb);
    s_pd_handle = xTaskCreateStatic(task_pd_entry, "PD", PD_TASK_STACK_WORDS,
                                    NULL, 2U, s_pd_stack, &s_pd_tcb);
    s_log_handle = xTaskCreateStatic(task_log_entry, "LOG", LOG_TASK_STACK_WORDS,
                                     NULL, 1U, s_log_stack, &s_log_tcb);

    configASSERT(s_ctrl_handle != NULL);
    configASSERT(s_pd_handle != NULL);
    configASSERT(s_log_handle != NULL);

    vTaskStartScheduler();

    for (;;) {
    }
}
