#ifndef PD_SINK_POLICY_H
#define PD_SINK_POLICY_H

#include <stdbool.h>
#include <stdint.h>

#define PD_SINK_PROFILE_COUNT 6U

typedef enum {
    PD_PROFILE_5V = 0,
    PD_PROFILE_9V,
    PD_PROFILE_12V,
    PD_PROFILE_15V,
    PD_PROFILE_18V,
    PD_PROFILE_20V
} pd_profile_idx_t;

typedef enum {
    PD_STATE_UNATTACHED = 0,
    PD_STATE_DISCOVER_CAPS,
    PD_STATE_REQUEST_SENT,
    PD_STATE_READY,
    PD_STATE_COMM_FAULT_SAFE_5V
} pd_sink_state_t;

typedef struct {
    bool supported[PD_SINK_PROFILE_COUNT];
    uint16_t max_current_ma[PD_SINK_PROFILE_COUNT];
    bool attached;
} pd_caps_t;

typedef struct {
    pd_sink_state_t state;
    uint16_t target_mv;
    uint16_t selected_mv;
    uint16_t selected_max_ma;
    uint16_t settle_ticks;
    uint8_t comm_err_cnt;
    bool request_pending;
    pd_profile_idx_t pending_profile;
} pd_sink_fsm_t;

typedef struct {
    bool link_ok;
    bool attached;
    uint16_t vbus_mv;
    uint16_t desired_mv;
    const pd_caps_t *caps;
} pd_sink_input_t;

typedef struct {
    pd_sink_state_t state;
    bool should_write_request;
    pd_profile_idx_t request_profile;
    uint16_t active_mv;
    uint16_t active_max_ma;
    bool power_ready;
} pd_sink_output_t;

/**
 * @brief Initialize PD sink policy state machine to safe 5V unattached state.
 */
void pd_sink_init(pd_sink_fsm_t *fsm, uint16_t initial_desired_mv);

/**
 * @brief Select the highest supported PDO profile not exceeding desired_mv.
 *        Falls back to 5V if no higher profile is available.
 */
pd_profile_idx_t pd_sink_select_profile(const pd_caps_t *caps,
                                        uint16_t desired_mv);

/**
 * @brief Convert profile index to nominal millivolts.
 */
uint16_t pd_sink_profile_mv(pd_profile_idx_t profile);

/**
 * @brief Return the next strictly higher supported PDO voltage (mV) above current_mv,
 *        or 0 if the charger has no higher supported PDO.
 */
uint16_t pd_sink_next_higher_mv(const pd_caps_t *caps, uint16_t current_mv);

typedef struct {
    uint8_t sat_ticks;
    uint8_t cooldown_ticks;
} pd_auto_boost_t;

typedef struct {
    bool enabled;
    bool power_ready;
    bool fault_active;
    uint16_t current_mv;
    uint16_t ibus_ma;
    uint16_t duty_permille;
    uint32_t target_power_mw;
    uint32_t meas_power_mw;
    const pd_caps_t *caps;
} pd_auto_boost_input_t;

/**
 * @brief Reset auto-boost saturation and cooldown counters.
 */
void pd_auto_boost_init(pd_auto_boost_t *ab);

/**
 * @brief Evaluate one periodic tick (50ms) of automatic upward PD voltage stepping.
 *
 * Steps up ONE supported PDO gear when PWM duty is persistently saturated (>=940pm),
 * measured power falls short (<92% of target), a real load is present (>=20mA),
 * no fault is latched, and the charger advertises a higher PDO.
 *
 * @return Next target voltage in mV (> current_mv) when stepping up, or 0 for no change.
 */
uint16_t pd_auto_boost_step(pd_auto_boost_t *ab, const pd_auto_boost_input_t *in);

typedef enum {
    PD_SAFE_DROP_NONE = 0,
    PD_SAFE_DROP_IDLE,
    PD_SAFE_DROP_FAULT
} pd_safe_drop_reason_t;

typedef struct {
    bool was_loaded;
    bool fault_handled;
    uint8_t idle_ticks;
} pd_safe_5v_t;

/**
 * @brief Initialize vSafe5V idle/fault fallback supervisor.
 */
void pd_safe_5v_init(pd_safe_5v_t *sf);

/**
 * @brief Notify supervisor that the user explicitly requested a manual voltage
 *        while idle so the rail will not be forced back to 5V.
 */
void pd_safe_5v_notify_manual_v(pd_safe_5v_t *sf);

/**
 * @brief Evaluate one periodic tick (50ms) of vSafe5V fallback protection.
 *
 * - Latched fault (>5V): drops immediately to 5V (PD_SAFE_DROP_FAULT).
 * - Active load turned off (target_power_mw == 0 while >5V): waits 500ms for
 *   PWM/current to settle to zero, then drops to 5V (PD_SAFE_DROP_IDLE).
 */
pd_safe_drop_reason_t pd_safe_5v_step(pd_safe_5v_t *sf,
                                      uint16_t active_mv,
                                      uint32_t target_power_mw,
                                      bool fault_active);

/**
 * @brief Execute one deterministic periodic step of the PD sink policy FSM.
 */
void pd_sink_step(pd_sink_fsm_t *fsm,
                  const pd_sink_input_t *in,
                  pd_sink_output_t *out);

#endif /* PD_SINK_POLICY_H */
