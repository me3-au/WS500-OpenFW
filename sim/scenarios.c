/*
 * scenarios.c — §8.1 SIL scenario suite (regression style, all asserting).
 *
 * Each scenario is a function; assertions reuse the control/test CHECK macros.
 * Deterministic: fixed RNG seeds, no wall clock. Accelerated time = loop
 * iterations at the core's 10 ms tick.
 *
 * The #1 property everywhere (checked every tick by sil.c invariants): field
 * duty NEVER exceeds the rotor clamp — ≈25 % at 48 V, ≈21 % at 57.6 V — on
 * this 4 Ω / 12 V-rotor / 48 V install (PROJECT_PLAN §5).
 *
 * SPDX-License-Identifier: MIT
 */
#include "../control/test/test.h"
#include "sil.h"
#include "field.h"
#include "faults.h"
#include <stdio.h>

static void banner(const char *name) { printf("== %s\n", name); }

/* --------------------------------------------------------------------------
 * 0) Plant calibration vs the 2026-07-24 stock-firmware reference trace:
 *    25 % field @ 2330 RPM → ~140 A / ~7.6 kW at ~54.7 V; 100 % duty is
 *    rotor overdrive (12–14 A, ~580+ W into the 4 Ω winding).
 * -------------------------------------------------------------------------- */
void scn_plant_calibration(void)
{
    banner("plant_calibration (reference trace 2026-07-24)");
    plant_cfg_t cfg; plant_cfg_default(&cfg);
    plant_t p; plant_init(&p, &cfg, 0.538f);

    /* Quasi-static mid-charge point: pin SOC (the trace is a snapshot). */
    for (int i = 0; i < 5000; i++) {
        p.soc = 0.538f;
        plant_step(&p, 0.25f, 2330.0f, 0.0f, 0.01f);
    }
    CHECK_FEQ(p.i_alt_a, 140.0f, 12.0f);            /* ~140 A */
    CHECK_FEQ(p.vbus_v, 54.7f, 0.6f);               /* ~54.7 V */
    CHECK_FEQ(p.i_field_a, 3.42f, 0.20f);           /* 0.25 × 54.7 / 4 Ω */
    CHECK_FEQ(p.i_alt_a * p.vbus_v, 7600.0f, 800.0f); /* ~7.6 kW */

    /* Driver-stage FET heating ballpark (trace: 31→45 °C under load). */
    CHECK(p.driver_temp_c > 35.0f && p.driver_temp_c < 55.0f);

    /* 100 % duty = overdrive: 12–14 A field, ~580+ W rotor dissipation. */
    plant_init(&p, &cfg, 0.538f);
    for (int i = 0; i < 2000; i++) {
        p.soc = 0.538f;
        plant_step(&p, 1.0f, 2330.0f, 0.0f, 0.01f);
    }
    CHECK(p.i_field_a >= 11.5f && p.i_field_a <= 15.0f);
    CHECK(p.p_rotor_w >= 500.0f && p.p_rotor_w <= 900.0f);
}

/* --------------------------------------------------------------------------
 * 1) Full CHARGE→REST cycle at 16S/48 V: BULK ramp → CV → charged exit →
 *    FLOAT hold → (deep discharge) → T3 revert back to BULK.
 * -------------------------------------------------------------------------- */
void scn_charge_rest_cycle(void)
{
    banner("charge_rest_cycle (16S/48V full cycle)");
    sil_t s; sil_init(&s, 0xC0FFEE01u, 0.55f);
    s.engine_rpm = 2800.0f;      /* cruise: enough EMF headroom to reach CV */
    s.house_load_w = 200.0f;

    bool saw_bulk = false, reached_cv = false;
    float max_vcell = 0.0f;
    uint32_t charged_tick = 0;

    const uint32_t max_ticks = 2u * 3600u * 100u;   /* 2 h sim budget */
    for (uint32_t i = 0; i < max_ticks; i++) {
        sil_step(&s);
        if (s.cmd.state == CTRL_BULK) saw_bulk = true;
        const float vcell = s.plant.vbus_v / 16.0f;
        if (vcell > max_vcell) max_vcell = vcell;
        if (vcell >= 3.59f) reached_cv = true;
        if (s.cmd.state == CTRL_FLOAT) { charged_tick = i; break; }
    }

    CHECK(saw_bulk);
    CHECK(reached_cv);                       /* CV target was actually reached */
    CHECK(s.cmd.state == CTRL_FLOAT);        /* charged exit happened */
    CHECK(charged_tick > 0 && charged_tick < max_ticks);
    CHECK(s.plant.soc > 0.93f);              /* genuinely full */
    CHECK(max_vcell < 3.70f);                /* never into OV territory */
    CHECK(!(s.cmd.faults & CTRL_FAULT_OVERVOLTAGE));

    /* REST: hold FLOAT for 10 min — stays put, field within clamp. */
    sil_run_s(&s, 600.0f);
    CHECK(s.cmd.state == CTRL_FLOAT);
    CHECK(s.inv_violations == 0);

    /* Deep discharge with the engine stopped: voltage sags below the 3.28
     * revert threshold → T3 → BULK. */
    s.engine_rpm = 0.0f;
    s.house_load_w = 6000.0f;
    uint32_t revert_tick = 0;
    for (uint32_t i = 0; i < 30u * 60u * 100u; i++) {   /* ≤ 30 min */
        sil_step(&s);
        if (s.cmd.state == CTRL_BULK) { revert_tick = i; break; }
    }
    CHECK(revert_tick > 0);
    CHECK(s.cmd.state == CTRL_BULK);

    /* Engine restarts → charging resumes. */
    s.engine_rpm = 2800.0f;
    s.house_load_w = 200.0f;
    sil_run_s(&s, 120.0f);
    CHECK(s.plant.i_alt_a > 30.0f);
    CHECK(s.inv_violations == 0);
    printf("   charged in %.1f min, revert after %.1f min discharge\n",
           (double)charged_tick / 6000.0, (double)revert_tick / 6000.0);
}

