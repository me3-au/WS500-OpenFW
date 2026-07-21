/*
 * can_n2k.c — stub. Bring in ttlappalainen/NMEA2000 (MIT) + an STM32 bxCAN driver
 * shim; map documented PGNs (127488 engine rapid, 127508 DC status, 61444 engine
 * RPM in, 61443 engine load) per the public Comms & Config Guide.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "can_n2k.h"

void can_n2k_init(void) { /* TODO: bxCAN init @ configured bitrate + N2K device. */ }
void can_n2k_poll(void) { /* TODO: NMEA2000.ParseMessages() pump. */ }
void can_n2k_publish(const reg_inputs_t *in, const reg_state_t *reg)
{
    (void)in; (void)reg; /* TODO: transmit status PGNs. */
}
