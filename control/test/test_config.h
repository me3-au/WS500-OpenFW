/*
 * test_config.h — shared config fixtures for the native tests.
 *
 * One spec-faithful baseline config (PROFILE_SPEC §1 defaults, §3.1/§3.2
 * defaults, the seven §4.1 profiles) plus a helper that edits ONE §1 primitive
 * and re-derives the others into a still-legal ladder — needed because the §1
 * orderings couple the five primitives, so "set v_bulk to its minimum" is not a
 * single-field edit.
 *
 * Sized for the installed unit (PROJECT_PLAN §5): 16S / 48 V, 300 Ah — the same
 * bank prop.h uses, so the §3.2 C-relative tail window is the same in both.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TEST_CONFIG_H
#define WS500_TEST_CONFIG_H

#include "test.h"
#include <string.h>
#include "config_codec.h"
#include "config_validate.h"
#include "config_store.h"

#define CFGT_CELLS     16u
#define CFGT_BANK_AH   300.0f
#define CFGT_MAX_W     8000.0f

/* §3.2 materialized default: 0.04 x C x cells x 3.45 V/cell. */
#define CFGT_P_TAIL_W  (0.04f * CFGT_BANK_AH * (float)CFGT_CELLS * 3.45f)

static inline void cfgt_profile(cfg_profile_t *pr, uint8_t id,
                                cfg_prim_ref_t cv, bool solar_finish,
                                ctrl_rest_mode_t mode, cfg_prim_ref_t rest,
                                float rest_cap_w, float v_revert,
                                uint16_t revert_hold_s, int8_t soc_revert)
{
    pr->flags            = 0u;
    pr->cv_target_ref    = cv;
    pr->rest_voltage_ref = rest;
    pr->p.id                 = id;
    pr->p.cv_target_vcell    = 0.0f;    /* filled by cfg_resolve_primitives() */
    pr->p.exit_at_cv_entry   = solar_finish;
    pr->p.rest_mode          = mode;
    pr->p.rest_voltage_vcell = 0.0f;    /* ditto */
    pr->p.rest_power_cap_w   = rest_cap_w;
    pr->p.v_revert_vcell     = v_revert;
    pr->p.t_revert_hold_s    = revert_hold_s;
    pr->p.soc_revert_pct     = soc_revert;
    pr->p.ah_revert          = 0.30f * CFGT_BANK_AH;   /* §4.1 default 0.30 C */
}

