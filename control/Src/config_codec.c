/*
 * config_codec.c — packed config record codec (Decision #6a option B). PURE.
 * Layout, framing and CRC contract: config_codec.h.
 * SPDX-License-Identifier: MIT
 */
#include "config_codec.h"

/* ---- little-endian primitives ---------------------------------------------
 * Every scalar goes through these; nothing in this file casts a struct to
 * bytes. `o` is advanced by the width written/read so the encode and decode
 * walks are literally the same sequence of calls in the same order — the one
 * property that keeps the two halves from drifting apart. */
static void put_u8(uint8_t *b, size_t *o, uint8_t v)   { b[(*o)++] = v; }
static void put_i8(uint8_t *b, size_t *o, int8_t v)    { b[(*o)++] = (uint8_t)v; }

static void put_u16(uint8_t *b, size_t *o, uint16_t v)
{
    b[(*o)++] = (uint8_t)(v & 0xFFu);
    b[(*o)++] = (uint8_t)((v >> 8) & 0xFFu);
}

static void put_u32(uint8_t *b, size_t *o, uint32_t v)
{
    b[(*o)++] = (uint8_t)(v & 0xFFu);
    b[(*o)++] = (uint8_t)((v >> 8) & 0xFFu);
    b[(*o)++] = (uint8_t)((v >> 16) & 0xFFu);
    b[(*o)++] = (uint8_t)((v >> 24) & 0xFFu);
}

/* IEEE-754 single, bit pattern verbatim so NAN (a live "disabled" value) and
 * signed zero survive the round trip untouched. */
static void put_f32(uint8_t *b, size_t *o, float v)
{
    union { float f; uint32_t u; } cv;
    cv.f = v;
    put_u32(b, o, cv.u);
}

static uint8_t  get_u8(const uint8_t *b, size_t *o)  { return b[(*o)++]; }
static int8_t   get_i8(const uint8_t *b, size_t *o)  { return (int8_t)b[(*o)++]; }

static uint16_t get_u16(const uint8_t *b, size_t *o)
{
    uint16_t v = (uint16_t)b[*o];
    v = (uint16_t)(v | ((uint16_t)b[*o + 1] << 8));
    *o += 2;
    return v;
}

static uint32_t get_u32(const uint8_t *b, size_t *o)
{
    uint32_t v = (uint32_t)b[*o]
               | ((uint32_t)b[*o + 1] << 8)
               | ((uint32_t)b[*o + 2] << 16)
               | ((uint32_t)b[*o + 3] << 24);
    *o += 4;
    return v;
}

static float get_f32(const uint8_t *b, size_t *o)
{
    union { float f; uint32_t u; } cv;
    cv.u = get_u32(b, o);
    return cv.f;
}

