/*
 * ina2xx.c — INA226/228/238 driver stub. Register map + address confirmed from
 * the stock firmware (see board.h INA_* / docs/WS500_HARDWARE_SPEC.md §6c).
 * Implement the I2C transfers against I2C1 (PB6/PB7) and the per-variant LSB math.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "ina2xx.h"
#include "board.h"

static ina_variant_t s_variant;
static float s_current_lsb;   /* A per current-register LSB */

/* TODO: implement 16-bit register read/write over I2C1 (HAL_I2C_Mem_Read/Write,
 * DevAddress = INA_I2C_ADDR7<<1, MemAddSize = 1 byte, big-endian data). */
static uint16_t ina_read(uint8_t reg) { (void)reg; return 0; }
static void     ina_write(uint8_t reg, uint16_t val) { (void)reg; (void)val; }

bool ina2xx_init(float shunt_fs_a, float shunt_fs_mv)
{
    (void)shunt_fs_mv;
    uint16_t manuf = ina_read(INA_REG_MANUF_ID);   /* expect 0x5449 'TI' */
    uint16_t die   = ina_read(INA_REG_DIE_ID);
    switch (die) {
        case INA_DIEID_INA226: s_variant = INA226; break;
        case INA_DIEID_INA228: s_variant = INA228; break;
        case INA_DIEID_INA238: s_variant = INA238; break;
        default: s_variant = INA_NONE; break;
    }
    if (manuf != 0x5449 || s_variant == INA_NONE) return false;

    /* Current LSB = max_expected_current / 2^15. TODO: set CALIBRATION register
     * per the datasheet (variant-specific shunt-voltage LSB) and shunt value. */
    s_current_lsb = shunt_fs_a / 32768.0f;
    ina_write(INA_REG_CONFIG, 0x0000 /* TODO averaging/conv-time */);
    ina_write(INA_REG_CALIBRATION, 0x0000 /* TODO from shunt */);
    return true;
}

ina_variant_t ina2xx_variant(void) { return s_variant; }

float ina2xx_current_a(void)
{
    int16_t raw = (int16_t)ina_read(INA_REG_CURRENT);
    return raw * s_current_lsb;
}
float ina2xx_bus_v(void)
{
    uint16_t raw = ina_read(INA_REG_BUS_V);
    /* TODO: apply variant BUS_V LSB (INA226 1.25mV; INA228/238 differ). */
    return raw * 0.00125f;
}
float ina2xx_power_w(void) { return (float)ina_read(INA_REG_POWER); /* TODO LSB */ }
