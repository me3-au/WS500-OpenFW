/*
 * config_msg.h — USB-CDC JSON-lines config protocol (GH#16, deliverable #6). PURE.
 * SPDX-License-Identifier: MIT
 *
 * The MESSAGE layer: newline-delimited JSON, one object per line, in both
 * directions. Syntax is config_json.h, the config document is config_doc.h, and
 * the byte transport is somebody else's problem — this module reaches the wire
 * only through an injected sink, so the whole protocol is exercised natively in
 * control/test/ with no USB stack anywhere near it.
 *
 * Contracts implemented here:
 *   - docs/VERSIONING.md §2, the hello handshake. Compatibility is NEGOTIATED:
 *     the client announces the highest protocol version it speaks, the firmware
 *     answers with what it actually is (version, schema, capability flags) and
 *     lets the client decide. The firmware never refuses a hello — a client
 *     that cannot be told what it is talking to cannot explain itself to a user
 *     standing in an engine room.
 *   - docs/VERSIONING.md §3 + decision #6a-B, config reads and writes. The
 *     firmware is the schema authority; writes are validated, never repaired,
 *     and rejected by the validator's own error NAME.
 *
 * ---------------------------------------------------------------------------
 * GRAMMAR
 * ---------------------------------------------------------------------------
 * Request                          Reply
 * -------------------------------  ------------------------------------------
 * {"t":"hello","proto":N}          {"t":"hello","fw":..,"git":..,"proto":1,
 *                                   "schema":1,"caps":["cfg"]}
 * {"t":"cfg-get"}                  {"t":"cfg","cfg":{ ...PROFILE_SPEC §7... }}
 * {"t":"cfg-set","cfg":{...}}      {"t":"ok","generation":N,"unknown_keys":K}
 *                              or  {"t":"err","code":..,"detail":..,
 *                                   "profile":P,"unknown_keys":K}
 * (anything else)                  {"t":"err","code":"CFG_ERR_MSG",...}
 *
 * `cfg-get`'s reply nests the document under "cfg" so that the reply body can
 * be handed straight back as a `cfg-set` — the round trip is the same bytes.
 *
 * Error codes are STRINGS, never numbers, and the ones a config write can
 * produce are the validator's own enum names (cfg_err_name(), e.g.
 * "CFG_ERR_RANGE_V_BULK") so the wire, the firmware source and the spec all say
 * the same word. The five codes this layer adds are the ones the validator
 * cannot express:
 *
 *   CFG_ERR_JSON    malformed JSON, over-long line, or trailing junk
 *   CFG_ERR_MSG     missing/unknown "t", or cfg-set without a "cfg" object
 *   CFG_ERR_TYPE    a known key with an impossible value (see config_doc.h)
 *   CFG_ERR_SCHEMA  "schema_version" present and not the firmware's own
 *   CFG_ERR_STORE   validated fine, but the EEPROM write/verify failed
 *
 * ---------------------------------------------------------------------------
 * UNKNOWN KEYS
 * ---------------------------------------------------------------------------
 * PROFILE_SPEC §7 promises "unknown keys preserved on round-trip". Decision
 * #6a-B puts that promise on the CLIENT ("the app translates the file"), because
 * the firmware's native store is a packed binary record with no room for
 * arbitrary JSON. So the firmware IGNORES unknown keys on parse — but it
 * counts every one and reports the total as "unknown_keys" in the ok/err reply.
 * The client can then see that its document was not stored whole, instead of
 * discovering it on the next export.
 */
#ifndef WS500_CONFIG_MSG_H
#define WS500_CONFIG_MSG_H

#include "config_json.h"
#include "config_validate.h"   /* cfg_err_t + cfg_err_name/cfg_err_str */

/* Wire protocol version announced in the hello reply (VERSIONING §1). Bumped
 * only for a breaking message-shape change — additive features are new
 * capability strings, not a bump. */
#define CFG_PROTO_VERSION   1

/* Capability flags this build announces. ONE entry today: the config surface
 * shipped with GH#16. Telemetry, the DFU handoff and the $AST-style status line
 * each add their own string when they land — never a version comparison. */
#define CFG_CAP_CFG         "cfg"

/*
 * Longest accepted request line, in bytes, and the reason it is not the round
 * 2 KB: a full §7 document emitted by cfg-get measures ~2.4 KB for the seven
 * profiles, and a client MUST be able to send back what it was given. The value
 * is checked against the real emitter in control/test/test_protocol.c so it
 * cannot rot. A longer line is not buffered — it is dropped to the next
 * newline and answered with CFG_ERR_JSON.
 */
#define CFG_MSG_LINE_MAX    3072

/* Streaming chunk for replies. Replies are written through a sink, so this
 * bounds RAM, not reply size — a 2.4 KB config document goes out in 128-byte
 * pieces. */
#define CFG_MSG_CHUNK       128

/* Everything the message layer needs from its host, injected so that control/
 * stays HAL-free and the tests can supply an in-RAM store. */
typedef struct {
    /* The live config the regulator is running. Never NULL in a bound host. */
    const ctrl_config_t *(*get)(void *ctx);

    /* Persist and adopt an already-VALIDATED config. Returns true on success
     * with *generation set to the newly written record's generation. The
     * message layer calls ctrl_config_validate() first, always — a host must
     * not have to re-check. */
    bool (*save)(void *ctx, const ctrl_config_t *cfg, uint32_t *generation);

    void            *ctx;      /* passed back to get/save */
    cfg_json_sink_fn tx;       /* where reply bytes go */
    void            *tx_ctx;   /* passed back to tx */
    const char      *fw;       /* FW_VERSION_STRING; NULL omits the field */
    const char      *git;      /* short git hash; NULL or "" omits the field */
} cfg_msg_host_t;

/* Protocol state: one line under assembly plus the scratch a config write
 * needs. Sized entirely at compile time — no allocation, no growth. */
typedef struct {
    cfg_msg_host_t host;
    char           line[CFG_MSG_LINE_MAX];
    int            len;
    bool           overflow;          /* dropping bytes until the next newline */
    char           chunk[CFG_MSG_CHUNK];
    ctrl_config_t  scratch;           /* cfg-set works here, never on the live config */
} cfg_msg_t;

/* Bind a protocol instance to its host. Safe to call again to rebind. */
void cfg_msg_init(cfg_msg_t *m, const cfg_msg_host_t *host);

/* Feed received bytes. Complete lines are dispatched and answered inline, so
 * this call may emit through host.tx before it returns. Partial lines are held
 * until their newline arrives; a line longer than CFG_MSG_LINE_MAX is dropped
 * to the newline and answered with CFG_ERR_JSON. */
void cfg_msg_rx(cfg_msg_t *m, const uint8_t *data, int len);

/* Handle ONE complete line (no newline needed). Exposed for tests and for a
 * transport that already does line assembly. */
void cfg_msg_line(cfg_msg_t *m, const char *line, int len);

#endif /* WS500_CONFIG_MSG_H */
