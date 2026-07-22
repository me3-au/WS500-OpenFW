/*
 * faults.c — fault severity + disposition (§7, §9). PURE.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "faults.h"

/* Hard field-open (safety) faults. */
#define OPEN_MASK  (CTRL_FAULT_OVERVOLTAGE | CTRL_FAULT_FIELD_SHORT | \
                    CTRL_FAULT_FIELD_OVERCUR | CTRL_FAULT_OVERSPEED | \
                    CTRL_FAULT_WATCHDOG | CTRL_FAULT_BATT_DTDT)

/* Recoverable-critical → Limp Home. */
#define LIMP_MASK  (CTRL_FAULT_LOST_VBAT_SENSE | CTRL_FAULT_LOST_BMS | \
                    CTRL_FAULT_IMPLAUSIBLE_SHUNT)

/* FAULT-severity but not field-open (charge-window blocks handled by the engine). */
#define FAULT_MASK (CTRL_FAULT_BATT_LOWTEMP | CTRL_FAULT_BATT_HIGHTEMP)

/* Warnings — derate / advisory, no state change. */
#define WARN_MASK  (CTRL_FAULT_FIELD_OPEN | CTRL_FAULT_SELF_OVERTEMP | \
                    CTRL_FAULT_THERMAL_DIVERGE)

ctrl_severity_t ctrl_fault_severity(uint32_t faults)
{
    if (faults & OPEN_MASK)                 return CTRL_SEV_CRITICAL;
    if (faults & (LIMP_MASK | FAULT_MASK))  return CTRL_SEV_FAULT;
    if (faults & WARN_MASK)                 return CTRL_SEV_WARN;
    return CTRL_SEV_INFO;
}

ctrl_disposition_t ctrl_fault_disposition(uint32_t faults)
{
    if (faults & OPEN_MASK) return CTRL_DISP_OPEN;
    if (faults & LIMP_MASK) return CTRL_DISP_LIMP;
    return CTRL_DISP_CONTINUE;
}
