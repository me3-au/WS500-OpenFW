/*
 * n2k_addrclaim.h — ISO 11783-81 / NMEA 2000 address claim (PURE).
 *
 * Without this a device is not a legal node on an N2K/J1939 bus: every node
 * must claim a unique 8-bit source address by broadcasting its 64-bit ISO
 * NAME on PGN 60928 and defending/yielding that address per the NAME's
 * numeric value, before it may transmit anything else. This module is the
 * claim-state half of GH#18 deliverable #9 (CAN_INTEGRATION.md §2); the
 * bxCAN glue that drives it from real hardware and real time lives in
 * Core/Src/can_n2k.c.
 *
 * PURE — no HAL, no allocation, no file-scope statics: every piece of state
 * lives in the caller-owned n2k_ac_t, so two instances (e.g. a host-side
 * simulator and the firmware) can run independently, and the whole state
 * machine is host-testable without a bus.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_N2K_ADDRCLAIM_H
#define WS500_N2K_ADDRCLAIM_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>
#include "n2k_encode.h"   /* n2k_frame_t, n2k_can_id/_pgn/_src */

/* PGNs this module speaks (ISO 11783-5 / NMEA 2000 administrative set). */
#define N2K_PGN_ADDR_CLAIM   60928u   /* ISO Address Claim */
#define N2K_PGN_ISO_REQUEST  59904u   /* ISO Request */

/* Reserved addresses (ISO 11783-5 §4.6.1 / NMEA 2000): 254 = the null
 * address a device with no free address must fall silent on; 255 = the
 * global/broadcast destination, never a claimable source. */
#define N2K_ADDR_NULL        254u
#define N2K_ADDR_GLOBAL      255u

/*
 * 64-bit ISO NAME field composition (ISO 11783-81 / NMEA 2000 NAME; the same
 * public bit layout reproduced by every open N2K stack, e.g. ttlappalainen/
 * NMEA2000's tN2kName — a protocol fact, not WS500-specific, so cited here
 * rather than treated as a hardware fact). Bit ranges, LSB first:
 *   0-20  (21b) Unique Number / Identity Number
 *   21-31 (11b) Manufacturer Code
 *   32-34 ( 3b) Device Instance Lower
 *   35-39 ( 5b) Device Instance Upper
 *   40-47 ( 8b) Device Function
 *   48    ( 1b) reserved (transmitted 0)
 *   49-55 ( 7b) Device Class
 *   56-59 ( 4b) System Instance
 *   60-62 ( 3b) Industry Group
 *   63    ( 1b) Arbitrary Address Capable
 */
typedef struct {
    uint32_t unique_number;         /* 21 bits: 0..0x1FFFFF */
    uint16_t manufacturer_code;     /* 11 bits: 0..0x7FF */
    uint8_t  device_instance_lower; /*  3 bits: 0..7 */
    uint8_t  device_instance_upper; /*  5 bits: 0..31 */
    uint8_t  device_function;       /*  8 bits */
    uint8_t  device_class;          /*  7 bits: 0..127 */
    uint8_t  system_instance;       /*  4 bits: 0..15 */
    uint8_t  industry_group;        /*  3 bits: 0..7 */
    bool     arbitrary_addr_capable;
} n2k_name_fields_t;

/* Pack the fields above into the 64-bit wire NAME (out-of-range fields are
 * masked to their bit width, never rejected — callers own correctness of
 * their own constant tables). */
uint64_t n2k_name_build(const n2k_name_fields_t *f);

typedef enum {
    N2K_AC_CLAIMING = 0,  /* a claim is on the bus; waiting out the 250 ms
                           * contention window (ISO 11783-5 §4.4.3.2 / NMEA
                           * 2000: the standard settle time before a claim
                           * may be treated as won) */
    N2K_AC_CLAIMED,       /* window elapsed unopposed — legal to transmit */
    N2K_AC_NULL           /* every address (0..253) was contested and lost;
                           * silent on N2K_ADDR_NULL per ISO 11783-5 §4.4.3.3 */
} n2k_ac_state_t;

/* Address-claim context. Every field is caller-owned state — pure, no
 * hidden globals (see file header). */
typedef struct {
    uint64_t       name;
    uint8_t        pref_addr;   /* first address tried */
    uint8_t        addr;        /* current candidate/claimed address, or
                                 * N2K_ADDR_NULL once exhausted */
    uint16_t       tries;       /* addresses attempted so far, 0..254 */
    n2k_ac_state_t state;
    uint32_t       claim_ms;    /* now_ms when the current CLAIMING window
                                 * (re)started */
    bool           need_claim_tx; /* a claim frame must go out on the next
                                   * n2k_ac_tick() call */
    bool           need_repeat_tx; /* an extra claim frame is owed on the next
                                    * tick — either defending our address
                                    * against a losing contender or answering
                                    * an ISO Request. Unlike need_claim_tx it
                                    * does NOT restart the 250 ms window. */
} n2k_ac_t;

/*
 * Seed the claim state machine: `name` is this device's ISO NAME (build with
 * n2k_name_build), `preferred_addr` the first source address to try (0..253
 * — the exact value doesn't matter for correctness since this device is
 * arbitrary-address-capable and will step off it on contention). Enters
 * N2K_AC_CLAIMING; the caller must not transmit until n2k_ac_ready() is true.
 */
void n2k_ac_init(n2k_ac_t *ctx, uint64_t name, uint8_t preferred_addr);

/*
 * Advance the claim state machine and collect frames to send this cycle
 * (a fresh/repeated Address Claim, and/or a response to a pending ISO
 * Request) into `out` (capacity `max`). Never transmits while
 * N2K_AC_STATE_NULL (ISO 11783-5 §4.4.3.3: a device with no free address
 * stays silent). Returns the number of frames written (0..2), or -1 on
 * NULL ctx/out.
 */
int n2k_ac_tick(n2k_ac_t *ctx, uint32_t now_ms, n2k_frame_t *out, size_t max);

/*
 * Feed one received frame to the claim state machine. Only PGN 60928
 * (Address Claim) and 59904 (ISO Request) are consulted; everything else is
 * ignored. `id` is the 29-bit CAN identifier, `data`/`len` the payload.
 *   - A competing 60928 claim FOR OUR CURRENT ADDRESS with a numerically
 *     LOWER NAME than ours beats us (ISO 11783-5 §4.4.3: lower NAME wins);
 *     we step to the next candidate address and restart the 250 ms window.
 *     A competing claim with a higher (or equal, which cannot legally happen
 *     with unique NAMEs) NAME loses to us — and we then owe the bus a REPEAT
 *     of our own claim on the next tick (§4.4.3.3). That rebuttal is not
 *     optional: it is the only signal that tells the contender to yield, so
 *     a winner that stays silent leaves two nodes sharing one address.
 *   - A 59904 ISO Request for PGN 60928, addressed to us or broadcast, is
 *     queued for a response on the next n2k_ac_tick() — but only once
 *     n2k_ac_ready() would be true; a request arriving mid-claim or in the
 *     null state gets no answer.
 */
void n2k_ac_rx(n2k_ac_t *ctx, uint32_t id, const uint8_t *data, size_t len);

/* True once the current address has cleared its 250 ms contention window
 * unopposed — the only state in which transmitting anything else is legal. */
bool n2k_ac_ready(const n2k_ac_t *ctx);

/* Current (candidate or claimed) source address; N2K_ADDR_NULL once the
 * address space is exhausted. Valid to read in any state. */
uint8_t n2k_ac_address(const n2k_ac_t *ctx);

#endif /* WS500_N2K_ADDRCLAIM_H */
