#include "tft_hud.h"

#include "FreeRTOS.h"
#include "bc1_decode.h"
#include "bg_cyber_mech.h"
#include "bsp_board.h"
#include "prot_ovp_ocp.h"
#include "task.h"

#include <stddef.h>
#include <string.h>

/* Strip buffer: 160 x 4 pixels = 640 RGB565 pixels = 1,280 bytes SRAM. */
#define TFT_STRIP_PIXELS  (TFT_WIDTH * 4U)
static uint16_t s_strip_buf[TFT_STRIP_PIXELS];

/* RGB565 little-endian colors -> byte-swapped for direct 8-bit SPI DMA output. */
#define RGB565_BE(r8, g8, b8) \
    ((uint16_t)((((((uint16_t)(r8) & 0xF8U) << 8) | (((uint16_t)(g8) & 0xFCU) << 3) | ((uint16_t)(b8) >> 3)) >> 8) | \
                (((((uint16_t)(r8) & 0xF8U) << 8) | (((uint16_t)(g8) & 0xFCU) << 3) | ((uint16_t)(b8) >> 3)) << 8)))

#define COL_WHITE_BE   RGB565_BE(235, 252, 255)
#define COL_CYAN_BE    RGB565_BE(0,   238, 255)
#define COL_GREEN_BE   RGB565_BE(50,  255, 130)
#define COL_AMBER_BE   RGB565_BE(255, 195, 50)
#define COL_RED_BE     RGB565_BE(255, 55,  85)
#define COL_PINK_BE    RGB565_BE(255, 60,  155)
#define COL_SOFTRED_BE RGB565_BE(255, 150, 165)
#define COL_LABEL_BE   RGB565_BE(120, 155, 195)
#define COL_MUTED_BE   RGB565_BE(150, 165, 190)
#define COL_BAR_OFF_BE RGB565_BE(20,  28,  52)

typedef struct {
    char ch;
    uint8_t rows[7]; /* 5-bit row bitmask in bits [4:0], MSB on left */
} glyph_5x7_t;

static const glyph_5x7_t s_font_5x7[] = {
    {'0', {0x0EU, 0x11U, 0x13U, 0x15U, 0x19U, 0x11U, 0x0EU}},
    {'1', {0x04U, 0x0CU, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU}},
    {'2', {0x0EU, 0x11U, 0x01U, 0x06U, 0x08U, 0x10U, 0x1FU}},
    {'3', {0x1EU, 0x01U, 0x01U, 0x0EU, 0x01U, 0x01U, 0x1EU}},
    {'4', {0x02U, 0x06U, 0x0AU, 0x12U, 0x1FU, 0x02U, 0x02U}},
    {'5', {0x1FU, 0x10U, 0x1EU, 0x01U, 0x01U, 0x11U, 0x0EU}},
    {'6', {0x0EU, 0x10U, 0x1EU, 0x11U, 0x11U, 0x11U, 0x0EU}},
    {'7', {0x1FU, 0x01U, 0x02U, 0x04U, 0x08U, 0x08U, 0x08U}},
    {'8', {0x0EU, 0x11U, 0x11U, 0x0EU, 0x11U, 0x11U, 0x0EU}},
    {'9', {0x0EU, 0x11U, 0x11U, 0x0FU, 0x01U, 0x02U, 0x0CU}},
    {'.', {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x0CU, 0x0CU}},
    {':', {0x00U, 0x0CU, 0x0CU, 0x00U, 0x0CU, 0x0CU, 0x00U}},
    {'-', {0x00U, 0x00U, 0x00U, 0x1FU, 0x00U, 0x00U, 0x00U}},
    {' ', {0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U, 0x00U}},
    {'A', {0x0EU, 0x11U, 0x11U, 0x1FU, 0x11U, 0x11U, 0x11U}},
    {'B', {0x1EU, 0x11U, 0x11U, 0x1EU, 0x11U, 0x11U, 0x1EU}},
    {'C', {0x0EU, 0x11U, 0x10U, 0x10U, 0x10U, 0x11U, 0x0EU}},
    {'D', {0x1CU, 0x12U, 0x11U, 0x11U, 0x11U, 0x12U, 0x1CU}},
    {'E', {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x1FU}},
    {'F', {0x1FU, 0x10U, 0x10U, 0x1EU, 0x10U, 0x10U, 0x10U}},
    {'I', {0x0EU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x0EU}},
    {'K', {0x11U, 0x12U, 0x14U, 0x18U, 0x14U, 0x12U, 0x11U}},
    {'L', {0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x10U, 0x1FU}},
    {'M', {0x11U, 0x1BU, 0x15U, 0x15U, 0x11U, 0x11U, 0x11U}},
    {'O', {0x0EU, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU}},
    {'P', {0x1EU, 0x11U, 0x11U, 0x1EU, 0x10U, 0x10U, 0x10U}},
    {'R', {0x1EU, 0x11U, 0x11U, 0x1EU, 0x14U, 0x12U, 0x11U}},
    {'S', {0x0FU, 0x10U, 0x10U, 0x0EU, 0x01U, 0x01U, 0x1EU}},
    {'T', {0x1FU, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U, 0x04U}},
    {'U', {0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x11U, 0x0EU}},
    {'V', {0x11U, 0x11U, 0x11U, 0x11U, 0x0AU, 0x0AU, 0x04U}},
    {'W', {0x11U, 0x11U, 0x11U, 0x15U, 0x15U, 0x1BU, 0x11U}}
};

