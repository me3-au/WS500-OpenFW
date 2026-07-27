/*
 * bms_rx.h — BMS/DVCC control-in (CAN_INTEGRATION.md §1, §3; deliverable #10,
 * PROJECT_PLAN §1). PURE — no HAL.
 * SPDX-License-Identifier: MIT
 *
 * Design principle (CAN_INTEGRATION.md §1, quoted verbatim because it is the
 * governing constraint for this whole file): "Every battery/charge-control
 * integration is a driver behind a single internal interface: { charge-
 * voltage limit, charge-current limit, SOC, alarms, pre-disconnect }. Vendor
 * quirks never reach the control logic, and adding a new BMS is a driver,
 * not a feature." "Every inbound signal has a declared loss-of-signal
 * fallback — if BMS/DVCC comms drop, the regulator falls back to its own
 * profile limits (and raises a fault), it does not free-run."
 *
 * This file is that interface (bms_snapshot_t) plus ONE vendor driver: the
 * widely-deployed "CAN-BMS" / Victron-compatible frame set that REC, JK and
 * others emit (CAN_INTEGRATION.md §8.1 rows 5–6) — 0x351 (CVL/CCL/DCL), 0x355
 * (SOC/SOH), 0x356 (pack V/I/T), 0x35A (alarms/warnings). These are 11-bit
 * STANDARD CAN identifiers, unlike the 29-bit extended IDs the N2K/RV-C Tx
 * path uses (n2k_encode.h) — Core/Src/can_n2k.c's Rx path and filter banks
 * must admit standard-ID frames too, which is new as of this deliverable.
 *
 * [SPEC-SIGNOFF] / bench-pending: the byte layout and scale factors below are
 * reconstructed from the widely-implemented public documentation of this
 * frame family (the same "CAN-bus BMS" protocol Victron's own GX firmware
 * consumes, which is why REC/JK calling it "Victron-compatible" makes their
 * output interoperable) — there is no REC/JK vendor datasheet or bench BMS
 * checked into or available to this repo to verify against byte-for-byte.
 * Falsify against a real BMS or vendor CAN protocol document before trusting
 * this numerically on a live 48 V pack. The alarm/warning bytes (0x35A) are
 * decoded at BYTE granularity only (any bit set in the byte pair, not which
 * bit) for exactly this reason — individual bit-to-condition assignments
 * within that frame are the least consistently documented part of this
 * family across vendors, and this driver does not need to know WHICH alarm
 * fired to know the safe response (force the CCL ceiling to 0 W).
 *
 * [SPEC-GAP] CVL (charge-voltage limit) is decoded and exposed on
 * bms_snapshot_t (the §1 interface requires it), but has NO consumer in the
 * control core today: ctrl_ceilings_t (control.h) is Watts-only — there is no
 * voltage-domain ceiling in the arbitration min() (arbitration.c) — and the
 * only voltage-domain CV target the engine reads is
 * ctrl_profile_t.cv_target_vcell, a fixed config value with no
 * externally-supplied override input anywhere in control.c (confirmed by
 * inspection: cv_target_vcell is read in control.c, never written from
 * outside config_get()). PROFILE_SPEC_LFP.md §6 says plainly "CVL below the
 * profile CV target simply wins in the min()" — that min() does not exist in
 * code yet. Per this deliverable's scope ("do NOT modify control/ core
 * control logic to accommodate this"), this driver decodes and exposes CVL
 * correctly and stops there; wiring it in needs a control-core change (e.g. a
 * new ctrl_ceilings_t.bms_cvl_vcell field, min()'d against cv_target_vcell
 * inside control.c's CV clamp) that is out of this file's scope and needs its
 * own review. See the deliverable report for the fix shape this suggests.
 *
 * [SPEC-GAP] / TODO(GH#10): pre_disconnect is exposed on bms_snapshot_t (the
 * §1 interface requires it) but this driver always reports it false. No
 * verified bit for "BMS is about to open its contactor" exists within the
 * 0x351/0x355/0x356/0x35A frame set decoded here — CAN_INTEGRATION.md ties
 * pre-disconnect specifically to the Victron BMS row (Lynx Smart / VE.Bus via
 * GX, its own proprietary status frames), not to the REC/JK row this driver
 * implements, and guessing a bit position in 0x35A without a vendor doc to
 * check it against is exactly the kind of hardware-fact invention this
 * project's ground rules forbid. Once a verified source exists: the intended
 * consumer is a soft field ramp-down BEFORE the contactor opens
 * (CAN_INTEGRATION.md §3, PROFILE_SPEC_LFP.md §6) — that is field-drive-path
 * logic and needs its own safety review; this driver's job stops at exposing
 * the flag.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_BMS_RX_H
#define WS500_BMS_RX_H

#include <stddef.h>
#include "control.h"

/* Standard (11-bit) CAN identifiers this driver decodes. CAN_INTEGRATION.md
 * §8.1 rows 5–6 / this deliverable's brief. */
