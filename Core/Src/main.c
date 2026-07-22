/*
 * main.c — WS500-OpenFW app orchestrator.
 *
 * Wires drivers (hardware) to the pure control core: read sensors → assemble the
 * control input → run the engine at a fixed rate → apply the field command →
 * pump comms. The engine (control/) never touches hardware; this layer does.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "board.h"
#include "field_drive.h"
#include "sensors.h"
#include "ina2xx.h"
#include "control.h"
#include "limits.h"
#include "thermal.h"
#include "config_protocol.h"
#include "can_n2k.h"

#define LOOP_PERIOD_MS  10U

int main(void)
{
    HAL_Init();
    board_init();

    field_drive_init();     /* starts at 0% — fail-safe */
    sensors_init();
    config_init();
    can_n2k_init();

    ctrl_t         ctrl;    ctrl_init(&ctrl);
    ctrl_globals_t g;
    ctrl_profile_t prof;

    ctrl_thermal_cfg_t thcfg; config_get_thermal(&thcfg);
    ctrl_thermal_t     thermal; ctrl_thermal_init(&thermal, &thcfg);

    const float dt_s = (float)LOOP_PERIOD_MS / 1000.0f;
    uint32_t next = HAL_GetTick();
    for (;;) {
        can_n2k_poll();
        config_poll();

        if ((int32_t)(HAL_GetTick() - next) >= 0) {
            next += LOOP_PERIOD_MS;

            sensors_update();
            sensor_readings_t r;
            sensors_read(&r);
            config_get(&g, &prof);

            /* Assemble the control input from physical readings. Signals from
             * not-yet-implemented drivers use conservative fail-safe placeholders. */
            ctrl_measured_t m = {
                .vbat_pack_v  = r.vbat_pack_v,
                .vcomp_pack_v = r.vbat_pack_v,          /* no R model yet */
                .amps_batt    = r.amps_batt,
                .watts_batt   = ina2xx_power_w(),
                .isrc         = CTRL_ISRC_NONE,         /* TODO: from ShuntAtBat config */
                .v_supply_v   = r.vbat_pack_v,          /* TODO: measure field supply */
                .alt_hotspot_c= r.alt_temp_c,           /* no hot-spot model yet */
                .batt_temp_c  = r.batt_temp_c,
                .driver_temp_c= r.driver_temp_c,
                .rpm          = 0.0f,
                .rpm_state    = CTRL_RPM_LOST,          /* TODO: TIM2/CAN fusion */
                .run_state    = CTRL_RUN_NOT_RUNNING,
                .soc_pct      = -1.0f,
                .soc_trusted  = false,
                .ignition     = false,                  /* TODO: read PB13 (dio driver) */
                .feature_in   = false,
                .ext_faults   = 0u,                     /* TODO: OR in BMS/shunt faults */
            };

            /* Build arbitration ceilings: hardware limit set + thermal governor.
             * (BMS/engine/belt/capability ceilings land with their subsystems.) */
            ctrl_ceilings_t ceil = {
                .thermal_w = CTRL_CEILING_INACTIVE, .bms_ccl_w = CTRL_CEILING_INACTIVE,
                .battery_c_w = CTRL_CEILING_INACTIVE, .wiring_w = CTRL_CEILING_INACTIVE,
                .alt_absolute_w = CTRL_CEILING_INACTIVE, .alt_capability_w = CTRL_CEILING_INACTIVE,
                .belt_w = CTRL_CEILING_INACTIVE, .engine_w = CTRL_CEILING_INACTIVE,
                .user_cap_w = CTRL_CEILING_INACTIVE,
            };
            ctrl_limits_t lim; config_get_limits(&lim);
            ctrl_limits_apply(&ceil, &lim, &g, r.vbat_pack_v);
            ceil.thermal_w = ctrl_thermal_update(&thermal, &thcfg, r.alt_temp_c, dt_s);

            ctrl_command_t cmd = ctrl_tick(&ctrl, &m, &ceil, &prof, &g, LOOP_PERIOD_MS);
            if (cmd.field_open) field_drive_fault_cutoff();
            else                field_drive_set(cmd.field_duty);

            can_n2k_publish(&m, &ctrl);
        }
    }
}

void SysTick_Handler(void) { HAL_IncTick(); }
void Error_Handler(void)   { field_drive_off(); for (;;) { } }
