/*
 * config_protocol.c — the app's config owner: compiled defaults, the persisted
 * record, and the USB-CDC JSON-lines surface that lets a client read and write
 * them (GH#16, deliverable #6).
 *
 * It holds ONE ctrl_config_t (the whole PROFILE_SPEC §7 model) and publishes
 * the slice the engine consumes — globals + the resolved active profile +
 * the §2.1 limit set. That is why the full config lives here rather than in the
 * protocol layer: cfg-get has to be able to show the seven profiles, not just
 * the one the engine happens to be running.
 *
 * Defaults below are placeholders (profile 1 "Bulk, Float Norm", PROFILE_SPEC
 * §3/§4/§7), NOT tuned values; max_charge_power_w = 0 keeps output off until a
 * real config is loaded. They are also the FALLBACK: a persisted record only
 * displaces them if it survives both the store (magic/version/CRC/generation,
 * config_store.c) and ctrl_config_validate().
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_protocol.h"
#include "config_store_glue.h"
#include "config_validate.h"
#include "config_msg.h"
#include "cfg_stream.h"
#include "telem_stream.h"
#include "version.h"
#include <math.h>

/* The whole model. Everything else in this file is a view of it. */
static ctrl_config_t s_cfg;

/* Published views the engine/app read each tick. */
static ctrl_globals_t s_g;
static ctrl_profile_t s_prof;
static ctrl_limits_t  s_lim;
static ctrl_thermal_cfg_t s_th;

/* JSON-lines protocol instance (~3.7 KB of the part's 16 KB, dominated by the
 * CFG_MSG_LINE_MAX request buffer — see config_msg.h for why that size). */
static cfg_msg_t s_msg;

/* Boot-time store result, kept for the §7 fault/telemetry surface. */
static cfg_store_status_t s_store_status = CFG_STORE_EMPTY;
static cfg_err_t          s_store_val_err = CFG_OK;

/* Derive the engine-facing views from s_cfg. Called after every change to the
 * config, and only from here — the engine must never see a half-applied edit. */
static void config_publish(void)
{
    s_g   = s_cfg.globals;
    s_lim = s_cfg.limits;

    const uint8_t idx = (s_cfg.active_profile >= 1u &&
                         s_cfg.active_profile <= CFG_PROFILE_COUNT)
                        ? (uint8_t)(s_cfg.active_profile - 1u) : 0u;
    s_prof = s_cfg.profiles[idx].p;
}

/* One §4.1 profile slot. rest_cap_pct is the spec's "% max" column, resolved
 * against max_charge_power_w here so the stored value is explicit Watts (the
 * same materialize-at-config-time rule §3.2 sets for p_tail_w). */
static void config_default_profile(cfg_profile_t *pr, uint8_t id,
                                   cfg_prim_ref_t cv, bool solar_finish,
                                   ctrl_rest_mode_t mode, cfg_prim_ref_t rest,
                                   float rest_cap_pct, float v_revert,
                                   uint16_t revert_hold_s, int8_t soc_revert)
{
    pr->flags               = 0u;
    pr->cv_target_ref       = cv;
    pr->rest_voltage_ref    = rest;
    pr->p.id                = id;
    pr->p.cv_target_vcell   = 0.0f;      /* filled by cfg_resolve_primitives() */
    pr->p.exit_at_cv_entry  = solar_finish;
    pr->p.rest_mode         = mode;
    pr->p.rest_voltage_vcell = 0.0f;     /* ditto */
    pr->p.rest_power_cap_w  = rest_cap_pct * s_cfg.globals.max_charge_power_w;
    pr->p.v_revert_vcell    = v_revert;
    pr->p.t_revert_hold_s   = revert_hold_s;
    pr->p.soc_revert_pct    = soc_revert;
    pr->p.ah_revert         = 0.30f * s_cfg.globals.bank_capacity_ah;  /* §4.1 */
}

