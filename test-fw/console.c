/*
 * console.c — line-oriented command console. Contract: console.h.
 *
 * FLOAT PRINTING: newlib-nano (cmake/gcc-arm-none-eabi.cmake, --specs=
 * nano.specs) has no floating-point conversion in printf, so this file never
 * calls snprintf("%f"/"%g") — same constraint control/Src/config_json.c
 * documents for the same reason. Rather than hand-roll a second float
 * formatter, this reuses cfg_json_fmt_g() (control/Inc/config_json.h),
 * which is PURE C (no control-core types, HAL-free) and was written and
 * exposed for exactly this reason ("this module cannot call snprintf('%g')
 * and neither can anything else in the firmware" — config_json.c's own file
 * header). test-fw links control/Src/config_json.c for this one function;
 * see CMakeLists.txt.
 *
 * SPDX-License-Identifier: MIT
 */
#include "console.h"
#include "usb_cdc.h"
#include "board.h"
#include "field_drive.h"
#include "field_guard.h"
#include "dio.h"
#include "sensors.h"
#include "ina2xx.h"
#include "stator_rpm.h"
#include "eeprom24c16.h"
#include "can_test.h"
#include "i2c_scan.h"
#include "crash_record.h"
#include "watchdog.h"
#include "config_json.h"
#include "stm32f0xx_hal.h"

#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <math.h>

/* ---- tiny line-builder: append-only, bounds-checked, no libc float ------- */

#define LB_CAP 200

typedef struct { char buf[LB_CAP]; int len; } linebuf_t;

static void lb_reset(linebuf_t *lb) { lb->len = 0; }

static void lb_str(linebuf_t *lb, const char *s)
{
    while (*s != '\0' && lb->len < LB_CAP - 1) lb->buf[lb->len++] = *s++;
}

static void lb_char(linebuf_t *lb, char c)
{
    if (lb->len < LB_CAP - 1) lb->buf[lb->len++] = c;
}

static void lb_uint(linebuf_t *lb, uint32_t v)
{
    char tmp[10];
    int  n = 0;
    if (v == 0u) { lb_char(lb, '0'); return; }
    while (v > 0u && n < (int)sizeof tmp) { tmp[n++] = (char)('0' + (v % 10u)); v /= 10u; }
    while (n > 0) lb_char(lb, tmp[--n]);
}

/* Two-digit uppercase hex — used for I2C 7-bit addresses. */
static void lb_hex2(linebuf_t *lb, uint8_t v)
{
    static const char H[] = "0123456789ABCDEF";
    lb_char(lb, H[(v >> 4) & 0xFu]);
    lb_char(lb, H[v & 0xFu]);
}

/* Eight-digit uppercase hex, e.g. for a crash record's flash PC address. */
static void lb_hex32(linebuf_t *lb, uint32_t v)
{
    lb_hex2(lb, (uint8_t)(v >> 24));
    lb_hex2(lb, (uint8_t)(v >> 16));
    lb_hex2(lb, (uint8_t)(v >> 8));
    lb_hex2(lb, (uint8_t)v);
}

/* Float, `sig` significant digits. NaN/Inf print as "NaN" rather than
 * whatever cfg_json_fmt_g's documented -1 failure would otherwise leave —
 * this firmware uses NAN as a live "sensor open/unavailable" value
 * (sensors.c, stator_rpm.c), so the console must be able to show it clearly,
 * not as an error. */
static void lb_float(linebuf_t *lb, float v, int sig)
{
    if (!isfinite(v)) { lb_str(lb, "NaN"); return; }
    char tmp[40];
    int  n = cfg_json_fmt_g(tmp, (int)sizeof tmp, (double)v, sig);
    if (n < 0) { lb_str(lb, "ERR"); return; }
    for (int i = 0; i < n && lb->len < LB_CAP - 1; i++) lb->buf[lb->len++] = tmp[i];
}

static void lb_flush(linebuf_t *lb)
{
    (void)usb_cdc_write((const uint8_t *)lb->buf, lb->len);
    lb_reset(lb);
}

