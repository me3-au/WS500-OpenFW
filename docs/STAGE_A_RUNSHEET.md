# WS500-OpenFW — Stage A Bench Runsheet

> **Status:** draft, owner review pending. Registered in the `PROJECT_PLAN.md` §0 doc
> map (row added 2026-07-28).
> **Scope: Stage A only** — see `SAFETY.md` and `PROJECT_PLAN.md` §5 "Hardware Access
> Ladder." Every procedure below is **observation only**: the stock firmware is never
> written to, no `$` config-write command is ever sent, no DFU write/erase/RDP-change
> operation is ever issued, and the field/rotor is never commanded. This is the
> concrete, step-by-step execution of the six Stage-A items enumerated in `SAFETY.md` /
> `PROJECT_PLAN.md` §5, plus the read-only M1-adjacent flash-backup rehearsal.

---

## 0. Before you start

**This is the live, installed unit — read this whole section before touching anything.**

- **Authoritative safety doc:** `SAFETY.md` (extracted from `PROJECT_PLAN.md` §5). This
  runsheet does not repeat its rules — it applies them to seven concrete procedures.
  Re-read `SAFETY.md`'s "Installed-Unit Reality" section before every session: the WS500
  is installed and in service on a 48 V / 16S bank with a 4 Ω (12 V-class) rotor, and it
  is the **only unit that exists** for this project.
- **Nothing here writes config, flash, or the field.** If a step would do any of those,
  it is out of scope and is called out explicitly in §3 "NOT IN THIS RUNSHEET."
- **M1 (proven DFU backup/restore) is not yet done.** Procedure 7 below is the concrete
  M1 rehearsal, and it is the one procedure in this runsheet that puts the unit into the
  ST ROM DFU bootloader — a qualitatively different (though still read-only, still
  unbrickable-by-design) state than Procedures 1–6. Read Procedure 7's own preamble
  before attempting it.
- **Tooling gap:** `dfu-util` / STM32CubeProgrammer are **not yet installed on this
  machine** (task instruction, confirmed against `FLASH_AND_RECOVERY.md`'s M1 hook,
  which flags the same gap). Procedure 7 cannot start until one of these is installed —
  see its own "Prerequisite" block.

### STOP immediately if…

- The status LED changes to a fault blink pattern that wasn't there before you started
  probing (`USER_MANUAL.md` / the stock manual's LED codes) — the goal is
  observation-*only*; if probing itself appears to be perturbing the running unit, stop
  and reassess before continuing.
- Charging output (AST `AltState`, `BatVolts`, `AltAmps`) shows an unexpected drop,
  spike, or mode change coincident with a probing action.
- Any smoke, unusual smell, or unexpected heat from the unit or harness.
- After a reset/DFU excursion (Procedures 6 or 7), the unit does **not** cleanly resume
  normal operation (LED back to its running pattern, `$` port re-enumerates, no fault
  latched). Do not attempt to "fix" this by writing/erasing/flashing anything — the ROM
  DFU bootloader is unbrickable by design and a plain power cycle should always recover
  the app (BOOT0 is not strapped for DFU entry in this install; DFU entry is
  press-and-hold-reset only, per `FLASH_AND_RECOVERY.md`). If a power cycle doesn't
  recover it, stop the session and treat it as an M1 blocker, not something to
  improvise around.
- You catch yourself about to type a `$` command that isn't one of the read-only queries
  named in this runsheet (`$RAS:`, `$RSS:`, `$RCP:n`) — anything starting `$CP`, `$SC`,
  `$CC`, `$CD`, `$MSR`, `$FRM`, `$RBT`, `$EDB` is a Stage B/C action. Stop and re-check
  before sending.
- Procedure 7's RDP check comes back ≥1 — stop, do not attempt to clear it (that mass-erases
  the stock image); the held `.dfu` image becomes the primary backup instead (see
  `FLASH_AND_RECOVERY.md`).

---

## 1. Equipment (master list)

Per-procedure deltas are called out in each section; this is what you need on hand
before starting a session.

- Laptop/PC with a free USB-A/USB-C port, PowerShell available.
- USB cable rated for data (not charge-only) — connects to the WS500's USB port for the
  `$` protocol.
- Digital multimeter (DMM), DC-capable, with test leads/probe hooks.
- Oscilloscope (2+ channels; 4 preferred for Procedure 6) or a logic analyzer with
  comparable bandwidth, plus probes/hooks and a ground reference lead.
- A CAN bus interface capable of **listen-only / silent monitoring** at up to 250 kbit/s
  (the standard NMEA 2000/RV-C rate; confirm the actual rate via the `$` protocol per
  Procedure 2 rather than assuming it) — e.g. a USB-CAN adapter and logging tool of your
  choice. **Must not transmit** onto the bus (no ACKs, no injected frames) — use its
  documented silent/listen-only mode.
