#include "dev_husb238.h"
#include <stddef.h>

#define REG_PD_STATUS0   0x00U
#define REG_PD_STATUS1   0x01U
#define REG_SRC_PDO_5V   0x02U
#define REG_SRC_PDO_SEL  0x08U
#define REG_GO_COMMAND   0x09U

#define STATUS1_ATTACH_MASK  0x40U
#define PDO_DETECTED_MASK    0x80U
#define PDO_CURRENT_MASK     0x0FU

#define GO_CMD_REQUEST_PDO   0x01U

/* Current rating lookup table from HUSB238 datasheet bits [3:0] (in mA). */
static const uint16_t k_curr_ma_lut[16] = {
    500U,  700U,  1000U, 1250U, 1500U, 1750U, 2000U, 2250U,
    2500U, 2750U, 3000U, 3250U, 3500U, 4000U, 4500U, 5000U
};

/* Maps pd_profile_idx_t (5/9/12/15/18/20V) -> HUSB238 SRC_PDO[7:4] code. */
static const uint8_t k_pdo_select_code[PD_SINK_PROFILE_COUNT] = {
    0x10U, /* 5V  -> 0001 */
    0x20U, /* 9V  -> 0010 */
    0x30U, /* 12V -> 0011 */
    0x80U, /* 15V -> 1000 */
    0x90U, /* 18V -> 1001 */
    0xA0U  /* 20V -> 1010 */
};

static uint16_t decode_status0_mv(uint8_t status0) {
    uint8_t code = (uint8_t)((status0 >> 4U) & 0x0FU);
    switch (code) {
    case 1U: return 5000U;
    case 2U: return 9000U;
    case 3U: return 12000U;
    case 4U: return 15000U;
    case 5U: return 18000U;
    case 6U: return 20000U;
    default: return 0U;
    }
}

bool husb238_probe(const i2c_bus_ops_t *bus) {
    uint8_t val = 0U;
    if ((bus == NULL) || (bus->read_reg == NULL)) {
        return false;
    }
    return bus->read_reg(bus->ctx, HUSB238_I2C_ADDR, REG_PD_STATUS1, &val, 1U);
}

bool husb238_read_caps(const i2c_bus_ops_t *bus,
                       pd_caps_t *caps_out,
                       uint16_t *active_mv_out) {
    uint8_t st0 = 0U;
    uint8_t st1 = 0U;

    if ((bus == NULL) || (bus->read_reg == NULL) || (caps_out == NULL)) {
        return false;
    }

    if (!bus->read_reg(bus->ctx, HUSB238_I2C_ADDR, REG_PD_STATUS0, &st0, 1U) ||
        !bus->read_reg(bus->ctx, HUSB238_I2C_ADDR, REG_PD_STATUS1, &st1, 1U)) {
        return false;
    }

    caps_out->attached = ((st1 & STATUS1_ATTACH_MASK) != 0U);
    if (active_mv_out != NULL) {
        *active_mv_out = caps_out->attached ? decode_status0_mv(st0) : 0U;
    }

    for (uint8_t i = 0U; i < PD_SINK_PROFILE_COUNT; i++) {
        uint8_t pdo_reg = 0U;
        if (!bus->read_reg(bus->ctx, HUSB238_I2C_ADDR,
                           (uint8_t)(REG_SRC_PDO_5V + i), &pdo_reg, 1U)) {
            return false;
        }
        if ((pdo_reg & PDO_DETECTED_MASK) != 0U) {
            caps_out->supported[i] = true;
            caps_out->max_current_ma[i] = k_curr_ma_lut[pdo_reg & PDO_CURRENT_MASK];
        } else {
            caps_out->supported[i] = false;
            caps_out->max_current_ma[i] = 0U;
        }
    }
    return true;
}

bool husb238_request_profile(const i2c_bus_ops_t *bus, pd_profile_idx_t profile) {
    uint8_t sel_val;
    uint8_t cmd_val = GO_CMD_REQUEST_PDO;

    if ((bus == NULL) || (bus->write_reg == NULL) ||
        ((uint8_t)profile >= PD_SINK_PROFILE_COUNT)) {
        return false;
    }

    sel_val = k_pdo_select_code[(uint8_t)profile];
    if (!bus->write_reg(bus->ctx, HUSB238_I2C_ADDR, REG_SRC_PDO_SEL, &sel_val, 1U)) {
        return false;
    }
    return bus->write_reg(bus->ctx, HUSB238_I2C_ADDR, REG_GO_COMMAND, &cmd_val, 1U);
}
