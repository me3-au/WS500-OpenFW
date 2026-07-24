/*
 * field_drive.h — alternator field PWM (TIM1) abstraction.
 * PA8 = TIM1_CH1 (AF2). Fault cutoff = software MOE-clear, as in stock (the stock
 * WS500 does NOT use TIM1 BKIN — BDTR.BKE=0 and no BKIN pin routed, §0.6 V1/V2);
 * our driver keeps break DISABLED too — re-enable only if a fault comparator is
 * ever wired to a break-capable pin.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_FIELD_DRIVE_H
#define WS500_FIELD_DRIVE_H

#include <stdbool.h>

void field_drive_init(void);        /* configure TIM1 PWM (break input disabled — BKIN unrouted) */
void field_drive_set(float duty);   /* duty 0.0..1.0; clamped */
void field_drive_off(void);         /* immediate 0% */
void field_drive_fault_cutoff(void);/* software MOE-clear -> field off, latched */
bool field_drive_faulted(void);

#endif /* WS500_FIELD_DRIVE_H */