static void config_defaults(void)
{
    /* §1 voltage primitives — the five names every profile references. */
    s_cfg.prim.v_bulk       = 3.60f;
    s_cfg.prim.v_bulk_cons  = 3.50f;
    s_cfg.prim.v_float      = 3.40f;
    s_cfg.prim.v_float_cons = 3.35f;
    s_cfg.prim.v_limp       = 3.30f;

    /* Globals (PROFILE_SPEC §3.1 defaults; 12 V / 4S placeholder). */
    ctrl_globals_t *g = &s_cfg.globals;
    g->cells_series      = 4;
    g->bank_capacity_ah  = 100.0f;
    g->max_charge_power_w = 0.0f;   /* 0 until configured — engine stays off */
    g->ramp_w_per_s      = 100.0f;
    g->p_tail_w          = 0.0f;
    g->t_tail_hold_s     = 60;
    g->t_vclamp_s        = 5;
    g->cv_hold_exit_min  = 15;   /* held at CV 15 min → full (voltage+time) */
    g->t_charge_max_min  = 480;
    g->warmup_time_s     = 30;
    g->warmup_coolant_c  = NAN;
    g->soc_target_pct    = -1;
    g->skip_bulk_vcell   = 0.0f;    /* off by default (enable to skip absorb when full) */
    g->skip_bulk_soc_pct = -1;      /* off */
    g->rotor_rated_v     = 12.0f;
    g->rotor_v_max       = NAN;
    g->allow_full_field_48v = false;
    g->limp_vcell        = 3.30f;   /* v_limp (PROFILE_SPEC §1) */
    g->limp_power_cap_w  = 250.0f;  /* placeholder reduced cap */

    /* GH#40: battery-temp charge-window gate. MUST default to NONE/false —
     * see control.h's ctrl_batt_temp_src_t doc comment for why shipping a
     * guessed channel binding is worse than the gap it closes. */
    g->batt_temp_src     = CTRL_BATT_TEMP_NONE;
    g->require_batt_temp = false;

    /* Hardware limit set (CONTROL_SPEC §2.1) — placeholders, commissioned per install. */
    s_cfg.limits.battery_c_limit    = 0.5f;   /* 0.5 C default LFP */
    s_cfg.limits.wiring_limit_a     = 0.0f;   /* unset */
    s_cfg.limits.alternator_limit_a = 0.0f;   /* unset */

    /* The seven §4.1 profiles. Every rest cap resolves to 0 W while
     * max_charge_power_w is 0 — deliberate: the placeholder config must not be
     * able to command output. */
    config_default_profile(&s_cfg.profiles[0], 1, CFG_PRIM_V_BULK,      false,
                           CTRL_REST_HOLD, CFG_PRIM_V_FLOAT,      1.00f, 3.28f, 30, 50);
    config_default_profile(&s_cfg.profiles[1], 2, CFG_PRIM_V_BULK,      false,
                           CTRL_REST_HOLD, CFG_PRIM_V_FLOAT_CONS, 0.25f, 3.22f, 60, 40);
    config_default_profile(&s_cfg.profiles[2], 3, CFG_PRIM_V_BULK,      true,
                           CTRL_REST_ZERO, CFG_PRIM_NONE,         0.00f, 3.25f, 60, 50);
    config_default_profile(&s_cfg.profiles[3], 4, CFG_PRIM_V_BULK_CONS, false,
                           CTRL_REST_HOLD, CFG_PRIM_V_FLOAT_CONS, 0.50f, 3.25f, 30, 50);
    config_default_profile(&s_cfg.profiles[4], 5, CFG_PRIM_V_BULK,      false,
                           CTRL_REST_ZERO, CFG_PRIM_NONE,         0.00f, 3.25f, 30, 50);
    /* 6 — Limp Home: FLOAT at v_limp directly, no Bulk, no exits ("—" in §4.1). */
    config_default_profile(&s_cfg.profiles[5], 6, CFG_PRIM_V_LIMP,      false,
                           CTRL_REST_HOLD, CFG_PRIM_V_LIMP,       0.25f, 0.0f,  0, -1);
    /* 7 — Fast Charge: schema slot reserved now, curve-based later. */
    config_default_profile(&s_cfg.profiles[6], 7, CFG_PRIM_NONE,        false,
                           CTRL_REST_ZERO, CFG_PRIM_NONE,         0.00f, 0.0f,  0, -1);
    s_cfg.profiles[6].flags = CFG_PROF_FLAG_RESERVED;

    s_cfg.active_profile = 1;

    cfg_resolve_primitives(&s_cfg);   /* §1 propagation into the profile table */
}

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
     * "config rejected" into "profile 4 rest voltage is not v_float_cons".
     * (The JSON surface already does exactly that, see config_msg.c.) */
    s_store_val_err = ctrl_config_validate(&stored, NULL);
    if (s_store_val_err != CFG_OK) return;        /* rejected, never repaired (§1) */

    s_cfg = stored;
    config_publish();

    /* NOT persisted by schema v1: the thermal governor config (ctrl_thermal_cfg_t
     * lives in thermal.h, and PROFILE_SPEC §7 has no thermal block) keeps its
     * placeholders below. Adding it is a schema_version bump. */
}

