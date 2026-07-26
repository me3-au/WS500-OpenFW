# WS500-OpenFW — Bench Safety

> **Extracted from PROJECT_PLAN.md §5 on 2026-07-26.** This document gates every hardware milestone.
> Authoritative details, updates, and the ongoing tracker remain in PROJECT_PLAN.md.

---

## Safety Rules

**Rules, in force until explicitly retired:**

1. **Current-limited bench supply** (13.2 V, start ≤1 A) — never a raw battery for bring-up.
2. **Dummy field load** — power resistor (~10 Ω, ≥50 W) until the loop + fault paths are proven.
3. **Fail-safe defaults** — firmware ships field-OFF; watchdog on; TIM1 break verified in M2.
4. **Duty-cycle cap compiled into test builds** (20 %).
5. **Never spin a real alternator without a battery connected** (load dump). Real-alternator work starts only in M6: dummy load → field coil on dead alternator → driven alternator on a battery bank with supervision.
6. **Recovery always one step away** — SWD permanently wired; stock restore rehearsed (M1).
7. **Exactly one WS500 exists — irreplaceable.** M1 (backup + rehearsed restore) is absolute; any test that could plausibly *damage* (not just brick) hardware gets a Renode dry run first.

---

## Installed-Unit Reality (2026-07)

The one WS500 is **installed and in service on a 48 V system with a 4 Ω (12 V-class) rotor** — it is not a spare bench unit, and any firmware mishap also takes down a live charging system. Two consequences:

### Rotor Overdrive: The #1 Hazard

A 12 V rotor on a ~48–57.6 V bus means sustained field duty above **≈25 % (≈21 % at 57.6 V absorption)** overdrives the rotor (100 % duty ≈ 12 A / ~580 W into a 3 A winding). The CONTROL_SPEC rotor duty clamp and the never-100%-duty bootstrap cap (PROJECT_PLAN §0.5) are *the* critical protections here, and both must be proven in virtual (PROJECT_PLAN §8) before any custom firmware runs on this unit.

**★ Confirmed on hardware (2026-07-24):** a live stock charge run (serial `AST` capture) showed the WS500 **pinning Field% at exactly 25 %** — holding there while delivering 140 A / 7.6 kW at ~2330 RPM, and ramping to 25 % in ~2 %/s steps then stopping. The unit is **field-clamp-limited, not voltage-limited** (BatV reached only ~54.7 V vs the 56 V target). So the 25 % rotor clamp our design specifies is exactly what the product does — our firmware must reproduce it. Reference trace: `ws500_chargerun.log` (session capture).

---

## Hardware Access Ladder

Hardware access is staged — readings first:

### Stage A — Observation Only (Safe Now, Stock Firmware Untouched)

**Scope:** safe to perform immediately without affecting the running system.

- USB `$` protocol readout (dump + archive the full stock config — also documents the stock parameter set)
- CAN bus sniffing (log the PGN set → validates CAN_INTEGRATION.md)
- Scope / DMM on harness wires:
  - Field PWM wire (measures the real PWM frequency: closes PROJECT_PLAN §0.6 V2 at the bench level)
  - Stator wire (frequency vs known RPM → K)
  - Battery / alt sense vs a reference meter (validates the 34.33:1 divider and INA readings end-to-end)

### Stage B — Reversible Config Interaction

**Gate:** only after the Stage-A config archive exists.

- `$` protocol writes are stock-supported and restorable from the dump.

### Stage C — Custom Firmware

**Gate:** only after M1 (proven DFU backup/restore) *and* the PROJECT_PLAN §8 virtual gauntlet passes.

- First flash happens on the bench (unit temporarily removed), never in-situ.
