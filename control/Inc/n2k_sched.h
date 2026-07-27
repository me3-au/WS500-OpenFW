/*
 * n2k_sched.h — NMEA 2000 Tx cadence engine (PURE).
 *
 * The scheduling half of GH#18 deliverable #9 (CAN_INTEGRATION.md §2):
 * given the current time, the dialect-neutral telemetry snapshot, and the
 * source address n2k_addrclaim.c has claimed, decide WHICH of the §2 PGNs
 * are due this cycle and encode them with n2k_encode.c. Core/Src/can_n2k.c
 * just pushes whatever comes out at the wire — every timing/priority
 * decision lives here so it is host-testable without a bus.
 *
 * PURE — no HAL, no allocation; all state lives in the caller-owned
 * n2k_sched_t (two independently-scheduled devices, or a test harness and
 * the firmware, never collide).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_N2K_SCHED_H
#define WS500_N2K_SCHED_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "n2k_encode.h"
#include "telemetry.h"

/*
 * Scheduler context: per-PGN "next due" timestamps (ms, same clock as the
 * `now_ms` passed to n2k_sched_tick — comparisons are wraparound-safe signed
 * differences, matching main.c's own `next += LOOP_PERIOD_MS` idiom) and
 * per-fast-packet-PGN sequence counters. Zero-initialised (or
 * n2k_sched_init()'d) state makes every PGN due immediately, so the first
 * tick after address claim settles bursts out the whole set in priority
 * order and catches up over the following cycles if it doesn't all fit.
 */
typedef struct {
    uint8_t  sid;              /* n2k_sid_next() cycle state */

    uint32_t next_127488_ms;
    uint32_t next_127508_ms;
    uint32_t next_127506_ms;
    uint32_t next_127750_ms;
    uint32_t next_126983_ms;   /* reset to "due now" whenever faults clear */
    uint32_t next_126985_ms;
    uint32_t next_126996_ms;
    uint32_t next_126998_ms;
    uint32_t next_prop_ms;

    uint8_t  seq_127506;       /* fast-packet seq, 0..7, each ++ on send */
    uint8_t  seq_126983;
    uint8_t  seq_126985;
    uint8_t  seq_126996;
    uint8_t  seq_126998;
    uint8_t  seq_prop;

    uint64_t                 device_name;  /* this device's ISO NAME, for
                                            * n2k_alert_t.source_name */
    const n2k_product_info_t *product;     /* owned by the caller (a const
                                            * table in can_n2k.c) */
    const char *install_desc1;             /* 126998 fields; may be NULL */
    const char *install_desc2;
    const char *mfg_info;
} n2k_sched_t;

/*
 * Seed the scheduler. `product`/`install_desc1`/`install_desc2`/`mfg_info`
 * are stored as pointers (not copied) — they must outlive the scheduler,
 * which in practice means a const table in Core/Src/can_n2k.c. Every PGN
 * starts "due immediately" (see n2k_sched_t doc comment above).
 */
void n2k_sched_init(n2k_sched_t *s, uint64_t device_name,
                    const n2k_product_info_t *product,
                    const char *install_desc1, const char *install_desc2,
                    const char *mfg_info);

/*
 * Advance the schedule by one cycle and encode whatever is due into `out`
 * (capacity `max_frames`), from the telemetry snapshot `t` and the claimed
 * source address `src`.
 *
 * `addr_ready` must be n2k_ac_ready() from the paired n2k_ac_t — transmitting
 * before address claim settles is a protocol violation, so when false this
 * returns 0 and touches no scheduling state (every PGN stays "due", so the
 * whole set fires on the first cycle after claim-ready rather than having
 * lost its place in the schedule). Decoupled from n2k_addrclaim.h by a plain
 * bool (rather than an #include) so this module stays independently testable.
 *
 * Per-PGN periods (standard N2K transmission intervals, cited at each PGN's
 * encode site below): 127488 ~100 ms; 127508/127506/127750 ~1500 ms; 126983
 * ~1000 ms while a fault is active (n2k_alert_from_telemetry() == 0 → neither
 * 126983 nor 126985 fire, and their "due" timers reset so a NEW fault alerts
 * on the next cycle rather than waiting out a stale interval); 126985 slower
 * than 126983; 126996/126998 are request-driven in the standard, but Rx
 * (ISO Request handling) isn't wired up yet, so they run on a 5 s heartbeat
 * instead — TODO(GH#10): replace with real ISO-Request-driven Tx once CAN Rx
 * lands, and delete this concession.
 *
 * One SID (n2k_sid_next) is drawn per call and shared by every PGN this
 * cycle encodes, since they all come from the same telemetry snapshot `t`.
 *
 * Bounded output: PGNs are attempted in fixed priority order (rapid update
 * first, then safety alerts, then status, then the low-priority info/
 * heartbeat PGNs last) and a PGN that doesn't fit in the remaining `out`
 * capacity is simply left "due" — its next-due timestamp and fast-packet seq
 * are NOT advanced — so it is retried (and gets first claim on the buffer)
 * on the next call rather than being silently dropped forever.
 *
 * Returns the number of frames written, or -1 on NULL s/t/out.
 */
int n2k_sched_tick(n2k_sched_t *s, uint32_t now_ms, bool addr_ready, uint8_t src,
                   const ctrl_telemetry_t *t, n2k_frame_t *out, size_t max_frames);

#endif /* WS500_N2K_SCHED_H */
