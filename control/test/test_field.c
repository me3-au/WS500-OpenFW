/*
 * test_field.c — CONTROL_SPEC §5.1 dynamic rotor duty clamp + effort mapping.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "field.h"

static ctrl_globals_t g_base(void)
{
    ctrl_globals_t g = {0};
    g.rotor_rated_v = 12.0f;
    g.rotor_v_max = NAN;          /* use rated */
    g.allow_full_field_48v = false;
    return g;
}

void test_field(void)
{
    ctrl_globals_t g = g_base();

    /* 48V bank charging at 57.6V → 12/57.6 ≈ 0.2083 (the whole point of §5.1). */
    CHECK_FEQ(ctrl_duty_max(&g, 57.6f), 12.0f / 57.6f, 0.001f);
    /* 48.0V nominal → exactly 0.25. */
    CHECK_FEQ(ctrl_duty_max(&g, 48.0f), 0.25f, 0.001f);

    /* 12V system: rotor_v ≥ supply → clamp to 1.0 (full authority). */
    CHECK_FEQ(ctrl_duty_max(&g, 12.0f), 1.0f, 0.001f);
    CHECK_FEQ(ctrl_duty_max(&g, 10.0f), 1.0f, 0.001f);

    /* Override ladder: explicit rotor_v_max raises the clamp. */
    g.rotor_v_max = 15.0f;
    CHECK_FEQ(ctrl_duty_max(&g, 60.0f), 0.25f, 0.001f);   /* 15/60 */
    g.rotor_v_max = NAN;

    /* allow_full_field_48v lifts the clamp entirely. */
    g.allow_full_field_48v = true;
    CHECK_FEQ(ctrl_duty_max(&g, 57.6f), 1.0f, 0.001f);
    g.allow_full_field_48v = false;

    /* Fail-safe: bad supply → 0. */
    CHECK_FEQ(ctrl_duty_max(&g, 0.0f), 0.0f, 0.001f);
    CHECK_FEQ(ctrl_duty_max(&g, -5.0f), 0.0f, 0.001f);

    /* [SIL-found 2026-07] NaN supply (lost VBat sense) must also fail safe to
     * 0 — previously returned NaN, which reached the PWM command in LIMP. */
    CHECK_FEQ(ctrl_duty_max(&g, NAN), 0.0f, 0.001f);
    {
        ctrl_globals_t gn = g_base(); gn.rotor_rated_v = NAN;
        CHECK_FEQ(ctrl_duty_max(&gn, 57.6f), 0.0f, 0.001f);
    }

    /* Effort → duty, with clamping of out-of-range effort. */
    CHECK_FEQ(ctrl_effort_to_duty(0.5f, 0.20f), 0.10f, 0.001f);
    CHECK_FEQ(ctrl_effort_to_duty(1.0f, 0.25f), 0.25f, 0.001f);
    CHECK_FEQ(ctrl_effort_to_duty(1.5f, 0.20f), 0.20f, 0.001f);   /* effort clamped to 1 */
    CHECK_FEQ(ctrl_effort_to_duty(-0.5f, 0.20f), 0.0f, 0.001f);

    /* ---- §5.2 run detection (GH#37) ---- */
    {
        ctrl_measured_t m = {0};                  /* zeroed inputs fail safe */
        CHECK(!ctrl_run_detected(&m));
        m.run_state = CTRL_RUN_RUNNING;           /* app probe claim */
        CHECK(ctrl_run_detected(&m));
        m.run_state = CTRL_RUN_NOT_RUNNING;
        m.rpm = 1200.0f; m.rpm_state = CTRL_RPM_VALID;   /* VALID RPM proves rotation */
        CHECK(ctrl_run_detected(&m));
        m.rpm_state = CTRL_RPM_STALE;             /* STALE keeps last-good (§3.1) */
        CHECK(ctrl_run_detected(&m));
        m.rpm_state = CTRL_RPM_LOST;              /* LOST ≠ zero, but not proof either */
        CHECK(!ctrl_run_detected(&m));
        m.rpm_state = CTRL_RPM_VALID; m.rpm = 0.0f;      /* valid zero = not turning */
        CHECK(!ctrl_run_detected(&m));
        m.rpm = NAN;                              /* garbage never counts as running */
        CHECK(!ctrl_run_detected(&m));
    }

    /* ---- §5.1 clamp-supply plausibility guard (GH#37) ---- */
    {
        ctrl_vsup_guard_t fg; ctrl_vsup_guard_init(&fg);
        bool bad;

        /* Seed + steady honest reading passes through. */
        CHECK_FEQ(ctrl_vsup_guard(&fg, 54.4f, 54.4f, 16, 10, &bad), 54.4f, 0.001f);
        CHECK(!bad);

        /* Rise follows instantly (tighter clamp, always safe). */
        CHECK_FEQ(ctrl_vsup_guard(&fg, 57.6f, 57.6f, 16, 10, &bad), 57.6f, 0.001f);
        CHECK(!bad);

        /* In-range false LOW step (57.6 → 49): distrusted — the guard keeps the
         * last-trusted (tighter) level, so the clamp never loosens. */
        float v = ctrl_vsup_guard(&fg, 49.0f, 49.0f, 16, 10, &bad);
        CHECK_FEQ(v, 57.6f, 0.001f);
        CHECK(bad);

        /* Cross-check: only ONE channel lies low → the higher channel wins
         * outright (tightest), no distrust needed for safety. */
        ctrl_vsup_guard_t fg2; ctrl_vsup_guard_init(&fg2);
        CHECK_FEQ(ctrl_vsup_guard(&fg2, 49.0f, 57.6f, 16, 10, &bad), 57.6f, 0.001f);

        /* Small in-band decline (< 4 %) is a plausible sag — followed. */
        ctrl_vsup_guard_t fg3; ctrl_vsup_guard_init(&fg3);
        ctrl_vsup_guard(&fg3, 54.0f, 54.0f, 16, 10, &bad);
        CHECK_FEQ(ctrl_vsup_guard(&fg3, 52.5f, 52.5f, 16, 10, &bad), 52.5f, 0.001f);
        CHECK(!bad);

        /* Sustained genuinely-lower level re-anchors after the distrust hold
         * (WARN visible throughout), so a real sag doesn't over-tighten forever. */
        ctrl_vsup_guard_t fg4; ctrl_vsup_guard_init(&fg4);
        ctrl_vsup_guard(&fg4, 57.6f, 57.6f, 16, 10, &bad);
        for (uint32_t t = 0; t < CTRL_VSUP_DISTRUST_MAX_MS; t += 1000)
            v = ctrl_vsup_guard(&fg4, 50.0f, 50.0f, 16, 1000, &bad);
        CHECK_FEQ(v, 50.0f, 0.001f);              /* accepted after the hold */

        /* Absurdly low reading with no trusted history → WORST CASE: highest
         * plausible bus = tightest clamp (never the loosest). */
        ctrl_vsup_guard_t fg5; ctrl_vsup_guard_init(&fg5);
        v = ctrl_vsup_guard(&fg5, 20.0f, 20.0f, 16, 10, &bad);
        CHECK(bad);
        CHECK_FEQ(v, 4.0f * 16.0f, 0.001f);       /* 64 V ⇒ duty_max 12/64 ≈ 18.8 % */

        /* NaN in → NaN out (duty 0 downstream — existing fail-safe). */
        ctrl_vsup_guard_t fg6; ctrl_vsup_guard_init(&fg6);
        CHECK(isnan(ctrl_vsup_guard(&fg6, NAN, NAN, 16, 10, &bad)));
    }
}
