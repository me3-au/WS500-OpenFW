# WS500-OpenFW — Bench Safety

> **Extracted from PROJECT_PLAN.md §5 on 2026-07-26.** This document gates every hardware milestone.
> Authoritative details, updates, and the ongoing tracker remain in PROJECT_PLAN.md.

---

## Safety Rules

**Rules, in force until explicitly retired** *(amended 2026-07-29 — in-place testing decision, below; rules 1–2 replace the retired bench-supply / dummy-load pair)*:

1. **Engine OFF for all pre-M6 hardware work.** Engine off ⇒ no generation and no load-dump path; the 48 V bank powers the unit exactly as in normal standby. The first engine-on minute is M6 entry, supervised (rule 5).
2. **5 A field-circuit breaker verified in circuit before any custom firmware energises the field.** Independent backstop for a stuck-high driver or a clamp-bypassing bug: 100 % duty ≈ 12–13 A ≈ 240–270 % of rating → a thermal breaker trips in seconds-to-tens-of-seconds, which the rotor's thermal mass rides out. It does **not** catch marginal overdrive (3–5 A sits under the trip curve) — that band belongs to the compiled cap (rule 4) and the 25 % production clamp, which is why the breaker supplements and never replaces them. `bench-pending`: sight the breaker's DC rating at the first session (it interrupts an inductive DC circuit).
3. **Fail-safe defaults** — firmware ships field-OFF; watchdog on; the **software** fault cutoff (MOE-clear — BKIN is unrouted, PROJECT_PLAN §0.6 V1/V2) verified in M2.
4. **Duty-cycle cap compiled into test builds (20 %)** — on this 48 V bank that is ≈2.4–2.8 A average into the 4 Ω rotor: under the ~3 A winding rating and under the 25 % clamp the stock unit itself runs. The cap is the *primary* rotor protection; the breaker (rule 2) is the backstop.
5. **Never spin the alternator without a battery connected** (load dump) — automatically satisfied in place (the bank is hardwired); becomes a live check again if the unit is ever bench-run. Real-power work starts only in M6: in-place engine-off proofs → first supervised engine run (CV-hold record, PROJECT_PLAN §2 M3/M6) → supervised charge runs.
6. **Recovery always one step away** — SWD permanently wired; stock restore rehearsed in place (M1).
7. **Exactly one WS500 exists — irreplaceable.** M1 (backup + rehearsed restore) is absolute; any test that could plausibly *damage* (not just brick) hardware gets a Renode dry run first.

---

## Installed-Unit Reality (2026-07)

The one WS500 is **installed and in service on a 48 V system with a 4 Ω (12 V-class) rotor** — it is not a spare bench unit, and any firmware mishap also takes down a live charging system. Two consequences:

### Rotor Overdrive: The #1 Hazard

A 12 V rotor on a ~48–57.6 V bus means sustained field duty above **≈25 % (≈21 % at 57.6 V absorption)** overdrives the rotor (100 % duty ≈ 12 A / ~580 W into a 3 A winding). The CONTROL_SPEC rotor duty clamp and the never-100%-duty bootstrap cap (PROJECT_PLAN §0.5) are *the* critical protections here, and both must be proven in virtual (PROJECT_PLAN §8) before any custom firmware runs on this unit.

**★ Confirmed on hardware (2026-07-24):** a live stock charge run (serial `AST` capture) showed the WS500 **pinning Field% at exactly 25 %** — holding there while delivering 140 A / 7.6 kW at ~2330 RPM, and ramping to 25 % in ~2 %/s steps then stopping. The unit is **field-clamp-limited, not voltage-limited** (BatV reached only ~54.7 V vs the 56 V target). So the 25 % rotor clamp our design specifies is exactly what the product does — our firmware must reproduce it. Reference trace: `ws500_chargerun.log` (session capture).

### In-Place Testing Decision (Owner, 2026-07-29)

The unit is hardwired into the installed system and pulling it for bench work is impractical; a **5 A breaker protects the field circuit**. All pre-M6 hardware stages — M1 DFU rehearsal, M2 test-fw, M3 engine-off proofs — therefore run **on the installed unit, in place, engine off**, over the same USB connection Stage A already uses. What each retired bench precaution maps to:

- **Current-limited supply** → the bank in standby (engine off ⇒ no generation; the unit draws what it always draws).
- **Dummy field load** → the real rotor behind the 20 % compiled cap (≈2.4–2.8 A, inside the winding rating) with the 5 A breaker as the stuck-high backstop.
- **"Unit removed to the bench"** → unnecessary — DFU and USB do not care where the unit is mounted.

Two consequences (folded into PROJECT_PLAN §2): the M3 closed-loop CV-hold record cannot exist engine-off and moves to M6 entry, and the M2 GPIO walk now drives real harness outputs — identify what each output wire feeds before toggling it.

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

- First flash happens **in place, engine off, 5 A field breaker verified** (2026-07-29 decision above — the bench-removal requirement is retired); never with the engine running.
