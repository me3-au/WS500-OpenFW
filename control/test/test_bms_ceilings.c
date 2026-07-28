/*
 * test_bms_ceilings.c — deliverable #10, the two hard preconditions
 * PROJECT_PLAN §1 row 10 called out before BMS control-in touches a real
 * pack: CVL enforcement (Part A) and pre-disconnect's soft field ramp
 * (Part B, FIELD-DRIVE PATH). PROFILE_SPEC_LFP.md §6/§8.1,
 * CONTROL_SPEC_NEXTGEN.md §6.3.
 *
 * Part A pins: CVL lowers the effective CV target; a CVL above the profile
 * target never raises it; NAN/absent CVL leaves the profile target
 * unchanged; the pack->cell unit conversion (ctrl_cell_from_pack), including
 * the safe-failure direction if that conversion were ever skipped.
 *
 * Part B pins: pre-disconnect ramps field_effort to zero within
 * CTRL_PREDISC_RAMP_S, holds it there for as long as it is asserted, and
 * does so unconditionally in BULK, FLOAT, LIMP, and under the §5.2
 * stationary-rotor gate — never overridden by a state that would otherwise
 * re-energise the field. Release is governed by the ordinary ramp_w_per_s
 * soft-start, not a snap-back.
 *
 * Several cases below seed ctrl_t's persistent fields directly rather than
 * driving the engine there tick-by-tick through the ordinary CV/power loop.
 * That loop's own dynamics (soft-start ramp, gain interplay) are already
 * covered by test_statemachine.c and the sim/ gauntlet; seeding isolates the
 * mechanism actually under test here (the min() in Part A, the ramp ceiling
 * in Part B) from that separately-tested behavior, and makes the expected
 * arithmetic exact instead of dependent on how many ticks warmup takes.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "control.h"
#include "field.h"

/* ---- shared fixtures ------------------------------------------------------ *
 * 16S / 48 V — the installed unit's own configuration (PROJECT_PLAN §0.6),
 * because Part A's whole point is pinning the pack<->cell conversion at the
 * real cell count where a factor-of-16 error would actually matter. */
static ctrl_globals_t G16(void)
{
    ctrl_globals_t g = {0};
    g.cells_series = 16;
    g.bank_capacity_ah = 300.0f;
    g.max_charge_power_w = 8000.0f;
    g.ramp_w_per_s = 200.0f;            /* realistic soft-start rate (sil.c) */
    g.p_tail_w = 200.0f;
    g.t_tail_hold_s = 60;
    g.t_vclamp_s = 5;
    g.cv_hold_exit_min = 15;
    g.t_charge_max_min = 480;
    g.warmup_time_s = 0;
    g.warmup_coolant_c = NAN;
    g.soc_target_pct = -1;
    g.skip_bulk_vcell = 0.0f;
    g.skip_bulk_soc_pct = -1;
    g.rotor_rated_v = 12.0f;
    g.rotor_v_max = NAN;
    g.allow_full_field_48v = true;      /* duty_max=1 -- isolates effort from
                                         * the (separately-tested) rotor
                                         * clamp for these cases */
    g.limp_vcell = 3.30f;
    g.limp_power_cap_w = 2000.0f;
    return g;
}

static ctrl_profile_t P16(void)
{
    ctrl_profile_t p = {0};
    p.id = 1;
    p.cv_target_vcell = 3.60f;
    p.exit_at_cv_entry = false;
    p.rest_mode = CTRL_REST_HOLD;
    p.rest_voltage_vcell = 3.40f;
    p.rest_power_cap_w = 8000.0f;
    p.v_revert_vcell = 3.28f;
    p.t_revert_hold_s = 30;
    p.soc_revert_pct = -1;
    p.ah_revert = 90.0f;
    return p;
}

