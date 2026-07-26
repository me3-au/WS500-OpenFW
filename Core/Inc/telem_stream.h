/*
 * telem_stream.h — the 1 Hz {"t":"telem"} stream over the USB CDC port
 * (GH#35, deliverable #20; PROJECT_PLAN §7 R6 surface hookup).
 *
 * Core-side pump for the pure emitter in control/Inc/telemetry_json.h: latches
 * the control tick's ctrl_telemetry_t, gathers the §7 diagnostics (crash
 * record, image CRC, stack watermark, err budgets, safe-state count) into the
 * pure layer's plain-number block, and streams one line per second through
 * cfg_proto_tx — but ONLY while a host is actually attached
 * (usb_cdc_link_up(): configured + DTR), so an unplugged port costs nothing
 * and a wedged one drops bytes instead of accumulating them.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TELEM_STREAM_H
#define WS500_TELEM_STREAM_H

#include "telemetry.h"   /* ctrl_telemetry_t */

/* Latch the newest snapshot; called once per 10 ms control tick from main.c.
 * A plain struct copy — the JSON work happens at most once a second. */
void telem_stream_update(const ctrl_telemetry_t *t);

/* Emit one line if the 1 Hz period elapsed AND the CDC link is up. Called
 * every main-loop iteration next to config_poll(); bounded work (one JSON
 * emission worst case), never blocks. */
void telem_stream_poll(void);

/* Emit one line NOW, unconditionally — the {"t":"telem-get"} handler
 * (config_protocol.c binds it as the cfg_msg telem hook). Works with the 1 Hz
 * stream gated off: the request arriving is itself proof somebody is listening.
 * Before the first control tick the measured fields read as zero — the window
 * is under one tick and honest zeros beat a stale invention. */
void telem_stream_send_now(void);

#endif /* WS500_TELEM_STREAM_H */
