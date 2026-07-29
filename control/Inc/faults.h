/*
 * faults.h — fault classification + field disposition (CONTROL_SPEC §7, §9). PURE.
 * Which faults cut the field vs. drop to Limp Home vs. are informational is fixed
 * in firmware here, not user config.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_FAULTS_H
#define WS500_FAULTS_H

#include "control.h"

/* Fault classification masks (shared with the engine for latching decisions).
 *
 * GH#39: CTRL_FAULT_DRIVER_OVERTEMP joins OPEN_MASK, not a new "component
 * protection" class — the switch itself is what's protected at 125 C, and
 * every existing OPEN-class member (OVERVOLTAGE, FIELD_SHORT, FIELD_OVERCUR,
 * OVERSPEED, WATCHDOG, BATT_DTDT) is the same shape: a hard physical limit
 * where continuing to drive current is itself the hazard, so LIMP (which
 * still drives the field, just less of it) is the wrong disposition. */
#define CTRL_FAULT_OPEN_MASK   (CTRL_FAULT_OVERVOLTAGE | CTRL_FAULT_FIELD_SHORT | \
                                CTRL_FAULT_FIELD_OVERCUR | CTRL_FAULT_OVERSPEED | \
                                CTRL_FAULT_WATCHDOG | CTRL_FAULT_BATT_DTDT | \
                                CTRL_FAULT_DRIVER_OVERTEMP)
/* GH#41 (§9.3 D1): SHUNT_OPEN / SHUNT_REVERSED join LIMP_MASK, the same class
 * as IMPLAUSIBLE_SHUNT — a shunt reading that is open or reversed means the
 * current signal cannot be trusted, which is precisely the condition LIMP
 * exists for (§7: "claimed current with static VBat -> fault, not runaway").
 * Confirmed against §7's protection list before landing (2026-07-28). */
#define CTRL_FAULT_LIMP_MASK   (CTRL_FAULT_LOST_VBAT_SENSE | CTRL_FAULT_LOST_BMS | \
                                CTRL_FAULT_IMPLAUSIBLE_SHUNT | CTRL_FAULT_SHUNT_OPEN | \
                                CTRL_FAULT_SHUNT_REVERSED)
#define CTRL_FAULT_BLOCK_MASK  (CTRL_FAULT_BATT_LOWTEMP | CTRL_FAULT_BATT_HIGHTEMP | \
                                CTRL_FAULT_BATT_TEMP_REQUIRED)
#define CTRL_FAULT_WARN_MASK   (CTRL_FAULT_FIELD_OPEN | CTRL_FAULT_SELF_OVERTEMP | \
                                CTRL_FAULT_THERMAL_DIVERGE | CTRL_FAULT_VSUP_IMPLAUSIBLE)

/*
 * GH#41: every currently-declared fault bit, OR'd together. Exists so
 * test_faults.c can verify mask *completeness* — every declared bit belongs
 * to exactly one of the four masks above — by iterating bit positions
 * programmatically instead of hand-listing fault names in the test (a list
 * that would rot exactly like the membership bug it exists to catch: D1 was
 * two bits declared in control.h and simply never added anywhere here).
 *
 * This union itself IS hand-maintained, but that burden is unavoidable short
 * of enum reflection C doesn't have; it is placed here, next to the masks it
 * validates, specifically so it is hard to add a fault bit without seeing it.
 * ADD NEW BITS HERE in the SAME change that appends them to control.h's
 * ctrl_fault_bits_t — the guard only protects a bit it knows about.
 */
#define CTRL_FAULT_ALL_MASK ( \
    CTRL_FAULT_OVERVOLTAGE | CTRL_FAULT_FIELD_SHORT | CTRL_FAULT_FIELD_OPEN | \
    CTRL_FAULT_FIELD_OVERCUR | CTRL_FAULT_SELF_OVERTEMP | CTRL_FAULT_OVERSPEED | \
    CTRL_FAULT_SHUNT_OPEN | CTRL_FAULT_SHUNT_REVERSED | CTRL_FAULT_BATT_DTDT | \
    CTRL_FAULT_BATT_LOWTEMP | CTRL_FAULT_BATT_HIGHTEMP | CTRL_FAULT_LOST_VBAT_SENSE | \
    CTRL_FAULT_LOST_BMS | CTRL_FAULT_IMPLAUSIBLE_SHUNT | CTRL_FAULT_THERMAL_DIVERGE | \
    CTRL_FAULT_WATCHDOG | CTRL_FAULT_VSUP_IMPLAUSIBLE | CTRL_FAULT_BATT_TEMP_REQUIRED | \
    CTRL_FAULT_DRIVER_OVERTEMP)

typedef enum {
    CTRL_DISP_CONTINUE = 0, /* no field-affecting fault */
    CTRL_DISP_LIMP,         /* recoverable-critical → Limp Home (FLOAT @ v_limp) */
    CTRL_DISP_OPEN          /* hard cutoff → field open */
} ctrl_disposition_t;

/* Highest severity among the active fault bits (INFO if none). */
ctrl_severity_t ctrl_fault_severity(uint32_t faults);

/* Field disposition (§7 ladder): OPEN dominates LIMP dominates CONTINUE. */
ctrl_disposition_t ctrl_fault_disposition(uint32_t faults);

/*
 * ctrl_fault_top_code — the wire code (§9.1: bit index + 1) of the single
 * highest-severity active fault; ties broken by the LOWEST code. Returns 0
 * if `faults` is 0 (code 0 = no fault, §9.1).
 *
 * GH#42: this is the exact selection loop `n2k_alert_from_telemetry()`
 * (n2k_encode.c) already used, pulled out here so the LED annunciator
 * (annunc.c, §9.2) can make "deterministically the same pick" the N2K alert
 * does — §9.2's own wording — instead of a second, maybe-diverging copy of
 * the loop. Both callers pass the raw `ctrl_telemetry_t.faults` bitfield.
 */
unsigned ctrl_fault_top_code(uint32_t faults);

#endif /* WS500_FAULTS_H */