/* ---- CRC-32 (reflected 0xEDB88320) ---------------------------------------- */
uint32_t cfg_crc32(const uint8_t *data, size_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    if (data == NULL) return 0u;
    for (size_t i = 0; i < len; i++) {
        crc ^= (uint32_t)data[i];
        for (int bit = 0; bit < 8; bit++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

/* ---- primitives ------------------------------------------------------------ */
float cfg_primitive_value(const cfg_primitives_t *prim, cfg_prim_ref_t ref)
{
    if (prim == NULL) return NAN;
    switch (ref) {
    case CFG_PRIM_V_BULK:       return prim->v_bulk;
    case CFG_PRIM_V_BULK_CONS:  return prim->v_bulk_cons;
    case CFG_PRIM_V_FLOAT:      return prim->v_float;
    case CFG_PRIM_V_FLOAT_CONS: return prim->v_float_cons;
    case CFG_PRIM_V_LIMP:       return prim->v_limp;
    default:                    return NAN;   /* CFG_PRIM_NONE / unknown */
    }
}

void cfg_resolve_primitives(ctrl_config_t *cfg)
{
    if (cfg == NULL) return;

    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        cfg_profile_t *pr = &cfg->profiles[i];
        const float cv   = cfg_primitive_value(&cfg->prim, pr->cv_target_ref);
        const float rest = cfg_primitive_value(&cfg->prim, pr->rest_voltage_ref);
        if (!isnan(cv))   pr->p.cv_target_vcell    = cv;
        if (!isnan(rest)) pr->p.rest_voltage_vcell = rest;
    }

    /* control.h documents globals.limp_vcell AS v_limp (§1) — one value, two
     * homes, because the engine takes the Limp Home target from globals rather
     * than from the profile table. Keep them identical here so the validator's
     * mismatch rule can only fire on a record that arrived already inconsistent. */
    cfg->globals.limp_vcell = cfg->prim.v_limp;
}

/* ---- payload walk ----------------------------------------------------------
 * ENC(x) / DEC(x) below are deliberately symmetric line-for-line. Any change to
 * one side without the other breaks the round-trip test in test_config.c. */
static void encode_payload(const ctrl_config_t *cfg, uint8_t *b, size_t *o)
{
    /* --- primitives (PROFILE_SPEC §1) --- */
    put_f32(b, o, cfg->prim.v_bulk);
    put_f32(b, o, cfg->prim.v_bulk_cons);
    put_f32(b, o, cfg->prim.v_float);
    put_f32(b, o, cfg->prim.v_float_cons);
    put_f32(b, o, cfg->prim.v_limp);

    /* --- globals (PROFILE_SPEC §3.1 + CONTROL_SPEC §5.1 field block) --- */
    const ctrl_globals_t *g = &cfg->globals;
    put_u8 (b, o, g->cells_series);
    put_f32(b, o, g->bank_capacity_ah);
    put_f32(b, o, g->max_charge_power_w);
    put_f32(b, o, g->ramp_w_per_s);
    put_f32(b, o, g->p_tail_w);
    put_u16(b, o, g->t_tail_hold_s);
    put_u16(b, o, g->t_vclamp_s);
    put_u16(b, o, g->cv_hold_exit_min);
    put_u16(b, o, g->t_charge_max_min);
    put_u16(b, o, g->warmup_time_s);
    put_f32(b, o, g->warmup_coolant_c);
    put_i8 (b, o, g->soc_target_pct);
    put_f32(b, o, g->skip_bulk_vcell);
    put_i8 (b, o, g->skip_bulk_soc_pct);
    put_f32(b, o, g->rotor_rated_v);
    put_f32(b, o, g->rotor_v_max);
    put_u8 (b, o, g->allow_full_field_48v ? 1u : 0u);
    put_f32(b, o, g->limp_vcell);
    put_f32(b, o, g->limp_power_cap_w);

    /* --- hardware limit set (CONTROL_SPEC §2.1) --- */
    put_f32(b, o, cfg->limits.battery_c_limit);
    put_f32(b, o, cfg->limits.wiring_limit_a);
    put_f32(b, o, cfg->limits.alternator_limit_a);

    /* --- profiles (PROFILE_SPEC §3.3 / §4.1), all 7 slots always present --- */
    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        const cfg_profile_t *pr = &cfg->profiles[i];
        put_u8 (b, o, pr->p.id);
        put_u8 (b, o, pr->flags);
        put_u8 (b, o, (uint8_t)pr->cv_target_ref);
        put_u8 (b, o, (uint8_t)pr->rest_voltage_ref);
        put_f32(b, o, pr->p.cv_target_vcell);
        put_u8 (b, o, pr->p.exit_at_cv_entry ? 1u : 0u);
        put_u8 (b, o, (uint8_t)pr->p.rest_mode);
        put_f32(b, o, pr->p.rest_voltage_vcell);
        put_f32(b, o, pr->p.rest_power_cap_w);
        put_f32(b, o, pr->p.v_revert_vcell);
        put_u16(b, o, pr->p.t_revert_hold_s);
        put_i8 (b, o, pr->p.soc_revert_pct);
        put_f32(b, o, pr->p.ah_revert);
    }

    put_u8(b, o, cfg->active_profile);

    /* --- GH#40 tail append (schema 2): battery-temp charge-window gate --- */
    put_u8(b, o, (uint8_t)g->batt_temp_src);
    put_u8(b, o, g->require_batt_temp ? 1u : 0u);
}

static void decode_payload(const uint8_t *b, size_t *o, ctrl_config_t *cfg)
{
    cfg->prim.v_bulk       = get_f32(b, o);
    cfg->prim.v_bulk_cons  = get_f32(b, o);
    cfg->prim.v_float      = get_f32(b, o);
    cfg->prim.v_float_cons = get_f32(b, o);
    cfg->prim.v_limp       = get_f32(b, o);

    ctrl_globals_t *g = &cfg->globals;
    g->cells_series        = get_u8 (b, o);
    g->bank_capacity_ah    = get_f32(b, o);
    g->max_charge_power_w  = get_f32(b, o);
    g->ramp_w_per_s        = get_f32(b, o);
    g->p_tail_w            = get_f32(b, o);
    g->t_tail_hold_s       = get_u16(b, o);
    g->t_vclamp_s          = get_u16(b, o);
    g->cv_hold_exit_min    = get_u16(b, o);
    g->t_charge_max_min    = get_u16(b, o);
    g->warmup_time_s       = get_u16(b, o);
    g->warmup_coolant_c    = get_f32(b, o);
    g->soc_target_pct      = get_i8 (b, o);
    g->skip_bulk_vcell     = get_f32(b, o);
    g->skip_bulk_soc_pct   = get_i8 (b, o);
    g->rotor_rated_v       = get_f32(b, o);
    g->rotor_v_max         = get_f32(b, o);
    g->allow_full_field_48v = (get_u8(b, o) != 0u);
    g->limp_vcell          = get_f32(b, o);
    g->limp_power_cap_w    = get_f32(b, o);

    cfg->limits.battery_c_limit    = get_f32(b, o);
    cfg->limits.wiring_limit_a     = get_f32(b, o);
    cfg->limits.alternator_limit_a = get_f32(b, o);

    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        cfg_profile_t *pr = &cfg->profiles[i];
        pr->p.id                = get_u8 (b, o);
        pr->flags               = get_u8 (b, o);
        pr->cv_target_ref       = (cfg_prim_ref_t)get_u8(b, o);
        pr->rest_voltage_ref    = (cfg_prim_ref_t)get_u8(b, o);
        pr->p.cv_target_vcell   = get_f32(b, o);
        pr->p.exit_at_cv_entry  = (get_u8(b, o) != 0u);
        /* An out-of-enum rest_mode is decoded VERBATIM, not coerced: silently
         * turning garbage into `hold` would hide a corrupt record from the
         * validator, which is the layer that owes the user a rejection (§1). */
        pr->p.rest_mode         = (ctrl_rest_mode_t)get_u8(b, o);
        pr->p.rest_voltage_vcell= get_f32(b, o);
        pr->p.rest_power_cap_w  = get_f32(b, o);
        pr->p.v_revert_vcell    = get_f32(b, o);
        pr->p.t_revert_hold_s   = get_u16(b, o);
        pr->p.soc_revert_pct    = get_i8 (b, o);
        pr->p.ah_revert         = get_f32(b, o);
    }

    cfg->active_profile = get_u8(b, o);

    /* --- GH#40 tail append (schema 2): battery-temp charge-window gate ---
     * Decoded VERBATIM, same reasoning as rest_mode above: an out-of-enum
     * batt_temp_src must reach the validator as a named rejection, not be
     * silently coerced to NONE (which would hide a corrupt record). */
    g->batt_temp_src    = (ctrl_batt_temp_src_t)get_u8(b, o);
    g->require_batt_temp = (get_u8(b, o) != 0u);
}

