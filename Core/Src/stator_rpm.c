/*
 * stator_rpm.c — PA10 EXTI rising-edge capture + TIM2 free-running 32-bit
 * timebase (~979.6 kHz). ISR stays minimal: it reads TIM2->CNT, diffs against
 * the previous edge, rejects implausible (too-short) periods, and pushes the
 * result into a small window; all filtering/RPM math happens in
 * stator_rpm_update(), called once per main-loop tick, not from the ISR.
 *
 * Facts (stock binary disasm 2026-07-23, PROJECT_PLAN §0.6 V1+V2): PA10 is an
 * EXTI rising-edge input (NOT a TIM2 capture channel); TIM2 is a plain
 * free-running 32-bit counter used purely as a timebase, and TIM2 on the F0
 * family IS a full 32-bit counter (RM0091) — the ISR's unsigned CNT diff is
 * therefore wrap-safe with no special-casing needed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "stator_rpm.h"
#include "board.h"
#include <math.h>

/* TIM2 prescaler: derived the same way field_drive.c derives TIM1's — from
 * OUR OWN 48 MHz APB1 clock (board_clock_config: APB1CLKDivider = DIV1, so no
 * APB-prescaler timer-clock doubling applies), divided down to match the
 * stock's documented ~979.6 kHz tick rate (STATOR_TIMEBASE_HZ, board.h):
 *   48,000,000 / (48+1) = 979,591.8 Hz  (~979.6 kHz, matches board.h's V1+V2
 *   figure to within 0.001%). */
#define STATOR_TIM2_PSC   48u

/* Small median/average window (task spec): reject a single noisy outlier via
 * median-of-N, N small enough for a trivial insertion sort on a Cortex-M0. */
#define STATOR_WINDOW_N   5u

/* Noise/bounce rejection: reject implausibly short periods outright. This is
 * firmware policy (not a hardware fact) — chosen generous (~10 kHz edge rate)
 * so it only catches glitches/bounce, never a genuine stator signal; retune
 * once real waveforms are on the bench. 100 us / 1.02 us/tick ~= 98 ticks. */
#define STATOR_MIN_PERIOD_TICKS   98u

/* Staleness/loss timeouts, in main-loop ticks worth of milliseconds. Firmware
 * policy (not a hardware fact) — tunable once real idle/cranking stator
 * frequencies are characterized on the bench. */
#define STATOR_STALE_MS   200u
#define STATOR_LOST_MS    800u

static TIM_HandleTypeDef htim2;

/* ISR-owned state (written only in HAL_GPIO_EXTI_Callback, read elsewhere only
 * under __disable_irq()/__enable_irq()). */
static volatile bool     s_have_prev;
static volatile uint32_t s_prev_cnt;
static volatile uint32_t s_period_ticks[STATOR_WINDOW_N];
static volatile uint8_t  s_win_idx;
static volatile uint8_t  s_win_count;
static volatile uint32_t s_last_edge_ms;

/* stator_rpm_update()-owned cached results (read by the public accessors). */
static float               s_freq_hz = NAN;
static stator_rpm_state_t  s_state   = STATOR_RPM_LOST;
static stator_rpm_cfg_t    s_cfg     = { 0u, 0.0f };

void stator_rpm_init(void)
{
    s_have_prev    = false;
    s_win_idx      = 0;
    s_win_count    = 0;
    s_last_edge_ms = HAL_GetTick();
    s_freq_hz      = NAN;
    s_state        = STATOR_RPM_LOST;

    /* TIM2: free-running 32-bit counter, NOT input capture (§0.6 V1+V2). Full
     * 32-bit period (ARR = 0xFFFFFFFF): wraparound is absorbed by the
     * unsigned-diff arithmetic in the EXTI callback below, so no reload
     * interrupt is needed — the timer is never itself an IRQ source. */
    __HAL_RCC_TIM2_CLK_ENABLE();
    htim2.Instance               = STATOR_TIM;
    htim2.Init.Prescaler         = STATOR_TIM2_PSC;
    htim2.Init.CounterMode       = TIM_COUNTERMODE_UP;
    htim2.Init.Period            = 0xFFFFFFFFu;
    htim2.Init.ClockDivision     = TIM_CLOCKDIVISION_DIV1;
    htim2.Init.AutoReloadPreload = TIM_AUTORELOAD_PRELOAD_DISABLE;
    HAL_TIM_Base_Init(&htim2);
    HAL_TIM_Base_Start(&htim2);

    /* PA10 EXTI mode is already configured in board_init() (GPIO_MODE_IT_RISING);
     * arm the NVIC line here. Only 6 live IRQs in stock (§0.6 V1); EXTI4_15 is
     * one of them. */
    HAL_NVIC_SetPriority(STATOR_EXTI_IRQn, 2, 0);
    HAL_NVIC_EnableIRQ(STATOR_EXTI_IRQn);
}

