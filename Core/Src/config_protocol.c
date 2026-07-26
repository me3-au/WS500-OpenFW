/*
 * config_protocol.c — resolves globals + active profile the engine reads.
 * Defaults below are placeholders (profile 1 "Bulk, Float Norm", PROFILE_SPEC
 * §3/§4/§7), NOT tuned values; max_charge_power_w = 0 keeps output off until a
 * real config is loaded. They are also the FALLBACK: a persisted record only
 * displaces them if it survives both the store (magic/version/CRC/generation,
 * config_store.c) and ctrl_config_validate(). USB-CDC parse lands here next.
 * SPDX-License-Identifier: MIT
 */
#include "config_protocol.h"
#include "config_store_glue.h"
#include "config_validate.h"
#include <math.h>

static ctrl_globals_t s_g;
static ctrl_profile_t s_prof;
static ctrl_limits_t  s_lim;
static ctrl_thermal_cfg_t s_th;

/* Boot-time store result, kept for the §7 fault/telemetry surface. */
static cfg_store_status_t s_store_status = CFG_STORE_EMPTY;
static cfg_err_t          s_store_val_err = CFG_OK;

/*
 * Overlay a persisted config on top of the defaults above — belt and braces:
 * the store has already proved the record's framing and CRC, and this proves
 * its CONTENT against PROFILE_SPEC §1/§3 before a single field reaches the
 * engine. Any failure at all leaves the compiled defaults in place.
 */
static void config_load_persisted(void)
{
    ctrl_config_t stored;
    s_store_status  = config_store_load(&stored);
    s_store_val_err = CFG_OK;

    if (s_store_status != CFG_STORE_OK) return;   /* blank part or I/O — defaults stand */

    /* NULL out-param for now: nothing consumes the failing profile index yet.
     * The §7 telemetry surface below should ask for it — that is what turns
     * "config rejected" into "profile 4 rest voltage is not v_float_cons". */
    s_store_val_err = ctrl_config_validate(&stored, NULL);
    if (s_store_val_err != CFG_OK) return;        /* rejected, never repaired (§1) */

    s_g    = stored.globals;
    s_lim  = stored.limits;
    s_prof = stored.profiles[stored.active_profile - 1u].p;

    /* NOT persisted by schema v1: the thermal governor config (ctrl_thermal_cfg_t
     * lives in thermal.h, and PROFILE_SPEC §7 has no thermal block) keeps its
     * placeholders below. Adding it is a schema_version bump. */
}

void config_init(void)
{
    /* Globals (PROFILE_SPEC §3.1 defaults; 12 V / 4S placeholder). */
    s_g.cells_series      = 4;
    s_g.bank_capacity_ah  = 100.0f;
    s_g.max_charge_power_w = 0.0f;   /* 0 until configured — engine stays off */
    s_g.ramp_w_per_s      = 100.0f;
    s_g.p_tail_w          = 0.0f;
    s_g.t_tail_hold_s     = 60;
    s_g.t_vclamp_s        = 5;
    s_g.cv_hold_exit_min  = 15;   /* held at CV 15 min → full (voltage+time) */
    s_g.t_charge_max_min  = 480;
    s_g.warmup_time_s     = 30;
    s_g.warmup_coolant_c  = NAN;
    s_g.soc_target_pct    = -1;
    s_g.skip_bulk_vcell   = 0.0f;   /* off by default (enable to skip absorb when full) */
    s_g.skip_bulk_soc_pct = -1;     /* off */
    s_g.rotor_rated_v     = 12.0f;
    s_g.rotor_v_max       = NAN;
    s_g.allow_full_field_48v = false;
    s_g.limp_vcell        = 3.30f;   /* v_limp (PROFILE_SPEC §1) */
    s_g.limp_power_cap_w  = 250.0f;  /* placeholder reduced cap */

    /* Active profile 1 — Bulk, Float Norm (PROFILE_SPEC §4.1). */
    s_prof.id                = 1;
    s_prof.cv_target_vcell   = 3.60f;
    s_prof.exit_at_cv_entry  = false;
    s_prof.rest_mode         = CTRL_REST_HOLD;
    s_prof.rest_voltage_vcell= 3.40f;
    s_prof.rest_power_cap_w  = 0.0f;   /* resolved from %max later */
    s_prof.v_revert_vcell    = 3.28f;
    s_prof.t_revert_hold_s   = 30;
    s_prof.soc_revert_pct    = 50;
    s_prof.ah_revert         = 30.0f;  /* 0.30 C of 100 Ah */

    /* Hardware limit set (CONTROL_SPEC §2.1) — placeholders, commissioned per install. */
    s_lim.battery_c_limit    = 0.5f;   /* 0.5 C default LFP */
    s_lim.wiring_limit_a     = 0.0f;   /* unset */
    s_lim.alternator_limit_a = 0.0f;   /* unset */

    /* Thermal governor (§4) — placeholders; mount/commissioning refine these. */
    s_th.tau_s          = 300.0f;
    s_th.target_c       = 95.0f;
    s_th.hard_c         = 110.0f;
    s_th.derate_floor_w = 100.0f;
    s_th.ceiling_max_w  = 100000.0f;   /* effectively unbound until temp climbs */
    s_th.gain_w_per_c_s = 50.0f;

    /* Persisted config wins over the placeholders above when it is both intact
     * and valid.
     * TODO(§7 / module #21): a rejected or unreadable store is currently silent
     * — s_store_status / s_store_val_err below are the hookup point for the
     * fault + telemetry surface (INFO "using defaults", WARN "config rejected:
     * <cfg_err_str>"), which does not exist yet. */
    config_load_persisted();
}

void config_poll(void) { /* TODO: USB CDC lines → parse/validate/apply. */ }

cfg_store_status_t config_store_status(void)       { return s_store_status; }
cfg_err_t          config_store_validate_err(void) { return s_store_val_err; }

void config_get(ctrl_globals_t *g, ctrl_profile_t *prof) { *g = s_g; *prof = s_prof; }
void config_get_limits(ctrl_limits_t *lim) { *lim = s_lim; }
void config_get_thermal(ctrl_thermal_cfg_t *th) { *th = s_th; }
