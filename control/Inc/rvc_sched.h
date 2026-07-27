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
 * ---- Address claim: reused as-is, WITH A CONFIRMED CAVEAT --------------
 * PROJECT_PLAN §1 row 10a's own brief says RV-C's address claim is
 * "dialect-neutral" and reuses n2k_addrclaim.c unmodified on PGN 60928 (the
 * standard J1939/N2K Address Claim PGN), with only the NAME's
 * industry-group field differing per dialect. Core/Src/can_n2k.c follows
 * that instruction literally: a second n2k_ac_t runs the RV-C claim on PGN
 * 60928/59904, exactly like the N2K one.
 *
 * BUT: primary-source research for this deliverable (a real shipped RV-C
 * product's own DGN reference guide — Xantrex Freedom SW-RVC RV-C DGN
 * Reference Guide 976-0452-01-01 Rev B, §3.3.2 "Address Claimed
 * ADDRESS_CLAIM", DGN hex 1EE00) shows RV-C's OWN address-claim DGN is
 * 0x1EE00 (126464 decimal) — Data Page 1 — not J1939/N2K's 0x00EE00 (60928,
 * Data Page 0). Every other RV-C administrative/status DGN in that same
 * reference guide (17F00, 17C00, 17B00, 1FECA, 1FFFD, 1FFC7, ...) is
 * likewise DP=1, consistent with rvc_encode.h's own structural note: RV-C
 * appears to deliberately reserve the whole DP=1 page for itself, plausibly
 * so it can coexist on a bus bridged to a DP=0 J1939 chassis network
 * without PGN collisions.
 *
 * [SPEC-SIGNOFF, the single most consequential one in this deliverable]: if
 * that primary-source reading is correct, reusing n2k_addrclaim.c unmodified
 * (hardcoded to PGN 60928/59904 via its own #defines, not parameters) means
 * this device's RV-C claim frames go out on the WRONG 29-bit CAN identifier
 * for a real RV-C bus — a real RV-C node would never see them, and an ISO
 * Request for RV-C's own admin DGN (plausibly 0x1EA00, DP=1-shifted from
 * 59904, though that value is NOT independently confirmed anywhere and is
 * pure extrapolation from the DP=1 pattern) would go unanswered.
 *
 * This deliverable ships the literal instruction (reuse n2k_addrclaim.c
 * unmodified, PGN 60928/59904) rather than silently patching a shared,
 * already-CI-green, already-tested module on unverified-against-primary-text
 * evidence — n2k_addrclaim.h's PGN constants are #defines, not per-instance
 * parameters, so making them dialect-specific is a real (if small) change to
 * a file this deliverable was told not to touch. Given RV-C Tx is disabled
 * by default (Core/Src/can_n2k.c) and has never touched a real bus regardless,
 * getting this wrong costs nothing today — but it MUST be resolved (read the
 * actual RV-C §3.3 spec text, or capture a real RV-C bus with a Cerbo in
 * RV-C-profile mode) before RV-C Tx is ever enabled for real. If the DP=1
 * finding holds, the fix is to give n2k_ac_t its own claim/request PGNs as
 * init-time fields (a small, additive change — NOT a duplicate of the state
 * machine) rather than forking n2k_addrclaim.c.
 *
 * SPDX-License-Identifier: MIT
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
 * Our own DC_SOURCE_STATUS device-priority value. No RV-C spec text or bus
 * capture confirms a "correct" number for an alternator/engine-driven
 * charger specifically — the cited examples above are all house-battery
 * devices (inverter/charger, solar, AC charger, BMS). 100 (the
 * inverter/charger tier) is the closest functional analog: we are, like an
 * inverter/charger, a device that actively PUSHES current into the bank
 * rather than just observing it (unlike a passive shunt monitor), so we
 * adopt the same tier Victron's own dbus-rvc implementation uses for that
 * device class [SPEC-SIGNOFF: verify against a real installation with a
 * dedicated battery monitor present — if this device's reported priority
 * is set too high, it could win over a purpose-built BMS/shunt that should
 * be the actual source of truth for SOC/capacity data we don't have anyway]. */
#define RVC_OUR_DEVICE_PRIORITY   100u

/*
 * "Has a higher-priority competitor gone quiet" timeout. Neither reference
 * source states an explicit RV-C-mandated defer/resume timing (only the
 * arbitration OUTCOME — highest priority wins — is documented); this value
 * is firmware policy, not a cited RV-C interval [SPEC-SIGNOFF], chosen as
 * 3x the 500 ms DC_SOURCE_STATUS broadcast period (Xantrex reference guide)
 * — the same "a few missed periods means gone, not just briefly delayed"
 * margin common to CAN keepalive conventions generally. */
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
