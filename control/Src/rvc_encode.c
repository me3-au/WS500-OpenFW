/*
 * rvc_encode.c — pure RV-C DGN encoders (contract: rvc_encode.h). PURE.
 * SPDX-License-Identifier: MIT
 */
#include "rvc_encode.h"
#include "faults.h"
#include <math.h>

/* ---- little-endian primitives (same idiom as n2k_encode.c's put_* helpers,
 * reproduced locally rather than shared — n2k_addrclaim.c already sets the
 * house precedent of each protocol module staying independent). ---------- */
static void put_u8(uint8_t *b, size_t *o, uint8_t v) { b[(*o)++] = v; }

static void put_u16(uint8_t *b, size_t *o, uint16_t v)
{
    b[(*o)++] = (uint8_t)(v & 0xFFu);
    b[(*o)++] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *b, size_t *o, uint32_t v)
{
    put_u16(b, o, (uint16_t)(v & 0xFFFFu));
    put_u16(b, o, (uint16_t)((v >> 16) & 0xFFFFu));
}

/* ---- scaled-field encoders. RV-C "not available" is the TOP code for the
 * field's width (rvc_encode.h); out-of-range finite values saturate ONE
 * code below that (house saturation policy, reused from n2k_encode.c for
 * in-repo consistency — see rvc_encode.h's file header). --------------- */

/* Unsigned, no offset: voltage fields and CHARGER_STATUS's plain (positive-
 * only) current field. Negative input clamps to 0, not NA — a value below
 * the field's representable floor is a range failure, not a missing
 * reading. */
static uint16_t enc_u16_unsigned(float v, double res)
{
    long long l;
    if (!isfinite(v)) return (uint16_t)RVC_NA_U16;
    l = llround((double)v / res);
    if (l < 0) l = 0;
    if (l > 0xFFFELL) l = 0xFFFE;
    return (uint16_t)l;
}

/* Offset-encoded (unsigned wire field representing a signed physical
 * quantity): raw = round(v/res) + zero_raw. Used for CHARGER_STATUS_2's
 * current (zero_raw 32000) and DC_SOURCE_STATUS_1's current (zero_raw
 * 2 000 000 000, see rvc_encode.h) — same idiom, different width/zero. */
static uint16_t enc_u16_offset(float v, double res, long zero_raw)
{
    long long l;
    if (!isfinite(v)) return (uint16_t)RVC_NA_U16;
    l = (long long)zero_raw + llround((double)v / res);
    if (l < 0) l = 0;
    if (l > 0xFFFELL) l = 0xFFFE;
    return (uint16_t)l;
}

static uint32_t enc_u32_offset(float v, double res, long long zero_raw)
{
    long long l;
    if (!isfinite(v)) return RVC_NA_U32;
    l = zero_raw + llround((double)v / res);
    if (l < 0) l = 0;
    if (l > 0xFFFFFFFELL) l = 0xFFFFFFFELL;
    return (uint32_t)l;
}

/* J1939 SPN "Type P" percent convention (0.4 %/bit, 0..250 = 0..100 %,
 * rvc_encode.h [SPEC-SIGNOFF]). `frac` is 0..1 (not 0..100). */
static uint8_t enc_pct(float frac, double res_pct_per_bit)
{
    long long l;
    if (!isfinite(frac)) return (uint8_t)RVC_NA_U8;
    l = llround((double)frac * 100.0 / res_pct_per_bit);
    if (l < 0) l = 0;
    if (l > 250) l = 250;
    return (uint8_t)l;
}

/* Classic J1939 SPN temperature (0.03125 °C/bit, offset -273 °C,
 * rvc_encode.h [SPEC-SIGNOFF]). */
static uint16_t enc_temp_j1939(float c)
{
    return enc_u16_unsigned(c + 273.0f, 0.03125);
}

/* CHARGER_STATUS_2's own 1-byte temperature (1 °C/bit, offset -40 °C,
 * thomasonw/RV-C-cited range, rvc_encode.h). */
static uint8_t enc_temp_u8_off40(float c)
{
    long long l;
    if (!isfinite(c)) return (uint8_t)RVC_NA_U8;
    l = llround((double)c) + 40;
    if (l < 0) l = 0;
    if (l > 0xFE) l = 0xFE;
    return (uint8_t)l;
}

/* CHARGER_STATUS operating-state mapping (rvc_encode.h): same 3-state
 * engine -> richer-enum translation n2k_encode.c's 127750 already performs,
 * targeting RV-C's own enum instead of N2K's (linuxkidd/rvc-monitor-py: 0
 * undefined, 1 do not charge, 2 bulk, 3 absorption, 4 overcharge, 5
 * equalize, 6 float, 7 constant voltage/current). RV-C's enum has no
 * distinct "fault" code, so a fault-severity reading maps to "do not
 * charge" (1) — the closest honest state, matching what the field driver
 * actually does on a fault (field open / limp). */
static uint8_t rvc_operating_state(const ctrl_telemetry_t *t)
{
    if (t->severity >= CTRL_SEV_FAULT) return 1u;         /* do not charge */
    if (t->state == CTRL_BULK)
        return (t->binding == CTRL_BIND_VOLTAGE_CLAMP) ? 3u : 2u; /* absorption/bulk */
    if (t->state == CTRL_FLOAT) return 6u;                /* float */
    return 1u;                                            /* standby -> do not charge */
}

