/*
 * control.c — two-stage LFP engine (CONTROL_SPEC / PROFILE_SPEC). PURE (no HAL).
 *
 * STANDBY / BULK / FLOAT with transitions T1–T4 on damped signals; power
 * arbitration in Watts; CV clamp replaces "absorption"; normalized field effort
 * with the §5.1 rotor duty clamp (on a plausibility-vetted supply voltage) and
 * the §5.2 stationary-rotor detect budget; raw-signal safety comparators
 * independent of the control path. Inner-loop gains here are structural
 * placeholders — the shape is correct; numeric tuning is bench/emulation work.
 *
 * SPDX-License-Identifier: MIT
 */
#include "control.h"
#include "arbitration.h"
#include "field.h"
#include "faults.h"

/* Hard safety thresholds — raw signals only (§1.4, §7). */
#define OV_VCELL_HARD      3.70f   /* per-cell overvoltage kill (above bulk 3.60) */
#define ALT_HOT_HARD_C     120.0f
#define DRIVER_HOT_HARD_C  120.0f
#define BATT_LOW_C         0.0f    /* Li low-temp charge cutoff */
#define BATT_HIGH_C        55.0f   /* high-temp charge abort */

/* Damped-signal time constants (s). */
#define TAU_VCTRL          1.0f
#define TAU_VREVERT        30.0f
#define TAU_PTAIL          60.0f

/* [SIL-found 2026-07] Tolerance band (V/cell) for the "voltage-clamped"
 * qualifier. The CV loop regulates ~V to the target from below, so with any
 * sensor noise/ripple `v >= cv_pack` flickers and the T2 hold timers
 * (tail / cv_hold_exit / Solar-Finish) reset forever — in the SIL long-soak
 * BULK could never exit at CV. Within-band counts as clamped. */
#define CV_CLAMP_BAND_VCELL 0.01f

/* Inner-loop gains (placeholders; bench-tunable). Normalized effort per unit
 * of error per SECOND.
 * [SIL-found 2026-07] Previously per-TICK (tick-rate dependent), and the power
 * gain (5e-5/W/tick @10 ms ≡ 5e-3/W/s) was marginally unstable against the
 * calibrated 48 V plant (loop gain ≈ 2 at cruise RPM → limit-cycle chatter and
 * ceiling overshoot in the §8.1 SIL BMS-step scenario). Gains are now
 * dt-scaled; KV is unchanged at the 10 ms design rate, KP reduced 5×. */
#define KV_PER_VOLT_S      2.0f      /* == 0.02/tick at the 10 ms design rate */
#define KP_PER_WATT_S      1.0e-3f   /* == 1e-5/tick at 10 ms (was 5e-5/tick) */

static float ema(float prev, float x, float tau_s, float dt_s)
{
    if (isnan(prev) || tau_s <= 0.0f) return x;   /* seed on first sample */
    float a = dt_s / (tau_s + dt_s);
    return prev + a * (x - prev);
}

static void enter(ctrl_t *c, ctrl_state_t s, ctrl_standby_reason_t r)
{
    c->state = s;
    c->standby_reason = r;
    c->time_in_state_ms = 0;
    c->tail_hold_ms = 0;
    c->revert_hold_ms = 0;
    c->vclamp_hold_ms = 0;
}

static bool tier_battery_truth(ctrl_isrc_tier_t t)
{
    return t == CTRL_ISRC_BATT_SHUNT || t == CTRL_ISRC_NMEA_BATT;
}

/* T3 revert test (FLOAT→BULK, STANDBY-rest→BULK). Advances the revert hold timer. */
static bool revert_met(ctrl_t *c, const ctrl_measured_t *m,
                       const ctrl_profile_t *p, const ctrl_globals_t *g, uint32_t dt_ms)
{
    const uint8_t cells = g->cells_series ? g->cells_series : 1;
    const float vr_pack = p->v_revert_vcell * (float)cells;
    bool met = false;

    if (c->v_revert_f <= vr_pack) {
        c->revert_hold_ms += dt_ms;
        if (c->revert_hold_ms >= (uint32_t)p->t_revert_hold_s * 1000u) met = true;
    } else {
        c->revert_hold_ms = 0;
    }
    if (p->soc_revert_pct >= 0 && m->soc_trusted &&
        m->soc_pct >= 0.0f && m->soc_pct <= (float)p->soc_revert_pct) met = true;
    /* T3 Ah path: net Ah discharged since charged, battery-truth tiers only. */
    if (tier_battery_truth(m->isrc) && p->ah_revert > 0.0f &&
        c->ah_since_charged >= p->ah_revert) met = true;

    return met;
}

