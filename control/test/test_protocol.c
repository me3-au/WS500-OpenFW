/*
 * test_protocol.c — USB-CDC JSON-lines config protocol (GH#16), native tests.
 *
 * Covers the three layers that landed together, in the order a byte travels:
 * the bounded JSON reader/writer (config_json.h), the PROFILE_SPEC §7 document
 * mapping (config_doc.h), and the message dispatch + line assembly
 * (config_msg.h). The protocol is deliberately host-injected, so all of it runs
 * here with an in-RAM config and no transport at all.
 *
 * The two tests that earn their keep on a safety-relevant device:
 *   - the ROUND TRIP: cfg-get's own output, fed straight back as cfg-set, must
 *     reproduce the config bit for bit. Anything less means the client's "save"
 *     button silently edits values the user never touched.
 *   - the ERROR SURFACE: every validator rule reachable from the wire comes back
 *     under its own enum name, never repaired and never renamed.
 *
 * SPDX-License-Identifier: MIT
 */
#define _CRT_SECURE_NO_WARNINGS   /* MSVC: strcpy/sprintf are fine in a test */

#include "test.h"
#include "test_config.h"

#include "config_json.h"
#include "config_doc.h"
#include "config_msg.h"

#include <string.h>
#include <stdlib.h>

/* ---- capture sink + host --------------------------------------------------- */

static char g_cap[16384];
static int  g_cap_len;

static void cap_sink(void *ctx, const char *d, int n)
{
    (void)ctx;
    if (g_cap_len + n < (int)sizeof g_cap) {
        memcpy(g_cap + g_cap_len, d, (size_t)n);
        g_cap_len += n;
        g_cap[g_cap_len] = '\0';
    }
}

static ctrl_config_t g_live;
static uint32_t      g_gen;
static bool          g_save_ok = true;
static int           g_saves;

static const ctrl_config_t *h_get(void *ctx) { (void)ctx; return &g_live; }

static bool h_save(void *ctx, const ctrl_config_t *cfg, uint32_t *generation)
{
    (void)ctx;
    if (!g_save_ok) return false;
    g_live = *cfg;
    g_gen++;
    g_saves++;
    if (generation != NULL) *generation = g_gen;
    return true;
}

static cfg_msg_t g_msg;

static void proto_reset(const char *git)
{
    const cfg_msg_host_t host = {
        h_get, h_save, NULL, cap_sink, NULL, "0.1.0-dev", git
    };
    cfg_msg_init(&g_msg, &host);
    g_live    = cfg_test_baseline();
    g_gen     = 7;            /* a non-zero generation, so "ok" cannot fake it */
    g_save_ok = true;
    g_saves   = 0;
}

/* Send one line and return the captured reply (NUL-terminated, "" if silent). */
static const char *send(const char *line)
{
    g_cap_len = 0;
    g_cap[0]  = '\0';
    cfg_msg_rx(&g_msg, (const uint8_t *)line, (int)strlen(line));
    cfg_msg_rx(&g_msg, (const uint8_t *)"\n", 1);
    return g_cap;
}

static bool has(const char *hay, const char *needle)
{
    return strstr(hay, needle) != NULL;
}

/* ---- field-wise config comparison ------------------------------------------ */

/* Bit comparison, not ==: NAN is a live "disabled" value here and must compare
 * equal to itself for a round-trip check to mean anything. */
static bool fbits(float a, float b)
{
    union { float f; uint32_t u; } x, y;
    x.f = a; y.f = b;
    return x.u == y.u;
}

