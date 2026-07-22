/*
 * config_protocol.h — config transport + resolution over USB CDC (stub).
 * Produces the RESOLVED control config (globals + active profile) the engine
 * consumes. The full profile schema (PROFILE_SPEC §7) parse/store lands here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_CONFIG_PROTOCOL_H
#define WS500_CONFIG_PROTOCOL_H

#include "control.h"

void config_init(void);              /* load persisted config (flash) + USB CDC */
void config_poll(void);              /* service inbound config traffic */

/* Snapshot the current resolved globals + active profile for the engine. */
void config_get(ctrl_globals_t *g, ctrl_profile_t *prof);

#endif /* WS500_CONFIG_PROTOCOL_H */
