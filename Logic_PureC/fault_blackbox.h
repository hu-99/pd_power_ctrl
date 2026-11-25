#ifndef FAULT_BLACKBOX_H
#define FAULT_BLACKBOX_H

#include <stdbool.h>
#include <stdint.h>

#define BBOX_PAGE_SIZE    128U
#define BBOX_MAX_RECORDS  6U
#define BBOX_MAGIC        0x42424F58UL /* "BBOX" */

typedef enum {
    BBOX_EVT_NONE = 0,
    BBOX_EVT_UVP = 1,
    BBOX_EVT_OVP = 2,
    BBOX_EVT_OCP_SOFT = 3,
    BBOX_EVT_OCP_HARD = 4,
    BBOX_EVT_IWDG_RESET = 5,
    BBOX_EVT_STACK_OVERFLOW = 6,
    BBOX_EVT_OTP = 7,
    BBOX_EVT_SENSOR_LOSS = 8
} bbox_event_t;

#define BBOX_FLAG_INA_ONLINE   (1U << 0)
#define BBOX_FLAG_HUSB_ONLINE  (1U << 1)
#define BBOX_FLAG_HW_ALERT     (1U << 2)
#define BBOX_FLAG_SIM_MODE     (1U << 3)

typedef struct {
    uint32_t uptime_ms;
    uint16_t vbus_mv;
    uint16_t ibus_ma;
    uint16_t power_mw;
    uint16_t duty_permille;
    uint8_t event_type;
    uint8_t flags;
    uint16_t extra;
} bbox_record_t;

typedef struct {
    uint32_t magic;
    uint32_t seq;
    uint16_t boot_count;
    uint16_t iwdg_reset_count;
    uint16_t fault_total_count;
    uint8_t write_idx;
    uint8_t valid_count;
    uint8_t last_reset_cause;
    uint8_t active_slot;
    uint16_t reserved0;
    uint32_t reserved1;
    bbox_record_t records[BBOX_MAX_RECORDS];
    uint32_t reserved2;
    uint32_t crc32;
} bbox_page_t;

typedef struct {
    bool (*read_page)(void *ctx, uint8_t slot, uint8_t *buf_128);
    bool (*erase_and_write_page)(void *ctx, uint8_t slot, const uint8_t *buf_128);
    void *ctx;
} bbox_flash_ops_t;

typedef struct {
    bbox_page_t state;
    uint8_t next_write_slot;
    bool loaded_from_flash;
} bbox_ctx_t;

/**
 * @brief Compute standard IEEE CRC-32 (0xEDB88320) without a RAM/ROM lookup table.
 */
uint32_t bbox_crc32(const uint8_t *data, uint32_t len);

/**
 * @brief Initialize blackbox context from dual ping-pong 128-byte Flash pages.
 * @note Never writes to Flash on init when Flash is virgin/empty.
 */
void bbox_init(bbox_ctx_t *ctx, const bbox_flash_ops_t *ops);

/**
 * @brief Record boot reset cause; writes to Flash ONLY when is_iwdg_reset is true.
 * @param iwdg_crumb Optional pre-reset RAM breadcrumb snapshot (may be NULL).
 * @return true if no Flash write was needed or Flash commit succeeded.
 */
bool bbox_record_boot(bbox_ctx_t *ctx,
                      const bbox_flash_ops_t *ops,
                      uint8_t reset_cause_mask,
                      bool is_iwdg_reset,
                      const bbox_record_t *iwdg_crumb);

/**
 * @brief Append one fault record to the ring buffer and commit to alternate Flash page.
 */
bool bbox_append_fault(bbox_ctx_t *ctx,
                       const bbox_flash_ops_t *ops,
                       const bbox_record_t *rec);

/**
 * @brief Clear stored records and counters across both Flash pages while advancing seq.
 */
bool bbox_clear(bbox_ctx_t *ctx, const bbox_flash_ops_t *ops);

/**
 * @brief Read stored record in newest-first order (0 = most recent).
 * @return true if index < valid_count and out_rec was populated.
 */
bool bbox_get_latest(const bbox_ctx_t *ctx, uint8_t index_newest_first, bbox_record_t *out_rec);

/**
 * @brief Return short ASCII tag for a bbox_event_t code.
 */
const char *bbox_event_name(uint8_t event_type);

#endif /* FAULT_BLACKBOX_H */
