/*
 * test_config.c — packed config record: codec, two-slot store, validator.
 *
 * Scope split with test_property.c: the PROFILE_SPEC §1 ordering rules and the
 * §3 numeric ranges are swept there (P6 owns the parameter table and drives the
 * validator with it). Here: the codec contract, the store's power-fail
 * behaviour, and every validator rule that is NOT a §1/§3 range — refs, ids,
 * rest-mode, the limit set, the rotor block, active_profile.
 *
 * SPDX-License-Identifier: MIT
 */
#include "test_config.h"

/* =========================================================================== *
 * mock EEPROM — 2 KB, 16-byte pages, with injectable failures
 * =========================================================================== */
#define MOCK_BYTES      2048u
#define MOCK_PAGE       16u

static uint8_t s_ee[MOCK_BYTES];
static bool    s_read_fail;          /* every read fails (dead part) */
static int     s_write_fail_at;      /* -1 = never; else fail once >= N bytes done */
static bool    s_write_corrupts;     /* write "succeeds" but a byte is wrong */

static void mock_reset(uint8_t fill)
{
    memset(s_ee, fill, sizeof s_ee);
    s_read_fail = false;
    s_write_fail_at = -1;
    s_write_corrupts = false;
}

static bool mock_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (s_read_fail) return false;
    if ((uint32_t)addr + (uint32_t)len > MOCK_BYTES) return false;
    for (uint16_t i = 0; i < len; i++) buf[i] = s_ee[addr + i];
    return true;
}

/* Page-granular like the real 24C16 (eeprom24c16.c chunks at 16-byte pages), so
 * an injected failure tears the record the way a power cut would: the pages
 * already programmed stay, the rest keeps whatever was there before. */
static bool mock_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if ((uint32_t)addr + (uint32_t)len > MOCK_BYTES) return false;
    uint16_t done = 0;
    while (done < len) {
        uint16_t page = (uint16_t)(MOCK_PAGE - ((addr + done) % MOCK_PAGE));
        if (page > (uint16_t)(len - done)) page = (uint16_t)(len - done);
        if (s_write_fail_at >= 0 && (int)done >= s_write_fail_at) return false;
        for (uint16_t i = 0; i < page; i++) s_ee[addr + done + i] = buf[done + i];
        done = (uint16_t)(done + page);
    }
    if (s_write_corrupts) s_ee[addr + 20u] ^= 0x01u;   /* silent bad cell */
    return true;
}

/* =========================================================================== *
 * helpers
 * =========================================================================== */
static bool same_f(float a, float b)          /* NaN == NaN here: NAN is a live value */
{
    if (isnan(a) && isnan(b)) return true;
    return a == b;
}

static bool profile_equal(const cfg_profile_t *a, const cfg_profile_t *b)
{
    return a->flags == b->flags &&
           a->cv_target_ref == b->cv_target_ref &&
           a->rest_voltage_ref == b->rest_voltage_ref &&
           a->p.id == b->p.id &&
           same_f(a->p.cv_target_vcell, b->p.cv_target_vcell) &&
           a->p.exit_at_cv_entry == b->p.exit_at_cv_entry &&
           a->p.rest_mode == b->p.rest_mode &&
           same_f(a->p.rest_voltage_vcell, b->p.rest_voltage_vcell) &&
           same_f(a->p.rest_power_cap_w, b->p.rest_power_cap_w) &&
           same_f(a->p.v_revert_vcell, b->p.v_revert_vcell) &&
           a->p.t_revert_hold_s == b->p.t_revert_hold_s &&
           a->p.soc_revert_pct == b->p.soc_revert_pct &&
           same_f(a->p.ah_revert, b->p.ah_revert);
}

