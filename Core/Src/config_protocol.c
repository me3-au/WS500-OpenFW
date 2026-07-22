/*
 * config_protocol.c — stub. Resolves globals + active profile the engine reads.
 * Defaults below are placeholders (profile 1 "Bulk, Float Norm", PROFILE_SPEC
 * §3/§4/§7), NOT tuned values; max_charge_power_w = 0 keeps output off until a
 * real config is loaded. Flash load + USB-CDC parse land here next.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config_protocol.h"
#include <math.h>

static ctrl_globals_t s_g;
static ctrl_profile_t s_prof;

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
    s_g.t_charge_max_min  = 480;
    s_g.warmup_time_s     = 30;
    s_g.warmup_coolant_c  = NAN;
    s_g.soc_target_pct    = -1;
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
}

void config_poll(void) { /* TODO: USB CDC lines → parse/validate/apply. */ }

void config_get(ctrl_globals_t *g, ctrl_profile_t *prof) { *g = s_g; *prof = s_prof; }
