/*
 * rvc_sched.h — RV-C Tx cadence engine + RBM election (PURE).
 * SPDX-License-Identifier: MIT
 *
 * The scheduling half of PROJECT_PLAN §1 row 10a / CAN_INTEGRATION.md §8:
 * given the current time and the dialect-neutral telemetry snapshot, decide
 * which RV-C DGNs (rvc_encode.h) are due this cycle. Mirrors n2k_sched.h's
 * shape exactly (bounded output, priority-ordered fill, overflow slips to
 * the next call, nothing before address claim settles) — see that file for
 * the fuller rationale; this header only documents what differs for RV-C.
 *
 * PURE — no HAL, no allocation; all state lives in the caller-owned
 * rvc_sched_t.
 *
 * ---- Address claim: RESOLVED against RV-C spec text (2026-07-28) --------
 * PROJECT_PLAN §1 row 10a's brief says RV-C's address claim is
 * "dialect-neutral" and reuses n2k_addrclaim.c unmodified on PGN 60928 (the
 * standard J1939/N2K Address Claim PGN); Core/Src/can_n2k.c does exactly
 * that. An earlier reading of a vendor DGN guide (Xantrex 976-0452-01-01
 * Rev B, "1EE00") suggested RV-C's own ADDRESS_CLAIM might live on Data
 * Page 1 (0x1EE00), which would have put our claim frames on a 29-bit
 * identifier no RV-C node watches. That fear is REFUTED from primary text:
 * the spec itself (RVIA, "RV-C Specification Full Layer", rev. 2025-07-31,
 * public download from rvia.org — cited, not vendored, for copyright;
 * §3.3.2 Table 3.3.2b, and the §7 DGN dictionary entry "ADDRESS_CLAIMED
 * EE00h 60928") defines the address-claimed DGN as EE00h = PGN 60928,
 * Data Page 0 — exactly what n2k_addrclaim.c implements. RV-C mirrors
 * NMEA 2000's own structure here: application DGNs live on DP=1, network
 * management stays on the inherited J1939 DP=0 PGNs. The reuse-as-is
 * decision stands; do NOT fork n2k_addrclaim.c.
 *
 * Two REAL deltas the same spec read surfaced (both TODO(GH#32); neither
 * invalidates reusing n2k_addrclaim.c unmodified):
 *
 * 1. Identifier form: RV-C broadcasts ADDRESS_CLAIMED with DGN-low = 00h
 *    (ID ..EE00xx) where J1939/N2K send destination-global (..EEFFxx); our
 *    reuse emits the EEFF form. Spec §4.3.1.2 tolerates this explicitly
 *    ("broadcast of incidental messages using the EEFFh identifiers are
 *    acceptable as long as the device correctly supports the EE00h
 *    messages") — and our Rx side does support them: n2k_can_id_pgn()
 *    zeroes the PS byte for PDU1, so claims/requests in either form parse
 *    to 60928/59904, and n2k_ac_rx() answers RV-C's directed address
 *    request (§3.3.2 Table 3.3.2a: a request for EE00h sent from SA 254
 *    to the desired address — data 00,EE,00 → requested PGN 60928,
 *    handled). Compliant as-is; emitting the EE00 form natively is
 *    optional polish.
 *
 * 2. Preferred start address: RVC_PREFERRED_ADDR (Core/Src/can_n2k.c) is
 *    35, inside RV-C's reserved/static DSA space (Table 7.2: 0-63). A
 *    charger-class device's preferred DYNAMIC range is 128-143 ("Power
 *    Components" — the range Table 7.2 gives Converter #1/#2 and Charge
 *    Controller). §3.3.2 permits the J1939-style start-low-and-count-up
 *    technique n2k_addrclaim.c uses, but the START address must sit in
 *    the device type's range — so 35 must become 128 before RV-C Tx is
 *    ever enabled on a real bus (walking past 143 under contention is
 *    fine; the spec's own fallback rules leave the range too).
 *
 * bench-pending (GH#32): first real-bus session, capture our claim and a
 * Cerbo-in-RV-C-profile claim (expect 18EE00xx/18EEFFxx forms, DP=0) and
 * confirm a directed address request draws our claim — the spec read
 * settles the encoding; the capture settles interop.
 */
