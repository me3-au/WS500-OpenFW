# WS500-OpenFW — Test Firmware (`test-fw`)

> **Extracted from PROJECT_PLAN.md §4 on 2026-07-26.**
> Authoritative details, updates, and the ongoing tracker remain in PROJECT_PLAN.md.

---

## Purpose

Separate small build target sharing `board.c`. Interactive over USB CDC. Confirms every I/O on the bench; results feed `board.h` constants and the hardware spec (`WS500_HARDWARE_SPEC.md`, `IO_COVERAGE.md`).

---

## Features

- **LED / GPIO walk** — confirm pin map + resolve Enable-vs-Feature-In, Lamp-vs-LED labels
- **Live ADC dump of all 7 channels** — bench-confirm the recovered scaling
- **Field PWM at commanded duty with a hard 20 % cap compiled in, dummy load only**
- **Field cutoff test** — trigger the software fault path → verify PWM hard-stops
  (TIM1 `MOE` clear). *Corrected vs the original §4 text: the stock board does
  **not** route BKIN (PROJECT_PLAN §0.6 V1/V2 — `BDTR.BKE=0`, no break AF pin);
  a hardware break-input test applies only if a fault comparator is ever wired
  (optional-improvement path).*
- **CAN loopback + external echo test**
- **I²C bus scan** — confirm INA226 @ 0x40 **and 24C16 EEPROM @ 0x50, both on
  I²C2** (PROJECT_PLAN §0.6 V3/V7; stock DeInits I²C1 at boot — scan both buses
  to confirm on hardware). *Corrected vs the original §4 text: the
  "0x0C/0x10/0x4C devices" item is **refuted** (V3 — length args misread as
  addresses); nothing to identify there.*

---

## Integration with Development

Everything proven by this firmware **feeds `board.h` constants and the HW spec.**
