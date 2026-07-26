/*
 * crash_record.c — R2: reset-cause decode + `.noinit` crash-record ring
 * (PROJECT_PLAN §7 R2). Contract and struct docs: crash_record.h.
 *
 * WHERE THE BLOCK LIVES. `s_p` is placed in the `.noinit` output section that
 * STM32F072xB_FLASH.ld carves out after `.bss` (see the linker script's own
 * comment block). The startup code copies `.data` and zeroes `.bss` only, so
 * `.noinit` keeps its bits across every warm reset — watchdog, software,
 * NRST — and holds whatever the SRAM powered up with after a cold start.
 *
 * WHY MAGIC **AND** CRC. The magic alone cannot tell "surviving data" from
 * "random SRAM that happens to start with our word"; the CRC alone cannot be
 * evaluated cheaply on garbage of unknown length. Together they are a strong
 * enough gate for a diagnostic structure: if either check fails we throw the
 * block away and start clean rather than reporting invented history. The CRC
 * is a bitwise CRC-32 computed in software rather than through the hardware
 * CRC unit, because this code runs in fault context: the CRC peripheral is
 * shared state (integrity.c uses it) and re-entering it from an exception
 * could corrupt an in-progress calculation. ~250 B * 8 iterations is a few
 * tens of microseconds at 48 MHz — irrelevant next to a reset.
 *
 * SPDX-License-Identifier: MIT
 */
#include "crash_record.h"
#include "stm32f0xx.h"
#include "stm32f0xx_hal.h"      /* HAL_GetTick() only — a plain accessor */

/* Magic: "WS5C" (WS500 Crash). Any change to the layout below MUST change this
 * value, otherwise an old block would be accepted with a new interpretation. */
#define CRASH_MAGIC   0x57533543u

typedef struct {
    uint32_t magic;
    uint32_t boot_count;
    uint32_t cause_count[RESET_CAUSE_COUNT];
    uint32_t next_seq;
    uint32_t seq_at_boot;   /* next_seq as of the last completed boot — lets us
                             * tell "a record was written since we last booted"
                             * from "there is an old record in the ring" */
    uint8_t  head;          /* index of the NEXT slot to write */
    uint8_t  count;         /* valid records, 0..CRASH_RING_LEN */
    uint8_t  iwdg_streak;
    uint8_t  fault_streak;
    crash_record_t ring[CRASH_RING_LEN];
    uint32_t crc;           /* CRC-32 over every byte above */
} crash_persist_t;

/* `used` keeps --gc-sections from discarding it; the linker script also KEEPs
 * the section. Alignment is 4 so the CRC can walk it as words. */
static crash_persist_t s_p __attribute__((section(".noinit"), used, aligned(4)));

/* This boot's decoded cause; plain .bss (re-derived every boot). */
static uint32_t s_boot_cause;

/*
 * The live regulator snapshot lives in ordinary .bss, NOT in the CRC-protected
 * `.noinit` block, and that is a deliberate performance decision: main.c
 * refreshes it every 10 ms control tick, and re-sealing a ~300-byte block with
 * a bitwise CRC at 100 Hz would burn real CPU for nothing. It only has to be
 * correct at the instant a fault copies it INTO a record — and that copy is
 * sealed, because crash_record_write() re-computes the block CRC.
 */
static crash_state_t s_live;

/* ---------------------------------------------------------------------- */

/* Bitwise CRC-32 (poly 0x04C11DB7 reflected = 0xEDB88320, init/final 0xFFFF
 * FFFF) — the same parameters embed_crc.py and integrity.c use, so one
 * definition of "CRC-32" holds across the firmware. No table: 1 KB of rodata
 * is worth more than the microseconds here. */
static uint32_t crc32_bytes(const uint8_t *p, uint32_t len)
{
    uint32_t crc = 0xFFFFFFFFu;
    for (uint32_t i = 0; i < len; i++) {
        crc ^= p[i];
        for (uint32_t b = 0; b < 8u; b++)
            crc = (crc >> 1) ^ (0xEDB88320u & (uint32_t)(-(int32_t)(crc & 1u)));
    }
    return crc ^ 0xFFFFFFFFu;
}

