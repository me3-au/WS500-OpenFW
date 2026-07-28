/*
 * config_store.h — two-slot power-fail-safe config store. PURE.
 *
 * Sits on top of a RAW byte-addressed EEPROM (Core/Src/eeprom24c16.c on the
 * target, an in-RAM mock in the tests) through INJECTED read/write function
 * pointers, so the whole slot/generation/verify policy is natively testable
 * with no HAL anywhere near it.
 *
 * Layout on the 2 KB 24C16 (PROJECT_PLAN §0.6 V7):
 *
 *   0x000 .. 0x3FF   slot A   (1024 B; record is CFG_RECORD_V1_BYTES = 312 at
 *                    schema 2 — the literal here is prose that has already
 *                    drifted once, so trust the macro and the static asserts
 *                    in config_codec.c, not this number)
 *   0x400 .. 0x7FF   slot B   (1024 B)
 *
 * Power-fail safety comes from never writing the slot that is currently live:
 *
 *   LOAD  — parse BOTH slots; a slot counts only with valid magic + version +
 *           payload_len + CRC. The winner is the highest generation by MODULAR
 *           comparison ((int32_t)(a - b) > 0), so the counter may wrap without
 *           the store silently reverting 4 billion writes. Neither valid =>
 *           CFG_STORE_EMPTY, and the CALLER falls back to its compiled defaults;
 *           this module never invents config.
 *   SAVE  — serialize with generation = live + 1 into the STANDBY slot, write,
 *           read back, re-verify the CRC and the generation, and only then
 *           report success and flip which slot is live. A power cut anywhere in
 *           that sequence leaves the previous record untouched and still the
 *           highest valid generation.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_STORE_H
#define WS500_CONFIG_STORE_H

#include "config_codec.h"

#define CFG_SLOT_A_ADDR   0x000u
#define CFG_SLOT_B_ADDR   0x400u
#define CFG_SLOT_BYTES    0x400u

/* The record has to fit the slot with room for schema growth. */
_Static_assert(CFG_RECORD_V1_BYTES <= CFG_SLOT_BYTES,
               "config record does not fit a 1 KB EEPROM slot");
_Static_assert(CFG_SLOT_A_ADDR + CFG_SLOT_BYTES <= CFG_SLOT_B_ADDR,
               "config slots overlap");

/* Injected I/O — same signatures as eeprom24c16_read/write. Both return false
 * on any failure; a partially filled buffer is allowed on a failed read. */
typedef bool (*cfg_store_read_fn)(uint16_t addr, uint8_t *buf, uint16_t len);
typedef bool (*cfg_store_write_fn)(uint16_t addr, const uint8_t *buf, uint16_t len);

typedef enum {
    CFG_STORE_OK = 0,
    CFG_STORE_EMPTY,    /* no valid record in either slot — caller uses defaults */
    CFG_STORE_IO,       /* the injected read/write reported failure */
    CFG_STORE_ENCODE,   /* serialization refused (buffer/pointer bug) */
    CFG_STORE_VERIFY,   /* read-back did not match what was written */
    CFG_STORE_ARG
} cfg_store_status_t;

typedef struct {
    cfg_store_read_fn  read;
    cfg_store_write_fn write;
    uint32_t generation;   /* generation of the live record (0 when none) */
    uint8_t  live_slot;    /* 0 = A, 1 = B — the slot save() must NOT touch */
    bool     have_live;    /* a valid record has been seen this session */
} cfg_store_t;

/* Bind the I/O. Leaves the store in the "nothing live" state: a save() before
 * any load() writes slot A at generation 1. */
void cfg_store_init(cfg_store_t *st, cfg_store_read_fn rd, cfg_store_write_fn wr);

/* Load the newest valid record into *out. On anything but CFG_STORE_OK, *out is
 * untouched. CFG_STORE_EMPTY is a normal first-boot result, not an error. */
cfg_store_status_t cfg_store_load(cfg_store_t *st, ctrl_config_t *out);

/* Write cfg to the standby slot at generation live+1 and verify it. The live
 * slot is only re-pointed after a successful verify. Does NOT validate the
 * config — call ctrl_config_validate() first; the store persists bytes, it does
 * not judge them. BLOCKS for roughly (pages touched) x the part's write cycle. */
cfg_store_status_t cfg_store_save(cfg_store_t *st, const ctrl_config_t *cfg);

/* Byte address of slot 0/1. */
uint16_t cfg_store_slot_addr(uint8_t slot);

/* True when a is strictly newer than b under modular (wrap-tolerant) ordering. */
static inline bool cfg_generation_newer(uint32_t a, uint32_t b)
{
    return (int32_t)(a - b) > 0;
}

#endif /* WS500_CONFIG_STORE_H */
