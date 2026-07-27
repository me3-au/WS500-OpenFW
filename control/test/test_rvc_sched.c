/*
 * test_rvc_sched.c — RV-C Tx cadence engine + RBM election (PROJECT_PLAN §1
 * row 10a, CAN_INTEGRATION.md §8): per-DGN periods, nothing before claim-
 * ready, bounded-output overflow behaviour, and the full RBM election —
 * defer to a higher-priority master, keep sending our own CHARGER_STATUS/
 * CHARGER_STATUS_2 while deferring DC_SOURCE_STATUS_1/2, and resume after
 * the master goes quiet for RVC_RBM_TIMEOUT_MS. Also covers wraparound
 * safety of the RBM "last seen" timer.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "rvc_sched.h"
#include <string.h>

static ctrl_telemetry_t make_snapshot(void)
{
    ctrl_telemetry_t t;
    memset(&t, 0, sizeof t);
    t.state             = CTRL_BULK;
    t.active_profile_id = 1;
    t.field_effort      = 0.5f;
    t.binding           = CTRL_BIND_STAGE;
    t.binding_w         = 1000.0f;
    t.faults            = 0;
    t.severity          = CTRL_SEV_INFO;
    t.cells_series      = 16;
    t.vbat_pack_v       = 54.0f;
    t.amps_batt         = 50.0f;
    t.alt_temp_c        = 60.0f;
    t.batt_temp_c       = 25.0f;
    t.rpm               = 1800.0f;
    t.rpm_state         = CTRL_RPM_VALID;
    return t;
}

/* Build a synthetic DC_SOURCE_STATUS_1 frame from `from_priority`, as if a
 * competing node broadcast it -- the same byte layout rvc_encode.h documents
 * (byte[0]=instance, byte[1]=device priority). */
static void make_dc1_frame(uint8_t from_priority, uint8_t src, n2k_frame_t *fr)
{
    fr->id  = n2k_can_id(6, RVC_DGN_DC_SOURCE_STATUS_1, src, 0xFF);
    fr->dlc = 8;
    memset(fr->data, 0, 8);
    fr->data[0] = 0;                /* instance */
    fr->data[1] = from_priority;    /* device priority */
}

/* Full no-defer burst: CHARGER_STATUS + CHARGER_STATUS_2 + DC_SOURCE_STATUS_1
 * + DC_SOURCE_STATUS_2 = 4 single frames (rvc_sched.h's declared order). */
#define FULL_BURST_FRAMES  4

static void test_not_ready(void)
{
    rvc_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_frame_t out[16];
    rvc_sched_init(&s, RVC_OUR_DEVICE_PRIORITY);

    int n = rvc_sched_tick(&s, 0u, false, 0x24u, &t, out, 16);
    CHECK(n == 0);
    n = rvc_sched_tick(&s, 10000u, false, 0x24u, &t, out, 16);
    CHECK(n == 0);
    CHECK(s.next_charger_status_ms == 0u);   /* untouched: still "due immediately" */

    /* Nothing was lost while waiting on claim. */
    n = rvc_sched_tick(&s, 10001u, true, 0x24u, &t, out, 16);
    CHECK(n == FULL_BURST_FRAMES);
}

