/*
 * regulator.h — alternator charge-regulation interface (CLEAN-ROOM).
 *
 * This is the control-logic boundary. The state machine and signatures here are a
 * FRESH design based on standard multi-stage charging (bulk / absorption / float)
 * and the PUBLISHED WS500 charge-profile behavior — NOT derived from the stock
 * firmware's algorithms. Implement regulator_step() from control theory.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_REGULATOR_H
#define WS500_REGULATOR_H

#include <stdint.h>
#include <stdbool.h>

/* Charge stages — standard multi-stage model (public/textbook). */
typedef enum {
    REG_DISABLED = 0,   /* charging off (BMS disconnect, fault clear, etc.) */
    REG_RAMP,           /* soft-start field ramp after enable */
    REG_BULK,           /* constant-current: field to hold current limit */
    REG_ABSORPTION,     /* constant-voltage at absorption target */
    REG_FLOAT,          /* reduced constant-voltage hold */
    REG_FAULT           /* latched fault: field forced off */
} reg_stage_t;

/* Live measurements (engineering units; filled by sensors.c). */
typedef struct {
    float vbat;         /* battery voltage  [V] (at system voltage, not 12V-norm) */
    float valt;         /* alternator output voltage [V] */
    float amps;         /* charge current [A] (signed; + = into battery) */
    float alt_temp_c;   /* alternator temperature [degC] (NAN if unknown) */
    float batt_temp_c;  /* battery temperature [degC] (NAN if unknown) */
    float rpm;          /* engine/alt RPM (0 if unknown) */
    bool  bms_charge_ok;/* CAN BMS permits charging */
} reg_inputs_t;

/* Configuration — mirrors the user-facing $CPx / $SCx parameters (see WS500 Util
 * ws_schema.json). Populate from the parsed config; all in engineering units. */
typedef struct {
    float sys_voltage;      /* nominal system voltage (12/24/36/48) */
    float v_absorption;     /* absorption target [V] */
    float v_float;          /* float target [V] */
    float amp_limit;        /* max charge current [A]; 0 = no limit */
    float ramp_rate;        /* field ramp per step (soft start) */
    float temp_comp_per_c;  /* voltage compensation per degC from 25C */
    float alt_temp_limit_c; /* alternator derate/cutback temperature */
    float field_max;        /* max field duty fraction 0..1 (small-alt protection) */
} reg_config_t;

/* Persistent controller state across steps. */
typedef struct {
    reg_stage_t stage;
    float       field;      /* current field command, 0..1 */
    uint32_t    stage_ms;   /* time in current stage */
    uint32_t    fault_code; /* bitfield; 0 = none */
} reg_state_t;

/* Command produced each step. */
typedef struct {
    float field;            /* field duty 0..1 -> field_drive_set() */
    bool  fault;            /* assert hardware field cutoff */
} reg_output_t;

void regulator_init(reg_state_t *st);

/*
 * regulator_step — advance the controller one tick.
 *   dt_ms : elapsed time since last call.
 * Returns the field command. MUST be fail-safe: on any doubt, command field = 0.
 *
 * >>> IMPLEMENT ME <<<  This is the core deliverable and must be written
 * independently. The stub in regulator.c holds the field at 0 (safe).
 */
reg_output_t regulator_step(reg_state_t *st,
                            const reg_inputs_t *in,
                            const reg_config_t *cfg,
                            uint32_t dt_ms);

#endif /* WS500_REGULATOR_H */
