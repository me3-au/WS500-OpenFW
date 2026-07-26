/*
 * telem_stream.c — pump the {"t":"telem"} line at 1 Hz + on demand (GH#35).
 * Contract: telem_stream.h; line schema: control/Inc/telemetry_json.h.
 *
 * This file is the ONLY place the §7 diagnostic surfaces are flattened for the
 * wire, so the mapping from firmware enums (crash_record.h, integrity.h,
 * err_budget.h) onto the pure layer's fixed wire order lives here — pinned
 * with static asserts so a reordered enum breaks the build, not the client.
 *
 * SPDX-License-Identifier: MIT
 */
#include "telem_stream.h"
#include "telemetry_json.h"
#include "cfg_stream.h"
#include "usb_cdc.h"
#include "crash_record.h"
#include "integrity.h"
#include "err_budget.h"
#include "safe_state.h"
#include "stm32f0xx_hal.h"   /* HAL_GetTick */
#include <string.h>

/* The wire order (telemetry_json.h) mirrors the firmware enums 1:1; if either
 * side grows, both counts move and this is where the compiler says so. */
_Static_assert(TELEM_RESET_COUNT == (int)RESET_CAUSE_COUNT,
               "telem wire reset-cause order out of sync with crash_record.h");
_Static_assert(TELEM_ERRB_INA226 == (int)ERRB_INA226 &&
               TELEM_ERRB_EEPROM == (int)ERRB_EEPROM &&
               TELEM_ERRB_CAN    == (int)ERRB_CAN &&
               TELEM_ERRB_COUNT  == (int)ERRB_COUNT,
               "telem wire err-budget order out of sync with err_budget.h");

#define TELEM_PERIOD_MS  1000u

static ctrl_telemetry_t s_tlm;      /* zeroed until the first control tick */
static uint32_t         s_next_ms;

static void telem_sink(void *ctx, const char *data, int len)
{
    (void)ctx;
    cfg_proto_tx((const uint8_t *)data, len);
}

static uint8_t crc_to_wire(void)
{
    switch (integrity_crc_status()) {
    case INTEGRITY_CRC_OK:        return TELEM_CRC_OK;
    case INTEGRITY_CRC_MISMATCH:  return TELEM_CRC_MISMATCH;
    case INTEGRITY_CRC_UNPATCHED: return TELEM_CRC_UNPATCHED;
    default:                      return TELEM_CRC_UNAVAILABLE;
    }
}

static void telem_emit(void)
{
    telem_diag_t d;
    memset(&d, 0, sizeof d);

    d.uptime_ms = HAL_GetTick();

    /* §7 R2: reset causes + crash-record summary. Index order asserted above. */
    static const reset_cause_t CAUSES[TELEM_RESET_COUNT] = {
        RESET_CAUSE_POR, RESET_CAUSE_PIN, RESET_CAUSE_SOFTWARE,
        RESET_CAUSE_IWDG, RESET_CAUSE_WWDG, RESET_CAUSE_LOWPOWER,
        RESET_CAUSE_OBL,
    };
    for (int i = 0; i < TELEM_RESET_COUNT; i++)
        d.reset_counts[i] = crash_reset_cause_count(CAUSES[i]);
    d.reset_cause_bits = crash_reset_cause();
    d.boot_count       = crash_boot_count();
    d.crash_count      = crash_record_count();
    const crash_record_t *cr = crash_record_get(0);
    if (cr != NULL) {
        d.last_crash_kind = cr->kind;
        d.last_crash_pc   = cr->pc;
    }

    /* §7 R4: image CRC verdict + stack high-watermark. */
    d.crc_status     = crc_to_wire();
    d.stack_used_max = integrity_stack_used_max();
    d.stack_free_min = integrity_stack_free_min();

    /* §7 R6: err-budget states. The latched mask uses the same bit order as
     * the reinit array (asserted above). */
    d.errb_fault_mask = errb_fault_mask();
    for (int i = 0; i < TELEM_ERRB_COUNT; i++)
        d.errb_reinits[i] = errb_reinits((errb_domain_t)i);

    /* §7 R0. */
    d.safe_state_entries = safe_state_entry_count();

    char chunk[128];   /* same streaming-chunk shape as the config replies */
    (void)telem_json_line(chunk, (int)sizeof chunk, telem_sink, NULL,
                          &s_tlm, &d);
}

void telem_stream_update(const ctrl_telemetry_t *t)
{
    if (t != NULL) s_tlm = *t;
}

void telem_stream_poll(void)
{
    const uint32_t now = HAL_GetTick();

    if (!usb_cdc_link_up()) {
        /* Keep the phase pinned to "one period after attach" so a host gets
         * its first line a second in, not a burst of backlog. */
        s_next_ms = now + TELEM_PERIOD_MS;
        return;
    }
    if ((int32_t)(now - s_next_ms) >= 0) {
        s_next_ms = now + TELEM_PERIOD_MS;
        telem_emit();
    }
}

void telem_stream_send_now(void)
{
    telem_emit();
}
