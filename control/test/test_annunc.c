/*
 * test_annunc.c — LED blink encoder (GH#42, CONTROL_SPEC_NEXTGEN.md §9.2).
 *
 * Oracle: the §9.1 Blink column, walked programmatically (floor(code/10)
 * long + code%10 short) rather than hand-transcribed per fault, so all 19
 * wire codes are exercised from the same table the spec is built from.
 * Also covers: tie-breaking (shared picker with n2k_encode.c, GH#42), the
 * fault<->no-fault heartbeat transition, that a fault change mid-pattern
 * cannot splice a malformed hybrid, and timing-boundary edge cases.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "annunc.h"
#include "control.h"

/* Every v1 code (§9.1) has at most 10 flashes (code 19 = 1L+9S); a segment
 * list alternates flash/inter-gap plus one trailing repeat gap, so 2*10 is
 * the largest a pattern can need. Generous headroom, not a tight bound. */
#define ANNUNC_MAX_SEGS 32

typedef struct { uint32_t dur; bool on; } annunc_seg_t;

/* Fast-forward exactly one full boot heartbeat cycle so `a` lands on the
 * very first instant (t=0) of whatever pattern `faults` currently selects —
 * every case below starts from this known boundary instead of hand-walking
 * ctrl_annunc_init()'s heartbeat state. Relies on annunc.h's documented
 * "re-pick only at a cycle boundary" contract: one full heartbeat period
 * (3000 ms) is exactly one boundary crossing away from init(). */
static void annunc_start(ctrl_annunc_t *a, uint32_t faults)
{
    ctrl_annunc_init(a);
    CHECK(ctrl_annunc_update(a, faults, CTRL_ANNUNC_HEARTBEAT_PERIOD_MS) == true);
}

/* Build the expected on/off segment list for a fault `code` per §9.1's
 * floor(code/10) long + code%10 short rule, ending with the 2 s repeat gap. */
static unsigned build_segments(unsigned code, annunc_seg_t *segs)
{
    unsigned longs = code / 10u, shorts = code % 10u, total = longs + shorts;
    unsigned n = 0, k;
    for (k = 0; k < total; k++) {
        segs[n].dur = (k < longs) ? CTRL_ANNUNC_LONG_MS : CTRL_ANNUNC_SHORT_MS;
        segs[n].on  = true;
        n++;
        if (k + 1 < total) {           /* inter-flash gap, not after the last */
            segs[n].dur = CTRL_ANNUNC_GAP_MS;
            segs[n].on  = false;
            n++;
        }
    }
    segs[n].dur = CTRL_ANNUNC_REPEAT_MS;
    segs[n].on  = false;
    n++;
    return n;
}

/*
 * check_pattern — verify `faults` produces exactly `code`'s §9.1 pattern.
 *
 * For every segment boundary B_i (cumulative ms from pattern start), two
 * independent fresh runs assert: state at B_i-1 is still segment i's value,
 * and state AT B_i already reads as the next segment's value (this engine's
 * documented convention: a phase's duration is the ON ramp, and the instant
 * the cumulative time reaches it, the transition has already happened —
 * see annunc.c). The last segment wraps to segment 0, so this also proves
 * the pattern repeats identically rather than drifting or truncating.
 */
static void check_pattern(uint32_t faults, unsigned code)
{
    annunc_seg_t segs[ANNUNC_MAX_SEGS];
    unsigned n = build_segments(code, segs);
    ctrl_annunc_t base;
    uint32_t cumulative = 0;
    unsigned i;

    annunc_start(&base, faults);

    for (i = 0; i < n; i++) {
        ctrl_annunc_t a;
        cumulative += segs[i].dur;

        a = base;
        CHECK(ctrl_annunc_update(&a, faults, cumulative - 1u) == segs[i].on);

        a = base;
        CHECK(ctrl_annunc_update(&a, faults, cumulative) == segs[(i + 1) % n].on);
    }
}

/* Every one of the 19 wire codes (§9.1), isolated to a single active bit so
 * ctrl_fault_top_code() trivially selects it regardless of severity ordering
 * (severity/code tie-breaking is exercised separately, below). Bit position
 * == code-1 is the §9.1 wire-stability contract itself, not an assumption. */
