/*
 * config_json.h — bounded JSON reader + writer for the config wire (GH#16). PURE.
 *
 * The transport half of Decision #6a option B: the EEPROM holds a packed binary
 * record (config_codec.h), and JSON exists ONLY at the client boundary. This
 * module is that boundary's syntax layer — it knows nothing about the config
 * schema (that is config_doc.h) and nothing about messages (config_msg.h).
 *
 * Written fresh, MIT, C11, HAL-free, and deliberately NOT a JSON library:
 *
 *   - NO dynamic allocation and NO document tree. Reading is a CURSOR over the
 *     caller's line buffer: the caller walks the members it knows and calls
 *     cfg_json_skip() over the ones it does not. A 16 KB-RAM Cortex-M0 cannot
 *     afford a parsed DOM of a ~2 KB document, and it does not need one.
 *   - Recursion exists in exactly ONE place (cfg_json_skip over nested
 *     containers) and is hard-bounded by CFG_JSON_DEPTH_MAX. Every other walk
 *     is a loop. The schema mapper's own nesting is static, so worst-case stack
 *     is a compile-time constant, not an input-controlled one.
 *   - Every token is length-bounded (CFG_JSON_NUM_CHARS_MAX, the caller's string
 *     capacity). An over-long token is an explicit error or a flagged
 *     truncation, never a buffer write past the end.
 *   - Writing STREAMS through a sink callback, so emitting a document costs a
 *     small fixed chunk buffer instead of a full-size reply buffer.
 *
 * Accepted grammar is the RFC 8259 subset the config wire needs: objects,
 * arrays, strings, numbers, true/false/null. Strictness is deliberate — the
 * looser a config parser is, the more ways a client has to *think* it wrote
 * something. So: no trailing commas, no comments, no leading `+`, no `.5`, no
 * `5.`, no leading zeros, no unescaped control characters, no NaN/Infinity
 * literals (a non-finite config value travels as JSON `null`).
 *
 * UTF-8 is NOT validated: bytes >= 0x80 inside a string pass through verbatim,
 * and \uXXXX is decoded to UTF-8 without checking surrogate pairing. Text
 * validity belongs to the client; the firmware only stores profile-name-shaped
 * bytes and must not reject a config over a mis-encoded label.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_JSON_H
#define WS500_CONFIG_JSON_H

#include <stdint.h>
#include <stdbool.h>
#include <stddef.h>

/* ---- bounds (all compile-time; nothing here scales with input) ------------- */

/* Maximum container nesting the reader will descend. The §7 document needs 4
 * (document -> "profiles" array -> profile object -> "rest" object); 6 leaves
 * room for one additive nesting level without touching this file, and turns a
 * depth bomb into an error at a fixed stack cost. */
#define CFG_JSON_DEPTH_MAX        6

/* Longest number TOKEN accepted, in characters. Far beyond any real value
 * (float carries ~9 significant digits), short enough that a megabyte of digits
 * is rejected at the token, not after being accumulated. */
#define CFG_JSON_NUM_CHARS_MAX    40

/* Capacity the schema mapper uses for object keys and short string values
 * (primitive refs, rest modes). Anything longer is truncated AND flagged, and a
 * truncated key can never match a known one — see cfg_json_obj_key(). */
#define CFG_JSON_KEY_MAX          48

/* Significant digits the float writer emits (printf "%.4g" shape). Config
 * values are human-scale quantities entered to at most four significant
 * figures, so this is the readable default — cfg_json_w_f32() falls back to
 * CFG_JSON_SIG_EXACT digits whenever four would not reproduce the same float. */
#define CFG_JSON_SIG_DEFAULT      4
#define CFG_JSON_SIG_EXACT        9

/* ---- reader ---------------------------------------------------------------- */

/* Why a walk stopped. Every one of these maps to the wire code CFG_ERR_JSON;
 * they are separated so tests and logs can say which rule was broken. */
