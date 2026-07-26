/*
 * ina2xx.c — TI INA226 driver (7-bit 0x40). INA226-ONLY, hardwired: the
 * stock firmware has no variant auto-detect (§0.6 V3 — the old INA228/238
 * die-ID probe was fiction and is removed; git history preserves it).
 *
 * ┌─ BUS CAVEAT — CONFIRM AT BENCH BEFORE FIRST HARDWARE COMMS ───────────────┐
 * │ This driver currently brings up I2C1 (PB6/PB7). The 2026-07-24 stock-      │
 * │ binary pass (§0.6 V7) found the shipped firmware DeInits I2C1 at boot and  │
 * │ runs BOTH the INA226 (0x40) and the 24C16 config EEPROM (0x50) on **I2C2** │
 * │ (PB10/PB11) — i.e. the INA226 is very likely wired to I2C2, and V3's       │
 * │ "I2C1" was the pre-DeInit reading. Evidence precedence puts a bench scope  │
 * │ (Stage-A, readings-only, safe now) above the binary, so this is flagged    │
 * │ not yet flipped: scope PB6/PB7 vs PB10/PB11 at boot — the pair carrying    │
 * │ 0x40/0xA0 traffic is the live bus. If it is I2C2, switch INSTANCE→I2C2,    │
 * │ pins→PB10/PB11, and the timing to I2C2's PCLK-derived value. See GH issue  │
 * │ for the bus-confirmation task. Getting this wrong = no sensor comms.       │
 * └───────────────────────────────────────────────────────────────────────────┘
 *
 * Register map (verified from the stock binary): 0x00 CONFIG, 0x01 SHUNT_V
 * (2.5 uV/LSB, signed 16-bit), 0x02 BUS_V (1.25 mV/LSB), 0x05 CALIBRATION,
 * 0x06 Mask/Enable (CVRF = conversion ready). All registers 16-bit big-endian
 * on the wire. Access pattern mirrors stock: gate on Mask/Enable CVRF, then
 * read SHUNT_V + BUS_V raw and scale in software.
 *
 * CALIBRATION strategy: the register is deliberately left at its POR value (0)
 * and the on-chip CURRENT (0x04) / POWER (0x03) registers are never read.
 * Current is computed in software as V_shunt / R_shunt (R_shunt from the
 * configured shunt rating, not a copied constant), and power as bus_v * current.
 * This is the simpler of the two allowed options: one scaling pathway, nothing
 * on-chip to keep consistent with the configured shunt.
 * SPDX-License-Identifier: MIT
 */
#include "ina2xx.h"
#include "board.h"
#include "err_budget.h"
#include <math.h>

/* CONFIG (0x00) field encodings — INA226 datasheet §7.6.1. The value written is
 * BUILT from these fields (never a copied magic constant). */
#define INA226_CFG_RESERVED14    (1u << 14)    /* reserved bit, reads/writes as 1 */
#define INA226_CFG_AVG_16        (0x2u << 9)   /* 16-sample averaging */
#define INA226_CFG_VBUSCT_1100US (0x4u << 6)   /* 1.1 ms bus conversion time */
#define INA226_CFG_VSHCT_1100US  (0x4u << 3)   /* 1.1 ms shunt conversion time */
#define INA226_CFG_MODE_CONT_ALL (0x7u)        /* continuous shunt + bus */

/* Fixed silicon LSBs (INA226 datasheet; SHUNT_V LSB corroborated upstream, V5). */
#define INA226_SHUNT_LSB_V       2.5e-6f       /* 2.5 uV per SHUNT_V count */
#define INA226_BUS_LSB_V         1.25e-3f      /* 1.25 mV per BUS_V count */

/* Mask/Enable (0x06) bits. Reading the register clears CVRF. */
#define INA226_MASKEN_CVRF       (1u << 3)     /* conversion-ready flag */

/* I2C1 kernel clock on the F072 defaults to HSI (8 MHz); TIMINGR below is the
 * ST reference value for 100 kHz standard mode at 8 MHz I2CCLK (RM0091 / CubeMX). */
#define INA_I2C1_TIMING          0x2000090Eu

