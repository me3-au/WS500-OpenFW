/*
 * sensors.h — analog acquisition driver (ADC1, 7-ch scan, x4 oversample+average).
 * Raw slots [0..3] = PA1/PA2/PA3/PC5; [4..6] = temp/VREFINT/VBAT internal.
 *
 * Driver layer: exposes PHYSICAL readings only (V, A, °C). It does NOT know the
 * control types — the app assembles ctrl_measured_t from these + the INA driver.
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_SENSORS_H
#define WS500_SENSORS_H

#include <stdint.h>
#include "control.h"   /* ctrl_batt_temp_src_t — Core/ may depend on control/,
                        * never the reverse (CLAUDE.md control/ HAL-free rule) */

/* Physical readings produced by the analog front-end (driver-owned type). */
typedef struct {
    float vbat_pack_v;   /* battery voltage via PC5 divider */
    float bus_v;         /* bus voltage at the shunt (INA226) */
    float amps_batt;     /* charge current from the INA shunt monitor */
    float alt_temp_c;    /* alternator/probe NTC #1 (PA1, beta 3950) — or, when
                           * batt_temp_src (sensors_read arg) claims this
                           * channel for the battery, this reads NAN and the
                           * value lands in batt_temp_c below instead (GH#40) */
    float alt_temp2_c;   /* alternator/probe NTC #2 (PA2, beta 3950) — a
                           * SEPARATE channel from alt_temp_c, not a duplicate:
                           * board.h notes PA1/PA2 are two distinct beta-3950
                           * inputs and "which is ATS vs the second probe is a
                           * harness detail — bench". test-fw's `adc` command
                           * (deliverable #14) is exactly that bench step, so
                           * both channels are exposed rather than only one.
                           * Same batt_temp_src carve-out as alt_temp_c above
                           * applies here when the source names THIS channel. */
    float driver_temp_c; /* FET/driver-stage NTC (PA3, beta 3380 — §0.6 V8) */
    float batt_temp_c;   /* battery temp — GH#40: fed from whichever of
                           * PA1/PA2 batt_temp_src names, NAN when NONE (the
                           * default; the harness Battery Temp Sense wire is
                           * real per WS500_HARDWARE_SPEC §6c row 9, but which
                           * physical ADC channel it lands on is bench-pending,
                           * §6b — see control.h ctrl_batt_temp_src_t) */
} sensor_readings_t;

void sensors_init(void);              /* ADC + DMA circular scan + INA init */
void sensors_update(void);            /* fold latest averaged frame */

uint16_t sensors_raw(unsigned slot);  /* averaged raw count for a scan slot */
float    sensors_vdda(void);          /* mV rail from VREFINT calibration */

/*
 * Fill physical readings. NAN marks an open/short sensor.
 *   batt_temp_src : GH#40 config selector — which of PA1/PA2 (if either) feeds
 *                   batt_temp_c instead of alt_temp_c/alt_temp2_c this cycle.
 *                   Caller reads config BEFORE calling this (main.c orders
 *                   config_get() ahead of sensors_read() for exactly this).
 */
void sensors_read(sensor_readings_t *out, ctrl_batt_temp_src_t batt_temp_src);

#endif /* WS500_SENSORS_H */
