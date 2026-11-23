#include "pd_sink_policy.h"
#include "test_framework.h"

static pd_caps_t make_standard_65w_caps(void) {
    pd_caps_t caps = {0};
    caps.attached = true;
    caps.supported[PD_PROFILE_5V] = true;
    caps.max_current_ma[PD_PROFILE_5V] = 3000U;
    caps.supported[PD_PROFILE_9V] = true;
    caps.max_current_ma[PD_PROFILE_9V] = 3000U;
    caps.supported[PD_PROFILE_12V] = true;
    caps.max_current_ma[PD_PROFILE_12V] = 3000U;
    caps.supported[PD_PROFILE_20V] = true;
    caps.max_current_ma[PD_PROFILE_20V] = 3250U;
    return caps;
}

static int test_pd_selects_highest_supported_below_target(void) {
    pd_caps_t caps = make_standard_65w_caps();

    /* 15V and 18V absent -> requesting 18V must fall back cleanly to 12V. */
    TEST_ASSERT_EQ_INT(PD_PROFILE_12V, pd_sink_select_profile(&caps, 18000U));
    TEST_ASSERT_EQ_INT(PD_PROFILE_20V, pd_sink_select_profile(&caps, 20000U));
    TEST_ASSERT_EQ_INT(PD_PROFILE_5V, pd_sink_select_profile(&caps, 5000U));
    return 0;
}

static int test_pd_attach_negotiate_settle_and_switch(void) {
    pd_sink_fsm_t fsm;
    pd_caps_t caps = make_standard_65w_caps();
    pd_sink_input_t in = {0};
    pd_sink_output_t out = {0};

    pd_sink_init(&fsm, 12000U);
    in.link_ok = true;
    in.attached = true;
    in.desired_mv = 12000U;
    in.vbus_mv = 5050U;
    in.caps = &caps;

    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_EQ_INT(PD_STATE_REQUEST_SENT, out.state);
    TEST_ASSERT_TRUE(out.should_write_request);
    TEST_ASSERT_EQ_INT(PD_PROFILE_12V, out.request_profile);
    TEST_ASSERT_TRUE(!out.power_ready);

    /* Bus settles to 12V -> FSM marks power_ready. */
    in.vbus_mv = 11980U;
    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_EQ_INT(PD_STATE_READY, out.state);
    TEST_ASSERT_TRUE(out.power_ready);
    TEST_ASSERT_EQ_INT(12000U, out.active_mv);

    /* User switches desired voltage to 20V -> triggers new request. */
    in.desired_mv = 20000U;
    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_EQ_INT(PD_STATE_DISCOVER_CAPS, out.state);
    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_EQ_INT(PD_STATE_REQUEST_SENT, out.state);
    TEST_ASSERT_TRUE(out.should_write_request);
    TEST_ASSERT_EQ_INT(PD_PROFILE_20V, out.request_profile);
    return 0;
}

static int test_pd_comm_fault_falls_back_to_safe_5v(void) {
    pd_sink_fsm_t fsm;
    pd_caps_t caps = make_standard_65w_caps();
    pd_sink_input_t in = {0};
    pd_sink_output_t out = {0};

    pd_sink_init(&fsm, 20000U);
    in.link_ok = true;
    in.attached = true;
    in.desired_mv = 20000U;
    in.vbus_mv = 20000U;
    in.caps = &caps;

    pd_sink_step(&fsm, &in, &out);
    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_EQ_INT(PD_STATE_READY, out.state);

    /* 3 consecutive I2C errors must drop to safe 5V fault state. */
    in.link_ok = false;
    pd_sink_step(&fsm, &in, &out);
    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_TRUE(out.state != PD_STATE_COMM_FAULT_SAFE_5V);
    pd_sink_step(&fsm, &in, &out);
    TEST_ASSERT_EQ_INT(PD_STATE_COMM_FAULT_SAFE_5V, out.state);
    TEST_ASSERT_TRUE(out.should_write_request);
    TEST_ASSERT_EQ_INT(PD_PROFILE_5V, out.request_profile);
    TEST_ASSERT_TRUE(!out.power_ready);
    return 0;
}

