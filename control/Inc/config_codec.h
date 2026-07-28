/*
 * config_codec.h — packed config record codec (Decision #6a option B). PURE.
 *
 * Native config only: the 24C16 holds a PACKED BINARY record; JSON exists only
 * at the client boundary (docs/DECISION_6A_CONFIG_STRATEGY.md). This module
 * turns the in-RAM config (PROFILE_SPEC §7 schema, expressed with control.h's
 * types) into a byte buffer and back, FIELD BY FIELD in little-endian order —
 * never a memcpy of a struct, so the on-disk layout is independent of compiler
 * padding, struct order and target ABI.
 *
 * Record framing (all little-endian; offsets are exact and frozen per schema
 * version — see the schema history note below):
 *
 *   off  size  field
 *   0    4     magic 'W','5','C','F'
 *   4    2     schema_version  (= CFG_SCHEMA_VERSION)
 *   6    2     flags           (reserved, must be 0)
 *   8    4     generation      (monotone; modular comparison, see config_store.h)
 *   12   2     payload_len     (= CFG_PAYLOAD_V1_BYTES for the current schema)
 *   14   2     reserved        (must be 0)
 *   16   N     payload         (blocks below)
 *   16+N 4     crc32           over bytes [0, 16+N) — no gaps
 *
 * SCHEMA HISTORY: no read-forward migration path yet (VERSIONING §3: future
 * work), so a schema_version bump means old/new records are mutually
 * unreadable — fine pre-M1, no custom firmware has ever written a field
 * EEPROM (PROJECT_PLAN §5). schema 1 -> 2 (GH#40, 2026-07-28): appended
 * `batt_temp_src` + `require_batt_temp` (1 byte each) at the payload TAIL
 * after `active_profile` (VERSIONING §3 tail-append style, atop the bump).
 * The `_V1_` names below predate the bump and stay as-is — plain symbol
 * names, not a promise of two live layouts at once.
 *
 * CRC32: standard reflected polynomial 0xEDB88320, init/final XOR
 * 0xFFFFFFFF (zlib/PNG "CRC-32" — check value 0xCBF43926 for "123456789").
 * Bitwise, no table: ~310 bytes at boot is nothing.
 *
 * Floats are IEEE-754 single-precision bit patterns, little-end first. NAN
 * round-trips (a live "disabled" value for warmup_coolant_c / rotor_v_max).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_CODEC_H
#define WS500_CONFIG_CODEC_H

#include "control.h"
#include <stddef.h>

/* ---- schema identity ------------------------------------------------------ */
#define CFG_MAGIC_0            'W'
#define CFG_MAGIC_1            '5'
#define CFG_MAGIC_2            'C'
#define CFG_MAGIC_3            'F'
#define CFG_SCHEMA_VERSION     2u   /* GH#40: batt_temp_src + require_batt_temp appended */

/* ---- header offsets (frozen) ---------------------------------------------- */
#define CFG_OFF_MAGIC          0u
#define CFG_OFF_VERSION        4u
#define CFG_OFF_FLAGS          6u
#define CFG_OFF_GENERATION     8u
#define CFG_OFF_PAYLOAD_LEN    12u
#define CFG_OFF_RESERVED       14u
#define CFG_HEADER_BYTES       16u
#define CFG_CRC_BYTES          4u

/* ---- payload block offsets (relative to CFG_HEADER_BYTES, frozen for v1) --- */
#define CFG_POFF_PRIMITIVES    0u      /* 5 x f32                    = 20 */
#define CFG_POFF_GLOBALS       20u     /* ctrl_globals_t, packed     = 54 */
#define CFG_POFF_LIMITS        74u     /* ctrl_limits_t, 3 x f32     = 12 */
#define CFG_POFF_PROFILES      86u     /* 7 x CFG_PROFILE_BYTES      = 203 */
#define CFG_POFF_ACTIVE        289u    /* active_profile u8          =  1 */
#define CFG_POFF_BATT_TEMP     290u    /* GH#40 tail: batt_temp_src u8, require_batt_temp u8 = 2 */
#define CFG_PAYLOAD_V1_BYTES   292u

#define CFG_PRIMITIVES_BYTES   20u
#define CFG_GLOBALS_BYTES      54u
#define CFG_LIMITS_BYTES       12u
#define CFG_PROFILE_BYTES      29u
#define CFG_PROFILE_COUNT      7u      /* PROFILE_SPEC §4.1: ids 1..7 */
#define CFG_BATT_TEMP_BYTES    2u      /* GH#40: batt_temp_src u8 + require_batt_temp u8 */

