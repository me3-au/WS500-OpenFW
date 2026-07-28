# WS500-OpenFW — User Manual & Algorithm Explanation

> **Status:** draft. Describes the v1 control design (authoritative specs:
> `CONTROL_SPEC_NEXTGEN.md`, `PROFILE_SPEC_LFP.md`). Inner-loop tuning values are
> still being finalized on the bench.

This manual explains **how the regulator thinks** — so that when you look at the
display and see a number, you understand why. The design philosophy is
*transparency*: there are no hidden modes. At every moment the regulator tells you
the one thing that is limiting output.

---

## 1. The big idea: two stages, one decision

Older regulators had a stack of stages (bulk/absorption/float/equalize…) and a pile
of special modes (small-alt, half-power, RPM tables). This firmware is **LiFePO4-first**
and deletes all of that. There are **two stages** and **one decision**.

### The two stages

- **CHARGE (BULK):** push power into the battery — as much as everything allows —
  up to the profile's target voltage.
- **REST (FLOAT):** the battery is charged; hold a gentle maintenance voltage, or
  stop entirely, per your profile.
- (**STANDBY** is the off state: engine not running, warming up, resting, or faulted.)

"Absorption" isn't a separate stage — it's simply **CHARGE limited by voltage**
(the display says *Bulk (Voltage)*).

### The one decision: minimum of the ceilings

Output power = the **lowest** of every active limit, in **Watts**:

```
output = min( profile power, thermal limit, BMS limit, battery C-rate,
              wiring ampacity, alternator rating, belt, engine budget, your cap )
```

Whichever is lowest **binds**, and the regulator **always tells you which one**.
This single mechanism replaces every legacy "mode" — there's nothing hidden to name.
If your output is 1800 W and the display says *Thermal*, the alternator is as hot as
you've allowed and everything else has more room.

*In **v1**, the active limits are: profile power, thermal, battery C-rate, wiring,
alternator, belt (if RPM is available from the stator), and your cap. **BMS limit is
v2 only** (via CAN). Engine budget becomes active when RPM is available.*

## 2. Voltages are per-cell

Every voltage is set in **volts per cell (V/cell)**; the pack value is `V/cell ×
cells` (4S/8S/16S for 12/24/48 V). One number is correct at every system voltage.
Named **primitives** (edited once, used everywhere):

| Primitive | V/cell | 12 V | 24 V | 48 V | Role |
|---|---|---|---|---|---|
| Bulk | 3.60 | 14.4 | 28.8 | 57.6 | Full-charge target |
| Bulk Conserve | 3.50 | 14.0 | 28.0 | 56.0 | Longevity target |
| Float | 3.40 | 13.6 | 27.2 | 54.4 | Normal hold |
| Float Conserve | 3.35 | 13.4 | 26.8 | 53.6 | Low hold (lets solar win) |
| Limp | 3.30 | 13.2 | 26.4 | 52.8 | Safe-mode hold |

## 3. Profiles — presets over the same engine

A profile is **data, not a mode**: it picks a CHARGE target, an exit rule, and a REST
behavior. Switching profiles never adds states.

| # | Profile | CHARGE | Exit | REST | Intent |
|---|---|---|---|---|---|
| 1 | Bulk, Float Norm | Bulk | tail current | hold Float | Default; engine carries loads |
| 2 | Bulk, Float Low *(Solar Priority)* | Bulk | tail | hold Float Conserve (low cap) | Solar wins finish/loads |
| 3 | Bulk, Solar Finish | Bulk | at voltage entry | off | Alternator lifts, solar absorbs |
| 4 | Bulk Conservative, Float Low | Bulk Conserve | tail | hold Float Conserve | Max cycle life |
| 5 | Bulk, Off | Bulk | tail | off | Charge then stop |
| 6 | **Limp Home** | Limp | — | hold Limp (25 % cap) | Safe mode + auto fallback |
| 7 | Fast Charge *(future)* | curve | curve | off | Reserved |

## 4. How CHARGE ends and REST reverts

- **Charged (CHARGE → REST):** the **primary rule is voltage + time** — once the pack
  has been **held at the target voltage for a set time** (default ~15 minutes at 3.6 V/
  cell), the battery is full. This needs no current sensor and works regardless of shunt
  placement, so it's the reliable default. *Optional faster exits* also apply when the
  data is available: **tail current** (battery-side current falls low while at target),
  a **SOC target** (trusted SOC source), **Solar Finish** (exit at target-voltage entry),
  and a **duration backstop**. Tail current is an optimization, not a requirement.
- **Revert (REST → CHARGE):** voltage sags below a per-cell threshold (held), or SOC
  drops, or a net amount of Ah has been drawn. Hold-times prevent flapping.
- **Skip re-charge on startup (optional).** If the battery is already full when the engine
  starts — e.g. a bank floating on solar — the regulator can **skip CHARGE and go straight
  to REST** instead of re-absorbing on every power cycle. Two independent triggers, off by
  default: by **resting voltage** ("skip if V/cell ≥ your solar-float setting") and/or by
  **SoC** ("skip if trusted SoC ≥ y %"). Evaluated once at start, on the resting voltage
  before any field is applied.

## 5. The ceilings, explained

- **Thermal governor (predictive).** The alternator is modeled as a thermal system;
  the regulator watches where temperature is *heading* (`T + τ·dT/dt`) and eases power
  **before** the hot-spot reaches target — no overshoot-then-slash. A cold alternator
  gets a safe *sprint* of extra power (thermal mass), tapering as it heats.
- **Hardware limit set** (commissioned once): **battery C-rate**, **wiring ampacity**,
  **alternator rating** — native units, converted to Watts at the present voltage.
