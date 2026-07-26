/*
 * config_msg.c — USB-CDC JSON-lines config protocol (GH#16). PURE.
 * Message grammar, error-code table and the unknown-key policy: config_msg.h.
 *
 * Order of operations for a config write is the whole safety story of this
 * file, so it is stated once here and never varied:
 *
 *   copy the live config -> overlay the document onto the COPY -> validate the
 *   copy -> persist -> only then let the host adopt it.
 *
 * Nothing touches the running config until the EEPROM has accepted the record
 * and verified it back (config_store.h), and a rejected document leaves the
 * regulator running exactly what it was running before — including when the
 * rejection happens halfway through parsing.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_msg.h"
#include "config_doc.h"

#include <string.h>

/* ---- reply plumbing -------------------------------------------------------- */

static void tx_raw(cfg_msg_t *m, const char *s, int n)
{
    if (m->host.tx != NULL) m->host.tx(m->host.tx_ctx, s, n);
}

static void reply_open(cfg_msg_t *m, cfg_json_w_t *w)
{
    cfg_json_w_init(w, m->chunk, CFG_MSG_CHUNK, m->host.tx, m->host.tx_ctx);
    cfg_json_w_obj(w);
}

/* Every reply is exactly one line: closing brace, flush, newline. */
static void reply_close(cfg_msg_t *m, cfg_json_w_t *w)
{
    cfg_json_w_end(w);
    (void)cfg_json_w_flush(w);
    tx_raw(m, "\n", 1);
}

/* `key` names the offending member when one is known; `profile_id` is the 1-based
 * profile the validator blamed (0 = none); `unknown` < 0 omits the count (there
 * is no meaningful count when the line never parsed). */
static void reply_err(cfg_msg_t *m, const char *code, const char *detail,
                      const char *key, int profile_id, int unknown)
{
    cfg_json_w_t w;
    reply_open(m, &w);
    cfg_json_w_key(&w, "t");    cfg_json_w_str(&w, "err");
    cfg_json_w_key(&w, "code"); cfg_json_w_str(&w, code);
    if (detail != NULL)         { cfg_json_w_key(&w, "detail"); cfg_json_w_str(&w, detail); }
    if (key != NULL && key[0] != '\0') { cfg_json_w_key(&w, "key"); cfg_json_w_str(&w, key); }
    if (profile_id > 0)         { cfg_json_w_key(&w, "profile"); cfg_json_w_int(&w, profile_id); }
    if (unknown >= 0)           { cfg_json_w_key(&w, "unknown_keys"); cfg_json_w_int(&w, unknown); }
    reply_close(m, &w);
}

static const char *json_err_detail(cfg_json_err_t e)
{
    switch (e) {
    case CFG_JSON_OK:            return "ok";
    case CFG_JSON_ERR_SYNTAX:    return "malformed JSON";
    case CFG_JSON_ERR_DEPTH:     return "nesting deeper than the parser allows";
    case CFG_JSON_ERR_TOOLONG:   return "token longer than the parser allows";
    case CFG_JSON_ERR_TRUNCATED: return "JSON ended inside a value";
    case CFG_JSON_ERR_TRAILING:  return "trailing data after the JSON object";
    default:                     return "malformed JSON";
    }
}

/* ---- handlers -------------------------------------------------------------- */

/*
 * VERSIONING §2. The client's own `proto` is read but NOT used to gate the
 * reply: the firmware always states what it is. A client newer or older than
 * this build gets the same honest answer and decides for itself whether to
 * proceed — "the client refuses politely", never the regulator.
 */
static void do_hello(cfg_msg_t *m)
{
    cfg_json_w_t w;
    reply_open(m, &w);
    cfg_json_w_key(&w, "t");  cfg_json_w_str(&w, "hello");
    if (m->host.fw != NULL)  { cfg_json_w_key(&w, "fw");  cfg_json_w_str(&w, m->host.fw); }
    if (m->host.git != NULL && m->host.git[0] != '\0') {
        cfg_json_w_key(&w, "git"); cfg_json_w_str(&w, m->host.git);
    }
    cfg_json_w_key(&w, "proto");  cfg_json_w_int(&w, (long)CFG_PROTO_VERSION);
    cfg_json_w_key(&w, "schema"); cfg_json_w_int(&w, (long)CFG_SCHEMA_VERSION);
    cfg_json_w_key(&w, "caps");
    cfg_json_w_arr(&w);
    cfg_json_w_str(&w, CFG_CAP_CFG);
    cfg_json_w_end(&w);
    reply_close(m, &w);
}

