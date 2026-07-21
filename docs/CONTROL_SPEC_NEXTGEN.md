# Next-Generation Alternator Regulator — Control Specification (Draft B)

Status: concept draft for review. Draft B supersedes Draft A: LiFePO4-first,
two-stage charging, one power-arbitration mechanism, RPM optional. The WS500's
legacy surface (Pb profile stack, 6-stage state machine, DIP switches, small-alt
mode, half mode, RFM/PBF/Feature-IN RPM conflicts) is deleted, not re-skinned.

**v1 target = existing WS500 hardware (§0.1).** Anything the board can't do is
marked ⟦future-hw⟧ and excluded from v1 scope.

---

## 0. Philosophy

**One chemistry philosophy.** Designed for LiFePO4 and similar flat-CV banks.
No equalize, no overcharge, no post-float, no per-cell temp-comp curves, no
flooded/AGM preset stack. (Pb users are served by the existing market.)

**Two stages.** CHARGE and REST. That is the whole state machine. User-facing
**named LFP profiles** (§1.3) are presets over this engine — they select voltage
primitives and rest behavior; they do not add states.

**Per-cell canonical voltages.** Every voltage setting is stored and specified
in **V/cell**; pack values derive from series cell count (12 V = 4S, 24 V = 8S,
48 V = 16S). One number is correct at every system voltage — this replaces the
WS500's 12 V file normalization with the one normalization that is actually
chemistry-native.

**One control unit.** Watts, at real system voltage. No 12 V normalization, no
500 Ah normalization, no percent-of-something derates.

**One arbitration mechanism.** Output = min() of active power ceilings. Small-alt
mode, half mode, and the RFM tables were all crude hand-built ceilings — they are
replaced by this one mechanism, not carried forward as modes.

**RPM is optional.** The regulator is fully functional with no RPM source: flat
power cap + thermal loop. RPM, when present, unlocks refinements (capability
curve, engine budget, overspeed, belt alarm) — it is never a dependency.

**No DIP switches.** App/file-configured named profiles. Nothing mechanical to
mirror or conflict with (`CP_Index`/`BC_Index` and their override fields are gone).

### 0.1 Target hardware — v1 is the WS500 platform

v1 runs on the existing WS500 board. Recovered inventory (see
[WS500_HARDWARE_SPEC.md](WS500_HARDWARE_SPEC.md)): STM32F072; field PWM on TIM1 with
BKIN hardware fault trip; stator capture on TIM2; **one** CAN bus (bxCAN); USB
CDC config port (ST system DFU bootloader in ROM); current + local bus voltage
via a TI INA226/228/238 on I²C at the **single** external shunt (battery- or
alternator-side per config); ADC: one external alternator NTC, one
regulator-internal **driver-stage NTC**, one battery NTC, VBat Kelvin-sense
divider; digital: 1× Ignition, 1× Feature-In, 1× Lamp/Feature-Out; status LED.

Everything in this spec is written to run on that inventory. Features needing
hardware the platform lacks are **excluded from v1** and marked ⟦future-hw⟧
where they appear:

| Excluded (no hardware) | v1 consequence |
| --- | --- |
| Rotor/field current metering | §5.1 runs in duty-clamp + driver-temp mode; no rotor R learning / temp observer |
| Second local shunt channel | Single-shunt placement rules (§6.2) |
| Dedicated crank-pulse RPM input | RPM sources = CAN + stator only (§3.1) |
| EGT input | EGT ceiling only if EGT arrives on CAN |
| Second CAN bus | All dialects + regulator sync share the one bus (§8) |
| BLE / Wi-Fi | Config, logs, firmware over USB CDC/DFU and CAN (§6.7) |
| Dedicated tach output pin | Synthesized tach is a Feature-Out **function** — competes with lamp/fan for the single output (§3.2) |

---

## 1. Charge model

### 1.1 Voltage primitives (V/cell, canonical)

All profile behavior is built from these named primitives. Pack voltage =
primitive × series cell count (4S / 8S / 16S for 12 / 24 / 48 V).

| Primitive | V/cell | 12 V (4S) | 24 V (8S) | 48 V (16S) | Role |
| --- | --- | --- | --- | --- | --- |
| Bulk | 3.60 | 14.40 | 28.80 | 57.60 | Full-charge CV target |
| Bulk Conserve | 3.50 | 14.00 | 28.00 | 56.00 | Longevity CV target |
| Float | 3.40 | 13.60 | 27.20 | 54.40 | Normal hold |
| Float Conserve | 3.35 | 13.40 | 26.80 | 53.60 | Low hold — sits below typical solar float so solar wins |
| Limp | 3.30 | 13.20 | 26.40 | 52.80 | Minimal-stress hold, safe mode |
| Fast | *(future)* | — | — | — | Curve-based (§1.3 profile 7) |

Primitives are user-adjustable within guard-railed ranges (e.g. Bulk
3.45–3.65 V/cell); profiles reference primitives by name, so one edit
propagates everywhere.

