/*
 * telemetry_json.c — emit the {"t":"telem"} line (GH#35, deliverable #20). PURE.
 * Schema and field meanings: telemetry_json.h.
 *
 * Uses the config wire's own streaming writer (config_json.h), so the line
 * inherits every property the config replies already proved: bounded RAM (one
 * chunk buffer), floats that read back bit-exact, and non-finite values
 * travelling as null.
 *
 * SPDX-License-Identifier: MIT
 */
#include "telemetry_json.h"

/* Unsigned 32-bit decimal. cfg_json_w_int() takes a long, which is 32-bit on
 * the target — a crash PC in the 0x8000_0000+ range (corrupt frame) would come
 * out negative. Raw emission through the writer keeps the comma bookkeeping. */
static void w_u32(cfg_json_w_t *w, uint32_t v)
{
    char rev[10];   /* 4294967295 = 10 digits */
    int  n = 0;
    do {
        rev[n++] = (char)('0' + (char)(v % 10u));
        v /= 10u;
    } while (v != 0u);

    char out[10];
    for (int i = 0; i < n; i++) out[i] = rev[n - 1 - i];
    cfg_json_w_raw(w, out, n);
}

static const char *crc_str(uint8_t s)
{
    switch (s) {
    case TELEM_CRC_OK:        return "ok";
    case TELEM_CRC_MISMATCH:  return "mismatch";
    case TELEM_CRC_UNPATCHED: return "unpatched";
    default:                  return "unavail";
    }
}

bool telem_json_line(char *chunk, int cap, cfg_json_sink_fn sink, void *ctx,
                     const ctrl_telemetry_t *t, const telem_diag_t *d)
{
    if (chunk == NULL || t == NULL || d == NULL) return false;

    cfg_json_w_t w;
    cfg_json_w_init(&w, chunk, cap, sink, ctx);

    cfg_json_w_obj(&w);
    cfg_json_w_key(&w, "t");       cfg_json_w_str(&w, "telem");
    cfg_json_w_key(&w, "up_ms");   w_u32(&w, d->uptime_ms);

    /* Control state — enum values are the control core's own (telemetry_json.h). */
    cfg_json_w_key(&w, "state");   cfg_json_w_int(&w, (long)t->state);
    cfg_json_w_key(&w, "standby"); cfg_json_w_int(&w, (long)t->standby_reason);
    cfg_json_w_key(&w, "profile"); cfg_json_w_int(&w, (long)t->active_profile_id);
    cfg_json_w_key(&w, "field");   cfg_json_w_f32(&w, t->field_effort);
    cfg_json_w_key(&w, "bind");    cfg_json_w_int(&w, (long)t->binding);
    cfg_json_w_key(&w, "bind_w");  cfg_json_w_f32(&w, t->binding_w);
    cfg_json_w_key(&w, "faults");  w_u32(&w, t->faults);
    cfg_json_w_key(&w, "sev");     cfg_json_w_int(&w, (long)t->severity);

    /* Measured — NAN (sensor lost / not fitted) becomes null on the wire. */
    cfg_json_w_key(&w, "cells");   cfg_json_w_int(&w, (long)t->cells_series);
    cfg_json_w_key(&w, "vbat");    cfg_json_w_f32(&w, t->vbat_pack_v);
    cfg_json_w_key(&w, "vcell");   cfg_json_w_f32(&w, t->v_cell);
    cfg_json_w_key(&w, "amps");    cfg_json_w_f32(&w, t->amps_batt);
    cfg_json_w_key(&w, "watts");   cfg_json_w_f32(&w, t->watts_batt);
    cfg_json_w_key(&w, "alt_c");   cfg_json_w_f32(&w, t->alt_temp_c);
    cfg_json_w_key(&w, "batt_c");  cfg_json_w_f32(&w, t->batt_temp_c);
    cfg_json_w_key(&w, "rpm");     cfg_json_w_f32(&w, t->rpm);
    cfg_json_w_key(&w, "rpm_st");  cfg_json_w_int(&w, (long)t->rpm_state);

    /* §7 diagnostics block. */
    cfg_json_w_key(&w, "diag");
    cfg_json_w_obj(&w);

    cfg_json_w_key(&w, "resets");
    cfg_json_w_arr(&w);
    for (int i = 0; i < TELEM_RESET_COUNT; i++) w_u32(&w, d->reset_counts[i]);
    cfg_json_w_end(&w);
    cfg_json_w_key(&w, "cause");      w_u32(&w, d->reset_cause_bits);
    cfg_json_w_key(&w, "boots");      w_u32(&w, d->boot_count);

    cfg_json_w_key(&w, "crashes");    cfg_json_w_int(&w, (long)d->crash_count);
    cfg_json_w_key(&w, "crash_kind"); cfg_json_w_int(&w, (long)d->last_crash_kind);
    cfg_json_w_key(&w, "crash_pc");   w_u32(&w, d->last_crash_pc);

    cfg_json_w_key(&w, "crc");        cfg_json_w_str(&w, crc_str(d->crc_status));
    cfg_json_w_key(&w, "stack_used"); w_u32(&w, d->stack_used_max);
    cfg_json_w_key(&w, "stack_free"); w_u32(&w, d->stack_free_min);

    cfg_json_w_key(&w, "errb");       w_u32(&w, d->errb_fault_mask);
    cfg_json_w_key(&w, "errb_reinit");
    cfg_json_w_arr(&w);
    for (int i = 0; i < TELEM_ERRB_COUNT; i++) w_u32(&w, d->errb_reinits[i]);
    cfg_json_w_end(&w);
    cfg_json_w_key(&w, "safe");       w_u32(&w, d->safe_state_entries);

    cfg_json_w_end(&w);   /* diag */
    cfg_json_w_end(&w);   /* line object */

    const bool ok = cfg_json_w_flush(&w);
    if (sink != NULL) sink(ctx, "\n", 1);
    return ok;
}