/* One-shot "build a line, terminate it, send it" for the common case. */
#define LINE(body) do { linebuf_t _lb; lb_reset(&_lb); body; lb_str(&_lb, "\r\n"); lb_flush(&_lb); } while (0)

static void con_prompt(void) { LINE(lb_str(&_lb, "> ")); }

/* ---- line assembly --------------------------------------------------------- */

#define CON_LINE_MAX  96
static char s_line[CON_LINE_MAX];
static int  s_line_len;

void console_greet(void)
{
    s_line_len = 0;   /* a fresh session starts on a clean line, never mid-command */
    LINE(lb_str(&_lb,
        "WS500-OpenFW test-fw (PROJECT_PLAN Sec4/M2, deliverable #14)"));
    LINE({
        lb_str(&_lb, "BENCH ONLY. DUMMY LOAD ONLY. Field duty capped at ");
        lb_float(&_lb, field_guard_cap() * 100.0f, 3);
        lb_str(&_lb, "% -- see PROJECT_PLAN Sec5.");
    });
    LINE(lb_str(&_lb, "Type 'help' for commands."));
    con_prompt();
}

void console_init(void)
{
    /* The greeting emitted here goes NOWHERE: console_init() runs at boot,
     * long before the host has enumerated us and asserted DTR, and
     * usb_cdc_write() correctly DROPS rather than blocks while unconfigured.
     * That silently swallowed the Sec5 safety banner -- the one thing a bench
     * operator most needs to read before typing `field`. main.c therefore
     * calls console_greet() again on the DTR RISING edge, which is the first
     * moment a terminal can actually receive it. Kept here as well so the
     * state reset happens even if the link never comes up. */
    console_greet();
}

/* ---- command implementations ---------------------------------------------- */

static void cmd_help(void)
{
    LINE(lb_str(&_lb, "commands:"));
    LINE(lb_str(&_lb, "  help                 this text"));
    LINE(lb_str(&_lb, "  status               uptime, reset cause, watchdog, driver health"));
    LINE(lb_str(&_lb, "  gpio                 DIP bank + PB13 + output states"));
    LINE(lb_str(&_lb, "  gpio outa <on|off>   drive PA9 (OUT_LAMP_OR_LED_PIN) directly"));
    LINE(lb_str(&_lb, "  gpio outb <on|off>   drive PB14 (STATUS_OUT_B_PINS) directly"));
    LINE(lb_str(&_lb, "  adc                  dump all 7 ADC channels, raw + scaled"));
    LINE(lb_str(&_lb, "  stator               stator/RPM capture status"));
    LINE(lb_str(&_lb, "  field <pct>          command field duty, 0..100 (CAPPED, see 'status')"));
    LINE(lb_str(&_lb, "  field off            field off now"));
    LINE(lb_str(&_lb, "  field                show current field-guard state"));
    LINE(lb_str(&_lb, "  cutoff               trigger the software fault cutoff (LATCHES until reset)"));
    LINE(lb_str(&_lb, "  can loop             bxCAN internal silent-loopback self-test (no wiring)"));
    LINE(lb_str(&_lb, "  can echo             bxCAN external echo test (needs a live terminated bus)"));
    LINE(lb_str(&_lb, "  i2cscan              scan I2C1 and I2C2 for ACKing addresses (~1.1s)"));
}

