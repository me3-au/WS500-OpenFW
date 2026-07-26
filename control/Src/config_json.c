/*
 * config_json.c — bounded JSON reader + writer for the config wire (GH#16). PURE.
 * Grammar, bounds and design rationale: config_json.h.
 *
 * Two things in here exist only because of the target, and both would be a
 * mistake to "simplify" away:
 *
 *   1. cfg_json_fmt_g() is a hand-rolled "%g". The firmware links newlib-nano
 *      (cmake/gcc-arm-none-eabi.cmake, --specs=nano.specs), whose printf has NO
 *      floating-point conversion at all — snprintf("%.4g", x) would silently
 *      emit garbage on the device while passing every host test. So the float
 *      formatter is ours, in integer arithmetic over a scaled mantissa.
 *   2. cfg_json_scan_num() is a hand-rolled strtod. Same reason plus locale:
 *      strtod honours LC_NUMERIC, and a config wire whose decimal separator
 *      depends on a locale is a bug waiting for a European bench session.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_json.h"

#include <math.h>
#include <string.h>

/* ---- small character helpers ---------------------------------------------- */
static bool is_ws(char c)    { return c == ' ' || c == '\t' || c == '\n' || c == '\r'; }
static bool is_digit(char c) { return c >= '0' && c <= '9'; }

/* Characters that may appear inside a number TOKEN. Used only to find the token
 * boundary cheaply; cfg_json_scan_num() is what decides it is a legal number. */
static bool is_num_char(char c)
{
    return is_digit(c) || c == '-' || c == '+' || c == '.' || c == 'e' || c == 'E';
}

