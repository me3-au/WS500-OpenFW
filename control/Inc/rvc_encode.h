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
 * The RV-C spec text is not vendored into this repo (copyright — it is a
 * public download from rvia.org), but as of the 2026-07-28 network-identity
 * spec-closure review it HAS been read: every scaling/layout marker below
 * was disposed against RVIA, "RV-C Specification Full Layer", rev.
 * 2025-07-31. Citations of the form "Table 5.3" / "§6.20.8b" refer to THAT
 * document — primary text, not vendor guides. The original corroborating
 * sources remain cited where they contributed: the Xantrex Freedom SW-RVC
 * DGN reference guide (976-0452-01-01 Rev B), the linuxkidd/rvc-monitor-py
 * rvc-spec.yml + its unit-conversion code, thomasonw/RV-C, and the Victron
 * Cerbo GX RV-C appendix. Items only a real bus can settle are marked
 * bench-pending (GH#32). NOTE: three encodings were found WRONG in the
 * process (charge-current offset, percent scale, DC current sign) — the
 * per-field notes below carry the verdicts; the encoder fixes are
 * TODO(GH#32) and BLOCK enabling RV-C Tx on a real bus.
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
 *     available" sentinel — signed off 2026-07-28: RV-C §3.2.3 names that
 *     top-below-NA code "Error / Out of Range" for all data types, so a
 *     pegged reading lands on the spec's own out-of-range code (and stays
 *     consistent with n2k_encode.c's same policy for N2K);
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
 * Structural note worth naming: every APPLICATION DGN below falls in the
 * 0x17000-0x1FFFF range, i.e. Data Page 1 (bit 16 of the PGN field set).
 * RV-C's NETWORK-MANAGEMENT messages, by contrast, stay on the inherited
 * J1939 Data Page 0 PGNs — address claim EE00h/60928, request EA00h/59904
 * (spec §3.3, settled 2026-07-28; see rvc_sched.h's file header) — the
 * same split NMEA 2000 itself uses (its application PGNs are DP=1 too).
 * So "RV-C reserves DP=1" holds for application data only and implies
 * nothing about the address claim. */
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
/* CHARGER_STATUS_2: RESOLVED (2026-07-28) — in the official spec after
 * all: §6.20.9, DGN 1FEA3h ("communicates the linkage of a given charging
 * device instance with its associated DC battery instance"), normal
 * broadcast gap 500 ms (Table 6.20.9a). The earlier "vendor-convergence,
 * not in the base spec" caveat reflected older revisions (thomasonw/RV-C
 * described it as an addition to the spec); the 2025-07-31 revision
 * defines it outright, and Table 6.20.9b confirms the byte layout
 * reconstructed below field-for-field. */
#define RVC_DGN_CHARGER_STATUS_2          0x1FEA3u

/* Bus-arbitration priority for these DGNs: RESOLVED (2026-07-28) — the
 * spec lists "Default priority 6" for every DGN this module emits (Tables
 * 6.5.2a/6.5.3a/6.5.4a/6.20.8a/6.20.9a). Matches the J1939/N2K status
 * default n2k_encode.c already uses. */
#define RVC_PRI_STATUS   6u

/*
 * ISO NAME "industry group" position for the RV-C identity
 * (n2k_addrclaim.h's n2k_name_fields_t.industry_group, reused
 * dialect-neutrally — Core/Src/can_n2k.c builds the RV-C NAME with this
 * value instead of N2K's 4/marine).
 *
 * RESOLVED (2026-07-28) from spec text, and stronger than the earlier
 * vendor-table evidence suggested: RV-C's ADDRESS_CLAIM field (§3.3.3
 * Table 3.3.3) defines NO industry group at all. Bytes 5-7 (bar the
 * arbitrary-address-capable bit) are an optional "Compatibility Field,
 * normally 0", and the three bits sitting where J1939 puts the industry
 * group (byte 7 bits 4-6) are "Always 0" — mandated. So 0 here is a spec
 * requirement, settled fact; the Xantrex guide's "Industry Group - 0" row
 * was this mandate seen through a vendor table. (Same section, same
 * verdict class: RV-C NAMEs carry no device class/function semantics
 * either — see Core/Src/can_n2k.c's RVC_DEVICE_CLASS/_FUNCTION note and
 * CAN_INTEGRATION.md §9.) */
#define RVC_INDUSTRY_GROUP   0u

/*
 * RVC_DGN_CHARGER_STATUS (single frame, 8 B; layout spec-confirmed
 * 2026-07-28 against §6.20.8 Table 6.20.8b; cadence 5000 ms or on change,
 * Table 6.20.8a):
 *   0    instance — 1..250; 0 is "Invalid" (Table 6.20.8b), so the
 *        scheduler default MUST become 1 — REVISED, rvc_sched.c's
 *        RVC_INSTANCE_DEFAULT is currently 0, code change TODO(GH#32)
 *   1-2  charge voltage      u16 LE, 0.05 V/bit (Table 5.3 — settled).
 *        Spec semantics: "Control voltage: the voltage DESIRED to be
 *        delivered to the battery" — a target, not a measurement (the
 *        measured pair lives in CHARGER_STATUS_2). We currently send
 *        measured vbat_pack_v; acceptable first cut, but wire the
 *        regulation target in with the GH#32 encoder pass.
 *   3-4  charge current      u16 LE — REVISED (2026-07-28): Table 5.3's
 *        16-bit current type is OFFSET-encoded (0.05 A/bit, raw 0x7D00 =
 *        0 A, range -1600..+1612.5 A), NOT plain unsigned as currently
 *        implemented. A compliant decoder (raw*0.05 - 1600) reads our
 *        +12 A today as -1588 A. Encoder fix REQUIRED before RV-C Tx is
 *        enabled — TODO(GH#32). The >= 0 A reporting floor stays (this
 *        is desired CHARGE current), expressed as raw >= 0x7D00.
 *   5    charge current percent of max   u8 — REVISED (2026-07-28):
 *        RV-C's percent type is 0.5 %/bit (Table 5.3: uint8, 0-125 %),
 *        not the 0.4 %/bit J1939 Type-P guess. TODO(GH#32). Still
 *        approximated from field_effort (duty ratio) — the v1 snapshot
 *        has no measured percent-of-ceiling figure (signed off as an
 *        honest approximation of the spec's "control current as a percent
 *        of the maximum").
 *   6    operating state     u8 enum — spec-confirmed (Table 6.20.8b):
 *        0 disable, 1 not charging, 2 bulk, 3 absorption, 4 overcharge,
 *        5 equalize, 6 float, 7 constant voltage/current
 *   7    bit0-1 default state on power-up (spec: 00b disabled/01b
 *        enabled; we send 11b = NA — not configurable yet, TODO(GH#10))
 *        bit2-3 auto recharge enable (spec: 00b/01b; 11b NA, same reason)
 *        bit4-7 force charge (spec: 0 not forced, 1 force bulk, 2 force
 *        float; we send 0xF = NA since command Rx isn't wired up)
 *
 * `t->amps_batt` is signed (+ charging); a net-discharge reading clamps
 * to 0 A rather than encoding negative — this field is the charger's
 * desired charge current, not net battery current (DC_SOURCE_STATUS_1
 * carries the signed net value).
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_charger_status(const ctrl_telemetry_t *t, uint8_t instance,
                              uint8_t src, n2k_frame_t *out);

/*
 * RVC_DGN_CHARGER_STATUS_2 (single frame, 8 B; layout spec-confirmed
 * 2026-07-28 against §6.20.9 Table 6.20.9b — it matches the earlier
 * thomasonw/RV-C reconstruction field-for-field):
 *   0    charger instance — 1..250, 0 "Invalid": default must become 1
 *        (REVISED, same rvc_sched.c TODO(GH#32) as CHARGER_STATUS byte 0)
 *   1    DC source instance the charger is associated with (spec marks it
 *        DEPRECATED but defines 0 = invalid / 255 = unknown) — REVISED:
 *        send 1 (Table 6.5.2b: DC instance 1 = "Main House Battery Bank",
 *        the bank we regulate), not the current 0; TODO(GH#32)
 *   2    charger priority ("0 = unassigned, higher value = higher
 *        priority") — the SAME value rvc_rbm_t uses for RBM arbitration,
 *        passed in so this encoder stays pure (tier verdict: 80, the
 *        spec's Charger tier — rvc_sched.h)
 *   3-4  charging voltage   u16 LE, 0.05 V/bit (Table 5.3) — "voltage as
 *        measured at charger": measured, vbat_pack_v is the right source
 *   5-6  charging current   u16 LE, 0.05 A/bit, offset raw 0x7D00 = 0 A
 *        (Table 5.3) — "current being delivered by charger"; the encoder
 *        already offset-encodes this field correctly (settled)
 *   7    charger temperature u8, 1 °C/bit, offset -40 (Table 5.3) —
 *        "temperature of charger": t->alt_temp_c (this device's own
 *        thermal state) confirmed as the intended source; the earlier
 *        battery-temp doubt is closed by the spec wording (settled)
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_charger_status_2(const ctrl_telemetry_t *t, uint8_t instance,
                                uint8_t dc_source_instance,
                                uint8_t device_priority, uint8_t src,
                                n2k_frame_t *out);

/*
 * RVC_DGN_DC_SOURCE_STATUS_1 (single frame, 8 B; layout spec-confirmed
 * 2026-07-28 against §6.5.2 Table 6.5.2b; normal broadcast gap 500 ms):
 *   0    DC instance — REVISED: 1 = "Main House Battery Bank" (0 is
 *        "Invalid"); scheduler default 0 must become 1, TODO(GH#32)
 *   1    device priority — the RBM arbitration value (rvc_sched.h; spec
 *        tiers in Table 6.5.2b, verdict 80 = Charger). This is the field
 *        other nodes read to decide whether WE are the winning master for
 *        this DC-source instance, and the field rvc_rbm_rx() reads from
 *        OTHER nodes' broadcasts of this same DGN.
 *   2-3  dc voltage   u16 LE, 0.05 V/bit (Table 5.3 — settled)
 *   4-7  dc current   u32 LE, 1 mA/bit, offset-encoded raw 0x77359400
 *        (2 000 000 000) = 0 A — settled: Table 5.3 states the zero code
 *        verbatim, closing what was the weakest-evidenced constant in
 *        this file. SIGN REVISED (2026-07-28): Table 6.5.2b defines
 *        POSITIVE as current flowing FROM the source (battery
 *        discharging), negative as the battery being recharged — the
 *        OPPOSITE of t->amps_batt's charging-positive convention. The
 *        encoder currently passes amps_batt through unnegated, so a
 *        charging bank would display as discharging. Encoder fix (negate)
 *        REQUIRED before RV-C Tx is enabled — TODO(GH#32).
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_dc_source_status_1(const ctrl_telemetry_t *t, uint8_t instance,
                                  uint8_t device_priority, uint8_t src,
                                  n2k_frame_t *out);

/*
 * RVC_DGN_DC_SOURCE_STATUS_2 (single frame, 8 B; layout spec-confirmed
 * 2026-07-28 against §6.5.3 Table 6.5.3b):
 *   0    DC instance (same REVISED default-1 note as DC_SOURCE_STATUS_1)
 *   1    device priority
 *   2-3  source temperature  u16 LE, 0.03125 °C/bit, offset -273 °C —
 *        settled: Table 5.3's 16-bit temperature is exactly the J1939
 *        convention previously guessed. Sourced from t->batt_temp_c: this
 *        DGN is battery-instance data, so battery temperature is the
 *        correct field (encodes NA in V1 — no battery-temp source
 *        exists, PROJECT_PLAN §1.1).
 *   4    state of charge   u8 % — settled 0.5 %/bit (Table 5.3), not the
 *        0.4 %/bit guess; moot in practice, always RVC_NA_U8 (no SOC in
 *        the v1 snapshot — same honesty n2k_encode_127506 applies)
 *   5-6  time remaining    u16 LE, minutes — NA always (not in snapshot)
 *   7    bit0-1 time-remaining interpretation (Table 6.5.3b: 00b
 *        to-empty, 01b to-full; 11b = not provided, interpreted as
 *        to-empty). We transmit the whole byte as 0xFF — 11b plus
 *        reserved-bits-as-1s — which is legal NA given time remaining is
 *        itself NA (settled; the byte is NOT plain padding as previously
 *        described).
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_dc_source_status_2(const ctrl_telemetry_t *t, uint8_t instance,
                                  uint8_t device_priority, uint8_t src,
                                  n2k_frame_t *out);

/*
 * RVC_DGN_DC_SOURCE_STATUS_3 (single frame, 8 B; layout spec-confirmed
 * 2026-07-28 against §6.5.4 Table 6.5.4b — SOH %, capacity remaining in
 * whole Ah, relative capacity %, AC RMS ripple u16 at 1 mV/bit). The v1
 * snapshot has NO state-of-health, capacity, relative-capacity, or ripple
 * data, so this encoder always emits the all-not-available payload; the
 * spec lists a 500 ms normal broadcast gap, but rvc_sched.c deliberately
 * never schedules an all-NA frame (the Xantrex reference product likewise
 * ships it request/response-only, "sent with no data on all fields") —
 * revisit when any of these fields gains a source. Encoder exists for
 * completeness/testing:
 *   0 instance   1 device priority
 *   2 state of health (NA)         3-4 capacity remaining (NA)
 *   5 relative capacity (NA)       6-7 AC RMS ripple (NA)
 *
 * Returns 1, or -1 on NULL args.
 */
int rvc_encode_dc_source_status_3(uint8_t instance, uint8_t device_priority,
                                  uint8_t src, n2k_frame_t *out);

#endif /* WS500_RVC_ENCODE_H */
