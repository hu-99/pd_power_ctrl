#include "power_pid.h"
#include <stdbool.h>
#include <stddef.h>

static uint32_t slew_limit_target(uint32_t current_target,
                                  uint32_t desired_target,
                                  uint16_t step_mw) {
    if (step_mw == 0U) {
        return desired_target;
    }
    if (desired_target > current_target) {
        uint32_t diff = desired_target - current_target;
        return (diff > step_mw) ? (current_target + step_mw) : desired_target;
    }
    if (desired_target < current_target) {
        uint32_t diff = current_target - desired_target;
        return (diff > step_mw) ? (current_target - step_mw) : desired_target;
    }
    return desired_target;
}

void power_pid_init(power_pid_t *pid, const power_pid_cfg_t *cfg) {
    if ((pid == NULL) || (cfg == NULL)) {
        return;
    }
    pid->cfg = *cfg;
    if (pid->cfg.max_duty_permille > POWER_PID_DUTY_MAX) {
        pid->cfg.max_duty_permille = POWER_PID_DUTY_MAX;
    }
    power_pid_reset(pid);
}

void power_pid_reset(power_pid_t *pid) {
    if (pid == NULL) {
        return;
    }
    pid->i_accum = 0;
    pid->prev_err_mw = 0;
    pid->ramped_target_mw = 0U;
    pid->last_duty = 0U;
}

uint16_t power_pid_step(power_pid_t *pid,
                        uint32_t target_mw,
                        uint32_t measured_mw) {
    if (pid == NULL) {
        return 0U;
    }
    if (target_mw == 0U) {
        power_pid_reset(pid);
        return 0U;
    }

    pid->ramped_target_mw =
        slew_limit_target(pid->ramped_target_mw, target_mw, pid->cfg.ramp_step_mw);

    int32_t err = (int32_t)pid->ramped_target_mw - (int32_t)measured_mw;
    int32_t derr = err - pid->prev_err_mw;
    pid->prev_err_mw = err;

    int32_t p_scaled = pid->cfg.kp_milli * err;
    int32_t d_scaled = pid->cfg.kd_milli * derr;
    int32_t max_scaled = (int32_t)pid->cfg.max_duty_permille * POWER_PID_SCALE;

    int32_t next_i = pid->i_accum + (pid->cfg.ki_milli * err);
    if (next_i > max_scaled) {
        next_i = max_scaled;
    } else if (next_i < 0) {
        next_i = 0;
    }

    int32_t raw_scaled = p_scaled + next_i + d_scaled;

    /* Conditional anti-windup: advance only up to saturation boundary, never wind up past it. */
    if ((raw_scaled > max_scaled) && (err > 0)) {
        int32_t room = max_scaled - p_scaled - d_scaled;
        if ((room > pid->i_accum) && (room <= max_scaled)) {
            pid->i_accum = room;
        }
    } else if ((raw_scaled < 0) && (err < 0)) {
        int32_t floor_i = -p_scaled - d_scaled;
        if (floor_i < 0) {
            floor_i = 0;
        }
        if (floor_i < pid->i_accum) {
            pid->i_accum = floor_i;
        }
    } else {
        pid->i_accum = next_i;
    }

    int32_t final_scaled = p_scaled + pid->i_accum + d_scaled;
    if (final_scaled <= 0) {
        pid->last_duty = 0U;
    } else if (final_scaled >= max_scaled) {
        pid->last_duty = pid->cfg.max_duty_permille;
    } else {
        pid->last_duty = (uint16_t)(final_scaled / POWER_PID_SCALE);
    }

    return pid->last_duty;
}
