#include "bc1_decode.h"

#include <stddef.h>

static uint16_t read_u16_le(const uint8_t *p) {
    return (uint16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static uint32_t read_u32_le(const uint8_t *p) {
    return (uint32_t)p[0] |
           ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) |
           ((uint32_t)p[3] << 24);
}

static uint16_t bswap16(uint16_t v) {
    return (uint16_t)((v >> 8) | (v << 8));
}

/*
 * Exact integer division by 3 for all x in [0, 681]:
 * (x * 683) >> 11 avoids software __aeabi_uidiv on Cortex-M0+.
 */
static uint32_t div3_fast(uint32_t x) {
    return (x * 683UL) >> 11;
}

static uint16_t blend_2_1_rgb565(uint16_t c_major, uint16_t c_minor) {
    uint32_t r0 = (uint32_t)((c_major >> 11) & 0x1FU);
    uint32_t g0 = (uint32_t)((c_major >> 5) & 0x3FU);
    uint32_t b0 = (uint32_t)(c_major & 0x1FU);

    uint32_t r1 = (uint32_t)((c_minor >> 11) & 0x1FU);
    uint32_t g1 = (uint32_t)((c_minor >> 5) & 0x3FU);
    uint32_t b1 = (uint32_t)(c_minor & 0x1FU);

    uint32_t r = div3_fast((2UL * r0) + r1 + 1UL);
    uint32_t g = div3_fast((2UL * g0) + g1 + 1UL);
    uint32_t b = div3_fast((2UL * b0) + b1 + 1UL);

    return (uint16_t)((r << 11) | (g << 5) | b);
}

static uint16_t blend_1_1_rgb565(uint16_t c0, uint16_t c1) {
    uint32_t r = (((uint32_t)((c0 >> 11) & 0x1FU)) + ((uint32_t)((c1 >> 11) & 0x1FU))) >> 1;
    uint32_t g = (((uint32_t)((c0 >> 5) & 0x3FU)) + ((uint32_t)((c1 >> 5) & 0x3FU))) >> 1;
    uint32_t b = (((uint32_t)(c0 & 0x1FU)) + ((uint32_t)(c1 & 0x1FU))) >> 1;

    return (uint16_t)((r << 11) | (g << 5) | b);
}

static void expand_palette(const uint8_t block[BC1_BLOCK_BYTES],
                           uint16_t pal[4],
                           bool big_endian) {
    uint16_t c0 = read_u16_le(&block[0]);
    uint16_t c1 = read_u16_le(&block[2]);

    pal[0] = c0;
    pal[1] = c1;
    if (c0 > c1) {
        pal[2] = blend_2_1_rgb565(c0, c1);
        pal[3] = blend_2_1_rgb565(c1, c0);
    } else {
        pal[2] = blend_1_1_rgb565(c0, c1);
        pal[3] = 0x0000U;
    }

    if (big_endian) {
        pal[0] = bswap16(pal[0]);
        pal[1] = bswap16(pal[1]);
        pal[2] = bswap16(pal[2]);
        pal[3] = bswap16(pal[3]);
    }
}

static bool image_is_valid(const bc1_image_t *img) {
    if (img == NULL || img->blocks == NULL) {
        return false;
    }
    if (img->width == 0U || img->height == 0U) {
        return false;
    }
    if ((img->width & 3U) != 0U || (img->height & 3U) != 0U) {
        return false;
    }
    return true;
}

uint32_t bc1_data_size(uint16_t width, uint16_t height) {
    if (width == 0U || height == 0U || (width & 3U) != 0U || (height & 3U) != 0U) {
        return 0UL;
    }
    return (uint32_t)(width >> 2) * (uint32_t)(height >> 2) * (uint32_t)BC1_BLOCK_BYTES;
}

void bc1_decode_block(const uint8_t block[BC1_BLOCK_BYTES],
                      uint16_t out_4x4[16],
                      bool big_endian) {
    uint16_t pal[4];
    uint32_t bits;
    uint32_t i;

    if (block == NULL || out_4x4 == NULL) {
        return;
    }

    expand_palette(block, pal, big_endian);
    bits = read_u32_le(&block[4]);
    for (i = 0U; i < 16U; i++) {
        out_4x4[i] = pal[bits & 3UL];
        bits >>= 2;
    }
}

bool bc1_decode_rect(const bc1_image_t *img,
                     uint16_t x,
                     uint16_t y,
                     uint16_t w,
                     uint16_t h,
                     uint16_t *out_buf,
                     bool big_endian) {
    uint16_t bx_start;
    uint16_t bx_end;
    uint16_t by_start;
    uint16_t by_end;
    uint16_t blocks_per_row;
    uint16_t by;

    if (!image_is_valid(img) || out_buf == NULL || w == 0U || h == 0U) {
        return false;
    }
    if ((uint32_t)x + (uint32_t)w > (uint32_t)img->width ||
        (uint32_t)y + (uint32_t)h > (uint32_t)img->height) {
        return false;
    }

    blocks_per_row = (uint16_t)(img->width >> 2);
    bx_start = (uint16_t)(x >> 2);
    bx_end = (uint16_t)((x + w - 1U) >> 2);
    by_start = (uint16_t)(y >> 2);
    by_end = (uint16_t)((y + h - 1U) >> 2);

    for (by = by_start; by <= by_end; by++) {
        uint16_t tile_y0 = (uint16_t)(by << 2);
        uint16_t ly_min = (tile_y0 < y) ? (uint16_t)(y - tile_y0) : 0U;
        uint16_t ly_max = ((uint32_t)tile_y0 + 3UL >= (uint32_t)y + (uint32_t)h)
                              ? (uint16_t)(y + h - 1U - tile_y0)
                              : 3U;
        uint16_t bx;

        for (bx = bx_start; bx <= bx_end; bx++) {
            uint16_t tile_x0 = (uint16_t)(bx << 2);
            uint16_t lx_min = (tile_x0 < x) ? (uint16_t)(x - tile_x0) : 0U;
            uint16_t lx_max = ((uint32_t)tile_x0 + 3UL >= (uint32_t)x + (uint32_t)w)
                                  ? (uint16_t)(x + w - 1U - tile_x0)
                                  : 3U;
            const uint8_t *blk =
                &img->blocks[((uint32_t)by * (uint32_t)blocks_per_row + (uint32_t)bx) *
                             (uint32_t)BC1_BLOCK_BYTES];
            uint16_t pal[4];
            uint32_t bits;
            uint16_t ly;

            expand_palette(blk, pal, big_endian);
            bits = read_u32_le(&blk[4]);

            for (ly = ly_min; ly <= ly_max; ly++) {
                uint32_t dst_row = (uint32_t)(tile_y0 + ly - y) * (uint32_t)w;
                uint8_t row_bits = (uint8_t)(bits >> (ly * 8U));
                uint16_t lx;

                for (lx = lx_min; lx <= lx_max; lx++) {
                    uint32_t idx = (uint32_t)((row_bits >> (lx * 2U)) & 3U);
                    out_buf[dst_row + (uint32_t)(tile_x0 + lx - x)] = pal[idx];
                }
            }
        }
    }

    return true;
}

bool bc1_sample_pixel(const bc1_image_t *img,
                      uint16_t x,
                      uint16_t y,
                      uint16_t *out_rgb565,
                      bool big_endian) {
    uint16_t blocks_per_row;
    uint32_t blk_idx;
    const uint8_t *blk;
    uint16_t pal[4];
    uint32_t bits;
    uint32_t shift;

    if (!image_is_valid(img) || out_rgb565 == NULL) {
        return false;
    }
    if (x >= img->width || y >= img->height) {
        return false;
    }

    blocks_per_row = (uint16_t)(img->width >> 2);
    blk_idx = (uint32_t)(y >> 2) * (uint32_t)blocks_per_row + (uint32_t)(x >> 2);
    blk = &img->blocks[blk_idx * (uint32_t)BC1_BLOCK_BYTES];

    expand_palette(blk, pal, big_endian);
    bits = read_u32_le(&blk[4]);
    shift = (uint32_t)(((y & 3U) * 4U + (x & 3U)) * 2U);
    *out_rgb565 = pal[(bits >> shift) & 3UL];
    return true;
}
