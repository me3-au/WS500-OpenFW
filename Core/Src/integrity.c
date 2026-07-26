/*
 * integrity.c — R4: boot-time image CRC + stack painting (PROJECT_PLAN §7 R4).
 * Contract: integrity.h. Linker plumbing: STM32F072xB_FLASH.ld (`.fw_crc`,
 * `.stack`, `_fw_image_start`). Build step: scripts/embed_crc.py.
 *
 * MATCHING zlib's CRC-32 WITH THE STM32 CRC UNIT. The build-time value is
 * computed by Python's zlib.crc32 — the ordinary, reflected CRC-32 everyone
 * else's tools also produce (poly 0x04C11DB7, init 0xFFFFFFFF, reflected in
 * and out, final XOR 0xFFFFFFFF). The STM32 peripheral computes the
 * NON-reflected form by default, so it has to be told:
 *     REV_IN  = 0b11  — reverse the input bit order by WORD, which combined
 *                       with feeding little-endian words straight out of flash
 *                       reproduces the byte-stream order zlib sees;
 *     REV_OUT = 1     — reverse the output bit order;
 *     INIT    = 0xFFFFFFFF (reset default), POLY = 0x04C11DB7 (reset default);
 *     the final XOR with 0xFFFFFFFF is done in software (the unit has no
 *     output-XOR stage).
 * Getting this wrong would silently report every good image as corrupt, so the
 * unit is self-tested against a known vector before it is trusted.
 *
 * WHY A MISMATCH DOES NOT BRICK THE UNIT. Tempting, but wrong for this device:
 * a false positive (a bad self-test, an un-patched build, a partially erased
 * last page) would leave a boat with no charging, and the code deciding to
 * stop is the same possibly-corrupt code the check just failed. The check
 * therefore REPORTS — status here, crash record, and (once #20 lands)
 * telemetry — and the decision to refuse to run stays with the operator and
 * with M4's update flow, where a bad CRC means "do not install", which is the
 * question this primitive really exists to answer. [SPEC-SIGNOFF]
 *
 * SPDX-License-Identifier: MIT
 */
#include "integrity.h"
#include "stm32f0xx.h"
#include <stddef.h>
#include <stdbool.h>

/* ---- Linker-provided geometry ------------------------------------------- */
extern uint32_t _fw_image_start[];  /* ORIGIN(FLASH) */
extern uint32_t _sstack[];          /* lowest stack address (bottom of RAM) */
extern uint32_t _estack[];          /* initial MSP (top of the stack region) */

/*
 * The stored image CRC. Placed alone in `.fw_crc`, which the linker script
 * puts LAST in flash, so it is the final word of the .bin and
 * scripts/embed_crc.py can patch it without parsing the map file.
 *
 * `volatile` is load-bearing: without it the compiler would fold the
 * initialiser into the comparison and the check would always compare the
 * placeholder against itself. The placeholder 0xFFFFFFFF also happens to be
 * erased-flash's value, which is exactly the right "nobody wrote this" signal.
 */
__attribute__((section(".fw_crc"), used))
volatile const uint32_t g_fw_crc = 0xFFFFFFFFu;

#define FW_CRC_PLACEHOLDER  0xFFFFFFFFu

/* zlib.crc32(b"\x00\x00\x00\x00") — verified with Python, not from memory. */
#define CRC_SELFTEST_INPUT    0x00000000u
#define CRC_SELFTEST_EXPECTED 0x2144DF1Cu

static integrity_crc_status_t s_crc_status = INTEGRITY_CRC_UNAVAILABLE;
static uint32_t s_crc_computed;
static uint32_t s_crc_stored;

/* Stack-painting state. */
#define STACK_PAINT_WORD    0xA5A5A5A5u
/* Words left untouched below the caller's frame when painting, so painting
 * cannot overwrite its own return address or a compiler-chosen spill slot. */
#define STACK_PAINT_GUARD_WORDS  32u
/* Words examined per housekeeping call — bounded work in the control loop. */
#define STACK_SCAN_STEP_WORDS    64u

static uint32_t *s_scan;        /* lowest word not yet proven still painted */
static uint32_t  s_used_max;

/* ---- Hardware CRC-32 ----------------------------------------------------- */