static bool config_equal(const ctrl_config_t *a, const ctrl_config_t *b)
{
    if (!same_f(a->prim.v_bulk, b->prim.v_bulk) ||
        !same_f(a->prim.v_bulk_cons, b->prim.v_bulk_cons) ||
        !same_f(a->prim.v_float, b->prim.v_float) ||
        !same_f(a->prim.v_float_cons, b->prim.v_float_cons) ||
        !same_f(a->prim.v_limp, b->prim.v_limp)) return false;

    const ctrl_globals_t *x = &a->globals, *y = &b->globals;
    if (x->cells_series != y->cells_series ||
        !same_f(x->bank_capacity_ah, y->bank_capacity_ah) ||
        !same_f(x->max_charge_power_w, y->max_charge_power_w) ||
        !same_f(x->ramp_w_per_s, y->ramp_w_per_s) ||
        !same_f(x->p_tail_w, y->p_tail_w) ||
        x->t_tail_hold_s != y->t_tail_hold_s ||
        x->t_vclamp_s != y->t_vclamp_s ||
        x->cv_hold_exit_min != y->cv_hold_exit_min ||
        x->t_charge_max_min != y->t_charge_max_min ||
        x->warmup_time_s != y->warmup_time_s ||
        !same_f(x->warmup_coolant_c, y->warmup_coolant_c) ||
        x->soc_target_pct != y->soc_target_pct ||
        !same_f(x->skip_bulk_vcell, y->skip_bulk_vcell) ||
        x->skip_bulk_soc_pct != y->skip_bulk_soc_pct ||
        !same_f(x->rotor_rated_v, y->rotor_rated_v) ||
        !same_f(x->rotor_v_max, y->rotor_v_max) ||
        x->allow_full_field_48v != y->allow_full_field_48v ||
        !same_f(x->limp_vcell, y->limp_vcell) ||
        !same_f(x->limp_power_cap_w, y->limp_power_cap_w) ||
        x->batt_temp_src != y->batt_temp_src ||
        x->require_batt_temp != y->require_batt_temp) return false;

    if (!same_f(a->limits.battery_c_limit, b->limits.battery_c_limit) ||
        !same_f(a->limits.wiring_limit_a, b->limits.wiring_limit_a) ||
        !same_f(a->limits.alternator_limit_a, b->limits.alternator_limit_a)) return false;

    for (unsigned i = 0; i < CFG_PROFILE_COUNT; i++)
        if (!profile_equal(&a->profiles[i], &b->profiles[i])) return false;

    return a->active_profile == b->active_profile;
}

/* One mutation, one expected code. Keeps each case to a single readable line. */
#define EXPECT_ERR(mutation, code) do {                                        \
    ctrl_config_t c = cfg_test_baseline();                                     \
    mutation;                                                                  \
    const cfg_err_t e_ = ctrl_config_validate(&c, NULL);                       \
    g_checks++;                                                                \
    if (e_ != (code)) { g_fails++;                                             \
        printf("FAIL %s:%d  expected %s (%d, %s), got %d (%s)\n",              \
               __FILE__, __LINE__, #code, (int)(code), cfg_err_str(code),      \
               (int)e_, cfg_err_str(e_)); }                                    \
} while (0)

/* =========================================================================== *
 * C1 — CRC-32 known answers
 * =========================================================================== */
static void c1_crc(void)
{
    const uint8_t check[] = { '1','2','3','4','5','6','7','8','9' };
    CHECK(cfg_crc32(check, sizeof check) == 0xCBF43926u);   /* the CRC-32 check value */
    CHECK(cfg_crc32(NULL, 0) == 0u);
    CHECK(cfg_crc32(check, 0) == 0u);                       /* empty message */

    /* A single flipped bit must change it (the property the store leans on). */
    uint8_t a[16], b[16];
    for (int i = 0; i < 16; i++) { a[i] = (uint8_t)i; b[i] = (uint8_t)i; }
    b[7] ^= 0x08u;
    CHECK(cfg_crc32(a, 16) != cfg_crc32(b, 16));
}

/* =========================================================================== *
 * C2 — record framing: golden header bytes + round trip
 * =========================================================================== */
