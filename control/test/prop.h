/*
 * prop.h — scaffolding for the PROJECT_PLAN §8.4 property / fault-injection
 * tests: a deterministic PRNG, an invariant accumulator, and the shared
 * core-invariant checker used by test_property.c and test_fuzz.c.
 *
 * Style note: property sweeps evaluate millions of points, so they do NOT emit
 * one CHECK() per point. Each invariant accumulates (points, violations) and
 * captures the FIRST violating context verbatim; prop_*_report() then folds the
 * whole sweep into one CHECK() per invariant with a reproducible printout.
 * Formatting only happens on a violation, so the hot path stays allocation- and
 * printf-free.
 *
 * No libc rand() anywhere — xorshift32 with explicit seeds, so every failure is
 * reproducible from the printed (seed, tick) pair.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TEST_PROP_H
#define WS500_TEST_PROP_H

#include "test.h"
#include "control.h"
#include "field.h"
#include "faults.h"

#include <stdarg.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

/* ---- deterministic PRNG (xorshift32) -------------------------------------- */
typedef struct { uint32_t s; } prop_rng_t;

static inline void prop_seed(prop_rng_t *r, uint32_t seed)
{
    r->s = seed ? seed : 0xA5A5A5A5u;
}
static inline uint32_t prop_u32(prop_rng_t *r)
{
    uint32_t x = r->s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    r->s = x ? x : 0xA5A5A5A5u;
    return r->s;
}
static inline float prop_unit(prop_rng_t *r)                 /* [0,1) */
{
    return (float)(prop_u32(r) >> 8) * (1.0f / 16777216.0f);
}
static inline float prop_between(prop_rng_t *r, float lo, float hi)
{
    return lo + (hi - lo) * prop_unit(r);
}
static inline float prop_sym(prop_rng_t *r)                  /* [-1,1) */
{
    return 2.0f * prop_unit(r) - 1.0f;
}
static inline int prop_chance(prop_rng_t *r, float p)
{
    return prop_unit(r) < p;
}
static inline uint32_t prop_below(prop_rng_t *r, uint32_t n)
{
    return n ? (prop_u32(r) % n) : 0u;
}

/* ---- violating-input context (formatted only on failure) ------------------ */
typedef struct {
    const char *tag;
    uint32_t    seed;
    uint64_t    tick;
    float v_true, vbat, vsup, amps, watts, alt_c, drv_c, batt_c, rpm;
    int   isrc, rpm_state, run_state, ignition;
    uint32_t ext_faults;
} prop_ctx_t;

static inline void prop_fmt_ctx(char *b, size_t n, const prop_ctx_t *cx)
{
    snprintf(b, n,
             "tag=%s seed=0x%08lX tick=%llu | v_true=%.5g vbat=%.5g vsup=%.5g A=%.5g W=%.5g "
             "alt=%.4g drv=%.4g batt=%.4g rpm=%.5g rpm_state=%d run=%d isrc=%d ign=%d ext=0x%08lx",
             cx->tag ? cx->tag : "-", (unsigned long)cx->seed, (unsigned long long)cx->tick,
             (double)cx->v_true, (double)cx->vbat, (double)cx->vsup,
             (double)cx->amps, (double)cx->watts,
             (double)cx->alt_c, (double)cx->drv_c, (double)cx->batt_c, (double)cx->rpm,
             cx->rpm_state, cx->run_state, cx->isrc, cx->ignition,
             (unsigned long)cx->ext_faults);
}

/* ---- invariant accumulator ------------------------------------------------ */
typedef struct {
    const char *name;
    uint64_t    points;
    uint64_t    violations;
    char        first[640];
} prop_inv_t;

static inline void prop_inv_init(prop_inv_t *iv, const char *name)
{
    iv->name = name; iv->points = 0; iv->violations = 0; iv->first[0] = '\0';
}

/* Every invariant point evaluated by any sweep (defined in test_property.c). */
extern uint64_t g_prop_points;

/* Record one evaluated point. `fmt` is expanded ONLY for the first violation. */
static inline void prop_note(prop_inv_t *iv, int ok, const prop_ctx_t *cx,
                             const char *fmt, ...)
{
    iv->points++;
    g_prop_points++;
    if (ok) return;
    if (iv->violations == 0) {
        char d[224];
        va_list ap; va_start(ap, fmt); vsnprintf(d, sizeof d, fmt, ap); va_end(ap);
        if (cx) {
            char c[384];
            prop_fmt_ctx(c, sizeof c, cx);
            snprintf(iv->first, sizeof iv->first, "%s || %s", d, c);
        } else {
            snprintf(iv->first, sizeof iv->first, "%s", d);
        }
    }
    iv->violations++;
}

