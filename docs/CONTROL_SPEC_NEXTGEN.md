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
[WS500_HARDWARE_SPEC.md](WS500_HARDWARE_SPEC.md)): STM32F072RB (LQFP64,
PROJECT_PLAN §0.6 V1); field PWM on TIM1 CH1 at **143.2 Hz / 10-bit**
(V2) — **no BKIN hardware trip: the break input is unrouted and the stock
`BDTR.BKE = 0`, so field cutoff is a software path** (V1+V2, see the
callout below); stator on **PA10 EXTI rising edge with TIM2 as a
free-running timebase** — not a TIM2 capture channel (V1+V2); **one** CAN bus
(bxCAN); USB CDC config port (ST system DFU bootloader in ROM); current +
local bus voltage via a **TI INA226** (single part, no variant auto-detect —
V3) on I²C at the **single** external shunt (battery- or alternator-side per
config); ADC: **two β3950 external NTC channels (PA1/PA2)** and one
regulator-internal **driver-stage NTC (PA3, β3380)** — **there is no battery
NTC** (V8); VBat Kelvin-sense divider; digital: 1× Ignition, 1× Feature-In,
1× Lamp/Feature-Out; status LED.

> **⚠ There is no hardware field-cutoff backstop on this board.** Earlier
> drafts of this spec listed a TIM1 BKIN comparator trip as the last-resort
> field cutoff. PROJECT_PLAN §0.6 V1 and V2 refuted it from the stock binary:
> no break alternate-function pin is configured on any port, and `BDTR.BKE = 0`
> in the stock `MX_TIM1_Init`. The stock product cuts the field in **software**
> by clearing `BDTR.MOE`. Our firmware does the same, through the single §7 R0
> `enter_safe_state()` funnel (`Core/Src/safe_state.c`). **Nothing in this spec
> may be relaxed on the assumption that hardware will catch it** — the software
> funnel is the whole protection, which is exactly why §7 R0 requires it to be
> callable from a fault handler with a corrupt stack. A real BKIN trip stays
> available only as a ⟦future-hw⟧ improvement, and only if a fault comparator
> is ever wired to a break-capable pin.

Everything in this spec is written to run on that inventory. Features needing
hardware the platform lacks are **excluded from v1** and marked ⟦future-hw⟧
where they appear:

| Excluded (no hardware) | v1 consequence |
| --- | --- |
| Rotor/field current metering | §5.1 runs in duty-clamp + driver-temp mode; no rotor R learning / temp observer |
| Hardware field-cutoff (TIM1 BKIN) | Refuted by §0.6 V1+V2 — cutoff is the software §7 R0 funnel; see the callout above |
| Local battery-temperature sensor (channel binding) | **Not excluded — corrected 2026-07-28, GH#40.** The harness Battery Temp Sense wire is real (§6c row 9) and lands on one of the two β3950 channels (PA1/PA2, §6b); only *which* channel is `bench-pending`. §4.2's `batt_temp_src` config ships inert (default `none`) until the bench step + a config write bind it — see §4.2 |
| Second local shunt channel | Single-shunt placement rules (§6.2) |
| Dedicated crank-pulse RPM input | RPM sources = CAN + stator only (§3.1); with CAN-IN deferred to v2, **stator is the sole v1 source** |
| EGT input | EGT ceiling only if EGT arrives on CAN (⇒ v2) |
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

Configured as `T_belt` (Nm at the shaft, physically exact) or — cleaner and
unit-consistent — as **power at a reference RPM** (`W_ref @ rpm_ref`). The curve is
linear through the origin, so one `(W, RPM)` point defines it entirely:

```
P_belt(rpm) = W_ref × (rpm / rpm_ref)
```

**Power, not amps, is the right anchor.** Belt load is torque × speed = shaft power
(≈ electrical power ÷ efficiency), and unlike amps it is **voltage-independent** — the
belt doesn't care whether the alternator is 12 V or 48 V, only how much power passes
through the pulley (200 A is ~2.7 kW at 12 V but ~11 kW at 48 V — a useless belt unit).
Watts also matches the arbitration domain (§2). The resulting ceiling is near-zero at
idle and grows with speed — matching observed belt behavior and replacing any fixed
"belt load %" guesswork. Requires an RPM source; with RPM LOST the belt
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

### 2.4 Precedence — DVCC, BMS, and local limits

> **Status: v1 enforces the local half of this only.** All inbound (CAN control-in)
> sources named below — BMS, DVCC, external monitors — are **v2 scope** (deliverable
> #10). In v1 nothing on the CAN bus can influence charging at all: CAN is Tx-only,
> read-only broadcast. This section is the settled **design intent** those v2 drivers
> must implement, written down now so the rule is fixed before the code exists.

**There is no priority ladder between sources, and that is the design.** A ranked
scheme ("BMS beats local", "DVCC beats BMS") has to answer *"what if the winner is
absent, stale, or wrong?"*, and every answer to that question is a way to charge
harder than something on the system asked for. This regulator answers it structurally
instead:

> **Every active constraint is in force simultaneously, and the most restrictive one
> governs. An external source can only ever *tighten* the outcome — it can never raise
> a local limit, and it can never be the reason a local limit is ignored.**

Four layers apply, none able to loosen the ones around it:

**1. The Watts min() — `ctrl_arbitrate()` (§2).** Every power ceiling competes on equal
footing regardless of origin: `thermal`, `bms_ccl` *(BMS or DVCC charge-current limit —
v2)*, `battery_c`, `wiring`, `alt_absolute`, `alt_capability`, `belt`, `engine`,
`user_cap`. Lowest wins and is reported as the binding source. Absent, stale or
implausible ceilings are set to `CTRL_CEILING_INACTIVE` (+inf) and drop out of the
min() by construction — **a missing input can never become a permission.**

**2. The voltage min() — CV target selection (v2).** The stage's CV target (profile
`cv_target_vcell` in CHARGE, `rest_voltage_vcell` in REST, `limp_vcell` in Limp Home)
is min()'d with the BMS **CVL**. §6.3: *"CVL below the profile CV target simply wins in
the min(); the profile does not fight it."* A CVL **above** the profile target must
change nothing — one-directional by construction, not by a check that could be
inverted. Voltage and power are separate domains; whichever demands the lower field
effort governs that tick.

