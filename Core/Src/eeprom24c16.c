/*
 * eeprom24c16.c — 24C16-class 2 KB config EEPROM on I2C2 (7-bit family 0x50).
 * RAW byte read/write only — no record format (config schema pending, GH#28).
 *
 * Device-address computation, block/page sizing, write-cycle timing, and the
 * /WP pin all come from PROJECT_PLAN §0.6 V7 (stock binary disasm 2026-07-24);
 * see board.h for the cited constants and EEPROM_WP_PIN. Polled HAL, bounded
 * retries — same robustness style as ina2xx.c (§7 R6): every transaction is a
 * fixed number of attempts with a fixed HAL timeout each, never an unbounded
 * wait/loop.
 *
 * SPDX-License-Identifier: MIT
 */
#include "eeprom24c16.h"
#include "board.h"

/* I2C2 kernel clock is PCLK1 — unlike I2C1, the F0's RCC_CFGR3 has no I2C2SW
 * mux bit (confirmed absent in the CMSIS register header; only I2C1SW exists),
 * so I2C2 cannot be steered to HSI the way ina2xx.c's comment describes for
 * I2C1. board_clock_config() sets APB1CLKDivider = DIV1, so PCLK1 = HCLK =
 * SYSCLK = BOARD_SYSCLK_HZ = 48 MHz.
 *
 * TIMING below is COMPUTED (not a copied/measured constant) from the
 * documented STM32 I2C TIMINGR field formula (RM0091 §22.4.10 / AN4235),
 * deliberately targeting a rate at or under 100 kHz Standard-mode rather than
 * an exact 100 kHz — slower is never a bus-spec violation, faster would be:
 *   PRESC = 11 (0xB)  -> t_presc = 48 MHz / (11+1)        = 250 ns
 *   SCLL  = 23 (0x17) -> t_LOW   = (23+1) * 250 ns = 6.00 us  (>= 4.7 us min)
 *   SCLH  = 19 (0x13) -> t_HIGH  = (19+1) * 250 ns = 5.00 us  (>= 4.0 us min)
 *   => SCL period 11.0 us => ~90.9 kHz, comfortably under the 100 kHz cap with
 *      margin on both the low- and high-time minimums.
 * SCLDEL/SDADEL are given a small nonzero margin (4/2 ticks = 1.0 us / 0.5 us)
 * for setup/hold headroom rather than 0.
 * NOT bench-measured — scope PB10/PB11 (same open item as the I2C2 bus
 * confirmation already flagged for the INA226, ina2xx.c/GH#36) before relying
 * on this against real silicon and bus capacitance. */
#define EEPROM_I2C2_TIMING       0xB0421317u

#define EEPROM_XACT_TIMEOUT_MS   20u    /* bounded HAL timeout per transaction */
#define EEPROM_XACT_ATTEMPTS     3u     /* retry x3 on NACK (spec) */
#define EEPROM_FAIL_LIMIT        9u     /* 3 whole transactions' worth, mirrors ina2xx.c */

static I2C_HandleTypeDef s_i2c2;
static uint32_t s_fail_total;
static uint32_t s_fail_consec;

/* 8-bit device address, computed per access — block-select bits folded into
 * the 0xA0 family nibble (§0.6 V7). Already in the HAL's "pre-shifted" 8-bit
 * form (bit0 = R/W, always 0 here), same convention as ina2xx.c's
 * `INA_I2C_ADDR7 << 1`. */
static uint16_t eeprom_dev_addr(uint16_t word_addr)
{
    return (uint16_t)(0xA0u | (((word_addr >> 8) & 0x7u) << 1));
}

static void wp_write_enable(bool enable)
{
    HAL_GPIO_WritePin(EEPROM_WP_PORT, EEPROM_WP_PIN,
                       enable ? GPIO_PIN_RESET : GPIO_PIN_SET);
}

