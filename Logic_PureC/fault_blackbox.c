#include "fault_blackbox.h"
#include <stddef.h>
#include <string.h>

/* Compile-time verification that structs match PY32F003 128-byte Flash page geometry. */
typedef char bbox_rec_size_chk[(sizeof(bbox_record_t) == 16U) ? 1 : -1];
typedef char bbox_page_size_chk[(sizeof(bbox_page_t) == BBOX_PAGE_SIZE) ? 1 : -1];

uint32_t bbox_crc32(const uint8_t *data, uint32_t len) {
    uint32_t crc = 0xFFFFFFFFUL;
    if (data == NULL) {
        return 0UL;
    }
    for (uint32_t i = 0U; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (uint8_t b = 0U; b < 8U; b++) {
            uint32_t mask = (crc & 1UL) ? 0xEDB88320UL : 0UL;
            crc = (crc >> 1U) ^ mask;
        }
    }
    return ~crc;
}

static bool page_is_valid(const bbox_page_t *page) {
    uint32_t calc;
    if ((page == NULL) || (page->magic != BBOX_MAGIC)) {
        return false;
    }
    if ((page->valid_count > BBOX_MAX_RECORDS) || (page->write_idx >= BBOX_MAX_RECORDS)) {
        return false;
    }
    calc = bbox_crc32((const uint8_t *)page, (uint32_t)(sizeof(bbox_page_t) - sizeof(uint32_t)));
    return (calc == page->crc32);
}

static bool commit_next_page(bbox_ctx_t *ctx, const bbox_flash_ops_t *ops) {
    uint8_t slot;
    if ((ctx == NULL) || (ops == NULL) || (ops->erase_and_write_page == NULL)) {
        return false;
    }

    slot = ctx->next_write_slot;
    ctx->state.magic = BBOX_MAGIC;
    ctx->state.seq++;
    ctx->state.active_slot = slot;
    ctx->state.crc32 = bbox_crc32((const uint8_t *)&ctx->state,
                                  (uint32_t)(sizeof(bbox_page_t) - sizeof(uint32_t)));

    if (!ops->erase_and_write_page(ops->ctx, slot, (const uint8_t *)&ctx->state)) {
        return false;
    }

    ctx->next_write_slot = (uint8_t)(slot ^ 1U);
    ctx->loaded_from_flash = true;
    return true;
}

static void append_record_to_ram(bbox_page_t *page, const bbox_record_t *rec) {
    page->records[page->write_idx] = *rec;
    page->write_idx = (uint8_t)((page->write_idx + 1U) % BBOX_MAX_RECORDS);
    if (page->valid_count < BBOX_MAX_RECORDS) {
        page->valid_count++;
    }
    if (page->fault_total_count < UINT16_MAX) {
        page->fault_total_count++;
    }
}

void bbox_init(bbox_ctx_t *ctx, const bbox_flash_ops_t *ops) {
    bbox_page_t pages[2];
    bool valid[2] = {false, false};
    int8_t best = -1;

    if (ctx == NULL) {
        return;
    }
    memset(ctx, 0, sizeof(*ctx));
    ctx->state.magic = BBOX_MAGIC;
    ctx->next_write_slot = 0U;

    if ((ops == NULL) || (ops->read_page == NULL)) {
        return;
    }

    for (uint8_t s = 0U; s < 2U; s++) {
        memset(&pages[s], 0xFF, sizeof(bbox_page_t));
        if (ops->read_page(ops->ctx, s, (uint8_t *)&pages[s])) {
            valid[s] = page_is_valid(&pages[s]);
        }
    }

    if (valid[0] && valid[1]) {
        best = ((int32_t)(pages[1].seq - pages[0].seq) > 0) ? 1 : 0;
    } else if (valid[0]) {
        best = 0;
    } else if (valid[1]) {
        best = 1;
    }

    if (best >= 0) {
        ctx->state = pages[best];
        ctx->next_write_slot = (uint8_t)((uint8_t)best ^ 1U);
        ctx->loaded_from_flash = true;
    }
}

bool bbox_record_boot(bbox_ctx_t *ctx,
                      const bbox_flash_ops_t *ops,
                      uint8_t reset_cause_mask,
                      bool is_iwdg_reset,
                      const bbox_record_t *iwdg_crumb) {
    bbox_record_t rec;
    if (ctx == NULL) {
        return false;
    }

    if (ctx->state.boot_count < UINT16_MAX) {
        ctx->state.boot_count++;
    }
    ctx->state.last_reset_cause = reset_cause_mask;

    if (!is_iwdg_reset) {
        /* Skip Flash erase/write on healthy power cycles to preserve Flash endurance. */
        return true;
    }

    if (ctx->state.iwdg_reset_count < UINT16_MAX) {
        ctx->state.iwdg_reset_count++;
    }

    if (iwdg_crumb != NULL) {
        rec = *iwdg_crumb;
        rec.event_type = (uint8_t)BBOX_EVT_IWDG_RESET;
    } else {
        memset(&rec, 0, sizeof(rec));
        rec.event_type = (uint8_t)BBOX_EVT_IWDG_RESET;
        rec.extra = (uint16_t)reset_cause_mask;
    }

    append_record_to_ram(&ctx->state, &rec);
    return commit_next_page(ctx, ops);
}

bool bbox_append_fault(bbox_ctx_t *ctx,
                       const bbox_flash_ops_t *ops,
                       const bbox_record_t *rec) {
    if ((ctx == NULL) || (rec == NULL)) {
        return false;
    }
    append_record_to_ram(&ctx->state, rec);
    return commit_next_page(ctx, ops);
}

bool bbox_clear(bbox_ctx_t *ctx, const bbox_flash_ops_t *ops) {
    uint32_t prev_seq;
    if (ctx == NULL) {
        return false;
    }

    prev_seq = ctx->state.seq;
    memset(&ctx->state, 0, sizeof(ctx->state));
    ctx->state.magic = BBOX_MAGIC;
    ctx->state.seq = prev_seq;

    /* Commit empty state to both ping-pong pages so old data can never resurrect. */
    if (!commit_next_page(ctx, ops)) {
        return false;
    }
    return commit_next_page(ctx, ops);
}

bool bbox_get_latest(const bbox_ctx_t *ctx, uint8_t index_newest_first, bbox_record_t *out_rec) {
    uint8_t idx;
    if ((ctx == NULL) || (out_rec == NULL)) {
        return false;
    }
    if (index_newest_first >= ctx->state.valid_count) {
        return false;
    }

    idx = (uint8_t)((ctx->state.write_idx + BBOX_MAX_RECORDS - 1U - index_newest_first) %
                    BBOX_MAX_RECORDS);
    *out_rec = ctx->state.records[idx];
    return true;
}

const char *bbox_event_name(uint8_t event_type) {
    switch (event_type) {
    case BBOX_EVT_UVP:
        return "UVP";
    case BBOX_EVT_OVP:
        return "OVP";
    case BBOX_EVT_OCP_SOFT:
        return "OCP_S";
    case BBOX_EVT_OCP_HARD:
        return "OCP_H";
    case BBOX_EVT_IWDG_RESET:
        return "IWDG";
    case BBOX_EVT_STACK_OVERFLOW:
        return "STK";
    case BBOX_EVT_OTP:
        return "OTP";
    default:
        return "NONE";
    }
}
