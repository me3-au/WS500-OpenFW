# LFP Charge Profile — Technical Specification (Draft 1, for review)

Companion to [CONTROL_SPEC_NEXTGEN.md](CONTROL_SPEC_NEXTGEN.md) (Draft B). That
document defines the product architecture; this one specifies the charge-profile
subsystem to implementation depth: state machine, transition conditions,
parameters, per-profile definitions, and the config schema.

Conventions used throughout:

- All voltages are **V/cell** (`v_cell`). Pack voltage = `v_cell × cells_series`
  (4 / 8 / 16 for 12 / 24 / 48 V systems).
- `V_comp` = IR-compensated battery-surface voltage (control spec §6.6). Where no
  resistance model has converged yet, `V_comp = V_meas` and control behaves
  identically to an uncompensated regulator.
- All powers are **Watts at real system voltage**.
- Damped signals are written `~X` (e.g. `~V_comp` = filtered per §5). Safety
  comparators use raw signals only.
- `C` = configured bank capacity in Ah (`bank_capacity_ah`).

---

## 1. Voltage primitives

Stored once, referenced by name from profiles. Editing a primitive propagates to
every profile that uses it.

| ID | Name | Default (V/cell) | Range (V/cell) | 12 V | 24 V | 48 V |
| --- | --- | --- | --- | --- | --- | --- |
| `v_bulk` | Bulk | 3.60 | 3.45 – 3.65 | 14.40 | 28.80 | 57.60 |
| `v_bulk_cons` | Bulk Conserve | 3.50 | 3.40 – 3.58 | 14.00 | 28.00 | 56.00 |
| `v_float` | Float | 3.40 | 3.33 – 3.45 | 13.60 | 27.20 | 54.40 |
| `v_float_cons` | Float Conserve | 3.35 | 3.28 – 3.40 | 13.40 | 26.80 | 53.60 |
| `v_limp` | Limp | 3.30 | 3.20 – 3.35 | 13.20 | 26.40 | 52.80 |
| `v_fast` | Fast *(future)* | — curve-based — | — | — | — | — |

Cross-primitive guard rails (enforced at config validation, not silently
clamped):

```
v_bulk      >  v_bulk_cons  >  v_float          (each by ≥ 0.03 V/cell)
v_float     >  v_float_cons >  v_limp           (each by ≥ 0.02 V/cell)
v_limp      ≥  3.20 V/cell                      (hard floor)
```

A config that violates an ordering is rejected with a specific error naming both
primitives — never auto-corrected.

---

## 2. State machine

Three states. Everything else the previous draft modeled as a state is either a
**binding ceiling** (visible in telemetry, §2.2) or a **standby reason**.

```
              enable + warmup + (first run | revert)
   ┌─────────┐ ──────────────────────────────▶ ┌────────┐
   │ STANDBY │                                 │  BULK  │
   │         │ ◀────────────────────────────── │        │
   └─────────┘   charged (zero-rest profiles,  └───┬────┘
        ▲        Solar Finish early exit)          │ charged
        │                                          ▼ (hold-rest profiles)
        │        revert (V / SOC / Ah)         ┌────────┐
        └───────────────────────────────────── │ FLOAT  │
                 (also Standby-rest → Bulk)    └────────┘

   Ignition off / unrecoverable fault → STANDBY, from any state.
```

### 2.1 States

| State | Control law | Notes |
| --- | --- | --- |
| `STANDBY` | Field open | One state, annotated with a reason: `off` (ignition), `warmup` (gate pending — `warmup_time_s` or coolant ≥ `warmup_coolant_c`), `rest` (charged, zero-rest profiles; micro-rest OCV sampling is free here), `fault` (latched, per control spec §7). Telemetry and logging stay alive. Tach stays alive (synthesized) |
| `BULK` | **Maximum field, subject to the arbitration min()** | One control law, no sub-states. The ceilings — thermal governor, current/power limit (user `max_charge_power_w`, BMS CCL), **voltage clamp at `cv_target`**, engine budget — are all active simultaneously; whichever binds is telemetered: *Bulk (Temp)*, *Bulk (Current Limit)*, *Bulk (Voltage)*, *Bulk (Engine)*. Classic "absorption" is simply Bulk (Voltage). Ramp-in at `ramp_w_per_s` on entry. R-learning pulses permitted while **not** voltage-clamped; suppressed once voltage-clamped so the tail measurement is clean |
| `FLOAT` | CV at the profile float voltage, power ≤ `rest_power_cap_w` | Same CV mechanism regardless of voltage: Float Norm (`v_float`), Float Low (`v_float_cons`), **Limp Home = Float at `v_limp` + its power cap**. No separate limp state |

