/*
 * err_budget.c — R6: peripheral error budgets (PROJECT_PLAN §7 R6).
 * Ladder, thresholds and the reason this module is policy-only: err_budget.h.
 *
 * Deliberately trivial: counters and comparisons, no hardware, no allocation,
 * no HAL, callable from anywhere including an ISR. The value is not in the
 * code, it is in having exactly ONE place where "how many failures is too
 * many" is written down, so the answer cannot drift per driver.
 *
 * SPDX-License-Identifier: MIT
 */
#include "err_budget.h"

typedef struct {
    uint32_t total;        /* lifetime failed transactions */
    uint32_t consecutive;  /* current failure streak */
    uint32_t ok_streak;    /* current success streak (for fault clearing) */
    uint32_t reinits;      /* times ERRB_ACTION_REINIT was returned */
    bool     faulted;      /* latched */
} errb_state_t;

static errb_state_t s_dom[ERRB_COUNT];

static bool valid(errb_domain_t d) { return (unsigned)d < (unsigned)ERRB_COUNT; }

errb_action_t errb_fail(errb_domain_t d)
{
    if (!valid(d)) return ERRB_ACTION_NONE;
    errb_state_t *s = &s_dom[d];

    if (s->total != UINT32_MAX) s->total++;
    if (s->consecutive != UINT32_MAX) s->consecutive++;
    s->ok_streak = 0u;

    if (s->consecutive >= ERRB_FAULT_AT) {
        s->faulted = true;
        return ERRB_ACTION_FAULT;
    }

    /* Re-init on every ERRB_REINIT_AT-th consecutive failure rather than only
     * the first, so a bus that wedges again after a successful re-init but
     * before a successful transaction still gets another attempt before the
     * fault latches. */
    if (s->consecutive != 0u && (s->consecutive % ERRB_REINIT_AT) == 0u) {
        if (s->reinits != UINT32_MAX) s->reinits++;
        return ERRB_ACTION_REINIT;
    }

    return ERRB_ACTION_NONE;
}

void errb_ok(errb_domain_t d)
{
    if (!valid(d)) return;
    errb_state_t *s = &s_dom[d];

    s->consecutive = 0u;
    if (s->ok_streak != UINT32_MAX) s->ok_streak++;
    if (s->faulted && s->ok_streak >= ERRB_RECOVER_OK) s->faulted = false;
}

bool errb_faulted(errb_domain_t d) { return valid(d) && s_dom[d].faulted; }

uint32_t errb_fault_mask(void)
{
    uint32_t m = 0u;
    for (unsigned i = 0; i < (unsigned)ERRB_COUNT; i++)
        if (s_dom[i].faulted) m |= (1u << i);
    return m;
}

uint32_t errb_total(errb_domain_t d)       { return valid(d) ? s_dom[d].total : 0u; }
uint32_t errb_consecutive(errb_domain_t d) { return valid(d) ? s_dom[d].consecutive : 0u; }
uint32_t errb_reinits(errb_domain_t d)     { return valid(d) ? s_dom[d].reinits : 0u; }
