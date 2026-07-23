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
| `QUICK_START.md` | Get-going guide (flash, connect, pick profile, run) | 🔨 draft |
| `USER_MANUAL.md` | User manual + algorithm explanation (how the regulator decides) | 🔨 draft |
| `CAN_INTEGRATION.md` | CAN/NMEA2000 integration (Tx telemetry, Rx BMS/DVCC/engine, sync) | 🔨 draft |
| `OPEN_SOURCE.md` | Project overview, architecture, provenance, build/test, contributing | 🔨 draft |
| `TEST_PLAN.md` | see deliverable #13 | ⬜ |

> The old plan named a single `SOFTWARE_DESIGN_SPEC.md` that was never written; that role is
> filled by `CONTROL_SPEC_NEXTGEN.md` + `PROFILE_SPEC_LFP.md`. Remaining design gaps to fold
> into those two: **fault-code bit enumeration**, **field PWM frequency** (see §0.6 V2), and
> **inner control-loop numerics (PI gains, loop rate)** — none specified anywhere yet.

> `docs/` also holds two gitignored reference PDFs (WS500 Product Manual 10.21.24;
> Wakespeed Comms & Config Guide v2.6.1) used by the RE work. Note `CONTROL_SPEC_NEXTGEN.md`
> and `PROFILE_SPEC_LFP.md` were revised after the last plan pass (charge-exit
> voltage+time-primary, skip-BULK-when-full startup, belt limit as Power@RPM) — statuses
> above remain 🔨.

## 0.5 Prior art / upstream — the GPL-3.0 ancestor (important)

The WS500 is the **4th-generation commercial evolution of Al (William A.) Thomason's
open-source VSR "Very Smart Regulator."** Generations 1–3 are **published open source**;
gen-4 (WS500) is the closed commercial port. This materially de-risks the project:

- **Software: GPL-3.0-or-later** (2016–2018, © William A. Thomason — license file + source
  headers verified in the clone). Full control stack is public — `Alternator.cpp` (regulation
  state machine), `CPE.cpp` (charge-profile entries → the `$CPx` model), `Sensors.cpp`
  (INA226 + NTC), `OSEnergy_Serial.cpp` (the `$` config protocol), `OSEnergy_CAN.cpp`.
  Repos: `Open-Source-Alternator-Regulator/alt-Source`, `AlternatorRegulator/VSR-Source`.
  Cloned to `../VSR-upstream/` (reference; not committed).