### 2.2 Transitions

All conditions are evaluated on damped signals (§5) and must hold
**continuously** for their hold time. First-match-wins.

| # | From → To | Condition |
| --- | --- | --- |
| T1 | STANDBY → BULK | Enable active, warmup gate satisfied, and (first activation OR a revert condition of T3 is met) |
| T2 | BULK → FLOAT *(hold-rest profiles)* or → STANDBY-rest *(zero-rest profiles)* | **Charged**, any of: (a) tail — `~P_bat ≤ p_tail_w` for `t_tail_hold_s` **while voltage-clamped**, tiers 1–2 only (§4.2); (b) SOC — `SOC ≥ soc_target_pct`, trusted SOC only (§4.3); (c) backstop — time in BULK ≥ `t_charge_max_min` → WARN `CHG_TIMEOUT`; (d) **Solar Finish** — profile flag `exit_at_cv_entry`: voltage clamp binding for `t_vclamp_s` → exit at clamp onset, no tail wait |
| T3 | FLOAT → BULK, STANDBY-rest → BULK | **Revert**, any of: `~V_comp ≤ v_revert` for `t_revert_hold_s`; `SOC ≤ soc_revert_pct` (trusted only); net Ah discharged since charged ≥ `ah_revert` (tiers 1–2 only) |
| T4 | any → STANDBY | Ignition off (reason `off`; warmup re-gated only if off > `warmup_reset_min`, default 30 min) or unrecoverable fault (reason `fault`, latched). Degraded-but-recoverable faults instead select the Limp Home profile → FLOAT at `v_limp` (control spec §7 ladder) |

**T2 primary exit — voltage + time (added, supersedes tail as the default).** The
reliable charged-exit is **held at `cv_target` continuously for `cv_hold_exit_min`
(default 15 min) → charged.** For LFP, ~10–15 min at 3.6 V/cell is full in practice. This
needs **no current truth** — it works at any shunt tier and any placement, so it is the
robust default. The tail-power exit (T2a) is **retained as an optional faster exit** when
battery-truth current is available (tiers 1–2); it is no longer required. Rationale: tail
current is "optimal" but placement-sensitive and fussy; voltage + time is adequate and
universal. (A smarter tail/SOC-blended exit can come later.)

**T1 skip-BULK-if-full (added, optional).** At first activation (power-up), if the battery
is already full the regulator goes **straight to REST**, not BULK — avoiding a needless
re-absorb on every power cycle (common with a bank floating on solar). Two independent,
off-by-default triggers, evaluated once on the resting voltage before field is applied:
`skip_bulk_vcell` (start in REST if resting V/cell ≥ this — set to the solar-float voltage)
and `skip_bulk_soc_pct` (start in REST if trusted SOC ≥ this). If the profile is zero-rest,
"REST" is STANDBY-rest; otherwise FLOAT. Normal REST→BULK reverts (T3) are unaffected.

Anti-flap guarantee: T2 and T3 hold times plus §5 damping make BULK↔FLOAT
oscillation impossible faster than `t_tail_hold_s + t_revert_hold_s`
(worst case minutes, not seconds).

---

## 3. Parameters

### 3.1 Global (shared by all profiles)

| Parameter | Unit | Default | Range | Notes |
| --- | --- | --- | --- | --- |
| `cells_series` | — | auto (4/8/16) | 4–16 | From system-voltage auto-detect, one-time confirm |
| `bank_capacity_ah` | Ah | — (required) | 40–3000 | Drives C-relative defaults below |
| `max_charge_power_w` | W | — (required) | 100–20000 | The headline knob; top of the arbitration min() |
| `ramp_w_per_s` | W/s | 100 | 10–1000 | Soft-start + belt protection |
| `t_vclamp_s` | s | 5 | 2–30 | Voltage-clamp-binding qualifier (Bulk (Voltage) annotation + T2d) |
| `t_charge_max_min` | min | 480 | 60–1440 | T2c backstop |
| `warmup_time_s` | s | 30 | 0–600 | 0 = disabled |
| `warmup_coolant_c` | °C | unset | 20–90 | CAN coolant gate, optional |
| `soc_target_pct` | % | unset | 70–100 | §4.3; unset = disabled |
| `topbalance_days` | days | 10 | 1–60, unset | With `soc_target_pct` set: every N days ignore the SOC exit (T2b) and charge to the tail exit (T2a) |