/* Skip-BULK-if-full test at first activation: battery already charged (resting
 * voltage or trusted SOC) → go straight to REST instead of re-absorbing. */
static bool skip_bulk_if_full(const ctrl_t *c, const ctrl_measured_t *m, const ctrl_globals_t *g)
{
    const uint8_t cells = g->cells_series ? g->cells_series : 1;
    if (g->skip_bulk_vcell > 0.0f && !isnan(c->v_ctrl_f) &&
        c->v_ctrl_f / (float)cells >= g->skip_bulk_vcell) return true;
    if (g->skip_bulk_soc_pct > 0 && m->soc_trusted &&
        m->soc_pct >= (float)g->skip_bulk_soc_pct) return true;
    return false;
}

void ctrl_init(ctrl_t *c)
{
    c->state = CTRL_STANDBY;
    c->standby_reason = CTRL_SB_OFF;
    c->effort = 0.0f;
    c->cmd_power_w = 0.0f;
    c->time_in_state_ms = 0;
    c->faults = CTRL_FAULT_NONE;
    c->ah_since_charged = 0.0f;
    c->v_ctrl_f = NAN;
    c->v_revert_f = NAN;
    c->p_tail_f = NAN;
    c->warmup_ms = 0;
    c->tail_hold_ms = 0;
    c->revert_hold_ms = 0;
    c->vclamp_hold_ms = 0;
    ctrl_vsup_guard_init(&c->vsup);
    c->run_probe_ms = 0;
}