typedef enum {
    CFG_JSON_OK = 0,
    CFG_JSON_ERR_SYNTAX,     /* not the grammar above at this position */
    CFG_JSON_ERR_DEPTH,      /* nesting beyond CFG_JSON_DEPTH_MAX */
    CFG_JSON_ERR_TOOLONG,    /* a token exceeded its hard bound */
    CFG_JSON_ERR_TRUNCATED,  /* input ended inside a value */
    CFG_JSON_ERR_TRAILING    /* non-whitespace after the top-level value */
} cfg_json_err_t;

/* The JSON value kinds this subset knows. */
typedef enum {
    CFG_JSON_T_NONE = 0,     /* end of input, or the reader is already in error */
    CFG_JSON_T_OBJ,
    CFG_JSON_T_ARR,
    CFG_JSON_T_STR,
    CFG_JSON_T_NUM,
    CFG_JSON_T_BOOL,
    CFG_JSON_T_NULL
} cfg_json_type_t;

/* Read cursor. Points into the caller's buffer and never copies it; the buffer
 * must outlive the cursor. `unknown` is a counter the SCHEMA layer bumps for
 * every member it chose to ignore — it lives here so one document walk yields
 * one count regardless of how many nested mappers touched it. */
typedef struct {
    const char    *s;
    int            len;
    int            pos;
    int            depth;
    uint32_t       seen;     /* bit d set = level d already yielded a member */
    int            unknown;
    cfg_json_err_t err;
} cfg_json_t;

/* Bind a cursor to `len` bytes of JSON text (no NUL required). */
void cfg_json_init(cfg_json_t *j, const char *text, int len);

/* Well-formedness check of a COMPLETE document: one top-level value, nothing
 * but whitespace after it. Returns CFG_JSON_OK or the first rule broken. This
 * is the accept/reject oracle the message layer uses before mapping. */
cfg_json_err_t cfg_json_validate(const char *text, int len);

/* Kind of the value at the cursor, without consuming it. CFG_JSON_T_NONE once
 * the cursor is in error or the input is exhausted. */
cfg_json_type_t cfg_json_peek(cfg_json_t *j);

/* Consume `{`. False (and the cursor errors) if the value is not an object. */
bool cfg_json_obj_begin(cfg_json_t *j);

/* Advance to the next member of the object opened by cfg_json_obj_begin().
 * Returns true with the member name copied into `key` (NUL-terminated, at most
 * cap-1 bytes) and the cursor positioned on its VALUE. Returns false when the
 * object closed (`}` consumed) or on error — check j->err to tell them apart.
 * `truncated`, when non-NULL, reports a name that did not fit: the schema layer
 * must then treat the member as unknown, because a truncated name could
 * otherwise alias a shorter known one. */
bool cfg_json_obj_key(cfg_json_t *j, char *key, int cap, bool *truncated);

/* Consume `[`. False (and the cursor errors) if the value is not an array. */
bool cfg_json_arr_begin(cfg_json_t *j);

/* Advance to the next element of the array opened by cfg_json_arr_begin().
 * True with the cursor on the element; false when the array closed or on error. */
bool cfg_json_arr_next(cfg_json_t *j);

/* Read a number. Out-of-float-range magnitudes come back as +/-HUGE_VAL rather
 * than an error — the SCHEMA layer owns "is this a legal config value", and a
 * caller that needs an integer must range-check what it gets. */
bool cfg_json_num(cfg_json_t *j, double *out);

/* Read `true`/`false`. */
bool cfg_json_bool(cfg_json_t *j, bool *out);

/* Read `null`. */
bool cfg_json_null(cfg_json_t *j);

/* Read a string into `out` (NUL-terminated, at most cap-1 bytes). Escapes are
 * decoded; \uXXXX becomes UTF-8. An over-long string is truncated and flagged
 * via `truncated` (may be NULL) — the value is still fully consumed, so the
 * walk continues. */
bool cfg_json_str(cfg_json_t *j, char *out, int cap, bool *truncated);

