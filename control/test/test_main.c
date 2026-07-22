/*
 * test_main.c — control-core unit-test runner (host, native gcc).
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"

int g_checks = 0;
int g_fails  = 0;

void test_arbitration(void);
void test_field(void);

int main(void)
{
    test_arbitration();
    test_field();

    printf("\n%d checks, %d failures\n", g_checks, g_fails);
    return g_fails ? 1 : 0;
}
