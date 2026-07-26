/*
 * config_validate.c — config guard rails (PROFILE_SPEC §1 + §3). PURE.
 * Reject, never repair: see config_validate.h for the contract and the code list.
 * SPDX-License-Identifier: MIT
 */
#include "config_validate.h"

/* All predicates are written in the NEGATED-POSITIVE form (`!(v >= lo && ...)`)
 * so a NaN falls through to "reject" instead of sliding past an ordered
 * comparison — the same NaN-proofing rule field.c uses for the rotor clamp. */
static bool in_range(float v, float lo, float hi, float eps)
{
    return (v >= lo - eps) && (v <= hi + eps);
}

static bool nonneg_finite(float v)
{
    return isfinite(v) && v >= 0.0f;
}

static cfg_err_t validate_primitives(const cfg_primitives_t *p)
{
    /* Hard floor first: §1 states the floor and the v_limp range minimum at the
     * same 3.20 V/cell, and the floor is the safety-bearing one, so it is the
     * code the user sees for a too-low limp voltage. */
    if (!(p->v_limp >= CFG_VLIMP_FLOOR_VCELL - CFG_VCELL_EPS)) return CFG_ERR_FLOOR_LIMP;

    if (!in_range(p->v_bulk,       3.45f, 3.65f, CFG_VCELL_EPS)) return CFG_ERR_RANGE_V_BULK;
    if (!in_range(p->v_bulk_cons,  3.40f, 3.58f, CFG_VCELL_EPS)) return CFG_ERR_RANGE_V_BULK_CONS;
    if (!in_range(p->v_float,      3.33f, 3.45f, CFG_VCELL_EPS)) return CFG_ERR_RANGE_V_FLOAT;
    if (!in_range(p->v_float_cons, 3.28f, 3.40f, CFG_VCELL_EPS)) return CFG_ERR_RANGE_V_FLOAT_CONS;
    if (!in_range(p->v_limp,       3.20f, 3.35f, CFG_VCELL_EPS)) return CFG_ERR_RANGE_V_LIMP;

    /* Cross-primitive guard rails, §1: each step down by at least its gap. */
    if (!(p->v_bulk       - p->v_bulk_cons  >= CFG_ORDER_GAP_BULK  - CFG_VCELL_EPS))
        return CFG_ERR_ORDER_BULK_CONS;
    if (!(p->v_bulk_cons  - p->v_float      >= CFG_ORDER_GAP_BULK  - CFG_VCELL_EPS))
        return CFG_ERR_ORDER_CONS_FLOAT;
    if (!(p->v_float      - p->v_float_cons >= CFG_ORDER_GAP_FLOAT - CFG_VCELL_EPS))
        return CFG_ERR_ORDER_FLOAT_FCONS;
    if (!(p->v_float_cons - p->v_limp       >= CFG_ORDER_GAP_FLOAT - CFG_VCELL_EPS))
        return CFG_ERR_ORDER_FCONS_LIMP;

    return CFG_OK;
}

