/*
 * crash_record.h — R2: reset-cause decode + crash-record ring (PROJECT_PLAN §7 R2).
 *
 * "A fault you can't read out didn't happen." Two jobs:
 *
 *   1. Decode RCC->CSR once per boot into named reset causes, and keep a
 *      per-cause counter that survives resets (POR / NRST / software / IWDG /
 *      WWDG / low-power / option-byte-load).
 *   2. Keep a small ring of crash records in a `.noinit` RAM section — RAM
 *      that the startup code neither zeroes nor initialises, so it carries
 *      across a warm reset but is garbage after a cold power-up. A magic word
 *      plus a CRC-32 over the block distinguishes "valid, survived a reset"
 *      from "cold boot, random bits".
 *
 * The stock WS500 does neither (it tracks resets with a bare `0xDEADBEEF` RAM
 * marker and never reads RCC->CSR — §7 "stock prior art"); this is one of the
 * three places §7 deliberately improves on it.
 *
 * Boundary rules: this module is written to be callable from a fault handler
 * (no HAL beyond HAL_GetTick(), no allocation, bounded loops, no blocking) and
 * holds NO control-core types — main.c pushes the regulator snapshot in via
 * crash_state_t, so Core/ stays free of control/ headers.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CRASH_RECORD_H
#define WS500_CRASH_RECORD_H

#include <stdint.h>
#include <stdbool.h>

/* Reset causes, one bit each; several can be set for one boot (e.g. POR+PIN). */
typedef enum {
    RESET_CAUSE_POR      = 1u << 0,   /* power-on / power-down reset */
    RESET_CAUSE_PIN      = 1u << 1,   /* NRST pin */
    RESET_CAUSE_SOFTWARE = 1u << 2,   /* NVIC_SystemReset() — our fault path */
    RESET_CAUSE_IWDG     = 1u << 3,   /* independent watchdog expiry */
    RESET_CAUSE_WWDG     = 1u << 4,   /* window watchdog (unused by us) */
    RESET_CAUSE_LOWPOWER = 1u << 5,   /* illegal low-power entry */
    RESET_CAUSE_OBL      = 1u << 6,   /* option-byte reload */
    RESET_CAUSE_COUNT    = 7          /* number of distinct causes above */
} reset_cause_t;

/* What produced a crash record. Stored in the record, surfaced in telemetry. */
typedef enum {
    CRASH_KIND_NONE          = 0,
    CRASH_KIND_HARDFAULT     = 1,   /* HardFault exception (the only fault
                                     * exception a Cortex-M0 has) */
    CRASH_KIND_NMI           = 2,   /* NMI — SRAM parity error (§7 R3) */
    CRASH_KIND_UNEXPECTED_IRQ = 3,  /* an interrupt nothing claims */
    CRASH_KIND_ERROR_HANDLER = 4,   /* Error_Handler() / HAL init failure */
    CRASH_KIND_ASSERT        = 5,   /* WS500_ASSERT / HAL assert_param */
    CRASH_KIND_BROWNOUT      = 6,   /* PVD: Vdd falling through the threshold */
    CRASH_KIND_WATCHDOG_LIMP = 7    /* booted into LIMP-and-stay (§7 R1) */
} crash_kind_t;

/*
 * Regulator snapshot stored with each crash record. Deliberately a small,
 * fixed, control-type-free struct: main.c refreshes it every control tick
 * (crash_record_set_state) so a fault handler never has to reach into the
 * control core to describe what the regulator was doing.
 *
 * TODO(GH#27): temps and the resolved binding ceiling are not wired yet —
 * they arrive with the telemetry deliverable (#20).
 */
typedef struct {
    float    vbat_v;        /* last measured pack voltage [V], NAN if unknown */
    float    amps_batt;     /* last resolved battery current [A], signed */
    float    field_duty;    /* last COMMANDED duty 0..1 (not measured) */
    uint32_t ctrl_faults;   /* ctrl_command_t.faults bitmask at the time */
    uint32_t ext_faults;    /* ext_faults the app fed into the engine */
    uint32_t loop_count;    /* control ticks since boot */
    uint8_t  ctrl_state;    /* ctrl_state_t as a plain number */
    uint8_t  field_open;    /* engine asked for hardware field-off */
    uint8_t  field_latched; /* field_drive_faulted() */
    uint8_t  reserved;
} crash_state_t;

/* One crash record. `sp` is the stack pointer that held the exception frame;
 * this firmware is single-stack (bare metal, no RTOS) so it is MSP in
 * practice, but sp_is_psp records which one it actually was. */
