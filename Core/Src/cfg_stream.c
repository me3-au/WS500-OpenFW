/*
 * cfg_stream.c — byte-stream transport for the config protocol, bound to the
 * USB CDC-ACM port (GH#35, deliverable #20).
 *
 * Contract and the reason this seam exists: cfg_stream.h. This file is the ONE
 * place the protocol meets a real transport, and it is two one-line forwards
 * on purpose — every non-trivial behaviour (rings, flow control, drop
 * accounting, the never-block rules) lives in usb_cdc.c where it can be stated
 * once against the hardware, and nothing above this seam changed when the stub
 * became real (which was the whole test of the split).
 *
 * cfg_proto_tx discards usb_cdc_write()'s partial-accept count deliberately:
 * the header's contract is best-effort ("a reply that does not fit is DROPPED,
 * not spun on"), and usb_cdc_tx_dropped() already keeps the honest ledger.
 *
 * SPDX-License-Identifier: MIT
 */
#include "cfg_stream.h"
#include "usb_cdc.h"

int cfg_proto_rx(uint8_t *buf, int cap)
{
    return usb_cdc_read(buf, cap);
}

void cfg_proto_tx(const uint8_t *buf, int len)
{
    (void)usb_cdc_write(buf, len);
}
