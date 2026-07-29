/*
 * faults.c — fault severity + disposition (§7, §9). PURE.
 * Masks live in faults.h so the engine shares the same classification.
 * SPDX-License-Identifier: MIT
 */
#include "faults.h"

ctrl_severity_t ctrl_fault_severity(uint32_t faults)
{
    if (faults & CTRL_FAULT_OPEN_MASK)                          return CTRL_SEV_CRITICAL;
    if (faults & (CTRL_FAULT_LIMP_MASK | CTRL_FAULT_BLOCK_MASK)) return CTRL_SEV_FAULT;
    if (faults & CTRL_FAULT_WARN_MASK)                         return CTRL_SEV_WARN;
    return CTRL_SEV_INFO;
}

ctrl_disposition_t ctrl_fault_disposition(uint32_t faults)
{
    if (faults & CTRL_FAULT_OPEN_MASK) return CTRL_DISP_OPEN;
    if (faults & CTRL_FAULT_LIMP_MASK) return CTRL_DISP_LIMP;
    return CTRL_DISP_CONTINUE;
}

unsigned ctrl_fault_top_code(uint32_t faults)
{
    unsigned bit, best_bit = 0;
    ctrl_severity_t best = CTRL_SEV_INFO;
    int found = 0;
    if (faults == 0) return 0;
    /* Highest-severity active fault wins; lowest bit index breaks ties so the
     * pick is deterministic for a given bitfield (moved here from
     * n2k_encode.c verbatim, GH#42 — see faults.h for why). */
    for (bit = 0; bit < 32; bit++) {
        uint32_t b = faults & (1u << bit);
        if (!b) continue;
        ctrl_severity_t s = ctrl_fault_severity(b);
        if (!found || s > best) { best = s; best_bit = bit; found = 1; }
    }
    return best_bit + 1u;
}
