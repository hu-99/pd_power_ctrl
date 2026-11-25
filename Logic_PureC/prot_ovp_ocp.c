#include "prot_ovp_ocp.h"
#include <stddef.h>

static uint8_t saturate_inc(uint8_t cnt) {
    return (cnt < UINT8_MAX) ? (uint8_t)(cnt + 1U) : UINT8_MAX;
}

static bool in_safe_recovery_band(const prot_cfg_t *cfg,
                                  uint16_t vbus_mv,
                                  uint16_t ibus_ma,
                                  bool hw_alert_active,
                                  int16_t temp_c) {
    uint16_t uvp_recover = cfg->uvp_mv + cfg->uvp_hyst_mv;
    uint16_t ovp_recover = (cfg->ovp_mv > cfg->ovp_hyst_mv)
                               ? (uint16_t)(cfg->ovp_mv - cfg->ovp_hyst_mv)
                               : 0U;

    if (hw_alert_active) {
        return false;
    }
    if ((vbus_mv < uvp_recover) || (vbus_mv > ovp_recover)) {
        return false;
    }
    if (ibus_ma >= cfg->ocp_soft_ma) {
        return false;
    }
    if ((cfg->otp_shutdown_c > 0) &&
        (temp_c > (int16_t)(cfg->otp_shutdown_c - cfg->otp_hyst_c))) {
        return false;
    }
    return true;
}

void prot_init(prot_fsm_t *fsm, const prot_cfg_t *cfg) {
    if ((fsm == NULL) || (cfg == NULL)) {
        return;
    }
    fsm->cfg = *cfg;
    fsm->latched_fault = PROT_FAULT_NONE;
    fsm->uvp_cnt = 0U;
    fsm->ovp_cnt = 0U;
    fsm->ocp_cnt = 0U;
    fsm->otp_cnt = 0U;
    fsm->sensor_loss_cnt = 0U;
    fsm->sensor_ever_online = false;
    fsm->output_allowed = true;
}

bool prot_step_sensor(prot_fsm_t *fsm, bool sensor_online) {
    uint8_t limit;
    if (fsm == NULL) {
        return false;
    }
    if (fsm->latched_fault != PROT_FAULT_NONE) {
        fsm->output_allowed = false;
        return false;
    }
    if (sensor_online) {
        fsm->sensor_ever_online = true;
        fsm->sensor_loss_cnt = 0U;
        return true;
    }
    if (!fsm->sensor_ever_online) {
        fsm->sensor_loss_cnt = 0U;
        return true;
    }

    limit = (fsm->cfg.sensor_loss_debounce_ticks == 0U)
                ? 1U
                : fsm->cfg.sensor_loss_debounce_ticks;
    fsm->sensor_loss_cnt = saturate_inc(fsm->sensor_loss_cnt);
    if (fsm->sensor_loss_cnt >= limit) {
        fsm->latched_fault = PROT_FAULT_SENSOR_LOSS;
    }
    /* Fail-safe: force output off during ANY missing sample, even before latching. */
    fsm->output_allowed = false;
    return false;
}