/* Robustness (§7 R6): every transaction is bounded — a fixed number of
 * attempts with a fixed HAL timeout each, never an unbounded wait/loop.
 * Two layers, and they answer different questions:
 *   INA_FAIL_LIMIT / ina2xx_ok() — "is the reading I am about to return
 *     trustworthy right now?" Local, self-clearing, drives NAN.
 *   err_budget.c (ERRB_INA226)   — "has this peripheral failed often enough
 *     that the SYSTEM should change behaviour?" Escalates bus re-init, then a
 *     latched fault bit that main.c feeds to the control core. */
#define INA_XACT_TIMEOUT_MS      5u
#define INA_XACT_ATTEMPTS        3u            /* tries per transaction */
#define INA_FAIL_LIMIT           9u            /* consecutive failed attempts ->
                                                  not-ok (== 3 whole transactions) */

static I2C_HandleTypeDef s_i2c1;
static float    s_r_shunt_ohm;    /* from configured shunt rating (board config) */
static float    s_current_a;      /* last good sample */
static float    s_bus_v;          /* last good sample */
static bool     s_have_sample;
static uint32_t s_fail_total;     /* cumulative failed attempts (never resets) */
static uint32_t s_fail_consec;    /* consecutive failed attempts (resets on success) */
static uint16_t s_cfg_word;       /* CONFIG value, replayed after a bus re-init */
static bool     s_reinit_busy;    /* re-entry guard, see ina_bus_reinit() */

static bool ina_i2c_bring_up(void);
static void ina_bus_reinit(void);

/* One failed transaction (all INA_XACT_ATTEMPTS attempts used up) reported to
 * the §7 R6 error budget, which decides between "keep going", "re-initialise
 * the bus" and "latch a fault". The fault bit reaches the control core via
 * main.c's ext_faults mapping — this driver never decides policy itself. */
static void ina_note_failure(void)
{
    if (errb_fail(ERRB_INA226) == ERRB_ACTION_REINIT) ina_bus_reinit();
}

/* 16-bit big-endian register read/write with bounded retries. On repeated
 * failure the counters advance and false is returned — the caller degrades to
 * NAN readings via ina2xx_ok(); nothing blocks or loops forever. */
static bool ina_read16(uint8_t reg, uint16_t *out)
{
    for (uint32_t attempt = 0; attempt < INA_XACT_ATTEMPTS; attempt++) {
        uint8_t be[2];
        if (HAL_I2C_Mem_Read(&s_i2c1, INA_I2C_ADDR7 << 1, reg,
                             I2C_MEMADD_SIZE_8BIT, be, 2U,
                             INA_XACT_TIMEOUT_MS) == HAL_OK) {
            *out = (uint16_t)(((uint16_t)be[0] << 8) | be[1]);
            s_fail_consec = 0;
            errb_ok(ERRB_INA226);
            return true;
        }
        s_fail_total++;
        s_fail_consec++;
    }
    ina_note_failure();
    return false;
}

static bool ina_write16(uint8_t reg, uint16_t val)
{
    for (uint32_t attempt = 0; attempt < INA_XACT_ATTEMPTS; attempt++) {
        uint8_t be[2] = { (uint8_t)(val >> 8), (uint8_t)(val & 0xFFu) };
        if (HAL_I2C_Mem_Write(&s_i2c1, INA_I2C_ADDR7 << 1, reg,
                              I2C_MEMADD_SIZE_8BIT, be, 2U,
                              INA_XACT_TIMEOUT_MS) == HAL_OK) {
            s_fail_consec = 0;
            errb_ok(ERRB_INA226);
            return true;
        }
        s_fail_total++;
        s_fail_consec++;
    }
    ina_note_failure();
    return false;
}

/*
 * Middle rung of the §7 R6 ladder: tear the I²C peripheral down and bring it
 * back up. This is the standard cure for the classic I²C failure mode — a
 * slave that NACKed mid-transfer and left the bus state machine wedged — and
 * it costs nothing when the real problem is elsewhere.
 *
 * The re-entry guard matters: re-initialising replays the INA226 CONFIG write,
 * which goes through ina_write16(), which on failure would ask the budget for
 * another re-init. Without the guard that recurses until the stack runs out.
 */
static void ina_bus_reinit(void)
{
    if (s_reinit_busy) return;
    s_reinit_busy = true;

    HAL_I2C_DeInit(&s_i2c1);
    if (ina_i2c_bring_up()) {
        /* The device may have browned out with the bus, so do not assume it
         * still holds our configuration. Failure here is not fatal: the next
         * sample attempt reports it through the same budget. */
        (void)ina_write16(INA_REG_CONFIG, s_cfg_word);
    }

    s_reinit_busy = false;
}