#define BMS_RX_ID_LIMITS   0x351u   /* CVL / CCL / DCL */
#define BMS_RX_ID_SOC      0x355u   /* SOC / SOH */
#define BMS_RX_ID_STATUS   0x356u   /* pack V / I / T (informational only) */
#define BMS_RX_ID_ALARM    0x35Au   /* alarms / warnings */

/* Loss-of-signal timeout per tracked frame group. The 0x35x set is typically
 * broadcast at ~1 Hz by REC/JK-compatible BMS firmware; 3x that gives margin
 * for a couple of missed/collided frames on a shared bus without either
 * flapping the fault or leaving a genuinely-dead link's ceiling trusted for
 * long. [SPEC-SIGNOFF]: no cited vendor datasheet's exact broadcast period is
 * in this repo — engineering choice, not a verified vendor spec. */
#define BMS_RX_SIGNAL_TIMEOUT_MS   3000u

/* Per-signal freshness state machine. NEVER means "this frame type has not
 * been seen since bms_rx_init()" — NOT a fault: a system with no BMS wired at
 * all is a legitimate, common configuration (CAN_INTEGRATION.md §7, "if you
 * use a BMS..."). FRESH/STALE only exist once at least one frame has
 * arrived; the FRESH->STALE transition on a signal that WAS talking and now
 * is not is the actual loss-of-signal event (bms_rx_snapshot()'s `lost`
 * output and each field's own fallback below) — never-seen and went-silent
 * are deliberately distinct states, not collapsed into one "no data" bit. */
typedef enum {
    BMS_SIG_NEVER = 0,
    BMS_SIG_FRESH,
    BMS_SIG_STALE
} bms_sig_state_t;

/* Per-signal tracking: one instance per 0x35x frame this driver decodes. */
typedef struct {
    uint32_t        last_rx_ms;  /* tick of the last accepted frame */
    bms_sig_state_t state;
} bms_sig_t;

/* Persistent driver state — one instance for the life of the firmware, fed
 * frames via bms_rx_frame() and polled once per control tick via
 * bms_rx_snapshot(). */
typedef struct {
    bms_sig_t limits;   /* 0x351 CVL/CCL/DCL */
    bms_sig_t soc;      /* 0x355 SOC/SOH */
    bms_sig_t status;   /* 0x356 V/I/T */
    bms_sig_t alarm;    /* 0x35A alarms/warnings */

    /* Last-decoded raw values, held between frames so a fresher OTHER
     * signal's snapshot poll doesn't blank out a still-valid one. NAN =
     * never decoded (bms_rx_init()). */
    float    cvl_v;          /* charge-voltage limit, pack V */
    float    ccl_a;          /* charge-current limit, A */
    float    dcl_a;          /* discharge-current limit, A — decoded because
                              * it shares 0x351's frame, NOT consumed
                              * anywhere (this is a charge regulator; there
                              * is no discharge path to limit) */
    float    soc_pct;        /* 0..100, or NAN */
    float    soh_pct;        /* 0..100, or NAN — no consumer in the control
                              * core (no SOH field exists); decoded for
                              * completeness/future telemetry only */
    float    pack_v;         /* 0x356, informational only — see file header:
                              * not cross-checked against the local shunt in
                              * this deliverable (future work) */
    float    pack_i_a;       /* 0x356, informational only */
    float    pack_temp_c;    /* 0x356, informational only */
    bool     alarm_active;   /* 0x35A bytes[0:1]: >=1 protection-level bit set */
    bool     warning_active; /* 0x35A bytes[2:3]: >=1 pre-alarm bit set */
} bms_rx_t;

/* The single dialect-neutral interface (CAN_INTEGRATION.md §1). Every vendor
 * driver in this file (today: just the CAN-BMS/REC/JK family) produces
 * exactly this shape; nothing past this struct in the control-core wiring
 * (main.c) ever sees a vendor frame or a vendor ID. */