### 3.2 Tail exit

`p_tail_w` defaults to a **C-relative value computed at config time**, then
stored as explicit Watts (visible and editable — no hidden math at runtime):

```
p_tail_w(default) = 0.04 × bank_capacity_ah × cells_series × 3.45
```

(≈ 0.04 C tail current at CV voltage; e.g. 400 Ah / 12 V → ≈ 220 W.)

| Parameter | Unit | Default | Range |
| --- | --- | --- | --- |
| `p_tail_w` | W | 0.04 C equiv (above) | 0.01 C – 0.10 C equiv |
| `t_tail_hold_s` | s | 60 | 10–600 |

Tier rules (§4.2): tail exit requires battery-truth current (tier 1–2). At tier
3–5 the tail exit is **disarmed** and T2c (duration backstop) plus T2b (if SOC
trusted) carry the exit, with a one-time INFO telling the user why.

### 3.3 Per-profile parameters

| Parameter | Unit | Notes |
| --- | --- | --- |
| `cv_target` | primitive ref | CHARGE CV voltage |
| `exit_at_cv_entry` | bool | Solar Finish flag (T2d) |
| `rest_mode` | `hold` / `zero` | |
| `rest_voltage` | primitive ref | `hold` only |
| `rest_power_cap_w` | W or %max | `hold` only; cap on REST output |
| `v_revert` | V/cell | T3 voltage revert threshold |
| `t_revert_hold_s` | s | T3 qualifier |
| `soc_revert_pct` | % | T3, trusted SOC only |
| `ah_revert` | Ah | T3, tier 1–2 only; default 0.30 × C |

---

## 4. Profile definitions

### 4.1 The seven profiles

Defaults below; every cell is user-editable within §3 ranges. "—" = exit/revert
path disabled for that profile.

| # | Profile | `cv_target` | `exit_at_cv_entry` | `rest_mode` | `rest_voltage` | `rest_power_cap_w` | `v_revert` | Notes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- |
| 1 | Bulk, Float Norm | `v_bulk` | no | hold | `v_float` | 100 % max | 3.28 | Default profile. Full power available at float — engine carries house loads |
| 2 | Bulk, Float Low *(Solar Priority)* | `v_bulk` | no | hold | `v_float_cons` | 25 % max | 3.22 | Rest sits below typical solar float (≈3.375–3.40) → solar wins finish + loads; low cap and low revert keep the alternator a backstop, not a competitor |
| 3 | Bulk, Solar Finish | `v_bulk` | **yes** | zero | — | — | 3.25 | Alternator does constant-power lifting only; CV/absorption is solar's job. T2d replaces T2a |
| 4 | Bulk Conservative, Float Low | `v_bulk_cons` | no | hold | `v_float_cons` | 50 % max | 3.25 | Max cycle life; liveaboard daily driver |
| 5 | Bulk, Off | `v_bulk` | no | zero | — | — | 3.25 | Charge-then-stop. Battery genuinely rests; micro-rest OCV anchoring is free here |
| 6 | Limp Home | `v_limp` (hold only) | — | hold | `v_limp` | 25 % max | — | Enters FLOAT at `v_limp` directly — no Bulk, no exits. Manual select **and** automatic degraded-mode target |
| 7 | Fast Charge *(future)* | `v_fast` curve | — | zero | — | — | 3.25 | Curve-based (V/W vs SOC). Profile ID, schema slot, UI slot reserved now |

All rest reverts additionally carry the profile-independent SOC/Ah paths of T3
where the tier permits them (`soc_revert_pct` default 50 %, `ah_revert` default
0.30 C).

### 4.2 Current-source tiers (binding for this subsystem)

v1 hardware has **one** local shunt (INA22x/238 on I²C), battery- or
alternator-side per config — tiers 1 and 3 are the same physical shunt in its
two placements, never simultaneous.