/* --------------------------------------------------------------------------
 * 2) ROTOR CLAMP — the #1 safety property, under deliberate abuse:
 *    RPM square waves, BMS ceiling toggles, load dumps, sensor noise, and a
 *    high-SOC (57.6 V-class) phase. Duty must NEVER exceed the clamp.
 * -------------------------------------------------------------------------- */
void scn_rotor_clamp(void)
{
    banner("rotor_clamp (#1 safety property, transient abuse)");
    sil_t s; sil_init(&s, 0xD00DFEEDu, 0.50f);
    s.inj.noise[PS_VBAT] = 0.01f;
    s.inj.noise[PS_AMPS] = 0.01f;

    /* Phase A: mid-SOC abuse — 20 min of square-wave transients. */
    for (uint32_t i = 0; i < 20u * 60u * 100u; i++) {
        s.plant.soc = 0.50f;                                  /* pin operating point */
        s.engine_rpm   = ((i / 2000u) % 2u) ? 3500.0f : 800.0f;   /* 20 s RPM steps */
        s.bms_ccl_w    = ((i / 700u)  % 2u) ? 1000.0f : 8000.0f;  /* 7 s BMS toggles */
        s.house_load_w = ((i / 1300u) % 2u) ? 4000.0f : 0.0f;     /* 13 s load dumps */
        sil_step(&s);
    }
    CHECK(s.inv_violations == 0);
    CHECK(s.max_duty_seen <= 0.25f + 0.015f);   /* ≈25 % ceiling at ~48–54 V */
    CHECK(s.max_ifield_seen <= 3.3f);           /* rotor current stays ~rated */
    printf("   phase A: max duty %.4f, max I_field %.2f A, max rotor %.1f W\n",
           (double)s.max_duty_seen, (double)s.max_ifield_seen,
           (double)s.max_rotor_w_seen);

    /* Phase B: high-SOC / high-voltage (bus toward 57.6 V) — the clamp must
     * tighten to ≈21 %. */
    sil_reset_maxima(&s);
    s.engine_rpm = 2800.0f;
    s.bms_ccl_w = CTRL_CEILING_INACTIVE;
    s.house_load_w = 0.0f;
    for (uint32_t i = 0; i < 10u * 60u * 100u; i++) {
        s.plant.soc = 0.995f;                   /* pinned at the CV wall */
        sil_step(&s);
    }
    CHECK(s.inv_violations == 0);
    /* Whenever the bus was ≥ 56 V the per-tick invariant enforced ≤ 12/V;
     * globally nothing may have exceeded the low-voltage bound either. */
    CHECK(s.max_duty_seen <= 0.25f + 0.015f);
    CHECK(s.max_ifield_seen <= 3.3f);
    printf("   phase B: max duty %.4f at bus %.1f V\n",
           (double)s.max_duty_seen, (double)s.plant.vbus_v);

    /* Phase C: sanity — with the explicit allow_full_field_48v override the
     * clamp lifts (proves phases A/B weren't vacuously passing). */
    sil_t s2; sil_init(&s2, 0xD00DFEEDu, 0.30f);
    s2.g.allow_full_field_48v = true;
    s2.inv_skip_clamp = true;
    s2.engine_rpm = 1200.0f;      /* low EMF → controller wants max field */
    sil_run_s(&s2, 120.0f);
    CHECK(s2.max_duty_seen > 0.30f);            /* clamp genuinely lifted */
}

/* --------------------------------------------------------------------------
 * 3) BMS ceiling step changes: arbitration follows (binding telemetered),
 *    down-steps apply immediately with no overshoot, up-steps ramp at
 *    ramp_w_per_s, CCL=0 kills the field promptly.
 * -------------------------------------------------------------------------- */
