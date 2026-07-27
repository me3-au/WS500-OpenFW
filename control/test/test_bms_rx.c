/*
 * test_bms_rx.c — BMS/DVCC control-in (deliverable #10, CAN_INTEGRATION.md
 * §1/§3): CAN-BMS/REC/JK frame decode against golden byte patterns, sign/
 * scale handling, per-signal staleness expiry, never-seen vs went-silent,
 * the Amps->Watts CCL conversion (including degenerate-voltage cases), alarm
 * mapping, pre-disconnect, malformed/short frames, and wraparound safety at
 * a late tick count.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "bms_rx.h"
#include "arbitration.h"
#include <string.h>

/* ---- golden frame builders -----------------------------------------------
 * Each byte pattern's arithmetic is shown at the call site (below), not
 * hidden in a helper -- the point is that the test itself demonstrates the
 * scale factor bms_rx.c claims to decode. */

static void test_decode_351_limits(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    /* CVL = 57.6 V -> raw = 576 (0.1 V/bit) = 0x0240 LE -> {0x40, 0x02}
     * CCL = 40.0 A -> raw = 400 (0.1 A/bit) = 0x0190 LE -> {0x90, 0x01}
     * DCL = 100.0 A -> raw = 1000 (0.1 A/bit) = 0x03E8 LE -> {0xE8, 0x03} */
    uint8_t d[6] = {0x40, 0x02, 0x90, 0x01, 0xE8, 0x03};
    bms_rx_frame(&b, 1000u, BMS_RX_ID_LIMITS, d, sizeof d);

    CHECK(b.limits.state == BMS_SIG_FRESH);
    CHECK(b.limits.last_rx_ms == 1000u);
    CHECK_FEQ(b.cvl_v, 57.6, 1e-4);
    CHECK_FEQ(b.ccl_a, 40.0, 1e-4);
    CHECK_FEQ(b.dcl_a, 100.0, 1e-4);

    /* Snapshot at a real pack voltage: ccl_w = 40.0 A x 48.0 V = 1920.0 W. */
    bms_rx_snapshot(&b, 1000u, 48.0f, &s);
    CHECK_FEQ(s.ccl_w, 1920.0, 1e-3);
    CHECK_FEQ(s.cvl_v, 57.6, 1e-4);
    CHECK(!s.lost);
}

static void test_decode_351_negative_ccl(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    /* CVL = 54.0 V -> raw 540 = 0x021C LE -> {0x1C, 0x02}
     * CCL = -10.0 A (malformed/implausible for a charge limit) -> raw = -100
     *   -> two's complement 16-bit: 65536 - 100 = 65436 = 0xFF9C LE ->
     *   {0x9C, 0xFF}
     * DCL = 0.0 A -> {0x00, 0x00} */
    uint8_t d[6] = {0x1C, 0x02, 0x9C, 0xFF, 0x00, 0x00};
    bms_rx_frame(&b, 0u, BMS_RX_ID_LIMITS, d, sizeof d);

    CHECK_FEQ(b.ccl_a, -10.0, 1e-4);   /* decode itself is honest about sign */

    /* But the Watts conversion refuses to trust a negative CCL as a real
     * ceiling -- see bms_rx.c's ccl_to_watts() doc comment. */
    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);
}

static void test_decode_355_soc_soh(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    /* SOC = 76 % -> raw 76 (1 %/bit, unsigned) = 0x004C LE -> {0x4C, 0x00}
     * SOH = 98 % -> raw 98 = 0x0062 LE -> {0x62, 0x00} */
    uint8_t d[4] = {0x4C, 0x00, 0x62, 0x00};
    bms_rx_frame(&b, 500u, BMS_RX_ID_SOC, d, sizeof d);

    CHECK(b.soc.state == BMS_SIG_FRESH);
    CHECK_FEQ(b.soc_pct, 76.0, 1e-6);
    CHECK_FEQ(b.soh_pct, 98.0, 1e-6);

    bms_rx_snapshot(&b, 500u, 48.0f, &s);
    CHECK_FEQ(s.soc_pct, 76.0, 1e-6);
    CHECK(s.soc_trusted);
}