**3. Hard overrides that bypass both min()s.** Not ceilings — stops.

| Override | Effect | Status |
|---|---|---|
| BMS **pre-disconnect** | soft field ramp to zero, in every state, applied last so nothing can re-energise it — load-dump prevention by protocol, not just by TVS | v2 |
| BMS **protection alarm** | charge ceiling forced to 0 W; must stay latched while the alarm signal is *stale* (alarm-then-silence is a BMS acting on its own alarm, not the alarm clearing) | v2 |
| **Fault ladder** (§7) | CRITICAL opens the field; LIMP-class swaps in Limp Home caps | **v1** |
| **Stationary-rotor gate** (§5.2) | no rotation → field held to the pulsed detect budget | **v1** |
| **Rotor duty clamp** (§5.1) | hard bound on field *duty* from the field-supply voltage, beneath everything above | **v1** |

The rotor clamp is the floor under all of it: on the reference install it is the
binding protection most of the time, and **nothing on the CAN bus can widen it** —
by construction, in v1 and v2 alike.

**4. Loss of signal is a declared state, never a silent one (v2).** If a source that
was present goes silent, its ceilings revert to inactive, the regulator falls back to
its own profile and hardware limits, and it raises a fault. It must **not** keep
enforcing the last value it heard — stale data is not truth — and it must **not**
free-run. Note the deliberate asymmetry: a source that was *never* present is a
legitimate configuration, not a fault. Distinguishing "BMS died before power-on" from
"no BMS fitted" needs an explicit `bms_required` commissioning flag (v2).

#### What "DVCC" does and does not mean here

DVCC is Victron's **system-level** charge-control scheme: a GX device aggregates a
managed battery's limits — plus any user-set limits and SVS/SCS behaviour — and
distributes them to **Victron** charge sources over VE.Can / VE.Bus / VE.Direct.

**There is no standard mechanism by which a GX pushes DVCC limits to a third-party
device, and this regulator is a third-party device.** The practical consequence, which
must be designed for rather than discovered later:

- What a v2 driver *can* read is what the **battery itself broadcasts** — the CAN-BMS
  frame set (`0x351` CVL/CCL/DCL, `0x355` SOC/SOH, `0x356` V/I/T, `0x35A` alarms,
  `0x35C` charge-enable). On a shared bus this covers the common case well: the
  regulator honours the same battery-authored limits the GX is itself consuming, and
  needs no GX at all.
- What it **cannot** see are limits that originate *in the GX* rather than in the
  battery — a charge-current limit typed into the GX UI, or a DVCC value the GX
  computes or overrides. In such an install the battery's own broadcast still binds
  us, but the *system* limit may be lower than what we enforce.

