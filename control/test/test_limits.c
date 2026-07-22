/*
 * test_limits.c — hardware limit set → Watts ceilings (§2.1), + arbitration hook.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "limits.h"
#include "arbitration.h"

void test_limits(void)
{
    ctrl_globals_t g = {0};
    g.bank_capacity_ah = 100.0f;
    ctrl_limits_t lim = {0};
    ctrl_ceilings_t c = test_ceil_none();

    /* Everything unset → all inactive. */
    ctrl_limits_apply(&c, &lim, &g, 13.6f);
    CHECK(isinf(c.battery_c_w));
    CHECK(isinf(c.wiring_w));
    CHECK(isinf(c.alt_absolute_w));

    /* Battery C 0.5 × 100 Ah × 13.6 V = 680 W. */
    lim.battery_c_limit = 0.5f;
    ctrl_limits_apply(&c, &lim, &g, 13.6f);
    CHECK_FEQ(c.battery_c_w, 680.0f, 0.1f);

    /* Wiring 250 A × 13.6 V = 3400 W. */
    lim.wiring_limit_a = 250.0f;
    ctrl_limits_apply(&c, &lim, &g, 13.6f);
    CHECK_FEQ(c.wiring_w, 3400.0f, 0.1f);

    /* Alternator 220 A × 13.6 V = 2992 W. */
    lim.alternator_limit_a = 220.0f;
    ctrl_limits_apply(&c, &lim, &g, 13.6f);
    CHECK_FEQ(c.alt_absolute_w, 2992.0f, 0.1f);

    /* Non-positive voltage → all inactive (fail-safe). */
    ctrl_limits_apply(&c, &lim, &g, 0.0f);
    CHECK(isinf(c.battery_c_w));
    CHECK(isinf(c.wiring_w));
    CHECK(isinf(c.alt_absolute_w));

    /* Integration: battery-C (680 W) is the lowest → it binds against a 3000 W stage. */
    ctrl_limits_apply(&c, &lim, &g, 13.6f);
    ctrl_arb_t r = ctrl_arbitrate(3000.0f, &c);
    CHECK(r.src == CTRL_BIND_BATTERY_C);
    CHECK_FEQ(r.watts, 680.0f, 0.1f);
}
