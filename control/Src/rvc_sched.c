/*
 * rvc_sched.c — RV-C Tx cadence engine + RBM election (contract:
 * rvc_sched.h). PURE.
 * SPDX-License-Identifier: MIT
 */
#include "rvc_sched.h"

/* Single-bank device: no multi-instance story (mirrors n2k_sched.c's own
 * N2K_INSTANCE_DEFAULT). */
#define RVC_INSTANCE_DEFAULT   0u

/* Per-DGN periods (ms). Cited in rvc_sched.h's rvc_sched_tick doc comment. */
#define RVC_PERIOD_CHARGER_STATUS_MS    5000u  /* Xantrex reference guide */
#define RVC_PERIOD_CHARGER_STATUS_2_MS   500u  /* [SPEC-SIGNOFF]: no cited
                                                 * interval for this
                                                 * vendor-convergence DGN;
                                                 * matched to the
                                                 * DC_SOURCE_STATUS cadence
                                                 * since it carries the same
                                                 * kind of live V/A/T data */
#define RVC_PERIOD_DC1_MS                500u  /* Xantrex reference guide */
#define RVC_PERIOD_DC2_MS                500u  /* Xantrex reference guide */

/* Wraparound-safe "is it time yet" — identical idiom to n2k_sched.c's own
 * due(), reproduced locally rather than shared (each protocol module stays
 * independent, n2k_addrclaim.c's own house precedent). */
static bool due(uint32_t now_ms, uint32_t next_ms)
{
    return (int32_t)(now_ms - next_ms) >= 0;
}

/* ---- RBM election (contract: rvc_sched.h) -------------------------------- */

void rvc_rbm_init(rvc_rbm_t *r, uint8_t our_priority)
{
    if (!r) return;
    r->our_priority   = our_priority;
    r->master_seen    = false;
    r->master_priority = 0u;
    r->master_last_ms = 0u;
}

void rvc_rbm_rx(rvc_rbm_t *r, uint32_t now_ms, uint32_t id,
                const uint8_t *data, size_t len)
{
    uint32_t pgn;
    uint8_t  priority;
    if (!r || !data || len < 2) return;

    pgn = n2k_can_id_pgn(id);
    if (pgn != RVC_DGN_DC_SOURCE_STATUS_1 && pgn != RVC_DGN_DC_SOURCE_STATUS_2)
        return;

    priority = data[1];              /* byte[1] = device priority, both DGNs */
    if (priority <= r->our_priority) return;   /* only a WINNER makes us defer */

    r->master_seen     = true;
    r->master_priority = priority;
    r->master_last_ms  = now_ms;
}

bool rvc_rbm_defer(const rvc_rbm_t *r, uint32_t now_ms)
{
    if (!r || !r->master_seen) return false;
    /* Elapsed-since-last-seen, wraparound-safe via unsigned difference (see
     * rvc_sched.h's doc comment on why this differs from due()'s signed
     * form). */
    return (uint32_t)(now_ms - r->master_last_ms) < RVC_RBM_TIMEOUT_MS;
}

/* ---- scheduler (contract: rvc_sched.h) ----------------------------------- */

void rvc_sched_init(rvc_sched_t *s, uint8_t our_priority)
{
    rvc_sched_t z = {0};
    *s = z;
    rvc_rbm_init(&s->rbm, our_priority);
    /* next_*_ms all 0 -> due on the first tick, same boot-time-only caveat
     * n2k_sched_init() documents (valid because HAL_GetTick() starts at 0;
     * runtime re-arms must anchor on now_ms, never 0 — see below). */
}

void rvc_sched_rx(rvc_sched_t *s, uint32_t now_ms, uint32_t id,
                  const uint8_t *data, size_t len)
{
    if (!s) return;
    rvc_rbm_rx(&s->rbm, now_ms, id, data, len);
}

int rvc_sched_tick(rvc_sched_t *s, uint32_t now_ms, bool addr_ready,
                   uint8_t src, const ctrl_telemetry_t *t,
                   n2k_frame_t *out, size_t max_frames)
{
    size_t count = 0;
    bool   defer;

    if (!s || !t || !out) return -1;
    if (!addr_ready) return 0;   /* protocol violation to Tx before claim settles */

    /* ---- 1) CHARGER_STATUS: our own identity, never RBM-gated. ----------- */
    if (due(now_ms, s->next_charger_status_ms) && count < max_frames) {
        if (rvc_encode_charger_status(t, RVC_INSTANCE_DEFAULT, src,
                                      &out[count]) == 1) {
            count++;
            s->next_charger_status_ms = now_ms + RVC_PERIOD_CHARGER_STATUS_MS;
        }
    }

    /* ---- 2) CHARGER_STATUS_2: same identity, never RBM-gated. ------------ */
    if (due(now_ms, s->next_charger_status2_ms) && count < max_frames) {
        if (rvc_encode_charger_status_2(t, RVC_INSTANCE_DEFAULT,
                                        RVC_INSTANCE_DEFAULT,
                                        s->rbm.our_priority, src,
                                        &out[count]) == 1) {
            count++;
            s->next_charger_status2_ms = now_ms + RVC_PERIOD_CHARGER_STATUS_2_MS;
        }
    }

    /* ---- 3) DC_SOURCE_STATUS_1/2: RBM-gated (rvc_sched.h boundary). A
     * deferred DGN's "due" timer is anchored on now_ms (not 0 — the same
     * wraparound lesson n2k_sched.c's alert re-arm already encodes), so it
     * fires on the very next non-deferred cycle rather than waiting out
     * whatever was left of the previous period. --------------------------- */
    defer = rvc_rbm_defer(&s->rbm, now_ms);
    if (defer) {
        s->next_dc1_ms = now_ms;
        s->next_dc2_ms = now_ms;
    } else {
        if (due(now_ms, s->next_dc1_ms) && count < max_frames) {
            if (rvc_encode_dc_source_status_1(t, RVC_INSTANCE_DEFAULT,
                                              s->rbm.our_priority, src,
                                              &out[count]) == 1) {
                count++;
                s->next_dc1_ms = now_ms + RVC_PERIOD_DC1_MS;
            }
        }
        if (due(now_ms, s->next_dc2_ms) && count < max_frames) {
            if (rvc_encode_dc_source_status_2(t, RVC_INSTANCE_DEFAULT,
                                              s->rbm.our_priority, src,
                                              &out[count]) == 1) {
                count++;
                s->next_dc2_ms = now_ms + RVC_PERIOD_DC2_MS;
            }
        }
    }

    return (int)count;
}
