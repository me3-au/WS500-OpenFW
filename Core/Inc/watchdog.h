/*
 * watchdog.h — R1: checkpoint-kicked windowed IWDG (PROJECT_PLAN §7 R1).
 *
 * The independent watchdog runs off the LSI, so it survives a main-clock
 * failure — but a watchdog is only worth anything if the thing that kicks it
 * proves the system is still DOING ITS JOB. A naked timer or ISR kick just
 * proves the interrupt controller works while the control loop is dead.
 *
 * So the kick is gated on a task-alive vector: every subsystem reports a
 * checkpoint when it completes its work for the cycle, and watchdog_service()
 * refreshes the IWDG only when EVERY checkpoint has reported within its own
 * time budget. One stalled subsystem stops the kicks and the watchdog resets
 * the part — which is the outcome we want, because a regulator that has
 * stopped sensing must not keep driving the field.
 *
 * The window (F0's IWDG has WINR) closes the other half of the trap: a kick
 * that arrives too EARLY also resets the part, so a runaway loop spinning at
 * full speed through the kick path is caught too.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_WATCHDOG_H
#define WS500_WATCHDOG_H

#include <stdint.h>
#include <stdbool.h>

/*
 * The task-alive vector. Every entry must report within its budget or the
 * watchdog stops being kicked. Keep this list short and meaningful: each entry
 * is a promise that something observable happened, not that code was reached.
 */
typedef enum {
    WDG_CP_CONTROL = 0,  /* ctrl_tick() ran and its command was applied */
    WDG_CP_SENSORS,      /* a sensor acquisition cycle completed */
    WDG_CP_COMMS,        /* CAN + USB/config service pumped */
    WDG_CP_COUNT
} wdg_checkpoint_t;

/*
 * Start the windowed IWDG. Call LAST in the init sequence, immediately before
 * entering the main loop: once started the IWDG cannot be stopped, and the
 * first kick can only happen after every checkpoint has reported once (i.e.
 * after one full loop iteration).
 *
 * Also latches the §7 R1 escalation decision for this boot (see
 * watchdog_limp_latched) and freezes the counter while a debugger has the core
 * halted, so single-stepping does not reset the part.
 */
void watchdog_init(void);

/* Report that a subsystem finished its work for this cycle. Cheap (one store);
 * safe to call more than once per cycle. Ignores out-of-range values. */
void watchdog_checkpoint(wdg_checkpoint_t cp);

/*
 * Kick the IWDG if — and only if — every checkpoint has reported within its
 * budget and the hardware refresh window is open. Call once per control cycle.
 * When it declines to kick it records why (watchdog_starved_mask), which is
 * what turns "the watchdog fired" into "the watchdog fired because sensors
 * stopped" in the crash record.
 */
void watchdog_service(void);

/*
 * True when this boot must run field-off and stay there: the §7 R1 escalation
 * (three consecutive watchdog- or fault-caused boots — see crash_record.h)
 * has tripped. main.c ORs this into the control core's existing
 * CTRL_FAULT_WATCHDOG input; there is deliberately no separate limp mode in
 * the driver layer and no change to control/.
 */
bool watchdog_limp_latched(void);

/* Checkpoints that were stale (or never reported) at the last service call,
 * as a bitmask of (1u << wdg_checkpoint_t). 0 = healthy. */
uint32_t watchdog_starved_mask(void);

/* Successful kicks since boot (diagnostics/telemetry). */
uint32_t watchdog_kick_count(void);

/* Service calls that declined to kick because a checkpoint was stale. A
 * non-zero value with the system still running means it recovered — worth
 * seeing in telemetry, it is the early warning for a marginal subsystem. */
uint32_t watchdog_starve_count(void);

#endif /* WS500_WATCHDOG_H */