/* Consume the value at the cursor whatever it is, including whole nested
 * containers. The ONE recursive function in this module; bounded by
 * CFG_JSON_DEPTH_MAX. */
bool cfg_json_skip(cfg_json_t *j);

/* ---- writer ---------------------------------------------------------------- */

/* Where finished bytes go when the chunk buffer fills (and on flush). A NULL
 * sink makes the writer buffer-only: it stops at capacity and raises `ovf`. */
typedef void (*cfg_json_sink_fn)(void *ctx, const char *data, int len);

/* Streaming writer. `buf` is a working chunk, not the document: with a sink
 * bound, a 128-byte chunk emits a document of any size. Container separators
 * are tracked in `comma` (one bit per open level), so callers never write
 * commas or worry about the first member. */
typedef struct {
    char            *buf;
    int              cap;
    int              len;
    cfg_json_sink_fn sink;
    void            *ctx;
    uint32_t         comma;     /* bit d set = level d already has a member */
    uint32_t         isarr;     /* bit d set = level d is an array, not an object */
    int              depth;
    bool             pending;   /* a key was written; the next value is ITS value */
    bool             ovf;       /* buffer full with no sink, or depth overflow */
} cfg_json_w_t;

/* Bind a writer to a chunk buffer and an optional sink. */
void cfg_json_w_init(cfg_json_w_t *w, char *buf, int cap,
                     cfg_json_sink_fn sink, void *ctx);

/* Push any buffered bytes to the sink. Must be called once the document is
 * complete; returns false if the writer overflowed at any point. */
bool cfg_json_w_flush(cfg_json_w_t *w);

/* Open/close containers. `w_end` closes whichever container is innermost. */
void cfg_json_w_obj(cfg_json_w_t *w);
void cfg_json_w_arr(cfg_json_w_t *w);
void cfg_json_w_end(cfg_json_w_t *w);

/* Write a member name; the next w_* call writes its value. */
void cfg_json_w_key(cfg_json_w_t *w, const char *key);

/* Write a string value, escaping what RFC 8259 requires (and nothing else —
 * bytes >= 0x80 are copied through, matching the reader's UTF-8 stance). */
void cfg_json_w_str(cfg_json_w_t *w, const char *s);

/* Write a signed integer value. */
void cfg_json_w_int(cfg_json_w_t *w, long v);

/* Write a float value at CFG_JSON_SIG_DEFAULT significant digits, widening to
 * CFG_JSON_SIG_EXACT only when the short form would not read back as the same
 * float — so the wire stays human-readable without ever being lossy.
 * NON-FINITE VALUES ARE WRITTEN AS `null`: NAN is a live "disabled" value in
 * this config (warmup_coolant_c, rotor_v_max) and JSON has no NaN literal, so
 * null IS the encoding of "unset" (PROFILE_SPEC §7 shows exactly that). */
void cfg_json_w_f32(cfg_json_w_t *w, float v);

/* Write `true`/`false`. */
void cfg_json_w_bool(cfg_json_w_t *w, bool v);

/* Write `null`. */
void cfg_json_w_null(cfg_json_w_t *w);

/* Write an already-formatted raw fragment (used for fixed literals). */
void cfg_json_w_raw(cfg_json_w_t *w, const char *s, int len);

/* Format `v` with `sig` significant digits in printf "%g" shape into `out`.
 * Exposed because the target links newlib-nano, whose printf has NO float
 * support — this module cannot call snprintf("%g") and neither can anything
 * else in the firmware. Returns the length written, or -1 if it would not fit. */
int cfg_json_fmt_g(char *out, int cap, double v, int sig);

/* Parse a JSON number from `s` (up to `len` bytes), the reader's own scanner.
 * Sets *consumed to the token length. Exposed so the writer can check its own
 * output for faithfulness and so tests can drive the scanner directly. */
bool cfg_json_scan_num(const char *s, int len, int *consumed, double *out);

#endif /* WS500_CONFIG_JSON_H */
