/*
 * rvc_encode.h — pure RV-C DGN encoders over the telemetry snapshot.
 * SPDX-License-Identifier: MIT
 *
 * The second Tx dialect (CAN_INTEGRATION.md §0, §8; PROJECT_PLAN §1 row 10a):
 * a Victron Cerbo's VE.Can port runs EITHER the N2K profile OR the RV-C
 * profile, never both, so an RV owner only sees this regulator if it also
 * speaks RV-C. Maps the SAME dialect-neutral ctrl_telemetry_t
 * (control/Inc/telemetry.h) the N2K encoder (n2k_encode.h) reads — the
 * control core stays untouched, exactly like the N2K half.
 *
 * PURE — no HAL, no allocation, fixed-size outputs, C11 float only.
 *
 * ---- Frame/identifier reuse (why this file has no frame struct of its
 * own) --------------------------------------------------------------------
 * RV-C is J1939-based with the SAME 29-bit identifier composition N2K uses:
 * 3-bit priority, an 18-bit PGN/DGN field, 8-bit source address
 * (CAN_INTEGRATION.md §8: "Both are J1939/CAN-based"). n2k_can_id() already
 * masks its `pgn` parameter to 17 bits (0x1FFFF) before shifting — which is
 * wide enough to carry RV-C's Data-Page-1 DGNs (every DGN below is in the
 * 0x1Fxxx/0x17xxx range, i.e. DP=1) with no code change. So this module
 * reuses n2k_frame_t and n2k_can_id()/n2k_can_id_pgn()/n2k_can_id_src()
 * as-is rather than duplicating them — the identifier math is genuinely
 * dialect-neutral, only the DGN values and payload layouts differ.
 *
 * RV-C also has NO fast-packet transport: every DGN below fits one 8-byte
 * frame (confirmed against a real product's own DGN table, cited per-DGN
 * below), unlike N2K which needs multi-frame fast-packet for anything over
 * 8 bytes. n2k_fp_split() is therefore never called from this file.
 *
 * ---- Sourcing / honesty note ---------------------------------------------
 * The RV-C spec text (rv-c.com, RVIA) is NOT checked into this repo — same
 * situation n2k_encode.c documents for N2K. Facts below are corroborated
 * against PUBLIC, PRIMARY sources: a real shipped RV-C product's own DGN
 * reference guide (Xantrex Freedom SW-RVC RV-C DGN Reference Guide,
 * 976-0452-01-01 Rev B, Sep 2022 — a device that actually ships and talks to
 * real RV-C networks) for DGN numbers, broadcast intervals, field order and
 * a couple of concrete resolution numbers; a public RV-C DGN YAML
 * reproduction (linuxkidd/rvc-monitor-py, etc/rvc/rvc-spec.yml) for
 * confirming byte offsets/widths independently; and a Victron Cerbo GX RV-C
 * appendix for DGN hex/priority cross-checks. Where a resolution/offset
 * constant is NOT stated verbatim in either source, it is marked
 * [SPEC-SIGNOFF] with the concrete falsification step (a real spec-text
 * read or a bus capture against a real RV-C network).
 *
 * Wire conventions (differ from N2K in several places — see each site):
 *   - little-endian multi-byte fields (same as N2K);
 *   - "not available" sentinels are RV-C's own, NOT N2K's: top-of-range for
 *     every bit width — 2-bit fields use 0b11 (not N2K's per-type sentinel
 *     scheme), 4-bit fields use 0xF, u8 uses 0xFF, u16 uses 0xFFFF, u32 uses
 *     0xFFFFFFFF. No signed (two's-complement) fields are used anywhere
 *     below — RV-C represents signed quantities via UNSIGNED fields with a
 *     documented zero-offset instead (see the current fields below), unlike
 *     N2K's s16 sentinel (0x7FFF);
 *   - out-of-range finite values SATURATE one code below the "not
 *     available" sentinel (same "a pegged gauge is more honest than a blank
 *     one" policy n2k_encode.c already established — reused here for
 *     in-house consistency, not because RV-C's spec text is confirmed to
 *     say the same thing) [SPEC-SIGNOFF];
 *   - RV-C's own unit scalings are NOT N2K's (N2K: 0.01 V/bit, 0.1 A/bit).
 *     Per-field resolutions are cited at each encoder below.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_RVC_ENCODE_H
#define WS500_RVC_ENCODE_H

#include <stdint.h>
#include <stddef.h>
#include "telemetry.h"
#include "n2k_encode.h"   /* n2k_frame_t, n2k_can_id()/_pgn()/_src() — reused
                           * as-is, see the file header above */

