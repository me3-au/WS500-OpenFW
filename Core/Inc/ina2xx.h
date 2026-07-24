/*
 * ina2xx.h — TI INA226 current + bus-voltage monitor over I2C1 (addr 0x40).
 *
 * The WS500's single current input (500A/50mV shunt, battery- or alternator-side
 * per $CCN ShuntAtBat) is read by this monitor — confirmed from the stock binary
 * (disasm 2026-07-23, §0.6 V3): INA226 only, hardwired; stock reads regs 0x06
 * (Mask/Enable conversion-ready), 0x02 (bus V), 0x01 (shunt V), 16-bit BE. Our
 * driver mirrors that access pattern: gate on conversion-ready, read the raw
 * shunt/bus registers, and scale in software — current = V_shunt / R_shunt with
 * R_shunt derived from the configured shunt rating, no magic CALIBRATION
 * constant (see ina2xx.c). The INA reports both charge current and local bus
 * voltage. There is no variant auto-detect: the old INA228/238 die-ID probe was
 * fiction (§0.6 V3) and has been removed — see git history.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_INA2XX_H
#define WS500_INA2XX_H

#include <stdint.h>
#include <stdbool.h>

/* Bring up I2C1 and configure the INA226 for continuous shunt+bus conversions.
 * R_shunt is derived from the configured shunt rating (board config), e.g.
 * 500A/50mV -> 100 uOhm. Returns false on invalid arguments or if the device
 * fails to configure (readings then report NAN; caller may escalate). */
bool ina2xx_init(float shunt_full_scale_a, float shunt_full_scale_mv);

float ina2xx_current_a(void);   /* charge current [A]  (SHUNT_V / R_shunt); NAN if unavailable */
float ina2xx_bus_v(void);       /* bus voltage at shunt [V] (BUS_V); NAN if unavailable */
float ina2xx_power_w(void);     /* [W] = bus_v * current, software product; NAN if unavailable */

/* Health (§7 R6 style): ina2xx_ok() goes false once the consecutive-failure
 * budget is exhausted — readings report NAN so the caller can escalate; it
 * self-recovers on the next successful transaction. The cumulative transaction-
 * failure counter never resets (telemetry/diagnostics). */
bool     ina2xx_ok(void);
uint32_t ina2xx_xact_failures(void);

#endif /* WS500_INA2XX_H */
