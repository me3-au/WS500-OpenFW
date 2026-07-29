/*
 * field_guard.h — test-fw ONLY: the sole path a bench-commanded field duty
 * takes to field_drive_set() (PROJECT_PLAN §4/M2 deliverable #14, §5 bench
 * safety, CONTRIBUTING.md's "compiled max-duty cap" rule).
 *
 * field_drive.c (shared with the production firmware) clamps to the full
 * 0-100% range because the real regulator needs it; this module exists so
 * that TEST-FW never asks for more than the installed 4-ohm rotor can safely
 * see (in-place, engine-off testing -- Sec5 decision, 2026-07-29).
 * Three independent guarantees, each load-bearing on its own:
 *
 *   1. HARD 20% DUTY CAP, compiled in, structurally unexceedable. field_
 *      guard_set() is the ONLY function in this firmware that calls
 *      field_drive_set() with a possibly-nonzero duty, and it unconditionally
 *      clamps to FIELD_GUARD_DUTY_CAP (field_guard.c) before the value
 *      reaches the timer. There is no command, config value, or build flag
 *      that raises the cap — changing it requires editing field_guard.c
 *      itself. (PROJECT_PLAN §5 rule 4: "duty-cycle cap compiled into test
 *      builds (20%)"; the installed unit's rotor is 12V-class on a
 *      48-57.6V bus, so sustained duty above ~25% overdrives it — §5's
 *      "installed-unit reality" note.)
 *   2. SELF-EXPIRING COMMAND. A commanded nonzero duty is only held for
 *      FIELD_GUARD_TIMEOUT_MS after the operator's most recent `field <pct>`
 *      command; field_guard_poll(), called every main-loop tick, drops the
 *      field the instant that window closes with no further input. An
 *      operator who walks away cannot leave the field energised.
 *   3. FAIL-SAFE DEFAULT. Every path that is not "operator just issued a
 *      fresh field <pct> command" leaves the field OFF: boot (field_drive_
 *      init() already starts at 0%, and field_guard_init() asserts it
 *      again), timeout, USB link drop (field_guard_link_lost(), called from
 *      main.c the instant CDC DTR drops — "a USB cable that falls out must
 *      not leave the field energized"), and any fault (field_guard never
 *      intercepts field_drive_fault_cutoff(); once that latches,
 *      field_drive_set() itself forces 0% regardless of what this module
 *      asks for — field_drive.c's s_faulted check).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TESTFW_FIELD_GUARD_H
#define WS500_TESTFW_FIELD_GUARD_H

#include <stdint.h>
#include <stdbool.h>

/* Reset to the fail-safe default (field off, no command held). Call once at
 * boot, after field_drive_init(). */
void field_guard_init(void);

/*
 * Request a field duty, given as a PERCENT (0..100, pre-clamp). Returns the
 * duty actually applied (0.0..FIELD_GUARD_DUTY_CAP, as a 0..1 fraction) so
 * the console can tell the operator when their request was capped. Refreshes
 * the self-expiry timer to FIELD_GUARD_TIMEOUT_MS from now — the ONLY way to
 * hold a nonzero duty longer than that window is to call this again.
 */
float field_guard_set(float requested_duty_pct_0_100);

/* Force the field off immediately and clear the held-command state. Safe to
 * call at any time, including when nothing is commanded. */
void field_guard_off(void);

/* Call once per main-loop tick (test-fw/main.c, 10 ms cadence): drops the
 * field the instant the self-expiry window closes. Never blocks. */
void field_guard_poll(void);

/* Call the instant the USB console link is observed to drop (main.c watches
 * usb_cdc_link_up()): forces the field off without waiting for the timeout. */
void field_guard_link_lost(void);

/* True while a nonzero duty is being actively held (i.e. has not expired,
 * been cancelled, or been overridden by a field_drive fault latch). */
bool field_guard_commanded(void);

/* Last duty actually written to field_drive_set(), as a 0..1 fraction
 * (already post-clamp). 0.0 whenever field_guard_commanded() is false. */
float field_guard_duty(void);

/* The compiled cap itself (0..1 fraction), for the console to print rather
 * than hard-coding the number a second place. */
float field_guard_cap(void);

/* Milliseconds remaining before the current command self-expires; 0 when
 * nothing is commanded. */
uint32_t field_guard_remaining_ms(void);

#endif /* WS500_TESTFW_FIELD_GUARD_H */