static bool cfg_same(const ctrl_config_t *a, const ctrl_config_t *b)
{
    if (!fbits(a->prim.v_bulk, b->prim.v_bulk)) return false;
    if (!fbits(a->prim.v_bulk_cons, b->prim.v_bulk_cons)) return false;
    if (!fbits(a->prim.v_float, b->prim.v_float)) return false;
    if (!fbits(a->prim.v_float_cons, b->prim.v_float_cons)) return false;
    if (!fbits(a->prim.v_limp, b->prim.v_limp)) return false;

    const ctrl_globals_t *x = &a->globals, *y = &b->globals;
    if (x->cells_series != y->cells_series) return false;
    if (!fbits(x->bank_capacity_ah, y->bank_capacity_ah)) return false;
    if (!fbits(x->max_charge_power_w, y->max_charge_power_w)) return false;
    if (!fbits(x->ramp_w_per_s, y->ramp_w_per_s)) return false;
    if (!fbits(x->p_tail_w, y->p_tail_w)) return false;
    if (x->t_tail_hold_s != y->t_tail_hold_s) return false;
    if (x->t_vclamp_s != y->t_vclamp_s) return false;
    if (x->cv_hold_exit_min != y->cv_hold_exit_min) return false;
    if (x->t_charge_max_min != y->t_charge_max_min) return false;
    if (x->warmup_time_s != y->warmup_time_s) return false;
    if (!fbits(x->warmup_coolant_c, y->warmup_coolant_c)) return false;
    if (x->soc_target_pct != y->soc_target_pct) return false;
    if (!fbits(x->skip_bulk_vcell, y->skip_bulk_vcell)) return false;
    if (x->skip_bulk_soc_pct != y->skip_bulk_soc_pct) return false;
    if (!fbits(x->rotor_rated_v, y->rotor_rated_v)) return false;
    if (!fbits(x->rotor_v_max, y->rotor_v_max)) return false;
    if (x->allow_full_field_48v != y->allow_full_field_48v) return false;
    if (!fbits(x->limp_vcell, y->limp_vcell)) return false;
    if (!fbits(x->limp_power_cap_w, y->limp_power_cap_w)) return false;

    if (!fbits(a->limits.battery_c_limit, b->limits.battery_c_limit)) return false;
    if (!fbits(a->limits.wiring_limit_a, b->limits.wiring_limit_a)) return false;
    if (!fbits(a->limits.alternator_limit_a, b->limits.alternator_limit_a)) return false;

    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++) {
        const cfg_profile_t *p = &a->profiles[i], *q = &b->profiles[i];
        if (p->flags != q->flags) return false;
        if (p->cv_target_ref != q->cv_target_ref) return false;
        if (p->rest_voltage_ref != q->rest_voltage_ref) return false;
        if (p->p.id != q->p.id) return false;
        if (!fbits(p->p.cv_target_vcell, q->p.cv_target_vcell)) return false;
        if (p->p.exit_at_cv_entry != q->p.exit_at_cv_entry) return false;
        if (p->p.rest_mode != q->p.rest_mode) return false;
        if (!fbits(p->p.rest_voltage_vcell, q->p.rest_voltage_vcell)) return false;
        if (!fbits(p->p.rest_power_cap_w, q->p.rest_power_cap_w)) return false;
        if (!fbits(p->p.v_revert_vcell, q->p.v_revert_vcell)) return false;
        if (p->p.t_revert_hold_s != q->p.t_revert_hold_s) return false;
        if (p->p.soc_revert_pct != q->p.soc_revert_pct) return false;
        if (!fbits(p->p.ah_revert, q->p.ah_revert)) return false;
    }
    return a->active_profile == b->active_profile;
}

/* ---- 1. parser accept / reject matrix -------------------------------------- */

