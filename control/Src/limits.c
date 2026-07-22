/*
 * limits.c — hardware limit set → Watts ceilings (§2.1). PURE.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "limits.h"

void ctrl_limits_apply(ctrl_ceilings_t *c, const ctrl_limits_t *lim,
                       const ctrl_globals_t *g, float vbat_pack_v)
{
    const bool v_ok = (vbat_pack_v > 0.0f);

    /* Battery C-rate: A = C × bank_Ah; W = A × V. */
    c->battery_c_w = (v_ok && lim->battery_c_limit > 0.0f && g->bank_capacity_ah > 0.0f)
        ? lim->battery_c_limit * g->bank_capacity_ah * vbat_pack_v
        : CTRL_CEILING_INACTIVE;

    /* Wiring ampacity: W = A × V. */
    c->wiring_w = (v_ok && lim->wiring_limit_a > 0.0f)
        ? lim->wiring_limit_a * vbat_pack_v
        : CTRL_CEILING_INACTIVE;

    /* Alternator absolute cap: W = A × V. */
    c->alt_absolute_w = (v_ok && lim->alternator_limit_a > 0.0f)
        ? lim->alternator_limit_a * vbat_pack_v
        : CTRL_CEILING_INACTIVE;
}
