/*
 * can_n2k.h — bxCAN + NMEA2000 / RV-C glue.
 * Tx (telemetry → Cerbo) is the near-term target; Rx (BMS/DVCC control) is later.
 * Reuse the MIT ttlappalainen/NMEA2000 library on bxCAN (PB8/PB9).
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CAN_N2K_H
#define WS500_CAN_N2K_H

#include <stdint.h>
#include "control.h"
#include "telemetry.h"

/* Bring up bxCAN (250 kbit/s N2K rate) + the NMEA2000 stack; safe to call with
 * no bus attached — errors surface via the driver error budget (§7 R6), never
 * a blocking wait. */
void can_n2k_init(void);

/* Service the CAN stack from the 10 ms main loop: Tx queue drain, Rx dispatch,
 * bus-off auto-recovery bookkeeping. Non-blocking. */
void can_n2k_poll(void);

/* Broadcast the telemetry snapshot as NMEA2000 PGNs (RV-C encoder added later). */
void can_n2k_publish(const ctrl_telemetry_t *t);

/*
 * Report a bxCAN bus-off condition (§7 R6 "CAN bus-off: auto-recovery +
 * counter"). Called by the CAN driver when it observes ESR.BOFF — bxCAN
 * recovers on its own after 128×11 recessive bits, so the firmware's job is
 * to COUNT it, not to fix it: a link that keeps going bus-off is a wiring or
 * termination fault, and after the §7 R6 budget is exhausted it latches a
 * fault the control core treats as a lost external source.
 *
 * Exposed as a hook rather than called internally because bxCAN itself is not
 * up yet — TODO(GH#27): call this from the bus-off detection path when the CAN
 * driver lands, and delete this note.
 */
void can_n2k_note_bus_off(void);

/* Bus-off events counted since boot (telemetry/diagnostics). */
uint32_t can_n2k_bus_off_count(void);

#endif /* WS500_CAN_N2K_H */
