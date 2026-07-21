/*
 * regulator.c — CLEAN-ROOM control-logic stub.
 *
 * Ships FAIL-SAFE: field held at 0 in every state. Implement the real multi-stage
 * regulation here, from control theory + the PUBLISHED WS500 charge-profile
 * behavior. Do NOT transcribe the stock firmware's algorithm.
 *
 * Suggested implementation order:
 *   1. RAMP: ease `field` up by cfg->ramp_rate until current or voltage responds.
 *   2. BULK: PI loop driving `amps` toward cfg->amp_limit (field-limited).
 *   3. ABSORPTION: PI loop holding `vbat` at temp-compensated cfg->v_absorption;
 *      exit to FLOAT on tail-current / timer.
 *   4. FLOAT: hold cfg->v_float.
 *   5. Cross-cutting: alt-temp derate (cfg->alt_temp_limit_c), field_max clamp,
 *      BMS-disconnect handling (in->bms_charge_ok), fault latching -> REG_FAULT.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "regulator.h"
#include <math.h>

void regulator_init(reg_state_t *st)
{
    st->stage = REG_DISABLED;
    st->field = 0.0f;
    st->stage_ms = 0;
    st->fault_code = 0;
}

/* Temperature-compensated absorption/float target (helper you can build on). */
static inline float temp_compensated(float target, float batt_temp_c, float per_c)
{
    if (isnan(batt_temp_c) || per_c == 0.0f) return target;
    return target + (25.0f - batt_temp_c) * per_c;
}

reg_output_t regulator_step(reg_state_t *st,
                            const reg_inputs_t *in,
                            const reg_config_t *cfg,
                            uint32_t dt_ms)
{
    (void)in; (void)cfg; (void)dt_ms; (void)temp_compensated;

    st->stage_ms += dt_ms;

    /* -------------------------------------------------------------------- *
     *  >>> IMPLEMENT THE REGULATION STATE MACHINE HERE <<<                  *
     *  Until then, hold the field OFF unconditionally (fail-safe).          *
     * -------------------------------------------------------------------- */
    st->field = 0.0f;

    reg_output_t out = { .field = st->field, .fault = false };
    return out;
}
