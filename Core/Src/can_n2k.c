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
 * SPDX-License-Identifier: MIT
 */
#include "can_n2k.h"
#include "err_budget.h"

static uint32_t s_bus_off_events;

void can_n2k_init(void) { /* TODO(GH#27): bxCAN init + NMEA2000 device announce. */ }

void can_n2k_poll(void)
{
    /* TODO(GH#27): NMEA2000.ParseMessages() pump (Rx later), and poll
     * CAN->ESR.BOFF here so can_n2k_note_bus_off() is driven by real hardware
     * state instead of waiting for a caller. */
}

void can_n2k_publish(const ctrl_telemetry_t *t)
{
    (void)t; /* TODO(GH#27): map snapshot → N2K PGNs (see file header). */
}

/* §7 R6 bus-off accounting. Contract: can_n2k.h. Deliberately does NOT try to
 * force recovery — bxCAN's automatic bus-off recovery does that in hardware,
 * and a software "fix" would only mask how often it is happening. */
void can_n2k_note_bus_off(void)
{
    if (s_bus_off_events != UINT32_MAX) s_bus_off_events++;
    (void)errb_fail(ERRB_CAN);   /* REINIT rung is a no-op here: see above */
}

uint32_t can_n2k_bus_off_count(void) { return s_bus_off_events; }
