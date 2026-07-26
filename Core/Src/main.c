/*
 * main.c — WS500-OpenFW app orchestrator.
 *
 * Wires drivers (hardware) to the pure control core: read sensors → assemble the
 * control input → run the engine at a fixed rate → apply the field command →
 * pump comms. The engine (control/) never touches hardware; this layer does.
 *
 * It is also where the PROJECT_PLAN §7 robustness layer is wired to the rest of
 * the firmware, because this is the only file allowed to see both sides:
 *   - §7 R1: subsystem checkpoints + the watchdog service call (the IWDG is
 *     kicked from here and nowhere else, and only when the loop did its work);
 *   - §7 R2: the regulator snapshot every crash record carries;
 *   - §7 R4: the stack high-watermark scan in housekeeping;
 *   - §7 R1/R6: translating driver-layer escalations into the control core's
 *     EXISTING ext_faults bits, so nothing in control/ has to change.
 *
 * SPDX-License-Identifier: MIT
 */
#include "board.h"
#include "field_drive.h"
#include "sensors.h"
#include "ina2xx.h"
#include "eeprom24c16.h"
#include "stator_rpm.h"
#include "dio.h"
#include "control.h"
#include "limits.h"
#include "thermal.h"
#include "telemetry.h"
#include "config_protocol.h"
#include "can_n2k.h"
#include "crash_record.h"
#include "watchdog.h"
#include "integrity.h"
#include "pvd.h"
#include "err_budget.h"
#include <math.h>

#define LOOP_PERIOD_MS  10U

/*
 * Driver-layer escalations -> the control core's existing fault bits.
 *
 * This mapping is the whole reason §7's robustness layer needs no change to
 * control/: the engine already knows how to degrade for a shunt it cannot
 * believe, a lost external source, or a watchdog event, and those dispositions
 * (LIMP vs field-open) are fixed in firmware by design (control/faults.h).
 * Inventing a parallel safe-mode in the driver layer would give the regulator
 * two authorities on the same question.
 */
static uint32_t app_ext_faults(void)
{
    uint32_t f = 0u;

    /* §7 R1 escalation: three consecutive watchdog-/fault-caused boots. The
     * engine's disposition for CTRL_FAULT_WATCHDOG is field-open, which IS
     * "boot into field-off LIMP and stay there". */
    if (watchdog_limp_latched()) f |= CTRL_FAULT_WATCHDOG;

    /* §7 R6: the INA226 is the only current source. Once its budget is spent
     * the readings are already NAN; this tells the engine WHY, so it takes the
     * spec'd LIMP path instead of merely seeing a missing signal. */
    if (errb_faulted(ERRB_INA226)) f |= CTRL_FAULT_IMPLAUSIBLE_SHUNT;

    /* §7 R6: a CAN link that keeps going bus-off means BMS/DVCC ceilings stop
     * arriving — the engine's LOST_BMS disposition (LIMP) is the right one. */
    if (errb_faulted(ERRB_CAN)) f |= CTRL_FAULT_LOST_BMS;

    /* ERRB_EEPROM deliberately maps to NO control fault: the config it holds is
     * already resident in RAM, so a dead config store cannot make charging
     * unsafe — it only makes changes unsaveable. That is a diagnostic, not a
     * charging fault. TODO(GH#27): surface it through telemetry (#20).
     * Likewise integrity_crc_status(): a failed image CRC is reported, not
     * acted on — see integrity.c for why. */
    return f;
}