/* ---- JSON protocol host ----------------------------------------------------
 * The three hooks control/Src/config_msg.c needs. They are the ONLY way the
 * protocol can reach the config or the EEPROM, which is what keeps the whole
 * message layer natively testable against an in-RAM store. */

static const ctrl_config_t *config_host_get(void *ctx)
{
    (void)ctx;
    return &s_cfg;
}

/*
 * Persist FIRST, adopt second. cfg_msg has already validated this config, so
 * the only remaining question is whether the EEPROM took it — and a config the
 * regulator is running that did not survive the write would come back as
 * something else after the next power cycle. Store-then-publish means a failed
 * write leaves both the record and the running config untouched.
 */
static bool config_host_save(void *ctx, const ctrl_config_t *cfg, uint32_t *generation)
{
    (void)ctx;
    if (config_store_save(cfg) != CFG_STORE_OK) return false;

    s_cfg = *cfg;
    config_publish();
    s_store_status  = CFG_STORE_OK;
    s_store_val_err = CFG_OK;
    if (generation != NULL) *generation = config_store_generation();
    return true;
}

static void config_host_tx(void *ctx, const char *data, int len)
{
    (void)ctx;
    cfg_proto_tx((const uint8_t *)data, len);
}

/* {"t":"telem-get"} → one snapshot on demand (GH#35, #20). Binding this hook
 * is also what makes the hello reply advertise "telem" (config_msg.h). */
static void config_host_telem(void *ctx)
{
    (void)ctx;
    telem_stream_send_now();
}

void config_init(void)
{
    config_defaults();
    config_publish();

    /* Thermal governor (§4) — placeholders; mount/commissioning refine these. */
    s_th.tau_s          = 300.0f;
    s_th.target_c       = 95.0f;
    s_th.hard_c         = 110.0f;
    s_th.derate_floor_w = 100.0f;
    s_th.ceiling_max_w  = 100000.0f;   /* effectively unbound until temp climbs */
    s_th.gain_w_per_c_s = 50.0f;

    /* Persisted config wins over the placeholders above when it is both intact
     * and valid.
     * TODO(GH#35): a rejected or unreadable store is currently silent
     * — s_store_status / s_store_val_err below are the hookup point for the
     * fault + telemetry surface (INFO "using defaults", WARN "config rejected:
     * <cfg_err_str>"). The telemetry `diag` object is where it belongs; it is
     * NOT a control fault (ERRB_EEPROM maps to none — see main.c for why a
     * dead config store cannot make charging unsafe). */
    config_load_persisted();

    const cfg_msg_host_t host = {
        .get     = config_host_get,
        .save    = config_host_save,
        .ctx     = NULL,
        .tx      = config_host_tx,
        .tx_ctx  = NULL,
        .fw      = FW_VERSION_STRING,
        .git     = FW_GIT_HASH,        /* empty unless the build supplied one */
        .telem   = config_host_telem,  /* advertises the "telem" cap (GH#35) */
        .telem_ctx = NULL,
    };
    cfg_msg_init(&s_msg, &host);
}

/*
 * Service inbound config traffic. Bounded work per call: at most
 * CONFIG_POLL_CHUNKS reads, so a client streaming a large document spreads
 * across loop iterations instead of monopolising one.
 *
 * TODO(GH#16): a cfg-set that reaches the store BLOCKS for the 24C16's write
 * cycle (tens of ms, config_store.h), which overruns one 10 ms control tick.
 * That is survivable today — the main loop's deadline scheduler catches up and
 * the field command is unchanged across the stall — but the right answer is a
 * deferred/chunked save once there is a place to defer it to.
 */
#define CONFIG_POLL_CHUNKS  4
#define CONFIG_POLL_BYTES   64

void config_poll(void)
{
    uint8_t buf[CONFIG_POLL_BYTES];
    for (int i = 0; i < CONFIG_POLL_CHUNKS; i++) {
        const int n = cfg_proto_rx(buf, (int)sizeof buf);
        if (n <= 0) break;
        cfg_msg_rx(&s_msg, buf, n);
    }
}

cfg_store_status_t config_store_status(void)       { return s_store_status; }
cfg_err_t          config_store_validate_err(void) { return s_store_val_err; }

void config_get(ctrl_globals_t *g, ctrl_profile_t *prof) { *g = s_g; *prof = s_prof; }
void config_get_limits(ctrl_limits_t *lim) { *lim = s_lim; }
void config_get_thermal(ctrl_thermal_cfg_t *th) { *th = s_th; }
