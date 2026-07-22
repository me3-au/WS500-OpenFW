/*
 * faults.c — fault severity + disposition (§7, §9). PURE.
 * Masks live in faults.h so the engine shares the same classification.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
