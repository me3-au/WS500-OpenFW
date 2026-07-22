/*
 * thermal.c — predictive thermal governor (§4.1). PURE.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "thermal.h"

void ctrl_thermal_init(ctrl_thermal_t *th, const ctrl_thermal_cfg_t *cfg)
{
    th->t_prev = NAN;
    th->ceiling_w = cfg->ceiling_max_w;
}

float ctrl_thermal_update(ctrl_thermal_t *th, const ctrl_thermal_cfg_t *cfg,
                          float hotspot_c, float dt_s)
{
    if (isnan(hotspot_c)) {           /* no sensor → no thermal ceiling here */
        th->t_prev = NAN;
        return CTRL_CEILING_INACTIVE;
    }

    /* dT/dt from the previous sample. */
    float dTdt = 0.0f;
    if (!isnan(th->t_prev) && dt_s > 0.0f) dTdt = (hotspot_c - th->t_prev) / dt_s;
    th->t_prev = hotspot_c;

    /* Projected steady-state temperature and its error vs target. */
    const float t_proj = hotspot_c + cfg->tau_s * dTdt;
    const float err = cfg->target_c - t_proj;      /* >0 = headroom, <0 = overshooting */

    /* Integrate the ceiling toward holding T_proj == target (smooth, no steps). */
    th->ceiling_w += cfg->gain_w_per_c_s * err * dt_s;

    /* Hard limit → straight to floor. */
    if (hotspot_c >= cfg->hard_c) th->ceiling_w = cfg->derate_floor_w;

    if (th->ceiling_w < cfg->derate_floor_w) th->ceiling_w = cfg->derate_floor_w;
    if (th->ceiling_w > cfg->ceiling_max_w)  th->ceiling_w = cfg->ceiling_max_w;
    return th->ceiling_w;
}
