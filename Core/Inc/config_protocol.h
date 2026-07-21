/*
 * config_protocol.h — "$XXX:" / "$CPx:n" text config over USB CDC (stub).
 * Protocol is PUBLIC (Wakespeed Comms & Config Guide v2.6.1; field semantics in
 * the WS500 Util ws_schema.json). Implement parse/store/apply here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_CONFIG_PROTOCOL_H
#define WS500_CONFIG_PROTOCOL_H

#include "regulator.h"

void config_init(void);              /* load persisted config (flash) + USB CDC */
void config_poll(void);              /* service inbound "$..." lines */
void config_get(reg_config_t *out);  /* current regulator config snapshot */

#endif /* WS500_CONFIG_PROTOCOL_H */
