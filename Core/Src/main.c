/*
 * main.c — WS500-OpenFW entry point and top-level control loop.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "board.h"
#include "field_drive.h"
#include "sensors.h"
#include "regulator.h"
#include "config_protocol.h"
#include "can_n2k.h"

#define LOOP_PERIOD_MS  10U

int main(void)
{
    HAL_Init();
    board_init();

    field_drive_init();     /* starts at 0% — fail-safe */
    sensors_init();
    config_init();          /* $XXX: config over USB CDC (stub) */
    can_n2k_init();         /* NMEA2000 / RV-C (stub) */

    reg_state_t   reg;      regulator_init(&reg);
    reg_config_t  cfg;      config_get(&cfg);     /* from parsed $CPx/$SCx */
    reg_inputs_t  in;

    uint32_t next = HAL_GetTick();
    for (;;) {
        can_n2k_poll();
        config_poll();      /* handle USB CDC config traffic */

        if ((int32_t)(HAL_GetTick() - next) >= 0) {
            next += LOOP_PERIOD_MS;

            sensors_update();
            sensors_read(&in);
            config_get(&cfg);

            reg_output_t o = regulator_step(&reg, &in, &cfg, LOOP_PERIOD_MS);
            if (o.fault) field_drive_fault_cutoff();
            else         field_drive_set(o.field);

            can_n2k_publish(&in, &reg);   /* status PGNs */
        }
    }
}

void SysTick_Handler(void) { HAL_IncTick(); }
void Error_Handler(void)   { field_drive_off(); for (;;) { } }
