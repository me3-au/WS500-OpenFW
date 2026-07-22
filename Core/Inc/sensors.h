/*
 * sensors.h — analog acquisition driver (ADC1, 7-ch scan, x4 oversample+average).
 * Raw slots [0..3] = PA1/PA2/PA3/PC5; [4..6] = temp/VREFINT/VBAT internal.
 *
 * Driver layer: exposes PHYSICAL readings only (V, A, °C). It does NOT know the
 * control types — the app assembles ctrl_measured_t from these + the INA driver.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_SENSORS_H
#define WS500_SENSORS_H

#include <stdint.h>

/* Physical readings produced by the analog front-end (driver-owned type). */
typedef struct {
    float vbat_pack_v;   /* battery voltage via PC5 divider */
    float bus_v;         /* bus voltage at the shunt (INA226/228/238) */
    float amps_batt;     /* charge current from the INA shunt monitor */
    float alt_temp_c;    /* alternator NTC (PA1) */
    float driver_temp_c; /* internal driver-stage NTC (PA2) */
    float batt_temp_c;   /* battery NTC (PA3) */
} sensor_readings_t;

void sensors_init(void);              /* ADC + DMA circular scan + INA init */
void sensors_update(void);            /* fold latest averaged frame */

uint16_t sensors_raw(unsigned slot);  /* averaged raw count for a scan slot */
float    sensors_vdda(void);          /* mV rail from VREFINT calibration */

/* Fill physical readings. NAN marks an open/short sensor. */
void sensors_read(sensor_readings_t *out);

#endif /* WS500_SENSORS_H */
