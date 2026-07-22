/*
 * telemetry.c — build the dialect-neutral telemetry snapshot. PURE.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "telemetry.h"
#include "faults.h"

void ctrl_build_telemetry(ctrl_telemetry_t *t,
                          const ctrl_measured_t *m,
                          const ctrl_command_t  *cmd,
                          const ctrl_globals_t  *g,
                          const ctrl_profile_t  *prof)
{
    t->state             = cmd->state;
    t->standby_reason    = cmd->standby_reason;
    t->active_profile_id = prof->id;
    t->field_effort      = cmd->field_effort;
    t->binding           = cmd->binding;
    t->binding_w         = cmd->binding_w;
    t->faults            = cmd->faults;
    t->severity          = ctrl_fault_severity(cmd->faults);

    t->cells_series = g->cells_series;
    t->vbat_pack_v  = m->vbat_pack_v;
    t->v_cell       = g->cells_series ? m->vbat_pack_v / (float)g->cells_series : NAN;
    t->amps_batt    = m->amps_batt;
    t->watts_batt   = m->watts_batt;
    t->alt_temp_c   = m->alt_hotspot_c;
    t->batt_temp_c  = m->batt_temp_c;
    t->rpm          = m->rpm;
    t->rpm_state    = m->rpm_state;
}
