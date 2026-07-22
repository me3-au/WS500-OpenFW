/*
 * test_statemachine.c — engine transitions T1–T4, safety, CV clamp.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "control.h"

static ctrl_globals_t G(void)
{
    ctrl_globals_t g = {0};
    g.cells_series = 4;                 /* 12 V */
    g.bank_capacity_ah = 100.0f;
    g.max_charge_power_w = 1000.0f;
    g.ramp_w_per_s = 100000.0f;         /* fast ramp so power target is reached at once */
    g.p_tail_w = 50.0f;
    g.t_tail_hold_s = 2;
    g.t_vclamp_s = 2;
    g.cv_hold_exit_min = 0;   /* off by default in tests; enabled per-case */
    g.t_charge_max_min = 480;
    g.warmup_time_s = 0;                /* no warmup delay in tests */
    g.warmup_coolant_c = NAN;
    g.soc_target_pct = -1;
    g.skip_bulk_vcell = 0.0f;           /* off by default; enabled per-case */
    g.skip_bulk_soc_pct = -1;
    g.rotor_rated_v = 12.0f;
    g.rotor_v_max = NAN;
    g.allow_full_field_48v = false;
    g.limp_vcell = 3.30f;
    g.limp_power_cap_w = 250.0f;
    return g;
}

static ctrl_profile_t P1(void)
{
    ctrl_profile_t p = {0};
    p.id = 1;
    p.cv_target_vcell = 3.60f;
    p.exit_at_cv_entry = false;
    p.rest_mode = CTRL_REST_HOLD;
    p.rest_voltage_vcell = 3.40f;
    p.rest_power_cap_w = 500.0f;
    p.v_revert_vcell = 3.28f;
    p.t_revert_hold_s = 2;
    p.soc_revert_pct = -1;
    p.ah_revert = 30.0f;
    return p;
}

/* Measurement at a given per-cell voltage; healthy, battery-side shunt, running. */
static ctrl_measured_t M(float vcell)
{
    ctrl_measured_t m = {0};
    m.vbat_pack_v = vcell * 4.0f;
    m.vcomp_pack_v = vcell * 4.0f;
    m.amps_batt = 20.0f;
    m.watts_batt = 200.0f;
    m.isrc = CTRL_ISRC_BATT_SHUNT;
    m.v_supply_v = 14.0f;
    m.alt_hotspot_c = 60.0f;
    m.batt_temp_c = 25.0f;
    m.driver_temp_c = 50.0f;
    m.rpm = 0.0f;
    m.rpm_state = CTRL_RPM_LOST;
    m.run_state = CTRL_RUN_NOT_RUNNING;
    m.soc_pct = -1.0f;
    m.soc_trusted = false;
    m.ignition = true;
    m.feature_in = false;
    return m;
}

