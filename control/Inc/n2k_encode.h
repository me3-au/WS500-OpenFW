/*
 * n2k_encode.h — pure NMEA 2000 PGN encoders over the telemetry snapshot.
 *
 * The Tx half of the CAN telemetry path (CAN_INTEGRATION.md §0, §2; feeds
 * GH#18 deliverable #9 "CAN Tx → Cerbo"). Maps the dialect-neutral
 * ctrl_telemetry_t to raw 29-bit-ID CAN frames for the PGN set listed in
 * CAN_INTEGRATION.md §2: 127508, 127506, 127488, 127750, 126983/126985,
 * 126996/126998, plus the proprietary fast-packet. The bxCAN driver
 * (Core/Src/can_n2k.c, GH#27) will feed these frames to the wire; nothing
 * here touches hardware.
 *
 * PURE — no HAL, no allocation, fixed-size outputs, C11 float only.
 *
 * Wire conventions used throughout (public N2K/J1939 field definitions; the
 * MIT ttlappalainen/NMEA2000 library headers were used as the reference for
 * per-PGN field order/units — facts only, no code copied):
 *   - little-endian multi-byte fields;
 *   - units: volts 0.01 V, amps 0.1 A signed, temperature 0.01 K unsigned,
 *     RPM 0.25 rpm unsigned;
 *   - "not available": 0xFF (u8), 0xFFFF (u16), 0x7FFF (s16), 0xFFFFFFFF…;
 *   - out-of-range values SATURATE at the last valid code (u8 0xFC,
 *     u16 0xFFFC, s16 ±0x7FFC/−32768) rather than reading as "missing" —
 *     a pegged gauge is more honest than a blank one [SPEC-SIGNOFF];
 *   - unused/reserved bits transmit as 1s (J1939 convention).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_N2K_ENCODE_H
#define WS500_N2K_ENCODE_H

#include <stdint.h>
#include <stddef.h>
#include "telemetry.h"

/* One classic CAN data frame: 29-bit extended identifier + up to 8 bytes.
 * N2K frames always carry dlc = 8 (fast-packet tails pad with 0xFF). */
typedef struct {
    uint32_t id;       /* 29-bit extended CAN identifier (no IDE/RTR flags) */
    uint8_t  dlc;      /* data length, always 8 for N2K */
    uint8_t  data[8];
} n2k_frame_t;

/* Fast-packet limits (N2K fast-packet transport): first frame carries 6 data
 * bytes, each following frame 7, frame counter is 5 bits → 6 + 31×7 = 223. */
#define N2K_FP_MAX_PAYLOAD  223u
#define N2K_FP_MAX_FRAMES   32u

/* "Data not available" sentinels (J1939/N2K). */
#define N2K_NA_U8   0xFFu
#define N2K_NA_U16  0xFFFFu
#define N2K_NA_S16  0x7FFF
#define N2K_NA_U32  0xFFFFFFFFu

/* Proprietary fast-packet header (CAN_INTEGRATION.md §2 last row): 11-bit
 * manufacturer code + 2 reserved bits (1s) + 3-bit industry group (4 =
 * marine). No official NMEA manufacturer code exists for this open project;
 * 2046 is the top of the code space and conventionally used by hobby/open
 * devices [SPEC-SIGNOFF]. */
#define N2K_PROP_MFG_CODE   2046u
#define N2K_PROP_INDUSTRY   4u
/* Proprietary-B fast-packet PGN range starts at 130816; we use the first. */
#define N2K_PGN_PROP_TELEM  130816u

/* Compose a 29-bit CAN identifier per J1939/N2K: priority (3 bits) << 26,
 * then the 18-bit PGN field, then the 8-bit source address. For PDU2 PGNs
 * (PF ≥ 240 — all PGNs in this module) the PGN's low byte is the group
 * extension and goes on the wire verbatim; for PDU1 the low byte is replaced
 * by `dest` (0xFF = broadcast). */
uint32_t n2k_can_id(uint8_t priority, uint32_t pgn, uint8_t src, uint8_t dest);

/* Extract the 3-bit priority from a 29-bit N2K CAN identifier. */
uint8_t n2k_can_id_prio(uint32_t id);

/* Extract the PGN from a 29-bit N2K CAN identifier (PDU1 → PS byte zeroed,
 * since a destination address is not part of the PGN). */
uint32_t n2k_can_id_pgn(uint32_t id);

/* Extract the 8-bit source address from a 29-bit N2K CAN identifier. */
uint8_t n2k_can_id_src(uint32_t id);

