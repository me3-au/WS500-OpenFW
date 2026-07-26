/*
 * test_fuzz.c — PROJECT_PLAN §8.4 randomized sensor-noise / fault-injection
 * runs against the pure core, with FIXED seeds so every failure is replayable.
 *
 * Each run is a long tick loop (10 ms, the designed rate) in which:
 *   - a "true" bus voltage random-walks, and the engine's own field command
 *     feeds back into it (crude, deliberately not the §8.1 plant — this layer
 *     hunts invariant breaks, not physical fidelity);
 *   - EVERY sensor channel gets independent noise, dropouts (NaN), spikes,
 *     sign flips and bounded lie episodes;
 *   - config, ceilings, ignition, engine speed and external faults are all
 *     randomized, including OPEN-class faults followed by a reset;
 *   - the standing invariants are re-asserted on every single tick.
 *
 * The safety invariant is checked against the TRUE bus voltage, not the
 * reported one: commanded duty must never exceed ctrl_duty_max(v_true), so a
 * lying sensor cannot buy extra rotor volts. Two deliberate allowances:
 *   1. tolerance = the injected zero-mean supply-noise fraction — the core can
 *      only be as accurate as its input;
 *   2. while a supply-channel lie or dropout episode is active the true bus is
 *      held constant, and episodes are bounded well under
 *      CTRL_VSUP_DISTRUST_MAX_MS. Unbounded/from-boot lies are a characterized
 *      design residual, pinned separately in test_property.c (P2b).
 *
 * SPDX-License-Identifier: MIT
 */
#include "prop.h"
#include "arbitration.h"

#define FUZZ_TICKS   250000u          /* ~42 min of simulated time per seed @10 ms */
#define FUZZ_DT_MS   10u

static const uint32_t FUZZ_SEEDS[] = {
    0x00000001u, 0x0BADF00Du, 0xDEADBEEFu, 0x5EED1234u, 0xA5A5A5A5u, 0x13579BDFu,
    0xFEEDFACEu, 0x2468ACE0u, 0xC0FFEE01u, 0x7FFFFFFFu, 0x80000001u, 0x1234ABCDu,
    0x00C0FFEEu, 0x9E3779B9u, 0x45D9F3B1u, 0xB5297A4Du
};
#define N_FUZZ_SEEDS ((int)(sizeof FUZZ_SEEDS / sizeof FUZZ_SEEDS[0]))

/* One noisy sensor channel: base noise plus dropout / spike / lie episodes. */
typedef struct {
    float    noise;       /* fractional (multiplicative) noise amplitude */
    uint32_t drop_left;   /* ticks of NaN remaining */
    uint32_t lie_left;    /* ticks of lie remaining */
    float    lie_factor;
} fz_chan_t;

static float fz_read(fz_chan_t *ch, prop_rng_t *r, float truth, uint64_t tick,
                     int allow_episodes, uint32_t max_episode)
{
    if (allow_episodes && tick > 100u) {
        if (ch->drop_left == 0u && ch->lie_left == 0u) {
            if (prop_chance(r, 0.0002f)) {
                ch->drop_left = 1u + prop_below(r, max_episode);
            } else if (prop_chance(r, 0.0004f)) {
                ch->lie_left = 1u + prop_below(r, max_episode);
                /* Either a big low step (must be distrusted) or a high reading
                 * (can only tighten the clamp). Nothing in the "plausible sag"
                 * band — that band is followed by design; see P2b. */
                ch->lie_factor = prop_chance(r, 0.5f)
                                 ? prop_between(r, 0.20f, 0.80f)
                                 : prop_between(r, 1.20f, 3.00f);
            }
        }
    }
    if (ch->drop_left) { ch->drop_left--; return NAN; }

    float v = truth * (1.0f + ch->noise * prop_sym(r));
    if (ch->lie_left) { ch->lie_left--; v *= ch->lie_factor; }
    if (prop_chance(r, 0.0005f)) v *= prop_between(r, 0.05f, 8.0f);   /* single-tick spike */
    if (prop_chance(r, 0.0002f)) v = -v;                              /* sign flip */
    return v;
}

/* Additive-noise channel for temperatures (absolute units, not fractional). */
static float fz_read_abs(fz_chan_t *ch, prop_rng_t *r, float truth, float amp)
{
    if (prop_chance(r, 0.0005f)) ch->drop_left = 1u + prop_below(r, 500u);
    if (ch->drop_left) { ch->drop_left--; return NAN; }
    float v = truth + amp * prop_sym(r);
    if (prop_chance(r, 0.0004f)) v += prop_between(r, -200.0f, 300.0f);  /* spike */
    return v;
}

