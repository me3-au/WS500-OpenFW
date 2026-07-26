/*
 * config_protocol.h — config transport + resolution over USB CDC (stub).
 * Produces the RESOLVED control config (globals + active profile + hardware
 * limit set + thermal governor config) the engine/app consume. Full profile
 * schema (PROFILE_SPEC §7) parse/store lands here.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_PROTOCOL_H
#define WS500_CONFIG_PROTOCOL_H

#include "control.h"
#include "thermal.h"        /* ctrl_thermal_cfg_t */
#include "config_store.h"   /* cfg_store_status_t */
#include "config_validate.h"/* cfg_err_t */

void config_init(void);              /* compiled defaults, then the EEPROM record */
void config_poll(void);              /* service inbound config traffic */

/* How the boot-time load went, for the §7 fault/telemetry surface (module #21).
 * CFG_STORE_OK + CFG_OK = the running config came from the EEPROM; anything
 * else means the compiled defaults are in force and why. */
cfg_store_status_t config_store_status(void);
cfg_err_t          config_store_validate_err(void);

/* Snapshot the current resolved config for the engine/app. */
void config_get(ctrl_globals_t *g, ctrl_profile_t *prof);
void config_get_limits(ctrl_limits_t *lim);
void config_get_thermal(ctrl_thermal_cfg_t *th);

#endif /* WS500_CONFIG_PROTOCOL_H */
