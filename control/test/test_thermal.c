/*
 * test_thermal.c — §4 predictive governor: prediction pulls the ceiling early,
 * hard limit floors, missing sensor → inactive.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "test.h"
#include "thermal.h"

static ctrl_thermal_cfg_t CFG(void)
{
    ctrl_thermal_cfg_t c = {0};
    c.tau_s = 300.0f;
    c.target_c = 95.0f;
    c.hard_c = 110.0f;
    c.derate_floor_w = 100.0f;
    c.ceiling_max_w = 3000.0f;
    c.gain_w_per_c_s = 50.0f;
    return c;
}

void test_thermal(void)
{
    ctrl_thermal_cfg_t cfg = CFG();

    /* Missing sensor → no thermal ceiling. */
    {
        ctrl_thermal_t th; ctrl_thermal_init(&th, &cfg);
        CHECK(isinf(ctrl_thermal_update(&th, &cfg, NAN, 1.0f)));
    }

    /* Steady at 90 °C (below target, headroom) → ceiling holds at max. */
    ctrl_thermal_t a; ctrl_thermal_init(&a, &cfg);
    float ca = 0.0f;
    for (int i = 0; i < 60; i++) ca = ctrl_thermal_update(&a, &cfg, 90.0f, 1.0f);
    CHECK_FEQ(ca, 3000.0f, 1.0f);

    /* Heating through 90 °C at +0.05 °C/s → T_proj ≈ 105 > target → ceiling derates
     * *before* the hot-spot reaches target (the predictive win). */
    ctrl_thermal_t b; ctrl_thermal_init(&b, &cfg);
    float cb = 0.0f, t = 90.0f;
    for (int i = 0; i < 60; i++) { cb = ctrl_thermal_update(&b, &cfg, t, 1.0f); t += 0.05f; }
    CHECK(cb < ca);            /* prediction pulled B down while A (steady) stayed high */
    CHECK(cb < 3000.0f);

    /* Hard limit → floor immediately. */
    ctrl_thermal_t c; ctrl_thermal_init(&c, &cfg);
    float cc = 0.0f;
    for (int i = 0; i < 5; i++) cc = ctrl_thermal_update(&c, &cfg, 115.0f, 1.0f);
    CHECK_FEQ(cc, 100.0f, 0.1f);
}
