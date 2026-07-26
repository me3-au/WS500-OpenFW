/*
 * watchdog.c — R1: checkpoint-kicked windowed IWDG (PROJECT_PLAN §7 R1).
 * Policy and call contract: watchdog.h.
 * SPDX-License-Identifier: MIT
 *
 * ============================ CONSTANT DERIVATION ==========================
 * All of the numbers below follow from two facts and one decision.
 *
 * FACT 1 — the LSI is not accurate. STM32F072 datasheet: LSI ~40 kHz typical,
 *   specified 30 kHz .. 60 kHz over voltage and temperature. Every IWDG period
 *   therefore has to be read as a RANGE, and both ends of that range have to
 *   be safe. This is the single most important thing about sizing an IWDG and
 *   the usual source of field failures ("worked on my bench at 25 °C").
 *
 * FACT 2 — the control loop is a 10 ms fixed-rate loop (main.c LOOP_PERIOD_MS).
 *
 * DECISION — the timeout should be ~10 loop periods: long enough that normal
 *   jitter, a slow I²C transaction or a config write can never trip it, short
 *   enough that a hung regulator stops driving the field in well under the
 *   time an alternator needs to do damage (~100 ms, vs the several seconds a
 *   thermal event takes to develop, and vs the stock firmware's 6.5–26 s).
 *
 *   IWDG_PRESCALER = 4  -> LSI/64  -> 1.60 ms/tick nominal (1.07 .. 2.13 ms)
 *   IWDG_RELOAD    = 62 -> 63 ticks:  100.8 ms nominal, 67.2 ms fast-LSI,
 *                                     134.4 ms slow-LSI
 *   Worst case (fast LSI, 67 ms) is still 6.7 loop periods — a missed kick or
 *   two never resets us; a genuinely stopped loop always does.
 *
 * WINDOW — IWDG_WINDOW = RELOAD - 3, i.e. a refresh is only accepted once at
 *   least 3 ticks have elapsed since the last one: 4.8 ms nominal, 6.4 ms at
 *   the SLOW end of the LSI range (the slow end is the binding one here — a
 *   slower LSI makes the minimum interval LONGER in real time, so it is what
 *   the software floor has to clear).
 *
 * SOFTWARE FLOOR — WDG_MIN_KICK_INTERVAL_MS = 8 ms, deliberately sandwiched
 *   between the 6.4 ms worst-case hardware minimum and the 10 ms loop period:
 *     * above 6.4 ms, so a legitimate kick can never trip the window even with
 *       the LSI at the slow end of spec. This matters because main.c's loop
 *       gate is catch-up style (`next += LOOP_PERIOD_MS`), so an overrun cycle
 *       can be followed IMMEDIATELY by the next one — without this floor those
 *       back-to-back iterations would look exactly like a runaway loop;
 *     * below 10 ms, so the normal cadence is never suppressed.
 *   Skipping a kick is always safe (the next one is 10 ms away and the timeout
 *   is at least 67 ms); tripping the window is not.
 *
 * All of the above are engineering choices, not spec-mandated values, and are
 * marked [SPEC-SIGNOFF] pending review.
 * ===========================================================================
 */
#include "watchdog.h"
#include "crash_record.h"
#include "safe_state.h"
#include "stm32f0xx.h"
#include "stm32f0xx_hal.h"    /* HAL_GetTick() only */

/* [SPEC-SIGNOFF] — see the derivation block above. */
#define IWDG_PRESCALER_CODE   4u    /* PR=4 -> LSI/64 */
#define IWDG_RELOAD           62u   /* 63 ticks ~= 100.8 ms @40 kHz LSI */
#define IWDG_WINDOW           (IWDG_RELOAD - 3u)
#define WDG_MIN_KICK_INTERVAL_MS  8u

/* Per-checkpoint staleness budgets [ms]. Control and sensors run every 10 ms
 * so 30 ms is three chances; comms is pumped every loop iteration but its work
 * (a config line, a CAN frame) is bursty, so it gets a looser 60 ms. A budget
 * looser than the IWDG timeout would be pointless — all of these are well
 * inside the 67 ms worst-case timeout. [SPEC-SIGNOFF] */
static const uint32_t s_budget_ms[WDG_CP_COUNT] = {
    [WDG_CP_CONTROL] = 30u,
    [WDG_CP_SENSORS] = 30u,
    [WDG_CP_COMMS]   = 60u,
};

/* How long the system must run cleanly before we forget the escalation streak.
 * 60 s is far longer than any boot-time transient (config load, first CAN
 * traffic, alternator spin-up) and short enough that a genuinely healthy unit
 * clears its history on the first charge cycle. Clearing it any earlier would
 * defeat §7 R1's "don't oscillate reset<->run": a fault that kills us 5 s into
 * every boot must still be seen as consecutive. [SPEC-SIGNOFF] */
