/*
 * test_n2k_addrclaim.c — ISO 11783-81 / NMEA 2000 address claim (GH#18):
 * NAME bit layout, claim won/lost, address stepping, exhaustion -> null
 * address, the 250 ms settle gate, ISO-Request response, output overflow.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "n2k_addrclaim.h"

/* Little-endian NAME bytes, for building synthetic competing-claim frames
 * (test-side inverse of n2k_addrclaim.c's put_u64_le). */
static void put_name(uint8_t *data, uint64_t name)
{
    for (int i = 0; i < 8; i++) data[i] = (uint8_t)(name >> (8 * i));
}

static void test_name_layout(void)
{
    n2k_name_fields_t f = {
        .unique_number         = 0x1ABCDEu,   /* distinctive 21-bit value */
        .manufacturer_code     = 0x3EFu,      /* distinctive 11-bit value */
        .device_instance_lower = 0x5u,
        .device_instance_upper = 0x1Bu,
        .device_function       = 0x81u,
        .device_class          = 0x55u,
        .system_instance       = 0x9u,
        .industry_group        = 0x4u,        /* 4 = marine */
        .arbitrary_addr_capable = true,
    };
    uint64_t n = n2k_name_build(&f);

    /* Verify every field lands at its documented bit range (n2k_addrclaim.h)
     * by extraction rather than a hand-assembled hex literal. */
    CHECK((n & 0x1FFFFFu) == 0x1ABCDEu);
    CHECK(((n >> 21) & 0x7FFu) == 0x3EFu);
    CHECK(((n >> 32) & 0x7u)   == 0x5u);
    CHECK(((n >> 35) & 0x1Fu)  == 0x1Bu);
    CHECK(((n >> 40) & 0xFFu)  == 0x81u);
    CHECK(((n >> 48) & 0x1u)   == 0u);      /* reserved bit transmits 0 */
    CHECK(((n >> 49) & 0x7Fu)  == 0x55u);
    CHECK(((n >> 56) & 0xFu)   == 0x9u);
    CHECK(((n >> 60) & 0x7u)   == 0x4u);
    CHECK(((n >> 63) & 0x1u)   == 1u);

    /* Out-of-range fields are masked to their bit width, not rejected. */
    n2k_name_fields_t g = {0};
    g.unique_number = 0xFFFFFFFFu;   /* only the low 21 bits should survive */
    CHECK(n2k_name_build(&g) == 0x1FFFFFu);

    n2k_name_fields_t h = {0};
    h.arbitrary_addr_capable = false;
    CHECK(n2k_name_build(&h) == 0u);
}

static void test_claim_won(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];
    uint8_t data[8];
    int n;

    n2k_ac_init(&ctx, 0x1122334455667788ull, 30u);
    CHECK(!n2k_ac_ready(&ctx));
    CHECK(n2k_ac_address(&ctx) == 30u);

    n = n2k_ac_tick(&ctx, 1000u, fr, 4);
    CHECK(n == 1);
    CHECK(fr[0].id == n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 30u, 0xFFu));
    CHECK(fr[0].dlc == 8);
    CHECK(fr[0].data[0] == 0x88 && fr[0].data[7] == 0x11);  /* NAME LE ends */
    CHECK(!n2k_ac_ready(&ctx));                              /* not settled */

    /* A competing claim on our address with a NUMERICALLY HIGHER NAME loses
     * to us (lower NAME wins, ISO 11783-5 §4.4.3) -- we keep our address. */
    put_name(data, 0x9999999999999999ull);
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 30u, 0xFFu), data, 8);
    CHECK(n2k_ac_address(&ctx) == 30u);

    /* Settle window elapses with nobody beating us -> ready. */
    (void)n2k_ac_tick(&ctx, 1000u + 250u, fr, 4);
    CHECK(n2k_ac_ready(&ctx));
    CHECK(n2k_ac_address(&ctx) == 30u);
}

static void test_claim_lost_lower_name(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];
    uint8_t data[8];
    int n;

    /* Our NAME is large, so NAME=0 is trivially a numerically lower (i.e.
     * winning) competitor. */
    n2k_ac_init(&ctx, 0x8000000000000000ull, 40u);
    (void)n2k_ac_tick(&ctx, 0u, fr, 4);
    CHECK(n2k_ac_address(&ctx) == 40u);

    put_name(data, 1ull);   /* lower than our NAME -> beats us */
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 40u, 0xFFu), data, 8);

    CHECK(n2k_ac_address(&ctx) == 41u);   /* stepped to the next candidate */
    CHECK(!n2k_ac_ready(&ctx));

    n = n2k_ac_tick(&ctx, 10u, fr, 4);    /* re-claims on the new address */
    CHECK(n == 1);
    CHECK(fr[0].id == n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 41u, 0xFFu));
}

