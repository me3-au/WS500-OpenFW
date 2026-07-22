/*
 * arbitration.c — min() of active power ceilings (CONTROL_SPEC §2). PURE.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "arbitration.h"

ctrl_arb_t ctrl_arbitrate(float stage_w, const ctrl_ceilings_t *c)
{
    ctrl_arb_t r = { stage_w, CTRL_BIND_STAGE };

    /* A ceiling binds only if it is strictly lower than the current winner.
     * `x < r.watts` is false for +inf and for NaN, so inactive/garbage ceilings
     * are ignored by construction. */
    #define CONSIDER(w, s)  do { if ((w) < r.watts) { r.watts = (w); r.src = (s); } } while (0)
    CONSIDER(c->thermal_w,        CTRL_BIND_THERMAL);
    CONSIDER(c->bms_ccl_w,        CTRL_BIND_BMS);
    CONSIDER(c->battery_c_w,      CTRL_BIND_BATTERY_C);
    CONSIDER(c->wiring_w,         CTRL_BIND_WIRING);
    CONSIDER(c->alt_absolute_w,   CTRL_BIND_ALT_ABSOLUTE);
    CONSIDER(c->alt_capability_w, CTRL_BIND_ALT_CAPABILITY);
    CONSIDER(c->belt_w,           CTRL_BIND_BELT);
    CONSIDER(c->engine_w,         CTRL_BIND_ENGINE);
    CONSIDER(c->user_cap_w,       CTRL_BIND_USER_CAP);
    #undef CONSIDER

    if (!(r.watts >= 0.0f)) r.watts = 0.0f;  /* never command negative (also catches NaN stage) */
    return r;
}
