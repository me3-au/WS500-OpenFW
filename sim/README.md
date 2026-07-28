# `sim/` — §8.1 SIL harness (the pre-hardware test gauntlet)

Software-in-the-loop simulation of the **pure control core** (`control/`)
against a physics plant model of the **actual installed system**: a 16S / 48 V
LFP bank charged by an alternator with a **4 Ω, 12 V-class rotor** — the exact
configuration where rotor overdrive is the #1 hazard
([PROJECT_PLAN §5](../docs/PROJECT_PLAN.md)).

This is a **regression suite, not a demo**: every scenario asserts invariants,
runs deterministically (fixed RNG seeds, no wall clock), and the whole gauntlet
runs in CI on every push.

## Gate role (PROJECT_PLAN §5 / §8)

**Stage C — flashing custom firmware on the one installed WS500 — requires this
suite green in CI** (together with 8.2 Renode, 8.3 binary verification, and 8.4
property tests). The unit is live on a 48 V system; the rotor duty clamp
(≈25 % at 48 V, ≈21 % at 57.6 V) must be *proven* here before any custom build
touches hardware.

## Build & run

Native, no HAL, pure C11 — same recipe as the control unit tests:

```sh
# CI / Linux / macOS
gcc -std=c11 -O2 -Wall -Wextra -I control/Inc -I sim \
    control/Src/*.c sim/*.c -lm -o sil_tests
./sil_tests

# Windows host without gcc (zig compiler wheel: pip install ziglang)
python -m ziglang cc -std=c11 -O2 -Wall -Wextra -I control/Inc -I sim \
    control/Src/*.c sim/*.c -o sil_tests.exe
./sil_tests.exe
```

Exit code 0 = all scenarios green. Accelerated time is plain loop iterations at
the core's designed 10 ms tick (`Core/Src/main.c` `LOOP_PERIOD_MS`); the
long-soak covers 5,000,000 ticks (~14 h simulated) in seconds of wall time.

## Files

| File | Role |
| --- | --- |
| `plant.h` / `plant.c` | Plant model: LFP battery (OCV/SOC knee curve, internal R, coulomb counting, thermal mass), alternator (saturating EMF vs RPM × field current, pulley ratio, rectifier limit, self-heating), rotor/field circuit (averaged `I_f = duty × V_bus / R`), driver-stage FET heating, and a sensor layer with deterministic noise / dropout (NaN) / implausible-value injection per channel |
| `sil.h` / `sil.c` | Closed-loop runner mirroring `Core/Src/main.c`: sense → assemble `ctrl_measured_t` → hardware limits + thermal governor ceilings → `ctrl_tick()` @10 ms → apply duty back to the plant. Checks the standing invariants **every tick** |
| `scenarios.c` | The scenario suite (below) |
| `sil_main.c` | Runner `main()`; reuses `control/test/test.h` CHECK macros |

## Calibration — pinned to the real unit

Anchored to the **2026-07-24 stock-firmware charge run** on the installed unit
(`ws500_chargerun.log`): field pinned at exactly **25 %**, **~140 A / ~7.6 kW**
at **~2330 RPM**, battery **~54.7 V** while clamp-limited (56 V target not
reached), FET temp 31→45 °C.

Model: `E = k_e · tanh(I_f / 3.0 A) · rpm_alt`, output
`I = (E − OCV) / (R_stator + R_int)`, with `pulley = 2.5`,
`R_stator = 40 mΩ`, `R_int = 12 mΩ`, `k_e = 0.012713`. At the trace point
(duty 0.25 → `I_f = 0.25 × 54.7 / 4 Ω = 3.42 A`, alt 5825 RPM) this reproduces
140 A into 54.7 V self-consistently; `scn_plant_calibration` asserts it. At
100 % duty the model gives 12–14 A field ≈ 580+ W into the ~3 A winding — the
overdrive hazard the clamp exists to prevent.

Note the deliberate difference from stock: stock pins a **static 25 %**; our
core computes the **dynamic** clamp `12 V / V_supply` (§5.1), so at 54.7 V it
runs ~21.9 % (`I_f` = exactly 3.0 A) and delivers ~66 A at 2330 RPM. The
*shape* matches stock — clamp-limited, CV target unreached at that RPM — which
`scn_reference_trace` asserts.

