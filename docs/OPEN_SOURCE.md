# WS500-OpenFW — Open-Source Project Guide

> **Status:** draft. Project overview + contributor guide.

Open-source replacement firmware for the **Wakespeed WS500** alternator-regulator
hardware. GPL-3.0. Built for LiFePO4-first, two-stage charging with everything in real
units.

---

## 1. Why this project exists

Wakespeed (the WS500's maker) is winding down. The WS500 is good hardware, and it has a
**fully open-source ancestor** — Al (William A.) Thomason's **VSR "Very Smart Regulator"**
(the WS500 is its 4th-generation commercial evolution). This project keeps the hardware
alive with a fresh, modern firmware that any owner can read, build, flash, and improve.

**Design philosophy — know the history, don't keep it in code.** The reverse-engineered
WS500 binary and the GPL VSR source are used only as **reference** (to understand the
hardware and validate facts). The firmware is a **clean-slate, modern implementation**: we
deliberately drop the legacy surface (6-stage Pb machine, DIP switches, RPM tables, 12 V/
500 Ah normalization) and adopt smarter methods (two-stage LFP, one watts-arbitration, named
profiles). No legacy concept is carried forward "because that's how it was done."

## 2. Provenance & licensing

- **This firmware: GPL-3.0-or-later.**
- **Upstream lineage:** Al Thomason's VSR — **software GPL-3.0**, **hardware CC BY-SA 4.0**
  (pre-2015 releases were CC BY-NC-SA — not used). Any reused VSR code must keep its GPL
  headers and **attribute William A. Thomason**; reused hardware design is CC BY-SA.
- **Hardware interface** (pin map, sensors, INA current path) was recovered by **black-box
  analysis of the compiled stock firmware** — interface fact, not copied expression. The
  WS500's proprietary STM32 firmware is **not** used.
- **Third-party components:** STM32Cube HAL (BSD-3), NMEA2000 library (MIT) — see `NOTICE`.

See `WS500_HARDWARE_SPEC.md` for the full provenance note.

## 3. Target hardware (recovered)

| | |
|---|---|
| MCU | STM32F072xB (Cortex-M0, 128 KB flash / 16 KB SRAM, crystal-less USB) |
| Field drive | PA8 = TIM1_CH1 PWM + hardware break (fault cutoff) |
| Sensing | ADC: 3× NTC + battery voltage (PC5 divider); current+bus V via TI INA226/228/238 on I²C |
| RPM | stator on TIM2 |
| Comms | 1× CAN (bxCAN), USB CDC config, USB DFU (ROM) |

Full detail: `WS500_HARDWARE_SPEC.md`, `IO_COVERAGE.md`.

## 4. Architecture

Layered, with the control brain kept **hardware-independent** so it can be unit-tested on
a PC and in emulation without the (single, irreplaceable) board:

```
control/   PURE control core — NO HAL, host/CI/Renode testable
           engine (state machine) · arbitration · field · limits · faults · thermal
Core/      drivers + BSP + app  (HAL-facing)
           board · field_drive · sensors · ina2xx · can · config · main (orchestrator)
```

**Dependency rule:** the control core never includes the HAL; drivers never include the
control types; `main.c` wires them. This is what makes the logic testable off-target.

## 5. Build & test

**Firmware (ARM):**
```sh
sh scripts/fetch_deps.sh        # vendors STM32Cube HAL/CMSIS (pinned)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build             # -> build/ws500-openfw.elf / .bin
```

**Control-core unit tests (native, no hardware):**
```sh
gcc -std=c11 -Wall -Wextra -I control/Inc control/Src/*.c control/test/*.c -lm -o t && ./t
```

**CI** (`.github/workflows/build.yml`) runs both on every push: a `tests` job (native gcc
runs the unit tests) and a `firmware` job (arm-none-eabi cross-build). Both must be green.

## 6. Repository layout

| Path | What |
|---|---|
| `control/` | pure control core + unit tests |
| `Core/` | drivers, BSP, app |
| `docs/` | specs, manuals, hardware docs, this guide |
| `scripts/`, `cmake/`, `.github/` | dependency fetch, toolchain, CI |
| `Drivers/` | vendored HAL/CMSIS (fetched, gitignored) |

Key docs: `PROJECT_PLAN.md` (roadmap/tracker), `CONTROL_SPEC_NEXTGEN.md` +
`PROFILE_SPEC_LFP.md` (authoritative design), `USER_MANUAL.md`, `CAN_INTEGRATION.md`,
`CLIENT_CONNECTIVITY.md`.

## 7. Status & roadmap

**Done (CI-verified):** the pure control engine — two-stage state machine, watts
arbitration, CV/field loop with rotor clamp, predictive thermal governor, fault ladder,
hardware limit set — all unit-tested and wired end-to-end.

**In progress:** driver side (INA2xx I²C, stator RPM, digital I/O, CAN Rx/Tx), the config
schema + client tools, inner-loop tuning, and bench/emulation bring-up.

Milestones and the deliverables map live in `PROJECT_PLAN.md`.

## 8. Contributing

- **The control core is `float`, pure C11, no dynamic allocation, no HAL.** Keep it that
  way — it's what lets us test without hardware.
- **Add a unit test** for any control-core change (`control/test/`), and keep the safety
  invariants: field-open on uncertainty; raw-signal safety comparators independent of the
  control path.
- Match the authoritative specs; if you change behavior, change the spec too.
- One logical change per PR; CI (tests + firmware) must be green.
- By contributing you agree your work is licensed GPL-3.0-or-later.

## 9. Safety & liability

This is **experimental firmware for a safety-relevant device** (it drives real charging
current). It ships fail-safe (field off), but **you are responsible** for validating any
configuration on your hardware. Bench-test on a current-limited supply into a dummy load
before connecting a real alternator (`SAFETY.md`). No warranty — see the GPL.
