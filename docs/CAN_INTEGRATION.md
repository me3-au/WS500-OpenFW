# WS500-OpenFW — CAN / NMEA 2000 Integration

> **Status:** draft. Describes the v1 CAN design (authoritative: `CONTROL_SPEC_NEXTGEN.md`
> §8). The CAN driver/PGN layer is not yet implemented — this is the target behavior.

The regulator has **one** CAN bus (bxCAN on the WS500). Over that single bus it speaks
several dialects at once — NMEA 2000, J1939, Victron VE.Can, and CAN-BMS frames —
configured per install. This document explains what it **sends**, what it **listens
for**, and how it fits common systems.

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

---

*Exact PGN field mappings and the per-vendor BMS driver details are an implementation
deliverable; this document is the integration contract.*