void test_statemachine(void)
{
    ctrl_globals_t  g = G();
    ctrl_profile_t  p = P1();
    ctrl_ceilings_t c = test_ceil_none();

    /* 1) Ignition off → STANDBY/OFF, field open, zero duty. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.30f); m.ignition = false;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.state == CTRL_STANDBY);
        CHECK(cmd.standby_reason == CTRL_SB_OFF);
        CHECK(cmd.field_open);
        CHECK_FEQ(cmd.field_duty, 0.0f, 1e-6);
    }

    /* 2) Enable + warmup=0 → BULK; below CV → effort rises, field not open. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.30f);
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.state == CTRL_BULK);
        float e1 = cmd.field_effort;
        ctrl_command_t cmd2 = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd2.field_effort >= e1);
        CHECK(!cmd2.field_open);
    }

    /* 3) Overvoltage (raw) → critical fault → STANDBY/FAULT + field open. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.75f);           /* ≥ 3.70 hard OV */
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.faults & CTRL_FAULT_OVERVOLTAGE);
        CHECK(cmd.state == CTRL_STANDBY);
        CHECK(cmd.standby_reason == CTRL_SB_FAULT);
        CHECK(cmd.field_open);
    }

    /* 4) Battery low-temp → charge block → STANDBY, field open. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.30f); m.batt_temp_c = -5.0f;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.faults & CTRL_FAULT_BATT_LOWTEMP);
        CHECK(cmd.state == CTRL_STANDBY);
        CHECK(cmd.field_open);
    }

    /* 5) Sustained voltage above CV target → VOLTAGE_CLAMP binds, effort pulled low. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.30f);
        ctrl_tick(&e, &m, &c, &p, &g, 100);     /* → BULK */
        ctrl_measured_t mh = M(3.65f);          /* above CV 3.60 */
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 60; i++) cmd = ctrl_tick(&e, &mh, &c, &p, &g, 100);
        CHECK(cmd.binding == CTRL_BIND_VOLTAGE_CLAMP);
        CHECK(cmd.field_effort < 0.10f);
    }

    /* 6) BULK → FLOAT on tail power (clamped, battery-truth tier, hold met). */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.60f);           /* at CV → clamped */
        m.watts_batt = 10.0f;                   /* below p_tail 50 */
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);  /* → BULK */
        for (int i = 0; i < 30; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100); /* >2s hold */
        CHECK(cmd.state == CTRL_FLOAT);
    }

    /* 7) FLOAT → BULK on sustained low voltage (revert). Slow filter → 1s ticks. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.60f); m.watts_batt = 10.0f;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        for (int i = 0; i < 30; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100); /* → FLOAT */
        CHECK(cmd.state == CTRL_FLOAT);
        ctrl_measured_t ml = M(3.20f);          /* below revert 3.28 */
        for (int i = 0; i < 150; i++) cmd = ctrl_tick(&e, &ml, &c, &p, &g, 1000); /* settle 30s filter */
        CHECK(cmd.state == CTRL_BULK);
    }

    /* 8) External recoverable fault (LOST_BMS) → Limp Home = FLOAT, not field-open. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.35f);
        m.ext_faults = CTRL_FAULT_LOST_BMS;
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 5; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.faults & CTRL_FAULT_LOST_BMS);
        CHECK(cmd.state == CTRL_FLOAT);
        CHECK(!cmd.field_open);
    }

    /* 9) OPEN fault latches: overvoltage persists after voltage returns to normal. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t hi = M(3.75f);
        ctrl_tick(&e, &hi, &c, &p, &g, 100);    /* trips OV */
        ctrl_measured_t ok = M(3.30f);          /* voltage back to normal */
        ctrl_command_t cmd = ctrl_tick(&e, &ok, &c, &p, &g, 100);
        CHECK(cmd.faults & CTRL_FAULT_OVERVOLTAGE);   /* still latched */
        CHECK(cmd.state == CTRL_STANDBY);
        CHECK(cmd.field_open);
    }

    /* 10) Voltage+time exit: held at CV target for cv_hold_exit_min charges even with
     *     tail DISARMED (alt-side shunt) and high power — no current truth needed. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_globals_t gt = G(); gt.cv_hold_exit_min = 1;   /* 1 minute */
        ctrl_measured_t m = M(3.60f);        /* at CV target → clamped */
        m.isrc = CTRL_ISRC_ALT_SHUNT;        /* tier 3: tail exit disarmed */
        m.watts_batt = 500.0f;               /* well above tail — not a tail exit */
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 70; i++) cmd = ctrl_tick(&e, &m, &c, &p, &gt, 1000); /* >60 s at CV */
        CHECK(cmd.state == CTRL_FLOAT);
    }

    /* 11) Skip-bulk-if-full (voltage): resting above skip threshold on startup →
     *     start in FLOAT, not BULK (no re-absorb every power cycle). */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_globals_t gs = G(); gs.skip_bulk_vcell = 3.375f;    /* ~solar float */
        ctrl_measured_t full = M(3.40f);          /* resting above threshold */
        ctrl_command_t cmd = ctrl_tick(&e, &full, &c, &p, &gs, 100);
        CHECK(cmd.state == CTRL_FLOAT);
        ctrl_t e2; ctrl_init(&e2);
        ctrl_measured_t low = M(3.30f);           /* below → needs charge */
        ctrl_command_t cmd2 = ctrl_tick(&e2, &low, &c, &p, &gs, 100);
        CHECK(cmd2.state == CTRL_BULK);
    }

    /* 12) Skip-bulk-if-full (SoC): trusted SOC over threshold → start in FLOAT;
     *     untrusted SOC must NOT skip. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_globals_t gs = G(); gs.skip_bulk_soc_pct = 95;
        ctrl_measured_t m = M(3.35f); m.soc_pct = 98.0f; m.soc_trusted = true;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &gs, 100);
        CHECK(cmd.state == CTRL_FLOAT);
        ctrl_t e2; ctrl_init(&e2);
        ctrl_measured_t m2 = M(3.35f); m2.soc_pct = 98.0f; m2.soc_trusted = false;
        ctrl_command_t cmd2 = ctrl_tick(&e2, &m2, &c, &p, &gs, 100);
        CHECK(cmd2.state == CTRL_BULK);
    }
}
