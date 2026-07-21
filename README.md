# WS500-OpenFW

Open-source alternator-regulator firmware for the Wakespeed WS500 hardware
(STM32F072). Clean-room reimplementation: **hardware interface reverse-engineered from
the compiled stock firmware as a black box; all control logic written fresh.**

> **Provenance / license note.** This project deliberately contains **none of the original
> Wakespeed firmware's control algorithms, code, or expression.** The pin map and peripheral
> configuration were recovered by black-box analysis of the compiled DFU image (see
> `docs/WS500_HARDWARE_SPEC.md`) — that is interface fact, not copyrightable expression.
> The regulation logic in `Core/Src/regulator.c` is a fresh implementation to be written
> from control theory and the *published* charge-profile behavior. Keep it that way.

## Target hardware (recovered — see `docs/WS500_HARDWARE_SPEC.md`)

| | |
|---|---|
| MCU | STM32F072xB (Arm Cortex-M0, 128 KB flash / 16 KB SRAM) |
| Package | likely 100-pin (VB) — GPIOE is used; confirm from board |
| Clock | 48 MHz from HSI48 + CRS (crystal-less USB) |
| Field PWM | PA8 = TIM1_CH1 (AF2); PB15 = TIM1_CH3N (AF2); TIM1 BKIN for fault cutoff |
| Analog in | PA1/PA2/PA3 (ADC_IN1/2/3) + PC5 (ADC_IN15), 7-ch scan ×4 oversampled |
| CAN | PB8 = RX, PB9 = TX (AF4) |
| I²C | I²C1 PB6/PB7, I²C2 PB10/PB11 (AF1) |
| USB | PA11/PA12 (FS, CDC virtual COM — config channel) |

## What's here vs. what you add

**Provided (this skeleton):**
- `Core/Inc/board.h` — the recovered pin map as `#define`s (single source of truth).
- `Core/Src/board.c` — GPIO/TIM1/ADC/CAN/I²C/USB init to that exact map + 48 MHz clock.
- `Core/Src/field_drive.c` — TIM1 field-PWM abstraction with break/fault.
- `Core/Src/sensors.c` — 7-ch oversampled ADC acquisition; raw→engineering-unit scaling
  left as `TODO` (bind each channel by signal-injection — see below).
- `Core/Src/regulator.c` — **control-loop stubs** with a clean state machine skeleton.
  *This is the part you implement.* Ships fail-safe (field off).
- `Core/Src/config_protocol.c`, `can_n2k.c` — glue stubs for the `$XXX:` config protocol
  and the NMEA2000 stack.
- Linker script, CMake build, toolchain file.

**You must add (not vendored here):**
- **STM32Cube HAL for F0** — drop `Drivers/STM32F0xx_HAL_Driver` and `Drivers/CMSIS` from
  [STM32CubeF0](https://github.com/STMicroelectronics/STM32CubeF0) (BSD-3). See
  `Drivers/README.md`.
- **NMEA2000 library** — [`ttlappalainen/NMEA2000`](https://github.com/ttlappalainen/NMEA2000)
  (MIT), plus an STM32 CAN driver shim. Reuse directly.

## Resolve the analog channel binding first

`sensors.c` reads PA1/PA2/PA3/PC5 but does **not** yet know which is battery voltage vs.
alternator voltage vs. shunt current vs. temperature — that's set by the board wiring, not
the firmware. Bind it empirically: inject a known voltage on one pin at a time (Renode
emulation or a bench rig) and see which `sensor_raw[]` slot moves. Four injections →
definitive map. Fill in `SENSOR_CH_*` in `board.h` and the scaling in `sensors.c`.

## Build

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build
# -> build/ws500-openfw.elf / .bin
```

Flash via DFU (the stock USB bootloader still works): `dfu-util -a 0 -s 0x08000000 -D build/ws500-openfw.bin`, or SWD with OpenOCD.

## License

GPL-3.0-or-later (matches the WS500 Util tooling). See `LICENSE`.