static void test_annunc_all_codes(void)
{
    unsigned code;
    for (code = 1; code <= 19; code++)
        check_pattern(1u << (code - 1u), code);
}

/* §9.2: "deterministically the same pick" as n2k_alert_from_telemetry() —
 * highest severity wins, lowest code breaks a severity tie. */
static void test_annunc_tiebreak(void)
{
    /* OVERVOLTAGE (code 1, CRITICAL) beats SELF_OVERTEMP (code 5, WARN):
     * mirrors test_n2k_encode.c's identical bitfield -> alert_id 1 case, so
     * the LED and the N2K alert can never disagree on this fault. */
    check_pattern(CTRL_FAULT_OVERVOLTAGE | CTRL_FAULT_SELF_OVERTEMP, 1);

    /* Equal severity (both CRITICAL/OPEN-class): lowest code wins — code 1
     * (OVERVOLTAGE) over code 19 (DRIVER_OVERTEMP), not the reverse. */
    check_pattern(CTRL_FAULT_OVERVOLTAGE | CTRL_FAULT_DRIVER_OVERTEMP, 1);

    /* Same tie, opposite bit order in the literal: a bitfield has no
     * "first" bit, so the result must not depend on operand order. */
    check_pattern(CTRL_FAULT_DRIVER_OVERTEMP | CTRL_FAULT_WATCHDOG, 16);
}

/* §9.2's "no active fault -> state layer": this file's heartbeat definition
 * (annunc.h). Confirms the shape AND that it cannot alias a fault pattern:
 * shorter on-time than any fault flash, longer period than the fastest full
 * fault repeat cycle (code 1: 200 + 2000 = 2200 ms). */
static void test_annunc_heartbeat(void)
{
    ctrl_annunc_t a;
    ctrl_annunc_init(&a);

    CHECK(ctrl_annunc_update(&a, 0, CTRL_ANNUNC_HEARTBEAT_ON_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, 0, 1u) == false);
    CHECK(ctrl_annunc_update(&a, 0,
          CTRL_ANNUNC_HEARTBEAT_PERIOD_MS - CTRL_ANNUNC_HEARTBEAT_ON_MS - 1u) == false);
    CHECK(ctrl_annunc_update(&a, 0, 1u) == true);   /* repeats */

    CHECK(CTRL_ANNUNC_HEARTBEAT_ON_MS < CTRL_ANNUNC_SHORT_MS);
    CHECK(CTRL_ANNUNC_HEARTBEAT_PERIOD_MS >
          (CTRL_ANNUNC_SHORT_MS + CTRL_ANNUNC_REPEAT_MS));
}

/* Fault <-> no-fault <-> fault: neither transition may cut a pattern (or the
 * heartbeat pulse) short — the pick only changes at a cycle boundary. */
static void test_annunc_transitions(void)
{
    ctrl_annunc_t a;

    /* Boot heartbeat in flight; a fault becomes active mid-pulse and must
     * NOT preempt it. */
    ctrl_annunc_init(&a);
    CHECK(ctrl_annunc_update(&a, CTRL_FAULT_OVERVOLTAGE,
          CTRL_ANNUNC_HEARTBEAT_ON_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, CTRL_FAULT_OVERVOLTAGE, 1u) == false);
    CHECK(ctrl_annunc_update(&a, CTRL_FAULT_OVERVOLTAGE,
          CTRL_ANNUNC_HEARTBEAT_PERIOD_MS - CTRL_ANNUNC_HEARTBEAT_ON_MS - 1u) == false);
    /* Heartbeat's own boundary: the fault pattern (code 1 = 1S) starts now. */
    CHECK(ctrl_annunc_update(&a, CTRL_FAULT_OVERVOLTAGE, 1u) == true);

    /* Fault clears mid-pattern: code 1's single short flash + its 2 s repeat
     * gap must finish exactly as if the fault were still active. */
    CHECK(ctrl_annunc_update(&a, 0, CTRL_ANNUNC_SHORT_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, 0, 1u) == false);
    CHECK(ctrl_annunc_update(&a, 0, CTRL_ANNUNC_REPEAT_MS - 1u) == false);
    /* Repeat-gap boundary: no fault now active, heartbeat resumes. */
    CHECK(ctrl_annunc_update(&a, 0, 1u) == true);
}

