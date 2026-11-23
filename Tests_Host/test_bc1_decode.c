#include "bc1_decode.h"
#include "test_framework.h"

#include <stdint.h>

static void pack_bc1_block(uint8_t out[8], uint16_t c0, uint16_t c1, uint32_t indices) {
    out[0] = (uint8_t)(c0 & 0xFFU);
    out[1] = (uint8_t)((c0 >> 8) & 0xFFU);
    out[2] = (uint8_t)(c1 & 0xFFU);
    out[3] = (uint8_t)((c1 >> 8) & 0xFFU);
    out[4] = (uint8_t)(indices & 0xFFUL);
    out[5] = (uint8_t)((indices >> 8) & 0xFFUL);
    out[6] = (uint8_t)((indices >> 16) & 0xFFUL);
    out[7] = (uint8_t)((indices >> 24) & 0xFFUL);
}

static uint16_t ref_blend_2_1(uint16_t c_major, uint16_t c_minor) {
    uint32_t r0 = (c_major >> 11) & 0x1FU;
    uint32_t g0 = (c_major >> 5) & 0x3FU;
    uint32_t b0 = c_major & 0x1FU;
    uint32_t r1 = (c_minor >> 11) & 0x1FU;
    uint32_t g1 = (c_minor >> 5) & 0x3FU;
    uint32_t b1 = c_minor & 0x1FU;
    uint32_t r = (2UL * r0 + r1 + 1UL) / 3UL;
    uint32_t g = (2UL * g0 + g1 + 1UL) / 3UL;
    uint32_t b = (2UL * b0 + b1 + 1UL) / 3UL;
    return (uint16_t)((r << 11) | (g << 5) | b);
}

static int test_bc1_size_and_validation(void) {
    uint8_t dummy[8] = {0};
    uint16_t px = 0U;
    bc1_image_t bad_dim = {162U, 80U, dummy};
    bc1_image_t ok_img = {4U, 4U, dummy};

    TEST_ASSERT_EQ_INT(6400, (int)bc1_data_size(160U, 80U));
    TEST_ASSERT_EQ_INT(0, (int)bc1_data_size(0U, 80U));
    TEST_ASSERT_EQ_INT(0, (int)bc1_data_size(161U, 80U));
    TEST_ASSERT_EQ_INT(0, (int)bc1_data_size(160U, 82U));

    TEST_ASSERT_TRUE(!bc1_sample_pixel(&bad_dim, 0U, 0U, &px, false));
    TEST_ASSERT_TRUE(!bc1_sample_pixel(&ok_img, 4U, 0U, &px, false));
    TEST_ASSERT_TRUE(!bc1_decode_rect(&ok_img, 2U, 2U, 3U, 2U, &px, false));
    TEST_ASSERT_TRUE(!bc1_decode_rect(&ok_img, 0U, 0U, 0U, 4U, &px, false));
    return 0;
}

static int test_bc1_4color_mode_and_big_endian(void) {
    /* Pure white (0xFFFF) > pure blue-ish dark (0x0843) -> 4-color opaque mode */
    uint16_t c0 = 0xFFFFU;
    uint16_t c1 = 0x0843U;
    uint16_t exp2 = ref_blend_2_1(c0, c1);
    uint16_t exp3 = ref_blend_2_1(c1, c0);
    /* Pattern repeating [0, 1, 2, 3] across all 16 pixels */
    uint32_t indices = 0xE4E4E4E4UL;
    uint8_t blk[8];
    uint16_t le_out[16];
    uint16_t be_out[16];
    int i;

    pack_bc1_block(blk, c0, c1, indices);
    bc1_decode_block(blk, le_out, false);
    bc1_decode_block(blk, be_out, true);

    for (i = 0; i < 16; i++) {
        uint16_t expected = (i % 4 == 0) ? c0 :
                            (i % 4 == 1) ? c1 :
                            (i % 4 == 2) ? exp2 : exp3;
        uint16_t expected_be = (uint16_t)((expected >> 8) | (expected << 8));
        TEST_ASSERT_EQ_INT(expected, le_out[i]);
        TEST_ASSERT_EQ_INT(expected_be, be_out[i]);
    }

    /* Stress-check div3_fast equivalence across extreme channel combinations */
    pack_bc1_block(blk, 0xF800U, 0x001FU, 0x000000E4UL);
    bc1_decode_block(blk, le_out, false);
    TEST_ASSERT_EQ_INT(ref_blend_2_1(0xF800U, 0x001FU), le_out[2]);
    TEST_ASSERT_EQ_INT(ref_blend_2_1(0x001FU, 0xF800U), le_out[3]);
    return 0;
}