static void test_decode_356_status(void)
{
    bms_rx_t b;
    bms_rx_init(&b);

    /* V = 52.30 V -> raw 5230 (0.01 V/bit) = 0x146E LE -> {0x6E, 0x14}
     * I = 15.0 A (+ = charging) -> raw 150 (0.1 A/bit) = 0x0096 LE ->
     *   {0x96, 0x00}
     * T = -5.0 C -> raw -50 (0.1 C/bit) -> two's complement:
     *   65536 - 50 = 65486 = 0xFFCE LE -> {0xCE, 0xFF} */
    uint8_t d[6] = {0x6E, 0x14, 0x96, 0x00, 0xCE, 0xFF};
    bms_rx_frame(&b, 0u, BMS_RX_ID_STATUS, d, sizeof d);

    CHECK(b.status.state == BMS_SIG_FRESH);
    CHECK_FEQ(b.pack_v, 52.30, 1e-3);
    CHECK_FEQ(b.pack_i_a, 15.0, 1e-4);
    CHECK_FEQ(b.pack_temp_c, -5.0, 1e-4);
}

static void test_decode_35a_alarms(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    /* alarm bytes[0:1] = {0x01, 0x00} -> at least one alarm bit set.
     * warning bytes[2:3] = {0x00, 0x02} -> at least one warning bit set. */
    uint8_t d[4] = {0x01, 0x00, 0x00, 0x02};
    bms_rx_frame(&b, 0u, BMS_RX_ID_ALARM, d, sizeof d);
    CHECK(b.alarm.state == BMS_SIG_FRESH);
    CHECK(b.alarm_active);
    CHECK(b.warning_active);

    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK(s.alarm_active);
    CHECK(s.warning_active);

    /* All-zero alarm frame -> neither flag. */
    bms_rx_init(&b);
    uint8_t clear[4] = {0, 0, 0, 0};
    bms_rx_frame(&b, 0u, BMS_RX_ID_ALARM, clear, sizeof clear);
    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK(!s.alarm_active);
    CHECK(!s.warning_active);
}

/* A fresh, active alarm forces the CCL ceiling to 0 W regardless of what the
 * CCL field itself says -- the "BMS is alarming AND still reporting a
 * nonzero CCL" case bms_rx.c's file comment calls out explicitly. */
static void test_alarm_forces_ccl_zero(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};   /* CCL=40.0A */
    uint8_t alarm[4]  = {0x04, 0x00, 0x00, 0x00};               /* alarm set */
    bms_rx_frame(&b, 0u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_frame(&b, 0u, BMS_RX_ID_ALARM, alarm, sizeof alarm);

    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK(s.alarm_active);
    CHECK(s.ccl_w == 0.0f);   /* not 1920 W -- alarm wins */
}

/* Alarm-then-silence must NOT release the 0 W stop on the timeout: a BMS that
 * opens its protection contactor can lose its own supply and stop talking, so
 * "alarming" followed by silence is the signature of the alarm being ACTED ON.
 * Only a live BMS saying "clear" clears it. */