typedef struct {
    char ch;
    uint8_t width;
    uint8_t rows[5]; /* LSB-aligned bitmask of `width` bits */
} glyph_3x5_t;

static const glyph_3x5_t s_font_3x5[] = {
    {'0', 3U, {0x07U, 0x05U, 0x05U, 0x05U, 0x07U}},
    {'1', 3U, {0x02U, 0x06U, 0x02U, 0x02U, 0x07U}},
    {'2', 3U, {0x07U, 0x01U, 0x07U, 0x04U, 0x07U}},
    {'3', 3U, {0x07U, 0x01U, 0x07U, 0x01U, 0x07U}},
    {'4', 3U, {0x05U, 0x05U, 0x07U, 0x01U, 0x01U}},
    {'5', 3U, {0x07U, 0x04U, 0x07U, 0x01U, 0x07U}},
    {'6', 3U, {0x07U, 0x04U, 0x07U, 0x05U, 0x07U}},
    {'7', 3U, {0x07U, 0x01U, 0x01U, 0x02U, 0x02U}},
    {'8', 3U, {0x07U, 0x05U, 0x07U, 0x05U, 0x07U}},
    {'9', 3U, {0x07U, 0x05U, 0x07U, 0x01U, 0x07U}},
    {'-', 3U, {0x00U, 0x00U, 0x07U, 0x00U, 0x00U}},
    {'C', 3U, {0x07U, 0x04U, 0x04U, 0x04U, 0x07U}},
    {'T', 3U, {0x07U, 0x02U, 0x02U, 0x02U, 0x02U}},
    {':', 1U, {0x00U, 0x01U, 0x00U, 0x01U, 0x00U}},
    {' ', 2U, {0x00U, 0x00U, 0x00U, 0x00U, 0x00U}}
};

typedef struct {
    bool valid;
    bool live_mode;
    uint8_t pd_v;
    uint8_t fault;
    uint16_t pwr_tenth_w;
    uint16_t vbus_tenth_v;
    uint16_t ibus_cma;
    uint8_t bar_segs;
    int16_t temp_c;
} hud_cache_t;

static hud_cache_t s_cache;

static void tft_cmd(uint8_t cmd) {
    bsp_tft_set_dc(false);
    bsp_tft_set_cs(false);
    bsp_tft_tx_byte(cmd);
    bsp_tft_set_cs(true);
    bsp_tft_set_dc(true);
}

static void tft_data8(uint8_t val) {
    bsp_tft_set_dc(true);
    bsp_tft_set_cs(false);
    bsp_tft_tx_byte(val);
    bsp_tft_set_cs(true);
}

static void tft_data16(uint16_t val) {
    bsp_tft_set_dc(true);
    bsp_tft_set_cs(false);
    bsp_tft_tx_byte((uint8_t)(val >> 8));
    bsp_tft_tx_byte((uint8_t)(val & 0xFFU));
    bsp_tft_set_cs(true);
}