static int test_bc1_3color_and_black_mode(void) {
    /* c0 <= c1 triggers 3-color + black mode */
    uint16_t c0 = 0x0842U; /* R=1, G=2, B=2 */
    uint16_t c1 = 0xF81FU; /* R=31, G=0, B=31 */
    uint16_t exp_mid = (uint16_t)((((1U + 31U) >> 1) << 11) |
                                  (((2U + 0U) >> 1) << 5) |
                                  ((2U + 31U) >> 1));
    uint8_t blk[8];
    uint16_t out[16];

    pack_bc1_block(blk, c0, c1, 0x000000E4UL);
    bc1_decode_block(blk, out, false);

    TEST_ASSERT_EQ_INT(c0, out[0]);
    TEST_ASSERT_EQ_INT(c1, out[1]);
    TEST_ASSERT_EQ_INT(exp_mid, out[2]);
    TEST_ASSERT_EQ_INT(0x0000, out[3]);
    return 0;
}

static int test_bc1_unaligned_rect_matches_sample(void) {
    /* 12x8 image = 3x2 = 6 distinct blocks */
    uint8_t blocks[6 * 8];
    bc1_image_t img = {12U, 8U, blocks};
    uint16_t rect_le[7 * 5];
    uint16_t rect_be[7 * 5];
    uint16_t b;
    uint16_t rx;
    uint16_t ry;

    for (b = 0U; b < 6U; b++) {
        uint16_t c0 = (uint16_t)(0xF000U - (b * 0x1100U));
        uint16_t c1 = (uint16_t)(0x0010U + (b * 0x0221U));
        uint32_t idx = 0x1B4E93C6UL ^ ((uint32_t)b * 0x11223344UL);
        pack_bc1_block(&blocks[b * 8U], c0, c1, idx);
    }

    /* Decode unaligned rectangle crossing 3x2 blocks: x=2, y=1, w=7, h=5 */
    TEST_ASSERT_TRUE(bc1_decode_rect(&img, 2U, 1U, 7U, 5U, rect_le, false));
    TEST_ASSERT_TRUE(bc1_decode_rect(&img, 2U, 1U, 7U, 5U, rect_be, true));

    for (ry = 0U; ry < 5U; ry++) {
        for (rx = 0U; rx < 7U; rx++) {
            uint16_t sp_le = 0U;
            uint16_t sp_be = 0U;
            TEST_ASSERT_TRUE(bc1_sample_pixel(&img, (uint16_t)(2U + rx),
                                              (uint16_t)(1U + ry), &sp_le, false));
            TEST_ASSERT_TRUE(bc1_sample_pixel(&img, (uint16_t)(2U + rx),
                                              (uint16_t)(1U + ry), &sp_be, true));
            TEST_ASSERT_EQ_INT(sp_le, rect_le[ry * 7U + rx]);
            TEST_ASSERT_EQ_INT(sp_be, rect_be[ry * 7U + rx]);
            TEST_ASSERT_EQ_INT((uint16_t)((sp_le >> 8) | (sp_le << 8)), sp_be);
        }
    }
    return 0;
}

int run_bc1_decode_tests(void) {
    RUN_TEST(test_bc1_size_and_validation);
    RUN_TEST(test_bc1_4color_mode_and_big_endian);
    RUN_TEST(test_bc1_3color_and_black_mode);
    RUN_TEST(test_bc1_unaligned_rect_matches_sample);
    return 0;
}