static void test_parser_matrix(void)
{
    static const char *const ACCEPT[] = {
        "{}", "[]", "{\"a\":1}", "{ \"a\" : [ 1 , 2 , { \"b\" : null } ] }",
        "1", "0", "-0", "-0.5e-3", "1e999", "\"x\"", "true", "false", "null",
        "  {\"a\":1}  ", "{\"a\":\"\\u00e9\"}", "{\"a\":\"\\\"\\\\\\/\\b\\f\\n\\r\\t\"}",
        "[[[[[[1]]]]]]",                    /* exactly CFG_JSON_DEPTH_MAX levels */
        "{\"k\":\"\xC3\xA9 \xFF\xFE raw\"}" /* invalid UTF-8 passes through */
    };
    for (unsigned i = 0; i < sizeof ACCEPT / sizeof ACCEPT[0]; i++) {
        const cfg_json_err_t e = cfg_json_validate(ACCEPT[i], (int)strlen(ACCEPT[i]));
        if (e != CFG_JSON_OK) printf("  accept case %u: %s\n", i, ACCEPT[i]);
        CHECK(e == CFG_JSON_OK);
    }

    struct { const char *text; cfg_json_err_t want; } REJECT[] = {
        { "",                 CFG_JSON_ERR_TRUNCATED },
        { "{",                CFG_JSON_ERR_TRUNCATED },
        { "{\"a\"",           CFG_JSON_ERR_TRUNCATED },
        { "{\"a\":",          CFG_JSON_ERR_TRUNCATED },
        { "{\"a\":1",         CFG_JSON_ERR_TRUNCATED },
        { "[1,2",             CFG_JSON_ERR_TRUNCATED },
        { "{\"a\":\"unterminated", CFG_JSON_ERR_TRUNCATED },
        { "{\"a\":}",         CFG_JSON_ERR_SYNTAX },
        { "{\"a\":1,}",       CFG_JSON_ERR_SYNTAX },   /* no trailing commas */
        { "[1,]",             CFG_JSON_ERR_SYNTAX },
        { "{'a':1}",          CFG_JSON_ERR_SYNTAX },   /* no single quotes */
        { "{a:1}",            CFG_JSON_ERR_SYNTAX },   /* no bare keys */
        { "+1",               CFG_JSON_ERR_SYNTAX },
        { ".5",               CFG_JSON_ERR_SYNTAX },
        { "5.",               CFG_JSON_ERR_SYNTAX },
        { "01",               CFG_JSON_ERR_SYNTAX },   /* leading zero */
        { "1.2.3",            CFG_JSON_ERR_SYNTAX },
        { "1e",               CFG_JSON_ERR_SYNTAX },
        { "NaN",              CFG_JSON_ERR_SYNTAX },
        { "Infinity",         CFG_JSON_ERR_SYNTAX },
        { "{\"a\":\"\\q\"}",  CFG_JSON_ERR_SYNTAX },   /* unknown escape */
        { "{\"a\":\"x\ty\"}", CFG_JSON_ERR_SYNTAX },   /* raw control char */
        { "{\"a\":1}x",       CFG_JSON_ERR_TRAILING },
        { "{\"a\":1} {\"b\":2}", CFG_JSON_ERR_TRAILING },
        { "[[[[[[[1]]]]]]]",  CFG_JSON_ERR_DEPTH },    /* depth bomb, one over */
    };
    for (unsigned i = 0; i < sizeof REJECT / sizeof REJECT[0]; i++) {
        const cfg_json_err_t e = cfg_json_validate(REJECT[i].text,
                                                   (int)strlen(REJECT[i].text));
        if (e != REJECT[i].want)
            printf("  reject case %u (%s): got %d want %d\n",
                   i, REJECT[i].text, (int)e, (int)REJECT[i].want);
        CHECK(e == REJECT[i].want);
    }

    /* A depth bomb of any size costs one error, not one stack frame per level. */
    {
        static char bomb[512];
        int n = 0;
        for (int i = 0; i < 200; i++) bomb[n++] = '[';
        bomb[n++] = '1';
        for (int i = 0; i < 200; i++) bomb[n++] = ']';
        CHECK(cfg_json_validate(bomb, n) == CFG_JSON_ERR_DEPTH);
    }

    /* A number token longer than CFG_JSON_NUM_CHARS_MAX is refused AT THE TOKEN,
     * so a megabyte of digits never gets accumulated. */
    {
        static char big[400];
        int n = 0;
        big[n++] = '[';
        for (int i = 0; i < 300; i++) big[n++] = '9';
        big[n++] = ']';
        CHECK(cfg_json_validate(big, n) == CFG_JSON_ERR_TOOLONG);
    }

    /* Huge-but-short numbers are legal JSON; they become infinity and it is the
     * VALIDATOR's job to refuse them, not the parser's. */
    {
        double v = 0.0;
        int    used = 0;
        CHECK(cfg_json_scan_num("1e999", 5, &used, &v) && used == 5);
        CHECK(v > 1e308);
        CHECK(cfg_json_scan_num("-1e999", 6, &used, &v) && used == 6);
        CHECK(v < -1e308);
        CHECK(cfg_json_scan_num("1e-999", 6, &used, &v) && used == 6 && v == 0.0);
    }

    /* An over-long key is reported as truncated, so the schema layer treats it
     * as unknown instead of aliasing a real key. */
    {
        static char line[256];
        strcpy(line, "{\"");
        for (int i = 0; i < 120; i++) strcat(line, "k");
        strcat(line, "\":1}");
        cfg_json_t j;
        cfg_json_init(&j, line, (int)strlen(line));
        char k[CFG_JSON_KEY_MAX];
        bool tr = false;
        CHECK(cfg_json_obj_begin(&j));
        CHECK(cfg_json_obj_key(&j, k, (int)sizeof k, &tr));
        CHECK(tr);
        CHECK((int)strlen(k) == CFG_JSON_KEY_MAX - 1);
    }
}

/* ---- 2. number formatting / scanning --------------------------------------- */

static float emit_parse_f32(float v)
{
    char b[64];
    cfg_json_w_t w;
    cfg_json_w_init(&w, b, (int)sizeof b, NULL, NULL);
    cfg_json_w_f32(&w, v);
    (void)cfg_json_w_flush(&w);
    b[w.len] = '\0';

    double back = 0.0;
    int    used = 0;
    if (!cfg_json_scan_num(b, w.len, &used, &back) || used != w.len) return NAN;
    return (float)back;
}

