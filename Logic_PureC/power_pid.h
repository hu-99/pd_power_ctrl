#ifndef POWER_PID_H
#define POWER_PID_H

#include <stdint.h>

#define POWER_PID_DUTY_MAX 1000U
#define POWER_PID_SCALE 1000

typedef struct {
    int32_t kp_milli;
    int32_t ki_milli;
    int32_t kd_milli;
    uint16_t ramp_step_mw;
    uint16_t max_duty_permille;
} power_pid_cfg_t;

typedef struct {
    power_pid_cfg_t cfg;
    int32_t i_accum;
    int32_t prev_err_mw;
    uint32_t ramped_target_mw;
    uint16_t last_duty;
} power_pid_t;

/**
 * @brief Initialize closed-loop power PID controller with fixed-point gains.
 */
void power_pid_init(power_pid_t *pid, const power_pid_cfg_t *cfg);

/**
 * @brief Reset integrator, derivative memory, and ramped setpoint on fault/stop.
 */
void power_pid_reset(power_pid_t *pid);

/**
 * @brief Update PID for one control tick.
 * @param target_mw   Requested load power in milliwatts.
 * @param measured_mw Measured load power from INA226 in milliwatts.
 * @return Output PWM duty cycle in permille [0..max_duty_permille].
 */
uint16_t power_pid_step(power_pid_t *pid,
                        uint32_t target_mw,
                        uint32_t measured_mw);

#endif /* POWER_PID_H */
