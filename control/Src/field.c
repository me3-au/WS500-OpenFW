/*
 * field.c — field-effort → duty mapping with rotor clamp (§5.1), run-detect
 * gate (§5.2) and clamp-supply plausibility guard. PURE.
 * SPDX-License-Identifier: MIT
 */
#include "field.h"

float ctrl_duty_max(const ctrl_globals_t *g, float v_supply_v)
{
    if (g->allow_full_field_48v) return 1.0f;

    float rotor_v = isnan(g->rotor_v_max) ? g->rotor_rated_v : g->rotor_v_max;
    /* [SIL-found 2026-07] Negated-positive test so NaN (lost VBat/supply sense)
     * also fails safe to 0 — `v <= 0` is false for NaN and produced a NaN duty. */
    if (!(v_supply_v > 0.0f) || !(rotor_v > 0.0f)) return 0.0f;  /* fail-safe */

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

bool ctrl_run_detected(const ctrl_measured_t *m)
{
    /* App-side probe machinery (§5.2 source 2) says the machine is spinning. */
    if (m->run_state == CTRL_RUN_RUNNING) return true;
    /* A non-LOST fused RPM > 0 also proves rotation (§5.2 source 1; STALE keeps
     * the last-good value — demotion needs LOST, "RPM=0 is never inferred from
     * silence", §3.1). Everything else fails safe to NOT-RUNNING. */
    return m->rpm_state != CTRL_RPM_LOST && isfinite(m->rpm) && m->rpm > 0.0f;
}

void ctrl_vsup_guard_init(ctrl_vsup_guard_t *fg)
{
    fg->v_track = NAN;
    fg->distrust_ms = 0;
}

float ctrl_vsup_guard(ctrl_vsup_guard_t *fg, float v_supply_v, float vbat_pack_v,
                      uint8_t cells, uint32_t dt_ms, bool *implausible)
{
    *implausible = false;

    /* Cross-check: take the HIGHER of the two supply channels — can only make
     * the clamp tighter, so a false-low on one channel never loosens it. */
    float v = v_supply_v;
    if (isfinite(vbat_pack_v) && (!isfinite(v) || vbat_pack_v > v)) v = vbat_pack_v;
    if (!isfinite(v)) return NAN;                 /* no reading → duty 0 downstream */

    /* Plausibility band from the confirmed series cell count; unconfigured
     * assumes 16S — the tightest (most conservative) band. */
    const float c    = cells ? (float)cells : 16.0f;
    const float v_hi = CTRL_VSUP_MAX_VCELL * c;
    const float v_lo = CTRL_VSUP_MIN_VCELL * c;

    /* Absurdly low reading: worst-case fallback — last-trusted level if we have
     * one, else the highest plausible bus (tightest possible clamp). */
    if (v < v_lo) {
        *implausible = true;
        return (fg->v_track >= v_lo) ? fg->v_track : v_hi;   /* NaN track → v_hi */
    }

    if (isnan(fg->v_track)) fg->v_track = (v < v_hi) ? v : v_hi;  /* seed */

    if (v >= fg->v_track * (1.0f - CTRL_VSUP_STEP_FRAC)) {
        /* Rise or small in-band fall: plausible — follow (rises tighten
         * immediately; the track saturates at v_hi so a one-tick high glitch
         * can't pin the clamp absurdly tight forever). */
        fg->v_track = (v < v_hi) ? v : v_hi;
        fg->distrust_ms = 0;
    } else {
        /* Reading stepped implausibly far BELOW trust: if false, using it would
         * loosen the clamp onto the true higher voltage — hold the tighter
         * last-trusted level. A genuinely lower bus re-anchors after the
         * distrust hold (WARN visible throughout), or sooner if the reading
         * recovers into band. */
        *implausible = true;
        fg->distrust_ms += dt_ms;
        if (fg->distrust_ms >= CTRL_VSUP_DISTRUST_MAX_MS) {
            fg->v_track = v;
            fg->distrust_ms = 0;
        }
    }

    return (v > fg->v_track) ? v : fg->v_track;   /* the tighter of the two */
}
