#ifndef DEV_HUSB238_H
#define DEV_HUSB238_H

#include "i2c_bus.h"
#include "pd_sink_policy.h"
#include <stdbool.h>
#include <stdint.h>

#define HUSB238_I2C_ADDR 0x08U

/**
 * @brief Probe HUSB238 presence on I2C bus.
 */
bool husb238_probe(const i2c_bus_ops_t *bus);

/**
 * @brief Read attach state, all 6 charger PDO tiers (5V..20V), and active contract voltage.
 */
bool husb238_read_caps(const i2c_bus_ops_t *bus,
                       pd_caps_t *caps_out,
                       uint16_t *active_mv_out);

/**
 * @brief Select a target PDO profile (SRC_PDO 0x08) and issue GO_COMMAND (0x09 = 0x01).
 */
bool husb238_request_profile(const i2c_bus_ops_t *bus, pd_profile_idx_t profile);

#endif /* DEV_HUSB238_H */
