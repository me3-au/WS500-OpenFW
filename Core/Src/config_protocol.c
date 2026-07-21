/*
 * config_protocol.c — stub. Parse the public "$XXX:" config protocol here and map
 * fields onto reg_config_t. Defaults below are safe placeholders, NOT tuned values.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "config_protocol.h"

static reg_config_t s_cfg;

void config_init(void)
{
    /* TODO: read persisted config from flash; open USB CDC. Placeholder defaults: */
    s_cfg.sys_voltage     = 12.0f;
    s_cfg.v_absorption    = 14.4f;
    s_cfg.v_float         = 13.5f;
    s_cfg.amp_limit       = 0.0f;    /* 0 = no current limit until configured */
    s_cfg.ramp_rate       = 0.01f;
    s_cfg.temp_comp_per_c = 0.0f;
    s_cfg.alt_temp_limit_c= 105.0f;
    s_cfg.field_max       = 1.0f;
}

void config_poll(void) { /* TODO: read USB CDC lines, dispatch "$CMD:args". */ }

void config_get(reg_config_t *out) { *out = s_cfg; }
