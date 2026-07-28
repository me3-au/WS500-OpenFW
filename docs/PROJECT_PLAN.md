# WS500-OpenFW — Project Plan

Status legend: ✅ done · 🔨 in progress · ⬜ not started · 🧩 decision needed

Repo is **public** at `github.com/me3-au/WS500-OpenFW` (personal account `me3-au`). This
doc is the master tracker, mirrored as **GitHub Issues + Milestones** (7 milestones M0–M6).
**Issue sync 2026-07-24:** closed #5 (BOOT0 — DFU entry via reset button), #9 (BKIN —
refuted V1/V2), #12 (state machine — done in `control/`), #14 (superseded by the two
specs); created #23–#35 covering OSS hygiene, versioning, Renode (#19→GH#25), the SIL
gauntlet (GH#26), robustness §7 (GH#27), decision #6a (GH#28), V7 (GH#29), web client,
CAN Rx, RV-C, SAFETY.md, spec gaps, telemetry.

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
| `FLASH_AND_RECOVERY.md` | Backup/rollback/update procedures | ✅ extracted from §6 (2026-07-26); M1 rehearsal pending |
| `SAFETY.md` | Bench-safety rules | ✅ extracted from §5 (2026-07-26) |
| `STAGE_A_RUNSHEET.md` | Step-by-step **observation-only** bench procedures (7) executing the §5 Stage-A ladder + the read-only M1 backup half; results-capture table keyed to §0.6 V-items | 🔨 draft 2026-07-28, owner review pending |
| `test-fw/README.md` | Bring-up test firmware spec | ✅ extracted from §4 (2026-07-26, BKIN + 0x0C/0x10/0x4C items corrected per V1–V3/V7); firmware itself ⬜ |
| `CLIENT_CONNECTIVITY.md` | Programming/firmware/monitoring across PC/Mac/iOS/Android over USB+CAN (decision) | ✅ |
| `QUICK_START.md` | Get-going guide (flash, connect, pick profile, run) | 🔨 draft |
| `USER_MANUAL.md` | User manual + algorithm explanation (how the regulator decides) | 🔨 draft |
| `CAN_INTEGRATION.md` | CAN/NMEA2000 integration (Tx telemetry = V1; Rx BMS/DVCC/engine + multi-unit sync = **V2**, §1.1) | 🔨 draft |
| `OPEN_SOURCE.md` | Project overview, architecture, provenance, build/test, contributing | 🔨 draft |
| `TEST_PLAN.md` | see deliverable #13 | ✅ assembled from §8/§1/§2 (2026-07-26) |
| `DECISION_6A_CONFIG_STRATEGY.md` | Decision #6a ADR (config strategy) | ✅ **ACCEPTED: B — clean break** (owner, 2026-07-26); app translates stock dumps |
| `VERSIONING.md` | Client↔protocol↔config↔firmware compatibility standard (SemVer + schema_version + proto handshake + caps flags) | ✅ adopted 2026-07-26 |

> The old plan named a single `SOFTWARE_DESIGN_SPEC.md` that was never written; that role is
> filled by `CONTROL_SPEC_NEXTGEN.md` + `PROFILE_SPEC_LFP.md`. Remaining design gaps to fold
> into those two: **fault-code bit enumeration**, **field PWM frequency** (see §0.6 V2), and
> **inner control-loop numerics (PI gains, loop rate)** — none specified anywhere yet.

> `docs/` also holds two gitignored reference PDFs (WS500 Product Manual 10.21.24;
> Wakespeed Comms & Config Guide v2.6.1) used by the RE work. A third primary source
> joined the corpus 2026-07-28 and is **not** in-repo: the **RVIA "RV-C Specification
> Full Layer", rev. 2025-07-31**, a free public download from rvia.org. It settled the
> whole RV-C sign-off batch from spec text instead of vendor-doc inference — including
> refuting the suspected ADDRESS_CLAIM DP bug and exposing seven real encoding defects
> (CAN_INTEGRATION.md §9). Worth knowing it exists before anyone infers RV-C behaviour
> from a vendor manual again. Note `CONTROL_SPEC_NEXTGEN.md`
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
| V1 | **Pin map — RESOLVED from the stock binary (2026-07-23).** All 17 `HAL_GPIO_Init` sites + RCC AHBENR decoded. **PACKAGE = 64-pin LQFP64 (STM32F072RB)** — only GPIOA/B/C clocked (D/E never), and PC4/PC5/PC10 are configured (don't exist on 48-pin CB); refutes both the 48-pin and the 100-pin guesses. Confirmed: PA8/PB15 TIM1_CH1/CH3N AF2, CAN PB8/9 AF4, I2C1 PB6/7 + I2C2 PB10/11 AF1, USB peripheral live. **BKIN REFUTED** (no break AF pin — corroborates V2; cutoff is software). **DIP inputs CONFIRMED**: PA4/5/6 **+PA7** and PB0/1 **+PB2**, all pull-up (resolves the doc conflict, adds two undocumented). **Stator = PA10 via EXTI rising-edge (was 🔴)**, not a TIM2 channel — EXTI counts edges + diffs TIM2->CNT. PB13 = enable/feature input (polled). Only **6 live IRQs**: EXTI4_15, DMA1_CH1, ADC, **TIM7 (system tick)**, CAN, USB — I²C/CAN move by poll/DMA | stock binary (done) | package, DIP pins, BKIN, stator pin all settled; feeds board.h/HW-spec corrections | ✅ |
| V2 | **Field PWM frequency — RESOLVED from the stock binary (2026-07-23).** `MX_TIM1_Init` @0x08005024: PSC=**326**, ARR=**1024**, TIM1CLK=48 MHz (HSE 8×6=48; APB÷2 → timer doubler) → **143.2 Hz, 10-bit duty**. Not 400/244/122. The verbal "~400 Hz" (Al, 2024) was imprecise memory / the loss ceiling, not the setpoint. `field_drive.c` **corrected 400 → 143 Hz** (PSC=326, FIELD_PWM_MAX=1024) to mirror the stock field-driver tuning. **Also found:** TIM1 channels = CH1 (PA8 field) + CH3/CH3N (PB15) both configured; **BDTR BKE=0 → stock does NOT use hardware BKIN** (field cutoff is software MOE-clear); **TIM2 = plain free-running 32-bit counter @ ~979.6 kHz (1.02 µs/tick), NOT input capture** — stator period is software CNT-diffing | stock binary (done); Stage-A scope on field wire = bench confirmation | 400 Hz driver default corrected to 143 Hz; BKIN + TIM2-capture claims refuted | ✅ |
| V3 | **INA driver — RESOLVED from the stock binary (2026-07-23).** The FW drives **INA226 only**, hardwired: reader `0x800B530` reads regs **0x06** (Mask/Enable, conversion-ready gate), **0x02** (bus V), **0x01** (shunt V), 16-bit big-endian, on **I²C1 @ 0x40**. **Auto-detect is REFUTED** — the die-IDs 0x2260/0x2280/0x2380 and mfr 0x5449 don't exist in the image, no ID register (0xFF/0x3F) is ever read, no detection table. The HW-spec "INA226/228/238 auto-detect" paragraph is fiction — strike it. **CALIBRATION is computed at runtime from the shunt ratio** (not a magic constant) — our `ina2xx.c` must derive CALIB from the configured shunt, not copy 0x4523. Also: the "0x0C/0x10/0x4C secondary I²C cluster" is **REFUTED** — those were length/size args misread as addresses | stock binary (done) | INA part + register map + CALIB strategy settled for `ina2xx.c` | ✅ |
| V4 | **ADC scaling — RESOLVED from the stock binary (2026-07-23).** 7-ch scan, **×4 software averaging** (routine `0x08014242`; F072 has no HW oversampler — confirmed), **12-bit** (FS 4095). Constants: **β3950** (PA1/PA2) and **β3380** (PA3) stored as *integer* literals (why the earlier float search missed them); **34.3333** divider + **3.3** Vref (PC5, FS ≈113.3 V); **10 kΩ** NTC pull-up. NTC = Beta/Steinhart (`logf`); voltage = linear. AF pin map independently matches V1 | stock binary (done) | scaling constants confirmed for our `sensors.c`; "×4 oversample" wording corrected | ✅ |
| V5 | Mine the upstream `CPU_STM32` block (`SmartRegulator.h` v1.3.1) for WS500 facts | done 2026-07-23 | **Done — see outcome note below.** Corroborated: TIM1_CH1 field PWM, TIM2-as-µs-counter stator method, INA226 @0x40 regs 0x01/0x02 (+CONFIG 0x00 = `0x4523`, STATUS 0x06, 2.5 µV shunt LSB), IWDG use, 8 DIP inputs, Feature-In/Out, MCU family (alt target STM32F078xx). Upstream-silent: all GPIO pins (cubeMX-generated, not in source), BKIN, CH3N. New: prototype field PWM = **244 Hz**; config in **external I²C EEPROM @0x50**; **β3380 = FET temp, not battery** | ✅ |
| V6 | **Stock image booted in Renode — RESOLVED (2026-07-26).** Dynamic peripheral-bus corroboration of the static RE: **TIM1 PSC=0x146 (326), ARR=0x400 (1024) written live → 143.2 Hz / 10-bit duty (V2 ✓✓)**, PWM-mode-1 on CH1; clock init = HSE→PLL, SYSCLK=PLL, APB÷2 (V2 clock math ✓); **TIM7 = system tick** (sole early NVIC IRQ, real vector handler, uwTick polling — V1 ✓); FreeRTOS present (SVC/PendSV/SysTick priorities — corroborates V7's osDelay); ADC configured continuous+DMA scan (V4 ✓); **I²C1 probe times out (18 M-read poll), then I²C2 carries both devices: CR2 SADD=0x80 → 7-bit 0x40 INA226 (3-byte AUTOEND config write, V3 ✓) and SADD=0xAC → 0xA0-family computed EEPROM address (0x50\|block, 1-byte word-address + RELOAD read — the exact V7 24C16 pattern) — V7's I²C1→I²C2 rebind story observed live ✓✓**. Renode 1.16.1 needed 3 Python stubs (RCC ready-bit mirror, FLASH ACR, ADC handshakes — `renode/v6-stubs/`) + HAL-tick advancement (stock spins on uwTick with PRIMASK set pre-scheduler under emulation) | Renode + stubs (done) | V1–V4, V7 all dynamically corroborated; no static claim contradicted | ✅ |
| V7 | **Config storage — RESOLVED from the stock binary (2026-07-24, deeper pass; findings in `../New folder/i2c2_findings.md`).** The config store is a **24C16-class 2 KB serial EEPROM at 7-bit 0x50**, write-protected by **PA15 = /WP** (driven low around writes). The earlier "EEPROM@0x50 refuted — no 0xA0 transactions" verdict was **wrong because the DevAddress is computed, not literal**: `0xA0 | ((addr>>8)&7)<<1` (`adds …,#0xA0` at `0x0800DDF4` read / `0x0800E6C8` write) — block-select bits + 8-bit word address = textbook 24C16. Read `fn_DDC4`: 1-byte word address, 256-byte block chunking, retry×3. Write `fn_E694` → `HAL_I2C_Mem_Write` (`0x0800300C`): **16-byte page programming with 7 ms `osDelay` per page** (write-cycle wait ⇒ EEPROM, not FRAM). **Config-store confirmed, not display**: boot-load cluster `0x0800C3E0–0x0800C5F0` reads **0x84-byte records** validated by magic (`0x873A/0xC03A`, `0xF9AC/0xA97`) + CRC (`0x080131E0`); write-on-change is read-modify-write (`fn_D680`). Driver is **polled HAL, not interrupt-driven** (both I²C IRQ vectors are the weak default); the previously-fingered `0x8014050/0x8014084` were mis-identified — they're the **UART console TX path** (PA9, string table `0x0801B418`). **Bus correction (revises V3):** the shared active-handle slot `0x20000174` is written exactly once, to **hi2c2** at boot (`0x0800555E`), and hi2c1 is DeInit'd — so **both the EEPROM *and* the INA226 run on I²C2 (PB10/PB11, AF1)**, not I²C1. The boot INA probe on the zero-init hi2c1 fails deterministically, then the code rebinds everything to I²C2; V3's "INA226 @ I²C1" was the pre-rebind reading. Bench falsification (Stage-A, readings-only): scope PB6/PB7 vs PB10/PB11 at boot — the pair carrying 0x40/0xA0 traffic is live, the other idles after the DeInit | disassembly done (`../New folder/i2c2_findings.md`); Stage-A scope confirms bus | resolves the M4 config-store design → **24C16 EEPROM driver @0x50 + PA15 /WP on I²C2**; moves INA226 to I²C2 | ✅ |
| V8 | **β3380 channel identity — RESOLVED from the stock binary (2026-07-23), confidence med-high.** PA3/β3380 is the **FET/driver over-temp sensor, NOT battery**: its result (`0x200003E0`) drives a **125 °C over-temp fault** (`0x4029`), an over-temp status classifier, and telemetry — and **nothing feeds any `V_target += k·(Tref−T)` temp-comp**. Battery temp for temp-comp arrives over **CAN/BMS**, not the local ADC (agrees with upstream VSR; refutes our "BTS" label). **Design consequence:** our temp-comp must source battery temp from CAN (or a correctly-identified harness input), and PA3 belongs to the **thermal governor** (`thermal.c`), not temp-comp. β3950 (PA1/PA2, 160 °C clamp) are the alternator/hot-probe channels | stock binary (done) | corrects the sensor labelling; **redirects temp-comp sourcing and CONTROL_SPEC §5.1** | ✅ |

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

**Doc corrections owed** (tracked, not yet applied to HW-spec/IO_COVERAGE — binary now
settles most of them):
- **Package: STM32F072RB, LQFP64** (V1) — replace the "package TBD / 48 vs 100-pin" language
  in HW-spec §2/§7 and IO_COVERAGE lines 6–8/49 with the confirmed 64-pin part.
- **Field PWM = 143.2 Hz** (V2) — replace IO_COVERAGE's unsourced "1 kHz-class".
- **BKIN not used** (V1+V2) — downgrade IO_COVERAGE's ✅ "TIM1 BKIN field fault cutoff";
  stock cutoff is software MOE-clear. (Our firmware keeps BKIN only as an optional
  improvement, valid only if a fault comparator is wired — pin not routed in stock.)
- **Stator = PA10 / EXTI rising edge + TIM2 timebase** (V1+V2) — correct "TIM2 input
  capture" in HW-spec §6c line 177 / IO_COVERAGE line 25 (was 🔴 pin).
- **DIP inputs = PA4/5/6/7 + PB0/1/2, pull-up** (V1) — reconcile the HW-spec §6b roster
  with IO_COVERAGE's ✅; add the newly-found PA7/PB2.
- **System tick = TIM7** (V1); active peripheral set = ADC+DMA, CAN, USB, TIM7, EXTI only.
- **INA226 only, no auto-detect** (V3) — strike the HW-spec §6c "INA226/228/238 auto-detect
  via die-ID" paragraph; regs 0x01/0x02/0x06 big-endian @0x40; CALIB from shunt.
  **Bus corrected (V7, 2026-07-24):** the INA226 is on **I²C2**, not I²C1 — the stock FW
  DeInits I²C1 at boot and binds both the INA226 (0x40) and the config EEPROM (0x50) to
  I²C2 (PB10/PB11). Bench-scope confirm (Stage A). Our `ina2xx.c` carries a BUS CAVEAT
  comment and the bus is tracked as GH#36.
- **Config store IS a 24C16 EEPROM @0x50 on I²C2, /WP = PA15** (V7 — RESOLVED 2026-07-24;
  the earlier "NOT EEPROM@0x50" was wrong, the device address is computed not literal).
  Not internal flash (that part stands — stock MCU never programs its own flash). Remove
  IO_COVERAGE's "internal flash likely (FLASH_IF)". §6 FLASH_AND_RECOVERY: config lives in
  an external I²C EEPROM, not a flash page. *(IO_COVERAGE + HW-spec edits applied 2026-07-24.)*
- **β3380/PA3 = FET/driver temp, not BTS** (V8) — relabel HW-spec §6b/§6c; temp-comp sources
  battery temp from CAN.
- ~~CONTROL_SPEC §5.1 internal-NTC dependency~~ — **closed 2026-07-28.** V8 confirms PA3/β3380
  *is* the driver-stage channel (with a stock 125 °C fault), which is exactly what §5.1's
  proxy guard needs; §5.1 now cites V8 instead of resting on an inferred channel. The same
  pass corrected four places where CONTROL_SPEC still promised a **TIM1 BKIN hardware
  cutoff** that V1+V2 refuted — a spec asserting a hardware backstop that does not exist is
  the one doc error that could justify weakening a software protection, so §0.1 now carries
  an explicit "there is no hardware field-cutoff backstop" callout. `IO_COVERAGE.md` and the
  firmware were already correct; only the spec lagged.
- **NEW gap found while drafting the Stage-A runsheet (2026-07-28): `WS500_HARDWARE_SPEC.md`
  §6c's harness pinout table has no field-output wire entry** (it enumerates wires 1–4, 6,
  8–13). So the one Stage-A procedure that needs to physically find a wire — scoping the
  field PWM to close V2 at bench tier — cannot say "probe wire N". The runsheet makes
  physical identification an explicit first sub-step rather than guessing. Fill the table
  in at the bench and the guesswork disappears; until then this is `bench-pending`.
- Still open: bootstrap max-duty cap (§0.5);
  re-clone VSR upstream unshallow if pre-2015 license history ever matters.
- **Update our own `board.h`**: STATOR path is EXTI10 (PA10), not a TIM2 capture channel;
  the `STATOR_TIM = TIM2` define is the timebase, and RPM will be EXTI-edge + CNT-diff.

## 1. Deliverables map

| # | Deliverable | Artifact | Milestone | Status |
|---|-------------|----------|-----------|--------|
| 1 | Project management | this doc → GitHub Issues/Milestones (synced 2026-07-24) | M0 | ✅ |
| 2 | Git hosting | remote exists, public | M0 | ✅ |
| 3 | HW documentation | `WS500_HARDWARE_SPEC.md`, `IO_COVERAGE.md` | M0–M2 | ✅ (open items) |
| 4 | Control/design spec | `CONTROL_SPEC_NEXTGEN.md` + `PROFILE_SPEC_LFP.md` (+ fault codes, loop numerics); **cross-referenced against the GPL VSR source (§0.5)** | M2.5–M3 | 🔨 |
| 5 | OS firmware | `Core/` (board/field/main real; sensors/ina/config/can stub) + pure `control/` core | M2–M3 | 🔨 |
| 6 | Config protocol + store | **Firmware side ✅ 2026-07-26**: codec (310-B v1 record, CRC-32) + §1/§3 validator (named codes, never repairs) + two-slot power-fail-safe store + **JSON-lines protocol** (VERSIONING.md hello, cfg-get/cfg-set fixed point, bit-exact float round-trip, bounded no-alloc parser in the pure core). **Remaining**: USB CDC transport glue (with #20/GH#35) + bench round-trip | M4 | 🔨 |
| 6a | **Config strategy decision** | **DECIDED: B — clean break to the PROFILE_SPEC §7 JSON schema**; packed binary+CRC in EEPROM; stock-dump translation lives in the client app | M2/M3 | ✅ (2026-07-26) |
| 7 | Update/rollback/backup/recovery | `FLASH_AND_RECOVERY.md` (+ §3) | M1 | ⬜ |
| 8 | Client app | **WebSerial/WebUSB web app** (PC/Mac/Android: program+monitor+firmware, one codebase) + native `tools/ws500ctl/` CLI (scripting/CI/flash). iOS = monitor via CAN/VRM only. See `CLIENT_CONNECTIVITY.md` | M4 | ⬜ |
| 9 | **CAN Tx telemetry (NMEA2000 → Cerbo)** | **Firmware side ✅ 2026-07-27 (GH#18)**: pure encoders for the §2 PGN set (127508/127506/127488/127750, 126983/126985 alerts with plain-language text, 126996/126998, proprietary fast-packet) + pure ISO 11783-81 **address claim** (NAME, contest/yield/defend, null-address exhaustion) + pure **Tx cadence engine** + bxCAN glue @250 kbit/s (drop-not-block ring, real `ESR.BOFF` → §7 R6 budget). 241 host checks. **Sign-offs closed 2026-07-28** (CAN_INTEGRATION.md §9): device class 35, preferred address 34, DB version 2100, LEN 1 and the mfg code all signed off — **but device *function* was wrong: 140 is "Engine", not "Alternator" (141)**, so a Cerbo would have listed this regulator as an engine. Fix tracked in §9. **Bench-pending**: real-bus enumeration + how a Cerbo categorizes the device once 141 lands (§8 caveat) | **M3** | ✅ fw side |
| 10 | CAN Rx for control — **DEFERRED TO v2** (owner, 2026-07-28) | BMS/DVCC ceilings + engine RPM into arbitration. **Not a v1 feature.** CAN_INTEGRATION.md §0 always said control-in "comes later"; a 2026-07-27 session built a first slice against that ordering and it has been reverted off `main` (the work, plus an unreviewed CVL ceiling and pre-disconnect ramp, is parked on branch `wip/can-in-v2-cvl-predisconnect`). Re-entry needs: a real design pass on **DVCC** (a GX does **not** push DVCC limits to third-party devices — only the battery's own broadcast frames reach us, so GX-originated limits are invisible; see CONTROL_SPEC §2.4), the CVL consumer, pre-disconnect, a `bms_required` flag, and the field-path safety gate the parked ramp never passed | **v2** | ⬜ |
| 10a | **RV-C Tx dialect** (+ RBM election) | **Firmware side ✅ 2026-07-27**: pure `rvc_encode.c` (CHARGER_STATUS, CHARGER_STATUS_2, DC_SOURCE_STATUS_1/2/3 — all single-frame, no fast-packet) + `rvc_sched.c` (cadence + **RBM election**: DC_SOURCE DGNs defer to a higher-priority master, CHARGER_STATUS never gated) over the same snapshot; dialect selector in `can_n2k.c` defaults **RV-C off** (driver state, no config-schema bump — TODO(GH#10)). 105 host checks. **⚠ ADDRESS_CLAIM DP — RESOLVED 2026-07-28, implementation was CORRECT.** The official RVIA *RV-C Specification Full Layer* (rev. 2025-07-31) is publicly downloadable and settles it from primary text in three places: ADDRESS_CLAIMED is `EE00h` = **PGN 60928, DP=0** — exactly the J1939 encoding `n2k_addrclaim.c` reuses. The feared `0x1EE00` was a vendor-doc artifact; do not fork the claim machine. **But the same pass found seven encodings that ARE wrong** (CAN_INTEGRATION.md §9): device function 140→141, RV-C preferred address 35→128 (35 is reserved static space), instance 0→1 (0 = "Invalid"), priority 100→80, CHARGER_STATUS current → 0x7D00-offset (as shipped, +12 A decodes as −1588 A), percent 0.4→0.5 %/bit, and DC_SOURCE_STATUS_1 current sign (a charging bank read as discharging). All Tx-only, so misinformation on a shared bus rather than a hazard. Also settled: the "30/129 are an inverter-charger's codes" worry is **moot** — an RV-C NAME has no class/function fields at all (§3.3.3) — and industry group 0 is spec-mandated, not a fallback | M3 (close behind N2K) | ✅ fw side |
| 11 | CAN docs | `CAN_INTEGRATION.md` | M3/M5 | 🔨 draft |
| 12 | User documentation | `USER_MANUAL.md` (install, config, LED codes, troubleshooting) | M6 | 🔨 draft |
| 13 | Testing + bug tracking | control-core unit tests (CI) ✅ + **SIL gauntlet `sim/` (CI, §8.1)** ✅; `TEST_PLAN.md` ⬜; Renode ⬜; bench HIL ⬜; GitHub Issues ✅ | M0→M6 | 🔨 |
| 14 | Bring-up test firmware | **Built 2026-07-27**: `test-fw/` is a second CMake target (`ws500-testfw.elf`, CI-built) sharing `board.c` + the production drivers — USB-CDC console with `gpio`/`adc`/`field`/`cutoff`/`can`/`i2cscan`. Field path is funnelled through `field_guard.c`: compiled-in **20 % duty cap**, field-off default, and a **5 s self-expiry** so a dropped console can't leave the field energised. The `i2cscan` command is the bench falsification step for the `ina2xx.c` I²C1-vs-I²C2 bus caveat (GH#36). *Never run on hardware — that is M2 itself* | M2 | ✅ fw side |
| 15 | Bench safety | `SAFETY.md` (§5); gates every hardware milestone | all HW | 🔨 |
| 16 | **License + third-party NOTICE** | **LICENSE = MIT** ✅ + `NOTICE` ✅ (CMSIS Apache-2.0 / HAL BSD-3 / NMEA2000 MIT-planned; Thomason courtesy attribution; **no GPL code in-tree — VSR is reference-only**); README/OPEN_SOURCE license text aligned | **M0 (now)** | ✅ |
| 17 | **OSS hygiene** | `CONTRIBUTING.md` (no-GPL + safety-gate rules), `CODE_OF_CONDUCT.md`, `SECURITY.md` (unsafe-charging = security-priority), issue/PR templates incl. hardware-fact provenance template, README badges — all done 2026-07-24 | M0 | ✅ |
| 18 | **Versioning + release** | `VERSION` (0.1.0-dev) + `Core/Inc/version.h` + `CHANGELOG.md` ✅ (2026-07-24); tag/release flow exercised at M6 | M0→M6 | 🔨 |
| 19 | **Emulation harness** | Renode model (STM32F072 + peripherals + INA stub) for hardware-free dev/CI; part of the §8 virtual-first strategy with the SIL plant sim (8.1). ✅ **CI-green 2026-07-26**: `renode/` harness boots the real ELF, `ctrl_tick` liveness verified in the `emulation` job. V6 stock-trace use still ⬜ | M0→M1 | ✅ |
| 20 | **Telemetry / logging** | **USB half ✅ 2026-07-27**: MIT CDC-ACM transport (ST middleware rejected, SLA0044) + 1 Hz JSON telem line + telem-get with the §7 diag surfaces; caps ["cfg","telem"]. Bench-pending: real-host enumeration; release: permanent PID. **CAN half ✅ 2026-07-27** — #9's proprietary fast-packet carries the full snapshot (field effort, binding source + ceiling, temps, RPM state, active profile, fault bits) at 1.5 s. **Error counters ✅ 2026-07-28**: `can_boff` / `can_txdrop` (from `can_n2k_bus_off_count()` / `can_n2k_tx_dropped_count()`) now ride the `diag` object of the 1 Hz line, closing the §7 R6 "every error counter is visible" requirement and the `TODO(GH#10)` in `can_n2k.h`; asserted in the pure-layer test (2125 host checks). Remaining: bench host-enumeration only | M4–M5 | 🔨 |
| 21 | **Robustness / error reporting** | §7 R0–R6 ✅ **implemented + CI-proven 2026-07-27** (GH#27 closed): funnel, windowed checkpoint IWDG, `.noinit` crash ring + reset-cause counters, two-stage M0 fault handlers, flash CRC (report-only) + stack painting + bottom-of-RAM stack, PVD, err budgets. Renode proves the M3 fault-path criterion in CI. Bench-pending: IWDG/PVD behavior, SRAM-parity + BOR option bytes (manual, M1-adjacent); telemetry surface with #20 | M3–M4 | ✅ fw side |

### 1.1 V2-deferred scope (owner decision, 2026-07-28)

**All CAN-IN (control inputs over CAN) is out of V1** and moves to V2:

- **BMS control-in** (CAN-BMS/REC/JK 0x351/0x355/0x356/0x35A + Victron BMS paths) —
  ceilings, SOC, alarms into arbitration.
- **BMS pre-disconnect handling** (soft field ramp before contactor opening).
- **Victron DVCC via GX/Cerbo** (CVL/CCL/DCL).
- **Victron / external battery-monitor V & I ingestion** (N2K 127508/127506 —
  battery-side current truth for tail-exit re-arm).
- **J1939 engine RPM / coolant** (stator RPM is the sole V1 RPM source).
- **Multi-WS500 coordination/sync** (leader/follower load sharing).

CAN **Tx** (N2K + RV-C telemetry out, #9/#10a) **stays in V1** — read-only broadcast,
cannot affect the loop.

**V1 consequences of the deferral** (each already consistent with the specs' fallback
posture, now the *only* posture in V1):

1. Arbitration runs on **profile + hardware limits only**; the BMS/DVCC ceilings simply
   aren't in the min() (CAN_INTEGRATION.md §0 stance, now V1-final).
2. ~~**The committed #10 slice needs a default-off gate**~~ — **resolved by the revert, not
   by a gate (verified in-tree 2026-07-28).** The `b2da41a` revert took the whole decoder
   out: `can_drain_rx()` now hands frames only to `n2k_ac_rx()` (address claim) and
   `rvc_sched_rx()` (RBM election); the Rx filters admit PGN 60928/59904 **plus filter
   bank 2's RV-C `DC_SOURCE_STATUS_1/2` DGNs, which the RBM election needs**
   (`can_n2k.c` — election input only, never a control input); and `main.c` passes
   `bms_ccl_w = CTRL_CEILING_INACTIVE` unconditionally. There is no control-Rx path left
   to gate. No firmware task.
3. **⚠ But the revert left a live mis-wiring in the fault ladder.** `main.c` still maps
   the CAN error budget onto the control core's BMS-loss fault:
   `if (errb_faulted(ERRB_CAN)) f |= CTRL_FAULT_LOST_BMS;` — and `CTRL_FAULT_LOST_BMS` is
   in `CTRL_FAULT_LIMP_MASK` (`control/Inc/faults.h`). That mapping was correct when CAN
   carried BMS ceilings. **In V1 it is not:** CAN is Tx-only telemetry that cannot affect
   the loop, so a bus-off — a wiring fault, a missing terminator, a Cerbo powered down —
   now drops a healthy regulator into LIMP for the loss of an input V1 never consumed.
   The failure is *toward* less charging rather than more, so it is not a hazard, but it
   is a real availability defect on a live 48 V system and it must not ship. Fix is small
   (stop synthesising `LOST_BMS` from `ERRB_CAN` in V1; keep the counter in telemetry
   per #20) but it lands on the **fault ladder**, so it takes the `safety-reviewer` gate
   per CLAUDE.md. **FIXED + safety-reviewed 2026-07-28** (verdict: cleared — LIMP touches
   no timer, no clamp, no run-detect gate, so removing it restores availability and
   removes no protection). Restore the mapping as a V2 entry criterion when CAN-IN
   returns — **and note the restored mapping is necessary but NOT sufficient**: a BMS that
   goes silent on a *healthy* bus produces no bus-off and no `ERRB_CAN`, so V2 also needs
   a per-message freshness timeout on the decoded ceiling itself.
4. **Battery-temp sourcing has no V1 source at all** (V8: PA3 is FET temp; battery temp
   would arrive via CAN/BMS, which is V2). CONTROL_SPEC §4.2/§5.1 now treat battery-temp
   compensation as V2. ⚠ **Correction (safety review, 2026-07-28):** the earlier claim
   here that "LFP temp *window* enforcement from local sensors is unaffected" was
   **wrong** — there is no local battery sensor to enforce it with, so the low/high-temp
   charge window **cannot arm in V1**. That is a protection regression versus the stock
   firmware on this exact install (stock sources battery temp over CAN), and the fallback
   — the BMS's own cold-charge disconnect — is itself a load-dump hazard while the
   pre-disconnect soft-ramp is also V2-deferred. Tracked as a blocking item before M4
   real-bank charging; needs an annunciated "window unarmed" state plus explicit owner
   acceptance, not silence.
5. **Tail-current exit with an alternator-side shunt stays disarmed** in V1 (no external
   N2K monitor to re-arm it) — battery-side shunt is the V1 recommendation
   (CAN_INTEGRATION.md §4).
6. The 2026-07-27 safety-review preconditions (CVL consumer, pre-disconnect,
   `bms_required`) become **V2 entry criteria**, not M3/M5 blockers.

## 2. Milestones (safety and recovery come before any flash write)

Each milestone lists **exit criteria**. `→` marks a hard gate.

- **M0 — Infrastructure** *(mostly done)*.
  Done: public remote ✅, CI (`.github/workflows/build.yml`) ✅, HAL vendoring
  (`scripts/fetch_deps.sh`) ✅, `stm32f0xx_hal_conf.h` ✅.
  Done also: **license + NOTICE (#16)** ✅ (MIT + NOTICE, 2026-07-23); **OSS hygiene
  (#17)** ✅, **versioning scaffold (#18)** ✅, **issue/milestone sync** ✅ (all
  2026-07-24); **Renode emulation harness (#19 / GH#25)** ✅ (2026-07-26 — CI
  `emulation` job boots the real ELF and verifies `ctrl_tick` iterates; see
  `renode/README.md`).
  *Exit criteria all met (2026-07-26):* repo builds green in CI; MIT LICENSE + NOTICE in
  place; issues created; emulator runs the built ELF far enough to exercise `main()`.
  **M0 complete.**
- **M1 — Backup & recovery proven** → *no custom firmware is flashed before this passes.*
  Verified stock-image backup, documented+rehearsed DFU restore, SWD permanently wired,
  BOOT0/DFU entry-exit rehearsed. *Exit:* stock image demonstrably restores the unit via DFU
  on the bench; `FLASH_AND_RECOVERY.md` written.
- **M2 — Bring-up firmware** (`test-fw`, §4). 🔨 *firmware written 2026-07-27 (#14) and
  CI-built; everything below is the bench session it enables.* Confirm every I/O on the bench; **bench-confirm
  ADC scaling** (binding is already recovered); resolve the two label unknowns (PB13 =
  Enable vs Feature-In; which output = Lamp vs LED); identify the 0x0C/0x10/0x4C I²C devices;
  confirm package + field-driver topology. *Exit:* `board.h` constants bench-verified; I/O
  coverage all ✅.
- **M2.5 — Control-model reconciliation** → *gates M3.* ✅ **mostly done.**
  Legacy `regulator.{h,c}` deleted; replaced by the pure, HAL-free **`control/`** core
  (spec-native `ctrl_*` vocabulary, two-stage CHARGE/REST, per-cell V, watts arbitration).
  Built + CI-tested: `control` engine, `arbitration`, `field` (rotor clamp), `limits`,
  `faults` (OPEN/LIMP ladder), `thermal` governor — all unit-tested on the native CI runner;
  wired end-to-end in the app. #6a config strategy **decided 2026-07-26 (B — clean
  break; see the ADR)** — **M2.5 fully complete.**
- **M3 — Core firmware.** 🔨 *in progress.* **Done (pure/CI-tested):** two-stage engine +
  profile 1, arbitration, CV/field loop, rotor clamp, thermal governor, fault ladder,
  hardware limit set. **Driver trio added 2026-07-26**: `stator_rpm.c` (PA10 EXTI +
  TIM2 CNT-diff per V1/V2, feeds the §5.2 run-detect gate real RPM, fail-safe LOST
  until poles/pulley configured), `eeprom24c16.c` (raw 24C16 per V7, PA15 /WP, no
  record format until #6a), `dio.c` (DIP bank + PB13→ignition as flagged interim;
  Lamp-vs-LED still unresolved). Bench-pending: I2C2 TIMING value, PB13 pull/role.
  **ADC gap found + fixed 2026-07-27** (while building #14): `HAL_ADC_MspInit` did
  not exist anywhere in the tree — ADC1/DMA1 clocks were never enabled and
  `hdma_adc1` was never linked, so `HAL_ADC_Start_DMA()` would have failed on
  first contact with real hardware and **the whole sensor scan would never have
  run**. `sensors.c` had a stale comment saying board.c owed the callback; it had
  simply never been written, and with no hardware nothing could catch it. Now
  owned by `sensors.c` itself, matching the per-driver-owns-its-MSP-hook pattern.
  **CAN Tx added 2026-07-27 (#9/GH#18)**: pure N2K encoders + ISO address claim +
  cadence engine + bxCAN glue @250 kbit/s — the regulator now has a complete
  telemetry-out path in firmware, host-tested but never on a wire.
  **RV-C Tx (#10a)** landed the same day.
  **Remaining:** ~~INA2xx I²C transfers~~ — **the transfers are written**
  (re-checked 2026-07-28): `ina2xx.c` does bounded-retry 16-bit big-endian
  `HAL_I2C_Mem_Read/Write`, CVRF-gated sampling, software `I = V_shunt/R_shunt`
  with CALIBRATION left at POR, and the §7 R6 re-init rung with a re-entry
  guard. What is actually outstanding is **which bus** (GH#36): the driver
  brings up **I2C1** while V7 says the board runs both devices on **I2C2** —
  bench-gated per the §0.6 evidence precedence, and test-fw's `i2cscan` is the
  tool for it. Also remaining: inner-loop gain tuning (GH#34), the §1.1
  consequence-3 `LOST_BMS`↔`ERRB_CAN` fault-ladder fix, and bench bring-up.
  **Clock source DECIDED 2026-07-28 (GH#38): SYSCLK = HSE 8 MHz → PLL ×6 →
  48 MHz, matching stock; HSI48 stays as the USB clock only.** `board.c` is
  crystal-less today (HSI48 + CRS synced to USB SOF), but this regulator
  normally runs with **no USB host attached**, so CRS never syncs and HSI48
  free-runs at up to ±3 % — well outside CAN's ~±0.5 % bit-timing budget. The
  crystal is known populated (V2 + V6, and stock blocks on HSERDY on a working
  unit), so no bench step is owed. **Not a safety item:** the rotor clamp is a
  duty *ratio* and is independent of PWM carrier frequency, so clock drift
  cannot loosen it. HSE start-up failure must fall back to HSI48 and keep
  charging with CAN marked untrustworthy — never hang, never safe-state.
  Implementation is sequenced behind the Renode work because the emulated RCC
  may not raise HSERDY (cf. `renode/v6-stubs/`). **CAN Rx (#10) is NOT an M3 item — it is v2** (see the
  deliverable row); do not pull it forward. (§7 robustness
  layer #21 is done, CI-proven 2026-07-27). *Exit:* closed-loop CV hold on the
  bench supply into a dummy load, with fault cutoff verified; IWDG active; an induced
  HardFault provably lands in safe state + clean reboot.
  **Added 2026-07-28 (GH#34): a ≥10 min CV-hold ripple/duty record is a HARD exit
  criterion, not an optional measurement.** §5.3's derivation puts the CV channel's
  phase margin at only **≈13–15°** against a 45° norm. The decision (PM) is *not* to
  retune `KV` blind: SIL shows convergent bounded behaviour, and safety does not rest
  on that margin — the OV path and the rotor clamp bound the worst case at rated rotor
  current regardless. But an underdamped loop is exactly what a bench measurement
  settles cheaply and what guesswork settles badly, so M3 does not close until the
  record exists. Decision rule already written into §5.3: halve `KV` or shorten τ if
  CV-hold chatter exceeds ±0.005 V/cell. Two companion measurements ride along —
  rotor τ = L/R via a duty step on the dummy load (if τ > 50 ms, derate `KP` 2× first)
  and dI_field/de across RPM, which closes the high-RPM margin caveat.
- **M4 — Config + client app.** Config schema (per #6a), flash config store (CRC+version),
  `ws500ctl` read/write/verify, FW update via CLI, telemetry stream (#20). *Exit:* config
  round-trips; FW updates via `ws500ctl`; config survives an update.
- **M5 — CAN Tx / NMEA2000.** Status PGN set, RBM participation, `CAN_TECHNICAL_SPEC.md` +
  user doc. *(Tx only — CAN-IN / Rx control was #10's other half here and is now V2, §1.1.)*
  *Exit:* regulator telemeters valid PGNs and participates in RBM on a real bus.
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
  - **★ Confirmed on hardware (2026-07-24):** a live stock charge run (serial `AST` capture)
    showed the WS500 **pinning Field% at exactly 25 %** — holding there while delivering
    140 A / 7.6 kW at ~2330 RPM, and ramping to 25 % in ~2 %/s steps then stopping. The unit
    is **field-clamp-limited, not voltage-limited** (BatV reached only ~54.7 V vs the 56 V
    target). So the 25 % rotor clamp our design specifies is exactly what the product does —
    our firmware must reproduce it. Reference trace: `ws500_chargerun.log` (session capture).
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
- **Unbrickable floor:** the ST **system DFU bootloader in ROM** can't be erased.
  **DFU entry is already available and documented (confirmed 2026-07-24):** the manual's
  firmware-upgrade procedure — **press-and-hold the reset button** — enters the ST ROM DFU
  bootloader (the stock `.dfu` carries VID 0x0483 / PID 0xDF11, ST's system-bootloader IDs;
  the device re-enumerates as "STM32 BOOTLOADER"). So we do **not** need to locate a BOOT0
  access point to enter DFU — that IO_COVERAGE open item is resolved for the entry path
  (BOOT0 still relevant only as a fallback if the app ever won't hand off). **M1 hook:** the
  read-only full-flash backup + RDP check + clean DFU exit is the concrete M1 rehearsal;
  needs dfu-util or STM32CubeProgrammer (not yet installed). Discipline: **read/upload only,
  never write/erase, never touch RDP** (disabling RDP mass-erases the stock image).
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
> *Stock prior art (V4/binary):* the stock WS500 uses **IWDG non-windowed, max reload
> (RLR=4095), PR=7 (÷256) ≈26 s at boot and PR=4 (÷64) ≈6.5 s on fault re-arm paths**, with
> `DBG_IWDG_STOP` set for debugger freeze. It **does not read RCC→CSR reset-cause** (tracks
> resets via a `0xDEADBEEF` RAM marker instead) and has **no PVD/brown-out**. Our §7 below
> deliberately improves on all three (windowed + shorter reload, real reset-cause decode, PVD).

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

- **8.1 SIL — simulated charge cycles against a plant model.** ✅ **BUILT 2026-07-24
  (`sim/`, GH#26).** `control/` is pure and HAL-free by design, so it drives `ctrl_tick()`
  natively against a plant: LFP battery model (OCV/SOC curve, internal resistance, thermal
  mass), alternator model (output vs RPM × field duty, belt/pulley), harness delays. The
  alternator model is calibrated to the 2026-07-24 stock reference trace (≈25 % field @
  ~2330 RPM ⇒ ~140 A / 7.6 kW at ~54.7 V). **9 scenarios / 111 checks, all green** in CI
  (new `sil` job): CHARGE→REST cycles at 16S/48 V, temp window, BMS ceiling steps,
  engine-speed transients, sensor dropout + implausible-value injection (faults latch, field
  safe), **rotor-clamp verification with this exact install's parameters (4 Ω / 12 V rotor
  on 48–57.6 V bus → duty ceiling ≈25 %/≈21 %, verified never exceeded under 20 min of
  transient abuse)**, reference-trace shape, and a 5 M-tick long-soak (zero invariant
  violations). The main gauntlet — a regression suite, not a demo. **Extended 2026-07-26
  (GH#37 fixes): now 12 scenarios / 148 checks** — adds stalled-rotor (run-detect gate),
  lying-VBat (clamp plausibility guard), and Ah-revert timing.
  - **Payoff: 4 control-core bugs caught + fixed with tests** — two NaN-propagation faults
    (lost VBat → NaN field duty; shunt dropout → effort integrator latched permanently),
    tick-rate-dependent + marginally-unstable inner-loop gains (chatter matching live
    observation; now dt-scaled, KP −5×), and a knife-edge CV-exit qualifier that stalled
    T2 hold timers under sensor noise. **GH#37 items FIXED in-core 2026-07-26** (were
    documented-not-fixed): §5.2 run-detect gate (no rotation ⇒ field held to a pulse-cycled
    ≤5 % detect budget), clamp-supply plausibility guard (false-low readings can never
    loosen duty_max; worst-case = tightest clamp), and the T3 Ah-revert integrator (net
    Ah, battery-truth tiers only). Residual (documented in `sim/README.md`): a consistent
    from-boot false-low on the single physical voltage source is undetectable in-core;
    detect-budget/guard constants are `[SPEC-SIGNOFF]` placeholders pending spec review.
- **8.2 Renode — whole-firmware emulation (#19).** ✅ **HARNESS BUILT + CI-GREEN
  2026-07-26 (`renode/`, GH#25).** Stock Renode STM32F072 platform (thin overlay, no
  hand-written peripherals); the CI `emulation` job boots the real `ws500-openfw.elf`
  and asserts via function-name logging that `ctrl_tick` enters and re-enters (10 ms
  loop lives) and `Error_Handler` is never hit. All five predicted bring-up risks
  (HSI48RDY, bxCAN INAK, ADC/DMA, I²C stub, renode-test plumbing) cleared — only fix
  needed was `${CURDIR}`-anchoring the robot-test paths. **Still to build on it:**
  fault-path tests (§7: induced HardFault → safe state + crash record), watchdog
  starvation, DFU-adjacent boot behavior, INA226/EEPROM I²C device stubs, and the V6
  stock-binary trace (`ws500-stock-trace.resc`). Also dry-runs any risky bench
  procedure first (§5 rule 7).
- **8.3 Stock-binary verification (§0.6 V1–V4, V6–V8).** Every RE fact our drivers depend
  on gets re-derived or refuted before the driver is trusted (BKIN, PWM frequency, INA
  register map, config storage, β3380 channel identity).
- **8.4 Property/fault-injection tests on the pure core.** ✅ **BUILT 2026-07-26**
  (`control/test/prop.h`, `test_property.c`, `test_fuzz.c` — 48 M invariant points,
  ~2 s runtime, in the CI tests job via the existing glob). Sweeps P1–P7 (rotor clamp
  under all input combos incl. NaN/inf, fault-disposition over all 2^17 bit subsets,
  arbitration monotonicity, profile boundary configs) + 16 fixed-seed fuzz runs
  (4 M ticks, 5 seeds pin duty/duty_max = 1.0000 exactly at the boundary).
  **Payoff: 2 latent NaN-escape defects found and fixed** (ctrl_effort_to_duty NaN
  effort; ctrl_duty_max inf/inf) **+ 1 new clamp-guard hole closed**: a false-low
  drifting slower than the 4 %/tick step threshold walked the clamp to 1.8× rated
  rotor exposure → added `CTRL_VSUP_FLOOR_VCELL` (3.2 V/cell, v_limp's spec floor)
  hard-bounding ANY false-low to ≈1.13× rated on this install. Residual design
  points pinned as self-expiring EXPECTED-GAP markers. Spec gaps logged: 7 params
  lack §3 ranges; `rest_power_cap_w` %-form unimplemented; no config validator
  exists yet (§1 guard rails unenforced — M4/#6a work).

**Reference data for 8.1 (captured 2026-07-24):** a live stock charge run over serial gives
a real trace to validate the plant model + rotor clamp against — 25 % field clamp,
alternator output 140 A / 7.6 kW at ~2330 RPM / 25 % field, field ramp ≈2 %/s, FET-temp rise
31→45 °C under 140 A load, RPM sensing valid under real stator signal. Archive
`ws500_chargerun.log` (and a decoded CSV) as a SIL fixture. **Note (2026-07-28):**
`.gitignore` now blocks `*.log` and bench-capture CSVs by default because this repo is
public and a `$RAS:` dump carries the regulator password in plain text
(STAGE_A_RUNSHEET §Procedure 1). This particular capture is an `AST` *status* stream, not
a config dump, so it holds no password — but it is still install-identifying. Commit the
**decoded CSV** as the fixture and leave the raw log local; if the raw log is genuinely
wanted in-tree, it takes a deliberate `git add -f` after a read-through, never a casual
`git add -A`. Stage-A serial capability
(read-only `$` query + passive monitor via PowerShell `SerialPort`) is proven and repeatable.

*Gate:* Stage C in §5 (first custom-firmware flash) requires 8.1–8.4 green in CI, plus the
§0.6 queue resolved for every constant the flashed build uses.
