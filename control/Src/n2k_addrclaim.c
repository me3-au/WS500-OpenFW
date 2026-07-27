/*
 * n2k_addrclaim.c — ISO 11783-81 / NMEA 2000 address claim (contract:
 * n2k_addrclaim.h). PURE.
 * SPDX-License-Identifier: MIT
 */
#include "n2k_addrclaim.h"

/* ISO 11783-5 §4.4.3.2 / NMEA 2000: a claimed address is only safe to use
 * once no competing claim has arrived for this long. */
#define N2K_AC_SETTLE_MS   250u

/* Default priority for both administrative PGNs on this bus (ISO 11783-5 /
 * NMEA 2000 convention — address claim and ISO request are both priority 6,
 * same tier as the periodic status PGNs in n2k_encode.c's PRI_STATUS). */
#define N2K_AC_PRIO   6u

/* ---- little-endian NAME packing (mirrors n2k_encode.c's put_* idiom, but
 * this module stays independent of that file's static helpers). ---------- */
static void put_u64_le(uint8_t *b, uint64_t v)
{
    for (int i = 0; i < 8; i++) b[i] = (uint8_t)(v >> (8 * i));
}

static uint64_t get_u64_le(const uint8_t *b)
{
    uint64_t v = 0;
    for (int i = 7; i >= 0; i--) v = (v << 8) | b[i];
    return v;
}

/* ---- NAME construction (bit layout: n2k_addrclaim.h) -------------------- */
uint64_t n2k_name_build(const n2k_name_fields_t *f)
{
    uint64_t n = 0;
    n |= ((uint64_t)(f->unique_number & 0x1FFFFFu));                 /* 0-20 */
    n |= ((uint64_t)(f->manufacturer_code & 0x7FFu)) << 21;          /* 21-31 */
    n |= ((uint64_t)(f->device_instance_lower & 0x7u)) << 32;        /* 32-34 */
    n |= ((uint64_t)(f->device_instance_upper & 0x1Fu)) << 35;       /* 35-39 */
    n |= ((uint64_t)f->device_function) << 40;                       /* 40-47 */
    /* bit 48 reserved, transmitted 0 */
    n |= ((uint64_t)(f->device_class & 0x7Fu)) << 49;                /* 49-55 */
    n |= ((uint64_t)(f->system_instance & 0xFu)) << 56;              /* 56-59 */
    n |= ((uint64_t)(f->industry_group & 0x7u)) << 60;               /* 60-62 */
    n |= f->arbitrary_addr_capable ? ((uint64_t)1u << 63) : 0u;      /* 63 */
    return n;
}

/* Build our current Address Claim frame (broadcast — every node needs to see
 * every claim to arbitrate correctly, even though PGN 60928 is technically a
 * PDU1/destination-specific format). */
static void build_claim_frame(const n2k_ac_t *ctx, n2k_frame_t *out)
{
    out->id  = n2k_can_id(N2K_AC_PRIO, N2K_PGN_ADDR_CLAIM, ctx->addr, 0xFFu);
    out->dlc = 8;
    put_u64_le(out->data, ctx->name);
}

/* Step to the next candidate address and restart the settle window. Cycles
 * every address exactly once (pref, pref+1, ... pref+253, mod 254) before
 * giving up — ISO 11783-5 §4.4.3.3: arbitrary-address-capable devices try
 * the whole space before falling silent on the null address. */
static void restart_claim(n2k_ac_t *ctx)
{
    ctx->tries++;
    if (ctx->tries >= 254u) {
        ctx->state         = N2K_AC_NULL;
        ctx->addr          = (uint8_t)N2K_ADDR_NULL;
        ctx->need_claim_tx = false;
        ctx->need_repeat_tx = false;
        return;
    }
    ctx->addr          = (uint8_t)(((uint32_t)ctx->pref_addr + ctx->tries) % 254u);
    ctx->state          = N2K_AC_CLAIMING;
    ctx->need_claim_tx = true;
}

