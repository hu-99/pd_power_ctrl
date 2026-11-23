#include "pd_sink_policy.h"
#include <stddef.h>

#define PD_COMM_ERR_LIMIT 3U
#define PD_SETTLE_TIMEOUT_TICKS 20U
#define PD_VBUS_TOLERANCE_PCT 12U

#define PD_AUTOBOOST_DUTY_THRESH_PM   940U
#define PD_AUTOBOOST_MIN_LOAD_MA      20U
#define PD_AUTOBOOST_SHORTFALL_PCT    92U
#define PD_AUTOBOOST_SAT_TICKS        20U /* 20 * 50ms = 1.0s sustained saturation */
#define PD_AUTOBOOST_COOLDOWN_TICKS   30U /* 30 * 50ms = 1.5s settle after step-up */
#define PD_SAFE5V_IDLE_DELAY_TICKS    10U /* 10 * 50ms = 500ms settle after P=0 before dropping rail */

static const uint16_t k_profile_mv[PD_SINK_PROFILE_COUNT] = {
    5000U, 9000U, 12000U, 15000U, 18000U, 20000U
};

static bool vbus_within_tolerance(uint16_t vbus_mv, uint16_t nominal_mv) {
    uint32_t margin_mv = ((uint32_t)nominal_mv * PD_VBUS_TOLERANCE_PCT) / 100U;
    uint32_t low_mv = (nominal_mv > margin_mv) ? (nominal_mv - margin_mv) : 0U;
    uint32_t high_mv = (uint32_t)nominal_mv + margin_mv;
    return ((uint32_t)vbus_mv >= low_mv) && ((uint32_t)vbus_mv <= high_mv);
}

static void reset_to_safe_5v(pd_sink_fsm_t *fsm, pd_sink_state_t next_state) {
    fsm->state = next_state;
    fsm->selected_mv = 5000U;
    fsm->selected_max_ma = 500U;
    fsm->settle_ticks = 0U;
    fsm->request_pending = false;
    fsm->pending_profile = PD_PROFILE_5V;
}

void pd_sink_init(pd_sink_fsm_t *fsm, uint16_t initial_desired_mv) {
    if (fsm == NULL) {
        return;
    }
    fsm->target_mv = (initial_desired_mv >= 5000U) ? initial_desired_mv : 5000U;
    fsm->comm_err_cnt = 0U;
    reset_to_safe_5v(fsm, PD_STATE_UNATTACHED);
}

uint16_t pd_sink_profile_mv(pd_profile_idx_t profile) {
    if ((uint8_t)profile >= PD_SINK_PROFILE_COUNT) {
        return 5000U;
    }
    return k_profile_mv[(uint8_t)profile];
}

pd_profile_idx_t pd_sink_select_profile(const pd_caps_t *caps,
                                        uint16_t desired_mv) {
    if ((caps == NULL) || !caps->attached) {
        return PD_PROFILE_5V;
    }

    for (int8_t idx = (int8_t)PD_SINK_PROFILE_COUNT - 1; idx >= 0; idx--) {
        if (caps->supported[idx] && (caps->max_current_ma[idx] > 0U) &&
            (k_profile_mv[idx] <= desired_mv)) {
            return (pd_profile_idx_t)idx;
        }
    }
    return PD_PROFILE_5V;
}

uint16_t pd_sink_next_higher_mv(const pd_caps_t *caps, uint16_t current_mv) {
    if ((caps == NULL) || !caps->attached) {
        return 0U;
    }

    for (uint8_t idx = 0U; idx < PD_SINK_PROFILE_COUNT; idx++) {
        if (caps->supported[idx] && (caps->max_current_ma[idx] > 0U) &&
            (k_profile_mv[idx] > current_mv)) {
            return k_profile_mv[idx];
        }
    }
    return 0U;
}

void pd_auto_boost_init(pd_auto_boost_t *ab) {
    if (ab == NULL) {
        return;
    }
    ab->sat_ticks = 0U;
    ab->cooldown_ticks = 0U;
}

uint16_t pd_auto_boost_step(pd_auto_boost_t *ab, const pd_auto_boost_input_t *in) {
    if ((ab == NULL) || (in == NULL)) {
        return 0U;
    }

    if (ab->cooldown_ticks > 0U) {
        ab->cooldown_ticks--;
        ab->sat_ticks = 0U;
        return 0U;
    }

    if (!in->enabled || !in->power_ready || in->fault_active ||
        (in->target_power_mw == 0UL) || (in->caps == NULL)) {
        ab->sat_ticks = 0U;
        return 0U;
    }

    /* Require real load current (>=20mA) so open-circuit never auto-boosts voltage. */
    if ((in->ibus_ma >= PD_AUTOBOOST_MIN_LOAD_MA) &&
        (in->duty_permille >= PD_AUTOBOOST_DUTY_THRESH_PM) &&
        ((in->meas_power_mw * 100UL) < (in->target_power_mw * PD_AUTOBOOST_SHORTFALL_PCT))) {
        if (ab->sat_ticks < UINT8_MAX) {
            ab->sat_ticks++;
        }
        if (ab->sat_ticks >= PD_AUTOBOOST_SAT_TICKS) {
            uint16_t next_mv = pd_sink_next_higher_mv(in->caps, in->current_mv);
            ab->sat_ticks = 0U;
            if (next_mv > in->current_mv) {
                ab->cooldown_ticks = PD_AUTOBOOST_COOLDOWN_TICKS;
                return next_mv;
            }
        }
    } else {
        ab->sat_ticks = 0U;
    }

    return 0U;
}

void pd_safe_5v_init(pd_safe_5v_t *sf) {
    if (sf == NULL) {
        return;
    }
    sf->was_loaded = false;
    sf->fault_handled = false;
    sf->idle_ticks = 0U;
}

