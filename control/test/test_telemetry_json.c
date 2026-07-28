/*
 * test_telemetry_json.c — the {"t":"telem"} line emitter (GH#35, #20).
 *
 * What earns its keep here:
 *   - the line is VALID JSON by the wire's own validator (cfg_json_validate),
 *     so a client reusing its config-reply parser can never choke on telemetry;
 *   - unsigned 32-bit fields survive above LONG_MAX (a corrupt-frame crash PC
 *     of 0xFFFFFFFF must not print as -1);
 *   - non-finite measurements travel as null, same rule as the config wire;
 *   - the output is chunk-size invariant, because the firmware streams it
 *     through a 128-byte buffer and the tests must prove that shape lossless.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "telemetry_json.h"

#include <string.h>

static char g_line[2048];
static int  g_len;

static void line_sink(void *ctx, const char *d, int n)
{
    (void)ctx;
    if (g_len + n < (int)sizeof g_line) {
        memcpy(g_line + g_len, d, (size_t)n);
        g_len += n;
        g_line[g_len] = '\0';
    }
}

static bool has(const char *needle)
{
    return strstr(g_line, needle) != NULL;
}

static ctrl_telemetry_t sample_telemetry(void)
{
    ctrl_telemetry_t t;
    memset(&t, 0, sizeof t);
    t.state             = CTRL_BULK;
    t.standby_reason    = CTRL_SB_OFF;
    t.active_profile_id = 2;
    t.field_effort      = 0.42f;
    t.binding           = CTRL_BIND_THERMAL;
    t.binding_w         = 1200.0f;
    t.faults            = 0xFFFFFFFFu;      /* u32 boundary through "faults" */
    t.severity          = CTRL_SEV_WARN;
    t.cells_series      = 16;
    t.vbat_pack_v       = 52.1f;
    t.v_cell            = 3.256f;
    t.amps_batt         = -3.5f;
    t.watts_batt        = 531.0f;
    t.alt_temp_c        = 71.5f;
    t.batt_temp_c       = NAN;              /* sensor not fitted → null */
    t.rpm               = NAN;              /* signal lost → null */
    t.rpm_state         = CTRL_RPM_LOST;
    return t;
}

static telem_diag_t sample_diag(void)
{
    telem_diag_t d;
    memset(&d, 0, sizeof d);
    d.uptime_ms          = 4000000000u;     /* > LONG_MAX: forces w_u32 */
    d.reset_counts[TELEM_RESET_POR]      = 3;
    d.reset_counts[TELEM_RESET_PIN]      = 1;
    d.reset_counts[TELEM_RESET_SOFTWARE] = 2;
    d.reset_cause_bits   = 1u << TELEM_RESET_SOFTWARE;
    d.boot_count         = 6;
    d.crash_count        = 2;
    d.last_crash_kind    = 1;               /* HardFault, crash_record.h */
    d.last_crash_pc      = 0xFFFFFFFFu;     /* corrupt frame — must not go -1 */
    d.crc_status         = TELEM_CRC_UNPATCHED;
    d.stack_used_max     = 812;
    d.stack_free_min     = 1236;
    d.errb_fault_mask    = 1u << TELEM_ERRB_CAN;
    d.errb_reinits[TELEM_ERRB_INA226] = 1;
    d.errb_reinits[TELEM_ERRB_CAN]    = 2;
    d.safe_state_entries = 1;
    return d;
}

static bool emit(int chunk_cap, const ctrl_telemetry_t *t, const telem_diag_t *d)
{
    static char chunk[512];
    g_len = 0;
    g_line[0] = '\0';
    return telem_json_line(chunk, chunk_cap, line_sink, NULL, t, d);
}