static void test_number_format(void)
{
    struct { double v; const char *want; } G[] = {
        { 0.0,      "0"          },
        { 3.6,      "3.6"        },
        { 8000.0,   "8000"       },
        { 662.4,    "662.4"      },
        { 250.0,    "250"        },
        { 0.5,      "0.5"        },
        { 0.0001,   "0.0001"     },
        { 0.00001,  "1e-05"      },
        { -3.28,    "-3.28"      },
        { 12345.0,  "1.235e+04"  },   /* 4 significant digits, %g's own rule */
    };
    for (unsigned i = 0; i < sizeof G / sizeof G[0]; i++) {
        char b[40];
        const int n = cfg_json_fmt_g(b, (int)sizeof b, G[i].v, CFG_JSON_SIG_DEFAULT);
        if (n < 0 || strcmp(b, G[i].want) != 0)
            printf("  fmt_g(%g) = \"%s\" want \"%s\"\n", G[i].v, (n < 0) ? "?" : b, G[i].want);
        CHECK(n > 0 && strcmp(b, G[i].want) == 0);
    }

    /* The fidelity guard: 12345 W is a legal max_charge_power_w and MUST NOT
     * come back as 12350 just because four digits reads nicer. */
    CHECK(fbits(emit_parse_f32(12345.0f), 12345.0f));

    /* Sweep: every one of these must survive emit -> parse bit-exactly. */
    static const float SWEEP[] = {
        0.0f, -0.0f, 1.0f, -1.0f, 0.5f, 3.2f, 3.28f, 3.35f, 3.45f, 3.65f,
        12.0f, 100.0f, 250.0f, 662.4f, 1656.0f, 8000.0f, 12345.0f, 19999.0f,
        0.3333333f, 1.0e-7f, 1.0e20f, -7.75e-3f, 3000.0f, 0.04f
    };
    for (unsigned i = 0; i < sizeof SWEEP / sizeof SWEEP[0]; i++) {
        if (!fbits(emit_parse_f32(SWEEP[i]), SWEEP[i]))
            printf("  float round-trip failed for %g\n", (double)SWEEP[i]);
        CHECK(fbits(emit_parse_f32(SWEEP[i]), SWEEP[i]));
    }

    /* Pseudo-random sweep over the magnitudes a config can hold. */
    {
        unsigned seed = 20260726u;
        int bad = 0;
        for (int i = 0; i < 2000; i++) {
            seed = seed * 1103515245u + 12345u;
            const float mag = (float)((seed >> 16) % 10000u) / 100.0f;
            seed = seed * 1103515245u + 12345u;
            const int   dec = (int)((seed >> 16) % 7u) - 3;
            float v = mag;
            for (int k = 0; k < dec; k++)  v *= 10.0f;
            for (int k = 0; k > dec; k--)  v /= 10.0f;
            if (!fbits(emit_parse_f32(v), v)) bad++;
        }
        if (bad) printf("  %d/2000 random floats did not round-trip\n", bad);
        CHECK(bad == 0);
    }

    /* Non-finite values travel as null — JSON has no NaN, and NAN IS the
     * configured "disabled" for warmup_coolant_c / rotor_v_max. */
    {
        char b[32];
        cfg_json_w_t w;
        cfg_json_w_init(&w, b, (int)sizeof b, NULL, NULL);
        cfg_json_w_f32(&w, NAN);
        (void)cfg_json_w_flush(&w);
        b[w.len] = '\0';
        CHECK(strcmp(b, "null") == 0);
    }
}

/* ---- 3. hello handshake (VERSIONING §2) ------------------------------------ */

static void test_hello(void)
{
    proto_reset("");
    const char *r = send("{\"t\":\"hello\",\"proto\":1}");
    CHECK(has(r, "\"t\":\"hello\""));
    CHECK(has(r, "\"fw\":\"0.1.0-dev\""));
    CHECK(has(r, "\"proto\":1"));
    CHECK(has(r, "\"schema\":1"));
    /* Only what THIS deliverable ships. Telemetry adds its own flag later. */
    CHECK(has(r, "\"caps\":[\"cfg\"]"));
    CHECK(!has(r, "\"telem\""));
    CHECK(!has(r, "\"git\""));          /* empty hash omits the field entirely */
    CHECK(r[strlen(r) - 1] == '\n');

    /* A client from the future gets the same honest answer — the firmware never
     * refuses a hello; refusing is the client's job (VERSIONING §2). */
    const char *r2 = send("{\"t\":\"hello\",\"proto\":99}");
    CHECK(has(r2, "\"proto\":1") && has(r2, "\"caps\":[\"cfg\"]"));

    /* proto is optional, and key order is not significant. */
    CHECK(has(send("{\"t\":\"hello\"}"), "\"proto\":1"));
    CHECK(has(send("{\"proto\":1,\"t\":\"hello\"}"), "\"schema\":1"));

    proto_reset("deadbee");
    CHECK(has(send("{\"t\":\"hello\",\"proto\":1}"), "\"git\":\"deadbee\""));
}

/* ---- 4. cfg-get / cfg-set round trip --------------------------------------- */

static char g_doc[8192];

/* Turn a cfg-get reply into the cfg-set that sends it straight back. */
static int reply_to_cfg_set(const char *reply, char *out, int cap)
{
    static const char PFX[] = "{\"t\":\"cfg\",\"cfg\":";
    const int plen = (int)strlen(PFX);
    int       rlen = (int)strlen(reply);
    if (rlen < plen || strncmp(reply, PFX, (size_t)plen) != 0) return -1;
    while (rlen > 0 && (reply[rlen - 1] == '\n' || reply[rlen - 1] == '\r')) rlen--;
    if (rlen <= plen || reply[rlen - 1] != '}') return -1;

    const char *doc = reply + plen;
    const int   dlen = rlen - plen - 1;          /* drop the envelope's closing } */
    const int   need = 20 + dlen + 2;
    if (need >= cap) return -1;
    int n = 0;
    n += sprintf(out + n, "{\"t\":\"cfg-set\",\"cfg\":");
    memcpy(out + n, doc, (size_t)dlen);
    n += dlen;
    out[n++] = '}';
    out[n]   = '\0';
    return n;
}

