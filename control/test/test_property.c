/*
 * test_property.c — PROJECT_PLAN §8.4 property tests: sweep-based invariant
 * checks over ALL input combinations, and boundary sweeps on the PROFILE_SPEC
 * §1/§3 parameter ranges.
 *
 * The invariant that matters most for the installed unit (PROJECT_PLAN §5): a
 * 4 Ω 12 V rotor on a 48 V bus is destroyed by sustained duty above ~25 %, so
 * commanded field duty must NEVER exceed ctrl_duty_max(TRUE supply voltage) —
 * no matter what the sensors report, what the config says, or what state the
 * engine is in. Everything here is a sweep, not an example.
 *
 * Sections:
 *   P1  stateless field map        — ctrl_duty_max / ctrl_effort_to_duty
 *   P2  clamp-supply guard         — ctrl_vsup_guard, grid + random sequences
 *   P3  arbitration                — min(), monotonicity in every ceiling
 *   P4  engine input grid          — dense combinatorial ctrl_tick() sweep
 *   P5  fault latching / clearing  — OPEN latches, WARN/LIMP/BLOCK do not
 *   P6  profile boundary sweeps    — §1/§3 edges + no-silent-auto-correction
 *   P6b config validator conformance — the same table, against ctrl_config_validate
 *   P7  ceiling producers          — ctrl_limits_apply / ctrl_thermal_update
 *
 * SPDX-License-Identifier: MIT
 */
#include "prop.h"
#include "arbitration.h"
#include "limits.h"
#include "thermal.h"
#include "test_config.h"   /* baseline config + §1 primitive ladder (P6 conformance) */

/* ---- shared input alphabets ------------------------------------------------ */

/* Supply / bus voltages: garbage, negative, zero, 12 V, 24 V, 48 V classes, absurd. */
static const float V_SWEEP[] = {
    NAN, INFINITY, -INFINITY, -100.0f, -12.0f, -1e-6f, 0.0f, 1e-6f, 0.5f, 6.0f,
    11.9f, 12.0f, 12.6f, 13.8f, 14.4f, 24.0f, 27.2f, 28.8f, 42.0f, 48.0f,
    52.8f, 54.4f, 57.6f, 60.0f, 64.0f, 100.0f, 1.0e6f
};
#define N_V ((int)(sizeof V_SWEEP / sizeof V_SWEEP[0]))

/* Rotor voltage ratings (also used for the rotor_v_max override; NAN = unset). */
static const float ROTOR_SWEEP[] = {
    NAN, -INFINITY, -1.0f, 0.0f, 1e-6f, 6.0f, 12.0f, 15.0f, 24.0f, 48.0f, 1.0e6f, INFINITY
};
#define N_ROTOR ((int)(sizeof ROTOR_SWEEP / sizeof ROTOR_SWEEP[0]))

static const float EFFORT_SWEEP[] = {
    -INFINITY, -1.0f, -1e-6f, 0.0f, 1e-6f, 0.001f, 0.25f, 0.5f, 0.9f, 0.999f,
    1.0f, 1.0f + 1e-6f, 2.0f, 1.0e9f, INFINITY
};
#define N_EFFORT ((int)(sizeof EFFORT_SWEEP / sizeof EFFORT_SWEEP[0]))

/* =========================================================================== *
 * P1 — stateless field map (field.c)
 * =========================================================================== */
static void p1_field_map(void)
{
    prop_inv_t dm_range, dm_mono, duty_range, duty_le_dm, duty_mono, phys, nan_eff, dm_infinf;
    prop_inv_init(&dm_range,   "P1 duty_max finite and in [0,1]");
    prop_inv_init(&dm_infinf,  "P1 duty_max is fail-safe for infinite rotor_v AND supply");
    prop_inv_init(&dm_mono,    "P1 duty_max non-increasing in V_supply");
    prop_inv_init(&duty_range, "P1 duty finite and in [0,1] (finite effort)");
    prop_inv_init(&duty_le_dm, "P1 duty <= duty_max");
    prop_inv_init(&duty_mono,  "P1 duty non-decreasing in effort");
    prop_inv_init(&phys,       "P1 duty x V_supply <= rotor rated V");
    prop_inv_init(&nan_eff,    "P1 NaN effort => finite duty");

    for (int ir = 0; ir < N_ROTOR; ir++) {
        for (int im = 0; im < N_ROTOR; im++) {
            for (int af = 0; af < 2; af++) {
                ctrl_globals_t g = prop_globals(16);
                g.rotor_rated_v = ROTOR_SWEEP[ir];
                g.rotor_v_max   = ROTOR_SWEEP[im];
                g.allow_full_field_48v = (af != 0);

                float prev_dm = 2.0f, prev_v = -1.0f;
                for (int iv = 0; iv < N_V; iv++) {
                    const float v  = V_SWEEP[iv];
                    const float dm = ctrl_duty_max(&g, v);
                    const int   dm_ok = isfinite(dm) && dm >= 0.0f && dm <= 1.0f;

                    /* rotor_v == +inf AND V_supply == +inf is the one input pair
                     * that makes the division indeterminate; kept out of the
                     * strict invariant and pinned as an EXPECTED-GAP below. */
                    const float rv_eff = prop_rotor_v(&g);
                    const int inf_over_inf = !g.allow_full_field_48v &&
                                             rv_eff > 0.0f && !isfinite(rv_eff) &&
                                             v > 0.0f && !isfinite(v);
                    if (inf_over_inf) {
                        prop_note(&dm_infinf, dm_ok, NULL,
                                  "ctrl_duty_max(rotor_v=%.4g, V_supply=%.4g) = %.6g "
                                  "(rated=%.4g vmax=%.4g)",
                                  (double)rv_eff, (double)v, (double)dm,
                                  (double)g.rotor_rated_v, (double)g.rotor_v_max);
                        continue;      /* duty invariants below need a usable duty_max */
                    }
                    prop_note(&dm_range, dm_ok, NULL,
                              "duty_max=%.6g rated=%.4g vmax=%.4g full=%d V=%.5g",
                              (double)dm, (double)g.rotor_rated_v, (double)g.rotor_v_max,
                              af, (double)v);

                    /* Monotone over the ascending finite-positive tail of V_SWEEP. */
                    if (isfinite(v) && v > 0.0f) {
                        if (prev_v > 0.0f && v >= prev_v)
                            prop_note(&dm_mono, dm <= prev_dm + 1e-6f, NULL,
                                      "duty_max rose: V %.5g->%.5g gave %.6g->%.6g",
                                      (double)prev_v, (double)v, (double)prev_dm, (double)dm);
                        prev_v = v; prev_dm = dm;
                    }

                    float prev_duty = -1.0f;
                    for (int ie = 0; ie < N_EFFORT; ie++) {
                        const float e = EFFORT_SWEEP[ie];
                        const float d = ctrl_effort_to_duty(e, dm);
                        prop_note(&duty_range, isfinite(d) && d >= 0.0f && d <= 1.0f, NULL,
                                  "effort=%.6g duty_max=%.6g -> duty=%.6g",
                                  (double)e, (double)dm, (double)d);
                        prop_note(&duty_le_dm, !isfinite(d) || d <= dm + 1e-6f, NULL,
                                  "effort=%.6g duty=%.6g > duty_max=%.6g",
                                  (double)e, (double)d, (double)dm);
                        prop_note(&duty_mono, !isfinite(d) || d >= prev_duty - 1e-6f, NULL,
                                  "effort=%.6g duty=%.6g < previous %.6g",
                                  (double)e, (double)d, (double)prev_duty);
                        if (isfinite(d)) prev_duty = d;

                        if (!g.allow_full_field_48v && isfinite(v) && v > 0.0f) {
                            const float rv = prop_rotor_v(&g);
                            if (isfinite(rv) && rv > 0.0f)
                                prop_note(&phys, d * v <= rv * 1.0000001f + 1e-3f, NULL,
                                          "duty %.6g x V %.5g = %.5g V on a %.5g V rotor",
                                          (double)d, (double)v, (double)(d * v), (double)rv);
                        }
                    }

                    /* Separate accumulator: NaN effort (see EXPECTED-GAP below). */
                    prop_note(&nan_eff, isfinite(ctrl_effort_to_duty(NAN, dm)), NULL,
                              "ctrl_effort_to_duty(NAN, %.6g) = %.6g (duty_max from "
                              "rated=%.4g vmax=%.4g full=%d V=%.5g)",
                              (double)dm, (double)ctrl_effort_to_duty(NAN, dm),
                              (double)g.rotor_rated_v, (double)g.rotor_v_max, af, (double)v);
                }
            }
        }
    }

    prop_report(&dm_range);
    prop_report(&dm_mono);
    /* [§8.4-FOUND 2026-07-26, FIXED same day after safety review] rotor_v=+inf
     * with V_supply=+inf gave inf/inf = NaN escaping the ordered clamps;
     * ctrl_duty_max() now requires isfinite(rotor_v) and NaN-proofs the range
     * clamp (negated-positive form). Strict invariant from here on. */
    prop_report(&dm_infinf);
    prop_report(&duty_range);
    prop_report(&duty_le_dm);
    prop_report(&duty_mono);
    prop_report(&phys);

    /* [§8.4-FOUND 2026-07-26, FIXED same day after safety review] NaN effort
     * escaped the ordered [0,1] clamp; ctrl_effort_to_duty() now uses the
     * negated-positive form. Strict invariant from here on. */
    prop_report(&nan_eff);
}

