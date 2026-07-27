/*
 * can_test.h — test-fw's bxCAN bench self-test: internal silent-loopback
 * (no wiring needed) and an external echo test (needs a terminated, live
 * bus — analyzer or a second node). PROJECT_PLAN §4/M2 deliverable #14.
 *
 * Deliberately NOT can_n2k.c/n2k_sched.c/n2k_addrclaim.c: those implement the
 * full NMEA2000/RV-C protocol stack (address claim, PGN scheduling) over a
 * control/telemetry snapshot this bring-up firmware has no reason to carry.
 * This module only needs the bxCAN bit-timing/bring-up PATTERN, which it
 * takes from can_n2k.c's documented math (250 kbit/s, 87.5% sample point —
 * see can_test.c) without linking any of that code.
 *
 * Tick-driven, never blocking: a test is STARTED by a console command and
 * SERVICED by can_test_poll() from the main-loop tick, matching the whole
 * codebase's "never block" house style (usb_cdc.h, can_n2k.h). This also
 * keeps the §7-style watchdog checkpoints flowing normally while a test with
 * a multi-hundred-millisecond timeout window is in flight — a synchronous
 * blocking wait here would starve the watchdog for no good reason.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TESTFW_CAN_TEST_H
#define WS500_TESTFW_CAN_TEST_H

#include <stdint.h>
#include <stdbool.h>

/* Reset internal state. The CAN peripheral itself is NOT brought up here —
 * only on the first can_test_start_loop()/can_test_start_echo() call — so a
 * freshly booted test-fw puts nothing on the physical bus until an operator
 * explicitly asks for a test. */
void can_test_init(void);

/*
 * Start the internal self-test: bxCAN in CAN_MODE_SILENT_LOOPBACK (silent =
 * never drives the physical TX pin; loopback = internally routes TX to RX).
 * No external wiring required — proves the peripheral clocking, bit-timing
 * and mailbox/FIFO plumbing are alive. Returns false if bxCAN bring-up or
 * the initial transmit failed immediately (console should report that as an
 * instant FAIL rather than waiting on can_test_poll()).
 */
bool can_test_start_loop(void);

/*
 * Start the external test: bxCAN in CAN_MODE_NORMAL, transmitting a
 * distinctive test frame onto the real bus and watching for it to come back
 * on Rx. On a live CAN bus this SHOULD succeed with no special "echo" device:
 * standard CAN self-reception means a transmitting node also receives its
 * own frame once at least one other node acknowledges it — so this test
 * needs a terminated bus with a real second node or analyzer present, not a
 * bespoke echo responder. Without one, AutoRetransmission means the frame
 * never completes (bxCAN keeps retrying until this test's bounded window
 * times out), which is the correct, safe, informative failure mode — not a
 * hang. Returns false if bxCAN bring-up or the initial transmit failed
 * immediately.
 */
bool can_test_start_echo(void);

/* True while a test is in flight (started, not yet concluded). */
bool can_test_busy(void);

/*
 * Advance the state machine one tick. Call every main-loop tick (test-fw/
 * main.c, ~10 ms cadence) regardless of whether a test is running — a no-op
 * when idle. Returns true on EXACTLY the tick a running test concludes
 * (pass or timeout), which is the console's cue to print the result via
 * can_test_last_pass()/can_test_last_was_echo(). Never blocks.
 */
bool can_test_poll(void);

/* Outcome of the most recently concluded test (loop or echo). Meaningless
 * before any test has ever concluded. */
bool can_test_last_pass(void);

/* True if the most recently concluded test was the external echo test
 * (false = internal loopback). */
bool can_test_last_was_echo(void);

#endif /* WS500_TESTFW_CAN_TEST_H */
