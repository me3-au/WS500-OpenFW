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
> config field. **Before anyone enables it, resolve the open question flagged
> in `control/Inc/rvc_sched.h`: whether RV-C address claim is DP=1 (`0x1EE00`)
> rather than the J1939 `60928` currently implemented** — if it is, RV-C claim
> frames go out where no RV-C node will look. §5 (multi-regulator sync)
> remains target behaviour.
> The §8 ingestion caveat is the open question that matters most:
> whether a real Cerbo actually *categorizes and displays* this device is
> bench-pending, and the device class/function codes it turns on are marked
> `[SPEC-SIGNOFF]` in `Core/Src/can_n2k.c` until then.
>
> **§3 (Rx / control-in), partial as of deliverable #10:** the §1 dialect-
> neutral interface (`control/bms_rx.h`/`.c`, host-tested) plus ONE vendor
> driver — the CAN-BMS/REC/JK 11-bit standard-ID frame set (`0x351`/`0x355`/
> `0x356`/`0x35A`) — decodes into `ctrl_ceilings_t.bms_ccl_w` and
> `ctrl_measured_t.soc_pct`/`soc_trusted`, with per-signal loss-of-signal
> fallback raising `CTRL_FAULT_LOST_BMS` (`Core/Src/main.c`). Byte layout and
> scale factors are `[SPEC-SIGNOFF]`/bench-pending — reconstructed from public
> documentation of this frame family, not verified against a REC/JK datasheet
> or bench unit; alarm/warning bytes are decoded at byte-pair granularity only
> (which byte, not which bit) for the same reason. **Two gaps found while
> building this, both `[SPEC-GAP]`:** (1) **CVL has no consumer in the control
> core** — `ctrl_ceilings_t` is Watts-only and `ctrl_profile_t.cv_target_vcell`
> takes no external override, so this driver decodes and exposes CVL but it
> is not yet in any min(); §6.3's "CVL below the profile CV target simply wins
> in the min()" is not implemented. (2) **pre-disconnect is not decodable**
> from the REC/JK frame set — no verified bit exists in `0x35A` for it; the
> field is exposed on the interface and always reads false. Victron DVCC-via-
> GX, JK vendor-quirk layers, J1939 engine input, N2K 127508/127506 inbound
> battery-monitor ingestion, and multi-regulator sync (§5) remain
> `TODO(GH#10)`.

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
- **CAN Rx (control IN — BMS/DVCC ceilings)** comes **later**; until then those
  ceilings simply aren't in the arbitration min() and the regulator runs on its own
  profile + hardware limits.
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

## 8. RV-C — a second Tx dialect (co-important, not just RV)

RV-C is the **RV industry's** CAN standard (motorhomes/trailers), as NMEA 2000 is the
**marine** one. Both are J1939/CAN-based; a given CAN port runs one profile or the other.

- **The Victron Cerbo GX supports RV-C** (both IN and OUT) — its VE.Can port is set to
  *either* the VE.Can (N2K) profile *or* the RV-C profile, **not both** (the Cerbo has a
  single port; only Venus/Ekrano GX have two). So **an RV owner whose Cerbo runs the RV-C
  profile will only see us if we speak RV-C.** RV-C is therefore *co-important with N2K*,
  not an RV-only afterthought.
- **RV-C Tx** emits its own DGNs: `CHARGER_STATUS`, `DC_SOURCE_STATUS_1/2/3` (V/A/T/SoC),
  `ALTERNATOR_STATUS`.
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

---

*Exact PGN/DGN field mappings and the per-vendor BMS driver details are an implementation
deliverable; this document is the integration contract.*