/* =========================================================================== *
 * P2 — clamp-supply plausibility guard (§5.1)
 * =========================================================================== */
static void p2_vsup_guard(void)
{
    static const uint8_t CELLS[] = { 0, 4, 8, 16 };
    prop_inv_t nan_iff, not_looser, positive, dm_tighter, step_bound;
    prop_inv_init(&nan_iff,    "P2 guard returns NaN iff no channel is finite");
    prop_inv_init(&not_looser, "P2 guard output >= max(finite channels) (never looser than truth)");
    prop_inv_init(&positive,   "P2 finite guard output is > 0");
    prop_inv_init(&dm_tighter, "P2 duty_max(guard) <= duty_max(max channel)");
    prop_inv_init(&step_bound, "P2 clamp voltage loosens by <= CTRL_VSUP_STEP_FRAC per tick");

    ctrl_globals_t g = prop_globals(16);

    /* --- grid: every (v_supply, vbat, cells) on a freshly seeded guard --- */
    for (int ic = 0; ic < 4; ic++) {
        for (int i = 0; i < N_V; i++) {
            for (int j = 0; j < N_V; j++) {
                const float vs = V_SWEEP[i], vb = V_SWEEP[j];
                ctrl_vsup_guard_t fg; ctrl_vsup_guard_init(&fg);
                bool bad = false;
                const float out = ctrl_vsup_guard(&fg, vs, vb, CELLS[ic], 10, &bad);

                const int any_finite = isfinite(vs) || isfinite(vb);
                float truth = -INFINITY;
                if (isfinite(vs)) truth = vs;
                if (isfinite(vb) && vb > truth) truth = vb;

                prop_note(&nan_iff, (isnan(out) != 0) == (any_finite == 0), NULL,
                          "vs=%.5g vb=%.5g cells=%u -> %.6g",
                          (double)vs, (double)vb, (unsigned)CELLS[ic], (double)out);
                if (!isnan(out)) {
                    prop_note(&not_looser, out >= truth - 1e-3f, NULL,
                              "vs=%.5g vb=%.5g cells=%u -> %.6g < truth %.6g",
                              (double)vs, (double)vb, (unsigned)CELLS[ic],
                              (double)out, (double)truth);
                    prop_note(&positive, isfinite(out) && out > 0.0f, NULL,
                              "vs=%.5g vb=%.5g cells=%u -> %.6g",
                              (double)vs, (double)vb, (unsigned)CELLS[ic], (double)out);
                    if (truth > 0.0f)
                        prop_note(&dm_tighter,
                                  ctrl_duty_max(&g, out) <= ctrl_duty_max(&g, truth) + 1e-6f, NULL,
                                  "vs=%.5g vb=%.5g cells=%u: duty_max %.6g > truth's %.6g",
                                  (double)vs, (double)vb, (unsigned)CELLS[ic],
                                  (double)ctrl_duty_max(&g, out),
                                  (double)ctrl_duty_max(&g, truth));
                }
            }
        }
    }

    /* --- random sequences: lies, dropouts, spikes, sags on both channels ---
     * Kept well under CTRL_VSUP_DISTRUST_MAX_MS so the deliberate 5-minute
     * re-anchor (characterized in P2b) is not what we are measuring here. */
    for (uint32_t seed = 1; seed <= 200u; seed++) {
        prop_rng_t r; prop_seed(&r, seed);
        ctrl_vsup_guard_t fg; ctrl_vsup_guard_init(&fg);
        const uint8_t cells = 16;
        float bus = prop_between(&r, 42.0f, 60.0f);

        for (int t = 0; t < 300; t++) {
            bus += prop_sym(&r) * 0.05f;
            if (bus < 40.0f) bus = 40.0f;
            if (bus > 62.0f) bus = 62.0f;

            float vs = bus, vb = bus;
            if (prop_chance(&r, 0.05f)) vs = NAN;                       /* dropout */
            if (prop_chance(&r, 0.05f)) vb = NAN;
            if (prop_chance(&r, 0.05f)) vs = bus * prop_between(&r, 0.2f, 0.9f); /* false low */
            if (prop_chance(&r, 0.03f)) vb = bus * prop_between(&r, 1.1f, 4.0f); /* spike high */
            if (prop_chance(&r, 0.02f)) vs = -bus;                      /* sign flip */

            const float track_prev = fg.v_track;
            bool bad = false;
            const float out = ctrl_vsup_guard(&fg, vs, vb, cells, 10, &bad);

            float truth = -INFINITY;
            if (isfinite(vs)) truth = vs;
            if (isfinite(vb) && vb > truth) truth = vb;
            if (!isnan(out)) {
                prop_note(&not_looser, out >= truth - 1e-3f, NULL,
                          "seq seed=%u t=%d vs=%.5g vb=%.5g -> %.6g < truth %.6g",
                          (unsigned)seed, t, (double)vs, (double)vb, (double)out, (double)truth);
                if (isfinite(track_prev))
                    prop_note(&step_bound,
                              out >= track_prev * (1.0f - CTRL_VSUP_STEP_FRAC) - 1e-3f, NULL,
                              "seq seed=%u t=%d track %.6g -> out %.6g (> %.1f%% looser)",
                              (unsigned)seed, t, (double)track_prev, (double)out,
                              (double)(100.0f * CTRL_VSUP_STEP_FRAC));
            }
        }
    }

    prop_report(&nan_iff);
    prop_report(&not_looser);
    prop_report(&positive);
    prop_report(&dm_tighter);
    prop_report(&step_bound);
}

/* P2b — the two characterized ways a LYING supply sensor still loosens the
 * clamp. Both are inherent to a single physical voltage source and are already
 * flagged in sim/README.md as residual; this pins the exact numbers. */
