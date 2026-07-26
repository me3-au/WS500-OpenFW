/*
 * safe_state.h — R0: the one field-off funnel (PROJECT_PLAN §7 R0).
 *
 * Every failure path in this firmware — HardFault, NMI, unexpected IRQ,
 * watchdog escalation, PVD brown-out warning, assert — ends here, and nowhere
 * else. The implementation is deliberately the smallest, most dependency-free
 * thing in the tree so that it is still correct in the worst context we can be
 * called from: an exception handler, on a corrupt stack, with peripheral state
 * unknown and possibly before board_init() ran.
 *
 * Boundary rule: NO HAL, NO other module, no library calls, no statics that
 * matter for the field-off action itself — direct register writes only.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_SAFE_STATE_H
#define WS500_SAFE_STATE_H

#include <stdint.h>

/*
 * Force the alternator field OFF, unconditionally and immediately.
 *
 * Sequence (each step alone is sufficient on healthy hardware; all three run
 * so that no single failed peripheral can leave the field energised):
 *   1. TIM1 BDTR.MOE = 0      — hardware main-output disable (the stock-proven
 *                               cutoff, field_drive.c/§0.6 V1+V2: BKIN is not
 *                               routed on this board, so MOE is the backstop).
 *   2. TIM1 CCR1/CCR3 = 0     — 0 % duty, so re-enabling MOE cannot resume the
 *                               previous duty.
 *   3. PA8 / PB15 forced to GPIO push-pull output LOW — the field drive is
 *                               active-high (TIM_OCPOLARITY_HIGH, idle-state
 *                               RESET, boots at CCR=0 == off), so LOW is off
 *                               even if the timer is left in an odd state.
 *
 * Safe to call from any context, at any time, any number of times, including
 * before board_init(): it enables the GPIOA/GPIOB/TIM1 clocks itself rather
 * than assuming someone else did. It calls nothing, allocates nothing, and
 * never blocks or loops — it cannot fail and has no failure mode to report.
 *
 * It does NOT reset the MCU and does NOT latch anything in the control core;
 * callers decide what happens next (crash record + reset, or LIMP-and-stay).
 */
void enter_safe_state(void);

/* How many times enter_safe_state() has run since boot. Diagnostics only
 * (telemetry/crash record); never a control input. Saturates at UINT32_MAX. */
uint32_t safe_state_entry_count(void);

#endif /* WS500_SAFE_STATE_H */