static void c2_codec(void)
{
    const ctrl_config_t src = cfg_test_baseline();
    uint8_t buf[CFG_RECORD_V1_BYTES];

    const size_t n = cfg_encode(&src, 0x11223344u, buf, sizeof buf);
    CHECK(n == CFG_RECORD_V1_BYTES);
    CHECK(CFG_RECORD_V1_BYTES == 312u);          /* 16 header + 292 payload + 4 CRC */

    /* Golden header — byte for byte, little-endian (config_codec.h layout). */
    CHECK(buf[0] == 'W' && buf[1] == '5' && buf[2] == 'C' && buf[3] == 'F');
    CHECK(buf[4] == 0x02u && buf[5] == 0x00u);   /* schema_version = 2 (GH#40) */
    CHECK(buf[6] == 0x00u && buf[7] == 0x00u);   /* flags reserved */
    CHECK(buf[8] == 0x44u && buf[9] == 0x33u &&
          buf[10] == 0x22u && buf[11] == 0x11u); /* generation 0x11223344 LE */
    CHECK(buf[12] == 0x24u && buf[13] == 0x01u); /* payload_len 292 = 0x0124 LE */
    CHECK(buf[14] == 0x00u && buf[15] == 0x00u); /* reserved */

    /* CRC trails the record and covers everything before it, with no gaps. */
    const uint32_t crc = (uint32_t)buf[308] | ((uint32_t)buf[309] << 8) |
                         ((uint32_t)buf[310] << 16) | ((uint32_t)buf[311] << 24);
    CHECK(crc == cfg_crc32(buf, CFG_HEADER_BYTES + CFG_PAYLOAD_V1_BYTES));

    /* Determinism: same input, same bytes. */
    uint8_t buf2[CFG_RECORD_V1_BYTES];
    CHECK(cfg_encode(&src, 0x11223344u, buf2, sizeof buf2) == CFG_RECORD_V1_BYTES);
    CHECK(memcmp(buf, buf2, sizeof buf) == 0);

    /* Round trip: every field survives, NANs included. */
    ctrl_config_t back;
    memset(&back, 0xA5, sizeof back);
    uint32_t gen = 0;
    CHECK(cfg_decode(buf, sizeof buf, &back, &gen) == CFG_DEC_OK);
    CHECK(gen == 0x11223344u);
    CHECK(config_equal(&src, &back));
    CHECK(isnan(back.globals.warmup_coolant_c));  /* NAN = "unset" must not normalize */
    CHECK(isnan(back.globals.rotor_v_max));
    CHECK(back.profiles[6].flags == CFG_PROF_FLAG_RESERVED);
    CHECK(back.profiles[3].cv_target_ref == CFG_PRIM_V_BULK_CONS);

    /* A decoded record is still a valid one. */
    CHECK(ctrl_config_validate(&back, NULL) == CFG_OK);

    /* Extreme-but-representable values round-trip bit-exactly. */
    {
        ctrl_config_t odd = src;
        odd.globals.bank_capacity_ah = 1.0e-30f;
        odd.globals.ramp_w_per_s     = -INFINITY;
        odd.globals.p_tail_w         = 3.4028235e38f;
        odd.profiles[0].p.ah_revert  = NAN;
        odd.profiles[0].p.soc_revert_pct = -128;
        odd.profiles[0].p.t_revert_hold_s = 65535u;
        uint8_t ob[CFG_RECORD_V1_BYTES];
        ctrl_config_t ov;
        CHECK(cfg_encode(&odd, 1u, ob, sizeof ob) == CFG_RECORD_V1_BYTES);
        CHECK(cfg_decode(ob, sizeof ob, &ov, NULL) == CFG_DEC_OK);
        CHECK(config_equal(&odd, &ov));
    }

    /* Too small a buffer is refused, not truncated. */
    CHECK(cfg_encode(&src, 1u, buf, CFG_RECORD_V1_BYTES - 1u) == 0u);
    CHECK(cfg_encode(NULL, 1u, buf, sizeof buf) == 0u);
}

/* =========================================================================== *
 * C3 — rejection: single-bit corruption anywhere, magic, version, length
 * =========================================================================== */