static void p2b_vsup_residuals(void)
{
    prop_inv_t boot_lie, reanchor, slow_drift, floor_bound;
    prop_inv_init(&boot_lie,   "P2b from-boot false-low keeps clamp tied to truth");
    prop_inv_init(&reanchor,   "P2b sustained false-low never re-anchors below truth");
    prop_inv_init(&slow_drift, "P2b sub-step drifting lie never walks the clamp down");
    prop_inv_init(&floor_bound,
                  "P2b HARD BOUND: clamp voltage never below CTRL_VSUP_FLOOR_VCELL x cells");

    ctrl_globals_t g = prop_globals(16);
    const float truth = 57.6f;
    const float floorv = CTRL_VSUP_FLOOR_VCELL * 16.0f;
    bool bad = false;

    /* (a) The sensor has ALWAYS read low — no honest history to compare with. */
    {
        ctrl_vsup_guard_t fg; ctrl_vsup_guard_init(&fg);
        const float out = ctrl_vsup_guard(&fg, 49.0f, 49.0f, 16, 10, &bad);
        prop_note(&boot_lie, out >= truth - 1e-3f, NULL,
                  "boot lie 49.0 V on a true %.1f V bus -> clamp voltage %.4g, "
                  "duty_max %.4f (truth %.4f) => rotor sees up to %.2f V",
                  (double)truth, (double)out, (double)ctrl_duty_max(&g, out),
                  (double)ctrl_duty_max(&g, truth),
                  (double)(ctrl_duty_max(&g, out) * truth));
        prop_note(&floor_bound, out >= floorv - 1e-3f, NULL,
                  "boot lie: clamp voltage %.4g below floor %.4g", (double)out, (double)floorv);
    }

    /* (b) Honest first, then a step lie held past CTRL_VSUP_DISTRUST_MAX_MS. */
    {
        ctrl_vsup_guard_t fg; ctrl_vsup_guard_init(&fg);
        ctrl_vsup_guard(&fg, truth, truth, 16, 10, &bad);
        float out = truth;
        for (uint32_t t = 0; t <= CTRL_VSUP_DISTRUST_MAX_MS; t += 1000)
            out = ctrl_vsup_guard(&fg, 49.0f, 49.0f, 16, 1000, &bad);
        prop_note(&reanchor, out >= truth - 1e-3f, NULL,
                  "step lie 57.6->49.0 V held %u ms re-anchors to %.4g; duty_max "
                  "%.4f vs truth %.4f => rotor sees up to %.2f V",
                  (unsigned)CTRL_VSUP_DISTRUST_MAX_MS, (double)out,
                  (double)ctrl_duty_max(&g, out), (double)ctrl_duty_max(&g, truth),
                  (double)(ctrl_duty_max(&g, out) * truth));
        prop_note(&floor_bound, out >= floorv - 1e-3f, NULL,
                  "re-anchor: clamp voltage %.4g below floor %.4g", (double)out, (double)floorv);
    }

    /* (c) A lie that descends slower than CTRL_VSUP_STEP_FRAC per tick. */
    {
        ctrl_vsup_guard_t fg; ctrl_vsup_guard_init(&fg);
        float reported = truth, out = truth;
        ctrl_vsup_guard(&fg, reported, reported, 16, 10, &bad);
        for (int t = 0; t < 200; t++) {
            reported *= (1.0f - CTRL_VSUP_STEP_FRAC * 0.5f);
            if (reported < 2.0f * 16.0f) break;
            out = ctrl_vsup_guard(&fg, reported, reported, 16, 10, &bad);
        }
        prop_note(&slow_drift, out >= truth - 1e-3f, NULL,
                  "lie drifting %.1f%%/tick walks the clamp voltage from %.1f to "
                  "%.4g V; duty_max %.4f vs truth %.4f => rotor sees up to %.2f V",
                  (double)(100.0f * CTRL_VSUP_STEP_FRAC * 0.5f), (double)truth, (double)out,
                  (double)ctrl_duty_max(&g, out), (double)ctrl_duty_max(&g, truth),
                  (double)(ctrl_duty_max(&g, out) * truth));
        prop_note(&floor_bound, out >= floorv - 1e-3f, NULL,
                  "slow drift: clamp voltage %.4g below floor %.4g", (double)out, (double)floorv);
    }

    /* All three are deliberate design points, not regressions: with ONE physical
     * voltage source the core cannot distinguish a lying sensor from a real sag.
     * They are the reason the §5.1 guard is "best effort, never looser than what
     * it can prove", and they bound what the SIL/HIL layers must cover instead. */
    prop_report_expected_gap(&boot_lie,
        "a false-low present from boot has no honest history to be distrusted "
        "against (sim/README.md residual) — the clamp follows the lie");
    prop_report_expected_gap(&reanchor,
        "CTRL_VSUP_DISTRUST_MAX_MS (5 min) deliberately re-anchors onto a "
        "sustained lower reading — indistinguishable in-core from a real sag");
    prop_report_expected_gap(&slow_drift,
        "a lie changing by < CTRL_VSUP_STEP_FRAC per tick is by construction "
        "'a plausible sag' and is followed — but only down to the hard floor "
        "(2026-07-26 fix: was 1.8x rated rotor exposure, now bounded ~1.13x)");
    /* The mitigation IS strict: no lie, however shaped, takes the clamp voltage
     * below CTRL_VSUP_FLOOR_VCELL x cells (added after §8.4 found the slow-drift
     * walk-down; safety-reviewed). */
    prop_report(&floor_bound);
}

/* =========================================================================== *
 * P3 — arbitration: min(), monotonicity in every ceiling
 * =========================================================================== */
#define N_CEIL 10

static float ceil_get(const ctrl_ceilings_t *c, int i)
{
    switch (i) {
    case 0: return c->thermal_w;      case 1: return c->bms_ccl_w;
    case 2: return c->battery_c_w;    case 3: return c->wiring_w;
    case 4: return c->alt_absolute_w; case 5: return c->alt_capability_w;
    case 6: return c->belt_w;         case 7: return c->engine_w;
    case 8: return c->driver_thermal_w;  /* GH#39 */
    default: return c->user_cap_w;
    }
}
static void ceil_set(ctrl_ceilings_t *c, int i, float v)
{
    switch (i) {
    case 0: c->thermal_w = v; break;      case 1: c->bms_ccl_w = v; break;
    case 2: c->battery_c_w = v; break;    case 3: c->wiring_w = v; break;
    case 4: c->alt_absolute_w = v; break; case 5: c->alt_capability_w = v; break;
    case 6: c->belt_w = v; break;         case 7: c->engine_w = v; break;
    case 8: c->driver_thermal_w = v; break;  /* GH#39 */
    default: c->user_cap_w = v; break;
    }
}
static ctrl_bind_src_t ceil_src(int i)
{
    switch (i) {
    case 0: return CTRL_BIND_THERMAL;      case 1: return CTRL_BIND_BMS;
    case 2: return CTRL_BIND_BATTERY_C;    case 3: return CTRL_BIND_WIRING;
    case 4: return CTRL_BIND_ALT_ABSOLUTE; case 5: return CTRL_BIND_ALT_CAPABILITY;
    case 6: return CTRL_BIND_BELT;         case 7: return CTRL_BIND_ENGINE;
    case 8: return CTRL_BIND_DRIVER_THERMAL;  /* GH#39 */
    default: return CTRL_BIND_USER_CAP;
    }
}

/* Pick a ceiling value from the interesting alphabet. */
static float arb_pick(prop_rng_t *r)
{
    switch (prop_below(r, 8)) {
    case 0: return CTRL_CEILING_INACTIVE;
    case 1: return NAN;
    case 2: return -INFINITY;
    case 3: return 0.0f;
    case 4: return -1000.0f;
    case 5: return prop_between(r, 0.0f, 50.0f);
    case 6: return prop_between(r, 50.0f, 20000.0f);
    default: return prop_between(r, 0.0f, 1.0e9f);
    }
}

static void p3_arbitration(void)
{
    prop_inv_t nonneg, not_nan, le_stage, mono_down, mono_up, src_ok, mono_stage;
    prop_inv_init(&nonneg,     "P3 arbitrated watts >= 0");
    prop_inv_init(&not_nan,    "P3 arbitrated watts is never NaN");
    prop_inv_init(&le_stage,   "P3 arbitrated watts <= stage ceiling");
    prop_inv_init(&mono_down,  "P3 lowering any ceiling never raises watts");
    prop_inv_init(&mono_up,    "P3 raising any ceiling never lowers watts");
    prop_inv_init(&src_ok,     "P3 reported binding source equals the winning value");
    prop_inv_init(&mono_stage, "P3 lowering the stage ceiling never raises watts");

    prop_rng_t r; prop_seed(&r, 0x5EED0003u);

    for (int n = 0; n < 20000; n++) {
        ctrl_ceilings_t c;
        for (int i = 0; i < N_CEIL; i++) ceil_set(&c, i, arb_pick(&r));

        float stage;
        switch (prop_below(&r, 6)) {
        case 0:  stage = CTRL_CEILING_INACTIVE; break;
        case 1:  stage = NAN;                   break;
        case 2:  stage = -500.0f;               break;
        case 3:  stage = 0.0f;                  break;
        case 4:  stage = prop_between(&r, 0.0f, 20000.0f); break;
        default: stage = prop_between(&r, 0.0f, 1.0e6f);   break;
        }

        const ctrl_arb_t a = ctrl_arbitrate(stage, &c);

        prop_note(&not_nan, !isnan(a.watts), NULL, "stage=%.6g -> NaN watts", (double)stage);
        prop_note(&nonneg, a.watts >= 0.0f, NULL,
                  "stage=%.6g -> watts=%.6g", (double)stage, (double)a.watts);
        if (isfinite(stage) && stage >= 0.0f)
            prop_note(&le_stage, a.watts <= stage + 1e-3f, NULL,
                      "stage=%.6g -> watts=%.6g", (double)stage, (double)a.watts);

        if (a.src == CTRL_BIND_STAGE) {
            /* A negative/NaN stage is floored to 0 by the "never command
             * negative" rule, so STAGE may report 0 for a negative stage. */
            const float stage_eff = (stage > 0.0f) ? stage : 0.0f;
            prop_note(&src_ok, !isfinite(stage) || a.watts <= stage_eff + 1e-3f, NULL,
                      "src=STAGE watts=%.6g stage=%.6g", (double)a.watts, (double)stage);
        } else {
            int idx = -1;
            for (int i = 0; i < N_CEIL; i++) if (ceil_src(i) == a.src) idx = i;
            const float cw = idx >= 0 ? ceil_get(&c, idx) : NAN;
            prop_note(&src_ok, idx >= 0 && (cw == a.watts || (cw < 0.0f && a.watts == 0.0f)), NULL,
                      "src=%d claims %.6g but watts=%.6g", (int)a.src, (double)cw, (double)a.watts);
        }

        /* Monotonicity: perturb one ceiling at a time, both directions. */
        for (int i = 0; i < N_CEIL; i++) {
            const float orig = ceil_get(&c, i);
            const float delta = prop_between(&r, 1.0f, 5000.0f);

            if (isfinite(orig)) {
                ceil_set(&c, i, orig - delta);
                const ctrl_arb_t lo = ctrl_arbitrate(stage, &c);
                prop_note(&mono_down, lo.watts <= a.watts + 1e-3f, NULL,
                          "ceiling[%d] %.6g -> %.6g raised watts %.6g -> %.6g",
                          i, (double)orig, (double)(orig - delta),
                          (double)a.watts, (double)lo.watts);

                ceil_set(&c, i, orig + delta);
                const ctrl_arb_t hi = ctrl_arbitrate(stage, &c);
                prop_note(&mono_up, hi.watts >= a.watts - 1e-3f, NULL,
                          "ceiling[%d] %.6g -> %.6g lowered watts %.6g -> %.6g",
                          i, (double)orig, (double)(orig + delta),
                          (double)a.watts, (double)hi.watts);
            } else if (orig == -INFINITY) {
                /* -inf is the lowest possible ceiling: making it finite is a
                 * RAISE, so watts may only go up. */
                ceil_set(&c, i, delta);
                const ctrl_arb_t hi = ctrl_arbitrate(stage, &c);
                prop_note(&mono_up, hi.watts >= a.watts - 1e-3f, NULL,
                          "ceiling[%d] -inf -> %.6g lowered watts %.6g -> %.6g",
                          i, (double)delta, (double)a.watts, (double)hi.watts);
            } else {
                /* An INACTIVE (+inf) or NaN (never binds) ceiling made finite
                 * can only bind lower. */
                ceil_set(&c, i, delta);
                const ctrl_arb_t lo = ctrl_arbitrate(stage, &c);
                prop_note(&mono_down, lo.watts <= a.watts + 1e-3f, NULL,
                          "ceiling[%d] %.6g -> %.6g raised watts %.6g -> %.6g",
                          i, (double)orig, (double)delta, (double)a.watts, (double)lo.watts);
            }
            ceil_set(&c, i, orig);
        }

        if (isfinite(stage)) {
            const ctrl_arb_t s = ctrl_arbitrate(stage - 250.0f, &c);
            prop_note(&mono_stage, s.watts <= a.watts + 1e-3f, NULL,
                      "stage %.6g -> %.6g raised watts %.6g -> %.6g",
                      (double)stage, (double)(stage - 250.0f), (double)a.watts, (double)s.watts);
        }
    }

    prop_report(&not_nan);
    prop_report(&nonneg);
    prop_report(&le_stage);
    prop_report(&mono_down);
    prop_report(&mono_up);
    prop_report(&src_ok);
    prop_report(&mono_stage);
}

