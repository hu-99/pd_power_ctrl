#include "power_pid.h"
#include "test_framework.h"

/* Simulated resistive load (e.g. 35 ohm golden resistor series):
 * P_full_mw = V_mv^2 / R_ohm / 1000; measured_mw = P_full_mw * duty / 1000. */
static uint32_t simulate_load_mw(uint32_t vbus_mv, uint32_t r_ohm, uint16_t duty) {
    uint64_t p_full_mw = ((uint64_t)vbus_mv * vbus_mv) / ((uint64_t)r_ohm * 1000U);
    return (uint32_t)((p_full_mw * duty) / POWER_PID_DUTY_MAX);
}

static int test_pid_tracks_constant_power_across_voltage_step(void) {
    power_pid_t pid;
    power_pid_cfg_t cfg = {
        .kp_milli = 25,
        .ki_milli = 15,
        .kd_milli = 2,
        .ramp_step_mw = 500U,
        .max_duty_permille = 950U
    };
    uint32_t target_mw = 5000U;
    uint16_t duty = 0U;
    uint32_t meas_mw = 0U;

    power_pid_init(&pid, &cfg);

    /* Phase 1: 12V rail across 35 ohm (full power ~4.11W -> wait, use 25 ohm: ~5.76W,
     * or target 3000mW across 35 ohm so 12V has headroom!). */
    target_mw = 3000U;
    for (int i = 0; i < 60; i++) {
        duty = power_pid_step(&pid, target_mw, meas_mw);
        meas_mw = simulate_load_mw(12000U, 35U, duty);
    }
    TEST_ASSERT_TRUE((meas_mw >= 2850U) && (meas_mw <= 3150U));

    /* Phase 2: Sudden step jump 12V -> 20V (e.g., PD rail switch).
     * Controller must reduce duty and settle back near 3000mW. */
    uint16_t duty_12v = duty;
    meas_mw = simulate_load_mw(20000U, 35U, duty_12v);
    for (int i = 0; i < 60; i++) {
        duty = power_pid_step(&pid, target_mw, meas_mw);
        meas_mw = simulate_load_mw(20000U, 35U, duty);
    }
    TEST_ASSERT_TRUE(duty < duty_12v);
    TEST_ASSERT_TRUE((meas_mw >= 2850U) && (meas_mw <= 3150U));
    return 0;
}

static int test_pid_anti_windup_and_zero_reset(void) {
    power_pid_t pid;
    power_pid_cfg_t cfg = {
        .kp_milli = 40,
        .ki_milli = 20,
        .kd_milli = 0,
        .ramp_step_mw = 0U,
        .max_duty_permille = 900U
    };
    uint16_t duty = 0U;

    power_pid_init(&pid, &cfg);

    /* Drive open-circuit load (measured = 0mW) for 100 ticks -> clamped at 900. */
    for (int i = 0; i < 100; i++) {
        duty = power_pid_step(&pid, 5000U, 0U);
    }
    TEST_ASSERT_EQ_INT(900U, duty);
    TEST_ASSERT_TRUE(pid.i_accum <= (int32_t)cfg.max_duty_permille * POWER_PID_SCALE);

    /* Setting target_mw = 0 must reset integrator and immediately cut output to 0. */
    duty = power_pid_step(&pid, 0U, 0U);
    TEST_ASSERT_EQ_INT(0U, duty);
    TEST_ASSERT_EQ_INT(0, pid.i_accum);
    return 0;
}

int run_power_pid_tests(void) {
    RUN_TEST(test_pid_tracks_constant_power_across_voltage_step);
    RUN_TEST(test_pid_anti_windup_and_zero_reset);
    return 0;
}
