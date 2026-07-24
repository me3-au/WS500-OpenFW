/*
 * plant.h — SIL plant model for the §8.1 virtual gauntlet (PROJECT_PLAN §8).
 *
 * Pure C11, no HAL, no dynamic allocation. Models the *installed* system the
 * one WS500 lives on (PROJECT_PLAN §5): 16S LFP 48 V bank, alternator with a
 * 4 Ω 12 V-class rotor on the 48–57.6 V bus, belt/pulley ratio, house loads.
 *
 * Calibrated against the 2026-07-24 stock-firmware charge run
 * (ws500_chargerun.log): 25 % field @ ~2330 engine RPM → ~140 A / ~7.6 kW at
 * ~54.7 V battery, FET temp 31→45 °C. See plant_cfg_default() and the
 * scn_plant_calibration scenario.
 *
 * The sensor layer supports deterministic (fixed-seed) noise, dropout (NaN)
 * and implausible-value injection per channel — the control core must stay
 * safe under all of it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_SIM_PLANT_H
#define WS500_SIM_PLANT_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>

/* ---- Configuration -------------------------------------------------------- */
typedef struct {
    /* Battery: 16S LFP */
    uint8_t cells_series;       /* 16 for the 48 V install */
    float   capacity_ah;        /* bank capacity */
    float   r_int_ohm;          /* pack internal + wiring resistance (charge path) */
    float   batt_heat_j_per_k;  /* battery thermal mass */
    float   batt_rth_k_per_w;   /* battery→ambient thermal resistance */

    /* Alternator (electrical) */
    float   pulley_ratio;       /* alt RPM = engine RPM × ratio */
    float   k_e;                /* EMF: E = k_e × phi(I_f) × rpm_alt  [V per rpm] */
    float   i_field_sat_a;      /* flux saturation knee: phi = tanh(I_f / this) */
    float   r_stator_ohm;       /* effective source resistance (stator + rectifier) */
    float   i_rect_max_a;       /* rectifier/stator hard current limit */

    /* Alternator (thermal) */
    float   alt_heat_j_per_k;
    float   alt_rth_k_per_w;
    float   alt_loss_w_per_a;   /* modeled heat per output amp (copper+iron+diode) */

    /* Rotor / field circuit: 4 Ω 12 V-class winding on the 48 V bus.
     * Freewheel diodes + long L/R smooth the current, so the average model
     * I_f = duty × V_bus / R is physically valid (PROJECT_PLAN §0.5). */
    float   rotor_r_ohm;        /* 4.0 on this install */
    float   rotor_rated_v;      /* 12 V class */
    float   rotor_rated_a;      /* ~3 A continuous winding rating */
    float   rotor_heat_j_per_k;
    float   rotor_rth_k_per_w;

    /* Regulator driver stage (the WS500-internal FET NTC, PA3/β3380) */
    float   drv_k_c_per_a2;     /* steady ΔT ≈ this × I_f² */
    float   drv_tau_s;

    float   ambient_c;
} plant_cfg_t;

/* ---- State ---------------------------------------------------------------- */
typedef struct {
    plant_cfg_t cfg;

    float  soc;            /* 0..1 (may exceed 1 slightly — OCV wall handles V) */
    float  vbus_v;         /* battery terminal / bus voltage (true value) */
    float  i_field_a;      /* average rotor current */
    float  p_rotor_w;      /* rotor dissipation I_f²·R */
    float  i_alt_a;        /* alternator output current */
    float  i_batt_a;       /* battery current (+ = charging) */

    float  batt_temp_c;
    float  alt_temp_c;
    float  rotor_temp_c;
    float  driver_temp_c;

    double charge_ah;      /* cumulative coulomb count (diagnostic) */
} plant_t;

/* ---- Sensor layer --------------------------------------------------------- */
enum {
    PS_VBAT = 0,   /* bus/battery voltage  (noise: fractional) */
    PS_AMPS,       /* battery current      (noise: fractional) */
    PS_ALT_T,      /* alternator NTC       (noise: absolute °C) */
    PS_BATT_T,     /* battery NTC          (noise: absolute °C) */
    PS_DRV_T,      /* driver-stage NTC     (noise: absolute °C) */
    PS_RPM,        /* fused engine RPM     (noise: fractional) */
    PS_COUNT
};

typedef struct {
    uint32_t rng;                    /* LCG state — seed fixed per scenario */
    float    noise[PS_COUNT];        /* 0 = clean */
    bool     dropout[PS_COUNT];      /* true → channel reads NaN */
    bool     override_en[PS_COUNT];  /* true → channel reads override value */
    float    override_v[PS_COUNT];   /* implausible-value injection */
} plant_inject_t;

typedef struct {
    float vbat_v;
    float amps_batt_a;
    float alt_temp_c;
    float batt_temp_c;
    float driver_temp_c;
    float rpm;
} plant_sensed_t;

/* ---- API ------------------------------------------------------------------ */
/* Default config = the real installed unit, calibrated to the reference trace. */
void plant_cfg_default(plant_cfg_t *cfg);

void plant_init(plant_t *p, const plant_cfg_t *cfg, float soc0);

/* Advance the physics one step. field_duty ∈ [0,1] as applied to the field
 * switch; engine_rpm at the crank; house_load_w = DC loads on the bus. */
void plant_step(plant_t *p, float field_duty, float engine_rpm,
                float house_load_w, float dt_s);

/* Per-cell open-circuit voltage vs SOC (flat LFP knee shape). */
float plant_ocv_cell(float soc);

void  plant_inject_init(plant_inject_t *inj, uint32_t seed);
float plant_rng_uniform(plant_inject_t *inj);   /* deterministic, in [-1, 1] */

/* Read the sensors through the injection layer. */
void plant_sense(const plant_t *p, plant_inject_t *inj, float engine_rpm,
                 plant_sensed_t *out);

#endif /* WS500_SIM_PLANT_H */