static void c3_rejection(void)
{
    const ctrl_config_t src = cfg_test_baseline();
    uint8_t buf[CFG_RECORD_V1_BYTES];
    CHECK(cfg_encode(&src, 7u, buf, sizeof buf) == CFG_RECORD_V1_BYTES);

    /* Every single-bit flip in the whole record must be rejected — by the CRC
     * for payload/generation bits, by the frame checks for magic/version/len. */
    ctrl_config_t sink;
    int accepted = 0, first_byte = -1, first_bit = -1;
    for (size_t i = 0; i < CFG_RECORD_V1_BYTES; i++) {
        for (int bit = 0; bit < 8; bit++) {
            buf[i] ^= (uint8_t)(1u << bit);
            if (cfg_decode(buf, sizeof buf, &sink, NULL) == CFG_DEC_OK) {
                if (accepted == 0) { first_byte = (int)i; first_bit = bit; }
                accepted++;
            }
            buf[i] ^= (uint8_t)(1u << bit);
        }
    }
    if (accepted)
        printf("  [C3] %d single-bit corruptions accepted, first at byte %d bit %d\n",
               accepted, first_byte, first_bit);
    CHECK(accepted == 0);
    printf("  [C3] %u single-bit flips, all rejected\n",
           (unsigned)(CFG_RECORD_V1_BYTES * 8u));

    /* Named frame rejections. */
    uint8_t t[CFG_RECORD_V1_BYTES];
    memcpy(t, buf, sizeof t);
    t[2] = 'X';
    CHECK(cfg_decode(t, sizeof t, &sink, NULL) == CFG_DEC_ERR_MAGIC);

    memcpy(t, buf, sizeof t);
    t[4] = 3u;                                   /* schema_version 3 (3 != CFG_SCHEMA_VERSION==2) */
    CHECK(cfg_decode(t, sizeof t, &sink, NULL) == CFG_DEC_ERR_VERSION);
    memcpy(t, buf, sizeof t);
    t[4] = 1u;                                   /* schema_version 1 — the OLD version is
                                                   * just as wrong as any other non-native one */
    CHECK(cfg_decode(t, sizeof t, &sink, NULL) == CFG_DEC_ERR_VERSION);

    memcpy(t, buf, sizeof t);
    t[12] = 0x20u;                               /* payload_len 288 (real is 292) */
    CHECK(cfg_decode(t, sizeof t, &sink, NULL) == CFG_DEC_ERR_LEN);

    memcpy(t, buf, sizeof t);
    t[308] ^= 0xFFu;                             /* CRC byte (payload grew to 292, GH#40) */
    CHECK(cfg_decode(t, sizeof t, &sink, NULL) == CFG_DEC_ERR_CRC);

    CHECK(cfg_decode(buf, CFG_HEADER_BYTES - 1u, &sink, NULL) == CFG_DEC_ERR_SHORT);
    CHECK(cfg_decode(buf, CFG_RECORD_V1_BYTES - 1u, &sink, NULL) == CFG_DEC_ERR_SHORT);
    CHECK(cfg_decode(NULL, sizeof buf, &sink, NULL) == CFG_DEC_ERR_ARG);
    CHECK(cfg_decode(buf, sizeof buf, NULL, NULL) == CFG_DEC_ERR_ARG);

    /* A rejected record must not half-populate the caller's config. */
    {
        ctrl_config_t keep, before;
        memset(&keep, 0x5A, sizeof keep);
        memcpy(&before, &keep, sizeof before);   /* memcpy: padding must match too */
        memcpy(t, buf, sizeof t);
        t[200] ^= 0x40u;
        CHECK(cfg_decode(t, sizeof t, &keep, NULL) == CFG_DEC_ERR_CRC);
        CHECK(memcmp(&keep, &before, sizeof keep) == 0);
    }
}

/* =========================================================================== *
 * C4 — two-slot store: alternation, torn writes, verify, wraparound
 * =========================================================================== */
