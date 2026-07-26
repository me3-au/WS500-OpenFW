/*
 * config_validate.h — config guard rails (PROFILE_SPEC §1 + §3). PURE.
 *
 * PROFILE_SPEC §1: "A config that violates an ordering is rejected with a
 * specific error naming both primitives — never auto-corrected." That is the
 * whole contract of this module, extended to every §3 range: one error code per
 * rule, and NOTHING is ever clamped, rounded or repaired on the way through.
 * The engine's half of the same contract (it uses configured values verbatim,
 * it does not quietly pull them back into range) is proven by test_property.c
 * P6; this is the layer that owes the user the rejection.
 *
 * Some parameters exist in control.h but have no numeric range anywhere in
 * PROFILE_SPEC §3 (the §8.4 gap list: v_revert, t_revert_hold_s, soc_revert_pct,
 * ah_revert, rest_power_cap_w, cv_hold_exit_min, skip_bulk_vcell). Those get
 * PHYSICAL SANITY only — finite, non-negative where a negative is meaningless —
 * and every one is marked `[SPEC-GAP] no §3 range` at its check in the .c so the
 * list stays greppable when the spec catches up.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_VALIDATE_H
#define WS500_CONFIG_VALIDATE_H

#include "config_codec.h"

/* One code per rule. Order of evaluation is the order below: the §1 primitives
 * are settled before anything that reads them, and cells_series/bank_capacity_ah
 * are settled before the C-relative p_tail_w window that derives from them. */
typedef enum {
    CFG_OK = 0,

    CFG_ERR_ARG,                    /* NULL config */

    /* ---- PROFILE_SPEC §1 voltage primitives ---- */
    CFG_ERR_FLOOR_LIMP,             /* v_limp >= 3.20 V/cell (hard floor) */
    CFG_ERR_RANGE_V_BULK,           /* v_bulk       in 3.45 - 3.65 */
    CFG_ERR_RANGE_V_BULK_CONS,      /* v_bulk_cons  in 3.40 - 3.58 */
    CFG_ERR_RANGE_V_FLOAT,          /* v_float      in 3.33 - 3.45 */
    CFG_ERR_RANGE_V_FLOAT_CONS,     /* v_float_cons in 3.28 - 3.40 */
    CFG_ERR_RANGE_V_LIMP,           /* v_limp       in 3.20 - 3.35 */
    CFG_ERR_ORDER_BULK_CONS,        /* v_bulk       > v_bulk_cons  by >= 0.03 */
    CFG_ERR_ORDER_CONS_FLOAT,       /* v_bulk_cons  > v_float      by >= 0.03 */
    CFG_ERR_ORDER_FLOAT_FCONS,      /* v_float      > v_float_cons by >= 0.02 */
    CFG_ERR_ORDER_FCONS_LIMP,       /* v_float_cons > v_limp       by >= 0.02 */
    CFG_ERR_LIMP_VCELL_MISMATCH,    /* globals.limp_vcell != primitives.v_limp */

    /* ---- PROFILE_SPEC §3.1 globals ---- */
    CFG_ERR_RANGE_CELLS,            /* cells_series       4 - 16 */
    CFG_ERR_RANGE_BANK_AH,          /* bank_capacity_ah   40 - 3000 Ah */
    CFG_ERR_RANGE_MAX_POWER,        /* max_charge_power_w 100 - 20000 W */
    CFG_ERR_RANGE_RAMP,             /* ramp_w_per_s       10 - 1000 W/s */
    CFG_ERR_RANGE_T_VCLAMP,         /* t_vclamp_s         2 - 30 s */
    CFG_ERR_RANGE_T_CHARGE_MAX,     /* t_charge_max_min   60 - 1440 min */
    CFG_ERR_RANGE_WARMUP_TIME,      /* warmup_time_s      0 - 600 s */
    CFG_ERR_RANGE_WARMUP_COOLANT,   /* warmup_coolant_c   20 - 90 C, NAN = off */
    CFG_ERR_RANGE_SOC_TARGET,       /* soc_target_pct     70 - 100 %, -1 = off */

    /* ---- PROFILE_SPEC §3.2 tail exit (C-relative window) ---- */
    CFG_ERR_RANGE_P_TAIL,           /* p_tail_w  0.01C - 0.10C equivalent W */
    CFG_ERR_RANGE_T_TAIL_HOLD,      /* t_tail_hold_s 10 - 600 s */

    /* ---- CONTROL_SPEC §5.1 field / rotor, §2.1 limit set ---- */
    CFG_ERR_SANITY_ROTOR_RATED,     /* rotor_rated_v finite and > 0 (duty_max divisor) */
    CFG_ERR_SANITY_ROTOR_VMAX,      /* rotor_v_max finite and > 0, or NAN = use rated */
    CFG_ERR_SANITY_LIMP_POWER_CAP,  /* limp_power_cap_w finite and >= 0 */
    CFG_ERR_SANITY_BATTERY_C,       /* battery_c_limit finite and >= 0 */
    CFG_ERR_SANITY_WIRING_A,        /* wiring_limit_a finite and >= 0 */
    CFG_ERR_SANITY_ALT_A,           /* alternator_limit_a finite and >= 0 */

    /* ---- T1 skip-BULK-if-full (PROFILE_SPEC §2.2) ---- */
    CFG_ERR_SANITY_SKIP_BULK_VCELL, /* [SPEC-GAP] finite and >= 0 (0 = off) */
    CFG_ERR_RANGE_SKIP_BULK_SOC,    /* skip_bulk_soc_pct <= 100 (<= 0 = off) */

    /* ---- profiles (PROFILE_SPEC §3.3 / §4.1) ---- */
    CFG_ERR_PROFILE_ID,             /* profiles[i].id must be i+1 */
    CFG_ERR_PROFILE_REST_MODE,      /* rest_mode is hold | zero */
    CFG_ERR_PROFILE_CV_REF,         /* cv_target != the primitive it references */
    CFG_ERR_PROFILE_CV_RANGE,       /* literal cv_target outside the §1 span */
    CFG_ERR_PROFILE_REST_V_REF,     /* rest_voltage != the primitive it references */
    CFG_ERR_PROFILE_REST_V_RANGE,   /* literal rest_voltage outside the §1 span */
    CFG_ERR_SANITY_REST_POWER_CAP,  /* [SPEC-GAP] finite and >= 0 */
    CFG_ERR_SANITY_V_REVERT,        /* [SPEC-GAP] finite and >= 0 */
    CFG_ERR_RANGE_SOC_REVERT,       /* [SPEC-GAP] -1 = off, else 0 - 100 */
    CFG_ERR_SANITY_AH_REVERT,       /* [SPEC-GAP] finite and >= 0 */

    CFG_ERR_ACTIVE_PROFILE,         /* active_profile 1..7 and not a reserved slot */

    CFG_ERR__COUNT
} cfg_err_t;