ctrl_command_t ctrl_tick(ctrl_t *c,
                         const ctrl_measured_t *m,
                         const ctrl_ceilings_t *ceil,
                         const ctrl_profile_t  *prof,
                         const ctrl_globals_t  *g,
                         uint32_t dt_ms)
{
    const float dt_s = (float)dt_ms / 1000.0f;
    const uint8_t cells = g->cells_series ? g->cells_series : 1;
    c->time_in_state_ms += dt_ms;

    /* ---- damped signals (§1.4) ---- */
    c->v_ctrl_f   = ema(c->v_ctrl_f,   m->vcomp_pack_v, TAU_VCTRL,   dt_s);
    c->v_revert_f = ema(c->v_revert_f, m->vcomp_pack_v, TAU_VREVERT, dt_s);
    c->p_tail_f   = ema(c->p_tail_f,   m->watts_batt,   TAU_PTAIL,   dt_s);

    /* ---- §5.1 clamp-supply plausibility (vetted voltage for duty_max) ---- */
    bool vsup_bad = false;
    const float v_clamp = ctrl_vsup_guard(&c->vsup, m->v_supply_v, m->vbat_pack_v,
                                          cells, dt_ms, &vsup_bad);

    /* ---- T3 Ah-revert integrator (PROFILE_SPEC §2.2/§5) ---- *
     * Net Ah discharged since the last charged exit; battery-truth tiers only
     * (§4.2). Charging pays the deficit back down, never below zero. */
    if ((c->state == CTRL_FLOAT ||
         (c->state == CTRL_STANDBY && c->standby_reason == CTRL_SB_REST)) &&
        tier_battery_truth(m->isrc) && isfinite(m->amps_batt)) {
        c->ah_since_charged -= m->amps_batt * dt_s / 3600.0f;  /* +A = charging */
        if (c->ah_since_charged < 0.0f) c->ah_since_charged = 0.0f;
    }

    /* ---- faults: latch OPEN-class, recompute the rest live, merge external ---- */
    uint32_t faults = c->faults & CTRL_FAULT_OPEN_MASK;   /* criticals latch until reset */
    if (vsup_bad) faults |= CTRL_FAULT_VSUP_IMPLAUSIBLE;  /* WARN: clamp holds tight */
    if (!isnan(m->vbat_pack_v)) {
        if (m->vbat_pack_v / (float)cells >= OV_VCELL_HARD) faults |= CTRL_FAULT_OVERVOLTAGE;
    } else {
        faults |= CTRL_FAULT_LOST_VBAT_SENSE;             /* recoverable → LIMP */
    }
    if (!isnan(m->alt_hotspot_c) && m->alt_hotspot_c >= ALT_HOT_HARD_C)   faults |= CTRL_FAULT_SELF_OVERTEMP;
    if (!isnan(m->driver_temp_c) && m->driver_temp_c >= DRIVER_HOT_HARD_C) faults |= CTRL_FAULT_SELF_OVERTEMP;
    if (!isnan(m->batt_temp_c)) {
        if (m->batt_temp_c <= BATT_LOW_C)  faults |= CTRL_FAULT_BATT_LOWTEMP;
        if (m->batt_temp_c >= BATT_HIGH_C) faults |= CTRL_FAULT_BATT_HIGHTEMP;
    }
    faults |= m->ext_faults;                              /* BMS loss, shunt implausible, … */
    c->faults = faults;

    const ctrl_disposition_t disp = ctrl_fault_disposition(faults);
    const bool open       = (disp == CTRL_DISP_OPEN);
    const bool limp       = (disp == CTRL_DISP_LIMP);
    const bool batt_block = (faults & CTRL_FAULT_BLOCK_MASK) != 0;
    const bool enable     = m->ignition && !open && !batt_block;

    /* ---- state machine (T1–T4) ---- */
    if (!enable) {
        enter(c, CTRL_STANDBY, open ? CTRL_SB_FAULT : CTRL_SB_OFF);
    } else if (limp) {
        if (c->state != CTRL_FLOAT) enter(c, CTRL_FLOAT, CTRL_SB_OFF);  /* Limp Home = FLOAT@v_limp */
    } else {
        switch (c->state) {
        case CTRL_STANDBY:
            if (c->standby_reason == CTRL_SB_OFF || c->standby_reason == CTRL_SB_FAULT) {
                enter(c, CTRL_STANDBY, CTRL_SB_WARMUP);
                c->warmup_ms = 0;
            }
            if (c->standby_reason == CTRL_SB_WARMUP) {              /* T1 warmup gate */
                c->warmup_ms += dt_ms;
                if (g->warmup_time_s == 0 || c->warmup_ms >= (uint32_t)g->warmup_time_s * 1000u) {
                    if (skip_bulk_if_full(c, m, g)) {              /* already full → skip BULK */
                        c->ah_since_charged = 0.0f;                /* full = new T3 Ah baseline */
                        if (prof->rest_mode == CTRL_REST_ZERO) enter(c, CTRL_STANDBY, CTRL_SB_REST);
                        else                                   enter(c, CTRL_FLOAT, CTRL_SB_OFF);
                    } else {
                        enter(c, CTRL_BULK, CTRL_SB_OFF);
                    }
                }
            } else if (c->standby_reason == CTRL_SB_REST) {         /* T3 revert from zero-rest */
                if (revert_met(c, m, prof, g, dt_ms)) enter(c, CTRL_BULK, CTRL_SB_OFF);
            }
            break;

        case CTRL_BULK: {                                          /* T2 charged exits */
            const float cv_pack = prof->cv_target_vcell * (float)cells;
            const bool clamped =
                (c->v_ctrl_f >= cv_pack - CV_CLAMP_BAND_VCELL * (float)cells);
            if (clamped) c->vclamp_hold_ms += dt_ms; else c->vclamp_hold_ms = 0;

            bool charged = false;
            if (prof->exit_at_cv_entry) {                          /* T2d Solar Finish */
                if (c->vclamp_hold_ms >= (uint32_t)g->t_vclamp_s * 1000u) charged = true;
            } else {
                /* PRIMARY: held at CV target for cv_hold_exit_min → full. Voltage+time
                 * only — robust regardless of shunt placement or current truth. */
                if (g->cv_hold_exit_min > 0 &&
                    c->vclamp_hold_ms >= (uint32_t)g->cv_hold_exit_min * 60000u) charged = true;
                /* OPTIONAL bonus: tail power (only with battery-truth current). */
                if (tier_battery_truth(m->isrc) && clamped && c->p_tail_f <= g->p_tail_w) {
                    c->tail_hold_ms += dt_ms;                      /* T2a tail power */
                    if (c->tail_hold_ms >= (uint32_t)g->t_tail_hold_s * 1000u) charged = true;
                } else {
                    c->tail_hold_ms = 0;
                }
                if (g->soc_target_pct >= 0 && m->soc_trusted &&    /* T2b SOC target */
                    m->soc_pct >= (float)g->soc_target_pct) charged = true;
            }
            if (c->time_in_state_ms >= (uint32_t)g->t_charge_max_min * 60000u) charged = true; /* T2c */

            if (charged) {
                c->ah_since_charged = 0.0f;                    /* T3 Ah counts from here */
                if (prof->rest_mode == CTRL_REST_ZERO) enter(c, CTRL_STANDBY, CTRL_SB_REST);
                else                                   enter(c, CTRL_FLOAT, CTRL_SB_OFF);
            }
            break;
        }

        case CTRL_FLOAT:
            if (revert_met(c, m, prof, g, dt_ms)) enter(c, CTRL_BULK, CTRL_SB_OFF); /* T3 */
            break;
        }
    }

    /* ---- field control (normalized effort) ---- */
    float effort = c->effort;
    ctrl_bind_src_t bind = CTRL_BIND_NONE;
    float bind_w = 0.0f;
    bool field_open = false;

    if (c->state == CTRL_STANDBY) {
        effort = 0.0f;
        field_open = true;
        c->cmd_power_w = 0.0f;
    } else if (isnan(c->v_ctrl_f)) {
        /* No usable voltage (e.g. lost VBat sense) — cannot regulate safely → off. */
        effort = 0.0f;
        c->cmd_power_w = 0.0f;
        bind = CTRL_BIND_NONE;
    } else {
        float stage_w, cv_vcell;
        if (c->state == CTRL_BULK)      { stage_w = g->max_charge_power_w;  cv_vcell = prof->cv_target_vcell; }
        else if (limp)                  { stage_w = g->limp_power_cap_w;    cv_vcell = g->limp_vcell; }
        else                            { stage_w = prof->rest_power_cap_w; cv_vcell = prof->rest_voltage_vcell; }

        const ctrl_arb_t arb = ctrl_arbitrate(stage_w, ceil);

        /* Ramp commanded power UP; DOWN is immediate (§3.5 asymmetry). */
        const float ramp_step = g->ramp_w_per_s * dt_s;
        if (arb.watts > c->cmd_power_w) {
            c->cmd_power_w += ramp_step;
            if (c->cmd_power_w > arb.watts) c->cmd_power_w = arb.watts;
        } else {
            c->cmd_power_w = arb.watts;
        }

        /* CV clamp vs power: the lower effort demand governs. */
        const float cv_pack = cv_vcell * (float)cells;
        const float v_err = cv_pack - c->v_ctrl_f;          /* >0 = below CV target */
        const float p_err = c->cmd_power_w - m->watts_batt;  /* >0 = below power target */
        const float e_from_v = effort + KV_PER_VOLT_S * v_err * dt_s;
        const float e_from_p = effort + KP_PER_WATT_S * p_err * dt_s;

        if (e_from_v <= e_from_p) { effort = e_from_v; bind = CTRL_BIND_VOLTAGE_CLAMP; bind_w = m->watts_batt; }
        else                      { effort = e_from_p; bind = arb.src;                 bind_w = arb.watts; }

        /* [SIL-found 2026-07] A NaN measurement (e.g. shunt dropout → watts_batt
         * = NaN) must never reach the PWM, and must not latch: NaN passed both
         * range clamps below and stuck in c->effort permanently. */
        if (!isfinite(effort)) { effort = 0.0f; bind = CTRL_BIND_NONE; bind_w = 0.0f; }

        if (effort < 0.0f) effort = 0.0f;
        if (effort > 1.0f) effort = 1.0f;
        if (arb.watts <= 0.0f) { effort = 0.0f; bind = CTRL_BIND_STAGE; bind_w = 0.0f; } /* unconfigured → off */
    }

    /* ---- §5.2 stationary-rotor gate ---- *
     * No rotation detected → the field is held to the detect budget: a small
     * pulse-cycled excitation (also the run-detect probe waveform) instead of
     * I²R-cooking a fanless rotor at the clamp. Commanded power resets so the
     * ramp_w_per_s soft start governs the resume when the engine runs again. */
    if (!field_open && !ctrl_run_detected(m)) {
        c->run_probe_ms += dt_ms;
        if (c->run_probe_ms >= CTRL_RUN_PROBE_PERIOD_MS) c->run_probe_ms = 0;
        const float budget = (c->run_probe_ms < CTRL_RUN_PROBE_ON_MS)
                             ? CTRL_RUN_DETECT_EFFORT : 0.0f;
        if (effort > budget) { effort = budget; bind = CTRL_BIND_RUN_DETECT; bind_w = 0.0f; }
        c->cmd_power_w = 0.0f;
    } else {
        c->run_probe_ms = 0;
    }

    c->effort = effort;
    const float duty_max = ctrl_duty_max(g, v_clamp);   /* §5.1, vetted supply */
    const float duty = ctrl_effort_to_duty(effort, duty_max);
    if (!field_open && effort >= 1.0f && duty_max < 1.0f) bind = CTRL_BIND_ROTOR_CLAMP;

    ctrl_command_t cmd = {
        .field_effort   = effort,
        .field_duty     = duty,
        .field_open     = field_open,
        .state          = c->state,
        .standby_reason = c->standby_reason,
        .binding        = bind,
        .binding_w      = bind_w,
        .faults         = c->faults,
    };
    return cmd;
}
