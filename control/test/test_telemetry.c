/*
 * test_telemetry.c — dialect-neutral snapshot build (derived fields + severity).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "telemetry.h"

void test_telemetry(void)
{
    ctrl_measured_t m = {0};
    m.vbat_pack_v = 54.0f;      /* 16S → 3.375 V/cell */
    m.amps_batt = 60.0f;
    m.watts_batt = 3240.0f;
    m.alt_hotspot_c = 88.0f;
    m.batt_temp_c = 22.0f;
    m.rpm = 1500.0f;
    m.rpm_state = CTRL_RPM_VALID;

    ctrl_command_t cmd = {0};
    cmd.state = CTRL_BULK;
    cmd.field_effort = 0.42f;
    cmd.binding = CTRL_BIND_THERMAL;
    cmd.binding_w = 3000.0f;
    cmd.faults = CTRL_FAULT_SELF_OVERTEMP;   /* WARN */

    ctrl_globals_t g = {0}; g.cells_series = 16;
    ctrl_profile_t p = {0}; p.id = 2;

    ctrl_telemetry_t t;
    ctrl_build_telemetry(&t, &m, &cmd, &g, &p);

    CHECK(t.state == CTRL_BULK);
    CHECK(t.active_profile_id == 2);
    CHECK(t.binding == CTRL_BIND_THERMAL);
    CHECK_FEQ(t.binding_w, 3000.0f, 0.01f);
    CHECK(t.severity == CTRL_SEV_WARN);            /* derived from faults */
    CHECK_FEQ(t.v_cell, 54.0f / 16.0f, 0.001f);    /* per-cell derived */
    CHECK_FEQ(t.watts_batt, 3240.0f, 0.01f);
    CHECK(t.rpm_state == CTRL_RPM_VALID);
    CHECK_FEQ(t.field_effort, 0.42f, 0.001f);

    /* Zero cells → NaN per-cell, no divide-by-zero. */
    g.cells_series = 0;
    ctrl_build_telemetry(&t, &m, &cmd, &g, &p);
    CHECK(isnan(t.v_cell));
}