/* Winning a contest obliges us to RE-SEND our claim (ISO 11783-5 §4.4.3.3):
 * the rebuttal is the only thing that tells the loser to yield, so a silent
 * winner leaves two nodes sharing one address. */
static void test_claim_defence(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];
    uint8_t data[8];

    n2k_ac_init(&ctx, 0x1000000000000000ull, 30u);
    CHECK(n2k_ac_tick(&ctx, 1000u, fr, 4) == 1);          /* initial claim */
    CHECK(n2k_ac_tick(&ctx, 1250u, fr, 4) == 0);          /* nothing owed */
    CHECK(n2k_ac_ready(&ctx));

    /* Loser (higher NAME) claims our address -> we owe exactly one rebuttal. */
    put_name(data, 0x9999999999999999ull);
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 30u, 0xFFu), data, 8);
    CHECK(n2k_ac_tick(&ctx, 1300u, fr, 4) == 1);
    CHECK(fr[0].id == n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 30u, 0xFFu));
    CHECK(fr[0].data[0] == 0x00 && fr[0].data[7] == 0x10); /* our NAME, LE */
    CHECK(n2k_ac_ready(&ctx));                             /* still ours */
    CHECK(n2k_ac_tick(&ctx, 1350u, fr, 4) == 0);           /* owed once only */

    /* A defence must not restart our settle window: a contender claiming
     * repeatedly during the window can't hold us in CLAIMING forever. */
    n2k_ac_init(&ctx, 0x1000000000000000ull, 30u);
    CHECK(n2k_ac_tick(&ctx, 0u, fr, 4) == 1);
    for (uint32_t ms = 50u; ms < 250u; ms += 50u) {
        n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 30u, 0xFFu), data, 8);
        CHECK(n2k_ac_tick(&ctx, ms, fr, 4) == 1);          /* defends */
        CHECK(!n2k_ac_ready(&ctx));                        /* window running */
    }
    (void)n2k_ac_tick(&ctx, 250u, fr, 4);
    CHECK(n2k_ac_ready(&ctx));   /* settled on the ORIGINAL 250 ms deadline */
}

/* The settle clock starts when the claim frame actually leaves, not when it
 * was queued: a tick with no room to emit must not promote us to CLAIMED. */
static void test_settle_requires_transmitted_claim(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];

    n2k_ac_init(&ctx, 0x1122334455667788ull, 30u);
    /* Starve the output for well past the settle time. claim_ms is still its
     * initial 0, so an unguarded settle check would declare an address won
     * that was never announced. */
    CHECK(n2k_ac_tick(&ctx, 5000u, fr, 0) == 0);
    CHECK(!n2k_ac_ready(&ctx));

    CHECK(n2k_ac_tick(&ctx, 5000u, fr, 4) == 1);   /* now it goes out */
    CHECK(!n2k_ac_ready(&ctx));                    /* window starts HERE */
    (void)n2k_ac_tick(&ctx, 5000u + 249u, fr, 4);
    CHECK(!n2k_ac_ready(&ctx));
    (void)n2k_ac_tick(&ctx, 5000u + 250u, fr, 4);
    CHECK(n2k_ac_ready(&ctx));
}

static void test_address_stepping(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];
    uint8_t data[8];
    put_name(data, 0ull);   /* always the winning (lower) competitor below */

    n2k_ac_init(&ctx, 0xFFFFFFFFFFFFFFFFull, 100u);
    for (uint8_t expect = 101u; expect < 106u; expect++) {
        uint8_t before = n2k_ac_address(&ctx);
        n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ADDR_CLAIM, before, 0xFFu), data, 8);
        CHECK(n2k_ac_address(&ctx) == expect);
    }
    (void)fr;
}

static void test_exhaustion_to_null(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];
    uint8_t data[8];
    int n;
    put_name(data, 0ull);   /* always the winning (lower) competitor below */

    n2k_ac_init(&ctx, 0xFFFFFFFFFFFFFFFEull, 0u);
    for (int i = 0; i < 254; i++) {
        uint8_t addr = n2k_ac_address(&ctx);
        n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ADDR_CLAIM, addr, 0xFFu), data, 8);
    }
    CHECK(n2k_ac_address(&ctx) == N2K_ADDR_NULL);
    CHECK(!n2k_ac_ready(&ctx));

    n = n2k_ac_tick(&ctx, 999999u, fr, 4);
    CHECK(n == 0);   /* silent forever, ISO 11783-5 §4.4.3.3 */

    /* Even a well-formed ISO Request gets no answer from the null address. */
    uint8_t req[3] = { 0x00, 0xEE, 0x00 };   /* requested PGN 60928, LE */
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ISO_REQUEST, 77u, 0xFFu), req, 3);
    n = n2k_ac_tick(&ctx, 1000000u, fr, 4);
    CHECK(n == 0);
}

