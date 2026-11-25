#ifndef PROT_OVP_OCP_H
#define PROT_OVP_OCP_H

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    PROT_FAULT_NONE = 0,
    PROT_FAULT_UVP,
    PROT_FAULT_OVP,
    PROT_FAULT_OCP_SOFT,
    PROT_FAULT_OCP_HARD,
    PROT_FAULT_OTP,
    PROT_FAULT_SENSOR_LOSS
} prot_fault_t;

typedef struct {
    uint16_t uvp_mv;
    uint16_t uvp_hyst_mv;
    uint16_t ovp_mv;
    uint16_t ovp_hyst_mv;
    uint16_t ocp_soft_ma;
    uint16_t ocp_hard_ma;
    uint8_t uvp_debounce_ticks;
    uint8_t ovp_debounce_ticks;
    uint8_t ocp_debounce_ticks;
    int16_t otp_derate_c;
    int16_t otp_shutdown_c;
    int16_t otp_hyst_c;
    uint8_t otp_debounce_ticks;
} prot_cfg_t;

typedef struct {
    prot_cfg_t cfg;
    prot_fault_t latched_fault;
    uint8_t uvp_cnt;
    uint8_t ovp_cnt;
    uint8_t ocp_cnt;
    uint8_t otp_cnt;
    bool sensor_ever_online;
    bool output_allowed;
} prot_fsm_t;

/**
 * @brief Initialize protection supervisor with safe thresholds and cleared latches.
 */
void prot_init(prot_fsm_t *fsm, const prot_cfg_t *cfg);

/**
 * @brief Monitor power-sensor I2C link health; latches PROT_FAULT_SENSOR_LOSS (duty=0)
 *        immediately if the sensor was previously online and drops offline.
 * @return true if sensor link is healthy (or has never been attached yet).
 */
bool prot_step_sensor(prot_fsm_t *fsm, bool sensor_online);

/**
 * @brief Update electrical protection state (UVP/OVP/OCP/HW_ALERT) for one sample tick.
 * @return true if power stage output remains permitted.
 */
bool prot_step(prot_fsm_t *fsm,
               uint16_t vbus_mv,
               uint16_t ibus_ma,
               bool hw_alert_active);

/**
 * @brief Update over-temperature protection state using live MCU/board temperature (degC).
 * @return true if output remains permitted.
 */
bool prot_step_thermal(prot_fsm_t *fsm, int16_t temp_c);

/**
 * @brief Linearly fold back requested power between derate_start_c (100%) and shutdown_c (25% -> 0).
 */
uint32_t prot_thermal_limit_mw(uint32_t req_power_mw,
                               int16_t temp_c,
                               int16_t derate_start_c,
                               int16_t shutdown_c);

/**
 * @brief Attempt to clear latched fault once voltage/current/alert are in safe hysteresis band.
 */
bool prot_clear_latch(prot_fsm_t *fsm,
                      uint16_t vbus_mv,
                      uint16_t ibus_ma,
                      bool hw_alert_active);

/**
 * @brief Attempt to clear latched fault including thermal hysteresis check against temp_c.
 */
bool prot_clear_latch_with_temp(prot_fsm_t *fsm,
                                uint16_t vbus_mv,
                                uint16_t ibus_ma,
                                bool hw_alert_active,
                                int16_t temp_c);

#endif /* PROT_OVP_OCP_H */