| Tier | Source | Tail exit (T2a) | Ah revert (T3) | R-learning pulses |
| --- | --- | --- | --- | --- |
| 1 | Local shunt, battery-side | ✔ | ✔ | ✔ |
| 2 | NMEA battery current | ✔ (latency ≤ 2 s required) | ✔ | ✔ |
| 3 | Local shunt, alternator-side | ✘ (alt ≠ battery current) | ✘ | ✔ |
| 4 | Estimated alt current (from field duty + RPM; no field-current sense on v1) | ✘ | ✘ | ✔ (coarse) |
| 5 | None | ✘ | ✘ | ✘ (micro-rest OCV only) |

The active tier is telemetered; every disarmed exit is reported once as INFO at
profile activation, not silently.

### 4.3 Trusted SOC

`SOC` is trusted for T2b/T3 when sourced from: (a) a BMS integration (control
spec §8.1), or (b) on-device coulomb counting at tier 1–2 **with** an OCV anchor
(micro-rest, §6.6 of control spec) within the last `soc_anchor_max_age_h`
(default 48 h). Untrusted SOC never gates a transition; it may still be
displayed (labeled "estimate").

---

## 5. Damping / averaging map

Per control spec §1.4, restated here with the transitions they guard:

| Signal | Default τ | Range | Guards |
| --- | --- | --- | --- |
| `~V_comp` (control) | 1 s | 0.2–5 s | CV loops (clamp + Float), T2d |
| `~P_bat` (tail) | 60 s | 10–600 s | T2a |
| `~V_comp` (revert) | 30 s | 5–300 s | T3 voltage path |
| SOC (revert/target) | event-driven | — | T2b, T3 SOC path |
| Ah integrator | continuous | — | T3 Ah path |

Raw-signal rule: overvoltage kill, overspeed, hard temp limit never read damped
values (control spec §1.4).

---

## 6. Interactions with the rest of the system

