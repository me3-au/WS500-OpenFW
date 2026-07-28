# WS500-OpenFW — CAN / NMEA 2000 Integration

> **Status:** draft. Describes the v1 CAN design (authoritative: `CONTROL_SPEC_NEXTGEN.md`
> §8).
>
> **Implemented as of 2026-07-27: both Tx dialects — §2 (N2K, GH#18/#9) and
> §8 (RV-C, #10a).** N2K encoders (`control/n2k_encode.c`), ISO 11783-81
> address claim (`control/n2k_addrclaim.c`), both cadence engines
> (`control/n2k_sched.c`, `control/rvc_sched.c` — the latter including the §8
> RBM election), the RV-C DGN encoders (`control/rvc_encode.c`) and the bxCAN
> glue (`Core/can_n2k.c`) are written and host-tested in CI — but have **never
> run on a real bus**, so both are "built, unverified", not "working". RV-C is
> **disabled by default**; enabling it is a driver-level call today, not yet a
> config field. **Spec-closure 2026-07-28 (§9): the address-claim
> question is RESOLVED — RV-C ADDRESS_CLAIM is `60928`/DP=0, exactly as
> implemented** (the `0x1EE00` fear from a vendor guide is refuted by the RV-C
> spec text itself, now read: RVIA "RV-C Specification Full Layer" rev.
> 2025-07-31). The same read found seven identity/scaling defects that are
> **blocking before either dialect's Tx meets a real bus** — see the §9.2
> table (N2K device function 140→141, "Engine"→"DC Generator/Alternator";
> RV-C instance 0→1, device priority 100→80, CHARGER_STATUS current offset
> encoding, DC_SOURCE_STATUS_1 current sign, percent scale 0.4→0.5 %/bit,
> preferred address 35→128). §3 (Rx / control-in) and §5
> (multi-regulator sync) remain target behaviour.
> The §8 ingestion caveat remains the open question that matters most:
> whether a real Cerbo actually *categorizes and displays* this device is
> bench-pending (#18 N2K, #32 RV-C). The identity codes it turns on are now
> spec-dispositioned in §9, including verdicts for the values living in
> `Core/Src/can_n2k.c` (which the docs/spec pass could not edit).

The regulator has **one** CAN bus (bxCAN on the WS500). Over that single bus it speaks
several dialects at once — NMEA 2000, J1939, Victron VE.Can, RV-C, and CAN-BMS frames —
configured per install. This document explains what it **sends**, what it **listens
for**, and how it fits common systems.

---

## 0. Priority & architecture (v1)

- **CAN Tx (telemetry OUT) is the near-term target** — get the regulator onto a
  **Victron Cerbo GX**. Read-only broadcast, so it can't affect the control loop.
- **A Cerbo's VE.Can port is *either* NMEA 2000 *or* RV-C — not both** (mutually
  exclusive per port; the Cerbo has one port — only Venus/Ekrano GX have two). So
  **which dialect reaches a given Cerbo depends on the owner's port profile**: marine
  installs usually run VE.Can/N2K, RV installs often run RV-C. To appear on *any* Cerbo
  the regulator should therefore support **both N2K and RV-C Tx**.
  *(Caveat: Victron's ingestion is device-type-specific — the documented RV-C-IN types
  are tanks/batteries/senders, not alternators — so verify the device actually shows on
  real hardware; it may need to present as a DC-source/charger type the GX accepts.)*
- **CAN Rx (control IN — BMS/DVCC ceilings) is V2 scope** (owner decision 2026-07-28,
  PROJECT_PLAN §1.1 — covers BMS, pre-disconnect, DVCC, external battery-monitor V/I,
  J1939 engine, multi-WS500 sync). In V1 those ceilings simply aren't in the
  arbitration min() and the regulator runs on its own profile + hardware limits.
  The already-implemented BMS decoder slice stays in-tree but must be gated
  default-off for V1 (PROJECT_PLAN #10).
- **Dialect-neutral snapshot:** the firmware builds one internal telemetry snapshot
  (`control/telemetry.h`) describing *what* to report; per-dialect **encoders** map it
  to the wire. **N2K encoder first**, **RV-C encoder close behind** (both feed a Cerbo
  depending on its profile), reading the same snapshot. The control core never knows
  about wire formats.

---

## 1. Design principle: BMS/DVCC behind one interface

Every battery/charge-control integration is a **driver** behind a single internal
interface: `{ charge-voltage limit, charge-current limit, SOC, alarms, pre-disconnect }`.
Vendor quirks never reach the control logic, and adding a new BMS is a driver, not a
feature. **All inbound ceilings land in the same arbitration `min()`** (see
`USER_MANUAL.md` §1) — a BMS that says "max 40 A" simply becomes another ceiling, and
the display shows *BMS* when it binds.

Every inbound signal has a **declared loss-of-signal fallback** — if BMS/DVCC comms
drop, the regulator falls back to its own profile limits (and raises a fault), it does
not free-run.

## 2. What it transmits (telemetry out)

Appears on the network as an **alternator/charger** device:

| PGN | Content |
|---|---|
| 127508 | Battery status — V, A, temperature |
| 127506 | DC detailed status (when acting as bank monitor) |
| 127488 | Engine parameters rapid — relayed **fused RPM** for MFD tachs (optional) |
| 127750 | Charger status |
| 126983 / 126985 | Alerts (faults, plain-language) |
| 126996 / 126998 | Product / configuration info |
| *proprietary fast-packet* | Full telemetry: field effort, **binding ceiling**, temps, RPM source/state, active profile |

So a chart-plotter or Victron GX shows W / A / V, charge state, active profile, the
binding ceiling, temperatures, and faults — natively.

## 3. What it receives (control in) — **V2 scope** (PROJECT_PLAN §1.1)

| Source | Frames | Used for |
|---|---|---|
| **Victron DVCC** (via GX/Cerbo) | CVL / CCL (/DCL) | Charge ceilings into the arbitration min() |
| **Victron BMS** (Lynx Smart, VE.Bus) | DVCC path + BMS status | Ceilings, SOC, **pre-disconnect → soft field ramp** |
| **REC BMS** | 0x351 CVL/CCL, 0x355 SOC, 0x356 V/I/T, 0x35A alarms | Ceilings, SOC, alarms (no GX needed) |
| **JK BMS** | Victron-style CAN BMS frames | Same as above (quirks isolated in a driver) |
| **Engine** (J1939) | EEC1 (RPM), ET1 (coolant) | RPM source; warm-up gate |
| **External battery monitor** (N2K) | 127508 / 127506 | Battery-side current truth (arms tail exit when the local shunt is alt-side) |

**Pre-disconnect handling:** when a BMS signals it is about to open its contactor, the
regulator runs a **soft field ramp-down first** — load-dump prevention by protocol, not
just by TVS clamp.

## 4. Load-dependent tail detection (shunt placement)

The single shunt can be battery- or alternator-side:

- **Battery-side (recommended):** battery current is measured truth → tail-current
  charge-exit and Ah-based revert are fully armed.
- **Alternator-side:** with loads between alternator and battery, alternator current ≠
  battery current, so local tail logic is **disarmed** — but a battery-side current from
  an **external N2K battery monitor** re-arms it *(that re-arm path is CAN-IN, so **V2**;
  in V1 an alt-side shunt means tail exit stays disarmed — battery-side is the V1
  recommendation)*. The regulator states this at commissioning rather than letting the
  ambiguity ride.

## 5. Multi-regulator sync — **V2 scope** (PROJECT_PLAN §1.1)

Twin engines / twin alternators coordinate over the shared bus (leader/follower): shared
stage state, **load-sharing in Watts**, and one combined tail-exit decision — so two
regulators charging one bank don't fight.

## 6. Single-bus notes (v1)

One physical CAN carries every integration above **plus** the regulator-sync traffic.
The dialect mix (N2K / J1939 / Victron / BMS) is configured per install; bus-load
budgeting and ID-collision rules are handled in firmware, not by you. *(A second CAN
bus is `⟦future-hw⟧`.)*

## 7. Quick setup

1. Wire the CAN via the RJ45 connector; **terminate both ends** of the N2K backbone.
2. In config, enable the dialects your system uses (e.g. *NMEA2000 + Victron DVCC*, or
   *REC BMS*).
3. If you use a BMS, confirm its charge ceilings appear in the monitor (you should see
   *BMS* bind when the battery limits charging).
4. Declare your **shunt location** (§4) so tail-exit behaves correctly.
5. For twin installs, enable **regulator sync** on both units.

## 8. RV-C — a second Tx dialect (co-important, not just RV)

RV-C is the **RV industry's** CAN standard (motorhomes/trailers), as NMEA 2000 is the
**marine** one. Both are J1939/CAN-based; a given CAN port runs one profile or the other.

- **The Victron Cerbo GX supports RV-C** (both IN and OUT) — its VE.Can port is set to
  *either* the VE.Can (N2K) profile *or* the RV-C profile, **not both** (the Cerbo has a
  single port; only Venus/Ekrano GX have two). So **an RV owner whose Cerbo runs the RV-C
  profile will only see us if we speak RV-C.** RV-C is therefore *co-important with N2K*,
  not an RV-only afterthought.
- **RV-C Tx** emits its own DGNs: `CHARGER_STATUS`, `CHARGER_STATUS_2`,
  `DC_SOURCE_STATUS_1/2/3` (V/A/T/SoC). *(An earlier draft listed
  `ALTERNATOR_STATUS`; no DGN of that name exists in the 2025 RV-C spec — §9.)*
- **RBM (Remote Battery Master):** a priority-based *election* — only the highest-
  priority device broadcasts battery (`DC_SOURCE`) data. If we transmit RV-C battery data
  we must implement defer-to-higher-priority behavior (the stock WS500 supported this).
- **Ingestion caveat (both dialects):** Victron's documented RV-C-IN device types are
  tanks/batteries/senders — an alternator/charger isn't listed — and N2K ingestion is
  likewise device-type-specific. So *showing up* on the Cerbo must be **verified on real
  hardware**; the device may need to present as a DC-source/charger type the GX accepts.
- **Cost/impact:** RV-C is an **additional Tx encoder** (the NMEA2000 library doesn't do
  RV-C) plus the RBM election — bounded work. It reads the **same telemetry snapshot** as
  the N2K encoder, so the control core is untouched.

**Decision (v1):** build the **N2K encoder first** (library in hand), **RV-C close behind**
as the second encoder over the same snapshot — both are real paths to a Cerbo depending on
its port profile. Verify actual GX display on hardware for each.

## 9. Network-identity spec closure (2026-07-28)

The CAN network-identity `[SPEC-SIGNOFF]` markers were disposed on 2026-07-28
against primary sources: the official RV-C specification (RVIA, **"RV-C
Specification Full Layer", rev. July 31 2025** — public download from rvia.org,
cited rather than vendored for copyright), the NMEA 2000 device class & function
codes v2.00 (machine-readably mirrored in canboat's `DEVICE_FUNCTION` table),
and the linuxkidd/rvc-monitor-py decoder (independent unit-conversion
cross-check). Per-field detail lives in `control/Inc/rvc_sched.h`,
`control/Inc/rvc_encode.h` and `control/Inc/n2k_encode.h`; this section records
the verdicts for constants living in files the spec-closure pass could not edit
(`Core/Src/can_n2k.c`, `control/Src/*.c`) and the follow-up work.

### 9.1 ADDRESS_CLAIM data page — RESOLVED: implementation correct

RV-C's ADDRESS_CLAIMED DGN is **`EE00h` = PGN 60928, Data Page 0** (spec §3.3.2
Table 3.3.2b; DGN dictionary "ADDRESS_CLAIMED EE00h 60928") — exactly what the
reused `n2k_addrclaim.c` implements. The `0x1EE00`/DP=1 fear from the Xantrex
guide is refuted; RV-C mirrors N2K's own structure (application DGNs on DP=1,
network management on the inherited J1939 DP=0 PGNs). Do not fork
`n2k_addrclaim.c`. Two real deltas were found instead (detail in
`rvc_sched.h`'s file header): our claims use the J1939 `..EEFFxx` identifier
form, which spec §4.3.1.2 explicitly tolerates (our Rx handles both forms and
answers RV-C's directed address requests); and the preferred start address must
move into the charger dynamic range (row 7 below).

### 9.2 Verdicts requiring code changes — BLOCKING before the affected dialect's Tx is enabled on a real bus

| # | Where | Today | Verdict | Authority |
|---|---|---|---|---|
| 1 | `Core/Src/can_n2k.c` `N2K_DEVICE_FUNCTION` | 140 (comment calls it "Alternator") | **141** — in class 35, 140 = "Engine", 141 = "DC Generator/Alternator"; as-is an MFD lists us as an engine | NMEA class/function codes v2.00 (canboat) |
| 2 | `control/Src/rvc_sched.c` `RVC_INSTANCE_DEFAULT` | 0 | **1** — instance 0 = "Invalid" in every emitted DGN; DC instance 1 = Main House Battery Bank | RV-C Tables 6.5.2b / 6.20.8b / 6.20.9b |
| 3 | `control/Inc/rvc_sched.h` `RVC_OUR_DEVICE_PRIORITY` | 100 | **80** — spec tier list: 80 = Charger; 100 = Inverter/Charger (we are no inverter) | RV-C Table 6.5.2b |
| 4 | `control/Src/rvc_encode.c` CHARGER_STATUS charge current | plain unsigned 0.05 A/bit | **offset-encoded**, raw 0x7D00 = 0 A — a compliant decoder reads today's +12 A as −1588 A | RV-C Table 5.3 |
| 5 | `control/Src/rvc_encode.c` percent fields (CHARGER_STATUS byte 5) | 0.4 %/bit | **0.5 %/bit** (uint8, 0–125 %) | RV-C Table 5.3 |
| 6 | `control/Src/rvc_encode.c` DC_SOURCE_STATUS_1 current | `amps_batt` unnegated (+ = charging) | **negate** — RV-C: positive = flow FROM the source (discharge); today a charging bank reads as discharging | RV-C Table 6.5.2b |
| 7 | `Core/Src/can_n2k.c` `RVC_PREFERRED_ADDR` | 35 (in the reserved 0–63 DSA space) | **128** — charger dynamic range is 128–143; up-counting from an in-range start is a permitted technique | RV-C §3.3.2, Table 7.2 |

Non-blocking refinements for the same pass: CHARGER_STATUS voltage/current are
spec-defined as **control (desired)** values — the measured pair belongs in
CHARGER_STATUS_2 (we currently send measured in both); and the RV-C NAME's
"class 30 / function 129" lands in what §3.3.3 defines as an optional
**Compatibility Field, normally 0** — RV-C NAMEs have no device class/function
semantics, so the "inverter-charger's codes" concern is moot (RV-C
categorisation is by transmitted DGNs, not NAME). Keep 30/129 (harmless, mimics
a real product) or zero the field; decide from what the Cerbo does at the #32
bench session.

### 9.3 Signed off, no code change

- **N2K**: device class 35 "Electrical Generation" (correct for function 141);
  industry group 4 marine (settled); preferred address 34
  (arbitrary-address-capable — only affects first-boot races); `n2k_version`
  2100 and LEN 1 (informational; LEN 1 claims 50 mA — the unit is
  battery-powered, so this is conservative; re-measure/zero at bench if anyone
  cares); manufacturer code 2046 (top-of-range hobby convention, shows as
  "unknown manufacturer" on displays); encoder saturation policy
  (last-valid-code peg).
- **RV-C**: DGN numbers (already settled); CAN priority 6 for every emitted DGN
  (Tables 6.5.2a/6.5.3a/6.5.4a/6.20.8a/6.20.9a); NAME industry-group bits = 0
  (spec-mandated "Always 0" — upgraded from vendor-table evidence to settled);
  all voltage scalings (u16 0.05 V/bit) and temperature scalings (u8 1 °C
  offset −40; u16 0.03125 °C offset −273); DC_SOURCE_STATUS_1 u32 current
  offset raw 0x77359400 = 0 A (was the weakest constant in the file, now
  verbatim Table 5.3); CHARGER_STATUS_2 existence + byte layout (§6.20.9 —
  official, no longer "vendor convergence"); CHARGER_STATUS operating-state
  enum; RBM defer timeout 1500 ms (firmware policy, 3× the spec's 500 ms gap);
  cadences: CHARGER_STATUS 5000 ms (Table 6.20.8a), CHARGER_STATUS_2 500 ms
  (Table 6.20.9a — disposes `rvc_sched.c`'s cadence `[SPEC-SIGNOFF]`),
  DC_SOURCE_STATUS_1/2 500 ms (Tables 6.5.2a/6.5.3a). DC_SOURCE_STATUS_3 stays
  unscheduled: the spec lists a 500 ms normal gap but our payload would be
  100 % not-available; revisit when SOH/capacity data exists.
- **N2K cadences** (`n2k_sched.c`): 126985 at 2.5 s and the proprietary PGN at
  1.5 s stay firmware policy — no standard interval exists to cite; disposed as
  engineering choices.

### 9.4 Bench-pending — first-real-bus falsification list

For **#18 (N2K)**: analyzer/Cerbo shows NAME class 35 / function 141 and
categorises the device as alternator-family (after §9.2 row 1 lands);
product-info strings and LEN render sanely; the proprietary 2046 fast-packet is
ignored by third-party gear.
For **#32 (RV-C)**: capture our ADDRESS_CLAIM and a Cerbo-in-RV-C-mode claim —
expect DP=0 identifier forms `18EE00xx`/`18EEFFxx`; confirm a directed address
request (request for `EE00h` sent to our address from SA 254) draws our claim;
after §9.2 rows 2–7 land, confirm the Cerbo displays charger + DC-source data
at instance 1 / priority 80 with sane V/A signs and magnitudes; observe whether
NAME compatibility-field 30/129 vs 0 changes GX behaviour.


---

*Exact PGN/DGN field mappings and the per-vendor BMS driver details are an implementation
deliverable; this document is the integration contract.*
