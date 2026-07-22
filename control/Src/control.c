/*
 * control.c — two-stage LFP engine. PURE (no HAL). Fail-safe skeleton.
 *
 * Ships field-OPEN in every path: the control contract (see control.h) is not
 * implemented yet — this establishes the interface + a safe default so the app
 * builds and runs (field never energizes). Real state machine, arbitration,
 * CV loop, and transitions land next, per CONTROL_SPEC / PROFILE_SPEC.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "control.h"

void ctrl_init(ctrl_t *c)
{
    c->state = CTRL_STANDBY;
    c->standby_reason = CTRL_SB_OFF;
    c->effort = 0.0f;
    c->cmd_power_w = 0.0f;
    c->time_in_state_ms = 0;
    c->faults = CTRL_FAULT_NONE;
    c->ah_since_charged = 0.0f;
}

ctrl_command_t ctrl_tick(ctrl_t *c,
                         const ctrl_measured_t *m,
                         const ctrl_ceilings_t *ceil,
                         const ctrl_profile_t  *prof,
                         const ctrl_globals_t  *g,
                         uint32_t dt_ms)
{
    (void)m; (void)ceil; (void)prof; (void)g;
    c->time_in_state_ms += dt_ms;

    /* -------------------------------------------------------------------- *
     *  >>> IMPLEMENT THE TWO-STAGE ENGINE HERE (control.h contract) <<<     *
     *  Until then: field OPEN, unconditionally (fail-safe).                 *
     * -------------------------------------------------------------------- */
    c->state = CTRL_STANDBY;
    c->standby_reason = CTRL_SB_OFF;
    c->effort = 0.0f;

    ctrl_command_t cmd = {
        .field_effort  = 0.0f,
        .field_duty    = 0.0f,
        .field_open    = true,
        .state         = c->state,
        .standby_reason= c->standby_reason,
        .binding       = CTRL_BIND_NONE,
        .binding_w     = 0.0f,
        .faults        = c->faults,
    };
    return cmd;
}