static void test_alarm_stale_keeps_ccl_zero(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};   /* CCL=40.0A */
    uint8_t alarm[4]  = {0x04, 0x00, 0x00, 0x00};               /* alarm set */
    uint8_t clear[4]  = {0x00, 0x00, 0x00, 0x00};
    bms_rx_frame(&b, 1000u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_frame(&b, 1000u, BMS_RX_ID_ALARM,  alarm,  sizeof alarm);

    bms_rx_snapshot(&b, 1000u, 48.0f, &s);
    CHECK(s.ccl_w == 0.0f);

    /* Everything goes stale. The stop must survive it. */
    bms_rx_snapshot(&b, 1000u + BMS_RX_SIGNAL_TIMEOUT_MS + 1u, 48.0f, &s);
    CHECK(!s.alarm_active);        /* no longer FRESH -- honest about that */
    CHECK(s.ccl_w == 0.0f);        /* ...but still stopped */
    CHECK(s.lost);                 /* and the fault is raised */

    /* Far later, still stopped: the latch is not a grace period. */
    bms_rx_snapshot(&b, 1000u + 10u * BMS_RX_SIGNAL_TIMEOUT_MS, 48.0f, &s);
    CHECK(s.ccl_w == 0.0f);

    /* A live BMS saying "clear" is the only thing that releases it. */
    bms_rx_frame(&b, 60000u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_frame(&b, 60000u, BMS_RX_ID_ALARM,  clear,  sizeof clear);
    bms_rx_snapshot(&b, 60000u, 48.0f, &s);
    CHECK(!s.alarm_active);
    CHECK(!s.lost);
    CHECK_FEQ(s.ccl_w, 40.0 * 48.0, 1e-3);   /* charging permitted again */
}

/* An all-0xFF frame is a common "not available" fill in this very frame
 * family. Nothing may read it as a confident value — least of all SOC, where
 * 0xFFFF would decode as a trusted 65535 % (silently "completely full"). */
static void test_all_ff_frames_are_not_data(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    uint8_t ff[8] = {0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF};
    bms_rx_init(&b);

    bms_rx_frame(&b, 1000u, BMS_RX_ID_LIMITS, ff, sizeof ff);
    bms_rx_frame(&b, 1000u, BMS_RX_ID_SOC,    ff, sizeof ff);
    bms_rx_frame(&b, 1000u, BMS_RX_ID_STATUS, ff, sizeof ff);
    bms_rx_snapshot(&b, 1000u, 48.0f, &s);

    /* CCL decodes as -0.1 A (i16) -> not a ceiling at all, never a huge one. */
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);
    /* SOC 65535 % is outside 0..100 -> unknown, NOT clamped to a confident
     * 100 % (which would satisfy the T2b exit and skip-bulk-when-full). */
    CHECK(!s.soc_trusted);
    CHECK(s.soc_pct == -1.0f);
}

/* SOC domain guard, both ends, plus the ordinary in-range case. */
static void test_soc_out_of_range_is_untrusted(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    uint8_t soc[4];
    bms_rx_init(&b);

    /* 101 % -- just outside, must not be trusted or clamped. */
    soc[0] = 101u; soc[1] = 0u; soc[2] = 100u; soc[3] = 0u;
    bms_rx_frame(&b, 100u, BMS_RX_ID_SOC, soc, sizeof soc);
    bms_rx_snapshot(&b, 100u, 48.0f, &s);
    CHECK(!s.soc_trusted && s.soc_pct == -1.0f);

    /* 0 % is legitimate (a flat pack) and MUST stay trusted -- it is the
     * value that drives soc_revert_pct, so silently discarding it would
     * break revert, not just harden it. */
    soc[0] = 0u; soc[1] = 0u;
    bms_rx_frame(&b, 200u, BMS_RX_ID_SOC, soc, sizeof soc);
    bms_rx_snapshot(&b, 200u, 48.0f, &s);
    CHECK(s.soc_trusted && s.soc_pct == 0.0f);

    /* 100 % likewise. */
    soc[0] = 100u; soc[1] = 0u;
    bms_rx_frame(&b, 300u, BMS_RX_ID_SOC, soc, sizeof soc);
    bms_rx_snapshot(&b, 300u, 48.0f, &s);
    CHECK(s.soc_trusted && s.soc_pct == 100.0f);
}

