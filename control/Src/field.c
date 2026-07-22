/*
 * field.c — field-effort → duty mapping with rotor clamp (§5.1). PURE.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "field.h"

float ctrl_duty_max(const ctrl_globals_t *g, float v_supply_v)
{
    if (g->allow_full_field_48v) return 1.0f;

    float rotor_v = isnan(g->rotor_v_max) ? g->rotor_rated_v : g->rotor_v_max;
    if (v_supply_v <= 0.0f || rotor_v <= 0.0f) return 0.0f;  /* fail-safe */

    float dm = rotor_v / v_supply_v;
    if (dm < 0.0f) dm = 0.0f;
    if (dm > 1.0f) dm = 1.0f;
    return dm;
}

float ctrl_effort_to_duty(float effort, float duty_max)
{
    if (effort < 0.0f) effort = 0.0f;
    if (effort > 1.0f) effort = 1.0f;
    return effort * duty_max;
}