static void test_round_trip(void)
{
    proto_reset("");
    const ctrl_config_t baseline = g_live;

    const char *get = send("{\"t\":\"cfg-get\"}");
    CHECK(has(get, "\"t\":\"cfg\""));
    CHECK(has(get, "\"schema_version\":1"));
    CHECK(has(get, "\"fw\":\"0.1.0-dev\""));
    CHECK(has(get, "\"cv_target\":\"v_bulk\""));           /* §3.3 primitive ref */
    CHECK(has(get, "\"name\":\"Bulk, Float Norm\""));      /* §4.1 name table */
    CHECK(has(get, "\"rotor_v_max\":null"));               /* NAN -> null */
    CHECK(has(get, "\"warmup_coolant_c\":null"));
    CHECK(has(get, "\"soc_target_pct\":null"));            /* -1 -> null */
    CHECK(has(get, "\"reserved\":true"));                  /* profile 7 */

    const int n = reply_to_cfg_set(get, g_doc, (int)sizeof g_doc);
    CHECK(n > 0);
    printf("  cfg-get document: %d bytes; cfg-set line %d of %d max\n",
           (int)strlen(get) - 1, n, CFG_MSG_LINE_MAX);
    /* The line buffer MUST be able to hold what the firmware itself emits, with
     * headroom — this is the assertion CFG_MSG_LINE_MAX is sized against. */
    CHECK(n + 64 < CFG_MSG_LINE_MAX);

    /* Scramble the live config first, so the round trip proves the document
     * carries every field rather than that nothing changed. */
    g_live.prim.v_bulk             = 3.50f;
    g_live.globals.max_charge_power_w = 1234.0f;
    g_live.globals.cells_series    = 8;
    g_live.globals.rotor_v_max     = 15.0f;
    g_live.globals.soc_target_pct  = 88;
    g_live.limits.wiring_limit_a   = 1.0f;
    g_live.profiles[3].p.rest_power_cap_w = 1.0f;
    g_live.profiles[6].flags       = 0u;
    g_live.active_profile          = 5;

    const char *ok = send(g_doc);
    if (!has(ok, "\"t\":\"ok\"")) printf("  cfg-set reply: %s", ok);
    CHECK(has(ok, "\"t\":\"ok\""));
    CHECK(has(ok, "\"generation\":8"));
    CHECK(has(ok, "\"unknown_keys\":0"));
    CHECK(cfg_same(&g_live, &baseline));

    /* And the second lap is a fixed point: emit -> parse -> emit is identical. */
    const char *get2 = send("{\"t\":\"cfg-get\"}");
    CHECK(strcmp(get, get2) == 0);

    /* A five-significant-digit power must survive the wire (fidelity guard). */
    proto_reset("");
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"max_charge_power_w\":12345}}}"),
              "\"t\":\"ok\""));
    CHECK(fbits(g_live.globals.max_charge_power_w, 12345.0f));

    /* Overlay semantics: a one-key document changes one key. */
    proto_reset("");
    const ctrl_config_t before = g_live;
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"ramp_w_per_s\":150}}}"),
              "\"t\":\"ok\""));
    CHECK(fbits(g_live.globals.ramp_w_per_s, 150.0f));
    CHECK(fbits(g_live.globals.p_tail_w, before.globals.p_tail_w));
    CHECK(g_live.active_profile == before.active_profile);

    /* §1 propagation: editing a primitive moves every profile that references
     * it, and mirrors v_limp into globals.limp_vcell — even though "profiles"
     * is listed BEFORE "primitives" in this document. */
    proto_reset("");
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{"
                   "\"profiles\":[{\"id\":1,\"cv_target\":\"v_bulk\"}],"
                   "\"primitives\":{\"v_bulk\":3.55,\"v_limp\":3.25}}}"),
              "\"t\":\"ok\""));
    CHECK(fbits(g_live.profiles[0].p.cv_target_vcell, 3.55f));
    CHECK(fbits(g_live.globals.limp_vcell, 3.25f));
}

/* ---- 5. unknown-key counting ----------------------------------------------- */