/* A fault CODE CHANGE mid-pattern (not just fault<->no-fault) must not
 * splice a hybrid — the in-flight pattern finishes exactly as the code that
 * started it, even though the new fault is higher severity and would win
 * the picker immediately if it were re-run mid-flight. */
static void test_annunc_no_hybrid_mid_pattern(void)
{
    ctrl_annunc_t a;
    const uint32_t old_f = CTRL_FAULT_FIELD_OPEN;   /* code 3: 3S, WARN */
    const uint32_t new_f = CTRL_FAULT_OVERVOLTAGE;  /* code 1: 1S, CRITICAL —
                                                      * would preempt instantly
                                                      * if re-picked mid-pattern */

    annunc_start(&a, old_f);

    /* Flash 1/3, still code 3. */
    CHECK(ctrl_annunc_update(&a, old_f, CTRL_ANNUNC_SHORT_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, old_f, 1u) == false);              /* -> inter-gap 1 */

    /* The new, higher-severity fault appears here, mid-pattern. */
    CHECK(ctrl_annunc_update(&a, new_f, CTRL_ANNUNC_GAP_MS - 1u) == false);
    CHECK(ctrl_annunc_update(&a, new_f, 1u) == true);                /* flash 2/3 */
    CHECK(ctrl_annunc_update(&a, new_f, CTRL_ANNUNC_SHORT_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, new_f, 1u) == false);               /* -> inter-gap 2 */
    CHECK(ctrl_annunc_update(&a, new_f, CTRL_ANNUNC_GAP_MS - 1u) == false);
    CHECK(ctrl_annunc_update(&a, new_f, 1u) == true);                /* flash 3/3 */
    CHECK(ctrl_annunc_update(&a, new_f, CTRL_ANNUNC_SHORT_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, new_f, 1u) == false);               /* -> repeat gap, NOT a
                                                                      * 4th flash or a code-1
                                                                      * splice */
    CHECK(ctrl_annunc_update(&a, new_f, CTRL_ANNUNC_REPEAT_MS - 1u) == false);
    /* Only NOW, at the repeat-gap boundary, does the new fault take over. */
    CHECK(ctrl_annunc_update(&a, new_f, 1u) == true);
}

/* Timing-boundary edge cases beyond the per-code sweep above: a dt_ms == 0
 * tick must not move the phase clock, and a single dt_ms spanning several
 * phases in one jump (a coarse or catching-up caller) must still land in
 * the correct phase — annunc.h's caller-tick-rate independence. */
static void test_annunc_boundaries(void)
{
    ctrl_annunc_t a;

    ctrl_annunc_init(&a);
    CHECK(ctrl_annunc_update(&a, 0, 0u) == true);
    CHECK(ctrl_annunc_update(&a, 0, 0u) == true);
    CHECK(ctrl_annunc_update(&a, 0, CTRL_ANNUNC_HEARTBEAT_ON_MS - 1u) == true);
    CHECK(ctrl_annunc_update(&a, 0, 1u) == false);

    /* code 2 (CTRL_FAULT_FIELD_SHORT, 2S): one big dt jump spans flash 1
     * (200 ms) + the inter-gap (300 ms) + half of flash 2, delivered as a
     * single update() call rather than incremental ticks. */
    annunc_start(&a, CTRL_FAULT_FIELD_SHORT);
    CHECK(ctrl_annunc_update(&a, CTRL_FAULT_FIELD_SHORT,
          CTRL_ANNUNC_SHORT_MS + CTRL_ANNUNC_GAP_MS + (CTRL_ANNUNC_SHORT_MS / 2u))
          == true);
}

void test_annunc(void)
{
    test_annunc_all_codes();
    test_annunc_tiebreak();
    test_annunc_heartbeat();
    test_annunc_transitions();
    test_annunc_no_hybrid_mid_pattern();
    test_annunc_boundaries();
}
