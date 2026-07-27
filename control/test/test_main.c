/*
 * test_main.c — control-core unit-test runner (host, native gcc).
 * SPDX-License-Identifier: MIT
 */
#include "test.h"

int g_checks = 0;
int g_fails  = 0;

void test_arbitration(void);
void test_field(void);
void test_statemachine(void);
void test_limits(void);
void test_faults(void);
void test_thermal(void);
void test_telemetry(void);
void test_telemetry_json(void); /* {"t":"telem"} line emitter (GH#35, #20) */
void test_n2k_encode(void); /* NMEA2000 PGN encoders (CAN_INTEGRATION.md §2, GH#18) */
void test_n2k_addrclaim(void); /* ISO 11783-81 address claim (GH#18) */
void test_n2k_sched(void);  /* NMEA2000 Tx cadence engine (GH#18) */
void test_config(void);     /* packed config record: codec / slots / validator */
void test_protocol(void);   /* JSON config wire: parser / §7 document / messages */
void test_property(void);   /* §8.4 sweep-based invariant + boundary tests */
void test_fuzz(void);       /* §8.4 randomized fault injection, fixed seeds */

int main(void)
{
    test_arbitration();
    test_field();
    test_statemachine();
    test_limits();
    test_faults();
    test_thermal();
    test_telemetry();
    test_telemetry_json();
    test_n2k_encode();
    test_n2k_addrclaim();
    test_n2k_sched();
    test_config();
    test_protocol();
    test_property();
    test_fuzz();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