/* One CHECK() per invariant; prints the first repro on failure. */
static inline void prop_report(const prop_inv_t *iv)
{
    if (iv->violations) {
        printf("PROP-VIOLATION %s: %llu of %llu points\n  repro: %s\n",
               iv->name, (unsigned long long)iv->violations,
               (unsigned long long)iv->points, iv->first);
    }
    CHECK(iv->violations == 0);
}

/* A characterized, review-gated gap: the violation is REAL and deliberately not
 * fixed here (safety review gate, PROJECT_PLAN §5 / CLAUDE.md review gate).
 * Asserting violations>0 makes the marker self-expiring: if the core is fixed,
 * this fails and forces the note to be removed. */
static inline void prop_report_expected_gap(const prop_inv_t *iv, const char *why)
{
    printf("EXPECTED-GAP %s: %llu of %llu points violate — %s\n  repro: %s\n",
           iv->name, (unsigned long long)iv->violations,
           (unsigned long long)iv->points, why, iv->first);
    CHECK(iv->violations > 0);   /* self-expiring marker; see comment above */
}

/* ---- shared core invariants ----------------------------------------------- */
typedef struct {
    prop_inv_t finite;      /* effort/duty are finite numbers */
    prop_inv_t duty_rng;    /* duty ∈ [0,1] */
    prop_inv_t eff_rng;     /* effort ∈ [0,1] */
    prop_inv_t clamp;       /* duty ≤ ctrl_duty_max(TRUE supply)  ← THE safety one */
    prop_inv_t rotor_v;     /* duty × V_true ≤ rotor rated volts (physical form) */
    prop_inv_t open_zero;   /* field_open ⇒ duty == 0 */
    prop_inv_t open_latch;  /* OPEN-class fault ⇒ open + STANDBY/FAULT + duty 0 */
    prop_inv_t enums;       /* state / reason / binding stay inside their enums */
} prop_core_inv_t;

static inline void prop_core_init(prop_core_inv_t *iv)
{
    prop_inv_init(&iv->finite,     "cmd-finite (effort/duty are numbers)");
    prop_inv_init(&iv->duty_rng,   "duty in [0,1]");
    prop_inv_init(&iv->eff_rng,    "effort in [0,1]");
    prop_inv_init(&iv->clamp,      "ROTOR CLAMP: duty <= duty_max(true V_supply)");
    prop_inv_init(&iv->rotor_v,    "ROTOR CLAMP: duty x V_true <= rotor rated V");
    prop_inv_init(&iv->open_zero,  "field_open => duty == 0");
    prop_inv_init(&iv->open_latch, "OPEN-class fault => open + STANDBY/FAULT + duty 0");
    prop_inv_init(&iv->enums,      "state/reason/binding inside enum range");
}

static inline float prop_rotor_v(const ctrl_globals_t *g)
{
    return isnan(g->rotor_v_max) ? g->rotor_rated_v : g->rotor_v_max;
}

/*
 * Check every standing invariant of one ctrl_tick() result.
 *   v_ref : the TRUE field-supply voltage for this point (what the hardware
 *           actually puts across the rotor), independent of what the sensors
 *           reported. <=0 or non-finite ⇒ the clamp invariants are skipped
 *           (caller must supply the worst-case plausible bus instead).
 *   tol   : fractional allowance for injected supply-sensor noise (0 = exact).
 */
static inline void prop_core_check(prop_core_inv_t *iv, const ctrl_command_t *cmd,
                                   const ctrl_globals_t *g, float v_ref, float tol,
                                   const prop_ctx_t *cx)
{
    const float d = cmd->field_duty, e = cmd->field_effort;

    prop_note(&iv->finite, isfinite(d) && isfinite(e), cx,
              "effort=%.6g duty=%.6g", (double)e, (double)d);
    prop_note(&iv->duty_rng, isfinite(d) && d >= 0.0f && d <= 1.0f, cx,
              "duty=%.6g", (double)d);
    prop_note(&iv->eff_rng, isfinite(e) && e >= 0.0f && e <= 1.0f, cx,
              "effort=%.6g", (double)e);

    if (isfinite(v_ref) && v_ref > 0.0f && isfinite(d)) {
        const float dmax = ctrl_duty_max(g, v_ref);
        prop_note(&iv->clamp, d <= dmax * (1.0f + tol) + 1e-5f, cx,
                  "duty=%.6g > duty_max(%.5g V)=%.6g (tol=%.3g)",
                  (double)d, (double)v_ref, (double)dmax, (double)tol);
        if (!g->allow_full_field_48v) {          /* config may lift the clamp (§5.1) */
            const float rv = prop_rotor_v(g);
            if (isfinite(rv) && rv > 0.0f)
                prop_note(&iv->rotor_v, d * v_ref <= rv * (1.0f + tol) + 1e-3f, cx,
                          "duty*V = %.6g x %.5g = %.5g V > rotor %.5g V",
                          (double)d, (double)v_ref, (double)(d * v_ref), (double)rv);
        }
    }

    prop_note(&iv->open_zero, !cmd->field_open || (isfinite(d) && d == 0.0f), cx,
              "field_open with duty=%.6g", (double)d);

    prop_note(&iv->open_latch,
              !(cmd->faults & CTRL_FAULT_OPEN_MASK) ||
              (cmd->field_open && d == 0.0f &&
               cmd->state == CTRL_STANDBY && cmd->standby_reason == CTRL_SB_FAULT), cx,
              "faults=0x%08lx state=%d reason=%d open=%d duty=%.6g",
              (unsigned long)cmd->faults, (int)cmd->state, (int)cmd->standby_reason,
              (int)cmd->field_open, (double)d);

    prop_note(&iv->enums,
              (cmd->state == CTRL_STANDBY || cmd->state == CTRL_BULK || cmd->state == CTRL_FLOAT) &&
              (int)cmd->standby_reason >= 0 && cmd->standby_reason <= CTRL_SB_FAULT &&
              (int)cmd->binding >= 0 && cmd->binding <= CTRL_BIND_RUN_DETECT, cx,
              "state=%d reason=%d binding=%d",
              (int)cmd->state, (int)cmd->standby_reason, (int)cmd->binding);
}

