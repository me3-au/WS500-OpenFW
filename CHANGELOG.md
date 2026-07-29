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
- **Driver-stage over-temp protection** (GH#39) — the field switch had none: a 120 °C
  WARN that changed nothing, and no arbitration ceiling reading `driver_temp_c` at all,
  so a cooking FET kept driving at full commanded duty. That was weaker than the stock
  firmware, and it mattered because BKIN is unrouted and there is no field-current sense,
  making this NTC the switch stage's only guard. Now two layers: a graduated derate (a
  second thermal-governor instance feeding the normal watts arbitration as
  `CTRL_BIND_DRIVER_THERMAL`) and a hard block at 125 °C — new fault code 19,
  CRITICAL/OPEN, latching until reset, matching stock's threshold. All six governor
  constants are `[SPEC-SIGNOFF]` bench-pending; there is no thermal model of this board
  to derive them from.
- **Fault-mask completeness guard** (GH#41) — `SHUNT_OPEN`/`SHUNT_REVERSED` belonged to
  no disposition mask, so a lying current source read as INFO/CONTINUE against §7's
  intent; both now LIMP. The durable half is `test_fault_mask_completeness()`, which
  enumerates bit positions programmatically so the next unclassified bit fails the build
  instead of silently defaulting to CONTINUE.

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

### Changed
- **V1/V2 scope re-alignment (owner decision 2026-07-28)** — **All CAN Rx for control
  (BMS/DVCC ceilings, multi-unit sync, engine RPM, pre-disconnect, battery-temp sourcing)
  deferred to V2.** V1 scope now: CAN Tx telemetry only (read-only broadcast; cannot
  affect the loop). A committed first slice (CAN Rx with BMS ceilings) was reverted
  (commit b2da41a). Work is parked on branch `wip/can-in-v2-cvl-predisconnect`. See
  PROJECT_PLAN §1.1 for V2 entry criteria (DVCC design pass, CVL consumer, pre-disconnect,
  `bms_required` flag, field-path safety gate).

### Fixed
- **Alternator thermal protection could be silently disabled by configuration** (GH#43) —
  the app fed both the over-temp fault input and the thermal governor from ADC channel A
  alone and discarded `alt_temp2_c`, so binding the battery probe to channel A moved the
  alternator probe to the unread channel: the fault could never fire and the governor
  returned "no ceiling" forever, with nothing on the wire to say so. Both call sites now
  fold the max of whichever alternator-side channels are finite.
- **`ws500-testfw.elf` failed to link** — `board_clock_config()` called into the CAN stack
  on the HSE-fallback path, but `board.c` is shared with test-fw, which deliberately
  excludes `can_n2k.c`. The GH#38 "untrusted clock → go CAN-quiet" policy now lives in
  `can_n2k_init()`, which reads `board_clock_running_on_hse()`; behaviour is unchanged.
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
