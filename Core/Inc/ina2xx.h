/*
 * ina2xx.h — TI INA226 current+power monitor over I2C1 (addr 0x40).
 *
 * The WS500's single current input (500A/50mV shunt, battery- or alternator-side
 * per $CCN ShuntAtBat) is read by this monitor — confirmed from the stock binary
 * (disasm 2026-07-23, §0.6 V3): INA226 only, hardwired; stock reads regs 0x06
 * (Mask/Enable conversion-ready), 0x02 (bus V), 0x01 (shunt V), 16-bit BE, and
 * computes CALIBRATION at runtime from the configured shunt. It never reads
 * DIE_ID — our optional variant probe below is a sanity check, not stock
 * behavior. The INA reports both charge current and local bus voltage.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_INA2XX_H
#define WS500_INA2XX_H

#include <stdint.h>
#include <stdbool.h>

typedef enum { INA_NONE=0, INA226, INA228, INA238 } ina_variant_t;

/* Detect the chip (reads DIE_ID/MANUF_ID) and program CALIBRATION from the shunt. */
bool          ina2xx_init(float shunt_full_scale_a, float shunt_full_scale_mv);
ina_variant_t ina2xx_variant(void);

float ina2xx_current_a(void);   /* charge current [A]  (SHUNT_V * calibration) */
float ina2xx_bus_v(void);       /* bus voltage at shunt [V] (BUS_V) */
float ina2xx_power_w(void);     /* [W] (POWER) */

#endif /* WS500_INA2XX_H */
