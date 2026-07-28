/*
 * test_rvc_encode.c — RV-C DGN encoders (PROJECT_PLAN §1 row 10a,
 * CAN_INTEGRATION.md §8): golden frames (arithmetic shown inline), NaN ->
 * not-available sentinels, saturating/clamping range behaviour, 29-bit CAN
 * ID composition reused from n2k_can_id() for RV-C's own DP=1 DGN space.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "rvc_encode.h"
#include <string.h>

/* A "healthy bulk charge" snapshot, same shape as test_n2k_encode.c's
 * snap_bulk() -- reproduced locally, each test file is its own translation
 * unit (house convention). */
static ctrl_telemetry_t snap_bulk(void)
{
    ctrl_telemetry_t t;
    memset(&t, 0, sizeof t);
    t.state             = CTRL_BULK;
    t.standby_reason    = CTRL_SB_OFF;
    t.active_profile_id = 2;
    t.field_effort      = 0.42f;
    t.binding           = CTRL_BIND_THERMAL;   /* not VOLTAGE_CLAMP -> "bulk" */
    t.binding_w         = 3000.0f;
    t.faults            = 0;
    t.severity          = CTRL_SEV_WARN;       /* below CTRL_SEV_FAULT */
    t.cells_series      = 16;
    t.vbat_pack_v       = 54.0f;
    t.v_cell            = 54.0f / 16.0f;
    t.amps_batt         = 60.0f;
    t.watts_batt        = 3240.0f;
    t.alt_temp_c        = 88.0f;
    t.batt_temp_c       = 22.0f;
    t.rpm               = 1500.0f;
    t.rpm_state         = CTRL_RPM_VALID;
    return t;
}

/* n2k_can_id() reuse: confirms the SAME identifier composer that N2K uses
 * correctly places RV-C's DP=1 DGNs (rvc_encode.h's file header). PDU2
 * (0x1FFFD, PF=0xFF>=240): PS is the DGN's own group-extension byte, no
 * destination substitution. PDU1 (0x1EE00, RV-C's own address-claim DGN,
 * PF=0xEE<240, see rvc_sched.h's caveat): PS carries the destination
 * address, exactly like N2K's own 59904 ISO Request. */
static void test_can_id_reuse_for_rvc(void)
{
    /* prio 6, DGN 0x1FFFD, SA 0x23: id = 6<<26 | 0x1FFFD<<8 | 0x23 */
    uint32_t id = n2k_can_id(6, RVC_DGN_DC_SOURCE_STATUS_1, 0x23, 0xFF);
    CHECK(id == 0x19FFFD23u);
    CHECK(n2k_can_id_pgn(id) == RVC_DGN_DC_SOURCE_STATUS_1);
    CHECK(n2k_can_id_src(id) == 0x23);

    /* PDU1 (RV-C's own address-claim DGN, dest 0x45 lands in the PS byte on
     * the wire but is masked back out of the extracted PGN/DGN). */
    id = n2k_can_id(6, 0x1EE00u, 0x23, 0x45);
    CHECK(id == 0x19EE4523u);
    CHECK(n2k_can_id_pgn(id) == 0x1EE00u);
}

