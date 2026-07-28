/*
 * test_statemachine.c — engine transitions T1–T4, safety, CV clamp.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "control.h"
#include "field.h"     /* §5.2 detect-budget constants (cases 17–18) */

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
    m.rpm = 1800.0f;
    m.rpm_state = CTRL_RPM_VALID;
    m.run_state = CTRL_RUN_RUNNING;
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

    /* 13) [SIL-found 2026-07] Shunt dropout: watts_batt = NaN must never reach
     *     the field command (effort/duty stay finite) and must NOT latch —
     *     previously NaN stuck in the effort integrator permanently. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t ok = M(3.30f);
        for (int i = 0; i < 10; i++) ctrl_tick(&e, &ok, &c, &p, &g, 100);
        ctrl_measured_t bad = M(3.30f); bad.amps_batt = NAN; bad.watts_batt = NAN;
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 10; i++) cmd = ctrl_tick(&e, &bad, &c, &p, &g, 100);
        CHECK(isfinite(cmd.field_effort));
        CHECK(isfinite(cmd.field_duty));
        for (int i = 0; i < 10; i++) cmd = ctrl_tick(&e, &ok, &c, &p, &g, 100);
        CHECK(isfinite(cmd.field_effort));
        CHECK(cmd.field_effort > 0.0f);          /* regulation resumed */
    }

    /* 14) [SIL-found 2026-07] Lost VBat sense: v_supply NaN must yield a ZERO
     *     duty, not NaN (ctrl_duty_max previously passed NaN through). */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t ok = M(3.30f);
        for (int i = 0; i < 10; i++) ctrl_tick(&e, &ok, &c, &p, &g, 100);
        ctrl_measured_t bad = M(3.30f);
        bad.vbat_pack_v = NAN; bad.vcomp_pack_v = NAN; bad.v_supply_v = NAN;
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 10; i++) cmd = ctrl_tick(&e, &bad, &c, &p, &g, 100);
        CHECK(cmd.faults & CTRL_FAULT_LOST_VBAT_SENSE);
        CHECK(isfinite(cmd.field_duty));
        CHECK_FEQ(cmd.field_duty, 0.0f, 1e-6);
    }

    /* 15) T3 Ah revert (GH#37): in FLOAT with battery-truth current, net Ah
     *     discharged since charged reaching ah_revert reverts to BULK — with
     *     voltage held ABOVE the voltage-revert threshold the whole time. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.60f); m.watts_batt = 10.0f;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        for (int i = 0; i < 30; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100); /* → FLOAT (tail) */
        CHECK(cmd.state == CTRL_FLOAT);
        CHECK_FEQ(e.ah_since_charged, 0.0f, 1e-6);        /* reset at charged */

        ctrl_measured_t md = M(3.40f);                    /* above v_revert 3.28 */
        md.amps_batt = -30.0f; md.watts_batt = -30.0f * 13.6f;
        /* 30 Ah at 30 A = 1 h; run 1 h + margin in 1 s ticks. */
        for (int i = 0; i < 3700 && cmd.state == CTRL_FLOAT; i++)
            cmd = ctrl_tick(&e, &md, &c, &p, &g, 1000);
        CHECK(cmd.state == CTRL_BULK);
        CHECK(e.ah_since_charged >= p.ah_revert - 0.5f);  /* it was the Ah path */
    }

    /* 16) T3 Ah revert is DISARMED without battery truth (tier 3, §4.2) and
     *     must not fire; charging current pays the deficit back down. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.60f); m.watts_batt = 10.0f;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        for (int i = 0; i < 30; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100); /* → FLOAT */
        CHECK(cmd.state == CTRL_FLOAT);

        ctrl_measured_t md = M(3.40f);
        md.isrc = CTRL_ISRC_ALT_SHUNT;                    /* tier 3: no battery truth */
        md.amps_batt = -30.0f; md.watts_batt = -408.0f;
        for (int i = 0; i < 3700; i++) cmd = ctrl_tick(&e, &md, &c, &p, &g, 1000);
        CHECK(cmd.state == CTRL_FLOAT);                   /* never reverted */
        CHECK_FEQ(e.ah_since_charged, 0.0f, 1e-6);        /* never integrated */

        /* Battery truth again: discharge 20 Ah, recharge 20 Ah → deficit repaid,
         * no revert (net Ah, not gross). */
        ctrl_measured_t mt = M(3.40f);
        mt.amps_batt = -20.0f; mt.watts_batt = -272.0f;
        for (int i = 0; i < 3600; i++) cmd = ctrl_tick(&e, &mt, &c, &p, &g, 1000);
        CHECK(e.ah_since_charged > 19.0f && e.ah_since_charged < 21.0f);
        mt.amps_batt = 20.0f; mt.watts_batt = 272.0f;
        for (int i = 0; i < 3600; i++) cmd = ctrl_tick(&e, &mt, &c, &p, &g, 1000);
        CHECK_FEQ(e.ah_since_charged, 0.0f, 0.5f);
        CHECK(cmd.state == CTRL_FLOAT);
    }

    /* 17) §5.2 stationary-rotor gate (GH#37): no rotation detected → effort is
     *     held to the pulse-cycled detect budget, never the clamp. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.30f);                     /* wants max field */
        m.rpm = 0.0f; m.rpm_state = CTRL_RPM_LOST;
        m.run_state = CTRL_RUN_NOT_RUNNING;
        float e_max = 0.0f; int zero_ticks = 0, on_ticks = 0;
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 100; i++) {                   /* 1 s at 10 ms = 2 probe cycles */
            cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);
            if (cmd.field_effort > e_max) e_max = cmd.field_effort;
            if (cmd.field_effort == 0.0f) zero_ticks++; else on_ticks++;
        }
        CHECK(cmd.state == CTRL_BULK);                    /* engine logic unaffected */
        CHECK(!cmd.field_open);
        CHECK(e_max <= CTRL_RUN_DETECT_EFFORT + 1e-6f);   /* budget respected */
        CHECK(on_ticks > 0);                              /* probe pulses do fire */
        CHECK(zero_ticks > on_ticks);                     /* bounded on-time ratio */
        CHECK(cmd.binding == CTRL_BIND_RUN_DETECT ||
              cmd.field_effort <= CTRL_RUN_DETECT_EFFORT);

        /* Rotation appears (VALID RPM) → gate releases, effort climbs past the
         * budget under the normal loop. */
        m.rpm = 1800.0f; m.rpm_state = CTRL_RPM_VALID;
        for (int i = 0; i < 200; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);
        CHECK(cmd.field_effort > CTRL_RUN_DETECT_EFFORT);

        /* STALE keeps the last-good claim (§3.1) — still treated as running. */
        m.rpm_state = CTRL_RPM_STALE;
        cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);
        CHECK(cmd.field_effort > CTRL_RUN_DETECT_EFFORT);

        /* LOST + app says not running → back to the budget within one tick. */
        m.rpm = 0.0f; m.rpm_state = CTRL_RPM_LOST;
        cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);
        CHECK(cmd.field_effort <= CTRL_RUN_DETECT_EFFORT + 1e-6f);
    }

    /* 18) §5.1 clamp-supply plausibility (GH#37): an in-range-but-false-LOW
     *     supply reading must NOT loosen the duty clamp. 16S so the clamp is
     *     genuinely < 1 (48 V-class bus). */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_globals_t g16 = G(); g16.cells_series = 16;
        ctrl_profile_t p16 = P1();
        ctrl_measured_t m = M(3.40f);
        m.vbat_pack_v = m.vcomp_pack_v = 3.40f * 16.0f;   /* 54.4 V bus */
        m.v_supply_v = 54.4f;
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 3000; i++) cmd = ctrl_tick(&e, &m, &c, &p16, &g16, 10);
        CHECK_FEQ(cmd.field_duty, cmd.field_effort * (12.0f / 54.4f), 1e-4);

        /* Sensor lies low (in-range 49 V; true bus unchanged): duty_max must
         * hold at 12/54.4, not loosen to 12/49. WARN raised, not LIMP/OPEN. */
        ctrl_measured_t lie = m;
        lie.vbat_pack_v = lie.vcomp_pack_v = 49.0f; lie.v_supply_v = 49.0f;
        for (int i = 0; i < 3000; i++) cmd = ctrl_tick(&e, &lie, &c, &p16, &g16, 10);
        CHECK(cmd.faults & CTRL_FAULT_VSUP_IMPLAUSIBLE);
        CHECK(cmd.field_duty <= cmd.field_effort * (12.0f / 54.4f) + 1e-4f);
        CHECK(!cmd.field_open);

        /* Reading recovers → WARN clears, clamp follows the honest voltage. */
        for (int i = 0; i < 100; i++) cmd = ctrl_tick(&e, &m, &c, &p16, &g16, 10);
        CHECK(!(cmd.faults & CTRL_FAULT_VSUP_IMPLAUSIBLE));
    }

    /* 19) GH#40 default policy: batt_temp_src unset (batt_temp_c NaN, matching
     *     what sensors.c reports when the config is `none`) + require_batt_temp
     *     false (the shipped default) → the low/high-temp window simply stays
     *     unarmed (no CTRL_FAULT_BATT_LOWTEMP/HIGHTEMP — both need a valid
     *     reading to even evaluate) and charging proceeds UNBLOCKED. This is
     *     the "charge and annunciate" behavior CONTROL_SPEC §4.2 requires:
     *     the gap must be visible (telemetry batt_armed=false, test_telemetry.c)
     *     but must never itself stop a healthy charge. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_measured_t m = M(3.30f);
        m.batt_temp_c = NAN;                    /* batt_temp_src == none */
        ctrl_command_t cmd = {0};
        for (int i = 0; i < 5; i++) cmd = ctrl_tick(&e, &m, &c, &p, &g, 100);
        CHECK(cmd.state == CTRL_BULK);
        CHECK(!cmd.field_open);
        CHECK(!(cmd.faults & (CTRL_FAULT_BATT_LOWTEMP | CTRL_FAULT_BATT_HIGHTEMP |
                              CTRL_FAULT_BATT_TEMP_REQUIRED)));
    }

    /* 20) GH#40 opt-in: require_batt_temp true + batt_temp_c NaN → charging is
     *     BLOCKed (CTRL_FAULT_BATT_TEMP_REQUIRED), same field-off shape as the
     *     existing BATT_LOWTEMP block (case 4 above) — auto-resume BLOCK, not
     *     a latch (§9.1). Once a valid reading arrives the fault clears and
     *     the same tick's engine can proceed exactly as case 2. */
    {
        ctrl_t e; ctrl_init(&e);
        ctrl_globals_t greq = g; greq.require_batt_temp = true;
        ctrl_measured_t m = M(3.30f);
        m.batt_temp_c = NAN;
        ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &greq, 100);
        CHECK(cmd.faults & CTRL_FAULT_BATT_TEMP_REQUIRED);
        CHECK(cmd.state == CTRL_STANDBY);
        CHECK(cmd.field_open);

        /* Auto-resume: a valid reading arriving later clears the block and
         * lets BULK proceed, without needing ctrl_init(). */
        ctrl_measured_t ok = m; ok.batt_temp_c = 25.0f;
        for (int i = 0; i < 5; i++) cmd = ctrl_tick(&e, &ok, &c, &p, &greq, 100);
        CHECK(!(cmd.faults & CTRL_FAULT_BATT_TEMP_REQUIRED));
        CHECK(cmd.state == CTRL_BULK);
        CHECK(!cmd.field_open);
    }
}
