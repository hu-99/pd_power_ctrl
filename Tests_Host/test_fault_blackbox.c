#include "fault_blackbox.h"
#include "test_framework.h"
#include <string.h>

typedef struct {
    uint8_t pages[2][BBOX_PAGE_SIZE];
    uint32_t erase_writes;
    uint8_t last_written_slot;
    bool inject_write_fail;
} fake_flash_t;

static bool fake_read_page(void *ctx, uint8_t slot, uint8_t *buf_128) {
    fake_flash_t *ff = (fake_flash_t *)ctx;
    if ((ff == NULL) || (slot >= 2U) || (buf_128 == NULL)) {
        return false;
    }
    memcpy(buf_128, ff->pages[slot], BBOX_PAGE_SIZE);
    return true;
}

static bool fake_write_page(void *ctx, uint8_t slot, const uint8_t *buf_128) {
    fake_flash_t *ff = (fake_flash_t *)ctx;
    if ((ff == NULL) || (slot >= 2U) || (buf_128 == NULL) || ff->inject_write_fail) {
        return false;
    }
    memcpy(ff->pages[slot], buf_128, BBOX_PAGE_SIZE);
    ff->erase_writes++;
    ff->last_written_slot = slot;
    return true;
}

static int test_virgin_flash_no_wear_on_normal_boot(void) {
    fake_flash_t ff;
    bbox_flash_ops_t ops = {fake_read_page, fake_write_page, &ff};
    bbox_ctx_t ctx;
    bbox_record_t rec;

    memset(&ff, 0xFF, sizeof(ff));
    ff.erase_writes = 0U;
    ff.last_written_slot = 0xFFU;
    ff.inject_write_fail = false;

    bbox_init(&ctx, &ops);
    TEST_ASSERT_TRUE(!ctx.loaded_from_flash);
    TEST_ASSERT_EQ_INT(0, ctx.state.valid_count);
    TEST_ASSERT_EQ_INT(128, sizeof(bbox_page_t));
    TEST_ASSERT_EQ_INT(16, sizeof(bbox_record_t));

    TEST_ASSERT_TRUE(bbox_record_boot(&ctx, &ops, 0x01U, false, NULL));
    TEST_ASSERT_EQ_INT(0, ff.erase_writes);
    TEST_ASSERT_EQ_INT(1, ctx.state.boot_count);
    TEST_ASSERT_TRUE(!bbox_get_latest(&ctx, 0U, &rec));
    return 0;
}

static int test_pingpong_ring_wrap_and_order(void) {
    fake_flash_t ff;
    bbox_flash_ops_t ops = {fake_read_page, fake_write_page, &ff};
    bbox_ctx_t ctx;
    bbox_record_t rec;

    memset(&ff, 0xFF, sizeof(ff));
    ff.erase_writes = 0U;
    ff.last_written_slot = 0xFFU;
    ff.inject_write_fail = false;

    bbox_init(&ctx, &ops);

    for (uint8_t i = 0U; i < 8U; i++) {
        bbox_record_t in = {0};
        in.uptime_ms = (uint32_t)(1000U * (i + 1U));
        in.vbus_mv = (uint16_t)(12000U + i * 500U);
        in.ibus_ma = (uint16_t)(300U + i * 50U);
        in.power_mw = (uint16_t)(3500U + i * 100U);
        in.duty_permille = (uint16_t)(700U + i);
        in.event_type = (uint8_t)BBOX_EVT_OVP;
        in.flags = BBOX_FLAG_INA_ONLINE;
        TEST_ASSERT_TRUE(bbox_append_fault(&ctx, &ops, &in));
        TEST_ASSERT_EQ_INT(i & 1U, ff.last_written_slot);
    }

    TEST_ASSERT_EQ_INT(8, ff.erase_writes);
    TEST_ASSERT_EQ_INT(8, ctx.state.fault_total_count);
    TEST_ASSERT_EQ_INT(BBOX_MAX_RECORDS, ctx.state.valid_count);

    /* Reboot and verify recovery from newest Flash slot (slot 1, seq 8). */
    bbox_init(&ctx, &ops);
    TEST_ASSERT_TRUE(ctx.loaded_from_flash);
    TEST_ASSERT_EQ_INT(8, ctx.state.seq);
    TEST_ASSERT_EQ_INT(BBOX_MAX_RECORDS, ctx.state.valid_count);

    /* Index 0 should be write #7 (uptime 8000ms), index 5 should be write #2 (3000ms). */
    TEST_ASSERT_TRUE(bbox_get_latest(&ctx, 0U, &rec));
    TEST_ASSERT_EQ_INT(8000, rec.uptime_ms);
    TEST_ASSERT_EQ_INT(15500, rec.vbus_mv);
    TEST_ASSERT_TRUE(bbox_get_latest(&ctx, 5U, &rec));
    TEST_ASSERT_EQ_INT(3000, rec.uptime_ms);
    TEST_ASSERT_EQ_INT(13000, rec.vbus_mv);
    TEST_ASSERT_TRUE(!bbox_get_latest(&ctx, 6U, &rec));
    return 0;
}