static void tft_set_window(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    uint16_t x0 = (uint16_t)(x + TFT_X_OFFSET);
    uint16_t x1 = (uint16_t)(x0 + w - 1U);
    uint16_t y0 = (uint16_t)(y + TFT_Y_OFFSET);
    uint16_t y1 = (uint16_t)(y0 + h - 1U);

    tft_cmd(0x2AU);
    tft_data16(x0);
    tft_data16(x1);
    tft_cmd(0x2BU);
    tft_data16(y0);
    tft_data16(y1);
    tft_cmd(0x2CU);
}

static void tft_flush_rect_dma(uint16_t x, uint16_t y, uint16_t w, uint16_t h) {
    tft_set_window(x, y, w, h);
    bsp_tft_set_dc(true);
    bsp_tft_set_cs(false);
    bsp_tft_tx_dma((const uint8_t *)s_strip_buf, (uint16_t)((uint32_t)w * (uint32_t)h * 2UL));
    bsp_tft_set_cs(true);
}

static const uint8_t *lookup_5x7(char c) {
    uint32_t i;
    for (i = 0U; i < (sizeof(s_font_5x7) / sizeof(s_font_5x7[0])); i++) {
        if (s_font_5x7[i].ch == c) {
            return s_font_5x7[i].rows;
        }
    }
    return s_font_5x7[13].rows; /* space */
}

static const glyph_3x5_t *lookup_3x5(char c) {
    uint32_t i;
    for (i = 0U; i < (sizeof(s_font_3x5) / sizeof(s_font_3x5[0])); i++) {
        if (s_font_3x5[i].ch == c) {
            return &s_font_3x5[i];
        }
    }
    return &s_font_3x5[14]; /* space */
}

/*
 * Overlay 5x7 text into a strip buffer representing screen rect (bx, by, bw, bh).
 * Clips cleanly against the strip bounds so multi-strip big digits work seamlessly.
 */
static uint16_t strip_draw_text5x7(uint16_t bx,
                                   uint16_t by,
                                   uint16_t bw,
                                   uint16_t bh,
                                   uint16_t tx,
                                   uint16_t ty,
                                   const char *str,
                                   uint16_t color_be,
                                   uint8_t scale,
                                   uint8_t gap_px) {
    uint16_t cx = tx;

    while (*str != '\0') {
        char ch = *str++;
        const uint8_t *rows = lookup_5x7(ch);
        uint8_t gw = ((ch == '.') || (ch == ':')) ? 2U : 5U;
        uint8_t start_bit = ((ch == '.') || (ch == ':')) ? 3U : 4U;
        uint8_t r;

        for (r = 0U; r < 7U; r++) {
            uint8_t bits = rows[r];
            uint8_t c;
            for (c = 0U; c < gw; c++) {
                if ((bits & (uint8_t)(1U << (start_bit - c))) != 0U) {
                    uint8_t sy;
                    for (sy = 0U; sy < scale; sy++) {
                        uint16_t py = (uint16_t)(ty + ((uint16_t)r * scale) + sy);
                        if ((py >= by) && (py < (uint16_t)(by + bh))) {
                            uint8_t sx;
                            uint32_t row_off = (uint32_t)(py - by) * (uint32_t)bw;
                            for (sx = 0U; sx < scale; sx++) {
                                uint16_t px = (uint16_t)(cx + ((uint16_t)c * scale) + sx);
                                if ((px >= bx) && (px < (uint16_t)(bx + bw))) {
                                    s_strip_buf[row_off + (uint32_t)(px - bx)] = color_be;
                                }
                            }
                        }
                    }
                }
            }
        }
        cx = (uint16_t)(cx + ((uint16_t)gw * scale) + gap_px);
    }
    return cx;
}