static void do_cfg_get(cfg_msg_t *m)
{
    const ctrl_config_t *cfg = (m->host.get != NULL) ? m->host.get(m->host.ctx) : NULL;
    if (cfg == NULL) {
        reply_err(m, "CFG_ERR_STORE", "no config available", NULL, 0, -1);
        return;
    }
    cfg_json_w_t w;
    reply_open(m, &w);
    cfg_json_w_key(&w, "t");   cfg_json_w_str(&w, "cfg");
    cfg_json_w_key(&w, "cfg"); (void)cfg_doc_emit(&w, cfg, m->host.fw);
    reply_close(m, &w);
}

static void do_cfg_set(cfg_msg_t *m, const char *line, int len)
{
    const ctrl_config_t *live = (m->host.get != NULL) ? m->host.get(m->host.ctx) : NULL;
    if (live == NULL) {
        reply_err(m, "CFG_ERR_STORE", "no config available", NULL, 0, -1);
        return;
    }
    m->scratch = *live;          /* the copy the document is applied to */

    cfg_json_t j;
    cfg_json_init(&j, line, len);
    if (!cfg_json_obj_begin(&j)) {
        reply_err(m, "CFG_ERR_JSON", json_err_detail(j.err), NULL, 0, -1);
        return;
    }

    cfg_doc_info_t info;
    cfg_doc_err_t  de       = CFG_DOC_OK;
    bool           have_cfg = false;
    char           k[CFG_JSON_KEY_MAX];
    bool           tr       = false;

    memset(&info, 0, sizeof info);

    while (cfg_json_obj_key(&j, k, (int)sizeof k, &tr)) {
        if (!tr && strcmp(k, "cfg") == 0) {
            if (cfg_json_peek(&j) != CFG_JSON_T_OBJ) {
                reply_err(m, "CFG_ERR_MSG", "\"cfg\" must be an object", "cfg", 0, -1);
                return;
            }
            de = cfg_doc_parse(&j, &m->scratch, &info);
            have_cfg = true;
            if (de != CFG_DOC_OK) break;
        } else if (!tr && strcmp(k, "t") == 0) {
            if (!cfg_json_skip(&j)) break;
        } else {
            j.unknown++;                       /* envelope-level unknown member */
            if (!cfg_json_skip(&j)) break;
        }
    }

    const int unknown = j.unknown;

    if (j.err != CFG_JSON_OK) {
        reply_err(m, "CFG_ERR_JSON", json_err_detail(j.err), NULL, 0, unknown);
        return;
    }
    switch (de) {
    case CFG_DOC_OK: break;
    case CFG_DOC_ERR_SCHEMA:
        reply_err(m, "CFG_ERR_SCHEMA",
                  "config schema_version is not the one this firmware speaks",
                  info.bad_key, 0, unknown);
        return;
    case CFG_DOC_ERR_TYPE:
        reply_err(m, "CFG_ERR_TYPE", "value is not usable for this key",
                  info.bad_key, 0, unknown);
        return;
    default:
        reply_err(m, "CFG_ERR_JSON", json_err_detail(j.err), NULL, 0, unknown);
        return;
    }
    if (!have_cfg) {
        reply_err(m, "CFG_ERR_MSG", "cfg-set carries no \"cfg\" object", NULL, 0, unknown);
        return;
    }

    /* PROFILE_SPEC §1: the named rejection, never a repair. The validator owns
     * the verdict AND the vocabulary — this layer only carries it. */
    int bad_profile = -1;
    const cfg_err_t verdict = ctrl_config_validate(&m->scratch, &bad_profile);
    if (verdict != CFG_OK) {
        reply_err(m, cfg_err_name(verdict), cfg_err_str(verdict), NULL,
                  (bad_profile >= 0) ? bad_profile + 1 : 0, unknown);
        return;
    }

    uint32_t generation = 0;
    if (m->host.save == NULL || !m->host.save(m->host.ctx, &m->scratch, &generation)) {
        reply_err(m, "CFG_ERR_STORE", "config store write or verify failed",
                  NULL, 0, unknown);
        return;
    }

    cfg_json_w_t w;
    reply_open(m, &w);
    cfg_json_w_key(&w, "t");            cfg_json_w_str(&w, "ok");
    cfg_json_w_key(&w, "generation");   cfg_json_w_int(&w, (long)generation);
    cfg_json_w_key(&w, "unknown_keys"); cfg_json_w_int(&w, unknown);
    reply_close(m, &w);
}

