/*
 * n2k_encode.c — pure NMEA 2000 PGN encoders (contract: n2k_encode.h). PURE.
 *
 * Snapshot → raw CAN frames for the CAN_INTEGRATION.md §2 Tx set. Field
 * order/units per the public N2K PGN definitions (the MIT ttlappalainen/
 * NMEA2000 headers used as reference for layouts — facts only, no code
 * copied). All multi-byte fields little-endian; reserved bits transmit 1s.
 * SPDX-License-Identifier: MIT
 */
#include "n2k_encode.h"
#include "faults.h"
#include <string.h>
#include <math.h>

/* Default priorities: 2 for rapid-update and alert PGNs, 6 for periodic
 * status/info — matches the defaults the ttlappalainen/NMEA2000 stack (and
 * common commercial devices) put on the bus for these PGNs. */
#define PRI_RAPID   2u
#define PRI_ALERT   2u
#define PRI_STATUS  6u

/* ---- little-endian primitives (same walk-the-offset idiom as
 * config_codec.c: encode is a linear sequence of put_* calls, so the byte
 * layout is readable top-to-bottom). ------------------------------------- */
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

static void put_u64(uint8_t *b, size_t *o, uint64_t v)
{
    put_u32(b, o, (uint32_t)(v & 0xFFFFFFFFu));
    put_u32(b, o, (uint32_t)((v >> 32) & 0xFFFFFFFFu));
}

/* ---- scaled-field encoders: NaN/±inf → not-available, out-of-range
 * SATURATES at the last valid code (header rationale: a pegged gauge is
 * more honest than a blank one). ----------------------------------------- */
static uint16_t enc_u16(float v, double res)
{
    long long l;
    if (!isfinite(v)) return (uint16_t)N2K_NA_U16;
    l = llround((double)v / res);
    if (l < 0) l = 0;
    if (l > 0xFFFC) l = 0xFFFC;
    return (uint16_t)l;
}

static uint16_t enc_s16(float v, double res)
{
    long long l;
    if (!isfinite(v)) return (uint16_t)N2K_NA_S16;
    l = llround((double)v / res);
    if (l < -32768) l = -32768;
    if (l > 0x7FFC) l = 0x7FFC;
    return (uint16_t)((uint32_t)l & 0xFFFFu);
}

/* Celsius → wire Kelvin (0.01 K/bit u16). */
static uint16_t enc_temp_c(float c) { return enc_u16(c + 273.15f, 0.01); }

/* Normalized effort 0..1 → 0.5 %/bit, valid 0..200, NA 0xFF. */
static uint8_t enc_effort(float e)
{
    long long l;
    if (!isfinite(e)) return N2K_NA_U8;
    l = llround((double)e * 200.0);
    if (l < 0) l = 0;
    if (l > 200) l = 200;
    return (uint8_t)l;
}

/* ---- CAN identifier composition (J1939-21 / N2K): 3-bit priority, 18-bit
 * PGN field (EDP always 0 on N2K), 8-bit source address. ------------------ */
uint32_t n2k_can_id(uint8_t priority, uint32_t pgn, uint8_t src, uint8_t dest)
{
    uint32_t id_pgn = pgn & 0x1FFFFu;
    if (((id_pgn >> 8) & 0xFFu) < 240u)          /* PDU1: PS byte = dest */
        id_pgn = (id_pgn & 0x1FF00u) | dest;
    return ((uint32_t)(priority & 0x7u) << 26) | (id_pgn << 8) | src;
}

uint8_t n2k_can_id_prio(uint32_t id) { return (uint8_t)((id >> 26) & 0x7u); }

uint32_t n2k_can_id_pgn(uint32_t id)
{
    uint32_t pgn = (id >> 8) & 0x1FFFFu;
    if (((pgn >> 8) & 0xFFu) < 240u)             /* PDU1: dest is not PGN */
        pgn &= 0x1FF00u;
    return pgn;
}

uint8_t n2k_can_id_src(uint32_t id) { return (uint8_t)(id & 0xFFu); }

/* SID cycles 0..252; 253/254 are reserved and 255 means "not available",
 * so the counter must never emit them. */
uint8_t n2k_sid_next(uint8_t *sid)
{
    uint8_t v = *sid;
    if (v > 252u) v = 0u;                        /* heal an out-of-range seed */
    *sid = (uint8_t)((v >= 252u) ? 0u : (v + 1u));
    return v;
}

