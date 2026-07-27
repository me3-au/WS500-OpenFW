/*
 * console.h — test-fw's line-oriented command console over the shared
 * usb_cdc.c CDC-ACM transport. PROJECT_PLAN §4/M2 deliverable #14.
 *
 * Human-driven at a terminal: type `help` for the command list. This module
 * owns ALL printing in test-fw (drivers/tests stay print-free) and is the
 * only place `field <pct>` text is parsed — see field_guard.h for why the
 * actual duty clamp lives one layer down, not here.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TESTFW_CONSOLE_H
#define WS500_TESTFW_CONSOLE_H

/* Reset line-assembly state and print the banner + a prompt. Call once at
 * boot, after usb_cdc_init(). */
void console_init(void);

/*
 * Re-print the banner + prompt on a freshly-opened session. Call on the DTR
 * RISING edge: the boot-time banner from console_init() is written before the
 * host has enumerated us, so the transport drops it and the operator never
 * sees the PROJECT_PLAN §5 "BENCH ONLY / DUMMY LOAD ONLY / capped at 20 %"
 * notice — which is precisely the reader who must not miss it. Also resets
 * line assembly, so a half-typed command from a previous session cannot
 * prefix the first command of this one.
 */
void console_greet(void);

/*
 * Byte pump + command dispatch. Call every main-loop iteration (test-fw/
 * main.c), NOT gated to the 10 ms tick — matches the production main.c's
 * treatment of config_poll()/telem_stream_poll() as every-iteration comms
 * work. Never blocks: drains whatever usb_cdc_read() currently has queued,
 * dispatches any complete (newline-terminated) lines, and returns.
 */
void console_poll(void);

/* Print the outcome of the CAN self-test that just concluded. Call from
 * test-fw/main.c exactly on the tick can_test_poll() returns true. */
void console_report_can_result(void);

/* Print the results table of the I2C scan that just concluded. Call from
 * test-fw/main.c exactly on the tick i2c_scan_poll() returns true. */
void console_report_i2c_result(void);

#endif /* WS500_TESTFW_CONSOLE_H */