/* A config that ctrl_config_validate() must accept unmodified. */
static inline ctrl_config_t cfg_test_baseline(void)
{
    ctrl_config_t c;
    memset(&c, 0, sizeof c);

    /* §1 primitive defaults. */
    c.prim.v_bulk       = 3.60f;
    c.prim.v_bulk_cons  = 3.50f;
    c.prim.v_float      = 3.40f;
    c.prim.v_float_cons = 3.35f;
    c.prim.v_limp       = 3.30f;

    /* §3.1 / §3.2 globals. */
    c.globals.cells_series        = (uint8_t)CFGT_CELLS;
    c.globals.bank_capacity_ah    = CFGT_BANK_AH;
    c.globals.max_charge_power_w  = CFGT_MAX_W;
    c.globals.ramp_w_per_s        = 200.0f;
    c.globals.p_tail_w            = CFGT_P_TAIL_W;
    c.globals.t_tail_hold_s       = 60;
    c.globals.t_vclamp_s          = 5;
    c.globals.cv_hold_exit_min    = 15;
    c.globals.t_charge_max_min    = 480;
    c.globals.warmup_time_s       = 30;
    c.globals.warmup_coolant_c    = NAN;   /* unset */
    c.globals.soc_target_pct      = -1;    /* unset */
    c.globals.skip_bulk_vcell     = 0.0f;  /* off */
    c.globals.skip_bulk_soc_pct   = -1;    /* off */
    c.globals.rotor_rated_v       = 12.0f; /* §5.1, 12 V rotor on the 48 V bus */
    c.globals.rotor_v_max         = NAN;   /* use rated */
    c.globals.allow_full_field_48v = false;
    c.globals.limp_vcell          = 3.30f; /* mirrors v_limp */
    c.globals.limp_power_cap_w    = 0.25f * CFGT_MAX_W;

    /* CONTROL_SPEC §2.1 limit set. */
    c.limits.battery_c_limit    = 0.5f;
    c.limits.wiring_limit_a     = 250.0f;
    c.limits.alternator_limit_a = 220.0f;

    /* §4.1 the seven profiles. rest_power_cap_w is the %max column resolved
     * against max_charge_power_w. */
    cfgt_profile(&c.profiles[0], 1, CFG_PRIM_V_BULK,      false, CTRL_REST_HOLD,
                 CFG_PRIM_V_FLOAT,      1.00f * CFGT_MAX_W, 3.28f, 30, 50);
    cfgt_profile(&c.profiles[1], 2, CFG_PRIM_V_BULK,      false, CTRL_REST_HOLD,
                 CFG_PRIM_V_FLOAT_CONS, 0.25f * CFGT_MAX_W, 3.22f, 60, 40);
    cfgt_profile(&c.profiles[2], 3, CFG_PRIM_V_BULK,      true,  CTRL_REST_ZERO,
                 CFG_PRIM_NONE,         0.0f,               3.25f, 60, 50);
    cfgt_profile(&c.profiles[3], 4, CFG_PRIM_V_BULK_CONS, false, CTRL_REST_HOLD,
                 CFG_PRIM_V_FLOAT_CONS, 0.50f * CFGT_MAX_W, 3.25f, 30, 50);
    cfgt_profile(&c.profiles[4], 5, CFG_PRIM_V_BULK,      false, CTRL_REST_ZERO,
                 CFG_PRIM_NONE,         0.0f,               3.25f, 30, 50);
    /* 6 — Limp Home: FLOAT at v_limp directly, no Bulk, no exits ("—"). */
    cfgt_profile(&c.profiles[5], 6, CFG_PRIM_V_LIMP,      false, CTRL_REST_HOLD,
                 CFG_PRIM_V_LIMP,       0.25f * CFGT_MAX_W, 0.0f,  0, -1);
    /* 7 — Fast Charge: schema slot reserved now, curve-based later. */
    cfgt_profile(&c.profiles[6], 7, CFG_PRIM_NONE,        false, CTRL_REST_ZERO,
                 CFG_PRIM_NONE,         0.0f,               0.0f,  0, -1);
    c.profiles[6].flags = CFG_PROF_FLAG_RESERVED;

    c.active_profile = 1;

    cfg_resolve_primitives(&c);
    return c;
}

/* Set one §1 primitive and re-derive the rest into a legal ladder: everything
 * below the edited one steps down by its gap + margin, everything above steps
 * up, each clamped to its own §1 range. The edited primitive itself is NEVER
 * clamped — that is the whole point when the caller is probing an out-of-range
 * value and expects a specific rejection. */
static inline void cfg_test_set_primitive(ctrl_config_t *c, cfg_prim_ref_t which, float v)
{
    static const float LO[5]  = { 3.45f, 3.40f, 3.33f, 3.28f, 3.20f };
    static const float HI[5]  = { 3.65f, 3.58f, 3.45f, 3.40f, 3.35f };
    static const float GAP[4] = { 0.03f, 0.03f, 0.02f, 0.02f };   /* §1, i -> i+1 */
    const float margin = 0.02f;      /* keeps every step clear of the gap minimum */

    float p[5] = { c->prim.v_bulk, c->prim.v_bulk_cons, c->prim.v_float,
                   c->prim.v_float_cons, c->prim.v_limp };
    const int k = (int)which - 1;
    if (k < 0 || k > 4) return;

    p[k] = v;
    for (int i = k + 1; i <= 4; i++) {
        float x = p[i - 1] - GAP[i - 1] - margin;
        p[i] = (x < LO[i]) ? LO[i] : ((x > HI[i]) ? HI[i] : x);
    }
    for (int i = k - 1; i >= 0; i--) {
        float x = p[i + 1] + GAP[i] + margin;
        p[i] = (x < LO[i]) ? LO[i] : ((x > HI[i]) ? HI[i] : x);
    }

    c->prim.v_bulk = p[0]; c->prim.v_bulk_cons = p[1]; c->prim.v_float = p[2];
    c->prim.v_float_cons = p[3]; c->prim.v_limp = p[4];
    cfg_resolve_primitives(c);       /* profiles + globals.limp_vcell follow */
}

#endif /* WS500_TEST_CONFIG_H */