static void test_charger_status(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;
    uint16_t raw_v, raw_i;
    float decoded_a;

    /* Golden: voltage 54.00/0.05 = 1080 = 0x0438 -> 38 04
     *         current offset-encoded (CAN_INTEGRATION.md §9.2 row 4):
     *           raw = 32000 + 60.0/0.05 = 32000+1200 = 33200 = 0x81B0 -> B0 81
     *         pct = 0.42*100/0.5 = 84 = 0x54 (§9.2 row 5: 0.5 %/bit, not 0.4)
     *         op state: BULK, binding THERMAL (not clamp) -> 2 (bulk)
     *         byte7: default-on-power-up(11) | recharge-en(11)<<2 |
     *                force-charge(1111)<<4 = 0x03|0x0C|0xF0 = 0xFF */
    const uint8_t want[8] = { 0x00, 0x38, 0x04, 0xB0, 0x81, 0x54, 0x02, 0xFF };
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19FFC723u);   /* prio 6, DGN 0x1FFC7, SA 0x23 */
    CHECK(fr.dlc == 8);
    CHECK(memcmp(fr.data, want, 8) == 0);

    /* Bug-class regression (CAN_INTEGRATION.md §9.2 row 4): a KNOWN positive
     * charging current must decode back to a positive value through the
     * documented offset formula (Table 5.3: raw*0.05 - 1600), not the old
     * plain-unsigned reading that turned +60 A into -1588 A. */
    raw_v = (uint16_t)(fr.data[1] | (fr.data[2] << 8));
    raw_i = (uint16_t)(fr.data[3] | (fr.data[4] << 8));
    CHECK(raw_v * 0.05 == 54.0);
    decoded_a = (float)(raw_i * 0.05 - 1600.0);
    CHECK(decoded_a == 60.0f);

    /* Operating-state ladder. */
    t.binding = CTRL_BIND_VOLTAGE_CLAMP;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[6] == 3);                        /* absorption */
    t.state = CTRL_FLOAT; t.binding = CTRL_BIND_STAGE;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[6] == 6);                        /* float */
    t.state = CTRL_STANDBY;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[6] == 1);                        /* do not charge */
    t.state = CTRL_BULK; t.severity = CTRL_SEV_CRITICAL;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[6] == 1);                        /* fault -> do not charge */

    /* Negative net current clamps to 0 A -- under the offset encoding that
     * is raw 0x7D00 (32000), NOT raw 0 (rvc_encode.c's inline comment: this
     * is a semantic floor on desired charge current, not an encoding-
     * limitation clamp -- the offset encoder COULD represent negative
     * values here). */
    t.severity = CTRL_SEV_WARN;
    t.amps_batt = -10.0f;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[3] == 0x00 && fr.data[4] == 0x7D);

    /* NaN inputs -> not-available (0xFFFF / 0xFF). */
    t.vbat_pack_v = NAN; t.amps_batt = NAN; t.field_effort = NAN;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[1] == 0xFF && fr.data[2] == 0xFF);   /* voltage NA */
    CHECK(fr.data[3] == 0xFF && fr.data[4] == 0xFF);   /* current NA */
    CHECK(fr.data[5] == 0xFF);                          /* pct NA */

    /* Saturation: 5000 V / 0.05 = 100000 > 0xFFFE -> pegs at 0xFFFE. Effort
     * out of [0,1] clamps at the pct field's own 0..250 range. */
    t.vbat_pack_v = 5000.0f; t.field_effort = 1.5f;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[1] == 0xFE && fr.data[2] == 0xFF);
    CHECK(fr.data[5] == 250);
    t.field_effort = -0.5f;
    CHECK(rvc_encode_charger_status(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[5] == 0);

    CHECK(rvc_encode_charger_status(NULL, 0, 0x23, &fr) == -1);
}

