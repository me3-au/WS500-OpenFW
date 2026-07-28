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
}