static void strip_draw_text3x5(uint16_t bx,
                               uint16_t by,
                               uint16_t bw,
                               uint16_t bh,
                               uint16_t tx,
                               uint16_t ty,
                               const char *str,
                               uint16_t color_be) {
    uint16_t cx = tx;
    while (*str != '\0') {
        const glyph_3x5_t *g = lookup_3x5(*str++);
        uint8_t r;
        for (r = 0U; r < 5U; r++) {
            uint16_t py = (uint16_t)(ty + r);
            if ((py >= by) && (py < (uint16_t)(by + bh))) {
                uint32_t row_off = (uint32_t)(py - by) * (uint32_t)bw;
                uint8_t c;
                for (c = 0U; c < g->width; c++) {
                    if ((g->rows[r] & (uint8_t)(1U << (g->width - 1U - c))) != 0U) {
                        uint16_t px = (uint16_t)(cx + c);
                        if ((px >= bx) && (px < (uint16_t)(bx + bw))) {
                            s_strip_buf[row_off + (uint32_t)(px - bx)] = color_be;
                        }
                    }
                }
            }
        }
        cx = (uint16_t)(cx + g->width + 1U);
    }
}

static void strip_fill_rect(uint16_t bx,
                            uint16_t by,
                            uint16_t bw,
                            uint16_t bh,
                            uint16_t rx,
                            uint16_t ry,
                            uint16_t rw,
                            uint16_t rh,
                            uint16_t color_be) {
    uint16_t y;
    for (y = ry; y < (uint16_t)(ry + rh); y++) {
        if ((y >= by) && (y < (uint16_t)(by + bh))) {
            uint32_t row_off = (uint32_t)(y - by) * (uint32_t)bw;
            uint16_t x;
            for (x = rx; x < (uint16_t)(rx + rw); x++) {
                if ((x >= bx) && (x < (uint16_t)(bx + bw))) {
                    s_strip_buf[row_off + (uint32_t)(x - bx)] = color_be;
                }
            }
        }
    }
}

static void fmt_u2(char *dst, uint32_t val) {
    if (val > 99U) {
        val = 99U;
    }
    dst[0] = (char)('0' + (val / 10U));
    dst[1] = (char)('0' + (val % 10U));
}

static void draw_full_background(void) {
    uint16_t y;
    for (y = 0U; y < TFT_HEIGHT; y = (uint16_t)(y + 4U)) {
        (void)bc1_decode_rect(&g_bg_cyber_mech, 0U, y, TFT_WIDTH, 4U, s_strip_buf, true);
        tft_flush_rect_dma(0U, y, TFT_WIDTH, 4U);
    }
}

static void draw_header_zone(bool live_mode, uint8_t pd_v, uint8_t fault) {
    const uint16_t rx = 18U, ry = 15U, rw = 74U, rh = 7U;
    char left_str[8];

    (void)bc1_decode_rect(&g_bg_cyber_mech, rx, ry, rw, rh, s_strip_buf, true);

    if (fault != (uint8_t)PROT_FAULT_NONE) {
        const char *tag = "FLT:ERR";
        if (fault == (uint8_t)PROT_FAULT_UVP) {
            tag = "FLT:UVP";
        } else if (fault == (uint8_t)PROT_FAULT_OVP) {
            tag = "FLT:OVP";
        } else if (fault == (uint8_t)PROT_FAULT_OCP_SOFT) {
            tag = "FLT:OCP";
        } else if (fault == (uint8_t)PROT_FAULT_OCP_HARD) {
            tag = "FLT:ALE";
        } else if (fault == (uint8_t)PROT_FAULT_OTP) {
            tag = "FLT:OTP";
        }
        (void)strip_draw_text5x7(rx, ry, rw, rh, 18U, 15U, tag, COL_RED_BE, 1U, 1U);
        (void)strip_draw_text5x7(rx, ry, rw, rh, 66U, 15U, "LOCK", COL_AMBER_BE, 1U, 1U);
    } else {
        left_str[0] = 'P';
        left_str[1] = 'D';
        left_str[2] = ' ';
        fmt_u2(&left_str[3], pd_v);
        left_str[5] = 'V';
        left_str[6] = '\0';
        (void)strip_draw_text5x7(rx, ry, rw, rh, 18U, 15U, left_str, COL_CYAN_BE, 1U, 1U);
        (void)strip_draw_text5x7(rx, ry, rw, rh, live_mode ? 64U : 68U, 15U,
                                 live_mode ? "LIVE" : "SIM",
                                 live_mode ? COL_GREEN_BE : COL_AMBER_BE, 1U, 1U);
    }

    tft_flush_rect_dma(rx, ry, rw, rh);
}