### 1.2 Stages (the engine)

**CHARGE** — constant-power ramp to the profile's CV target, then CV hold.
Exits on: tail power (W, measured at the **battery** — §6.2), SOC target
(optional, §6.1), max duration (backstop), or the profile's early-exit rule
(§1.3 "Solar Finish").

**REST** — per profile: `hold <primitive>` (CV at reduced voltage, small power
cap) or `zero` (true zero field; tach keeps working, §3). Reverts to CHARGE on
voltage / SOC / net-Ah threshold — first match wins.

### 1.3 Named LFP profiles

| # | Profile | CHARGE target | CHARGE exit | REST | Intent |
| --- | --- | --- | --- | --- | --- |
| 1 | **Bulk, Float Norm** | Bulk | tail power | hold Float | Default. Engine carries house loads underway |
| 2 | **Bulk, Float Low** *(Solar Priority)* | Bulk | tail power | hold Float Conserve | Rest voltage sits below solar float → solar carries loads/finish; alternator backstops |
| 3 | **Bulk, Solar Finish** | Bulk | **early**: exit at CV entry (no CV hold) | zero | Alternator does the cheap constant-power lifting; solar does absorption. Motor-then-anchor pattern |
| 4 | **Bulk Conservative, Float Low** | Bulk Conserve | tail power | hold Float Conserve | Max cycle life; liveaboard daily driver |
| 5 | **Bulk, Off** | Bulk | tail power | zero | Charge-then-stop; battery genuinely rests |
| 6 | **Limp Home** | Limp | n/a — hold only | hold Limp | Safe mode: reduced power cap (default 25 % of max W), minimal bank stress. Selectable manually **and** the automatic fallback for critical sensor/BMS faults (§7) |
| 7 | **Fast Charge** *(future)* | curve | curve | zero | Curve-based (V/W vs SOC) fast charge. Reserved: profile ID, config schema slot, and UI slot allocated now; controller ships later |

Profiles are selectable from app, Feature-IN, or NMEA command; the active
profile is telemetered. All profiles share the same two-stage engine — a
profile is data, not code. Implementation-depth detail (state machine,
transitions, parameter tables, config schema) lives in
[PROFILE_SPEC_LFP.md](PROFILE_SPEC_LFP.md).

### 1.4 Averaging / damping (adjustable within ranges)

Every control decision runs on a filtered signal with a bounded, user-tunable
time constant — fast enough to be safe, slow enough not to chatter:

| Filtered signal | Default | Range | Consumer |
| --- | --- | --- | --- |
| VBat (control) | 1 s | 0.2–5 s | CV loop |
| Tail power (exit) | 60 s | 10–600 s | CHARGE exit — long enough to ignore load transients (windlass, inverter) |
| REST revert (V / SOC / Ah) | 30 s | 5–300 s | Re-charge trigger — no flapping between stages |
| Alt temperature | 5 s | 1–30 s | Thermal loop (§4) |
| RPM | 0.5 s | 0.1–2 s | Ceilings + tach output |
| Belt-slip ratio deviation | 2 s | 0.5–10 s | Slip alarm (§3.3) |

Safety comparators (overvoltage kill, overspeed, hard temp limit) always run on
**raw** signals — damping applies to control and exits, never to protection.

---

## 2. Power arbitration

Commanded output = **min()** of every active ceiling. Arbitration runs in
Watts; ceilings whose *native* unit is Amps or torque are converted at the
present measured voltage / RPM each cycle (the user configures them in their
native unit — nobody is asked to express wiring ampacity in Watts).

1. Stage limit (§1)
2. Thermal loop (§4)
3. BMS ceiling (§6.3)
4. Hardware limit set (§2.1): battery C, wiring A, alternator A, belt (§2.2)
5. Alternator capability @RPM — *optional, only if RPM present* (§3.4)
6. Engine budget — *optional*: a single "max engine Watts" number, or the
   white-space curves in Watts over [idle_rpm, max_rpm], with a Full and a
   Feature-selected variant (§3.5)
7. User manual cap / "quiet mode" input

The **binding ceiling is always telemetered and displayed** — the user can see at
a glance *why* output is what it is. This transparency is what lets us delete all
the special modes: there is nothing hidden to name.

### 2.1 Hardware limit set

Static ratings of the installed system, independent of profile and stage:

| Limit | Native unit | Converted | Notes |
| --- | --- | --- | --- |
| **Battery C limit** | C-rate | A = C × bank Ah → W @ VBat | Chemistry/bank ceiling independent of BMS (protects when BMS comms are absent or lying); typical LFP default 0.5 C |
| **Wiring limit** | A (total) | W @ VBat | Ampacity of the charge path as installed. Applies to **alternator output current** (alt shunt / estimate when available, battery current otherwise — conservative source preferred) |
| **Alternator limit** | A (total) | W @ VBat | Absolute rectifier/stator rating — a hard cap even when cold. Distinct from the thermal-performance capability curve (§3.4): that one says what it *can* do at temperature, this one what it may *never* exceed |
| **Belt limit** | Nm (or A @ reference RPM) | W @ RPM | RPM-dependent — see §2.2 |