/* =========================================================================== *
 * P4 — dense combinatorial ctrl_tick() sweep
 * =========================================================================== */
static void p4_engine_grid(void)
{
    static const float VCELL[]  = { NAN, -1.0f, 0.0f, 2.50f, 3.10f, 3.30f, 3.60f, 3.69f, 3.75f, 8.0f };
    static const float AMPS[]   = { NAN, -INFINITY, -200.0f, 0.0f, 60.0f, 1.0e6f };
    static const float TEMPS[][3] = {                     /* alt, driver, battery */
        { 60.0f, 50.0f, 25.0f }, { NAN, NAN, NAN }, { 125.0f, 50.0f, 25.0f },
        { 60.0f, 130.0f, 25.0f }, { 60.0f, 50.0f, -5.0f }, { 60.0f, 50.0f, 60.0f }
    };
    static const float RPMS[][1] = { { 1800.0f }, { 0.0f }, { NAN }, { 900.0f }, { -100.0f } };
    static const ctrl_rpm_state_t RPMST[] = {
        CTRL_RPM_VALID, CTRL_RPM_VALID, CTRL_RPM_LOST, CTRL_RPM_STALE, CTRL_RPM_VALID };
    static const ctrl_run_state_t RUNST[] = {
        CTRL_RUN_RUNNING, CTRL_RUN_NOT_RUNNING, CTRL_RUN_NOT_RUNNING,
        CTRL_RUN_NOT_RUNNING, CTRL_RUN_RUNNING };
    static const uint32_t EXTF[] = {
        0u, CTRL_FAULT_FIELD_OPEN, CTRL_FAULT_LOST_BMS, CTRL_FAULT_IMPLAUSIBLE_SHUNT,
        CTRL_FAULT_WATCHDOG, CTRL_FAULT_FIELD_SHORT | CTRL_FAULT_OVERSPEED,
        CTRL_FAULT_THERMAL_DIVERGE };
    static const uint8_t CELLSETS[] = { 16, 4 };
    static const uint32_t DTS[] = { 1u, 10u, 100u, 1000u };

    prop_core_inv_t iv; prop_core_init(&iv);
    prop_inv_t open_cause;
    prop_inv_init(&open_cause, "P4 field_open only in STANDBY");

    uint64_t combos = 0;

    for (int ics = 0; ics < 2; ics++) {
        const uint8_t cells = CELLSETS[ics];
        ctrl_globals_t g = prop_globals(cells);
        ctrl_profile_t p = prop_profile();

        for (int iv_c = 0; iv_c < 10; iv_c++)
        for (int ia = 0; ia < 6; ia++)
        for (int it = 0; it < 6; it++)
        for (int ir = 0; ir < 5; ir++)
        for (int is = 1; is <= 5; is++)
        for (int ig = 0; ig < 2; ig++)
        for (int ie = 0; ie < 7; ie++) {
            combos++;

            ctrl_measured_t m;
            memset(&m, 0, sizeof m);
            m.vbat_pack_v  = VCELL[iv_c] * (float)cells;
            m.vcomp_pack_v = m.vbat_pack_v;
            m.v_supply_v   = m.vbat_pack_v;      /* honest supply sensing */
            m.amps_batt    = AMPS[ia];
            m.watts_batt   = AMPS[ia] * 50.0f;
            m.isrc         = (ctrl_isrc_tier_t)is;
            m.alt_hotspot_c = TEMPS[it][0];
            m.driver_temp_c = TEMPS[it][1];
            m.batt_temp_c   = TEMPS[it][2];
            m.rpm          = RPMS[ir][0];
            m.rpm_state    = RPMST[ir];
            m.run_state    = RUNST[ir];
            m.soc_pct      = -1.0f;
            m.soc_trusted  = false;
            m.ignition     = (ig != 0);
            m.ext_faults   = EXTF[ie];

            /* Ceilings rotate deterministically through four shapes. */
            ctrl_ceilings_t c = test_ceil_none();
            switch (combos & 3u) {
            case 1: c.bms_ccl_w = 500.0f; break;
            case 2: c.thermal_w = 0.0f;   break;
            case 3: c.user_cap_w = -100.0f; c.belt_w = 1500.0f; break;
            default: break;
            }

            /* The TRUE bus for this point; nonsense readings mean the core can
             * only rely on the worst plausible bus (CTRL_VSUP_MAX_VCELL x cells). */
            const float v_ref = (isfinite(m.vbat_pack_v) && m.vbat_pack_v > 0.0f)
                                ? m.vbat_pack_v : CTRL_VSUP_MAX_VCELL * (float)cells;

            prop_ctx_t cx;
            memset(&cx, 0, sizeof cx);
            cx.tag = (cells == 16) ? "P4/16S" : "P4/4S";
            cx.v_true = v_ref; cx.vbat = m.vbat_pack_v; cx.vsup = m.v_supply_v;
            cx.amps = m.amps_batt; cx.watts = m.watts_batt;
            cx.alt_c = m.alt_hotspot_c; cx.drv_c = m.driver_temp_c; cx.batt_c = m.batt_temp_c;
            cx.rpm = m.rpm; cx.isrc = is; cx.rpm_state = (int)m.rpm_state;
            cx.run_state = (int)m.run_state; cx.ignition = ig; cx.ext_faults = m.ext_faults;

            ctrl_t e; ctrl_init(&e);
            for (int t = 0; t < 3; t++) {
                cx.tick = (uint64_t)t;
                const ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, DTS[(combos + (unsigned)t) & 3u]);
                prop_core_check(&iv, &cmd, &g, v_ref, 0.0f, &cx);
                prop_note(&open_cause, !cmd.field_open || cmd.state == CTRL_STANDBY, &cx,
                          "field_open in state %d", (int)cmd.state);
            }
        }
    }

    printf("  [P4] %llu input combinations x 3 ticks\n", (unsigned long long)combos);
    prop_core_report(&iv);
    prop_report(&open_cause);
}

/* =========================================================================== *
 * P5 — fault latching / clearing
 * =========================================================================== */
