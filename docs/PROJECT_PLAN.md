# WS500-OpenFW — Project Plan

Status legend: ✅ done · 🔨 in progress · ⬜ not started · 🧩 decision needed

Repo is **public** at `github.com/me3-au/WS500-OpenFW` (personal account `me3-au`). This
doc is the master tracker; the deliverables/milestones below should now be mirrored as
**GitHub Issues + Milestones** (the repo is already pushed — do this now, not "after push").

**Authoritative design direction:** the **two-stage LFP** model in
[`CONTROL_SPEC_NEXTGEN.md`](CONTROL_SPEC_NEXTGEN.md) (Draft B) **supersedes** the older
multi-stage Pb model still encoded in `Core/Inc/regulator.h` / `Core/Src/regulator.c`.
That code is **legacy** and is scheduled for rewrite in **M2.5** (below). Where this plan and
the old code interface disagree, the spec wins.

**Design philosophy — know the history, don't keep it in code.** The GPL VSR source and the
reverse-engineered WS500 binary are **reference and validation only**: they establish legal
footing, explain the hardware, and let us cross-check facts. They are **not** a codebase to
port. The firmware is a **clean-slate, modern implementation** — we deliberately **ditch the
legacy surface** (6-stage Pb machine, absorption stage, DIP switches, small-alt/half modes,
RFM/PBF/Feature-IN RPM conflicts, 12 V / 500 Ah normalization) and adopt smarter methods
(two-stage CHARGE/REST, per-cell V, single watts arbitration, named LFP profiles). No legacy
concept is carried forward "because that's how it was done." History informs; it does not
constrain the code.

---

## 0. Document map (the design corpus)

| Doc | Purpose | Status |
|---|---|---|
| `PROJECT_PLAN.md` (this) | Tracker → GitHub Issues/Milestones | 🔨 |
| `WS500_HARDWARE_SPEC.md` | Reverse-engineered hardware facts (MCU, pin map, sensors) | ✅ (few open items §7) |
| `IO_COVERAGE.md` | Per-pin I/O completeness map | ✅ |
| `CONTROL_SPEC_NEXTGEN.md` | **Authoritative** control architecture (2-stage LFP, watts arbitration) | 🔨 Draft B |
| `PROFILE_SPEC_LFP.md` | Charge-profile engine: state machine, params, JSON schema | 🔨 Draft 1 (8 open Qs §8) |
| `FLASH_AND_RECOVERY.md` | Backup/rollback/update procedures | ⬜ (constraints in §3 here, to extract) |
| `SAFETY.md` | Bench-safety rules | ⬜ (rules in §5 here, to extract) |
| `test-fw/README.md` | Bring-up test firmware spec | ⬜ (spec in §4 here, to extract) |
| `CLIENT_CONNECTIVITY.md` | Programming/firmware/monitoring across PC/Mac/iOS/Android over USB+CAN (decision) | ✅ |
| `CAN_TECHNICAL_SPEC.md` / `CAN_USER_GUIDE.md` / `USER_GUIDE.md` / `TEST_PLAN.md` | see deliverables | ⬜ |

> The old plan named a single `SOFTWARE_DESIGN_SPEC.md` that was never written; that role is
> filled by `CONTROL_SPEC_NEXTGEN.md` + `PROFILE_SPEC_LFP.md`. Remaining design gaps to fold
> into those two: **fault-code bit enumeration**, **field PWM frequency**, and **inner
> control-loop numerics (PI gains, loop rate)** — none specified anywhere yet.

## 0.5 Prior art / upstream — the GPL-3.0 ancestor (important)

The WS500 is the **4th-generation commercial evolution of Al (William A.) Thomason's
open-source VSR "Very Smart Regulator."** Generations 1–3 are **published open source**;
gen-4 (WS500) is the closed commercial port. This materially de-risks the project:

- **Software: GPL-3.0** (2016–2018, © William A. Thomason). Full control stack is public —
  `Alternator.cpp` (regulation state machine), `CPE.cpp` (charge-profile entries → the
  `$CPx` model), `Sensors.cpp` (INA226 + NTC), `OSEnergy_Serial.cpp` (the `$` config
  protocol), `OSEnergy_CAN.cpp`. Repos: `Open-Source-Alternator-Regulator/alt-Source`,
  `AlternatorRegulator/VSR-Source`. Cloned to `../VSR-upstream/` (reference; not committed).
