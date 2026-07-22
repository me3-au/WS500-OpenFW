/*
 * telemetry.h — dialect-neutral telemetry snapshot (CONTROL_SPEC §6.4, §8). PURE.
 *
 * One struct describing "what the regulator wants to report", built from the
 * engine's outputs. Per-dialect encoders (NMEA2000 now → Cerbo; RV-C later)
 * consume this — so the control core never knows about wire formats and adding a
 * dialect is a bounded task.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_TELEMETRY_H
#define WS500_TELEMETRY_H

#include "control.h"

typedef struct {
    /* Control state */
    ctrl_state_t          state;
    ctrl_standby_reason_t standby_reason;
    uint8_t               active_profile_id;
    float                 field_effort;   /* 0..1 */
    ctrl_bind_src_t       binding;        /* why output is what it is */
    float                 binding_w;
    uint32_t              faults;
    ctrl_severity_t       severity;

    /* Measured */
    uint8_t               cells_series;
    float                 vbat_pack_v;
    float                 v_cell;         /* vbat / cells */
    float                 amps_batt;
    float                 watts_batt;
    float                 alt_temp_c;
    float                 batt_temp_c;
    float                 rpm;
    ctrl_rpm_state_t      rpm_state;
} ctrl_telemetry_t;

/* Build the snapshot from the tick's command + measurements + active config. */
void ctrl_build_telemetry(ctrl_telemetry_t *t,
                          const ctrl_measured_t *m,
                          const ctrl_command_t  *cmd,
                          const ctrl_globals_t  *g,
                          const ctrl_profile_t  *prof);

#endif /* WS500_TELEMETRY_H */
