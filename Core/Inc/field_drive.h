/*
 * field_drive.h — alternator field PWM (TIM1) abstraction.
 * PA8 = TIM1_CH1 (AF2). Fault cutoff = software MOE-clear, as in stock (the stock
 * WS500 does NOT use TIM1 BKIN — BDTR.BKE=0, §0.6 V1/V2); BKIN remains an
 * optional improvement only if a fault comparator is ever wired.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_FIELD_DRIVE_H
#define WS500_FIELD_DRIVE_H

#include <stdbool.h>

void field_drive_init(void);        /* configure TIM1 PWM + break input */
void field_drive_set(float duty);   /* duty 0.0..1.0; clamped */
void field_drive_off(void);         /* immediate 0% */
void field_drive_fault_cutoff(void);/* assert hardware break -> field off, latched */
bool field_drive_faulted(void);

#endif /* WS500_FIELD_DRIVE_H */