static void p5_fault_latching(void)
{
    /* Every member of CTRL_FAULT_OPEN_MASK. Kept in step with faults.h by
     * hand — the count below is derived, never literal, because this list
     * lagging the mask is exactly how GH#39's new OPEN bit went unswept. */
    static const uint32_t OPEN_BITS[] = {
        CTRL_FAULT_OVERVOLTAGE, CTRL_FAULT_FIELD_SHORT, CTRL_FAULT_FIELD_OVERCUR,
        CTRL_FAULT_OVERSPEED, CTRL_FAULT_WATCHDOG, CTRL_FAULT_BATT_DTDT,
        CTRL_FAULT_DRIVER_OVERTEMP /* GH#39 */ };
    const int N_OPEN_BITS = (int)(sizeof OPEN_BITS / sizeof OPEN_BITS[0]);
    static const uint32_t CLEARABLE[] = {
        CTRL_FAULT_FIELD_OPEN, CTRL_FAULT_LOST_BMS, CTRL_FAULT_IMPLAUSIBLE_SHUNT,
        CTRL_FAULT_LOST_VBAT_SENSE, CTRL_FAULT_THERMAL_DIVERGE,
        CTRL_FAULT_BATT_LOWTEMP, CTRL_FAULT_BATT_HIGHTEMP };

    prop_inv_t latched, safe_while_latched, cleared, reset_clears;
    prop_inv_init(&latched,            "P5 OPEN-class fault stays latched after the cause clears");
    prop_inv_init(&safe_while_latched, "P5 latched OPEN fault holds field open at duty 0");
    prop_inv_init(&cleared,            "P5 non-OPEN faults clear when their cause clears");
    prop_inv_init(&reset_clears,       "P5 ctrl_init() clears every latch");

    ctrl_globals_t g = prop_globals(16);
    ctrl_profile_t p = prop_profile();
    ctrl_ceilings_t c = test_ceil_none();

    /* Every OPEN bit, injected through ext_faults for one tick, then removed. */
    for (int i = 0; i < N_OPEN_BITS; i++) {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t ok = prop_measured(16, 3.35f);
        ctrl_measured_t bad = ok; bad.ext_faults = OPEN_BITS[i];

        for (int t = 0; t < 5; t++) ctrl_tick(&e, &ok, &c, &p, &g, 10);
        ctrl_tick(&e, &bad, &c, &p, &g, 10);

        ctrl_command_t cmd = ctrl_tick(&e, &ok, &c, &p, &g, 10);
        for (int t = 0; t < 500; t++) cmd = ctrl_tick(&e, &ok, &c, &p, &g, 100);

        prop_note(&latched, (cmd.faults & OPEN_BITS[i]) != 0, NULL,
                  "bit 0x%08lx released after the cause cleared (faults=0x%08lx)",
                  (unsigned long)OPEN_BITS[i], (unsigned long)cmd.faults);
        prop_note(&safe_while_latched,
                  cmd.field_open && cmd.field_duty == 0.0f &&
                  cmd.state == CTRL_STANDBY && cmd.standby_reason == CTRL_SB_FAULT, NULL,
                  "bit 0x%08lx: open=%d duty=%.6g state=%d reason=%d",
                  (unsigned long)OPEN_BITS[i], (int)cmd.field_open, (double)cmd.field_duty,
                  (int)cmd.state, (int)cmd.standby_reason);

        ctrl_init(&e);
        cmd = ctrl_tick(&e, &ok, &c, &p, &g, 10);
        prop_note(&reset_clears, (cmd.faults & OPEN_BITS[i]) == 0, NULL,
                  "bit 0x%08lx survived ctrl_init() (faults=0x%08lx)",
                  (unsigned long)OPEN_BITS[i], (unsigned long)cmd.faults);
    }

    /* Non-OPEN classes must NOT latch. */
    for (int i = 0; i < 7; i++) {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t ok = prop_measured(16, 3.35f);
        ctrl_measured_t bad = ok;
        if (CLEARABLE[i] == CTRL_FAULT_BATT_LOWTEMP)       bad.batt_temp_c = -10.0f;
        else if (CLEARABLE[i] == CTRL_FAULT_BATT_HIGHTEMP) bad.batt_temp_c = 70.0f;
        else if (CLEARABLE[i] == CTRL_FAULT_LOST_VBAT_SENSE) {
            bad.vbat_pack_v = NAN; bad.vcomp_pack_v = NAN;
        } else bad.ext_faults = CLEARABLE[i];

        for (int t = 0; t < 20; t++) ctrl_tick(&e, &bad, &c, &p, &g, 10);
        ctrl_command_t cmd = {0};
        for (int t = 0; t < 20; t++) cmd = ctrl_tick(&e, &ok, &c, &p, &g, 10);
        prop_note(&cleared, (cmd.faults & CLEARABLE[i]) == 0, NULL,
                  "bit 0x%08lx latched but is not OPEN-class (faults=0x%08lx)",
                  (unsigned long)CLEARABLE[i], (unsigned long)cmd.faults);
    }

    prop_report(&latched);
    prop_report(&safe_while_latched);
    prop_report(&cleared);
    prop_report(&reset_clears);

    /* Disposition/severity are total functions over the whole declared fault
     * space (GH#39 added CTRL_FAULT_DRIVER_OVERTEMP at bit 18; GH#40 added
     * CTRL_FAULT_BATT_TEMP_REQUIRED at bit 17). The bound is derived from
     * CTRL_FAULT_ALL_MASK rather than written as a literal width: a hardcoded
     * `1u << 18` silently stopped covering the newest bit the moment one was
     * appended, which is the same rot GH#41's mask-completeness test exists
     * to kill — see faults.h. */
    {
        prop_inv_t disp_total;
        prop_inv_init(&disp_total, "P5 disposition/severity agree over all fault sets");
        /* Exhaustive over 2^n sets, so n cannot grow without bound: 22 bits is
         * ~4.2 M iterations, already the practical ceiling for a CI sweep. If
         * a future fault bit pushes past it, this reports rather than silently
         * under-covering — swap to sampled coverage at that point. */
        uint32_t sweep_bits = 0u;
        while (sweep_bits < 22u && ((uint32_t)CTRL_FAULT_ALL_MASK >> sweep_bits) != 0u) sweep_bits++;
        prop_note(&disp_total, ((uint32_t)CTRL_FAULT_ALL_MASK >> sweep_bits) == 0u, NULL,
                  "CTRL_FAULT_ALL_MASK=0x%08lx exceeds the %u-bit exhaustive sweep",
                  (unsigned long)CTRL_FAULT_ALL_MASK, sweep_bits);
        for (uint32_t f = 0; f < (1u << sweep_bits); f++) {
            const ctrl_disposition_t d = ctrl_fault_disposition(f);
            const ctrl_severity_t    s = ctrl_fault_severity(f);
            const int open_bits  = (f & CTRL_FAULT_OPEN_MASK) != 0;
            const int limp_bits  = (f & CTRL_FAULT_LIMP_MASK) != 0;
            const int ok =
                (open_bits ? (d == CTRL_DISP_OPEN && s == CTRL_SEV_CRITICAL)
                 : limp_bits ? (d == CTRL_DISP_LIMP && s == CTRL_SEV_FAULT)
                 : d == CTRL_DISP_CONTINUE) &&
                (s >= CTRL_SEV_INFO && s <= CTRL_SEV_CRITICAL);
            prop_note(&disp_total, ok, NULL,
                      "faults=0x%08lx -> disposition=%d severity=%d",
                      (unsigned long)f, (int)d, (int)s);
        }
        prop_report(&disp_total);
    }
}

/* =========================================================================== *
 * P6 — PROFILE_SPEC §1/§3 boundary sweeps
 * =========================================================================== */
enum {
    PP_CELLS = 0, PP_BANK_AH, PP_MAXW, PP_RAMP, PP_TVCLAMP, PP_TCHGMAX, PP_WARMUP,
    PP_COOLANT, PP_SOC_TARGET, PP_PTAIL, PP_TTAIL, PP_CVTARGET, PP_RESTV, PP_LIMPV,
    PP_VREVERT, PP_TREVERT, PP_SOCREVERT, PP_AHREVERT, PP_RESTCAP, PP_CVHOLD,
    PP_SKIPBULK, PP_N
};

typedef struct {
    int         id;
    const char *name;
    float       lo, hi;         /* PROFILE_SPEC range */
    float       below, above;   /* just outside it */
    int         spec_ranged;    /* 0 = no numeric range in the spec (see report) */
} prop_param_t;

