/*
 * annunc.c — status-LED blink encoder (contract: annunc.h, §9.2). PURE.
 * SPDX-License-Identifier: MIT
 */
#include "annunc.h"
#include "faults.h"

/* Load the flash-count shape for `code` into `a` (§9.1 blink column:
 * floor(code/10) long + code mod 10 short; code 0 = heartbeat, one pulse). */
static void annunc_load_pattern(ctrl_annunc_t *a, unsigned code)
{
    a->code = code;
    if (code == 0) {
        a->longs = 0;
        a->total = 1;                 /* the single heartbeat pulse */
    } else {
        a->longs = code / 10u;
        a->total = a->longs + (code % 10u);
    }
    a->flash_idx = 0;
}

/* On-time of flash `idx` within the current pattern (0-based). */
static uint32_t annunc_flash_ms(const ctrl_annunc_t *a, unsigned idx)
{
    if (a->code == 0) return CTRL_ANNUNC_HEARTBEAT_ON_MS;
    return (idx < a->longs) ? CTRL_ANNUNC_LONG_MS : CTRL_ANNUNC_SHORT_MS;
}

/* Off-time before the cycle repeats (the boundary where the next fault pick
 * is latched in — see annunc.h's ctrl_annunc_update() contract). */
static uint32_t annunc_repeat_gap_ms(const ctrl_annunc_t *a)
{
    return (a->code == 0)
        ? (CTRL_ANNUNC_HEARTBEAT_PERIOD_MS - CTRL_ANNUNC_HEARTBEAT_ON_MS)
        : CTRL_ANNUNC_REPEAT_MS;
}

void ctrl_annunc_init(ctrl_annunc_t *a)
{
    annunc_load_pattern(a, 0);        /* start on the heartbeat, no fault yet */
    a->phase = CTRL_ANNUNC_PH_FLASH;
    a->phase_ms = 0;
}

bool ctrl_annunc_update(ctrl_annunc_t *a, uint32_t faults, uint32_t dt_ms)
{
    a->phase_ms += dt_ms;

    /* A single dt_ms can span many phase transitions (a coarse caller tick,
     * or a deliberate big test/catch-up jump) — walk them one at a time
     * rather than assuming dt_ms fits inside one phase. */
    for (;;) {
        uint32_t dur;
        switch (a->phase) {
        case CTRL_ANNUNC_PH_FLASH:
            dur = annunc_flash_ms(a, a->flash_idx);
            if (a->phase_ms < dur) return true;
            a->phase_ms -= dur;
            a->flash_idx++;
            a->phase = (a->flash_idx >= a->total) ? CTRL_ANNUNC_PH_GAP
                                                   : CTRL_ANNUNC_PH_INTER;
            break;

        case CTRL_ANNUNC_PH_INTER:
            dur = CTRL_ANNUNC_GAP_MS;
            if (a->phase_ms < dur) return false;
            a->phase_ms -= dur;
            a->phase = CTRL_ANNUNC_PH_FLASH;
            break;

        case CTRL_ANNUNC_PH_GAP:
            dur = annunc_repeat_gap_ms(a);
            if (a->phase_ms < dur) return false;
            a->phase_ms -= dur;
            /* Cycle boundary: the only point the fault pick may change
             * (annunc.h contract) — same picker the N2K alert uses (GH#42). */
            annunc_load_pattern(a, ctrl_fault_top_code(faults));
            a->phase = CTRL_ANNUNC_PH_FLASH;
            break;

        default:
            return false;  /* unreachable; keeps -Wswitch-enum builds quiet */
        }
    }
}
