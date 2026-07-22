/*
 * test_field.c — CONTROL_SPEC §5.1 dynamic rotor duty clamp + effort mapping.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

    /* Effort → duty, with clamping of out-of-range effort. */
    CHECK_FEQ(ctrl_effort_to_duty(0.5f, 0.20f), 0.10f, 0.001f);
    CHECK_FEQ(ctrl_effort_to_duty(1.0f, 0.25f), 0.25f, 0.001f);
    CHECK_FEQ(ctrl_effort_to_duty(1.5f, 0.20f), 0.20f, 0.001f);   /* effort clamped to 1 */
    CHECK_FEQ(ctrl_effort_to_duty(-0.5f, 0.20f), 0.0f, 0.001f);
}