static void test_unknown_keys(void)
{
    proto_reset("");

    /* One at the envelope, one per §7 block with no schema-v1 home. */
    const char *r = send(
        "{\"t\":\"cfg-set\",\"extra\":true,\"cfg\":{"
        "\"nope\":1,"
        "\"engine\":{\"idle_rpm\":800,\"curve_full\":[[800,400]]},"
        "\"limits\":{\"belt\":{\"w_ref\":4000,\"rpm_ref\":4000}},"
        "\"global\":{\"topbalance_days\":10},"
        "\"profiles\":[{\"id\":1,\"rest\":{\"power_cap_pct\":100},"
        "\"revert\":{\"ah_frac_c\":0.3}}]"
        "}}");
    if (!has(r, "\"unknown_keys\":7")) printf("  unknown-key reply: %s", r);
    CHECK(has(r, "\"t\":\"ok\""));
    CHECK(has(r, "\"unknown_keys\":7"));

    /* `name` and `fw` are KNOWN keys the firmware simply does not store — they
     * must not inflate the count, or every round trip would look lossy. */
    proto_reset("");
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"schema_version\":1,\"fw\":\"x\","
                   "\"profiles\":[{\"id\":1,\"name\":\"Anything\"}]}}"),
              "\"unknown_keys\":0"));

    /* An over-long key cannot alias a real one; it is counted as unknown. */
    proto_reset("");
    {
        static char line[512];
        strcpy(line, "{\"t\":\"cfg-set\",\"cfg\":{\"");
        for (int i = 0; i < 100; i++) strcat(line, "z");
        strcat(line, "\":1}}");
        CHECK(has(send(line), "\"unknown_keys\":1"));
    }
}

/* ---- 6. validator errors, by name ------------------------------------------ */

static void expect_code(const char *patch, cfg_err_t want)
{
    static char line[1024];
    proto_reset("");
    sprintf(line, "{\"t\":\"cfg-set\",\"cfg\":%s}", patch);
    const char *r = send(line);

    static char needle[96];
    sprintf(needle, "\"code\":\"%s\"", cfg_err_name(want));
    if (!has(r, needle)) printf("  want %s for %s\n     got %s", needle, patch, r);
    CHECK(has(r, needle));
    CHECK(has(r, "\"t\":\"err\""));
    /* Rejected means rejected: nothing reached the store. */
    CHECK(g_saves == 0);
}