typedef struct {
    uint32_t seq;         /* monotonic, never reused; 0 = empty slot */
    uint32_t uptime_ms;   /* HAL tick at the time of the fault */
    uint32_t pc;          /* stacked PC — the faulting instruction */
    uint32_t lr;          /* stacked LR — who called the faulting code */
    uint32_t xpsr;        /* stacked xPSR (bits 8:0 = the active exception) */
    uint32_t sp;          /* SP holding the frame (0 if it was implausible) */
    uint32_t exc_return;  /* EXC_RETURN in LR on entry */
    uint16_t kind;        /* crash_kind_t */
    uint8_t  sp_is_psp;   /* 1 = frame was on the process stack */
    uint8_t  reset_cause; /* reset_cause_t bits of the boot it happened in */
    crash_state_t state;
} crash_record_t;

/* How many records the ring holds. 4 keeps the whole `.noinit` block under
 * ~300 B of the part's 16 KB while still showing a repeating-fault pattern
 * (which is what you actually need to diagnose a reset loop). [SPEC-SIGNOFF] */
#define CRASH_RING_LEN  4u

/*
 * Decode this boot's reset cause and validate the `.noinit` block. Call ONCE,
 * as early in main() as possible and before anything that can fault — it is
 * what makes every later crash_record_* call safe. Clears the RCC reset flags
 * so the next boot reports its own cause, bumps the per-cause counters and the
 * consecutive-fault streaks, and re-initialises the block from scratch if the
 * magic or CRC says it is cold-boot garbage.
 */
void crash_record_boot(void);

/* Reset causes decoded for THIS boot (bitwise OR of reset_cause_t). */
uint32_t crash_reset_cause(void);

/* Lifetime count for one cause. `cause` is a single reset_cause_t bit;
 * returns 0 for a mask with anything other than exactly one known bit. */
uint32_t crash_reset_cause_count(reset_cause_t cause);

/* Boots since the `.noinit` block was last cold-initialised. */
uint32_t crash_boot_count(void);

/* Consecutive IWDG-caused boots (§7 R1 escalation counter). */
uint8_t crash_iwdg_streak(void);

/* Consecutive boots caused by our own fault-path reset (§7 R3). */
uint8_t crash_fault_streak(void);

/*
 * True when this boot must come up field-off and STAY there: either streak has
 * reached CRASH_LIMP_STREAK. Read by main.c, which turns it into the existing
 * CTRL_FAULT_WATCHDOG input to the control core — no control-core change and
 * no second, competing safe-mode concept (§7 R1 "boot straight into field-off
 * LIMP and stay there; don't oscillate reset<->run").
 */
bool crash_limp_latched(void);

/* Consecutive fault/watchdog boots that trip LIMP-and-stay. Three is the §7 R1
 * "N": one is noise (a glitch, a brown-out), two can still be coincidence,
 * three is a reproducible fault that will not fix itself by rebooting a fourth
 * time. [SPEC-SIGNOFF] */
#define CRASH_LIMP_STREAK  3u

/*
 * Declare the system healthy: zero the streaks so a later, unrelated fault
 * starts counting from scratch. watchdog.c calls this once the loop has run
 * cleanly for its healthy-runtime budget. No effect on the records or the
 * lifetime counters.
 */
void crash_clear_streaks(void);

/* Refresh the regulator snapshot that will be stored with the NEXT crash
 * record. A plain struct copy — no CRC work — so it is cheap enough to call
 * every 10 ms control tick. */
void crash_record_set_state(const crash_state_t *st);

/*
 * Append a crash record and re-seal the block. Called from the fault C stage
 * with the values recovered from the stacked exception frame; `frame` may be
 * NULL when there is no frame to report (assert / brown-out), in which case
 * pc/lr/xpsr/sp are recorded as 0. Never blocks; safe from any context.
 */
void crash_record_write(crash_kind_t kind, const uint32_t *frame,
                        uint32_t frame_sp, bool sp_is_psp, uint32_t exc_return);

/* Records currently held (0..CRASH_RING_LEN). */
uint8_t crash_record_count(void);

/* Newest record is index 0. Returns NULL when idx >= crash_record_count(). */
const crash_record_t *crash_record_get(uint8_t idx);

/*
 * Recompute the block's CRC so it survives the next reset. crash_record_write
 * and crash_record_set_state already do this; the PVD path calls it directly
 * to seal whatever is in RAM before the supply collapses (§7 R5).
 */
void crash_record_flush(void);

#endif /* WS500_CRASH_RECORD_H */
