/*
 * field_guard.c — test-fw's field-command safety funnel. Contract, the three
 * guarantees, and why each matters: field_guard.h. Read that first.
 *
 * SPDX-License-Identifier: MIT
 */
#include "field_guard.h"
#include "field_drive.h"
#include "stm32f0xx_hal.h"   /* HAL_GetTick() only */

/*
 * SAFETY-CRITICAL CONSTANT — see field_guard.h guarantee 1. This is the ONLY
 * place in test-fw the cap is written down: no command sets it, no config
 * value overrides it, and the CMake target for this firmware (CMakeLists.txt)
 * carries no compile-definition that could redefine it. Changing the bench
 * duty ceiling means editing this line, in a reviewed commit, on purpose.
 *
 * PROJECT_PLAN §5 rule 4 ("duty-cycle cap compiled into test builds (20%)")
 * and CONTRIBUTING.md's "the compiled max-duty cap ... must never be
 * weakened". The installed unit's rotor is 4 ohm / 12V-class on a 48-57.6V
 * bus; §5's "installed-unit reality" note puts sustained overdrive above
 * ~25% (~21% at 57.6V absorption) — 20% sits with margin under that, and
 * matches the number PROJECT_PLAN §4/§5 both name explicitly.
 */
#define FIELD_GUARD_DUTY_CAP    0.20f

/*
 * Self-expiry window — field_guard.h guarantee 2. "A few seconds" per the
 * task brief: long enough that an operator watching a meter and re-issuing
 * `field <pct>` to hold a reading is never racing the clock, short enough
 * that a dropped session or a distracted operator cannot leave 20% field
 * current flowing into the dummy load (or, if this is ever run against a
 * rotor, the rotor) for any meaningful time. [SPEC-SIGNOFF: an engineering
 * choice for this test firmware, not a spec-mandated value.]
 */
#define FIELD_GUARD_TIMEOUT_MS  5000u

static bool     s_commanded;      /* true while a nonzero duty is being held */
static uint32_t s_last_cmd_ms;
static float    s_applied_duty;   /* last duty ACTUALLY written, 0..1, post-clamp */

void field_guard_init(void)
{
    s_commanded    = false;
    s_last_cmd_ms  = HAL_GetTick();
    s_applied_duty = 0.0f;
    field_drive_off();   /* fail-safe, explicit — field_drive_init() already
                          * starts at 0%; this just asserts it a second time
                          * so field_guard's own state and the hardware never
                          * disagree about the boot default. */
}

float field_guard_set(float requested_duty_pct_0_100)
{
    float duty = requested_duty_pct_0_100 / 100.0f;
    /* THE clamp — field_guard.h guarantee 1; this is the only call to
     * field_drive_set() with a possibly-nonzero duty in all of test-fw.
     *
     * INVERTED comparisons so NaN cannot walk through: `duty < 0` and
     * `duty > CAP` are both false for NaN, which would have made the header's
     * "structurally unexceedable" claim untrue and — worse — left
     * s_commanded false (NaN > 0 is false), so the 5 s self-expiry would
     * never have armed to clean it up. Written this way NaN lands on 0.
     * Unreachable from the console today (cfg_json_scan_num emits only finite
     * values or ±HUGE_VAL, and +inf clamps correctly), but this is the one
     * safety-critical clamp in this firmware and it should not depend on the
     * parser upstream of it staying that way. */
    if (!(duty > 0.0f)) duty = 0.0f;                            /* NaN, negatives */
    if (!(duty < FIELD_GUARD_DUTY_CAP)) duty = FIELD_GUARD_DUTY_CAP;

    /* Don't book a command the hardware is refusing to honour. After a
     * `cutoff` latch (or a LIMP boot), field_drive_set() forces CCR to 0 and
     * safe_state may have taken PA8 to GPIO-low — so recording duty/commanded
     * here would make `status` report "applied=15% ... faulted=YES", which
     * reads like the field might be live when it provably is not. Report the
     * truth: nothing applied, nothing to expire. */
    if (field_drive_faulted()) {
        field_drive_set(0.0f);
        s_applied_duty = 0.0f;
        s_commanded    = false;
        return 0.0f;
    }

    field_drive_set(duty);
    s_applied_duty = duty;
    s_commanded    = (duty > 0.0f);
    s_last_cmd_ms  = HAL_GetTick();
    return duty;
}

void field_guard_off(void)
{
    field_drive_off();
    s_commanded    = false;
    s_applied_duty = 0.0f;
}

void field_guard_poll(void)
{
    if (!s_commanded) return;
    /* Unsigned wraparound-safe elapsed-time check, same idiom as stator_rpm.c
     * and watchdog.c use for HAL_GetTick() deltas. */
    if ((uint32_t)(HAL_GetTick() - s_last_cmd_ms) >= FIELD_GUARD_TIMEOUT_MS)
        field_guard_off();   /* self-expiry — field_guard.h guarantee 2 */
}

void field_guard_link_lost(void)
{
    /* Don't wait for the timeout window once the console is gone — the task
     * brief calls this out by name ("a USB cable that falls out must not
     * leave the field energized"). Cheap to call even when nothing is
     * commanded (field_guard_off() is a no-op-ish reassertion in that case). */
    if (s_commanded) field_guard_off();
}

bool  field_guard_commanded(void) { return s_commanded; }
float field_guard_duty(void)      { return s_applied_duty; }
float field_guard_cap(void)       { return FIELD_GUARD_DUTY_CAP; }

uint32_t field_guard_remaining_ms(void)
{
    if (!s_commanded) return 0u;
    const uint32_t elapsed = (uint32_t)(HAL_GetTick() - s_last_cmd_ms);
    return (elapsed >= FIELD_GUARD_TIMEOUT_MS) ? 0u
                                                : (FIELD_GUARD_TIMEOUT_MS - elapsed);
}
