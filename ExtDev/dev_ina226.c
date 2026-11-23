#include "dev_ina226.h"
#include <stddef.h>

#define REG_CONFIG      0x00U
#define REG_SHUNT_V     0x01U
#define REG_BUS_V       0x02U
#define REG_CALIB       0x05U
#define REG_MASK_EN     0x06U
#define REG_ALERT_LIM   0x07U
#define REG_MFG_ID      0xFEU

#define INA226_MFG_ID_TI      0x5449U

/* INA226: AVG=4, VBUSCT=1.1ms, VSHCT=1.1ms, continuous shunt+bus */
#define INA226_CFG_CONT_4AVG  0x4327U
/* INA226 Mask/Enable: Bit15 SOL, Bit0 LEN, APOL=0 (active low) */
#define INA226_MASK_SOL_LATCH 0x8001U

/* INA219: BRNG=32V (1<<13), PG=/8 +-320mV (3<<11), BADC=4avg (0xA<<7, 2.13ms),
 *         SADC=16avg (0xC<<3, 8.51ms), cont (7) -> 10.64ms cycle (< 20ms PID tick) */
#define INA219_CFG_32V_16AVG  0x3D67U

static ina_chip_t s_detected_chip = INA_CHIP_NONE;

static bool write_u16_be(const i2c_bus_ops_t *bus, uint8_t reg, uint16_t val) {
    uint8_t buf[2];
    buf[0] = (uint8_t)(val >> 8U);
    buf[1] = (uint8_t)(val & 0xFFU);
    return bus->write_reg(bus->ctx, INA226_I2C_ADDR, reg, buf, 2U);
}

static bool read_u16_be(const i2c_bus_ops_t *bus, uint8_t reg, uint16_t *val) {
    uint8_t buf[2] = {0U, 0U};
    if (!bus->read_reg(bus->ctx, INA226_I2C_ADDR, reg, buf, 2U)) {
        return false;
    }
    *val = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
    return true;
}

ina_chip_t ina226_detected_chip(void) {
    return s_detected_chip;
}

bool ina226_probe(const i2c_bus_ops_t *bus) {
    uint16_t cfg = 0U;
    if ((bus == NULL) || (bus->read_reg == NULL)) {
        return false;
    }
    return read_u16_be(bus, REG_CONFIG, &cfg);
}

bool ina226_init(const i2c_bus_ops_t *bus,
                 uint16_t r_shunt_mohm,
                 uint16_t alert_ocp_ma) {
    uint16_t mfg_id = 0U;
    uint16_t cfg_check = 0U;

    if ((bus == NULL) || (bus->write_reg == NULL) ||
        (bus->read_reg == NULL) || (r_shunt_mohm == 0U)) {
        s_detected_chip = INA_CHIP_NONE;
        return false;
    }

    if (!read_u16_be(bus, REG_CONFIG, &cfg_check)) {
        s_detected_chip = INA_CHIP_NONE;
        return false;
    }

    /* Distinguish INA226 (has 0xFE == 0x5449 "TI") from INA219 (no 0xFE register). */
    if (read_u16_be(bus, REG_MFG_ID, &mfg_id) && (mfg_id == INA226_MFG_ID_TI)) {
        uint32_t cal;
        if (!write_u16_be(bus, REG_CONFIG, INA226_CFG_CONT_4AVG)) {
            s_detected_chip = INA_CHIP_NONE;
            return false;
        }

        /* Current_LSB = 25uA -> Cal = 0.00512 / (25e-6 * R_ohm) = 204800 / R_mohm */
        cal = 204800UL / r_shunt_mohm;
        if (cal > 0x7FFFUL) {
            cal = 0x7FFFUL;
        }
        if (!write_u16_be(bus, REG_CALIB, (uint16_t)cal)) {
            s_detected_chip = INA_CHIP_NONE;
            return false;
        }

        if (alert_ocp_ma > 0U) {
            /* V_shunt(uV) = I_ma * R_mohm; Alert_LSB = 2.5uV -> reg = (I_ma * R_mohm * 2) / 5 */
            uint32_t lim = ((uint32_t)alert_ocp_ma * r_shunt_mohm * 2UL) / 5UL;
            if (lim > 0x7FFFUL) {
                lim = 0x7FFFUL;
            }
            if (!write_u16_be(bus, REG_ALERT_LIM, (uint16_t)lim) ||
                !write_u16_be(bus, REG_MASK_EN, INA226_MASK_SOL_LATCH)) {
                s_detected_chip = INA_CHIP_NONE;
                return false;
            }
        } else {
            if (!write_u16_be(bus, REG_MASK_EN, 0x0000U)) {
                s_detected_chip = INA_CHIP_NONE;
                return false;
            }
        }

        s_detected_chip = INA_CHIP_226;
        return true;
    }

    /* Fallback to INA219: 32V range, +-320mV PGA, 4-sample 12-bit continuous conversion. */
    {
        uint32_t cal = (409600UL / r_shunt_mohm) & 0xFFFEUL;
        if (cal > 0xFFFEUL) {
            cal = 0xFFFEUL;
        }
        if (!write_u16_be(bus, REG_CONFIG, INA219_CFG_32V_16AVG) ||
            !write_u16_be(bus, REG_CALIB, (uint16_t)cal)) {
            s_detected_chip = INA_CHIP_NONE;
            return false;
        }
        s_detected_chip = INA_CHIP_219;
        return true;
    }
}