#ifndef WS500_RVC_SCHED_H
#define WS500_RVC_SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "rvc_encode.h"
#include "telemetry.h"

/*
 * RBM (Remote Battery Master) election state (CAN_INTEGRATION.md §8): RV-C
 * allows only the highest-`device priority` node to broadcast DC_SOURCE_*
 * data for a given DC-source instance (confirmed by both reference sources
 * — Victron's own "RV-C Device Priority" appendix page gives concrete
 * examples: inverter/charger prio 100, solar prio 90, AC charger prio 80,
 * battery monitor/BMS prio 119, HIGHEST priority wins the right to
 * broadcast). This device has exactly one DC-source instance (the bank it
 * regulates), so a single scalar "have we seen a higher-priority
 * competitor recently" is sufficient — no per-instance table.
 *
 * Boundary this election governs (CAN_INTEGRATION.md §8, restated because
 * getting it wrong either way is a real interop failure, not just cosmetic):
 * deferring applies ONLY to DC_SOURCE_STATUS_1/2/3 (rvc_sched_tick below).
 * RVC_DGN_CHARGER_STATUS and RVC_DGN_CHARGER_STATUS_2 are OUR identity as a
 * charger, not battery-instance data — nothing else on the bus can claim to
 * BE this alternator/charger, so those two always transmit once address
 * claim settles, regardless of RBM state.
 */
typedef struct {
    uint8_t  our_priority;      /* our own DC_SOURCE broadcast priority */
    bool     master_seen;       /* a higher-priority competitor has been
                                 * observed at least once (explicit flag,
                                 * not a sentinel timestamp — sidesteps any
                                 * wraparound ambiguity on the very first
                                 * observation) */
    uint8_t  master_priority;   /* highest competing priority last seen */
    uint32_t master_last_ms;    /* now_ms of that last observation */
} rvc_rbm_t;

/*
 * Our own DC_SOURCE_STATUS device-priority value. RESOLVED (2026-07-28)
 * from spec text: RV-C Table 6.5.2b enumerates the tiers — 120 battery
 * SOC/BMS device, 100 inverter/charger, 80 CHARGER, 60 inverter, 40
 * voltmeter/ammeter, 20 voltmeter, 0 "no priority, always reporting" —
 * and adds "designers should consider making this value configurable".
 * An engine-driven alternator regulator is a charger, not an
 * inverter/charger, so the earlier 100 guess (inverter/charger tier) is
 * REVISED: the shipped value must be 80 (code change TODO(GH#32) — this
 * pass is spec/comments only, so the define below still reads 100 until
 * that lands). 80 keeps us below a BMS/battery monitor (120 — which
 * should own SOC/capacity truth) and above inverters and passive meters:
 * exactly the arbitration shape the RBM election wants. */
#define RVC_OUR_DEVICE_PRIORITY   100u

/*
 * "Has a higher-priority competitor gone quiet" timeout. RV-C specifies
 * the arbitration OUTCOME (highest device priority broadcasts) and the
 * 500 ms normal broadcast gap for DC_SOURCE_STATUS_1/2 (Tables
 * 6.5.2a/6.5.3a) but no defer/resume timing, so this remains firmware
 * policy (signed off 2026-07-28): 3x the spec's 500 ms broadcast period —
 * the usual "a few missed periods means gone, not just briefly delayed"
 * CAN keepalive margin. */
#define RVC_RBM_TIMEOUT_MS   1500u

/* Seed the election state. */
void rvc_rbm_init(rvc_rbm_t *r, uint8_t our_priority);

/*
 * Feed one received frame to the election. Only RVC_DGN_DC_SOURCE_STATUS_1
 * and _2 are consulted (both carry `device priority` at byte[1], confirmed
 * layout — rvc_encode.h); everything else is ignored. A frame from a node
 * whose priority is <= ours is ignored too — only a HIGHER-priority
 * competitor can make us defer. `id` is the 29-bit CAN identifier,
 * `data`/`len` the payload. NULL-safe (no-ops on NULL args).
 */
void rvc_rbm_rx(rvc_rbm_t *r, uint32_t now_ms, uint32_t id,
                const uint8_t *data, size_t len);

