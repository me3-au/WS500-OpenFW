/*
 * control.h — alternator charge-regulation engine (CLEAN-SLATE, two-stage LFP).
 *
 * PURE control core: no HAL, no hardware headers — host- and Renode-testable.
 * Authoritative design: docs/CONTROL_SPEC_NEXTGEN.md (Draft B) +
 * docs/PROFILE_SPEC_LFP.md (Draft 1). Nothing from the legacy WS500/Pb model
 * (6-stage stack, absorption, DIP, small-alt/half modes, RFM/PBF, 12V/500Ah
 * normalization, temp-comp curves) is carried forward — see CONTROL_SPEC App. A.
 *
 * Engine shape:
 *   - THREE states: STANDBY / BULK / FLOAT (PROFILE_SPEC §2).
 *   - ONE arbitration: commanded power = min() of active ceilings, Watts at real
 *     system voltage (CONTROL_SPEC §2); the binding ceiling is always reported.
 *   - Per-cell canonical voltages (V/cell); pack = v_cell × cells_series.
 *   - Inner loop on NORMALIZED EFFORT e = duty/duty_max ∈ [0,1] (§5.1).
 *
 * The app assembles ctrl_measured_t from drivers; sub-systems (arbitration,
 * thermal, rpm, bms, faults, config) feed ceilings + resolved profile in.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_CONTROL_H
#define WS500_CONTROL_H

#include <stdint.h>
#include <stdbool.h>
#include <math.h>   /* INFINITY, NAN */

/* An inactive power ceiling is +infinity, so it never wins the min(). */
#define CTRL_CEILING_INACTIVE  (INFINITY)

/* ---- State machine (PROFILE_SPEC §2) -------------------------------------- */
typedef enum {
    CTRL_STANDBY = 0,   /* field open; annotated with a reason below */
    CTRL_BULK,          /* maximum field subject to arbitration min() */
    CTRL_FLOAT          /* CV at profile float/limp voltage, power ≤ rest cap */
} ctrl_state_t;

typedef enum {
    CTRL_SB_OFF = 0,    /* ignition/enable inactive */
    CTRL_SB_WARMUP,     /* warmup gate pending (time or coolant) */
    CTRL_SB_REST,       /* charged, zero-rest profile (micro-rest OCV free here) */
    CTRL_SB_FAULT       /* latched unrecoverable fault */
} ctrl_standby_reason_t;

/* Which ceiling is binding — telemetered so the user sees *why* (CONTROL_SPEC §2). */
typedef enum {
    CTRL_BIND_NONE = 0,
    CTRL_BIND_STAGE,          /* profile power (max_charge_power_w / rest cap) */
    CTRL_BIND_VOLTAGE_CLAMP,  /* CV target reached — the old "absorption" */
    CTRL_BIND_THERMAL,        /* predictive thermal governor (§4) */
    CTRL_BIND_BMS,            /* BMS CCL / DVCC CCL (§6.3) */
    CTRL_BIND_BATTERY_C,      /* battery C-rate limit (§2.1) */
    CTRL_BIND_WIRING,         /* charge-path ampacity (§2.1) */
    CTRL_BIND_ALT_ABSOLUTE,   /* alternator rectifier/stator hard cap (§2.1) */
    CTRL_BIND_ALT_CAPABILITY, /* alternator capability @RPM (§3.4, optional) */
    CTRL_BIND_BELT,           /* belt torque ceiling (§2.2, optional) */
    CTRL_BIND_ENGINE,         /* engine white-space budget (§3.5, optional) */
    CTRL_BIND_USER_CAP,       /* manual cap / quiet mode */
    CTRL_BIND_ROTOR_CLAMP     /* field-effort clamped at duty_max (§5.1) */
} ctrl_bind_src_t;

/* ---- Signal-quality / mode state ------------------------------------------ */
typedef enum { CTRL_RPM_VALID = 0, CTRL_RPM_STALE, CTRL_RPM_LOST } ctrl_rpm_state_t;
typedef enum { CTRL_RUN_NOT_RUNNING = 0, CTRL_RUN_RUNNING } ctrl_run_state_t;

