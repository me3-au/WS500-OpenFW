/*
 * dio.h — DIP switches, enable/feature input, and status outputs.
 *
 * DIP bank (PA4..PA7 + PB0..PB2, all pull-up, §0.6 V1): read once at boot
 * (trusted immediately — these are static battery-capacity-code switches, not
 * expected to move mid-run) and re-polled + debounced afterward in case of a
 * mid-run change. PB13 (enable/feature input; pin/role class confirmed by the
 * hardware spec, exact Enable-vs-Feature-In split still bench-pending) is
 * debounced the same way.
 *
 * Outputs are named dio_out_a/dio_out_b (NOT "lamp"/"led") because which
 * physical signal is which is unresolved until M2 bench work (IO_COVERAGE:
 * "Which output = Lamp vs LED -> bench"):
 *   dio_out_a = PA9 (board.h OUT_LAMP_OR_LED_PIN, the "busiest" GPIOA status pin)
 *   dio_out_b = PB14 (board.h STATUS_OUT_B_PINS)
 * PA0 (formerly grouped with these two plus PA15 in board.h's STATUS_OUT_A_PINS)
 * is deliberately left unclaimed here: PA15 turned out to be the EEPROM /WP
 * line (§0.6 V7, not a status output), and with that removed there is no
 * remaining fact tying PA0 specifically to either output channel — bundling it
 * with PA9 without evidence would be a guess. Bench work should resolve PA0's
 * role before any driver claims it.
 *
 * Debounce: a pure main-loop-tick integrator, no timers — call dio_poll() once
 * per main-loop tick (10 ms, matching main.c's LOOP_PERIOD_MS).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_DIO_H
#define WS500_DIO_H

#include <stdint.h>
#include <stdbool.h>

void dio_init(void);   /* boot-time DIP read (trusted immediately) + outputs fail-safe-off */
void dio_poll(void);   /* call once per main-loop tick -- debounces inputs */

/* Battery-capacity DIP bank, 7 bits: bit0=PA4, bit1=PA5, bit2=PA6, bit3=PA7,
 * bit4=PB0, bit5=PB1, bit6=PB2 (board.h comment; §0.6 V1). Raw pin levels
 * (bit=1 means the pin reads HIGH) -- the active-high/low switch-closure
 * convention is a config-schema decision (GH#28), not asserted here. */
uint8_t dio_dip_value(void);          /* current debounced value */
uint8_t dio_dip_boot_value(void);     /* value latched at dio_init(), undebounced */

bool dio_ctrl_in(void);               /* debounced PB13 (enable/feature), raw pin level */

void dio_set_out_a(bool on);          /* PA9 */
void dio_set_out_b(bool on);          /* PB14 */

#endif /* WS500_DIO_H */
