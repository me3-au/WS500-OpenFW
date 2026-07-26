/*
 * safe_state.c — R0: the one field-off funnel (PROJECT_PLAN §7 R0).
 *
 * Contract, call contexts and the reason this file has no dependencies:
 * safe_state.h. Read that first.
 *
 * Implementation notes that matter for correctness:
 *
 * - Only <stm32f0xx.h> (CMSIS register definitions — headers, not code) is
 *   included. No HAL, no field_drive.c: field_drive.c owns a
 *   TIM_HandleTypeDef whose contents we cannot trust in a fault context, and
 *   __HAL_TIM_MOE_DISABLE dereferences it.
 * - No function calls, no arrays, no library helpers. The generated code is a
 *   short run of load/modify/store on peripheral registers, so it needs at
 *   most a couple of callee-saved registers of stack — and the fault entry in
 *   fault_handlers.c re-points MSP at the top of the stack region before
 *   calling anything, so even a stack-overflow fault reaches this code with a
 *   usable stack.
 * - Pin identities are hard-coded here rather than taken from board.h's
 *   HAL-typed macros so that this file never pulls in the HAL. They are
 *   asserted against board.h at compile time below: if the pin map ever
 *   changes, this file fails the build rather than silently driving the wrong
 *   pin. (Pin facts: PROJECT_PLAN §0.6 V1/V2 — PA8 = TIM1_CH1, PB15 =
 *   TIM1_CH3N.)
 *
 * SPDX-License-Identifier: MIT
 */
#include "safe_state.h"
#include "stm32f0xx.h"

/* Field-drive pins, as raw bit numbers (see file header for why not board.h). */
#define SS_FIELD_A_PIN   8u    /* PA8  = TIM1_CH1  */
#define SS_FIELD_B_PIN   15u   /* PB15 = TIM1_CH3N */

/* Compile-time cross-check against the authoritative pin map. board.h is
 * HAL-typed, so it is included ONLY for this check, and only for the
 * preprocessor constants — no HAL code is referenced. */
#include "board.h"
_Static_assert(FIELD_PWM_PIN  == (1u << SS_FIELD_A_PIN),  "safe_state: PA8 pin map drifted from board.h");
_Static_assert(FIELD_PWMN_PIN == (1u << SS_FIELD_B_PIN),  "safe_state: PB15 pin map drifted from board.h");

static volatile uint32_t s_entries;

void enter_safe_state(void)
{
    /* Clocks first: we may be called before board_init(), or after a fault
     * that left RCC half-configured. Writing a peripheral whose bus clock is
     * gated is silently dropped on this family, which would make the whole
     * funnel a no-op — so never assume, always enable. Idempotent. */
    RCC->AHBENR  |= RCC_AHBENR_GPIOAEN | RCC_AHBENR_GPIOBEN;
    RCC->APB2ENR |= RCC_APB2ENR_TIM1EN;

    /* 1. Hardware main-output disable — outputs go inactive in one write. */
    TIM1->BDTR &= ~TIM_BDTR_MOE;

    /* 2. Duty to zero on both field channels, and drop the capture/compare
     *    output enables, so nothing can resume the old duty if MOE is ever
     *    re-asserted (e.g. by a HAL call in a later boot stage). */
    TIM1->CCR1 = 0u;
    TIM1->CCR3 = 0u;
    TIM1->CCER &= ~(TIM_CCER_CC1E | TIM_CCER_CC1NE |
                    TIM_CCER_CC3E | TIM_CCER_CC3NE);

    /* 3. Take the pins away from the timer and drive them LOW. Order matters:
     *    set the output data bit BEFORE switching the mode, so the pin never
     *    passes through a briefly-undefined output level. MODER is 2 bits per
     *    pin: 00 = input, 01 = general-purpose output. OTYPER 0 = push-pull
     *    (a floating gate on a field driver is not an "off" we want to rely
     *    on). */
    GPIOA->BSRR    = (uint32_t)1u << (SS_FIELD_A_PIN + 16u);   /* BR8  -> low */
    GPIOA->OTYPER &= ~((uint32_t)1u << SS_FIELD_A_PIN);
    GPIOA->MODER   = (GPIOA->MODER & ~((uint32_t)3u << (SS_FIELD_A_PIN * 2u)))
                     | ((uint32_t)1u << (SS_FIELD_A_PIN * 2u));

    GPIOB->BSRR    = (uint32_t)1u << (SS_FIELD_B_PIN + 16u);   /* BR15 -> low */
    GPIOB->OTYPER &= ~((uint32_t)1u << SS_FIELD_B_PIN);
    GPIOB->MODER   = (GPIOB->MODER & ~((uint32_t)3u << (SS_FIELD_B_PIN * 2u)))
                     | ((uint32_t)1u << (SS_FIELD_B_PIN * 2u));

    /* Bookkeeping last: the field is already off before we touch RAM, so a
     * fault while updating this counter cannot delay the safe state. */
    if (s_entries != UINT32_MAX) s_entries++;
}

uint32_t safe_state_entry_count(void) { return s_entries; }