static int test_corrupt_newer_page_falls_back_to_backup(void) {
    fake_flash_t ff;
    bbox_flash_ops_t ops = {fake_read_page, fake_write_page, &ff};
    bbox_ctx_t ctx;
    bbox_record_t rec;

    memset(&ff, 0xFF, sizeof(ff));
    ff.erase_writes = 0U;
    ff.inject_write_fail = false;

    bbox_init(&ctx, &ops);

    bbox_record_t r1 = {.uptime_ms = 1111UL, .vbus_mv = 12000U, .event_type = BBOX_EVT_UVP};
    bbox_record_t r2 = {.uptime_ms = 2222UL, .vbus_mv = 21900U, .event_type = BBOX_EVT_OVP};
    TEST_ASSERT_TRUE(bbox_append_fault(&ctx, &ops, &r1)); /* -> slot 0, seq 1 */
    TEST_ASSERT_TRUE(bbox_append_fault(&ctx, &ops, &r2)); /* -> slot 1, seq 2 */

    /* Simulate brown-out corruption in newer slot 1. */
    ff.pages[1][28] ^= 0x5AU;

    bbox_init(&ctx, &ops);
    TEST_ASSERT_TRUE(ctx.loaded_from_flash);
    TEST_ASSERT_EQ_INT(1, ctx.state.seq);
    TEST_ASSERT_EQ_INT(1, ctx.next_write_slot);
    TEST_ASSERT_TRUE(bbox_get_latest(&ctx, 0U, &rec));
    TEST_ASSERT_EQ_INT(1111, rec.uptime_ms);
    TEST_ASSERT_EQ_INT(BBOX_EVT_UVP, rec.event_type);
    return 0;
}

static int test_iwdg_boot_logging_and_clear_both_pages(void) {
    fake_flash_t ff;
    bbox_flash_ops_t ops = {fake_read_page, fake_write_page, &ff};
    bbox_ctx_t ctx;
    bbox_record_t crumb = {0};
    bbox_record_t out = {0};

    memset(&ff, 0xFF, sizeof(ff));
    ff.erase_writes = 0U;
    ff.inject_write_fail = false;

    crumb.uptime_ms = 54321UL;
    crumb.vbus_mv = 19980U;
    crumb.ibus_ma = 420U;
    crumb.duty_permille = 640U;
    crumb.flags = BBOX_FLAG_SIM_MODE;
    crumb.extra = 0x0003U; /* e.g. PD+CTRL alive, LOG missed */

    bbox_init(&ctx, &ops);
    TEST_ASSERT_TRUE(bbox_record_boot(&ctx, &ops, 0x04U, true, &crumb));
    TEST_ASSERT_EQ_INT(1, ff.erase_writes);
    TEST_ASSERT_EQ_INT(1, ctx.state.iwdg_reset_count);
    TEST_ASSERT_TRUE(bbox_get_latest(&ctx, 0U, &out));
    TEST_ASSERT_EQ_INT(BBOX_EVT_IWDG_RESET, out.event_type);
    TEST_ASSERT_EQ_INT(0x0003U, out.extra);
    TEST_ASSERT_EQ_INT(54321, out.uptime_ms);

    TEST_ASSERT_TRUE(bbox_clear(&ctx, &ops));
    bbox_init(&ctx, &ops);
    TEST_ASSERT_TRUE(ctx.loaded_from_flash);
    TEST_ASSERT_EQ_INT(0, ctx.state.valid_count);
    TEST_ASSERT_EQ_INT(0, ctx.state.fault_total_count);
    TEST_ASSERT_EQ_INT(0, ctx.state.iwdg_reset_count);
    TEST_ASSERT_TRUE(!bbox_get_latest(&ctx, 0U, &out));
    return 0;
}

int run_fault_blackbox_tests(void) {
    RUN_TEST(test_virgin_flash_no_wear_on_normal_boot);
    RUN_TEST(test_pingpong_ring_wrap_and_order);
    RUN_TEST(test_corrupt_newer_page_falls_back_to_backup);
    RUN_TEST(test_iwdg_boot_logging_and_clear_both_pages);
    return 0;
}
