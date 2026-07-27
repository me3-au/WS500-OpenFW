/*
 * n2k_sched.c — NMEA 2000 Tx cadence engine (contract: n2k_sched.h). PURE.
 * SPDX-License-Identifier: MIT
 */
#include "n2k_sched.h"

/* Single-bank, single-connection device: no multi-instance story yet
 * (CAN_INTEGRATION.md doesn't define one), so every instance/connection
 * field is a fixed 0. */
#define N2K_INSTANCE_DEFAULT   0u
#define N2K_CONNECTION_DEFAULT 0u

/* Per-PGN periods (ms). Cited per-PGN below; see n2k_sched.h for the summary
 * and the house TODO(GH#10) on 126996/126998's heartbeat concession. */
#define N2K_PERIOD_127488_MS   100u   /* "Rapid Update" class PGN */
#define N2K_PERIOD_127508_MS  1500u   /* periodic status PGN */
#define N2K_PERIOD_127506_MS  1500u   /* periodic status PGN */
#define N2K_PERIOD_127750_MS  1500u   /* periodic status PGN */
#define N2K_PERIOD_126983_MS  1000u   /* Alert, while a fault is active */
/* Alert Text has no standard periodic rate (it is normally sent once per new
 * alert plus on request); we resend slower than 126983 so a display that
 * missed the first copy still catches it soon. Firmware policy, not a fixed
 * N2K interval [SPEC-SIGNOFF]. */
#define N2K_PERIOD_126985_MS  2500u
/* Product/Configuration Info are request-driven (ISO Request → 59904) in the
 * standard; we have no Rx yet (GH#10), so this heartbeat is a stand-in. */
#define N2K_PERIOD_126996_MS  5000u
#define N2K_PERIOD_126998_MS  5000u
/* Proprietary full-telemetry fast packet: our own PGN, no standard interval
 * to cite — grouped with the other status PGNs' cadence [SPEC-SIGNOFF]. */
#define N2K_PERIOD_PROP_MS    1500u

void n2k_sched_init(n2k_sched_t *s, uint64_t device_name,
                    const n2k_product_info_t *product,
                    const char *install_desc1, const char *install_desc2,
                    const char *mfg_info)
{
    n2k_sched_t z = {0};
    *s = z;
    s->device_name    = device_name;
    s->product         = product;
    s->install_desc1  = install_desc1;
    s->install_desc2  = install_desc2;
    s->mfg_info        = mfg_info;
    /* next_*_ms all 0 -> due on the first tick. Valid because this is a
     * boot-time call and HAL_GetTick() starts at 0: a 0 deadline only reads
     * as "due" while now_ms is in the low half of the range (see the due()
     * note below). Anything re-arming a timer at RUNTIME must anchor on
     * now_ms instead, never on 0. */
}

/* Wraparound-safe "is it time yet" — same idiom as main.c's own
 * `(int32_t)(HAL_GetTick() - next) >= 0` catch-up loop gate. */
static bool due(uint32_t now_ms, uint32_t next_ms)
{
    return (int32_t)(now_ms - next_ms) >= 0;
}