/* Went-silent then came back: the fault must clear and the ceiling return. */
static void test_stale_then_recover(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};   /* CCL=40.0A */
    bms_rx_init(&b);

    bms_rx_frame(&b, 1000u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_snapshot(&b, 1000u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK_FEQ(s.ccl_w, 40.0 * 48.0, 1e-3);

    bms_rx_snapshot(&b, 1000u + BMS_RX_SIGNAL_TIMEOUT_MS + 1u, 48.0f, &s);
    CHECK(s.lost);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);   /* stale never reads as fresh */

    bms_rx_frame(&b, 20000u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_snapshot(&b, 20000u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK_FEQ(s.ccl_w, 40.0 * 48.0, 1e-3);
}

static void test_malformed_short_frames(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    uint8_t short5[5] = {0x40, 0x02, 0x90, 0x01, 0xE8};   /* 0x351 needs 6 */
    bms_rx_frame(&b, 100u, BMS_RX_ID_LIMITS, short5, sizeof short5);
    CHECK(b.limits.state == BMS_SIG_NEVER);   /* dropped whole, not partial */

    uint8_t short3[3] = {0x4C, 0x00, 0x62};   /* 0x355 needs 4 */
    bms_rx_frame(&b, 100u, BMS_RX_ID_SOC, short3, sizeof short3);
    CHECK(b.soc.state == BMS_SIG_NEVER);

    uint8_t short5b[5] = {0x6E, 0x14, 0x96, 0x00, 0xCE};   /* 0x356 needs 6 */
    bms_rx_frame(&b, 100u, BMS_RX_ID_STATUS, short5b, sizeof short5b);
    CHECK(b.status.state == BMS_SIG_NEVER);

    uint8_t short3b[3] = {0x01, 0x00, 0x00};   /* 0x35A needs 4 */
    bms_rx_frame(&b, 100u, BMS_RX_ID_ALARM, short3b, sizeof short3b);
    CHECK(b.alarm.state == BMS_SIG_NEVER);

    /* NULL data / driver: no crash, nothing decoded. */
    bms_rx_frame(&b, 100u, BMS_RX_ID_LIMITS, NULL, 6u);
    bms_rx_frame(NULL, 100u, BMS_RX_ID_LIMITS, short5, sizeof short5);

    /* An id this driver doesn't know: ignored. */
    uint8_t junk[8] = {1, 2, 3, 4, 5, 6, 7, 8};
    bms_rx_frame(&b, 100u, 0x123u, junk, sizeof junk);
    CHECK(b.limits.state == BMS_SIG_NEVER);

    /* Everything above was rejected -- snapshot must show "never seen", not
     * a fault. */
    bms_rx_snapshot(&b, 100u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);
}

/* Never-seen (no BMS wired) vs went-silent (BMS was talking, then stopped)
 * must read differently: only the second is a fault. */
static void test_never_seen_vs_went_silent(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    /* Never seen: not a fault, ceilings simply inactive. */
    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);
    CHECK(s.soc_pct == -1.0f);
    CHECK(!s.soc_trusted);

    /* BMS starts talking. */
    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};   /* CCL=40A */
    bms_rx_frame(&b, 1000u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_snapshot(&b, 1000u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK_FEQ(s.ccl_w, 1920.0, 1e-3);

    /* Just under the timeout: still fresh. */
    bms_rx_snapshot(&b, 1000u + BMS_RX_SIGNAL_TIMEOUT_MS - 1u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK_FEQ(s.ccl_w, 1920.0, 1e-3);

    /* At the timeout: gone stale -- THIS is the fault, distinct from never
     * having been seen at all. */
    bms_rx_snapshot(&b, 1000u + BMS_RX_SIGNAL_TIMEOUT_MS, 48.0f, &s);
    CHECK(s.lost);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);   /* falls back, does not free-run */
}

/* Regression: staleness must survive a late tick count. sig_age() uses an
 * unsigned-difference (elapsed) comparison rather than a signed deadline
 * comparison specifically so this holds across the whole uint32_t range --
 * the same lesson n2k_sched.c's alert re-arm test already encodes for this
 * codebase (test_alert_rearm_at_late_uptime). */
static void test_wraparound_safety_at_late_uptime(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    const uint32_t late = 0x90000000u;   /* ~27.8 days of HAL ticks */
    bms_rx_init(&b);

    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};   /* CCL=40A */
    bms_rx_frame(&b, late, BMS_RX_ID_LIMITS, limits, sizeof limits);

    bms_rx_snapshot(&b, late + 100u, 48.0f, &s);
    CHECK(!s.lost);
    CHECK_FEQ(s.ccl_w, 1920.0, 1e-3);

    bms_rx_snapshot(&b, late + BMS_RX_SIGNAL_TIMEOUT_MS, 48.0f, &s);
    CHECK(s.lost);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);
}