static void test_cadence(void)
{
    rvc_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_frame_t out[16];
    int n;
    rvc_sched_init(&s, RVC_OUR_DEVICE_PRIORITY);

    /* First call: everything due immediately, fixed order (rvc_sched.h). */
    n = rvc_sched_tick(&s, 0u, true, 0x24u, &t, out, 16);
    CHECK(n == FULL_BURST_FRAMES);
    CHECK(n2k_can_id_pgn(out[0].id) == RVC_DGN_CHARGER_STATUS);
    CHECK(n2k_can_id_pgn(out[1].id) == RVC_DGN_CHARGER_STATUS_2);
    CHECK(n2k_can_id_pgn(out[2].id) == RVC_DGN_DC_SOURCE_STATUS_1);
    CHECK(n2k_can_id_pgn(out[3].id) == RVC_DGN_DC_SOURCE_STATUS_2);
    CHECK(s.next_charger_status_ms  == 5000u);
    CHECK(s.next_charger_status2_ms ==  500u);
    CHECK(s.next_dc1_ms             ==  500u);
    CHECK(s.next_dc2_ms             ==  500u);

    /* At t=500, the three 500 ms DGNs are due again; CHARGER_STATUS (5000 ms)
     * is not. */
    n = rvc_sched_tick(&s, 500u, true, 0x24u, &t, out, 16);
    CHECK(n == 3);
    CHECK(n2k_can_id_pgn(out[0].id) == RVC_DGN_CHARGER_STATUS_2);
    CHECK(n2k_can_id_pgn(out[1].id) == RVC_DGN_DC_SOURCE_STATUS_1);
    CHECK(n2k_can_id_pgn(out[2].id) == RVC_DGN_DC_SOURCE_STATUS_2);
    CHECK(s.next_charger_status2_ms == 1000u);

    /* At t=5000, everything is due together again. */
    n = rvc_sched_tick(&s, 5000u, true, 0x24u, &t, out, 16);
    CHECK(n == FULL_BURST_FRAMES);
    CHECK(s.next_charger_status_ms == 10000u);
}

static void test_rbm_defer_and_resume(void)
{
    rvc_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_frame_t out[16];
    n2k_frame_t rx;
    int n;
    rvc_sched_init(&s, RVC_OUR_DEVICE_PRIORITY);   /* our priority: 100 */

    /* Prime: everything fires once (nothing to defer yet). */
    n = rvc_sched_tick(&s, 0u, true, 0x24u, &t, out, 16);
    CHECK(n == FULL_BURST_FRAMES);

    /* A higher-priority competitor (119, e.g. a dedicated BMS/shunt per the
     * Victron worked example rvc_sched.h cites) broadcasts DC_SOURCE_STATUS_1.
     * A LOWER-or-equal priority competitor must NOT cause deferral. */
    make_dc1_frame(80u, 0x50u, &rx);   /* lower than us: ignored */
    rvc_sched_rx(&s, 500u, rx.id, rx.data, rx.dlc);
    CHECK(!rvc_rbm_defer(&s.rbm, 500u));

    make_dc1_frame(119u, 0x50u, &rx);  /* higher than us: we must defer */
    rvc_sched_rx(&s, 500u, rx.id, rx.data, rx.dlc);
    CHECK(rvc_rbm_defer(&s.rbm, 500u));

    /* While deferring: DC_SOURCE_STATUS_1/2 are skipped, but CHARGER_STATUS_2
     * (due again at t=500 from the priming call) still fires -- the RBM
     * boundary (rvc_sched.h): election governs DC_SOURCE only. */
    n = rvc_sched_tick(&s, 500u, true, 0x24u, &t, out, 16);
    CHECK(n == 1);
    CHECK(n2k_can_id_pgn(out[0].id) == RVC_DGN_CHARGER_STATUS_2);

    /* Deferred DC DGNs' "due" timers were anchored on now_ms (500), not
     * frozen or reset to 0 -- so they are ready to fire the instant deferral
     * lifts, not stuck waiting out a stale period. */
    CHECK(s.next_dc1_ms == 500u);
    CHECK(s.next_dc2_ms == 500u);

    /* Still deferring shortly after (master seen recently, well inside
     * RVC_RBM_TIMEOUT_MS). CHARGER_STATUS_2 isn't due yet at t=600 (fired at
     * 500, period 500 -> next due 1000), so nothing at all is due. */
    n = rvc_sched_tick(&s, 600u, true, 0x24u, &t, out, 16);
    CHECK(n == 0);
    CHECK(rvc_rbm_defer(&s.rbm, 600u));

    /* Master goes quiet. After RVC_RBM_TIMEOUT_MS (1500 ms) from its last
     * broadcast (t=500), deferral lifts and the DC DGNs -- anchored on
     * now_ms while deferred -- fire immediately. */
    CHECK(rvc_rbm_defer(&s.rbm, 500u + RVC_RBM_TIMEOUT_MS - 1u));
    CHECK(!rvc_rbm_defer(&s.rbm, 500u + RVC_RBM_TIMEOUT_MS));
    n = rvc_sched_tick(&s, 500u + RVC_RBM_TIMEOUT_MS, true, 0x24u, &t, out, 16);
    CHECK(n >= 2);   /* at least both DC DGNs; CHARGER_STATUS_2 may also be due */
    {
        bool saw_dc1 = false, saw_dc2 = false;
        for (int i = 0; i < n; i++) {
            if (n2k_can_id_pgn(out[i].id) == RVC_DGN_DC_SOURCE_STATUS_1) saw_dc1 = true;
            if (n2k_can_id_pgn(out[i].id) == RVC_DGN_DC_SOURCE_STATUS_2) saw_dc2 = true;
        }
        CHECK(saw_dc1 && saw_dc2);
    }
}