/* "Data not available" sentinels (RV-C convention — top of range for every
 * bit width; NOTE this differs from N2K's per-type sentinel scheme, e.g.
 * N2K's signed 16-bit fields use 0x7FFF, RV-C has no such field here). */
#define RVC_NA_U8    0xFFu
#define RVC_NA_U16   0xFFFFu
#define RVC_NA_U32   0xFFFFFFFFu
#define RVC_NA_BIT2  0x3u
#define RVC_NA_BIT4  0xFu

/* RV-C DGNs this module encodes (CAN_INTEGRATION.md §8 names all of these).
 * Hex values cross-checked against a real shipped product's own DGN table
 * (Xantrex Freedom SW-RVC RV-C DGN Reference Guide 976-0452-01-01 Rev B) and
 * an independent public YAML reproduction (linuxkidd/rvc-monitor-py
 * etc/rvc/rvc-spec.yml) — both agree on every value below, so these DGN
 * numbers are treated as settled facts, not [SPEC-SIGNOFF].
 *
 * Structural note worth naming: every RV-C DGN below (and RV-C's address
 * claim/admin DGNs — see rvc_sched.h and Core/Src/can_n2k.c) falls in the
 * 0x17000-0x1FFFF range, i.e. Data Page 1 (bit 16 of the PGN field set).
 * Standard J1939/N2K administrative PGNs (address claim 60928, ISO request
 * 59904) are Data Page 0 (0x00xxxx). RV-C appears to deliberately reserve
 * the whole DP=1 page for itself — plausibly so it can share a physical bus
 * with a DP=0 J1939 chassis network without PGN collisions. This has a
 * direct consequence for address claim — see rvc_sched.h's file header. */
#define RVC_DGN_DC_SOURCE_STATUS_1        0x1FFFDu   /* instance, device
                                                       * priority, dc voltage,
                                                       * dc current; 500 ms */
#define RVC_DGN_DC_SOURCE_STATUS_2        0x1FFFCu   /* + source temp, SOC,
                                                       * time remaining; 500 ms */
#define RVC_DGN_DC_SOURCE_STATUS_3        0x1FFFBu   /* SOH, capacity
                                                       * remaining, relative
                                                       * capacity, AC ripple —
                                                       * NOT periodically
                                                       * broadcast on the
                                                       * reference product
                                                       * (request/response
                                                       * only); see rvc_sched.h */