static void test_settle_gate(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[4];
    n2k_ac_init(&ctx, 1ull, 10u);

    (void)n2k_ac_tick(&ctx, 5000u, fr, 4);         /* claim goes out */
    CHECK(!n2k_ac_ready(&ctx));
    (void)n2k_ac_tick(&ctx, 5000u + 249u, fr, 4);  /* just short */
    CHECK(!n2k_ac_ready(&ctx));
    (void)n2k_ac_tick(&ctx, 5000u + 250u, fr, 4);  /* exactly settled */
    CHECK(n2k_ac_ready(&ctx));
}

static void test_iso_request_response(void)
{
    n2k_ac_t ctx, ctx2;
    n2k_frame_t fr[4];
    const uint8_t req[3]   = { 0x00, 0xEE, 0x00 };   /* PGN 60928, LE */
    const uint8_t other[3] = { 0x00, 0x00, 0x01 };   /* some other PGN */
    int n;

    n2k_ac_init(&ctx, 42ull, 20u);
    (void)n2k_ac_tick(&ctx, 0u, fr, 4);
    (void)n2k_ac_tick(&ctx, 250u, fr, 4);
    CHECK(n2k_ac_ready(&ctx));

    /* Broadcast request (dest 0xFF): answered with our claim frame. */
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ISO_REQUEST, 77u, 0xFFu), req, 3);
    n = n2k_ac_tick(&ctx, 251u, fr, 4);
    CHECK(n == 1);
    CHECK(fr[0].id == n2k_can_id(6, N2K_PGN_ADDR_CLAIM, 20u, 0xFFu));

    /* Directed at someone else's address: no response. */
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ISO_REQUEST, 77u, 5u), req, 3);
    n = n2k_ac_tick(&ctx, 252u, fr, 4);
    CHECK(n == 0);

    /* Request for a PGN other than 60928: no response. */
    n2k_ac_rx(&ctx, n2k_can_id(6, N2K_PGN_ISO_REQUEST, 77u, 0xFFu), other, 3);
    n = n2k_ac_tick(&ctx, 253u, fr, 4);
    CHECK(n == 0);

    /* Mid-claim (not yet ready): a request is not queued for later either. */
    n2k_ac_init(&ctx2, 99ull, 30u);
    (void)n2k_ac_tick(&ctx2, 0u, fr, 4);   /* claim sent, still settling */
    n2k_ac_rx(&ctx2, n2k_can_id(6, N2K_PGN_ISO_REQUEST, 77u, 0xFFu), req, 3);
    n = n2k_ac_tick(&ctx2, 1u, fr, 4);
    CHECK(n == 0);
}

static void test_overflow_and_null_args(void)
{
    n2k_ac_t ctx;
    n2k_frame_t fr[1];
    int n;

    n2k_ac_init(&ctx, 5ull, 60u);
    n = n2k_ac_tick(&ctx, 0u, fr, 0);      /* zero capacity: nothing written */
    CHECK(n == 0);
    CHECK(ctx.need_claim_tx);              /* stays pending, retried later */

    n = n2k_ac_tick(&ctx, 0u, fr, 1);
    CHECK(n == 1);
    CHECK(!ctx.need_claim_tx);

    CHECK(n2k_ac_tick(NULL, 0u, fr, 1) == -1);
    CHECK(n2k_ac_tick(&ctx, 0u, NULL, 1) == -1);

    /* NULL-safe accessors/rx. */
    CHECK(!n2k_ac_ready(NULL));
    CHECK(n2k_ac_address(NULL) == N2K_ADDR_NULL);
    n2k_ac_rx(NULL, 0, NULL, 0);   /* must not crash */
}

void test_n2k_addrclaim(void)
{
    int at_entry = g_checks;
    printf("\n-- ISO 11783-81/N2K address claim (GH#18)\n");
    test_name_layout();
    test_claim_won();
    test_claim_lost_lower_name();
    test_claim_defence();
    test_settle_requires_transmitted_claim();
    test_address_stepping();
    test_exhaustion_to_null();
    test_settle_gate();
    test_iso_request_response();
    test_overflow_and_null_args();
    printf("  [N2K-AC] %d checks\n", g_checks - at_entry);
}