/* Healthy running measurement at a given pack voltage; overridden per-case. */
static ctrl_measured_t M(float vbat_pack)
{
    ctrl_measured_t m = {0};
    m.vbat_pack_v  = vbat_pack;
    m.vcomp_pack_v = vbat_pack;
    m.amps_batt    = 100.0f;
    m.watts_batt   = 0.0f;      /* isolates the voltage loop -- see file header */
    m.isrc         = CTRL_ISRC_BATT_SHUNT;
    m.v_supply_v   = vbat_pack;
    m.alt_hotspot_c = 60.0f;
    m.batt_temp_c  = 25.0f;
    m.driver_temp_c = 50.0f;
    m.rpm          = 2000.0f;
    m.rpm_state    = CTRL_RPM_VALID;
    m.run_state    = CTRL_RUN_RUNNING;
    m.soc_pct      = -1.0f;
    m.soc_trusted  = false;
    m.ignition     = true;
    m.feature_in   = false;
    m.pre_disconnect = false;
    return m;
}

/* =========================================================================
 * Part A -- CVL enforcement
 * ========================================================================= */

/* Pin ctrl_cell_from_pack() itself: the ONE division this deliverable relies
 * on (Core/Src/main.c, sim/sil.c both call it exactly once to build
 * ctrl_ceilings_t.bms_cvl_vcell from bms_snapshot_t.cvl_v). A 16x error here
 * (feeding pack volts in raw, or multiplying instead of dividing) would be
 * "catastrophic in the unsafe direction" per this deliverable's brief --
 * pinning the arithmetic directly is cheap insurance. */
static void test_pack_cell_conversion_arithmetic(void)
{
    /* 57.6 V pack / 16 cells = 3.60 V/cell -- the profile's own CV target,
     * chosen so a conversion bug would be obvious against a known-good spec
     * value, not just an arbitrary number. */
    CHECK_FEQ(ctrl_cell_from_pack(57.6f, 16), 3.6, 1e-6);
    CHECK_FEQ(ctrl_cell_from_pack(54.4f, 16), 3.4, 1e-6);
    CHECK(isnan(ctrl_cell_from_pack(NAN, 16)));      /* stale/absent CVL in -> NAN out */
    CHECK(isnan(ctrl_cell_from_pack(57.6f, 0)));     /* cells_series unconfigured -> NAN, not a divide-by-zero crash */
}

/* CVL below the profile CV target wins in the min() (PROFILE_SPEC_LFP.md
 * §6). Seeded state: BULK, mid effort, v_ctrl_f between the CVL and the
 * profile target so the two cases actually disagree about which way effort
 * should move; cmd_power_w == watts_batt so the power loop contributes
 * (approximately) nothing and the voltage loop's effect is visible on its
 * own -- see file header on why this is seeded rather than warmed up. */
static void test_cvl_lowers_cv_target(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();          /* cv_target_vcell = 3.60, cv_pack = 57.6 */
    ctrl_measured_t m = M(56.8f);      /* 3.55 V/cell -- below profile target, above the CVL below */

    m.watts_batt = g.max_charge_power_w;   /* == cmd_power_w below: neutralizes the power loop */

    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_BULK;
    e.effort = 0.5f;
    e.cmd_power_w = g.max_charge_power_w;  /* already at the (only) ceiling -> no ramp-up
                                            * step this tick -> p_err == 0 exactly */
    e.v_ctrl_f = 56.8f;

    ctrl_ceilings_t c = test_ceil_none();
    c.bms_cvl_vcell = 3.40f;           /* per-cell; pack-equivalent 54.4 V, below both
                                        * the measured 56.8 V and the profile's 57.6 V */

    ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);

    /* Effective target became 3.40 V/cell (54.4 V pack) < measured 56.8 V ->
     * voltage error is NEGATIVE (above the now-lower target) -> effort must
     * move DOWN from its 0.5 seed, and the reason must be telemetered as the
     * BMS CVL specifically, not the plain voltage clamp. */
    CHECK(cmd.field_effort < 0.5f);
    CHECK(cmd.binding == CTRL_BIND_BMS_CVL);
    CHECK(!cmd.field_open);
}

/* Same seed, CVL ABOVE the profile's own target: must have NO effect --
 * bit-for-bit identical to the CVL-absent (NAN) case, because `< cv_vcell`
 * is false and cv_vcell is left exactly as the stage picked it. Proves the
 * "only ever lowers" property structurally, not just by one lucky number. */