static void fuzz_one(uint32_t seed, prop_core_inv_t *iv, prop_inv_t *open_only_standby,
                     prop_inv_t *effort_settles, float *max_ratio_out, uint64_t *ticks_out)
{
    prop_rng_t r; prop_seed(&r, seed);

    /* ---- randomized system + config (mostly in-range, sometimes at the edge) ---- */
    static const uint8_t CELLSETS[] = { 4, 8, 16 };
    const uint8_t cells = CELLSETS[prop_below(&r, 3)];
    ctrl_globals_t g = prop_globals(cells);
    ctrl_profile_t p = prop_profile();

    g.max_charge_power_w = prop_between(&r, 100.0f, 20000.0f);
    g.ramp_w_per_s       = prop_between(&r, 10.0f, 1000.0f);
    g.p_tail_w           = prop_between(&r, 20.0f, 800.0f);
    g.t_tail_hold_s      = (uint16_t)prop_between(&r, 10.0f, 600.0f);
    g.t_vclamp_s         = (uint16_t)prop_between(&r, 2.0f, 30.0f);
    g.cv_hold_exit_min   = (uint16_t)prop_between(&r, 0.0f, 20.0f);
    g.t_charge_max_min   = (uint16_t)prop_between(&r, 60.0f, 1440.0f);
    g.warmup_time_s      = (uint16_t)prop_between(&r, 0.0f, 60.0f);
    g.limp_vcell         = prop_between(&r, 3.20f, 3.35f);
    g.limp_power_cap_w   = prop_between(&r, 100.0f, 4000.0f);
    g.rotor_rated_v      = prop_chance(&r, 0.15f) ? prop_between(&r, 6.0f, 24.0f) : 12.0f;
    g.rotor_v_max        = prop_chance(&r, 0.15f) ? prop_between(&r, 6.0f, 30.0f) : NAN;
    g.allow_full_field_48v = prop_chance(&r, 0.08f);   /* config may lift the clamp (§5.1) */
    g.skip_bulk_vcell    = prop_chance(&r, 0.3f) ? prop_between(&r, 3.30f, 3.45f) : 0.0f;
    g.soc_target_pct     = prop_chance(&r, 0.3f) ? (int8_t)prop_between(&r, 70.0f, 100.0f) : -1;

    p.cv_target_vcell    = prop_between(&r, 3.45f, 3.65f);
    p.rest_voltage_vcell = prop_between(&r, 3.33f, 3.45f);
    p.rest_power_cap_w   = prop_between(&r, 100.0f, 8000.0f);
    p.v_revert_vcell     = prop_between(&r, 3.20f, 3.40f);
    p.t_revert_hold_s    = (uint16_t)prop_between(&r, 10.0f, 600.0f);
    p.ah_revert          = prop_between(&r, 0.0f, 300.0f);
    p.rest_mode          = prop_chance(&r, 0.5f) ? CTRL_REST_ZERO : CTRL_REST_HOLD;
    p.exit_at_cv_entry   = prop_chance(&r, 0.25f);
    p.soc_revert_pct     = prop_chance(&r, 0.3f) ? (int8_t)prop_between(&r, 10.0f, 90.0f) : -1;

    /* ---- sensor channels ---- */
    fz_chan_t ch_vsup = {0}, ch_vbat = {0}, ch_amps = {0},
              ch_alt = {0}, ch_drv = {0}, ch_batt = {0}, ch_rpm = {0};
    const float vnoise = prop_between(&r, 0.0f, 0.03f);   /* supply-sense noise band */
    ch_vsup.noise = vnoise; ch_vbat.noise = vnoise;
    /* duty_max is rotor_v / V_reported, so a reading low by a fraction nf
     * loosens the clamp by 1/(1-nf) - 1, NOT by nf. Using nf here would fail on
     * ~1 tick in 10^6 for purely arithmetic reasons (observed: seed 0x1234ABCD
     * tick 97563, V_true 20.805, reported 20.258, ratio 1.0270 vs nf 0.0267). */
    const float vtol = vnoise / (1.0f - vnoise);
    ch_amps.noise = prop_between(&r, 0.0f, 0.20f);
    ch_rpm.noise  = prop_between(&r, 0.0f, 0.10f);

    /* ---- world state ---- *
     * Crude alternator feedback (NOT the §8.1 plant — just enough closed loop
     * that the inner loop can actually saturate, so the clamp gets exercised at
     * its boundary instead of idling at 10 % of it): full effort at cruise RPM
     * makes ~160 A, matching the 2026-07-24 stock reference trace (140 A at the
     * ~25 % clamp), and charge current pushes the bus up. */
    float bus_true   = prop_between(&r, 3.15f, 3.45f) * (float)cells;
    float house_a    = prop_between(&r, 0.0f, 260.0f);   /* load-dominated runs pin
                                                          * the inner loop at the
                                                          * clamp — the stress case */
    float i_true     = 0.0f;
    float engine_rpm = 1800.0f;
    float alt_c = 55.0f, drv_c = 45.0f, batt_c = 22.0f, soc = prop_between(&r, 20.0f, 95.0f);
    bool  ignition = true;

    ctrl_ceilings_t c = test_ceil_none();
    ctrl_t e; ctrl_init(&e);

    prop_ctx_t cx; memset(&cx, 0, sizeof cx);
    cx.tag = "fuzz"; cx.seed = seed;

    float max_ratio = 0.0f;
    uint32_t latched_since = 0u;

    for (uint64_t t = 0; t < FUZZ_TICKS; t++) {
        /* --- world evolves --- */
        const bool sensors_glitching = (ch_vsup.drop_left || ch_vsup.lie_left ||
                                        ch_vbat.drop_left || ch_vbat.lie_left);
        const float rpm_frac = engine_rpm > 1500.0f ? 1.0f : engine_rpm / 1500.0f;
        i_true = 160.0f * e.effort * rpm_frac - house_a;
        if (!sensors_glitching) {           /* see header note (2) */
            bus_true += prop_sym(&r) * 0.02f + i_true * 2.0e-5f;
            const float lo = 2.6f * (float)cells, hi = 3.85f * (float)cells;
            if (bus_true < lo) bus_true = lo;
            if (bus_true > hi) bus_true = hi;
        }
        if (prop_chance(&r, 0.0005f)) engine_rpm = prop_between(&r, 0.0f, 3500.0f);
        if (prop_chance(&r, 0.0002f)) ignition = !ignition;
        alt_c  += prop_sym(&r) * 0.05f + (e.effort - 0.25f) * 0.01f;
        drv_c  += prop_sym(&r) * 0.05f;
        batt_c += prop_sym(&r) * 0.01f;
        if (alt_c < -20.0f)  alt_c = -20.0f;
        if (alt_c > 140.0f)  alt_c = 140.0f;
        if (drv_c < -20.0f)  drv_c = -20.0f;
        if (drv_c > 140.0f)  drv_c = 140.0f;
        if (batt_c < -15.0f) batt_c = -15.0f;
        if (batt_c > 65.0f)  batt_c = 65.0f;
        soc += prop_sym(&r) * 0.02f;
        if (soc < 0.0f)      soc = 0.0f;
        if (soc > 100.0f)    soc = 100.0f;

        /* --- ceilings wander, including inactive/zero/negative --- */
        if (prop_chance(&r, 0.002f)) {
            c = test_ceil_none();
            if (prop_chance(&r, 0.5f)) c.bms_ccl_w   = prop_between(&r, -100.0f, 9000.0f);
            if (prop_chance(&r, 0.4f)) c.thermal_w   = prop_between(&r, 0.0f, 9000.0f);
            if (prop_chance(&r, 0.3f)) c.user_cap_w  = prop_between(&r, 0.0f, 3000.0f);
            if (prop_chance(&r, 0.2f)) c.belt_w      = prop_between(&r, 0.0f, 6000.0f);
            if (prop_chance(&r, 0.1f)) c.engine_w    = NAN;
        }

        /* --- sensors --- */
        ctrl_measured_t m; memset(&m, 0, sizeof m);
        m.v_supply_v   = fz_read(&ch_vsup, &r, bus_true, t, 1, 1000u);
        m.vbat_pack_v  = fz_read(&ch_vbat, &r, bus_true, t, 1, 1000u);
        m.vcomp_pack_v = m.vbat_pack_v;
        m.amps_batt    = fz_read(&ch_amps, &r, i_true, t, 1, 2000u);
        m.watts_batt   = m.amps_batt * bus_true;
        m.isrc         = (ctrl_isrc_tier_t)(1u + prop_below(&r, 5u));
        m.alt_hotspot_c = fz_read_abs(&ch_alt,  &r, alt_c,  1.0f);
        m.driver_temp_c = fz_read_abs(&ch_drv,  &r, drv_c,  1.0f);
        m.batt_temp_c   = fz_read_abs(&ch_batt, &r, batt_c, 0.5f);
        m.rpm          = fz_read(&ch_rpm, &r, engine_rpm, t, 1, 3000u);
        m.rpm_state    = (ctrl_rpm_state_t)prop_below(&r, 3u);
        m.run_state    = prop_chance(&r, 0.7f) ? CTRL_RUN_RUNNING : CTRL_RUN_NOT_RUNNING;
        m.soc_pct      = soc;
        m.soc_trusted  = prop_chance(&r, 0.5f);
        m.ignition     = ignition;
        m.feature_in   = prop_chance(&r, 0.1f);
        m.ext_faults   = 0u;
        if (prop_chance(&r, 0.0008f)) m.ext_faults |= CTRL_FAULT_LOST_BMS;
        if (prop_chance(&r, 0.0008f)) m.ext_faults |= CTRL_FAULT_IMPLAUSIBLE_SHUNT;
        if (prop_chance(&r, 0.0004f)) m.ext_faults |= CTRL_FAULT_FIELD_OPEN;
        if (prop_chance(&r, 0.0001f)) m.ext_faults |= CTRL_FAULT_FIELD_OVERCUR;  /* OPEN class */
        if (prop_chance(&r, 0.0001f)) m.ext_faults |= CTRL_FAULT_WATCHDOG;       /* OPEN class */

        cx.tick = t;
        cx.v_true = bus_true; cx.vbat = m.vbat_pack_v; cx.vsup = m.v_supply_v;
        cx.amps = m.amps_batt; cx.watts = m.watts_batt;
        cx.alt_c = m.alt_hotspot_c; cx.drv_c = m.driver_temp_c; cx.batt_c = m.batt_temp_c;
        cx.rpm = m.rpm; cx.isrc = (int)m.isrc; cx.rpm_state = (int)m.rpm_state;
        cx.run_state = (int)m.run_state; cx.ignition = ignition ? 1 : 0;
        cx.ext_faults = m.ext_faults;

        const ctrl_command_t cmd = ctrl_tick(&e, &m, &c, &p, &g, FUZZ_DT_MS);

        prop_core_check(iv, &cmd, &g, bus_true, vtol, &cx);
        prop_note(open_only_standby, !cmd.field_open || cmd.state == CTRL_STANDBY, &cx,
                  "field_open in state %d", (int)cmd.state);

        /* Field must never be commanded while the engine reports STANDBY. */
        prop_note(effort_settles,
                  cmd.state != CTRL_STANDBY || cmd.field_effort == 0.0f, &cx,
                  "STANDBY with effort %.6g", (double)cmd.field_effort);

        const float dmax = ctrl_duty_max(&g, bus_true);
        if (dmax > 0.0f && cmd.field_duty / dmax > max_ratio) max_ratio = cmd.field_duty / dmax;

        /* A latched OPEN fault parks the unit; reset after a while (as the app
         * would on power cycle) so the rest of the run keeps exercising logic. */
        if (cmd.faults & CTRL_FAULT_OPEN_MASK) {
            if (++latched_since > 300u) { ctrl_init(&e); latched_since = 0u; }
        } else {
            latched_since = 0u;
        }
    }

    *max_ratio_out = max_ratio;
    *ticks_out = FUZZ_TICKS;
}