static void crc_configure(void)
{
    RCC->AHBENR |= RCC_AHBENR_CRCEN;
    /* POLYSIZE = 00 (32-bit) is the reset value; set REV_IN = 11, REV_OUT = 1.
     * RESET is written last so the unit restarts from INIT with the new
     * reversal settings applied. */
    CRC->CR = CRC_CR_REV_IN_0 | CRC_CR_REV_IN_1 | CRC_CR_REV_OUT;
    CRC->INIT = 0xFFFFFFFFu;
    CRC->CR |= CRC_CR_RESET;
}

static uint32_t crc_of_words(const uint32_t *p, uint32_t words)
{
    crc_configure();
    for (uint32_t i = 0; i < words; i++) CRC->DR = p[i];
    return CRC->DR ^ 0xFFFFFFFFu;   /* the unit has no output-XOR stage */
}

static int crc_unit_selftest(void)
{
    const uint32_t v = CRC_SELFTEST_INPUT;
    return crc_of_words(&v, 1u) == CRC_SELFTEST_EXPECTED;
}

static void check_image(void)
{
    s_crc_stored = g_fw_crc;

    if (!crc_unit_selftest()) {
        /* Do not report a mismatch we cannot stand behind. */
        s_crc_status = INTEGRITY_CRC_UNAVAILABLE;
        return;
    }

    if (s_crc_stored == FW_CRC_PLACEHOLDER) {
        /* Un-patched build (e.g. the ELF the Renode job loads, which never
         * goes through objcopy + embed_crc.py). Still compute the value so it
         * can be read out and compared by hand. */
        s_crc_status = INTEGRITY_CRC_UNPATCHED;
    }

    const uint32_t *start = _fw_image_start;
    const uint32_t *end   = (const uint32_t *)(uintptr_t)&g_fw_crc;
    if (end <= start) { s_crc_status = INTEGRITY_CRC_UNAVAILABLE; return; }

    s_crc_computed = crc_of_words(start, (uint32_t)(end - start));

    if (s_crc_status != INTEGRITY_CRC_UNPATCHED) {
        s_crc_status = (s_crc_computed == s_crc_stored)
                     ? INTEGRITY_CRC_OK : INTEGRITY_CRC_MISMATCH;
    }
}

/* ---- Stack painting ------------------------------------------------------ */

static void paint_stack(void)
{
    /* Everything from the bottom of the region up to (this frame - guard) is
     * unused by definition: nothing has run deeper than the current call yet.
     * `marker` is a stack object, so its address IS the current stack depth —
     * more honest than reading MSP and then calling a function. */
    uint32_t marker;
    uint32_t *top = (uint32_t *)((uintptr_t)&marker) - STACK_PAINT_GUARD_WORDS;
    for (uint32_t *p = _sstack; p < top; p++) *p = STACK_PAINT_WORD;
    s_scan = _sstack;
    s_used_max = 0u;
}

void integrity_housekeeping(void)
{
    if (s_scan == NULL) return;

    /* The watermark only ever moves UP (the stack only ever gets deeper), so
     * resuming from the last known-painted word is correct and makes the
     * amortised cost of this scan effectively zero. */
    const uint32_t *limit = _estack;
    bool found = false;
    for (uint32_t i = 0; i < STACK_SCAN_STEP_WORDS; i++) {
        if (s_scan >= limit || *s_scan != STACK_PAINT_WORD) {
            found = true;                         /* watermark located */
            break;
        }
        s_scan++;
    }

    /* Only record a reading once the scan has actually REACHED the boundary.
     * A partial sweep (step budget exhausted) still has s_scan somewhere below
     * the true watermark, and since s_used_max is a maximum, publishing that
     * would latch a permanent over-estimate — on the very first call it would
     * latch "the entire region is used". */
    if (found) {
        const uint32_t used = (uint32_t)((uintptr_t)_estack - (uintptr_t)s_scan);
        if (used > s_used_max) s_used_max = used;
    }
}

void integrity_init(void)
{
    paint_stack();
    check_image();
}

integrity_crc_status_t integrity_crc_status(void) { return s_crc_status; }
uint32_t integrity_crc_computed(void) { return s_crc_computed; }
uint32_t integrity_crc_stored(void)   { return s_crc_stored; }
uint32_t integrity_stack_used_max(void) { return s_used_max; }

uint32_t integrity_stack_free_min(void)
{
    if (s_scan == NULL) return 0u;
    return (uint32_t)((uintptr_t)s_scan - (uintptr_t)_sstack);
}