/* Return the SID to stamp on this measurement cycle and advance the counter.
 * SIDs cycle 0..252 (253/254 reserved, 255 = "not available"); every PGN
 * built from the same snapshot should carry the same SID so displays can
 * correlate them. */
uint8_t n2k_sid_next(uint8_t *sid);

/*
 * Split a fast-packet payload into wire frames (N2K fast-packet transport):
 * frame 0 = [seq<<5 | 0][total length][6 data bytes], frames 1..n =
 * [seq<<5 | n][7 data bytes], the tail padded with 0xFF. `seq` (0..7) must
 * change between successive transmissions of the same PGN so receivers can
 * detect interleaving. All frames carry `can_id`.
 * Returns the number of frames written, or -1 if the payload exceeds
 * N2K_FP_MAX_PAYLOAD / out capacity, or on NULL args / len == 0.
 */
int n2k_fp_split(uint32_t can_id, uint8_t seq,
                 const uint8_t *payload, size_t len,
                 n2k_frame_t *out, size_t max_frames);

/*
 * PGN 127508 Battery Status (single frame; CAN_INTEGRATION.md §2 "V, A,
 * temperature"): instance, pack voltage (0.01 V s16), battery current
 * (0.1 A s16, + = charging), battery temperature (0.01 K u16), SID.
 * NaN inputs encode as not-available. Returns 1 (frames written) or -1 on
 * NULL args.
 */
int n2k_encode_127508(const ctrl_telemetry_t *t, uint8_t instance,
                      uint8_t sid, uint8_t src, n2k_frame_t *out);

/*
 * PGN 127506 DC Detailed Status (fast packet, 11-byte payload;
 * CAN_INTEGRATION.md §2 "when acting as bank monitor"): SID, instance,
 * DC type = 0 (battery), SOC %, SOH %, time remaining (min), ripple (V),
 * capacity (Ah). The v1 snapshot carries none of SOC/SOH/time/ripple/
 * capacity, so those fields transmit as not-available until the snapshot
 * grows them — the PGN still announces us as a bank monitor. Returns the
 * frame count (2) or -1.
 */
int n2k_encode_127506(const ctrl_telemetry_t *t, uint8_t instance,
                      uint8_t sid, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames);

/*
 * PGN 127488 Engine Parameters Rapid (single frame; CAN_INTEGRATION.md §2
 * "relayed fused RPM for MFD tachs"): instance, engine speed (0.25 rpm u16),
 * boost pressure (NA), tilt/trim (NA). RPM encodes as not-available unless
 * rpm_state == CTRL_RPM_VALID — a stale tach must read blank, not frozen.
 * Returns 1 or -1.
 */
int n2k_encode_127488(const ctrl_telemetry_t *t, uint8_t instance,
                      uint8_t src, n2k_frame_t *out);

/*
 * PGN 127750 Converter (Charger) Status (single frame; CAN_INTEGRATION.md §2
 * "charger status"): SID, connection number, operating state mapped from the
 * control state (STANDBY→Off, BULK→Bulk, BULK@CV-clamp→Absorption,
 * FLOAT→Float, fault severity ≥ FAULT→Fault), and the four 2-bit status
 * fields (temperature/overload/low-DC/ramping) driven from faults/binding.
 * Returns 1 or -1.
 */
int n2k_encode_127750(const ctrl_telemetry_t *t, uint8_t connection,
                      uint8_t sid, uint8_t src, n2k_frame_t *out);

/* Alert identity + status for PGN 126983/126985 (N2K Alert group). Field
 * meanings follow the public Alert PGN definitions; enums below list only
 * the codes this firmware emits. */
typedef struct {
    uint8_t  type;            /* 1 emergency alarm, 2 alarm, 5 warning, 8 caution */
    uint8_t  category;        /* 0 navigational, 1 technical (we emit 1) */
    uint8_t  system;          /* alert system id (install-assigned) */
    uint8_t  subsystem;
    uint16_t alert_id;        /* unique per condition: fault bit index + 1 */
    uint64_t source_name;     /* ISO NAME of the reporting device */
    uint8_t  source_instance;
    uint8_t  source_index;
    uint8_t  occurrence;      /* occurrence counter, 1-based */
    uint8_t  trigger;         /* trigger condition: 2 = auto */
    uint8_t  threshold_status;/* 1 = threshold exceeded */
    uint8_t  priority;        /* alert priority byte */
    uint8_t  state;           /* 0 disabled, 1 normal, 2 active, 3 silenced, 4 ack'd */
} n2k_alert_t;

