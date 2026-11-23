#ifndef BSP_BOARD_H
#define BSP_BOARD_H

#include "fault_blackbox.h"
#include "i2c_bus.h"
#include <stdbool.h>
#include <stdint.h>

#define BSP_PWM_FREQ_HZ   20000U
#define BSP_PWM_PERIOD    1200U /* 24MHz / 20kHz = 1200 ticks */

/* Reset cause bitmask decoded from RCC_CSR at boot */
#define BSP_RST_PWR   (1U << 0)
#define BSP_RST_PIN   (1U << 1)
#define BSP_RST_SFT   (1U << 2)
#define BSP_RST_IWDG  (1U << 3)
#define BSP_RST_WWDG  (1U << 4)

/* Top-of-SRAM (0x20001FE0..0x20001FFF, excluded from IRAM1=0x1FE0) breadcrumb */
#define BSP_CRUMB_ADDR   0x20001FE0UL
#define BSP_CRUMB_MAGIC  0x57444743UL /* "WDGC" */

/* Last 2 x 128-byte Flash pages of PY32F003x8 (excluded from IROM1=0xFF00) */
#define BSP_BBOX_SLOT0_ADDR  0x0800FF00UL
#define BSP_BBOX_SLOT1_ADDR  0x0800FF80UL

#define BSP_ADC_DMA_FRAMES   4U
#define BSP_ADC_DMA_CHANS    2U
#define BSP_ADC_DMA_LEN      (BSP_ADC_DMA_FRAMES * BSP_ADC_DMA_CHANS)

typedef struct {
    uint32_t magic;
    uint32_t uptime_ms;
    uint16_t vbus_mv;
    uint16_t ibus_ma;
    uint16_t duty_permille;
    uint8_t alive_mask;
    uint8_t flags;
    uint32_t crc32;
} bsp_wdg_crumb_t;

/**
 * @brief Configure 24MHz HSI system clock, capture reset flags, PB5 LED,
 *        PA2/PA3 USART2 (115200 + RX IRQ), PA0 TIM1_CH3 20kHz PWM, PB6/PB7 I2C1, and PA4 ALERT.
 */
void bsp_board_init(void);

/**
 * @brief Return reset cause bitmask (BSP_RST_*) latched during bsp_board_init().
 */
uint8_t bsp_get_reset_cause_mask(void);

/**
 * @brief Initialize IWDG (~2.0s timeout on LSI/32) and freeze it while halted under debugger.
 */
void bsp_iwdg_init(void);

/**
 * @brief Reload the hardware independent watchdog counter.
 */
void bsp_iwdg_feed(void);

/**
 * @brief Read and invalidate the NOINIT top-of-SRAM watchdog breadcrumb.
 * @return true if a valid CRC-checked breadcrumb survived warm reset.
 */
bool bsp_crumb_consume(bsp_wdg_crumb_t *out_crumb);

/**
 * @brief Write an updated CRC-protected telemetry breadcrumb to top-of-SRAM NOINIT area.
 */
void bsp_crumb_update(uint32_t uptime_ms,
                      uint16_t vbus_mv,
                      uint16_t ibus_ma,
                      uint16_t duty_permille,
                      uint8_t alive_mask,
                      uint8_t flags);

/**
 * @brief Return static Flash backend operations for dual-page blackbox storage.
 */
const bbox_flash_ops_t *bsp_bbox_flash_ops(void);

/**
 * @brief Read averaged circular-DMA ADC telemetry (VDDA in mV and internal MCU temp in degC).
 * @return true when valid VREFINT sample is present in the DMA buffer.
 */
bool bsp_adc_read_telemetry(uint16_t *out_vdda_mv, int16_t *out_temp_c);

/**
 * @brief Transmit a signed 32-bit decimal integer over USART2.
 */
void bsp_uart_put_i32(int32_t val);

/**
 * @brief Toggle PB5 onboard LED (active-low on Start Kit V1.1).
 */
void bsp_led_toggle(void);

/**
 * @brief Update PA0 20kHz PWM duty cycle in permille [0..1000].
 */
void bsp_pwm_set_duty_permille(uint16_t duty_permille);

/**
 * @brief Return true when INA226 ALE pin (PA4, active-low) is asserted.
 */
bool bsp_alert_is_active(void);

/**
 * @brief Return pointer to static hardware I2C1 bus operations struct.
 */
const i2c_bus_ops_t *bsp_i2c1_bus(void);

/**
 * @brief Transmit a null-terminated string over USART2 (PA2 TX).
 */
void bsp_uart_puts(const char *str);

/**
 * @brief Transmit an unsigned 32-bit decimal integer over USART2.
 */
void bsp_uart_put_u32(uint32_t val);

/**
 * @brief Non-blocking check for a completed CRLF/LF command line from USART2 RX ring buffer.
 * @return true when a full null-terminated line is ready in out_buf.
 */
bool bsp_uart_pop_line(char *out_buf, uint8_t max_len);

/**
 * @brief Control 0.96" TFT RES (PA1), DC (PA6), and CS (PA12) pins.
 */
void bsp_tft_set_res(bool high);
void bsp_tft_set_dc(bool is_data);
void bsp_tft_set_cs(bool high);

/**
 * @brief Send a single command/parameter byte over SPI1 (PA5 SCK, PA7 MOSI, 12MHz).
 */
void bsp_tft_tx_byte(uint8_t byte);

/**
 * @brief Burst-send a pixel buffer over SPI1 via DMA1_Channel2 (non-blocking hardware shift).
 */
void bsp_tft_tx_dma(const uint8_t *buf, uint16_t len);

#endif /* BSP_BOARD_H */
