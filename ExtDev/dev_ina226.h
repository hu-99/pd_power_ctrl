#ifndef DEV_INA226_H
#define DEV_INA226_H

#include "i2c_bus.h"
#include <stdbool.h>
#include <stdint.h>

#define INA226_I2C_ADDR 0x40U

typedef enum {
    INA_CHIP_NONE = 0,
    INA_CHIP_226,
    INA_CHIP_219
} ina_chip_t;

typedef struct {
    uint16_t vbus_mv;
    uint16_t ibus_ma;
    uint32_t power_mw;
    int32_t shunt_uv;
} ina226_sample_t;

/**
 * @brief Return the INA chip model auto-detected during the last ina226_init().
 */
ina_chip_t ina226_detected_chip(void);

/**
 * @brief Probe INA226 presence on I2C bus.
 */
bool ina226_probe(const i2c_bus_ops_t *bus);

/**
 * @brief Configure INA226 averaging/conversion mode, calibration register, and
 *        hardware ALERT pin threshold for shunt over-current (active-low latched).
 * @param r_shunt_mohm   Board shunt resistor in milliohms (e.g. 100 for R100).
 * @param alert_ocp_ma   Hardware over-current alert threshold in mA (0 disables alert).
 */
bool ina226_init(const i2c_bus_ops_t *bus,
                 uint16_t r_shunt_mohm,
                 uint16_t alert_ocp_ma);

/**
 * @brief Read bus voltage and shunt voltage via I2C and compute integer mV/mA/mW.
 */
bool ina226_read_sample(const i2c_bus_ops_t *bus,
                        uint16_t r_shunt_mohm,
                        ina226_sample_t *out);

/**
 * @brief Clear latched INA226 ALERT flag by reading Mask/Enable register.
 */
bool ina226_clear_alert(const i2c_bus_ops_t *bus);

#endif /* DEV_INA226_H */
