/*
 * sil.c — SIL scenario runner: steps the plant, calls ctrl_tick() at the
 * designed 10 ms rate, applies the field command back, checks invariants.
 * SPDX-License-Identifier: MIT
 */
#include "sil.h"
#include <stdio.h>
#include <string.h>

void sil_init(sil_t *s, uint32_t seed, float soc0)
{
    memset(s, 0, sizeof(*s));

    plant_cfg_t cfg;
    plant_cfg_default(&cfg);
    plant_init(&s->plant, &cfg, soc0);
    plant_inject_init(&s->inj, seed);

    ctrl_init(&s->ctrl);

    /* Globals — PROFILE_SPEC §3.1 defaults on the 16S/300 Ah install. */
    s->g.cells_series       = 16;
    s->g.bank_capacity_ah   = 300.0f;
    s->g.max_charge_power_w = 8000.0f;
    s->g.ramp_w_per_s       = 200.0f;
    s->g.p_tail_w           = 0.04f * 300.0f * 16.0f * 3.45f;  /* ≈ 662 W */
    s->g.t_tail_hold_s      = 60;
    s->g.t_vclamp_s         = 5;
    s->g.cv_hold_exit_min   = 15;
    s->g.t_charge_max_min   = 480;
    s->g.warmup_time_s      = 5;
    s->g.warmup_coolant_c   = NAN;
    s->g.soc_target_pct     = -1;
    s->g.skip_bulk_vcell    = 0.0f;
    s->g.skip_bulk_soc_pct  = -1;
    s->g.rotor_rated_v      = 12.0f;
    s->g.rotor_v_max        = NAN;
    s->g.allow_full_field_48v = false;
    s->g.limp_vcell         = 3.30f;
    s->g.limp_power_cap_w   = 2000.0f;

    /* Profile 1 — Bulk, Float Norm (PROFILE_SPEC §4.1). */
    s->prof.id                 = 1;
    s->prof.cv_target_vcell    = 3.60f;
    s->prof.exit_at_cv_entry   = false;
    s->prof.rest_mode          = CTRL_REST_HOLD;
    s->prof.rest_voltage_vcell = 3.40f;
    s->prof.rest_power_cap_w   = 8000.0f;
    s->prof.v_revert_vcell     = 3.28f;
    s->prof.t_revert_hold_s    = 30;
    s->prof.soc_revert_pct     = -1;
    s->prof.ah_revert          = 90.0f;

    /* Hardware limit set (CONTROL_SPEC §2.1). */
    s->lim.battery_c_limit    = 0.8f;     /* 240 A on 300 Ah */
    s->lim.wiring_limit_a     = 250.0f;
    s->lim.alternator_limit_a = 250.0f;

    /* Thermal governor (CONTROL_SPEC §4). */
    s->thcfg.tau_s          = 300.0f;
    s->thcfg.target_c       = 95.0f;
    s->thcfg.hard_c         = 120.0f;
    s->thcfg.derate_floor_w = 500.0f;
    s->thcfg.ceiling_max_w  = 12000.0f;
    s->thcfg.gain_w_per_c_s = 5.0f;
    ctrl_thermal_init(&s->thermal, &s->thcfg);

    /* World defaults. */
    s->engine_rpm   = 2800.0f;
    s->house_load_w = 0.0f;
    s->ignition     = true;
    s->isrc         = CTRL_ISRC_BATT_SHUNT;
    s->feed_soc     = false;
    s->bms_ccl_w    = CTRL_CEILING_INACTIVE;
    s->user_cap_w   = CTRL_CEILING_INACTIVE;
    s->engine_w     = CTRL_CEILING_INACTIVE;
    s->ext_faults_extra = 0u;

    s->applied_duty = 0.0f;
    s->inv_skip_clamp = false;
    sil_reset_maxima(s);
}

void sil_reset_maxima(sil_t *s)
{
    s->max_duty_seen    = 0.0f;
    s->max_ifield_seen  = 0.0f;
    s->max_rotor_w_seen = 0.0f;
}

static void inv(sil_t *s, bool ok, const char *what)
{
    if (ok) return;
    s->inv_violations++;
    if (s->inv_violations <= 5) {
        printf("  [sil] INVARIANT VIOLATED @tick %llu: %s "
               "(duty=%.4f effort=%.4f vbus=%.2f v_meas=%.2f state=%d)\n",
               (unsigned long long)s->ticks, what,
               (double)s->cmd.field_duty, (double)s->cmd.field_effort,
               (double)s->plant.vbus_v, (double)s->meas.v_supply_v,
               (int)s->cmd.state);
    }
}

