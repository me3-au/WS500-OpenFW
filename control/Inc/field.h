/*
 * field.h — field-effort mapping + rotor protection (CONTROL_SPEC §5.1). PURE.
 * The inner loop commands NORMALIZED effort e∈[0,1]; the dynamic duty clamp
 * translates it to a safe absolute PWM duty for the 12V-rotor-on-48V case.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_FIELD_H
#define WS500_FIELD_H

#include "control.h"

/* Dynamic rotor-protection clamp: duty_max = rotor_v / v_supply, clamped [0,1].
 * rotor_v = rotor_v_max if set (override ladder) else rotor_rated_v.
 * allow_full_field_48v lifts the clamp to 1.0. v_supply<=0 or rotor_v<=0 → 0. */
float ctrl_duty_max(const ctrl_globals_t *g, float v_supply_v);

/* Absolute PWM duty for a commanded effort, effort clamped to [0,1] first. */
float ctrl_effort_to_duty(float effort, float duty_max);

#endif /* WS500_FIELD_H */