- **BMS ceiling** *(v2):* a battery's charge-current/voltage limit (DVCC / CAN BMS) is just
  another input to the min() — see `CAN_INTEGRATION.md`. *This is available in v2; v1 runs
  on your commissioned limits only.*
- **Engine budget & belt** *(needs an RPM source):* a Watts-vs-RPM curve keeps engine
  load and belt torque in bounds; near-zero at idle, rising with speed.

### Who's in charge: your BMS, your GX, and the regulator

Short answer: **nobody is "in charge". The most cautious limit wins, whoever set it.**

There's no pecking order where the BMS overrules the regulator or a Cerbo overrules
the BMS. Every limit that's currently valid applies at the same time, and the lowest
one decides. That has one consequence worth understanding, because it's the whole
safety argument:

> **Anything outside the regulator can only ever make it charge *less*. Nothing
> outside it can make it charge *more*.**

A BMS saying "60 A max" lowers your output if 60 A is the lowest limit going. A BMS
saying "300 A is fine" changes nothing — your alternator rating, wiring, thermal
limit and rotor protection are all still there, and one of them will be lower. Same
for a charge-voltage limit: a BMS can pull the target voltage *down* below your
profile's, never push it up.

If a device that was talking to the regulator goes quiet, the regulator does **not**
keep obeying the last thing it heard, and it does **not** start charging freely. It
drops back to its own profile and hardware limits, tells you it lost the link, and
carries on more conservatively. Stale information is treated as no information.

On top of all that sit a few things that aren't limits at all — they're stops. A
battery protection alarm, a fault, a stationary engine, and above all the **rotor
protection** of §6. The rotor clamp is the floor beneath everything: no setting, no
BMS, and no network message can widen it.

**A note if you run a Victron GX / Cerbo.** DVCC is Victron's system-level scheme, and
a GX distributes those limits to *Victron* equipment. It does not send them to
third-party devices, and this regulator is one. So a charge-current limit you type
into the GX screen does **not** reach the regulator. Set your install limits on the
regulator itself rather than assuming the GX holds it back.

> **Current status:** in **v1 the regulator ignores the CAN bus entirely for control
> purposes** — it transmits telemetry and receives nothing that can affect charging.
> Everything above about BMS and DVCC inputs describes **v2**. In v1 the limits in
> force are exactly the ones you commissioned, plus the regulator's own thermal, rotor
> and fault protections.

## 6. Rotor protection (the 48 V / 12 V-rotor problem)

Most 48 V alternators use a **12 V rotor**. The regulator continuously computes a
**duty-cycle clamp = rotor rating ÷ supply voltage** (≈ 21–25 % at 48 V) so the rotor
is never over-driven — and it does this in *normalized effort* so control resolution
is the same on 12 V and 48 V. On this hardware there is **no field-current sensor**, so
the internal driver-stage temperature acts as a backstop and the display honestly
reports "rotor model: unavailable." Over-driving is possible only via an explicit,
logged, plain-language override.

## 7. RPM & tach (optional)

RPM is **never required**. With no RPM source the regulator runs a flat power cap +
the thermal loop. When RPM *is* available, it unlocks the engine/belt curves, a real-RPM
**synthesized tach output**, and overspeed protection. Calibration is one learned
constant — no pole counts or pulley ratios to type.

**In v1**, the sole RPM source is the **stator signal** — the regulator measures alternator
frequency and converts it to engine RPM. *CAN engine RPM* (from a compatible engine computer
or gateway) is available in **v2**.

## 8. Faults & Limp Home

Faults carry a **severity** (INFO / WARN / FAULT / CRITICAL) and a **disposition**:

- **CRITICAL → field open:** overvoltage, field short/over-current, overspeed,
  watchdog, battery thermal runaway. Charging stops immediately — the field is cut at
  the timer's output-enable, ahead of the control loop, so it happens even if the
  regulation logic itself is what went wrong.
- **Recoverable → Limp Home:** lost voltage sense, implausible shunt (and **lost BMS comms** in v2).
  Rather than quitting, the regulator drops to **FLOAT at the Limp voltage** with a
  reduced power cap — safe, minimal-stress, still useful.
- **Charge-window blocks:** battery too cold/hot → charging pauses until it's back in
  range. *(v1 has no battery-temperature input; see §9 for what's available.)*

The **status LED** blink-codes the active fault; the app shows plain-language text and
a remedy. Nothing degrades silently — the active mode is always on the display.

## 9. Sensors it uses

- **Battery voltage** — Kelvin sense pair (mandatory-grade for Li CV).
- **Current + local bus voltage** — one INA-class shunt monitor, battery- or
  alternator-side (you declare which).
- **Alternator temperature** (external NTC probe) and **driver-stage temperature** (internal,
  rotor backstop). *(In v1, these two local sensors protect the alternator and regulator;
  battery temperature for charge-window gating is a **v2** feature via CAN/BMS.)*
- **Stator** frequency for RPM (optional; in v1, the stator signal is the sole engine-RPM
  source).

## 10. What "resistance & OCV learning" buys you (v1, where sensors allow)

The regulator can separate **wiring/IR voltage rise** from true battery voltage, so
CV targeting and tail exits act on what the *battery* is doing — and a **rising
connection resistance** over time raises a *"check your lugs"* warning before a
terminal gets hot. Brief zero-field "micro-rests" sample open-circuit voltage to keep
a state-of-charge estimate honest even without a BMS.

---

*For the exact parameters, thresholds, and state-transition math, see the engineering
specs `CONTROL_SPEC_NEXTGEN.md` and `PROFILE_SPEC_LFP.md`.*