static cfg_err_t validate_globals(const ctrl_globals_t *g, const cfg_primitives_t *prim)
{
    /* control.h documents limp_vcell AS v_limp; two homes for one value must not
     * disagree, or the engine would limp at a voltage the user never set. */
    if (!(fabsf(g->limp_vcell - prim->v_limp) <= CFG_VCELL_EPS))
        return CFG_ERR_LIMP_VCELL_MISMATCH;

    /* §3.1. cells_series and bank_capacity_ah come first: the §3.2 tail window
     * below is C-relative and meaningless until both are known good. */
    if (!(g->cells_series >= 4u && g->cells_series <= 16u))       return CFG_ERR_RANGE_CELLS;
    if (!in_range(g->bank_capacity_ah,   40.0f, 3000.0f,  0.0f))  return CFG_ERR_RANGE_BANK_AH;
    if (!in_range(g->max_charge_power_w, 100.0f, 20000.0f, 0.0f)) return CFG_ERR_RANGE_MAX_POWER;
    if (!in_range(g->ramp_w_per_s,       10.0f, 1000.0f,  0.0f))  return CFG_ERR_RANGE_RAMP;
    if (!(g->t_vclamp_s       >= 2u  && g->t_vclamp_s       <= 30u))   return CFG_ERR_RANGE_T_VCLAMP;
    if (!(g->t_charge_max_min >= 60u && g->t_charge_max_min <= 1440u)) return CFG_ERR_RANGE_T_CHARGE_MAX;
    if (!(g->warmup_time_s <= 600u))                                   return CFG_ERR_RANGE_WARMUP_TIME;

    /* NAN = disabled (control.h), so it is accepted before the range test. */
    if (!isnan(g->warmup_coolant_c) && !in_range(g->warmup_coolant_c, 20.0f, 90.0f, 0.0f))
        return CFG_ERR_RANGE_WARMUP_COOLANT;
    /* -1 = disabled (control.h). */
    if (g->soc_target_pct != -1 && !(g->soc_target_pct >= 70 && g->soc_target_pct <= 100))
        return CFG_ERR_RANGE_SOC_TARGET;

    /* §3.2: p_tail_w is stored as materialized Watts but its legal window is
     * C-relative — "0.01 C - 0.10 C equiv" against the same 3.45 V/cell the
     * default formula uses. */
    {
        const float c_equiv = g->bank_capacity_ah * (float)g->cells_series * 3.45f;
        const float lo = 0.01f * c_equiv, hi = 0.10f * c_equiv;
        /* Relative slack, sized for float32 rounding of the same product
         * computed elsewhere (~1e-7 relative) and nothing more — a window this
         * wide in Watts must not be widened further by its own tolerance. */
        if (!in_range(g->p_tail_w, lo, hi, hi * 1.0e-6f)) return CFG_ERR_RANGE_P_TAIL;
    }
    if (!(g->t_tail_hold_s >= 10u && g->t_tail_hold_s <= 600u)) return CFG_ERR_RANGE_T_TAIL_HOLD;

    /* §5.1 field block. rotor_rated_v is the duty_max divisor for the installed
     * 4 ohm 12 V rotor on a 48 V bus (PROJECT_PLAN §5) — a zero, negative or
     * non-finite rating is the one config error that reaches the rotor. */
    if (!(isfinite(g->rotor_rated_v) && g->rotor_rated_v > 0.0f))
        return CFG_ERR_SANITY_ROTOR_RATED;
    /* NAN = "use rated" (control.h); any other value must be a real voltage. */
    if (!isnan(g->rotor_v_max) && !(isfinite(g->rotor_v_max) && g->rotor_v_max > 0.0f))
        return CFG_ERR_SANITY_ROTOR_VMAX;
    if (!nonneg_finite(g->limp_power_cap_w)) return CFG_ERR_SANITY_LIMP_POWER_CAP;

    /* [SPEC-GAP] no §3 range for skip_bulk_vcell — 0 = off, so sanity only. */
    if (!nonneg_finite(g->skip_bulk_vcell)) return CFG_ERR_SANITY_SKIP_BULK_VCELL;
    /* <= 0 = off (control.h), so only the upper end is constrained. */
    if (g->skip_bulk_soc_pct > 100) return CFG_ERR_RANGE_SKIP_BULK_SOC;

    /* [SPEC-GAP] no §3 range for cv_hold_exit_min — u16 minutes, 0 = off; there
     * is no representable value that is physically invalid, so nothing to
     * enforce until the spec gives it a window. */

    return CFG_OK;
}

static cfg_err_t validate_limits(const ctrl_limits_t *l)
{
    /* CONTROL_SPEC §2.1: <= 0 means "unset" (limits.c drops it from the min()),
     * so only NaN/inf/negative are rejections. */
    if (!nonneg_finite(l->battery_c_limit))    return CFG_ERR_SANITY_BATTERY_C;
    if (!nonneg_finite(l->wiring_limit_a))     return CFG_ERR_SANITY_WIRING_A;
    if (!nonneg_finite(l->alternator_limit_a)) return CFG_ERR_SANITY_ALT_A;
    return CFG_OK;
}

