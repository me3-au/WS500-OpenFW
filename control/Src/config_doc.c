/*
 * config_doc.c — ctrl_config_t <-> PROFILE_SPEC §7 JSON document (GH#16). PURE.
 * Key-by-key contract, the five documented deviations from the §7 sketch, and
 * the null/ref conventions: config_doc.h. Read that first — this file is the
 * mechanical half.
 *
 * House rule enforced throughout: this layer NEVER repairs a value. A number
 * that is the wrong type or cannot fit its storage is CFG_DOC_ERR_TYPE naming
 * the key; a number that is merely out of range is passed on untouched so that
 * ctrl_config_validate() can name the §1/§3 rule it breaks (PROFILE_SPEC §1:
 * "rejected with a specific error ... never auto-corrected").
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_doc.h"

#include <math.h>
#include <string.h>

/* §4.1 profile names. Firmware-side and read-only: schema v1 stores no strings,
 * so a client that renames a profile is renaming it in the client. */
static const char *const PROFILE_NAMES[CFG_PROFILE_COUNT] = {
    "Bulk, Float Norm",
    "Bulk, Float Low (Solar Priority)",
    "Bulk, Solar Finish",
    "Bulk Conservative, Float Low",
    "Bulk, Off",
    "Limp Home",
    "Fast Charge"
};

/* Indexed by cfg_prim_ref_t; slot 0 (CFG_PRIM_NONE) has no name by design — it
 * is the "this is a literal, not a reference" case (§3.3). */
static const char *const PRIM_NAMES[] = {
    NULL, "v_bulk", "v_bulk_cons", "v_float", "v_float_cons", "v_limp"
};

/* Indexed by ctrl_batt_temp_src_t (GH#40). Bench-pending channel binding —
 * control.h's ctrl_batt_temp_src_t doc comment has the full "why default
 * none" reasoning. */
static const char *const BATT_TEMP_SRC_NAMES[] = { "none", "adc_a", "adc_b" };

const char *cfg_doc_profile_name(uint8_t id)
{
    if (id < 1u || id > CFG_PROFILE_COUNT) return "";
    return PROFILE_NAMES[id - 1u];
}

/* ---- emit ------------------------------------------------------------------ */

/* -1 is the "disabled" spelling for every int8 knob in this config; on the wire
 * that is JSON null, which is what PROFILE_SPEC §7 already shows for the
 * optional ones ("soc_target_pct": null). Any other value goes as a number, so
 * the round trip is exact. */
static void emit_i8(cfg_json_w_t *w, int8_t v)
{
    if (v == -1) cfg_json_w_null(w);
    else         cfg_json_w_int(w, (long)v);
}

/* §3.3 primitive ref: a name when it references one of the §1 primitives, the
 * literal V/cell otherwise. */
static void emit_vref(cfg_json_w_t *w, cfg_prim_ref_t ref, float lit)
{
    if (ref >= CFG_PRIM_V_BULK && ref <= CFG_PRIM_V_LIMP)
        cfg_json_w_str(w, PRIM_NAMES[ref]);
    else
        cfg_json_w_f32(w, lit);
}