/* ---- framing --------------------------------------------------------------- */
size_t cfg_encode(const ctrl_config_t *cfg, uint32_t generation,
                  uint8_t *buf, size_t cap)
{
    if (cfg == NULL || buf == NULL || cap < CFG_RECORD_V1_BYTES) return 0u;

    size_t o = 0;
    put_u8 (buf, &o, (uint8_t)CFG_MAGIC_0);
    put_u8 (buf, &o, (uint8_t)CFG_MAGIC_1);
    put_u8 (buf, &o, (uint8_t)CFG_MAGIC_2);
    put_u8 (buf, &o, (uint8_t)CFG_MAGIC_3);
    put_u16(buf, &o, (uint16_t)CFG_SCHEMA_VERSION);
    put_u16(buf, &o, 0u);                                  /* flags, reserved */
    put_u32(buf, &o, generation);
    put_u16(buf, &o, (uint16_t)CFG_PAYLOAD_V1_BYTES);
    put_u16(buf, &o, 0u);                                  /* reserved */

    encode_payload(cfg, buf, &o);

    /* CRC covers header + payload with no gaps, then trails the record. */
    put_u32(buf, &o, cfg_crc32(buf, CFG_HEADER_BYTES + CFG_PAYLOAD_V1_BYTES));
    return o;
}