static inline void prop_core_report(const prop_core_inv_t *iv)
{
    prop_report(&iv->finite);
    prop_report(&iv->duty_rng);
    prop_report(&iv->eff_rng);
    prop_report(&iv->clamp);
    prop_report(&iv->rotor_v);
    prop_report(&iv->open_zero);
    prop_report(&iv->open_latch);
    prop_report(&iv->enums);
}

/* ---- shared baseline config ------------------------------------------------ *
 * The installed unit (PROJECT_PLAN §5): 16S / 48 V bank, 4 Ω 12 V rotor. The 4S
 * variant covers the 12 V case where the clamp saturates at 1.0. */
static inline ctrl_globals_t prop_globals(uint8_t cells)
{
    ctrl_globals_t g;
    memset(&g, 0, sizeof g);
    g.cells_series        = cells;
    g.bank_capacity_ah    = 300.0f;
    g.max_charge_power_w  = 8000.0f;
    g.ramp_w_per_s        = 200.0f;
    g.p_tail_w            = 0.04f * 300.0f * (float)cells * 3.45f;
    g.t_tail_hold_s       = 60;
    g.t_vclamp_s          = 5;
    g.cv_hold_exit_min    = 15;
    g.t_charge_max_min    = 480;
    g.warmup_time_s       = 0;
    g.warmup_coolant_c    = NAN;
    g.soc_target_pct      = -1;
    g.skip_bulk_vcell     = 0.0f;
    g.skip_bulk_soc_pct   = -1;
    g.rotor_rated_v       = 12.0f;
    g.rotor_v_max         = NAN;
    g.allow_full_field_48v = false;
    g.limp_vcell          = 3.30f;
    g.limp_power_cap_w    = 2000.0f;
    return g;
}

static inline ctrl_profile_t prop_profile(void)
{
    ctrl_profile_t p;
    memset(&p, 0, sizeof p);
    p.id                 = 1;
    p.cv_target_vcell    = 3.60f;
    p.exit_at_cv_entry   = false;
    p.rest_mode          = CTRL_REST_HOLD;
    p.rest_voltage_vcell = 3.40f;
    p.rest_power_cap_w   = 4000.0f;
    p.v_revert_vcell     = 3.28f;
    p.t_revert_hold_s    = 30;
    p.soc_revert_pct     = -1;
    p.ah_revert          = 90.0f;
    return p;
}

/* Healthy measurement at a per-cell voltage on a `cells`-series bank. */
static inline ctrl_measured_t prop_measured(uint8_t cells, float vcell)
{
    ctrl_measured_t m;
    memset(&m, 0, sizeof m);
    m.vbat_pack_v  = vcell * (float)cells;
    m.vcomp_pack_v = m.vbat_pack_v;
    m.amps_batt    = 40.0f;
    m.watts_batt   = 40.0f * m.vbat_pack_v;
    m.isrc         = CTRL_ISRC_BATT_SHUNT;
    m.v_supply_v   = m.vbat_pack_v;
    m.alt_hotspot_c = 60.0f;
    m.batt_temp_c   = 25.0f;
    m.driver_temp_c = 50.0f;
    m.rpm          = 1800.0f;
    m.rpm_state    = CTRL_RPM_VALID;
    m.run_state    = CTRL_RUN_RUNNING;
    m.soc_pct      = -1.0f;
    m.soc_trusted  = false;
    m.ignition     = true;
    m.feature_in   = false;
    m.ext_faults   = 0;
    return m;
}

#endif /* WS500_TEST_PROP_H */