bool cfg_doc_emit(cfg_json_w_t *w, const ctrl_config_t *cfg, const char *fw)
{
    if (w == NULL || cfg == NULL) return false;

    cfg_json_w_obj(w);

    /* VERSIONING §3: "Every exported JSON is stamped". */
    cfg_json_w_key(w, "schema_version"); cfg_json_w_int(w, (long)CFG_SCHEMA_VERSION);
    if (fw != NULL) { cfg_json_w_key(w, "fw"); cfg_json_w_str(w, fw); }

    cfg_json_w_key(w, "primitives");
    cfg_json_w_obj(w);
    cfg_json_w_key(w, "v_bulk");       cfg_json_w_f32(w, cfg->prim.v_bulk);
    cfg_json_w_key(w, "v_bulk_cons");  cfg_json_w_f32(w, cfg->prim.v_bulk_cons);
    cfg_json_w_key(w, "v_float");      cfg_json_w_f32(w, cfg->prim.v_float);
    cfg_json_w_key(w, "v_float_cons"); cfg_json_w_f32(w, cfg->prim.v_float_cons);
    cfg_json_w_key(w, "v_limp");       cfg_json_w_f32(w, cfg->prim.v_limp);
    cfg_json_w_end(w);

    cfg_json_w_key(w, "limits");
    cfg_json_w_obj(w);
    cfg_json_w_key(w, "battery_c_limit");    cfg_json_w_f32(w, cfg->limits.battery_c_limit);
    cfg_json_w_key(w, "wiring_limit_a");     cfg_json_w_f32(w, cfg->limits.wiring_limit_a);
    cfg_json_w_key(w, "alternator_limit_a"); cfg_json_w_f32(w, cfg->limits.alternator_limit_a);
    /* §7 nests the field block under limits even though control.h keeps these
     * three in globals — the wire follows the spec, the struct keeps its shape. */
    cfg_json_w_key(w, "field");
    cfg_json_w_obj(w);
    cfg_json_w_key(w, "rotor_rated_v");        cfg_json_w_f32(w, cfg->globals.rotor_rated_v);
    cfg_json_w_key(w, "rotor_v_max");          cfg_json_w_f32(w, cfg->globals.rotor_v_max);
    cfg_json_w_key(w, "allow_full_field_48v"); cfg_json_w_bool(w, cfg->globals.allow_full_field_48v);
    cfg_json_w_end(w);
    cfg_json_w_end(w);

    const ctrl_globals_t *g = &cfg->globals;
    cfg_json_w_key(w, "global");
    cfg_json_w_obj(w);
    cfg_json_w_key(w, "cells_series");       cfg_json_w_int(w, (long)g->cells_series);
    cfg_json_w_key(w, "bank_capacity_ah");   cfg_json_w_f32(w, g->bank_capacity_ah);
    cfg_json_w_key(w, "max_charge_power_w"); cfg_json_w_f32(w, g->max_charge_power_w);
    cfg_json_w_key(w, "ramp_w_per_s");       cfg_json_w_f32(w, g->ramp_w_per_s);
    cfg_json_w_key(w, "p_tail_w");           cfg_json_w_f32(w, g->p_tail_w);
    cfg_json_w_key(w, "t_tail_hold_s");      cfg_json_w_int(w, (long)g->t_tail_hold_s);
    cfg_json_w_key(w, "t_vclamp_s");         cfg_json_w_int(w, (long)g->t_vclamp_s);
    cfg_json_w_key(w, "cv_hold_exit_min");   cfg_json_w_int(w, (long)g->cv_hold_exit_min);
    cfg_json_w_key(w, "t_charge_max_min");   cfg_json_w_int(w, (long)g->t_charge_max_min);
    cfg_json_w_key(w, "warmup_time_s");      cfg_json_w_int(w, (long)g->warmup_time_s);
    cfg_json_w_key(w, "warmup_coolant_c");   cfg_json_w_f32(w, g->warmup_coolant_c);
    cfg_json_w_key(w, "soc_target_pct");     emit_i8(w, g->soc_target_pct);
    cfg_json_w_key(w, "skip_bulk_vcell");    cfg_json_w_f32(w, g->skip_bulk_vcell);
    cfg_json_w_key(w, "skip_bulk_soc_pct");  emit_i8(w, g->skip_bulk_soc_pct);
    cfg_json_w_key(w, "limp_vcell");         cfg_json_w_f32(w, g->limp_vcell);
    cfg_json_w_key(w, "limp_power_cap_w");   cfg_json_w_f32(w, g->limp_power_cap_w);
    /* GH#40 — schema 2. batt_temp_src is a closed enum, spelled the same way
     * rest.mode is (deviation-free wire name for a control.h enum). */
    cfg_json_w_key(w, "batt_temp_src");
    cfg_json_w_str(w, (g->batt_temp_src <= CTRL_BATT_TEMP_ADC_B)
                      ? BATT_TEMP_SRC_NAMES[g->batt_temp_src] : "none");
    cfg_json_w_key(w, "require_batt_temp");  cfg_json_w_bool(w, g->require_batt_temp);
    cfg_json_w_end(w);

    cfg_json_w_key(w, "profiles");
    cfg_json_w_arr(w);
    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        const cfg_profile_t  *pr = &cfg->profiles[i];
        const ctrl_profile_t *p  = &pr->p;

        cfg_json_w_obj(w);
        cfg_json_w_key(w, "id");   cfg_json_w_int(w, (long)p->id);
        cfg_json_w_key(w, "name"); cfg_json_w_str(w, cfg_doc_profile_name(p->id));
        cfg_json_w_key(w, "reserved");
        cfg_json_w_bool(w, (pr->flags & CFG_PROF_FLAG_RESERVED) != 0u);
        cfg_json_w_key(w, "cv_target");
        emit_vref(w, pr->cv_target_ref, p->cv_target_vcell);
        cfg_json_w_key(w, "exit_at_cv_entry"); cfg_json_w_bool(w, p->exit_at_cv_entry);

        /* The rest block is emitted in FULL for every profile, including
         * rest_mode "zero" where §7 shows only {"mode":"zero"} — omitting the
         * fields would make cfg-get -> cfg-set drop whatever the user had
         * configured behind a zero-mode profile. */
        cfg_json_w_key(w, "rest");
        cfg_json_w_obj(w);
        cfg_json_w_key(w, "mode");
        cfg_json_w_str(w, (p->rest_mode == CTRL_REST_ZERO) ? "zero" : "hold");
        cfg_json_w_key(w, "voltage");
        emit_vref(w, pr->rest_voltage_ref, p->rest_voltage_vcell);
        cfg_json_w_key(w, "power_cap_w"); cfg_json_w_f32(w, p->rest_power_cap_w);
        cfg_json_w_end(w);

        cfg_json_w_key(w, "revert");
        cfg_json_w_obj(w);
        cfg_json_w_key(w, "v_cell");  cfg_json_w_f32(w, p->v_revert_vcell);
        cfg_json_w_key(w, "hold_s");  cfg_json_w_int(w, (long)p->t_revert_hold_s);
        cfg_json_w_key(w, "soc_pct"); emit_i8(w, p->soc_revert_pct);
        cfg_json_w_key(w, "ah");      cfg_json_w_f32(w, p->ah_revert);
        cfg_json_w_end(w);

        cfg_json_w_end(w);
    }
    cfg_json_w_end(w);

    cfg_json_w_key(w, "active_profile"); cfg_json_w_int(w, (long)cfg->active_profile);

    cfg_json_w_end(w);
    return !w->ovf;
}

