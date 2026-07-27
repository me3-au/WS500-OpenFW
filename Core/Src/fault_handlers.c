/*
 * fault_handlers.c — R3: two-stage fault entry, one funnel, always a reboot
 * (PROJECT_PLAN §7 R3). Policy and call contract: fault_handlers.h.
 *
 * WHY TWO STAGES. The stacked exception frame (r0-r3, r12, LR, PC, xPSR) and
 * the EXC_RETURN value in LR are only visible at the instant the handler is
 * entered; any C prologue destroys the evidence and, worse, PUSHES onto a
 * stack that may be exactly what just faulted. So stage 1 is `naked` assembly
 * that (a) reads EXC_RETURN bit 2 to learn which stack holds the frame,
 * (b) re-points MSP at the top of the stack region so stage 2 has somewhere
 * safe to run — a stack overflow is a fault we specifically expect (§7 R4 puts
 * the stack at the bottom of RAM to *cause* this fault), and (c) tail-calls
 * the C stage. Clobbering the old stack is free: we are about to reset.
 *
 * ARMv6-M / THUMB-1 ONLY. This assembly must assemble for -mcpu=cortex-m0:
 * no IT blocks, no CBZ/CBNZ, no MOVW, no 32-bit conditional branches. Only
 * MOV/MOVS/TST/B<cc>/MRS/BL and a PC-relative literal load. The literal pool
 * is emitted inside the function (a forward `.word` reference), not left to
 * the assembler's default pool placement, because a Thumb-1 LDR-literal can
 * only reach FORWARD and only 1020 bytes.
 *
 * WHY EVERY HANDLER RESETS. A Cortex-M0 has no way to recover from a HardFault
 * (no fault-status registers to analyse, no MPU to re-arm, no way to know what
 * was corrupted). The only defensible actions are: make the hardware safe,
 * leave evidence, and start from a known state. Reset-looping is bounded by
 * the §7 R1 escalation counter in crash_record.c — three consecutive
 * fault-path boots latch LIMP-and-stay, so a permanently broken build ends up
 * parked field-off rather than power-cycling forever.
 *
 * SPDX-License-Identifier: MIT
 */
#include "fault_handlers.h"
#include "safe_state.h"
#include "crash_record.h"
#include "stm32f0xx.h"

/*
 * Stage-1 assembly, shared by every fatal vector. Expands to the whole body of
 * a naked function; `stage2` names the C function to tail-call.
 *
 * Line by line:
 *   movs/mov/tst/beq  test EXC_RETURN bit 2 — set means the frame is on PSP
 *   mrs r0, psp|msp   r0 := the SP that holds the 8-word exception frame
 *   movs r1, #1|#0    r1 := "was PSP"
 *   mov  r2, lr       r2 := EXC_RETURN, captured BEFORE the BL overwrites LR
 *   ldr  r3, 4f       load _estack from the literal placed at label 4 below.
 *                     A Thumb-1 LDR-literal only reaches FORWARD (unsigned
 *                     offset, max 1020 B), which is why the word is emitted
 *                     inside the function instead of via `ldr r3, =_estack`
 *                     and the assembler's default pool placement.
 *   mov  sp, r3       give stage 2 a known-good stack — the whole point of the
 *                     naked entry, since the fault may BE a stack overflow.
 *                     Clobbering the old stack is free: we reset next.
 *   bl   stage2       C stage; does not return.
 *   3: b 3b           unreachable trap. Not a "spin with the field energized":
 *                     enter_safe_state() is stage 2's first statement, so by
 *                     the time control could get here the field is off.
 */
#define FAULT_STAGE1_BODY(stage2)                                             \
    __asm volatile(                                                           \
        "  movs r0, #4              \n"                                       \
        "  mov  r1, lr              \n"                                       \
        "  tst  r0, r1              \n"                                       \
        "  beq  1f                  \n"                                       \
        "  mrs  r0, psp             \n"                                       \
        "  movs r1, #1              \n"                                       \
        "  b    2f                  \n"                                       \
        "1:                         \n"                                       \
        "  mrs  r0, msp             \n"                                       \
        "  movs r1, #0              \n"                                       \
        "2:                         \n"                                       \
        "  mov  r2, lr              \n"                                       \
        "  ldr  r3, 4f              \n"                                       \
        "  mov  sp, r3              \n"                                       \
        "  bl   " #stage2 "         \n"                                       \
        "3: b    3b                 \n"                                       \
        "  .align 2                 \n"                                       \
        "4: .word _estack           \n")

/*
 * Is `sp` a plausible pointer to an 8-word exception frame? A fault caused by
 * a wild pointer or a stack overflow can leave SP anywhere, and dereferencing
 * an unmapped address inside a HardFault handler on a Cortex-M0 means LOCKUP —
 * no second chance, no crash record. So the frame is only read when it lies
 * wholly inside SRAM and is word-aligned; otherwise the record stores zeros
 * and the SP itself, which is still the most useful thing we know.
 */
#define WS500_SRAM_BASE   0x20000000u
#define WS500_SRAM_SIZE   0x00004000u   /* 16 KB, STM32F072RB */