static cfg_err_t validate_profile(const cfg_profile_t *pr, const cfg_primitives_t *prim,
                                  unsigned index)
{
    if (pr->p.id != (uint8_t)(index + 1u)) return CFG_ERR_PROFILE_ID;

    /* §7 "reserved": true — an unused slot (profile 7, Fast Charge) carries no
     * meaningful parameters and is not held to them. It still round-trips
     * verbatim, so a client that understands it loses nothing. */
    if (pr->flags & CFG_PROF_FLAG_RESERVED) return CFG_OK;

    if (!(pr->p.rest_mode == CTRL_REST_HOLD || pr->p.rest_mode == CTRL_REST_ZERO))
        return CFG_ERR_PROFILE_REST_MODE;

    /* §3.3: cv_target is a primitive REF. When it names one, the resolved V/cell
     * must be that primitive (which §1 already range-checked); when it does not,
     * it is a literal and only the §1 span bounds it. */
    if (pr->cv_target_ref != CFG_PRIM_NONE) {
        const float ref = cfg_primitive_value(prim, pr->cv_target_ref);
        if (!(fabsf(pr->p.cv_target_vcell - ref) <= CFG_VCELL_EPS))
            return CFG_ERR_PROFILE_CV_REF;
    } else if (!in_range(pr->p.cv_target_vcell, CFG_VCELL_SPAN_LO, CFG_VCELL_SPAN_HI,
                         CFG_VCELL_EPS)) {
        return CFG_ERR_PROFILE_CV_RANGE;
    }

    /* rest_voltage / rest_power_cap_w are "—" for zero-rest profiles (§4.1) —
     * unused, so unvalidated. */
    if (pr->p.rest_mode == CTRL_REST_HOLD) {
        if (pr->rest_voltage_ref != CFG_PRIM_NONE) {
            const float ref = cfg_primitive_value(prim, pr->rest_voltage_ref);
            if (!(fabsf(pr->p.rest_voltage_vcell - ref) <= CFG_VCELL_EPS))
                return CFG_ERR_PROFILE_REST_V_REF;
        } else if (!in_range(pr->p.rest_voltage_vcell, CFG_VCELL_SPAN_LO, CFG_VCELL_SPAN_HI,
                             CFG_VCELL_EPS)) {
            return CFG_ERR_PROFILE_REST_V_RANGE;
        }
        /* [SPEC-GAP] no §3 range for rest_power_cap_w — §3.3 gives "W or %max"
         * and §4.1 gives percentages, no absolute window. Sanity only. */
        if (!nonneg_finite(pr->p.rest_power_cap_w)) return CFG_ERR_SANITY_REST_POWER_CAP;
    }

    /* [SPEC-GAP] no §3 range for v_revert (§4.1 lists per-profile defaults in a
     * 3.22-3.28 band but §3.3 states no range). Sanity only. */
    if (!nonneg_finite(pr->p.v_revert_vcell)) return CFG_ERR_SANITY_V_REVERT;
    /* [SPEC-GAP] no §3 range for t_revert_hold_s — u16 seconds, no
     * representable value is physically invalid. Nothing to enforce. */
    /* [SPEC-GAP] no §3 range for soc_revert_pct — -1 = disabled (control.h),
     * otherwise it is a percentage. */
    if (pr->p.soc_revert_pct < -1 || pr->p.soc_revert_pct > 100)
        return CFG_ERR_RANGE_SOC_REVERT;
    /* [SPEC-GAP] no §3 range for ah_revert (§4.1 default 0.30 C, no window). */
    if (!nonneg_finite(pr->p.ah_revert)) return CFG_ERR_SANITY_AH_REVERT;

    return CFG_OK;
}

cfg_err_t ctrl_config_validate(const ctrl_config_t *cfg, int *bad_profile)
{
    if (bad_profile != NULL) *bad_profile = -1;
    if (cfg == NULL) return CFG_ERR_ARG;

    cfg_err_t e = validate_primitives(&cfg->prim);
    if (e != CFG_OK) return e;

    e = validate_globals(&cfg->globals, &cfg->prim);
    if (e != CFG_OK) return e;

    e = validate_limits(&cfg->limits);
    if (e != CFG_OK) return e;

    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        e = validate_profile(&cfg->profiles[i], &cfg->prim, i);
        if (e != CFG_OK) {
            if (bad_profile != NULL) *bad_profile = (int)i;
            return e;
        }
    }

    if (cfg->active_profile < 1u || cfg->active_profile > CFG_PROFILE_COUNT)
        return CFG_ERR_ACTIVE_PROFILE;
    if (cfg->profiles[cfg->active_profile - 1u].flags & CFG_PROF_FLAG_RESERVED) {
        if (bad_profile != NULL) *bad_profile = (int)cfg->active_profile - 1;
        return CFG_ERR_ACTIVE_PROFILE;
    }

    return CFG_OK;
}