cfg_dec_t cfg_peek(const uint8_t *buf, size_t len, uint32_t *generation)
{
    if (buf == NULL) return CFG_DEC_ERR_ARG;
    if (len < CFG_HEADER_BYTES) return CFG_DEC_ERR_SHORT;

    if (buf[0] != (uint8_t)CFG_MAGIC_0 || buf[1] != (uint8_t)CFG_MAGIC_1 ||
        buf[2] != (uint8_t)CFG_MAGIC_2 || buf[3] != (uint8_t)CFG_MAGIC_3)
        return CFG_DEC_ERR_MAGIC;

    size_t o = CFG_OFF_VERSION;
    const uint16_t version = get_u16(buf, &o);
    if (version != (uint16_t)CFG_SCHEMA_VERSION) return CFG_DEC_ERR_VERSION;

    o = CFG_OFF_PAYLOAD_LEN;
    const uint16_t payload_len = get_u16(buf, &o);
    if (payload_len != (uint16_t)CFG_PAYLOAD_V1_BYTES) return CFG_DEC_ERR_LEN;

    if (len < CFG_RECORD_V1_BYTES) return CFG_DEC_ERR_SHORT;

    o = CFG_HEADER_BYTES + CFG_PAYLOAD_V1_BYTES;
    const uint32_t stored = get_u32(buf, &o);
    if (stored != cfg_crc32(buf, CFG_HEADER_BYTES + CFG_PAYLOAD_V1_BYTES))
        return CFG_DEC_ERR_CRC;

    /* `flags` and `reserved` are NOT rejected when non-zero: v1 writes them 0,
     * and a future version that assigns them must stay loadable by nothing but
     * its own schema_version anyway. CRC already proves they were intended. */
    if (generation != NULL) {
        o = CFG_OFF_GENERATION;
        *generation = get_u32(buf, &o);
    }
    return CFG_DEC_OK;
}

cfg_dec_t cfg_decode(const uint8_t *buf, size_t len,
                     ctrl_config_t *out, uint32_t *generation)
{
    if (out == NULL) return CFG_DEC_ERR_ARG;

    const cfg_dec_t frame = cfg_peek(buf, len, generation);
    if (frame != CFG_DEC_OK) return frame;   /* *out untouched on any rejection */

    size_t o = CFG_HEADER_BYTES;
    decode_payload(buf, &o, out);
    return CFG_DEC_OK;
}

/* ---- layout self-checks ----------------------------------------------------
 * The offsets in the header are the contract; these pin the arithmetic so a
 * field added without moving the block offsets fails the build, not the field. */
_Static_assert(CFG_POFF_GLOBALS  == CFG_POFF_PRIMITIVES + CFG_PRIMITIVES_BYTES,
               "primitives block size mismatch");
_Static_assert(CFG_POFF_LIMITS   == CFG_POFF_GLOBALS + CFG_GLOBALS_BYTES,
               "globals block size mismatch");
_Static_assert(CFG_POFF_PROFILES == CFG_POFF_LIMITS + CFG_LIMITS_BYTES,
               "limits block size mismatch");
_Static_assert(CFG_POFF_ACTIVE   == CFG_POFF_PROFILES +
                                    CFG_PROFILE_COUNT * CFG_PROFILE_BYTES,
               "profile block size mismatch");
_Static_assert(CFG_POFF_BATT_TEMP == CFG_POFF_ACTIVE + 1u,
               "active_profile block size mismatch");
_Static_assert(CFG_PAYLOAD_V1_BYTES == CFG_POFF_BATT_TEMP + CFG_BATT_TEMP_BYTES,
               "payload length mismatch");
