/*
 * field_drive.c — TIM1 field PWM. Fail-safe defaults (starts at 0%).
 * SPDX-License-Identifier: MIT
 */
#include "field_drive.h"
#include "board.h"

static TIM_HandleTypeDef htim1;
static bool s_faulted;

void field_drive_init(void)
{
    /* TIM1 PWM on PA8/CH1 at ~143 Hz — matches the stock WS500 (verified from the
     * stock binary's TIM1 config: PSC=326, ARR=1024, TIM1CLK=48 MHz -> 143.2 Hz,
     * 10-bit duty; PROJECT_PLAN §0.6 V2). The field-driver/snubber/rotor stage was
     * tuned around this; the lineage runs 122 Hz (AVR) / 143 Hz (WS500) / 244 Hz
     * (2018 proto), all << the ">400 Hz is lossy" ceiling.
     * ARR = FIELD_PWM_MAX so CCR maps 0..FIELD_PWM_MAX == 0..100%. */
    const uint32_t psc = 326;                /* 48MHz/(326+1) = 146.8 kHz timer clock */

    /* TIM1's APB2 bus clock, FIRST — every register write below is silently
     * DROPPED while the peripheral clock is gated (the same silicon fact
     * safe_state.c's header states, and the reason it re-enables the clock
     * itself before touching TIM1 from a fault context).
     *
     * GAP FOUND 2026-07-27 by the deliverable-#14 safety review: nothing in
     * this tree enabled TIM1EN except safe_state.c. board_init() enables only
     * GPIO/SYSCFG/CRS, this function never did, and the HAL's
     * HAL_TIM_PWM_MspInit default is an empty __weak stub — so on real
     * hardware the whole PSC/ARR/CCR/BDTR sequence below evaporated and NO
     * PWM would ever have appeared on PA8. Fails safe (field stuck off), but
     * it would have silently invalidated the M2 bench field-PWM and cutoff
     * items: reading BDTR back from a gated timer returns MOE=0 whether or
     * not the cutoff actually worked, so the cutoff test would have "passed"
     * against a peripheral that was never armed. Exact sibling of the missing
     * HAL_ADC_MspInit found the same day — neither is catchable without
     * hardware (Renode's timer model does not enforce RCC gating).
     * Clock-enable is owned here rather than in an MspInit hook to keep it
     * adjacent to the fact above; the effect is identical. */
    __HAL_RCC_TIM1_CLK_ENABLE();

    htim1.Instance = FIELD_TIM;
    htim1.Init.Prescaler = psc;
    htim1.Init.CounterMode = TIM_COUNTERMODE_UP;
    htim1.Init.Period = FIELD_PWM_MAX - 1;   /* 146.8kHz/1024 = 143.2 Hz PWM */
    htim1.Init.ClockDivision = TIM_CLOCKDIVISION_DIV1;
    htim1.Init.RepetitionCounter = 0;
    htim1.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_ENABLE;
    HAL_TIM_PWM_Init(&htim1);

    TIM_OC_InitTypeDef oc = {0};
    oc.OCMode = TIM_OCMODE_PWM1;
    oc.Pulse = 0;                            /* start at 0% — fail-safe */
    oc.OCPolarity = TIM_OCPOLARITY_HIGH;
    oc.OCNPolarity = TIM_OCNPOLARITY_HIGH;
    oc.OCFastMode = TIM_OCFAST_DISABLE;
    oc.OCIdleState = TIM_OCIDLESTATE_RESET;
    oc.OCNIdleState = TIM_OCNIDLESTATE_RESET;
    HAL_TIM_PWM_ConfigChannel(&htim1, &oc, FIELD_TIM_CH);

    /* Break input: DISABLED — hardware fact (§0.6 V1+V2, stock binary disasm
     * 2026-07-23): BDTR.BKE=0 in stock and NO BKIN pin is routed on the board.
     * Enabling break with a floating/unrouted break source is a hazard
     * (spurious PWM kill or undefined behavior), so it must stay off. The
     * fault cutoff is the software MOE-clear in field_drive_fault_cutoff(),
     * exactly as in stock. Re-enable (TIM_BREAK_ENABLE + a real polarity)
     * ONLY if a fault comparator is ever wired to a break-capable pin. */
    TIM_BreakDeadTimeConfigTypeDef bdt = {0};
    bdt.OffStateRunMode  = TIM_OSSR_DISABLE;
    bdt.OffStateIDLEMode = TIM_OSSI_DISABLE;
    bdt.LockLevel        = TIM_LOCKLEVEL_OFF;
    bdt.DeadTime         = 0;
    bdt.BreakState       = TIM_BREAK_DISABLE;
    bdt.BreakPolarity    = TIM_BREAKPOLARITY_HIGH;   /* don't care while BKE=0 */
    bdt.AutomaticOutput  = TIM_AUTOMATICOUTPUT_DISABLE;
    HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bdt);

    HAL_TIM_PWM_Start(&htim1, FIELD_TIM_CH);
    __HAL_TIM_MOE_ENABLE(&htim1);
    field_drive_off();
}

void field_drive_set(float duty)
{
    if (s_faulted) { __HAL_TIM_SET_COMPARE(&htim1, FIELD_TIM_CH, 0); return; }
    /* INVERTED comparisons, deliberately: `duty < 0` and `duty > 1` are BOTH
     * false for NaN, so the obvious form lets a NaN straight through to the
     * cast below — and (uint32_t)(NaN * FIELD_PWM_MAX) is undefined behaviour
     * whose plausible result (0xFFFFFFFF -> CCR > ARR) is 100 % duty pinned
     * on, i.e. the rotor-overdrive catastrophe PROJECT_PLAN §5 calls the #1
     * hazard on this install. Written as !(duty > 0) / !(duty < 1), NaN fails
     * both tests and lands on 0 — the safe end. This is the last line of
     * defence for every caller including the control core, which has already
     * had two NaN-propagation defects caught in SIL (§8.1) and two more in
     * the §8.4 property sweeps. */
    if (!(duty > 0.0f)) duty = 0.0f;      /* NaN and negatives -> 0 */
    if (!(duty < 1.0f)) duty = 1.0f;
    __HAL_TIM_SET_COMPARE(&htim1, FIELD_TIM_CH, (uint32_t)(duty * FIELD_PWM_MAX));
}

void field_drive_off(void) { __HAL_TIM_SET_COMPARE(&htim1, FIELD_TIM_CH, 0); }

void field_drive_fault_cutoff(void)
{
    s_faulted = true;
    __HAL_TIM_MOE_DISABLE(&htim1);   /* hardware main-output disable */
    field_drive_off();
}

bool field_drive_faulted(void) { return s_faulted; }
