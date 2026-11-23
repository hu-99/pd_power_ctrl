#include "dev_husb238.h"
#include "dev_ina226.h"
#include "test_framework.h"
#include <string.h>

typedef struct {
    uint8_t husb_regs[16];
    uint16_t ina_regs[8];
    bool husb_present;
    bool ina_present;
    bool ina_is_219;
} mock_i2c_bus_t;

static bool mock_write_reg(void *ctx,
                           uint8_t addr7,
                           uint8_t reg,
                           const uint8_t *buf,
                           uint8_t len) {
    mock_i2c_bus_t *m = (mock_i2c_bus_t *)ctx;
    if (addr7 == HUSB238_I2C_ADDR) {
        if (!m->husb_present || ((uint16_t)reg + len > 16U)) {
            return false;
        }
        memcpy(&m->husb_regs[reg], buf, len);
        return true;
    }
    if (addr7 == INA226_I2C_ADDR) {
        if (!m->ina_present || (len != 2U) || (reg >= 8U)) {
            return false;
        }
        m->ina_regs[reg] = (uint16_t)(((uint16_t)buf[0] << 8U) | buf[1]);
        return true;
    }
    return false;
}

static bool mock_read_reg(void *ctx,
                          uint8_t addr7,
                          uint8_t reg,
                          uint8_t *buf,
                          uint8_t len) {
    mock_i2c_bus_t *m = (mock_i2c_bus_t *)ctx;
    if (addr7 == HUSB238_I2C_ADDR) {
        if (!m->husb_present || ((uint16_t)reg + len > 16U)) {
            return false;
        }
        memcpy(buf, &m->husb_regs[reg], len);
        return true;
    }
    if (addr7 == INA226_I2C_ADDR) {
        if (!m->ina_present || (len != 2U)) {
            return false;
        }
        if (reg == 0xFEU) {
            if (m->ina_is_219) {
                return false; /* INA219 has no 0xFE manufacturer ID register */
            }
            buf[0] = 0x54U;
            buf[1] = 0x49U;
            return true;
        }
        if (reg >= 8U) {
            return false;
        }
        buf[0] = (uint8_t)(m->ina_regs[reg] >> 8U);
        buf[1] = (uint8_t)(m->ina_regs[reg] & 0xFFU);
        return true;
    }
    return false;
}

static int test_husb238_reads_caps_and_requests_pdo(void) {
    mock_i2c_bus_t mock = {0};
    i2c_bus_ops_t bus = {mock_write_reg, mock_read_reg, &mock};
    pd_caps_t caps = {0};
    uint16_t active_mv = 0U;

    mock.husb_present = true;
    mock.husb_regs[0x00] = 0x1AU; /* Active 5V (1), 3.0A */
    mock.husb_regs[0x01] = 0x40U; /* Attached */
    mock.husb_regs[0x02] = 0x8AU; /* 5V  detected, 3.0A (0x0A) */
    mock.husb_regs[0x03] = 0x8AU; /* 9V  detected, 3.0A */
    mock.husb_regs[0x04] = 0x8AU; /* 12V detected, 3.0A */
    mock.husb_regs[0x07] = 0x8BU; /* 20V detected, 3.25A (0x0B) */

    TEST_ASSERT_TRUE(husb238_probe(&bus));
    TEST_ASSERT_TRUE(husb238_read_caps(&bus, &caps, &active_mv));
    TEST_ASSERT_TRUE(caps.attached);
    TEST_ASSERT_EQ_INT(5000U, active_mv);
    TEST_ASSERT_TRUE(caps.supported[PD_PROFILE_12V]);
    TEST_ASSERT_TRUE(!caps.supported[PD_PROFILE_15V]);
    TEST_ASSERT_EQ_INT(3250U, caps.max_current_ma[PD_PROFILE_20V]);

    TEST_ASSERT_TRUE(husb238_request_profile(&bus, PD_PROFILE_20V));
    TEST_ASSERT_EQ_INT(0xA0U, mock.husb_regs[0x08]);
    TEST_ASSERT_EQ_INT(0x01U, mock.husb_regs[0x09]);
    return 0;
}

