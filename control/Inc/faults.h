/*
 * faults.h — fault classification + field disposition (CONTROL_SPEC §7, §9). PURE.
 * Which faults cut the field vs. drop to Limp Home vs. are informational is fixed
 * in firmware here, not user config.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_FAULTS_H
#define WS500_FAULTS_H

#include "control.h"

typedef enum {
    CTRL_DISP_CONTINUE = 0, /* no field-affecting fault */
    CTRL_DISP_LIMP,         /* recoverable-critical → Limp Home (FLOAT @ v_limp) */
    CTRL_DISP_OPEN          /* hard cutoff → field open */
} ctrl_disposition_t;

/* Highest severity among the active fault bits (INFO if none). */
ctrl_severity_t ctrl_fault_severity(uint32_t faults);

/* Field disposition (§7 ladder): OPEN dominates LIMP dominates CONTINUE. */
ctrl_disposition_t ctrl_fault_disposition(uint32_t faults);

#endif /* WS500_FAULTS_H */
