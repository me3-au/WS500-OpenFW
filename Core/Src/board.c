/*
 * board.c — clocks + peripheral GPIO init to the recovered WS500 pin map.
 * SPDX-License-Identifier: MIT
 */
#include "board.h"

void board_clock_config(void)
{
    /* Crystal-less: HSI48 as SYSCLK, CRS trims it against USB SOF. 48 MHz. */
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    osc.HSI48State = RCC_HSI48_ON;
    osc.PLL.PLLState = RCC_PLL_NONE;
    HAL_RCC_OscConfig(&osc);

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                    RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource = RCC_SYSCLKSOURCE_HSI48;
    clk.AHBCLKDivider = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider = RCC_HCLK_DIV1;
    HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1);

    __HAL_RCC_CRS_CLK_ENABLE();
    RCC_CRSInitTypeDef crs = {0};
    crs.Prescaler = RCC_CRS_SYNC_DIV1;
    crs.Source = RCC_CRS_SYNC_SOURCE_USB;
    crs.Polarity = RCC_CRS_SYNC_POLARITY_RISING;
    crs.ReloadValue = __HAL_RCC_CRS_RELOADVALUE_CALCULATE(48000000, 1000);
    crs.ErrorLimitValue = RCC_CRS_ERRORLIMIT_DEFAULT;
    crs.HSI48CalibrationValue = RCC_CRS_HSI48CALIBRATION_DEFAULT;
    HAL_RCCEx_CRSConfig(&crs);
}

static void gpio_af(GPIO_TypeDef *port, uint32_t pins, uint32_t af, uint32_t otype)
{
    GPIO_InitTypeDef g = {0};
    g.Pin = pins;
    g.Mode = otype ? GPIO_MODE_AF_OD : GPIO_MODE_AF_PP;
    g.Pull = otype ? GPIO_PULLUP : GPIO_NOPULL;
    g.Speed = GPIO_SPEED_FREQ_HIGH;
    g.Alternate = af;
    HAL_GPIO_Init(port, &g);
}

void board_init(void)
{
    board_clock_config();

    /* LQFP64 (STM32F072RB) — only ports A/B/C exist; stock clocks the same set
     * (binary disasm 2026-07-23, PROJECT_PLAN §0.6 V1). */
    __HAL_RCC_GPIOA_CLK_ENABLE();
    __HAL_RCC_GPIOB_CLK_ENABLE();
    __HAL_RCC_GPIOC_CLK_ENABLE();

    /* Field PWM: PA8 TIM1_CH1, PB15 TIM1_CH3N (AF2, push-pull). */
    gpio_af(FIELD_PWM_PORT,  FIELD_PWM_PIN,  FIELD_PWM_AF,  0);
    gpio_af(FIELD_PWMN_PORT, FIELD_PWMN_PIN, FIELD_PWMN_AF, 0);

    /* Analog inputs: PA1/PA2/PA3 + PC5 (no AF, analog mode). */
    GPIO_InitTypeDef a = {0};
    a.Mode = GPIO_MODE_ANALOG; a.Pull = GPIO_NOPULL;
    a.Pin = ADC_PINS_A; HAL_GPIO_Init(ADC_PORT_A, &a);
    a.Pin = ADC_PINS_C; HAL_GPIO_Init(ADC_PORT_C, &a);

    /* CAN: PB8 RX / PB9 TX (AF4). */
    gpio_af(CAN_PORT, CAN_RX_PIN | CAN_TX_PIN, CAN_AF, 0);

    /* I2C1 PB6/7, I2C2 PB10/11 (AF1, open-drain, pull-up). */
    gpio_af(I2C1_PORT, I2C1_SCL_PIN | I2C1_SDA_PIN, I2C_AF, 1);
    gpio_af(I2C2_PORT, I2C2_SCL_PIN | I2C2_SDA_PIN, GPIO_AF1_I2C2, 1);

    /* Stator input: PA10 as EXTI rising-edge (NOT a timer capture channel —
     * §0.6 V1/V2). The RPM driver arms STATOR_EXTI_IRQn and diffs TIM2->CNT
     * (free-running timebase) per edge. SYSCFG clock needed for EXTI routing. */
    __HAL_RCC_SYSCFG_CLK_ENABLE();
    GPIO_InitTypeDef s = {0};
    s.Pin = STATOR_PIN;
    s.Mode = GPIO_MODE_IT_RISING;
    s.Pull = GPIO_NOPULL;
    HAL_GPIO_Init(STATOR_PORT, &s);

    /* DIP switches: PA4-PA7 + PB0-PB2, inputs with pull-up (§0.6 V1). */
    GPIO_InitTypeDef d = {0};
    d.Mode = GPIO_MODE_INPUT;
    d.Pull = GPIO_PULLUP;
    d.Pin = DIP_PINS;       HAL_GPIO_Init(DIP_PORT, &d);
    d.Pin = DIP_EXTRA_PINS; HAL_GPIO_Init(DIP_EXTRA_PORT, &d);

    /* USB PA11/PA12 handled by the USB HAL/PCD init (fixed pins). */
}
