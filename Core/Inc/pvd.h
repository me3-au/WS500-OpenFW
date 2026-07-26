/*
 * pvd.h — R5: programmable voltage detector / brown-out warning
 * (PROJECT_PLAN §7 R5).
 *
 * "An alternator regulator lives on a supply that load-dumps — this is not
 * optional." The PVD fires an interrupt when Vdd falls through a threshold set
 * ABOVE the brown-out reset level, which buys the firmware the microseconds it
 * needs to do two things while it still has a working core:
 *
 *   1. enter_safe_state() — never let the supply collapse with the field
 *      driven; a half-powered gate driver is an undefined output stage.
 *   2. seal the crash record, so the next boot can say "we browned out"
 *      instead of showing an unexplained reset.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_PVD_H
#define WS500_PVD_H

#include <stdint.h>
#include <stdbool.h>

/*
 * Configure and enable the PVD (threshold + EXTI line 16 + NVIC). Call after
 * board_init() and after crash_record_boot(), since the handler writes a crash
 * record. Cheap and idempotent.
 */
void pvd_init(void);

/* Falling-edge PVD events seen since boot. Non-zero after a survived
 * brown-out dip — worth surfacing in telemetry, since it means the supply is
 * marginal even though the unit kept running. */
uint32_t pvd_event_count(void);

/* True while Vdd is currently BELOW the configured threshold (PWR_CSR.PVDO). */
bool pvd_below_threshold(void);

#endif /* WS500_PVD_H */