bool eeprom24c16_init(void)
{
    s_fail_total  = 0;
    s_fail_consec = 0;

    /* /WP GPIO mode itself is configured (and defaulted HIGH/protected) in
     * board_init(); re-assert protected here defensively. */
    wp_write_enable(false);

    __HAL_RCC_I2C2_CLK_ENABLE();
    s_i2c2.Instance              = I2C2;
    s_i2c2.Init.Timing           = EEPROM_I2C2_TIMING;
    s_i2c2.Init.OwnAddress1      = 0;
    s_i2c2.Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    s_i2c2.Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    s_i2c2.Init.OwnAddress2      = 0;
    s_i2c2.Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    s_i2c2.Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    s_i2c2.Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    return HAL_I2C_Init(&s_i2c2) == HAL_OK;
}

static bool eeprom_read_chunk(uint16_t addr, uint8_t *buf, uint16_t len)
{
    for (uint32_t attempt = 0; attempt < EEPROM_XACT_ATTEMPTS; attempt++) {
        if (HAL_I2C_Mem_Read(&s_i2c2, eeprom_dev_addr(addr), (uint16_t)(addr & 0xFFu),
                              I2C_MEMADD_SIZE_8BIT, buf, len,
                              EEPROM_XACT_TIMEOUT_MS) == HAL_OK) {
            s_fail_consec = 0;
            return true;
        }
        s_fail_total++;
        s_fail_consec++;
    }
    return false;
}

bool eeprom24c16_read(uint16_t addr, uint8_t *buf, uint16_t len)
{
    if (buf == NULL) return false;
    if ((uint32_t)addr + (uint32_t)len > EEPROM_SIZE_BYTES) return false;

    /* Chunk at 256-byte block boundaries: the device's internal word-address
     * counter is 8 bits and wraps WITHIN a block on its own, so a single
     * transaction may not cross a block boundary (§0.6 V7 "256-byte block
     * chunking"). */
    while (len > 0) {
        uint16_t block_off = (uint16_t)(addr % EEPROM_BLOCK_SIZE);
        uint16_t chunk = (uint16_t)(EEPROM_BLOCK_SIZE - block_off);
        if (chunk > len) chunk = len;

        if (!eeprom_read_chunk(addr, buf, chunk)) return false;

        addr = (uint16_t)(addr + chunk);
        buf += chunk;
        len = (uint16_t)(len - chunk);
    }
    return true;
}

static bool eeprom_write_page(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    for (uint32_t attempt = 0; attempt < EEPROM_XACT_ATTEMPTS; attempt++) {
        if (HAL_I2C_Mem_Write(&s_i2c2, eeprom_dev_addr(addr), (uint16_t)(addr & 0xFFu),
                               I2C_MEMADD_SIZE_8BIT, (uint8_t *)buf, len,
                               EEPROM_XACT_TIMEOUT_MS) == HAL_OK) {
            s_fail_consec = 0;
            HAL_Delay(EEPROM_WRITE_CYCLE_MS);   /* write-cycle wait, §0.6 V7 */
            return true;
        }
        s_fail_total++;
        s_fail_consec++;
    }
    return false;
}

bool eeprom24c16_write(uint16_t addr, const uint8_t *buf, uint16_t len)
{
    if (buf == NULL) return false;
    if ((uint32_t)addr + (uint32_t)len > EEPROM_SIZE_BYTES) return false;

    wp_write_enable(true);   /* /WP low -- write-enable, for the whole call */

    bool ok = true;
    while (len > 0) {
        /* Chunk at 16-byte page boundaries: writing across a page boundary in
         * one transaction wraps the on-chip page write buffer instead of
         * advancing to the next page (§0.6 V7 "16-byte page programming"). */
        uint16_t page_off = (uint16_t)(addr % EEPROM_PAGE_SIZE);
        uint16_t chunk = (uint16_t)(EEPROM_PAGE_SIZE - page_off);
        if (chunk > len) chunk = len;

        if (!eeprom_write_page(addr, buf, chunk)) { ok = false; break; }

        addr = (uint16_t)(addr + chunk);
        buf += chunk;
        len = (uint16_t)(len - chunk);
    }

    wp_write_enable(false);  /* /WP high -- protect again */
    return ok;
}

bool     eeprom24c16_ok(void)            { return s_fail_consec < EEPROM_FAIL_LIMIT; }
uint32_t eeprom24c16_xact_failures(void) { return s_fail_total; }