static void c4_store(void)
{
    ctrl_config_t cfg = cfg_test_baseline();
    ctrl_config_t got;
    cfg_store_t st;

    /* --- blank part --- */
    mock_reset(0xFF);
    cfg_store_init(&st, mock_read, mock_write);
    CHECK(cfg_store_load(&st, &got) == CFG_STORE_EMPTY);

    /* An unreadable part is NOT the same as a blank one. */
    s_read_fail = true;
    CHECK(cfg_store_load(&st, &got) == CFG_STORE_IO);
    s_read_fail = false;

    /* --- first save lands in slot A at generation 1 --- */
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);
    CHECK(st.live_slot == 0u && st.generation == 1u);
    {
        uint32_t gen = 0;
        CHECK(cfg_peek(&s_ee[CFG_SLOT_A_ADDR], CFG_RECORD_V1_BYTES, &gen) == CFG_DEC_OK);
        CHECK(gen == 1u);
        CHECK(cfg_peek(&s_ee[CFG_SLOT_B_ADDR], CFG_RECORD_V1_BYTES, NULL) != CFG_DEC_OK);
    }

    /* --- saves alternate; load always returns the newest --- */
    cfg.globals.ramp_w_per_s = 300.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);
    CHECK(st.live_slot == 1u && st.generation == 2u);      /* slot B */
    cfg.globals.ramp_w_per_s = 400.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);
    CHECK(st.live_slot == 0u && st.generation == 3u);      /* back to A */

    {
        cfg_store_t fresh;
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &got) == CFG_STORE_OK);
        CHECK(fresh.generation == 3u && fresh.live_slot == 0u);
        CHECK_FEQ(got.globals.ramp_w_per_s, 400.0f, 1e-6);
        CHECK(config_equal(&cfg, &got));
    }

    /* --- torn write: fail mid-record, the older generation still loads ------
     * live = A(gen 3), so this tears B(gen 2 -> 4). Four pages of the new
     * record land, the rest is the old one: magic and version still parse, the
     * CRC does not. */
    s_write_fail_at = 64;
    cfg.globals.ramp_w_per_s = 500.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_IO);
    CHECK(st.live_slot == 0u && st.generation == 3u);      /* live is untouched */
    s_write_fail_at = -1;
    {
        uint32_t gen = 0;
        CHECK(cfg_peek(&s_ee[CFG_SLOT_B_ADDR], CFG_RECORD_V1_BYTES, &gen) == CFG_DEC_ERR_CRC);
        cfg_store_t fresh;
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &got) == CFG_STORE_OK);
        CHECK(fresh.generation == 3u && fresh.live_slot == 0u);
        CHECK_FEQ(got.globals.ramp_w_per_s, 400.0f, 1e-6);  /* pre-tear config */
    }

    /* --- read-back verify catches a silently bad cell --- */
    s_write_corrupts = true;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_VERIFY);
    CHECK(st.live_slot == 0u && st.generation == 3u);
    s_write_corrupts = false;
    {
        cfg_store_t fresh;
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &got) == CFG_STORE_OK);
        CHECK_FEQ(got.globals.ramp_w_per_s, 400.0f, 1e-6);
    }

    /* --- a corrupt NEWER slot falls back to the older one ---------------- */
    mock_reset(0xFF);
    cfg_store_init(&st, mock_read, mock_write);
    cfg.globals.ramp_w_per_s = 111.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);       /* A, gen 1 */
    cfg.globals.ramp_w_per_s = 222.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);       /* B, gen 2 */
    s_ee[CFG_SLOT_B_ADDR + 100u] ^= 0x02u;                  /* rot the newer slot */
    {
        cfg_store_t fresh;
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &got) == CFG_STORE_OK);
        CHECK(fresh.generation == 1u && fresh.live_slot == 0u);
        CHECK_FEQ(got.globals.ramp_w_per_s, 111.0f, 1e-6);
    }
    /* Both slots rotten = empty, and the caller keeps its defaults. */
    s_ee[CFG_SLOT_A_ADDR + 100u] ^= 0x02u;
    {
        cfg_store_t fresh;
        ctrl_config_t untouched, before;
        memset(&untouched, 0x33, sizeof untouched);
        memcpy(&before, &untouched, sizeof before);
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &untouched) == CFG_STORE_EMPTY);
        CHECK(memcmp(&untouched, &before, sizeof before) == 0);
    }

    /* --- generation wraparound: 0 is NEWER than 0xFFFFFFFF ---------------- */
    CHECK(cfg_generation_newer(0u, 0xFFFFFFFFu));
    CHECK(!cfg_generation_newer(0xFFFFFFFFu, 0u));
    CHECK(cfg_generation_newer(2u, 1u) && !cfg_generation_newer(1u, 2u));

    mock_reset(0xFF);
    cfg.globals.ramp_w_per_s = 900.0f;
    {
        uint8_t rec[CFG_RECORD_V1_BYTES];
        CHECK(cfg_encode(&cfg, 0xFFFFFFFFu, rec, sizeof rec) == CFG_RECORD_V1_BYTES);
        memcpy(&s_ee[CFG_SLOT_A_ADDR], rec, sizeof rec);
    }
    cfg_store_init(&st, mock_read, mock_write);
    CHECK(cfg_store_load(&st, &got) == CFG_STORE_OK);
    CHECK(st.generation == 0xFFFFFFFFu && st.live_slot == 0u);

    /* The next save wraps to generation 0 in slot B and must still win. */
    cfg.globals.ramp_w_per_s = 950.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);
    CHECK(st.generation == 0u && st.live_slot == 1u);
    {
        cfg_store_t fresh;
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &got) == CFG_STORE_OK);
        CHECK(fresh.live_slot == 1u && fresh.generation == 0u);
        CHECK_FEQ(got.globals.ramp_w_per_s, 950.0f, 1e-6);
    }
    /* And the one after that keeps going forward (1 > 0), not backwards. */
    cfg.globals.ramp_w_per_s = 960.0f;
    CHECK(cfg_store_save(&st, &cfg) == CFG_STORE_OK);
    CHECK(st.generation == 1u && st.live_slot == 0u);
    {
        cfg_store_t fresh;
        cfg_store_init(&fresh, mock_read, mock_write);
        CHECK(cfg_store_load(&fresh, &got) == CFG_STORE_OK);
        CHECK_FEQ(got.globals.ramp_w_per_s, 960.0f, 1e-6);
    }

    /* --- argument guards --- */
    CHECK(cfg_store_load(NULL, &got) == CFG_STORE_ARG);
    CHECK(cfg_store_load(&st, NULL) == CFG_STORE_ARG);
    CHECK(cfg_store_save(&st, NULL) == CFG_STORE_ARG);
    CHECK(cfg_store_slot_addr(0) == CFG_SLOT_A_ADDR);
    CHECK(cfg_store_slot_addr(1) == CFG_SLOT_B_ADDR);
}