- **Hardware: CC BY-SA 4.0** (`alt-CAD`) + the [hardware design overview](https://arduinoalternatorregulator.blogspot.com/2010/06/hardware-design-overview.html),
  which **resolves several of our board-only open items**: field drive = two N-ch FETs +
  floating boost-driver (HIGH/LOW = P/N-type via jumpers) + charge pump, PWM-driven; sensing
  = INA-282 shunt amp → INA-226 (gen-3), which the WS500 modernized to a single
  INA226/228/238 at the shunt (matches our finding); NTC 10 K battery/alt temps; temp-comp
  `setPointVolts += (77 - batTemp) × 0.0168` (°F). Gen-3 MCU = **ATmega64M1** (8-bit AVR),
  not the WS500's STM32F072 — so **algorithms port, low-level peripheral code does not.**
- **Pre-2015 releases were CC BY-NC-SA (non-commercial) — do not use those**; only the
  2015+ GPL-3.0 / CC BY-SA 4.0 material is safe for a project that may see commercial use.

**Consequence for the clean-room posture:** the control logic we deliberately avoided reading
out of the WS500 binary has a **GPL-3.0 open ancestor** — so the legal footing is settled.
But per the design philosophy above, we **do not port it.** The GPL source is **reference and
cross-check** (how the hardware is driven, proven algorithm shapes, validation of our RE
findings); the STM32 reverse-engineering is the source of truth for the *hardware interface*.
The control code itself is written **fresh** to the two-stage LFP spec — no AVR code, no
legacy structure, carried across. **If any GPL VSR code *is* ever reused verbatim, attribution
to William A. Thomason and GPL headers are mandatory** (deliverable #16); a clean fresh
implementation informed by open references is cleaner still. This grants no rights to the
WS500's proprietary STM32 firmware — and we don't need it.

## 1. Deliverables map

| # | Deliverable | Artifact | Milestone | Status |
|---|-------------|----------|-----------|--------|
| 1 | Project management | this doc → GitHub Issues/Milestones | M0 | 🔨 |
| 2 | Git hosting | remote exists, public | M0 | ✅ |
| 3 | HW documentation | `WS500_HARDWARE_SPEC.md`, `IO_COVERAGE.md` | M0–M2 | ✅ (open items) |
| 4 | Control/design spec | `CONTROL_SPEC_NEXTGEN.md` + `PROFILE_SPEC_LFP.md` (+ fault codes, loop numerics); **cross-referenced against the GPL VSR source (§0.5)** | M2.5–M3 | 🔨 |
| 5 | OS firmware | `Core/` (board/field/main real; sensors/regulator/ina/config/can stub) | M2–M3 | 🔨 |
| 6 | Config protocol + store | `config_protocol.c` + flash-page store | M4 | 🔨 / 🧩 (see #6a) |
| 6a | **Config strategy decision** | stock `$`-compatible vs new JSON profile schema (they conflict) | M2/M3 | 🧩 |
| 7 | Update/rollback/backup/recovery | `FLASH_AND_RECOVERY.md` (+ §3) | M1 | ⬜ |
| 8 | Client app | **WebSerial/WebUSB web app** (PC/Mac/Android: program+monitor+firmware, one codebase) + native `tools/ws500ctl/` CLI (scripting/CI/flash). iOS = monitor via CAN/VRM only. See `CLIENT_CONNECTIVITY.md` | M4 | ⬜ |
| 9 | CAN Rx for control | BMS charge-permission/current in (feeds the loop) | M3 | ⬜ |
| 10 | CAN Tx telemetry + RBM | status PGNs, regulator sync + `CAN_TECHNICAL_SPEC.md` | M5 | ⬜ |
| 11 | CAN user doc | `CAN_USER_GUIDE.md` | M5 | ⬜ |
| 12 | User documentation | `USER_GUIDE.md` (install, config, LED codes, troubleshooting) | M6 | ⬜ |
| 13 | Testing + bug tracking | `TEST_PLAN.md`; Renode emulation + bench HIL; GitHub Issues | M0→M6 | ⬜ |
| 14 | Bring-up test firmware | `test-fw/` (§4). *ADC binding already recovered — this confirms scaling on bench* | M2 | ⬜ |
| 15 | Bench safety | `SAFETY.md` (§5); gates every hardware milestone | all HW | 🔨 |
| 16 | **License + third-party NOTICE** | full GPL-3.0 text (LICENSE is a placeholder); NOTICE for HAL (BSD-3) / NMEA2000 (MIT); **attribution to William A. Thomason + GPL headers preserved on any reused VSR code (§0.5); CC BY-SA 4.0 attribution for reused hardware** | **M0 (now)** | ⬜ |
| 17 | **OSS hygiene** | `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, issue/PR templates, README badges | M0 | ⬜ |
| 18 | **Versioning + release** | `VERSION`/macro, `CHANGELOG.md`, tag/release flow | M0→M6 | ⬜ |
| 19 | **Emulation harness** | Renode model (STM32F072 + peripherals + INA stub) for hardware-free dev/CI | M0→M1 | ⬜ |
| 20 | **Telemetry / logging** | log stream over USB CDC / CAN (per `CONTROL_SPEC`) | M4–M5 | ⬜ |

## 2. Milestones (safety and recovery come before any flash write)

Each milestone lists **exit criteria**. `→` marks a hard gate.

- **M0 — Infrastructure** *(mostly done)*.
  Done: public remote ✅, CI (`.github/workflows/build.yml`) ✅, HAL vendoring
  (`scripts/fetch_deps.sh`) ✅, `stm32f0xx_hal_conf.h` ✅.
  Remaining: **license + NOTICE (#16)**, OSS hygiene (#17), versioning scaffold (#18),
  convert this table to Issues/Milestones, **stand up the Renode emulation harness (#19)**.
  *Exit:* repo builds green in CI; full GPL text in place; issues created; emulator runs the
  built ELF far enough to exercise `main()`.
- **M1 — Backup & recovery proven** → *no custom firmware is flashed before this passes.*
  Verified stock-image backup, documented+rehearsed DFU restore, SWD permanently wired,
  BOOT0/DFU entry-exit rehearsed. *Exit:* stock image demonstrably restores the unit via DFU
  on the bench; `FLASH_AND_RECOVERY.md` written.
- **M2 — Bring-up firmware** (`test-fw`, §4). Confirm every I/O on the bench; **bench-confirm
  ADC scaling** (binding is already recovered); resolve the two label unknowns (PB13 =
  Enable vs Feature-In; which output = Lamp vs LED); identify the 0x0C/0x10/0x4C I²C devices;
  confirm package + field-driver topology. *Exit:* `board.h` constants bench-verified; I/O
  coverage all ✅.
- **M2.5 — Control-model reconciliation** → *gates M3.* ✅ **mostly done.**
  Legacy `regulator.{h,c}` deleted; replaced by the pure, HAL-free **`control/`** core
  (spec-native `ctrl_*` vocabulary, two-stage CHARGE/REST, per-cell V, watts arbitration).
  Built + CI-tested: `control` engine, `arbitration`, `field` (rotor clamp), `limits`,
  `faults` (OPEN/LIMP ladder), `thermal` governor — all unit-tested on the native CI runner;
  wired end-to-end in the app. **Remaining:** decision **#6a config strategy** (still 🧩).
- **M3 — Core firmware.** 🔨 *in progress.* **Done (pure/CI-tested):** two-stage engine +
  profile 1, arbitration, CV/field loop, rotor clamp, thermal governor, fault ladder,
  hardware limit set. **Remaining:** finish the driver side — INA2xx I²C transfers, stator/
  TIM2 RPM capture, DIO (enable/Feature-In/lamp/LED), **CAN Rx for control (#9)** (BMS
  permission/current) — plus inner-loop gain tuning, watchdog, and bench bring-up. *Exit:*
  closed-loop CV hold on the bench supply into a dummy load, with fault cutoff verified.
- **M4 — Config + client app.** Config schema (per #6a), flash config store (CRC+version),
  `ws500ctl` read/write/verify, FW update via CLI, telemetry stream (#20). *Exit:* config
  round-trips; FW updates via `ws500ctl`; config survives an update.
- **M5 — CAN Tx / NMEA2000.** Status PGN set, RBM participation, `CAN_TECHNICAL_SPEC.md` +
  user doc. *Exit:* regulator telemeters valid PGNs and participates in RBM on a real bus.
- **M6 — Real-alternator trials + release.** Staged live testing per §5 exit ladder; user
  guide; **versioned tagged release** with CHANGELOG. *Exit:* driven alternator charges a
  bank under supervision; `v0.1.0` tagged.

## 3. Risk register

| Risk | Impact | Mitigation |
|---|---|---|
| Brick the only unit | project-ending (no spare) | M1 gate: proven DFU restore before first flash; SWD wired; ROM DFU is unerasable |
| Wrong INA2xx scaling → bad current | unsafe charging | bench-verify against a reference meter (M2/M3); emulation cross-check |
| Field-driver damage | hardware loss | dummy load, 20 % duty cap in test builds, TIM1 BKIN cutoff, current-limited supply (§5) |
| License non-compliance (public + placeholder LICENSE) | legal/OSS | **#16 in M0, immediate** |
| Single irreplaceable unit | any HW test is high-stakes | emulation-first (#19); dry-run risky tests in Renode; golden stock image |
| Control-model drift (code vs spec) | rework, bugs | M2.5 reconciliation; spec is single source of truth |
| Draft specs with open questions | design churn | track `PROFILE_SPEC` §8 questions as issues; resolve before M3 coding they touch |

---

## 4. Bring-up test firmware (`test-fw`) — *to extract to `test-fw/README.md`*

Separate small build target sharing `board.c`. Interactive over USB CDC:
- LED / GPIO walk (confirm pin map + resolve Enable-vs-Feature-In, Lamp-vs-LED labels)
- Live ADC dump of all 7 channels (bench-confirm the recovered scaling)
- Field PWM at commanded duty **with a hard 20 % cap compiled in**, dummy load only
- TIM1 break-input test (assert fault line → verify PWM hard-stops)
- CAN loopback + external echo test
- I²C bus scan (confirm INA2xx @ 0x40; identify 0x0C/0x10/0x4C)

Everything it proves feeds `board.h` constants and the HW spec.

## 5. Bench safety — *to extract to `SAFETY.md`* — protecting the one WS500

Rules, in force until explicitly retired:
1. **Current-limited bench supply** (13.2 V, start ≤1 A) — never a raw battery for bring-up.
2. **Dummy field load** — power resistor (~10 Ω, ≥50 W) until the loop + fault paths are proven.
3. **Fail-safe defaults** — firmware ships field-OFF; watchdog on; TIM1 break verified in M2.
4. **Duty-cycle cap compiled into test builds** (20 %).
5. **Never spin a real alternator without a battery connected** (load dump). Real-alternator
   work starts only in M6: dummy load → field coil on dead alternator → driven alternator on
   a battery bank with supervision.
6. **Recovery always one step away** — SWD permanently wired; stock restore rehearsed (M1).
7. **Exactly one WS500 exists — irreplaceable.** M1 (backup + rehearsed restore) is absolute;
   any test that could plausibly *damage* (not just brick) hardware gets a Renode dry run first.

## 6. Flash / update / rollback / backup / recovery — *to extract to `FLASH_AND_RECOVERY.md`*

**Chip facts:** STM32F072xB = 128 KB single-bank flash → no A/B slots (not worth halving flash).
- **Unbrickable floor:** the ST **system DFU bootloader in ROM** can't be erased. Force
  system-memory boot via the **BOOT0 access point** (locate/verify on the board — open item in
  [`IO_COVERAGE.md`](IO_COVERAGE.md), "needs board/schematic").
- **Backup first:** full SWD flash readout of the stock unit before anything. ⚠️ If RDP ≥1 is
  set, readout is blocked and disabling it mass-erases — then the stock DFU image file we
  already hold *is* the backup; prove it restores before relying on it.
- **Rollback = restore stock image via DFU** (rehearsed procedure).
- **Update path:** stock ROM DFU (`dfu-util`), driven by `ws500ctl`. A CRC-checked,
  config-preserving custom bootloader is a *later* nice-to-have, not a dependency.
- **Config survives updates:** last flash page(s), outside the app image, CRC + version;
  `ws500ctl` exports/imports as text.
- **No flash protections, ever (decided):** our firmware never sets RDP or WRP. The chip stays
  fully readable/reflashable via SWD and DFU — recovery is never locked out. (The *stock* unit
  may still ship with RDP set, affecting only the backup step above.)
