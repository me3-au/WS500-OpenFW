# WS500-OpenFW — Test Plan

> **Assembled from PROJECT_PLAN.md §8 (virtual-first test strategy), §1 (deliverables), and §2 (milestone exit criteria) on 2026-07-26.**
> Authoritative details, updates, and the ongoing tracker remain in PROJECT_PLAN.md.

---

## Test Strategy: Virtual-First, Four Layers

**Policy (PROJECT_PLAN §8):** everything that can be proven without the unit, is — the only WS500 is live on a 48 V system (see SAFETY.md). Four virtual layers, cheapest first; all run in CI:

---

## Layer 8.1: SIL — Simulated Charge Cycles

**Status:** ✅ **BUILT 2026-07-24** (`sim/`, GH#26).

**What it does:** `control/` is pure and HAL-free by design, so it drives `ctrl_tick()` natively against a plant:
- LFP battery model (OCV/SOC curve, internal resistance, thermal mass)
- Alternator model (output vs RPM × field duty, belt/pulley)
- Harness delays

The alternator model is calibrated to the 2026-07-24 stock reference trace (≈25 % field @ ~2330 RPM ⇒ ~140 A / 7.6 kW at ~54.7 V).

**Test matrix:** 9 scenarios / 111 checks, all green in CI (new `sil` job):
- CHARGE→REST cycles at 16S/48 V
- Temp window
- BMS ceiling steps
- Engine-speed transients
- Sensor dropout + implausible-value injection (faults latch, field safe)
- **Rotor-clamp verification with this exact install's parameters** (4 Ω / 12 V rotor on 48–57.6 V bus → duty ceiling ≈25 %/≈21 %, verified never exceeded under 20 min of transient abuse)
- Reference-trace shape
- 5 M-tick long-soak (zero invariant violations)

**Bugs caught and fixed:** 4 control-core bugs:
- Two NaN-propagation faults (lost VBat → NaN field duty; shunt dropout → effort integrator latched permanently)
- Tick-rate-dependent + marginally-unstable inner-loop gains (chatter matching live observation; now dt-scaled, KP −5×)
- Knife-edge CV-exit qualifier that stalled T2 hold timers under sensor noise

**Documented but not yet fixed (app-side / spec, GH#37):**
- Run-detect/stationary-rotor budget (CONTROL_SPEC §5.2) absent from the pure core
- Dynamic clamp trusts measured supply V (in-range sensor lies loosen it — needs plausibility)
- T3 Ah-revert integrator unwired

---

## Layer 8.2: Renode — Whole-Firmware Emulation

**Status:** ✅ **HARNESS BUILT + CI-GREEN 2026-07-26** (`renode/`, GH#25).

**What it does:** Stock Renode STM32F072 platform (thin overlay, no hand-written peripherals); the CI `emulation` job boots the real `ws500-openfw.elf` and asserts via function-name logging that `ctrl_tick` enters and re-enters (10 ms loop lives) and `Error_Handler` is never hit.

**Bring-up risks cleared:** all five predicted (HSI48RDY, bxCAN INAK, ADC/DMA, I²C stub, renode-test plumbing) — only fix needed was `${CURDIR}`-anchoring the robot-test paths.

**Still to build on it:**
- Fault-path tests (§7: induced HardFault → safe state + crash record)
- Watchdog starvation
- DFU-adjacent boot behavior
- INA226/EEPROM I²C device stubs
- V6 stock-binary trace (`ws500-stock-trace.resc`)
- Dry-runs of risky bench procedures first (SAFETY.md rule 7)

---

## Layer 8.3: Stock-Binary Verification

**Status:** ✅ **RESOLVED in PROJECT_PLAN §0.6 V1–V4, V6–V8.**

**What it does:** every RE fact our drivers depend on gets re-derived or refuted before the driver is trusted:
- BKIN usage
- PWM frequency (resolved to 143.2 Hz)
- INA register map (INA226 only, @0x40 on I²C2)
- Config storage (24C16 EEPROM @0x50, /WP = PA15, I²C2)
- β3380 channel identity (FET/driver temp, not battery)

See PROJECT_PLAN §0.6 for the full virtual-verification queue and resolution status.

---

## Layer 8.4: Property / Fault-Injection Tests

**Status:** ⬜ **Planned.**

**What it will do:** extend `control/test/` beyond example-based:
- Sweep-based invariant checks (field ≤ clamp under *all* input combinations; faults latch; arbitration monotonicity)
- Boundary sweeps on profile parameters
- Randomized sensor-noise runs with fixed seeds

---

## Reference Data

**Captured 2026-07-24:** a live stock charge run over serial gives a real trace to validate the plant model + rotor clamp against — 25 % field clamp, alternator output 140 A / 7.6 kW at ~2330 RPM / 25 % field, field ramp ≈2 %/s, FET-temp rise 31→45 °C under 140 A load, RPM sensing valid under real stator signal. Archive `ws500_chargerun.log` (and a decoded CSV) as a SIL fixture. Stage-A serial capability (read-only `$` query + passive monitor via PowerShell `SerialPort`) is proven and repeatable.

---

## Stage C Gate: Custom-Firmware Readiness

**Gate (PROJECT_PLAN §5):** Stage C in the hardware-access ladder (first custom-firmware flash) requires:
1. **Layers 8.1–8.4 all green in CI**
2. **PROJECT_PLAN §0.6 virtual-verification queue resolved** for every constant the flashed build uses

Until both conditions hold, custom firmware remains on the developer's workstation and the stock unit stays on the live 48 V system.

---

## Milestone Exit Criteria (From PROJECT_PLAN §2)

### M1 — Backup & Recovery Proven

> Exit: stock image demonstrably restores the unit via DFU on the bench; `FLASH_AND_RECOVERY.md` written.

### M2 — Bring-Up Firmware

> Exit: `board.h` constants bench-verified; I/O coverage all ✅.

### M2.5 — Control-Model Reconciliation

> Exit criteria: legacy `regulator.{h,c}` deleted; replaced by pure, HAL-free `control/` core.

### M3 — Core Firmware

> Exit: closed-loop CV hold on the bench supply into a dummy load, with fault cutoff verified; IWDG active; an induced HardFault provably lands in safe state + crash record + clean reboot.

### M4 — Config + Client App

> Exit: config round-trips; FW updates via `ws500ctl`; config survives an update.

### M5 — CAN Tx / NMEA2000

> Exit: regulator telemeters valid PGNs and participates in RBM on a real bus.

### M6 — Real-Alternator Trials + Release

> Exit: driven alternator charges a bank under supervision; `v0.1.0` tagged.

---

## Deliverable #13 Summary (From PROJECT_PLAN §1)

| Milestone | Artifact | Status |
|-----------|----------|--------|
| M0→M6 | **Testing + bug tracking** | 🔨 |
| | control-core unit tests (CI) ✅ + **SIL gauntlet `sim/` (CI, §8.1)** ✅ | ✅ |
| | `TEST_PLAN.md` | ✅ (this doc, 2026-07-26) |
| | Renode | ✅ harness (further tests planned, §8.2) |
| | bench HIL | ⬜ |
| | GitHub Issues | ✅ |

See PROJECT_PLAN §2 for full milestone definitions and gating logic.
