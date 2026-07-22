/*
 * config_protocol.h — config transport + resolution over USB CDC (stub).
 * Produces the RESOLVED control config (globals + active profile + hardware
 * limit set + thermal governor config) the engine/app consume. Full profile
 * schema (PROFILE_SPEC §7) parse/store lands here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_CONFIG_PROTOCOL_H
#define WS500_CONFIG_PROTOCOL_H

#include "control.h"
#include "thermal.h"   /* ctrl_thermal_cfg_t */

void config_init(void);              /* load persisted config (flash) + USB CDC */
void config_poll(void);              /* service inbound config traffic */

/* Snapshot the current resolved config for the engine/app. */
void config_get(ctrl_globals_t *g, ctrl_profile_t *prof);
void config_get_limits(ctrl_limits_t *lim);
void config_get_thermal(ctrl_thermal_cfg_t *th);

#endif /* WS500_CONFIG_PROTOCOL_H */
