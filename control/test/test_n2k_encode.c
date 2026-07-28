/*
 * test_n2k_encode.c — NMEA 2000 PGN encoders (CAN_INTEGRATION.md §2 Tx set):
 * golden frames (arithmetic shown inline), NaN → not-available sentinels,
 * saturating range clamps, fast-packet split/reassembly round trip, 29-bit
 * CAN ID composition, SID counter.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "n2k_encode.h"
#include <string.h>

/* Reference fast-packet reassembler (test-side inverse of n2k_fp_split):
 * validates the per-frame sequence/counter bytes while collecting payload. */
static int fp_reassemble(const n2k_frame_t *fr, int n, uint8_t *out, size_t *len)
{
    size_t total, off = 0;
    int f;
    if (n < 1) return -1;
    total = fr[0].data[1];
    for (f = 0; f < n; f++) {
        size_t di = (f == 0) ? 2u : 1u;
        if ((fr[f].data[0] & 0x1Fu) != (unsigned)f) return -1;     /* counter */
        if ((fr[f].data[0] >> 5) != (fr[0].data[0] >> 5)) return -1; /* seq  */
        while (di < 8 && off < total) out[off++] = fr[f].data[di++];
    }
    *len = total;
    return (off == total) ? 0 : -1;
}

/* A fully-populated "healthy bulk charge" snapshot shared by the goldens. */
static ctrl_telemetry_t snap_bulk(void)
{
    ctrl_telemetry_t t;
    memset(&t, 0, sizeof t);
    t.state             = CTRL_BULK;
    t.standby_reason    = CTRL_SB_OFF;
    t.active_profile_id = 2;
    t.field_effort      = 0.42f;
    t.binding           = CTRL_BIND_THERMAL;
    t.binding_w         = 3000.0f;
    t.faults            = CTRL_FAULT_SELF_OVERTEMP;   /* WARN */
    t.severity          = CTRL_SEV_WARN;
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

static void test_can_id(void)
{
    /* PDU2 broadcast: prio 6, PGN 127508 = 0x1F214, SA 0x23:
     * id = 6<<26 | 0x1F214<<8 | 0x23 = 0x18000000|0x1F21400|0x23 = 0x19F21423 */
    uint32_t id = n2k_can_id(6, 127508u, 0x23, 0xFF);
    CHECK(id == 0x19F21423u);
    CHECK(n2k_can_id_prio(id) == 6);
    CHECK(n2k_can_id_pgn(id) == 127508u);
    CHECK(n2k_can_id_src(id) == 0x23);

    /* PDU1 (PF < 240): PGN 59904 = 0xEA00, dest 0x45 lands in the PS byte
     * on the wire but is masked back out of the extracted PGN. */
    id = n2k_can_id(6, 59904u, 0x23, 0x45);
    CHECK(id == 0x18EA4523u);
    CHECK(n2k_can_id_pgn(id) == 59904u);
}

static void test_sid(void)
{
    uint8_t sid = 0;
    CHECK(n2k_sid_next(&sid) == 0);   /* returns current, then advances */
    CHECK(n2k_sid_next(&sid) == 1);
    sid = 252;                        /* top of the valid range... */
    CHECK(n2k_sid_next(&sid) == 252);
    CHECK(sid == 0);                  /* ...wraps to 0, never 253/254/255 */
    sid = 255;                        /* out-of-range seed heals to 0 */
    CHECK(n2k_sid_next(&sid) == 0);
}

static void test_fp_split(void)
{
    n2k_frame_t fr[N2K_FP_MAX_FRAMES];
    uint8_t payload[N2K_FP_MAX_PAYLOAD], back[N2K_FP_MAX_PAYLOAD];
    size_t i, blen = 0;
    int n;

    for (i = 0; i < sizeof payload; i++) payload[i] = (uint8_t)(i * 7u + 3u);

    /* 6 bytes fit the first frame alone; 7 need a second. */
    CHECK(n2k_fp_split(0x123u, 0, payload, 6, fr, 32) == 1);
    CHECK(n2k_fp_split(0x123u, 0, payload, 7, fr, 32) == 2);

    /* Max-size round trip: 223 B = 6 + 31*7 → exactly 32 frames. */
    n = n2k_fp_split(0x19F01234u, 5, payload, sizeof payload, fr, 32);
    CHECK(n == 32);
    CHECK(fr[0].data[0] == 0xA0);         /* seq 5 << 5 | frame 0 */
    CHECK(fr[0].data[1] == 223);          /* total length byte */
    CHECK(fr[31].data[0] == 0xBF);        /* seq 5 << 5 | frame 31 */
    CHECK(fr[31].id == 0x19F01234u && fr[31].dlc == 8);
    CHECK(fp_reassemble(fr, n, back, &blen) == 0);
    CHECK(blen == sizeof payload);
    CHECK(memcmp(back, payload, sizeof payload) == 0);

    /* Short-payload tail pads with 0xFF: 8 B → frame 1 carries bytes 6..7
     * at data[1..2], padding fills data[3..7]. */
    n = n2k_fp_split(0x1u, 2, payload, 8, fr, 32);
    CHECK(n == 2);
    CHECK(fr[1].data[1] == payload[6] && fr[1].data[2] == payload[7]);
    CHECK(fr[1].data[3] == 0xFF && fr[1].data[7] == 0xFF);

    /* Errors: oversize payload, zero payload, too few output frames. */
    CHECK(n2k_fp_split(0x1u, 0, payload, 224, fr, 32) == -1);
    CHECK(n2k_fp_split(0x1u, 0, payload, 0, fr, 32) == -1);
    CHECK(n2k_fp_split(0x1u, 0, payload, 20, fr, 2) == -1);
}

static void test_127508(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;

    /* Golden: V = 54.00 / 0.01 = 5400 = 0x1518 → 18 15
     *         I = 60.0  / 0.1  =  600 = 0x0258 → 58 02
     *         T = (22 + 273.15) / 0.01 = 29515 = 0x734B → 4B 73
     *         [instance][V lo hi][I lo hi][T lo hi][SID] */
    const uint8_t want[8] = { 0x00, 0x18, 0x15, 0x58, 0x02, 0x4B, 0x73, 0x07 };
    CHECK(n2k_encode_127508(&t, 0, 7, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19F21423u && fr.dlc == 8);   /* prio 6, PGN 127508, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    /* NaN inputs → not-available sentinels (s16 0x7FFF, u16 0xFFFF). */
    t.vbat_pack_v = NAN; t.amps_batt = NAN; t.batt_temp_c = NAN;
    CHECK(n2k_encode_127508(&t, 0, 7, 0x23, &fr) == 1);
    CHECK(fr.data[1] == 0xFF && fr.data[2] == 0x7F);   /* voltage NA */
    CHECK(fr.data[3] == 0xFF && fr.data[4] == 0x7F);   /* current NA */
    CHECK(fr.data[5] == 0xFF && fr.data[6] == 0xFF);   /* temp NA */

    /* Saturating clamps: 400 V / 0.01 = 40000 > 32764 → 0x7FFC;
     * -4000 A / 0.1 = -40000 < -32768 → 0x8000;
     * 1000 °C → 127315 counts > 65532 → 0xFFFC. */
    t.vbat_pack_v = 400.0f; t.amps_batt = -4000.0f; t.batt_temp_c = 1000.0f;
    CHECK(n2k_encode_127508(&t, 0, 7, 0x23, &fr) == 1);
    CHECK(fr.data[1] == 0xFC && fr.data[2] == 0x7F);
    CHECK(fr.data[3] == 0x00 && fr.data[4] == 0x80);
    CHECK(fr.data[5] == 0xFC && fr.data[6] == 0xFF);

    CHECK(n2k_encode_127508(NULL, 0, 7, 0x23, &fr) == -1);
}

static void test_127506(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr[4];
    /* 11-byte payload → 2 frames. Golden, seq 3:
     * payload = [SID=07][inst=00][type=00 battery][SOC FF][SOH FF]
     *           [time FFFF][ripple FFFF][capacity FFFF]
     * frame0 = [3<<5|0=0x60][len=0x0B][07 00 00 FF FF FF]
     * frame1 = [3<<5|1=0x61][FF FF FF FF FF] + 2 pad FF */
    const uint8_t f0[8] = { 0x60, 0x0B, 0x07, 0x00, 0x00, 0xFF, 0xFF, 0xFF };
    const uint8_t f1[8] = { 0x61, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(n2k_encode_127506(&t, 0, 7, 0x23, 3, fr, 4) == 2);
    CHECK(fr[0].id == 0x19F21223u);   /* prio 6, PGN 127506 = 0x1F212, SA 0x23 */
    CHECK(memcmp(fr[0].data, f0, 8) == 0);
    CHECK(memcmp(fr[1].data, f1, 8) == 0);
}

static void test_127488(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;
    /* Golden: RPM = 1500 / 0.25 = 6000 = 0x1770 → 70 17
     * [inst][rpm lo hi][boost NA FFFF][trim NA 7F][reserved FF FF] */
    const uint8_t want[8] = { 0x00, 0x70, 0x17, 0xFF, 0xFF, 0x7F, 0xFF, 0xFF };
    CHECK(n2k_encode_127488(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.id == 0x09F20023u);      /* prio 2, PGN 127488 = 0x1F200, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    /* Stale/lost tach must transmit NA, not the last frozen value. */
    t.rpm_state = CTRL_RPM_STALE;
    CHECK(n2k_encode_127488(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[1] == 0xFF && fr.data[2] == 0xFF);

    /* Over-range RPM saturates: 20000 / 0.25 = 80000 → 0xFFFC. */
    t.rpm_state = CTRL_RPM_VALID; t.rpm = 20000.0f;
    CHECK(n2k_encode_127488(&t, 0, 0x23, &fr) == 1);
    CHECK(fr.data[1] == 0xFC && fr.data[2] == 0xFF);
}

static void test_127750(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr;
    t.faults = 0;                     /* healthy: no overtemp fault bits */
    t.severity = CTRL_SEV_INFO;

    /* Bulk with the thermal governor binding → op 3, temp-state 1 (warning),
     * overload/low-DC 0, ramping NA (3): bits = 01 | 11<<6 = 0xC1.
     * [SID=07][conn=01][op=03][bits=C1][FF FF FF FF] */
    const uint8_t want[8] = { 0x07, 0x01, 0x03, 0xC1, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(n2k_encode_127750(&t, 1, 7, 0x23, &fr) == 1);
    CHECK(fr.id == 0x19F30623u);      /* prio 6, PGN 127750 = 0x1F306, SA 0x23 */
    CHECK(memcmp(fr.data, want, 8) == 0);

    /* State ladder: CV clamp in BULK reads as "absorption" (4). */
    t.binding = CTRL_BIND_VOLTAGE_CLAMP;
    CHECK(n2k_encode_127750(&t, 1, 7, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 4 && (fr.data[3] & 0x3) == 0);  /* temp ok again */

    /* deliverable #10: a BMS-lowered CVL is the same "absorption" condition
     * to the GX display -- BULK is still holding at a voltage clamp, just a
     * lower one (control.h's CTRL_BIND_BMS_CVL doc comment). */
    t.binding = CTRL_BIND_BMS_CVL;
    CHECK(n2k_encode_127750(&t, 1, 7, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 4);

    t.state = CTRL_FLOAT; t.binding = CTRL_BIND_STAGE;
    CHECK(n2k_encode_127750(&t, 1, 7, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 5);
    t.state = CTRL_STANDBY;
    CHECK(n2k_encode_127750(&t, 1, 7, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 0);

    /* Fault severity dominates the state mapping; hard overtemp → temp 2. */
    t.state = CTRL_BULK;
    t.severity = CTRL_SEV_CRITICAL;
    t.faults |= CTRL_FAULT_BATT_HIGHTEMP;
    CHECK(n2k_encode_127750(&t, 1, 7, 0x23, &fr) == 1);
    CHECK(fr.data[2] == 2 && (fr.data[3] & 0x3) == 2);
}

static void test_alerts(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_alert_t a;
    n2k_frame_t fr[8];
    uint8_t back[N2K_FP_MAX_PAYLOAD];
    size_t blen = 0;
    int n;

    /* Highest severity wins: OVERVOLTAGE (bit 0, CRITICAL) beats
     * SELF_OVERTEMP (bit 4, WARN) → id = 0+1, type 1 (emergency). */
    t.faults = CTRL_FAULT_OVERVOLTAGE | CTRL_FAULT_SELF_OVERTEMP;
    CHECK(n2k_alert_from_telemetry(&t, 0x1122334455667788ull, &a) == 1);
    CHECK(a.alert_id == 1 && a.type == 1 && a.state == 2 && a.category == 1);

    /* 126983 golden (28-B payload → 5 frames), seq 1:
     * [type|cat<<4 = 0x11][sys 00][sub 00][id 0100]
     * [NAME LE 88 77 66 55 44 33 22 11][inst 00][idx 00][occ 01]
     * [flags C0][ack NAME FF×8][trig|thr<<4 = 0x12][prio 01][state 02] */
    n = n2k_encode_126983(&a, 0x23, 1, fr, 8);
    CHECK(n == 5);
    CHECK(fr[0].id == n2k_can_id(2, 126983u, 0x23, 0xFF));
    CHECK(fr[0].data[0] == 0x20 && fr[0].data[1] == 28);   /* seq 1, len 28 */
    CHECK(fr[0].data[2] == 0x11);                          /* type|cat */
    CHECK(fp_reassemble(fr, n, back, &blen) == 0 && blen == 28);
    {
        const uint8_t want[28] = {
            0x11, 0x00, 0x00, 0x01, 0x00,
            0x88, 0x77, 0x66, 0x55, 0x44, 0x33, 0x22, 0x11,
            0x00, 0x00, 0x01, 0xC0,
            0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF,
            0x12, 0x01, 0x02
        };
        CHECK(memcmp(back, want, 28) == 0);
    }

    /* 126985: identity + language + [len+2][0x01 ASCII][chars] strings. */
    n = n2k_encode_126985(&a, n2k_alert_text(CTRL_FAULT_OVERVOLTAGE), NULL,
                          0x23, 2, fr, 8);
    CHECK(n > 0);
    CHECK(fp_reassemble(fr, n, back, &blen) == 0);
    {
        size_t dl = strlen(n2k_alert_text(CTRL_FAULT_OVERVOLTAGE));
        CHECK(blen == 17u + (dl + 2u) + 2u);   /* ident+lang, desc, empty loc */
        CHECK(back[16] == 0);                  /* language: English (US) */
        CHECK(back[17] == (uint8_t)(dl + 2u) && back[18] == 0x01);
        CHECK(memcmp(&back[19], n2k_alert_text(CTRL_FAULT_OVERVOLTAGE), dl) == 0);
        CHECK(back[19 + dl] == 2 && back[20 + dl] == 0x01); /* empty location */
    }

    /* No faults → nothing to send; unknown bit → fallback text. */
    t.faults = 0;
    CHECK(n2k_alert_from_telemetry(&t, 0, &a) == 0);
    CHECK(strcmp(n2k_alert_text(1u << 30), "unknown fault") == 0);
    CHECK(strcmp(n2k_alert_text(CTRL_FAULT_LOST_BMS),
                 "Lost BMS communication - limp home") == 0);
}

static void test_product_config_info(void)
{
    n2k_frame_t fr[N2K_FP_MAX_FRAMES];
    uint8_t back[N2K_FP_MAX_PAYLOAD];
    size_t blen = 0;
    int n;
    n2k_product_info_t p;
    memset(&p, 0, sizeof p);
    p.n2k_version   = 2101;       /* 0x0835 → 35 08 */
    p.product_code  = 12345;      /* 0x3039 → 39 30 */
    p.model_id      = "WS500-OpenFW";
    p.sw_version    = "0.1.0";
    p.model_version = "WS500";
    p.serial        = "0001";
    p.cert_level    = 0;
    p.load_equiv    = 1;

    /* 134-B payload: 2+2 + 4×32 + 1+1 → 1 + ceil(128/7) = 20 frames. */
    n = n2k_encode_126996(&p, 0x23, 0, fr, N2K_FP_MAX_FRAMES);
    CHECK(n == 20);
    CHECK(fp_reassemble(fr, n, back, &blen) == 0 && blen == 134);
    CHECK(back[0] == 0x35 && back[1] == 0x08);
    CHECK(back[2] == 0x39 && back[3] == 0x30);
    CHECK(memcmp(&back[4], "WS500-OpenFW", 12) == 0);
    CHECK(back[16] == ' ' && back[35] == ' ');   /* space padding to 32 */
    CHECK(memcmp(&back[36], "0.1.0", 5) == 0);   /* sw field at 4+32 */
    CHECK(back[132] == 0 && back[133] == 1);     /* cert, LEN */

    /* 126998: three [len+2][0x01][chars] strings, NULL → empty. */
    n = n2k_encode_126998("Engine room", "Port alternator", NULL, 0x23, 0,
                          fr, N2K_FP_MAX_FRAMES);
    CHECK(n > 0);
    CHECK(fp_reassemble(fr, n, back, &blen) == 0);
    CHECK(blen == (11u + 2u) + (15u + 2u) + 2u);
    CHECK(back[0] == 13 && back[1] == 0x01);
    CHECK(memcmp(&back[2], "Engine room", 11) == 0);
    CHECK(back[13] == 17 && back[14] == 0x01);
    CHECK(memcmp(&back[15], "Port alternator", 15) == 0);
    CHECK(back[30] == 2 && back[31] == 0x01);    /* empty mfg-info string */
}

static void test_prop_telemetry(void)
{
    ctrl_telemetry_t t = snap_bulk();
    n2k_frame_t fr[8];
    /* Golden, seq 5, SID 7, SA 0x23. Payload (22 B):
     *  mfg header = 2046 | 0x3<<11 | 4<<13 = 0x9FFE → FE 9F
     *  [SID 07][state BULK=01][reason 00][profile 02]
     *  effort 0.42 × 200 = 84 = 0x54
     *  [binding THERMAL=04] binding_w 3000 = 0x0BB8 → B8 0B
     *    (THERMAL's ordinal shifted 3->4 when deliverable #10 inserted
     *    CTRL_BIND_BMS_CVL ahead of it in ctrl_bind_src_t, control.h)
     *  alt 88 °C → 36115 = 0x8D13 → 13 8D; batt 22 °C → 0x734B → 4B 73
     *  rpm 1500/0.25 = 6000 = 0x1770 → 70 17; [rpm_state 00]
     *  faults SELF_OVERTEMP = 0x10 → 10 00 00 00; [severity WARN=01]
     * 22 B → 4 frames; PGN 130816 = 0x1FF00, prio 6 → id 0x19FF0023. */
    const uint8_t f0[8] = { 0xA0, 0x16, 0xFE, 0x9F, 0x07, 0x01, 0x00, 0x02 };
    const uint8_t f1[8] = { 0xA1, 0x54, 0x04, 0xB8, 0x0B, 0x13, 0x8D, 0x4B };
    const uint8_t f2[8] = { 0xA2, 0x73, 0x70, 0x17, 0x00, 0x10, 0x00, 0x00 };
    const uint8_t f3[8] = { 0xA3, 0x00, 0x01, 0xFF, 0xFF, 0xFF, 0xFF, 0xFF };
    CHECK(n2k_encode_prop_telemetry(&t, 7, 0x23, 5, fr, 8) == 4);
    CHECK(fr[0].id == 0x19FF0023u);
    CHECK(memcmp(fr[0].data, f0, 8) == 0);
    CHECK(memcmp(fr[1].data, f1, 8) == 0);
    CHECK(memcmp(fr[2].data, f2, 8) == 0);
    CHECK(memcmp(fr[3].data, f3, 8) == 0);

    /* No binding ceiling (+inf) → NA; effort clamps to the 0..200 range. */
    t.binding = CTRL_BIND_NONE;
    t.binding_w = INFINITY;
    t.field_effort = 1.5f;
    CHECK(n2k_encode_prop_telemetry(&t, 7, 0x23, 5, fr, 8) == 4);
    CHECK(fr[1].data[1] == 200);                       /* effort pegged */
    CHECK(fr[1].data[3] == 0xFF && fr[1].data[4] == 0xFF); /* ceiling NA */
    t.field_effort = -0.5f;
    CHECK(n2k_encode_prop_telemetry(&t, 7, 0x23, 5, fr, 8) == 4);
    CHECK(fr[1].data[1] == 0);
}

void test_n2k_encode(void)
{
    int at_entry = g_checks;
    printf("\n-- NMEA2000 PGN encoders (CAN_INTEGRATION.md §2, GH#18)\n");
    test_can_id();
    test_sid();
    test_fp_split();
    test_127508();
    test_127506();
    test_127488();
    test_127750();
    test_alerts();
    test_product_config_info();
    test_prop_telemetry();
    printf("  [N2K] %d checks\n", g_checks - at_entry);
}