static int frame_plausible(uint32_t sp)
{
    if (sp & 3u) return 0;
    if (sp < WS500_SRAM_BASE) return 0;
    if (sp > (WS500_SRAM_BASE + WS500_SRAM_SIZE - 32u)) return 0;
    return 1;
}

/*
 * The funnel. Order is a safety property, not a style choice:
 *   1. field off — the only step that must never be skipped or delayed;
 *   2. crash record — evidence, written to `.noinit` which survives the reset;
 *   3. reset — a known-good state, with the escalation counter watching.
 */
static void fault_finish(crash_kind_t kind, uint32_t frame_sp,
                         uint32_t sp_is_psp, uint32_t exc_return)
{
    enter_safe_state();

    const uint32_t *frame = frame_plausible(frame_sp)
                          ? (const uint32_t *)frame_sp : (const uint32_t *)0;
    crash_record_write(kind, frame, frame_sp, sp_is_psp != 0u, exc_return);

    /* CMSIS: DSB, write AIRCR VECTKEY|SYSRESETREQ, DSB, then spin until the
     * reset takes effect. That spin is the one permitted "wait forever" in the
     * firmware — the field is already off and the core is being reset. */
    NVIC_SystemReset();
}

void fault_hardfault_stage2(uint32_t frame_sp, uint32_t sp_is_psp,
                            uint32_t exc_return)
{
    fault_finish(CRASH_KIND_HARDFAULT, frame_sp, sp_is_psp, exc_return);
}

void fault_nmi_stage2(uint32_t frame_sp, uint32_t sp_is_psp,
                      uint32_t exc_return)
{
    fault_finish(CRASH_KIND_NMI, frame_sp, sp_is_psp, exc_return);
}

void fault_unexpected_irq_stage2(uint32_t frame_sp, uint32_t sp_is_psp,
                                 uint32_t exc_return)
{
    fault_finish(CRASH_KIND_UNEXPECTED_IRQ, frame_sp, sp_is_psp, exc_return);
}

/* ---- Stage-1 entries (vector-table symbols) ------------------------------ */

/* HardFault: the only fault exception a Cortex-M0 has. Everything the bigger
 * cores would report as MemManage/BusFault/UsageFault escalates to here. */
__attribute__((naked, used)) void HardFault_Handler(void)
{
    FAULT_STAGE1_BODY(fault_hardfault_stage2);
}

/*
 * NMI. On the STM32F0 the SRAM parity error is routed to NMI when parity
 * checking is enabled, and §7 R3 treats it as fatal: memory contents can no
 * longer be trusted, so continuing would be worse than rebooting.
 *
 * bench-pending / TODO(GH#27): enabling the parity check itself is an OPTION
 * BYTE (`RAM_PARITY_CHECK` in the F072 user option bytes), not a runtime
 * register — firmware cannot turn it on for its own current boot, and
 * programming option bytes triggers an option-byte reload reset. This project
 * does not write option bytes from firmware (same policy as "no RDP/WRP, ever"
 * — PROJECT_PLAN §6): the handler is implemented and correct, and enabling the
 * parity check is recorded as a manual, M1-adjacent bench step to be done with
 * the DFU/SWD tooling that already has to be proven for backup/restore. Until
 * then this vector can still fire from a deliberately pended NMI, which is
 * exactly what the Renode fault test uses.
 */
__attribute__((naked, used)) void NMI_Handler(void)
{
    FAULT_STAGE1_BODY(fault_nmi_stage2);
}

/*
 * The unexpected-interrupt entry. The vendored ST startup file defines a
 * non-weak `Default_Handler` that is a bare `b .` loop and weak-aliases every
 * IRQ vector to it, so it cannot be overridden — instead each IRQ this
 * firmware does NOT claim is aliased to this handler below, which displaces
 * the spin with the funnel.
 *
 * MAINTENANCE RULE: when a driver starts using a peripheral interrupt, DELETE
 * its line from the alias list. Leaving it produces a duplicate-symbol link
 * error, which is the failure mode we want (loud, at build time).
 */
__attribute__((naked, used)) void ws500_unexpected_irq_entry(void)
{
    FAULT_STAGE1_BODY(fault_unexpected_irq_stage2);
}

#define WS500_UNCLAIMED_IRQ __attribute__((alias("ws500_unexpected_irq_entry"), used))

/* Core exceptions this bare-metal firmware never uses (no RTOS, no SVC). */
void SVC_Handler(void)    WS500_UNCLAIMED_IRQ;
void PendSV_Handler(void) WS500_UNCLAIMED_IRQ;

/* A funnel alias that YIELDS to a strong definition elsewhere. Used for
 * vectors a driver claims in the main firmware but which no linked driver
 * claims in the test-fw target (CMakeLists.txt builds two images from this
 * file). A plain alias would collide with the driver's own handler; leaving
 * the vector out entirely drops it to the startup file's Default_Handler,
 * which is a bare `b .` — a §7 R3 violation ("never a bare while(1)": spinning
 * with the field energised is the worst possible failure mode). Weak gives us
 * both: the driver wins where it exists, the funnel covers the image where it
 * doesn't. */
