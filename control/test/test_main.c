/*
 * test_main.c — control-core unit-test runner (host, native gcc).
 * SPDX-License-Identifier: GPL-3.0-or-later
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

int main(void)
{
    test_arbitration();
    test_field();
    test_statemachine();
    test_limits();
    test_faults();
    test_thermal();
    test_telemetry();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