static void draw_power_zone(uint16_t pwr_tenth_w, uint8_t fault) {
    /* Left hero readout area: x=17, y=28, w=74, h=21 -> sliced into 8 + 8 + 5 rows. */
    const uint16_t rx = 17U, ry = 28U, rw = 74U, rh = 21U;
    char num[5];
    uint16_t col_num = (fault != 0U) ? COL_RED_BE : COL_WHITE_BE;
    uint16_t col_w = (fault != 0U) ? COL_SOFTRED_BE : COL_CYAN_BE;
    uint16_t sy;

    if (pwr_tenth_w > 999U) {
        pwr_tenth_w = 999U;
    }
    fmt_u2(&num[0], (uint32_t)(pwr_tenth_w / 10U));
    num[2] = '.';
    num[3] = (char)('0' + (pwr_tenth_w % 10U));
    num[4] = '\0';

    for (sy = 0U; sy < rh; sy = (uint16_t)(sy + 8U)) {
        uint16_t sh = ((uint16_t)(rh - sy) > 8U) ? 8U : (uint16_t)(rh - sy);
        uint16_t end_x;
        (void)bc1_decode_rect(&g_bg_cyber_mech, rx, (uint16_t)(ry + sy), rw, sh, s_strip_buf, true);
        end_x = strip_draw_text5x7(rx, (uint16_t)(ry + sy), rw, sh, 17U, 28U, num, col_num, 3U, 2U);
        (void)strip_draw_text5x7(rx, (uint16_t)(ry + sy), rw, sh,
                                 (uint16_t)(end_x + 2U), 35U, "W", col_w, 2U, 1U);
        tft_flush_rect_dma(rx, (uint16_t)(ry + sy), rw, sh);
    }
}

static void draw_vbus_zone(uint16_t vbus_tenth_v, uint8_t fault) {
    const uint16_t rx = 102U, ry = 15U, rw = 40U, rh = 16U;
    char val_str[6];
    bool v_bad = (fault == (uint8_t)PROT_FAULT_OVP) || (fault == (uint8_t)PROT_FAULT_UVP);

    if (vbus_tenth_v > 999U) {
        vbus_tenth_v = 999U;
    }
    fmt_u2(&val_str[0], (uint32_t)(vbus_tenth_v / 10U));
    val_str[2] = '.';
    val_str[3] = (char)('0' + (vbus_tenth_v % 10U));
    val_str[4] = 'V';
    val_str[5] = '\0';

    (void)bc1_decode_rect(&g_bg_cyber_mech, rx, ry, rw, rh, s_strip_buf, true);
    (void)strip_draw_text5x7(rx, ry, rw, rh, 102U, 15U, "VBUS",
                             v_bad ? COL_SOFTRED_BE : COL_LABEL_BE, 1U, 1U);
    (void)strip_draw_text5x7(rx, ry, rw, rh, 102U, 24U, val_str,
                             v_bad ? COL_RED_BE : COL_CYAN_BE, 1U, 1U);
    tft_flush_rect_dma(rx, ry, rw, rh);
}

static void draw_ibus_zone(uint16_t ibus_cma, uint8_t fault) {
    const uint16_t rx = 102U, ry = 41U, rw = 40U, rh = 16U;
    char val_str[6];
    bool i_bad = (fault == (uint8_t)PROT_FAULT_OCP_SOFT) || (fault == (uint8_t)PROT_FAULT_OCP_HARD);

    if (ibus_cma > 999U) {
        ibus_cma = 999U;
    }
    val_str[0] = (char)('0' + (ibus_cma / 100U));
    val_str[1] = '.';
    val_str[2] = (char)('0' + ((ibus_cma / 10U) % 10U));
    val_str[3] = (char)('0' + (ibus_cma % 10U));
    val_str[4] = 'A';
    val_str[5] = '\0';

    (void)bc1_decode_rect(&g_bg_cyber_mech, rx, ry, rw, rh, s_strip_buf, true);
    (void)strip_draw_text5x7(rx, ry, rw, rh, 102U, 41U, "IBUS",
                             i_bad ? COL_SOFTRED_BE : COL_LABEL_BE, 1U, 1U);
    (void)strip_draw_text5x7(rx, ry, rw, rh, 102U, 50U, val_str,
                             i_bad ? COL_RED_BE : ((fault != 0U) ? COL_MUTED_BE : COL_AMBER_BE),
                             1U, 1U);
    tft_flush_rect_dma(rx, ry, rw, rh);
}

