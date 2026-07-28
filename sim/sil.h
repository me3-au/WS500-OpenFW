/*
 * sil.h — §8.1 SIL harness: plant + pure control core, closed loop.
 *
 * Mirrors the app orchestration in Core/Src/main.c: read (simulated) sensors →
 * assemble ctrl_measured_t → limits + thermal governor ceilings → ctrl_tick()
 * at the designed 10 ms rate → apply the commanded field duty back to the
 * plant. Accelerated time = plain loop iterations (no wall clock anywhere).
 *
 * Every sil_step() also checks the standing invariants — most importantly the
 * #1 safety property of this install (PROJECT_PLAN §5): commanded field duty
 * must NEVER exceed the rotor clamp (≈25 % at 48 V, ≈21 % at 57.6 V).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_SIM_SIL_H
#define WS500_SIM_SIL_H

#include "plant.h"
#include "control.h"
#include "limits.h"
#include "thermal.h"

#define SIL_DT_MS  10u   /* the core's designed tick (Core/Src/main.c LOOP_PERIOD_MS) */

typedef struct {
    /* Plant + injection */
    plant_t        plant;
    plant_inject_t inj;

    /* Control core + app-side subsystems (as in main.c) */
    ctrl_t             ctrl;
    ctrl_thermal_t     thermal;
    ctrl_thermal_cfg_t thcfg;
    ctrl_globals_t     g;
    ctrl_profile_t     prof;
    ctrl_limits_t      lim;

    /* Scenario-driven world inputs */
    float engine_rpm;      /* crank RPM (0 = stopped) */
    float house_load_w;    /* DC loads on the bus */
    bool  ignition;
    ctrl_isrc_tier_t isrc; /* shunt tier fed to the core */
    bool  feed_soc;        /* feed plant SOC as trusted SOC */
    float bms_ccl_w;       /* BMS ceiling (INACTIVE = none) */
    float bms_cvl_vcell;   /* deliverable #10: BMS charge-voltage limit,
                            * V/CELL (NAN = none/inactive) — scenarios set
                            * this directly rather than routing through
                            * bms_rx.c, matching how bms_ccl_w above is
                            * already fed straight in as a ceiling */
    bool  pre_disconnect;  /* deliverable #10: BMS pre-disconnect warning */
    float user_cap_w;
    float engine_w;
    uint32_t ext_faults_extra;  /* extra app-detected faults OR'd in */

    /* Last cycle results */
    ctrl_measured_t meas;
    ctrl_command_t  cmd;
    float           applied_duty;
    uint64_t        ticks;

    /* Invariant accounting */
    bool     inv_skip_clamp;    /* for allow_full_field sanity sub-runs */
    uint32_t inv_violations;
    float    max_duty_seen;
    float    max_ifield_seen;
    float    max_rotor_w_seen;
} sil_t;

/* Initialize with the installed-unit defaults (16S / 300 Ah / 4 Ω rotor,
 * profile 1, spec-default globals, hardware limit set, thermal governor). */
void sil_init(sil_t *s, uint32_t seed, float soc0);

void sil_step(sil_t *s);                     /* one 10 ms tick + invariants */
void sil_run_ticks(sil_t *s, uint32_t n);
void sil_run_s(sil_t *s, float sim_seconds);
void sil_reset_maxima(sil_t *s);

#endif /* WS500_SIM_SIL_H */