- CAT-5/5e/6 cable + an RJ45 breakout/tap (or a spare terminator you're comfortable
  temporarily removing and reinstating) to reach CAN_H/CAN_L on the WS500's RJ45 CAN
  port (pin 1 = CAN_H, pin 2 = CAN_L, pin 3 = CAN_GND — do **not** connect CAN_GND, the
  Comms & Config Guide explicitly warns this risks a ground loop against ALT-).
- A known-good reference tachometer or RPM source for Procedure 4 (e.g. a contactless/
  strobe tach on the crank or belt, or an already-trusted engine RPM readout) —
  independent of the WS500's own stator-derived RPM, since that's what's being validated.
- (Procedure 5, optional) A DC clamp ammeter, if cross-checking `AltAmps`/`BatAmps`
  against a physical current measurement in addition to the voltage check.
- (Procedure 6) Fine-pitch scope/logic-analyzer probes or micro-grabbers capable of
  reaching LQFP64 (0.5 mm pitch) pins/test points — see that procedure's note on
  enclosure access.
- (Procedure 7) `dfu-util` **or** STM32CubeProgrammer installed — **not yet true on this
  machine**; install one before attempting Procedure 7.

---

## 2. Procedures

### Procedure 1 — USB `$` protocol readout (full config + status dump)

**Purpose:** archive the complete stock parameter set and current status over the
existing, proven USB serial channel — the primary Stage-A artifact. Per
`DECISION_6A_CONFIG_STRATEGY.md`, this archive is also the direct input to the
client-app's one-time stock-dump-to-native-profile import tool (deliverable #6a).

**What this settles:** not a numbered `PROJECT_PLAN` §0.6 V-item — it produces the
**stock-dump artifact** referenced by `SAFETY.md` Stage A and `DECISION_6A_CONFIG_STRATEGY.md`
§"Options," and gives ground truth for the stock parameter set feeding the #6a import
tool.

**Equipment:** USB cable, laptop/PowerShell (or any serial terminal — PuTTY with
session logging is documented in the Wakespeed Comms & Config Guide as an alternative,
but this runsheet follows the **already-proven, repeatable PowerShell `SerialPort`
method** cited in `PROJECT_PLAN.md` §8, so no new tooling is introduced).