static void cmd_status(void)
{
    LINE({
        lb_str(&_lb, "uptime_ms="); lb_uint(&_lb, HAL_GetTick());
        lb_str(&_lb, " boots="); lb_uint(&_lb, crash_boot_count());
        lb_str(&_lb, " reset_cause=0x"); lb_hex2(&_lb, (uint8_t)crash_reset_cause());
    });
    LINE({
        lb_str(&_lb, "reset counts: POR="); lb_uint(&_lb, crash_reset_cause_count(RESET_CAUSE_POR));
        lb_str(&_lb, " PIN=");  lb_uint(&_lb, crash_reset_cause_count(RESET_CAUSE_PIN));
        lb_str(&_lb, " SW=");   lb_uint(&_lb, crash_reset_cause_count(RESET_CAUSE_SOFTWARE));
        lb_str(&_lb, " IWDG="); lb_uint(&_lb, crash_reset_cause_count(RESET_CAUSE_IWDG));
    });
    LINE({
        lb_str(&_lb, "crash records="); lb_uint(&_lb, crash_record_count());
        lb_str(&_lb, " fault_streak="); lb_uint(&_lb, crash_fault_streak());
        lb_str(&_lb, " iwdg_streak=");  lb_uint(&_lb, crash_iwdg_streak());
        lb_str(&_lb, " limp_latched="); lb_str(&_lb, watchdog_limp_latched() ? "YES" : "no");
    });
    if (crash_record_count() > 0u) {
        const crash_record_t *r = crash_record_get(0);
        if (r != NULL) {
            LINE({
                lb_str(&_lb, "newest crash: kind="); lb_uint(&_lb, r->kind);
                lb_str(&_lb, " uptime_ms="); lb_uint(&_lb, r->uptime_ms);
                lb_str(&_lb, " pc=0x"); lb_hex32(&_lb, r->pc);
                lb_str(&_lb, " lr=0x"); lb_hex32(&_lb, r->lr);
            });
        }
    }
    LINE({
        lb_str(&_lb, "watchdog: kicks="); lb_uint(&_lb, watchdog_kick_count());
        lb_str(&_lb, " starves="); lb_uint(&_lb, watchdog_starve_count());
        lb_str(&_lb, " starved_mask=0x"); lb_hex2(&_lb, (uint8_t)watchdog_starved_mask());
    });
    LINE({
        lb_str(&_lb, "INA226 ok="); lb_str(&_lb, ina2xx_ok() ? "yes" : "NO");
        lb_str(&_lb, " xact_fail="); lb_uint(&_lb, ina2xx_xact_failures());
        lb_str(&_lb, "  EEPROM ok="); lb_str(&_lb, eeprom24c16_ok() ? "yes" : "NO");
        lb_str(&_lb, " xact_fail="); lb_uint(&_lb, eeprom24c16_xact_failures());
    });
    LINE({
        lb_str(&_lb, "field: cap="); lb_float(&_lb, field_guard_cap() * 100.0f, 3);
        lb_str(&_lb, "%  faulted="); lb_str(&_lb, field_drive_faulted() ? "YES (reset to clear)" : "no");
    });
}

static void print_out(linebuf_t *lb, const char *label, bool on)
{
    lb_str(lb, label); lb_str(lb, "="); lb_str(lb, on ? "HIGH" : "low"); lb_str(lb, "  ");
}

/* dio.c does not expose the current output levels (only setters) -- track
 * them here in the console, which is the only place in test-fw that sets
 * them, so this shadow state can never disagree with the hardware. */
static bool s_out_a, s_out_b;

static void cmd_gpio(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "outa") == 0) {
        if (argc < 3) { LINE(lb_str(&_lb, "usage: gpio outa <on|off>")); return; }
        bool on = (strcmp(argv[2], "on") == 0);
        dio_set_out_a(on); s_out_a = on;
        LINE({ lb_str(&_lb, "PA9 (out_a) -> "); lb_str(&_lb, on ? "HIGH" : "low"); });
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "outb") == 0) {
        if (argc < 3) { LINE(lb_str(&_lb, "usage: gpio outb <on|off>")); return; }
        bool on = (strcmp(argv[2], "on") == 0);
        dio_set_out_b(on); s_out_b = on;
        LINE({ lb_str(&_lb, "PB14 (out_b) -> "); lb_str(&_lb, on ? "HIGH" : "low"); });
        return;
    }

    LINE({
        lb_str(&_lb, "DIP (PA4-7,PB0-2, batt-cap bank, Sec0.6 V1): 0x");
        lb_hex2(&_lb, dio_dip_value());
        lb_str(&_lb, "  (boot value 0x"); lb_hex2(&_lb, dio_dip_boot_value()); lb_str(&_lb, ")");
    });
    LINE({
        lb_str(&_lb, "PB13 (CTRL_IN, Enable/Ignition OR Feature-In -- split bench-pending, Sec0.6 V1) = ");
        lb_str(&_lb, dio_ctrl_in() ? "HIGH" : "low");
    });
    LINE({ print_out(&_lb, "out_a(PA9)", s_out_a); print_out(&_lb, "out_b(PB14)", s_out_b); });
    LINE(lb_str(&_lb, "use 'gpio outa on|off' / 'gpio outb on|off' while watching the physical lamp/LED"));
}