#define WS500_UNCLAIMED_IRQ_WEAK \
    __attribute__((weak, alias("ws500_unexpected_irq_entry"), used))

/* Peripheral IRQs, in vector-table order. CLAIMED elsewhere and therefore
 * absent from this list: EXTI4_15 (stator_rpm.c), SysTick (main.c).
 *
 * PVD_VDDIO2 is the one split case: pvd.c claims it in the main firmware, but
 * test-fw does not link pvd.c, so it gets the WEAK funnel alias below —
 * displaced by pvd.c's strong handler in ws500-openfw.elf, and covering the
 * otherwise-bare-while(1) vector in ws500-testfw.elf. (Unreachable in test-fw
 * either way — PWR_CR.PVDE is never set there — but §7 R3 is about not
 * leaving the trap armed, not about whether we expect it to fire.) */
void PVD_VDDIO2_IRQHandler(void)             WS500_UNCLAIMED_IRQ_WEAK;

void WWDG_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void RTC_IRQHandler(void)                    WS500_UNCLAIMED_IRQ;
void FLASH_IRQHandler(void)                  WS500_UNCLAIMED_IRQ;
void RCC_CRS_IRQHandler(void)                WS500_UNCLAIMED_IRQ;
void EXTI0_1_IRQHandler(void)                WS500_UNCLAIMED_IRQ;
void EXTI2_3_IRQHandler(void)                WS500_UNCLAIMED_IRQ;
void TSC_IRQHandler(void)                    WS500_UNCLAIMED_IRQ;
void DMA1_Channel1_IRQHandler(void)          WS500_UNCLAIMED_IRQ;
void DMA1_Channel2_3_IRQHandler(void)        WS500_UNCLAIMED_IRQ;
void DMA1_Channel4_5_6_7_IRQHandler(void)    WS500_UNCLAIMED_IRQ;
void ADC1_COMP_IRQHandler(void)              WS500_UNCLAIMED_IRQ;
void TIM1_BRK_UP_TRG_COM_IRQHandler(void)    WS500_UNCLAIMED_IRQ;
void TIM1_CC_IRQHandler(void)                WS500_UNCLAIMED_IRQ;
void TIM2_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void TIM3_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void TIM6_DAC_IRQHandler(void)               WS500_UNCLAIMED_IRQ;
void TIM7_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void TIM14_IRQHandler(void)                  WS500_UNCLAIMED_IRQ;
void TIM15_IRQHandler(void)                  WS500_UNCLAIMED_IRQ;
void TIM16_IRQHandler(void)                  WS500_UNCLAIMED_IRQ;
void TIM17_IRQHandler(void)                  WS500_UNCLAIMED_IRQ;
void I2C1_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void I2C2_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void SPI1_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void SPI2_IRQHandler(void)                   WS500_UNCLAIMED_IRQ;
void USART1_IRQHandler(void)                 WS500_UNCLAIMED_IRQ;
void USART2_IRQHandler(void)                 WS500_UNCLAIMED_IRQ;
void USART3_4_IRQHandler(void)               WS500_UNCLAIMED_IRQ;
void CEC_CAN_IRQHandler(void)                WS500_UNCLAIMED_IRQ;
/* USB_IRQHandler: CLAIMED by Core/Src/usb_cdc.c (GH#35, 2026-07-27) — alias
 * removed per the maintenance rule above (the duplicate-symbol link error this
 * line caused was the designed hand-off signal). */

/* ---- Non-exception entries into the same funnel -------------------------- */

void Error_Handler(void)
{
    /* No exception frame exists here — the caller's context is ordinary C —
     * so the record stores the return address as the "PC" via __builtin_
     * return_address, which is the useful half of what a frame would give. */
    enter_safe_state();
    uint32_t frame[8] = {0};
    frame[5] = (uint32_t)(uintptr_t)__builtin_return_address(0);  /* LR slot */
    frame[6] = frame[5];                                          /* PC slot */
    crash_record_write(CRASH_KIND_ERROR_HANDLER, frame, 0u, false, 0u);
    NVIC_SystemReset();
}

void fault_assert_failed(uint32_t file_id, uint32_t line)
{
    enter_safe_state();
    uint32_t frame[8] = {0};
    frame[5] = file_id;   /* LR slot carries WHICH file  */
    frame[6] = line;      /* PC slot carries WHICH line  */
    crash_record_write(CRASH_KIND_ASSERT, frame, 0u, false, 0u);
    NVIC_SystemReset();
}

#ifdef USE_FULL_ASSERT
/* HAL parameter asserts route to the same place (§7 "Assert policy": HAL
 * assert_param routes to the same macro"). Only the line number is portable
 * here; the file pointer is a rodata address we deliberately do not chase. */
void assert_failed(uint8_t *file, uint32_t line)
{
    (void)file;
    fault_assert_failed(WS500_FILE_ID('H', 'A', 'L', '_'), line);
}
#endif
