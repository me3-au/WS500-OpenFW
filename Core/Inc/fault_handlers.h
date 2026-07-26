/*
 * fault_handlers.h — R3: exception handlers that report, then reset
 * (PROJECT_PLAN §7 R3).
 *
 * The Cortex-M0 constraint that shapes this: there is exactly ONE fault
 * exception (HardFault — no MemManage/BusFault/UsageFault vectors, no MPU, no
 * fault-status registers). So every abnormal entry into the vector table is
 * funnelled through the same three steps, in this order and no other:
 *
 *      enter_safe_state()  ->  crash_record_write()  ->  NVIC_SystemReset()
 *
 * Field off FIRST (it is the only step that must not be allowed to fail),
 * evidence SECOND, reboot THIRD. §7 R3's hard rule — "never a bare while(1);
 * spinning with the field energized is the worst possible failure mode" — is
 * why the vendored startup's `Default_Handler` (a `b .` loop) is displaced by
 * per-IRQ aliases in fault_handlers.c, and why Error_Handler() reboots.
 *
 * Nothing here is called by application code except Error_Handler() and the
 * assert hook; the rest are vector-table entries.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_FAULT_HANDLERS_H
#define WS500_FAULT_HANDLERS_H

#include <stdint.h>

/*
 * C stage of the two-stage HardFault handler. Called from the naked assembly
 * entry (which is the only thing that can see the stacked frame and the
 * EXC_RETURN value) with a scratch stack already re-pointed at the top of the
 * stack region, so it is safe even when the fault WAS a stack overflow.
 *
 *   frame_sp    — the SP that holds the 8-word exception frame
 *   sp_is_psp   — non-zero if that SP was the process stack (bit 2 of LR)
 *   exc_return  — the EXC_RETURN value the exception entered with
 *
 * Does not return: it enters the safe state, writes a crash record and resets.
 * Exposed in this header (rather than kept static) because the assembly entry
 * calls it by name and the Renode fault test asserts on the symbol.
 */
void fault_hardfault_stage2(uint32_t frame_sp, uint32_t sp_is_psp,
                            uint32_t exc_return);

/* C stage for NMI. On this part NMI means an SRAM parity error (§7 R3) —
 * memory we cannot trust, so it is treated as fatal exactly like a HardFault.
 * Same arguments and same no-return contract as fault_hardfault_stage2. */
void fault_nmi_stage2(uint32_t frame_sp, uint32_t sp_is_psp,
                      uint32_t exc_return);

/* C stage for any interrupt no driver claims — the replacement for the
 * vendored startup's spinning Default_Handler. Same contract. */
void fault_unexpected_irq_stage2(uint32_t frame_sp, uint32_t sp_is_psp,
                                 uint32_t exc_return);

/*
 * Unrecoverable-initialisation hook. Kept as `Error_Handler` because that is
 * the name ST-generated code and our own drivers call, but it no longer spins:
 * safe state, crash record (CRASH_KIND_ERROR_HANDLER), reset. Does not return.
 */
void Error_Handler(void);

/*
 * Release assert. Records file-id/line in the crash record's PC/LR fields,
 * enters the safe state and resets — it never just spins (§7 "Assert policy").
 * Use the WS500_ASSERT macro rather than calling this directly.
 */
void fault_assert_failed(uint32_t file_id, uint32_t line);

/* Cheap file identity for asserts: the first four characters of __FILE__ would
 * need a runtime string, so callers pass a 32-bit tag of their own choosing.
 * A module's tag is its four-character name, e.g. WS500_FILE_ID('m','a','i','n'). */
#define WS500_FILE_ID(a, b, c, d) \
    ((uint32_t)(a) | ((uint32_t)(b) << 8) | ((uint32_t)(c) << 16) | ((uint32_t)(d) << 24))

/* Assert that stays in release builds by design: a violated invariant in a
 * field regulator must produce a recorded, field-off reboot, not undefined
 * behaviour. Use for conditions that are impossible unless the firmware is
 * already broken — never for runtime/sensor conditions, which belong in the
 * fault ladder. */
#define WS500_ASSERT(file_id, cond) \
    do { if (!(cond)) fault_assert_failed((file_id), __LINE__); } while (0)

#endif /* WS500_FAULT_HANDLERS_H */
