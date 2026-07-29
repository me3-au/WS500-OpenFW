/*
 * config_protocol.h — the app's config owner and its USB-CDC surface.
 *
 * Holds the whole PROFILE_SPEC §7 model, loads/persists it through the config
 * store, serves the JSON-lines protocol (control/Inc/config_msg.h) over the
 * cfg_stream.h byte transport, and publishes the RESOLVED slice the engine
 * consumes: globals + active profile + hardware limit set + thermal config.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_PROTOCOL_H
#define WS500_CONFIG_PROTOCOL_H

#include "control.h"
#include "thermal.h"        /* ctrl_thermal_cfg_t */
#include "config_store.h"   /* cfg_store_status_t */
#include "config_validate.h"/* cfg_err_t */

void config_init(void);              /* compiled defaults, then the EEPROM record */

/* Service inbound config traffic: drains cfg_proto_rx() and dispatches complete
 * JSON lines. Bounded work per call; safe to call every main-loop iteration.
 * A config WRITE performed here blocks on the EEPROM (see config_protocol.c). */
void config_poll(void);

/* How the boot-time load went, for the §7 fault/telemetry surface (module #21).
 * CFG_STORE_OK + CFG_OK = the running config came from the EEPROM; anything
 * else means the compiled defaults are in force and why. */
cfg_store_status_t config_store_status(void);
cfg_err_t          config_store_validate_err(void);

/* Snapshot the current resolved config for the engine/app. */
void config_get(ctrl_globals_t *g, ctrl_profile_t *prof);
void config_get_limits(ctrl_limits_t *lim);
void config_get_thermal(ctrl_thermal_cfg_t *th);

/* GH#39: driver-stage (PA3) thermal-governor config — a second, independent
 * ctrl_thermal_cfg_t from config_get_thermal()'s alternator governor. See
 * config_protocol.c's config_init() for the [SPEC-SIGNOFF] constants and why. */
void config_get_driver_thermal(ctrl_thermal_cfg_t *th);

#endif /* WS500_CONFIG_PROTOCOL_H */