static int hex_val(char c)
{
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

/* ---- powers of ten ---------------------------------------------------------
 * 1e0 .. 1e22 are EXACTLY representable as doubles, so a single multiply or
 * divide by one of these is correctly rounded. Everything here scales in one
 * step from the original value for that reason — repeatedly dividing by 10 to
 * walk an exponent accumulates error and costs digits we cannot spare. */
static const double POW10[23] = {
    1e0,  1e1,  1e2,  1e3,  1e4,  1e5,  1e6,  1e7,  1e8,  1e9,  1e10, 1e11,
    1e12, 1e13, 1e14, 1e15, 1e16, 1e17, 1e18, 1e19, 1e20, 1e21, 1e22
};

/* v x 10^n, splitting |n| > 22 into exact chunks. */
static double scale_pow10(double v, int n)
{
    while (n >= 22)  { v *= 1e22; n -= 22; }
    while (n <= -22) { v /= 1e22; n += 22; }
    if (n > 0)      v *= POW10[n];
    else if (n < 0) v /= POW10[-n];
    return v;
}

static uint32_t pow10u(int n)
{
    uint32_t r = 1u;
    for (int i = 0; i < n; i++) r *= 10u;
    return r;
}

/* ---- number scanner (strtod replacement) ---------------------------------- */
bool cfg_json_scan_num(const char *s, int len, int *consumed, double *out)
{
    if (s == NULL || out == NULL || consumed == NULL || len <= 0) return false;

    int  i   = 0;
    bool neg = false;
    if (s[i] == '-') { neg = true; i++; }

    /* RFC 8259: an integer part is mandatory and may not carry a leading zero.
     * Both rules are kept: "+1", ".5" and "01" are rejected, not guessed at. */
    if (i >= len || !is_digit(s[i])) return false;

    uint64_t mant  = 0;
    int      sig   = 0;    /* digits actually accumulated into mant */
    int      exp10 = 0;

    if (s[i] == '0') {
        i++;
        if (i < len && is_digit(s[i])) return false;   /* leading zero */
        sig = 1;
    } else {
        while (i < len && is_digit(s[i])) {
            /* 19 digits is past double's precision; further integer digits only
             * move the decimal exponent, they cannot change the value. */
            if (sig < 19) { mant = mant * 10u + (uint64_t)(s[i] - '0'); sig++; }
            else          { exp10++; }
            i++;
        }
    }

    if (i < len && s[i] == '.') {
        i++;
        if (i >= len || !is_digit(s[i])) return false;   /* "5." is not a number */
        while (i < len && is_digit(s[i])) {
            if (sig < 19) { mant = mant * 10u + (uint64_t)(s[i] - '0'); sig++; exp10--; }
            i++;                                          /* excess digits dropped */
        }
    }

    if (i < len && (s[i] == 'e' || s[i] == 'E')) {
        i++;
        int esign = 1;
        if (i < len && (s[i] == '+' || s[i] == '-')) { if (s[i] == '-') esign = -1; i++; }
        if (i >= len || !is_digit(s[i])) return false;
        int ev = 0;
        while (i < len && is_digit(s[i])) {
            if (ev < 100000) ev = ev * 10 + (s[i] - '0');   /* saturates; sign kept */
            i++;
        }
        exp10 += esign * ev;
    }

    double v;
    if (mant == 0u)          v = 0.0;
    else if (exp10 >  400)   v = HUGE_VAL;    /* overflows float and double alike */
    else if (exp10 < -400)   v = 0.0;
    else                     v = scale_pow10((double)mant, exp10);

    *out      = neg ? -v : v;
    *consumed = i;
    return true;
}

/* ---- "%g" formatter -------------------------------------------------------- */
static int fmt_zero(char *out, int cap, bool neg)
{
    const int n = neg ? 2 : 1;
    if (cap < n + 1) return -1;
    int i = 0;
    if (neg) out[i++] = '-';
    out[i++] = '0';
    out[i]   = '\0';
    return n;
}

/* Round-to-nearest into uint32 WITHOUT the undefined behaviour of casting an
 * out-of-range double. The exponent estimate below is only ever off by one, so
 * saturating here is unreachable in practice — it exists so that a future
 * change to the estimate cannot turn a formatting bug into UB. */
static uint32_t round_u32(double x)
{
    if (!(x > 0.0))    return 0u;
    if (x >= 4.0e9)    return 0xFFFFFFFFu;
    return (uint32_t)(x + 0.5);
}

int cfg_json_fmt_g(char *out, int cap, double v, int sig)
{
    if (out == NULL || cap <= 1) return -1;
    if (!isfinite(v)) return -1;                 /* callers emit `null` instead */
    if (sig < 1) sig = 1;
    if (sig > 9) sig = 9;                        /* 9 digits keep mant in uint32 */

    char tmp[40];
    int  n   = 0;
    bool neg = (v < 0.0);
    double a = neg ? -v : v;

    if (a == 0.0) {
        /* -0.0 keeps its sign ("-0" is legal JSON). Nothing in this config
         * MEANS negative zero, but a formatter with one silent exception to
         * bit-exact round-tripping is a formatter you cannot test as exact. */
        return fmt_zero(out, cap, signbit(v));
    }

    /* Estimate the decimal exponent (1 <= a/10^e10 < 10). Error here is
     * harmless — the mantissa below is recomputed from the ORIGINAL value, and
     * the two adjust steps fix an estimate that is off by one. */
    int e10 = 0;
    {
        double t = a;
        while (t >= 1e22)  { t /= 1e22; e10 += 22; }
        while (t >= 10.0)  { t /= 10.0; e10 += 1;  }
        while (t <  1e-21) { t *= 1e22; e10 -= 22; }
        while (t <  1.0)   { t *= 10.0; e10 -= 1;  }
    }

    const uint32_t pmax = pow10u(sig);           /* 10^sig     */
    const uint32_t pmin = pmax / 10u;            /* 10^(sig-1) */
    uint32_t m = round_u32(scale_pow10(a, sig - 1 - e10));
    if (m >= pmax) {                             /* rounding carried a digit */
        e10++;
        m = round_u32(scale_pow10(a, sig - 1 - e10));
    } else if (m < pmin) {                       /* estimate was one too high */
        e10--;
        m = round_u32(scale_pow10(a, sig - 1 - e10));
    }
    if (m >= pmax) { m = pmax / 10u; e10++; }    /* belt and braces */
    if (m == 0u) return fmt_zero(out, cap, neg); /* underflowed to nothing */

    char dg[10];
    for (int i = sig - 1; i >= 0; i--) { dg[i] = (char)('0' + (int)(m % 10u)); m /= 10u; }
    int nd = sig;
    while (nd > 1 && dg[nd - 1] == '0') nd--;    /* %g strips trailing zeros */

    if (neg) tmp[n++] = '-';

    if (e10 < -4 || e10 >= sig) {                /* %e form, same rule as %g */
        tmp[n++] = dg[0];
        if (nd > 1) {
            tmp[n++] = '.';
            for (int i = 1; i < nd; i++) tmp[n++] = dg[i];
        }
        tmp[n++] = 'e';
        int ex = e10;
        if (ex < 0) { tmp[n++] = '-'; ex = -ex; } else { tmp[n++] = '+'; }
        if (ex >= 100) { tmp[n++] = (char)('0' + ex / 100); ex %= 100; }
        tmp[n++] = (char)('0' + ex / 10);
        tmp[n++] = (char)('0' + ex % 10);
    } else if (e10 >= 0) {                       /* fixed, >= 1 */
        const int ip = e10 + 1;
        for (int i = 0; i < ip; i++) tmp[n++] = (i < nd) ? dg[i] : '0';
        if (nd > ip) {
            tmp[n++] = '.';
            for (int i = ip; i < nd; i++) tmp[n++] = dg[i];
        }
    } else {                                     /* fixed, 0.000ddd */
        tmp[n++] = '0';
        tmp[n++] = '.';
        for (int i = 0; i < -e10 - 1; i++) tmp[n++] = '0';
        for (int i = 0; i < nd; i++) tmp[n++] = dg[i];
    }

    if (n + 1 > cap) return -1;
    memcpy(out, tmp, (size_t)n);
    out[n] = '\0';
    return n;
}

/* ---- reader ---------------------------------------------------------------- */
static bool fail(cfg_json_t *j, cfg_json_err_t e)
{
    if (j->err == CFG_JSON_OK) j->err = e;
    return false;
}

static void skip_ws(cfg_json_t *j)
{
    while (j->pos < j->len && is_ws(j->s[j->pos])) j->pos++;
}

/* End-of-input inside a value is TRUNCATED, not SYNTAX — the distinction is
 * what lets a caller tell "the client sent junk" from "the line was cut". */
static bool want_more(cfg_json_t *j)
{
    if (j->pos >= j->len) return fail(j, CFG_JSON_ERR_TRUNCATED);
    return true;
}

static bool lit(cfg_json_t *j, const char *word, int n)
{
    if (j->len - j->pos < n) return fail(j, CFG_JSON_ERR_TRUNCATED);
    if (memcmp(j->s + j->pos, word, (size_t)n) != 0) return fail(j, CFG_JSON_ERR_SYNTAX);
    j->pos += n;
    return true;
}

void cfg_json_init(cfg_json_t *j, const char *text, int len)
{
    if (j == NULL) return;
    j->s       = text;
    j->len     = (text != NULL && len > 0) ? len : 0;
    j->pos     = 0;
    j->depth   = 0;
    j->seen    = 0u;
    j->unknown = 0;
    j->err     = CFG_JSON_OK;
}

cfg_json_type_t cfg_json_peek(cfg_json_t *j)
{
    if (j == NULL || j->err != CFG_JSON_OK) return CFG_JSON_T_NONE;
    skip_ws(j);
    if (j->pos >= j->len) return CFG_JSON_T_NONE;
    const char c = j->s[j->pos];
    if (c == '{') return CFG_JSON_T_OBJ;
    if (c == '[') return CFG_JSON_T_ARR;
    if (c == '"') return CFG_JSON_T_STR;
    if (c == '-' || is_digit(c)) return CFG_JSON_T_NUM;
    if (c == 't' || c == 'f') return CFG_JSON_T_BOOL;
    if (c == 'n') return CFG_JSON_T_NULL;
    return CFG_JSON_T_NONE;
}

/* Shared open for `{` and `[`: one depth budget, one place that can raise
 * CFG_JSON_ERR_DEPTH, so a depth bomb cannot slip in through the array path. */
static bool container_begin(cfg_json_t *j, char open)
{
    if (j == NULL || j->err != CFG_JSON_OK) return false;
    skip_ws(j);
    if (!want_more(j)) return false;
    if (j->s[j->pos] != open) return fail(j, CFG_JSON_ERR_SYNTAX);
    if (j->depth >= CFG_JSON_DEPTH_MAX) return fail(j, CFG_JSON_ERR_DEPTH);
    j->pos++;
    j->depth++;
    j->seen &= ~(1u << (j->depth - 1));
    return true;
}

bool cfg_json_obj_begin(cfg_json_t *j) { return container_begin(j, '{'); }
bool cfg_json_arr_begin(cfg_json_t *j) { return container_begin(j, '['); }

/* Advance past the separator inside a container. Returns 1 = an item follows,
 * 0 = the container closed, -1 = error. */
static int container_step(cfg_json_t *j, char close)
{
    if (j == NULL || j->err != CFG_JSON_OK) return -1;
    if (j->depth <= 0) { fail(j, CFG_JSON_ERR_SYNTAX); return -1; }

    skip_ws(j);
    if (!want_more(j)) return -1;

    const uint32_t bit = 1u << (j->depth - 1);
    if (j->s[j->pos] == close) { j->pos++; j->depth--; return 0; }

    if (j->seen & bit) {
        if (j->s[j->pos] != ',') { fail(j, CFG_JSON_ERR_SYNTAX); return -1; }
        j->pos++;
        skip_ws(j);
        if (!want_more(j)) return -1;
        /* No trailing commas: `[1,]` is a typo, and a config parser that
         * forgives typos is a config parser that guesses. */
        if (j->s[j->pos] == close) { fail(j, CFG_JSON_ERR_SYNTAX); return -1; }
    }
    j->seen |= bit;
    return 1;
}

bool cfg_json_obj_key(cfg_json_t *j, char *key, int cap, bool *truncated)
{
    const int step = container_step(j, '}');
    if (step != 1) return false;

    if (!cfg_json_str(j, key, cap, truncated)) return false;

    skip_ws(j);
    if (!want_more(j)) return false;
    if (j->s[j->pos] != ':') return fail(j, CFG_JSON_ERR_SYNTAX);
    j->pos++;
    return true;
}

bool cfg_json_arr_next(cfg_json_t *j)
{
    return container_step(j, ']') == 1;
}

bool cfg_json_num(cfg_json_t *j, double *out)
{
    if (j == NULL || out == NULL || j->err != CFG_JSON_OK) return false;
    skip_ws(j);
    if (!want_more(j)) return false;

    /* Bound the token BEFORE scanning it: a megabyte of digits is rejected here,
     * not accumulated first. */
    int probe = 0;
    const int avail = j->len - j->pos;
    while (probe < avail && probe <= CFG_JSON_NUM_CHARS_MAX &&
           is_num_char(j->s[j->pos + probe])) probe++;
    if (probe > CFG_JSON_NUM_CHARS_MAX) return fail(j, CFG_JSON_ERR_TOOLONG);

    int used = 0;
    double v = 0.0;
    if (!cfg_json_scan_num(j->s + j->pos, probe, &used, &v) || used <= 0)
        return fail(j, CFG_JSON_ERR_SYNTAX);
    if (used != probe) return fail(j, CFG_JSON_ERR_SYNTAX);  /* e.g. "1.2.3" */

    j->pos += used;
    *out = v;
    return true;
}

bool cfg_json_bool(cfg_json_t *j, bool *out)
{
    if (j == NULL || out == NULL || j->err != CFG_JSON_OK) return false;
    skip_ws(j);
    if (!want_more(j)) return false;
    if (j->s[j->pos] == 't') { if (!lit(j, "true", 4)) return false;  *out = true;  return true; }
    if (j->s[j->pos] == 'f') { if (!lit(j, "false", 5)) return false; *out = false; return true; }
    return fail(j, CFG_JSON_ERR_SYNTAX);
}

bool cfg_json_null(cfg_json_t *j)
{
    if (j == NULL || j->err != CFG_JSON_OK) return false;
    skip_ws(j);
    return lit(j, "null", 4);
}

/* Append one decoded byte, honouring the caller's capacity. `out == NULL` is
 * the skip case: the string is still fully consumed and validated. */
static void str_put(char *out, int cap, int *n, bool *trunc, char c)
{
    if (out == NULL || cap <= 0) return;
    if (*n < cap - 1) out[(*n)++] = c;
    else if (trunc != NULL) *trunc = true;
}

bool cfg_json_str(cfg_json_t *j, char *out, int cap, bool *truncated)
{
    if (j == NULL || j->err != CFG_JSON_OK) return false;
    if (truncated != NULL) *truncated = false;
    skip_ws(j);
    if (!want_more(j)) return false;
    if (j->s[j->pos] != '"') return fail(j, CFG_JSON_ERR_SYNTAX);
    j->pos++;

    int n = 0;
    for (;;) {
        if (j->pos >= j->len) return fail(j, CFG_JSON_ERR_TRUNCATED);
        const unsigned char c = (unsigned char)j->s[j->pos++];

        if (c == '"') break;

        if (c == '\\') {
            if (j->pos >= j->len) return fail(j, CFG_JSON_ERR_TRUNCATED);
            const char e = j->s[j->pos++];
            switch (e) {
            case '"':  str_put(out, cap, &n, truncated, '"');  break;
            case '\\': str_put(out, cap, &n, truncated, '\\'); break;
            case '/':  str_put(out, cap, &n, truncated, '/');  break;
            case 'b':  str_put(out, cap, &n, truncated, '\b'); break;
            case 'f':  str_put(out, cap, &n, truncated, '\f'); break;
            case 'n':  str_put(out, cap, &n, truncated, '\n'); break;
            case 'r':  str_put(out, cap, &n, truncated, '\r'); break;
            case 't':  str_put(out, cap, &n, truncated, '\t'); break;
            case 'u': {
                if (j->len - j->pos < 4) return fail(j, CFG_JSON_ERR_TRUNCATED);
                int cp = 0;
                for (int k = 0; k < 4; k++) {
                    const int h = hex_val(j->s[j->pos + k]);
                    if (h < 0) return fail(j, CFG_JSON_ERR_SYNTAX);
                    cp = (cp << 4) | h;
                }
                j->pos += 4;
                /* BMP -> UTF-8, surrogate pairing NOT checked (see the header:
                 * text validity is the client's job, not the regulator's). */
                if (cp < 0x80) {
                    str_put(out, cap, &n, truncated, (char)cp);
                } else if (cp < 0x800) {
                    str_put(out, cap, &n, truncated, (char)(0xC0 | (cp >> 6)));
                    str_put(out, cap, &n, truncated, (char)(0x80 | (cp & 0x3F)));
                } else {
                    str_put(out, cap, &n, truncated, (char)(0xE0 | (cp >> 12)));
                    str_put(out, cap, &n, truncated, (char)(0x80 | ((cp >> 6) & 0x3F)));
                    str_put(out, cap, &n, truncated, (char)(0x80 | (cp & 0x3F)));
                }
                break;
            }
            default: return fail(j, CFG_JSON_ERR_SYNTAX);
            }
            continue;
        }

        /* RFC 8259 forbids raw control characters; bytes >= 0x80 are copied
         * through unvalidated (deliberate — see the header's UTF-8 note). */
        if (c < 0x20u) return fail(j, CFG_JSON_ERR_SYNTAX);
        str_put(out, cap, &n, truncated, (char)c);
    }

    if (out != NULL && cap > 0) out[n] = '\0';
    return true;
}

bool cfg_json_skip(cfg_json_t *j)
{
    if (j == NULL || j->err != CFG_JSON_OK) return false;

    switch (cfg_json_peek(j)) {
    case CFG_JSON_T_OBJ: {
        if (!cfg_json_obj_begin(j)) return false;
        char  k[4];                       /* names are irrelevant when skipping */
        bool  tr = false;
        while (cfg_json_obj_key(j, k, (int)sizeof k, &tr)) {
            if (!cfg_json_skip(j)) return false;
        }
        return j->err == CFG_JSON_OK;
    }
    case CFG_JSON_T_ARR: {
        if (!cfg_json_arr_begin(j)) return false;
        while (cfg_json_arr_next(j)) {
            if (!cfg_json_skip(j)) return false;
        }
        return j->err == CFG_JSON_OK;
    }
    case CFG_JSON_T_STR:  return cfg_json_str(j, NULL, 0, NULL);
    case CFG_JSON_T_NUM:  { double d; return cfg_json_num(j, &d); }
    case CFG_JSON_T_BOOL: { bool b;   return cfg_json_bool(j, &b); }
    case CFG_JSON_T_NULL: return cfg_json_null(j);
    default:
        return fail(j, (j->pos >= j->len) ? CFG_JSON_ERR_TRUNCATED
                                          : CFG_JSON_ERR_SYNTAX);
    }
}

cfg_json_err_t cfg_json_validate(const char *text, int len)
{
    cfg_json_t j;
    cfg_json_init(&j, text, len);
    if (!cfg_json_skip(&j))
        return (j.err != CFG_JSON_OK) ? j.err : CFG_JSON_ERR_SYNTAX;
    skip_ws(&j);
    if (j.pos != j.len) return CFG_JSON_ERR_TRAILING;
    return CFG_JSON_OK;
}

/* ---- writer ---------------------------------------------------------------- */
static void wr(cfg_json_w_t *w, const char *s, int n)
{
    if (w == NULL || s == NULL) return;
    while (n > 0) {
        if (w->len >= w->cap) {
            if (w->sink != NULL) { w->sink(w->ctx, w->buf, w->len); w->len = 0; }
            else { w->ovf = true; return; }
        }
        int room = w->cap - w->len;
        const int k = (n < room) ? n : room;
        memcpy(w->buf + w->len, s, (size_t)k);
        w->len += k;
        s += k;
        n -= k;
    }
}

static void wr1(cfg_json_w_t *w, char c) { wr(w, &c, 1); }

/* Separator bookkeeping for the level we are about to write into. */
static void sep(cfg_json_w_t *w)
{
    if (w->pending) { w->pending = false; return; }   /* value of a written key */
    if (w->depth > 0 && w->depth <= 32) {
        const uint32_t bit = 1u << (w->depth - 1);
        if (w->comma & bit) wr1(w, ',');
        else                w->comma |= bit;
    }
}

/* Bare escaped string body, used for both keys and string values. */
static void wr_quoted(cfg_json_w_t *w, const char *s)
{
    wr1(w, '"');
    for (const unsigned char *p = (const unsigned char *)s; p != NULL && *p != 0u; p++) {
        const unsigned char c = *p;
        switch (c) {
        case '"':  wr(w, "\\\"", 2); break;
        case '\\': wr(w, "\\\\", 2); break;
        case '\b': wr(w, "\\b", 2);  break;
        case '\f': wr(w, "\\f", 2);  break;
        case '\n': wr(w, "\\n", 2);  break;
        case '\r': wr(w, "\\r", 2);  break;
        case '\t': wr(w, "\\t", 2);  break;
        default:
            if (c < 0x20u) {
                static const char HEX[] = "0123456789abcdef";
                char esc[6] = { '\\', 'u', '0', '0', 0, 0 };
                esc[4] = HEX[(c >> 4) & 0x0Fu];
                esc[5] = HEX[c & 0x0Fu];
                wr(w, esc, 6);
            } else {
                wr(w, (const char *)&c, 1);   /* >= 0x80 passes through */
            }
            break;
        }
    }
    wr1(w, '"');
}

void cfg_json_w_init(cfg_json_w_t *w, char *buf, int cap,
                     cfg_json_sink_fn sink, void *ctx)
{
    if (w == NULL) return;
    w->buf     = buf;
    w->cap     = (buf != NULL && cap > 0) ? cap : 0;
    w->len     = 0;
    w->sink    = sink;
    w->ctx     = ctx;
    w->comma   = 0u;
    w->isarr   = 0u;
    w->depth   = 0;
    w->pending = false;
    w->ovf     = (w->cap == 0);
}

bool cfg_json_w_flush(cfg_json_w_t *w)
{
    if (w == NULL) return false;
    if (w->sink != NULL && w->len > 0) { w->sink(w->ctx, w->buf, w->len); w->len = 0; }
    return !w->ovf;
}

static void container_open(cfg_json_w_t *w, char c, bool arr)
{
    if (w == NULL) return;
    sep(w);
    wr1(w, c);
    if (w->depth >= 32) { w->ovf = true; return; }
    w->depth++;
    const uint32_t bit = 1u << (w->depth - 1);
    w->comma &= ~bit;
    if (arr) w->isarr |= bit; else w->isarr &= ~bit;
}

void cfg_json_w_obj(cfg_json_w_t *w) { container_open(w, '{', false); }
void cfg_json_w_arr(cfg_json_w_t *w) { container_open(w, '[', true); }

void cfg_json_w_end(cfg_json_w_t *w)
{
    if (w == NULL || w->depth <= 0) return;
    const uint32_t bit = 1u << (w->depth - 1);
    wr1(w, (w->isarr & bit) ? ']' : '}');
    w->depth--;
    w->pending = false;
}

void cfg_json_w_key(cfg_json_w_t *w, const char *key)
{
    if (w == NULL || key == NULL) return;
    sep(w);
    wr_quoted(w, key);
    wr1(w, ':');
    w->pending = true;
}

void cfg_json_w_str(cfg_json_w_t *w, const char *s)
{
    if (w == NULL) return;
    sep(w);
    wr_quoted(w, (s != NULL) ? s : "");
}

void cfg_json_w_raw(cfg_json_w_t *w, const char *s, int len)
{
    if (w == NULL) return;
    sep(w);
    wr(w, s, len);
}

void cfg_json_w_bool(cfg_json_w_t *w, bool v)
{
    if (w == NULL) return;
    sep(w);
    if (v) wr(w, "true", 4); else wr(w, "false", 5);
}

void cfg_json_w_null(cfg_json_w_t *w)
{
    if (w == NULL) return;
    sep(w);
    wr(w, "null", 4);
}

void cfg_json_w_int(cfg_json_w_t *w, long v)
{
    if (w == NULL) return;
    char b[24];
    int  n = 0;
    unsigned long u = (v < 0) ? (unsigned long)(-(v + 1)) + 1ul : (unsigned long)v;
    char d[20];
    int  k = 0;
    do { d[k++] = (char)('0' + (int)(u % 10ul)); u /= 10ul; } while (u != 0ul && k < 20);
    if (v < 0) b[n++] = '-';
    while (k > 0) b[n++] = d[--k];
    sep(w);
    wr(w, b, n);
}

void cfg_json_w_f32(cfg_json_w_t *w, float v)
{
    if (w == NULL) return;
    if (!isfinite(v)) { cfg_json_w_null(w); return; }

    char b[40];
    int  n = cfg_json_fmt_g(b, (int)sizeof b, (double)v, CFG_JSON_SIG_DEFAULT);
    if (n > 0) {
        /* Fidelity guard: four significant digits is the readable default, but
         * a value that does not read back as the SAME float would make
         * cfg-get -> cfg-set a silent editor of the user's config. Widen only
         * for those. */
        double back = 0.0;
        int    used = 0;
        if (!cfg_json_scan_num(b, n, &used, &back) || used != n || (float)back != v)
            n = cfg_json_fmt_g(b, (int)sizeof b, (double)v, CFG_JSON_SIG_EXACT);
    }
    if (n <= 0) { cfg_json_w_null(w); return; }

    sep(w);
    wr(w, b, n);
}
