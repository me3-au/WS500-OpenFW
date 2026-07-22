/*
 * test.h — minimal host unit-test harness for the pure control core.
 * No framework: compile control/Src + control/test with native gcc and run.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_TEST_H
#define WS500_TEST_H

#include <stdio.h>
#include <math.h>

extern int g_checks;
extern int g_fails;

#define CHECK(cond) do { \
    g_checks++; \
    if (!(cond)) { g_fails++; printf("FAIL %s:%d  %s\n", __FILE__, __LINE__, #cond); } \
} while (0)

#define CHECK_FEQ(a, b, eps) do { \
    g_checks++; \
    double _a = (double)(a), _b = (double)(b); \
    if (!(fabs(_a - _b) <= (eps))) { g_fails++; \
        printf("FAIL %s:%d  %s ~= %s  (%g vs %g)\n", __FILE__, __LINE__, #a, #b, _a, _b); } \
} while (0)

/* All-inactive ceilings helper (every ceiling = +inf). */
#include "control.h"
static inline ctrl_ceilings_t test_ceil_none(void)
{
    ctrl_ceilings_t c = {
        .thermal_w = CTRL_CEILING_INACTIVE, .bms_ccl_w = CTRL_CEILING_INACTIVE,
        .battery_c_w = CTRL_CEILING_INACTIVE, .wiring_w = CTRL_CEILING_INACTIVE,
        .alt_absolute_w = CTRL_CEILING_INACTIVE, .alt_capability_w = CTRL_CEILING_INACTIVE,
        .belt_w = CTRL_CEILING_INACTIVE, .engine_w = CTRL_CEILING_INACTIVE,
        .user_cap_w = CTRL_CEILING_INACTIVE,
    };
    return c;
}

#endif /* WS500_TEST_H */
