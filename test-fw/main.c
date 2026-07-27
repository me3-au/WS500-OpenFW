/*
 * main.c — WS500-OpenFW bring-up test firmware (test-fw, deliverable #14,
 * PROJECT_PLAN Sec4 / M2).
 *
 * SPDX-License-Identifier: MIT
 *
 * ============================================================================
 *  BENCH USE ONLY. DUMMY LOAD ONLY. READ PROJECT_PLAN.md Sec5 BEFORE FLASHING.
 * ============================================================================
 * There is exactly ONE WS500 in this project and it is installed and LIVE on
 * a 48V system with a 4-ohm (12V-class) rotor (PROJECT_PLAN Sec5
 * "installed-unit reality"). This firmware exists to confirm every I/O on the
 * BENCH, with:
 *   - a current-limited bench supply (13.2V, start <=1A) -- never a raw
 *     battery, and never the installed unit;
 *   - a dummy field load (power resistor, ~10 ohm, >=50W) -- never a rotor
 *     coil directly, until the loop + fault paths are proven on the bench;
 *   - the field PWM path capped at 20% duty, COMPILED IN and structurally
 *     unexceedable -- see field_guard.c, the ONLY module in this firmware
 *     permitted to call field_drive_set() with a nonzero duty. field_drive.c
 *     itself (shared with the production firmware) still allows the full
 *     0-100% range, because the real regulator needs it; the cap here is a
 *     test-fw-only policy layered on top, not a change to that shared driver.
 * If this firmware is ever run against the live installed unit rather than a
 * dedicated bench unit, the full PROJECT_PLAN Sec5 access ladder (readings ->
 * config -> bench flash) still applies -- this firmware does not grant an
 * exception to it.
 *
 * Shares Core/Src/board.c and the production drivers (field_drive.c,
 * safe_state.c, sensors.c, ina2xx.c, eeprom24c16.c, stator_rpm.c, dio.c,
 * usb_cdc.c) and the Sec7-style robustness trio (safe_state / crash_record /
 * fault_handlers / watchdog) rather than duplicating any of it. test-fw's own
 * code is the console (console.c), the field-command safety funnel
 * (field_guard.c), and the two bench self-tests (can_test.c, i2c_scan.c).
 * Deliberately NOT shared: can_n2k.c (the full NMEA2000/RV-C stack, which
 * needs a control/telemetry snapshot this bring-up tool has no reason to
 * carry) and the config/telemetry pipeline (config_protocol.c,
 * cfg_stream.c, telem_stream.c) -- test-fw's console IS the operator
 * interface, it does not need the JSON config wire underneath it.
 *
 * Never a bare while(1) (PROJECT_PLAN Sec7 R3): the main loop below is a
 * for(;;) that does bounded work and returns to the top every iteration
 * (the same idiom Core/Src/main.c uses), and every abnormal exit from normal
 * control flow (HardFault, NMI, an unclaimed IRQ, an assert) is funnelled
 * through fault_handlers.c -> enter_safe_state() -> crash_record_write() ->
 * NVIC_SystemReset(), exactly as in the production firmware.
 */
#include "board.h"
#include "field_drive.h"
#include "field_guard.h"
#include "sensors.h"
#include "eeprom24c16.h"
#include "stator_rpm.h"
#include "dio.h"
#include "usb_cdc.h"
#include "crash_record.h"
#include "watchdog.h"
#include "console.h"
#include "can_test.h"
#include "i2c_scan.h"

#include <stdint.h>
#include <stdbool.h>

#define LOOP_PERIOD_MS  10U

int main(void)
{
    /* FIRST, before anything can fault: decode why we rebooted and validate
     * the `.noinit` crash-record block (Sec7 R2) -- mirrors Core/Src/main.c. */
    crash_record_boot();

    HAL_Init();
    board_init();

    field_drive_init();   /* TIM1 PWM, starts at 0% -- fail-safe (field_drive.c) */
    field_guard_init();   /* belt-and-braces: re-assert field-off explicitly
                           * through THIS firmware's own safety funnel too */

    sensors_init();        /* ADC1 7-ch scan (DMA) + INA226 bring-up (I2C1) */
    eeprom24c16_init();    /* I2C2 config-EEPROM bring-up (raw driver only) */
    stator_rpm_init();     /* PA10 EXTI + TIM2 timebase */
    dio_init();             /* DIP boot-read + PB13 + status outputs fail-safe off */
    usb_cdc_init();         /* CDC-ACM console transport; non-blocking by
                             * contract even with no host attached (usb_cdc.h) */

    console_init();
    can_test_init();
    i2c_scan_init();

    uint32_t next    = HAL_GetTick();
    bool     link_was_up = false;

    /* LAST init step, same reasoning as Core/Src/main.c: the IWDG cannot be
     * stopped once started, and the first kick needs a full loop iteration's
     * worth of checkpoints to have already reported. */
    watchdog_init();

    for (;;) {
        console_poll();               /* byte pump + command dispatch, every
                                       * iteration -- never gated to the 10ms
                                       * tick, matching production main.c's
                                       * treatment of comms work */
        watchdog_checkpoint(WDG_CP_COMMS);

        /* Console-disconnect safety: cut the field the instant DTR drops,
         * rather than waiting out field_guard's self-expiry timeout -- the
         * task brief calls this out by name ("a USB cable that falls out
         * must not leave the field energized"). Edge-detected so this only
         * fires once per disconnect, not every iteration the link is down. */
        const bool link_up = usb_cdc_link_up();
        if (link_was_up && !link_up) field_guard_link_lost();
        /* Rising edge: the boot banner was written into a void (the host had
         * not enumerated us yet, and the CDC transport drops when unconfigured
         * rather than blocking), so re-print it now -- this is the first
         * instant a terminal can actually receive the Sec5 safety notice. */
        if (!link_was_up && link_up) console_greet();
        link_was_up = link_up;

        if ((int32_t)(HAL_GetTick() - next) >= 0) {
            next += LOOP_PERIOD_MS;

            sensors_update();
            stator_rpm_update();
            dio_poll();
            watchdog_checkpoint(WDG_CP_SENSORS);

            field_guard_poll();          /* self-expiring field command, Sec5 */
            const bool can_done = can_test_poll();
            const bool i2c_done = i2c_scan_poll();
            if (can_done) console_report_can_result();
            if (i2c_done) console_report_i2c_result();
            watchdog_checkpoint(WDG_CP_CONTROL);   /* re-purposed for test-fw:
                                        * "the periodic safety/test-servicing
                                        * work for this cycle completed", the
                                        * closest test-fw analogue to
                                        * production main.c's "command reached
                                        * the hardware" checkpoint */

            /* Only kicks if every checkpoint above reported inside its
             * budget (Sec7 R1) -- a hang anywhere in this firmware, including
             * in test-fw's own new code, resets the part rather than leaving
             * whatever duty was last commanded running unsupervised. */
            watchdog_service();
        }
    }
}

void SysTick_Handler(void) { HAL_IncTick(); }