All four are optional (unset = not in the min()), but the commissioning flow
asks for them explicitly — an unset limit is a decision, not an oversight.

### 2.2 Belt limit (torque-domain, slip-adaptive)

Belt grip is a **torque** limit, not a power limit. Since shaft power =
torque × angular speed, the transmittable electrical power rises ~linearly
with RPM:

```
P_belt(rpm) = (T_belt × 2π × rpm_alt / 60) × η_alt
```

Configured either as `T_belt` (Nm at the alternator shaft) or, more
installer-friendly, as **amps at a reference RPM** (`A_ref @ rpm_ref`), from
which `T_belt` is derived once. The resulting ceiling is near-zero at idle and
grows with speed — which matches observed belt behavior and replaces any
fixed "belt load %" guesswork. Requires an RPM source; with RPM LOST the belt
ceiling falls back to its idle-RPM value (conservative).

**Slip-adaptive trim.** The belt-slip detector (§3.3 — transient
stator-vs-reference ratio deviation) closes the loop:

- On detected slip: the belt ceiling is trimmed down 10 % immediately, WARN
  raised, and the event logged with the W/RPM operating point.
- Repeated slip at similar operating points → the learned `T_belt` is reduced
  to fit under the observed slip boundary (the spec's "slip curve", learned
  rather than typed).
- Recovery: trims decay slowly (days of slip-free running), and a replaced/
  re-tensioned belt is a one-tap "reset belt learning" in the app.
- No slip detector available (no dual RPM source) → the static curve stands,
  and the soft ramp (`ramp_w_per_s`) remains the transient-torque protection.

### 2.3 Warmup (engine-temp assisted, optional)

The warmup gate (profile spec T1) is satisfied by `warmup_time_s` **or**
coolant temperature ≥ `warmup_coolant_c` when engine temp is available on CAN
(§8.1 row 7) — whichever comes first, both optional. Additionally, an optional
**cold-engine ramp** scales the engine-budget ceiling from
`cold_power_pct` → 100 % between `coolant_cold_c` and `warmup_coolant_c`, for
installs that want reduced load on a cold engine rather than a binary gate.
Unset = plain timer behavior.

---

## 3. RPM subsystem (optional)

### 3.1 Sources, fused, priority-ordered

1. CAN engine RPM (N2K 127488 / J1939 EEC1) — field-independent
2. Stator zero-crossing (TIM2 capture), with **probe sampling** at zero field:
   sub-charging pulses (~2 % duty, ≤50 ms, every 250–1000 ms, <0.1 A avg) keep
   the stator readable while contributing no meaningful charge

*(A dedicated crank-pulse input is ⟦future-hw⟧ — the WS500 harness has no such
wire.)*

**Stator quality gating.** At low field the stator waveform is small and noisy
and can produce wild frequency readings. Stator-derived RPM is therefore
trusted only when: field duty ≥ a minimum threshold **or** the reading was
taken inside a probe pulse (known-good excitation); the value sits inside the
plausibility band `[0.5 × idle_rpm, 1.2 × max_rpm]`; and successive readings
pass a slew filter (no physically impossible RPM jumps). Readings failing any
gate are discarded — the fused RPM falls to a higher-priority source or goes
STALE. Garbage never reaches the curves.

Validity state machine VALID / STALE / LOST. On LOST, RPM-derived ceilings (§2
items 4–5) simply drop out of the min() in favor of the flat caps — signal loss
degrades gracefully instead of triggering rule chaos. "RPM = 0" is never inferred
from silence.

### 3.2 Synthesized tach output

Programmable pulses-per-rev, driven from the fused RPM value — **not** the raw
stator. The dash tach works at true zero field. `Tach Min Field` and `ForceTM`
do not exist in this product; the tach-vs-overcharge tradeoff is gone by
construction.

On v1 hardware the tach signal is a **Feature-Out function**: the single
Lamp/Feature-Out pin is assigned to exactly one job (lamp, tach, fan, alarm,
…), chosen in config. Installs that need both a lamp and a tach drive the lamp
over CAN/GX instead. ⟦future-hw: dedicated tach pin⟧

### 3.3 Calibration — one constant, no poles, no ratio

Pole count and pulley ratio are never needed individually. Stator frequency is
a single linear map to engine RPM:

```
f_stator = K × engine_rpm        (K = poles × ratio / 60 — but nobody types that)
```

The regulator stores and learns only **K**. Three calibration paths, best
available wins:

1. **CAN auto-learn** (zero effort). Any period with both CAN RPM and gated
   stator readings VALID continuously regresses K. Most installs calibrate
   themselves on the first motor out of the marina.
2. **Guided test points** (no CAN needed). The app walks the user through
   capture points — e.g. idle, 1000, 1500, 2000, 2500, 3000 — user holds each,
   types the dash-tach value, regulator averages gated stator frequency over
   the hold window and discards outliers. One point is mathematically enough;
   each extra point tightens K (regression) and checks linearity. A fit that
   is *not* linear is itself a finding (belt slip at load, or noise) and is
   reported, not silently averaged in. Capture quality (spread, point count)
   is stored with K.