void test_telemetry_json(void)
{
    const int at_entry = g_checks;
    printf("-- telemetry JSON line (GH#35 deliverable #20)\n");

    const ctrl_telemetry_t t = sample_telemetry();
    const telem_diag_t     d = sample_diag();

    CHECK(emit(128, &t, &d));               /* the firmware's chunk size */
    printf("  [TELEM] line length %d B\n", g_len - 1);

    /* Exactly one LF-terminated line of wire-valid JSON. */
    CHECK(g_len > 0 && g_line[g_len - 1] == '\n');
    CHECK(strchr(g_line, '\n') == g_line + g_len - 1);
    CHECK(cfg_json_validate(g_line, g_len - 1) == CFG_JSON_OK);

    CHECK(has("{\"t\":\"telem\""));
    CHECK(has("\"up_ms\":4000000000"));     /* u32 above LONG_MAX */
    CHECK(has("\"state\":1"));
    CHECK(has("\"standby\":0"));
    CHECK(has("\"profile\":2"));
    CHECK(has("\"field\":0.42"));
    /* CTRL_BIND_THERMAL's ordinal shifted 3->4 when deliverable #10 inserted
     * CTRL_BIND_BMS_CVL ahead of it in ctrl_bind_src_t (control.h) -- the wire
     * is a raw enum ordinal by design (client owns pretty-printing,
     * telemetry_json.h), so this is exactly the kind of renumbering that is
     * expected pre-v1 and just needs the golden value updated to match. */
    CHECK(has("\"bind\":4"));
    CHECK(has("\"bind_w\":1200"));
    CHECK(has("\"faults\":4294967295"));
    CHECK(has("\"sev\":1"));
    CHECK(has("\"cells\":16"));
    CHECK(has("\"vbat\":52.1"));
    CHECK(has("\"vcell\":3.256"));
    CHECK(has("\"amps\":-3.5"));
    CHECK(has("\"watts\":531"));
    CHECK(has("\"alt_c\":71.5"));
    CHECK(has("\"batt_c\":null"));          /* NAN → null, config-wire rule */
    CHECK(has("\"rpm\":null"));
    CHECK(has("\"rpm_st\":2"));

    CHECK(has("\"resets\":[3,1,2,0,0,0,0]"));
    CHECK(has("\"cause\":4"));
    CHECK(has("\"boots\":6"));
    CHECK(has("\"crashes\":2"));
    CHECK(has("\"crash_kind\":1"));
    CHECK(has("\"crash_pc\":4294967295")); /* NOT -1 */
    CHECK(has("\"crc\":\"unpatched\""));
    CHECK(has("\"stack_used\":812"));
    CHECK(has("\"stack_free\":1236"));
    CHECK(has("\"errb\":4"));
    CHECK(has("\"errb_reinit\":[1,0,2]"));
    CHECK(has("\"safe\":1"));

    /* All four CRC verdict strings, and nothing outside the closed set. */
    {
        telem_diag_t dc = d;
        static const struct { uint8_t s; const char *want; } CRC[] = {
            { TELEM_CRC_OK,          "\"crc\":\"ok\""        },
            { TELEM_CRC_MISMATCH,    "\"crc\":\"mismatch\""  },
            { TELEM_CRC_UNPATCHED,   "\"crc\":\"unpatched\"" },
            { TELEM_CRC_UNAVAILABLE, "\"crc\":\"unavail\""   },
            { 99,                    "\"crc\":\"unavail\""   },  /* out of range */
        };
        for (unsigned i = 0; i < sizeof CRC / sizeof CRC[0]; i++) {
            dc.crc_status = CRC[i].s;
            CHECK(emit(128, &t, &dc));
            CHECK(has(CRC[i].want));
        }
    }

    /* Chunk-size invariance: the streamed shape loses nothing. */
    {
        static char big[2048];
        CHECK(emit(24, &t, &d));            /* pathologically small chunk */
        memcpy(big, g_line, (size_t)g_len + 1u);
        const int small_len = g_len;
        CHECK(emit(400, &t, &d));
        CHECK(small_len == g_len);
        CHECK(strcmp(big, g_line) == 0);
    }

    /* An all-zero snapshot (telem-get before the first control tick) is still
     * one valid line — honest zeros, never garbage. */
    {
        ctrl_telemetry_t tz;
        telem_diag_t     dz;
        memset(&tz, 0, sizeof tz);
        memset(&dz, 0, sizeof dz);
        CHECK(emit(128, &tz, &dz));
        CHECK(cfg_json_validate(g_line, g_len - 1) == CFG_JSON_OK);
        CHECK(has("\"crashes\":0"));
    }

    /* NULL inputs refuse rather than emit a broken line. */
    CHECK(!telem_json_line(NULL, 0, line_sink, NULL, &t, &d));

    printf("  [TELEM] %d checks\n", g_checks - at_entry);
}
