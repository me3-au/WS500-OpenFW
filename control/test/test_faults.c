/*
 * test_faults.c — §7 severity + disposition ladder.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "faults.h"

void test_faults(void)
{
    /* Severity. */
    CHECK(ctrl_fault_severity(CTRL_FAULT_NONE) == CTRL_SEV_INFO);
    CHECK(ctrl_fault_severity(CTRL_FAULT_SELF_OVERTEMP) == CTRL_SEV_WARN);
    CHECK(ctrl_fault_severity(CTRL_FAULT_BATT_LOWTEMP) == CTRL_SEV_FAULT);
    CHECK(ctrl_fault_severity(CTRL_FAULT_BATT_TEMP_REQUIRED) == CTRL_SEV_FAULT);   /* GH#40 */
    CHECK(ctrl_fault_severity(CTRL_FAULT_LOST_BMS) == CTRL_SEV_FAULT);
    CHECK(ctrl_fault_severity(CTRL_FAULT_OVERVOLTAGE) == CTRL_SEV_CRITICAL);
    /* Highest severity wins. */
    CHECK(ctrl_fault_severity(CTRL_FAULT_SELF_OVERTEMP | CTRL_FAULT_OVERVOLTAGE) == CTRL_SEV_CRITICAL);
    CHECK(ctrl_fault_severity(CTRL_FAULT_SELF_OVERTEMP | CTRL_FAULT_LOST_BMS) == CTRL_SEV_FAULT);

    /* Disposition ladder. */
    CHECK(ctrl_fault_disposition(CTRL_FAULT_NONE) == CTRL_DISP_CONTINUE);
    CHECK(ctrl_fault_disposition(CTRL_FAULT_SELF_OVERTEMP) == CTRL_DISP_CONTINUE); /* warn only */
    CHECK(ctrl_fault_disposition(CTRL_FAULT_LOST_BMS) == CTRL_DISP_LIMP);
    /* BLOCK-class faults are not in ctrl_disposition_t's ladder at all — like
     * BATT_LOWTEMP/HIGHTEMP, control.c gates charging on CTRL_FAULT_BLOCK_MASK
     * directly (the `batt_block` local), so disposition() reports CONTINUE. */
    CHECK(ctrl_fault_disposition(CTRL_FAULT_BATT_TEMP_REQUIRED) == CTRL_DISP_CONTINUE);
    CHECK(ctrl_fault_disposition(CTRL_FAULT_OVERVOLTAGE) == CTRL_DISP_OPEN);
    /* OPEN dominates LIMP. */
    CHECK(ctrl_fault_disposition(CTRL_FAULT_LOST_BMS | CTRL_FAULT_OVERVOLTAGE) == CTRL_DISP_OPEN);

    /* GH#41 (§9.3 D1): SHUNT_OPEN / SHUNT_REVERSED must LIMP, not CONTINUE —
     * a lying current source is the IMPLAUSIBLE_SHUNT class. */
    CHECK(ctrl_fault_severity(CTRL_FAULT_SHUNT_OPEN) == CTRL_SEV_FAULT);
    CHECK(ctrl_fault_severity(CTRL_FAULT_SHUNT_REVERSED) == CTRL_SEV_FAULT);
    CHECK(ctrl_fault_disposition(CTRL_FAULT_SHUNT_OPEN) == CTRL_DISP_LIMP);
    CHECK(ctrl_fault_disposition(CTRL_FAULT_SHUNT_REVERSED) == CTRL_DISP_LIMP);

    /* GH#39: driver-stage hard block is CRITICAL/OPEN, distinct from and
     * dominant over the WARN-only SELF_OVERTEMP bit. */
    CHECK(ctrl_fault_severity(CTRL_FAULT_DRIVER_OVERTEMP) == CTRL_SEV_CRITICAL);
    CHECK(ctrl_fault_disposition(CTRL_FAULT_DRIVER_OVERTEMP) == CTRL_DISP_OPEN);
    CHECK(ctrl_fault_disposition(CTRL_FAULT_SELF_OVERTEMP | CTRL_FAULT_DRIVER_OVERTEMP)
          == CTRL_DISP_OPEN);
}

/*
 * test_fault_mask_completeness — GH#41's real deliverable. D1 (SHUNT_OPEN /
 * SHUNT_REVERSED belonging to no mask) was found by inspection; this guard
 * makes the NEXT such omission fail the build instead of silently defaulting
 * to CTRL_DISP_CONTINUE. It enumerates bit positions programmatically against
 * CTRL_FAULT_ALL_MASK (faults.h) rather than hand-listing fault names here —
 * see faults.h for why ALL_MASK itself still has to be hand-maintained, and
 * where to add a bit when one is appended to control.h.
 */
void test_fault_mask_completeness(void)
{
    for (int bit = 0; bit < 32; bit++) {
        const uint32_t b = (uint32_t)1u << bit;
        if (!(b & CTRL_FAULT_ALL_MASK)) continue;   /* not a declared fault bit */

        int n = 0;
        if (b & CTRL_FAULT_OPEN_MASK)  n++;
        if (b & CTRL_FAULT_LIMP_MASK)  n++;
        if (b & CTRL_FAULT_BLOCK_MASK) n++;
        if (b & CTRL_FAULT_WARN_MASK)  n++;

        if (n != 1) {
            printf("FAIL fault bit %d (0x%08lx) belongs to %d dispositions "
                   "(want exactly 1)\n", bit, (unsigned long)b, n);
        }
        CHECK(n == 1);
    }
}
