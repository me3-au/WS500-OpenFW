/*
 * config_store_glue.h — binds the pure config store to the 24C16 driver.
 *
 * The whole slot/generation/verify policy lives in control/Src/config_store.c
 * (HAL-free, natively tested). This file is the ONLY place that names the
 * hardware: it hands cfg_store_t the eeprom24c16 read/write entry points and
 * owns the single store instance. Nothing else in Core/ should reach past it.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_CONFIG_STORE_GLUE_H
#define WS500_CONFIG_STORE_GLUE_H

#include "config_store.h"

/* Newest valid record from the EEPROM. CFG_STORE_EMPTY on a blank/corrupt part
 * is normal on first boot — the caller keeps its compiled defaults. */
cfg_store_status_t config_store_load(ctrl_config_t *out);

/* Persist to the standby slot (write + read-back verify). BLOCKS for tens of ms
 * — config-write paths only, never the 10 ms control loop. */
cfg_store_status_t config_store_save(const ctrl_config_t *cfg);

/* Generation of the record currently live (0 = none seen). */
uint32_t config_store_generation(void);

#endif /* WS500_CONFIG_STORE_GLUE_H */