static int test_ina226_init_and_conversion_math(void) {
    mock_i2c_bus_t mock = {0};
    i2c_bus_ops_t bus = {mock_write_reg, mock_read_reg, &mock};
    ina226_sample_t sample = {0};

    mock.ina_present = true;
    TEST_ASSERT_TRUE(ina226_probe(&bus));
    TEST_ASSERT_TRUE(ina226_init(&bus, 100U, 780U));

    /* Cal = 204800 / 100 = 2048 (0x0800); Alert = 780 * 100 * 2 / 5 = 31200 */
    TEST_ASSERT_EQ_INT(0x4327U, mock.ina_regs[0x00]);
    TEST_ASSERT_EQ_INT(2048U, mock.ina_regs[0x05]);
    TEST_ASSERT_EQ_INT(0x8001U, mock.ina_regs[0x06]);
    TEST_ASSERT_EQ_INT(31200U, mock.ina_regs[0x07]);

    /* Simulate VBUS = 20000mV (raw = 16000), I = 500mA over 0.1 ohm -> 50mV (raw = 20000) */
    mock.ina_regs[0x01] = 20000U;
    mock.ina_regs[0x02] = 16000U;
    TEST_ASSERT_TRUE(ina226_read_sample(&bus, 100U, &sample));
    TEST_ASSERT_EQ_INT(INA_CHIP_226, (int)ina226_detected_chip());
    TEST_ASSERT_EQ_INT(20000U, sample.vbus_mv);
    TEST_ASSERT_EQ_INT(500U, sample.ibus_ma);
    TEST_ASSERT_EQ_INT(10000U, sample.power_mw);
    return 0;
}

static int test_ina219_auto_detect_and_conversion_math(void) {
    mock_i2c_bus_t mock = {0};
    i2c_bus_ops_t bus = {mock_write_reg, mock_read_reg, &mock};
    ina226_sample_t sample = {0};

    mock.ina_present = true;
    mock.ina_is_219 = true;
    TEST_ASSERT_TRUE(ina226_probe(&bus));
    TEST_ASSERT_TRUE(ina226_init(&bus, 100U, 780U));
    TEST_ASSERT_EQ_INT(INA_CHIP_219, (int)ina226_detected_chip());
    TEST_ASSERT_EQ_INT(0x3D67U, mock.ina_regs[0x00]);
    TEST_ASSERT_EQ_INT(4096U, mock.ina_regs[0x05]);

    /* VBUS = 20000mV -> (5000 << 3) | CNVR(0x02) = 0x9C42; I = 570mA over R100 -> 57mV -> raw=5700 */
    mock.ina_regs[0x01] = 5700U;
    mock.ina_regs[0x02] = (uint16_t)((5000U << 3U) | 0x02U);
    TEST_ASSERT_TRUE(ina226_read_sample(&bus, 100U, &sample));
    TEST_ASSERT_EQ_INT(20000U, sample.vbus_mv);
    TEST_ASSERT_EQ_INT(570U, sample.ibus_ma);
    TEST_ASSERT_EQ_INT(11400U, sample.power_mw);
    TEST_ASSERT_TRUE(ina226_clear_alert(&bus));
    return 0;
}

static int test_missing_i2c_device_fails_cleanly(void) {
    mock_i2c_bus_t mock = {0};
    i2c_bus_ops_t bus = {mock_write_reg, mock_read_reg, &mock};
    ina226_sample_t sample = {0};

    TEST_ASSERT_TRUE(!husb238_probe(&bus));
    TEST_ASSERT_TRUE(!ina226_probe(&bus));
    TEST_ASSERT_TRUE(!ina226_init(&bus, 100U, 780U));
    TEST_ASSERT_TRUE(!ina226_read_sample(&bus, 100U, &sample));
    return 0;
}

int run_extdev_driver_tests(void) {
    RUN_TEST(test_husb238_reads_caps_and_requests_pdo);
    RUN_TEST(test_ina226_init_and_conversion_math);
    RUN_TEST(test_ina219_auto_detect_and_conversion_math);
    RUN_TEST(test_missing_i2c_device_fails_cleanly);
    return 0;
}