/* ---- parse ----------------------------------------------------------------- */

typedef struct {
    cfg_json_t     *j;
    ctrl_config_t  *cfg;
    cfg_doc_info_t *info;
    cfg_doc_err_t   err;
    bool            saw_v_limp;      /* the document set primitives.v_limp */
    bool            saw_limp_vcell;  /* ... and/or global.limp_vcell */
} dc_t;

static bool dc_fail(dc_t *d, cfg_doc_err_t e, const char *key)
{
    if (d->err == CFG_DOC_OK) {
        d->err = e;
        if (key != NULL) {
            size_t n = strlen(key);
            if (n >= sizeof d->info->bad_key) n = sizeof d->info->bad_key - 1u;
            memcpy(d->info->bad_key, key, n);
            d->info->bad_key[n] = '\0';
        }
    }
    return false;
}

/* The reader stopped; translate once so every caller can just `return false`. */
static bool dc_json(dc_t *d) { return dc_fail(d, CFG_DOC_ERR_JSON, NULL); }

static bool dc_unknown(dc_t *d)
{
    d->j->unknown++;
    if (!cfg_json_skip(d->j)) return dc_json(d);
    return true;
}

/* A double that came off the wire, narrowed to float without invoking the
 * undefined behaviour of casting an out-of-range double. Infinity survives so
 * the validator's "must be finite" rules can reject it by name. */
