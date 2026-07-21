/*
 * sensors.h — analog acquisition (ADC1, 7-ch scan, x4 oversample+average).
 * Raw slots [0..3] = PA1/PA2/PA3/PC5; [4..6] = temp/VREFINT/VBAT internal.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_SENSORS_H
#define WS500_SENSORS_H

#include <stdint.h>
#include "regulator.h"

void sensors_init(void);              /* ADC + DMA circular scan */
void sensors_update(void);            /* fold latest averaged frame */

uint16_t sensors_raw(unsigned slot);  /* averaged raw count for a scan slot */
float    sensors_vdda(void);          /* mV rail from VREFINT calibration */

/* Fill engineering-unit measurements for the regulator. Scaling is TODO until the
 * channel binding + divider/shunt constants are confirmed (see board.h notes). */
void sensors_read(reg_inputs_t *out);

#endif /* WS500_SENSORS_H */