static void cmd_adc(void)
{
    sensor_readings_t r;
    sensors_read(&r);

    LINE({
        lb_str(&_lb, "ADC (12-bit, x4 sw-averaged, Sec0.6 V4)  Vdda=");
        lb_float(&_lb, sensors_vdda(), 5); lb_str(&_lb, " mV");
    });
    LINE({
        lb_str(&_lb, "[0] PA1  NTC b3950 raw="); lb_uint(&_lb, sensors_raw(SENSOR_CH_TEMP_A1));
        lb_str(&_lb, "  temp="); lb_float(&_lb, r.alt_temp_c, 4); lb_str(&_lb, " C  (alt/probe #1)");
    });
    LINE({
        lb_str(&_lb, "[1] PA2  NTC b3950 raw="); lb_uint(&_lb, sensors_raw(SENSOR_CH_TEMP_A2));
        lb_str(&_lb, "  temp="); lb_float(&_lb, r.alt_temp2_c, 4); lb_str(&_lb, " C  (alt/probe #2)");
    });
    LINE({
        lb_str(&_lb, "[2] PA3  NTC b3380 raw="); lb_uint(&_lb, sensors_raw(SENSOR_CH_TEMP_FET));
        lb_str(&_lb, "  temp="); lb_float(&_lb, r.driver_temp_c, 4); lb_str(&_lb, " C  (FET/driver, NOT battery, Sec0.6 V8)");
    });
    LINE({
        lb_str(&_lb, "[3] PC5  VBAT   raw="); lb_uint(&_lb, sensors_raw(SENSOR_CH_VBAT));
        lb_str(&_lb, "  volts="); lb_float(&_lb, r.vbat_pack_v, 6); lb_str(&_lb, " V  (34.3333:1 divider)");
    });
    LINE({
        lb_str(&_lb, "[4] internal temp sensor raw="); lb_uint(&_lb, sensors_raw(SENSOR_SLOT_TEMPSENSOR));
        lb_str(&_lb, "  [5] VREFINT raw="); lb_uint(&_lb, sensors_raw(SENSOR_SLOT_VREFINT));
        lb_str(&_lb, "  [6] VBAT(int) raw="); lb_uint(&_lb, sensors_raw(SENSOR_SLOT_VBAT_INT));
    });
    LINE({
        lb_str(&_lb, "INA226 (I2C, addr 0x40, see 'i2cscan' for which bus): current=");
        lb_float(&_lb, r.amps_batt, 5); lb_str(&_lb, " A  bus_v="); lb_float(&_lb, r.bus_v, 5);
        lb_str(&_lb, " V  ok="); lb_str(&_lb, ina2xx_ok() ? "yes" : "NO");
    });
    LINE(lb_str(&_lb,
        "NOTE: NTC R0/Rfixed ratio is a placeholder (see sensors.c) -- cross-check"
        " temp readings against a reference meter before trusting absolute values."));
}

static void cmd_stator(void)
{
    static const char *ST[] = { "FRESH", "STALE", "LOST" };
    stator_rpm_state_t st = stator_rpm_state();
    LINE({
        lb_str(&_lb, "PA10 stator: state="); lb_str(&_lb, ST[(int)st]);
        lb_str(&_lb, "  freq="); lb_float(&_lb, stator_rpm_freq_hz(), 5); lb_str(&_lb, " Hz");
        lb_str(&_lb, "  rpm="); lb_float(&_lb, stator_rpm_rpm(), 5);
        lb_str(&_lb, " (NaN expected: poles/pulley_ratio unconfigured, GH#28)");
    });
}