static uint32_t block_crc(void)
{
    return crc32_bytes((const uint8_t *)&s_p,
                       (uint32_t)(sizeof(s_p) - sizeof(s_p.crc)));
}

void crash_record_flush(void) { s_p.crc = block_crc(); }

static void block_reset(void)
{
    uint8_t *p = (uint8_t *)&s_p;
    for (uint32_t i = 0; i < sizeof(s_p); i++) p[i] = 0u;
    s_p.magic       = CRASH_MAGIC;
    s_p.next_seq    = 1u;
    s_p.seq_at_boot = 1u;
    crash_record_flush();
}

/* Map RCC->CSR's reset flags to our cause bits. F072 RM0091 §7.4.10: bit 31
 * LPWRRSTF, 30 WWDGRSTF, 29 IWDGRSTF, 28 SFTRSTF, 27 PORRSTF, 26 PINRSTF,
 * 25 OBLRSTF; bit 24 RMVF clears them all. */
static uint32_t decode_reset_flags(uint32_t csr)
{
    uint32_t c = 0u;
    if (csr & RCC_CSR_PORRSTF)  c |= RESET_CAUSE_POR;
    if (csr & RCC_CSR_PINRSTF)  c |= RESET_CAUSE_PIN;
    if (csr & RCC_CSR_SFTRSTF)  c |= RESET_CAUSE_SOFTWARE;
    if (csr & RCC_CSR_IWDGRSTF) c |= RESET_CAUSE_IWDG;
    if (csr & RCC_CSR_WWDGRSTF) c |= RESET_CAUSE_WWDG;
    if (csr & RCC_CSR_LPWRRSTF) c |= RESET_CAUSE_LOWPOWER;
    if (csr & RCC_CSR_OBLRSTF)  c |= RESET_CAUSE_OBL;
    return c;
}

/* Index of a single-bit cause, or -1. Keeps the counter array dense without
 * exposing array indices in the public API. */
static int cause_index(uint32_t bit)
{
    for (int i = 0; i < (int)RESET_CAUSE_COUNT; i++)
        if (bit == (1u << i)) return i;
    return -1;
}

void crash_record_boot(void)
{
    const uint32_t csr = RCC->CSR;
    s_boot_cause = decode_reset_flags(csr);
    /* Clear the flags now: they are sticky, so leaving them set would make the
     * NEXT boot report this boot's cause as well as its own. */
    RCC->CSR |= RCC_CSR_RMVF;

    if (s_p.magic != CRASH_MAGIC || s_p.crc != block_crc() ||
        s_p.head >= CRASH_RING_LEN || s_p.count > CRASH_RING_LEN) {
        /* Cold power-up (or a block we can no longer trust): start clean.
         * POR is the expected reason to land here; anything else means RAM
         * was disturbed, and inventing history would be worse than losing it. */
        block_reset();
    }

    s_p.boot_count++;
    for (int i = 0; i < (int)RESET_CAUSE_COUNT; i++)
        if (s_boot_cause & (1u << i)) s_p.cause_count[i]++;

    /* Streaks: "consecutive" means consecutive boots, so a boot that was NOT
     * caused by the mechanism resets that mechanism's streak. A power cycle
     * (POR) or a deliberate NRST clears both — the operator has intervened,
     * and holding a latched LIMP across that would be user-hostile and is not
     * what §7 R1 asks for (it is about not oscillating reset<->run on its
     * own). */
    if (s_boot_cause & RESET_CAUSE_IWDG) {
        if (s_p.iwdg_streak < 255u) s_p.iwdg_streak++;
    } else if (s_boot_cause & (RESET_CAUSE_POR | RESET_CAUSE_PIN)) {
        s_p.iwdg_streak = 0u;
    }

    /* A software reset is OURS (the fault path) only if a crash record was
     * written since the previous boot. SFTRSTF alone cannot distinguish that
     * from a future DFU-entry or config-applied reboot, and "the ring is
     * non-empty" cannot either — that record may be hours and several healthy
     * boots old. Comparing the sequence counter answers the actual question. */
    const bool record_since_last_boot = (s_p.next_seq != s_p.seq_at_boot);
    const bool after_fault_record =
        (s_boot_cause & RESET_CAUSE_SOFTWARE) && record_since_last_boot;
    if (after_fault_record) {
        if (s_p.fault_streak < 255u) s_p.fault_streak++;
    } else if (s_boot_cause & (RESET_CAUSE_POR | RESET_CAUSE_PIN)) {
        s_p.fault_streak = 0u;
    }

    s_p.seq_at_boot = s_p.next_seq;
    crash_record_flush();
}

