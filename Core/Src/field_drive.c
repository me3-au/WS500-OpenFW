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

    /* Break input: TIM1_BKIN would force outputs to idle (off) in hardware on a
     * fault line. NOTE (§0.6 V2): the STOCK WS500 does NOT use hardware BKIN — it
     * cuts the field in software by clearing MOE (see field_drive_fault_cutoff).
     * Keeping BKIN is a deliberate improvement, but only valid if a BKIN pin is
     * actually routed to a fault comparator on the board (pending §0.6 V1 / bench).
     * Until confirmed, MOE-clear is the proven primary cutoff. */
    TIM_BreakDeadTimeConfigTypeDef bdt = {0};
    bdt.OffStateRunMode  = TIM_OSSR_DISABLE;
    bdt.OffStateIDLEMode = TIM_OSSI_DISABLE;
    bdt.LockLevel        = TIM_LOCKLEVEL_OFF;
    bdt.DeadTime         = 0;
    bdt.BreakState       = TIM_BREAK_ENABLE;
    bdt.BreakPolarity    = TIM_BREAKPOLARITY_HIGH;
    bdt.AutomaticOutput  = TIM_AUTOMATICOUTPUT_DISABLE;
    HAL_TIMEx_ConfigBreakDeadTime(&htim1, &bdt);

    HAL_TIM_PWM_Start(&htim1, FIELD_TIM_CH);
    __HAL_TIM_MOE_ENABLE(&htim1);
    field_drive_off();
}

void field_drive_set(float duty)
{
    if (s_faulted) { __HAL_TIM_SET_COMPARE(&htim1, FIELD_TIM_CH, 0); return; }
    if (duty < 0.0f) duty = 0.0f;
    if (duty > 1.0f) duty = 1.0f;
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
