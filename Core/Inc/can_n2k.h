/*
 * can_n2k.h — bxCAN + NMEA2000 / RV-C glue.
 *
 * Tx (telemetry → Cerbo, GH#18 deliverable #9) is implemented for BOTH
 * dialects as of PROJECT_PLAN §1 row 10a: N2K PGNs and RV-C DGNs, selectable
 * independently (can_n2k_set_n2k_enabled()/can_n2k_set_rvc_enabled() below;
 * default N2K on, RV-C off). Rx now also covers BMS/DVCC control ceilings
 * (deliverable #10, CAN_INTEGRATION.md §1/§3): the CAN-BMS/REC/JK 11-bit
 * standard-ID frame set (control/bms_rx.h) is decoded alongside the existing
 * 29-bit extended-ID address-claim/RBM traffic — see can_n2k_bms_snapshot()
 * below. The RV-C half additionally listens for OTHER nodes'
 * DC_SOURCE_STATUS broadcasts for the RBM election (control/rvc_sched.c),
 * which is the other piece of "control in" this file touches, since RBM
 * isn't a charge-control ceiling either. MIDDLEWARE DECISION (mirrors GH#35's usb_cdc.c
 * note): this is NOT the ttlappalainen/NMEA2000 library — the PGN encoders
 * (control/n2k_encode.c), address claim (control/n2k_addrclaim.c, shared
 * dialect-neutrally by both N2K and RV-C — see can_n2k.c), N2K Tx scheduler
 * (control/n2k_sched.c) and RV-C Tx scheduler + RBM (control/rvc_sched.c)
 * are self-written pure C, using public reference material only for wire
 * layouts/DGN field order (facts, no code copied — see each file's header).
 * This file is the thin bxCAN HAL glue (PB8/PB9, AF4, board.h) that drives
 * them from real hardware and real time; polling-only (no CAN IRQ), matching
 * usb_cdc.c's house style: never block, drop-and-count when a buffer is full.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CAN_N2K_H
#define WS500_CAN_N2K_H

#include <stdint.h>
#include <stdbool.h>
#include "control.h"
#include "telemetry.h"
#include "bms_rx.h"

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

/*
 * Broadcast the telemetry snapshot over every ENABLED Tx dialect
 * (can_n2k_set_n2k_enabled() / can_n2k_set_rvc_enabled() below) — N2K PGNs
 * via control/n2k_sched.c, RV-C DGNs via control/rvc_sched.c
 * (PROJECT_PLAN §1 row 10a). Each dialect's own address-claim readiness
 * gates its own frames independently (see can_n2k.c).
 */
void can_n2k_publish(const ctrl_telemetry_t *t);

/*
 * Pull the current dialect-neutral BMS snapshot (deliverable #10,
 * CAN_INTEGRATION.md §1) — decoded from whatever CAN-BMS/REC/JK standard-ID
 * frames can_n2k_poll() has drained into the pure control/bms_rx.c driver
 * since boot. Call once per control tick, AFTER can_n2k_poll() has run this
 * cycle (main.c's loop order already satisfies this — poll() runs every
 * iteration, snapshot is only read inside the 10 ms tick).
 *
 * vbat_pack_v: pass the SAME battery-pack voltage reading the control core
 * is using this cycle — see bms_rx_snapshot()'s doc comment (control/
 * bms_rx.h) for why this must be the pack voltage, never the field-supply
 * voltage, and what a lost/implausible reading does to the output.
 *
 * Safe to call even if can_n2k_init() failed (s_ok false) or no BMS frame
 * has ever arrived: returns the "never seen" snapshot (every ceiling
 * inactive, `lost` false) rather than garbage.
 */
void can_n2k_bms_snapshot(float vbat_pack_v, bms_snapshot_t *out);

/*
 * Dialect selector (PROJECT_PLAN §1 row 10a / CAN_INTEGRATION.md §7: "the
 * dialect is a per-install config choice"). Driver-level runtime state, NOT
 * part of ctrl_config_t's versioned schema — this task deliberately does not
 * bump the config schema to add it. TODO(GH#10): surface this in the §7
 * config document once a schema revision is due for other reasons; until
 * then it defaults to the values below and can only be changed by a call
 * from firmware code (e.g. a future debug/test hook), not over the wire.
 *
 * Default: N2K enabled, RV-C disabled — enabling this deliverable must NOT
 * change what a Cerbo already sees on N2K. Toggling RV-C on lazily brings up
 * its own address-claim identity on first enable (Core/Src/can_n2k.c); it is
 * never re-torn-down by disabling again (ISO 11783 has no clean "give back
 * my address" primitive) — a corner case out of this deliverable's scope.
 */
/* Enable/disable the N2K Tx dialect (default: enabled). */
void can_n2k_set_n2k_enabled(bool enabled);
/* True if the N2K Tx dialect is currently enabled. */
bool can_n2k_n2k_enabled(void);
/* Enable/disable the RV-C Tx dialect (default: disabled). */
void can_n2k_set_rvc_enabled(bool enabled);
/* True if the RV-C Tx dialect is currently enabled. */
bool can_n2k_rvc_enabled(void);

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
