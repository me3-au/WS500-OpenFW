/*
 * plant.c — SIL plant model (battery + alternator + rotor + sensors).
 *
 * Calibration anchor (2026-07-24 stock charge run, ws500_chargerun.log):
 *   25 % field duty @ ~2330 engine RPM → ~140 A / ~7.6 kW into the 48 V bank
 *   at ~54.7 V battery. At 54.7 V, 25 % duty puts I_f = 0.25×54.7/4 ≈ 3.42 A
 *   through the 4 Ω rotor. Constants below reproduce that point:
 *     E = k_e × tanh(I_f/3.0) × rpm_alt, rpm_alt = 2330 × 2.5 = 5825
 *     E = 54.7 + 140×R_s  with R_s = 0.040 → E = 60.30
 *     k_e = 60.30 / (tanh(3.419/3.0) × 5825) = 0.012713
 *   Battery: OCV(SOC≈0.54) ≈ 3.314 V/cell → 53.02 V pack; 140 A × 12 mΩ
 *   internal+wiring = 1.68 V rise → 54.70 V terminal. Self-consistent.
 *
 * 100 % duty on this install ≈ 12–14 A field ≈ 580+ W into a ~3 A-rated
 * winding — the §5 rotor-overdrive hazard the clamp scenarios verify against.
 *
 * SPDX-License-Identifier: MIT
 */
#include "plant.h"

/* ---- LFP OCV curve (V/cell vs SOC) — flat plateau, steep knees ------------ */
float plant_ocv_cell(float soc)
{
    static const float S[] = { 0.00f, 0.02f, 0.05f, 0.10f, 0.20f, 0.40f,
                               0.70f, 0.90f, 0.97f, 1.00f, 1.03f };
    static const float V[] = { 2.50f, 3.00f, 3.15f, 3.22f, 3.27f, 3.30f,
                               3.33f, 3.35f, 3.40f, 3.45f, 3.80f };
    const int n = (int)(sizeof(S) / sizeof(S[0]));

    if (soc <= S[0]) return V[0];
    if (soc >= S[n - 1]) {
        /* Overcharge wall: keep climbing steeply, clamped for sanity. */
        float v = V[n - 1] + (soc - S[n - 1]) * 11.67f;
        return v > 4.20f ? 4.20f : v;
    }
    for (int i = 1; i < n; i++) {
        if (soc <= S[i]) {
            const float f = (soc - S[i - 1]) / (S[i] - S[i - 1]);
            return V[i - 1] + f * (V[i] - V[i - 1]);
        }
    }
    return V[n - 1];  /* unreachable */
}

/* ---- Defaults: the real installed unit ------------------------------------ */
void plant_cfg_default(plant_cfg_t *cfg)
{
    cfg->cells_series      = 16;
    cfg->capacity_ah       = 300.0f;
    cfg->r_int_ohm         = 0.012f;      /* pack + charge-path wiring */
    cfg->batt_heat_j_per_k = 30000.0f;
    cfg->batt_rth_k_per_w  = 0.04f;

    cfg->pulley_ratio      = 2.5f;
    cfg->k_e               = 0.012713f;   /* calibrated — see file header */
    cfg->i_field_sat_a     = 3.0f;
    cfg->r_stator_ohm      = 0.040f;
    cfg->i_rect_max_a      = 250.0f;

    cfg->alt_heat_j_per_k  = 5170.0f;     /* τ ≈ 300 s with rth below */
    cfg->alt_rth_k_per_w   = 0.058f;
    cfg->alt_loss_w_per_a  = 8.0f;        /* 140 A → ~1.1 kW modeled heat */

    cfg->rotor_r_ohm       = 4.0f;
    cfg->rotor_rated_v     = 12.0f;
    cfg->rotor_rated_a     = 3.0f;
    cfg->rotor_heat_j_per_k = 400.0f;
    cfg->rotor_rth_k_per_w  = 0.15f;

    cfg->drv_k_c_per_a2    = 1.7f;        /* FET 31→45 °C @ I_f≈3.4 A (trace) */
    cfg->drv_tau_s         = 60.0f;

    cfg->ambient_c         = 25.0f;
}

void plant_init(plant_t *p, const plant_cfg_t *cfg, float soc0)
{
    plant_cfg_t c = *cfg;      /* self-copy safe (p->cfg may alias *cfg) */
    p->cfg = c;
    p->soc = soc0;
    p->vbus_v = plant_ocv_cell(soc0) * (float)c.cells_series;
    p->i_field_a = 0.0f;
    p->p_rotor_w = 0.0f;
    p->i_alt_a = 0.0f;
    p->i_batt_a = 0.0f;
    p->batt_temp_c  = c.ambient_c;
    p->alt_temp_c   = c.ambient_c;
    p->rotor_temp_c = c.ambient_c;
    p->driver_temp_c = c.ambient_c;
    p->charge_ah = 0.0;
}