static void test_validator_surface(void)
{
    /* §1 primitives. */
    expect_code("{\"primitives\":{\"v_limp\":3.0}}",          CFG_ERR_FLOOR_LIMP);
    expect_code("{\"primitives\":{\"v_bulk\":9}}",            CFG_ERR_RANGE_V_BULK);
    expect_code("{\"primitives\":{\"v_bulk_cons\":3.0}}",     CFG_ERR_RANGE_V_BULK_CONS);
    expect_code("{\"primitives\":{\"v_float\":3.0}}",         CFG_ERR_RANGE_V_FLOAT);
    expect_code("{\"primitives\":{\"v_float_cons\":3.0}}",    CFG_ERR_RANGE_V_FLOAT_CONS);
    expect_code("{\"primitives\":{\"v_limp\":3.5}}",          CFG_ERR_RANGE_V_LIMP);
    expect_code("{\"primitives\":{\"v_bulk\":3.50}}",         CFG_ERR_ORDER_BULK_CONS);
    expect_code("{\"primitives\":{\"v_bulk_cons\":3.42}}",    CFG_ERR_ORDER_CONS_FLOAT);
    expect_code("{\"primitives\":{\"v_float\":3.36}}",        CFG_ERR_ORDER_FLOAT_FCONS);
    expect_code("{\"primitives\":{\"v_float_cons\":3.31}}",   CFG_ERR_ORDER_FCONS_LIMP);
    /* Reachable only because the document may state limp_vcell explicitly; a
     * plain v_limp edit mirrors instead (see config_doc.c resolve_after_parse). */
    expect_code("{\"primitives\":{\"v_limp\":3.30},\"global\":{\"limp_vcell\":3.25}}",
                CFG_ERR_LIMP_VCELL_MISMATCH);

    /* §3.1 globals + §3.2 tail + §5.1 field. */
    expect_code("{\"global\":{\"cells_series\":3}}",          CFG_ERR_RANGE_CELLS);
    expect_code("{\"global\":{\"bank_capacity_ah\":10}}",     CFG_ERR_RANGE_BANK_AH);
    expect_code("{\"global\":{\"max_charge_power_w\":50}}",   CFG_ERR_RANGE_MAX_POWER);
    expect_code("{\"global\":{\"ramp_w_per_s\":5}}",          CFG_ERR_RANGE_RAMP);
    expect_code("{\"global\":{\"t_vclamp_s\":1}}",            CFG_ERR_RANGE_T_VCLAMP);
    expect_code("{\"global\":{\"t_charge_max_min\":10}}",     CFG_ERR_RANGE_T_CHARGE_MAX);
    expect_code("{\"global\":{\"warmup_time_s\":700}}",       CFG_ERR_RANGE_WARMUP_TIME);
    expect_code("{\"global\":{\"warmup_coolant_c\":10}}",     CFG_ERR_RANGE_WARMUP_COOLANT);
    expect_code("{\"global\":{\"soc_target_pct\":50}}",       CFG_ERR_RANGE_SOC_TARGET);
    expect_code("{\"global\":{\"p_tail_w\":1}}",              CFG_ERR_RANGE_P_TAIL);
    expect_code("{\"global\":{\"t_tail_hold_s\":5}}",         CFG_ERR_RANGE_T_TAIL_HOLD);
    expect_code("{\"limits\":{\"field\":{\"rotor_rated_v\":0}}}",  CFG_ERR_SANITY_ROTOR_RATED);
    expect_code("{\"limits\":{\"field\":{\"rotor_v_max\":-1}}}",   CFG_ERR_SANITY_ROTOR_VMAX);
    expect_code("{\"global\":{\"limp_power_cap_w\":-1}}",     CFG_ERR_SANITY_LIMP_POWER_CAP);
    expect_code("{\"global\":{\"skip_bulk_vcell\":-1}}",      CFG_ERR_SANITY_SKIP_BULK_VCELL);
    expect_code("{\"global\":{\"skip_bulk_soc_pct\":120}}",   CFG_ERR_RANGE_SKIP_BULK_SOC);

    /* §2.1 limit set. */
    expect_code("{\"limits\":{\"battery_c_limit\":-1}}",      CFG_ERR_SANITY_BATTERY_C);
    expect_code("{\"limits\":{\"wiring_limit_a\":-1}}",       CFG_ERR_SANITY_WIRING_A);
    expect_code("{\"limits\":{\"alternator_limit_a\":-1}}",   CFG_ERR_SANITY_ALT_A);

    /* §3.3 / §4.1 profiles. */
    expect_code("{\"profiles\":[{\"id\":5}]}",                        CFG_ERR_PROFILE_ID);
    expect_code("{\"profiles\":[{\"rest\":{\"mode\":\"nope\"}}]}",    CFG_ERR_PROFILE_REST_MODE);
    expect_code("{\"profiles\":[{\"cv_target\":9.0}]}",               CFG_ERR_PROFILE_CV_RANGE);
    expect_code("{\"profiles\":[{\"rest\":{\"voltage\":9.0}}]}",      CFG_ERR_PROFILE_REST_V_RANGE);
    expect_code("{\"profiles\":[{\"rest\":{\"power_cap_w\":-1}}]}",   CFG_ERR_SANITY_REST_POWER_CAP);
    expect_code("{\"profiles\":[{\"revert\":{\"v_cell\":-1}}]}",      CFG_ERR_SANITY_V_REVERT);
    expect_code("{\"profiles\":[{\"revert\":{\"soc_pct\":120}}]}",    CFG_ERR_RANGE_SOC_REVERT);
    expect_code("{\"profiles\":[{\"revert\":{\"ah\":-1}}]}",          CFG_ERR_SANITY_AH_REVERT);
    expect_code("{\"active_profile\":9}",                             CFG_ERR_ACTIVE_PROFILE);
    /* Selecting the reserved slot 7 is the same code, with the profile named. */
    {
        proto_reset("");
        const char *r = send("{\"t\":\"cfg-set\",\"cfg\":{\"active_profile\":7}}");
        CHECK(has(r, "\"code\":\"CFG_ERR_ACTIVE_PROFILE\""));
        CHECK(has(r, "\"profile\":7"));
    }

    /* An out-of-range value is REPORTED, never repaired: the live config is
     * untouched after every rejection above. */
    {
        proto_reset("");
        const ctrl_config_t before = g_live;
        (void)send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"max_charge_power_w\":50}}}");
        CHECK(cfg_same(&g_live, &before));
    }

    /*
     * The name table must cover the whole enum — that is what lets a client
     * match on "code" instead of parsing prose. CFG_ERR_ARG,
     * CFG_ERR_PROFILE_CV_REF and CFG_ERR_PROFILE_REST_V_REF are NOT probed
     * above and cannot be: a NULL config never reaches the wire, and a JSON
     * document has no way to state a reference and a disagreeing value at the
     * same time (config_doc.c resolves refs after parsing). They guard the
     * EEPROM path, so their names are all the wire owes them.
     */
    for (int e = 0; e < (int)CFG_ERR__COUNT; e++) {
        const char *name = cfg_err_name((cfg_err_t)e);
        CHECK(strcmp(name, "CFG_ERR_UNKNOWN") != 0);
        CHECK(strncmp(name, "CFG_", 4) == 0);
        for (int f = 0; f < e; f++)
            CHECK(strcmp(name, cfg_err_name((cfg_err_t)f)) != 0);
    }
}

/* ---- 7. protocol-level errors + line assembly ------------------------------ */