#define WDG_HEALTHY_RUNTIME_MS  60000u

static uint32_t s_last_report[WDG_CP_COUNT];
static bool     s_reported[WDG_CP_COUNT];
static uint32_t s_last_kick_ms;
static uint32_t s_starved_mask;
static uint32_t s_kicks;
static uint32_t s_starves;
static bool     s_limp;
static bool     s_streaks_cleared;
static bool     s_running;

/* Bounded wait for the IWDG's PR/RLR/WINR shadow registers to accept an
 * update. Never an unbounded poll: on a part whose LSI failed to start, SR
 * would never clear and an unbounded loop here would hang the whole boot in
 * the one place that is supposed to protect against hangs. The bound is ~40
 * IWDG ticks' worth of CPU cycles at 48 MHz — far more than the few LSI cycles
 * the update needs. If it expires we continue anyway: a watchdog configured
 * with stale-but-valid values is better than no boot. */
static void iwdg_wait_ready(uint32_t mask)
{
    for (uint32_t i = 0; i < 200000u; i++) {
        if ((IWDG->SR & mask) == 0u) return;
    }
}

void watchdog_init(void)
{
    /* Freeze the counter while a debugger holds the core halted. Costs nothing
     * in the field (the bit only has an effect when the core is halted by a
     * debugger) and makes single-stepping the control loop possible at all. */
    RCC->APB2ENR   |= RCC_APB2ENR_DBGMCUEN;
    DBGMCU->APB1FZ |= DBGMCU_APB1_FZ_DBG_IWDG_STOP;

    /* §7 R1 escalation: decided once, here, from the counters crash_record.c
     * carried across the reset. Held in a local flag so the rest of the
     * firmware sees a stable answer for the whole boot even if the streaks are
     * cleared later by a healthy runtime. */
    s_limp = crash_limp_latched();
    if (s_limp) {
        /* Belt and braces: the control core will command field-open via
         * CTRL_FAULT_WATCHDOG (main.c), but do not wait a whole loop
         * iteration to make the hardware safe. */
        enter_safe_state();
    }

    /* KR=0xCCCC starts the IWDG (and the LSI with it — no need to enable or
     * poll LSIRDY, which would be another unbounded wait). KR=0x5555 unlocks
     * PR/RLR/WINR. Writing WINR performs a reload, so the counter starts from
     * a full period. RM0091 §21. */
    IWDG->KR  = 0xCCCCu;
    IWDG->KR  = 0x5555u;
    IWDG->PR  = IWDG_PRESCALER_CODE;
    IWDG->RLR = IWDG_RELOAD;
    iwdg_wait_ready(IWDG_SR_PVU | IWDG_SR_RVU);
    IWDG->WINR = IWDG_WINDOW;
    iwdg_wait_ready(IWDG_SR_WVU);

    const uint32_t now = HAL_GetTick();
    for (unsigned i = 0; i < (unsigned)WDG_CP_COUNT; i++) {
        s_reported[i]    = false;
        s_last_report[i] = now;
    }
    s_last_kick_ms = now;
    s_starved_mask = 0u;
    s_running      = true;
}

void watchdog_checkpoint(wdg_checkpoint_t cp)
{
    if ((unsigned)cp >= (unsigned)WDG_CP_COUNT) return;
    s_last_report[cp] = HAL_GetTick();
    s_reported[cp]    = true;
}

void watchdog_service(void)
{
    if (!s_running) return;

    const uint32_t now = HAL_GetTick();

    uint32_t stale = 0u;
    for (unsigned i = 0; i < (unsigned)WDG_CP_COUNT; i++) {
        if (!s_reported[i] || (now - s_last_report[i]) > s_budget_ms[i])
            stale |= (1u << i);
    }
    s_starved_mask = stale;

    if (stale != 0u) {
        s_starves++;
        return;                     /* let the IWDG do its job */
    }

    /* Hardware window: too-early refreshes reset the part, so hold off until
     * the software floor has passed (see the derivation block). */
    if ((now - s_last_kick_ms) < WDG_MIN_KICK_INTERVAL_MS) return;

    IWDG->KR = 0xAAAAu;             /* refresh */
    s_last_kick_ms = now;
    s_kicks++;

    /* A long clean run means the escalation streak was a one-off. Do this once
     * per boot, from the only place that knows the loop is genuinely healthy. */
    if (!s_streaks_cleared && now >= WDG_HEALTHY_RUNTIME_MS) {
        crash_clear_streaks();
        s_streaks_cleared = true;
    }
}

bool     watchdog_limp_latched(void) { return s_limp; }
uint32_t watchdog_starved_mask(void) { return s_starved_mask; }
uint32_t watchdog_kick_count(void)   { return s_kicks; }
uint32_t watchdog_starve_count(void) { return s_starves; }