/* Stock-mirroring acquisition: poll Mask/Enable; when CVRF is set (the read
 * clears it) fetch SHUNT_V + BUS_V and refresh the cached sample. Bounded:
 * at most 3 transactions per call, no waiting on CVRF — if the conversion
 * isn't ready yet the previous sample simply stays current. */
static void ina_sample(void)
{
    uint16_t masken, shunt_raw, bus_raw;

    if (!ina_read16(INA_REG_MASK_ENABLE, &masken)) return;
    if (!(masken & INA226_MASKEN_CVRF)) return;   /* no new conversion yet */

    if (!ina_read16(INA_REG_SHUNT_V, &shunt_raw)) return;
    if (!ina_read16(INA_REG_BUS_V, &bus_raw)) return;

    /* SHUNT_V is signed (charge vs discharge); BUS_V is unsigned. */
    s_current_a   = (float)(int16_t)shunt_raw * INA226_SHUNT_LSB_V / s_r_shunt_ohm;
    s_bus_v       = (float)bus_raw * INA226_BUS_LSB_V;
    s_have_sample = true;
}

/* I²C peripheral bring-up, factored out so the §7 R6 re-init rung can reuse
 * exactly the same configuration as the initial one. */
static bool ina_i2c_bring_up(void)
{
    /* Driver owns I2C1 (PB6/PB7 AF set up in board_init). */
    __HAL_RCC_I2C1_CLK_ENABLE();
    s_i2c1.Instance             = I2C1;
    s_i2c1.Init.Timing          = INA_I2C1_TIMING;
    s_i2c1.Init.OwnAddress1     = 0;
    s_i2c1.Init.AddressingMode  = I2C_ADDRESSINGMODE_7BIT;
    s_i2c1.Init.DualAddressMode = I2C_DUALADDRESS_DISABLE;
    s_i2c1.Init.OwnAddress2     = 0;
    s_i2c1.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    s_i2c1.Init.GeneralCallMode = I2C_GENERALCALL_DISABLE;
    s_i2c1.Init.NoStretchMode   = I2C_NOSTRETCH_DISABLE;
    return HAL_I2C_Init(&s_i2c1) == HAL_OK;
}

bool ina2xx_init(float shunt_full_scale_a, float shunt_full_scale_mv)
{
    if (!(shunt_full_scale_a > 0.0f) || !(shunt_full_scale_mv > 0.0f))
        return false;
    /* R_shunt from the configured rating: 500A/50mV -> 100 uOhm. */
    s_r_shunt_ohm = (shunt_full_scale_mv / 1000.0f) / shunt_full_scale_a;
    s_have_sample = false;

    if (!ina_i2c_bring_up()) return false;

    /* Continuous shunt+bus, 16x averaging, 1.1 ms conversions -> a fresh
     * averaged pair roughly every 35 ms, plenty for the 10 ms control loop
     * (stale-by-one readings are fine; CVRF gates freshness). */
    s_cfg_word = INA226_CFG_RESERVED14 | INA226_CFG_AVG_16 |
                 INA226_CFG_VBUSCT_1100US | INA226_CFG_VSHCT_1100US |
                 INA226_CFG_MODE_CONT_ALL;
    if (!ina_write16(INA_REG_CONFIG, s_cfg_word)) return false;

    /* CALIBRATION (0x05) intentionally left at POR 0: it only feeds the on-chip
     * CURRENT/POWER registers, which this driver never reads (see file header). */
    return true;
}

bool     ina2xx_ok(void)            { return s_fail_consec < INA_FAIL_LIMIT; }
uint32_t ina2xx_xact_failures(void) { return s_fail_total; }

float ina2xx_current_a(void)
{
    ina_sample();
    return (s_have_sample && ina2xx_ok()) ? s_current_a : NAN;
}

float ina2xx_bus_v(void)
{
    ina_sample();
    return (s_have_sample && ina2xx_ok()) ? s_bus_v : NAN;
}

/* Software power: product of the cached shunt-derived current and bus voltage
 * (no bus traffic — the cache is refreshed by the current/bus accessors each
 * control tick). The on-chip POWER register is unused (CALIBRATION == 0). */
float ina2xx_power_w(void)
{
    return (s_have_sample && ina2xx_ok()) ? (s_bus_v * s_current_a) : NAN;
}