static void test_message_errors(void)
{
    proto_reset("");

    CHECK(has(send("{\"t\":\"cfg-get\","), "\"code\":\"CFG_ERR_JSON\""));
    CHECK(has(send("not json at all"),     "\"code\":\"CFG_ERR_JSON\""));
    CHECK(has(send("[1,2,3]"),             "\"code\":\"CFG_ERR_MSG\""));
    CHECK(has(send("{\"x\":1}"),           "\"code\":\"CFG_ERR_MSG\""));
    CHECK(has(send("{\"t\":42}"),          "\"code\":\"CFG_ERR_MSG\""));
    CHECK(has(send("{\"t\":\"telemetry\"}"), "\"code\":\"CFG_ERR_MSG\""));
    CHECK(has(send("{\"t\":\"cfg-set\"}"), "\"code\":\"CFG_ERR_MSG\""));
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":7}"), "\"code\":\"CFG_ERR_MSG\""));

    /* Wrong schema version is a refusal, not a migration (VERSIONING §3). */
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"schema_version\":2}}"),
              "\"code\":\"CFG_ERR_SCHEMA\""));

    /* Type errors name the key that could not be used. */
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"cells_series\":\"eight\"}}}"),
              "\"code\":\"CFG_ERR_TYPE\""));
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"cells_series\":8.5}}}"),
              "\"key\":\"cells_series\""));
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"t_vclamp_s\":70000}}}"),
              "\"code\":\"CFG_ERR_TYPE\""));
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"profiles\":[{\"cv_target\":\"v_nope\"}]}}"),
              "\"key\":\"cv_target\""));
    CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"profiles\":[{},{},{},{},{},{},{},{}]}}"),
              "\"key\":\"profiles\""));

    /* A store that refuses is reported as such — and nothing is adopted. */
    {
        proto_reset("");
        const ctrl_config_t before = g_live;
        g_save_ok = false;
        CHECK(has(send("{\"t\":\"cfg-set\",\"cfg\":{\"global\":{\"ramp_w_per_s\":150}}}"),
                  "\"code\":\"CFG_ERR_STORE\""));
        CHECK(cfg_same(&g_live, &before));
    }

    /* Blank lines are silent; CRLF is tolerated; several lines may arrive in one
     * chunk and are answered in order. */
    proto_reset("");
    g_cap_len = 0; g_cap[0] = '\0';
    cfg_msg_rx(&g_msg, (const uint8_t *)"\n \n\r\n", 5);
    CHECK(g_cap_len == 0);

    g_cap_len = 0; g_cap[0] = '\0';
    cfg_msg_rx(&g_msg, (const uint8_t *)"{\"t\":\"hello\"}\r\n{\"t\":\"hello\"}\n", 29);
    {
        int lines = 0;
        for (int i = 0; i < g_cap_len; i++) if (g_cap[i] == '\n') lines++;
        CHECK(lines == 2);
    }

    /* A line split across chunks is reassembled. */
    g_cap_len = 0; g_cap[0] = '\0';
    cfg_msg_rx(&g_msg, (const uint8_t *)"{\"t\":\"he", 8);
    CHECK(g_cap_len == 0);
    cfg_msg_rx(&g_msg, (const uint8_t *)"llo\"}\n", 6);
    CHECK(has(g_cap, "\"caps\":[\"cfg\"]"));

    /* Over-long line: dropped to the newline, answered once, and the NEXT line
     * still works — the parser must not be left mid-line. */
    {
        static char huge[CFG_MSG_LINE_MAX + 200];
        int n = 0;
        n += sprintf(huge, "{\"t\":\"cfg-set\",\"cfg\":{\"pad\":\"");
        while (n < CFG_MSG_LINE_MAX + 100) huge[n++] = 'x';
        huge[n] = '\0';
        g_cap_len = 0; g_cap[0] = '\0';
        cfg_msg_rx(&g_msg, (const uint8_t *)huge, n);
        CHECK(g_cap_len == 0);                    /* nothing until the newline */
        cfg_msg_rx(&g_msg, (const uint8_t *)"\"}}\n", 4);
        CHECK(has(g_cap, "\"code\":\"CFG_ERR_JSON\""));
        CHECK(has(g_cap, "line longer"));
        CHECK(has(send("{\"t\":\"hello\"}"), "\"caps\":[\"cfg\"]"));
    }
}

void test_protocol(void)
{
    const int at_entry = g_checks;
    printf("-- protocol (JSON config wire, GH#16)\n");
    /* Printed, not asserted: the STM32F072 has 16 KB of RAM and this instance is
     * the single biggest static object the firmware owns. If it ever stops being
     * a comfortable fraction of that, CFG_MSG_LINE_MAX is the knob. */
    printf("  [PROTO] sizeof cfg_msg_t = %u B (ctrl_config_t = %u B)\n",
           (unsigned)sizeof(cfg_msg_t), (unsigned)sizeof(ctrl_config_t));
    test_parser_matrix();
    test_number_format();
    test_hello();
    test_round_trip();
    test_unknown_keys();
    test_validator_surface();
    test_message_errors();
    printf("  [PROTO] %d checks\n", g_checks - at_entry);
}