void n2k_ac_init(n2k_ac_t *ctx, uint64_t name, uint8_t preferred_addr)
{
    ctx->name          = name;
    ctx->pref_addr     = preferred_addr;
    ctx->addr          = preferred_addr;
    ctx->tries         = 0u;
    ctx->state          = N2K_AC_CLAIMING;
    ctx->claim_ms      = 0u;
    ctx->need_claim_tx = true;
    ctx->need_repeat_tx = false;
}

int n2k_ac_tick(n2k_ac_t *ctx, uint32_t now_ms, n2k_frame_t *out, size_t max)
{
    size_t count = 0;
    if (!ctx || !out) return -1;

    if (ctx->state == N2K_AC_NULL) return 0;   /* silent, ISO 11783-5 §4.4.3.3 */

    if (ctx->need_claim_tx && count < max) {
        build_claim_frame(ctx, &out[count]);
        count++;
        ctx->claim_ms      = now_ms;
        ctx->need_claim_tx = false;
    }
    if (ctx->need_repeat_tx && count < max) {
        build_claim_frame(ctx, &out[count]);
        count++;
        ctx->need_repeat_tx = false;
    }
    /* Only the frame actually leaving starts the clock: if `out` had no room
     * for the claim above, need_claim_tx is still pending and the window has
     * not begun. Promoting here on a stale claim_ms would declare an address
     * won that was never announced. */
    if (ctx->state == N2K_AC_CLAIMING && !ctx->need_claim_tx &&
        (now_ms - ctx->claim_ms) >= N2K_AC_SETTLE_MS) {
        ctx->state = N2K_AC_CLAIMED;
    }
    return (int)count;
}

void n2k_ac_rx(n2k_ac_t *ctx, uint32_t id, const uint8_t *data, size_t len)
{
    uint32_t pgn;
    uint8_t  from;
    if (!ctx || !data || ctx->state == N2K_AC_NULL) return;

    pgn = n2k_can_id_pgn(id);
    if (pgn == N2K_PGN_ADDR_CLAIM) {
        if (len < 8) return;
        from = n2k_can_id_src(id);
        if (from != ctx->addr) return;         /* contest on our address only */
        if (get_u64_le(data) < ctx->name) {     /* lower NAME wins (§4.4.3) */
            restart_claim(ctx);
        } else {
            /* We win — but silence is NOT how you win. ISO 11783-5 §4.4.3.3:
             * the higher-priority (lower-NAME) node must RE-SEND its claim,
             * because that rebuttal is the only thing that tells the loser to
             * yield. Staying quiet leaves both nodes believing they own the
             * address, which is exactly the collision the protocol exists to
             * prevent. Repeat (not restart): a defence must not reset our own
             * 250 ms window, or a chatty contender could hold us in CLAIMING
             * forever. */
            ctx->need_repeat_tx = true;
        }
        return;
    }
    if (pgn == N2K_PGN_ISO_REQUEST) {
        uint8_t dest;
        uint32_t req_pgn;
        if (len < 3) return;
        /* PGN 59904 is PDU1 (PF=0xEA<240): the ID's PS byte carries the
         * destination address verbatim, same convention n2k_can_id() uses
         * on transmit (n2k_encode.h). */
        dest = (uint8_t)(id >> 8);
        if (dest != N2K_ADDR_GLOBAL && dest != ctx->addr) return;
        req_pgn = (uint32_t)data[0] | ((uint32_t)data[1] << 8) |
                 ((uint32_t)data[2] << 16);
        if (req_pgn != N2K_PGN_ADDR_CLAIM) return;
        /* Only answer once our own claim has settled — a request arriving
         * mid-claim or after exhaustion gets no answer (n2k_addrclaim.h). */
        if (ctx->state == N2K_AC_CLAIMED) ctx->need_repeat_tx = true;
    }
}

bool n2k_ac_ready(const n2k_ac_t *ctx)
{
    return ctx && ctx->state == N2K_AC_CLAIMED;
}

uint8_t n2k_ac_address(const n2k_ac_t *ctx)
{
    return ctx ? ctx->addr : (uint8_t)N2K_ADDR_NULL;
}