- **Cross-checked against the source (2026-07) — correlation is high.** Confirmed: the
  `$CPx/$SCx` protocol; the 6-stage Pb legacy characterization; stator-frequency RPM (our
  learned-K repackages upstream's `poles × ratio / 60`); sensing chain shunt → INA-282
  (50× amp) → INA-226 (I²C digitizes both current *and* local bus voltage); NTC constants
  10 K, **β3950 external probes / β3380 internal FET sensor** — exactly the split our binary
  RE recovered independently (strong two-source confirmation). Temp-comp is **°C-native**:
  `targetBatVolts += (25 − T_C) × BAT_TEMP_1C_COMP × systemVoltMult`, default 0.030 V/°C,
  per-profile configurable, cold-clamped (−9 °C default); the °F form previously quoted here
  (`(77 − batTemp) × 0.0168`) is that default restated.
- **Upstream already targets the WS500's MCU:** `SmartRegulator.h` (v1.3.1) contains a
  **`CPU_STM32` STM32F072xB hardware block** (product code 200; stator on `htim2`;
  `FIELD_PWM_MAX 0xFE`). The "closed commercial port" has a partially published STM32
  ancestor — a large, legal, *virtual* cross-check resource for the WS500 target (§0.6 V5).
  Still reference-only; not ported.
- **Field-drive ancestry, corrected:** the charge pump belongs to **gen-1/2 only**. Gen-3
  and the STM32 lineage replaced it with a **bootstrap (boost-cap) FET driver** and cap max
  field duty (`FIELD_PWM_MAX 0xFC`/`0xFE`) so the boost cap can refresh — **sustained 100 %
  duty is forbidden by the hardware**. Our field module must carry an equivalent compiled
  max-duty cap (CONTROL_SPEC open item; the rotor voltage clamp alone doesn't guarantee it
  on 12 V systems). The "two N-ch FETs, P/N-type via jumpers" topology is schematic/blog
  sourced only — not verifiable from code. Upstream runs field PWM at **122 Hz on AVR** (with
  an explicit ">400 Hz is lossy" comment) and **244 Hz on the 2018 STM32 prototype**
  (cubeMX-set); the WS500 itself runs **~400 Hz per Al Thomason (verbal, ~2024 phone call
  with the project owner)** — our driver default is now 400 Hz, with §0.6 V2 confirming
  from the binary before bench field tests. **Rotor back-EMF is
  dumped through snubber/freewheel diodes** (reported): the field current free-wheels during
  PWM off-time, so with the rotor's long L/R time constant (~4 Ω, high inductance) the rotor
  current is well-smoothed at these frequencies and the **average-voltage duty model behind
  the rotor clamp (duty ≈ V_rotor_rated / V_bus) is physically valid**. Verify the diode
  arrangement at the bench (board-level fact); it does not protect against alternator
  load dump — that remains battery-connected-only territory (§5).
- **Hardware license: CC BY-SA 4.0 is *reported, not substantiated in the clone*** —
  `alt-CAD` carries **no license file**; the claim traces to the external
  [hardware design overview blog](https://arduinoalternatorregulator.blogspot.com/2010/06/hardware-design-overview.html).
  Verify before reusing any CAD material. Gen-3 MCU = **ATmega64M1** (8-bit AVR), not the
  WS500's STM32F072 — so **algorithms port, low-level peripheral code does not.**
- **Pre-2015 releases reportedly CC BY-NC-SA (non-commercial) — unverified**: our clone is
  shallow (HEAD = v1.3.1 only, no pre-2015 history; `alt-Documentation` clone is empty).
  Treat as true out of caution — only use 2015+ GPL material.

**Consequence for the clean-room posture:** the control logic we deliberately avoided reading
out of the WS500 binary has a **GPL-3.0-or-later open ancestor** — so the legal footing is settled.
But per the design philosophy above, we **do not port it.** The GPL source is **reference and
cross-check** (how the hardware is driven, proven algorithm shapes, validation of our RE
findings); the STM32 reverse-engineering is the source of truth for the *hardware interface*.
The control code itself is written **fresh** to the two-stage LFP spec — no AVR code, no
legacy structure, carried across.

**License decision (2026-07): the project ships under a permissive license — MIT.** We want
the least restrictive terms possible, so the earlier "GPL if we reuse VSR code" posture is
replaced by a hard rule: **no GPL code in-tree, ever.** The VSR source stays reference and
validation only — facts and algorithms are not copyrightable, expression is; anything that
would be a paste gets independently rewritten. Courtesy (non-binding) attribution to
William A. Thomason goes in NOTICE/docs. This also rules out GPL *libraries* as dependencies
(e.g. canboat); the approved dependency set is entirely permissive — **CMSIS (Apache-2.0),
STM32 HAL (BSD-3-Clause), ttlappalainen/NMEA2000 (MIT), Unity test framework (MIT)** —
and `dfu-util` (GPL) is fine as an external *tool* since nothing links against it.
(If a patent grant ever matters commercially, Apache-2.0 is the drop-in alternative; MIT
is the default for maximum simplicity.) A 2026-07 search found **no permissive-licensed
alternator-control prior art** — newer community derivatives (e.g. the VSR Mini Mega
refactor) inherit the GPL — which is fine: the clean-slate design needs none of it. This
grants no rights to the WS500's proprietary STM32 firmware — and we don't need it.

## 0.6 Research confidence — virtual verification queue

A 2026-07 skeptical audit of the research found that `WS500_HARDWARE_SPEC.md` is honest
about provenance, but **`IO_COVERAGE.md` upgrades several inferred items to ✅ Confirmed**,
and downstream specs build on those upgrades (e.g. CONTROL_SPEC §5.1 rotor protection rests
on the *inferred* internal-NTC channel). The items below are re-verifiable **without
hardware** — by static disassembly of the stock binary, datasheet cross-checks, Renode
(#19), and the upstream `CPU_STM32` block (§0.5). Work them before the bench milestones
they feed.

**Evidence precedence for WS500 facts** (the VSR predates the WS500 — the compiled stock
firmware is a *later derivative*, so lineage runs AVR → 2018 STM32 prototype → production
WS500 binary):

1. **Bench measurement** on the real unit (physical ground truth);
2. **the stock WS500 binary** (it *is* the product — its constants define what the shipped
   firmware assumes about the board);
3. **upstream STM32 prototype** (ancestral prior — useful where the binary is silent or our
   decode is ambiguous, stale where the product evolved);
4. **AVR gen-3 source** (older prior still — algorithm shapes and conventions).

So an upstream↔RE mismatch (34.33:1 vs ~2:1 divider, INA auto-detect vs hardcoded INA226)
is **expected evolution, not an RE error** — the binary wins. Upstream priors still bite in
two places: where our claim **isn't from the binary at all** (the unsourced "1 kHz" — both
upstream data points say hundreds of Hz, so V2 must read the real TIM1 config), and where
our **own reading of the binary is internally inconsistent** (the INA register-map/auto-detect
clash, V3 — evolution can't excuse an impossible decode).

| # | Claim to re-verify | Method | What it settles | Status |
|---|---|---|---|---|
| V1 | Pin map: which GPIO ports/pins are actually RCC-clocked + `GPIO_Init`'ed | re-derive from binary (MSP-init at `0x08002AEC`) | package question (GPIOD/E referenced-vs-used contradiction, IO_COVERAGE lines 8 vs 49); the DIP-pin contradiction (PA4/PA5/PA6/PB0/PB1 marked ✅ in IO_COVERAGE but absent from HW-spec §6b roster); **TIM1 BKIN pin** (listed ✅ but never identified — our `field_drive.c` already relies on it); TIM2 capture channel/pin | ⬜ |
| V2 | **Field PWM frequency** — "1 kHz-class" in IO_COVERAGE was unsourced. Priors converge on hundreds of Hz: **122 Hz** AVR (">400 Hz lossy" comment), **244 Hz** 2018 STM32 prototype, and **~400 Hz for the WS500 — verbal from Al Thomason (the designer), ~mid-2024 phone call, recalled 2026-07** (first-party but unwritten; below binary/bench in the hierarchy). `field_drive.c` default **changed 1 kHz → 400 Hz** (2026-07-23) on that basis; V2 stays open to confirm from the stock TIM1 config | recover stock TIM1 ARR/PSC from binary or Renode boot; Stage-A scope on the field wire (§5) is the bench-grade closure | confirms/corrects the 400 Hz driver default; gates bench field tests | 🔨 |
| V3 | INA "auto-detect INA226/228/238" — internally inconsistent: cited register map (0x01 SHUNT_V / 0x02 BUS_V / 0x03 POWER / 0xFF DIE_ID) is **INA226-only**; INA228/238 use a different map (VSHUNT 0x04, DEVICE_ID 0x3F) | re-examine I²C driver disassembly vs all three datasheets | which part(s) the stock FW really supports; correct register map for our `ina2xx.c` | ⬜ |
| V4 | §6b AF assignments; "×4 oversample" | STM32F072 datasheet AF table (note the forcing argument: USB owns PA11/12 ⇒ CAN *must* be PB8/9); RM0091 (F072 has **no HW oversampler** → confirm software averaging at `0x08014242`) | independent confirmation of pin map; correct wording | ⬜ |
| V5 | Mine the upstream `CPU_STM32` block (`SmartRegulator.h` v1.3.1) for WS500 facts | done 2026-07-23 | **Done — see outcome note below.** Corroborated: TIM1_CH1 field PWM, TIM2-as-µs-counter stator method, INA226 @0x40 regs 0x01/0x02 (+CONFIG 0x00 = `0x4523`, STATUS 0x06, 2.5 µV shunt LSB), IWDG use, 8 DIP inputs, Feature-In/Out, MCU family (alt target STM32F078xx). Upstream-silent: all GPIO pins (cubeMX-generated, not in source), BKIN, CH3N. New: prototype field PWM = **244 Hz**; config in **external I²C EEPROM @0x50**; **β3380 = FET temp, not battery** | ✅ |
| V6 | Boot the stock image in Renode (#19); log peripheral writes (TIM1 config, ADC scan setup, I²C traffic to 0x40 **and 0x50**) | dynamic corroboration of V1–V3, V7 | | ⬜ |
| V7 | **Config storage location** — upstream STM32 stores config in an **external I²C EEPROM @ 0x50** (M24C04-family, 16-byte pages, on the same bus as the INA226); RE guessed "internal flash likely" (IO_COVERAGE line 42) and the unknown-I²C list (0x0C/0x10/0x4C) doesn't include 0x50 | re-scan the binary's I²C address literals + `FLASH_IF` call sites; Renode I²C trace | where config lives (drives the M4 config-store design — flash page vs EEPROM driver) | ⬜ |
| V8 | **β3380 channel identity** — upstream defines Beta 3380 as the **onboard FET/driver temp** sensor, *not* battery; our RE labels PA3 (β3380) "battery-class"/BTS | re-check the PA3 conversion/clamps + how the harness BTS input is digitized; upstream NTC constants (10 K feed, 100 Ω ground-iso on external probes only) may distinguish onboard vs harness channels | which ADC channel is really battery temp — **feeds temp-comp and CONTROL_SPEC §5.1 rotor protection**; a swap here would mis-compensate charge voltage | ⬜ |

**V5 outcome note (2026-07-23):** the upstream `CPU_STM32` block is a **skeletal 2018
prototype** (HW "1.0.0", 2018-07-29) — field PWM, CAN, USB CDC, EEPROM config, and DIP read
are implemented, but the **entire sensing datapath (INA226 read, ADC/NTC sampling) is
stubbed**, and the STM32 scaler constants are placeholders (`AALT_SCALER` even has an
integer-division-to-zero bug; `VALT_SCALER` ≈ 2:1 vs our recovered 34.33:1 — different board
revision). So: upstream STM32 **header facts** (timers, I²C addresses, register values,
`FIELD_PWM_MAX 0xFE`) are usable; upstream STM32 **scalers are not ground truth**; and all
pin assignments live in a cubeMX project that isn't in the repo, so upstream can neither
confirm nor refute the disassembled pin map — V1 remains binary-only. The production WS500
firmware clearly evolved well past this snapshot (e.g. its INA variant auto-detect has no
upstream counterpart).

**Genuinely bench-only (don't chase in virtual):** field-driver topology/part numbers, exact
analog resistor values (only ratios are in FW), which of PA1/PA2 = ATS vs internal (identical
β in FW), RDP level, SWD/BOOT0 access — correctly scoped 🔵 in `IO_COVERAGE.md`.

**Doc corrections owed** (tracked, not yet applied): downgrade IO_COVERAGE ✅ items (BKIN,
DIP pins, "1 kHz", INA auto-detect) to match evidence; reconcile HW-spec §6b roster vs
IO_COVERAGE DIP claims after V1; CONTROL_SPEC §5.1 to note its dependency on the inferred
internal NTC and to add the bootstrap max-duty cap (§0.5); re-clone VSR upstream unshallow
if the pre-2015 license history ever matters.

## 1. Deliverables map

| # | Deliverable | Artifact | Milestone | Status |
|---|-------------|----------|-----------|--------|
| 1 | Project management | this doc → GitHub Issues/Milestones | M0 | 🔨 |
| 2 | Git hosting | remote exists, public | M0 | ✅ |
| 3 | HW documentation | `WS500_HARDWARE_SPEC.md`, `IO_COVERAGE.md` | M0–M2 | ✅ (open items) |
| 4 | Control/design spec | `CONTROL_SPEC_NEXTGEN.md` + `PROFILE_SPEC_LFP.md` (+ fault codes, loop numerics); **cross-referenced against the GPL VSR source (§0.5)** | M2.5–M3 | 🔨 |
| 5 | OS firmware | `Core/` (board/field/main real; sensors/ina/config/can stub) + pure `control/` core | M2–M3 | 🔨 |
| 6 | Config protocol + store | `config_protocol.c` + flash-page store | M4 | 🔨 / 🧩 (see #6a) |
| 6a | **Config strategy decision** | stock `$`-compatible vs new JSON profile schema (they conflict) | M2/M3 | 🧩 |
| 7 | Update/rollback/backup/recovery | `FLASH_AND_RECOVERY.md` (+ §3) | M1 | ⬜ |
| 8 | Client app | **WebSerial/WebUSB web app** (PC/Mac/Android: program+monitor+firmware, one codebase) + native `tools/ws500ctl/` CLI (scripting/CI/flash). iOS = monitor via CAN/VRM only. See `CLIENT_CONNECTIVITY.md` | M4 | ⬜ |
| 9 | **CAN Tx telemetry (NMEA2000 → Cerbo)** | broadcast the dialect-neutral snapshot as N2K PGNs; **near-term / low-risk** (read-only) | **M3** | 🔨 snapshot done |
| 10 | CAN Rx for control | BMS/DVCC ceilings + engine RPM into arbitration | M5 | ⬜ |
| 10a | **RV-C Tx dialect** (+ RBM election) | 2nd encoder over the same snapshot; **co-important** — a Cerbo's port is N2K *or* RV-C, so RV owners need this to see us | M3 (close behind N2K) | ⬜ |
| 11 | CAN docs | `CAN_INTEGRATION.md` | M3/M5 | 🔨 draft |
| 12 | User documentation | `USER_MANUAL.md` (install, config, LED codes, troubleshooting) | M6 | 🔨 draft |
| 13 | Testing + bug tracking | `TEST_PLAN.md`; Renode emulation + bench HIL; GitHub Issues | M0→M6 | ⬜ |
| 14 | Bring-up test firmware | `test-fw/` (§4). *ADC binding already recovered — this confirms scaling on bench* | M2 | ⬜ |
| 15 | Bench safety | `SAFETY.md` (§5); gates every hardware milestone | all HW | 🔨 |
| 16 | **License + third-party NOTICE** | **LICENSE = MIT** ✅ + `NOTICE` ✅ (CMSIS Apache-2.0 / HAL BSD-3 / NMEA2000 MIT-planned; Thomason courtesy attribution; **no GPL code in-tree — VSR is reference-only**); README/OPEN_SOURCE license text aligned | **M0 (now)** | ✅ |
| 17 | **OSS hygiene** | `CONTRIBUTING.md`, `CODE_OF_CONDUCT.md`, `SECURITY.md`, issue/PR templates, README badges | M0 | ⬜ |
| 18 | **Versioning + release** | `VERSION`/macro, `CHANGELOG.md`, tag/release flow | M0→M6 | ⬜ |
| 19 | **Emulation harness** | Renode model (STM32F072 + peripherals + INA stub) for hardware-free dev/CI; part of the §8 virtual-first strategy with the SIL plant sim (8.1) | M0→M1 | ⬜ |
| 20 | **Telemetry / logging** | log stream over USB CDC / CAN (per `CONTROL_SPEC`) | M4–M5 | ⬜ |
| 21 | **Robustness / error reporting** | §7: safe-state funnel, IWDG checkpoint policy, reset-cause + `.noinit` crash records, HardFault/NMI handlers, flash CRC + SRAM parity, PVD brown-out, peripheral error budgets | M3–M4 | ⬜ |

## 2. Milestones (safety and recovery come before any flash write)

Each milestone lists **exit criteria**. `→` marks a hard gate.

- **M0 — Infrastructure** *(mostly done)*.
  Done: public remote ✅, CI (`.github/workflows/build.yml`) ✅, HAL vendoring
  (`scripts/fetch_deps.sh`) ✅, `stm32f0xx_hal_conf.h` ✅.
  Done also: **license + NOTICE (#16)** ✅ (MIT + NOTICE, 2026-07-23).
  Remaining: OSS hygiene (#17), versioning scaffold (#18),
  convert this table to Issues/Milestones, **stand up the Renode emulation harness (#19)**.
  *Exit:* repo builds green in CI; MIT LICENSE + NOTICE in place; issues created; emulator
  runs the built ELF far enough to exercise `main()`.
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
  TIM2 RPM capture, DIO (enable/Feature-In/lamp/LED), **CAN Rx for control (#10)** (BMS
  permission/current) — plus inner-loop gain tuning, the §7 robustness layer (#21: IWDG,
  fault handlers, crash records), and bench bring-up. *Exit:* closed-loop CV hold on the
  bench supply into a dummy load, with fault cutoff verified; IWDG active; an induced
  HardFault provably lands in safe state + crash record + clean reboot.
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
| Accidental GPL ingestion (would forfeit the MIT/permissive goal) | legal/OSS | LICENSE+NOTICE done (#16 ✅); no-GPL-in-tree rule (§0.5); dependency additions require a license check |
| Single irreplaceable unit — **installed, in service, 48 V bank, 4 Ω/12 V rotor** (§5) | any HW test is high-stakes *and* disrupts a live system; rotor overdrive is the top hazard | virtual-first gauntlet (§8) gates Stage C; staged access ladder (§5): readings-only → config → bench flash; rotor clamp + duty cap proven in SIL first |
| Control-model drift (code vs spec) | rework, bugs | M2.5 reconciliation; spec is single source of truth |
| Draft specs with open questions | design churn | track `PROFILE_SPEC` §8 questions as issues; resolve before M3 coding they touch |
| Research confidence inflation (IO_COVERAGE marks inferred items ✅; specs build on them) | firmware written against wrong hardware facts (BKIN, DIP pins, INA variant) | §0.6 virtual queue V1–V6; downgrade IO_COVERAGE statuses to match evidence |
| Field PWM frequency unknown (unsourced "1 kHz" vs upstream's 122 Hz / ">400 Hz lossy") | field losses / driver stress on bench | V2 (recover stock TIM1 config) before any bench field test |
| Bootstrap gate driver forbids sustained 100 % field duty (no charge pump, §0.5) | field drive dropout / driver damage at high duty | compiled max-duty cap in the field module (CONTROL_SPEC open item) |

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

**Installed-unit reality (2026-07):** the one WS500 is **installed and in service on a 48 V
system with a 4 Ω (12 V-class) rotor** — it is not a spare bench unit, and any firmware
mishap also takes down a live charging system. Two consequences:

- **Rotor overdrive is the #1 hazard on this exact install.** A 12 V rotor on a ~48–57.6 V
  bus means sustained field duty above **≈25 % (≈21 % at 57.6 V absorption)** overdrives the
  rotor (100 % duty ≈ 12 A / ~580 W into a 3 A winding). The CONTROL_SPEC rotor duty clamp
  and the never-100 %-duty bootstrap cap (§0.5) are *the* critical protections here, and
  both must be proven in virtual (§8) before any custom firmware runs on this unit.
- **Hardware access is staged — readings first:**
  - **Stage A — observation only (safe now, stock firmware untouched):** USB `$` protocol
    readout (dump + archive the full stock config — also documents the stock parameter set),
    CAN bus sniffing (log the PGN set → validates `CAN_INTEGRATION.md`), and scope/DMM on
    harness wires — field PWM wire (**measures the real PWM frequency: closes §0.6 V2 at
    the bench level**), stator wire (frequency vs known RPM → K), battery/alt sense vs a
    reference meter (validates the 34.33:1 divider and INA readings end-to-end).
  - **Stage B — reversible config interaction:** only after the Stage-A config archive
    exists; `$` writes are stock-supported and restorable from the dump.
  - **Stage C — custom firmware:** only after M1 (proven DFU backup/restore) *and* the §8
    virtual gauntlet passes. First flash happens on the bench (unit temporarily removed),
    never in-situ.

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

## 7. Robustness, watchdogs & error reporting (decided) — deliverable #21

What "good code" requires on this CPU. Constraint that shapes everything: the STM32F072 is a
**Cortex-M0** — HardFault only (no MemManage/BusFault/UsageFault vectors), **no MPU**, no
stack-limit registers — so protection is layered and every failure path funnels to one place.

- **R0 — Safe state, one funnel.** A single `enter_safe_state()`: field PWM off
  (TIM1 `BDTR.MOE = 0` + duty 0 + pin low), minimal code, no HAL, callable from any context
  including fault handlers with a corrupt stack. Every mechanism below lands here. The
  hardware backstop beneath it is TIM1 **BKIN** (pin still to be identified — §0.6 V1).
- **R1 — IWDG, checkpoint-kicked.** IWDG (independent LSI clock — survives main-clock
  failure), **windowed** mode (F0 IWDG has `WINR` — catches runaway-fast loops too). Kicked
  from the main loop **only** when every subsystem checkpoint bit (control tick, sensor
  acquisition, CAN/USB service) has reported within budget — a task-alive vector, **never a
  naked timer/ISR kick**. Escalation: a `.noinit` consecutive-watchdog-reset counter; ≥N →
  boot straight into field-off LIMP and stay there (don't oscillate reset↔run). Dev builds
  freeze IWDG under debug via DBGMCU. The `WDG_SW` option byte (watchdog hardware-started
  from reset) is a later hardening option — deferred, it complicates bench debugging.
- **R2 — Reset-cause + crash records.** On every boot decode `RCC->CSR` flags (POR, NRST
  pin, software, IWDG, WWDG, low-power), keep per-cause counters. A small crash-record ring
  in a **`.noinit` RAM section** (magic + CRC to detect cold power-up): uptime, reset/fault
  cause, faulting PC/LR, regulator state, and a telemetry snapshot (Vbat, field duty, temps).
  Last record optionally mirrored to a flash "black box" page (same wear strategy as the
  config page). Surfaced via the USB `$` protocol, the telemetry stream (#20), and an LED
  blink code — a fault you can't read out didn't happen.
- **R3 — Fault handlers that report, then reset.** Two-stage **HardFault** handler: naked
  asm captures the stacked frame (PC/LR/xPSR + MSP/PSP) → C stage calls `enter_safe_state()`,
  writes the crash record, `NVIC_SystemReset()`. **NMI** = SRAM **parity error** (enable the
  F072's RAM parity check option) → same path, treated as fatal. `Default_Handler` for
  unexpected IRQs → same path. **Never a bare `while(1)`** — spinning with the field
  energized is the worst possible failure mode.
- **R4 — Integrity checks.** Boot-time **CRC32 of the flash image** using the hardware CRC
  unit against a linker-embedded value (also the update-verification primitive for M4).
  **Stack painting + high-watermark** check in housekeeping (no MPU means overflow is
  otherwise silent); place the stack at the **bottom of RAM** in the linker script so
  overflow faults into invalid memory instead of quietly corrupting `.bss`/`.noinit`.
- **R5 — Power supervision.** **PVD** interrupt set above the brown-out threshold: on
  falling Vdd → safe state + flush the crash record before power collapses; BOR level via
  option bytes. An alternator regulator lives on a supply that load-dumps — this is not
  optional.
- **R6 — Peripheral error budgets.** Every driver keeps error counters that escalate into
  the existing `faults.c` ladder instead of silently retrying forever: I²C/INA transaction
  failures (retry → bus reinit → fault after N), CAN bus-off (auto-recovery + counter),
  ADC/DMA sequence sanity. All counters visible in telemetry (#20).
- **Assert policy.** Release `ASSERT` logs file-id/line into a crash record and enters safe
  state — it never just spins; HAL `assert_param` routes to the same macro.

*Milestone hooks:* M3 exit adds "IWDG active; induced HardFault provably lands in safe state
+ crash record + clean reboot." M4 adds the flash black-box + `ws500ctl` readout of crash
records and reset counters.

## 8. Virtual-first test strategy (decided 2026-07) — maximize testing before hardware

Policy: **everything that can be proven without the unit, is** — the only WS500 is live on
a 48 V system (§5). Four virtual layers, cheapest first; all run in CI:

- **8.1 SIL — simulated charge cycles against a plant model.** `control/` is pure and
  HAL-free by design, so drive `ctrl_tick()` natively against a simple plant: LFP battery
  model (OCV/SOC curve, internal resistance, thermal mass), alternator model (output vs RPM
  × field duty, belt/pulley), harness delays. Scenarios: full CHARGE→REST cycles at 16S/48 V,
  cold/hot temp comp, BMS ceiling steps, engine-speed transients, load dump, sensor dropout
  and implausible-value injection, **rotor-clamp verification with this exact install's
  parameters (4 Ω / 12 V rotor on 48–57.6 V bus → duty ceiling ≈25 %/≈21 %)**, and
  long-soak (accelerated time) for state-machine leaks. This is the main gauntlet — a
  regression suite, not a demo.
- **8.2 Renode — whole-firmware emulation (#19).** STM32F072 machine model + INA226 I²C
  stub + CAN loopback: boot the real ELF, exercise `main()`'s loop, fault paths (§7:
  induced HardFault → safe state + crash record), watchdog starvation, and DFU-adjacent
  boot behavior. Also dry-runs any risky bench procedure first (§5 rule 7).
- **8.3 Stock-binary verification (§0.6 V1–V4, V6–V8).** Every RE fact our drivers depend
  on gets re-derived or refuted before the driver is trusted (BKIN, PWM frequency, INA
  register map, config storage, β3380 channel identity).
- **8.4 Property/fault-injection tests on the pure core.** Extend `control/test/` beyond
  example-based: sweep-based invariant checks (field ≤ clamp under *all* input
  combinations; faults latch; arbitration monotonicity), boundary sweeps on profile
  parameters, and randomized sensor-noise runs with fixed seeds.

*Gate:* Stage C in §5 (first custom-firmware flash) requires 8.1–8.4 green in CI, plus the
§0.6 queue resolved for every constant the flashed build uses.
