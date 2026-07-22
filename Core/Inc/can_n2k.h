/*
 * can_n2k.h — bxCAN + NMEA2000 / RV-C glue (stub).
 * Wire the MIT-licensed ttlappalainen/NMEA2000 library to bxCAN (PB8/PB9) here.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_CAN_N2K_H
#define WS500_CAN_N2K_H

#include "control.h"

void can_n2k_init(void);
void can_n2k_poll(void);
void can_n2k_publish(const ctrl_measured_t *m, const ctrl_t *c);

#endif /* WS500_CAN_N2K_H */
