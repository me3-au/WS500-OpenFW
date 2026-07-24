# Changelog

All notable changes to WS500-OpenFW will be documented in this file.

The format is based on [Keep a Changelog](https://keepachangelog.com/en/1.1.0/),
and this project adheres to [Semantic Versioning](https://semver.org/spec/v2.0.0.html).

There are no tagged releases yet. **`v0.1.0` will be the first tagged release, cut at
milestone M6** (real-alternator trials passed) per `docs/PROJECT_PLAN.md`. Until then,
`main` carries version `0.1.0-dev`.

## [Unreleased]

Current state, honestly:

### Added
- **Documentation + reverse-engineering corpus** — recovered STM32F072RB hardware
  interface (`docs/WS500_HARDWARE_SPEC.md`, `docs/IO_COVERAGE.md`, binary-verified facts in
  `docs/PROJECT_PLAN.md` §0.6), control/profile specs (`CONTROL_SPEC_NEXTGEN.md`,
  `PROFILE_SPEC_LFP.md`), CAN/user/connectivity docs, project plan with staged safety
  ladder.
- **Pure control core** (`control/`) — HAL-free C11: two-stage CHARGE/REST state machine,
  watts arbitration, CV/field loop with rotor duty clamp, hardware limit set, fault ladder
  (OPEN/LIMP), predictive thermal governor, telemetry — unit-tested natively in CI on
  every push.
- **SIL plant-simulation gauntlet** (`sim/`, §8.1) — LFP battery + alternator + 4 Ω/12 V
  rotor plant model (calibrated to the on-hardware stock charge run) driving the real
  control tick; 9 scenarios / 111 checks in CI including a never-exceed rotor-clamp proof
  and a 5 M-tick long-soak. This is the Stage-C flash gate.
- **Driver/BSP layer** (`Core/`) — recovered pin map (`board.h`), GPIO/TIM1/ADC/CAN/I²C/USB
  init; field-drive abstraction (143 Hz, software fault cutoff — TIM1 break disabled, no
  BKIN in stock hardware); working INA226 driver (I²C, CVRF-gated, software scaling, bounded
  retries — bus pending Stage-A scope confirmation, see caveat in `ina2xx.c`); sensor
  acquisition skeleton. CAN Rx/Tx and config protocol not yet functional.
- **Build + CI** — CMake ARM cross-build (arm-none-eabi), pinned HAL/CMSIS vendoring,
  GitHub Actions running the native control-test job, the new SIL gauntlet job, and the
  firmware build.

### Fixed
- **Four control-core defects caught by the SIL gauntlet** — NaN field duty on lost VBat
  sense; permanent effort-integrator latch on a NaN shunt reading; tick-rate-dependent,
  marginally-unstable inner-loop gains (now dt-scaled, KP reduced 5×); and a knife-edge
  CV-exit qualifier that stalled the charge-complete timers under sensor noise. Each has a
  regression test.
- **Licensing** — MIT LICENSE + third-party NOTICE; no-GPL-in-tree rule (VSR upstream is
  reference-only).
- **OSS scaffolding** — CONTRIBUTING, CODE_OF_CONDUCT, SECURITY, issue/PR templates,
  versioning (`VERSION`, `Core/Inc/version.h`, this changelog).

### Not yet
- **No hardware has been flashed.** The M1 gate (proven stock-image backup + rehearsed DFU
  restore) has not been passed; the single existing WS500 runs stock firmware, in service
  on a live 48 V system. Hardware access so far is Stage-A observation only (readings,
  serial capture — including on-hardware confirmation of the 25 % rotor field clamp).
- Renode emulation harness, bring-up test firmware, config store/schema, `ws500ctl`
  client, NMEA2000 Tx — all planned (deliverables #19, #4–#11 in the plan).
- Config storage identified as an external 24C16 EEPROM @0x50 (I²C2, /WP=PA15) from the
  stock binary (§0.6 V7); no EEPROM driver written yet, and the INA226/EEPROM bus (I²C2 vs
  I²C1) awaits a Stage-A scope check.