/* Current-source tier (PROFILE_SPEC §4.2) — governs which exits are armed. */
typedef enum {
    CTRL_ISRC_BATT_SHUNT = 1,  /* tier 1: local shunt, battery-side (truth) */
    CTRL_ISRC_NMEA_BATT  = 2,  /* tier 2: external NMEA battery current */
    CTRL_ISRC_ALT_SHUNT  = 3,  /* tier 3: local shunt, alternator-side (tail disarmed) */
    CTRL_ISRC_EST_ALT    = 4,  /* tier 4: estimated alt current (coarse) */
    CTRL_ISRC_NONE       = 5   /* tier 5: voltage-only */
} ctrl_isrc_tier_t;

/* ---- Fault model (CONTROL_SPEC §7, §9) ------------------------------------ */
typedef enum { CTRL_SEV_INFO = 0, CTRL_SEV_WARN, CTRL_SEV_FAULT, CTRL_SEV_CRITICAL } ctrl_severity_t;

/* Fault bitfield; which faults LIMP vs OPEN is fixed in firmware, not config (§7). */
typedef enum {
    CTRL_FAULT_NONE            = 0,
    CTRL_FAULT_OVERVOLTAGE     = 1u << 0,   /* CRITICAL: fast field kill */
    CTRL_FAULT_FIELD_SHORT     = 1u << 1,   /* CRITICAL */
    CTRL_FAULT_FIELD_OPEN      = 1u << 2,   /* WARN/FAULT */
    CTRL_FAULT_FIELD_OVERCUR   = 1u << 3,   /* CRITICAL: BKIN territory */
    CTRL_FAULT_SELF_OVERTEMP   = 1u << 4,   /* driver-stage NTC (§5.1) */
    CTRL_FAULT_OVERSPEED       = 1u << 5,   /* CRITICAL, RPM present */
    CTRL_FAULT_SHUNT_OPEN      = 1u << 6,   /* claimed I with static VBat (§7) */
    CTRL_FAULT_SHUNT_REVERSED  = 1u << 7,
    CTRL_FAULT_BATT_DTDT       = 1u << 8,   /* thermal runaway → charge abort */
    CTRL_FAULT_BATT_LOWTEMP    = 1u << 9,   /* Li low-temp charge cutoff (hard) */
    CTRL_FAULT_BATT_HIGHTEMP   = 1u << 10,  /* high-temp charge abort */
    CTRL_FAULT_LOST_VBAT_SENSE = 1u << 11,  /* recoverable → LIMP */
    CTRL_FAULT_LOST_BMS        = 1u << 12,  /* recoverable → LIMP */
    CTRL_FAULT_IMPLAUSIBLE_SHUNT = 1u << 13,/* recoverable → LIMP */
    CTRL_FAULT_THERMAL_DIVERGE = 1u << 14,  /* governor model divergence (§4.1) */
    CTRL_FAULT_WATCHDOG        = 1u << 15    /* → field open */
} ctrl_fault_bits_t;

/* ---- Live measurements the engine consumes -------------------------------- *
 * Pack-referenced; engine derives V/cell via cells_series. NAN = unavailable. */
