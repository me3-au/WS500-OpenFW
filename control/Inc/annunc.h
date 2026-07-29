/*
 * annunc.h — status-LED blink encoder (CONTROL_SPEC_NEXTGEN.md §9.2). PURE.
 *
 * GH#42: the fault layer blinks the SAME pick `n2k_alert_from_telemetry()`
 * makes (faults.h's `ctrl_fault_top_code()`), as a floor(code/10)-long +
 * (code mod 10)-short flash pattern; the "no active fault" state layer is a
 * heartbeat defined in this file (see below). No pin is bound yet — PA9
 * (board.h `OUT_LAMP_OR_LED_PIN`) is stock-binary console TX evidence
 * (PROJECT_PLAN §0.6 V7), while IO_COVERAGE.md names PA0/PB14 instead, and
 * PA0 is not even GPIO-initialised (board.c). No tier-1 (bench) evidence
 * exists for any of the three, so per §0.6 evidence precedence and §5 this
 * module stops at pure logic; the pin binding is Stage A work.
 *
 * Caller-owned state only (`ctrl_annunc_t`) — no file-scope statics, the
 * same rule thermal.c's two independent governor instances rely on. All
 * timing is fixed by §9.2, not configurable.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_ANNUNC_H
#define WS500_ANNUNC_H

#include "control.h"

/* Fault-layer flash timing (§9.2, fixed by spec). */
#define CTRL_ANNUNC_LONG_MS    600u   /* one "long" flash, on-time */
#define CTRL_ANNUNC_SHORT_MS   200u   /* one "short" flash, on-time */
#define CTRL_ANNUNC_GAP_MS     300u   /* off-time between two flashes of one pattern */
#define CTRL_ANNUNC_REPEAT_MS 2000u   /* off-time before the pattern repeats */

/*
 * State-layer ("no active fault") heartbeat — §9.2 says this layer is
 * "defined with the LED driver, not here"; GH#42 is that definition. A
 * single short on-pulse well below any fault flash's on-time, repeating on a
 * period longer than every fault pattern's *shortest* full repeat cycle
 * (code 1 = 1 short: 200 ms on + 2000 ms gap = 2200 ms) so a healthy unit's
 * blink can never be super-imposed or aliased into looking like a stuck or
 * legitimate fault code at a glance — the two must look different, not just
 * be different, since misreading "healthy" as any fault code (or worse, the
 * reverse) on the one output the operator has in the field is the failure
 * this layer exists to prevent.
 */
#define CTRL_ANNUNC_HEARTBEAT_ON_MS     100u
#define CTRL_ANNUNC_HEARTBEAT_PERIOD_MS 3000u

/* Internal phase within the current flash/gap cycle. */
typedef enum {
    CTRL_ANNUNC_PH_FLASH = 0,  /* lamp on: one long/short flash, or the heartbeat pulse */
    CTRL_ANNUNC_PH_INTER,      /* lamp off: between two flashes of the same pattern */
    CTRL_ANNUNC_PH_GAP         /* lamp off: before the pattern (or heartbeat) repeats */
} ctrl_annunc_phase_t;

/*
 * Caller-owned blink state. Zero-initialize via ctrl_annunc_init(), then
 * pass the SAME instance to every ctrl_annunc_update() call — nothing here
 * is a static or a singleton, matching every other PURE module's pattern
 * (ctrl_thermal_t, ctrl_vsup_guard_t, ...).
 */
typedef struct {
    unsigned code;         /* wire code (§9.1) latched for the CURRENT cycle;
                            * 0 = heartbeat (no active fault) */
    unsigned longs;         /* long-flash count for `code` (0 for heartbeat) */
    unsigned total;         /* total flash count this cycle (>=1 always) */
    unsigned flash_idx;      /* next flash index to emit within the cycle, 0-based */
    ctrl_annunc_phase_t phase;
    uint32_t phase_ms;      /* elapsed ms within the current phase */
} ctrl_annunc_t;

/* Idle-initialize: starts on the heartbeat (state) layer, lamp about to turn
 * on at the very first ctrl_annunc_update() call. Call once before use. */
void ctrl_annunc_init(ctrl_annunc_t *a);

/*
 * ctrl_annunc_update — advance the blink state machine one tick.
 *   a      : caller-owned state, persists across calls (see ctrl_annunc_t)
 *   faults : active fault bitfield — pass `ctrl_telemetry_t.faults` straight
 *            through; this is the natural subscription point (telemetry.h)
 *   dt_ms  : elapsed ms since the previous call (any size; large jumps are
 *            walked through phase-by-phase, so a coarse caller tick rate is
 *            fine and so is a single big catch-up call)
 * Returns true if the lamp should be ON for this instant.
 *
 * The fault pick (ctrl_fault_top_code(), faults.h) is only re-evaluated at a
 * cycle boundary (the end of the repeat/heartbeat gap) — never mid-pattern —
 * so a fault appearing, changing, or clearing mid-blink can only ever change
 * what plays NEXT, never truncate or splice the pattern currently on screen
 * into a malformed hybrid. Worst case an operator waits up to one full cycle
 * (<= ~2.4 s for the longest v1 pattern, code 19) to see a new fault appear;
 * the LED is advisory only and never gates the field-drive path, so that
 * latency is a display detail, not a safety one.
 */
bool ctrl_annunc_update(ctrl_annunc_t *a, uint32_t faults, uint32_t dt_ms);

#endif /* WS500_ANNUNC_H */
