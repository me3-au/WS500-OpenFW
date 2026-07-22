/*
 * thermal.h — predictive thermal governor (CONTROL_SPEC §4). PURE.
 * Regulates where temperature is *heading* (T + τ·dT/dt), not just where it is,
 * emitting a continuous power ceiling into arbitration. No step derates.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_THERMAL_H
#define WS500_THERMAL_H

#include "control.h"

typedef struct {
    float tau_s;          /* thermal time constant (seeded by mount; learned) */
    float target_c;       /* hot-spot target */
    float hard_c;         /* hard limit → pull straight to floor (raw fault elsewhere) */
    float derate_floor_w; /* loop never pulls below this (§4.2) */
    float ceiling_max_w;  /* upper bound of the governor ceiling */
    float gain_w_per_c_s; /* integral gain: W per °C of projection error per second */
} ctrl_thermal_cfg_t;

typedef struct {
    float t_prev;     /* previous hot-spot temp for dT/dt; NAN = unseeded */
    float ceiling_w;  /* current governor output */
} ctrl_thermal_t;

void ctrl_thermal_init(ctrl_thermal_t *th, const ctrl_thermal_cfg_t *cfg);

/* Advance and return the thermal power ceiling. hot-spot NaN → INACTIVE (sensor
 * policy handled by the app), and the dT/dt history is reset. */
float ctrl_thermal_update(ctrl_thermal_t *th, const ctrl_thermal_cfg_t *cfg,
                          float hotspot_c, float dt_s);

#endif /* WS500_THERMAL_H */