static float to_f32(double v)
{
    if (v >  3.4028235e38) return INFINITY;
    if (v < -3.4028235e38) return -INFINITY;
    return (float)v;
}

/* number | null. Anything else is a type error naming `key`. */
static bool rd_num(dc_t *d, const char *key, double *v, bool *is_null)
{
    const cfg_json_type_t t = cfg_json_peek(d->j);
    if (t == CFG_JSON_T_NULL) {
        if (!cfg_json_null(d->j)) return dc_json(d);
        *is_null = true;
        *v = 0.0;
        return true;
    }
    if (t != CFG_JSON_T_NUM) return dc_fail(d, CFG_DOC_ERR_TYPE, key);
    if (!cfg_json_num(d->j, v)) return dc_json(d);
    *is_null = false;
    return true;
}

/* Float field. null means NAN, which for warmup_coolant_c / rotor_v_max IS the
 * configured "off" and for everything else is a value the validator rejects. */
static bool rd_f32(dc_t *d, const char *key, float *dst)
{
    double v = 0.0;
    bool   isnull = false;
    if (!rd_num(d, key, &v, &isnull)) return false;
    *dst = isnull ? NAN : to_f32(v);
    return true;
}

/* Integer field. Non-integral or out of the field's storage range is a TYPE
 * error, not a clamp: 70000 in a u16 is a client bug, and silently storing 4464
 * would be the worst possible answer. */
static bool rd_int(dc_t *d, const char *key, double lo, double hi, double *out)
{
    double v = 0.0;
    bool   isnull = false;
    if (!rd_num(d, key, &v, &isnull)) return false;
    if (isnull) return dc_fail(d, CFG_DOC_ERR_TYPE, key);
    if (!isfinite(v) || v != floor(v) || v < lo || v > hi)
        return dc_fail(d, CFG_DOC_ERR_TYPE, key);
    *out = v;
    return true;
}

static bool rd_u8(dc_t *d, const char *key, uint8_t *dst)
{
    double v;
    if (!rd_int(d, key, 0.0, 255.0, &v)) return false;
    *dst = (uint8_t)v;
    return true;
}

static bool rd_u16(dc_t *d, const char *key, uint16_t *dst)
{
    double v;
    if (!rd_int(d, key, 0.0, 65535.0, &v)) return false;
    *dst = (uint16_t)v;
    return true;
}

/* int8 with the -1 = disabled convention: null reads back as -1. */
static bool rd_i8(dc_t *d, const char *key, int8_t *dst)
{
    const cfg_json_type_t t = cfg_json_peek(d->j);
    if (t == CFG_JSON_T_NULL) {
        if (!cfg_json_null(d->j)) return dc_json(d);
        *dst = -1;
        return true;
    }
    double v;
    if (!rd_int(d, key, -128.0, 127.0, &v)) return false;
    *dst = (int8_t)v;
    return true;
}

static bool rd_bool(dc_t *d, const char *key, bool *dst)
{
    if (cfg_json_peek(d->j) != CFG_JSON_T_BOOL) return dc_fail(d, CFG_DOC_ERR_TYPE, key);
    if (!cfg_json_bool(d->j, dst)) return dc_json(d);
    return true;
}

