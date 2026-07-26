/*
 * config_store.c — two-slot power-fail-safe config store. PURE.
 * Slot/generation/verify policy and the layout are documented in config_store.h.
 * SPDX-License-Identifier: MIT
 */
#include "config_store.h"

uint16_t cfg_store_slot_addr(uint8_t slot)
{
    return (slot == 0u) ? (uint16_t)CFG_SLOT_A_ADDR : (uint16_t)CFG_SLOT_B_ADDR;
}

void cfg_store_init(cfg_store_t *st, cfg_store_read_fn rd, cfg_store_write_fn wr)
{
    if (st == NULL) return;
    st->read       = rd;
    st->write      = wr;
    st->generation = 0u;
    st->live_slot  = 1u;      /* nothing live: standby = A, so the first save
                               * lands in slot A at generation 1 */
    st->have_live  = false;
}

cfg_store_status_t cfg_store_load(cfg_store_t *st, ctrl_config_t *out)
{
    if (st == NULL || st->read == NULL || out == NULL) return CFG_STORE_ARG;

    /* Boot/config-path only — never the 10 ms control loop — so a record-sized
     * stack buffer is cheaper than a permanent static one. */
    uint8_t buf[CFG_RECORD_V1_BYTES];

    bool     any_read = false;   /* at least one slot was readable at all */
    bool     found    = false;
    uint32_t best_gen = 0u;
    uint8_t  best_slot = 0u;

    /* Slots are walked in order and *out is only written when the slot beats
     * what we already have, so a valid-but-older slot B can never clobber the
     * config decoded from slot A. */
    for (uint8_t slot = 0u; slot < 2u; slot++) {
        if (!st->read(cfg_store_slot_addr(slot), buf, (uint16_t)CFG_RECORD_V1_BYTES))
            continue;                                   /* unreadable slot */
        any_read = true;

        uint32_t gen = 0u;
        if (cfg_peek(buf, sizeof buf, &gen) != CFG_DEC_OK)
            continue;                                   /* blank/torn/corrupt */
        if (found && !cfg_generation_newer(gen, best_gen))
            continue;                                   /* older than the winner so far */
        if (cfg_decode(buf, sizeof buf, out, &gen) != CFG_DEC_OK)
            continue;                                   /* unreachable: peek passed */

        found     = true;
        best_gen  = gen;
        best_slot = slot;
    }

    if (!found) {
        st->have_live  = false;
        st->generation = 0u;
        st->live_slot  = 1u;                            /* first save -> slot A */
        /* Blank EEPROM and dead EEPROM are different problems for the caller:
         * EMPTY means "use defaults, then persist"; IO means "the part did not
         * answer" and must not be papered over with a write. */
        return any_read ? CFG_STORE_EMPTY : CFG_STORE_IO;
    }

    st->have_live  = true;
    st->generation = best_gen;
    st->live_slot  = best_slot;
    return CFG_STORE_OK;
}

cfg_store_status_t cfg_store_save(cfg_store_t *st, const ctrl_config_t *cfg)
{
    if (st == NULL || st->read == NULL || st->write == NULL || cfg == NULL)
        return CFG_STORE_ARG;

    /* Standby slot only. With nothing live, live_slot is parked at B so this
     * still resolves to A. */
    const uint8_t  slot = (uint8_t)(st->live_slot ^ 1u);
    const uint16_t addr = cfg_store_slot_addr(slot);
    const uint32_t gen  = st->generation + 1u;          /* wraps; load() compares modularly */

    uint8_t buf[CFG_RECORD_V1_BYTES];
    if (cfg_encode(cfg, gen, buf, sizeof buf) != (size_t)CFG_RECORD_V1_BYTES)
        return CFG_STORE_ENCODE;

    if (!st->write(addr, buf, (uint16_t)CFG_RECORD_V1_BYTES))
        return CFG_STORE_IO;

    /* Read-back verify: the frame (CRC included) has to parse, the generation
     * has to be the one we just wrote, and the bytes have to be identical. A
     * write that NACKed halfway, a stuck page, or a part that silently dropped a
     * page all fail here — and the live slot is still untouched. */
    uint8_t rb[CFG_RECORD_V1_BYTES];
    if (!st->read(addr, rb, (uint16_t)CFG_RECORD_V1_BYTES))
        return CFG_STORE_IO;

    uint32_t rb_gen = 0u;
    if (cfg_peek(rb, sizeof rb, &rb_gen) != CFG_DEC_OK || rb_gen != gen)
        return CFG_STORE_VERIFY;
    for (size_t i = 0; i < (size_t)CFG_RECORD_V1_BYTES; i++)
        if (rb[i] != buf[i]) return CFG_STORE_VERIFY;

    /* Commit: only now does the new slot become the one we refuse to overwrite. */
    st->generation = gen;
    st->live_slot  = slot;
    st->have_live  = true;
    return CFG_STORE_OK;
}