void test_fuzz(void)
{
    prop_core_inv_t iv; prop_core_init(&iv);
    prop_inv_t open_only_standby, standby_zero;
    prop_inv_init(&open_only_standby, "FZ field_open only in STANDBY");
    prop_inv_init(&standby_zero,      "FZ STANDBY commands zero effort");

    printf("\n-- §8.4 randomized fault-injection runs (fixed seeds) --\n");

    const uint64_t points_before = g_prop_points;
    uint64_t total = 0;
    for (int i = 0; i < N_FUZZ_SEEDS; i++) {
        float ratio = 0.0f; uint64_t ticks = 0;
        fuzz_one(FUZZ_SEEDS[i], &iv, &open_only_standby, &standby_zero, &ratio, &ticks);
        total += ticks;
        printf("  seed 0x%08lX: %llu ticks, peak duty/duty_max(true V) = %.4f\n",
               (unsigned long)FUZZ_SEEDS[i], (unsigned long long)ticks, (double)ratio);
    }
    printf("  [FZ] %d seeds, %llu ticks, %llu invariant points\n", N_FUZZ_SEEDS,
           (unsigned long long)total, (unsigned long long)(g_prop_points - points_before));

    prop_core_report(&iv);
    prop_report(&open_only_standby);
    prop_report(&standby_zero);
}