void scn_bms_steps(void)
{
    banner("bms_ceiling_steps (arbitration follows, no overshoot)");
    sil_t s; sil_init(&s, 0xB3550001u, 0.50f);
    s.engine_rpm = 2800.0f;

    /* Settle at full power (ramp 200 W/s → 8 kW in 40 s + margin), SOC pinned. */
    for (uint32_t i = 0; i < 90u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    const float w_full = s.plant.i_batt_a * s.plant.vbus_v;
    CHECK(w_full > 6000.0f);

    /* Step DOWN 8000→3000: immediate, no overshoot above the new ceiling. */
    s.bms_ccl_w = 3000.0f;
    for (uint32_t i = 0; i < 5u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    float w_max = 0.0f, w_min = 1e9f;
    for (uint32_t i = 0; i < 10u * 100u; i++) {      /* settled window */
        s.plant.soc = 0.50f; sil_step(&s);
        const float w = s.plant.i_batt_a * s.plant.vbus_v;
        if (w > w_max) w_max = w;
        if (w < w_min) w_min = w;
    }
    CHECK(w_max <= 3000.0f * 1.10f + 100.0f);        /* no overshoot */
    CHECK(w_max - w_min <= 0.25f * 3000.0f);         /* no limit-cycle chatter */
    CHECK(s.cmd.binding == CTRL_BIND_BMS);           /* binding telemetered */

    /* Step UP 3000→8000: rises only at ramp_w_per_s (200 W/s). */
    s.bms_ccl_w = 8000.0f;
    float w_prev = s.plant.i_batt_a * s.plant.vbus_v;
    float max_rise_per_s = 0.0f;
    for (int sec = 0; sec < 30; sec++) {
        for (uint32_t i = 0; i < 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        const float w = s.plant.i_batt_a * s.plant.vbus_v;
        if (w - w_prev > max_rise_per_s) max_rise_per_s = w - w_prev;
        w_prev = w;
    }
    CHECK(max_rise_per_s <= 200.0f * 1.5f + 50.0f);  /* bounded by the ramp */

    /* Step to a hard 400 W ceiling. */
    s.bms_ccl_w = 400.0f;
    for (uint32_t i = 0; i < 5u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    const float w_low = s.plant.i_batt_a * s.plant.vbus_v;
    CHECK(w_low <= 400.0f * 1.20f + 60.0f);

    /* CCL = 0 (BMS says stop): field effectively off within 1 s. */
    s.bms_ccl_w = 0.0f;
    for (uint32_t i = 0; i < 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.cmd.field_duty < 0.005f);
    CHECK(s.inv_violations == 0);
}

/* --------------------------------------------------------------------------
 * 4) Engine RPM transients: idle↔cruise steps and stall-to-zero. Output
 *    follows physics; no spurious faults; clamp holds throughout.
 * -------------------------------------------------------------------------- */
void scn_rpm_transients(void)
{
    banner("rpm_transients (idle/cruise steps, stall)");
    sil_t s; sil_init(&s, 0x59A11EDu, 0.50f);
    s.engine_rpm = 2330.0f;

    for (uint32_t i = 0; i < 120u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    const float i_cruise = s.plant.i_alt_a;
    CHECK(i_cruise > 50.0f && i_cruise < 100.0f);   /* clamp-limited @2330 RPM */

    /* Drop to idle: with the rotor clamp the EMF collapses → near-zero output.
     * That is correct physics on this install; no faults may appear. */
    s.engine_rpm = 800.0f;
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.plant.i_alt_a < 5.0f);
    CHECK(ctrl_fault_disposition(s.cmd.faults) == CTRL_DISP_CONTINUE);
    CHECK(s.cmd.state == CTRL_BULK);

    /* Back to cruise: output recovers. */
    s.engine_rpm = 2330.0f;
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.plant.i_alt_a > 0.8f * i_cruise);

    /* Stall to zero: RPM sensor goes LOST (NaN), engine stopped. The core must
     * stay sane — no OV/overspeed faults, clamp intact — and the §5.2 run-detect
     * gate must pull the field down to the detect budget (scn_stalled_rotor
     * covers the gate in depth; here we just confirm it engages). */
    s.engine_rpm = 0.0f;
    s.inj.dropout[PS_RPM] = true;
    sil_reset_maxima(&s);
    for (uint32_t i = 0; i < 120u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(ctrl_fault_disposition(s.cmd.faults) == CTRL_DISP_CONTINUE);
    CHECK(s.cmd.state == CTRL_BULK);
    CHECK(s.plant.i_alt_a < 1.0f);                  /* no output at 0 RPM */
    CHECK(s.max_duty_seen <= CTRL_RUN_DETECT_EFFORT * 0.25f + 0.01f); /* gated */

    /* Restart: recovers again. */
    s.engine_rpm = 2330.0f;
    s.inj.dropout[PS_RPM] = false;
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.plant.i_alt_a > 0.8f * i_cruise);
    CHECK(s.inv_violations == 0);
}

/* --------------------------------------------------------------------------
 * 5) Cold / hot temperature behavior: Li charge-window gates (block + resume)
 *    and the predictive thermal governor holding the alternator at target.
 * -------------------------------------------------------------------------- */
void scn_temperature(void)
{
    banner("temperature (Li window gates + thermal governor)");

    /* a) Cold battery (−5 °C): charge blocked, field open; warming resumes. */
    {
        sil_t s; sil_init(&s, 0x7E3B0001u, 0.50f);
        s.inj.override_en[PS_BATT_T] = true;
        s.inj.override_v[PS_BATT_T] = -5.0f;
        sil_run_s(&s, 10.0f);
        CHECK(s.cmd.faults & CTRL_FAULT_BATT_LOWTEMP);
        CHECK(s.cmd.state == CTRL_STANDBY);
        CHECK(s.cmd.field_open);
        CHECK(s.applied_duty == 0.0f);

        s.inj.override_en[PS_BATT_T] = false;       /* warmed to ambient */
        sil_run_s(&s, 30.0f);                       /* warmup gate 5 s + margin */
        CHECK(!(s.cmd.faults & CTRL_FAULT_BATT_LOWTEMP));
        CHECK(s.cmd.state == CTRL_BULK);
        CHECK(s.inv_violations == 0);
    }

    /* b) Hot battery (60 °C): high-temp charge abort, then resume. */
    {
        sil_t s; sil_init(&s, 0x7E3B0002u, 0.50f);
        s.inj.override_en[PS_BATT_T] = true;
        s.inj.override_v[PS_BATT_T] = 60.0f;
        sil_run_s(&s, 10.0f);
        CHECK(s.cmd.faults & CTRL_FAULT_BATT_HIGHTEMP);
        CHECK(s.cmd.state == CTRL_STANDBY);
        CHECK(s.cmd.field_open);

        s.inj.override_en[PS_BATT_T] = false;
        sil_run_s(&s, 30.0f);
        CHECK(s.cmd.state == CTRL_BULK);
        CHECK(s.inv_violations == 0);
    }

    /* c) Hot engine room (45 °C ambient): the governor must hold the
     *    alternator near the 95 °C target by shedding power — no runaway. */
    {
        sil_t s; sil_init(&s, 0x7E3B0003u, 0.50f);
        s.plant.cfg.ambient_c = 45.0f;
        plant_init(&s.plant, &s.plant.cfg, 0.50f);
        s.engine_rpm = 2800.0f;

        bool bound_thermal = false;
        float t_max = 0.0f;
        for (uint32_t i = 0; i < 40u * 60u * 100u; i++) {   /* 40 min */
            s.plant.soc = 0.50f;
            sil_step(&s);
            if (s.cmd.binding == CTRL_BIND_THERMAL) bound_thermal = true;
            if (i > 15u * 60u * 100u && s.plant.alt_temp_c > t_max)
                t_max = s.plant.alt_temp_c;
        }
        CHECK(bound_thermal);                        /* governor engaged */
        CHECK(t_max <= 105.0f);                      /* no thermal runaway */
        CHECK_FEQ(s.plant.alt_temp_c, 95.0f, 8.0f);  /* converged near target */
        const float w_end = s.plant.i_batt_a * s.plant.vbus_v;
        CHECK(w_end < 7000.0f);                      /* power genuinely shed */
        CHECK(s.inv_violations == 0);
    }

    /* d) Raw hard limit (125 °C sensed): fault raised AND power pulled to the
     *    derate floor — "fast pull to floor + fault" (§4.2). */
    {
        sil_t s; sil_init(&s, 0x7E3B0004u, 0.50f);
        s.engine_rpm = 2800.0f;
        for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        s.inj.override_en[PS_ALT_T] = true;
        s.inj.override_v[PS_ALT_T] = 125.0f;
        for (uint32_t i = 0; i < 20u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(s.cmd.faults & CTRL_FAULT_SELF_OVERTEMP);
        const float w = s.plant.i_batt_a * s.plant.vbus_v;
        CHECK(w <= 500.0f * 1.5f + 100.0f);          /* at/near the 500 W floor */
        CHECK(s.inv_violations == 0);
    }
}

/* --------------------------------------------------------------------------
 * 6) Sensor dropout + implausible-value injection: faults latch where they
 *    must, the field goes safe, recoverable paths limp and resume.
 * -------------------------------------------------------------------------- */
void scn_sensor_faults(void)
{
    banner("sensor_faults (dropout / implausible injection)");

    /* a) VBat dropout → LOST_VBAT_SENSE → limp, duty exactly 0 (not NaN);
     *    recovery resumes regulation. */
    {
        sil_t s; sil_init(&s, 0x5E450001u, 0.50f);
        for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }

        s.inj.dropout[PS_VBAT] = true;
        for (uint32_t i = 0; i < 5u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(s.cmd.faults & CTRL_FAULT_LOST_VBAT_SENSE);
        CHECK(isfinite(s.cmd.field_duty));
        CHECK(s.cmd.field_duty == 0.0f);             /* no voltage → no field */

        s.inj.dropout[PS_VBAT] = false;
        for (uint32_t i = 0; i < 120u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(!(s.cmd.faults & CTRL_FAULT_LOST_VBAT_SENSE));
        CHECK(s.plant.i_alt_a > 30.0f);              /* charging again */
        CHECK(s.inv_violations == 0);
    }

    /* b) Shunt dropout (amps → NaN, so watts → NaN): the field command must
     *    stay finite through the outage and regulation must recover after. */
    {
        sil_t s; sil_init(&s, 0x5E450002u, 0.50f);
        for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        const float i_before = s.plant.i_alt_a;

        s.inj.dropout[PS_AMPS] = true;
        for (uint32_t i = 0; i < 10u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(isfinite(s.cmd.field_effort));         /* NaN must not latch */
        CHECK(isfinite(s.cmd.field_duty));

        s.inj.dropout[PS_AMPS] = false;
        for (uint32_t i = 0; i < 120u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(s.plant.i_alt_a > 0.7f * i_before);    /* recovered */
        CHECK(s.inv_violations == 0);
    }

    /* c) Implausible shunt value (+2000 A): app plausibility flags it →
     *    IMPLAUSIBLE_SHUNT → Limp Home (FLOAT @ v_limp), field NOT open;
     *    clears when the value returns to sane. */
    {
        sil_t s; sil_init(&s, 0x5E450003u, 0.50f);
        for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }

        s.inj.override_en[PS_AMPS] = true;
        s.inj.override_v[PS_AMPS] = 2000.0f;
        for (uint32_t i = 0; i < 10u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(s.cmd.faults & CTRL_FAULT_IMPLAUSIBLE_SHUNT);
        CHECK(s.cmd.state == CTRL_FLOAT);            /* Limp Home, not field-off */
        CHECK(!s.cmd.field_open);
        CHECK(isfinite(s.cmd.field_duty));

        s.inj.override_en[PS_AMPS] = false;
        for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(!(s.cmd.faults & CTRL_FAULT_IMPLAUSIBLE_SHUNT));
        CHECK(s.inv_violations == 0);
    }

    /* d) Overvoltage injection (sensor claims 3.85 V/cell): CRITICAL fault
     *    LATCHES — field open and it STAYS open after the value recovers. */
    {
        sil_t s; sil_init(&s, 0x5E450004u, 0.50f);
        for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }

        s.inj.override_en[PS_VBAT] = true;
        s.inj.override_v[PS_VBAT] = 3.85f * 16.0f;   /* 61.6 V — over 3.70/cell */
        for (uint32_t i = 0; i < 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(s.cmd.faults & CTRL_FAULT_OVERVOLTAGE);
        CHECK(s.cmd.field_open);
        CHECK(s.applied_duty == 0.0f);

        s.inj.override_en[PS_VBAT] = false;          /* reading back to normal */
        sil_run_s(&s, 60.0f);
        CHECK(s.cmd.faults & CTRL_FAULT_OVERVOLTAGE);   /* still latched */
        CHECK(s.cmd.state == CTRL_STANDBY);
        CHECK(s.cmd.standby_reason == CTRL_SB_FAULT);
        CHECK(s.cmd.field_open);
        CHECK(s.inv_violations == 0);
    }

    /* e) Noise storm (3 % V/I, ±2 °C temps) for 5 sim-minutes: stays safe,
     *    keeps charging, no faults latch. */
    {
        sil_t s; sil_init(&s, 0x5E450005u, 0.50f);
        s.inj.noise[PS_VBAT] = 0.03f;  s.inj.noise[PS_AMPS] = 0.03f;
        s.inj.noise[PS_ALT_T] = 2.0f;  s.inj.noise[PS_BATT_T] = 2.0f;
        s.inj.noise[PS_DRV_T] = 2.0f;  s.inj.noise[PS_RPM] = 0.02f;
        for (uint32_t i = 0; i < 5u * 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
        CHECK(s.cmd.state == CTRL_BULK);
        CHECK(ctrl_fault_disposition(s.cmd.faults) == CTRL_DISP_CONTINUE);
        CHECK(s.plant.i_alt_a > 30.0f);
        CHECK(s.inv_violations == 0);
    }
}

/* --------------------------------------------------------------------------
 * 7) Reference-trace shape reproduction: at 2330 RPM mid-charge the unit is
 *    field-clamp-limited, not voltage-limited (stock pinned 25 %, 54.7 V,
 *    56 V target unreached). Our dynamic clamp must show the same shape.
 * -------------------------------------------------------------------------- */
void scn_reference_trace(void)
{
    banner("reference_trace (clamp-limited charge, stock-trace shape)");
    sil_t s; sil_init(&s, 0x7ACE0001u, 0.50f);
    s.engine_rpm = 2330.0f;
    s.g.max_charge_power_w = 9000.0f;       /* above what the clamp permits */

    /* Ramp-in: the core soft-starts in the POWER domain (ramp_w_per_s), so
     * battery watts must stay under the configured 200 W/s envelope. (Stock
     * ramps *duty* at ~2 %/s; our duty crosses the sub-excitation dead zone —
     * where EMF < bus and no current can flow — quickly, then tracks the
     * power ramp. Same gentle-on-the-belt outcome, different domain.) */
    float duty_prev = 0.0f, max_duty_rise_per_s = 0.0f;
    bool w_under_envelope = true;
    for (int sec = 0; sec < 120; sec++) {
        sil_run_ticks(&s, 100);
        const float w = s.plant.i_batt_a * s.plant.vbus_v;
        if (w > 200.0f * (float)(sec + 1) + 400.0f) w_under_envelope = false;
        const float rise = s.cmd.field_duty - duty_prev;
        if (rise > max_duty_rise_per_s) max_duty_rise_per_s = rise;
        duty_prev = s.cmd.field_duty;
    }
    CHECK(w_under_envelope);                /* watts obey the soft-start ramp */
    CHECK(max_duty_rise_per_s <= 0.15f);    /* duty still builds, not a step */

    /* Settled, still mid-charge: clamp-limited operation. */
    for (uint32_t i = 0; i < 5u * 60u * 100u; i++) { s.plant.soc = 0.52f; sil_step(&s); }
    CHECK(s.cmd.binding == CTRL_BIND_ROTOR_CLAMP);       /* telemetered why */
    CHECK(s.cmd.field_effort >= 0.99f);                  /* railed at clamp */
    CHECK_FEQ(s.cmd.field_duty, 12.0f / s.plant.vbus_v, 0.004f);
    CHECK(s.plant.vbus_v < 56.0f);          /* CV target unreached — like stock */
    CHECK(s.plant.i_alt_a > 45.0f && s.plant.i_alt_a < 100.0f);
    CHECK_FEQ(s.plant.i_field_a, 3.0f, 0.15f);  /* dynamic clamp ⇒ I_f = 12 V/4 Ω */
    CHECK(s.inv_violations == 0);
    printf("   settled: duty %.3f (12/V=%.3f), %.0f A, bus %.2f V\n",
           (double)s.cmd.field_duty, (double)(12.0f / s.plant.vbus_v),
           (double)s.plant.i_alt_a, (double)s.plant.vbus_v);
}

/* --------------------------------------------------------------------------
 * 9) STALLED ROTOR (GH#37 / CONTROL_SPEC §5.2): engine dies mid-charge with
 *    ignition still on and the bus live. Without the run-detect gate the core
 *    held the field at the clamp — ~3 A of pure I²R into a fanless rotor,
 *    indefinitely. Now the field must fall to the pulse-cycled detect budget
 *    within spec time and stay there for hours without heating the rotor.
 * -------------------------------------------------------------------------- */
void scn_stalled_rotor(void)
{
    banner("stalled_rotor (§5.2 run-detect gate, key-on-engine-off)");
    sil_t s; sil_init(&s, 0x57A11ED1u, 0.50f);
    s.engine_rpm = 2330.0f;

    /* Charge normally for 2 min — field settles at the rotor clamp. */
    for (uint32_t i = 0; i < 120u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.cmd.field_duty > 0.15f);                /* genuinely energized before */

    /* Engine stalls (stator stops; sil reports RPM LOST + NOT_RUNNING). The
     * field must be at/below the detect budget within 1 s of the stall. */
    s.engine_rpm = 0.0f;
    const float budget_duty = CTRL_RUN_DETECT_EFFORT * 0.25f;   /* ≈5 % of ≈25 % clamp */
    sil_run_ticks(&s, 100);                         /* 1 s */
    CHECK(s.cmd.field_duty <= budget_duty + 1e-4f);

    /* Key-on-engine-off soak: 2 h stalled. Budget respected every tick, the
     * pulse duty cycle is bounded, and the rotor never warms — the exact
     * "can never cook the rotor" property §5.2 demands. */
    sil_reset_maxima(&s);
    uint32_t on_ticks = 0, n = 2u * 3600u * 100u;
    for (uint32_t i = 0; i < n; i++) {
        sil_step(&s);
        if (s.cmd.field_duty > 0.0f) on_ticks++;
    }
    CHECK(s.inv_violations == 0);
    CHECK(s.max_duty_seen <= budget_duty + 1e-4f);  /* never above the budget */
    CHECK(s.max_ifield_seen <= 0.25f);              /* ~0.15 A pulses, not 3 A */
    CHECK(s.max_rotor_w_seen <= 1.0f);              /* vs ~29 W ungated */
    CHECK((float)on_ticks / (float)n <= 0.15f);     /* bounded on-time ratio */
    CHECK(s.plant.rotor_temp_c <= s.plant.cfg.ambient_c + 2.0f);  /* stone cold */
    CHECK(ctrl_fault_disposition(s.cmd.faults) == CTRL_DISP_CONTINUE);
    printf("   stalled 2 h: max duty %.4f (budget %.4f), max I_f %.3f A, "
           "rotor %.1f C, on-ratio %.3f\n",
           (double)s.max_duty_seen, (double)budget_duty,
           (double)s.max_ifield_seen, (double)s.plant.rotor_temp_c,
           (double)((float)on_ticks / (float)n));

    /* Engine restarts → gate releases, soft ramp resumes, charging recovers. */
    s.engine_rpm = 2330.0f;
    for (uint32_t i = 0; i < 120u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.plant.i_alt_a > 30.0f);
    CHECK(s.inv_violations == 0);
}

/* --------------------------------------------------------------------------
 * 10) LYING VBAT (GH#37 / CONTROL_SPEC §5.1): the dynamic clamp is computed
 *     from MEASURED supply voltage — an in-range-but-false-LOW reading used to
 *     loosen duty_max onto the true (higher) bus. The plausibility guard must
 *     hold the clamp at the last-trusted level: NEVER looser than the clamp
 *     the TRUE voltage would give. False-HIGH must tighten (safe direction).
 * -------------------------------------------------------------------------- */
void scn_lying_vbat(void)
{
    banner("lying_vbat (§5.1 clamp-supply plausibility)");
    sil_t s; sil_init(&s, 0x11E5BA77u, 0.52f);
    s.engine_rpm = 2330.0f;

    /* Settle clamp-limited at the true bus (~54.5 V → duty ≈ 12/54.5). */
    for (uint32_t i = 0; i < 180u * 100u; i++) { s.plant.soc = 0.52f; sil_step(&s); }
    CHECK(s.cmd.binding == CTRL_BIND_ROTOR_CLAMP);
    CHECK_FEQ(s.cmd.field_duty, 12.0f / s.plant.vbus_v, 0.004f);

    /* Phase A: false LOW — sensor claims 49.0 V (in plausible range for 16S),
     * true bus stays ~54.5 V. 2 min lie (inside the distrust hold): the duty
     * must NEVER exceed the true-voltage clamp, rotor current stays ≤ rated,
     * and the distrust is telemetered as a WARN — not silent. */
    s.inj.override_en[PS_VBAT] = true;
    s.inj.override_v[PS_VBAT] = 49.0f;
    sil_reset_maxima(&s);
    float true_clamp_min = 1.0f;
    for (uint32_t i = 0; i < 120u * 100u; i++) {
        s.plant.soc = 0.52f;
        sil_step(&s);
        const float tc = 12.0f / s.plant.vbus_v;    /* clamp truth would give */
        if (tc < true_clamp_min) true_clamp_min = tc;
        if (s.cmd.field_duty > tc + 1e-4f) {        /* the #1 assertion */
            CHECK(false);
            break;
        }
    }
    CHECK(s.max_duty_seen <= true_clamp_min + 1e-3f);   /* never loosened */
    CHECK(s.max_ifield_seen <= 3.05f);                  /* rotor at/below rated */
    CHECK(s.cmd.faults & CTRL_FAULT_VSUP_IMPLAUSIBLE);  /* WARN raised */
    CHECK(ctrl_fault_disposition(s.cmd.faults) == CTRL_DISP_CONTINUE); /* still charging */
    CHECK(s.plant.i_alt_a > 30.0f);                     /* output not killed either */
    printf("   false-low: max duty %.4f vs true clamp %.4f, max I_f %.2f A\n",
           (double)s.max_duty_seen, (double)true_clamp_min,
           (double)s.max_ifield_seen);

    /* Reading recovers → WARN clears, clamp follows the honest voltage again. */
    s.inj.override_en[PS_VBAT] = false;
    for (uint32_t i = 0; i < 30u * 100u; i++) { s.plant.soc = 0.52f; sil_step(&s); }
    CHECK(!(s.cmd.faults & CTRL_FAULT_VSUP_IMPLAUSIBLE));
    CHECK_FEQ(s.cmd.field_duty, 12.0f / s.plant.vbus_v, 0.004f);

    /* Phase B: false HIGH (58 V claimed, true ~54.5 V) — followed immediately
     * because a higher assumed supply can only TIGHTEN the clamp. */
    s.inj.override_en[PS_VBAT] = true;
    s.inj.override_v[PS_VBAT] = 58.0f;
    sil_reset_maxima(&s);
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.52f; sil_step(&s); }
    CHECK(s.max_duty_seen <= 12.0f / 58.0f + 1e-3f);    /* tightened to 12/58 */
    CHECK(s.inv_violations == 0);
}

/* --------------------------------------------------------------------------
 * 11) T3 AH-REVERT (GH#37 / PROFILE_SPEC §2.2): with a zero-rest profile the
 *     bank rests at STANDBY while house loads discharge it. LFP voltage is
 *     flat there — the Ah integrator, not voltage, must bring CHARGE back at
 *     the configured net-Ah deficit (battery-truth tiers only).
 * -------------------------------------------------------------------------- */
void scn_ah_revert(void)
{
    banner("ah_revert (T3 net-Ah integrator, zero-rest profile)");
    sil_t s; sil_init(&s, 0xA43E7E41u, 0.85f);
    s.engine_rpm = 2800.0f;

    /* Small bank + short CV hold so the charge phase is quick; zero-rest
     * profile; Ah revert armed at 15 Ah; voltage revert parked far below the
     * LFP plateau so only the Ah path can fire. */
    s.g.bank_capacity_ah  = 100.0f;
    s.g.p_tail_w          = 0.04f * 100.0f * 16.0f * 3.45f;   /* ≈ 221 W */
    s.g.cv_hold_exit_min  = 2;
    s.plant.cfg.capacity_ah = 100.0f;
    plant_init(&s.plant, &s.plant.cfg, 0.85f);
    s.prof.rest_mode      = CTRL_REST_ZERO;
    s.prof.ah_revert      = 15.0f;
    s.prof.v_revert_vcell = 3.05f;          /* 48.8 V pack — unreachable here */

    /* Charge to the charged exit → STANDBY-rest (zero field). */
    uint32_t rest_tick = 0;
    for (uint32_t i = 0; i < 2u * 3600u * 100u; i++) {
        sil_step(&s);
        if (s.cmd.state == CTRL_STANDBY && s.cmd.standby_reason == CTRL_SB_REST) {
            rest_tick = i; break;
        }
    }
    CHECK(rest_tick > 0);
    CHECK_FEQ(s.ctrl.ah_since_charged, 0.0f, 0.01f);   /* reset at charged */
    CHECK(s.cmd.field_open);                           /* true zero field */

    /* House loads discharge the resting bank: ~2 kW ≈ 38 A. Expected revert
     * after 15 Ah ≈ 23–24 min; voltage stays on the plateau the whole time. */
    s.house_load_w = 2000.0f;
    uint32_t revert_tick = 0;
    float min_vcell = 10.0f;
    for (uint32_t i = 0; i < 60u * 60u * 100u; i++) {  /* ≤ 1 h */
        sil_step(&s);
        const float vc = s.plant.vbus_v / 16.0f;
        if (vc < min_vcell) min_vcell = vc;
        if (s.cmd.state == CTRL_BULK) { revert_tick = i; break; }
    }
    CHECK(revert_tick > 0);                            /* it DID revert */
    CHECK(min_vcell > s.prof.v_revert_vcell);          /* …and NOT via voltage */
    /* Timing: 15 Ah at ~38 A → ~23.5 min; accept 18–32 min (sag/eff. margins). */
    const float revert_min = (float)revert_tick / 6000.0f;
    CHECK(revert_min > 18.0f && revert_min < 32.0f);
    CHECK(s.ctrl.ah_since_charged >= 14.5f);           /* integrator did the work */
    printf("   reverted after %.1f min (%.1f Ah), min %.3f V/cell\n",
           (double)revert_min, (double)s.ctrl.ah_since_charged, (double)min_vcell);

    /* Recharge completes → integrator resets for the next cycle. */
    s.house_load_w = 0.0f;
    for (uint32_t i = 0; i < 3600u * 100u; i++) {
        sil_step(&s);
        if (s.cmd.state == CTRL_STANDBY && s.cmd.standby_reason == CTRL_SB_REST) break;
    }
    CHECK(s.cmd.state == CTRL_STANDBY && s.cmd.standby_reason == CTRL_SB_REST);
    CHECK_FEQ(s.ctrl.ah_since_charged, 0.0f, 0.01f);
    CHECK(s.inv_violations == 0);

    /* Control sub-run: tier 3 (alternator-side shunt) — Ah revert must stay
     * DISARMED (PROFILE_SPEC §4.2); the same discharge does not revert. */
    sil_t s2; sil_init(&s2, 0xA43E7E42u, 0.85f);
    s2.engine_rpm = 2800.0f;
    s2.isrc = CTRL_ISRC_ALT_SHUNT;
    s2.g.bank_capacity_ah  = 100.0f;
    s2.g.cv_hold_exit_min  = 2;
    s2.plant.cfg.capacity_ah = 100.0f;
    plant_init(&s2.plant, &s2.plant.cfg, 0.85f);
    s2.prof.rest_mode      = CTRL_REST_ZERO;
    s2.prof.ah_revert      = 15.0f;
    s2.prof.v_revert_vcell = 3.05f;
    for (uint32_t i = 0; i < 2u * 3600u * 100u; i++) {
        sil_step(&s2);
        if (s2.cmd.state == CTRL_STANDBY && s2.cmd.standby_reason == CTRL_SB_REST) break;
    }
    CHECK(s2.cmd.standby_reason == CTRL_SB_REST);
    s2.house_load_w = 2000.0f;
    for (uint32_t i = 0; i < 40u * 60u * 100u; i++) sil_step(&s2);   /* 40 min */
    CHECK(s2.cmd.state == CTRL_STANDBY);               /* no battery truth → no Ah revert */
    CHECK_FEQ(s2.ctrl.ah_since_charged, 0.0f, 0.01f);
    CHECK(s2.inv_violations == 0);
}

/* --------------------------------------------------------------------------
 * 8) Long soak: millions of ticks of engine/load/ignition cycling with mild
 *    noise. State machine must never leak: states stay valid, outputs stay
 *    finite, the bank cycles BULK↔FLOAT↔STANDBY repeatedly.
 * -------------------------------------------------------------------------- */
void scn_long_soak(void)
{
    banner("long_soak (5,000,000 ticks ≈ 13.9 h sim)");
    sil_t s; sil_init(&s, 0x50AC0001u, 0.80f);

    /* Smaller bank cycles faster; revert set inside the plateau so discharge
     * phases genuinely re-trigger BULK. */
    s.g.bank_capacity_ah = 100.0f;
    s.g.p_tail_w = 0.04f * 100.0f * 16.0f * 3.45f;   /* ≈ 221 W */
    s.prof.v_revert_vcell = 3.32f;
    s.plant.cfg.capacity_ah = 100.0f;
    plant_init(&s.plant, &s.plant.cfg, 0.80f);
    s.inj.noise[PS_VBAT] = 0.005f;
    s.inj.noise[PS_AMPS] = 0.005f;
    s.inj.noise[PS_ALT_T] = 0.5f;

    uint32_t bulk_entries = 0, float_entries = 0, standby_entries = 0;
    ctrl_state_t prev = CTRL_STANDBY;

    const uint32_t N = 5000000u;
    for (uint32_t i = 0; i < N; i++) {
        /* 40-min cycle: engine+ignition on 30 min, off 10 min. */
        const uint32_t phase = i % (40u * 60u * 100u);
        const bool on = phase < (30u * 60u * 100u);
        s.engine_rpm = on ? 2600.0f : 0.0f;
        s.ignition   = on;
        /* Load square wave, 5-min period: 400 W ↔ 2500 W. */
        s.house_load_w = ((i / (5u * 60u * 100u)) % 2u) ? 2500.0f : 400.0f;

        sil_step(&s);

        if (s.cmd.state != prev) {
            if (s.cmd.state == CTRL_BULK)    bulk_entries++;
            if (s.cmd.state == CTRL_FLOAT)   float_entries++;
            if (s.cmd.state == CTRL_STANDBY) standby_entries++;
            prev = s.cmd.state;
        }
    }

    CHECK(s.inv_violations == 0);            /* every-tick invariants held */
    CHECK(bulk_entries >= 3);                /* the machine genuinely cycled */
    CHECK(float_entries >= 3);
    CHECK(standby_entries >= 10);
    CHECK(s.plant.soc > 0.25f && s.plant.soc < 1.05f);
    CHECK(isfinite(s.ctrl.v_ctrl_f) && isfinite(s.ctrl.p_tail_f));
    CHECK(s.ctrl.time_in_state_ms < (uint64_t)N * SIL_DT_MS);  /* no stuck timer */
    CHECK(s.plant.alt_temp_c < 150.0f && s.plant.batt_temp_c < 60.0f);
    CHECK(s.max_ifield_seen <= 3.3f);        /* rotor never overdriven, ever */
    printf("   entries: BULK %u, FLOAT %u, STANDBY %u; final SOC %.2f, max duty %.4f\n",
           bulk_entries, float_entries, standby_entries,
           (double)s.plant.soc, (double)s.max_duty_seen);
}

/* --------------------------------------------------------------------------
 * 9) Driver-stage over-temp (§5.1, GH#39): the FET/driver NTC (PA3) is the
 *    only guard the field switch has (BKIN unrouted, §0.6 V1+V2; no field-
 *    current sense, §0.6 V4) — this is where "does the protection actually
 *    bite under transient abuse" gets answered for it, the way scn_temperature
 *    already answers it for the alternator governor. The plant's physical
 *    driver_temp_c model can't reach these temperatures under a clamp-
 *    respecting duty (I_f is capped near the ~3.4 A calibration point, so
 *    I_f^2 heating tops out around 45 C, scn_plant_calibration) — exactly why
 *    a real guard is needed rather than relying on "it can't get that hot":
 *    a stuck fan or degraded heatsink is a cooling-path failure, not a
 *    current-path one, and this model has no cooling-failure knob. So, like
 *    scn_temperature's block (d), the sensor CHANNEL is overridden directly
 *    (plant_inject_t) to inject the fault condition regardless of the
 *    physical model, and the closed loop is left to react for real.
 * -------------------------------------------------------------------------- */
void scn_driver_overtemp(void)
{
    banner("driver_overtemp (§5.1 graduated derate + 125 C hard block, GH#39)");

    /* a) Healthy baseline: settle into BULK at cruise RPM, record power. */
    sil_t s; sil_init(&s, 0xD817E001u, 0.50f);
    s.engine_rpm = 2800.0f;
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.cmd.state == CTRL_BULK);
    CHECK(!s.cmd.field_open);
    const float w_baseline = s.plant.i_batt_a * s.plant.vbus_v;
    CHECK(w_baseline > 3000.0f);              /* genuinely charging, not stalled */
    CHECK(s.inv_violations == 0);

    /* b) Onset of derate (110 C, between target 100 and hard 120): the
     *    governor engages and power is measurably reduced -- a derate, not a
     *    cutoff: no fault yet, field still driving. */
    s.inj.override_en[PS_DRV_T] = true;
    s.inj.override_v[PS_DRV_T]  = 110.0f;
    bool bound_driver_thermal = false;
    for (uint32_t i = 0; i < 5u * 60u * 100u; i++) {   /* 5 min */
        s.plant.soc = 0.50f;
        sil_step(&s);
        if (s.cmd.binding == CTRL_BIND_DRIVER_THERMAL) bound_driver_thermal = true;
    }
    CHECK(bound_driver_thermal);                       /* governor engaged, telemetered */
    CHECK(!s.cmd.field_open);
    CHECK(!(s.cmd.faults & CTRL_FAULT_DRIVER_OVERTEMP));
    const float w_derated = s.plant.i_batt_a * s.plant.vbus_v;
    CHECK(w_derated < w_baseline * 0.5f);              /* the derate actually bites */
    CHECK(s.inv_violations == 0);

    /* c) At the hard_c edge (120 C, where WARN already fires, control.c
     *    DRIVER_HOT_HARD_C): ceiling pulled to the derate floor -- "fast pull
     *    to floor" (§4.2) -- still no hard fault, still no cutoff. */
    s.inj.override_v[PS_DRV_T] = 120.0f;
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(!(s.cmd.faults & CTRL_FAULT_DRIVER_OVERTEMP));
    CHECK(!s.cmd.field_open);
    const float w_floor = s.plant.i_batt_a * s.plant.vbus_v;
    CHECK(w_floor <= s.drv_thcfg.derate_floor_w * 1.5f + 200.0f);
    CHECK(s.inv_violations == 0);

    /* d) 125 C: hard block. CTRL_FAULT_DRIVER_OVERTEMP fires and the field
     *    opens immediately -- matching, never weaker than, stock's single
     *    125 C fault (§0.6 V8, `0x4029`). */
    s.inj.override_v[PS_DRV_T] = 125.0f;
    for (uint32_t i = 0; i < 10u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.cmd.faults & CTRL_FAULT_DRIVER_OVERTEMP);
    CHECK(s.cmd.field_open);
    CHECK(s.applied_duty == 0.0f);
    CHECK(s.inv_violations == 0);

    /* e) Cools back to a healthy reading -- the OPEN-class latch does NOT
     *    auto-resume (control.c: OPEN-class faults persist until reset, same
     *    as OVERVOLTAGE/WATCHDOG/etc.). A switch that hit 125 C needs
     *    intervention, not a cooldown -- unlike the BLOCK-class battery-temp
     *    faults scn_temperature() (a)/(b) exercise elsewhere in this file,
     *    which DO auto-resume by design (§9.1 D7). */
    s.inj.override_en[PS_DRV_T] = false;
    for (uint32_t i = 0; i < 60u * 100u; i++) { s.plant.soc = 0.50f; sil_step(&s); }
    CHECK(s.cmd.faults & CTRL_FAULT_DRIVER_OVERTEMP);
    CHECK(s.cmd.field_open);
    CHECK(s.applied_duty == 0.0f);
    CHECK(s.inv_violations == 0);
}