typedef struct {
    float    ccl_w;          /* charge-current-limit ceiling, WATTS, already
                              * converted at the caller-supplied pack voltage
                              * this cycle (CONTROL_SPEC_NEXTGEN.md §2:
                              * ceilings native in Amps "are converted at the
                              * present measured voltage / RPM each cycle").
                              * CTRL_CEILING_INACTIVE if the limits signal
                              * isn't fresh, the conversion voltage is
                              * unusable, or an active alarm forces 0 W (see
                              * bms_rx.c) — feed straight into
                              * ctrl_ceilings_t.bms_ccl_w. */
    float    cvl_v;          /* charge-voltage limit, pack V. [SPEC-GAP]: no
                              * consumer in the control core today — see this
                              * file's header. NAN if the limits signal isn't
                              * fresh. */
    float    soc_pct;        /* -1.0f if unknown (ctrl_measured_t.soc_pct's
                              * own documented convention, control.h) */
    bool     soc_trusted;    /* true only while the SOC signal is FRESH —
                              * feed straight into ctrl_measured_t.soc_trusted */
    bool     alarm_active;   /* BMS-reported protection-level alarm (any bit);
                              * already reflected in ccl_w above (forced to
                              * 0 W) — exposed separately for telemetry, not
                              * a second thing to act on */
    bool     warning_active; /* BMS-reported pre-alarm warning (any bit);
                              * informational only, does not affect ccl_w */
    bool     pre_disconnect; /* TODO(GH#10) [SPEC-GAP]: always false today —
                              * see this file's header */
    bool     lost;           /* true = the safety-relevant signal group
                              * (limits and/or alarm) WAS fresh and has now
                              * gone stale — caller must OR
                              * CTRL_FAULT_LOST_BMS into ext_faults. Never
                              * true merely because no BMS is wired at all
                              * (BMS_SIG_NEVER, not BMS_SIG_STALE) — see
                              * bms_sig_state_t's doc comment. */
} bms_snapshot_t;

/* Zero-initialize driver state. Every signal starts at BMS_SIG_NEVER (enum
 * value 0), not STALE — "no BMS wired" must never read as a loss-of-signal
 * fault (bms_sig_state_t doc comment). */
void bms_rx_init(bms_rx_t *b);

/*
 * Feed one standard-ID (11-bit) CAN frame to the driver. Ignores any id this
 * driver doesn't decode (silently — the same bus carries N2K/RV-C extended-ID
 * traffic this driver has no business looking at) and any frame too short
 * for the fields its id claims (malformed/short frames are dropped whole,
 * never partially decoded into a mix of new and stale field values).
 * Updates the matching signal's freshness timer/state only on acceptance.
 *
 * now_ms: caller's current tick (HAL_GetTick() in Core/). Anchoring the
 * timestamp here, at the moment of reception, rather than computing "age" at
 * decode time, is what makes bms_rx_snapshot()'s staleness check
 * wraparound-safe over the full uint32_t range (n2k_sched.c / rvc_sched.c's
 * own due()/rvc_rbm_defer() idiom, reused below).
 */
void bms_rx_frame(bms_rx_t *b, uint32_t now_ms, uint32_t id,
                  const uint8_t *data, size_t len);

/*
 * Age out any signal whose last accepted frame is older than
 * BMS_RX_SIGNAL_TIMEOUT_MS and produce the dialect-neutral snapshot
 * (CAN_INTEGRATION.md §1). Call once per control tick, after draining
 * whatever CAN frames arrived this cycle into bms_rx_frame().
 *
 * vbat_pack_v: the SAME battery-pack voltage the control core is using this
 * cycle (ctrl_measured_t.vbat_pack_v) — CCL (Amps) is converted to ccl_w
 * (Watts) at THIS voltage, per CONTROL_SPEC_NEXTGEN.md §2's "converted at the
 * present measured voltage" rule. Deliberately NEVER the field-supply
 * voltage (ctrl_measured_t.v_supply_v) — on this hardware that is a
 * different, ~12 V-class domain from the 48 V pack (PROJECT_PLAN §0.6);
 * feeding it in here by mistake would scale the ceiling by roughly 4x in the
 * unsafe (too-permissive) direction. NAN or <=0 vbat_pack_v (lost VBat sense)
 * yields ccl_w = CTRL_CEILING_INACTIVE — the same defensive idiom
 * ctrl_limits_apply() already uses (limits.c: `v_ok = vbat_pack_v > 0.0f`,
 * false for NaN by IEEE 754 comparison semantics, no isnan() needed). This
 * driver does not need a second opinion on a lost VBat sense: the engine's
 * own CTRL_FAULT_LOST_VBAT_SENSE disposition (LIMP) already covers the
 * charging system when that happens.
 */
void bms_rx_snapshot(bms_rx_t *b, uint32_t now_ms, float vbat_pack_v,
                     bms_snapshot_t *out);

#endif /* WS500_BMS_RX_H */