static void test_charger_status_2(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;

    /* Golden: voltage 54.00/0.05 = 1080 = 0x0438 -> 38 04
     *         current offset: 32000 + 60.0/0.05 = 32000+1200 = 33200 =
     *                         0x81B0 -> B0 81
     *         temp: alt_temp_c 88 + 40 = 128 = 0x80 */
    const uint8_t want[8] = { 0x00, 0x00, 0x64, 0x38, 0x04, 0xB0, 0x81, 0x80 };
    CHECK(rvc_encode_charger_status_2(&t, 0, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19FEA323u);   /* prio 6, DGN 0x1FEA3, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    /* This field DOES support negative current (unlike plain
     * CHARGER_STATUS): a large negative reading clamps at the wire field's
     * own floor (0) rather than reading as NA. */
    t.amps_batt = -5000.0f;
    CHECK(rvc_encode_charger_status_2(&t, 0, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.data[5] == 0x00 && fr.data[6] == 0x00);

    t.vbat_pack_v = NAN; t.amps_batt = NAN; t.alt_temp_c = NAN;
    CHECK(rvc_encode_charger_status_2(&t, 0, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.data[3] == 0xFF && fr.data[4] == 0xFF);
    CHECK(fr.data[5] == 0xFF && fr.data[6] == 0xFF);
    CHECK(fr.data[7] == 0xFF);

    CHECK(rvc_encode_charger_status_2(NULL, 0, 0, 100, 0x23, &fr) == -1);
}

static void test_dc_source_status_1(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;
    uint32_t raw_i;
    double physical_a;

    /* Golden: voltage 54.00/0.05 = 1080 = 0x0438 -> 38 04
     *         current: SIGN NEGATED (CAN_INTEGRATION.md §9.2 row 6 --
     *         t->amps_batt is charging-positive, this DGN's wire convention
     *         is discharge-positive), then offset-encoded:
     *           raw = 2 000 000 000 + (-60.0)/0.001
     *               = 2 000 000 000 - 60 000 = 1 999 940 000
     *               = 0x7734A9A0 -> LE A0 A9 34 77 */
    const uint8_t want[8] = { 0x00, 0x64, 0x38, 0x04, 0xA0, 0xA9, 0x34, 0x77 };
    CHECK(rvc_encode_dc_source_status_1(&t, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19FFFD23u);   /* prio 6, DGN 0x1FFFD, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    /* Bug-class regression (CAN_INTEGRATION.md §9.2 row 6): a KNOWN
     * positive charging current (t->amps_batt = +60 A) must decode, through
     * the documented Table 5.3 formula (physical = (raw - 2e9) * 0.001,
     * positive = discharge), to a NEGATIVE physical value -- i.e. the bank
     * reads as recharging, not discharging -- and negating that physical
     * value back must recover our own charging-positive +60 A. */
    raw_i = (uint32_t)fr.data[4] | ((uint32_t)fr.data[5] << 8) |
            ((uint32_t)fr.data[6] << 16) | ((uint32_t)fr.data[7] << 24);
    physical_a = ((double)raw_i - 2000000000.0) * 0.001;
    CHECK(physical_a == -60.0);
    CHECK(-physical_a == (double)t.amps_batt);

    /* NaN -> NA (all 0xFF, both the u16 voltage and the u32 current). */
    t.vbat_pack_v = NAN; t.amps_batt = NAN;
    CHECK(rvc_encode_dc_source_status_1(&t, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 0xFF && fr.data[3] == 0xFF);
    CHECK(fr.data[4] == 0xFF && fr.data[5] == 0xFF &&
         fr.data[6] == 0xFF && fr.data[7] == 0xFF);

    /* Saturation at the top of the 32-bit field: after negation, an extreme
     * NET-DISCHARGE reading (amps_batt very negative) is the case that now
     * saturates high -- pegs at 0xFFFFFFFE, one below the NA sentinel. */
    t.amps_batt = -3000000.0f;
    CHECK(rvc_encode_dc_source_status_1(&t, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.data[4] == 0xFE && fr.data[5] == 0xFF &&
         fr.data[6] == 0xFF && fr.data[7] == 0xFF);

    /* Symmetric case: after negation, an extreme NET-CHARGE reading
     * (amps_batt very positive) is the case that now floors at the wire
     * field's own bottom (0x00000000) instead -- the sign flip moved which
     * physical direction saturates which way, so both directions need
     * covering, not just the one the old (unnegated) test happened to hit. */
    t.amps_batt = 3000000.0f;
    CHECK(rvc_encode_dc_source_status_1(&t, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.data[4] == 0x00 && fr.data[5] == 0x00 &&
         fr.data[6] == 0x00 && fr.data[7] == 0x00);

    CHECK(rvc_encode_dc_source_status_1(NULL, 0, 100, 0x23, &fr) == -1);
}

static void test_dc_source_status_2(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;

    /* Golden: temp (22 + 273) / 0.03125 = 295 * 32 = 9440 = 0x24E0 -> E0 24
     * SOC / time-remaining: not in the v1 snapshot -> always NA. */
    const uint8_t want[8] = { 0x00, 0x64, 0xE0, 0x24, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(rvc_encode_dc_source_status_2(&t, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19FFFC23u);   /* prio 6, DGN 0x1FFFC, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    t.batt_temp_c = NAN;
    CHECK(rvc_encode_dc_source_status_2(&t, 0, 100, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 0xFF && fr.data[3] == 0xFF);

    CHECK(rvc_encode_dc_source_status_2(NULL, 0, 100, 0x23, &fr) == -1);
}

static void test_dc_source_status_3(void)
{
    n2k_frame_t fr;
    /* Always all-not-available (rvc_encode.h: the v1 snapshot has no SOH/
     * capacity data, matching the reference product's own behaviour). */
    const uint8_t want[8] = { 0x00, 0x64, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(rvc_encode_dc_source_status_3(0, 100, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19FFFB23u);   /* prio 6, DGN 0x1FFFB, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    CHECK(rvc_encode_dc_source_status_3(0, 100, 0x23, NULL) == -1);
}

void test_rvc_encode(void)
{
    int at_entry = g_checks;
    printf("\n-- RV-C DGN encoders (CAN_INTEGRATION.md §8, PROJECT_PLAN §1 row 10a)\n");
    test_can_id_reuse_for_rvc();
    test_charger_status();
    test_charger_status_2();
    test_dc_source_status_1();
    test_dc_source_status_2();
    test_dc_source_status_3();
    printf("  [RVC] %d checks\n", g_checks - at_entry);
}
