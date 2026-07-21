# WS500-OpenFW — Project Plan

Status legend: ✅ exists · 🔨 in progress · ⬜ not started

This is the master tracking doc until the repo is pushed to GitHub, at which point
each ⬜/🔨 line becomes a GitHub Issue and the milestones below become GitHub Milestones.

---

## 1. Deliverables map

| # | Deliverable | Artifact | Status |
|---|-------------|----------|--------|
| 1 | Project management | This doc → GitHub Issues + Milestones after push | 🔨 |
| 2 | Git hosting | Local repo exists; needs GitHub remote (private first, public at release) | 🔨 |
| 3 | HW documentation | `docs/WS500_HARDWARE_SPEC.md` (open items in its §7) | ✅/🔨 |
| 4 | Software Design Spec | `docs/SOFTWARE_DESIGN_SPEC.md` — architecture, regulator state machine, charge profiles, fault model | ⬜ |
| 5 | OS firmware | `Core/` skeleton; needs HAL vendored, sensors bound, regulator implemented | 🔨 |
| 6 | Config files | Stock-compatible `$` ASCII protocol (`config_protocol.c`) + flash-page config store | 🔨 |
| 7 | Update / rollback / backup / recovery | `docs/FLASH_AND_RECOVERY.md` + procedures below (§3) | ⬜ |
| 8 | Client app (read/write config + FW) | `tools/ws500ctl/` Python CLI over USB CDC; GUI later if wanted | ⬜ |
| 9 | CAN integration — technical spec | `docs/CAN_TECHNICAL_SPEC.md` — PGN set, RBM/DC-source behavior, addressing | ⬜ |
| 10 | CAN integration — user doc | `docs/CAN_USER_GUIDE.md` | ⬜ |
| 11 | User documentation | `docs/USER_GUIDE.md` (install, config, LED codes, troubleshooting) | ⬜ |
| 12 | Testing + bug tracking | `docs/TEST_PLAN.md` + GitHub Issues; Renode emulation + bench HIL | ⬜ |
| 13 | Bring-up / IO-confirm test firmware | `test-fw/` build target (see §4) — also solves the ADC channel-binding task | ⬜ |
| 14 | Bench safety — don't kill the test unit | §5 below; gates every hardware milestone | ⬜ |

## 2. Milestones (order is deliberate — safety and recovery come before any flash write)

- **M0 — Infrastructure.** GitHub remote, CI build (arm-none-eabi via GitHub Actions),
  vendor STM32CubeF0 HAL, repo builds clean. Convert this table to Issues.
- **M1 — Backup & recovery proven.** Verified stock-image backup, documented restore,
  SWD wired, DFU entry/exit rehearsed. **No custom firmware is flashed before M1 passes.**
- **M2 — Bring-up firmware.** `test-fw` confirms every IO (§4); ADC channel binding
  resolved; `SENSOR_CH_*` filled in `board.h`.
- **M3 — Core firmware.** Sensors scaled, field PWM into dummy load, regulator state
  machine + first charge profile, watchdog, fault/break path tested.
- **M4 — Config + client app.** `$` protocol round-trip, flash config store,
  `ws500ctl` read/write/verify, firmware update via CLI.
- **M5 — CAN/NMEA2000.** PGN transmit set, RBM participation, tech spec + user doc.
- **M6 — Real-alternator trials + docs.** Staged live testing (§5 exit criteria),
  user guide, tagged release.

## 3. Firmware update / rollback / backup / recovery (design constraints)

**Chip facts that shape this:** STM32F072xB = 128 KB single-bank flash. No dual-bank,
so a true A/B slot scheme costs half the flash — not worth it. Instead:

- **Unbrickable floor:** the ST **system bootloader in ROM** (DFU over USB) cannot be
  erased. Worst case is always recoverable if we can force system-memory boot
  (BOOT0 pad or empty-flash boot). Locate/verify the BOOT0 access point on the board — HW spec §7 open item.
- **Backup first:** full flash readout of the stock unit via SWD **before anything else**.
  ⚠️ If RDP (readout protection) level ≥1 is set, readout is blocked and disabling RDP
  mass-erases the chip — in that case the stock DFU image file we already have *is* the
  backup; verify it restores on the bench before relying on it.
- **Rollback = restore stock image via DFU.** Documented, rehearsed procedure in
  `docs/FLASH_AND_RECOVERY.md`.
- **Update path:** stock ROM DFU for now (`dfu-util`), driven by `ws500ctl` for a
  one-command experience. A tiny custom bootloader (CRC-checked app, config-preserving
  update over CDC) is a *later* nice-to-have, not a dependency.
- **Config survives updates:** config lives in the last flash page(s), outside the app
  image region, with CRC + version; `ws500ctl` can export/import as a text file.

## 4. Bring-up test firmware (`test-fw`)

Separate small build target sharing `board.c`. Interactive over USB CDC:

- LED / GPIO walk (confirm pin map visually)
- Live ADC dump of all 7 channels (drives the signal-injection channel binding)
- Field PWM at commanded duty **with hard 20 % cap** compiled in, dummy load only
- TIM1 break-input test (assert fault line → verify PWM hard-stops)
- CAN loopback + external echo test
- I²C bus scan (finds the INA2xx / temp sensors)

Everything it proves feeds directly into `board.h` constants and the HW spec.

## 5. Bench safety — protecting the test WS500

Rules, in force until explicitly retired:

1. **Current-limited bench supply** (13.2 V, start ≤1 A limit) — never a raw battery
   for early bring-up. A battery can source hundreds of amps into a fault.
2. **Dummy field load** — power resistor (~10 Ω, ≥50 W) in place of a real alternator
   field until the control loop and fault paths are proven. A real field coil is
   inductive and unforgiving.
3. **Fail-safe defaults** — firmware ships field-OFF; watchdog enabled; TIM1 break
   (hardware fault cutoff) verified in M2 before any closed-loop work.
4. **Duty-cycle cap compiled into test builds** (20 %) so no software bug can command
   full field on the bench.
5. **Never spin a real alternator without a battery connected** (load dump kills
   diodes/regulator). Real-alternator work starts only in M6, with staged exit
   criteria: dummy load → field coil on dead alternator → driven alternator on
   battery bank with supervision.
6. **Recovery always one step away** — SWD permanently wired to the bench unit; stock
   image restore rehearsed (M1) before first custom flash.
7. If a second WS500 unit is affordable, keep one **golden unit** on stock firmware,
   untouched, as reference and fallback.
