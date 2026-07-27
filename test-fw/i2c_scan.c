/*
 * i2c_scan.c — I2C1 + I2C2 bus scan state machine. Contract: i2c_scan.h.
 *
 * TEMPORARY, SEPARATE HANDLES — why this does not reuse ina2xx.c's/
 * eeprom24c16.c's own I2C_HandleTypeDef instances: those are file-static
 * (deliberately private — this driver has no business poking their internal
 * state) and each owns its bus CONTINUOUSLY (the INA226 free-runs conversions
 * on I2C1; the EEPROM is idle-but-configured on I2C2). This scanner brings up
 * its OWN handles on the same physical peripherals, scans, then tears them
 * down again. That transiently disturbs whichever of those two drivers is
 * live on the bus being probed: their next transaction after the scan will
 * fail, because this scanner's HAL_I2C_DeInit() at the end of the scan resets
 * the peripheral registers those drivers' cached handles still point at.
 *
 * This is EXPECTED and SELF-HEALING, not a bug to route around: both
 * ina2xx.c and eeprom24c16.c already implement the PROJECT_PLAN §7 R6 error
 * budget (err_budget.c) specifically for "the bus went wrong somehow" — a
 * few consecutive failed transactions trigger their own ina_bus_reinit()/
 * eeprom_bus_reinit(), which re-configures the peripheral with THEIR config
 * and resumes. No data is at risk: a scan only issues zero-length
 * IsDeviceReady probes, never a real read or a page write, and the EEPROM's
 * /WP stays high (protected) throughout since eeprom24c16_write() is never
 * called here. The console prints a note about this every time a scan
 * completes (test-fw/console.c) so a bench operator watching INA226/EEPROM
 * telemetry glitch for a second during `i2cscan` isn't left wondering why.
 *
 * TIMING constants are the SAME numeric values ina2xx.c (I2C1, HSI 8 MHz) and
 * eeprom24c16.c (I2C2, PCLK1 48 MHz) already derived and documented for these
 * exact buses — copied here rather than re-derived a third time, since the
 * clock tree facts they're computed from (board_clock_config()) haven't
 * changed.
 *
 * SPDX-License-Identifier: MIT
 */
#include "i2c_scan.h"
#include "board.h"
#include "stm32f0xx_hal.h"
#include <string.h>

#define I2C_SCAN_ADDR_MIN   0x08u   /* below this is the reserved block
                                     * (general call, HS-mode, etc.) */
#define I2C_SCAN_ADDR_MAX   0x77u   /* above this is the reserved 0x78..0x7F
                                     * block (10-bit addressing, etc.) */

/* Copied from ina2xx.c: ST reference value for 100 kHz standard mode at
 * I2C1's 8 MHz HSI kernel clock (RM0091 / CubeMX). */
#define SCAN_I2C1_TIMING   0x2000090Eu
/* Copied from eeprom24c16.c: computed for I2C2's 48 MHz PCLK1 kernel clock,
 * ~90.9 kHz (under the 100 kHz Standard-mode cap with margin). */
#define SCAN_I2C2_TIMING   0xB0421317u

#define SCAN_XACT_TRIALS    1u   /* one shot per address: a real device ACKs
                                  * on the first try, and a bounded scan is
                                  * more useful than a slow, thorough one for
                                  * a bench "what's on this bus" check */
#define SCAN_XACT_TIMEOUT_MS 2u

typedef enum { SCAN_IDLE = 0, SCAN_RUNNING } scan_state_t;

static I2C_HandleTypeDef s_i2c1;
static I2C_HandleTypeDef s_i2c2;
static bool              s_i2c1_up;
static bool              s_i2c2_up;
static scan_state_t      s_state;
static uint8_t            s_addr;          /* next 7-bit address to probe */
static bool               s_found1[128];   /* indexed directly by addr7 */
static bool               s_found2[128];

static bool bring_up(I2C_HandleTypeDef *hi2c, I2C_TypeDef *inst, uint32_t timing)
{
    hi2c->Instance              = inst;
    hi2c->Init.Timing           = timing;
    hi2c->Init.OwnAddress1      = 0;
    hi2c->Init.AddressingMode   = I2C_ADDRESSINGMODE_7BIT;
    hi2c->Init.DualAddressMode  = I2C_DUALADDRESS_DISABLE;
    hi2c->Init.OwnAddress2      = 0;
    hi2c->Init.OwnAddress2Masks = I2C_OA2_NOMASK;
    hi2c->Init.GeneralCallMode  = I2C_GENERALCALL_DISABLE;
    hi2c->Init.NoStretchMode    = I2C_NOSTRETCH_DISABLE;
    return HAL_I2C_Init(hi2c) == HAL_OK;
}

void i2c_scan_init(void)
{
    s_state   = SCAN_IDLE;
    s_i2c1_up = false;
    s_i2c2_up = false;
    memset(s_found1, 0, sizeof s_found1);
    memset(s_found2, 0, sizeof s_found2);
}

bool i2c_scan_start(void)
{
    if (s_state == SCAN_RUNNING) return false;

    memset(s_found1, 0, sizeof s_found1);
    memset(s_found2, 0, sizeof s_found2);

    __HAL_RCC_I2C1_CLK_ENABLE();
    __HAL_RCC_I2C2_CLK_ENABLE();
    /* If one bus fails to configure the scan still proceeds on the other —
     * a dead I2C1 peripheral is itself a useful bench finding, not a reason
     * to abandon the I2C2 half of the report. */
    s_i2c1_up = bring_up(&s_i2c1, I2C1, SCAN_I2C1_TIMING);
    s_i2c2_up = bring_up(&s_i2c2, I2C2, SCAN_I2C2_TIMING);

    s_addr  = I2C_SCAN_ADDR_MIN;
    s_state = SCAN_RUNNING;
    return true;
}

bool i2c_scan_busy(void) { return s_state == SCAN_RUNNING; }

bool i2c_scan_poll(void)
{
    if (s_state != SCAN_RUNNING) return false;

    if (s_i2c1_up &&
        HAL_I2C_IsDeviceReady(&s_i2c1, (uint16_t)(s_addr << 1), SCAN_XACT_TRIALS,
                              SCAN_XACT_TIMEOUT_MS) == HAL_OK) {
        s_found1[s_addr] = true;
    }
    if (s_i2c2_up &&
        HAL_I2C_IsDeviceReady(&s_i2c2, (uint16_t)(s_addr << 1), SCAN_XACT_TRIALS,
                              SCAN_XACT_TIMEOUT_MS) == HAL_OK) {
        s_found2[s_addr] = true;
    }

    if (s_addr >= I2C_SCAN_ADDR_MAX) {
        if (s_i2c1_up) { (void)HAL_I2C_DeInit(&s_i2c1); s_i2c1_up = false; }
        if (s_i2c2_up) { (void)HAL_I2C_DeInit(&s_i2c2); s_i2c2_up = false; }
        s_state = SCAN_IDLE;
        return true;
    }
    s_addr++;
    return false;
}

bool i2c_scan_found(int bus, uint8_t addr7)
{
    if (addr7 >= 128u) return false;
    if (bus == 1) return s_found1[addr7];
    if (bus == 2) return s_found2[addr7];
    return false;
}
