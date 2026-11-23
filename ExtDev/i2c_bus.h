#ifndef I2C_BUS_H
#define I2C_BUS_H

#include <stdbool.h>
#include <stdint.h>

/**
 * @brief Hardware-agnostic I2C register bus operations for dependency injection
 *        (allows testing ExtDev drivers on PC host without MCU hardware).
 */
typedef struct {
    bool (*write_reg)(void *ctx,
                      uint8_t addr7,
                      uint8_t reg,
                      const uint8_t *buf,
                      uint8_t len);
    bool (*read_reg)(void *ctx,
                     uint8_t addr7,
                     uint8_t reg,
                     uint8_t *buf,
                     uint8_t len);
    void *ctx;
} i2c_bus_ops_t;

#endif /* I2C_BUS_H */