/*
 * Fill an n2k_alert_t from the snapshot's fault bitfield (CAN_INTEGRATION.md
 * §2 "alerts (faults, plain-language)"): picks the highest-severity active
 * fault (lowest bit index breaks ties), maps severity → alert type
 * (CRITICAL→emergency, FAULT→alarm, WARN→warning, INFO→caution), sets
 * alert_id = bit index + 1, state = active. `device_name` is this device's
 * ISO NAME (from the address-claim identity — a parameter here so the
 * encoder stays pure). Returns 1 if a fault is active (alert filled),
 * 0 if faults == 0 (nothing to send), -1 on NULL args.
 */
int n2k_alert_from_telemetry(const ctrl_telemetry_t *t, uint64_t device_name,
                             n2k_alert_t *a);

/* Plain-language text for a single ctrl_fault_bits_t bit (exactly one bit
 * set), for PGN 126985. Unknown bit → "unknown fault". Never NULL. */
const char *n2k_alert_text(uint32_t fault_bit);

/*
 * PGN 126983 Alert (fast packet, 28-byte payload): the alert's identity,
 * status flags (not silenced / not acknowledged / no escalation; the
 * acknowledge NAME transmits as not-available), trigger condition,
 * threshold status, priority and state. Returns the frame count or -1.
 */
int n2k_encode_126983(const n2k_alert_t *a, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames);

/*
 * PGN 126985 Alert Text (fast packet, variable): the alert's identity plus
 * language = 0 (English US) and two variable-length strings (description,
 * location), each wire-encoded as [len+2][0x01 ASCII][chars]. `location`
 * may be NULL/empty (encodes as an empty string). Returns the frame count
 * or -1 (also when the combined text would exceed the fast-packet limit).
 */
int n2k_encode_126985(const n2k_alert_t *a, const char *description,
                      const char *location, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames);

/* Static product identity for PGN 126996 (strings are truncated / space-
 * padded to the 32-byte wire fields). Owned by the caller — typically a
 * const table in the driver half next to the address-claim NAME. */
typedef struct {
    uint16_t    n2k_version;    /* database version × 1000, e.g. 2101 */
    uint16_t    product_code;
    const char *model_id;       /* ≤ 32 chars each */
    const char *sw_version;
    const char *model_version;
    const char *serial;
    uint8_t     cert_level;
    uint8_t     load_equiv;     /* LEN, units of 50 mA from the bus */
} n2k_product_info_t;

/*
 * PGN 126996 Product Information (fast packet, 134-byte payload;
 * CAN_INTEGRATION.md §2 "product info"): N2K db version, product code, four
 * fixed 32-byte string fields, certification level, load equivalency.
 * Returns the frame count (20) or -1.
 */
int n2k_encode_126996(const n2k_product_info_t *p, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames);

/*
 * PGN 126998 Configuration Information (fast packet, variable;
 * CAN_INTEGRATION.md §2 "configuration info"): three variable-length strings
 * — installation description 1, installation description 2, manufacturer
 * info — each wire-encoded as [len+2][0x01 ASCII][chars]. NULL strings
 * encode as empty. Returns the frame count or -1 on overflow/NULL out.
 */
int n2k_encode_126998(const char *install_desc1, const char *install_desc2,
                      const char *mfg_info, uint8_t src, uint8_t seq,
                      n2k_frame_t *out, size_t max_frames);

/*
 * Proprietary fast-packet full telemetry (CAN_INTEGRATION.md §2 last row:
 * "field effort, binding ceiling, temps, RPM source/state, active profile").
 * PGN N2K_PGN_PROP_TELEM, 22-byte payload, layout (offsets after the
 * 2-byte mfg/industry header at 0):
 *    2 SID                       3 state (ctrl_state_t)
 *    4 standby_reason            5 active_profile_id
 *    6 field effort (0.5 %/bit, 0..200, NA 0xFF)
 *    7 binding source (ctrl_bind_src_t)
 *    8 binding ceiling W (u16; +inf/none → NA)
 *   10 alternator temp (0.01 K u16)   12 battery temp (0.01 K u16)
 *   14 rpm (0.25 rpm u16)             16 rpm_state (ctrl_rpm_state_t)
 *   17 faults (u32 bitfield)          21 severity (ctrl_severity_t)
 * Returns the frame count (4) or -1.
 */
int n2k_encode_prop_telemetry(const ctrl_telemetry_t *t, uint8_t sid,
                              uint8_t src, uint8_t seq,
                              n2k_frame_t *out, size_t max_frames);

#endif /* WS500_N2K_ENCODE_H */