/* =========================================================================== *
 * C5 — validator rules outside the §1/§3 numeric table (that lives in P6)
 * =========================================================================== */
static void c5_validator(void)
{
    /* The spec-faithful baseline is accepted with nothing touched. */
    {
        ctrl_config_t c = cfg_test_baseline();
        ctrl_config_t before;
        memcpy(&before, &c, sizeof before);
        int bad = 99;
        CHECK(ctrl_config_validate(&c, &bad) == CFG_OK);
        CHECK(bad == -1);
        CHECK(memcmp(&c, &before, sizeof c) == 0);      /* never repairs (§1) */
    }
    CHECK(ctrl_config_validate(NULL, NULL) == CFG_ERR_ARG);

    /* limp_vcell is v_limp — two homes for one value must agree. */
    EXPECT_ERR(c.globals.limp_vcell = 3.31f, CFG_ERR_LIMP_VCELL_MISMATCH);

    /* §5.1 rotor block: rotor_rated_v is the duty_max divisor. */
    EXPECT_ERR(c.globals.rotor_rated_v = 0.0f,      CFG_ERR_SANITY_ROTOR_RATED);
    EXPECT_ERR(c.globals.rotor_rated_v = -12.0f,    CFG_ERR_SANITY_ROTOR_RATED);
    EXPECT_ERR(c.globals.rotor_rated_v = NAN,       CFG_ERR_SANITY_ROTOR_RATED);
    EXPECT_ERR(c.globals.rotor_rated_v = INFINITY,  CFG_ERR_SANITY_ROTOR_RATED);
    EXPECT_ERR(c.globals.rotor_v_max = -1.0f,       CFG_ERR_SANITY_ROTOR_VMAX);
    EXPECT_ERR(c.globals.rotor_v_max = INFINITY,    CFG_ERR_SANITY_ROTOR_VMAX);
    EXPECT_ERR(c.globals.rotor_v_max = 15.0f,       CFG_OK);      /* override is legal */
    EXPECT_ERR(c.globals.limp_power_cap_w = NAN,    CFG_ERR_SANITY_LIMP_POWER_CAP);
    EXPECT_ERR(c.globals.limp_power_cap_w = -1.0f,  CFG_ERR_SANITY_LIMP_POWER_CAP);

    /* GH#40 battery-temp charge-window gate: batt_temp_src is a closed enum,
     * NONE/ADC_A/ADC_B all legal, anything else (including a decode-verbatim
     * corrupt value) is a named rejection, never coerced to NONE. */
    EXPECT_ERR(c.globals.batt_temp_src = CTRL_BATT_TEMP_NONE,        CFG_OK);
    EXPECT_ERR(c.globals.batt_temp_src = CTRL_BATT_TEMP_ADC_A,       CFG_OK);
    EXPECT_ERR(c.globals.batt_temp_src = CTRL_BATT_TEMP_ADC_B,       CFG_OK);
    EXPECT_ERR(c.globals.batt_temp_src = (ctrl_batt_temp_src_t)3,    CFG_ERR_RANGE_BATT_TEMP_SRC);
    EXPECT_ERR(c.globals.batt_temp_src = (ctrl_batt_temp_src_t)255,  CFG_ERR_RANGE_BATT_TEMP_SRC);
    /* require_batt_temp is a plain bool — both values are legal on their own. */
    EXPECT_ERR(c.globals.require_batt_temp = true,  CFG_OK);
    EXPECT_ERR(c.globals.require_batt_temp = false, CFG_OK);

    /* §2.1 limit set: <= 0 is "unset", NaN/inf/negative are not. */
    EXPECT_ERR(c.limits.battery_c_limit = 0.0f,     CFG_OK);
    EXPECT_ERR(c.limits.battery_c_limit = NAN,      CFG_ERR_SANITY_BATTERY_C);
    EXPECT_ERR(c.limits.wiring_limit_a = -1.0f,     CFG_ERR_SANITY_WIRING_A);
    EXPECT_ERR(c.limits.alternator_limit_a = INFINITY, CFG_ERR_SANITY_ALT_A);

    /* T1 skip-BULK. */
    EXPECT_ERR(c.globals.skip_bulk_vcell = -0.1f,   CFG_ERR_SANITY_SKIP_BULK_VCELL);
    EXPECT_ERR(c.globals.skip_bulk_vcell = NAN,     CFG_ERR_SANITY_SKIP_BULK_VCELL);
    EXPECT_ERR(c.globals.skip_bulk_soc_pct = 101,   CFG_ERR_RANGE_SKIP_BULK_SOC);
    EXPECT_ERR(c.globals.skip_bulk_soc_pct = 95,    CFG_OK);
    EXPECT_ERR(c.globals.skip_bulk_soc_pct = -1,    CFG_OK);      /* off */

    /* Profile identity + shape. */
    EXPECT_ERR(c.profiles[3].p.id = 9,              CFG_ERR_PROFILE_ID);
    EXPECT_ERR(c.profiles[0].p.rest_mode = (ctrl_rest_mode_t)7, CFG_ERR_PROFILE_REST_MODE);
    {
        ctrl_config_t c = cfg_test_baseline();
        c.profiles[3].p.id = 9;
        int bad = -1;
        CHECK(ctrl_config_validate(&c, &bad) == CFG_ERR_PROFILE_ID);
        CHECK(bad == 3);                            /* the failing slot is reported */
    }

    /* §3.3 primitive refs: a resolved value must be the primitive it names. */
    EXPECT_ERR(c.profiles[0].p.cv_target_vcell = 3.55f,    CFG_ERR_PROFILE_CV_REF);
    EXPECT_ERR(c.profiles[0].p.rest_voltage_vcell = 3.38f, CFG_ERR_PROFILE_REST_V_REF);
    /* A literal (no ref) is bounded by the §1 span instead. */
    EXPECT_ERR((c.profiles[0].cv_target_ref = CFG_PRIM_NONE,
                c.profiles[0].p.cv_target_vcell = 4.00f),   CFG_ERR_PROFILE_CV_RANGE);
    EXPECT_ERR((c.profiles[0].cv_target_ref = CFG_PRIM_NONE,
                c.profiles[0].p.cv_target_vcell = 3.55f),   CFG_OK);
    EXPECT_ERR((c.profiles[0].rest_voltage_ref = CFG_PRIM_NONE,
                c.profiles[0].p.rest_voltage_vcell = 3.10f), CFG_ERR_PROFILE_REST_V_RANGE);
    /* Zero-rest profiles carry no rest voltage or cap at all ("—", §4.1). */
    EXPECT_ERR((c.profiles[2].p.rest_voltage_vcell = 99.0f,
                c.profiles[2].p.rest_power_cap_w = -5.0f),  CFG_OK);

    /* Per-profile sanity for the parameters §3 gives no range for. */
    EXPECT_ERR(c.profiles[0].p.rest_power_cap_w = -1.0f, CFG_ERR_SANITY_REST_POWER_CAP);
    EXPECT_ERR(c.profiles[1].p.v_revert_vcell = NAN,     CFG_ERR_SANITY_V_REVERT);
    EXPECT_ERR(c.profiles[1].p.soc_revert_pct = -2,      CFG_ERR_RANGE_SOC_REVERT);
    EXPECT_ERR(c.profiles[1].p.soc_revert_pct = 101,     CFG_ERR_RANGE_SOC_REVERT);
    EXPECT_ERR(c.profiles[1].p.soc_revert_pct = -1,      CFG_OK);   /* disabled */
    EXPECT_ERR(c.profiles[1].p.ah_revert = -1.0f,        CFG_ERR_SANITY_AH_REVERT);
    EXPECT_ERR(c.profiles[1].p.t_revert_hold_s = 65535u, CFG_OK);   /* [SPEC-GAP] no range */
    EXPECT_ERR(c.globals.cv_hold_exit_min = 65535u,      CFG_OK);   /* [SPEC-GAP] no range */

    /* A reserved slot is not held to any of it. */
    EXPECT_ERR((c.profiles[6].p.rest_mode = (ctrl_rest_mode_t)9,
                c.profiles[6].p.v_revert_vcell = NAN),   CFG_OK);
    EXPECT_ERR(c.profiles[6].p.id = 3,                   CFG_ERR_PROFILE_ID);

    /* active_profile. */
    EXPECT_ERR(c.active_profile = 0,  CFG_ERR_ACTIVE_PROFILE);
    EXPECT_ERR(c.active_profile = 8,  CFG_ERR_ACTIVE_PROFILE);
    EXPECT_ERR(c.active_profile = 7,  CFG_ERR_ACTIVE_PROFILE);   /* reserved slot */
    EXPECT_ERR(c.active_profile = 6,  CFG_OK);                   /* Limp Home is selectable */

    /* Every code carries its own text (the client's error surface): no code may
     * fall through to the "unknown" default. */
    {
        const char *unknown = cfg_err_str(CFG_ERR__COUNT);
        int missing = 0;
        for (int e = 0; e < (int)CFG_ERR__COUNT; e++) {
            const char *s = cfg_err_str((cfg_err_t)e);
            if (s == NULL || s[0] == '\0' || s == unknown) {
                printf("  [C5] code %d has no text\n", e);
                missing++;
            }
        }
        CHECK(missing == 0);
    }
}