static void cmd_field(int argc, char *argv[])
{
    if (argc >= 2 && strcmp(argv[1], "off") == 0) {
        field_guard_off();
        LINE(lb_str(&_lb, "field OFF"));
        return;
    }
    if (argc >= 2) {
        int consumed = 0; double val = 0.0;
        if (!cfg_json_scan_num(argv[1], (int)strlen(argv[1]), &consumed, &val) ||
            consumed != (int)strlen(argv[1])) {
            LINE(lb_str(&_lb, "usage: field <0..100> | field off"));
            return;
        }
        const float applied = field_guard_set((float)val);
        LINE({
            lb_str(&_lb, "requested "); lb_float(&_lb, (float)val, 5);
            lb_str(&_lb, "% -> applied "); lb_float(&_lb, applied * 100.0f, 5);
            lb_str(&_lb, "% (hard cap "); lb_float(&_lb, field_guard_cap() * 100.0f, 3);
            lb_str(&_lb, "%, PROJECT_PLAN Sec5)");
        });
        LINE(lb_str(&_lb,
            "self-expires in ~5s unless refreshed with another 'field <pct>' -- "
            "'field off' or a dropped USB link cut it immediately"));
        return;
    }
    LINE({
        lb_str(&_lb, "commanded="); lb_str(&_lb, field_guard_commanded() ? "yes" : "no");
        lb_str(&_lb, "  applied="); lb_float(&_lb, field_guard_duty() * 100.0f, 5);
        lb_str(&_lb, "%  remaining="); lb_uint(&_lb, field_guard_remaining_ms());
        lb_str(&_lb, " ms  faulted="); lb_str(&_lb, field_drive_faulted() ? "YES" : "no");
    });
}

static void cmd_cutoff(void)
{
    const bool moe_before = (TIM1->BDTR & TIM_BDTR_MOE) != 0u;
    field_drive_fault_cutoff();
    field_guard_off();   /* keep field_guard's own bookkeeping honest */
    const bool moe_after = (TIM1->BDTR & TIM_BDTR_MOE) != 0u;

    LINE({ lb_str(&_lb, "BEFORE: TIM1 BDTR.MOE="); lb_uint(&_lb, moe_before ? 1u : 0u); lb_str(&_lb, " (1=PWM enabled)"); });
    LINE({ lb_str(&_lb, "AFTER:  TIM1 BDTR.MOE="); lb_uint(&_lb, moe_after ? 1u : 0u);  lb_str(&_lb, " (outputs forced inactive)"); });
    LINE(lb_str(&_lb,
        "this is the SOFTWARE cutoff path (BDTR.BKE=0, BKIN unrouted -- PROJECT_PLAN Sec0.6 V1/V2),"
        " NOT a hardware break-input test."));
    LINE(lb_str(&_lb, "field is now LATCHED off (field_drive_faulted()) -- reset the board to clear it."));
}

static void cmd_can(int argc, char *argv[])
{
    if (can_test_busy()) { LINE(lb_str(&_lb, "a CAN test is already running -- wait for the result")); return; }

    if (argc >= 2 && strcmp(argv[1], "loop") == 0) {
        if (!can_test_start_loop()) LINE(lb_str(&_lb, "CAN bring-up FAILED (bxCAN init/filter/start or first TX)"));
        else LINE(lb_str(&_lb, "loopback self-test running (silent, no bus traffic) -- result in <1s"));
        return;
    }
    if (argc >= 2 && strcmp(argv[1], "echo") == 0) {
        if (!can_test_start_echo()) LINE(lb_str(&_lb, "CAN bring-up FAILED (bxCAN init/filter/start or first TX)"));
        else LINE(lb_str(&_lb,
            "external echo test running (normal mode, needs a terminated live bus"
            " with another node/analyzer to ACK) -- result in <2s"));
        return;
    }
    LINE(lb_str(&_lb, "usage: can loop | can echo"));
}

void console_report_can_result(void)
{
    LINE({
        lb_str(&_lb, "CAN "); lb_str(&_lb, can_test_last_was_echo() ? "echo" : "loop");
        lb_str(&_lb, " test: "); lb_str(&_lb, can_test_last_pass() ? "PASS" : "FAIL/TIMEOUT");
    });
    if (!can_test_last_pass() && can_test_last_was_echo()) {
        LINE(lb_str(&_lb,
            "(expected without a terminated bus + a second live node/analyzer to ACK"
            " the frame -- AutoRetransmission keeps a lone transmitter retrying"
            " until the window closed, which is the correct, bounded failure mode)"));
    }
    con_prompt();
}