const char *cfg_err_str(cfg_err_t e)
{
    switch (e) {
    case CFG_OK:                        return "ok";
    case CFG_ERR_ARG:                   return "null config";
    case CFG_ERR_FLOOR_LIMP:            return "v_limp below the 3.20 V/cell hard floor";
    case CFG_ERR_RANGE_V_BULK:          return "v_bulk out of range 3.45-3.65";
    case CFG_ERR_RANGE_V_BULK_CONS:     return "v_bulk_cons out of range 3.40-3.58";
    case CFG_ERR_RANGE_V_FLOAT:         return "v_float out of range 3.33-3.45";
    case CFG_ERR_RANGE_V_FLOAT_CONS:    return "v_float_cons out of range 3.28-3.40";
    case CFG_ERR_RANGE_V_LIMP:          return "v_limp out of range 3.20-3.35";
    case CFG_ERR_ORDER_BULK_CONS:       return "v_bulk must exceed v_bulk_cons by 0.03 V/cell";
    case CFG_ERR_ORDER_CONS_FLOAT:      return "v_bulk_cons must exceed v_float by 0.03 V/cell";
    case CFG_ERR_ORDER_FLOAT_FCONS:     return "v_float must exceed v_float_cons by 0.02 V/cell";
    case CFG_ERR_ORDER_FCONS_LIMP:      return "v_float_cons must exceed v_limp by 0.02 V/cell";
    case CFG_ERR_LIMP_VCELL_MISMATCH:   return "limp_vcell disagrees with primitive v_limp";
    case CFG_ERR_RANGE_CELLS:           return "cells_series out of range 4-16";
    case CFG_ERR_RANGE_BANK_AH:         return "bank_capacity_ah out of range 40-3000";
    case CFG_ERR_RANGE_MAX_POWER:       return "max_charge_power_w out of range 100-20000";
    case CFG_ERR_RANGE_RAMP:            return "ramp_w_per_s out of range 10-1000";
    case CFG_ERR_RANGE_T_VCLAMP:        return "t_vclamp_s out of range 2-30";
    case CFG_ERR_RANGE_T_CHARGE_MAX:    return "t_charge_max_min out of range 60-1440";
    case CFG_ERR_RANGE_WARMUP_TIME:     return "warmup_time_s out of range 0-600";
    case CFG_ERR_RANGE_WARMUP_COOLANT:  return "warmup_coolant_c out of range 20-90";
    case CFG_ERR_RANGE_SOC_TARGET:      return "soc_target_pct out of range 70-100";
    case CFG_ERR_RANGE_P_TAIL:          return "p_tail_w outside the 0.01C-0.10C window";
    case CFG_ERR_RANGE_T_TAIL_HOLD:     return "t_tail_hold_s out of range 10-600";
    case CFG_ERR_SANITY_ROTOR_RATED:    return "rotor_rated_v must be finite and positive";
    case CFG_ERR_SANITY_ROTOR_VMAX:     return "rotor_v_max must be finite and positive, or NaN";
    case CFG_ERR_SANITY_LIMP_POWER_CAP: return "limp_power_cap_w must be finite and non-negative";
    case CFG_ERR_SANITY_BATTERY_C:      return "battery_c_limit must be finite and non-negative";
    case CFG_ERR_SANITY_WIRING_A:       return "wiring_limit_a must be finite and non-negative";
    case CFG_ERR_SANITY_ALT_A:          return "alternator_limit_a must be finite and non-negative";
    case CFG_ERR_SANITY_SKIP_BULK_VCELL:return "skip_bulk_vcell must be finite and non-negative";
    case CFG_ERR_RANGE_SKIP_BULK_SOC:   return "skip_bulk_soc_pct above 100";
    case CFG_ERR_PROFILE_ID:            return "profile id does not match its slot";
    case CFG_ERR_PROFILE_REST_MODE:     return "rest_mode is neither hold nor zero";
    case CFG_ERR_PROFILE_CV_REF:        return "cv_target disagrees with the primitive it references";
    case CFG_ERR_PROFILE_CV_RANGE:      return "literal cv_target outside the 3.20-3.65 span";
    case CFG_ERR_PROFILE_REST_V_REF:    return "rest_voltage disagrees with the primitive it references";
    case CFG_ERR_PROFILE_REST_V_RANGE:  return "literal rest_voltage outside the 3.20-3.65 span";
    case CFG_ERR_SANITY_REST_POWER_CAP: return "rest_power_cap_w must be finite and non-negative";
    case CFG_ERR_SANITY_V_REVERT:       return "v_revert must be finite and non-negative";
    case CFG_ERR_RANGE_SOC_REVERT:      return "soc_revert_pct out of range -1..100";
    case CFG_ERR_SANITY_AH_REVERT:      return "ah_revert must be finite and non-negative";
    case CFG_ERR_ACTIVE_PROFILE:        return "active_profile is out of range or a reserved slot";
    default:                            return "unknown";
    }
}