static void test_cvl_above_target_does_not_raise(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();
    ctrl_measured_t m = M(56.8f);
    m.watts_batt = g.max_charge_power_w;   /* neutralizes the power loop -- see
                                            * test_cvl_lowers_cv_target */

    ctrl_t e_high; ctrl_init(&e_high);
    e_high.state = CTRL_BULK; e_high.effort = 0.5f;
    e_high.cmd_power_w = g.max_charge_power_w; e_high.v_ctrl_f = 56.8f;
    ctrl_ceilings_t c_high = test_ceil_none();
    c_high.bms_cvl_vcell = 3.80f;      /* ABOVE the 3.60 profile target */
    ctrl_command_t cmd_high = ctrl_tick(&e_high, &m, &c_high, &p, &g, 10);

    ctrl_t e_none; ctrl_init(&e_none);
    e_none.state = CTRL_BULK; e_none.effort = 0.5f;
    e_none.cmd_power_w = g.max_charge_power_w; e_none.v_ctrl_f = 56.8f;
    ctrl_ceilings_t c_none = test_ceil_none();   /* bms_cvl_vcell = NAN */
    ctrl_command_t cmd_none = ctrl_tick(&e_none, &m, &c_none, &p, &g, 10);

    CHECK_FEQ(cmd_high.field_effort, cmd_none.field_effort, 1e-9);
    CHECK(cmd_high.binding == cmd_none.binding);
    CHECK(cmd_high.binding != CTRL_BIND_BMS_CVL);   /* did not bind -- never raised, never even touched */
}

/* Stale/absent CVL (NAN, bms_rx.c's own convention for "signal not fresh")
 * must leave the profile's own target in sole control -- same assertion as
 * above, stated directly against a NAN input rather than inferred from
 * equality with a second case. */
static void test_cvl_nan_leaves_profile_target(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();
    ctrl_measured_t m = M(58.0f);      /* 3.625 V/cell -- ABOVE the 3.60 target, so
                                        * the profile's own clamp should be actively
                                        * pulling effort down; a stale CVL must not
                                        * change that behavior at all. */
    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_BULK; e.effort = 0.5f; e.cmd_power_w = 0.0f; e.v_ctrl_f = 58.0f;

    ctrl_ceilings_t c = test_ceil_none();
    c.bms_cvl_vcell = NAN;             /* explicit, not just test_ceil_none()'s default */

    ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);
    CHECK(cmd.field_effort < 0.5f);              /* profile's own CV clamp still active */
    CHECK(cmd.binding == CTRL_BIND_VOLTAGE_CLAMP); /* profile target, NOT the BMS one */
}

/* Canary for the unsafe direction named in this deliverable's brief: if a
 * future change ever fed the RAW pack-volt CVL into ctrl_ceilings_t without
 * ctrl_cell_from_pack() (e.g. 57.6 instead of 3.60), the failure must be
 * SILENTLY INERT (never binds, because 57.6 is nowhere near a per-cell
 * target and `<` stays false), not a false clamp -- there is no numeric
 * accident that turns a unit mistake into an active, wrong clamp here. */
static void test_wrong_unit_cvl_is_silently_inert(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();
    ctrl_measured_t m = M(56.8f);
    m.watts_batt = g.max_charge_power_w;   /* neutralizes the power loop -- see
                                            * test_cvl_lowers_cv_target */

    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_BULK; e.effort = 0.5f;
    e.cmd_power_w = g.max_charge_power_w; e.v_ctrl_f = 56.8f;

    ctrl_ceilings_t c = test_ceil_none();
    c.bms_cvl_vcell = 57.6f;           /* WRONG: pack volts, not per-cell */

    ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, 10);
    CHECK(cmd.binding != CTRL_BIND_BMS_CVL);   /* never bound */
    CHECK_FEQ(cmd.field_effort, 0.5f, 1e-9);    /* identical to "no CVL at all" -- see
                                                 * test_cvl_above_target_does_not_raise */
}

/* =========================================================================
 * Part B -- BMS pre-disconnect: unconditional soft ramp to zero
 * (FIELD-DRIVE PATH -- needs safety review, see the deliverable report)
 * ========================================================================= */

#define TICK_MS   10u
#define RAMP_TICKS  30u   /* CTRL_PREDISC_RAMP_S (0.3 s) / (TICK_MS/1000) */

