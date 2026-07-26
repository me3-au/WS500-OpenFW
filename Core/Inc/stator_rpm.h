/*
 * stator_rpm.h — stator/RPM capture: PA10 EXTI rising edge + TIM2 free-running
 * 32-bit timebase (~979.6 kHz, 1.02 us/tick). Software CNT-diffing per edge,
 * NOT TIM2 input-capture (PROJECT_PLAN §0.6 V1+V2 — corrects the earlier
 * "TIM2 input capture" description; see board.h STATOR_* defines).
 *
 * Driver layer only: exposes stator frequency [Hz], RPM (via a configurable
 * pole-count/pulley-ratio, placeholders pending the config schema, GH#28),
 * and a driver-local tri-state signal-quality flag. Deliberately NOT the
 * control core's ctrl_rpm_state_t (control/ stays HAL-free and driver-type-
 * agnostic, the same separation sensors.h already keeps from control.h) —
 * main.c maps stator_rpm_state_t -> ctrl_rpm_state_t when assembling
 * ctrl_measured_t, and additionally gates VALID on stator_rpm_rpm() being a
 * real number (NAN whenever poles/pulley_ratio aren't configured yet), so an
 * unconfigured ratio can never masquerade as a trustworthy RPM reading.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_STATOR_RPM_H
#define WS500_STATOR_RPM_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    STATOR_RPM_FRESH = 0,  /* a plausible edge was accepted recently */
    STATOR_RPM_STALE,      /* no accepted edge for a while; last reading held, suspect */
    STATOR_RPM_LOST        /* no accepted edge for much longer; treat as absent */
} stator_rpm_state_t;

/* poles/pulley_ratio: PLACEHOLDERS, not WS500-specific verified facts (the
 * config schema that will supply real values is GH#28, pending). 0 means
 * "not yet configured" -> stator_rpm_rpm() returns NAN until
 * stator_rpm_set_config() is called with real values; stator_rpm_freq_hz()
 * needs neither and works as soon as edges are seen. */
typedef struct {
    uint8_t poles;         /* alternator pole count; 0 = unconfigured */
    float   pulley_ratio;  /* engine:alternator pulley ratio; 0 = unconfigured */
} stator_rpm_cfg_t;

void stator_rpm_init(void);                              /* arms PA10 EXTI + TIM2 timebase */
void stator_rpm_set_config(const stator_rpm_cfg_t *cfg);  /* from config_protocol once GH#28 lands */
void stator_rpm_update(void);                             /* call once per main-loop tick (10 ms) */

float               stator_rpm_freq_hz(void);  /* stator edge frequency [Hz]; NAN if not FRESH */
float               stator_rpm_rpm(void);      /* f*60/(poles/2*pulley_ratio); NAN if unavailable */
stator_rpm_state_t  stator_rpm_state(void);

#endif /* WS500_STATOR_RPM_H */