typedef struct {
    float vbat_pack_v;     /* measured battery voltage, Kelvin sense (pack) */
    float vcomp_pack_v;    /* IR-compensated surface voltage (§6.6); == vbat if no R model */
    float amps_batt;       /* battery current [A], signed (+ = charging), resolved tier */
    float watts_batt;      /* battery power [W] */
    ctrl_isrc_tier_t isrc; /* tier amps_batt came from */
    float v_supply_v;      /* field supply voltage — sets duty_max (§5.1) */

    float alt_hotspot_c;   /* estimated alternator hot-spot temp (§4.2); NAN if lost */
    float batt_temp_c;     /* battery temp — charge-window gate only; NAN if none */
    float driver_temp_c;   /* internal driver-stage NTC (§5.1 rotor proxy) */

    float   rpm;           /* fused engine RPM (§3.1); valid only when rpm_state==VALID */
    ctrl_rpm_state_t rpm_state;
    ctrl_run_state_t run_state;  /* engine RUN-DETECT (§5.2) */

    float soc_pct;         /* -1 if unknown */
    bool  soc_trusted;     /* BMS or anchored coulomb count (§4.3) */

    bool  ignition;        /* enable/wake input */
    bool  feature_in;      /* the single assignable Feature-IN, function-resolved */

    uint32_t ext_faults;   /* faults detected by other modules (BMS loss, shunt
                            * implausible, …), OR'd in by the app (ctrl_fault_bits_t) */
} ctrl_measured_t;

/* ---- Arbitration ceilings (Watts; INACTIVE = +inf) ------------------------ */
typedef struct {
    float thermal_w;       /* §4 governor output */
    float bms_ccl_w;       /* §6.3 */
    float battery_c_w;     /* §2.1 */
    float wiring_w;        /* §2.1 */
    float alt_absolute_w;  /* §2.1 hard cap */
    float alt_capability_w;/* §3.4 @RPM, optional */
    float belt_w;          /* §2.2, optional */
    float engine_w;        /* §3.5 white-space, optional */
    float user_cap_w;      /* §2 item 7 */
} ctrl_ceilings_t;

/* ---- Resolved active profile + globals (V/cell) --------------------------- *
 * The full multi-profile schema (PROFILE_SPEC §7) lives in the config module;
 * the engine holds ONE resolved active profile plus the globals below. */
typedef enum { CTRL_REST_HOLD = 0, CTRL_REST_ZERO } ctrl_rest_mode_t;

typedef struct {
    uint8_t id;                 /* profile id 1..7 */
    float   cv_target_vcell;    /* BULK CV voltage (V/cell) */
    bool    exit_at_cv_entry;   /* Solar-Finish flag (T2d) */
    ctrl_rest_mode_t rest_mode; /* hold | zero */
    float   rest_voltage_vcell; /* FLOAT/Limp CV (hold only) */
    float   rest_power_cap_w;   /* cap on REST output (hold only) */
    float   v_revert_vcell;     /* T3 voltage revert threshold */
    uint16_t t_revert_hold_s;
    int8_t  soc_revert_pct;     /* -1 = disabled */
    float   ah_revert;          /* Ah discharged to revert (tiers 1–2) */
} ctrl_profile_t;

typedef struct {
    uint8_t cells_series;       /* 4 / 8 / 16 */
    float   bank_capacity_ah;
    float   max_charge_power_w; /* top of the arbitration min() in BULK */
    float   ramp_w_per_s;       /* soft-start + belt protection */
    float   p_tail_w;           /* T2a tail-power exit (materialized) */
    uint16_t t_tail_hold_s;
    uint16_t t_vclamp_s;        /* voltage-clamp qualifier (T2d / Bulk(Voltage)) */
    uint16_t t_charge_max_min;  /* T2c backstop */
    uint16_t warmup_time_s;
    float   warmup_coolant_c;   /* NAN = disabled */
    int8_t  soc_target_pct;     /* -1 = disabled */
    float   rotor_rated_v;      /* default 12 on 48V systems (§5.1) */
    float   rotor_v_max;        /* NAN = use rated; override ladder */
    bool    allow_full_field_48v;

    /* Limp Home target (§7 degraded-mode ladder) — safe-mode FLOAT. */
    float   limp_vcell;         /* v_limp, default 3.30 V/cell */
    float   limp_power_cap_w;   /* reduced power cap in limp */
} ctrl_globals_t;

/* ---- Hardware limit set (CONTROL_SPEC §2.1), native units ----------------- *
 * Static ratings of the installed system; converted to Watts each cycle at the
 * present voltage. <= 0 = unset (drops out of the arbitration min()). Belt is
 * RPM-dependent and handled by the RPM subsystem, not here. */