int main(void)
{
    /* FIRST, before anything can fault or clear a flag: decode why we rebooted
     * and validate the `.noinit` block (§7 R2). Everything else in the §7 layer
     * depends on this having run. */
    crash_record_boot();

    /* Paint the stack while it is still shallow, and CRC the image (§7 R4). */
    integrity_init();

    HAL_Init();
    board_init();

    /* Brown-out warning (§7 R5) as early as the PWR clock allows — a supply
     * collapse during the rest of init must still land in the safe state. */
    pvd_init();

    field_drive_init();     /* starts at 0% — fail-safe */
    sensors_init();
    eeprom24c16_init();     /* I2C2 config store bring-up; RAW driver only, no
                             * record format yet -- config_protocol.c will
                             * read/write it once the schema lands (GH#28) */
    stator_rpm_init();      /* PA10 EXTI + TIM2 timebase */
    dio_init();              /* DIP boot-read + PB13 + status outputs */
    config_init();
    can_n2k_init();

    ctrl_t         ctrl;    ctrl_init(&ctrl);
    ctrl_globals_t g;
    ctrl_profile_t prof;

    ctrl_thermal_cfg_t thcfg; config_get_thermal(&thcfg);
    ctrl_thermal_t     thermal; ctrl_thermal_init(&thermal, &thcfg);

    const float dt_s = (float)LOOP_PERIOD_MS / 1000.0f;
    uint32_t next = HAL_GetTick();
    uint32_t tick_count = 0u;

    /* LAST init step (§7 R1): the IWDG cannot be stopped once started, and the
     * first kick needs one full loop iteration's worth of checkpoints, so it is
     * armed only when everything it watches actually exists.
     * TODO(GH#27): init-phase hangs (before this point) are therefore covered
     * by the fault handlers and the PVD, but not by the watchdog. Closing that
     * gap needs the WDG_SW option byte (hardware-started IWDG), which §7 R1
     * defers because it complicates bench debugging. */
    watchdog_init();

    for (;;) {
        can_n2k_poll();
        config_poll();
        /* Comms checkpoint: pumped every iteration, not just every 10 ms. */
        watchdog_checkpoint(WDG_CP_COMMS);

        if ((int32_t)(HAL_GetTick() - next) >= 0) {
            next += LOOP_PERIOD_MS;
            tick_count++;

            sensors_update();
            stator_rpm_update();
            dio_poll();
            sensor_readings_t r;
            sensors_read(&r);
            config_get(&g, &prof);
            watchdog_checkpoint(WDG_CP_SENSORS);

            /* RPM: stator_rpm_rpm() is NAN whenever poles/pulley_ratio aren't
             * configured yet (GH#28) OR the signal itself is lost/implausible
             * -- either way that must show up as rpm_state != VALID, never as
             * a VALID state paired with a NaN value (a NaN silently fails
             * downstream ">" comparisons like overspeed, which would be
             * unsafe). Real signal freshness (stator_rpm_state()) is only
             * consulted once rpm is confirmed to be a real number. */
            const float stator_rpm_val = stator_rpm_rpm();
            ctrl_rpm_state_t rpm_state;
            if (isnan(stator_rpm_val)) {
                rpm_state = CTRL_RPM_LOST;
            } else {
                switch (stator_rpm_state()) {
                    case STATOR_RPM_FRESH: rpm_state = CTRL_RPM_VALID; break;
                    case STATOR_RPM_STALE: rpm_state = CTRL_RPM_STALE; break;
                    default:                rpm_state = CTRL_RPM_LOST;  break;
                }
            }

            /* Assemble the control input from physical readings. Signals from
             * not-yet-implemented drivers use conservative fail-safe placeholders. */
            ctrl_measured_t m = {
                .vbat_pack_v  = r.vbat_pack_v,
                .vcomp_pack_v = r.vbat_pack_v,          /* no R model yet */
                .amps_batt    = r.amps_batt,
                .watts_batt   = ina2xx_power_w(),
                .isrc         = CTRL_ISRC_NONE,         /* TODO: from ShuntAtBat config */
                .v_supply_v   = r.vbat_pack_v,          /* TODO: measure field supply */
                .alt_hotspot_c= r.alt_temp_c,           /* no hot-spot model yet */
                .batt_temp_c  = r.batt_temp_c,
                .driver_temp_c= r.driver_temp_c,
                .rpm          = stator_rpm_val,
                .rpm_state    = rpm_state,
                .run_state    = CTRL_RUN_NOT_RUNNING,
                .soc_pct      = -1.0f,
                .soc_trusted  = false,
                /* PB13 is "Enable/Ignition OR Feature-In" (board.h/HW-spec;
                 * exact split bench-pending). Wired to ignition as the
                 * conservative interim choice -- "gates a control branch" is
                 * the first-listed interpretation, and without it the engine
                 * would have no enable source at all. Revisit once bench work
                 * (or a second, still-unidentified pin) resolves the split. */
                .ignition     = dio_ctrl_in(),
                .feature_in   = false,                  /* TODO: same PB13 ambiguity above */
                /* §7 R1/R6 escalations, mapped onto the engine's own fault
                 * bits (app_ext_faults above). TODO(GH#27): OR in BMS-sourced
                 * faults here too once CAN Rx lands. */
                .ext_faults   = app_ext_faults(),
            };

            /* Build arbitration ceilings: hardware limit set + thermal governor.
             * (BMS/engine/belt/capability ceilings land with their subsystems.) */
            ctrl_ceilings_t ceil = {
                .thermal_w = CTRL_CEILING_INACTIVE, .bms_ccl_w = CTRL_CEILING_INACTIVE,
                .battery_c_w = CTRL_CEILING_INACTIVE, .wiring_w = CTRL_CEILING_INACTIVE,
                .alt_absolute_w = CTRL_CEILING_INACTIVE, .alt_capability_w = CTRL_CEILING_INACTIVE,
                .belt_w = CTRL_CEILING_INACTIVE, .engine_w = CTRL_CEILING_INACTIVE,
                .user_cap_w = CTRL_CEILING_INACTIVE,
            };
            ctrl_limits_t lim; config_get_limits(&lim);
            ctrl_limits_apply(&ceil, &lim, &g, r.vbat_pack_v);
            ceil.thermal_w = ctrl_thermal_update(&thermal, &thcfg, r.alt_temp_c, dt_s);

            ctrl_command_t cmd = ctrl_tick(&ctrl, &m, &ceil, &prof, &g, LOOP_PERIOD_MS);
            if (cmd.field_open) field_drive_fault_cutoff();
            else                field_drive_set(cmd.field_duty);
            /* Control checkpoint: reported only AFTER the command reached the
             * hardware. "ctrl_tick returned" would be a weaker promise. */
            watchdog_checkpoint(WDG_CP_CONTROL);

            ctrl_telemetry_t tlm;
            ctrl_build_telemetry(&tlm, &m, &cmd, &g, &prof);
            can_n2k_publish(&tlm);   /* NMEA2000 out → Cerbo */

            /* §7 R2: keep the snapshot a crash record will carry current. */
            const crash_state_t snap = {
                .vbat_v        = r.vbat_pack_v,
                .amps_batt     = r.amps_batt,
                .field_duty    = cmd.field_duty,
                .ctrl_faults   = cmd.faults,
                .ext_faults    = m.ext_faults,
                .loop_count    = tick_count,
                .ctrl_state    = (uint8_t)cmd.state,
                .field_open    = cmd.field_open ? 1u : 0u,
                .field_latched = field_drive_faulted() ? 1u : 0u,
                .reserved      = 0u,
            };
            crash_record_set_state(&snap);

            /* §7 R4: advance the stack high-watermark scan (bounded work). */
            integrity_housekeeping();

            /* §7 R1: the ONLY IWDG kick in the firmware, and it only happens
             * if every checkpoint above reported inside its budget. */
            watchdog_service();
        }
    }
}

void SysTick_Handler(void) { HAL_IncTick(); }
/* Error_Handler() now lives in fault_handlers.c: §7 R3 forbids the old
 * `field_drive_off(); for(;;){}` — a spin, even with the field off, leaves a
 * dead regulator that no watchdog escalation can explain. It funnels through
 * enter_safe_state() + a crash record + a reset like every other fault. */