static void test_rbm_timer_wraparound_safety(void)
{
    rvc_rbm_t r;
    n2k_frame_t rx;
    const uint32_t late = 0xFFFFFF00u;   /* close to the uint32 wrap point */
    rvc_rbm_init(&r, 100u);

    make_dc1_frame(119u, 0x50u, &rx);
    rvc_rbm_rx(&r, late, rx.id, rx.data, rx.dlc);
    CHECK(rvc_rbm_defer(&r, late));

    /* Still deferring just before the timeout elapses, spanning the wrap
     * (late + 1499 wraps past 0xFFFFFFFF). An unsigned-difference check
     * handles this correctly; a naive signed/absolute comparison would not. */
    CHECK(rvc_rbm_defer(&r, late + (RVC_RBM_TIMEOUT_MS - 1u)));
    /* Deferral lifts exactly at the timeout, on the far side of the wrap. */
    CHECK(!rvc_rbm_defer(&r, late + RVC_RBM_TIMEOUT_MS));
}

static void test_overflow_and_null_args(void)
{
    rvc_sched_t s, s2;
    ctrl_telemetry_t t = make_snapshot();
    n2k_frame_t out[2];
    int n;
    rvc_sched_init(&s, RVC_OUR_DEVICE_PRIORITY);

    /* Only room for 2 of the 4 due DGNs: the highest-priority ones (fixed
     * order, rvc_sched.h) get through; the rest stay "due" (untouched
     * next_*_ms), not silently dropped. */
    n = rvc_sched_tick(&s, 0u, true, 0x24u, &t, out, 2);
    CHECK(n == 2);
    CHECK(s.next_charger_status_ms  == 5000u);   /* fit: fired */
    CHECK(s.next_charger_status2_ms ==  500u);    /* fit: fired */
    CHECK(s.next_dc1_ms == 0u);                   /* didn't fit: still due */
    CHECK(s.next_dc2_ms == 0u);

    n = rvc_sched_tick(NULL, 0u, true, 0x24u, &t, out, 2);
    CHECK(n == -1);
    n = rvc_sched_tick(&s, 0u, true, 0x24u, NULL, out, 2);
    CHECK(n == -1);
    n = rvc_sched_tick(&s, 0u, true, 0x24u, &t, NULL, 2);
    CHECK(n == -1);

    rvc_sched_init(&s2, RVC_OUR_DEVICE_PRIORITY);
    n = rvc_sched_tick(&s2, 0u, true, 0x24u, &t, out, 0);
    CHECK(n == 0);
    CHECK(s2.next_charger_status_ms == 0u);

    /* rvc_rbm_defer/rvc_rbm_rx are NULL-safe. */
    CHECK(!rvc_rbm_defer(NULL, 0u));
    rvc_rbm_rx(NULL, 0u, 0u, NULL, 0);   /* must not crash */
}

void test_rvc_sched(void)
{
    int at_entry = g_checks;
    printf("\n-- RV-C Tx cadence engine + RBM election (PROJECT_PLAN §1 row 10a)\n");
    test_not_ready();
    test_cadence();
    test_rbm_defer_and_resume();
    test_rbm_timer_wraparound_safety();
    test_overflow_and_null_args();
    printf("  [RVC-SCHED] %d checks\n", g_checks - at_entry);
}
