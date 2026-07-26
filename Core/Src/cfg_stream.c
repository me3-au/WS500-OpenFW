/*
 * cfg_stream.c — byte-stream transport for the config protocol (STUB).
 *
 * Contract and the reason this seam exists: cfg_stream.h.
 *
 * TODO(GH#35): there is no USB device stack in this tree, so this file is a
 * deliberate no-op — rx reports "nothing received", tx discards. That keeps the
 * firmware linking and keeps config_protocol.c's pump on the real code path
 * (it runs every loop iteration and simply never sees a line), so the day the
 * CDC class lands the only file that changes is this one.
 *
 * Deliberately NOT stubbed as a loopback: a loopback would make the regulator
 * answer its own replies as if they were requests.
 *
 * SPDX-License-Identifier: MIT
 */
#include "cfg_stream.h"

int cfg_proto_rx(uint8_t *buf, int cap)
{
    (void)buf;
    (void)cap;
    return 0;
}

void cfg_proto_tx(const uint8_t *buf, int len)
{
    (void)buf;
    (void)len;
}
