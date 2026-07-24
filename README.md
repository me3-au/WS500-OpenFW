# WS500-OpenFW

[![build](https://github.com/me3-au/WS500-OpenFW/actions/workflows/build.yml/badge.svg)](https://github.com/me3-au/WS500-OpenFW/actions/workflows/build.yml)
[![License: MIT](https://img.shields.io/badge/license-MIT-blue.svg)](LICENSE)

Open-source alternator-regulator firmware for the Wakespeed WS500 hardware
(STM32F072). Clean-room reimplementation: **hardware interface reverse-engineered from
the compiled stock firmware as a black box; all control logic written fresh.**

> **Provenance / license note.** This project deliberately contains **none of the original
> Wakespeed firmware's control algorithms, code, or expression.** The pin map and peripheral
> configuration were recovered by black-box analysis of the compiled DFU image (see
> `docs/WS500_HARDWARE_SPEC.md`) — that is interface fact, not copyrightable expression.
> The regulation logic in `control/` is a fresh implementation written from control theory
> and the *published* charge-profile behavior (spec: `docs/CONTROL_SPEC_NEXTGEN.md`).
> Keep it that way.

## Target hardware (recovered — see `docs/WS500_HARDWARE_SPEC.md`)

| | |
|---|---|
| MCU | STM32F072RB (Arm Cortex-M0, 128 KB flash / 16 KB SRAM), **LQFP64** — confirmed from the stock binary (only GPIOA/B/C clocked; PC4/PC5/PC10 in use) |
| Clock | 48 MHz sysclk (stock firmware: HSE 8 MHz × PLL6) |
| Field PWM | PA8 = TIM1_CH1 (AF2); PB15 = TIM1_CH3/CH3N (AF2). Stock runs **143.2 Hz, 10-bit duty** (PSC=326, ARR=1024). BKIN is **not** used — fault cutoff is software (MOE clear) |
| Stator (RPM) | PA10 via **EXTI rising edge**, timed against free-running TIM2 (~1 µs tick) — not timer input capture |
| Analog in | PA1/PA2 (β3950 NTC probes) + PA3 (β3380 **FET temp**) + PC5 (battery V, 34.33:1 divider), 7-ch scan, ×4 software-averaged |
| CAN | PB8 = RX, PB9 = TX (AF4) |
| I²C | I²C1 PB6/PB7 (INA226 @ 0x40 — current/voltage sense); I²C2 PB10/PB11 (unidentified ~300-byte device — config-store suspect) |
| USB | PA11/PA12 (FS, CDC virtual COM — config channel) |

## What's here vs. what you add

**Provided:**
- `control/` — the **pure, HAL-free control core** (two-stage CHARGE/REST LFP engine,
  watts arbitration, CV/field loop with rotor duty clamp, thermal governor, fault ladder,
  hardware limits) — unit-tested natively in CI. This is the authoritative implementation
  of `docs/CONTROL_SPEC_NEXTGEN.md` + `docs/PROFILE_SPEC_LFP.md`.
- `Core/Inc/board.h` — the recovered pin map as `#define`s (single source of truth).
- `Core/Src/board.c` — GPIO/TIM1/ADC/CAN/I²C/USB init to that exact map + 48 MHz clock.
- `Core/Src/field_drive.c` — TIM1 field-PWM abstraction (143 Hz, stock-matching; software
  fault cutoff; rotor-clamp-aware).
- `Core/Src/sensors.c` — 7-ch averaged ADC acquisition; scaling constants recovered from
  the stock binary (`docs/PROJECT_PLAN.md` §0.6 V4), pending bench confirmation.
- `Core/Src/config_protocol.c`, `can_n2k.c`, `ina2xx.c` — driver/glue stubs in progress.
- Linker script, CMake build, toolchain file.

**Vendored dependencies:** run `scripts/fetch_deps.sh` to fetch the STM32Cube HAL for F0 +
CMSIS (pinned; see `Drivers/README.md`). The NMEA2000 library
([`ttlappalainen/NMEA2000`](https://github.com/ttlappalainen/NMEA2000), MIT) is planned for
the CAN milestone.

## Build

```sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build
# -> build/ws500-openfw.elf / .bin
```

Flash via DFU (the stock USB bootloader still works): `dfu-util -a 0 -s 0x08000000 -D build/ws500-openfw.bin`, or SWD with OpenOCD.

## License

MIT. See `LICENSE` and `NOTICE`. The GPL VSR ancestor is reference-only — no GPL code is
included in this codebase (see `docs/PROJECT_PLAN.md` §0.5).
