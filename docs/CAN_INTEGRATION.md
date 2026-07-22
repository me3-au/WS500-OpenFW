# WS500-OpenFW — CAN / NMEA 2000 Integration

> **Status:** draft. Describes the v1 CAN design (authoritative: `CONTROL_SPEC_NEXTGEN.md`
> §8). The CAN driver/PGN layer is not yet implemented — this is the target behavior.

The regulator has **one** CAN bus (bxCAN on the WS500). Over that single bus it speaks
several dialects at once — NMEA 2000, J1939, Victron VE.Can, RV-C, and CAN-BMS frames —
configured per install. This document explains what it **sends**, what it **listens
for**, and how it fits common systems.

---

## 0. Priority & architecture (v1)

- **CAN Tx (telemetry OUT) is the near-term target** — specifically **NMEA 2000 →
  Victron Cerbo GX**. It's read-only broadcast, so it can't affect the control loop,
  and the engine already produces everything worth reporting. Ships early.
- **CAN Rx (control IN — BMS/DVCC ceilings)** comes **later**; until then those
  ceilings simply aren't in the arbitration min() and the regulator runs on its own
  profile + hardware limits.
- **Dialect-neutral snapshot:** the firmware builds one internal telemetry snapshot
  (`control/telemetry.h`) describing *what* to report; per-dialect **encoders** map it
  to the wire. **NMEA 2000 encoder first** (Cerbo); **RV-C encoder later** (§8), reading
  the same snapshot. The control core never knows about wire formats.

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

## 3. What it receives (control in)

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
  an **external N2K battery monitor** re-arms it. The regulator states this at commissioning
  rather than letting the ambiguity ride.

## 5. Multi-regulator sync

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

## 8. RV-C (RV systems) — a later dialect

RV-C is the **RV industry's** CAN standard (motorhomes/trailers), as NMEA 2000 is the
**marine** one. Both are J1939/CAN-based and **coexist on one physical bus**, but use
different message sets and addressing.

- **Victron / Cerbo uses NMEA 2000, not RV-C** — so RV-C is *not* needed for the Cerbo
  goal. It matters for **RV-C systems** (RV-C displays, Firefly, etc.).
- **RV-C Tx** would emit its own DGNs: `CHARGER_STATUS`, `DC_SOURCE_STATUS_1/2/3`
  (V/A/T/SoC), `ALTERNATOR_STATUS`.
- **RBM (Remote Battery Master):** a priority-based *election* — only the highest-
  priority device broadcasts battery (`DC_SOURCE`) data. If we transmit RV-C battery
  data we must implement defer-to-higher-priority behavior (the stock WS500 supported
  this). NMEA 2000 telemetry does not need it.
- **Cost/impact:** RV-C is an **additional Tx encoder** (the NMEA2000 library doesn't do
  RV-C) plus the RBM election — bounded work, added when there's an RV-C user need. It
  reads the **same telemetry snapshot** as the N2K encoder, so the control core is
  untouched.

**Decision (v1):** NMEA 2000 out (Cerbo) first; RV-C is a scoped follow-on dialect.

---

*Exact PGN/DGN field mappings and the per-vendor BMS driver details are an implementation
deliverable; this document is the integration contract.*