/*
 * True if a higher-priority competitor has been heard within
 * RVC_RBM_TIMEOUT_MS of `now_ms` — i.e. we must NOT transmit
 * DC_SOURCE_STATUS_1/2/3 this cycle. Wraparound-safe: the elapsed-time
 * comparison is an unsigned difference against the timeout (correct as
 * long as real elapsed time between observations stays far below the ~49.7
 * day uint32 wrap period, true for a sub-2-second timeout), NOT a signed
 * "deadline passed" comparison like n2k_sched.c's due() — a different
 * question (elapsed-since-last-seen vs. deadline-reached) needs a different
 * (still wraparound-safe) idiom. NULL-safe (returns false).
 */
bool rvc_rbm_defer(const rvc_rbm_t *r, uint32_t now_ms);

/*
 * Scheduler context: per-DGN "next due" timestamps (ms, same clock/idiom as
 * n2k_sched_t) plus the RBM election state. Zero-initialised (or
 * rvc_sched_init()'d) state makes every DGN due immediately, same
 * first-tick-bursts-the-whole-set behaviour n2k_sched.h documents.
 */
typedef struct {
    uint32_t next_charger_status_ms;
    uint32_t next_charger_status2_ms;
    uint32_t next_dc1_ms;
    uint32_t next_dc2_ms;
    rvc_rbm_t rbm;
} rvc_sched_t;

/* Seed the scheduler (also seeds the RBM election with RVC_OUR_DEVICE_PRIORITY,
 * unless the caller wants a different value — `our_priority` is a parameter
 * so a test harness, or a future config surface, can override it). */
void rvc_sched_init(rvc_sched_t *s, uint8_t our_priority);

/* Feed one received frame to the RBM election (forwards to rvc_rbm_rx).
 * Exposed as the scheduler's own Rx entrypoint so Core/Src/can_n2k.c has one
 * call site per dialect, mirroring n2k_ac_rx's role for the N2K side. */
void rvc_sched_rx(rvc_sched_t *s, uint32_t now_ms, uint32_t id,
                  const uint8_t *data, size_t len);

/*
 * Advance the schedule by one cycle and encode whatever is due into `out`
 * (capacity `max_frames`), from the telemetry snapshot `t` and the claimed
 * source address `src`.
 *
 * `addr_ready` must be n2k_ac_ready() from the RV-C n2k_ac_t instance
 * (Core/Src/can_n2k.c) — transmitting before address claim settles is a
 * protocol violation exactly as it is for N2K; when false this returns 0
 * and touches no scheduling state, same "whole set fires on the first
 * ready cycle" contract n2k_sched_tick documents.
 *
 * Per-DGN periods (cited at each #define in rvc_sched.c): CHARGER_STATUS
 * 5000 ms, CHARGER_STATUS_2 500 ms, DC_SOURCE_STATUS_1/2 500 ms.
 *
 * DC_SOURCE_STATUS_1/2 are additionally gated on `!rvc_rbm_defer(...)` —
 * skipped (their "due" timers reset to `now_ms` so they fire immediately
 * once we stop deferring, same anchor-on-now_ms idiom n2k_sched.c uses for
 * its alert re-arm, not a literal 0) whenever a higher-priority competitor
 * is active. CHARGER_STATUS and CHARGER_STATUS_2 are NEVER gated by RBM
 * (rvc_rbm_t's doc comment above states the boundary).
 *
 * DC_SOURCE_STATUS_3 is never scheduled here at all (rvc_encode.h: the
 * reference product itself does not periodically broadcast it either).
 *
 * Bounded output: DGNs are attempted in fixed priority order (CHARGER_STATUS
 * first — our own identity — then CHARGER_STATUS_2, then DC_SOURCE_STATUS_1,
 * then _2) and a DGN that doesn't fit in the remaining `out` capacity is
 * left "due" (its next-due timestamp is NOT advanced) so it is retried, with
 * first claim on the buffer, on the next call — never silently dropped.
 *
 * Returns the number of frames written, or -1 on NULL s/t/out.
 */
int rvc_sched_tick(rvc_sched_t *s, uint32_t now_ms, bool addr_ready,
                   uint8_t src, const ctrl_telemetry_t *t,
                   n2k_frame_t *out, size_t max_frames);

#endif /* WS500_RVC_SCHED_H */