static void draw_bottom_zone(uint8_t bar_segs, int16_t temp_c, uint8_t fault) {
    const uint16_t rx = 18U, ry = 65U, rw = 128U, rh = 5U;
    char t_str[7];
    uint8_t i;
    int16_t clamped_t = temp_c;
    uint16_t t_col;

    (void)bc1_decode_rect(&g_bg_cyber_mech, rx, ry, rw, rh, s_strip_buf, true);

    /* 16 segments * (4px width + 2px gap) = 96px bar */
    for (i = 0U; i < 16U; i++) {
        uint16_t sx = (uint16_t)(18U + ((uint16_t)i * 6U));
        uint16_t col;
        if (fault != 0U) {
            col = COL_RED_BE;
        } else if (i < bar_segs) {
            if (i < 8U) {
                col = COL_CYAN_BE;
            } else if (i < 13U) {
                col = COL_GREEN_BE;
            } else {
                col = COL_PINK_BE;
            }
        } else {
            col = COL_BAR_OFF_BE;
        }
        strip_fill_rect(rx, ry, rw, rh, sx, 65U, 4U, 4U, col);
    }

    if (clamped_t < 0) {
        clamped_t = 0;
    } else if (clamped_t > 99) {
        clamped_t = 99;
    }
    t_str[0] = 'T';
    t_str[1] = ':';
    fmt_u2(&t_str[2], (uint32_t)clamped_t);
    t_str[4] = 'C';
    t_str[5] = '\0';

    if ((fault == (uint8_t)PROT_FAULT_OTP) || (clamped_t >= 85)) {
        t_col = COL_RED_BE;
    } else if (clamped_t >= 65) {
        t_col = COL_AMBER_BE;
    } else {
        t_col = COL_GREEN_BE;
    }
    strip_draw_text3x5(rx, ry, rw, rh, 120U, 65U, t_str, t_col);

    tft_flush_rect_dma(rx, ry, rw, rh);
}

void tft_hud_init(void) {
    memset(&s_cache, 0, sizeof(s_cache));

    /* Hardware reset pulse */
    bsp_tft_set_cs(true);
    bsp_tft_set_res(false);
    vTaskDelay(pdMS_TO_TICKS(50U));
    bsp_tft_set_res(true);
    vTaskDelay(pdMS_TO_TICKS(120U));

    /* Exact BOE UN096T-16080-RGB-8 ST7735S factory initialization sequence */
    tft_cmd(0x11U); /* Sleep out */
    vTaskDelay(pdMS_TO_TICKS(120U));

    tft_cmd(0xB1U); tft_data8(0x05U); tft_data8(0x3CU); tft_data8(0x3CU);
    tft_cmd(0xB2U); tft_data8(0x05U); tft_data8(0x3CU); tft_data8(0x3CU);
    tft_cmd(0xB3U);
    tft_data8(0x05U); tft_data8(0x3CU); tft_data8(0x3CU);
    tft_data8(0x05U); tft_data8(0x3CU); tft_data8(0x3CU);

    tft_cmd(0xB4U); tft_data8(0x03U); /* Dot inversion */

    tft_cmd(0xC0U); tft_data8(0xABU); tft_data8(0x0BU); tft_data8(0x04U);
    tft_cmd(0xC1U); tft_data8(0xC5U);
    tft_cmd(0xC2U); tft_data8(0x0DU); tft_data8(0x00U);
    tft_cmd(0xC3U); tft_data8(0x8DU); tft_data8(0x6AU);
    tft_cmd(0xC4U); tft_data8(0x8DU); tft_data8(0xEEU);
    tft_cmd(0xC5U); tft_data8(0x0FU); /* VCOM */

    tft_cmd(0xE0U); /* Positive Gamma */
    tft_data8(0x07U); tft_data8(0x0EU); tft_data8(0x08U); tft_data8(0x07U);
    tft_data8(0x10U); tft_data8(0x07U); tft_data8(0x02U); tft_data8(0x07U);
    tft_data8(0x09U); tft_data8(0x0FU); tft_data8(0x25U); tft_data8(0x36U);
    tft_data8(0x00U); tft_data8(0x08U); tft_data8(0x04U); tft_data8(0x10U);

    tft_cmd(0xE1U); /* Negative Gamma */
    tft_data8(0x0AU); tft_data8(0x0DU); tft_data8(0x08U); tft_data8(0x07U);
    tft_data8(0x0FU); tft_data8(0x07U); tft_data8(0x02U); tft_data8(0x07U);
    tft_data8(0x09U); tft_data8(0x0FU); tft_data8(0x25U); tft_data8(0x35U);
    tft_data8(0x00U); tft_data8(0x09U); tft_data8(0x04U); tft_data8(0x10U);

    tft_cmd(0xFCU); tft_data8(0x80U);
    tft_cmd(0x3AU); tft_data8(0x05U); /* 16-bit RGB565 */

    tft_cmd(0x36U);
#if TFT_ROTATE_180
    tft_data8(0xA8U); /* Landscape 160x80 flipped 180 deg, BGR */
#else
    tft_data8(0x78U); /* Landscape 160x80 normal, BGR */
#endif

#if TFT_INVERT_COLORS
    tft_cmd(0x21U); /* Display inversion ON */
#else
    tft_cmd(0x20U); /* Display inversion OFF (BOE default) */
#endif

    draw_full_background();
    tft_cmd(0x29U); /* Display ON */
}

