/* STM32F072xB — 128 KB flash @ 0x08000000, 16 KB SRAM @ 0x20000000.
 * Memory sizes recovered from the stock image (vector SP=0x20003FB0, ~111KB used).
 *
 * Two robustness features from PROJECT_PLAN §7 are implemented HERE rather than
 * in C, because only the linker can place memory:
 *
 * §7 R4 — STACK AT THE BOTTOM OF RAM. The `.stack` region is allocated FIRST,
 *   so it occupies 0x20000000..0x20000000+_Min_Stack_Size and grows DOWN out of
 *   the chip's SRAM into unmapped address space. On a Cortex-M0 there is no MPU
 *   and no stack-limit register, so this placement is the only way to make a
 *   stack overflow a *fault* instead of a silent corruption of `.bss`. Overrun
 *   the stack and the next push aborts -> HardFault -> §7 R3 handler -> safe
 *   state + crash record + reboot. With the old top-of-RAM stack the same
 *   overflow would have quietly eaten the config, the crash records, and the
 *   control state, and kept running.
 *   The consequence to keep in mind: the stack is now a FIXED size, not
 *   "everything left over". _Min_Stack_Size is therefore a real budget —
 *   integrity.c paints the region and reports the high-watermark so the number
 *   can be set from evidence.
 *
 * §7 R2 — `.noinit` FOR CRASH RECORDS. `.noinit` is placed after `.bss` and
 *   marked NOLOAD: the startup code copies `.data` and zeroes `.bss` only, so
 *   nothing touches these bytes and they survive every warm reset (IWDG,
 *   software, NRST). crash_record.c gates them with a magic word + CRC-32 so a
 *   cold power-up's random SRAM cannot masquerade as history. NOLOAD also keeps
 *   the block out of the .bin (it has no initial value to load).
 *
 * §7 R4 — FLASH CRC SLOT. `.fw_crc` is the LAST section in FLASH, after the
 *   `.data` load image, and holds exactly one word. `objcopy -O binary`
 *   therefore ends the .bin with that word, which is what lets
 *   scripts/embed_crc.py patch it without parsing the map file: CRC-32 of
 *   everything before it, written into the last four bytes. integrity.c
 *   recomputes the same range at boot.
 *
 * SPDX-License-Identifier: MIT */
ENTRY(Reset_Handler)

/* Stack budget (see §7 R4 note above). 4 KB of the 16 KB SRAM: main()'s frame
 * holds the engine context plus the per-tick measured/ceiling/command/telemetry
 * structs (~600 B measured from the type definitions), and the deepest call
 * chains are HAL I2C/ADC plus soft-float helpers. 4 KB is ~6x that worst-case
 * estimate, which is the right side of the trade while `.bss` still fits with
 * room to spare. [SPEC-SIGNOFF] — replace this estimate with the first
 * integrity_stack_used_max() reading from the emulation/bench run. */
_Min_Heap_Size  = 0x200;
_Min_Stack_Size = 0x1000;

MEMORY
{
  FLASH (rx) : ORIGIN = 0x08000000, LENGTH = 128K
  RAM  (xrw) : ORIGIN = 0x20000000, LENGTH = 16K
}

/* First byte covered by the boot-time image CRC (§7 R4). */
_fw_image_start = ORIGIN(FLASH);

SECTIONS
{
  .isr_vector : { . = ALIGN(4); KEEP(*(.isr_vector)) . = ALIGN(4); } >FLASH
  .text : {
    . = ALIGN(4);
    *(.text) *(.text*) *(.glue_7) *(.glue_7t) *(.eh_frame)
    KEEP(*(.init)) KEEP(*(.fini)) . = ALIGN(4); _etext = .;
  } >FLASH
  .rodata : { . = ALIGN(4); *(.rodata) *(.rodata*) . = ALIGN(4); } >FLASH
  .ARM.extab : { *(.ARM.extab* .gnu.linkonce.armextab.*) } >FLASH
  .ARM : { __exidx_start = .; *(.ARM.exidx*) __exidx_end = .; } >FLASH
  .preinit_array : { PROVIDE_HIDDEN(__preinit_array_start = .); KEEP(*(.preinit_array*)) PROVIDE_HIDDEN(__preinit_array_end = .); } >FLASH
  .init_array : { PROVIDE_HIDDEN(__init_array_start = .); KEEP(*(SORT(.init_array.*))) KEEP(*(.init_array*)) PROVIDE_HIDDEN(__init_array_end = .); } >FLASH
  .fini_array : { PROVIDE_HIDDEN(__fini_array_start = .); KEEP(*(SORT(.fini_array.*))) KEEP(*(.fini_array*)) PROVIDE_HIDDEN(__fini_array_end = .); } >FLASH

  /* --- RAM, in address order. .stack MUST stay first (§7 R4). ------------- */
  .stack (NOLOAD) : {
    . = ALIGN(8);
    _sstack = .;                 /* lowest stack address — one word below this
                                  * is unmapped, which is the point */
    . = . + _Min_Stack_Size;
    . = ALIGN(8);
    _estack = .;                 /* initial MSP: startup + fault entry use it */
  } >RAM

  _sidata = LOADADDR(.data);
  .data : {
    . = ALIGN(4); _sdata = .;
    *(.data) *(.data*) . = ALIGN(4); _edata = .;
  } >RAM AT>FLASH

  .bss : {
    . = ALIGN(4); _sbss = .; __bss_start__ = _sbss;
    *(.bss) *(.bss*) *(COMMON) . = ALIGN(4); _ebss = .; __bss_end__ = _ebss;
  } >RAM

  /* §7 R2: never initialised, never zeroed, survives warm resets. Must stay
   * OUTSIDE [_sbss,_ebss) or the startup loop would wipe it. */
  .noinit (NOLOAD) : {
    . = ALIGN(4); _snoinit = .;
    KEEP(*(.noinit)) KEEP(*(.noinit*))
    . = ALIGN(4); _enoinit = .;
  } >RAM

  ._user_heap : {
    . = ALIGN(8); PROVIDE(end = .); PROVIDE(_end = .);
    . = . + _Min_Heap_Size; . = ALIGN(8);
  } >RAM

  /* §7 R4: LAST in FLASH so the .bin ends with the CRC word. The placeholder
   * value compiled into it (0xFFFFFFFF, see integrity.c) means "not patched" —
   * that is a distinct, non-fatal status, not a mismatch. */
  .fw_crc : {
    . = ALIGN(4);
    _fw_crc_addr = .;
    KEEP(*(.fw_crc))
    . = ALIGN(4);
  } >FLASH

  /DISCARD/ : { libc.a(*) libm.a(*) libgcc.a(*) }
  .ARM.attributes 0 : { *(.ARM.attributes) }
}