- **Arbitration (control spec §2).** The profile contributes exactly one
  ceiling: `max_charge_power_w` in BULK, `rest_power_cap_w` in FLOAT
  (Limp Home's cap included — it is just a FLOAT config), 0 in STANDBY.
  Everything else (thermal, engine, BMS, the Bulk voltage clamp) stacks in the
  same min() — profiles never encode thermal or engine behavior.
- **BMS ceilings (§6.3/§8.1).** CVL below the profile CV target simply wins in
  the min(); the profile does not fight it. A BMS pre-disconnect warning
  triggers soft ramp regardless of state.
- **R/OCV learning (§6.6).** Pulse probes only in BULK while not
  voltage-clamped; micro-rests only in STANDBY (reasons `rest`/`off`).
  Voltage-clamped Bulk is measurement-quiet so the tail decision is clean.
- **Thermal governor (§4.1).** Orthogonal by construction — it is another
  ceiling. Profile logic never reads temperature except the battery
  charge-window gate (low-temp Li cutoff = hard fault, not a profile concern).
- **Telemetry.** Broadcast at 1 Hz: active profile #, state, `V_comp`, active
  ceiling source, tier, disarmed-exit flags, time-in-state. GX display shows
  profile name + state verbatim (§8.1).

---

## 7. Config schema sketch (machine-readable)

```jsonc
{
  "primitives": {                    // V/cell, validated per §1 orderings
    "v_bulk": 3.60,
    "v_bulk_cons": 3.50,
    "v_float": 3.40,
    "v_float_cons": 3.35,
    "v_limp": 3.30
  },
  "limits": {                        // hardware limit set, control spec §2.1 — native units
    "battery_c_limit": 0.5,          // C-rate
    "wiring_limit_a": 250,           // total A, charge path
    "alternator_limit_a": 220,       // absolute rectifier/stator A
    "belt": { "a_ref": 150, "rpm_ref": 4000 },  // or {"torque_nm": ...}; slip-adaptive, §2.2
    "warmup": { "coolant_c": 60, "cold_power_pct": 40, "cold_c": 20 },  // §2.3, optional
    "field": { "rotor_rated_v": 12, "rotor_v_max": null, "allow_full_field_48v": false }  // §5.1 — v1 duty-clamp mode (WS500 hw has no rotor current sense)
  },
  "engine": {                        // white-space curves, control spec §3.5 — engine RPM, Watts
    "idle_rpm": 800,
    "max_rpm": 3800,
    "curve_full":    [ [800, 400], [1200, 1500], [1800, 2600], [2600, 3000] ],
    "curve_feature": [ [800, 200], [1200, 600], [2600, 1000] ]   // optional; Feature-IN selects
  },
  "global": {
    "cells_series": 8,               // confirmed once after auto-detect
    "bank_capacity_ah": 400,
    "max_charge_power_w": 3000,
    "ramp_w_per_s": 100,
    "p_tail_w": 440,                 // materialized 0.04C default; editable
    "t_tail_hold_s": 60,
    "t_charge_max_min": 480,
    "soc_target_pct": null,
    "topbalance_days": null,
    "warmup_time_s": 30
  },
  "profiles": [
    { "id": 1, "name": "Bulk, Float Norm",
      "cv_target": "v_bulk", "exit_at_cv_entry": false,
      "rest": { "mode": "hold", "voltage": "v_float", "power_cap_pct": 100 },
      "revert": { "v_cell": 3.28, "hold_s": 30, "soc_pct": 50, "ah_frac_c": 0.30 } },
    { "id": 2, "name": "Bulk, Float Low (Solar Priority)",
      "cv_target": "v_bulk", "exit_at_cv_entry": false,
      "rest": { "mode": "hold", "voltage": "v_float_cons", "power_cap_pct": 25 },
      "revert": { "v_cell": 3.22, "hold_s": 60, "soc_pct": 40, "ah_frac_c": 0.30 } },
    { "id": 3, "name": "Bulk, Solar Finish",
      "cv_target": "v_bulk", "exit_at_cv_entry": true,
      "rest": { "mode": "zero" },
      "revert": { "v_cell": 3.25, "hold_s": 60, "soc_pct": 50, "ah_frac_c": 0.30 } },
    { "id": 4, "name": "Bulk Conservative, Float Low",
      "cv_target": "v_bulk_cons", "exit_at_cv_entry": false,
      "rest": { "mode": "hold", "voltage": "v_float_cons", "power_cap_pct": 50 },
      "revert": { "v_cell": 3.25, "hold_s": 30, "soc_pct": 50, "ah_frac_c": 0.30 } },
    { "id": 5, "name": "Bulk, Off",
      "cv_target": "v_bulk", "exit_at_cv_entry": false,
      "rest": { "mode": "zero" },
      "revert": { "v_cell": 3.25, "hold_s": 30, "soc_pct": 50, "ah_frac_c": 0.30 } },
    { "id": 6, "name": "Limp Home",
      "limp": { "voltage": "v_limp", "power_cap_pct": 25 } },
    { "id": 7, "name": "Fast Charge", "reserved": true }
  ],
  "active_profile": 1
}
```

Schema rules: unknown keys preserved on round-trip (future-proofing, same
philosophy as the WS500 Util); config changes logged as diffs; CRC over the
whole block.

---

## 8. Open questions for review

1. **Tail threshold representation.** Spec stores `p_tail_w` as materialized
   Watts (computed from 0.04 C at config time). Alternative: store the C-rate
   and recompute — survives bank-capacity edits but hides the number. Current
   choice favors explicitness.
2. **Profile 2 rest cap (25 %) and revert (3.22).** Both chosen to make the
   alternator a genuine backstop rather than a solar competitor. Too
   conservative for boats with small arrays?
3. **Profile 3 revert.** After a Solar-Finish exit at CV entry, revert at 3.25
   sustained means a cloudy day brings the alternator back — correct? Or should
   profile 3 refuse to re-CHARGE within the same engine run (one-shot
   semantics)?
4. **Profile 1 vs 4 rest caps (100 % vs 50 %).** Is a cap on FLOAT power
   even wanted for profile 1, or should "hold at float, full power" be the only
   Norm behavior?
5. **`v_revert` values generally** (3.22–3.28/cell band): these map to roughly
   20–60 % SoC resting on typical LFP OCV curves — sane, but they interact with
   loads (surface sag). The `t_revert_hold_s` damping is the guard; are the
   defaults long enough for inverter-heavy boats?
6. **WARMUP re-gating** (`warmup_reset_min` = 30): restart within 30 min skips
   warmup. Reasonable for lock/marina stop-start?
7. **Tier-2 latency bound** (NMEA current ≤ 2 s for tail exit): strict enough
   for a 60 s-damped decision? Could likely relax to 5 s.
8. **Belt limit entry unit** (control spec §2.2): torque in Nm is physically
   right but installer-hostile; `A_ref @ rpm_ref` is friendlier but assumes the
   installer knows a safe operating point. Ship both? And: is the 10 %
   immediate trim + slow decay the right slip-response shape, or should first
   slip freeze the ceiling at 90 % of the slip point until manual reset?