static int test_pd_auto_boost_steps_up_and_stops_at_max_pdo(void) {
    pd_caps_t caps = {0};
    pd_auto_boost_t ab;
    pd_auto_boost_input_t in = {0};
    uint16_t next_mv = 0U;
    int i;

    /* 20W charger supporting only 5V, 9V, 12V */
    caps.attached = true;
    caps.supported[PD_PROFILE_5V] = true;
    caps.max_current_ma[PD_PROFILE_5V] = 3000U;
    caps.supported[PD_PROFILE_9V] = true;
    caps.max_current_ma[PD_PROFILE_9V] = 2220U;
    caps.supported[PD_PROFILE_12V] = true;
    caps.max_current_ma[PD_PROFILE_12V] = 1670U;

    TEST_ASSERT_EQ_INT(9000U, pd_sink_next_higher_mv(&caps, 5000U));
    TEST_ASSERT_EQ_INT(12000U, pd_sink_next_higher_mv(&caps, 9000U));
    TEST_ASSERT_EQ_INT(0U, pd_sink_next_higher_mv(&caps, 12000U));

    pd_auto_boost_init(&ab);
    in.enabled = true;
    in.power_ready = true;
    in.fault_active = false;
    in.current_mv = 5000U;
    in.ibus_ma = 450U;
    in.duty_permille = 950U;
    in.target_power_mw = 5000U;
    in.meas_power_mw = 2170U;
    in.caps = &caps;

    /* 19 ticks must not fire yet; 20th tick (1.0s) steps up 5V -> 9V */
    for (i = 0; i < 19; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }
    next_mv = pd_auto_boost_step(&ab, &in);
    TEST_ASSERT_EQ_INT(9000U, next_mv);

    /* During 30-tick cooldown, must not trigger another boost */
    in.current_mv = 9000U;
    in.meas_power_mw = 7000U;
    in.target_power_mw = 10000U;
    for (i = 0; i < 30; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }
    /* After cooldown, 20 saturated ticks step up 9V -> 12V */
    for (i = 0; i < 19; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }
    TEST_ASSERT_EQ_INT(12000U, pd_auto_boost_step(&ab, &in));

    /* At max 12V PDO, must return 0 cleanly even if still saturated */
    in.current_mv = 12000U;
    for (i = 0; i < 60; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }
    return 0;
}

static int test_pd_auto_boost_ignores_open_circuit_and_fault(void) {
    pd_caps_t caps = make_standard_65w_caps();
    pd_auto_boost_t ab;
    pd_auto_boost_input_t in = {0};
    int i;

    pd_auto_boost_init(&ab);
    in.enabled = true;
    in.power_ready = true;
    in.fault_active = false;
    in.current_mv = 5000U;
    in.ibus_ma = 0U; /* Open circuit: no load attached */
    in.duty_permille = 950U;
    in.target_power_mw = 5000U;
    in.meas_power_mw = 0U;
    in.caps = &caps;

    for (i = 0; i < 40; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }

    /* With load present but fault latched or feature disabled, must also stay silent */
    in.ibus_ma = 450U;
    in.meas_power_mw = 2170U;
    in.fault_active = true;
    for (i = 0; i < 40; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }

    in.fault_active = false;
    in.enabled = false;
    for (i = 0; i < 40; i++) {
        TEST_ASSERT_EQ_INT(0U, pd_auto_boost_step(&ab, &in));
    }
    return 0;
}

static int test_pd_safe_5v_drops_on_idle_after_load_and_respects_manual_v(void) {
    pd_safe_5v_t sf;
    int i;

    pd_safe_5v_init(&sf);

    /* Never loaded yet: idle at 12V (manual bench test) must NOT be forced down. */
    for (i = 0; i < 30; i++) {
        TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 0UL, false));
    }

    /* Active load at 12V arms idle fallback. */
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 5000UL, false));

    /* Load turned off (P=0): waits 9 ticks (450ms), drops on 10th tick (500ms). */
    for (i = 0; i < 9; i++) {
        TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 0UL, false));
    }
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_IDLE, pd_safe_5v_step(&sf, 12000U, 0UL, false));

    /* Once dropped, manual V 9000 while idle must stay at 9V. */
    pd_safe_5v_notify_manual_v(&sf);
    for (i = 0; i < 30; i++) {
        TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 9000U, 0UL, false));
    }

    /* Manual V during the 500ms idle countdown cancels the drop. */
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 6000UL, false));
    for (i = 0; i < 5; i++) {
        TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 0UL, false));
    }
    pd_safe_5v_notify_manual_v(&sf);
    for (i = 0; i < 20; i++) {
        TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 0UL, false));
    }
    return 0;
}

static int test_pd_safe_5v_drops_immediately_on_fault_and_rearms(void) {
    pd_safe_5v_t sf;

    pd_safe_5v_init(&sf);

    /* Fault while at 12V (even if never loaded) drops immediately, once. */
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_FAULT, pd_safe_5v_step(&sf, 12000U, 0UL, true));
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 12000U, 0UL, true));
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 5000U, 0UL, true));

    /* Clear fault -> re-arm for future faults. */
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_NONE, pd_safe_5v_step(&sf, 5000U, 0UL, false));
    TEST_ASSERT_EQ_INT(PD_SAFE_DROP_FAULT, pd_safe_5v_step(&sf, 20000U, 10000UL, true));
    return 0;
}

int run_pd_sink_policy_tests(void) {
    RUN_TEST(test_pd_selects_highest_supported_below_target);
    RUN_TEST(test_pd_attach_negotiate_settle_and_switch);
    RUN_TEST(test_pd_comm_fault_falls_back_to_safe_5v);
    RUN_TEST(test_pd_auto_boost_steps_up_and_stops_at_max_pdo);
    RUN_TEST(test_pd_auto_boost_ignores_open_circuit_and_fault);
    RUN_TEST(test_pd_safe_5v_drops_on_idle_after_load_and_respects_manual_v);
    RUN_TEST(test_pd_safe_5v_drops_immediately_on_fault_and_rearms);
    return 0;
}