3. **Uncalibrated / normalized mode** (no known RPM at all). The regulator
   measures `f_idle` (sustained stable minimum after warmup, or one-tap "this
   is idle") and `f_max` (highest gated frequency seen). The white-space and
   capability curve domains anchor to `[f_idle, f_max]` directly — the curves
   need a *consistent axis*, not physical units. RPM-unit features (real-RPM
   tach synthesis, MFD RPM display, CAN cross-check, belt torque curve) are
   unavailable and say so; everything else runs normally.

**Continuous cross-check** between any two VALID sources: slow drift in K →
recalibration suggestion; transient deviation under load → **belt-slip alarm**
(feeds §2.2's adaptive belt ceiling).

### 3.4 What RPM unlocks (all optional)

- Alternator capability curve: 8-point Watts-vs-RPM, entered or **learned** by a
  commissioning sweep.
- Engine load ("white space") curves — §3.5.
- Belt limit curve (§2.2).
- Overspeed field cut.

### 3.5 Engine load curves ("white space", Watts)

The engine-budget ceiling is a **Watts-vs-RPM curve over a bounded domain** —
because Watts (÷ efficiency) is what the engine actually feels as shaft load.

**Domain.** Defined by `idle_rpm` and `max_rpm` (engine RPM). Up to 8
breakpoints `{rpm, watts}`, strictly increasing RPM, linearly interpolated,
all breakpoints inside the domain. Evaluation:

- `RPM < idle_rpm` → **the curve is never evaluated below idle.** Readings
  below idle are either engine-stopping (ignition path handles that) or noise
  (§3.1 gating discards them); the ceiling holds the `idle_rpm` value.
  Below-idle values can therefore never drag the budget toward zero.
- `RPM > max_rpm` → clamp to the `max_rpm` value (overspeed protection is a
  separate raw-signal comparator, not this curve).
- RPM STALE → last-good curve value; RPM LOST → the curve leaves the min()
  and the flat caps govern (§3.1).

**Exact evaluation — the inputs are contractual.** The curve is evaluated as
piecewise-linear through the *exact* entered breakpoints. No spline fitting,
no smoothing, no moving averages, no monotonicity "correction" of the Watts
axis: a dip entered at 1400 RPM (resonance band, gearbox harmonic) and a peak
at 2200 are deliberate installer decisions and are honored to the number.
Anything the firmware does to the curve representation (storage, display,
transfer) must round-trip the breakpoints bit-exact. Learned curves (§3.4
capability sweep) may *propose* breakpoints — once accepted they are treated
identically: exact.

**Asymmetric application.** A dip must not be skated over by rate limiting:
when the evaluated ceiling **decreases** (RPM moving into a dip, curve switch
downward), the new ceiling applies immediately — bounded only by physical
field response. Only ceiling **increases** pass through the `ramp_w_per_s`
slew. The RPM signal feeding evaluation is the short-τ damped RPM (default
0.5 s, §1.4 of the profile spec) — quick enough to track a sweep through a
narrow dip; the dip binds whenever damped RPM is inside its span, for exactly
as long as it is inside.

**Two named curves.**

| Curve | Selected by | Intended use |
| --- | --- | --- |
| `curve_full` | default | Normal operation — full available engine budget |
| `curve_feature` | Feature-IN (or app / NMEA command) | Alternate budget: trolling/sailing low-load mode, genset-sharing, "quiet cabin", get-home derate — whatever the installer assigns |

Both share the same domain (`idle_rpm` / `max_rpm`). Switching curves is
slew-limited through `ramp_w_per_s` — flipping Feature-IN never step-changes
belt torque. If `curve_feature` is unset, Feature-IN's curve-select function is
inert and reported as such.

---

## 4. Thermal management

Closed-loop and **rate-aware**: the controller regulates where the temperature
is *heading*, not just where it is. Its output is a continuous power ceiling
into §2. The alternator runs as hard as it can *while holding* (e.g.) 95 °C
hot-spot — no step derates, ever.

### 4.1 Predictive thermal governor

The alternator is modeled online as a first-order thermal system. From filtered
temperature, dT/dt, and present output power, the governor continuously
estimates the **projected steady-state temperature**:

```
T_projected ≈ T_now + τ · dT/dt        (τ = learned thermal time constant)
```

and sets the power ceiling so that `T_projected` converges on the target —
max field is adjusted **dynamically from the trajectory**, not from threshold
crossings:

- **Heating fast** → `T_projected` is high even while `T_now` is fine → ceiling
  comes down early and smoothly. No overshoot-then-slash.
- **Thermal-mass sprint** → a cold alternator has real headroom: the governor
  permits power *above* the steady-state-sustainable level while `T_projected`
  remains at/under target given the remaining thermal mass, tapering as the
  hot-spot rises. Short motoring windows (lock transits, harbor maneuvers)
  harvest meaningfully more energy — safely.
- **Cooling** → recovery is proportional to observed trajectory, no sawtooth.

**Model learning.** τ and effective thermal resistance are seeded by mount
location (§4.2), optionally calibrated by the commissioning sweep (§3.4), and
refined online from every heat-up/cool-down the regulator observes. The model
is per-install, not per-datasheet.

**Guardrails.** The governor is an optimizer, never a safety authority: the
hard-limit comparator stays on raw temperature (§1.4). If prediction error
exceeds a plausibility band (model divergence — airflow change, failing fan,
wrong mount config), the governor falls back to plain PI on measured hot-spot
and raises a warning. Sustained anomalous dT/dt at steady power raises a
**cooling-degradation alarm** (blocked airflow / failed fan / slipping belt)
before any limit is hit.

### 4.2 Sensors and configuration

- **Sensor mount location** — per alt-temp sensor: `laminations` /
  `case_front` / `case_rear`. The loop regulates an **estimated hot-spot**
  (stator winding) temperature, not the raw reading: each location carries a
  firmware-default hot-spot offset and thermal lag (laminations ≈ small offset,
  short lag; case mounts ≈ larger offset, longer lag — case rear additionally
  biased for rectifier proximity). Offset and lag seed the predictive
  governor's thermal model (§4.1), so a slow case sensor is compensated in the
  projection instead of discovered in the overshoot. Offsets are
  overridable per sensor for instrumented installs; target and hard-limit temps
  are always expressed in hot-spot terms so moving a sensor never silently
  changes how hard the alternator runs.
- Target temp + hard-limit temp (fast pull to floor + fault).
- **Derate floor (W)**: the loop may not pull below this unless hard limit trips
  — "pulled back to nothing" is impossible by configuration.
- Ceiling slew limit — governor output changes are rate-limited (no chatter).
- Fan call on Feature-Out (when assigned) engages when `T_projected`
  approaches target — cooling is spent *before* output is (§4.1 makes this
  predictive, not reactive).
- One external alternator sensor on v1; the internal driver-stage NTC guards
  the regulator/field switch as a separate comparator (it is not part of the
  alternator hot-spot model). ⟦future-hw: second alt sensor, controller on
  max() of estimates, divergence plausibility warning⟧
- Battery temp is only a charge-window gate (low-temp Li cutoff = hard fault,
  high-temp = charge abort). No comp curves.

---

## 5. Inputs / Outputs

### Inputs (v1 = WS500 harness)

| Input | Notes |
| --- | --- |
| B+ / field supply (12/24/48 V systems) | Single platform across system voltages |
| VBat Kelvin sense pair | PC5 divider; mandatory-grade accuracy for Li CV |
| Shunt ±50 mV (one, battery- **or** alternator-side) | Digitized by the I²C INA22x/238 → V, A, **W** on-device. Placement declared in config (§6.2) |
| Alt temp ×1 (external NTC), battery temp ×1 | Open/short detect; REQUIRED/OPTIONAL/IGNORE policy with declared fallback; mount location (`laminations` / `case_front` / `case_rear`, §4) |
| Driver-stage temp (internal NTC) | Regulator/field-switch guard; rotor proxy on v1 (§5.1) |
| Stator/W input | RPM source 2 (TIM2 capture) |
| Ignition/enable | Wake |
| Feature-In ×1 | **One** assignable function: force-rest, quiet-mode cap, profile toggle, or `curve_feature` select. No repurposing logic |
| CAN ×1 (all dialects, §8), USB CDC | ⟦future-hw: second CAN, BLE/Wi-Fi, crank input, EGT input⟧ |

### Outputs (v1 = WS500 harness)

| Output | Notes |
| --- | --- |
| Field drive | TIM1 PWM + BKIN hardware fault trip; P/N-type per board topology; slew-limited soft ramp (belt + BMS-coordinated shutdown). Rotor clamp + ratiometric control — §5.1 |
| Lamp/Feature-Out ×1 | **One** assignable function: lamp, synthesized tach (§3.2), fan call, alarm, charge-active |
| Status LED | State + blink-coded faults |

### 5.1 Rotor protection (the 48 V / 12 V-rotor problem)

Most 48 V alternators carry a **12 V rotor**. The physical limit is **rotor
current** — I²R heating — not voltage; duty percentages are a proxy two steps
removed. The WS500 platform has **no field-current metering** (ADC scan fully
accounted for; field protection is the TIM1 BKIN comparator trip), so v1 runs
the best proxy the hardware allows, and the current-first design is recorded
as the ⟦future-hw⟧ upgrade path.

**v1 baseline — dynamic duty clamp + driver-temp guard:**

- **Primary clamp:** `duty_max = rotor_rated_v / V_supply`, computed
  **continuously from measured supply** (25 % at 48.0 V, ~21 % at 57.6 V
  charging). A static "25 %" cap would quietly over-volt the rotor by exactly
  the charge-voltage rise; the dynamic clamp cannot. `rotor_rated_v` defaults
  to **12 V whenever the system is 48 V**; confirmed at commissioning like
  `cells_series`.
- **Driver-stage NTC as proxy guard.** The internal driver-temp sensor (the
  "rotor drive temp sense") heats with field current and with cooling
  failures — a usable *alarm and derate trigger* for the switch stage, though
  not a rotor model. Sustained driver over-temp pulls field effort down and
  raises WARN.
- **BKIN hardware trip** remains the last-resort field cutoff (comparator,
  <µs, independent of firmware).
- The active mode is telemetered — v1 installs *see* "rotor model:
  unavailable (no current sense)". Silent degradation is the enemy.

⟦future-hw: current-first rotor control⟧ — with a low-side sense element in
the field return: current clamp `rotor_a_max` as primary, rotor resistance
learned from `V_applied / I_field`, and copper's tempco (+0.39 %/°C) turning
R-rise into an **estimated rotor temperature** — a sensor nobody installs.
Recorded here so the control architecture reserves the slot; excluded from v1.

**Override ladder:**

| Level | Condition | Behavior |
| --- | --- | --- |
| Default | duty ≤ `rotor_rated_v / V_supply` | Silent, always safe |
| Extended | user raises `rotor_v_max` above rated | Permitted; persistent WARN beyond +20 % (equivalently ~30 % duty at nominal 48 V) — rotor overdrive is a deliberate, visible state |
| Full field | explicit config flag `allow_full_field_48v` | Must be individually set, confirmed in-app with plain-language consequence text, logged as a config event, and telemetered as a standing CAUTION for as long as it is enabled. Never reachable by slider drag |

**Ratiometric control — no granularity loss.** With usable authority of only
~a quarter of the actuator range on 48 V, any algorithm that steps in
*absolute* duty points is 4× coarser than on 12 V — precisely the "subtracting
fixed percentages" failure that makes existing 48 V thermal regulation hunt.
Rules:

- Every control loop operates on **normalized field effort**
  `e = duty / duty_max` ∈ [0, 1] (on ⟦future-hw⟧,
  `e = I_commanded / I_allowed`) — 100 % effort means "everything this rotor
  may have," on every system voltage. Gains, steps, and slews are ratios of
  authority, never absolute duty points. Arbitration and `ramp_w_per_s` live
  in Watts and are unaffected; this rule governs the inner field loop.
- **PWM resolution is specified inside the span**: ≥ 10 effective bits across
  `[0, duty_max]` (≥ 1024 steps within the 25 %), via timer resolution and
  sub-LSB temporal dithering — not 10 bits across 0–100 % of which the rotor
  may only ever use a quarter.
- Telemetry reports **effort** (raw duty in the engineering view; rotor amps
  and estimated rotor temp on ⟦future-hw⟧). "Field 80 %" means the same thing
  on any system voltage: 80 % of what the rotor can give.

### 5.2 Engine-run detection & stationary-rotor budget

A stationary rotor has no fan and no relative airflow — its safe continuous
current is a small fraction of the running rating. Field excitation is
therefore gated on a **RUN-DETECT state**:

- **NOT-RUNNING (default at key-on):** field limited to the **detect
  budget** — on v1, a conservative fixed low duty (placeholder ~5 % of
  `duty_max`), pulse-cycled with a bounded on-time ratio, so an indefinite
  key-on-engine-off condition can never cook the rotor. The driver-temp NTC
  (§5.1) backstops the budget. ⟦future-hw: budget accounted live against the
  rotor temperature observer instead of fixed values⟧
- **Run detection, in priority order (§3.1):**
  1. CAN engine RPM > 0 — running is known before any field is applied.
  2. **Excitation probing (no CAN tach):** ramp or pulse the rotor within
     the detect budget while watching the stator — a spinning machine
     answers with an AC waveform at a gated-plausible frequency (§3.1); a
     stationary one answers with nothing (pure transformer coupling /
     DC response). Detection promotes to RUNNING; silence keeps the probe
     cycle going at a cadence the detect budget allows.
- **RUNNING → NOT-RUNNING:** fused RPM LOST *and* stator silent under
  excitation *and* no field-free source claims motion → drop back to detect
  mode (and BULK/FLOAT exit to STANDBY via the ignition path as applicable).
- The probe machinery is the same hardware path as §3.1's zero-field tach
  sampling — one mechanism, two consumers (RPM-while-resting, run-detect).

---

## 6. Additions (the genuinely new capabilities)

### 6.1 SOC-target charging
Stop CHARGE at a configured SOC (e.g. 90 %) for cycle-life, using BMS-reported
SOC or on-device coulomb counting. Daily-use ceiling + periodic 100 % top-balance
schedule (e.g. every N days charge to tail-power exit instead).

### 6.2 Shunt placement & load-aware tail detection
v1 has **one** shunt (INA22x/238 on I²C), installed battery- or
alternator-side, declared in config:

- **Battery-side (recommended):** battery current is measured truth → tail
  exit and Ah-revert fully armed (tier 1, §6.6).
- **Alternator-side:** with loads between alternator and battery, alternator
  current ≠ battery current and tail logic would lie — so local tail exit is
  **disarmed**; battery truth can still arrive from an external NMEA battery
  monitor (tier 2), re-arming it. The spec says this out loud at
  commissioning rather than letting the ambiguity ride.

⟦future-hw: second local shunt channel — both currents seen simultaneously,
house load reported separately, tail exit armed regardless of placement.⟧

### 6.3 BMS-coordinated everything
Charge-voltage/power ceilings accepted as arbitration inputs (DVCC-style / RV-C /
proprietary). BMS pre-disconnect warning triggers the soft field ramp **before**
the contactor opens — load-dump prevention by protocol, not just TVS.

### 6.4 Session + fleet telemetry
Per-engine-run report: Wh delivered, peak/avg W, peak temp, and **% of runtime
spent against each binding ceiling** (thermal vs engine vs stage) — directly
answers "what's my bottleneck?". 1 Hz flight recorder (≥72 h ring), event log
with config-change diffs, fault freeze-frames. Export CSV/JSON over USB CDC
(log depth bounded by the F072's 16 KB RAM / flash budget — ring sizes are a
firmware deliverable; the 72 h figure is aspirational, revise per hardware).

### 6.5 Multi-regulator sync
Leader/follower on the shared CAN bus: shared stage state, load-sharing in Watts, one combined
tail-exit decision. Twin engines / twin alternators without fighting.

### 6.6 System resistance & OCV learning

**Goal:** separate IR voltage rise from true state-of-charge voltage, so CV
targeting, tail exits, and SoC estimates run on what the *battery* is doing —
not on wiring drop and charge-current overpotential.

**Current-source abstraction.** Installs vary; the regulator binds "battery
current" to the best available source and every consumer knows the tier it is
running on:

| Tier | Source | Notes |
| --- | --- | --- |
| 1 | Local shunt, battery-side (`ShuntAtBat`) | Direct truth |
| 2 | NMEA battery current (external monitor) | Latency-checked, plausibility-checked |
| 3 | Local shunt, alternator-side | Alt current ≠ battery current under load; usable for R-learning pulses (ΔI is alt-driven), degraded for tail exit |
| 4 | Estimated alternator current | From field **duty/effort** + RPM + a fleet-learned alternator model (no field-current sense on v1); coarse |
| 5 | None | Voltage-only strategies (micro-rest OCV, below) |

Multiple sources present → cross-checked; disagreement beyond a band raises a
plausibility warning (bad shunt / bad N2K data caught early).

**Pulse-probe resistance learning (tiers 1–4).** During CHARGE while voltage is
well below the CV target (headroom guaranteed, and V-rise is IR-dominated), the
regulator injects short controlled field steps (±ΔW, 1–2 s, rate-limited):

```
R_ohmic  = ΔV/ΔI   (instantaneous step)
R_polar  = slower tail of the same response
```

Pulses are averaged over many events, and rejected when ΔV and ΔI don't
correlate (a house-load transient landed mid-pulse). **Natural transients are
free probes**: any correlated ΔV/ΔI step (windlass, inverter, fridge) updates
the estimate opportunistically — instrumented installs converge without
pulsing at all.

**Micro-rest OCV sampling (works even at tier 5).** Brief zero-field windows
(~1 s, rate-limited, only outside CV hold) let the ohmic component vanish
instantly; the sampled relaxed voltage approximates OCV **without knowing the
current at all**. On LFP the mid-range OCV curve is flat, but the ends are not
— which is exactly where anchoring matters.

**What the learned model feeds:**
- **Compensated voltage** `V_comp = V_meas − I·R` → CV entry and tail decisions
  reference battery-surface voltage, not sense-point voltage. (With Kelvin
  sense at the battery, learned R ≈ internal R; with B+ sense it includes
  wiring — the sense topology is declared in config so the estimate is labeled
  correctly.)
- **SoC anchoring** — micro-rest OCV corrects coulomb-counter drift, making
  §6.1 SOC-target charging honest without a BMS.
- **Connection-health trending** — per-session R is logged (§6.4); a rising
  trend fires a *"check connections/lugs"* warning long before a hot terminal
  becomes a fire. A step-change in R is flagged immediately.
- The thermal governor's sprint logic (§4.1) — better current estimates
  tighten the power model.

### 6.7 Firmware + config hygiene
Firmware update via **USB DFU** (the STM32 system bootloader is in ROM —
unbrickable path, app-guided). No wireless update on v1 ⟦future-hw: signed OTA⟧.
Human-readable versioned config file over USB CDC, CRC-checked at
boot; corrupt → safe defaults + fault (never silent factory reset). Config
changes logged as diffs (§6.4).

---

## 7. Safety / protections

- Overvoltage fast field kill (<1 ms) + load-dump TVS; soft-ramp path for
  coordinated shutdowns.
- Field driver short/open/over-current detect; regulator self over-temp.
- Overspeed cut (when RPM present).
- Shunt-open / reversed detection (claimed current with static VBat → fault, not
  runaway).
- Battery dT/dt runaway → charge abort + alarm.
- Watchdog + brown-out → field open.
- Sensor-loss policy per §5; every REQUIRED sensor's fallback is explicit config.
- **Degraded-mode ladder**: recoverable critical faults (lost VBat sense, lost
  BMS comms, implausible shunt) drop the regulator into **Limp Home** (§1.3
  profile 6) rather than field-off, when continuing at 3.30 V/cell + reduced
  power is provably safe for that fault class. Unrecoverable faults still go
  field-open. Which faults limp vs open is fixed in firmware, not user config.

## 8. Comms

### 8.1 Initial CAN integration matrix

| # | Integration | Protocol layer | Direction / use | Phase |
| --- | --- | --- | --- | --- |
| 1 | **Victron Display** (GX / Cerbo) | VE.Can (N2K-based): device announce, 127508/127506/127750 + Victron alternator/charger registers | Out — appear natively on GX as an alternator-charger: W/A/V, stage, active profile, binding ceiling, temps, faults | v1 |
| 2 | **Victron Program & Firmware** | VictronConnect / VE.Can vreg config + firmware channel | In — configure and update from the Victron toolchain | Future |
| 3 | **Victron DVCC** | CVL / CCL (/ DCL) distributed by GX | In — ceilings into the arbitration min() (§2, §6.3); loss of DVCC comms → declared fallback (own profile limits) | v1 |
| 4 | **Victron BMS** (Lynx Smart, VE.Bus via GX) | DVCC path above + BMS status frames | In — ceilings, SOC (feeds §6.1), pre-disconnect warning → soft ramp (§6.3) | v1 |
| 5 | **REC BMS** | CAN-bus BMS protocol (0x351 CVL/CCL, 0x355 SOC, 0x356 V/I/T, 0x35A alarms) | In — same consumers as #4, direct (no GX required) | v1 |
| 6 | **JK BMS** | Victron-style CAN BMS frames (0x351/0x355/0x356 family) | In — same consumers as #4; protocol quirks isolated in a per-vendor driver | v1 |
| 7 | **Engine input** | J1939: EEC1 (RPM → §3.1 source 1), ET1 (coolant → warmup gate), DM1 consume optional | In | v1 |

Design rule: every BMS/DVCC integration is a **driver behind one internal
interface** — `{CVL, CCL, SOC, alarms, pre-disconnect}` — so vendor quirks
never reach control logic, and adding a BMS is a driver, not a feature.
All inbound ceilings land in the same arbitration min() (§2); all inbound
comms have declared loss-of-signal fallbacks.

### 8.2 General NMEA2000

**Out**: 127508 (battery), 127506 (DC detail, when bank monitor), 127750
(charger status), optional 127488 relay of fused RPM for MFD tach, 126983/126985
alerts, 126996/126998 product info, proprietary fast-packet full telemetry
(field duty, binding ceiling, temps, RPM source/state).

**In**: 127488 (RPM), 127489 (coolant), external shunt 127508/127506 (§6.6
tier 2), sync bus (§6.5).

**J1939** (same bus, when enabled): charger status + DM1 diagnostics.

**Single-bus note (v1).** One physical CAN carries every integration in §8.1
plus the sync bus (§6.5). Dialect mix (N2K / J1939 / Victron / BMS frames) is
configured per install; bus-load budgeting and ID-collision rules are a
firmware-design deliverable, not a user concern. ⟦future-hw: second CAN⟧

## 9. Error reporting & logging

Structured faults: code, severity (INFO/WARN/FAULT/CRITICAL), latching flag,
first/last-seen, count, freeze-frame (V/A/W/RPM+state/temps/stage/binding
ceiling). Surfaced via LED blink, app plain-language + remedy, N2K alerts, DM1.
Logging per §6.4.

---

## Appendix A — Deleted from the WS500 model, and why

| WS500 concept | Why it's gone |
| --- | --- |
| CPA/CPO/CPF/CPP/CPE 6-stage stack | Li needs CHARGE + REST. Two stages, §1 |
| Pb chemistry presets, equalize, temp-comp curves | Li-first product |
| DIP switches + `CP_Index`/`BC_Index` mirrors | App-configured named profiles |
| Small-alt derate %, half mode + Trigger Half-Power RPM | One arbitration min() in Watts, §2 |
| RFM1–8 field map, PBF, Feature-IN gating conflicts | RPM-linked ceilings live in §2 as optional curves; Feature-INs are single-function |
| Tach Min Field / ForceTM | Synthesized tach + probe sampling, §3.2 |
| Alt Poles + Eng/Alt Drive Ratio as typed config | Collapsed into one learned constant K (§3.3); typed values survive only as an optional seed for K |
| 12 V voltage normalization, 500 Ah amp normalization | Real units everywhere |
| `SV_Override` | Auto-detect + one-time confirm |