/* §3.3 primitive ref: string = reference, number/null = literal V/cell. */
static bool rd_vref(dc_t *d, const char *key, cfg_prim_ref_t *ref, float *lit)
{
    const cfg_json_type_t t = cfg_json_peek(d->j);
    if (t == CFG_JSON_T_STR) {
        char s[CFG_JSON_KEY_MAX];
        bool tr = false;
        if (!cfg_json_str(d->j, s, (int)sizeof s, &tr)) return dc_json(d);
        if (tr) return dc_fail(d, CFG_DOC_ERR_TYPE, key);
        for (int i = (int)CFG_PRIM_V_BULK; i <= (int)CFG_PRIM_V_LIMP; i++) {
            if (strcmp(s, PRIM_NAMES[i]) == 0) { *ref = (cfg_prim_ref_t)i; return true; }
        }
        return dc_fail(d, CFG_DOC_ERR_TYPE, key);
    }
    if (t == CFG_JSON_T_NUM || t == CFG_JSON_T_NULL) {
        *ref = CFG_PRIM_NONE;
        return rd_f32(d, key, lit);
    }
    return dc_fail(d, CFG_DOC_ERR_TYPE, key);
}

/* Fetch the next member of an open object whose name FITS. Returns true with
 * the cursor on the value; false at the end of the object or on error (callers
 * check d->err, which every parse_* below does via its `return d->err ==
 * CFG_DOC_OK`). A key too long for `cap` is consumed as unknown here rather
 * than handed up: it cannot be one of ours, and matching a truncated name could
 * alias a real key. */
static bool next_key(dc_t *d, char *key, int cap)
{
    for (;;) {
        bool tr = false;
        if (!cfg_json_obj_key(d->j, key, cap, &tr)) {
            if (d->j->err != CFG_JSON_OK) dc_json(d);
            return false;
        }
        if (!tr) return true;
        if (!dc_unknown(d)) return false;
    }
}

#define DOC_FOREACH(d, key)  while (next_key((d), (key), CFG_JSON_KEY_MAX))