/* §1 constants, quoted from the spec so the numbers appear exactly once. */
#define CFG_VLIMP_FLOOR_VCELL   3.20f
#define CFG_ORDER_GAP_BULK      0.03f   /* v_bulk > v_bulk_cons > v_float */
#define CFG_ORDER_GAP_FLOAT     0.02f   /* v_float > v_float_cons > v_limp */
#define CFG_VCELL_SPAN_LO       3.20f   /* lowest §1 primitive minimum (v_limp) */
#define CFG_VCELL_SPAN_HI       3.65f   /* highest §1 primitive maximum (v_bulk) */

/* Comparison slack: enough to absorb float32 representation error on values
 * entered in hundredths of a volt, far below any range's own width. */
#define CFG_VCELL_EPS           1.0e-4f

/*
 * Validate the whole config. Returns CFG_OK or the FIRST rule violated, in the
 * documented evaluation order. Never modifies *cfg.
 *   bad_profile : optional; set to the 0-based profile index for profile-scoped
 *                 errors, -1 otherwise. May be NULL.
 */
cfg_err_t ctrl_config_validate(const ctrl_config_t *cfg, int *bad_profile);

/* Stable short text for a code (logging / client error surface). */
const char *cfg_err_str(cfg_err_t e);

/*
 * The code's own ENUM NAME, e.g. "CFG_ERR_RANGE_V_BULK". This is what the JSON
 * config protocol puts in a rejection's "code" field (config_msg.h), so that
 * the wire, this header and PROFILE_SPEC all use one word for one rule and a
 * client can match on it without parsing prose. cfg_err_str() stays the human
 * sentence and travels as "detail". Unknown codes return "CFG_ERR_UNKNOWN".
 */
const char *cfg_err_name(cfg_err_t e);

#endif /* WS500_CONFIG_VALIDATE_H */