static void cmd_i2cscan(void)
{
    if (i2c_scan_busy()) { LINE(lb_str(&_lb, "a scan is already running")); return; }
    (void)i2c_scan_start();
    LINE(lb_str(&_lb, "scanning I2C1 + I2C2, 0x08..0x77 (~1.1s) -- results print when done"));
    LINE(lb_str(&_lb,
        "note: this transiently disturbs the live INA226/EEPROM drivers on whichever"
        " bus they are actually on; they self-heal via the Sec7 R6 error budget"));
}

void console_report_i2c_result(void)
{
    LINE(lb_str(&_lb, "I2C scan results (Sec0.6 V3/V7: expect INA226 0x40 + EEPROM 0x50, both on I2C2):"));
    bool any1 = false, any2 = false;
    linebuf_t lb;

    lb_reset(&lb); lb_str(&lb, "  I2C1: ");
    for (uint8_t a = 0x08u; a <= 0x77u; a++) {
        if (i2c_scan_found(1, a)) { any1 = true; lb_hex2(&lb, a); lb_str(&lb, " "); }
    }
    if (!any1) lb_str(&lb, "(nothing found)");
    lb_str(&lb, "\r\n"); lb_flush(&lb);

    lb_reset(&lb); lb_str(&lb, "  I2C2: ");
    for (uint8_t a = 0x08u; a <= 0x77u; a++) {
        if (i2c_scan_found(2, a)) { any2 = true; lb_hex2(&lb, a); lb_str(&lb, " "); }
    }
    if (!any2) lb_str(&lb, "(nothing found)");
    lb_str(&lb, "\r\n"); lb_flush(&lb);

    con_prompt();
}

/* ---- dispatch --------------------------------------------------------------- */

static int split_args(char *s, char *argv[], int max_args)
{
    int argc = 0;
    while (*s != '\0') {
        while (*s == ' ') s++;
        if (*s == '\0') break;
        if (argc < max_args) argv[argc++] = s;
        while (*s != '\0' && *s != ' ') s++;
        if (*s != '\0') *s++ = '\0';
    }
    return argc;
}

static void dispatch(char *line)
{
    char *argv[4];
    int   argc = split_args(line, argv, 4);
    if (argc == 0) { con_prompt(); return; }

    if      (strcmp(argv[0], "help")    == 0) cmd_help();
    else if (strcmp(argv[0], "status")  == 0) cmd_status();
    else if (strcmp(argv[0], "gpio")    == 0) cmd_gpio(argc, argv);
    else if (strcmp(argv[0], "adc")     == 0) cmd_adc();
    else if (strcmp(argv[0], "stator")  == 0) cmd_stator();
    else if (strcmp(argv[0], "field")   == 0) cmd_field(argc, argv);
    else if (strcmp(argv[0], "cutoff")  == 0) cmd_cutoff();
    else if (strcmp(argv[0], "can")     == 0) cmd_can(argc, argv);
    else if (strcmp(argv[0], "i2cscan") == 0) cmd_i2cscan();
    else LINE({ lb_str(&_lb, "unknown command: "); lb_str(&_lb, argv[0]); lb_str(&_lb, " (try 'help')"); });

    con_prompt();
}

void console_poll(void)
{
    uint8_t chunk[64];
    int n = usb_cdc_read(chunk, (int)sizeof chunk);
    for (int i = 0; i < n; i++) {
        char c = (char)chunk[i];
        if (c == '\r') continue;
        if (c == '\n') {
            s_line[s_line_len] = '\0';
            dispatch(s_line);
            s_line_len = 0;
            continue;
        }
        if (s_line_len < CON_LINE_MAX - 1) s_line[s_line_len++] = c;
        /* else: line too long -- silently drop extra chars until the newline,
         * the truncated line above still dispatches something rather than
         * wedging the console on one bad paste. */
    }
}