static bool parse_primitives(dc_t *d)
{
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok;
        if      (!strcmp(k, "v_bulk"))       ok = rd_f32(d, k, &d->cfg->prim.v_bulk);
        else if (!strcmp(k, "v_bulk_cons"))  ok = rd_f32(d, k, &d->cfg->prim.v_bulk_cons);
        else if (!strcmp(k, "v_float"))      ok = rd_f32(d, k, &d->cfg->prim.v_float);
        else if (!strcmp(k, "v_float_cons")) ok = rd_f32(d, k, &d->cfg->prim.v_float_cons);
        else if (!strcmp(k, "v_limp"))     { ok = rd_f32(d, k, &d->cfg->prim.v_limp);
                                             d->saw_v_limp = ok; }
        else                                 ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_field(dc_t *d)
{
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok;
        if      (!strcmp(k, "rotor_rated_v")) ok = rd_f32(d, k, &d->cfg->globals.rotor_rated_v);
        else if (!strcmp(k, "rotor_v_max"))   ok = rd_f32(d, k, &d->cfg->globals.rotor_v_max);
        else if (!strcmp(k, "allow_full_field_48v"))
                                              ok = rd_bool(d, k, &d->cfg->globals.allow_full_field_48v);
        else                                  ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_limits(dc_t *d)
{
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok;
        if      (!strcmp(k, "battery_c_limit"))    ok = rd_f32(d, k, &d->cfg->limits.battery_c_limit);
        else if (!strcmp(k, "wiring_limit_a"))     ok = rd_f32(d, k, &d->cfg->limits.wiring_limit_a);
        else if (!strcmp(k, "alternator_limit_a")) ok = rd_f32(d, k, &d->cfg->limits.alternator_limit_a);
        else if (!strcmp(k, "field"))              ok = parse_field(d);
        /* "belt" and "warmup": §7 blocks with no schema-v1 home (config_doc.h
         * deviation 1) — fall through to the unknown counter on purpose. */
        else                                       ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_global(dc_t *d)
{
    ctrl_globals_t *g = &d->cfg->globals;
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok;
        if      (!strcmp(k, "cells_series"))       ok = rd_u8 (d, k, &g->cells_series);
        else if (!strcmp(k, "bank_capacity_ah"))   ok = rd_f32(d, k, &g->bank_capacity_ah);
        else if (!strcmp(k, "max_charge_power_w")) ok = rd_f32(d, k, &g->max_charge_power_w);
        else if (!strcmp(k, "ramp_w_per_s"))       ok = rd_f32(d, k, &g->ramp_w_per_s);
        else if (!strcmp(k, "p_tail_w"))           ok = rd_f32(d, k, &g->p_tail_w);
        else if (!strcmp(k, "t_tail_hold_s"))      ok = rd_u16(d, k, &g->t_tail_hold_s);
        else if (!strcmp(k, "t_vclamp_s"))         ok = rd_u16(d, k, &g->t_vclamp_s);
        else if (!strcmp(k, "cv_hold_exit_min"))   ok = rd_u16(d, k, &g->cv_hold_exit_min);
        else if (!strcmp(k, "t_charge_max_min"))   ok = rd_u16(d, k, &g->t_charge_max_min);
        else if (!strcmp(k, "warmup_time_s"))      ok = rd_u16(d, k, &g->warmup_time_s);
        else if (!strcmp(k, "warmup_coolant_c"))   ok = rd_f32(d, k, &g->warmup_coolant_c);
        else if (!strcmp(k, "soc_target_pct"))     ok = rd_i8 (d, k, &g->soc_target_pct);
        else if (!strcmp(k, "skip_bulk_vcell"))    ok = rd_f32(d, k, &g->skip_bulk_vcell);
        else if (!strcmp(k, "skip_bulk_soc_pct"))  ok = rd_i8 (d, k, &g->skip_bulk_soc_pct);
        else if (!strcmp(k, "limp_power_cap_w"))   ok = rd_f32(d, k, &g->limp_power_cap_w);
        else if (!strcmp(k, "limp_vcell"))       { ok = rd_f32(d, k, &g->limp_vcell);
                                                   d->saw_limp_vcell = ok; }
        else if (!strcmp(k, "batt_temp_src")) {
            /* GH#40: string enum, same shape as profiles[].rest.mode. An
             * unrecognised spelling is stored OUT OF ENUM on purpose (mirrors
             * parse_rest()'s rest_mode handling below) so ctrl_config_validate()
             * returns CFG_ERR_RANGE_BATT_TEMP_SRC under its own name instead of
             * this layer guessing what the client meant. */
            if (cfg_json_peek(d->j) != CFG_JSON_T_STR) { ok = dc_fail(d, CFG_DOC_ERR_TYPE, k); }
            else {
                char s[CFG_JSON_KEY_MAX];
                bool tr = false;
                if (!cfg_json_str(d->j, s, (int)sizeof s, &tr)) { ok = dc_json(d); }
                else if (!strcmp(s, "none"))    { g->batt_temp_src = CTRL_BATT_TEMP_NONE;  ok = true; }
                else if (!strcmp(s, "adc_a"))   { g->batt_temp_src = CTRL_BATT_TEMP_ADC_A; ok = true; }
                else if (!strcmp(s, "adc_b"))   { g->batt_temp_src = CTRL_BATT_TEMP_ADC_B; ok = true; }
                else { g->batt_temp_src = (ctrl_batt_temp_src_t)0xFFu; ok = true; }
            }
        }
        else if (!strcmp(k, "require_batt_temp"))
                                                    ok = rd_bool(d, k, &g->require_batt_temp);
        /* "topbalance_days": §3.1/§7 parameter with no control.h field
         * (config_doc.h deviation 2) — counted, not stored. */
        else                                       ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_rest(dc_t *d, cfg_profile_t *pr)
{
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok = true;
        if (!strcmp(k, "mode")) {
            if (cfg_json_peek(d->j) != CFG_JSON_T_STR) { ok = dc_fail(d, CFG_DOC_ERR_TYPE, k); }
            else {
                char s[CFG_JSON_KEY_MAX];
                bool tr = false;
                if (!cfg_json_str(d->j, s, (int)sizeof s, &tr)) ok = dc_json(d);
                else if (!strcmp(s, "hold")) pr->p.rest_mode = CTRL_REST_HOLD;
                else if (!strcmp(s, "zero")) pr->p.rest_mode = CTRL_REST_ZERO;
                else {
                    /* An unrecognised spelling is stored OUT OF ENUM on purpose:
                     * ctrl_config_validate() then returns
                     * CFG_ERR_PROFILE_REST_MODE and the client gets the
                     * validator's own name for the rule, not this layer's guess
                     * at what "Hold " might have meant. */
                    pr->p.rest_mode = (ctrl_rest_mode_t)(CTRL_REST_ZERO + 1);
                }
            }
        }
        else if (!strcmp(k, "voltage"))
            ok = rd_vref(d, k, &pr->rest_voltage_ref, &pr->p.rest_voltage_vcell);
        else if (!strcmp(k, "power_cap_w"))
            ok = rd_f32(d, k, &pr->p.rest_power_cap_w);
        /* "power_cap_pct": §7 spells the cap as a percentage of max
         * (config_doc.h deviation 3) — counted, not converted. */
        else
            ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_revert(dc_t *d, ctrl_profile_t *p)
{
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok;
        if      (!strcmp(k, "v_cell"))  ok = rd_f32(d, k, &p->v_revert_vcell);
        else if (!strcmp(k, "hold_s"))  ok = rd_u16(d, k, &p->t_revert_hold_s);
        else if (!strcmp(k, "soc_pct")) ok = rd_i8 (d, k, &p->soc_revert_pct);
        else if (!strcmp(k, "ah"))      ok = rd_f32(d, k, &p->ah_revert);
        /* "ah_frac_c": §7 spells this as a fraction of C (deviation 4). */
        else                            ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_profile(dc_t *d, cfg_profile_t *pr)
{
    if (!cfg_json_obj_begin(d->j)) return dc_json(d);
    char k[CFG_JSON_KEY_MAX];
    DOC_FOREACH(d, k) {
        bool ok = true;
        if (!strcmp(k, "id")) {
            /* Taken verbatim so a mismatched id reaches the validator as
             * CFG_ERR_PROFILE_ID instead of being quietly overwritten. */
            ok = rd_u8(d, k, &pr->p.id);
        }
        else if (!strcmp(k, "name")) {
            /* Known but not stored (schema v1 has no strings) — accepted and
             * NOT counted as unknown, because the firmware does recognise it. */
            if (!cfg_json_skip(d->j)) ok = dc_json(d);
        }
        else if (!strcmp(k, "reserved")) {
            bool res = false;
            ok = rd_bool(d, k, &res);
            if (ok) {
                if (res) pr->flags |= CFG_PROF_FLAG_RESERVED;
                else     pr->flags = (uint8_t)(pr->flags & ~CFG_PROF_FLAG_RESERVED);
            }
        }
        else if (!strcmp(k, "cv_target"))
            ok = rd_vref(d, k, &pr->cv_target_ref, &pr->p.cv_target_vcell);
        else if (!strcmp(k, "exit_at_cv_entry"))
            ok = rd_bool(d, k, &pr->p.exit_at_cv_entry);
        else if (!strcmp(k, "rest"))   ok = parse_rest(d, pr);
        else if (!strcmp(k, "revert")) ok = parse_revert(d, &pr->p);
        /* "limp": §7 gives profile 6 its own block (deviation 5) — counted. */
        else                           ok = dc_unknown(d);
        if (!ok) return false;
    }
    return d->err == CFG_DOC_OK;
}

static bool parse_profiles(dc_t *d)
{
    if (!cfg_json_arr_begin(d->j)) return dc_json(d);
    unsigned i = 0;
    while (cfg_json_arr_next(d->j)) {
        if (i >= CFG_PROFILE_COUNT) return dc_fail(d, CFG_DOC_ERR_TYPE, "profiles");
        if (!parse_profile(d, &d->cfg->profiles[i])) return false;
        i++;
    }
    if (d->j->err != CFG_JSON_OK) return dc_json(d);
    return true;
}

/* PROFILE_SPEC §1 propagation, run AFTER the whole document so key order cannot
 * change meaning. Deliberately not cfg_resolve_primitives(): that one also
 * mirrors v_limp into globals.limp_vcell unconditionally, which would make
 * CFG_ERR_LIMP_VCELL_MISMATCH unreachable from the wire. Here the mirror only
 * fires when the document moved v_limp WITHOUT stating limp_vcell — so an
 * ordinary primitive edit just works, and a document that states both
 * inconsistently gets the validator's rejection it deserves. */
static void resolve_after_parse(dc_t *d)
{
    ctrl_config_t *c = d->cfg;
    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        cfg_profile_t *pr = &c->profiles[i];
        const float cv   = cfg_primitive_value(&c->prim, pr->cv_target_ref);
        const float rest = cfg_primitive_value(&c->prim, pr->rest_voltage_ref);
        if (!isnan(cv))   pr->p.cv_target_vcell    = cv;
        if (!isnan(rest)) pr->p.rest_voltage_vcell = rest;
    }
    if (d->saw_v_limp && !d->saw_limp_vcell) c->globals.limp_vcell = c->prim.v_limp;
}

cfg_doc_err_t cfg_doc_parse(cfg_json_t *j, ctrl_config_t *cfg, cfg_doc_info_t *info)
{
    cfg_doc_info_t local;
    if (info == NULL) info = &local;
    info->unknown_keys   = 0;
    info->schema_version = -1;
    info->bad_key[0]     = '\0';

    if (j == NULL || cfg == NULL) return CFG_DOC_ERR_JSON;

    dc_t d = { j, cfg, info, CFG_DOC_OK, false, false };

    if (!cfg_json_obj_begin(j)) { dc_json(&d); goto done; }

    {
        char k[CFG_JSON_KEY_MAX];
        DOC_FOREACH(&d, k) {
            bool ok;
            if (!strcmp(k, "schema_version")) {
                double v;
                ok = rd_int(&d, k, 0.0, 65535.0, &v);
                if (ok) {
                    info->schema_version = (int)v;
                    /* VERSIONING §3: the firmware is the schema authority and
                     * accepts writes only in its native version. An ABSENT
                     * stamp is tolerated (a hand-written one-key edit is a
                     * legitimate client); a WRONG one never is. */
                    if ((unsigned)v != CFG_SCHEMA_VERSION)
                        ok = dc_fail(&d, CFG_DOC_ERR_SCHEMA, k);
                }
            }
            else if (!strcmp(k, "primitives"))     ok = parse_primitives(&d);
            else if (!strcmp(k, "limits"))         ok = parse_limits(&d);
            else if (!strcmp(k, "global"))         ok = parse_global(&d);
            else if (!strcmp(k, "profiles"))       ok = parse_profiles(&d);
            else if (!strcmp(k, "active_profile")) ok = rd_u8(&d, k, &cfg->active_profile);
            else if (!strcmp(k, "fw")) {
                /* Informational stamp written by cfg-get; a client echoing the
                 * document back must not be told its own reply is unknown. */
                if (!cfg_json_skip(j)) ok = dc_json(&d); else ok = true;
            }
            /* "engine": §7 white-space curves, no schema-v1 home (deviation 1). */
            else                                   ok = dc_unknown(&d);
            if (!ok) goto done;
        }
    }

    if (d.err == CFG_DOC_OK) resolve_after_parse(&d);

done:
    info->unknown_keys = j->unknown;
    return d.err;
}
