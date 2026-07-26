/*
 * field.h — field-effort mapping + rotor protection (CONTROL_SPEC §5.1/§5.2). PURE.
 * The inner loop commands NORMALIZED effort e∈[0,1]; the dynamic duty clamp
 * translates it to a safe absolute PWM duty for the 12V-rotor-on-48V case.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_FIELD_H
#define WS500_FIELD_H

#include "control.h"

/* ---- §5.2 stationary-rotor detect budget ----------------------------------- *
 * A non-spinning rotor has no fan/airflow; while NOT-RUNNING the field is held
 * to a small pulse-cycled excitation (doubles as the run-detect probe, §3.1).
 * Spec gives "~5 % of duty_max" as a placeholder; the pulse timing below is
 * aligned to the §3.1 probe cadence (≤50 ms pulses every 250–1000 ms).
 * [SPEC-SIGNOFF] all three values are conservative placeholders pending review. */
#define CTRL_RUN_DETECT_EFFORT     0.05f   /* ≤5 % of duty_max while not running */
#define CTRL_RUN_PROBE_PERIOD_MS   500u    /* probe cycle */
#define CTRL_RUN_PROBE_ON_MS       50u     /* bounded on-time (10 % ratio) */

/* ---- §5.1 clamp-supply plausibility guard ---------------------------------- *
 * duty_max = rotor_v / V_supply: a false-LOW (but in-range) reading loosens the
 * clamp onto the true higher bus voltage. The guard vets the voltage the clamp
 * uses: rises are followed instantly (tighter clamp), implausible drops keep
 * the last-trusted (tighter) level, and out-of-band lows fall back to the
 * WORST CASE (highest plausible bus ⇒ tightest clamp) — never the loosest.
 * [SPEC-SIGNOFF] band/step/re-anchor values are engineering choices, not spec'd. */
#define CTRL_VSUP_MAX_VCELL        4.0f    /* highest plausible live bus (V/cell) */
#define CTRL_VSUP_MIN_VCELL        2.0f    /* below this the reading is nonsense */
#define CTRL_VSUP_FLOOR_VCELL      3.2f    /* clamp-voltage hard floor (V/cell).
                                            * §8.4 found a lie drifting slower than
                                            * CTRL_VSUP_STEP_FRAC/tick walks the clamp
                                            * loose (rotor at 1.8x rated). The floor —
                                            * PROFILE_SPEC §1's v_limp hard minimum, no
                                            * charging bus sits below it — bounds ANY
                                            * false-low's rotor exposure to
                                            * rotor_v x V_true/(3.2 x cells) ≈ 1.13x
                                            * rated worst-case on this install.
                                            * [SPEC-SIGNOFF] */
#define CTRL_VSUP_STEP_FRAC        0.04f   /* reading > 4 % below track → distrust */
#define CTRL_VSUP_DISTRUST_MAX_MS  300000u /* sustained low level re-anchors after 5 min
                                            * (WARN telemetered the whole time) */

/* Dynamic rotor-protection clamp: duty_max = rotor_v / v_supply, clamped [0,1].
 * rotor_v = rotor_v_max if set (override ladder) else rotor_rated_v.
 * allow_full_field_48v lifts the clamp to 1.0. v_supply<=0 or rotor_v<=0 → 0. */
float ctrl_duty_max(const ctrl_globals_t *g, float v_supply_v);

/* Absolute PWM duty for a commanded effort, effort clamped to [0,1] first. */
float ctrl_effort_to_duty(float effort, float duty_max);

/* §5.2 run detection from the core's inputs, priority per spec: a VALID fused
 * RPM > 0 proves rotation (CAN or gated stator); otherwise the app's
 * probe-driven run_state claim. Zeroed inputs fail safe to NOT-RUNNING. */
bool ctrl_run_detected(const ctrl_measured_t *m);

void ctrl_vsup_guard_init(ctrl_vsup_guard_t *fg);

/* Vet the voltage the duty clamp is computed from. Takes both supply channels
 * the core receives (uses the HIGHER — a cross-check that can only tighten),
 * returns the clamp voltage; NAN in → NAN out (→ duty 0 downstream, fail-safe).
 * Sets *implausible while the raw reading is being distrusted (WARN). */
float ctrl_vsup_guard(ctrl_vsup_guard_t *fg, float v_supply_v, float vbat_pack_v,
                      uint8_t cells, uint32_t dt_ms, bool *implausible);

#endif /* WS500_FIELD_H */