#define RVC_DGN_CHARGER_STATUS            0x1FFC7u   /* charge V/A, state;
                                                       * 5000 ms (reference
                                                       * product's own cadence) */
/* CHARGER_STATUS_2 is NOT in the numbered RV-C spec section list (absent
 * from the Xantrex reference guide's own supported-DGN table) — it is a
 * cross-vendor convergence documented independently by a second real
 * implementation (thomasonw/RV-C, a NMEA2000-library RV-C add-on: "Addition
 * to the RV-C specification") AND by Victron's own Cerbo GX RV-C appendix
 * (same hex, same "DC voltage, current" description, "charger priority
 * aligns with DC source priority"). Two independent real implementations
 * agreeing on the DGN number and field set makes this worth emitting (it is
 * our only single-frame source of DC voltage+current+temperature together
 * from a "charger" identity), but it is NOT confirmed against the base
 * RV-C spec text itself [SPEC-SIGNOFF]. */
#define RVC_DGN_CHARGER_STATUS_2          0x1FEA3u

/* Default bus-arbitration priority for these DGNs. Neither reference source
 * states RV-C's own 3-bit CAN priority convention explicitly (only the
 * broadcast INTERVAL is documented); 6 is the same default N2K/J1939 uses
 * for periodic status PGNs (n2k_encode.c's PRI_STATUS) and is the universal
 * J1939-family fallback absent contrary info [SPEC-SIGNOFF]. */
#define RVC_PRI_STATUS   6u

/*
 * ISO NAME Industry Group for RV-C devices (n2k_addrclaim.h's
 * n2k_name_fields_t.industry_group — reused dialect-neutrally,
 * Core/Src/can_n2k.c builds a second NAME with this value for the RV-C
 * identity instead of N2K's own N2K_PROP_INDUSTRY=4/marine).
 *
 * This is 0 (Global — "all industries"), NOT 4 (marine, N2K's own value)
 * and NOT 5 either, despite that being a plausible first guess ("RV-C =
 * on-highway/RV, industry group 5" — the standard J1939-81 Industry Group
 * table lists 1=On-Highway, 5=Industrial/Process Control/Stationary; RV-C
 * doesn't map cleanly onto either). VERIFIED against a real shipped RV-C
 * product's own address-claim wire table (Xantrex Freedom SW-RVC RV-C DGN
 * Reference Guide 976-0452-01-01 Rev B, §3.3.2 "Address Claimed
 * ADDRESS_CLAIM": "Compatibility Field | Industry Group - 0"). This is
 * primary evidence from a real device's actual on-wire NAME, not a guess —
 * treated as settled for THIS deliverable, though still worth confirming
 * against the base RV-C spec text (not in this repo) or a bus capture
 * before it is load-bearing for real interop [SPEC-SIGNOFF, but the
 * strongest-evidenced one in this file]. */
#define RVC_INDUSTRY_GROUP   0u

/*
 * RVC_DGN_CHARGER_STATUS (single frame, 8 B; byte layout confirmed against
 * both reference sources — Xantrex's field-name order and
 * linuxkidd/rvc-monitor-py's byte offsets agree):
 *   0    instance
 *   1-2  charge voltage      u16 LE, 0.05 V/bit  [SPEC-SIGNOFF: resolution
 *        not stated by either primary source for THIS field; reused from
 *        the confirmed 0.05 V/bit convention documented for the related
 *        vendor-extension field below (CHARGER_STATUS_2's DC voltage,
 *        thomasonw/RV-C README: "0-3212.5V in 50mV steps") since both are
 *        RV-C voltage fields and no second resolution is documented anywhere]
 *   3-4  charge current      u16 LE, 0.05 A/bit, UNSIGNED (no offset — a
 *        charger only ever pushes current; see below) [SPEC-SIGNOFF]
 *   5    charge current percent of max   u8, 0.4 %/bit (J1939 "Type P"
 *        percent convention RV-C's diagnostic messages already borrow
 *        elsewhere — e.g. DM_RV reuses raw J1939 SPN/FMI) [SPEC-SIGNOFF]
 *   6    operating state     u8 enum (0 undefined, 1 do not charge, 2 bulk,
 *        3 absorption, 4 overcharge, 5 equalize, 6 float, 7 constant
 *        voltage/current) — per linuxkidd/rvc-monitor-py
 *   7    bit0-1 default state on power-up (2b, NA=0b11 — not configurable
 *        yet, TODO(GH#10) same concession n2k_encode.c's 126998 heartbeat
 *        makes for missing Rx-driven config)
 *        bit2-3 auto recharge enable (2b, NA=0b11, same reason)
 *        bit4-7 force charge (4b, 0-cancel,1-bulk,2-float,14-no change,
 *        15-undefined — CHARGER_COMMAND's own enum, Xantrex §6.20.11;
 *        we report 15/undefined since Rx isn't wired up)
 *
 * `t->amps_batt` is signed (+ charging); this field cannot represent
 * negative, so a net-discharge reading (house loads exceeding alternator
 * output) clamps to 0 rather than wrapping — a charger reporting "0 A
 * charge current" while the battery is actually discharging is honest
 * within this field's own definition (it is not a net-battery-current
 * field, see CHARGER_STATUS_2 / DC_SOURCE_STATUS_1 for the signed value).
 *
 * `charge_current_percent_of_max` is approximated from field_effort (duty
 * ratio), not a measured percent of a configured current ceiling — the v1
 * snapshot has no such ceiling-relative figure [SPEC-SIGNOFF].
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_charger_status(const ctrl_telemetry_t *t, uint8_t instance,
                              uint8_t src, n2k_frame_t *out);

/*
 * RVC_DGN_CHARGER_STATUS_2 (single frame, 8 B; vendor-convergence DGN, see
 * the macro's doc comment above). Byte order INFERRED from the
 * thomasonw/RV-C README's field list order (Instance, DC Source Instance,
 * Device Priority, DC Voltage, DC Current, Temperature) — the README gives
 * value ranges but not explicit byte offsets, so the layout below is our
 * best reconstruction, not independently byte-confirmed [SPEC-SIGNOFF]:
 *   0    instance
 *   1    dc source instance (0 — single-bank device, no multi-instance story)
 *   2    device priority (the SAME value rvc_rbm_t uses for RBM arbitration,
 *        passed in so this encoder stays pure/parameter-driven)
 *   3-4  dc voltage    u16 LE, 0.05 V/bit — thomasonw/RV-C: "0-3212.5V in
 *        50mV steps"
 *   5-6  dc current    u16 LE, 0.05 A/bit, offset-encoded: raw 0x7D00
 *        (32000) = 0 A, so amps = (raw - 32000) * 0.05 — thomasonw/RV-C:
 *        "-1600-1612.5A in 50mA steps, 0x7D00 = 0A" (this field IS signed,
 *        unlike CHARGER_STATUS's plain current, because it doubles as a
 *        pass-through/inverting reading on the reference product; we only
 *        ever report ≥0 in practice but the encoding supports negative)
 *   7    temperature  u8, 1 °C/bit, offset -40 (temp_c = raw - 40) —
 *        thomasonw/RV-C: "-40-210°C in 1°C steps". Mapped from
 *        t->alt_temp_c (this device's own thermal state — the closest
 *        analog to "the charger unit's temperature" the v1 snapshot has;
 *        t->batt_temp_c is a plausible alternate reading and the DGN's own
 *        intent here is unconfirmed) [SPEC-SIGNOFF]
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_charger_status_2(const ctrl_telemetry_t *t, uint8_t instance,
                                uint8_t dc_source_instance,
                                uint8_t device_priority, uint8_t src,
                                n2k_frame_t *out);

/*
 * RVC_DGN_DC_SOURCE_STATUS_1 (single frame, 8 B; byte layout confirmed by
 * BOTH reference sources independently):
 *   0    instance
 *   1    device priority — the RBM arbitration value (rvc_sched.h); this is
 *        the field other nodes read to decide whether WE are the winning
 *        master for this DC-source instance, and the field rvc_rbm_rx()
 *        reads from OTHER nodes' broadcasts of this same DGN
 *   2-3  dc voltage   u16 LE, 0.05 V/bit (same resolution as the charger
 *        voltage fields above — RV-C appears to use one voltage convention
 *        throughout) [SPEC-SIGNOFF: not independently re-confirmed for this
 *        specific field, reused from the corroborated CHARGER_STATUS_2 value]
 *   4-7  dc current   u32 LE, 1 mA/bit, offset-encoded: raw 2 000 000 000 =
 *        0 A, so amps = (raw - 2 000 000 000) * 0.001 [SPEC-SIGNOFF: the
 *        32-bit width and 1 mA/bit resolution are corroborated (both
 *        reference sources agree the field is a 4-byte "A"-unit value,
 *        matching the task brief's own recollection of "commonly a 32-bit
 *        1 mA-per-bit offset-encoded field"); the EXACT offset constant
 *        (2 000 000 000) is NOT stated verbatim in either source — it is
 *        the widely-cited value for this exact DGN across public RV-C
 *        decoder write-ups, reproduced here from general familiarity, not
 *        from primary text. Falsification: a real spec-text read of RV-C
 *        §6.5.2, or a bus capture of a real DC_SOURCE_STATUS_1 frame from a
 *        known-good device (e.g. a Victron SmartShunt in RV-C mode) at a
 *        known current.]
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_dc_source_status_1(const ctrl_telemetry_t *t, uint8_t instance,
                                  uint8_t device_priority, uint8_t src,
                                  n2k_frame_t *out);

/*
 * RVC_DGN_DC_SOURCE_STATUS_2 (single frame, 7 of 8 B used; layout confirmed
 * by both reference sources):
 *   0    instance
 *   1    device priority
 *   2-3  source temperature  u16 LE, 0.03125 °C/bit, offset -273 °C (the
 *        classic J1939 SPN temperature convention — SAE J1939-71 Type T:
 *        temp_c = raw * 0.03125 - 273, range -273..+1735 °C, NA=0xFFFF —
 *        RV-C's own spec text is not confirmed to reuse this verbatim but
 *        it is the standard J1939-family temperature encoding and RV-C is
 *        explicitly J1939-derived) [SPEC-SIGNOFF]. Sourced from
 *        t->batt_temp_c: this DGN is battery-instance data (DC_SOURCE, not
 *        charger), so battery temperature is the correct field, unlike
 *        CHARGER_STATUS_2's own temperature above.
 *   4    state of charge   u8, 0.4 %/bit (same convention as
 *        rvc_encode_charger_status's percent field) [SPEC-SIGNOFF]. The v1
 *        ctrl_telemetry_t snapshot carries NO SOC field — this always
 *        transmits RVC_NA_U8, same "not in the snapshot yet" honesty
 *        n2k_encode_127506 already applies to N2K's own SOC/SOH fields.
 *   5-6  time remaining    u16 LE, NA always — not in the v1 snapshot,
 *        also not implemented on the reference product itself (Xantrex
 *        marks it "x = not implemented")
 *   7    reserved/pad, transmitted 0xFF (RV-C convention mirrors J1939's
 *        "unused bits/bytes transmit as 1s")
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_dc_source_status_2(const ctrl_telemetry_t *t, uint8_t instance,
                                  uint8_t device_priority, uint8_t src,
                                  n2k_frame_t *out);

/*
 * RVC_DGN_DC_SOURCE_STATUS_3 (single frame, 8 B). The v1 snapshot has NO
 * state-of-health, capacity, relative-capacity, or ripple data, and the
 * reference product itself does not periodically broadcast this DGN at all
 * — it is request/response only, "sent with no data on all fields" (Xantrex
 * reference guide, verbatim). rvc_sched.c therefore never schedules this
 * DGN (see its file header); this encoder exists for completeness/testing
 * and always emits the all-not-available payload the reference product
 * itself sends:
 *   0 instance   1 device priority
 *   2 state of health (NA)         3-4 capacity remaining (NA)
 *   5 relative capacity (NA)       6-7 AC RMS ripple (NA)
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_dc_source_status_3(uint8_t instance, uint8_t device_priority,
                                  uint8_t src, n2k_frame_t *out);

#endif /* WS500_RVC_ENCODE_H */