/* Degenerate conversion-voltage cases: NaN, zero, negative -- none of them
 * may produce a finite Watts ceiling that is more permissive than "no
 * ceiling from the BMS at all". */
static void test_degenerate_voltage(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};   /* CCL=40A */
    bms_rx_frame(&b, 0u, BMS_RX_ID_LIMITS, limits, sizeof limits);

    bms_rx_snapshot(&b, 0u, NAN, &s);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);

    bms_rx_snapshot(&b, 0u, 0.0f, &s);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);

    bms_rx_snapshot(&b, 0u, -48.0f, &s);
    CHECK(s.ccl_w == CTRL_CEILING_INACTIVE);

    /* A sane voltage after a bad one still converts correctly -- the driver
     * doesn't latch a "poisoned" state from one bad snapshot call. */
    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK_FEQ(s.ccl_w, 1920.0, 1e-3);
}

/* pre_disconnect: no verified bit exists in the decoded frame set (bms_rx.h
 * [SPEC-GAP]) -- must always read false, not a guessed value, regardless of
 * how much other BMS traffic is flowing. */
static void test_pre_disconnect_always_false(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    bms_rx_init(&b);

    uint8_t limits[6] = {0x1C, 0x02, 0x90, 0x01, 0x00, 0x00};
    uint8_t soc[4]    = {0x4C, 0x00, 0x62, 0x00};
    uint8_t alarm[4]  = {0x01, 0x00, 0x00, 0x00};
    bms_rx_frame(&b, 0u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_frame(&b, 0u, BMS_RX_ID_SOC, soc, sizeof soc);
    bms_rx_frame(&b, 0u, BMS_RX_ID_ALARM, alarm, sizeof alarm);

    bms_rx_snapshot(&b, 0u, 48.0f, &s);
    CHECK(!s.pre_disconnect);
}

/* End-to-end sanity: a fresh BMS ceiling actually binds arbitration when it
 * is the lowest active ceiling, and is reported as CTRL_BIND_BMS -- the
 * consumer contract main.c relies on (arbitration.c already has bms_ccl_w
 * wired into its min(), unmodified by this deliverable). */
static void test_binds_arbitration(void)
{
    bms_rx_t b;
    bms_snapshot_t s;
    ctrl_ceilings_t c = test_ceil_none();
    bms_rx_init(&b);

    uint8_t limits[6] = {0x1C, 0x02, 0x64, 0x00, 0x00, 0x00};   /* CCL=10.0A */
    bms_rx_frame(&b, 0u, BMS_RX_ID_LIMITS, limits, sizeof limits);
    bms_rx_snapshot(&b, 0u, 48.0f, &s);   /* ccl_w = 10.0 x 48.0 = 480 W */
    c.bms_ccl_w = s.ccl_w;

    ctrl_arb_t arb = ctrl_arbitrate(2000.0f, &c);   /* stage ceiling 2000 W */
    CHECK(arb.src == CTRL_BIND_BMS);
    CHECK_FEQ(arb.watts, 480.0, 1e-3);
}

void test_bms_rx(void)
{
    int at_entry = g_checks;
    printf("\n-- BMS/DVCC control-in (deliverable #10)\n");
    test_decode_351_limits();
    test_decode_351_negative_ccl();
    test_decode_355_soc_soh();
    test_decode_356_status();
    test_decode_35a_alarms();
    test_alarm_forces_ccl_zero();
    test_alarm_stale_keeps_ccl_zero();
    test_all_ff_frames_are_not_data();
    test_soc_out_of_range_is_untrusted();
    test_stale_then_recover();
    test_malformed_short_frames();
    test_never_seen_vs_went_silent();
    test_wraparound_safety_at_late_uptime();
    test_degenerate_voltage();
    test_pre_disconnect_always_false();
    test_binds_arbitration();
    printf("  [BMS-RX] %d checks\n", g_checks - at_entry);
}
