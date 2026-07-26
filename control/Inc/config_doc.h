/*
 * config_doc.h — ctrl_config_t <-> PROFILE_SPEC §7 JSON document (GH#16). PURE.
 * SPDX-License-Identifier: MIT
 *
 * The SCHEMA layer of the config wire: config_json.h owns syntax, config_msg.h
 * owns messages, and this file owns the one thing that has to agree with the
 * spec — which key means which field, in which unit.
 *
 * Direction of travel (Decision #6a option B): the EEPROM record is the native
 * form (config_codec.h) and JSON is the CLIENT BOUNDARY only. So this module
 * never invents a value; it maps, and hands the result to ctrl_config_validate()
 * which is the only thing allowed to say no.
 *
 * ---------------------------------------------------------------------------
 * WHERE THE DOCUMENT DEVIATES FROM THE §7 SKETCH, AND WHY
 * ---------------------------------------------------------------------------
 * §7 is a sketch that predates the codec; five of its shapes have no schema-v1
 * home or a different unit. Each deviation is deliberate and listed here so a
 * reader comparing spec to code finds the answer without archaeology. The
 * spec-side keys are all ACCEPTED-AND-IGNORED, which means they land in the
 * unknown-key count and the client is told, never silently dropped.
 *
 *  1. `limits.belt`, `limits.warmup`, and the whole top-level `engine` block
 *     [SPEC-GAP]: no field exists for them in ctrl_config_t / the packed record
 *     (config_codec.h payload v1). Adding them is a schema_version bump, not a
 *     wire change. Not emitted; ignored + counted on parse.
 *  2. `global.topbalance_days` [SPEC-GAP]: §3.1 lists it, control.h has no
 *     field. Same treatment as 1.
 *  3. `rest.power_cap_pct` -> emitted as `rest.power_cap_w`. §3.3 allows "W or
 *     %max" and the codec stores WATTS. Transporting a percentage would make
 *     the value depend on max_charge_power_w at both ends and quantise on every
 *     round trip — the same explicitness argument §3.2 already makes for
 *     p_tail_w ("materialized ... no hidden math at runtime"). `power_cap_pct`
 *     is ignored + counted; the client does the percentage arithmetic.
 *  4. `revert.ah_frac_c` -> emitted as `revert.ah` (absolute Ah). Identical
 *     reasoning to 3: ctrl_profile_t.ah_revert is Ah.
 *  5. Profile 6's `limp` block -> emitted as an ordinary `rest` block. §4.1
 *     itself says Limp Home "is just a FLOAT config" (§6), and the engine has
 *     exactly one rest mechanism; a second spelling of it would be a second
 *     code path. `limp` is ignored + counted.
 *
 * Additions beyond §7, all because control.h grew fields the sketch predates:
 * `global.t_vclamp_s` (§3.1, absent from the sketch's global block),
 * `cv_hold_exit_min`, `skip_bulk_vcell`, `skip_bulk_soc_pct`, `limp_vcell`,
 * `limp_power_cap_w`, and `profiles[].reserved` (the codec's
 * CFG_PROF_FLAG_RESERVED, which §7 already spells `"reserved": true`).
 *
 * `profiles[].name` is EMITTED from a firmware-side §4.1 name table and
 * accepted-but-not-stored on parse (schema v1 has no string storage). It is a
 * known key, so it does NOT count as unknown.
 *
 * Conventions that apply document-wide:
 *   - A disabled/unset value travels as JSON `null`: NAN floats
 *     (warmup_coolant_c, rotor_v_max) and the -1 integer sentinels
 *     (soc_target_pct, soc_revert_pct, skip_bulk_soc_pct). Round-trips exactly.
 *   - `cv_target` / `rest.voltage` are a STRING when they reference a §1
 *     primitive ("v_bulk", ...) and a NUMBER when they are a literal V/cell
 *     (§3.3 "primitive ref"; CFG_PRIM_NONE is the literal case).
 *   - Parsing is an OVERLAY on the config passed in: absent keys keep their
 *     current value. That is what makes a one-field edit a one-key document,
 *     and it is also VERSIONING §3's "decoders MUST default missing fields".
 */
#ifndef WS500_CONFIG_DOC_H
#define WS500_CONFIG_DOC_H

#include "config_codec.h"
#include "config_json.h"

/* Outcome of mapping a document onto a config. Note what is NOT here: no
 * "value out of range". Ranges belong to ctrl_config_validate(), which names
 * the rule it broke; this layer only reports what the SCHEMA cannot express. */
typedef enum {
    CFG_DOC_OK = 0,
    CFG_DOC_ERR_JSON,     /* the reader stopped: syntax, depth, token bound */
    CFG_DOC_ERR_TYPE,     /* right key, impossible value (wrong JSON type,
                           * unknown enum spelling, integer field out of its
                           * storage range, more than CFG_PROFILE_COUNT slots) */
    CFG_DOC_ERR_SCHEMA    /* schema_version present and not ours (VERSIONING §3) */
} cfg_doc_err_t;

/* What the parse observed, for the reply the client gets back. */
typedef struct {
    int  unknown_keys;                 /* members ignored anywhere in the doc */
    int  schema_version;               /* as sent; -1 if the document omitted it */
    char bad_key[CFG_JSON_KEY_MAX];    /* key that failed, "" when none */
} cfg_doc_info_t;

/* Write the §7 document (one JSON object, braces included) for `cfg` at the
 * writer's cursor, stamped with `schema_version` and `fw` per VERSIONING §3.
 * `fw` may be NULL to omit the stamp. Returns false only if the writer
 * overflowed — with a sink bound, that cannot happen. */
bool cfg_doc_emit(cfg_json_w_t *w, const ctrl_config_t *cfg, const char *fw);

/* Overlay the document at the reader's cursor onto *cfg (see the header's
 * "Parsing is an OVERLAY" note), then re-resolve primitive references so a
 * document may list `profiles` before `primitives` without changing meaning.
 * `info` is always filled in, including on failure. *cfg may be partially
 * updated when this fails — callers MUST work on a scratch copy and only adopt
 * it after ctrl_config_validate() agrees. */
cfg_doc_err_t cfg_doc_parse(cfg_json_t *j, ctrl_config_t *cfg, cfg_doc_info_t *info);

/* The §4.1 display name for profile id 1..CFG_PROFILE_COUNT; "" out of range. */
const char *cfg_doc_profile_name(uint8_t id);

#endif /* WS500_CONFIG_DOC_H */