/* BULK, from a high-effort charging state: ramp reaches zero within the
 * stated bound, is strictly non-increasing while ramping, holds at zero for
 * as long as the warning stays asserted, is telemetered distinctly while
 * actively binding, and release afterward is a soft resume (governed by
 * ramp_w_per_s), not a snap-back to the pre-warning effort. */
static void test_predisconnect_ramps_and_holds_in_bulk(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();
    ctrl_measured_t m = M(44.8f);      /* 2.80 V/cell -- deeply below CV target,
                                        * so the CV/power loops keep PUSHING for
                                        * more effort throughout, the harder test
                                        * of the ramp's authority */
    ctrl_ceilings_t c = test_ceil_none();

    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_BULK;
    e.effort = 1.0f;                   /* high-effort charging state */
    e.cmd_power_w = 8000.0f;
    e.v_ctrl_f = 44.8f;

    /* Baseline: pre-disconnect NOT asserted -- effort stays high. */
    ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
    CHECK(cmd.state == CTRL_BULK);
    CHECK(!cmd.field_open);
    CHECK(cmd.field_effort > 0.9f);

    /* Assert pre-disconnect and ramp. */
    m.pre_disconnect = true;
    float prev = cmd.field_effort;
    bool saw_predisc_bind = false;
    for (uint32_t t = 1; t <= RAMP_TICKS; t++) {
        cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
        CHECK(cmd.field_effort <= prev + 1e-6f);      /* monotonic non-increase */
        CHECK(cmd.field_duty   <= 1.0f && cmd.field_duty >= 0.0f);
        if (cmd.binding == CTRL_BIND_BMS_PREDISCONNECT) saw_predisc_bind = true;
        prev = cmd.field_effort;
    }
    CHECK(saw_predisc_bind);
    CHECK_FEQ(cmd.field_effort, 0.0f, 1e-3);
    CHECK_FEQ(cmd.field_duty,   0.0f, 1e-3);

    /* Holds at zero -- not a grace period, not periodically re-armed. */
    for (int t = 0; t < 100; t++) {
        cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
        CHECK(cmd.field_effort == 0.0f);
        CHECK(cmd.field_duty   == 0.0f);
    }

    /* Release: the very next tick must NOT snap back to the pre-warning
     * effort -- cmd_power_w was reset to 0 throughout, so resume is governed
     * by the ordinary ramp_w_per_s soft-start. */
    m.pre_disconnect = false;
    cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
    CHECK(cmd.field_effort < 0.05f);   /* nowhere near the 1.0 pre-warning value */

    /* Continuing to run, effort should be climbing again (soft resume in
     * progress), still well below the original high-effort value. */
    for (int t = 0; t < 50; t++) cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
    CHECK(cmd.field_effort > 0.0f);
    CHECK(cmd.field_effort < 0.5f);
}

/* FLOAT (ordinary rest, not limp): pre-disconnect still ramps to zero within
 * the same bound. Voltage seeded comfortably above v_revert so the T3
 * revert path doesn't fire mid-test and confuse the picture. */
static void test_predisconnect_ramps_in_float(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();          /* v_revert_vcell = 3.28 -> pack 52.48 */
    ctrl_measured_t m = M(55.2f);      /* 3.45 V/cell -- above revert, in FLOAT's normal band */
    ctrl_ceilings_t c = test_ceil_none();

    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_FLOAT;
    e.effort = 0.3f;
    e.cmd_power_w = 500.0f;
    e.v_ctrl_f = 55.2f;
    e.v_revert_f = 55.2f;              /* seeded == measured: no spurious T3 revert */

    ctrl_command_t cmd = {0};
    m.pre_disconnect = true;
    for (uint32_t t = 1; t <= RAMP_TICKS; t++)
        cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);

    CHECK_FEQ(cmd.field_effort, 0.0f, 1e-3);
    CHECK_FEQ(cmd.field_duty,   0.0f, 1e-3);
    /* Held, not released, on continued assertion. */
    for (int t = 0; t < 20; t++) cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
    CHECK(cmd.field_effort == 0.0f);
}