## Standing invariants (checked every tick of every scenario)

- **Rotor clamp (#1):** `field_duty ≤ rotor_v / V_supply(measured)` — ≈25 % at
  48 V, ≈21 % at 57.6 V — plus an independent physical check
  `duty × V_bus(true) / 4 Ω ≤ 3 A × 1.10`. Never exceeded under any transient.
- Outputs finite and in range (`duty`/`effort` ∈ [0,1] — a NaN duty on
  hardware is an undefined PWM compare).
- `field_open` ⇒ duty 0; no usable supply reading ⇒ duty 0.
- State / standby-reason / binding enums always valid; plant state finite.

## Scenarios

| Scenario | What it proves |
| --- | --- |
| `plant_calibration` | Plant reproduces the stock reference trace (140 A / 54.7 V / 3.42 A field @ 25 % / 2330 RPM); 100 % duty = 12–14 A / ~580+ W overdrive |
| `charge_rest_cycle` | Full 16S/48 V cycle: BULK ramp → CV reached → charged exit → FLOAT hold → deep-discharge T3 revert → BULK again; no overvoltage ever |
| `rotor_clamp` | **#1 safety property** under abuse: RPM square waves, BMS ceiling toggles, load dumps, sensor noise, high-SOC 57 V phase — duty never exceeds the clamp, rotor current ≤ ~3 A; plus an `allow_full_field_48v` sanity phase proving the check isn't vacuous |
| `bms_ceiling_steps` | Arbitration follows CCL steps: down-steps immediate with no overshoot and no limit-cycle chatter, binding telemetered as BMS, up-steps bounded by `ramp_w_per_s`, CCL = 0 kills the field within 1 s |
| `rpm_transients` | Idle↔cruise steps and stall-to-zero (RPM sensor LOST): output follows physics, no spurious faults, recovery on restart |
| `temperature` | Li charge-window gates (−5 °C block / 60 °C abort, both resume on recovery); predictive thermal governor holds the alternator near the 95 °C target in a 45 °C engine room; raw 125 °C hard limit → fault + power pulled to the derate floor |
| `sensor_faults` | VBat dropout → LIMP + duty exactly 0 (not NaN) + recovery; shunt dropout (NaN watts) never reaches the PWM and un-latches; implausible +2000 A → Limp Home (FLOAT @ `v_limp`, field *not* open); injected overvoltage → CRITICAL **latches**, field stays open after the reading recovers; 3 % noise storm stays safe and charging |
| `reference_trace` | Stock-trace shape at 2330 RPM mid-charge: railed at the rotor clamp (binding = `ROTOR_CLAMP`), `duty = 12/V_bus`, `I_f = 3.0 A`, CV target unreached — clamp-limited exactly like the stock unit; soft-start watts obey the `ramp_w_per_s` envelope |
| `stalled_rotor` | **§5.2 run-detect gate (GH#37):** engine dies mid-charge, bus live, ignition on — field falls to the pulse-cycled detect budget (≤5 % of `duty_max`, ≤10 % on-time) within 1 s and holds it for a 2 h key-on-engine-off soak: `I_f` ≤ 0.25 A, rotor dissipation ≤ 1 W, rotor stays at ambient; charging soft-ramps back when the engine restarts |
| `lying_vbat` | **§5.1 clamp-supply plausibility (GH#37):** in-range false-LOW VBat (49 V claimed, ~54.5 V true) — the duty clamp NEVER loosens beyond the true-voltage clamp (checked every tick), rotor current stays ≤ rated, distrust telemetered as a WARN and cleared on recovery; false-HIGH tightens immediately (safe direction) |
| `ah_revert` | **T3 Ah integrator (GH#37):** zero-rest profile, bank resting on the flat LFP plateau while 2 kW of house loads discharge it — CHARGE returns via the net-Ah path (15 Ah ≈ 24 min at ~38 A) with voltage never near `v_revert`; integrator resets at the next charged exit; tier-3 (alt-side shunt) control run proves the path stays disarmed without battery truth |
| `long_soak` | 5,000,000 ticks (~14 h) of engine/ignition/load cycling with noise: 21 BULK / 21 FLOAT / 21 STANDBY entries, no state-machine leaks, timers sane, outputs finite, rotor never overdriven |

## Control-core defects found by this harness (fixed, flagged `[SIL-found 2026-07]`)

1. **NaN duty on lost VBat sense** — `ctrl_duty_max()` used `v_supply <= 0` as
   its fail-safe test, which is false for NaN → `duty = NaN` reached the PWM
   command while in LIMP. Fixed with negated-positive checks (`field.c`).
2. **NaN effort latch on shunt dropout** — `watts_batt = NaN` propagated
   through the power loop into the effort integrator and **stuck permanently**
   (NaN passes both range clamps). Fixed with an `isfinite` guard
   (`control.c`).
3. **Power-loop instability** — the inner-loop gains were per-tick (tick-rate
   dependent) and the power gain was marginally unstable against the calibrated
   plant (loop gain ≈ 2 at cruise RPM → chatter and ceiling overshoot in the
   BMS-step scenario). Gains are now dt-scaled (per-second) and KP reduced 5×
   (`control.c`).
4. **CV exits unreachable under noise** — the "voltage-clamped" qualifier was a
   knife-edge `v ≥ cv_target`; the CV loop regulates to the target *from
   below*, so with any sensor noise the T2 hold timers (tail, `cv_hold_exit`,
   Solar-Finish) reset forever and BULK could only exit via the 8 h backstop.
   Fixed with a 0.01 V/cell tolerance band (`control.c`).

Each fix carries a matching unit test in `control/test/` (cases 13/14 in
`test_statemachine.c`, NaN cases in `test_field.c`).

## Formerly-known gaps — now fixed in the core (GH#37, 2026-07)

The three gaps this harness originally documented-but-did-not-assert are now
implemented in the pure core and asserted by the scenarios above:

1. **§5.2 run-detect / stationary-rotor budget** — `ctrl_run_detected()`
   (`field.c`) + the detect-budget gate in `ctrl_tick()`: no rotation evidence
   (VALID fused RPM > 0 or the app's probe-driven `run_state`) → effort held to
   a pulse-cycled ≤5 %-of-`duty_max` budget and commanded power reset so the
   soft ramp governs the resume. Asserted by `scn_stalled_rotor`.
2. **T3 Ah-revert** — `ah_since_charged` integrates net battery Ah during
   FLOAT / STANDBY-rest (battery-truth tiers only), resets at every charged
   exit, and arms the T3 Ah path in `revert_met()`. Asserted by `scn_ah_revert`.
3. **Clamp-supply plausibility** — `ctrl_vsup_guard()` (`field.c`) vets the
   voltage `duty_max` is computed from: rises follow instantly (tighter),
   implausible drops (> 4 % below the trusted level) hold the tighter
   last-trusted voltage and raise `CTRL_FAULT_VSUP_IMPLAUSIBLE` (WARN), and
   out-of-band lows fall back to the worst case (highest plausible bus =
   tightest clamp). Asserted by `scn_lying_vbat`.

The detect-budget and clamp-guard constants behind fixes 1 and 3 were
`[SPEC-SIGNOFF]` placeholders in `control/Inc/field.h`; they are signed off as
of 2026-07-28 (GH#34), derivations recorded in `docs/CONTROL_SPEC_NEXTGEN.md`
§5.1.1 (guard) and §5.2 (detect budget). The inner-loop gains this harness
retuned are now specified — with claimed margins and their stated limits — in
CONTROL_SPEC §5.3.

## Remaining known limitations (documented, not asserted)

- **A consistent false-low from boot is undetectable** with a single physical
  voltage source: the guard's cross-check and step detection need either a
  trusted history or a second channel. A sustained (> 5 min) stable lower
  reading is eventually re-trusted (WARN visible the whole time) — otherwise a
  genuine bus re-level would over-tighten forever.
- The soft-start ramps **watts** (per spec), not duty: duty crosses the
  sub-excitation dead zone (EMF < bus voltage, zero current possible) quickly,
  then tracks the power ramp — unlike stock's fixed ~2 %/s duty ramp.
