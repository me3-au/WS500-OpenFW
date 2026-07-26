/*
 * telemetry_json.h — the {"t":"telem"} JSON line for the USB-CDC wire (GH#35,
 * deliverable #20). PURE.
 * SPDX-License-Identifier: MIT
 *
 * One function turning the dialect-neutral telemetry snapshot (telemetry.h)
 * plus a plain-number diagnostics block into a single newline-terminated JSON
 * object, streamed through the same cfg_json_sink_fn the config protocol
 * replies use. Pure on purpose: the §7 robustness modules that SOURCE the
 * diagnostics (crash_record.h, integrity.h, err_budget.h, safe_state.h) are
 * Core/ and register-facing, so the Core-side pump (Core/Src/telem_stream.c)
 * flattens their answers into telem_diag_t and control/ never sees a HAL type
 * — which is what lets this emitter run in the native test suite byte for byte.
 *
 * ---------------------------------------------------------------------------
 * LINE SCHEMA (all keys always present, one line, LF-terminated)
 * ---------------------------------------------------------------------------
 * {"t":"telem","up_ms":U,
 *  "state":N,"standby":N,"profile":N,"field":F,"bind":N,"bind_w":F,
 *  "faults":U,"sev":N,
 *  "cells":N,"vbat":F,"vcell":F,"amps":F,"watts":F,
 *  "alt_c":F,"batt_c":F,"rpm":F,"rpm_st":N,
 *  "diag":{"resets":[U*7],"cause":U,"boots":U,
 *          "crashes":N,"crash_kind":N,"crash_pc":U,
 *          "crc":"ok|mismatch|unpatched|unavail",
 *          "stack_used":U,"stack_free":U,
 *          "errb":U,"errb_reinit":[U*3],"safe":U}}
 *
 * N = small integer, U = unsigned 32-bit decimal, F = float (non-finite → null,
 * the same "unset travels as null" rule the config wire uses, config_json.h).
 * Enum-valued fields carry the control core's own numbering (ctrl_state_t,
 * ctrl_standby_reason_t, ctrl_bind_src_t, ctrl_severity_t, ctrl_rpm_state_t,
 * ctrl_fault_bits_t) — the client owns pretty-printing, exactly as it owns the
 * config file (decision #6a-B). "crc" is a string because its four values are
 * a closed set this header defines, not a control-core enum.
 *
 * "diag" member meanings (all sourced from PROJECT_PLAN §7 surfaces):
 *   resets       lifetime per-cause reset counters, index order fixed by
 *                TELEM_RESET_* below (POR, NRST pin, software, IWDG, WWDG,
 *                low-power, option-byte-load)
 *   cause        this boot's decoded reset-cause bits (same bit order)
 *   boots        boots since the crash ring was last cold-initialised
 *   crashes      crash records currently held (0..ring length)
 *   crash_kind   newest record's kind (crash_record.h crash_kind_t; 0 = none)
 *   crash_pc     newest record's faulting PC (0 when crashes == 0)
 *   crc          boot-time flash image CRC verdict (§7 R4)
 *   stack_used / stack_free   stack high-watermark scan, bytes (§7 R4)
 *   errb         latched err-budget fault mask, bit order TELEM_ERRB_* (§7 R6)
 *   errb_reinit  per-domain reinit counters, same order
 *   safe         safe_state_entry_count() (§7 R0)
 */
#ifndef WS500_TELEMETRY_JSON_H
#define WS500_TELEMETRY_JSON_H

#include "telemetry.h"
#include "config_json.h"   /* cfg_json_sink_fn */

/* Fixed index order of diag.resets[] / bit order of diag.cause. The Core-side
 * pump maps crash_record.h's reset_cause_t onto these and static-asserts the
 * counts agree, so the wire order cannot drift from the firmware's. */
#define TELEM_RESET_POR       0
#define TELEM_RESET_PIN       1
#define TELEM_RESET_SOFTWARE  2
#define TELEM_RESET_IWDG      3
#define TELEM_RESET_WWDG      4
#define TELEM_RESET_LOWPOWER  5
#define TELEM_RESET_OBL       6
#define TELEM_RESET_COUNT     7

/* Fixed index/bit order of diag.errb / diag.errb_reinit[] (mirrors §7 R6's
 * errb_domain_t; asserted equal Core-side, same as the reset causes above). */
#define TELEM_ERRB_INA226     0
#define TELEM_ERRB_EEPROM     1
#define TELEM_ERRB_CAN        2
#define TELEM_ERRB_COUNT      3

/* Image-CRC verdict for telem_diag_t.crc_status → the "crc" string. A closed
 * set defined HERE (not integrity.h's enum) so the wire contract is owned by
 * the pure layer; Core maps integrity_crc_status_t onto it explicitly. */
#define TELEM_CRC_OK          0   /* "ok" */
#define TELEM_CRC_MISMATCH    1   /* "mismatch" */
#define TELEM_CRC_UNPATCHED   2   /* "unpatched" — ELF-loaded/debug build */
#define TELEM_CRC_UNAVAILABLE 3   /* "unavail" — CRC unit self-test failed */

/* The §7 diagnostics block, flattened to plain numbers by the Core-side pump.
 * Everything here is a copy — the emitter never reaches back into a module. */
typedef struct {
    uint32_t uptime_ms;                        /* HAL tick at emit time */
    uint32_t reset_counts[TELEM_RESET_COUNT];  /* lifetime, per cause */
    uint32_t reset_cause_bits;                 /* this boot, TELEM_RESET_* bits */
    uint32_t boot_count;
    uint8_t  crash_count;                      /* records held in the ring */
    uint16_t last_crash_kind;                  /* newest record; 0 = none */
    uint32_t last_crash_pc;
    uint8_t  crc_status;                       /* TELEM_CRC_* */
    uint32_t stack_used_max;                   /* bytes */
    uint32_t stack_free_min;                   /* bytes */
    uint32_t errb_fault_mask;                  /* TELEM_ERRB_* bits */
    uint32_t errb_reinits[TELEM_ERRB_COUNT];
    uint32_t safe_state_entries;
} telem_diag_t;

/*
 * Emit one complete telemetry line (schema above, including the trailing
 * newline) through `sink`, using `chunk`/`cap` as the streaming buffer exactly
 * like the config replies do (config_msg.h CFG_MSG_CHUNK is a good size).
 * Returns false if the writer overflowed (only possible with a NULL sink or a
 * pathological chunk size); the transport's best-effort contract makes the
 * return advisory — a dropped line is re-asked-for, never retried here.
 */
bool telem_json_line(char *chunk, int cap, cfg_json_sink_fn sink, void *ctx,
                     const ctrl_telemetry_t *t, const telem_diag_t *d);

#endif /* WS500_TELEMETRY_JSON_H */