bool prot_step(prot_fsm_t *fsm,
               uint16_t vbus_mv,
               uint16_t ibus_ma,
               bool hw_alert_active) {
    if (fsm == NULL) {
        return false;
    }

    if (fsm->latched_fault != PROT_FAULT_NONE) {
        fsm->output_allowed = false;
        return false;
    }

    /* Hardware ALERT or extreme over-current trips immediately in 1 tick. */
    if (hw_alert_active || (ibus_ma >= fsm->cfg.ocp_hard_ma)) {
        fsm->latched_fault = PROT_FAULT_OCP_HARD;
        fsm->output_allowed = false;
        return false;
    }

    if (vbus_mv >= fsm->cfg.ovp_mv) {
        fsm->ovp_cnt = saturate_inc(fsm->ovp_cnt);
        if (fsm->ovp_cnt >= fsm->cfg.ovp_debounce_ticks) {
            fsm->latched_fault = PROT_FAULT_OVP;
            fsm->output_allowed = false;
            return false;
        }
    } else {
        fsm->ovp_cnt = 0U;
    }

    if (ibus_ma >= fsm->cfg.ocp_soft_ma) {
        fsm->ocp_cnt = saturate_inc(fsm->ocp_cnt);
        if (fsm->ocp_cnt >= fsm->cfg.ocp_debounce_ticks) {
            fsm->latched_fault = PROT_FAULT_OCP_SOFT;
            fsm->output_allowed = false;
            return false;
        }
    } else {
        fsm->ocp_cnt = 0U;
    }

    if (vbus_mv <= fsm->cfg.uvp_mv) {
        fsm->uvp_cnt = saturate_inc(fsm->uvp_cnt);
        if (fsm->uvp_cnt >= fsm->cfg.uvp_debounce_ticks) {
            fsm->latched_fault = PROT_FAULT_UVP;
            fsm->output_allowed = false;
            return false;
        }
    } else {
        fsm->uvp_cnt = 0U;
    }

    fsm->output_allowed = true;
    return true;
}

bool prot_step_thermal(prot_fsm_t *fsm, int16_t temp_c) {
    if (fsm == NULL) {
        return false;
    }
    if (fsm->latched_fault != PROT_FAULT_NONE) {
        fsm->output_allowed = false;
        return false;
    }
    if ((fsm->cfg.otp_shutdown_c <= 0) || (fsm->cfg.otp_debounce_ticks == 0U)) {
        return fsm->output_allowed;
    }

    if (temp_c >= fsm->cfg.otp_shutdown_c) {
        fsm->otp_cnt = saturate_inc(fsm->otp_cnt);
        if (fsm->otp_cnt >= fsm->cfg.otp_debounce_ticks) {
            fsm->latched_fault = PROT_FAULT_OTP;
            fsm->output_allowed = false;
            return false;
        }
    } else {
        fsm->otp_cnt = 0U;
    }
    return fsm->output_allowed;
}

uint32_t prot_thermal_limit_mw(uint32_t req_power_mw,
                               int16_t temp_c,
                               int16_t derate_start_c,
                               int16_t shutdown_c) {
    int32_t span;
    int32_t excess;
    int32_t scale_pct;

    if ((shutdown_c <= derate_start_c) || (temp_c < derate_start_c)) {
        return req_power_mw;
    }
    if (temp_c >= shutdown_c) {
        return 0U;
    }

    span = (int32_t)(shutdown_c - derate_start_c);
    excess = (int32_t)(temp_c - derate_start_c);
    scale_pct = 100 - ((75 * excess) / span);
    if (scale_pct < 25) {
        scale_pct = 25;
    }
    return (req_power_mw * (uint32_t)scale_pct) / 100U;
}

bool prot_clear_latch_with_temp(prot_fsm_t *fsm,
                                uint16_t vbus_mv,
                                uint16_t ibus_ma,
                                bool hw_alert_active,
                                int16_t temp_c) {
    if (fsm == NULL) {
        return false;
    }

    if (!in_safe_recovery_band(&fsm->cfg, vbus_mv, ibus_ma, hw_alert_active, temp_c)) {
        return false;
    }

    fsm->latched_fault = PROT_FAULT_NONE;
    fsm->uvp_cnt = 0U;
    fsm->ovp_cnt = 0U;
    fsm->ocp_cnt = 0U;
    fsm->otp_cnt = 0U;
    fsm->sensor_loss_cnt = 0U;
    fsm->output_allowed = true;
    return true;
}

bool prot_clear_latch(prot_fsm_t *fsm,
                      uint16_t vbus_mv,
                      uint16_t ibus_ma,
                      bool hw_alert_active) {
    return prot_clear_latch_with_temp(fsm, vbus_mv, ibus_ma, hw_alert_active, 25);
}
