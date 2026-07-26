/*
 * cfg_stream.h — byte-stream transport for the config protocol.
 *
 * The seam between the pure JSON-lines protocol (control/Src/config_msg.c) and
 * whatever actually moves bytes. It is exactly two functions on purpose: the
 * protocol layer is a line assembler over an opaque octet stream, so it does
 * not care whether those octets came from USB CDC, a UART, or a test harness —
 * and control/ stays HAL-free because the ONLY file that will ever include a
 * USB header is the one implementing these.
 *
 * Bound since GH#35 to the USB CDC-ACM port (usb_cdc.h — PA11/PA12, the same
 * interface the stock firmware enumerates on). The binding changed nothing
 * above this header, which was the whole test of the split; usb_cdc.c owns the
 * rings, flow control and never-block rules, cfg_stream.c just forwards.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CFG_STREAM_H
#define WS500_CFG_STREAM_H

#include <stdint.h>

/* Drain up to `cap` received bytes into `buf`. Returns the count, 0 when the
 * host has sent nothing. MUST NOT BLOCK: it is called from the main loop
 * alongside the 10 ms control tick. */
int cfg_proto_rx(uint8_t *buf, int cap);

/* Queue `len` bytes toward the host. Best-effort and non-blocking: a reply that
 * does not fit the transport's buffer is DROPPED, not spun on. A wedged or
 * absent client must never be able to stall the control loop, and the client's
 * own recovery is to ask again. */
void cfg_proto_tx(const uint8_t *buf, int len);

#endif /* WS500_CFG_STREAM_H */
