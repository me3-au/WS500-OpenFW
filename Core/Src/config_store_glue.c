/*
 * config_store_glue.c — binds the pure config store to the 24C16 driver.
 *
 * eeprom24c16_read/write already have exactly the signatures cfg_store_t wants
 * (uint16_t addr, buffer, uint16_t len -> bool), so the binding is a straight
 * hand-off with no shim logic — deliberately: any policy added here would be
 * policy that the native tests in control/test/test_config.c cannot see.
 *
 * SPDX-License-Identifier: MIT
 */
#include "config_store_glue.h"
#include "eeprom24c16.h"

static cfg_store_t s_store;
static bool        s_bound;

/* Lazy bind so call order with eeprom24c16_init() cannot matter: the driver is
 * only touched when a load/save actually runs. */
static cfg_store_t *store(void)
{
    if (!s_bound) {
        cfg_store_init(&s_store, eeprom24c16_read, eeprom24c16_write);
        s_bound = true;
    }
    return &s_store;
}

cfg_store_status_t config_store_load(ctrl_config_t *out)
{
    return cfg_store_load(store(), out);
}

cfg_store_status_t config_store_save(const ctrl_config_t *cfg)
{
    return cfg_store_save(store(), cfg);
}

uint32_t config_store_generation(void)
{
    return store()->generation;
}