/* ---- dispatch -------------------------------------------------------------- */

/* Find the "t" member without assuming it comes first — JSON objects are
 * unordered and a client is entitled to emit its keys in any order. */
static bool find_type(const char *line, int len, char *out, int cap)
{
    cfg_json_t j;
    cfg_json_init(&j, line, len);
    if (!cfg_json_obj_begin(&j)) return false;

    char k[CFG_JSON_KEY_MAX];
    bool tr = false;
    while (cfg_json_obj_key(&j, k, (int)sizeof k, &tr)) {
        if (!tr && strcmp(k, "t") == 0) {
            if (cfg_json_peek(&j) != CFG_JSON_T_STR) return false;
            bool tr2 = false;
            if (!cfg_json_str(&j, out, cap, &tr2) || tr2) return false;
            return true;
        }
        if (!cfg_json_skip(&j)) return false;
    }
    return false;
}

void cfg_msg_init(cfg_msg_t *m, const cfg_msg_host_t *host)
{
    if (m == NULL) return;
    memset(m, 0, sizeof *m);
    if (host != NULL) m->host = *host;
}

void cfg_msg_line(cfg_msg_t *m, const char *line, int len)
{
    if (m == NULL || line == NULL) return;

    while (len > 0 && (line[len - 1] == '\r' || line[len - 1] == '\n')) len--;
    int lead = 0;
    while (lead < len && (line[lead] == ' ' || line[lead] == '\t' || line[lead] == '\r')) lead++;
    line += lead;
    len  -= lead;

    /* A blank line is not an error: terminals, keep-alives and CRLF all produce
     * them, and answering each one with an error would be a chatty loop. */
    if (len <= 0) return;

    const cfg_json_err_t je = cfg_json_validate(line, len);
    if (je != CFG_JSON_OK) {
        reply_err(m, "CFG_ERR_JSON", json_err_detail(je), NULL, 0, -1);
        return;
    }

    char t[CFG_JSON_KEY_MAX];
    if (!find_type(line, len, t, (int)sizeof t)) {
        reply_err(m, "CFG_ERR_MSG", "message has no string \"t\" member", NULL, 0, -1);
        return;
    }

    if (strcmp(t, "hello") == 0)        do_hello(m);
    else if (strcmp(t, "cfg-get") == 0) do_cfg_get(m);
    else if (strcmp(t, "cfg-set") == 0) do_cfg_set(m, line, len);
    else reply_err(m, "CFG_ERR_MSG", "unknown message type", NULL, 0, -1);
}

void cfg_msg_rx(cfg_msg_t *m, const uint8_t *data, int len)
{
    if (m == NULL || data == NULL) return;

    for (int i = 0; i < len; i++) {
        const char c = (char)data[i];

        if (c == '\n') {
            if (m->overflow) {
                /* The line was dropped, so it cannot be answered on its merits —
                 * but it MUST be answered, or a client that pasted a too-long
                 * document waits forever for a reply that will never come. */
                m->overflow = false;
                m->len      = 0;
                reply_err(m, "CFG_ERR_JSON", "line longer than the protocol allows",
                          NULL, 0, -1);
            } else {
                cfg_msg_line(m, m->line, m->len);
                m->len = 0;
            }
            continue;
        }

        if (m->overflow) continue;                 /* still discarding to the newline */
        if (m->len >= CFG_MSG_LINE_MAX) { m->overflow = true; m->len = 0; continue; }
        m->line[m->len++] = c;
    }
}
