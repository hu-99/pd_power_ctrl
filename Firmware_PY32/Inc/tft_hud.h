#ifndef TFT_HUD_H
#define TFT_HUD_H

#include <stdbool.h>
#include <stdint.h>

/*
 * Vendor BOE 0.96" IPS 80x160 ST7735S panel defaults (UN096T-16080-RGB-8):
 * - Landscape 160x80 uses MADCTL=0x78 (or 0xA8 if rotated 180 deg)
 * - Active GRAM offset in landscape mode: X=0, Y=24
 * - BOE panel uses normal color polarity (0x21 inversion disabled by default)
 */
#define TFT_WIDTH          160U
#define TFT_HEIGHT         80U
#define TFT_X_OFFSET       0U
#define TFT_Y_OFFSET       24U
#define TFT_ROTATE_180     0U
#define TFT_INVERT_COLORS  0U

typedef struct {
    bool live_mode;           /* true: [LIVE] INA226 online, false: [SIM] */
    uint16_t pd_target_mv;    /* Negotiated / requested PD voltage (mV)   */
    uint16_t vbus_mv;         /* Measured bus voltage (mV)                */
    uint16_t ibus_ma;         /* Measured bus current (mA)                */
    uint32_t power_mw;        /* Measured output power (mW)               */
    uint16_t duty_permille;   /* PWM duty cycle [0..1000]                 */
    int16_t mcu_temp_c;       /* Internal MCU temperature (degC)          */
    uint8_t prot_fault;       /* prot_fault_t code (0 = NONE)             */
} tft_hud_state_t;

/**
 * @brief Reset & initialize ST7735S (BOE 0.96" IPS 160x80) and paint the 6.25KB BC1 HUD background.
 * @note  Call from a FreeRTOS task context (uses vTaskDelay for ST7735S reset / sleep-out timing).
 */
void tft_hud_init(void);

/**
 * @brief Incrementally refresh only the dirty telemetry regions on the 160x80 cyber HUD.
 */
void tft_hud_update(const tft_hud_state_t *st);

#endif /* TFT_HUD_H */