#define CFG_RECORD_V1_BYTES \
    (CFG_HEADER_BYTES + CFG_PAYLOAD_V1_BYTES + CFG_CRC_BYTES)   /* 312 */

/* ---- voltage primitives (PROFILE_SPEC §1) ---------------------------------
 * "Stored once, referenced by name from profiles" — so they are a first-class
 * block, not five copies inside the profiles. control.h has no home for them
 * (the engine only ever sees ONE resolved profile), so the config layer owns
 * them and resolves refs into ctrl_profile_t before handing it to the engine. */
typedef struct {
    float v_bulk;
    float v_bulk_cons;
    float v_float;
    float v_float_cons;
    float v_limp;
} cfg_primitives_t;

/* Which primitive a profile voltage references (PROFILE_SPEC §3.3 "primitive
 * ref"). NONE = the stored V/cell is a literal, not a reference — the case for
 * the reserved curve-based profile 7 (§4.1) and for any future literal. */
typedef enum {
    CFG_PRIM_NONE        = 0,
    CFG_PRIM_V_BULK      = 1,
    CFG_PRIM_V_BULK_CONS = 2,
    CFG_PRIM_V_FLOAT     = 3,
    CFG_PRIM_V_FLOAT_CONS= 4,
    CFG_PRIM_V_LIMP      = 5
} cfg_prim_ref_t;

/* Per-profile config-layer extras that ctrl_profile_t has no field for. */
#define CFG_PROF_FLAG_RESERVED  (1u << 0)   /* §7 "reserved": true — slot unused */

typedef struct {
    ctrl_profile_t p;                 /* the resolved profile the engine consumes */
    uint8_t        flags;             /* CFG_PROF_FLAG_* */
    cfg_prim_ref_t cv_target_ref;     /* §3.3 cv_target is a primitive ref */
    cfg_prim_ref_t rest_voltage_ref;  /* §3.3 rest_voltage is a primitive ref */
} cfg_profile_t;

/* The whole persisted config. */
typedef struct {
    cfg_primitives_t prim;
    ctrl_globals_t   globals;
    ctrl_limits_t    limits;
    cfg_profile_t    profiles[CFG_PROFILE_COUNT];
    uint8_t          active_profile;  /* 1..CFG_PROFILE_COUNT */
} ctrl_config_t;

/* ---- decode result --------------------------------------------------------- */
typedef enum {
    CFG_DEC_OK = 0,
    CFG_DEC_ERR_SHORT,     /* buffer smaller than the framed record */
    CFG_DEC_ERR_MAGIC,
    CFG_DEC_ERR_VERSION,   /* schema_version != CFG_SCHEMA_VERSION */
    CFG_DEC_ERR_LEN,       /* payload_len not the one this version defines */
    CFG_DEC_ERR_CRC,
    CFG_DEC_ERR_ARG
} cfg_dec_t;

/* ---- API ------------------------------------------------------------------- */

/* Standard reflected CRC-32 (0xEDB88320). Exposed for the store's verify path
 * and for tests. */
uint32_t cfg_crc32(const uint8_t *data, size_t len);

/* Serialize cfg into buf. Returns bytes written (CFG_RECORD_V1_BYTES) or 0 if
 * cap is too small / a pointer is NULL. Fully deterministic: the same config
 * and generation always produce the same bytes. */
size_t cfg_encode(const ctrl_config_t *cfg, uint32_t generation,
                  uint8_t *buf, size_t cap);

/* Parse buf. Frame is checked FIRST (magic, version, length, CRC) and no field
 * is written to *out unless every frame check passes — a rejected record never
 * half-populates the caller's config. `generation` may be NULL. */
cfg_dec_t cfg_decode(const uint8_t *buf, size_t len,
                     ctrl_config_t *out, uint32_t *generation);

/* Frame-only check (no field decode): the cheap half of cfg_decode, used by the
 * store to compare generations before committing to a slot. */
cfg_dec_t cfg_peek(const uint8_t *buf, size_t len, uint32_t *generation);

/* Re-resolve every profile voltage that carries a primitive ref from the
 * primitives block, and mirror v_limp into globals.limp_vcell. This is
 * PROFILE_SPEC §1's "editing a primitive propagates to every profile that uses
 * it" — call it after any primitive edit, before validating. */
void cfg_resolve_primitives(ctrl_config_t *cfg);

/* The V/cell a ref names; NAN for CFG_PRIM_NONE or an out-of-range ref. */
float cfg_primitive_value(const cfg_primitives_t *prim, cfg_prim_ref_t ref);

#endif /* WS500_CONFIG_CODEC_H */