int n2k_sched_tick(n2k_sched_t *s, uint32_t now_ms, bool addr_ready, uint8_t src,
                   const ctrl_telemetry_t *t, n2k_frame_t *out, size_t max_frames)
{
    size_t count = 0;
    uint8_t sid;
    n2k_alert_t alert;
    int have_alert;

    if (!s || !t || !out) return -1;
    if (!addr_ready) return 0;   /* protocol violation to Tx before claim settles */

    /* One SID for every PGN this call encodes -- they all read the same `t`. */
    sid = n2k_sid_next(&s->sid);

    /* ---- 1) 127488 Engine Parameters Rapid: highest urgency (100 ms, and
     * the only single-frame rapid PGN in the set) goes first. ------------- */
    if (due(now_ms, s->next_127488_ms) && count < max_frames) {
        if (n2k_encode_127488(t, N2K_INSTANCE_DEFAULT, src, &out[count]) == 1) {
            count++;
            s->next_127488_ms = now_ms + N2K_PERIOD_127488_MS;
        }
    }

    /* ---- 2) Alerts: safety-relevant, so scheduled ahead of routine status.
     * No active fault -> nothing to send AND both "due" timers reset, so a
     * newly-raised fault alerts on the very next cycle instead of waiting
     * out whatever was left of the previous interval. ---------------------- */
    have_alert = n2k_alert_from_telemetry(t, s->device_name, &alert);
    if (have_alert == 1) {
        if (due(now_ms, s->next_126983_ms) && count < max_frames) {
            uint8_t seq = s->seq_126983;
            int n = n2k_encode_126983(&alert, src, seq, &out[count],
                                      max_frames - count);
            if (n > 0) {
                count += (size_t)n;
                s->seq_126983     = (uint8_t)((seq + 1u) & 0x7u);
                s->next_126983_ms = now_ms + N2K_PERIOD_126983_MS;
            }
        }
        if (due(now_ms, s->next_126985_ms) && count < max_frames) {
            uint8_t seq = s->seq_126985;
            /* alert.alert_id is (fault bit index + 1) -- n2k_alert_from_
             * telemetry's contract (n2k_encode.h) -- so the bit reconstructs
             * as 1 << (alert_id - 1) for the plain-language lookup. */
            const char *txt = n2k_alert_text(1u << (alert.alert_id - 1u));
            int n = n2k_encode_126985(&alert, txt, NULL, src, seq, &out[count],
                                      max_frames - count);
            if (n > 0) {
                count += (size_t)n;
                s->seq_126985     = (uint8_t)((seq + 1u) & 0x7u);
                s->next_126985_ms = now_ms + N2K_PERIOD_126985_MS;
            }
        }
    } else {
        /* Arm both alert timers to fire on the very next cycle, so a newly
         * raised fault alerts immediately instead of waiting out whatever was
         * left of the previous interval.
         *
         * `now_ms`, NOT 0: due() is the wraparound-safe signed-difference
         * form, so a literal 0 deadline reads as "not due" for the whole
         * second half of the tick range — uptime 24.8..49.7 days would
         * silently stop alerting. Anchoring on the current tick is due-now at
         * every point in the cycle. */
        s->next_126983_ms = now_ms;
        s->next_126985_ms = now_ms;
    }

    /* ---- 3) Routine status set (1500 ms). --------------------------------- */
    if (due(now_ms, s->next_127508_ms) && count < max_frames) {
        if (n2k_encode_127508(t, N2K_INSTANCE_DEFAULT, sid, src, &out[count]) == 1) {
            count++;
            s->next_127508_ms = now_ms + N2K_PERIOD_127508_MS;
        }
    }
    if (due(now_ms, s->next_127506_ms) && count < max_frames) {
        uint8_t seq = s->seq_127506;
        int n = n2k_encode_127506(t, N2K_INSTANCE_DEFAULT, sid, src, seq,
                                  &out[count], max_frames - count);
        if (n > 0) {
            count += (size_t)n;
            s->seq_127506     = (uint8_t)((seq + 1u) & 0x7u);
            s->next_127506_ms = now_ms + N2K_PERIOD_127506_MS;
        }
    }
    if (due(now_ms, s->next_127750_ms) && count < max_frames) {
        if (n2k_encode_127750(t, N2K_CONNECTION_DEFAULT, sid, src, &out[count]) == 1) {
            count++;
            s->next_127750_ms = now_ms + N2K_PERIOD_127750_MS;
        }
    }

    /* ---- 4) Proprietary full telemetry. ----------------------------------- */
    if (due(now_ms, s->next_prop_ms) && count < max_frames) {
        uint8_t seq = s->seq_prop;
        int n = n2k_encode_prop_telemetry(t, sid, src, seq, &out[count],
                                          max_frames - count);
        if (n > 0) {
            count += (size_t)n;
            s->seq_prop     = (uint8_t)((seq + 1u) & 0x7u);
            s->next_prop_ms = now_ms + N2K_PERIOD_PROP_MS;
        }
    }

    /* ---- 5) Product/config info: lowest priority, request-driven-in-spec
     * heartbeat (TODO(GH#10): n2k_sched.h). Last claim on the buffer. ------ */
    if (due(now_ms, s->next_126996_ms) && count < max_frames && s->product) {
        uint8_t seq = s->seq_126996;
        int n = n2k_encode_126996(s->product, src, seq, &out[count],
                                  max_frames - count);
        if (n > 0) {
            count += (size_t)n;
            s->seq_126996     = (uint8_t)((seq + 1u) & 0x7u);
            s->next_126996_ms = now_ms + N2K_PERIOD_126996_MS;
        }
    }
    if (due(now_ms, s->next_126998_ms) && count < max_frames) {
        uint8_t seq = s->seq_126998;
        int n = n2k_encode_126998(s->install_desc1, s->install_desc2,
                                  s->mfg_info, src, seq, &out[count],
                                  max_frames - count);
        if (n > 0) {
            count += (size_t)n;
            s->seq_126998     = (uint8_t)((seq + 1u) & 0x7u);
            s->next_126998_ms = now_ms + N2K_PERIOD_126998_MS;
        }
    }

    return (int)count;
}
