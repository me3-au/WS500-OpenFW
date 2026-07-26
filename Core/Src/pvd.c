/*
 * pvd.c — R5: brown-out warning via the programmable voltage detector
 * (PROJECT_PLAN §7 R5). Contract: pvd.h.
 *
 * ======================= THRESHOLD CHOICE [SPEC-SIGNOFF] ===================
 * PLS = level 5 = 2.7 V (falling ~2.6 V typ; STM32F072 datasheet PVD
 * characteristics). The choice is bounded on both sides:
 *
 *  ABOVE the reset threshold. The PVD is worthless if the part resets before
 *    the interrupt is serviced, so the threshold has to sit above the BOR
 *    level. The F0's selectable BOR levels top out around 2.8 V, and the
 *    factory default sits well below 2.7 V, so level 5 gives real warning time
 *    at the default. If the BOR option byte is ever raised to its highest
 *    level this ordering breaks and the PVD level must be raised with it —
 *    which is also why this firmware does not program option bytes (see
 *    fault_handlers.c's NMI/parity note and PROJECT_PLAN §6).
 *
 *  BELOW the operating rail with margin. The 3.3 V rail must be able to sag
 *    under a load step, an inrush or an engine crank without producing a
 *    nuisance safe-state. 2.7 V is ~18 % below nominal — deeper than any sag a
 *    healthy regulator should produce, shallow enough to catch a real collapse
 *    with milliseconds to spare at typical bulk-capacitance decay rates.
 *
 * Not bench-measured: the actual rail behaviour on a load dump is a
 * bench-pending observation (§0.6 evidence ladder), and the level is a
 * one-constant change if it proves wrong.
 * ===========================================================================
 *
 * The handler is deliberately tiny and does not try to be clever: on a falling
 * edge there is no "maybe it will recover" branch worth taking. Field off,
 * seal the record, return. If the supply recovers the regulator keeps running
 * (the control core re-commands duty on the next tick, and the field driver's
 * own latch is untouched); if it does not, the state we needed is already in
 * `.noinit`.
 *
 * SPDX-License-Identifier: MIT
 */
#include "pvd.h"
#include "safe_state.h"
#include "crash_record.h"
#include "stm32f0xx.h"

/* EXTI line 16 is the PVD output on this family (RM0091 §12.1.3). */
#define PVD_EXTI_LINE   (1u << 16)

static volatile uint32_t s_events;

void pvd_init(void)
{
    RCC->APB1ENR |= RCC_APB1ENR_PWREN;

    /* Threshold first, enable second: never let the detector run for even one
     * cycle against the reset-default level. */
    PWR->CR = (PWR->CR & ~(uint32_t)PWR_CR_PLS) | PWR_CR_PLS_LEV5;
    PWR->CR |= PWR_CR_PVDE;

    /* EXTI line 16: FALLING edge only. The rising edge (supply recovering)
     * carries no action — reacting to it would just add a second event to
     * reason about, and the control loop re-establishes duty on its own. */
    EXTI->RTSR &= ~(uint32_t)PVD_EXTI_LINE;
    EXTI->FTSR |=  (uint32_t)PVD_EXTI_LINE;
    EXTI->PR    =  (uint32_t)PVD_EXTI_LINE;   /* discard anything latched during setup */
    EXTI->IMR  |=  (uint32_t)PVD_EXTI_LINE;

    /* Priority 0 — the highest available to a configurable exception on this
     * core. A brown-out outranks every other interrupt in this firmware: the
     * others can be late, this one cannot. */
    NVIC_SetPriority(PVD_VDDIO2_IRQn, 0);
    NVIC_ClearPendingIRQ(PVD_VDDIO2_IRQn);
    NVIC_EnableIRQ(PVD_VDDIO2_IRQn);
}

uint32_t pvd_event_count(void) { return s_events; }

bool pvd_below_threshold(void) { return (PWR->CSR & PWR_CSR_PVDO) != 0u; }

/*
 * Vdd fell through the threshold. Ordering matches the §7 R3 funnel: hardware
 * safe first, evidence second. Unlike the fault handlers this does NOT reset —
 * the supply is already doing that if it keeps falling, and a self-inflicted
 * reset would throw away a recoverable dip.
 */
void PVD_VDDIO2_IRQHandler(void)
{
    EXTI->PR = (uint32_t)PVD_EXTI_LINE;    /* ack first: no re-entry storm */

    enter_safe_state();

    if (s_events != UINT32_MAX) s_events++;

    /* Record only the FIRST dip of a boot. A marginal supply can chatter
     * through the threshold, and a ring of four identical brown-out entries
     * would evict the HardFault record that actually explains the failure.
     * The counter above still shows how many dips there were. */
    if (s_events == 1u)
        crash_record_write(CRASH_KIND_BROWNOUT, (const uint32_t *)0, 0u, false, 0u);
    else
        crash_record_flush();
}
