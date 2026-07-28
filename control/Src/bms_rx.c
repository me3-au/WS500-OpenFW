/*
 * bms_rx.c — BMS/DVCC control-in: CAN-BMS/REC/JK vendor driver + the single
 * dialect-neutral interface (contract: bms_rx.h). PURE.
 * SPDX-License-Identifier: MIT
 */
#include "bms_rx.h"
#include <string.h>

/* ---- little-endian field decode, local to this module (n2k_addrclaim.c's
 * own precedent: "this module stays independent of that file's static
 * helpers" rather than sharing across protocol modules). ------------------ */
static int16_t get_i16_le(const uint8_t *d)
{
    return (int16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
}

static uint16_t get_u16_le(const uint8_t *d)
{
    return (uint16_t)((uint16_t)d[0] | ((uint16_t)d[1] << 8));
}

/* ---- wraparound-safe "has this signal gone stale" -----------------------
 * Unsigned-difference form (elapsed-since-last-seen), identical idiom to
 * rvc_sched.c's rvc_rbm_defer() — correct across a uint32_t tick wraparound
 * regardless of the absolute tick value, unlike a signed "now < deadline"
 * comparison would be for an ever-increasing deadline. */
static void sig_age(bms_sig_t *s, uint32_t now_ms)
{
    if (s->state == BMS_SIG_FRESH &&
        (uint32_t)(now_ms - s->last_rx_ms) >= BMS_RX_SIGNAL_TIMEOUT_MS) {
        s->state = BMS_SIG_STALE;
    }
}

static void sig_mark_fresh(bms_sig_t *s, uint32_t now_ms)
{
    s->last_rx_ms = now_ms;
    s->state      = BMS_SIG_FRESH;
}

/* ---- per-frame decode ----------------------------------------------------
 * Scale factors: [SPEC-SIGNOFF]/bench-pending, see bms_rx.h's file header —
 * reconstructed from the widely-implemented public "CAN-bus BMS" protocol
 * documentation, not verified against a REC/JK vendor datasheet or bench
 * unit. Frames shorter than the length required below are rejected whole by
 * the caller (bms_rx_frame()) before these run — every decode here assumes
 * `len` was already checked. */

/* 0x351 Charge/Discharge limits: CVL, CCL, DCL — each a signed int16 LE,
 * 0.1-unit resolution (0.1 V for CVL, 0.1 A for CCL/DCL). Requires 6 bytes;
 * a 4th field (discharge voltage limit) exists in some vendor variants but
 * is outside CAN_INTEGRATION.md's row-5/6 frame description and this
 * deliverable's scope, so it is neither required nor decoded. */
static void decode_351(bms_rx_t *b, const uint8_t *d)
{
    b->cvl_v = (float)get_i16_le(&d[0]) * 0.1f;   /* 0.1 V/bit */
    b->ccl_a = (float)get_i16_le(&d[2]) * 0.1f;   /* 0.1 A/bit */
    b->dcl_a = (float)get_i16_le(&d[4]) * 0.1f;   /* 0.1 A/bit, not consumed
                                                    * (no discharge path) */
}

/* 0x355 SOC/SOH: each an UNSIGNED int16 LE, 1 %/bit (not the 0.1 CVL/CCL
 * scale — this pair is documented as whole percent, 0..100). Requires 4
 * bytes. */
static void decode_355(bms_rx_t *b, const uint8_t *d)
{
    b->soc_pct = (float)get_u16_le(&d[0]);   /* 1 %/bit */
    b->soh_pct = (float)get_u16_le(&d[2]);   /* 1 %/bit, no core consumer */
}

/* 0x356 pack Voltage/Current/Temperature: each a signed int16 LE — 0.01 V/bit
 * for voltage, 0.1 A/bit for current (+ = charging, matching
 * ctrl_measured_t.amps_batt's own sign convention, control.h), 0.1 °C/bit
 * for temperature. Requires 6 bytes. Informational only in this deliverable
 * — see bms_rx.h's file header. */
static void decode_356(bms_rx_t *b, const uint8_t *d)
{
    b->pack_v      = (float)get_i16_le(&d[0]) * 0.01f;
    b->pack_i_a    = (float)get_i16_le(&d[2]) * 0.1f;
    b->pack_temp_c = (float)get_i16_le(&d[4]) * 0.1f;
}

/* 0x35A alarms/warnings: bytes[0:1] = alarm (protection-level) bit field,
 * bytes[2:3] = warning (pre-alarm) bit field. Decoded at BYTE-PAIR
 * granularity only — bms_rx.h's file header explains why individual bit
 * assignments within each pair are not trusted here. Requires 4 bytes. */
static void decode_35a(bms_rx_t *b, const uint8_t *d)
{
    b->alarm_active   = (d[0] != 0u) || (d[1] != 0u);
    b->warning_active = (d[2] != 0u) || (d[3] != 0u);
}

/* 0x35C Charge/Discharge enable: byte[0] bit 0 = charge enable, bit 1 =
 * discharge enable (not decoded -- no discharge path, same "not this
 * regulator's business" reasoning 0x351's DCL field documents). [SPEC-SIGNOFF]
 * / bench-pending, same standing as every other layout in this file (bms_rx.h
 * file header): bit position reconstructed from the same public CAN-BMS/
 * Victron-compatible documentation, no vendor datasheet in-tree to verify
 * against byte-for-byte. Requires 1 byte; some vendor variants send up to 8
 * with additional immediate-charge/discharge flags this driver does not
 * need. */
static void decode_35c(bms_rx_t *b, const uint8_t *d)
{
    b->charge_enabled = (d[0] & 0x01u) != 0u;
}

/* ---- public API (contract: bms_rx.h) ------------------------------------ */

void bms_rx_init(bms_rx_t *b)
{
    bms_rx_t z;
    if (!b) return;
    memset(&z, 0, sizeof z);   /* .limits/.soc/.status/.alarm/.enable ->
                               * state=NEVER (enum 0), last_rx_ms=0: no BMS
                               * wired reads as NEVER, not a fault (bms_rx.h).
                               * charge_enabled=false from this memset is
                               * inert -- see bms_rx_t's doc comment on that
                               * field. */
    z.cvl_v       = NAN;
    z.ccl_a       = NAN;
    z.dcl_a       = NAN;
    z.soc_pct     = NAN;
    z.soh_pct     = NAN;
    z.pack_v      = NAN;
    z.pack_i_a    = NAN;
    z.pack_temp_c = NAN;
    *b = z;
}

void bms_rx_frame(bms_rx_t *b, uint32_t now_ms, uint32_t id,
                  const uint8_t *data, size_t len)
{
    if (!b || !data) return;

    switch (id) {
    case BMS_RX_ID_LIMITS:
        if (len < 6u) return;             /* malformed/short: drop whole frame */
        decode_351(b, data);
        sig_mark_fresh(&b->limits, now_ms);
        break;
    case BMS_RX_ID_SOC:
        if (len < 4u) return;
        decode_355(b, data);
        sig_mark_fresh(&b->soc, now_ms);
        break;
    case BMS_RX_ID_STATUS:
        if (len < 6u) return;
        decode_356(b, data);
        sig_mark_fresh(&b->status, now_ms);
        break;
    case BMS_RX_ID_ALARM:
        if (len < 4u) return;
        decode_35a(b, data);
        sig_mark_fresh(&b->alarm, now_ms);
        break;
    case BMS_RX_ID_ENABLE:
        if (len < 1u) return;
        decode_35c(b, data);
        sig_mark_fresh(&b->enable, now_ms);
        break;
    default:
        return;   /* not a frame this driver decodes */
    }
}

/* Amps -> Watts at the given pack voltage. Conservative-by-construction:
 * `vbat_pack_v > 0.0f` is false for NaN, negative, and zero alike (IEEE 754
 * comparison semantics, no isnan() needed — identical idiom to limits.c's
 * ctrl_limits_apply()), and a negative-CCL frame (no vendor doc confirms one
 * exists, but nothing rules it out either) is treated the same as "no CCL"
 * rather than trusted as a sign-inverted ceiling. Both failure paths return
 * CTRL_CEILING_INACTIVE, never a wrong finite number — bms_rx_snapshot()'s
 * caller relies on INACTIVE ceilings dropping out of arbitration.c's min()
 * by construction (arbitration.c's own comment: "< is false for +inf and for
 * NaN"). */
static float ccl_to_watts(float ccl_a, float vbat_pack_v)
{
    if (!(vbat_pack_v > 0.0f)) return CTRL_CEILING_INACTIVE;
    if (!(ccl_a >= 0.0f))      return CTRL_CEILING_INACTIVE;
    return ccl_a * vbat_pack_v;
}

void bms_rx_snapshot(bms_rx_t *b, uint32_t now_ms, float vbat_pack_v,
                     bms_snapshot_t *out)
{
    bool limits_fresh, soc_fresh, alarm_fresh, enable_fresh;
    if (!b || !out) return;

    sig_age(&b->limits, now_ms);
    sig_age(&b->soc,    now_ms);
    sig_age(&b->status, now_ms);
    sig_age(&b->alarm,  now_ms);
    sig_age(&b->enable, now_ms);

    limits_fresh = (b->limits.state == BMS_SIG_FRESH);
    soc_fresh    = (b->soc.state    == BMS_SIG_FRESH);
    alarm_fresh  = (b->alarm.state  == BMS_SIG_FRESH);
    enable_fresh = (b->enable.state == BMS_SIG_FRESH);

    /* CCL ceiling + CVL: only from a FRESH limits signal. Staleness (or
     * never-seen) falls back to CTRL_CEILING_INACTIVE / NAN — i.e. "this
     * driver contributes nothing" — which is exactly CAN_INTEGRATION.md
     * §1's "falls back to its own profile limits": the OTHER ceilings
     * (battery C-rate, wiring, alternator absolute, profile stage) stay in
     * arbitration.c's min() regardless, so the regulator never free-runs
     * just because the BMS went quiet. */
    out->ccl_w = limits_fresh ? ccl_to_watts(b->ccl_a, vbat_pack_v)
                              : CTRL_CEILING_INACTIVE;
    out->cvl_v = limits_fresh ? b->cvl_v : NAN;

    /* A fresh, active protection-level alarm overrides CCL to the strictest
     * possible ceiling (0 W) regardless of what the CCL field itself says —
     * a BMS that is alarming AND still reporting a nonzero CCL is exactly
     * the "lying" case CONTROL_SPEC_NEXTGEN.md §2.1's battery-C-limit note
     * anticipates ("protects when BMS comms are absent or lying"), and this
     * driver does not need to know WHICH alarm fired to know the safe
     * response is "stop". This is arbitration-ceiling behaviour, not a new
     * fault bit — see bms_rx.h's file header on why touching faults.h/
     * ctrl_fault_bits_t is out of this deliverable's scope. */
    out->alarm_active = alarm_fresh && b->alarm_active;
    if (out->alarm_active) out->ccl_w = 0.0f;
    out->warning_active = alarm_fresh && b->warning_active;

    /* ALARM-THEN-SILENCE LATCH. If the last alarm frame we heard said
     * "alarming" and the signal has since gone STALE, keep forcing 0 W until
     * a fresh 0x35A actually clears it — do NOT let the timeout release the
     * stop.
     *
     * Alarm-then-silence is not the same event as plain silence, and the
     * difference matters because the two failure modes are CORRELATED: a BMS
     * opening its protection contactor can lose its own supply and stop
     * transmitting, so "cell over-voltage" followed by silence is the
     * signature of the alarm being ACTED ON, not of it going away. Releasing
     * on the timeout would resume charging (into LIMP, since alarm-staleness
     * also sets `lost` below — but charging nonetheless) into a pack whose
     * last known state was a protection alarm.
     *
     * Only a live BMS saying "clear" clears it. That is the strictly
     * conservative reading of the last thing the battery told us, and it is
     * still bounded: a genuinely-recovered BMS re-asserts freshness on its
     * next broadcast. [SPEC-SIGNOFF] */
    if (!alarm_fresh && b->alarm.state == BMS_SIG_STALE && b->alarm_active)
        out->ccl_w = 0.0f;

    /* SOC is range-checked before it is trusted, because it is the ONE input
     * here that can make the regulator charge HARDER rather than softer: a
     * low SOC satisfies `soc_revert_pct` and forces REST/FLOAT->BULK
     * (control.c), so a node reporting 0 % every tick would pin the pack at
     * the CV target indefinitely. Everything else on this interface is a
     * ceiling, and a bad ceiling can only ever under-permit.
     *
     * Out-of-domain values are treated as UNKNOWN, not clamped: 0xFFFF is a
     * common "not available" fill in this very frame family and decodes to
     * 65535 %, which would otherwise read as "trusted, completely full" —
     * silently satisfying the T2b exit and skip-bulk-when-full. Clamping that
     * to 100 % would invent a confident reading out of a null; -1/untrusted
     * is the honest answer and the core already knows what to do with it.
     * The 0..100 domain is what bms_rx.h documents for this field. */
    {
        const bool soc_sane = soc_fresh &&
                              (b->soc_pct >= 0.0f) && (b->soc_pct <= 100.0f);
        out->soc_pct     = soc_sane ? b->soc_pct : -1.0f;  /* ctrl_measured_t's
                                                            * own "-1 = unknown"
                                                            * convention */
        out->soc_trusted = soc_sane;
    }

    /* deliverable #10: pre_disconnect from 0x35C's charge-enable bit
     * deasserting -- see bms_rx.h's file header for why this is the honest
     * source in this frame family. */
    out->pre_disconnect = enable_fresh && !b->charge_enabled;

    /* PRE-DISCONNECT-THEN-SILENCE LATCH -- same reasoning as the
     * ALARM-THEN-SILENCE latch above, applied to the ENABLE signal
     * specifically: if the last 0x35C we heard said "charge disabled" and the
     * ENABLE signal has since gone STALE, keep reporting pre_disconnect until
     * a fresh 0x35C actually re-asserts charge-enable. A BMS that deasserts
     * charge-enable and then stops transmitting is the signature of the
     * disconnect actually happening (it may be opening its own contactor and
     * losing its own supply), not of the warning clearing.
     *
     * Deliberately NOT triggered by ordinary comms loss: a signal that was
     * NEVER fresh (no BMS wired, or this frame simply isn't sent), or one
     * that was last fresh with charge_enabled=true before going stale, does
     * NOT synthesize a pre-disconnect here -- that is plain silence, and its
     * declared, already-reviewed fallback is `lost` below -> ext_faults ->
     * CTRL_FAULT_LOST_BMS -> LIMP. This driver does not invent a harder
     * response than the spec assigns to ordinary comms loss; it only latches
     * the specific case where the last thing the BMS said was "stop". */
    if (!enable_fresh && b->enable.state == BMS_SIG_STALE && !b->charge_enabled)
        out->pre_disconnect = true;

    /* "Lost" = the safety-relevant signal group (limits, alarm, and/or
     * enable) WAS fresh and has now gone silent — never true merely because
     * no BMS is wired at all (bms_sig_state_t's NEVER vs STALE distinction,
     * bms_rx.h). SOC/status staleness alone does not set this: each already
     * degrades on its own (soc_trusted=false above) without needing a
     * whole-system LIMP disposition. */
    out->lost = (b->limits.state == BMS_SIG_STALE) ||
               (b->enable.state  == BMS_SIG_STALE) ||
               (b->alarm.state  == BMS_SIG_STALE);
}
