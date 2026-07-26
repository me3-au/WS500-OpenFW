/*
 * dio.c — DIP switches + enable/feature input (debounced) + status outputs.
 * GPIO modes for all these pins are configured centrally in board_init(); this
 * file only reads/writes pin state and does the debounce bookkeeping.
 * SPDX-License-Identifier: MIT
 */
#include "dio.h"
#include "board.h"

/* Debounce integrator depth: consecutive stable 10 ms polls required before a
 * change is accepted. Firmware policy (not a hardware fact) — generous enough
 * to ride out ordinary mechanical switch bounce (typically well under 20 ms)
 * at a 10 ms poll tick; retune once real hardware bounce is characterized. */
#define DIO_DEBOUNCE_TICKS   5u

static uint8_t s_dip_boot;
static uint8_t s_dip_debounced;
static uint8_t s_dip_candidate;
static uint8_t s_dip_stable_count;

static bool    s_ctrlin_debounced;
static bool    s_ctrlin_candidate;
static uint8_t s_ctrlin_stable_count;

/* Packs the 7-bit DIP bank: bit0=PA4 .. bit3=PA7, bit4=PB0 .. bit6=PB2
 * (board.h comment; §0.6 V1). */
static uint8_t read_dip_raw(void)
{
    uint8_t v = 0;
    if (HAL_GPIO_ReadPin(DIP_PORT, GPIO_PIN_4) == GPIO_PIN_SET) v |= (1u << 0);
    if (HAL_GPIO_ReadPin(DIP_PORT, GPIO_PIN_5) == GPIO_PIN_SET) v |= (1u << 1);
    if (HAL_GPIO_ReadPin(DIP_PORT, GPIO_PIN_6) == GPIO_PIN_SET) v |= (1u << 2);
    if (HAL_GPIO_ReadPin(DIP_PORT, GPIO_PIN_7) == GPIO_PIN_SET) v |= (1u << 3);
    if (HAL_GPIO_ReadPin(DIP_EXTRA_PORT, GPIO_PIN_0) == GPIO_PIN_SET) v |= (1u << 4);
    if (HAL_GPIO_ReadPin(DIP_EXTRA_PORT, GPIO_PIN_1) == GPIO_PIN_SET) v |= (1u << 5);
    if (HAL_GPIO_ReadPin(DIP_EXTRA_PORT, GPIO_PIN_2) == GPIO_PIN_SET) v |= (1u << 6);
    return v;
}

void dio_init(void)
{
    /* Boot read is trusted immediately: DIP switches are static hardware
     * configuration, not expected to change mid-run (board.h / §0.6 V1). */
    s_dip_boot         = read_dip_raw();
    s_dip_debounced     = s_dip_boot;
    s_dip_candidate     = s_dip_boot;
    s_dip_stable_count  = 0;

    s_ctrlin_debounced    = (HAL_GPIO_ReadPin(CTRL_IN_PORT, CTRL_IN_PIN) == GPIO_PIN_SET);
    s_ctrlin_candidate    = s_ctrlin_debounced;
    s_ctrlin_stable_count = 0;

    /* Outputs fail-safe to off — same convention as field_drive's start-at-0%. */
    dio_set_out_a(false);
    dio_set_out_b(false);
}

void dio_poll(void)
{
    /* DIP bank: integrator debounce on the whole packed value. In practice the
     * DIP switches change rarely and (being physically adjacent) together;
     * per-bit debounce state would add complexity with no evidenced benefit. */
    uint8_t raw = read_dip_raw();
    if (raw == s_dip_candidate) {
        if (s_dip_stable_count < DIO_DEBOUNCE_TICKS) s_dip_stable_count++;
        else s_dip_debounced = s_dip_candidate;
    } else {
        s_dip_candidate    = raw;
        s_dip_stable_count = 0;
    }

    bool ctrl_raw = (HAL_GPIO_ReadPin(CTRL_IN_PORT, CTRL_IN_PIN) == GPIO_PIN_SET);
    if (ctrl_raw == s_ctrlin_candidate) {
        if (s_ctrlin_stable_count < DIO_DEBOUNCE_TICKS) s_ctrlin_stable_count++;
        else s_ctrlin_debounced = s_ctrlin_candidate;
    } else {
        s_ctrlin_candidate    = ctrl_raw;
        s_ctrlin_stable_count = 0;
    }
}

uint8_t dio_dip_value(void)      { return s_dip_debounced; }
uint8_t dio_dip_boot_value(void) { return s_dip_boot; }
bool    dio_ctrl_in(void)        { return s_ctrlin_debounced; }

void dio_set_out_a(bool on)
{
    HAL_GPIO_WritePin(GPIOA, OUT_LAMP_OR_LED_PIN, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}

void dio_set_out_b(bool on)
{
    HAL_GPIO_WritePin(GPIOB, STATUS_OUT_B_PINS, on ? GPIO_PIN_SET : GPIO_PIN_RESET);
}