void tft_hud_update(const tft_hud_state_t *st) {
    uint8_t pd_v;
    uint16_t pwr_tenth_w;
    uint16_t vbus_tenth_v;
    uint16_t ibus_cma;
    uint8_t bar_segs;

    if (st == NULL) {
        return;
    }

    pd_v = (uint8_t)((st->pd_target_mv + 500U) / 1000U);
    pwr_tenth_w = (uint16_t)((st->power_mw + 50UL) / 100UL);
    vbus_tenth_v = (uint16_t)((st->vbus_mv + 50U) / 100U);
    ibus_cma = (uint16_t)((st->ibus_ma + 5U) / 10U);
    bar_segs = (uint8_t)(((uint32_t)st->duty_permille * 16UL + 500UL) / 1000UL);
    if (bar_segs > 16U) {
        bar_segs = 16U;
    }

    if ((!s_cache.valid) ||
        (s_cache.live_mode != st->live_mode) ||
        (s_cache.pd_v != pd_v) ||
        (s_cache.fault != st->prot_fault)) {
        draw_header_zone(st->live_mode, pd_v, st->prot_fault);
    }

    if ((!s_cache.valid) ||
        (s_cache.pwr_tenth_w != pwr_tenth_w) ||
        (s_cache.fault != st->prot_fault)) {
        draw_power_zone(pwr_tenth_w, st->prot_fault);
    }

    if ((!s_cache.valid) ||
        (s_cache.vbus_tenth_v != vbus_tenth_v) ||
        (s_cache.fault != st->prot_fault)) {
        draw_vbus_zone(vbus_tenth_v, st->prot_fault);
    }

    if ((!s_cache.valid) ||
        (s_cache.ibus_cma != ibus_cma) ||
        (s_cache.fault != st->prot_fault)) {
        draw_ibus_zone(ibus_cma, st->prot_fault);
    }

    if ((!s_cache.valid) ||
        (s_cache.bar_segs != bar_segs) ||
        (s_cache.temp_c != st->mcu_temp_c) ||
        (s_cache.fault != st->prot_fault)) {
        draw_bottom_zone(bar_segs, st->mcu_temp_c, st->prot_fault);
    }

    s_cache.valid = true;
    s_cache.live_mode = st->live_mode;
    s_cache.pd_v = pd_v;
    s_cache.fault = st->prot_fault;
    s_cache.pwr_tenth_w = pwr_tenth_w;
    s_cache.vbus_tenth_v = vbus_tenth_v;
    s_cache.ibus_cma = ibus_cma;
    s_cache.bar_segs = bar_segs;
    s_cache.temp_c = st->mcu_temp_c;
}