**Convenience automation (optional):** `tools/stage_a_capture` runs steps 2-7 below for you
— it opens the port, drains whatever's already flowing, sends `$RAS:@` then `$RCP:1@`..
`$RCP:8@`..`$RCP:0@` in order (and *only* those — read-only by construction, see that
tool's README), waits for each `AOK;`/`NAK;` terminator before sending the next command,
then lets the passive `AST;` stream run for a configurable window, writing both a raw and
a password-redacted log file. It is a convenience, **not a replacement** for the manual
steps below — this runsheet's PowerShell procedure remains the documented fallback if the
tool misbehaves or Python/`pyserial` aren't available at the bench. See
`tools/stage_a_capture/README.md` for install/usage; a short summary:

```sh
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stage_a_capture
pip install -e ".[test]"
python -m stage_a_capture.capture --port <COM_PORT> --outdir C:\Users\adren\stage-a-logs
```

**Command reference** (from `docs/Wakespeed-Communications-and-Configuration-Guide-v2.6.1-1.pdf`,
already an in-repo gitignored reference per `PROJECT_PLAN.md` §0 — consult it directly
for anything not covered here): commands begin with `$`, end with CR/LF or `@`, and the
device replies over the same port. These three are **read-only queries**:

| Command | Effect |
|---|---|
| `$RAS:` | dumps every status string the regulator knows (`DST`, `DCV`, `ENG`, `CST`, `CPE` for the **currently selected** profile, `NPC`, `SCV`, `SST`), terminated by `AOK;` |
| `$RCP:n` (n = 1..8) | dumps the **stored** Charge Profile Entry n — run this 8× to capture the full CPE table (`$RAS:` only returns the currently-selected one) |
| `$RCP:0` | dumps whichever CPE is currently selected (redundant with the matching `$RCP:n` above, but cheap to include) |

**Steps:**

1. Connect USB; in Windows Device Manager, identify the WS500's serial (CDC) COM port.
   Record its friendly name / VID:PID as shown (this is the app-mode descriptor — see
   §5 "Candidate additions," it isn't documented anywhere in-repo yet).
2. Open PowerShell and drive the port with `System.IO.Ports.SerialPort` (the method
   already proven per `PROJECT_PLAN.md` §8's "Stage-A serial capability… proven and
   repeatable"). Baud/parity/stop-bit settings are accepted but not meaningful over USB
   CDC (per `test-fw/README.md`'s note on the same CDC-ACM implementation) — any
   consistent value works.
3. Open a session log file (timestamped filename) that captures **everything** received
   on the port from connection onward — the unit also pushes unsolicited `AST;`
   (and `DST;` if a DC-DC converter is configured) status lines continuously, so a
   passive capture alone is a useful cross-check even before you send anything.
4. Send `$RAS:` (or `$RAS:@`). Wait for `AOK;` before sending the next command — the
   device suspends other status output while it's processing a command, so waiting for
   the terminator avoids interleaved garbage.
5. Send `$RCP:1@` through `$RCP:8@` in sequence, one at a time, waiting for each `CPE;`
   reply before sending the next.
6. Let the passive `AST;` stream run for a few minutes after the queries to capture live
   status alongside the static config (useful cross-reference for Procedures 4/5).
7. Close and archive the log.

**Record:** the full session log file (raw, unparsed) as the archival artifact — this
*is* the Stage-A artifact `DECISION_6A_CONFIG_STRATEGY.md` refers to. Also separately
note: COM port VID:PID/descriptor string, and the `SST;` line's `Version` field (stock
firmware version — expect `AREG2.6.1`-class per the Comms & Config Guide revision this
runsheet cites; record whatever actually comes back).

> **Privacy/security note — read before archiving or committing anything:** `$RAS:`'s
> `NPC;` line includes the regulator's configured **password** in plain text *unless*
> it was set with a leading `.` (hidden). This repo is **public**
> (`PROJECT_PLAN.md` header). Treat the raw log as sensitive: keep it out of the public
> repo unless the `NPC;` line (and any other operationally sensitive field) is redacted
> first. `tools/stage_a_capture` writes a redacted copy automatically alongside the raw
> one for exactly this reason. The client-app import tool (deliverable #6a,
> `tools/stock_config_import`) should consume the raw file from wherever it's archived
> locally, not from a committed copy — it never needs the password field either way.

**Pass/fail:** PASS = both `$RAS:` and all 8 `$RCP:n` return well-formed replies ending
in `AOK;` and the log is archived. **Unexpected result → stop:** a `NAK;` on a
documented-valid command, or the port stops responding — do not retry aggressively;
disconnect, reconnect, and try once more, then stop and note it rather than
troubleshooting live against the installed unit.

---

### Procedure 2 — CAN bus sniff (listen-only)

**Purpose:** log the PGN/DGN set the stock unit actually transmits and receives, to
validate `CAN_INTEGRATION.md`'s planned Tx set against real stock behavior, and to see
whether any Cerbo/GX or other device already on this system's CAN bus categorizes the
WS500 in a way that informs `CAN_INTEGRATION.md` §8's ingestion caveat.

**What this settles:** not a numbered §0.6 V-item — it validates `CAN_INTEGRATION.md`
(cited explicitly in `SAFETY.md`'s Stage A list) against the stock unit's real traffic,
which is a different, useful data point even though the *stock* PGN set is not what our
OpenFW firmware will transmit (§0.5: "we deliberately ditch the legacy surface"; our own
Tx set is `CAN_INTEGRATION.md` §2, already built per `PROJECT_PLAN.md` deliverable #9).
Treat any correlation between the two as informative, not as a spec requirement.

**Equipment:** CAN interface in listen-only/silent mode, CAT-5 tap on the WS500's RJ45
CAN port (CAN_H/CAN_L only, per §1 above).

**Steps:**

1. **Before connecting anything:** confirm your CAN tool's listen-only/silent mode is
   actually engaged (many tools default to active/ACK-generating mode) — an accidental
   ACK or injected frame on this bus is not a Stage-A action.
2. Tap CAN_H (RJ45 pin 1) / CAN_L (RJ45 pin 2) via the breakout, without breaking the
   existing daisy-chain termination (`CAN_INTEGRATION.md` §7 / the Comms & Config Guide:
   terminate both ends, no loops).
3. Log raw frames for a session that spans at least one full status broadcast cycle from
   every PGN/DGN of interest (the Comms & Config Guide Appendix B rates most J1939/N2K
   status messages well under 5 s; give it a few minutes to be safe).
4. Separately, via Procedure 1's serial session, send `$RSS:CST@` and record the
   `BitRate` field from the `CST;` reply — this gives you the configured bit rate
   directly from the device instead of having to infer it from the capture, and is a
   handy cross-check that your CAN tool is sampling correctly.
5. From the raw capture, extract: the set of PGNs/DGNs actually seen (compare against
   the Comms & Config Guide Appendix B's documented list and against
   `CAN_INTEGRATION.md` §2's planned OpenFW Tx set), the source address(es) claimed, and
   whether a GX/Cerbo or other node is present and responding (address-claim exchange,
   product-info requests, etc.).

**Record:** raw capture file, extracted PGN/DGN list with observed cadence, bit rate
(from both the capture and the `$RSS:CST@` query — flag any mismatch), and whether a
GX/Cerbo is present on this bus and how it appears to categorize the device (if
determinable from the capture alone — a definitive answer may require the GX's own UI,
which is outside a passive CAN capture and outside Stage A's tooling here).

**Pass/fail:** PASS = a clean capture with an identifiable PGN/DGN set and bit rate.
**Unexpected result → stop:** if your tool reports it went active (transmitted
anything) — stop, verify the tool's listen-only setting, and do not resume until
confirmed silent.

---

### Procedure 3 — Field PWM frequency (scope on the field wire)

**Purpose:** bench-confirm the real field-drive PWM frequency, closing `PROJECT_PLAN.md`
§0.6 V2 at the bench-measurement tier (the top of the evidence-precedence list in §0.6)
and settling the "~400 Hz verbal vs 143.2 Hz binary-RE" question with a direct
measurement.

**What this settles:** `PROJECT_PLAN.md` §0.6 **V2** (field PWM = 143.2 Hz, PSC=326,
ARR=1024, 10-bit duty — currently ✅ from binary + Renode dynamic trace, this is the
last, bench-tier leg).

**Equipment:** oscilloscope.

**Gap to flag before starting:** the manual's harness pinout table (`WS500_HARDWARE_SPEC.md`
§6c) enumerates wires 1–4, 6, 8–13 — there is **no field-output wire number** in that
table (the field drive goes to the alternator's F/rotor terminal, evidently via a
separate connector/lead from the 13-wire main harness, not documented in any tracked
doc). **This is a physical-identification step you must do first, safely, without
disturbing the running system** — identify the field output lead(s) by visual
inspection/tracing at the unit before probing. Do not guess and probe an unidentified
wire.

**Steps:**

1. Identify the field-drive output lead(s) to the alternator by physical inspection
   (do not disconnect anything; this is a non-invasive scope tap on an existing,
   connected wire).
2. Probe the field-drive line with the scope, referenced to the alternator/regulator
   local ground (not battery negative directly, to avoid an unnecessary ground path
   through the harness — use whatever local ground reference your scope setup already
   uses for the other Stage-A taps).
3. Capture while the unit is actively driving field (i.e., during a charge cycle — cross
   reference against the `$RAS:` / passive `AST;` stream from Procedure 1 running
   concurrently, so you can correlate the scope trace's duty cycle against the AST
   `FLD %` field at the same moment).
4. Measure: fundamental PWM frequency, and duty cycle at capture time.

**Record:** measured frequency (Hz), measured duty (%) alongside the concurrently-logged
AST `FLD %` for cross-check, and a saved scope capture/screenshot if your instrument
supports it.

**Pass/fail:** **PASS** = measured frequency ≈ 143.2 Hz, consistent with §0.6 V2 (PSC=326,
ARR=1024 @ 48 MHz timer clock). **Unexpected result → stop and flag for review:** a
measurement near 400 Hz, ~1 kHz, or any value materially different from 143.2 Hz
contradicts the binary-RE + Renode-corroborated V2 finding and must not simply be
"corrected in"; it needs investigation (possible wrong-wire capture per the gap above,
scope setup issue, or a genuine surprise) before any design decision relies on it.

---

### Procedure 4 — Stator frequency vs known RPM (learned-K)

**Purpose:** derive/verify the empirical stator-frequency-to-RPM constant (poles ×
pulley ratio ÷ 60, per `PROJECT_PLAN.md` §0.5's "learned-K") against a real, known RPM,
for `stator_rpm.c`'s poles/pulley configuration.

**What this settles:** not a numbered §0.6 V-item (the stator **pin and acquisition
method** — PA10 EXTI rising-edge + TIM2 free-running timebase — is already ✅ resolved
by V1/V2 from the binary). This procedure settles the **physical constant**
(`stator_rpm.c`'s poles/pulley config, currently "fail-safe LOST until configured" per
`PROJECT_PLAN.md` §2 M3 notes), which is install-specific data, not a hardware fact.

**Equipment:** oscilloscope (or frequency counter), independent reference tachometer.

**Steps:**

1. Locate wire 8 (Yellow, "Stator") on the harness — labeled directly in
   `WS500_HARDWARE_SPEC.md` §6c, no identification ambiguity here.
2. Scope/count the stator signal frequency while the engine runs at one or more known,
   steady RPMs (per your independent reference tachometer).
3. Concurrently read the WS500's own derived RPM from the `AST;` stream (Procedure 1) or
   `$RSS:AST@` — the Comms & Config Guide documents `RPMs` in the `AST;` string as
   "Measured RPMs of engine (derived from alternator RPMs and the engine/alternator
   drive ratio)."
4. At each steady RPM point, record: reference RPM, measured stator frequency (Hz), and
   the WS500's own reported `RPMs`.

**Record:** a table of (reference RPM, stator Hz, WS500-reported RPM) at 2+ engine speed
points if practical — enough points to derive/sanity-check the pole count × drive-ratio
relationship, or at minimum to confirm the WS500's own reported RPM tracks a known
reference sanely.

**Pass/fail:** PASS = stator frequency scales linearly with reference RPM as expected,
and the WS500's own reported RPM is in the right ballpark (it's already using *some*
poles/ratio default — this isn't a pass/fail on our config, since our firmware's
`stator_rpm.c` values haven't been set from this data yet). **Unexpected result → stop:**
no stator signal at all while the engine is confirmed running (check wire 8 continuity/
connection before assuming a driver problem).

---

> **⚠ Gate on the NTC channel binding (added 2026-07-28, GH#8 / GH#40 / GH#43).**
> While you have the harness accessible, the other high-value measurement is
> **which of PA1/PA2 carries harness wire 4 (Alternator Temp Sense) and which
> carries wire 9 (Battery Temp Sense)** — inject a known resistance on each wire
> in turn and see which ADC slot moves. That binding is what arms the LFP
> low-temperature charge cutoff in V1 (GH#40), so it is worth doing.
>
> **The `adc_a` prohibition that used to stand here is lifted (GH#43,
> 2026-07-28).** It existed because the app fed alternator over-temp and the
> thermal governor from channel A only, discarding `alt_temp2_c` — so binding
> the battery probe to A silently disabled alternator thermal protection. Both
> call sites now fold `ctrl_nan_max2(alt_temp_c, alt_temp2_c)`, the max of
> whichever alternator-side channels are finite, so the surviving channel keeps
> the guard alive whichever one the battery probe claims. **Either `adc_a` or
> `adc_b` is safe to configure.**
>
> What has NOT changed is which physical wire each channel is: the PA1/PA2
> mapping is still bench-pending. Record what the resistance-injection test
> above tells you, and set `batt_temp_src` from that measurement rather than
> from the pin names — this procedure is what establishes the mapping, so
> nothing downstream should assume it before this step is done and written up.

### Procedure 5 — Battery/alt sense vs reference DMM

**Purpose:** validate the 34.33:1 voltage-divider scaling (§0.6 V4) and the INA226
current/voltage readings (§0.6 V3) end-to-end, against ground truth.

**What this settles:** `PROJECT_PLAN.md` §0.6 **V4** (ADC scaling: 34.3333:1 divider,
3.3 V Vref, PC5) at the bench-measurement tier, and provides an end-to-end sanity check
on **V3** (INA226 register map / CALIB-from-shunt) via the reported `AltAmps`/`BatAmps`
values — V3 itself is a register-map fact already resolved from the binary; this
procedure checks the *result*, not the register map.

**Equipment:** DMM; DC clamp ammeter (optional, for the current cross-check).

**Steps:**

1. DMM across wire 10/11 (Voltage Sense +/-, Kelvin sense per `WS500_HARDWARE_SPEC.md`
   §6c) — or directly at the battery terminals if that's a cleaner tap — and record the
   reading.
2. Concurrently pull `BatVolts` (and `AltVolts`) from the `AST;` stream (Procedure 1) or
   `$RSS:AST@` and compare against the DMM reading.
3. If a clamp ammeter is available: clamp it around the battery or alternator cable
   (whichever matches the configured shunt location — check the `CST;` `ShuntAtBat?`
   field from Procedure 1's `$RAS:` capture to know which), and compare against the
   concurrent `AltAmps`/`BatAmps` from `AST;`.
4. Repeat at more than one point in time if the charge state is changing (e.g. once
   during bulk, once near float) to check the scaling holds across the operating range,
   not just at one instant.

**Record:** DMM voltage vs AST-reported `BatVolts`/`AltVolts` (and current, if measured)
at each sample point, with timestamps so they can be cross-referenced against
Procedure 1's log.

**Pass/fail:** PASS = AST-reported values track the DMM/clamp-meter readings within a
reasonable tolerance (define what "reasonable" means for your instruments — e.g. DMM
accuracy spec plus the AST string's own stated resolution: 1 mV for voltage, 0.1 A for
current). **Unexpected result → stop and flag:** a large, consistent offset (not just
sampling jitter) between DMM and AST voltage contradicts the recovered 34.33:1 divider
and needs investigation before that constant is trusted further.

---

### Procedure 6 — GH#36: I²C bus identification (PB6/PB7 vs PB10/PB11)

**Purpose:** settle which physical I²C bus is actually live — PB6/PB7 (I²C1) or
PB10/PB11 (I²C2) — resolving the one open item under §0.6 V7, tracked as **GH#36**.
Getting this wrong means `Core/Src/ina2xx.c` (and the EEPROM driver) target a dead bus
and sensor/config comms simply don't happen.

**What this settles:** `PROJECT_PLAN.md` §0.6 **V7**'s residual bench caveat — "the
shared active-handle slot is bound to hi2c2… bench falsification (Stage-A, readings-only):
scope PB6/PB7 vs PB10/PB11 at boot — the pair carrying 0x40/0xA0 traffic is live." This
also indirectly re-confirms **V3**'s bus-correction note (V3 originally read I²C1 from
the pre-DeInit state; V7 corrected this to I²C2).

**Equipment:** oscilloscope or logic analyzer with **at least 2 channels** (4 preferred
— PB6, PB7, PB10, PB11 simultaneously, so a single boot event settles both buses in one
capture instead of two separate power cycles), fine-pitch probes/grabbers.

**This procedure is more invasive than Procedures 1–5 — read this before starting:**

- **PB6/PB7/PB10/PB11 are internal MCU pins on the LQFP64 package** (§0.6 V1 confirms
  the package), not signals broken out to the external 13-wire harness described in
  `WS500_HARDWARE_SPEC.md` §6c. Reaching them requires **opening the enclosure** to
  access PCB test points/pads or the MCU pins directly — assess whether you have
  suitable fine-pitch (0.5 mm) probing gear (micro-grabbers, or magnet-wire tack leads)
  before committing to this step. If you don't, defer this procedure rather than forcing
  a probe onto adjacent pins.
- **The bus assignment is only observable "at boot"** (§0.6 V7: I²C1 is inited then
  DeInit'd early in the boot sequence, before the INA226/EEPROM traffic starts on I²C2).
  On an already-running, continuously-powered installed unit, observing this means
  triggering an MCU reset. `FLASH_AND_RECOVERY.md` documents that a **short press** of
  the unit's reset button causes a normal reboot (a **press-and-hold for 5 s**, per the
  Comms & Config Guide's "Firmware Recovery" section, is what enters DFU — a short press
  is a plain reset, not a DFU-entry action). Verify after the reset that the unit
  resumed normal operation (LED pattern, `$` port responsive) rather than landing in DFU.
- **Do this only with the engine off / no active charge cycle in progress.** A momentary
  MCU reboot briefly interrupts regulation; on a live 48 V system that's a needless risk
  to take while the unit is actively charging, even though the reboot itself doesn't
  touch the field-drive path. This precondition isn't stated in `SAFETY.md` verbatim —
  it's an added, conservative reading of "avoid needless risk to the live system"
  applied specifically to this procedure; use judgement, but default to deferring the
  reboot until the system is quiescent.

**Steps:**

1. Confirm the engine is off / no active charge cycle (see precondition above).
2. Open the enclosure per whatever the unit's teardown procedure is (not documented in
   this repo — use manufacturer service guidance / visual inspection; do not force
   anything).
3. Set up probes on PB6/PB7 and PB10/PB11 (or as many as your channel count allows).
   Set the scope/analyzer to trigger on power-up / the earliest activity you can catch
   (a normal-mode capture with a generous pre-trigger buffer, since you don't have a
   clean external trigger signal for "MCU reset" — a single short press of the reset
   button is your event).
4. Short-press the reset button (not press-and-hold) to trigger a normal MCU reboot.
5. Capture the first several hundred milliseconds to a few seconds after the reset —
   long enough to see I²C1's brief init-then-DeInit (if it happens at all — the pattern
   V7 describes may be too fast to catch on a first pass; a second capture with tighter
   framing may be needed) and I²C2's ongoing traffic to 0x40 (INA226) and the computed
   0xA0-family address (24C16 EEPROM, per V7).
6. Confirm the unit resumed normal operation after the reset (see the "STOP immediately
   if" list in §0).

**Record:** which bus (I²C1 or I²C2) shows the 0x40/0xA0-class traffic, a saved
capture/screenshot, and explicit confirmation the unit returned to normal operation
after the reset.

**Pass/fail:** PASS (and closes GH#36) = PB10/PB11 (I²C2) carries the 0x40/0xA0 traffic
and PB6/PB7 (I²C1) is idle after the brief init/DeInit window, consistent with V7's
disassembly-derived finding — `ina2xx.c`'s BUS CAVEAT comment can then drop the caveat
language. **Unexpected result → stop and flag for review, do not silently "fix" the
driver:** if I²C1 (PB6/PB7) turns out to carry the live traffic instead, that
contradicts V7 and needs a second pass at the disassembly before `ina2xx.c` is
re-pointed — don't change the bus in code off a single ambiguous capture.

---

### Procedure 7 — M1-adjacent: full-flash backup + RDP check (read-only)

**Purpose:** the concrete M1 rehearsal — a full read-only backup of the stock flash
image, plus a check of the readout-protection (RDP) level, both via the ST ROM DFU
bootloader. This is **GH#3**. It is grouped with the Stage-A ladder because it is
strictly read/upload-only and touches nothing that isn't already reversible — but it is
listed **last and separately** because it is the one procedure that briefly takes the
unit out of application firmware and into the ROM bootloader, and because the M1
milestone (`PROJECT_PLAN.md` §2) is not yet closed — this procedure *is* what closes it.

**What this settles:** `PROJECT_PLAN.md` §2 **M1** exit criterion ("stock image
demonstrably restores the unit via DFU on the bench") — this procedure covers the
*backup* half; the *restore* half is a separate, later rehearsal once a spare flash
target or confidence in the backup exists, and is explicitly **out of scope here** (see
§3).

**Prerequisite — not yet met on this machine:** `dfu-util` or STM32CubeProgrammer must
be installed first. `FLASH_AND_RECOVERY.md` names both as acceptable; STM32CubeProgrammer
additionally gives a friendlier RDP-level readout (its Option Bytes panel shows RDP
directly) than parsing `dfu-util` output, so consider it for the RDP-check half even if
`dfu-util` is your choice for the bulk upload. **Do not proceed with this procedure
until one is installed and you've confirmed you can invoke it.**

**Discipline (from `FLASH_AND_RECOVERY.md`, restated here because it's the one hard
constraint of this whole procedure):** **read/upload only. Never write/erase. Never
touch RDP.** Disabling RDP mass-erases the stock image — if RDP ≥1 is found, that is the
end of this procedure for this session (see Pass/fail below), not a problem to solve by
changing the RDP level.

**Equipment:** USB cable, `dfu-util` or STM32CubeProgrammer installed on the laptop.

**Precondition:** same as Procedure 6 — do this with the engine off / no active charge
cycle in progress, since it briefly takes the application out of service.

**Steps:**

1. Enter DFU mode: press-and-hold the unit's reset button for 5 seconds and release
   (per `FLASH_AND_RECOVERY.md` / the Comms & Config Guide's "Firmware Recovery"
   section). Confirm the unit re-enumerates as "STM32 BOOTLOADER" (VID 0x0483 /
   PID 0xDF11).
2. **Check RDP first, before any read.** Using STM32CubeProgrammer's connect/Option
   Bytes view (or `dfu-util`/equivalent read of the option-byte region if using
   `dfu-util` alone — consult its documentation for the exact invocation on this OS, the
   exact CLI isn't pinned in this repo), determine the RDP level.
   - **If RDP ≥ 1:** stop here. Do not attempt a flash read (it will be blocked, and
     disabling RDP to proceed would mass-erase the stock image). The already-held stock
     `.dfu` image file **is** the backup in this case — `FLASH_AND_RECOVERY.md` says
     explicitly to "prove it restores before relying on it," which is a **separate,
     later Stage-C-adjacent exercise**, not part of this session.
   - **If RDP = 0:** proceed to step 3.
3. Perform a full read-only upload of the flash image (the "read/upload" direction —
   e.g. `dfu-util`'s `-U` upload flag, or STM32CubeProgrammer's read/upload function;
   confirm you're using the read path, not the `-D`/download-to-device path used for
   writes, which is a different flag in `dfu-util` and shown for writing, not reading,
   in `QUICK_START.md`'s example).
4. Save the resulting binary with a clear, timestamped filename.
5. Exit DFU cleanly — either the tool's own "leave" option or a plain power cycle — and
   confirm the unit resumes normal operation (LED pattern, `$` port responsive again).

**Record:** the RDP level found, the saved flash-image binary (filename + size + a
checksum you compute independently, e.g. SHA-256, so future integrity checks don't
depend on trusting the tool's own report), and confirmation the unit returned to normal
operation afterward.

**Pass/fail:** PASS = either (a) RDP = 0 and a full flash image was successfully read
and saved, or (b) RDP ≥ 1 was detected and the procedure stopped cleanly without
attempting to clear it — **both are valid, complete outcomes for this session**; (b)
just means the stock `.dfu` file is the backup of record instead. **Unexpected result →
stop:** DFU entry fails to enumerate at all (check cable, check the reset-button timing
against the 5 s hold), or the unit does not cleanly return to normal operation after
exiting DFU (see the top-level "STOP immediately if" list — this is the scenario it's
mainly written for).

---

## 3. NOT IN THIS RUNSHEET (Stage B / Stage C — do not do these here)

Anything below requires the Stage-A config archive to exist first (Stage B) or requires
M1 + the §8 virtual gauntlet (Stage C). None of it belongs in a Stage-A session:

- Any `$CPx:`/`$SCx:`/`$CCN:`/`$CDD:` write command, `$MSR:` (factory reset), `$RBT:`
  (reboot-to-apply, only meaningful after a write), or `$FRM:` (force regulator mode) —
  all Stage B, and only after the Procedure 1 archive exists and is trusted.
- Any DFU **write/download** (`dfu-util -D`, STM32CubeProgrammer's flash/program
  action) of any image, stock or custom — Stage C, gated on M1 (both halves — backup
  *and* rehearsed restore) plus the `PROJECT_PLAN.md` §8 virtual-first gauntlet (SIL +
  Renode + stock-binary verification + property/fault-injection tests) being green.
- Disabling/changing RDP, WRP, or any other flash protection option byte.
- Flashing or running `test-fw` against this unit — `test-fw` is a bench-only bring-up
  target for a *dedicated* bench unit (its own README's safety section is explicit
  about this); it is never appropriate for the installed, live unit regardless of its
  own internal field-duty guards.
- Commanding the field, in any way, at any duty — the entire premise of Stage A is that
  the field-drive path is never touched.
- Restoring the Procedure 7 backup, or any `.dfu` image, to the unit — that's the other
  half of M1 and is a separate, later rehearsal, not part of this runsheet.

---

## 4. Results capture (fill in at the bench)

| # | Procedure | Date/time | Operator | Measured/observed | Settles (§0.6 / doc) | Result | Artifact / notes |
|---|---|---|---|---|---|---|---|
| 1 | USB `$` config + status dump | | | | stock-dump artifact (§6a / SAFETY.md) | bench-pending | |
| 2 | CAN bus sniff | | | | validates `CAN_INTEGRATION.md` | bench-pending | |
| 3 | Field PWM frequency | | | | §0.6 **V2** (bench tier) | bench-pending | |
| 4 | Stator frequency vs known RPM | | | | learned-K (`stator_rpm.c` config) | bench-pending | |
| 5 | Battery/alt sense vs DMM | | | | §0.6 **V4**; sanity-checks **V3** | bench-pending | |
| 6 | I²C bus ID (GH#36) | | | | §0.6 **V7** bench caveat; sanity-checks **V3** | bench-pending | |
| 7 | Full-flash backup + RDP check | | | | M1 exit criterion (backup half); GH#3 | bench-pending | |

---

## 5. Candidate additions — owner to confirm

Things this pass through the source docs suggests Stage A *could* usefully measure, but
that aren't part of the six items `SAFETY.md`/`PROJECT_PLAN.md` §5 explicitly enumerate,
and that this runsheet therefore does not assert as required. Flagging for the owner to
decide whether to fold in:

- **App-mode USB descriptor.** Only the **DFU-mode** VID:PID (0x0483/0xDF11, ST's ROM
  bootloader) is documented anywhere in this repo (`WS500_HARDWARE_SPEC.md` §1). The
  normal-operation `$`-protocol CDC port's own VID:PID/string descriptor isn't recorded
  anywhere. Procedure 1 step 1 already captures this as a free byproduct (Device
  Manager) — worth explicitly folding into `IO_COVERAGE.md`/`WS500_HARDWARE_SPEC.md` as
  a small addition once confirmed.
- **IR/contact-thermometer cross-check of `ATemp`/`FTemp`.** §0.6 **V8** claims PA3 is
  the internal FET/driver temperature channel (not battery), based on binary RE. A cheap
  physical cross-check — an IR or contact thermometer on the FET/driver area during a
  charge cycle, compared against the `AST;` `FTemp` field — would be a bench-tier
  corroboration of V8 at negligible cost/risk, but it isn't one of the six items named
  in `SAFETY.md`.
- **Passive PB13 (Enable vs Feature-In) and Lamp-vs-LED identification.** `IO_COVERAGE.md`
  currently scopes this as a bench item resolvable via `test-fw`'s `gpio` command — which
  requires flashing a dedicated bench unit, explicitly not this one. But wires 1
  (Brown, Ignition/Enable) and 3 (White, Feature-In) and 2 (Orange, Lamp/Feature-Out)
  are all on the external harness (`WS500_HARDWARE_SPEC.md` §6c + the Comms & Config
  Guide's wire-color notes) and could plausibly be **passively** correlated with a DMM
  against known engine start/stop events and the `$RAS:` `System Options` bit field
  (which reports live Feature-IN activation state) — no firmware flash required. Worth
  considering as a Stage-A addition since it could close two 🟡 `IO_COVERAGE.md` rows
  without waiting on the bench-unit-only `test-fw` path.

None of the above are asserted as facts in this runsheet — they're measurement
opportunities for the owner to accept, defer, or decline.
