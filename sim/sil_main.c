/*
 * sil_main.c — §8.1 SIL gauntlet runner (host-native, no HAL).
 *
 * Build (CI / any native cc):
 *   gcc -std=c11 -O2 -Wall -Wextra -I control/Inc -I sim \
 *       control/Src/(star).c sim/(star).c -lm -o sil_tests && ./sil_tests
 *
 * Exit code 0 = every scenario green (the Stage-C flashing gate, PROJECT_PLAN
 * §5/§8). Deterministic: fixed seeds, no wall clock.
 *
 * SPDX-License-Identifier: MIT
 */
#include "../control/test/test.h"

int g_checks = 0;
int g_fails  = 0;

void scn_plant_calibration(void);
void scn_charge_rest_cycle(void);
void scn_rotor_clamp(void);
void scn_bms_steps(void);
void scn_rpm_transients(void);
void scn_temperature(void);
void scn_sensor_faults(void);
void scn_reference_trace(void);
void scn_stalled_rotor(void);
void scn_lying_vbat(void);
void scn_ah_revert(void);
void scn_long_soak(void);
void scn_driver_overtemp(void);  /* GH#39 */

int main(void)
{
    printf("WS500-OpenFW SIL gauntlet (PROJECT_PLAN §8.1)\n");
    printf("plant: 16S LFP / 48 V, 4 ohm 12 V rotor; tick %u ms\n\n", 10u);

    scn_plant_calibration();
    scn_charge_rest_cycle();
    scn_rotor_clamp();
    scn_bms_steps();
    scn_rpm_transients();
    scn_temperature();
    scn_sensor_faults();
    scn_reference_trace();
    scn_stalled_rotor();
    scn_lying_vbat();
    scn_ah_revert();
    scn_long_soak();
    scn_driver_overtemp();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
