/*
 * field_drive.h — alternator field PWM (TIM1) abstraction.
 * PA8 = TIM1_CH1 (AF2); TIM1 BKIN = hardware field cutoff on fault.
 * SPDX-License-Identifier: GPL-3.0-or-later
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
