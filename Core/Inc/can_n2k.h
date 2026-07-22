/*
 * can_n2k.h — bxCAN + NMEA2000 / RV-C glue.
 * Tx (telemetry → Cerbo) is the near-term target; Rx (BMS/DVCC control) is later.
 * Reuse the MIT ttlappalainen/NMEA2000 library on bxCAN (PB8/PB9).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_CAN_N2K_H
#define WS500_CAN_N2K_H

#include "control.h"
#include "telemetry.h"

void can_n2k_init(void);
void can_n2k_poll(void);

/* Broadcast the telemetry snapshot as NMEA2000 PGNs (RV-C encoder added later). */
void can_n2k_publish(const ctrl_telemetry_t *t);

#endif /* WS500_CAN_N2K_H */
