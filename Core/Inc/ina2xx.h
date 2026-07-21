/*
 * ina2xx.h — TI INA226/INA228/INA238 current+power monitor over I2C (addr 0x40).
 *
 * The WS500's single current input (500A/50mV shunt, battery- or alternator-side
 * per $CCN ShuntAtBat) is read by this monitor — confirmed from the stock firmware
 * (reads SHUNT_V/BUS_V/POWER at 0x40, auto-detects via DIE_ID). The INA reports
 * both the charge current and the local bus voltage.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
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