uint32_t crash_reset_cause(void) { return s_boot_cause; }

uint32_t crash_reset_cause_count(reset_cause_t cause)
{
    const int i = cause_index((uint32_t)cause);
    return (i < 0) ? 0u : s_p.cause_count[i];
}

uint32_t crash_boot_count(void)   { return s_p.boot_count; }
uint8_t  crash_iwdg_streak(void)  { return s_p.iwdg_streak; }
uint8_t  crash_fault_streak(void) { return s_p.fault_streak; }

bool crash_limp_latched(void)
{
    return (s_p.iwdg_streak  >= CRASH_LIMP_STREAK) ||
           (s_p.fault_streak >= CRASH_LIMP_STREAK);
}

void crash_clear_streaks(void)
{
    if (s_p.iwdg_streak == 0u && s_p.fault_streak == 0u) return;
    s_p.iwdg_streak  = 0u;
    s_p.fault_streak = 0u;
    crash_record_flush();
}

void crash_record_set_state(const crash_state_t *st)
{
    if (st == NULL) return;
    s_live = *st;   /* no CRC here — see the s_live comment above */
}

/*
 * CONCURRENCY. This is called from exception context (fault handlers, the PVD
 * ISR) and can therefore interrupt main-context code that touches the block —
 * in practice only crash_clear_streaks(), which writes two bytes and re-seals.
 * An ISR always runs to completion before main resumes, so the ISR's record is
 * always internally consistent and correctly sealed; the worst outcome is that
 * main's subsequent flush re-seals a block the ISR already updated, which is
 * harmless. No critical section is used deliberately: masking interrupts on the
 * path that has to work when everything else has failed would add a way for
 * this code to make things worse.
 */
void crash_record_write(crash_kind_t kind, const uint32_t *frame,
                        uint32_t frame_sp, bool sp_is_psp, uint32_t exc_return)
{
    /* Defend against a block that a fault corrupted between boot and now:
     * write into a sane slot rather than off the end of the array. */
    if (s_p.head >= CRASH_RING_LEN) s_p.head = 0u;

    crash_record_t *r = &s_p.ring[s_p.head];
    r->seq        = s_p.next_seq++;
    r->uptime_ms  = HAL_GetTick();
    r->kind       = (uint16_t)kind;
    r->sp_is_psp  = sp_is_psp ? 1u : 0u;
    r->reset_cause = (uint8_t)s_boot_cause;
    r->sp         = frame ? frame_sp : 0u;
    r->exc_return = exc_return;
    /* Stacked frame layout (ARMv6-M): r0 r1 r2 r3 r12 LR PC xPSR. */
    r->lr         = frame ? frame[5] : 0u;
    r->pc         = frame ? frame[6] : 0u;
    r->xpsr       = frame ? frame[7] : 0u;
    r->state      = s_live;

    s_p.head = (uint8_t)((s_p.head + 1u) % CRASH_RING_LEN);
    if (s_p.count < CRASH_RING_LEN) s_p.count++;
    crash_record_flush();
}

uint8_t crash_record_count(void) { return s_p.count; }

const crash_record_t *crash_record_get(uint8_t idx)
{
    if (idx >= s_p.count) return NULL;
    /* head points at the next slot to write, so head-1 is the newest. */
    const uint32_t slot = ((uint32_t)s_p.head + CRASH_RING_LEN - 1u - idx) %
                          CRASH_RING_LEN;
    return &s_p.ring[slot];
}