static const prop_param_t PARAMS[PP_N] = {
    { PP_CELLS,      "cells_series",       4.f,    16.f,    3.f,     17.f,    1 },
    { PP_BANK_AH,    "bank_capacity_ah",   40.f,   3000.f,  39.f,    3001.f,  1 },
    { PP_MAXW,       "max_charge_power_w", 100.f,  20000.f, 99.f,    20001.f, 1 },
    { PP_RAMP,       "ramp_w_per_s",       10.f,   1000.f,  9.f,     1001.f,  1 },
    { PP_TVCLAMP,    "t_vclamp_s",         2.f,    30.f,    1.f,     31.f,    1 },
    { PP_TCHGMAX,    "t_charge_max_min",   60.f,   1440.f,  59.f,    1441.f,  1 },
    { PP_WARMUP,     "warmup_time_s",      0.f,    600.f,   0.f,     601.f,   1 },
    { PP_COOLANT,    "warmup_coolant_c",   20.f,   90.f,    19.f,    91.f,    1 },
    { PP_SOC_TARGET, "soc_target_pct",     70.f,   100.f,   69.f,    101.f,   1 },
    /* §3.2 window is C-relative (0.01C-0.10C at 3.45 V/cell), so these bounds
     * track the baseline bank: 300 Ah x 16S -> 165.6 .. 1656 W. */
    { PP_PTAIL,      "p_tail_w",           165.6f, 1656.f,  165.5f,  1656.1f, 1 },
    { PP_TTAIL,      "t_tail_hold_s",      10.f,   600.f,   9.f,     601.f,   1 },
    { PP_CVTARGET,   "cv_target (v_bulk)", 3.45f,  3.65f,   3.44f,   3.66f,   1 },
    { PP_RESTV,      "rest_voltage (v_float)", 3.33f, 3.45f, 3.32f,  3.46f,   1 },
    { PP_LIMPV,      "limp_vcell (v_limp)", 3.20f, 3.35f,   3.19f,   3.36f,   1 },
    { PP_VREVERT,    "v_revert",           3.20f,  3.40f,   3.19f,   3.41f,   0 },
    { PP_TREVERT,    "t_revert_hold_s",    10.f,   600.f,   9.f,     601.f,   0 },
    { PP_SOCREVERT,  "soc_revert_pct",     0.f,    100.f,   -1.f,    101.f,   0 },
    { PP_AHREVERT,   "ah_revert",          15.f,   300.f,   14.f,    301.f,   0 },
    { PP_RESTCAP,    "rest_power_cap_w",   100.f,  20000.f, 99.f,    20001.f, 0 },
    { PP_CVHOLD,     "cv_hold_exit_min",   0.f,    600.f,   0.f,     601.f,   0 },
    { PP_SKIPBULK,   "skip_bulk_vcell",    3.30f,  3.45f,   3.29f,   3.46f,   0 },
};

/* Unsigned-field setters go through this: converting a negative float straight
 * to an unsigned integer type is undefined behaviour, so negatives are pinned
 * to 0 and reported as "not representable" rather than smuggled in as 65535. */
static uint16_t as_u16(float v)
{
    if (!(v > 0.0f)) return 0u;
    if (v > 65535.0f) return 65535u;
    return (uint16_t)v;
}
static int8_t as_i8(float v)
{
    if (!(v > -128.0f)) return -128;
    if (v > 127.0f) return 127;
    return (int8_t)v;
}

static void set_param(int id, float v, ctrl_globals_t *g, ctrl_profile_t *p)
{
    switch (id) {
    case PP_CELLS:      g->cells_series = (v > 0.0f && v < 255.0f) ? (uint8_t)v : 0u; break;
    case PP_BANK_AH:    g->bank_capacity_ah = v; break;
    case PP_MAXW:       g->max_charge_power_w = v; break;
    case PP_RAMP:       g->ramp_w_per_s = v; break;
    case PP_TVCLAMP:    g->t_vclamp_s = as_u16(v); break;
    case PP_TCHGMAX:    g->t_charge_max_min = as_u16(v); break;
    case PP_WARMUP:     g->warmup_time_s = as_u16(v); break;
    case PP_COOLANT:    g->warmup_coolant_c = v; break;
    case PP_SOC_TARGET: g->soc_target_pct = as_i8(v); break;
    case PP_PTAIL:      g->p_tail_w = v; break;
    case PP_TTAIL:      g->t_tail_hold_s = as_u16(v); break;
    case PP_CVTARGET:   p->cv_target_vcell = v; break;
    case PP_RESTV:      p->rest_voltage_vcell = v; break;
    case PP_LIMPV:      g->limp_vcell = v; break;
    case PP_VREVERT:    p->v_revert_vcell = v; break;
    case PP_TREVERT:    p->t_revert_hold_s = as_u16(v); break;
    case PP_SOCREVERT:  p->soc_revert_pct = as_i8(v); break;
    case PP_AHREVERT:   p->ah_revert = v; break;
    case PP_RESTCAP:    p->rest_power_cap_w = v; break;
    case PP_CVHOLD:     g->cv_hold_exit_min = as_u16(v); break;
    case PP_SKIPBULK:   g->skip_bulk_vcell = v; break;
    default: break;
    }
}

static void p6_profile_bounds(void)
{
    prop_core_inv_t iv; prop_core_init(&iv);
    uint64_t runs = 0;

    /* Every parameter at min / max / just-below / just-above, everything else at
     * the installed-unit default, driven over a full 3.05 -> 3.68 V/cell ramp. */
    for (int ip = 0; ip < PP_N; ip++) {
        const float VALUES[4] = { PARAMS[ip].lo, PARAMS[ip].hi,
                                  PARAMS[ip].below, PARAMS[ip].above };
        for (int iv_i = 0; iv_i < 4; iv_i++) {
            ctrl_globals_t g = prop_globals(16);
            ctrl_profile_t p = prop_profile();
            set_param(PARAMS[ip].id, VALUES[iv_i], &g, &p);

            const uint8_t cells = g.cells_series ? g.cells_series : 16;
            ctrl_ceilings_t c = test_ceil_none();
            ctrl_t e; ctrl_init(&e);
            runs++;

            prop_ctx_t cx; memset(&cx, 0, sizeof cx);
            cx.tag = PARAMS[ip].name;
            cx.seed = (uint32_t)((ip << 4) | iv_i);

            for (int t = 0; t < 400; t++) {
                const float vcell = 3.05f + 0.63f * ((float)t / 400.0f);
                ctrl_measured_t m = prop_measured(cells, vcell);
                m.watts_batt = 2000.0f - 4.0f * (float)t;
                m.amps_batt  = m.watts_batt / m.vbat_pack_v;
                cx.tick = (uint64_t)t;
                cx.v_true = m.vbat_pack_v; cx.vbat = m.vbat_pack_v; cx.vsup = m.v_supply_v;
                cx.watts = m.watts_batt; cx.amps = m.amps_batt; cx.rpm = m.rpm;
                cx.ignition = 1; cx.isrc = (int)m.isrc;
                const ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
                prop_core_check(&iv, &cmd, &g, m.vbat_pack_v, 0.0f, &cx);
            }
        }
    }

    printf("  [P6] %llu boundary configurations x 400 ticks\n", (unsigned long long)runs);
    prop_core_report(&iv);

    /* ---- no silent auto-correction ----------------------------------------
     * PROFILE_SPEC §1: an out-of-range or ordering-violating config "is rejected
     * ... never auto-corrected". The engine's half of that contract is that it
     * uses configured values verbatim — it must not quietly pull them back into
     * range, which would hide the rejection that the (absent, see below) config
     * validator owes the user. */
    {
        /* cv_target 3.00 V/cell is below the §1 v_bulk minimum (3.45). If the
         * core silently raised it to 3.45, effort would climb at 3.20 V/cell;
         * honoring 3.00 means the CV loop drives effort to zero instead. */
        ctrl_globals_t g = prop_globals(16);
        ctrl_profile_t p = prop_profile();
        p.cv_target_vcell = 3.00f;
        ctrl_ceilings_t c = test_ceil_none();
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = prop_measured(16, 3.20f);
        ctrl_command_t cmd = {0};
        for (int t = 0; t < 300; t++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.binding == CTRL_BIND_VOLTAGE_CLAMP);
        CHECK(cmd.field_effort < 0.01f);           /* honored 3.00, not clamped to 3.45 */
    }
    {
        /* max_charge_power_w above the §3 maximum is passed through to the
         * arbitration stage ceiling unchanged. */
        ctrl_ceilings_t none = test_ceil_none();
        const ctrl_arb_t a = ctrl_arbitrate(50000.0f, &none);
        CHECK_FEQ(a.watts, 50000.0f, 1e-3);
        CHECK(a.src == CTRL_BIND_STAGE);
    }
    {
        /* ramp_w_per_s below the §3 minimum (10) is honored, not raised: after
         * 10 s the commanded power is ~10 W, not ~100 W. */
        ctrl_globals_t g = prop_globals(16);
        ctrl_profile_t p = prop_profile();
        g.ramp_w_per_s = 1.0f;
        ctrl_ceilings_t c = test_ceil_none();
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = prop_measured(16, 3.20f);
        for (int t = 0; t < 100; t++) ctrl_tick(&e, &m, &c, &p, &g, 100);   /* 10 s */
        CHECK(e.cmd_power_w > 9.0f && e.cmd_power_w < 11.0f);
    }

    /* The §1/§3 conformance table that used to be a GAP-NOTE here is now driven
     * against the real validator — see p6b_config_conformance() below. */
}

/* =========================================================================== *
 * P6b — the same parameter table, driven against the REAL validator
 * ===========================================================================
 * This section was a printed GAP-NOTE until control/Src/config_validate.c
 * landed: the engine provably neither rejects nor repairs an out-of-range
 * config (P6 above), so the rejection is owed by the config layer, and until
 * that layer existed the rules could only be listed. They are now strict.
 *
 * Same PARAMS table, driven into a full persisted config instead of into
 * (globals, profile) alone, because the validator sees things the engine never
 * does: the §1 primitives block, the primitive REFS the profiles carry, and the
 * hardware limit set.
 */

