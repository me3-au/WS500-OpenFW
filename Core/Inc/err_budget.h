/*
 * err_budget.h — R6: peripheral error budgets (PROJECT_PLAN §7 R6).
 *
 * The rule §7 R6 sets: a driver may retry, but it may not retry FOREVER while
 * pretending everything is fine. Every peripheral that can fail intermittently
 * gets a budget, and exhausting the budget escalates into the control core's
 * existing fault ladder instead of producing an endlessly stale reading.
 *
 * The ladder, applied per domain:
 *
 *      transaction retries   (inside the driver — bounded attempts, bounded
 *                             timeout per attempt; already how ina2xx.c and
 *                             eeprom24c16.c are written)
 *   -> ERRB_ACTION_REINIT    (N consecutive failed transactions: re-initialise
 *                             the bus — clears a stuck peripheral or a
 *                             wedged I²C state machine)
 *   -> ERRB_ACTION_FAULT     (M consecutive: latch a fault bit; main.c maps it
 *                             into ctrl_measured_t.ext_faults so the control
 *                             core degrades the way the spec already says it
 *                             should for a lost signal)
 *
 * This module is pure bookkeeping — no HAL, no hardware, no policy about WHAT
 * a domain means. That keeps the escalation rules in one auditable place and
 * lets the drivers stay about their own protocol. It holds no control-core
 * types either: main.c owns the mapping from these bits to
 * ctrl_fault_bits_t, so Core/ never includes control/ headers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_ERR_BUDGET_H
#define WS500_ERR_BUDGET_H

#include <stdint.h>
#include <stdbool.h>

typedef enum {
    ERRB_INA226 = 0,   /* I²C shunt/bus monitor — current + bus voltage */
    ERRB_EEPROM,       /* I²C config EEPROM */
    ERRB_CAN,          /* bxCAN bus-off / recovery */
    ERRB_COUNT
} errb_domain_t;

typedef enum {
    ERRB_ACTION_NONE = 0, /* inside budget — the driver's own retry is enough */
    ERRB_ACTION_REINIT,   /* re-initialise this peripheral/bus now */
    ERRB_ACTION_FAULT     /* budget exhausted — the fault bit is now latched */
} errb_action_t;

/* Consecutive failed TRANSACTIONS (not attempts) that trigger each step.
 * Three failed transactions is already ~9 attempts and ~15-60 ms of bus time
 * on the I²C domains — long past "noise", short of "give up". Six is two full
 * reinit cycles with no success in between, which is a peripheral that is not
 * coming back on its own. [SPEC-SIGNOFF] */
#define ERRB_REINIT_AT   3u
#define ERRB_FAULT_AT    6u

/* Consecutive successes needed to clear a latched fault. Hysteresis: a bus
 * that answers once after failing six times has not recovered, and flapping a
 * fault bit into the control core would make it chatter between degraded and
 * normal operation. [SPEC-SIGNOFF] */
#define ERRB_RECOVER_OK  5u

/*
 * Record one failed transaction and return what the caller must do about it.
 * The caller is expected to ACT on ERRB_ACTION_REINIT (drivers know how to
 * re-initialise themselves; this module does not). Returns the highest step
 * reached, so a caller that only handles REINIT can ignore the rest.
 */
errb_action_t errb_fail(errb_domain_t d);

/* Record one successful transaction: clears the consecutive-failure count and,
 * after ERRB_RECOVER_OK successes in a row, the latched fault. */
void errb_ok(errb_domain_t d);

/* True while the domain's fault is latched. */
bool errb_faulted(errb_domain_t d);

/* All latched domains as a bitmask of (1u << errb_domain_t) — this is what
 * main.c translates into ext_faults each control tick. */
uint32_t errb_fault_mask(void);

/* Diagnostics/telemetry counters. `total` never resets; `consecutive` is the
 * live streak; `reinits` counts how often the domain has been re-initialised
 * (a slowly climbing reinit count is a marginal bus you want to know about
 * before it becomes a fault). */
uint32_t errb_total(errb_domain_t d);
uint32_t errb_consecutive(errb_domain_t d);
uint32_t errb_reinits(errb_domain_t d);

#endif /* WS500_ERR_BUDGET_H */
