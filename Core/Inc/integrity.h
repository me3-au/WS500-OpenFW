/*
 * integrity.h — R4: image CRC + stack painting (PROJECT_PLAN §7 R4).
 *
 * Two independent "is this firmware still the firmware?" checks:
 *
 *  1. FLASH IMAGE CRC-32, computed at boot by the hardware CRC unit over the
 *     whole image and compared against a word the build patched into the last
 *     four bytes of the binary (scripts/embed_crc.py + the `.fw_crc` section in
 *     STM32F072xB_FLASH.ld). This is also the update-verification primitive
 *     M4 needs — the same computation validates a downloaded image.
 *
 *  2. STACK PAINTING + HIGH-WATERMARK. The Cortex-M0 has no MPU and no stack
 *     limit register, so nothing detects an overflow. §7 R4 puts the stack at
 *     the BOTTOM of RAM (linker script) so an overflow faults into unmapped
 *     memory instead of quietly eating `.bss`; painting the region and tracking
 *     how far the paint has been eaten turns "we have not overflowed yet" into
 *     a number you can watch shrink.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_INTEGRITY_H
#define WS500_INTEGRITY_H

#include <stdint.h>

typedef enum {
    INTEGRITY_CRC_OK = 0,    /* stored word matches the computed CRC */
    INTEGRITY_CRC_MISMATCH,  /* stored word differs — image or flash damaged */
    INTEGRITY_CRC_UNPATCHED, /* stored word is the placeholder: this build was
                              * never passed through scripts/embed_crc.py (an
                              * ELF loaded by a debugger/emulator, or a local
                              * build). Distinct from MISMATCH on purpose. */
    INTEGRITY_CRC_UNAVAILABLE /* the CRC unit failed its own self-test */
} integrity_crc_status_t;

/*
 * Paint the unused stack and verify the flash image. Call once, early in
 * main(), before any deep call chain — painting writes every stack word below
 * the caller's frame, so anything already on the stack above it survives but
 * anything BELOW is destroyed (there is nothing there yet at that point).
 */
void integrity_init(void);

/* Result of the boot-time image check. */
integrity_crc_status_t integrity_crc_status(void);

/* The CRC-32 computed over the image at boot, and the word stored in the
 * image. Equal when integrity_crc_status() == INTEGRITY_CRC_OK. */
uint32_t integrity_crc_computed(void);
uint32_t integrity_crc_stored(void);

/*
 * Advance the stack high-watermark scan. Cheap and bounded (a fixed small
 * number of words per call, resuming where it left off), so it can be called
 * every control cycle from housekeeping. The watermark only ever moves in one
 * direction, which is what makes the incremental scan valid.
 */
void integrity_housekeeping(void);

/* Deepest stack usage observed so far [bytes], and how much painted stack is
 * still untouched [bytes]. A free-min trending toward zero is the warning that
 * _Min_Stack_Size in the linker script needs raising. */
uint32_t integrity_stack_used_max(void);
uint32_t integrity_stack_free_min(void);

#endif /* WS500_INTEGRITY_H */