Closing that properly needs either a Victron-specific Rx driver or an explicit
commissioning rule ("set install limits on the regulator too; do not rely on the GX to
hold it back"). **Until it is closed, do not describe this regulator as DVCC-compliant
— it honours battery-broadcast limits, which is a different and smaller claim.**

---

## 3. RPM subsystem (optional)

### 3.1 Sources, fused, priority-ordered

1. CAN engine RPM (N2K 127488 / J1939 EEC1) — field-independent. **v2 only**:
   all CAN control-in is deferred (PROJECT_PLAN §1.1), so in v1 this source is
   never present and the fusion below degenerates to a single source
2. Stator zero-crossing (**PA10 EXTI edge + TIM2 timebase**, §0.1 — not a TIM2
   capture channel), with **probe sampling** at zero field:
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

**v1 has no second source to fall back to.** With CAN engine RPM deferred to v2,
a stator that fails its quality gates puts the fused value straight to LOST
rather than to a lower-priority source. That is safe by construction — LOST
drops the RPM ceilings out of the min() and the §5.2 run-detect gate
independently holds the field to the stationary-rotor budget until rotation is
confirmed — but it means the belt/engine-load ceilings (§2.2, §3.5) are
**inactive whenever the stator is unreadable in v1**, and the flat caps are
carrying the whole load. Do not spec any protection that depends on RPM being
available.

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
- One external alternator sensor on v1; the internal driver-stage NTC
  (PA3, β3380 — confirmed as the FET/driver over-temp channel by §0.6 V8, which
  also shows the stock firmware faulting it at 125 °C) guards the
  regulator/field switch as a separate guard path. It is **not** part of the
  alternator hot-spot model, and — since BKIN is unrouted (§0.1) — it is a
  *software* guard, not a comparator. ⟦future-hw: second alt sensor, controller
  on max() of estimates, divergence plausibility warning⟧
- Battery temp is only a charge-window gate (low-temp Li cutoff = hard fault,
  high-temp = charge abort). No comp curves.
  - **v1's battery-temperature source exists but is bench-pending, and ships
    inert by default (GH#40, corrects the "no v1 source at all" text this
    replaces).** §0.6 V8 only refuted PA3 as the battery sensor — it did not
    refute the harness. WS500_HARDWARE_SPEC §6c row 9 (Battery Temp Sense) is a
    real wire, and the board carries two identical β3950 NTC channels (PA1,
    PA2, §6b); one is the Alternator Temp Sense (harness wire 4) and the other
    very likely lands the battery probe. But **which physical channel is
    which is `bench-pending`** (§6b: "which of PA1/PA2 is ATS → bench"; open
    issue #8, ADC channel binding by signal injection) — nothing in the RE
    record distinguishes them, and guessing wrong is not cosmetic: an
    alternator probe reading 80 °C would look like a scalding battery and
    abort charging, or a warm alternator would mask a genuinely freezing bank.
    So the mechanism (`batt_temp_src` config, PROFILE_SPEC §3.1) ships
    complete and **defaults to `none`** — inert until a bench step identifies
    the channel and a config write binds it. Once bound, the low/high-temp
    charge-window gate arms automatically off `batt_temp_c` — no separate
    "enable" step beyond the channel binding itself.
  - **No-sensor policy is a configurable flag, default "charge and
    annunciate."** `require_batt_temp` (bool, default `false`, PROFILE_SPEC
    §3.1): false is the v1 default and matches history — the window simply
    stays unarmed and the firmware **must make that visible rather than
    silent** (`batt_armed` on the telemetry line, `control/Inc/telemetry_json.h`;
    `batt_c: null` alone is not annunciation, because null also describes a
    transient sensor glitch). true is the opt-in for cold-climate installs
    that would rather block charging than run with an unarmed low-temp gate —
    it raises `CTRL_FAULT_BATT_TEMP_REQUIRED` (§9.1 code 18) and BLOCKs
    charging (auto-resume) exactly like `BATT_LOWTEMP`/`BATT_HIGHTEMP` when
    `batt_temp_c` is invalid. A v1 build with the shipped defaults
    (`batt_temp_src: none`, `require_batt_temp: false`) behaves identically to
    the pre-GH#40 firmware: it reports the battery-temp input as *unavailable*
    and never reports a window as *satisfied*.
  - **Alternator thermal protection is unaffected by the choice of channel
    (fixed 2026-07-28, GH#43).** An earlier draft of this section found the
    app fed both the `alt_hotspot_c` fault input and the §4.1 thermal
    governor from channel A only (`main.c`), with `alt_temp2_c` consumed
    nowhere — so binding **`adc_a`** would have silently moved the alternator
    probe to the unread channel, and `CTRL_FAULT_SELF_OVERTEMP` could never
    have fired from the alternator NTC while the governor returned
    `CTRL_CEILING_INACTIVE` forever. That gap is closed: both call sites in
    `main.c` now derive their hot-spot input from `ctrl_nan_max2(alt_temp_c,
    alt_temp2_c)` (`control.h`) — the max of whichever alt-side channel(s)
    are finite, NaN-tolerant so a channel `batt_temp_src` claims for the
    battery probe never blinds the other. Binding **either** `adc_a` or
    `adc_b` to `batt_temp_src` now leaves alternator thermal protection
    intact; the `adc_a` prohibition that stood against the bench binding step
    (issue #8) is lifted here — **the STAGE_A_RUNSHEET.md / PROJECT_PLAN.md
    text recording that prohibition is a separate file this change does not
    touch and still needs updating to match.** Regression coverage:
    `control/test/test_nan_max.c` (the NaN-tolerant math) and
    `control/test/test_statemachine.c` (one alt channel NaN, protection still
    fires from the other) — see faults.h/control.h for the driver-stage NTC's
    own protection state, since GH#39 changed it too.

---

## 5. Inputs / Outputs

### Inputs (v1 = WS500 harness)

| Input | Notes |
| --- | --- |
| B+ / field supply (12/24/48 V systems) | Single platform across system voltages |
| VBat Kelvin sense pair | PC5 divider; mandatory-grade accuracy for Li CV |
| Shunt ±50 mV (one, battery- **or** alternator-side) | Digitized by the I²C **INA226** (§0.6 V3 — one part, no auto-detect) → V, A; **W computed in software** (CALIBRATION left at POR, on-chip CURRENT/POWER unused). Placement declared in config (§6.2) |
| Alt temp ×2 max (external β3950 NTC, PA1/PA2) | Open/short detect; REQUIRED/OPTIONAL/IGNORE policy with declared fallback; mount location (`laminations` / `case_front` / `case_rear`, §4). Which channel is the alternator probe vs. a second external probe is `bench-pending` (identical β in firmware — §0.6) |
| Battery temp (external β3950 NTC, `batt_temp_src` = PA1 or PA2) | **Corrected 2026-07-28, GH#40** — WS500_HARDWARE_SPEC §6c row 9 ("Battery Temp Sense") is a real harness wire; §0.6 V8 only refuted PA3, not the harness. Shares the alt-temp channel pair (Alt temp ×2 max row above): `batt_temp_src` names which of PA1/PA2 feeds `batt_temp_c` instead of an alternator probe. Default `none` (inert) — channel identity is `bench-pending` (§6b). See §4.2 |
| Driver-stage temp (internal NTC, PA3 β3380) | Regulator/field-switch guard; rotor proxy on v1 (§5.1) |
| Stator/W input | RPM source 2 — **PA10 EXTI edge + TIM2 timebase** (§0.1); sole v1 RPM source |
| Ignition/enable | Wake |
| Feature-In ×1 | **One** assignable function: force-rest, quiet-mode cap, profile toggle, or `curve_feature` select. No repurposing logic |
| CAN ×1 (all dialects, §8), USB CDC | ⟦future-hw: second CAN, BLE/Wi-Fi, crank input, EGT input⟧ |

### Outputs (v1 = WS500 harness)

| Output | Notes |
| --- | --- |
| Field drive | TIM1 CH1 PWM @143.2 Hz, 10-bit (§0.1); cutoff is **software** (`BDTR.MOE` clear via the §7 R0 funnel) — **no BKIN**; P/N-type per board topology; slew-limited soft ramp (belt shutdown; BMS-coordinated shutdown is v2). Rotor clamp + ratiometric control — §5.1 |
| Lamp/Feature-Out ×1 | **One** assignable function: lamp, synthesized tach (§3.2), fan call, alarm, charge-active |
| Status LED | State + blink-coded faults |

### 5.1 Rotor protection (the 48 V / 12 V-rotor problem)

Most 48 V alternators carry a **12 V rotor**. The physical limit is **rotor
current** — I²R heating — not voltage; duty percentages are a proxy two steps
removed. The WS500 platform has **no field-current metering** (the 7-channel ADC
scan is fully accounted for — §0.6 V4) **and no hardware field-cutoff** (BKIN
unrouted, `BDTR.BKE = 0` — §0.6 V1+V2). So v1 runs the best proxy the hardware
allows *and* carries the entire protection in software; the current-first design
is recorded as the ⟦future-hw⟧ upgrade path.

**v1 baseline — dynamic duty clamp + driver-temp guard:**

- **Primary clamp:** `duty_max = rotor_rated_v / V_supply`, computed
  **continuously from measured supply** (25 % at 48.0 V, ~21 % at 57.6 V
  charging). A static "25 %" cap would quietly over-volt the rotor by exactly
  the charge-voltage rise; the dynamic clamp cannot. `rotor_rated_v` defaults
  to **12 V whenever the system is 48 V**; confirmed at commissioning like
  `cells_series`.
- **Driver-stage NTC as proxy guard (derate implemented, 2026-07-28, GH#39).**
  The internal driver-temp sensor (PA3, β3380 — §0.6 V8 confirms this is the
  FET/driver channel and that stock faults it at 125 °C) heats with field
  current and with cooling failures — a usable *alarm and derate trigger* for
  the switch stage, though not a rotor model. Sustained driver over-temp now
  pulls field effort down AND raises WARN, closing the gap a 2026-07-28 safety
  review found (`control.c` raised `CTRL_FAULT_SELF_OVERTEMP` at 120 °C into
  `CTRL_FAULT_WARN_MASK` — disposition CONTINUE — while no arbitration ceiling
  consumed `driver_temp_c` at all, so a cooking field switch set a bit and the
  field kept driving at full commanded duty; weaker than stock, and it mattered
  more with BKIN refuted and no rotor-current sense — this NTC is the only
  guard the switch stage has). Two mechanisms, layered:
  - **Graduated derate**, `Core/Src/config_protocol.c`'s
    `config_get_driver_thermal()` feeding `ctrl_ceilings_t.driver_thermal_w` —
    a second instance of the §4.1 predictive governor (`thermal.c`) on
    `driver_temp_c`, arbitrated into the power `min()` like every other
    ceiling (`CTRL_BIND_DRIVER_THERMAL`), so it is reported the same way.
    **[SPEC-SIGNOFF] pending bench**, all four values: onset `target_c` 100 °C,
    at the derate floor by `hard_c` 120 °C (the point the existing WARN
    already fires), `derate_floor_w` 100 W (reused from the alternator
    governor's floor, no driver-specific number to prefer), `gain_w_per_c_s`
    50 (also reused — the target-to-hard span here, 20 °C, is comparable to
    the alternator governor's 15 °C). `tau_s` 30 s is the one value NOT
    copied from the alternator governor (300 s): a FET/heatsink is a much
    smaller thermal mass, so materially shorter is directionally right, but
    it is a guess, not a measurement — the highest-uncertainty constant here,
    flagged for bench characterization before any sustained-load run.
    None of these four numbers come from a thermal model of this board — we
    have none — they exist only to shape a derate ahead of the hard block
    below, bracketing the one number RE actually gave us.
  - **Hard block at 125 °C** — `CTRL_FAULT_DRIVER_OVERTEMP` (§9.1 code 19),
    a fault bit **separate from** `SELF_OVERTEMP`, in `CTRL_FAULT_OPEN_MASK`:
    CRITICAL, latches until reset, field opens immediately. Matches — never
    weaker than — stock's single 125 °C fault (`0x4029`). Deliberately not a
    disposition change to `SELF_OVERTEMP`: that bit stays WARN/CONTINUE and
    still conflates the alternator and driver readings in one bit (§9.3 D2,
    **not resolved by this change** — see the D2 entry for why a full split
    was judged out of scope here).
  - Both are `driver_temp_c`-only; `SELF_OVERTEMP`'s existing 120 °C WARN
    (both alt-side and driver readings) is unchanged.
  This also closes the "§5.1 rests on an *inferred* internal-NTC channel" open
  item that PROJECT_PLAN §0.6 recorded against this section: the channel is
  confirmed, and it is the driver stage, not the battery.
- **Last-resort field cutoff is software** — the §7 R0 `enter_safe_state()`
  funnel clears `BDTR.MOE`, zeroes the duty and drives the pin low. There is no
  comparator beneath it (§0.1). Two design obligations follow, and neither is
  optional on this board: the funnel must be reachable from every failure path
  including a fault handler on a corrupt stack, and the clamp above it must be
  correct-by-construction rather than backstopped — which is why §8.1/§8.4 of
  PROJECT_PLAN verify the clamp exhaustively in SIL and property tests instead
  of trusting a trip that does not exist.
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

#### 5.1.1 Clamp-supply plausibility guard — constants signed off (2026-07-28, GH#34)

`duty_max = rotor_v / V_supply` is only as trustworthy as the voltage it
divides by: an in-range-but-false-LOW reading loosens the clamp onto the true
(higher) bus — the §8.4 property sweep walked exactly such a lie to 1.8× rated
rotor voltage before the floor below existed. `ctrl_vsup_guard()`
(`control/Src/field.c`) vets the clamp voltage; its constants (declared in
`control/Inc/field.h`, previously `[SPEC-SIGNOFF]` placeholders) are signed
off as follows. The design rule they all serve: **every failure direction
resolves to the tighter clamp.**

| Constant | Value | Why this value |
| --- | --- | --- |
| `CTRL_VSUP_MAX_VCELL` | 4.0 V/cell | Worst-case fallback bus when no reading is trusted: comfortably above any legal charge target (§1 tops out at 3.65), so the fallback clamp `rotor_v / (4.0 × cells)` (≈18.8 % on 16S) is *tighter* than any real operating clamp — a fallback that can only under-drive the rotor. |
| `CTRL_VSUP_MIN_VCELL` | 2.0 V/cell | Below this a live charging bus is physically implausible (a 2.0 V/cell LFP bank is destroyed-flat, and the alternator holds the bus up while charging), so the reading is treated as nonsense, not as a bus level — the worst-case fallback applies. |
| `CTRL_VSUP_FLOOR_VCELL` | 3.2 V/cell | Hard floor under the *accepted* clamp voltage — `v_limp`'s §1 spec minimum, the lowest voltage any charging bus is allowed to sit at. Bounds ANY false-low, including the sub-step slow drift §8.4 found, to `rotor_v × V_true / (3.2 × cells)` ≈ 1.13× rated rotor volts (≈1.27× rated rotor watts) sustained worst case on this install — the accepted, WARN-telemetered residual. A *genuine* sub-floor bus is only ever clamped tighter by the floor (12 V / 51.2 V = 23.4 % where a true 48.0 V bus would allow 25 %) — the floor cannot loosen anything. |
| `CTRL_VSUP_STEP_FRAC` | 4 % | Distrust threshold for a downward step. The largest plausible *honest* one-tick sag on this install is a heavy load step through pack + wiring resistance (~140 A × 12 mΩ ≈ 1.7 V ≈ 3 % at 54 V); anything deeper is treated as a sensor lie until proven. A false positive costs only availability (clamp holds tighter, WARN visible), never safety. |
| `CTRL_VSUP_DISTRUST_MAX_MS` | 300 000 (5 min) | Re-anchor hold. A genuine bus re-level is re-trusted after 5 min of a stable lower reading — long enough that a transient lie cannot ride it, short enough that the clamp does not stay over-tight forever; WARN telemetered for the whole distrust interval. A lie that *survives* 5 min re-anchors, and the 3.2 V/cell floor row above then bounds the exposure. |

Residual (also documented in `sim/README.md`, accepted): a consistent
false-low present *from boot* on the single physical voltage source is
undetectable in-core; its exposure is bounded by the floor row. Closing it
fully needs a second, independent supply-sense channel ⟦future-hw⟧.

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

**Detect-budget constants — signed off (2026-07-28, GH#34).** The "~5 %"
placeholder above is now fixed (declared in `control/Inc/field.h`, asserted by
the SIL `stalled_rotor` scenario):

| Constant | Value | Why this value |
| --- | --- | --- |
| `CTRL_RUN_DETECT_EFFORT` | 0.05 (5 % of `duty_max`) | Effort maps to rotor current as `I_f = e × rotor_v / R_rotor` (§5.3), so the probe pulse drives ≈0.15 A into the 3 A-rated winding — excitation for the stator to answer, at ~0.25 % of rated rotor heating even if held continuously. Absolute duty ≈1.2 % on this install, inside §3.1's "~2 %". |
| `CTRL_RUN_PROBE_PERIOD_MS` | 500 ms | Middle of the §3.1 probe cadence band (250–1000 ms): run-detection latency ≤ 1 s without meaningfully raising the average excitation. |
| `CTRL_RUN_PROBE_ON_MS` | 50 ms | The §3.1 "≤50 ms" pulse bound; the 10 % on-ratio gives average `I_f` ≈ 15 mA (§3.1 requires < 0.1 A) and average rotor dissipation ≈ 9 mW — a stationary, fanless rotor stays at ambient indefinitely (SIL 2 h key-on-engine-off soak: `I_f` ≤ 0.25 A, ≤ 1 W, no temperature rise). |

Failure-mode check behind the sign-off: if the pulse cycling ever wedged fully
ON, continuous 5 % effort is 0.15 A ≈ 0.09 W into a 36 W-rated winding — the
budget stays safe even when its own timer breaks. What SIL **cannot** prove:
whether 0.15 A of excitation yields a readable stator waveform on the real
machine — probe *detectability* is `bench-pending` (pulse electrical behaviour
in place engine-off at Stage C; detectability itself needs the machine turning —
first M6 supervised run; §5 in-place decision 2026-07-29). If it falls short, raise the pulse amplitude
(the thermal argument above holds far beyond 5 %), not the on-ratio.

### 5.3 Inner field-loop numerics (v1 — specified 2026-07-28, GH#34)

This section specifies the shipped inner loop (`control/Src/control.c`) and
records the derivation the §8.1 SIL retune ("dt-scaled, KP −5×") was missing.
Scope honesty up front: the plant-gain figures come from the SIL plant
calibrated to **one** real operating point (the 2026-07-24 stock charge
trace); the quantities that cannot be derived without the bench are listed at
the end with the measurement that closes each. Changing any constant here is a
field-path change and takes the safety-review gate.

**Loop rate.** One control tick per main-loop period: **100 Hz (10 ms,
`LOOP_PERIOD_MS`)**, catch-up scheduler, IWDG-budgeted (§7 R1). Gains are
**dt-scaled (per-second)** — the SIL retune removed the original per-tick,
tick-rate-dependent form — so behavior is invariant under scheduling jitter;
the discrete-stability bound below holds to dt ≈ 20 ms (two missed ticks),
beyond which the watchdog is already the governing mechanism. The 143.2 Hz PWM
carrier (§0.6 V2) is asynchronous to the loop; the duty compare is rewritten
once per tick.

**Controller form.** Not a textbook PI: two parallel **integral-only
(velocity-form)** channels sharing ONE integrator — the normalized effort `e`
(§5.1):

```
e_v = e + KV · (V_cv − ~V_pack) · dt      (CV channel; damped ~V, τ = 1 s)
e_p = e + KP · (P_cmd − P_batt) · dt      (power channel; raw measured W)
e  ← clamp( min(e_v, e_p), 0, 1 )         (lower demand governs — §2.4 layer 2)

KV = 2.0    effort / (pack-volt · s)
KP = 1.0e-3 effort / (watt · s)
```

There is deliberately no proportional term: the battery's ohmic response makes
voltage/power react within one tick of an effort change (the plant supplies
the fast path), and every *protection* is outside this loop — the OV
comparator runs on raw signals (§1.4) and the §5.1 clamp bounds the actuator
no matter what the loop does. Loop stability is therefore a **quality**
property here, not a safety property: the worst any loop misbehavior can
command is `e = 1` ⇒ duty = `duty_max` ⇒ rated rotor current.

**What effort means physically (the derivation cornerstone).** With the §5.1
clamp unsaturated, `duty = e · rotor_v / V_bus`, so the averaged rotor current
is

```
I_f = duty · V_bus / R_rotor = e · rotor_v / R_rotor = 3.0 A × e   (12 V / 4 Ω install)
```

— **effort is a normalized rotor-current command, invariant to bus voltage**:
the same gains are correct at 48.0 V and at 57.6 V because the bus voltage
cancels. This is the §5.1 ratiometric rule made concrete (`e = 1` ⇔ rated
rotor current). On a 12 V system, where `duty_max` saturates at 1.0,
`I_f = e · V_bus / R_rotor` — similar span, but note KV acts on *pack* volts,
so a 12 V install sees ~¼ the CV-channel gain per V/cell of error (the plant's
volts-per-effort scale partially compensates). Only the 16S install is
v1-relevant; revisit before any other voltage class runs this code.

**Plant gains (calibrated SIL plant, §8.1).** Linearizing `sim/plant.c`
(anchored to the stock trace: `k_e` = 0.012713, saturation `tanh(I_f/3 A)`,
`R_stator + R_int` = 52 mΩ, `R_int` = 12 mΩ, pulley 2.5) around conducting
operating points:

```
dI_alt/de = (rotor_v/R_rotor) · k_e · rpm_alt · sech²(I_f/3 A)/3 / (R_s + R_int)
          ≈ 205 A/e at idle (800 eng RPM) … ≈ 600–750 A/e at cruise (2330 eng RPM)
dP/de     = V_bus · dI_alt/de ≈ 33–41 kW/e at cruise (worst case SIL exercises)
dV/de     = R_int · dI_alt/de ≈ 7–9 V/e instantaneous, through pack IR
```

The gain scales roughly with alternator speed (until deeper flux saturation),
so it keeps growing above cruise — see the margin caveat below.

**Power channel — margin claimed.** Discrete loop `Δe/tick = KP·dt·(P_cmd −
P_batt)`; per-tick loop gain `KP · dt · dP/de ≈ 0.33–0.41` at the calibrated
cruise point ⇒ smooth first-order settling, closed-loop τ ≈ 25–30 ms; **margin
≈ 2.4–3× to the alternation bound (per-tick gain 1) and ≈ 5–6× to divergence
(gain 2)** at that point. Cross-check that this *explains* the SIL finding
rather than re-tuning blind: the pre-fix gain (5e-3 /W/s ≡ 5e-5 /W/tick)
evaluates to ≈1.7–2.1 per tick on the same plant — inside the
alternation/limit-cycle band, exactly the chatter the SIL BMS-step scenario
reproduced and the live unit showed. The −5× reduction moved the loop out of
the oscillatory band for a derivable reason. **Caveat (stated, not hidden):**
because gain grows with RPM, extrapolating the single-point-calibrated
saturation model to an engine top end near 3800 RPM brings the per-tick gain
back toward ~1 — transient chatter at top RPM cannot be excluded from the
model alone. SIL's `rpm_transients` covers idle↔cruise; the high-RPM margin is
bench item 2 below.

**CV channel — margin claimed, and it is thin.** The CV channel integrates a
1 s-damped voltage (§1.4), so the open loop is `KV·G_v / (s · (1 + s·1 s))`
plus ~1.5 ticks of transport delay. At the highest-gain point SIL exercises
(cruise, CV binding, `G_v` ≈ 7–9 V/e): `KV·G_v ≈ 14–18 rad/s` ⇒ crossover
≈3.5–4 rad/s, **derived phase margin ≈ 13–15°** — well below the ≥45°
engineering norm (gain margin is comfortable, ≥20 dB; the −180° point sits
near 15 rad/s where the loop is small). Stated plainly rather than papered
over:

- **what this predicts:** under-damped, decaying ringing (~0.5–1 Hz) when the
  CV clamp first binds at speed — not divergence; the raw-signal OV comparator
  and the 0.01 V/cell clamp band sit outside it;
- **what SIL shows:** convergent CV holds, no sustained limit cycle, no
  overvoltage, across the §8.1 gauntlet including the 5 M-tick soak —
  empirically consistent with stable-but-underdamped;
- **why it ships anyway:** safety does not rest on this margin (see the
  controller-form note); the exposure is a bounded voltage wobble inside the
  clamp band;
- **the honest disposition:** KV = 2.0 is *flagged as likely to need a bench
  retune* — if CV-hold chatter exceeds ±0.005 V/cell sustained on the bench,
  halve KV and/or shorten the CV τ toward 0.5 s. That retune is a field-path
  change → safety gate.

**Anti-windup (all shipped; SIL/§8.4-tested).** (1) One shared integrator +
min-select: the channel not selected cannot wind up, because there is no
second state. (2) Hard clamp `e ∈ [0, 1]` every tick — railing at 1.0 against
the rotor clamp is bounded authority, not windup, and unwinds at gain-rate the
tick the selected error reverses. (3) NaN guard: a non-finite candidate effort
resets to 0 and never latches (the SIL-found shunt-dropout defect, fixed).
(4) `cmd_power_w` resets to 0 in STANDBY and under the §5.2 run-detect gate,
so every resume re-runs the `ramp_w_per_s` soft start.

**Slew.** Ramp-up lives in the *power* domain: `ramp_w_per_s` (10–1000 W/s,
default 100) bounds commanded-power rise; ceiling *decreases* apply
immediately (§3.5 asymmetry — a dip must not be skated over). There is no
separate effort-domain slew limiter in v1: per-tick effort motion is already
bounded by gain × error × dt and the [0, 1] clamp. Stock's fixed ~2 %/s duty
ramp is matched in spirit by configuring `ramp_w_per_s` low, not by an effort
slew.

**Cannot be derived without the bench** — each `bench-pending`, with the
measurement that closes it:

1. **Rotor electrical time constant `τ_r = L/R`.** L has never been measured;
   a claw-pole field winding at 4 Ω plausibly lands anywhere in ~25–250 ms.
   Two claims depend on it: the average-duty model behind the §5.1 clamp
   (needs `τ_r` ≫ the 7 ms PWM period — almost certainly true, and the §0.5
   freewheel path supports it) and the power-loop phase at its ≈33–41 rad/s
   equivalent crossover, which SIL cannot check because its rotor is algebraic
   (`i_f = duty·V/R`, no L). *Measurement:* duty-step the field into the real
   rotor in place, engine off, 5 A field breaker in circuit (M2/M3 test-fw,
   inside its 20 % cap — PROJECT_PLAN §5 in-place decision 2026-07-29); this
   measures the *true* winding L, which the retired dummy-load version never
   could. *Rule:* if `τ_r` > 50 ms, derate KP ×2 before any
   closed-loop CV work.
2. **True alternator gain `dI/de` across the RPM range.** The tanh saturation
   shape is calibrated to a single trace point; both margin caveats above
   inherit that. *Measurement:* the M6 capability sweep (§3.4) at 2–3 held
   RPMs, including near the top of the engine range.
3. **CV-hold quality on hardware** — accepts or retunes KV per the thin-margin
   disposition above. *Measurement:* the M6-entry supervised engine run (moved
   from the M3 exit — PROJECT_PLAN §5 in-place decision 2026-07-29: no
   generation engine-off, so no CV hold to record before then); record V ripple
   and duty spectrum for ≥10 min.

The margins claimed in this section are claims about the **calibrated SIL
plant** and are exactly as good as that calibration — that is the deliberate,
stated limit of their authority.

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
Logging per §6.4. *(Of that list, v1 today implements: the code set below,
live severity, the latching rule, the N2K alert path, the USB/CAN bitfield,
and crash-record freeze-frames. Per-fault first/last-seen/count and non-crash
freeze-frames are unimplemented — discrepancy D8 in §9.3.)*

### 9.1 Fault-code enumeration (authoritative, wire-stable — GH#34, 2026-07-28)

**The wire-visible fault code is `bit index + 1`** of the fault's
`ctrl_fault_bits_t` bit (`control/Inc/control.h`). This is not a new scheme:
it freezes what the firmware already transmits — `n2k_alert_from_telemetry()`
has always put `bit + 1` on the bus as the N2K `alert_id`; the USB telemetry
line, the proprietary fast-packet, and the §7 R2 crash record carry the raw
u32 bitfield (bit = code − 1); the LED (§9.2) blinks the same code.

**Stability contract: wire codes are stable forever.** New faults append at
the next free bit (code 19 = bit 18 landed here, GH#39; next free is code 20 =
bit 19). Codes are never reused, renumbered, or reordered by insertion;
retiring a fault retires its code permanently.
Code 0 = no fault. Renaming a fault's *text* is allowed (text is descriptive);
renumbering is a protocol break and is forbidden.

Severity and field disposition below are the **implemented** classification
(`control/Src/faults.c` masks — fixed in firmware, not config, per §7). Where
implementation disagrees with this spec's intent, the row is annotated and the
discrepancy is listed in §9.3 instead of being silently harmonized.

| Code | Bit | Name (`CTRL_FAULT_*`) | Severity | Field disposition | Latches | v1 detector | Wire text | Blink |
|---|---|---|---|---|---|---|---|---|
| 1 | 0 | `OVERVOLTAGE` | CRITICAL | OPEN | until reset | raw pack V ≥ 3.70 V/cell (`control.c`) | Battery overvoltage - field opened | 1S |
| 2 | 1 | `FIELD_SHORT` | CRITICAL | OPEN | until reset | none — reserved (no field-current sense on v1; D3) | Field driver short - field opened | 2S |
| 3 | 2 | `FIELD_OPEN` | WARN | none | no | none — reserved (D3) | Field circuit open | 3S |
| 4 | 3 | `FIELD_OVERCUR` | CRITICAL | OPEN | until reset | none — reserved (BKIN refuted §0.6 V1+V2; D3) | Field overcurrent - field opened | 4S |
| 5 | 4 | `SELF_OVERTEMP` | WARN | none ⚠(D2) | no | alt hot-spot ≥ 120 °C **or** driver NTC ≥ 120 °C (`control.c`; conflated — D2, not resolved by GH#39) | Regulator over temperature | 5S |
| 6 | 5 | `OVERSPEED` | CRITICAL | OPEN | until reset | none — no overspeed threshold parameter exists (D3) | Alternator overspeed - field opened | 6S |
| 7 | 6 | `SHUNT_OPEN` | FAULT | LIMP (auto-recover) | no | none (D3) | Current shunt open circuit | 7S |
| 8 | 7 | `SHUNT_REVERSED` | FAULT | LIMP (auto-recover) | no | none (D3) | Current shunt reversed | 8S |
| 9 | 8 | `BATT_DTDT` | CRITICAL | OPEN | until reset | none — no rate-of-change (dT/dt) detector implemented (D3); a `batt_temp_c` source may now be configured (GH#40) but this bit does not compute a derivative from it | Battery heating too fast - charge aborted | 9S |
| 10 | 9 | `BATT_LOWTEMP` | FAULT | BLOCK charge (auto-resume) | no | `control.c` ≤ 0 °C — arms once `batt_temp_src` names a channel (GH#40, default `none` = unarmed) | Battery too cold to charge | 1L |
| 11 | 10 | `BATT_HIGHTEMP` | FAULT | BLOCK charge (auto-resume) | no | `control.c` ≥ 55 °C — arms once `batt_temp_src` names a channel (GH#40, default `none` = unarmed) | Battery too hot - charge aborted | 1L+1S |
| 12 | 11 | `LOST_VBAT_SENSE` | FAULT | LIMP (auto-recover) | no | VBat reading NaN (`control.c`) | Lost battery voltage sense - limp home | 1L+2S |
| 13 | 12 | `LOST_BMS` | FAULT | LIMP (auto-recover) | no | none in v1 — CAN-IN deferred (PROJECT_PLAN §1.1) | Lost BMS communication - limp home | 1L+3S |
| 14 | 13 | `IMPLAUSIBLE_SHUNT` | FAULT | LIMP (auto-recover) | no | INA226 §7 R6 error budget spent (`main.c`); the §7 claimed-I-vs-static-V plausibility check itself is unimplemented (D3) | Implausible shunt reading - limp home | 1L+4S |
| 15 | 14 | `THERMAL_DIVERGE` | WARN | none | no | none — §4.1 divergence check unimplemented (D3) | Thermal model divergence | 1L+5S |
| 16 | 15 | `WATCHDOG` | CRITICAL | OPEN | until reset | ≥3 consecutive watchdog/fault boots (§7 R1, `main.c`) | Watchdog reset - field opened | 1L+6S |
| 17 | 16 | `VSUP_IMPLAUSIBLE` | WARN | none (clamp already holds tight) | clears on re-trust | `ctrl_vsup_guard()` distrust active (§5.1.1) | Field supply reading distrusted | 1L+7S |
| 18 | 17 | `BATT_TEMP_REQUIRED` | FAULT | BLOCK charge (auto-resume) | no | `control.c` — `require_batt_temp` set and `batt_temp_c` NaN (GH#40) | Battery temp required - charge blocked | 1L+8S |
| 19 | 18 | `DRIVER_OVERTEMP` | CRITICAL | OPEN | until reset | driver NTC (PA3) ≥ 125 °C (`control.c`, GH#39) — matches stock's single fault threshold (§0.6 V8, `0x4029`); the graduated derate ahead of it (§5.1) is `ctrl_ceilings_t.driver_thermal_w`, an arbitration ceiling, not a fault bit | Driver over-temp - field opened | 1L+9S |

Latching rule (implemented, `control.c`): **OPEN-class codes latch until MCU
reset** (power cycle / watchdog reboot; a deliberate fault-clear command does
not exist yet — when added it must be logged as a config-grade event). LIMP
and BLOCK classes re-evaluate live and self-clear/resume on recovery (the SIL
`temperature` and `sensor_faults` scenarios assert both directions). WARN
classes are live. The §2.4 "alarm latched while stale" rule for BMS protection
alarms is v2, arriving with CAN-IN.

### 9.2 LED blink encoding (contract fixed now; encoder not yet built — D4)

The status LED's *fault layer* blinks the single **highest-severity active
code**, ties broken by lowest code — deterministically the same pick
`n2k_alert_from_telemetry()` makes, so LED, MFD alert, and app always name the
same fault. Pattern: `⌊code/10⌋` long flashes (600 ms) then `code mod 10`
short flashes (200 ms), 300 ms between flashes, 2 s gap between repeats.
Examples: code 5 = 5 short; code 10 = 1 long; code 13 = 1 long + 3 short. No
active fault → the LED shows the state layer (charge-state indication, defined
with the LED driver, not here). Blink patterns inherit §9.1's stability
contract.

### 9.3 Code↔spec reconciliation — every mismatch found (GH#34 pass, 2026-07-28)

Found while producing §9.1; listed rather than papered over.

- **D1 — RETIRED 2026-07-28 (GH#41).** `SHUNT_OPEN` / `SHUNT_REVERSED`
  appeared in no `faults.h` mask, so `ctrl_fault_severity()` returned INFO and
  the disposition was CONTINUE — but §7 lists shunt-open/reversed detection
  as a *fault* ("claimed current with static VBat → fault, not runaway").
  Fixed: both now join `CTRL_FAULT_LIMP_MASK` (the `IMPLAUSIBLE_SHUNT`
  class — a lying current source cannot be trusted). The mask fix was the
  symptom; the real deliverable is `control/test/test_faults.c`'s
  `test_fault_mask_completeness()`, which enumerates every bit position
  against `CTRL_FAULT_ALL_MASK` (`faults.h`) programmatically so a future
  fault bit added without mask membership fails the build instead of
  defaulting to CONTINUE the way D1 did.
- **D2 — `SELF_OVERTEMP` conflates two sensors and under-acts. NOT resolved
  by GH#39 (2026-07-28) — still open, by explicit decision, not oversight.**
  One bit still covers both "alternator hot-spot ≥ 120 °C" and "driver-stage
  NTC ≥ 120 °C"; the wire text ("Regulator over temperature") is still wrong
  for the alternator case; the bit is still WARN/CONTINUE. What DID change:
  the "under-acts" half is fixed for the driver side specifically — GH#39
  added `CTRL_FAULT_DRIVER_OVERTEMP` (§9.1 code 19) as a NEW, driver-only,
  OPEN-class bit at 125 °C, plus a graduated derate ceiling
  (`driver_thermal_w`) ahead of it — so the driver stage is no longer
  unguarded, it just isn't guarded by *splitting* `SELF_OVERTEMP`. The
  originally-intended shape here (append-only: the alternator condition takes
  a new code, `SELF_OVERTEMP` keeps code 5 for the driver stage alone) was
  judged out of scope for the GH#39/#41/#43 batch — it is a bigger change
  (retext the wire string, decide whether the alternator gets its own
  disposition, re-verify every consumer of `SELF_OVERTEMP`) than "give the
  driver stage a working guard," and conflating the two would have made this
  batch harder to review. Left for a dedicated pass.
- **D3 — nine codes have no v1 detector** (2, 3, 4, 6, 7, 8, 9, 13, 15; and
  code 14's in-core plausibility half is also missing). Their codes,
  severities, and texts are fixed *now* so detectors can land later without
  any wire change; §7's protection list stays aspirational for these until
  then.
- **D4 — LED blink codes are promised (§5 outputs table; USER_MANUAL §8) but
  no blink encoder exists** — only the raw PA9/PB14 output drivers (`dio.c`).
  §9.2 fixes the contract; the implementation is outstanding M3/M4 work.
- **D5 — header-comment drift in `control.h`** (fixed in this pass, comments
  only): `FIELD_OPEN` said "WARN/FAULT" where the mask says WARN;
  `FIELD_OVERCUR` said "BKIN territory" — a hardware backstop §0.6 V1+V2
  refuted.
- **D6 — a battery-temp BLOCK is invisible in the state annotation.** While
  blocked (codes 10/11/18) the engine sits in STANDBY with reason `off`, not a
  dedicated reason — the fault bit is the only tell. Violates the "annotated
  with a reason" intent (PROFILE_SPEC §2.1); wants a `blocked` standby reason.
  Cosmetic, but this spec's visibility rule says no silent states.
- **D7 — §4.2's "low-temp Li cutoff = hard fault" reads as latching; the
  implementation (and the SIL `temperature` scenario) auto-resumes** when the
  battery re-enters the window. The §9.1 table fixes auto-resume BLOCK as
  authoritative: a charge *window* is a gate, not a latch — the hazard is
  charging outside the window, which the gate prevents in both directions.
- **D8 — per-fault bookkeeping unimplemented**: first/last-seen, count, and
  non-crash freeze-frames exist nowhere yet; today's surfaces are the live u32
  bitfield (USB JSON, proprietary fast-packet), the highest-severity N2K
  alert, and §7 R2 crash records. M4 telemetry work.

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