/* =========================================================================== *
 * C6 — primitive resolution (§1 "editing a primitive propagates")
 * =========================================================================== */
static void c6_resolution(void)
{
    ctrl_config_t c = cfg_test_baseline();
    CHECK_FEQ(c.profiles[0].p.cv_target_vcell, 3.60f, 1e-6);      /* v_bulk */
    CHECK_FEQ(c.profiles[3].p.cv_target_vcell, 3.50f, 1e-6);      /* v_bulk_cons */
    CHECK_FEQ(c.profiles[1].p.rest_voltage_vcell, 3.35f, 1e-6);   /* v_float_cons */
    CHECK_FEQ(c.profiles[5].p.cv_target_vcell, 3.30f, 1e-6);      /* v_limp */

    /* Edit one primitive: every profile that references it follows, and so does
     * globals.limp_vcell for v_limp. */
    c.prim.v_bulk = 3.58f;
    c.prim.v_limp = 3.26f;
    cfg_resolve_primitives(&c);
    CHECK_FEQ(c.profiles[0].p.cv_target_vcell, 3.58f, 1e-6);
    CHECK_FEQ(c.profiles[2].p.cv_target_vcell, 3.58f, 1e-6);
    CHECK_FEQ(c.profiles[3].p.cv_target_vcell, 3.50f, 1e-6);      /* unrelated ref */
    CHECK_FEQ(c.profiles[5].p.rest_voltage_vcell, 3.26f, 1e-6);
    CHECK_FEQ(c.globals.limp_vcell, 3.26f, 1e-6);
    CHECK(ctrl_config_validate(&c, NULL) == CFG_OK);

    /* Refs that name nothing resolve to NAN and leave the literal alone. */
    CHECK(isnan(cfg_primitive_value(&c.prim, CFG_PRIM_NONE)));
    CHECK(isnan(cfg_primitive_value(&c.prim, (cfg_prim_ref_t)99)));
    CHECK(isnan(cfg_primitive_value(NULL, CFG_PRIM_V_BULK)));
    c.profiles[6].p.cv_target_vcell = 3.99f;
    cfg_resolve_primitives(&c);
    CHECK_FEQ(c.profiles[6].p.cv_target_vcell, 3.99f, 1e-6);
    cfg_resolve_primitives(NULL);      /* must not fault */
}

void test_config(void)
{
    printf("\n-- config store (codec / slots / validator) --\n");
    c1_crc();
    c2_codec();
    c3_rejection();
    c4_store();
    c5_validator();
    c6_resolution();
}