/*
 * Enum name of the same code, for the JSON wire (config_msg.h). Kept in this
 * file, immediately below cfg_err_str(), on purpose: two tables over one enum
 * drift the moment they live apart, and test_protocol.c walks 0..CFG_ERR__COUNT
 * asserting both are populated and distinct.
 */
#define CFG_ERR_NAME_CASE(e)  case e: return #e

const char *cfg_err_name(cfg_err_t e)
{
    switch (e) {
    CFG_ERR_NAME_CASE(CFG_OK);
    CFG_ERR_NAME_CASE(CFG_ERR_ARG);
    CFG_ERR_NAME_CASE(CFG_ERR_FLOOR_LIMP);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_V_BULK);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_V_BULK_CONS);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_V_FLOAT);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_V_FLOAT_CONS);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_V_LIMP);
    CFG_ERR_NAME_CASE(CFG_ERR_ORDER_BULK_CONS);
    CFG_ERR_NAME_CASE(CFG_ERR_ORDER_CONS_FLOAT);
    CFG_ERR_NAME_CASE(CFG_ERR_ORDER_FLOAT_FCONS);
    CFG_ERR_NAME_CASE(CFG_ERR_ORDER_FCONS_LIMP);
    CFG_ERR_NAME_CASE(CFG_ERR_LIMP_VCELL_MISMATCH);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_CELLS);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_BANK_AH);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_MAX_POWER);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_RAMP);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_T_VCLAMP);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_T_CHARGE_MAX);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_WARMUP_TIME);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_WARMUP_COOLANT);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_SOC_TARGET);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_P_TAIL);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_T_TAIL_HOLD);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_ROTOR_RATED);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_ROTOR_VMAX);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_LIMP_POWER_CAP);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_BATTERY_C);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_WIRING_A);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_ALT_A);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_SKIP_BULK_VCELL);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_SKIP_BULK_SOC);
    CFG_ERR_NAME_CASE(CFG_ERR_PROFILE_ID);
    CFG_ERR_NAME_CASE(CFG_ERR_PROFILE_REST_MODE);
    CFG_ERR_NAME_CASE(CFG_ERR_PROFILE_CV_REF);
    CFG_ERR_NAME_CASE(CFG_ERR_PROFILE_CV_RANGE);
    CFG_ERR_NAME_CASE(CFG_ERR_PROFILE_REST_V_REF);
    CFG_ERR_NAME_CASE(CFG_ERR_PROFILE_REST_V_RANGE);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_REST_POWER_CAP);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_V_REVERT);
    CFG_ERR_NAME_CASE(CFG_ERR_RANGE_SOC_REVERT);
    CFG_ERR_NAME_CASE(CFG_ERR_SANITY_AH_REVERT);
    CFG_ERR_NAME_CASE(CFG_ERR_ACTIVE_PROFILE);
    default:                            return "CFG_ERR_UNKNOWN";
    }
}
