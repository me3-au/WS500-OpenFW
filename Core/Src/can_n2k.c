/*
 * can_n2k.c — stub. Near-term: NMEA2000 Tx telemetry → Victron Cerbo GX.
 *
 * Plan: bring up bxCAN @ 250 kbps, init the MIT ttlappalainen/NMEA2000 library as
 * an alternator/charger device (product info 126996 for announce), and broadcast
 * from the telemetry snapshot:
 *   127508 (battery: V/A/T), 127506 (DC detailed), 127750 (charger status),
 *   126983/126985 (alerts from faults), proprietary fast-packet (field effort,
 *   binding ceiling, active profile, RPM source/state).
 * Later: RV-C encoder (CHARGER_STATUS / DC_SOURCE_STATUS + RBM election) reading
 * the SAME snapshot; then Rx (DVCC/BMS ceilings into arbitration).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "can_n2k.h"

void can_n2k_init(void) { /* TODO: bxCAN init + NMEA2000 device announce. */ }
void can_n2k_poll(void) { /* TODO: NMEA2000.ParseMessages() pump (Rx later). */ }

void can_n2k_publish(const ctrl_telemetry_t *t)
{
    (void)t; /* TODO: map snapshot → N2K PGNs (see file header). */
}
