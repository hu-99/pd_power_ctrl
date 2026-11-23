#ifndef BC1_DECODE_H
#define BC1_DECODE_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define BC1_BLOCK_DIM    4U
#define BC1_BLOCK_BYTES  8U

typedef struct {
    uint16_t width;        /* Must be a non-zero multiple of 4 (e.g., 160) */
    uint16_t height;       /* Must be a non-zero multiple of 4 (e.g., 80)  */
    const uint8_t *blocks; /* Packed (width/4) * (height/4) * 8 bytes      */
} bc1_image_t;

/**
 * @brief Return encoded byte size for a width x height BC1 image, or 0 if invalid.
 */
uint32_t bc1_data_size(uint16_t width, uint16_t height);

/**
 * @brief Decode a single 8-byte BC1/DXT1 block into 16 row-major RGB565 pixels.
 * @param block      Pointer to 8-byte little-endian BC1 block.
 * @param out_4x4    Output buffer of 16 RGB565 pixels (row-major).
 * @param big_endian If true, byte-swap each RGB565 pixel for direct 8-bit SPI DMA output.
 */
void bc1_decode_block(const uint8_t block[BC1_BLOCK_BYTES],
                      uint16_t out_4x4[16],
                      bool big_endian);

/**
 * @brief Decode an arbitrary (possibly unaligned) rectangle from a BC1 image.
 *
 * Uses O(1) stack memory (~48 bytes) and visits only the 4x4 tiles overlapping
 * the requested rectangle, making it ideal for dirty-rect transparent text overlay
 * on RAM-constrained MCUs.
 *
 * @param img        Valid BC1 image descriptor.
 * @param x          Left X coordinate in pixels.
 * @param y          Top Y coordinate in pixels.
 * @param w          Rectangle width in pixels (> 0).
 * @param h          Rectangle height in pixels (> 0).
 * @param out_buf    Caller-supplied output buffer of at least (w * h) uint16_t pixels.
 * @param big_endian If true, byte-swap output pixels for direct SPI DMA transmission.
 * @return true on success, false if arguments or bounds are invalid.
 */
bool bc1_decode_rect(const bc1_image_t *img,
                     uint16_t x,
                     uint16_t y,
                     uint16_t w,
                     uint16_t h,
                     uint16_t *out_buf,
                     bool big_endian);

/**
 * @brief O(1) decode of a single pixel at (x, y) from a BC1 image.
 */
bool bc1_sample_pixel(const bc1_image_t *img,
                      uint16_t x,
                      uint16_t y,
                      uint16_t *out_rgb565,
                      bool big_endian);

#ifdef __cplusplus
}
#endif

#endif /* BC1_DECODE_H */
