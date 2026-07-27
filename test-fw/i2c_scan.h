/*
 * i2c_scan.h — test-fw's I2C1 + I2C2 bus scan: the bench falsification step
 * for the ina2xx.c "BUS CAVEAT" (PROJECT_PLAN §0.6 V7 corrects V3 — the
 * INA226 is expected on I2C2, not the I2C1 ina2xx.c currently drives; scan
 * BOTH and report what is actually there rather than trusting either claim).
 * PROJECT_PLAN §4/M2 deliverable #14.
 *
 * Expected findings, per §0.6 V3/V7: INA226 @ 0x40 and 24C16 EEPROM @ 0x50,
 * BOTH on I2C2, with I2C1 idle. The README's superseded "0x0C/0x10/0x4C
 * devices" item is REFUTED (V3 — those were length/size arguments misread as
 * addresses in an earlier disassembly pass) — there is nothing at those
 * addresses to identify; this scanner reports whatever ACKs, nothing more.
 *
 * Tick-driven, one address probed per bus per tick: probing all 112
 * candidate 7-bit addresses (0x08..0x77) on two buses synchronously would be
 * a multi-hundred-millisecond blocking call, which this codebase's "never
 * block" house style (usb_cdc.h, can_n2k.h) and the §7-style watchdog
 * checkpoint budgets (30-60 ms) both rule out. Spreading it over ~110 ticks
 * (~1.1 s at the 10 ms main-loop cadence) keeps every single tick's I2C work
 * bounded to two short HAL_I2C_IsDeviceReady() calls.
 *
 * BENCH NOTE: this scan brings up ITS OWN temporary I2C1/I2C2 handles,
 * separate from the persistent ones ina2xx.c and eeprom24c16.c own — see
 * i2c_scan.c for why, and for the (expected, self-healing) side effect on
 * those drivers' live traffic while a scan is in progress.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_TESTFW_I2C_SCAN_H
#define WS500_TESTFW_I2C_SCAN_H

#include <stdint.h>
#include <stdbool.h>

/* Reset internal state. Does not touch any I2C peripheral. */
void i2c_scan_init(void);

/* Begin a fresh scan of both buses. Returns false if a scan is already in
 * progress (call i2c_scan_busy() first, or just let it finish). */
bool i2c_scan_start(void);

/* True while a scan is in progress. */
bool i2c_scan_busy(void);

/*
 * Advance the scan by one step. Call every main-loop tick (test-fw/main.c);
 * a no-op when idle. Returns true on EXACTLY the tick the scan completes,
 * which is the console's cue to print the results table via
 * i2c_scan_found(). Never blocks.
 */
bool i2c_scan_poll(void);

/* True if the most recently completed scan found an ACK from `addr7`
 * (7-bit, 0x08..0x77) on I2C `bus` (1 or 2). False for any other `bus`
 * value or before any scan has ever completed. */
bool i2c_scan_found(int bus, uint8_t addr7);

#endif /* WS500_TESTFW_I2C_SCAN_H */
