/*
 * test_nan_max.c — ctrl_nan_max2() NaN-tolerance (GH#43 regression).
 *
 * The regression that matters: main.c derives alt_hotspot_c (and the §4.1
 * governor input) from ctrl_nan_max2(alt_temp_c, alt_temp2_c) so that one
 * channel going NaN (e.g. batt_temp_src stealing it, GH#40) never blinds
 * alternator over-temp protection. This is the pure core of that fix — the
 * NaN-handling math — tested in isolation since main.c itself (Core/Src, HAL
 * side) has no native test harness.
 * SPDX-License-Identifier: MIT
 */
#include "test.h"
#include "control.h"

void test_nan_max(void)
{
    /* Both finite -> ordinary max, either argument order. */
    CHECK_FEQ(ctrl_nan_max2(60.0f, 90.0f), 90.0f, 0.001f);
    CHECK_FEQ(ctrl_nan_max2(90.0f, 60.0f), 90.0f, 0.001f);
    CHECK_FEQ(ctrl_nan_max2(60.0f, 60.0f), 60.0f, 0.001f);

    /* One channel NaN (a dropped/stolen sensor) -> the LIVE channel wins,
     * never poisoned by the NaN. This is the exact GH#43 scenario: one
     * alt-side ADC channel reassigned to batt_temp_src reads NaN for
     * alt_temp_c, and protection must still see the other. */
    CHECK_FEQ(ctrl_nan_max2(NAN, 130.0f), 130.0f, 0.001f);
    CHECK_FEQ(ctrl_nan_max2(130.0f, NAN), 130.0f, 0.001f);

    /* Negative / zero readings are ordinary values, not sentinels. */
    CHECK_FEQ(ctrl_nan_max2(-10.0f, 5.0f), 5.0f, 0.001f);
    CHECK_FEQ(ctrl_nan_max2(NAN, -10.0f), -10.0f, 0.001f);

    /* Both NaN (both channels lost) -> NaN, so the fault input's own
     * `!isnan()` gate correctly treats it as "no reading", not a phantom 0. */
    CHECK(isnan(ctrl_nan_max2(NAN, NAN)));
}