/* LIMP (recoverable-critical fault -> Limp Home, CONTROL_SPEC §7): triggered
 * via CTRL_FAULT_LOST_BMS (a CTRL_FAULT_LIMP_MASK bit) with VBat sense still
 * valid, so this exercises the LIMP branch of the field-control section
 * (not the separate isnan(v_ctrl_f) "no usable voltage" path). Pre-disconnect
 * must still drive effort to zero -- LIMP's own reduced cap must not be
 * mistaken for "already safe enough" and left unramped. */
static void test_predisconnect_ramps_in_limp(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();
    ctrl_measured_t m = M(52.8f);      /* 3.30 V/cell == limp_vcell -- plausible limp operating point */
    m.ext_faults = CTRL_FAULT_LOST_BMS;
    ctrl_ceilings_t c = test_ceil_none();

    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_BULK;               /* engine will re-enter FLOAT@limp this tick */
    e.effort = 0.4f;
    e.cmd_power_w = 1000.0f;
    e.v_ctrl_f = 52.8f;

    ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
    CHECK(cmd.state == CTRL_FLOAT);    /* Limp Home = FLOAT @ v_limp */
    CHECK(!cmd.field_open);            /* recoverable -> limp, not field-off */

    m.pre_disconnect = true;
    for (uint32_t t = 1; t <= RAMP_TICKS; t++)
        cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);

    CHECK_FEQ(cmd.field_effort, 0.0f, 1e-3);
    CHECK_FEQ(cmd.field_duty,   0.0f, 1e-3);
}

/* §5.2 stationary-rotor gate: engine not running, so the run-detect budget
 * (<=5% of duty_max, pulsed) would ordinarily re-energise the field briefly
 * every probe cycle. Pre-disconnect must win regardless -- once the ramp
 * has settled, effort must read EXACTLY zero through at least one full
 * probe period (CTRL_RUN_PROBE_PERIOD_MS = 500 ms), proving the periodic
 * budget pulse never re-appears while the warning holds. */
static void test_predisconnect_overrides_rotor_gate(void)
{
    ctrl_globals_t g = G16();
    ctrl_profile_t p = P16();
    ctrl_measured_t m = M(44.8f);      /* deep discharge -- the gate's budget,
                                        * were it not overridden, would want
                                        * to push effort up, not just idle */
    m.run_state = CTRL_RUN_NOT_RUNNING;
    m.rpm_state = CTRL_RPM_LOST;
    m.rpm       = 0.0f;
    m.pre_disconnect = true;
    ctrl_ceilings_t c = test_ceil_none();

    ctrl_t e; ctrl_init(&e);
    e.state = CTRL_BULK;
    e.effort = 1.0f;
    e.cmd_power_w = 8000.0f;
    e.v_ctrl_f = 44.8f;

    /* Settle well past both the ramp bound and one full probe period. */
    ctrl_command_t cmd = {0};
    const uint32_t settle_ticks = RAMP_TICKS + (CTRL_RUN_PROBE_PERIOD_MS / TICK_MS) + 5u;
    for (uint32_t t = 0; t < settle_ticks; t++)
        cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
    CHECK(cmd.field_effort == 0.0f);
    CHECK(cmd.field_duty   == 0.0f);

    /* One more full probe period, checked every tick: the periodic budget
     * pulse must never reappear while pre-disconnect holds. */
    for (uint32_t t = 0; t < (CTRL_RUN_PROBE_PERIOD_MS / TICK_MS) + 2u; t++) {
        cmd = ctrl_tick(&e, &m, &c, &p, &g, TICK_MS);
        CHECK(cmd.field_effort == 0.0f);
        CHECK(cmd.field_duty   == 0.0f);
    }
}

void test_bms_ceilings(void)
{
    int at_entry = g_checks;
    printf("\n-- BMS CVL + pre-disconnect (deliverable #10 preconditions)\n");
    test_pack_cell_conversion_arithmetic();
    test_cvl_lowers_cv_target();
    test_cvl_above_target_does_not_raise();
    test_cvl_nan_leaves_profile_target();
    test_wrong_unit_cvl_is_silently_inert();
    test_predisconnect_ramps_and_holds_in_bulk();
    test_predisconnect_ramps_in_float();
    test_predisconnect_ramps_in_limp();
    test_predisconnect_overrides_rotor_gate();
    printf("  [BMS-CEIL] %d checks\n", g_checks - at_entry);
}