void plant_step(plant_t *p, float field_duty, float engine_rpm,
                float house_load_w, float dt_s)
{
    const plant_cfg_t *c = &p->cfg;

    if (!(field_duty >= 0.0f)) field_duty = 0.0f;   /* NaN/negative → 0 */
    if (field_duty > 1.0f)     field_duty = 1.0f;
    if (!(engine_rpm >= 0.0f)) engine_rpm = 0.0f;

    const float rpm_alt  = engine_rpm * c->pulley_ratio;
    const float ocv_pack = plant_ocv_cell(p->soc) * (float)c->cells_series;

    /* Solve the bus algebraically (fixed-point, 3 iterations — converges fast
     * because R_stator+R_int is small vs the voltage scale). */
    float v = (p->vbus_v > 1.0f) ? p->vbus_v : ocv_pack;
    float i_f = 0.0f, i_alt = 0.0f, i_load = 0.0f;
    for (int it = 0; it < 3; it++) {
        i_f = field_duty * v / c->rotor_r_ohm;               /* averaged L/R */
        const float phi = tanhf(i_f / c->i_field_sat_a);     /* flux saturation */
        const float e   = c->k_e * phi * rpm_alt;            /* internal EMF */
        i_load = (v > 1.0f) ? house_load_w / v : 0.0f;

        i_alt = (e - ocv_pack + i_load * c->r_int_ohm)
                / (c->r_stator_ohm + c->r_int_ohm);
        if (i_alt < 0.0f) i_alt = 0.0f;                      /* diodes block */
        if (i_alt > c->i_rect_max_a) i_alt = c->i_rect_max_a;

        v = ocv_pack + (i_alt - i_load) * c->r_int_ohm;
        if (v < 1.0f) v = 1.0f;
    }

    p->vbus_v    = v;
    p->i_field_a = i_f;
    p->p_rotor_w = i_f * i_f * c->rotor_r_ohm;
    p->i_alt_a   = i_alt;
    p->i_batt_a  = i_alt - i_load;

    /* Coulomb counting. */
    const float dah = p->i_batt_a * dt_s / 3600.0f;
    p->soc += dah / c->capacity_ah;
    p->charge_ah += (double)dah;
    if (p->soc < -0.05f) p->soc = -0.05f;

    /* Thermal masses (first-order). */
    const float amb = c->ambient_c;

    const float p_batt = p->i_batt_a * p->i_batt_a * c->r_int_ohm * 0.5f;
    p->batt_temp_c += (p_batt - (p->batt_temp_c - amb) / c->batt_rth_k_per_w)
                      / c->batt_heat_j_per_k * dt_s;

    const float p_alt = c->alt_loss_w_per_a * i_alt + (rpm_alt > 500.0f ? 50.0f : 0.0f);
    p->alt_temp_c += (p_alt - (p->alt_temp_c - amb) / c->alt_rth_k_per_w)
                     / c->alt_heat_j_per_k * dt_s;

    p->rotor_temp_c += (p->p_rotor_w - (p->rotor_temp_c - amb) / c->rotor_rth_k_per_w)
                       / c->rotor_heat_j_per_k * dt_s;

    const float drv_target = amb + c->drv_k_c_per_a2 * i_f * i_f;
    p->driver_temp_c += (drv_target - p->driver_temp_c) * (dt_s / c->drv_tau_s);
}

/* ---- Sensor layer (deterministic) ----------------------------------------- */
void plant_inject_init(plant_inject_t *inj, uint32_t seed)
{
    inj->rng = seed ? seed : 1u;
    for (int i = 0; i < PS_COUNT; i++) {
        inj->noise[i] = 0.0f;
        inj->dropout[i] = false;
        inj->override_en[i] = false;
        inj->override_v[i] = 0.0f;
    }
}

float plant_rng_uniform(plant_inject_t *inj)
{
    inj->rng = inj->rng * 1664525u + 1013904223u;             /* LCG */
    return ((float)(inj->rng >> 8) / 8388608.0f) - 1.0f;      /* [-1, 1) */
}

/* mul=true → multiplicative noise (fractional); false → additive (absolute). */
static float sense_ch(plant_inject_t *inj, int ch, float truth, bool mul)
{
    if (inj->dropout[ch])     return NAN;
    if (inj->override_en[ch]) return inj->override_v[ch];
    const float u = plant_rng_uniform(inj);
    return mul ? truth * (1.0f + inj->noise[ch] * u)
               : truth + inj->noise[ch] * u;
}

void plant_sense(const plant_t *p, plant_inject_t *inj, float engine_rpm,
                 plant_sensed_t *out)
{
    out->vbat_v        = sense_ch(inj, PS_VBAT,  p->vbus_v,       true);
    out->amps_batt_a   = sense_ch(inj, PS_AMPS,  p->i_batt_a,     true);
    out->alt_temp_c    = sense_ch(inj, PS_ALT_T, p->alt_temp_c,   false);
    out->batt_temp_c   = sense_ch(inj, PS_BATT_T, p->batt_temp_c, false);
    out->driver_temp_c = sense_ch(inj, PS_DRV_T, p->driver_temp_c, false);
    out->rpm           = sense_ch(inj, PS_RPM,   engine_rpm,      true);
}