/* Mirror of set_param(), into the persisted config. */
static void set_cfg_param(int id, float v, ctrl_config_t *c)
{
    ctrl_globals_t *g = &c->globals;
    ctrl_profile_t *p = &c->profiles[0].p;      /* profile 1 — the active one */

    switch (id) {
    /* cells_series and bank_capacity_ah move the C-relative §3.2 tail window
     * with them, so p_tail_w is re-materialized from its 0.04 C default exactly
     * as it would be at config time — otherwise this sweep would be testing
     * p_tail_w, not the parameter it names. */
    case PP_CELLS:
        g->cells_series = (v > 0.0f && v < 255.0f) ? (uint8_t)v : 0u;
        g->p_tail_w = 0.04f * g->bank_capacity_ah * (float)g->cells_series * 3.45f;
        break;
    case PP_BANK_AH:
        g->bank_capacity_ah = v;
        g->p_tail_w = 0.04f * v * (float)g->cells_series * 3.45f;
        break;
    case PP_MAXW:       g->max_charge_power_w = v; break;
    case PP_RAMP:       g->ramp_w_per_s = v; break;
    case PP_TVCLAMP:    g->t_vclamp_s = as_u16(v); break;
    case PP_TCHGMAX:    g->t_charge_max_min = as_u16(v); break;
    case PP_WARMUP:     g->warmup_time_s = as_u16(v); break;
    case PP_COOLANT:    g->warmup_coolant_c = v; break;
    case PP_SOC_TARGET: g->soc_target_pct = as_i8(v); break;
    case PP_PTAIL:      g->p_tail_w = v; break;
    case PP_TTAIL:      g->t_tail_hold_s = as_u16(v); break;
    /* The three voltages are §1 primitives, not free-standing profile fields:
     * setting one re-derives the ladder and re-resolves every profile ref. */
    case PP_CVTARGET:   cfg_test_set_primitive(c, CFG_PRIM_V_BULK,  v); break;
    case PP_RESTV:      cfg_test_set_primitive(c, CFG_PRIM_V_FLOAT, v); break;
    case PP_LIMPV:      cfg_test_set_primitive(c, CFG_PRIM_V_LIMP,  v); break;
    case PP_VREVERT:    p->v_revert_vcell = v; break;
    case PP_TREVERT:    p->t_revert_hold_s = as_u16(v); break;
    case PP_SOCREVERT:  p->soc_revert_pct = as_i8(v); break;
    case PP_AHREVERT:   p->ah_revert = v; break;
    case PP_RESTCAP:    p->rest_power_cap_w = v; break;
    case PP_CVHOLD:     g->cv_hold_exit_min = as_u16(v); break;
    case PP_SKIPBULK:   g->skip_bulk_vcell = v; break;
    default: break;
    }
}

/* One check: set `param` to `v`, expect exactly `want`. */
static void expect_cfg(int param, const char *what, float v, cfg_err_t want)
{
    ctrl_config_t c = cfg_test_baseline();
    set_cfg_param(param, v, &c);
    const cfg_err_t got = ctrl_config_validate(&c, NULL);
    g_checks++;
    if (got != want) {
        g_fails++;
        printf("FAIL %s:%d  %s %s = %.6g: expected %d (%s), got %d (%s)\n",
               __FILE__, __LINE__, PARAMS[param].name, what, (double)v,
               (int)want, cfg_err_str(want), (int)got, cfg_err_str(got));
    }
}

typedef struct {
    int       param;
    cfg_err_t err_below;     /* expected code just BELOW the §3 range */
    cfg_err_t err_above;     /* expected code just ABOVE it */
    int       below_is_out;  /* 0 when PARAMS[].below is not actually outside */
} prop_val_case_t;

/* Every §3-ranged parameter of the table, with the rule it must trip. */
static const prop_val_case_t VAL_CASES[] = {
    { PP_CELLS,      CFG_ERR_RANGE_CELLS,          CFG_ERR_RANGE_CELLS,          1 },
    { PP_BANK_AH,    CFG_ERR_RANGE_BANK_AH,        CFG_ERR_RANGE_BANK_AH,        1 },
    { PP_MAXW,       CFG_ERR_RANGE_MAX_POWER,      CFG_ERR_RANGE_MAX_POWER,      1 },
    { PP_RAMP,       CFG_ERR_RANGE_RAMP,           CFG_ERR_RANGE_RAMP,           1 },
    { PP_TVCLAMP,    CFG_ERR_RANGE_T_VCLAMP,       CFG_ERR_RANGE_T_VCLAMP,       1 },
    { PP_TCHGMAX,    CFG_ERR_RANGE_T_CHARGE_MAX,   CFG_ERR_RANGE_T_CHARGE_MAX,   1 },
    /* warmup_time_s starts at 0, so "just below" is not representable. */
    { PP_WARMUP,     CFG_OK,                       CFG_ERR_RANGE_WARMUP_TIME,    0 },
    { PP_COOLANT,    CFG_ERR_RANGE_WARMUP_COOLANT, CFG_ERR_RANGE_WARMUP_COOLANT, 1 },
    { PP_SOC_TARGET, CFG_ERR_RANGE_SOC_TARGET,     CFG_ERR_RANGE_SOC_TARGET,     1 },
    { PP_PTAIL,      CFG_ERR_RANGE_P_TAIL,         CFG_ERR_RANGE_P_TAIL,         1 },
    { PP_TTAIL,      CFG_ERR_RANGE_T_TAIL_HOLD,    CFG_ERR_RANGE_T_TAIL_HOLD,    1 },
    { PP_CVTARGET,   CFG_ERR_RANGE_V_BULK,         CFG_ERR_RANGE_V_BULK,         1 },
    { PP_RESTV,      CFG_ERR_RANGE_V_FLOAT,        CFG_ERR_RANGE_V_FLOAT,        1 },
    /* v_limp's lower bound IS the §1 hard floor — same number, and the floor is
     * the code the user gets for it. */
    { PP_LIMPV,      CFG_ERR_FLOOR_LIMP,           CFG_ERR_RANGE_V_LIMP,         1 },
};
#define N_VAL_CASES ((int)(sizeof VAL_CASES / sizeof VAL_CASES[0]))

