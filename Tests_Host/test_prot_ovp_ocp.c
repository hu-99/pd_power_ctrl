#include "prot_ovp_ocp.h"
#include "test_framework.h"

static prot_cfg_t make_default_prot_cfg(void) {
    prot_cfg_t cfg = {
        .uvp_mv = 4300U,
        .uvp_hyst_mv = 400U,
        .ovp_mv = 21500U,
        .ovp_hyst_mv = 1000U,
        .ocp_soft_ma = 750U,
        .ocp_hard_ma = 810U,
        .uvp_debounce_ticks = 3U,
        .ovp_debounce_ticks = 3U,
        .ocp_debounce_ticks = 2U
    };
    return cfg;
}

static int test_prot_ignores_transient_spike_and_latches_sustained_ovp(void) {
    prot_fsm_t fsm;
    prot_cfg_t cfg = make_default_prot_cfg();
    prot_init(&fsm, &cfg);

    /* 2-tick transient OVP glitch followed by normal rail must NOT trip. */
    TEST_ASSERT_TRUE(prot_step(&fsm, 22000U, 200U, false));
    TEST_ASSERT_TRUE(prot_step(&fsm, 22000U, 200U, false));
    TEST_ASSERT_TRUE(prot_step(&fsm, 12000U, 200U, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);

    /* 3 consecutive OVP ticks must trip and latch output disabled. */
    TEST_ASSERT_TRUE(prot_step(&fsm, 22000U, 200U, false));
    TEST_ASSERT_TRUE(prot_step(&fsm, 22000U, 200U, false));
    TEST_ASSERT_TRUE(!prot_step(&fsm, 22000U, 200U, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_OVP, fsm.latched_fault);

    /* Must stay latched even when voltage drops into hysteresis deadband (20800mV > 20500mV). */
    TEST_ASSERT_TRUE(!prot_step(&fsm, 20800U, 0U, false));
    TEST_ASSERT_TRUE(!prot_clear_latch(&fsm, 20800U, 0U, false));

    /* Below 20500mV recovery band -> explicit clear succeeds. */
    TEST_ASSERT_TRUE(prot_clear_latch(&fsm, 19800U, 0U, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);
    TEST_ASSERT_TRUE(fsm.output_allowed);
    return 0;
}

static int test_prot_hw_alert_trips_in_single_tick(void) {
    prot_fsm_t fsm;
    prot_cfg_t cfg = make_default_prot_cfg();
    prot_init(&fsm, &cfg);

    TEST_ASSERT_TRUE(!prot_step(&fsm, 12000U, 400U, true));
    TEST_ASSERT_EQ_INT(PROT_FAULT_OCP_HARD, fsm.latched_fault);
    TEST_ASSERT_TRUE(!prot_clear_latch(&fsm, 12000U, 0U, true));
    TEST_ASSERT_TRUE(prot_clear_latch(&fsm, 12000U, 0U, false));
    return 0;
}

static int test_prot_thermal_derating_curve(void) {
    TEST_ASSERT_EQ_INT(10000, prot_thermal_limit_mw(10000U, 60, 70, 85));
    TEST_ASSERT_EQ_INT(10000, prot_thermal_limit_mw(10000U, 70, 70, 85));
    /* Midpoint ~77/78C folds back roughly halfway between 100% and 25%. */
    TEST_ASSERT_TRUE(prot_thermal_limit_mw(10000U, 78, 70, 85) <= 6500U);
    TEST_ASSERT_TRUE(prot_thermal_limit_mw(10000U, 78, 70, 85) >= 5500U);
    TEST_ASSERT_EQ_INT(0, prot_thermal_limit_mw(10000U, 85, 70, 85));
    return 0;
}

static int test_prot_otp_debounce_latch_and_hysteresis_clear(void) {
    prot_fsm_t fsm;
    prot_cfg_t cfg = make_default_prot_cfg();
    cfg.otp_derate_c = 70;
    cfg.otp_shutdown_c = 85;
    cfg.otp_hyst_c = 5;
    cfg.otp_debounce_ticks = 3U;
    prot_init(&fsm, &cfg);

    /* 2-tick transient >85C followed by cool-down must not latch. */
    TEST_ASSERT_TRUE(prot_step(&fsm, 12000U, 200U, false));
    TEST_ASSERT_TRUE(prot_step_thermal(&fsm, 88));
    TEST_ASSERT_TRUE(prot_step_thermal(&fsm, 88));
    TEST_ASSERT_TRUE(prot_step_thermal(&fsm, 72));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);

    /* 3 consecutive ticks >=85C trip and latch OTP. */
    TEST_ASSERT_TRUE(prot_step_thermal(&fsm, 86));
    TEST_ASSERT_TRUE(prot_step_thermal(&fsm, 86));
    TEST_ASSERT_TRUE(!prot_step_thermal(&fsm, 86));
    TEST_ASSERT_EQ_INT(PROT_FAULT_OTP, fsm.latched_fault);

    /* Blocked in hysteresis band (82C > 80C), clears at 78C. */
    TEST_ASSERT_TRUE(!prot_clear_latch_with_temp(&fsm, 12000U, 0U, false, 82));
    TEST_ASSERT_TRUE(prot_clear_latch_with_temp(&fsm, 12000U, 0U, false, 78));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);
    return 0;
}

static int test_prot_sensor_loss_latches_only_after_ever_online(void) {
    prot_fsm_t fsm;
    prot_cfg_t cfg = make_default_prot_cfg();
    cfg.sensor_loss_debounce_ticks = 2U;
    prot_init(&fsm, &cfg);

    /* Never online yet (offline desk boot): does not latch sensor loss fault. */
    TEST_ASSERT_TRUE(prot_step_sensor(&fsm, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);

    /* Sensor comes online on I2C bus. */
    TEST_ASSERT_TRUE(prot_step_sensor(&fsm, true));
    TEST_ASSERT_TRUE(fsm.sensor_ever_online);
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);

    /* Single transient I2C glitch: forces output disabled immediately without latching,
     * then recovers cleanly when the next sample succeeds. */
    TEST_ASSERT_TRUE(!prot_step_sensor(&fsm, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);
    TEST_ASSERT_TRUE(!fsm.output_allowed);
    TEST_ASSERT_TRUE(prot_step_sensor(&fsm, true));
    TEST_ASSERT_EQ_INT(0, fsm.sensor_loss_cnt);

    /* Sustained I2C disconnect (2 consecutive ticks): latches PROT_FAULT_SENSOR_LOSS. */
    TEST_ASSERT_TRUE(!prot_step_sensor(&fsm, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);
    TEST_ASSERT_TRUE(!prot_step_sensor(&fsm, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_SENSOR_LOSS, fsm.latched_fault);
    TEST_ASSERT_TRUE(!fsm.output_allowed);
    TEST_ASSERT_TRUE(!prot_step(&fsm, 12000U, 100U, false));

    /* Reconnect sensor and explicitly clear latch. */
    TEST_ASSERT_TRUE(prot_clear_latch(&fsm, 12000U, 0U, false));
    TEST_ASSERT_EQ_INT(PROT_FAULT_NONE, fsm.latched_fault);
    TEST_ASSERT_EQ_INT(0, fsm.sensor_loss_cnt);
    return 0;
}

int run_prot_ovp_ocp_tests(void) {
    RUN_TEST(test_prot_ignores_transient_spike_and_latches_sustained_ovp);
    RUN_TEST(test_prot_hw_alert_trips_in_single_tick);
    RUN_TEST(test_prot_thermal_derating_curve);
    RUN_TEST(test_prot_otp_debounce_latch_and_hysteresis_clear);
    RUN_TEST(test_prot_sensor_loss_latches_only_after_ever_online);
    return 0;
}
