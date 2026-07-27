/*
 * test_n2k_sched.c — NMEA 2000 Tx cadence engine (GH#18): per-PGN periods,
 * SID sharing within a cycle, fast-packet seq advancing, alert gating on
 * faults, nothing before claim-ready, bounded-output overflow behaviour.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "n2k_sched.h"
#include <string.h>

/* A steady, no-fault "healthy bulk charge" snapshot -- same shape as
 * test_n2k_encode.c's snap_bulk(), reproduced locally since each test file
 * is its own translation unit. */
static ctrl_telemetry_t make_snapshot(void)
{
    ctrl_telemetry_t t;
    memset(&t, 0, sizeof t);
    t.state             = CTRL_BULK;
    t.active_profile_id = 1;
    t.field_effort       = 0.5f;
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

static n2k_product_info_t make_product(void)
{
    n2k_product_info_t p;
    memset(&p, 0, sizeof p);
    p.n2k_version   = 2100;
    p.product_code  = 0;
    p.model_id      = "WS500-OpenFW";
    p.sw_version    = "0.1.0";
    p.model_version = "WS500";
    p.serial        = "TEST0001";
    p.cert_level    = 0;
    p.load_equiv    = 1;
    return p;
}

/* Frame budget for a full no-fault burst: 127488(1) + 127508(1) + 127506(2)
 * + 127750(1) + prop(4) + 126996(20, fixed 134-B payload) + 126998(2: two
 * empty strings + mfg_info="Mfg" -> 9-B payload -> 1+ceil((9-6)/7)=2 frames,
 * n2k_fp_split's contract in n2k_encode.h) = 31. (126983/126985 excluded:
 * no active fault.) */
#define FULL_BURST_FRAMES  31

static void test_not_ready(void)
{
    n2k_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_product_info_t prod = make_product();
    n2k_frame_t out[64];
    n2k_sched_init(&s, 1ull, &prod, NULL, NULL, "Mfg");

    int n = n2k_sched_tick(&s, 0u, false, 0x21u, &t, out, 64);
    CHECK(n == 0);
    n = n2k_sched_tick(&s, 10000u, false, 0x21u, &t, out, 64);
    CHECK(n == 0);
    CHECK(s.next_127488_ms == 0u);   /* untouched: still "due immediately" */

    /* Nothing was lost while waiting on claim -- the full due set still
     * fires the first time addr_ready goes true. */
    n = n2k_sched_tick(&s, 10001u, true, 0x21u, &t, out, 64);
    CHECK(n == FULL_BURST_FRAMES);
}

static void test_cadence_and_sid_sharing(void)
{
    n2k_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_product_info_t prod = make_product();
    n2k_frame_t out[64];
    int n;
    n2k_sched_init(&s, 0xAABBull, &prod, NULL, NULL, "Mfg");

    /* First call: everything is "due immediately" (n2k_sched.h). Frame
     * layout for this snapshot (fixed emission order): [0]=127488,
     * [1]=127508, [2..3]=127506, [4]=127750, [5..8]=prop, [9..28]=126996,
     * [29..30]=126998. */
    n = n2k_sched_tick(&s, 0u, true, 0x21u, &t, out, 64);
    CHECK(n == FULL_BURST_FRAMES);
    CHECK(s.next_127488_ms == 100u);
    CHECK(s.next_127508_ms == 1500u);
    CHECK(s.next_127506_ms == 1500u);
    CHECK(s.next_127750_ms == 1500u);
    CHECK(s.next_prop_ms   == 1500u);
    CHECK(s.next_126996_ms == 5000u);
    CHECK(s.next_126998_ms == 5000u);
    CHECK(s.next_126983_ms == 0u);   /* no fault -> never scheduled */
    CHECK(s.next_126985_ms == 0u);

    /* SID sharing within this cycle: 127508's SID (data[7]), 127506's SID
     * (frame0 data[2]) and 127750's SID (data[0]) all read back the same
     * value (n2k_encode.c's per-PGN byte layout). */
    uint8_t sid0 = out[1].data[7];
    CHECK(out[2].data[2] == sid0);
    CHECK(out[4].data[0] == sid0);

    /* Nothing due again until the rapid PGN's own 100 ms period. */
    n = n2k_sched_tick(&s, 50u, true, 0x21u, &t, out, 64);
    CHECK(n == 0);

    /* At t=100 ONLY the rapid PGN fires -- not more, not less. */
    n = n2k_sched_tick(&s, 100u, true, 0x21u, &t, out, 64);
    CHECK(n == 1);
    CHECK(n2k_can_id_pgn(out[0].id) == 127488u);
    CHECK(s.next_127488_ms == 200u);
    CHECK(s.next_127508_ms == 1500u);   /* every other PGN untouched */

    /* At t=1500 the whole 1500 ms group is due again alongside the rapid
     * PGN (which has been overdue since t=200, but fires only once, not
     * once per missed period). New cycle -> new shared SID, and the
     * fast-packet seq for 127506 has advanced from its first transmission. */
    uint8_t seq_127506_a = out[2].data[0] >> 5;
    n = n2k_sched_tick(&s, 1500u, true, 0x21u, &t, out, 64);
    CHECK(n == 1 + 1 + 2 + 1 + 4);   /* 127488+127508+127506+127750+prop */
    CHECK(s.next_127488_ms == 1600u);
    CHECK(s.next_127508_ms == 3000u);
    CHECK(s.next_126996_ms == 5000u);   /* not yet due */

    uint8_t sid1 = out[1].data[7];
    uint8_t seq_127506_b = out[2].data[0] >> 5;
    CHECK(sid1 != sid0);                       /* new snapshot -> new SID */
    CHECK(out[2].data[2] == sid1);              /* still shared within THIS cycle */
    CHECK(out[4].data[0] == sid1);
    CHECK(seq_127506_b != seq_127506_a);        /* seq changed between the two
                                                 * 127506 transmissions */
}

static void test_alert_gating_on_faults(void)
{
    n2k_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_product_info_t prod = make_product();
    n2k_frame_t out[64];
    int n;
    n2k_sched_init(&s, 0x1234ull, &prod, NULL, NULL, "Mfg");

    n = n2k_sched_tick(&s, 0u, true, 0x21u, &t, out, 64);
    CHECK(n == FULL_BURST_FRAMES);   /* no fault -> alert PGNs excluded */

    /* A fault appears: both alert PGNs fire on the very next call, timed
     * from THIS call (not from t=0) -- a fresh fault is never delayed by a
     * stale interval. */
    t.faults   = CTRL_FAULT_SELF_OVERTEMP;
    t.severity = CTRL_SEV_WARN;
    n = n2k_sched_tick(&s, 10u, true, 0x21u, &t, out, 64);
    CHECK(n > 0);
    CHECK(s.next_126983_ms == 10u + 1000u);
    CHECK(s.next_126985_ms == 10u + 2500u);
    CHECK(s.seq_126983 == 1u);
    CHECK(s.seq_126985 == 1u);

    /* Not due yet at t=500 (1000 ms / 2500 ms haven't elapsed) -- state
     * unchanged even though other PGNs (127488) do fire in this call. */
    n = n2k_sched_tick(&s, 500u, true, 0x21u, &t, out, 64);
    CHECK(n > 0);
    CHECK(s.next_126983_ms == 1010u);
    CHECK(s.next_126985_ms == 2510u);

    /* 126983 keeps to its own 1000 ms cadence once armed; 126985 (2500 ms)
     * does not fire again yet. */
    n = n2k_sched_tick(&s, 1010u, true, 0x21u, &t, out, 64);
    CHECK(n > 0);
    CHECK(s.next_126983_ms == 2010u);
    CHECK(s.seq_126983 == 2u);
    CHECK(s.next_126985_ms == 2510u);   /* still not due */

    /* Fault clears: both timers re-arm to "due now" regardless of where they
     * were in their cycle (n2k_sched.h) -- anchored on now_ms, not 0. */
    t.faults = 0;
    n = n2k_sched_tick(&s, 1100u, true, 0x21u, &t, out, 64);
    (void)n;
    CHECK(s.next_126983_ms == 1100u);
    CHECK(s.next_126985_ms == 1100u);
}

/* Regression: the alert re-arm must survive a late tick count. due() is a
 * signed-difference comparison, so re-arming to a literal 0 reads as "not
 * due" for the whole upper half of the uint32 range -- a boat left on for
 * 24.8..49.7 days would go quiet on faults exactly when it matters. */
static void test_alert_rearm_at_late_uptime(void)
{
    n2k_sched_t s;
    ctrl_telemetry_t t = make_snapshot();
    n2k_product_info_t prod = make_product();
    n2k_frame_t out[64];
    const uint32_t late = 0x90000000u;   /* ~27.8 days of HAL ticks */
    int n;
    n2k_sched_init(&s, 0x1234ull, &prod, NULL, NULL, "Mfg");

    /* Run once with no fault at a late tick -- this is what re-arms. */
    (void)n2k_sched_tick(&s, late, true, 0x21u, &t, out, 64);
    CHECK(s.next_126983_ms == late);
    CHECK(s.next_126985_ms == late);

    /* A fault raised now must alert on the very next call. */
    t.faults   = CTRL_FAULT_OVERVOLTAGE;
    t.severity = CTRL_SEV_CRITICAL;
    n = n2k_sched_tick(&s, late + 10u, true, 0x21u, &t, out, 64);
    CHECK(n > 0);
    CHECK(s.next_126983_ms == late + 10u + 1000u);   /* it fired */
    CHECK(s.next_126985_ms == late + 10u + 2500u);
    CHECK(s.seq_126983 == 1u);
}

static void test_overflow_and_null_args(void)
{
    n2k_sched_t s, s2;
    ctrl_telemetry_t t = make_snapshot();
    n2k_product_info_t prod = make_product();
    n2k_frame_t out[3];
    int n;
    n2k_sched_init(&s, 1ull, &prod, NULL, NULL, "Mfg");

    /* Only room for a few frames: the highest-priority PGN (127488) must
     * still get through, and whatever doesn't fit stays "due" (untouched
     * next_*_ms) rather than being silently dropped forever. */
    n = n2k_sched_tick(&s, 0u, true, 0x21u, &t, out, 3);
    CHECK(n > 0 && n <= 3);
    CHECK(s.next_127488_ms == 100u);   /* highest priority: always fits first */
    CHECK(s.next_126996_ms == 0u || s.next_126998_ms == 0u);   /* something
                                                                 * didn't fit */

    CHECK(n2k_sched_tick(NULL, 0u, true, 0x21u, &t, out, 3) == -1);
    CHECK(n2k_sched_tick(&s, 0u, true, 0x21u, NULL, out, 3) == -1);
    CHECK(n2k_sched_tick(&s, 0u, true, 0x21u, &t, NULL, 3) == -1);

    /* max_frames == 0: nothing written, nothing crashes, nothing marked
     * sent. */
    n2k_sched_init(&s2, 1ull, &prod, NULL, NULL, "Mfg");
    n = n2k_sched_tick(&s2, 0u, true, 0x21u, &t, out, 0);
    CHECK(n == 0);
    CHECK(s2.next_127488_ms == 0u);
}

void test_n2k_sched(void)
{
    int at_entry = g_checks;
    printf("\n-- NMEA2000 Tx cadence engine (GH#18)\n");
    test_not_ready();
    test_cadence_and_sid_sharing();
    test_alert_gating_on_faults();
    test_alert_rearm_at_late_uptime();
    test_overflow_and_null_args();
    printf("  [N2K-SCHED] %d checks\n", g_checks - at_entry);
}