/* ---- fast-packet transport framing (contract: n2k_encode.h) ------------- */
int n2k_fp_split(uint32_t can_id, uint8_t seq,
                 const uint8_t *payload, size_t len,
                 n2k_frame_t *out, size_t max_frames)
{
    size_t nframes, off = 0, f;
    if (!payload || !out || len == 0 || len > N2K_FP_MAX_PAYLOAD) return -1;
    nframes = 1u + ((len > 6u) ? ((len - 6u + 6u) / 7u) : 0u);
    if (nframes > max_frames) return -1;
    for (f = 0; f < nframes; f++) {
        n2k_frame_t *fr = &out[f];
        size_t di = 1;
        fr->id  = can_id;
        fr->dlc = 8;
        fr->data[0] = (uint8_t)(((seq & 0x7u) << 5) | (f & 0x1Fu));
        if (f == 0) fr->data[di++] = (uint8_t)len; /* total payload length */
        while (di < 8)
            fr->data[di++] = (off < len) ? payload[off++] : 0xFFu;
    }
    return (int)nframes;
}

/* ---- PGN 127508 Battery Status ------------------------------------------ */
int n2k_encode_127508(const ctrl_telemetry_t *t, uint8_t instance,
                      uint8_t sid, uint8_t src, n2k_frame_t *out)
{
    size_t o = 0;
    if (!t || !out) return -1;
    out->id  = n2k_can_id(PRI_STATUS, 127508u, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u16(out->data, &o, enc_s16(t->vbat_pack_v, 0.01)); /* 0.01 V/bit */
    put_u16(out->data, &o, enc_s16(t->amps_batt,   0.1));  /* 0.1 A, + = charge */
    put_u16(out->data, &o, enc_temp_c(t->batt_temp_c));    /* 0.01 K/bit */
    put_u8 (out->data, &o, sid);
    return 1;
}

/* ---- PGN 127506 DC Detailed Status (fast packet, 11 B) ------------------ *
 * SOC/SOH/time-remaining/ripple/capacity are not in the v1 snapshot — they
 * transmit as not-available (see header). DC type 0 = battery: we report
 * the bank we regulate, per §2 "when acting as bank monitor". */
int n2k_encode_127506(const ctrl_telemetry_t *t, uint8_t instance,
                      uint8_t sid, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames)
{
    uint8_t p[11];
    size_t o = 0;
    if (!t || !out) return -1;
    put_u8 (p, &o, sid);
    put_u8 (p, &o, instance);
    put_u8 (p, &o, 0u);                 /* DC type: 0 = battery */
    put_u8 (p, &o, N2K_NA_U8);          /* state of charge, % */
    put_u8 (p, &o, N2K_NA_U8);          /* state of health, % */
    put_u16(p, &o, (uint16_t)N2K_NA_U16);   /* time remaining, min */
    put_u16(p, &o, (uint16_t)N2K_NA_U16);   /* ripple, 0.001 V/bit */
    put_u16(p, &o, (uint16_t)N2K_NA_U16);   /* capacity, 1 Ah/bit */
    return n2k_fp_split(n2k_can_id(PRI_STATUS, 127506u, src, 0xFFu),
                        seq, p, o, out, max_frames);
}

/* ---- PGN 127488 Engine Parameters Rapid --------------------------------- */
int n2k_encode_127488(const ctrl_telemetry_t *t, uint8_t instance,
                      uint8_t src, n2k_frame_t *out)
{
    size_t o = 0;
    float rpm;
    if (!t || !out) return -1;
    /* A stale/lost tach must read blank, not frozen at the last value —
     * the MFD has no other way to know the reading is dead. */
    rpm = (t->rpm_state == CTRL_RPM_VALID) ? t->rpm : NAN;
    out->id  = n2k_can_id(PRI_RAPID, 127488u, src, 0xFFu);
    out->dlc = 8;
    put_u8 (out->data, &o, instance);
    put_u16(out->data, &o, enc_u16(rpm, 0.25));     /* 0.25 rpm/bit */
    put_u16(out->data, &o, (uint16_t)N2K_NA_U16);   /* boost pressure */
    put_u8 (out->data, &o, 0x7Fu);                  /* tilt/trim s8 NA */
    put_u8 (out->data, &o, 0xFFu);                  /* reserved */
    put_u8 (out->data, &o, 0xFFu);
    return 1;
}

/* ---- PGN 127750 Converter (Charger) Status ------------------------------ *
 * Operating-state codes (public 127750 definition): 0 off, 2 fault, 3 bulk,
 * 4 absorption, 5 float. Mapping from the 3-state engine: BULK holding at
 * the CV clamp is what legacy chargers call "absorption", so we report 4
 * there — a Cerbo then shows the familiar bulk→absorption→float ladder even
 * though the engine itself has no absorption state (CONTROL_SPEC App. A). */
int n2k_encode_127750(const ctrl_telemetry_t *t, uint8_t connection,
                      uint8_t sid, uint8_t src, n2k_frame_t *out)
{
    size_t o = 0;
    uint8_t op, temp_state, bits;
    if (!t || !out) return -1;

    if (t->severity >= CTRL_SEV_FAULT)      op = 2u;    /* fault */
    else if (t->state == CTRL_BULK)
        op = (t->binding == CTRL_BIND_VOLTAGE_CLAMP) ? 4u : 3u;
    else if (t->state == CTRL_FLOAT)        op = 5u;    /* float */
    else                                    op = 0u;    /* standby → off */

    /* 2-bit status fields: 0 ok, 1 warning, 2 over-limit, 3 not available. */
    /* GH#39: the driver-stage governor and its 125 C fault report here on the
     * same footing as the alternator's — omitting them made an MFD read
     * "temperature OK" while the field was actively being derated for a hot
     * switch stage, which is the one moment the field is worth reporting. */
    if (t->faults & (CTRL_FAULT_SELF_OVERTEMP | CTRL_FAULT_BATT_HIGHTEMP |
                     CTRL_FAULT_DRIVER_OVERTEMP))
        temp_state = 2u;
    else if (t->binding == CTRL_BIND_THERMAL || t->binding == CTRL_BIND_DRIVER_THERMAL)
        temp_state = 1u;                    /* a governor is derating */
    else
        temp_state = 0u;
    bits = (uint8_t)(temp_state            /* temperature state   b0-1 */
           | (0u << 2)                     /* overload state      b2-3 */
           | (0u << 4)                     /* low-DC-voltage      b4-5 */
           | (3u << 6));                   /* ramping: not known  b6-7 */

    out->id  = n2k_can_id(PRI_STATUS, 127750u, src, 0xFFu);
    out->dlc = 8;
    put_u8(out->data, &o, sid);
    put_u8(out->data, &o, connection);
    put_u8(out->data, &o, op);
    put_u8(out->data, &o, bits);
    while (o < 8) put_u8(out->data, &o, 0xFFu);     /* reserved */
    return 1;
}

/* ---- alerts: fault bitfield → N2K Alert group --------------------------- */

/* Severity → N2K alert type (1 emergency alarm, 2 alarm, 5 warning,
 * 8 caution) and alert priority byte (lower = more urgent). */
static uint8_t alert_type_for(ctrl_severity_t sev)
{
    switch (sev) {
    case CTRL_SEV_CRITICAL: return 1u;
    case CTRL_SEV_FAULT:    return 2u;
    case CTRL_SEV_WARN:     return 5u;
    default:                return 8u;
    }
}

static uint8_t alert_prio_for(ctrl_severity_t sev)
{
    switch (sev) {
    case CTRL_SEV_CRITICAL: return 1u;
    case CTRL_SEV_FAULT:    return 32u;
    case CTRL_SEV_WARN:     return 128u;
    default:                return 200u;
    }
}

int n2k_alert_from_telemetry(const ctrl_telemetry_t *t, uint64_t device_name,
                             n2k_alert_t *a)
{
    unsigned code;
    ctrl_severity_t best;
    if (!t || !a) return -1;
    if (t->faults == 0) return 0;
    /* GH#42: the highest-severity/lowest-code pick is now shared with the LED
     * annunciator (faults.h ctrl_fault_top_code(), annunc.c) instead of a
     * second copy of this loop, so §9.2's "deterministically the same pick"
     * is structural, not a coincidence of two independent implementations. */
    code = ctrl_fault_top_code(t->faults);
    best = ctrl_fault_severity(1u << (code - 1u));
    memset(a, 0, sizeof *a);
    a->type             = alert_type_for(best);
    a->category         = 1u;                   /* technical */
    a->system           = 0u;
    a->subsystem        = 0u;
    a->alert_id         = (uint16_t)code;        /* bit index + 1 (§9.1) */
    a->source_name      = device_name;
    a->source_instance  = 0u;
    a->source_index     = 0u;
    a->occurrence       = 1u;
    a->trigger          = 2u;                   /* auto-triggered */
    a->threshold_status = 1u;                   /* threshold exceeded */
    a->priority         = alert_prio_for(best);
    a->state            = 2u;                   /* active */
    return 1;
}

/* Plain-language fault text (CAN_INTEGRATION.md §2 "plain-language") — what
 * a user sees on an MFD, so say what happened AND what the regulator did. */
const char *n2k_alert_text(uint32_t fault_bit)
{
    switch (fault_bit) {
    case CTRL_FAULT_OVERVOLTAGE:      return "Battery overvoltage - field opened";
    case CTRL_FAULT_FIELD_SHORT:      return "Field driver short - field opened";
    case CTRL_FAULT_FIELD_OPEN:       return "Field circuit open";
    case CTRL_FAULT_FIELD_OVERCUR:    return "Field overcurrent - field opened";
    case CTRL_FAULT_SELF_OVERTEMP:    return "Regulator over temperature";
    case CTRL_FAULT_OVERSPEED:        return "Alternator overspeed - field opened";
    case CTRL_FAULT_SHUNT_OPEN:       return "Current shunt open circuit";
    case CTRL_FAULT_SHUNT_REVERSED:   return "Current shunt reversed";
    case CTRL_FAULT_BATT_DTDT:        return "Battery heating too fast - charge aborted";
    case CTRL_FAULT_BATT_LOWTEMP:     return "Battery too cold to charge";
    case CTRL_FAULT_BATT_HIGHTEMP:    return "Battery too hot - charge aborted";
    case CTRL_FAULT_LOST_VBAT_SENSE:  return "Lost battery voltage sense - limp home";
    case CTRL_FAULT_LOST_BMS:         return "Lost BMS communication - limp home";
    case CTRL_FAULT_IMPLAUSIBLE_SHUNT:return "Implausible shunt reading - limp home";
    case CTRL_FAULT_THERMAL_DIVERGE:  return "Thermal model divergence";
    case CTRL_FAULT_WATCHDOG:         return "Watchdog reset - field opened";
    case CTRL_FAULT_VSUP_IMPLAUSIBLE: return "Field supply reading distrusted";
    case CTRL_FAULT_BATT_TEMP_REQUIRED: return "Battery temp required - charge blocked";
    case CTRL_FAULT_DRIVER_OVERTEMP:  return "Driver over-temp - field opened";
    default:                          return "unknown fault";
    }
}

/* Shared 16-byte alert-identity prefix of 126983/126985. Nibble order: the
 * first-listed field occupies the LOW nibble (N2K bit-field convention). */
static void put_alert_ident(uint8_t *b, size_t *o, const n2k_alert_t *a)
{
    put_u8 (b, o, (uint8_t)((a->type & 0x0Fu) | ((a->category & 0x0Fu) << 4)));
    put_u8 (b, o, a->system);
    put_u8 (b, o, a->subsystem);
    put_u16(b, o, a->alert_id);
    put_u64(b, o, a->source_name);
    put_u8 (b, o, a->source_instance);
    put_u8 (b, o, a->source_index);
    put_u8 (b, o, a->occurrence);
}

int n2k_encode_126983(const n2k_alert_t *a, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames)
{
    uint8_t p[28];
    size_t o = 0;
    if (!a || !out) return -1;
    put_alert_ident(p, &o, a);
    /* Status flags b0-5 all zero (not silenced / not acknowledged / not
     * escalated, none of those supported), reserved b6-7 = 1s. */
    put_u8 (p, &o, 0xC0u);
    put_u64(p, &o, UINT64_MAX);         /* acknowledge NAME: not available */
    put_u8 (p, &o, (uint8_t)((a->trigger & 0x0Fu)
                             | ((a->threshold_status & 0x0Fu) << 4)));
    put_u8 (p, &o, a->priority);
    put_u8 (p, &o, a->state);
    return n2k_fp_split(n2k_can_id(PRI_ALERT, 126983u, src, 0xFFu),
                        seq, p, o, out, max_frames);
}

/* Variable-length wire string: [total len = chars+2][0x01 = ASCII][chars].
 * Returns -1 if it would overflow the caller's payload buffer. */
static int put_var_str(uint8_t *b, size_t cap, size_t *o, const char *s)
{
    size_t n = s ? strlen(s) : 0;
    if (n > 253u || *o + n + 2u > cap) return -1;
    b[(*o)++] = (uint8_t)(n + 2u);
    b[(*o)++] = 0x01u;
    while (n--) b[(*o)++] = (uint8_t)*s++;
    return 0;
}

int n2k_encode_126985(const n2k_alert_t *a, const char *description,
                      const char *location, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames)
{
    uint8_t p[N2K_FP_MAX_PAYLOAD];
    size_t o = 0;
    if (!a || !out) return -1;
    put_alert_ident(p, &o, a);
    put_u8(p, &o, 0u);                  /* language: 0 = English (US) */
    if (put_var_str(p, sizeof p, &o, description) != 0) return -1;
    if (put_var_str(p, sizeof p, &o, location) != 0) return -1;
    return n2k_fp_split(n2k_can_id(PRI_ALERT, 126985u, src, 0xFFu),
                        seq, p, o, out, max_frames);
}

/* ---- PGN 126996 Product Information (fast packet, 134 B) ---------------- */

/* Fixed-width wire string: truncate, pad with spaces (printable padding —
 * displays render the field verbatim). */
static void put_fixed_str(uint8_t *b, size_t *o, const char *s, size_t n)
{
    size_t i = 0;
    if (s) for (; i < n && s[i]; i++) b[(*o)++] = (uint8_t)s[i];
    for (; i < n; i++) b[(*o)++] = (uint8_t)' ';
}

int n2k_encode_126996(const n2k_product_info_t *p, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames)
{
    uint8_t b[134];
    size_t o = 0;
    if (!p || !out) return -1;
    put_u16(b, &o, p->n2k_version);
    put_u16(b, &o, p->product_code);
    put_fixed_str(b, &o, p->model_id,      32);
    put_fixed_str(b, &o, p->sw_version,    32);
    put_fixed_str(b, &o, p->model_version, 32);
    put_fixed_str(b, &o, p->serial,        32);
    put_u8(b, &o, p->cert_level);
    put_u8(b, &o, p->load_equiv);
    return n2k_fp_split(n2k_can_id(PRI_STATUS, 126996u, src, 0xFFu),
                        seq, b, o, out, max_frames);
}

/* ---- PGN 126998 Configuration Information (fast packet, variable) ------- */
int n2k_encode_126998(const char *install_desc1, const char *install_desc2,
                      const char *mfg_info, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames)
{
    uint8_t p[N2K_FP_MAX_PAYLOAD];
    size_t o = 0;
    if (!out) return -1;
    if (put_var_str(p, sizeof p, &o, install_desc1) != 0) return -1;
    if (put_var_str(p, sizeof p, &o, install_desc2) != 0) return -1;
    if (put_var_str(p, sizeof p, &o, mfg_info) != 0) return -1;
    return n2k_fp_split(n2k_can_id(PRI_STATUS, 126998u, src, 0xFFu),
                        seq, p, o, out, max_frames);
}

/* ---- proprietary fast-packet full telemetry (layout: n2k_encode.h) ------ */
int n2k_encode_prop_telemetry(const ctrl_telemetry_t *t, uint8_t sid,
                              uint8_t src, uint8_t seq,
                              n2k_frame_t *out, size_t max_frames)
{
    uint8_t p[22];
    size_t o = 0;
    if (!t || !out) return -1;
    /* Proprietary header: mfg code b0-10, reserved b11-12 = 1s, industry
     * group b13-15 (J1939 proprietary-PGN convention). */
    put_u16(p, &o, (uint16_t)(N2K_PROP_MFG_CODE
                              | (0x3u << 11)
                              | (N2K_PROP_INDUSTRY << 13)));
    put_u8 (p, &o, sid);
    put_u8 (p, &o, (uint8_t)t->state);
    put_u8 (p, &o, (uint8_t)t->standby_reason);
    put_u8 (p, &o, t->active_profile_id);
    put_u8 (p, &o, enc_effort(t->field_effort));
    put_u8 (p, &o, (uint8_t)t->binding);
    /* binding_w is +inf when no ceiling binds — enc_u16 maps that to NA. */
    put_u16(p, &o, enc_u16(t->binding_w, 1.0));
    put_u16(p, &o, enc_temp_c(t->alt_temp_c));
    put_u16(p, &o, enc_temp_c(t->batt_temp_c));
    put_u16(p, &o, enc_u16((t->rpm_state == CTRL_RPM_VALID) ? t->rpm : NAN,
                           0.25));
    put_u8 (p, &o, (uint8_t)t->rpm_state);
    put_u32(p, &o, t->faults);
    put_u8 (p, &o, (uint8_t)t->severity);
    return n2k_fp_split(n2k_can_id(PRI_STATUS, N2K_PGN_PROP_TELEM, src, 0xFFu),
                        seq, p, o, out, max_frames);
}