static void p6b_config_conformance(void)
{
    /* ---- the baseline itself is legal, and nothing is repaired ------------- */
    {
        ctrl_config_t c = cfg_test_baseline();
        CHECK(ctrl_config_validate(&c, NULL) == CFG_OK);
    }

    /* ---- §1 cross-primitive guard rails, one violation at a time ----------
     * Each mutation stays inside every OTHER rule's range, so the code returned
     * names the rule under test and nothing else. */
    {
        ctrl_config_t c = cfg_test_baseline();
        c.prim.v_bulk_cons = 3.58f;             /* v_bulk 3.60, gap 0.02 < 0.03 */
        cfg_resolve_primitives(&c);
        CHECK(ctrl_config_validate(&c, NULL) == CFG_ERR_ORDER_BULK_CONS);
    }
    {
        ctrl_config_t c = cfg_test_baseline();
        c.prim.v_bulk_cons = 3.42f;             /* v_float 3.40, gap 0.02 < 0.03 */
        cfg_resolve_primitives(&c);
        CHECK(ctrl_config_validate(&c, NULL) == CFG_ERR_ORDER_CONS_FLOAT);
    }
    {
        ctrl_config_t c = cfg_test_baseline();
        c.prim.v_float_cons = 3.39f;            /* v_float 3.40, gap 0.01 < 0.02 */
        cfg_resolve_primitives(&c);
        CHECK(ctrl_config_validate(&c, NULL) == CFG_ERR_ORDER_FLOAT_FCONS);
    }
    {
        ctrl_config_t c = cfg_test_baseline();
        c.prim.v_limp = 3.34f;                  /* v_float_cons 3.35, gap 0.01 */
        cfg_resolve_primitives(&c);
        CHECK(ctrl_config_validate(&c, NULL) == CFG_ERR_ORDER_FCONS_LIMP);
    }
    {
        ctrl_config_t c = cfg_test_baseline();
        c.prim.v_limp = 3.19f;                  /* below the 3.20 hard floor */
        cfg_resolve_primitives(&c);
        CHECK(ctrl_config_validate(&c, NULL) == CFG_ERR_FLOOR_LIMP);
    }
    /* Exactly-at-the-gap must be ACCEPTED (§1 says ">= 0.03", not "> 0.03") —
     * one gap at a time, since the §1 ranges are too tight to sit all four on
     * their minimum simultaneously. Also exercises CFG_VCELL_EPS: 3.60f - 3.57f
     * is 0.0299997 in float32, and rejecting that would be an auto-correction
     * by arithmetic accident. */
    {
        static const float EXACT[4][4] = {          /* cons, float, fcons, limp */
            { 3.57f, 3.40f, 3.35f, 3.30f },         /* v_bulk - cons  = 0.03 */
            { 3.48f, 3.45f, 3.35f, 3.30f },         /* cons  - float  = 0.03 */
            { 3.50f, 3.40f, 3.38f, 3.30f },         /* float - fcons  = 0.02 */
            { 3.50f, 3.40f, 3.35f, 3.33f },         /* fcons - limp   = 0.02 */
        };
        for (int i = 0; i < 4; i++) {
            ctrl_config_t c = cfg_test_baseline();
            c.prim.v_bulk_cons  = EXACT[i][0];
            c.prim.v_float      = EXACT[i][1];
            c.prim.v_float_cons = EXACT[i][2];
            c.prim.v_limp       = EXACT[i][3];
            cfg_resolve_primitives(&c);
            CHECK(ctrl_config_validate(&c, NULL) == CFG_OK);
        }
    }

    /* ---- §3 ranges: min / max accepted, just-outside rejected by name ------ */
    for (int i = 0; i < N_VAL_CASES; i++) {
        const prop_val_case_t *vc = &VAL_CASES[i];
        const prop_param_t *pp = &PARAMS[vc->param];
        expect_cfg(vc->param, "min",   pp->lo,    CFG_OK);
        expect_cfg(vc->param, "max",   pp->hi,    CFG_OK);
        if (vc->below_is_out)
            expect_cfg(vc->param, "below", pp->below, vc->err_below);
        expect_cfg(vc->param, "above", pp->above, vc->err_above);
    }

    /* ---- the parameters PROFILE_SPEC §3 still gives no range for -----------
     * These are NOT free: the validator holds them to physical sanity. What it
     * cannot do is reject a value that is merely unreasonable, because the spec
     * does not say what reasonable is — so the table below stays as the standing
     * record of the gap, now with the enforced half asserted. */
    {
        static const struct { int param; float insane; cfg_err_t err; } SANITY[] = {
            { PP_VREVERT,   -1.0f, CFG_ERR_SANITY_V_REVERT },
            { PP_AHREVERT,  -1.0f, CFG_ERR_SANITY_AH_REVERT },
            { PP_RESTCAP,   -1.0f, CFG_ERR_SANITY_REST_POWER_CAP },
            { PP_SKIPBULK,  -1.0f, CFG_ERR_SANITY_SKIP_BULK_VCELL },
            { PP_VREVERT,    NAN,  CFG_ERR_SANITY_V_REVERT },
            { PP_AHREVERT,   NAN,  CFG_ERR_SANITY_AH_REVERT },
            { PP_RESTCAP,    NAN,  CFG_ERR_SANITY_REST_POWER_CAP },
            { PP_SKIPBULK,   NAN,  CFG_ERR_SANITY_SKIP_BULK_VCELL },
        };
        int unspecified = 0;
        for (int i = 0; i < PP_N; i++) if (!PARAMS[i].spec_ranged) unspecified++;

        /* Out-of-"range" values for these are accepted, by design: there is no
         * range. soc_revert_pct is the one exception — a percentage above 100
         * is not a spec range, it is arithmetic. */
        for (int i = 0; i < PP_N; i++) {
            if (PARAMS[i].spec_ranged) continue;
            if (PARAMS[i].id == PP_SOCREVERT) continue;
            expect_cfg(PARAMS[i].id, "below-nominal", PARAMS[i].below, CFG_OK);
            expect_cfg(PARAMS[i].id, "above-nominal", PARAMS[i].above, CFG_OK);
        }
        expect_cfg(PP_SOCREVERT, "disabled", -1.0f, CFG_OK);
        expect_cfg(PP_SOCREVERT, "above",   101.0f, CFG_ERR_RANGE_SOC_REVERT);

        for (size_t i = 0; i < sizeof SANITY / sizeof SANITY[0]; i++)
            expect_cfg(SANITY[i].param, "insane", SANITY[i].insane, SANITY[i].err);

        printf("  [P6] §1 orderings + %d of %d swept parameters are enforced by\n"
               "       ctrl_config_validate(); %d still have NO numeric range in\n"
               "       PROFILE_SPEC §3 and carry physical-sanity checks only:\n",
               PP_N - unspecified, (int)PP_N, unspecified);
        for (int i = 0; i < PP_N; i++)
            if (!PARAMS[i].spec_ranged)
                printf("         - %s: no numeric range in PROFILE_SPEC §3\n", PARAMS[i].name);
    }
}

/* =========================================================================== *
 * P7 — ceiling producers feeding arbitration
 * =========================================================================== */
static void p7_ceiling_producers(void)
{
    prop_inv_t lim_ok, th_ok;
    prop_inv_init(&lim_ok, "P7 limits ceilings are >= 0 and never NaN");
    prop_inv_init(&th_ok,  "P7 thermal ceiling stays inside [floor, max] or INACTIVE");

    prop_rng_t r; prop_seed(&r, 0x5EED0007u);

    for (int n = 0; n < 20000; n++) {
        ctrl_limits_t lim;
        lim.battery_c_limit    = prop_chance(&r, 0.2f) ? NAN : prop_between(&r, -1.0f, 5.0f);
        lim.wiring_limit_a     = prop_chance(&r, 0.2f) ? -1.0f : prop_between(&r, 0.0f, 500.0f);
        lim.alternator_limit_a = prop_chance(&r, 0.1f) ? INFINITY : prop_between(&r, 0.0f, 500.0f);

        ctrl_globals_t g = prop_globals(16);
        g.bank_capacity_ah = prop_chance(&r, 0.2f) ? 0.0f : prop_between(&r, 0.0f, 4000.0f);

        const float v = prop_chance(&r, 0.15f)
                        ? (prop_chance(&r, 0.5f) ? NAN : -10.0f)
                        : prop_between(&r, 0.0f, 80.0f);

        ctrl_ceilings_t c = test_ceil_none();
        ctrl_limits_apply(&c, &lim, &g, v);
        for (int i = 0; i < N_CEIL; i++) {
            const float w = ceil_get(&c, i);
            prop_note(&lim_ok, !isnan(w) && w >= 0.0f, NULL,
                      "ceiling[%d]=%.6g from C=%.4g Ah=%.4g wiring=%.4g alt=%.4g V=%.4g",
                      i, (double)w, (double)lim.battery_c_limit, (double)g.bank_capacity_ah,
                      (double)lim.wiring_limit_a, (double)lim.alternator_limit_a, (double)v);
        }
    }

    for (int n = 0; n < 2000; n++) {
        ctrl_thermal_cfg_t cfg;
        cfg.tau_s          = prop_between(&r, 0.0f, 600.0f);
        cfg.target_c       = prop_between(&r, 40.0f, 120.0f);
        cfg.hard_c         = cfg.target_c + prop_between(&r, 0.0f, 40.0f);
        cfg.derate_floor_w = prop_between(&r, 0.0f, 1000.0f);
        cfg.ceiling_max_w  = cfg.derate_floor_w + prop_between(&r, 0.0f, 20000.0f);
        cfg.gain_w_per_c_s = prop_between(&r, 0.0f, 500.0f);

        ctrl_thermal_t th; ctrl_thermal_init(&th, &cfg);
        for (int t = 0; t < 200; t++) {
            float hot;
            if (prop_chance(&r, 0.08f))      hot = NAN;
            else if (prop_chance(&r, 0.05f)) hot = prop_between(&r, -1e5f, 1e5f);
            else                             hot = prop_between(&r, -20.0f, 200.0f);
            const float dt = prop_chance(&r, 0.1f) ? 0.0f : prop_between(&r, 0.001f, 2.0f);
            const float w = ctrl_thermal_update(&th, &cfg, hot, dt);
            const int ok = (w == CTRL_CEILING_INACTIVE)
                           ? isnan(hot)
                           : (!isnan(w) && w >= cfg.derate_floor_w - 1e-3f &&
                              w <= cfg.ceiling_max_w + 1e-3f);
            prop_note(&th_ok, ok, NULL,
                      "hot=%.6g dt=%.4g -> %.6g outside [%.4g, %.4g]",
                      (double)hot, (double)dt, (double)w,
                      (double)cfg.derate_floor_w, (double)cfg.ceiling_max_w);
        }
    }

    prop_report(&lim_ok);
    prop_report(&th_ok);
}

uint64_t g_prop_points = 0;

void test_property(void)
{
    printf("\n-- §8.4 property sweeps --\n");
    p1_field_map();
    p2_vsup_guard();
    p2b_vsup_residuals();
    p3_arbitration();
    p4_engine_grid();
    p5_fault_latching();
    p6_profile_bounds();
    p6b_config_conformance();
    p7_ceiling_producers();
    printf("  [§8.4] sweeps evaluated %llu invariant points\n",
           (unsigned long long)g_prop_points);
}