bool ina226_read_sample(const i2c_bus_ops_t *bus,
                        uint16_t r_shunt_mohm,
                        ina226_sample_t *out) {
    uint16_t raw_shunt_u = 0U;
    uint16_t raw_bus = 0U;
    int16_t raw_shunt;
    int32_t shunt_uv;
    int32_t abs_shunt_uv;

    if ((bus == NULL) || (bus->read_reg == NULL) ||
        (out == NULL) || (r_shunt_mohm == 0U) ||
        (s_detected_chip == INA_CHIP_NONE)) {
        return false;
    }

    if (!read_u16_be(bus, REG_SHUNT_V, &raw_shunt_u) ||
        !read_u16_be(bus, REG_BUS_V, &raw_bus)) {
        return false;
    }

    raw_shunt = (int16_t)raw_shunt_u;
    if (s_detected_chip == INA_CHIP_219) {
        /* INA219: Shunt LSB = 10uV; Bus voltage is bits [15:3] with 4mV LSB. */
        shunt_uv = (int32_t)raw_shunt * 10;
        out->vbus_mv = (uint16_t)(((uint32_t)(raw_bus >> 3U)) * 4UL);
    } else {
        /* INA226: Shunt LSB = 2.5uV; Bus LSB = 1.25mV (= 5/4 mV). */
        shunt_uv = ((int32_t)raw_shunt * 5) / 2;
        out->vbus_mv = (uint16_t)(((uint32_t)raw_bus * 5UL) / 4UL);
    }

    abs_shunt_uv = (shunt_uv >= 0) ? shunt_uv : -shunt_uv;
    out->shunt_uv = shunt_uv;
    out->ibus_ma = (uint16_t)(((uint32_t)abs_shunt_uv + (r_shunt_mohm / 2U)) / r_shunt_mohm);
    out->power_mw = ((uint32_t)out->vbus_mv * (uint32_t)out->ibus_ma) / 1000UL;
    return true;
}

bool ina226_clear_alert(const i2c_bus_ops_t *bus) {
    uint16_t mask_val = 0U;
    if ((bus == NULL) || (bus->read_reg == NULL)) {
        return false;
    }
    if (s_detected_chip == INA_CHIP_219) {
        return true; /* INA219 has no hardware ALERT latch register */
    }
    return read_u16_be(bus, REG_MASK_EN, &mask_val);
}