typedef struct {
    float battery_c_limit;    /* C-rate (× bank Ah × V → W) */
    float wiring_limit_a;     /* charge-path ampacity (A → W) */
    float alternator_limit_a; /* absolute rectifier/stator cap (A → W) */
} ctrl_limits_t;

/* ---- Engine command output ------------------------------------------------ */
typedef struct {
    float          field_effort;   /* commanded e ∈ [0,1] = duty/duty_max (§5.1) */
    float          field_duty;     /* absolute PWM duty ∈ [0,1] = effort × duty_max */
    bool           field_open;     /* true → assert hardware field-off (BKIN/MOE) */
    ctrl_state_t   state;
    ctrl_standby_reason_t standby_reason;
    ctrl_bind_src_t binding;       /* which ceiling set field_duty (telemetry) */
    float          binding_w;      /* the binding power ceiling in Watts */
    uint32_t       faults;         /* active ctrl_fault_bits_t */
} ctrl_command_t;

/* ---- Persistent engine context -------------------------------------------- */
typedef struct {
    ctrl_state_t          state;
    ctrl_standby_reason_t standby_reason;
    float                 effort;        /* current normalized field effort */
    float                 cmd_power_w;   /* ramp-limited commanded power */
    uint32_t              time_in_state_ms;
    uint32_t              faults;        /* latched + active bits */
    float                 ah_since_charged; /* T3 Ah revert integrator */

    /* Damped signals (§1.4); NAN = unseeded. */
    float                 v_ctrl_f;      /* ~V_comp, fast (CV loop, 1 s) */
    float                 v_revert_f;    /* ~V_comp, slow (T3 revert, 30 s) */
    float                 p_tail_f;      /* ~P_bat (T2a tail, 60 s) */

    /* Transition hold timers (ms). */
    uint32_t              warmup_ms;
    uint32_t              tail_hold_ms;
    uint32_t              revert_hold_ms;
    uint32_t              vclamp_hold_ms;
} ctrl_t;

/* ---- API ------------------------------------------------------------------ */
void ctrl_init(ctrl_t *c);

/*
 * ctrl_tick — advance the engine one control tick.
 *   m     : live measurements (this cycle)
 *   ceil  : active power ceilings (Watts; INACTIVE = +inf)
 *   prof  : resolved active profile
 *   g     : global config
 *   dt_ms : elapsed since last call
 * Returns the field command. MUST be fail-safe: any uncertainty → field_open.
 *
 * Contract (to implement, pending review):
 *   1. Safety comparators on RAW signals (OV kill, overspeed, hard temp) →
 *      faults / field_open regardless of state (CONTROL_SPEC §1.4, §7).
 *   2. Transitions T1–T4 on DAMPED signals with hold times (PROFILE_SPEC §2.2).
 *   3. duty_max = clamp(rotor_rated_v / v_supply_v) unless override ladder (§5.1).
 *   4. Target power = profile stage power; CV loop caps effort at cv_target;
 *      arbitrated power = min(active ceilings); effort follows the lower demand;
 *      ramp UP via ramp_w_per_s, DOWN immediate (§3.5). Report binding source.
 *   5. Degraded-but-recoverable faults → Limp Home (FLOAT @ v_limp), not field-off.
 */
ctrl_command_t ctrl_tick(ctrl_t *c,
                         const ctrl_measured_t *m,
                         const ctrl_ceilings_t *ceil,
                         const ctrl_profile_t  *prof,
                         const ctrl_globals_t  *g,
                         uint32_t dt_ms);

/* Helpers (inline, pure). */
static inline float ctrl_pack_from_cell(float v_cell, uint8_t cells) { return v_cell * (float)cells; }
static inline float ctrl_cell_from_pack(float v_pack, uint8_t cells) { return cells ? v_pack / (float)cells : NAN; }

#endif /* WS500_CONTROL_H */