void stator_rpm_set_config(const stator_rpm_cfg_t *cfg)
{
    if (cfg != NULL) s_cfg = *cfg;
}

/* Minimal ISR body: one TIM2 read, one unsigned diff, one plausibility test,
 * one circular-buffer store. No filtering/averaging here. */
void HAL_GPIO_EXTI_Callback(uint16_t GPIO_Pin)
{
    if (GPIO_Pin != STATOR_PIN) return;

    uint32_t now_cnt = STATOR_TIM->CNT;

    if (s_have_prev) {
        uint32_t period = now_cnt - s_prev_cnt;  /* unsigned diff: wrap-safe on a 32-bit counter */
        if (period >= STATOR_MIN_PERIOD_TICKS) {
            s_period_ticks[s_win_idx] = period;
            s_win_idx = (uint8_t)((s_win_idx + 1u) % STATOR_WINDOW_N);
            if (s_win_count < STATOR_WINDOW_N) s_win_count++;
            s_last_edge_ms = HAL_GetTick();
        }
        /* else: implausible (too-short) period — dropped. s_prev_cnt still
         * advances below so the NEXT diff is against this real edge, not a
         * stale one further back. */
    } else {
        s_have_prev    = true;
        s_last_edge_ms = HAL_GetTick();
    }
    s_prev_cnt = now_cnt;
}

void EXTI4_15_IRQHandler(void)
{
    HAL_GPIO_EXTI_IRQHandler(STATOR_PIN);
}

/* Median-of-N on a tiny fixed-size array; N <= STATOR_WINDOW_N (currently 5),
 * so a plain insertion sort is more than adequate on a Cortex-M0. */
static uint32_t median_of(const uint32_t *arr, uint8_t n)
{
    uint32_t tmp[STATOR_WINDOW_N];
    for (uint8_t i = 0; i < n; i++) tmp[i] = arr[i];
    for (uint8_t i = 1; i < n; i++) {
        uint32_t key = tmp[i];
        int8_t j = (int8_t)i - 1;
        while (j >= 0 && tmp[j] > key) { tmp[j + 1] = tmp[j]; j--; }
        tmp[j + 1] = key;
    }
    return tmp[n / 2];
}

void stator_rpm_update(void)
{
    uint32_t period_snapshot[STATOR_WINDOW_N];
    uint8_t  count_snapshot;
    uint32_t last_edge_snapshot;
    bool     have_prev_snapshot;

    /* Cortex-M0 has no atomic multi-word load; take a consistent snapshot of
     * the ISR-owned state with interrupts briefly masked. */
    __disable_irq();
    have_prev_snapshot = s_have_prev;
    count_snapshot      = s_win_count;
    for (uint8_t i = 0; i < count_snapshot; i++) period_snapshot[i] = s_period_ticks[i];
    last_edge_snapshot  = s_last_edge_ms;
    __enable_irq();

    uint32_t age_ms = HAL_GetTick() - last_edge_snapshot;  /* wrap-safe unsigned diff */

    if (!have_prev_snapshot || age_ms >= STATOR_LOST_MS) {
        s_state   = STATOR_RPM_LOST;
        s_freq_hz = NAN;
        return;
    }
    s_state = (age_ms >= STATOR_STALE_MS) ? STATOR_RPM_STALE : STATOR_RPM_FRESH;

    if (count_snapshot == 0) {
        s_freq_hz = NAN;   /* first edge(s) seen, no accepted period yet */
        return;
    }

    uint32_t med_ticks = median_of(period_snapshot, count_snapshot);
    s_freq_hz = (med_ticks > 0u) ? ((float)STATOR_TIMEBASE_HZ / (float)med_ticks) : NAN;
}

float stator_rpm_freq_hz(void)
{
    return (s_state == STATOR_RPM_LOST) ? NAN : s_freq_hz;
}

stator_rpm_state_t stator_rpm_state(void) { return s_state; }

float stator_rpm_rpm(void)
{
    if (s_state == STATOR_RPM_LOST || isnan(s_freq_hz)) return NAN;
    if (s_cfg.poles == 0u || s_cfg.pulley_ratio <= 0.0f) return NAN;  /* unconfigured, GH#28 */

    /* RPM = f * 60 / (poles/2 * pulley_ratio). */
    return s_freq_hz * 60.0f / (((float)s_cfg.poles / 2.0f) * s_cfg.pulley_ratio);
}
