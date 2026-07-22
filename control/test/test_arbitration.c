/*
 * test_arbitration.c — CONTROL_SPEC §2 min()-of-ceilings behavior.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "arbitration.h"

void test_arbitration(void)
{
    ctrl_ceilings_t c = test_ceil_none();

    /* Nothing active → stage power binds. */
    ctrl_arb_t r = ctrl_arbitrate(3000.0f, &c);
    CHECK(r.src == CTRL_BIND_STAGE);
    CHECK_FEQ(r.watts, 3000.0f, 0.01f);

    /* Thermal below stage → thermal binds. */
    c.thermal_w = 1500.0f;
    r = ctrl_arbitrate(3000.0f, &c);
    CHECK(r.src == CTRL_BIND_THERMAL);
    CHECK_FEQ(r.watts, 1500.0f, 0.01f);

    /* BMS even lower → BMS binds. */
    c.bms_ccl_w = 800.0f;
    r = ctrl_arbitrate(3000.0f, &c);
    CHECK(r.src == CTRL_BIND_BMS);
    CHECK_FEQ(r.watts, 800.0f, 0.01f);

    /* A higher ceiling never displaces a lower one. */
    c.wiring_w = 2500.0f;
    r = ctrl_arbitrate(3000.0f, &c);
    CHECK(r.src == CTRL_BIND_BMS);
    CHECK_FEQ(r.watts, 800.0f, 0.01f);

    /* Stage itself lowest. */
    c = test_ceil_none();
    c.engine_w = 2000.0f;
    r = ctrl_arbitrate(500.0f, &c);
    CHECK(r.src == CTRL_BIND_STAGE);
    CHECK_FEQ(r.watts, 500.0f, 0.01f);

    /* Negative / NaN stage clamps to 0. */
    r = ctrl_arbitrate(-50.0f, &c);
    CHECK_FEQ(r.watts, 0.0f, 0.01f);
    r = ctrl_arbitrate(NAN, &c);
    CHECK_FEQ(r.watts, 0.0f, 0.01f);

    /* Every distinct ceiling can bind and reports its own source. */
    c = test_ceil_none(); c.belt_w = 123.0f;
    r = ctrl_arbitrate(9999.0f, &c);
    CHECK(r.src == CTRL_BIND_BELT);
    CHECK_FEQ(r.watts, 123.0f, 0.01f);
}