static void sil_invariants(sil_t *s)
{
    const ctrl_command_t *c = &s->cmd;

    /* Outputs must always be finite and in range — a NaN duty on hardware is
     * an undefined PWM compare value. */
    inv(s, isfinite(c->field_duty),   "field_duty finite");
    inv(s, isfinite(c->field_effort), "field_effort finite");
    inv(s, c->field_duty  >= 0.0f && c->field_duty  <= 1.0f, "field_duty in [0,1]");
    inv(s, c->field_effort >= 0.0f && c->field_effort <= 1.0f, "field_effort in [0,1]");
    inv(s, c->state == CTRL_STANDBY || c->state == CTRL_BULK || c->state == CTRL_FLOAT,
        "state valid");
    inv(s, c->standby_reason <= CTRL_SB_FAULT, "standby_reason valid");
    inv(s, c->binding <= CTRL_BIND_ROTOR_CLAMP, "binding valid");
    inv(s, !c->field_open || c->field_duty == 0.0f, "field_open implies zero duty");

    /* Plant sanity. */
    inv(s, isfinite(s->plant.vbus_v) && isfinite(s->plant.soc), "plant finite");

    /* ---- #1 safety property: the rotor clamp (CONTROL_SPEC §5.1) ---- */
    if (!s->inv_skip_clamp && !s->g.allow_full_field_48v) {
        const float rotor_v = isnan(s->g.rotor_v_max) ? s->g.rotor_rated_v
                                                      : s->g.rotor_v_max;
        const float v_meas = s->meas.v_supply_v;
        if (isfinite(v_meas) && v_meas > rotor_v) {
            /* The core's own promise: duty ≤ rotor_v / measured supply. */
            inv(s, c->field_duty <= rotor_v / v_meas + 1e-4f,
                "duty <= rotor_v / v_supply (clamp)");
        } else if (!isfinite(v_meas)) {
            /* No usable supply reading → the only safe duty is zero. */
            inv(s, c->field_duty == 0.0f, "no v_supply reading -> duty 0");
        }
        /* Physical rotor current, independent arithmetic against the TRUE bus
         * voltage. Only meaningful when the voltage channel is honest (small
         * noise, no implausible-value override). */
        if (!s->inj.override_en[PS_VBAT] && s->inj.noise[PS_VBAT] <= 0.03f &&
            s->plant.vbus_v > s->g.rotor_rated_v) {
            const float i_f_cmd = c->field_duty * s->plant.vbus_v
                                  / s->plant.cfg.rotor_r_ohm;
            inv(s, i_f_cmd <= s->plant.cfg.rotor_rated_a * 1.10f,
                "physical rotor current <= rated x1.10");
        }
    }

    if (c->field_duty > s->max_duty_seen)          s->max_duty_seen    = c->field_duty;
    if (s->plant.i_field_a > s->max_ifield_seen)   s->max_ifield_seen  = s->plant.i_field_a;
    if (s->plant.p_rotor_w > s->max_rotor_w_seen)  s->max_rotor_w_seen = s->plant.p_rotor_w;
}

void sil_step(sil_t *s)
{
    const float dt_s = (float)SIL_DT_MS / 1000.0f;

    /* 1) Physics with the duty applied at the end of the previous tick. */
    plant_step(&s->plant, s->applied_duty, s->engine_rpm, s->house_load_w, dt_s);

    /* 2) Sensors through the injection layer. */
    plant_sensed_t r;
    plant_sense(&s->plant, &s->inj, s->engine_rpm, &r);

    /* 3) Assemble the control input (mirrors Core/Src/main.c). */
    ctrl_measured_t m = {0};
    m.vbat_pack_v   = r.vbat_v;
    m.vcomp_pack_v  = r.vbat_v;                 /* no R model, as in the app */
    m.amps_batt     = r.amps_batt_a;
    m.watts_batt    = r.vbat_v * r.amps_batt_a;
    m.isrc          = s->isrc;
    m.v_supply_v    = r.vbat_v;                 /* field supply = bus */
    m.alt_hotspot_c = r.alt_temp_c;
    m.batt_temp_c   = r.batt_temp_c;
    m.driver_temp_c = r.driver_temp_c;
    m.rpm           = r.rpm;
    m.rpm_state     = (!isfinite(r.rpm) || r.rpm <= 0.0f) ? CTRL_RPM_LOST
                                                          : CTRL_RPM_VALID;
    m.run_state     = (s->engine_rpm > 100.0f) ? CTRL_RUN_RUNNING
                                               : CTRL_RUN_NOT_RUNNING;
    m.soc_pct       = s->feed_soc ? s->plant.soc * 100.0f : -1.0f;
    m.soc_trusted   = s->feed_soc;
    m.ignition      = s->ignition;
    m.feature_in    = false;

    /* App-side plausibility check, as a shunt driver would flag it. */
    m.ext_faults = s->ext_faults_extra;
    if (isfinite(m.amps_batt) && fabsf(m.amps_batt) > 600.0f)
        m.ext_faults |= CTRL_FAULT_IMPLAUSIBLE_SHUNT;

    /* 4) Ceilings: hardware limit set + thermal governor + scenario inputs. */
    ctrl_ceilings_t ceil = {
        .thermal_w = CTRL_CEILING_INACTIVE, .bms_ccl_w = CTRL_CEILING_INACTIVE,
        .battery_c_w = CTRL_CEILING_INACTIVE, .wiring_w = CTRL_CEILING_INACTIVE,
        .alt_absolute_w = CTRL_CEILING_INACTIVE,
        .alt_capability_w = CTRL_CEILING_INACTIVE,
        .belt_w = CTRL_CEILING_INACTIVE, .engine_w = CTRL_CEILING_INACTIVE,
        .user_cap_w = CTRL_CEILING_INACTIVE,
    };
    ctrl_limits_apply(&ceil, &s->lim, &s->g, r.vbat_v);
    ceil.thermal_w  = ctrl_thermal_update(&s->thermal, &s->thcfg, r.alt_temp_c, dt_s);
    ceil.bms_ccl_w  = s->bms_ccl_w;
    ceil.user_cap_w = s->user_cap_w;
    ceil.engine_w   = s->engine_w;

    /* 5) The core tick at its designed rate. */
    s->meas = m;
    s->cmd  = ctrl_tick(&s->ctrl, &m, &ceil, &s->prof, &s->g, SIL_DT_MS);

    /* 6) Apply the command back to the plant (as field_drive_set would). */
    s->applied_duty = s->cmd.field_open ? 0.0f : s->cmd.field_duty;

    s->ticks++;
    sil_invariants(s);
}

void sil_run_ticks(sil_t *s, uint32_t n)
{
    for (uint32_t i = 0; i < n; i++) sil_step(s);
}

void sil_run_s(sil_t *s, float sim_seconds)
{
    sil_run_ticks(s, (uint32_t)(sim_seconds * 1000.0f / (float)SIL_DT_MS));
}