void pd_safe_5v_notify_manual_v(pd_safe_5v_t *sf) {
    if (sf == NULL) {
        return;
    }
    sf->was_loaded = false;
    sf->idle_ticks = 0U;
}

pd_safe_drop_reason_t pd_safe_5v_step(pd_safe_5v_t *sf,
                                      uint16_t active_mv,
                                      uint32_t target_power_mw,
                                      bool fault_active) {
    if (sf == NULL) {
        return PD_SAFE_DROP_NONE;
    }

    if (fault_active) {
        sf->was_loaded = false;
        sf->idle_ticks = 0U;
        if (!sf->fault_handled) {
            sf->fault_handled = true;
            if (active_mv > 5000U) {
                return PD_SAFE_DROP_FAULT;
            }
        }
        return PD_SAFE_DROP_NONE;
    }

    sf->fault_handled = false;

    if (target_power_mw > 0UL) {
        sf->was_loaded = true;
        sf->idle_ticks = 0U;
        return PD_SAFE_DROP_NONE;
    }

    if (sf->was_loaded) {
        if (active_mv <= 5000U) {
            sf->was_loaded = false;
            sf->idle_ticks = 0U;
            return PD_SAFE_DROP_NONE;
        }
        if (sf->idle_ticks < UINT8_MAX) {
            sf->idle_ticks++;
        }
        if (sf->idle_ticks >= PD_SAFE5V_IDLE_DELAY_TICKS) {
            sf->was_loaded = false;
            sf->idle_ticks = 0U;
            return PD_SAFE_DROP_IDLE;
        }
    }

    return PD_SAFE_DROP_NONE;
}

static void handle_comm_fault(pd_sink_fsm_t *fsm, pd_sink_output_t *out) {
    if (fsm->comm_err_cnt < UINT8_MAX) {
        fsm->comm_err_cnt++;
    }
    if (fsm->comm_err_cnt >= PD_COMM_ERR_LIMIT) {
        reset_to_safe_5v(fsm, PD_STATE_COMM_FAULT_SAFE_5V);
        out->should_write_request = true;
        out->request_profile = PD_PROFILE_5V;
    }
}

void pd_sink_step(pd_sink_fsm_t *fsm,
                  const pd_sink_input_t *in,
                  pd_sink_output_t *out) {
    if ((fsm == NULL) || (in == NULL) || (out == NULL)) {
        return;
    }

    out->should_write_request = false;
    out->request_profile = PD_PROFILE_5V;

    if (!in->link_ok) {
        handle_comm_fault(fsm, out);
        out->state = fsm->state;
        out->active_mv = fsm->selected_mv;
        out->active_max_ma = fsm->selected_max_ma;
        out->power_ready = false;
        return;
    }

    /* Link healthy: recover from latched comm fault only after clean detach or attach refresh. */
    fsm->comm_err_cnt = 0U;

    if (!in->attached) {
        reset_to_safe_5v(fsm, PD_STATE_UNATTACHED);
        out->state = fsm->state;
        out->active_mv = 5000U;
        out->active_max_ma = 500U;
        out->power_ready = false;
        return;
    }

    fsm->target_mv = (in->desired_mv >= 5000U) ? in->desired_mv : 5000U;

    if ((fsm->state == PD_STATE_UNATTACHED) ||
        (fsm->state == PD_STATE_COMM_FAULT_SAFE_5V)) {
        fsm->state = PD_STATE_DISCOVER_CAPS;
    }

    if (fsm->state == PD_STATE_DISCOVER_CAPS) {
        pd_profile_idx_t best = pd_sink_select_profile(in->caps, fsm->target_mv);
        uint16_t best_mv = pd_sink_profile_mv(best);
        uint16_t best_ma = (in->caps != NULL && in->caps->supported[best])
                               ? in->caps->max_current_ma[best]
                               : 500U;

        fsm->pending_profile = best;
        fsm->selected_mv = best_mv;
        fsm->selected_max_ma = best_ma;
        fsm->settle_ticks = 0U;
        fsm->request_pending = true;
        fsm->state = PD_STATE_REQUEST_SENT;
        out->should_write_request = true;
        out->request_profile = best;
    } else if (fsm->state == PD_STATE_REQUEST_SENT) {
        if (vbus_within_tolerance(in->vbus_mv, fsm->selected_mv)) {
            fsm->request_pending = false;
            fsm->state = PD_STATE_READY;
        } else {
            fsm->settle_ticks++;
            if (fsm->settle_ticks >= PD_SETTLE_TIMEOUT_TICKS) {
                /* Charger failed to reach requested rail: fall back to safe 5V. */
                reset_to_safe_5v(fsm, PD_STATE_REQUEST_SENT);
                fsm->pending_profile = PD_PROFILE_5V;
                out->should_write_request = true;
                out->request_profile = PD_PROFILE_5V;
            }
        }
    } else if (fsm->state == PD_STATE_READY) {
        pd_profile_idx_t best = pd_sink_select_profile(in->caps, fsm->target_mv);
        uint16_t best_mv = pd_sink_profile_mv(best);

        if (best_mv != fsm->selected_mv) {
            fsm->state = PD_STATE_DISCOVER_CAPS;
        } else if (!vbus_within_tolerance(in->vbus_mv, fsm->selected_mv)) {
            /* Unexpected rail sag/surge while ready: drop back to settling check. */
            fsm->settle_ticks = 0U;
            fsm->state = PD_STATE_REQUEST_SENT;
        }
    }

    out->state = fsm->state;
    out->active_mv = fsm->selected_mv;
    out->active_max_ma = fsm->selected_max_ma;
    out->power_ready = (fsm->state == PD_STATE_READY);
}
