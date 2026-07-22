/*
 * limits.h — hardware limit set → Watts ceilings (CONTROL_SPEC §2.1). PURE.
 * Converts the static native-unit ratings to power ceilings at the present
 * voltage and writes them into the arbitration ceilings struct.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_LIMITS_H
#define WS500_LIMITS_H

#include "control.h"

/* Set c->battery_c_w / wiring_w / alt_absolute_w from lim + config at vbat_pack_v.
 * Unset (<=0) ratings and a non-positive voltage yield INACTIVE (+inf). Other
 * ceilings in `c` are left untouched. */
void ctrl_limits_apply(ctrl_ceilings_t *c, const ctrl_limits_t *lim,
                       const ctrl_globals_t *g, float vbat_pack_v);

#endif /* WS500_LIMITS_H */
