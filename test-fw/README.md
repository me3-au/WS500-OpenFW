# WS500-OpenFW — Test Firmware (`test-fw`)

> **Extracted from PROJECT_PLAN.md §4 on 2026-07-26; implementation added 2026-07-27
> (deliverable #14).** Authoritative details, updates, and the ongoing tracker remain
> in PROJECT_PLAN.md.

---

## SAFETY — read this before flashing anything

**There is exactly one WS500 in this project, and it is installed and LIVE on a 48V
system with a 4-ohm (12V-class) rotor** (PROJECT_PLAN §5 "installed-unit reality").
This firmware is built to run on the **bench**, not the installed unit:

1. **Current-limited bench supply** (13.2V, start ≤1A) — never a raw battery, and
   never the installed unit.
2. **Dummy field load** — a power resistor (~10 Ω, ≥50 W), never a rotor coil
   directly, until the loop and fault paths are proven on the bench.
3. **Field duty is capped at 20%, compiled in, and structurally unexceedable** —
   see `field_guard.c` below. `field_drive.c` itself (shared with the production
   firmware) still allows the full 0–100% range because the real regulator needs
   it; the 20% ceiling is a test-fw-only policy layered on top, in exactly one
   function, with no command, config value, or build flag able to raise it.
4. **A commanded field duty self-expires after ~5 s** unless refreshed, and drops
   immediately if the USB console disconnects (DTR drop) — an operator who walks
   away, or a cable that falls out, cannot leave the field energised.
5. Every abnormal path (HardFault, NMI, an unclaimed interrupt, an assert, a
   hung main loop caught by the watchdog) funnels through the same
   `enter_safe_state()` → crash record → reset path the production firmware
   uses (PROJECT_PLAN §7 R0/R3). There is no bare `while(1)` anywhere in this
   firmware.
6. `cutoff` (the software fault-cutoff test) **latches** — once triggered, the
   field stays off until the board is reset. This is intentional.

If this firmware is ever run against the live installed unit rather than a
dedicated bench unit, the full PROJECT_PLAN §5 access ladder (readings → config →
bench flash) still applies; nothing here grants an exception to it.

---

## Purpose

A separate, small, interactive-over-USB-CDC build target (`ws500-testfw.elf`,
alongside the main `ws500-openfw.elf`) that confirms every I/O on the bench.
Shares `Core/Src/board.c` and the production drivers (`field_drive.c`,
`safe_state.c`, `sensors.c`, `ina2xx.c`, `eeprom24c16.c`, `stator_rpm.c`, `dio.c`,
`usb_cdc.c`) and the §7-style robustness trio (`safe_state.c` / `crash_record.c` /
`fault_handlers.c` / `watchdog.c`) rather than duplicating any of it. test-fw's
own code is `main.c`, `console.c` (the command console), `field_guard.c` (the
field-command safety funnel), and the two bench self-tests `can_test.c` /
`i2c_scan.c`. Results feed `board.h` constants and the hardware spec
(`WS500_HARDWARE_SPEC.md`, `IO_COVERAGE.md`).

Deliberately **not** shared: `can_n2k.c` (the full NMEA2000/RV-C protocol stack,
which needs a control/telemetry snapshot this bring-up tool has no reason to
carry) and the config/telemetry pipeline (`config_protocol.c`, `cfg_stream.c`,
`telem_stream.c`) — the console *is* test-fw's operator interface.

---

## Building and connecting

```sh
sh scripts/fetch_deps.sh
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build -j        # builds BOTH ws500-openfw.elf and ws500-testfw.elf
```

Flash `build/ws500-testfw.bin` per the bench procedure in `FLASH_AND_RECOVERY.md`
(same DFU path as the main firmware). Connect over USB; it enumerates as a
CDC-ACM port ("WS500-OpenFW" / "WS500 Regulator", same descriptors as the main
firmware's config port — see `usb_cdc.c`). Open it in any serial terminal
(115200 8N1 settings are accepted but not used — there is no UART behind this
port) and type `help`.

---

## Commands

| Command | Effect |
|---|---|
| `help` | command list |
| `status` | uptime, reset cause + counts, crash-record summary, watchdog kick/starve counts, INA226/EEPROM driver health, field-guard state |
| `gpio` | DIP bank (PA4-7/PB0-2) + PB13 (`CTRL_IN`) + current output states |
| `gpio outa <on\|off>` | drive PA9 (`OUT_LAMP_OR_LED_PIN`) directly |
| `gpio outb <on\|off>` | drive PB14 (`STATUS_OUT_B_PINS`) directly |
| `adc` | all 7 ADC channels, raw count **and** scaled value side by side, plus INA226 current/bus-V |
| `stator` | PA10 stator capture: state (FRESH/STALE/LOST), frequency, RPM |
| `field <pct>` | command field duty 0–100 (**capped to 20%,** see Safety §3) |
| `field off` | field off immediately |
| `field` | show current field-guard state (commanded?, applied duty, time remaining) |
| `cutoff` | trigger the software fault-cutoff path; prints TIM1 `BDTR.MOE` before/after; **latches** |
| `can loop` | bxCAN internal silent-loopback self-test (no wiring needed) |
| `can echo` | bxCAN external echo test (needs a terminated, live bus with a second node/analyzer) |
| `i2cscan` | scan I2C1 and I2C2, 0x08–0x77, report every ACKing address on each bus |

### Resolving the two open label questions

- **PB13 = Enable/Ignition vs Feature-In**: run `gpio` repeatedly while toggling
  the harness signal on PB13 (manual wire 1/3) and correlate with expected
  behaviour; the console prints the raw level live.
- **Which output is Lamp vs LED**: run `gpio outa on` / `gpio outb on` (and
  `off`) one at a time while watching the physical indicators on the unit.

### ADC bench-confirmation

`adc` prints raw counts **and** scaled engineering values for all 7 channels
(PA1/PA2 NTC β3950, PA3 NTC β3380/FET-driver, PC5 VBAT via the 34.3333:1
divider, plus the three internal calibration channels), so a wrong scaling
constant shows up as a wrong *raw* count (bad divider/pin) vs. a wrong *scaled*
value (bad formula) — cross-check against a reference meter. **Known gap**:
`sensors.c`'s NTC formula uses a placeholder `R0/Rfixed` ratio (its own
in-code `TODO`) — trust the raw counts and the VBAT scaling; treat absolute
NTC temperatures as provisional until that ratio is bench-derived.

### CAN test semantics

`can loop` uses bxCAN's `SILENT_LOOPBACK` mode — internal to the peripheral,
puts nothing on the physical bus, always available. `can echo` transmits in
`NORMAL` mode: on a real, terminated CAN bus, a transmitting node also
receives its own frame once at least one other live node acknowledges it (an
ordinary property of CAN, not a special "echo" feature) — so this test needs a
terminated bus with a second node or a bus analyzer present, not a bespoke
echo responder. Without one, the transmit never completes (bxCAN's automatic
retransmission keeps retrying) and the test correctly times out and reports
`FAIL/TIMEOUT` after ~2 s — a safe, bounded, informative failure, not a hang.

### I2C scan and the BUS CAVEAT

`i2cscan` is the bench falsification step for `ina2xx.c`'s documented **BUS
CAVEAT**: PROJECT_PLAN §0.6 V3 originally placed the INA226 on I2C1, but the
later, more thorough V7 pass found the stock firmware runs **both** the INA226
(0x40) and the 24C16 config EEPROM (0x50) on **I2C2**, with I2C1 idle. Run
`i2cscan` and see which bus actually answers. It scans both I2C1 and I2C2
independently of the drivers that already own those buses at boot
(`ina2xx.c`, `eeprom24c16.c`); this transiently disturbs their live traffic
(their next transaction fails once), and they self-heal via the existing §7 R6
error-budget re-init within a few seconds — the console prints a note to this
effect every time a scan completes. No data is at risk: a scan issues only
zero-length "is this address present" probes, and the EEPROM's `/WP` line
never goes low outside `eeprom24c16_write()`, which a scan never calls.

**Corrected vs the original §4 text**: the "0x0C/0x10/0x4C devices" item is
**refuted** (PROJECT_PLAN §0.6 V3 — those were length/size arguments misread as
addresses in an earlier disassembly pass); `i2cscan` reports whatever actually
ACKs, nothing more.

### Field cutoff test

`cutoff` triggers `field_drive_fault_cutoff()` — the software MOE-clear path,
**not** a hardware break-input test: the stock board does not route BKIN
(PROJECT_PLAN §0.6 V1/V2 — `BDTR.BKE=0`, no break AF pin). The console prints
TIM1 `BDTR.MOE` before and after so the hard-stop is directly verifiable. This
latches `field_drive_faulted()` — `field <pct>` still parses afterward, but
`field_drive_set()` forces 0% regardless until the board is reset.

---

## What is and isn't included

Shares the §7-style robustness trio (`safe_state.c` / `crash_record.c` /
`fault_handlers.c` / `watchdog.c`) with the main firmware, so a hang anywhere —
including in this firmware's own new code — resets the part rather than
leaving whatever duty was last commanded running unsupervised, and every fault
still writes a crash record readable via `status`. **Not** included, as a
deliberate scope decision for a small bring-up target: `integrity.c` (flash
image CRC / stack watermark) and `pvd.c` (brown-out detection). Their absence
does not weaken the field-off guarantees above — those come from
`field_guard.c`, `field_drive.c`, and the shared `safe_state.c` funnel, none of
which depend on either module.

---

## Bug found and fixed while building this firmware

While wiring up the `adc` command, `sensors_init()`'s own comment
("NOTE: `HAL_ADC_MspInit` (in board.c) must enable ADC1 + DMA1 clocks") turned
out to describe a function that **did not exist anywhere in the tree**: ADC1's
and DMA1's clocks were never enabled, and the DMA channel was declared but
never configured or linked to the ADC handle. `HAL_ADC_Start_DMA()` would have
failed immediately (`NULL` `DMA_Handle`) on real hardware — the ADC scan this
whole module exists to run would never have produced data, on **either**
firmware target. Fixed in `Core/Src/sensors.c` (adds `HAL_ADC_MspInit`,
following the same per-driver-owns-its-MSP-hook pattern `usb_cdc.c` and
`can_n2k.c` already use). Flagged for review since it changes shared code both
firmware targets link — see the PR/commit for the full comment.

---

## Integration with Development

Everything proven by this firmware **feeds `board.h` constants and the HW spec.**
