/*
 * arbitration.h — power arbitration (CONTROL_SPEC §2). PURE.
 * Commanded power = min() of the profile stage ceiling and every active ceiling.
 * The binding source is returned so it can be telemetered.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_ARBITRATION_H
#define WS500_ARBITRATION_H

#include "control.h"

typedef struct {
    float           watts;   /* the winning (minimum) ceiling, ≥ 0 */
    ctrl_bind_src_t src;     /* which ceiling bound */
} ctrl_arb_t;

/* stage_w = the profile's contribution (max_charge_power_w in BULK, rest cap in
 * FLOAT, 0 in STANDBY). INACTIVE (+inf) and NaN ceilings never bind. */
ctrl_arb_t ctrl_arbitrate(float stage_w, const ctrl_ceilings_t *c);

#endif /* WS500_ARBITRATION_H */
