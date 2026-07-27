/*
 * can_n2k.h — bxCAN + NMEA2000 / RV-C glue.
 *
 * Tx (telemetry → Cerbo, GH#18 deliverable #9) is implemented; Rx (BMS/DVCC
 * control ceilings, GH#10) is later. MIDDLEWARE DECISION (mirrors GH#35's
 * usb_cdc.c note): this is NOT the ttlappalainen/NMEA2000 library — the PGN
 * encoders (control/n2k_encode.c), address claim (control/n2k_addrclaim.c)
 * and Tx scheduler (control/n2k_sched.c) are self-written pure C, using that
 * library's public headers only as a reference for wire layouts/PGN field
 * order (facts, no code copied — see each file's header). This file is the
 * thin bxCAN HAL glue (PB8/PB9, AF4, board.h) that drives them from real
 * hardware and real time; polling-only (no CAN IRQ), matching usb_cdc.c's
 * house style: never block, drop-and-count when a buffer is full.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CAN_N2K_H
#define WS500_CAN_N2K_H

#include <stdint.h>
#include "control.h"
#include "telemetry.h"

/*
 * Bring up bxCAN (250 kbit/s N2K rate) and the pure address-claim/scheduler
 * state. Safe to call with no bus attached: HAL_CAN_Init/Start's only wait is
 * the HAL's own bounded (few-ms, timeout-guarded) wait for the peripheral to
 * leave initialization mode — a one-time boot-path cost, not the "never
 * block" contract that applies to can_n2k_poll()/can_n2k_publish() in the
 * steady-state loop. On failure the port stays disabled (can_n2k_poll/
 * publish become no-ops) rather than retrying or hanging.
 */
void can_n2k_init(void);

/* Service the CAN stack from the 10 ms main loop: Tx queue drain, Rx dispatch,
 * bus-off auto-recovery bookkeeping. Non-blocking. */
void can_n2k_poll(void);

/* Broadcast the telemetry snapshot as NMEA2000 PGNs (RV-C encoder added later). */
void can_n2k_publish(const ctrl_telemetry_t *t);

/*
 * Report a bxCAN bus-off condition (§7 R6 "CAN bus-off: auto-recovery +
 * counter"). Called from can_n2k_poll() on the rising edge of CAN->ESR.BOFF
 * — bxCAN recovers on its own after 128×11 recessive bits (AutoBusOff
 * enabled in can_n2k_init()), so the firmware's job is to COUNT it, not to
 * fix it: a link that keeps going bus-off is a wiring or termination fault,
 * and after the §7 R6 budget is exhausted it latches a fault the control
 * core treats as a lost external source.
 *
 * Exposed as a public hook (rather than file-static) so a test harness can
 * drive it directly without real hardware.
 */
void can_n2k_note_bus_off(void);

/* Bus-off events counted since boot (telemetry/diagnostics). */
uint32_t can_n2k_bus_off_count(void);

/*
 * Frames discarded since boot because the software Tx ring was full — the
 * drop-not-block contract's other half. Distinct from a bus-off: this counts
 * frames the FIRMWARE threw away (the bus outran our drain, or never drained
 * at all because nothing is acknowledging), not bus errors. A steadily
 * climbing value means the Tx cadence is over-subscribed for the link.
 *
 * TODO(GH#10): surface this and can_n2k_bus_off_count() in the telemetry line
 * with the CAN half of deliverable #20 — §7 R6 wants every error counter
 * visible. The errb budget (ERRB_CAN) already reaches telemetry; these two
 * raw counters do not yet.
 */
uint32_t can_n2k_tx_dropped_count(void);

#endif /* WS500_CAN_N2K_H */