/* ---- RVC_DGN_CHARGER_STATUS ---------------------------------------------- */
int rvc_encode_charger_status(const ctrl_telemetry_t *t, uint8_t instance,
                              uint8_t src, n2k_frame_t *out)
{
    size_t o = 0;
    float amps;
    if (!t || !out) return -1;

    /* Charge current cannot go negative in this field (rvc_encode.h) — a
     * net-discharge reading clamps at the field's own floor. */
    amps = t->amps_batt;
    if (isfinite(amps) && amps < 0.0f) amps = 0.0f;

    out->id  = n2k_can_id(RVC_PRI_STATUS, RVC_DGN_CHARGER_STATUS, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u16(out->data, &o, enc_u16_unsigned(t->vbat_pack_v, 0.05));  /* charge V */
    put_u16(out->data, &o, enc_u16_unsigned(amps, 0.05));            /* charge A */
    put_u8 (out->data, &o, enc_pct(t->field_effort, 0.4));           /* % of max */
    put_u8 (out->data, &o, rvc_operating_state(t));                  /* op state */
    /* byte7: default-on-power-up(2b NA) | auto-recharge-enable(2b NA) |
     * force-charge(4b, 15 = undefined — Rx not wired up, TODO(GH#10)). */
    put_u8 (out->data, &o,
            (uint8_t)((RVC_NA_BIT2 & 0x3u)
                     | ((RVC_NA_BIT2 & 0x3u) << 2)
                     | ((RVC_NA_BIT4 & 0xFu) << 4)));
    return 1;
}

/* ---- RVC_DGN_CHARGER_STATUS_2 (vendor-convergence DGN, see rvc_encode.h) - */
int rvc_encode_charger_status_2(const ctrl_telemetry_t *t, uint8_t instance,
                                uint8_t dc_source_instance,
                                uint8_t device_priority, uint8_t src,
                                n2k_frame_t *out)
{
    size_t o = 0;
    if (!t || !out) return -1;
    out->id  = n2k_can_id(RVC_PRI_STATUS, RVC_DGN_CHARGER_STATUS_2, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u8 (out->data, &o, dc_source_instance);
    put_u8 (out->data, &o, device_priority);
    put_u16(out->data, &o, enc_u16_unsigned(t->vbat_pack_v, 0.05));
    put_u16(out->data, &o, enc_u16_offset(t->amps_batt, 0.05, 32000));
    put_u8 (out->data, &o, enc_temp_u8_off40(t->alt_temp_c));
    return 1;
}

/* ---- RVC_DGN_DC_SOURCE_STATUS_1 ------------------------------------------ */
int rvc_encode_dc_source_status_1(const ctrl_telemetry_t *t, uint8_t instance,
                                  uint8_t device_priority, uint8_t src,
                                  n2k_frame_t *out)
{
    size_t o = 0;
    if (!t || !out) return -1;
    out->id  = n2k_can_id(RVC_PRI_STATUS, RVC_DGN_DC_SOURCE_STATUS_1, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u8 (out->data, &o, device_priority);
    put_u16(out->data, &o, enc_u16_unsigned(t->vbat_pack_v, 0.05));
    put_u32(out->data, &o, enc_u32_offset(t->amps_batt, 0.001, 2000000000LL));
    return 1;
}

/* ---- RVC_DGN_DC_SOURCE_STATUS_2 ------------------------------------------ */
int rvc_encode_dc_source_status_2(const ctrl_telemetry_t *t, uint8_t instance,
                                  uint8_t device_priority, uint8_t src,
                                  n2k_frame_t *out)
{
    size_t o = 0;
    if (!t || !out) return -1;
    out->id  = n2k_can_id(RVC_PRI_STATUS, RVC_DGN_DC_SOURCE_STATUS_2, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u8 (out->data, &o, device_priority);
    put_u16(out->data, &o, enc_temp_j1939(t->batt_temp_c));
    put_u8 (out->data, &o, (uint8_t)RVC_NA_U8);     /* SOC: not in v1 snapshot */
    put_u16(out->data, &o, (uint16_t)RVC_NA_U16);   /* time remaining: NA */
    put_u8 (out->data, &o, 0xFFu);                  /* reserved/pad */
    return 1;
}

/* ---- RVC_DGN_DC_SOURCE_STATUS_3 (encoder only — not scheduled, see
 * rvc_encode.h / rvc_sched.c) ---------------------------------------------- */
int rvc_encode_dc_source_status_3(uint8_t instance, uint8_t device_priority,
                                  uint8_t src, n2k_frame_t *out)
{
    size_t o = 0;
    if (!out) return -1;
    out->id  = n2k_can_id(RVC_PRI_STATUS, RVC_DGN_DC_SOURCE_STATUS_3, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u8 (out->data, &o, device_priority);
    put_u8 (out->data, &o, (uint8_t)RVC_NA_U8);     /* state of health */
    put_u16(out->data, &o, (uint16_t)RVC_NA_U16);   /* capacity remaining */
    put_u8 (out->data, &o, (uint8_t)RVC_NA_U8);     /* relative capacity */
    put_u16(out->data, &o, (uint16_t)RVC_NA_U16);   /* AC RMS ripple */
    return 1;
}
