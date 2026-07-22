/*
 * can_n2k.c — stub. Bring in ttlappalainen/NMEA2000 (MIT) + an STM32 bxCAN driver
 * shim; map documented PGNs (127508 DC status, 127488 engine rapid, 127489
 * coolant, DVCC/BMS ceilings in) per CONTROL_SPEC §8.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "can_n2k.h"

void can_n2k_init(void) { /* TODO: bxCAN init @ configured bitrate + N2K device. */ }
void can_n2k_poll(void) { /* TODO: NMEA2000.ParseMessages() pump; ingest BMS/DVCC/RPM. */ }
void can_n2k_publish(const ctrl_measured_t *m, const ctrl_t *c)
{
    (void)m; (void)c; /* TODO: transmit status PGNs (W/A/V, state, binding ceiling). */
}
