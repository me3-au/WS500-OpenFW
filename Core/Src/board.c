/*
 * board.c — clocks + peripheral GPIO init to the recovered WS500 pin map.
 * SPDX-License-Identifier: MIT
 */
#include "board.h"
#include "can_n2k.h"   /* can_n2k_set_n2k_enabled()/set_rvc_enabled(): see the
                        * HSE-fallback CAN-suppression comment below. */

static bool s_hse_ok;   /* latched at boot; board_clock_running_on_hse() */

/* Bring SYSCLK up on HSE + PLLx6 (48 MHz, matches stock). Returns true iff
 * HAL reports both the crystal and the PLL locked AND the switch completed.
 * HAL_RCC_OscConfig()'s HSE wait is bounded by HSE_STARTUP_TIMEOUT (100 ms,
 * Core/Inc/stm32f0xx_hal_conf.h) and its PLL-lock wait by PLL_TIMEOUT_VALUE
 * (2 ms) -- both HAL-internal, both return HAL_TIMEOUT rather than spinning
 * forever, so this function itself never blocks unboundedly (GH#38
 * requirement 1). If HSE times out, HAL_RCC_OscConfig() returns immediately
 * without touching the PLL bits at all (stm32f0xx_hal_rcc.c: the HSE wait
 * loop's timeout path is an early `return HAL_TIMEOUT`, before the PLL
 * section runs), so a failed attempt here leaves no partial clock state for
 * the caller to unwind. */
static bool board_clock_try_hse(void)
{
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSE;
    osc.HSEState       = RCC_HSE_ON;
    osc.PLL.PLLState    = RCC_PLL_ON;
    osc.PLL.PLLSource   = RCC_PLLSOURCE_HSE;
    osc.PLL.PREDIV      = RCC_PREDIV_DIV1;   /* PLL entry = HSE/1 = 8 MHz */
    osc.PLL.PLLMUL      = RCC_PLL_MUL6;      /* 8 MHz x6 = 48 MHz, matches
                                              * stock (§0.6 V2/V6) */
    if (HAL_RCC_OscConfig(&osc) != HAL_OK) return false;

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                          RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource    = RCC_SYSCLKSOURCE_PLLCLK;
    clk.AHBCLKDivider   = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider  = RCC_HCLK_DIV2;     /* Keep APB/2 -- matches stock
                                              * (§0.6 V6). TIM1's clock-
                                              * doubling rule (APB prescaler
                                              * != 1 -> TIMxCLK = 2*PCLK1)
                                              * still lands TIM1CLK at
                                              * 48 MHz, same as APB/1 would,
                                              * so field_drive.c's PSC=326/
                                              * ARR=1024 -> 143.2 Hz (§0.6 V2)
                                              * stays exact either way -- this
                                              * choice is about matching
                                              * stock bit-for-bit, not about
                                              * the PWM math, which cannot
                                              * drift from it. */
    return HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1) == HAL_OK;
}

/* Fallback SYSCLK source: HSI48, exactly the pre-GH#38 configuration. Reached
 * only when board_clock_try_hse() above timed out -- i.e. the crystal did not
 * start. HSI48 free-runs up to +/-3% without CRS trim (no USB host attached
 * is the normal case), which is out of spec for CAN but entirely fine for
 * field PWM and the charge-stage timers (GH#38: this is a CAN-correctness
 * concern, not a safety one), so the regulator keeps charging rather than
 * hanging or faulting to safe state over a dead crystal. */
static void board_clock_fallback_hsi48(void)
{
    RCC_OscInitTypeDef osc = {0};
    osc.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    osc.HSI48State      = RCC_HSI48_ON;
    osc.PLL.PLLState    = RCC_PLL_NONE;
    (void)HAL_RCC_OscConfig(&osc);   /* bounded (HSI48_TIMEOUT_VALUE); return
                                     * ignored same as pre-GH#38 -- if even
                                     * HSI48 will not start, SYSCLK simply
                                     * stays on the reset-default HSI and the
                                     * rest of board_init() carries on
                                     * regardless (no path in this file
                                     * blocks on a clock ever coming up). */

    RCC_ClkInitTypeDef clk = {0};
    clk.ClockType      = RCC_CLOCKTYPE_SYSCLK | RCC_CLOCKTYPE_HCLK |
                          RCC_CLOCKTYPE_PCLK1;
    clk.SYSCLKSource    = RCC_SYSCLKSOURCE_HSI48;
    clk.AHBCLKDivider   = RCC_SYSCLK_DIV1;
    clk.APB1CLKDivider  = RCC_HCLK_DIV1;     /* pre-GH#38 value; TIM1CLK still
                                              * lands at 48 MHz here too (no
                                              * doubling needed since the
                                              * prescaler is already 1), so
                                              * PSC=326/ARR=1024 is unaffected
                                              * by which fallback divider is
                                              * used -- kept as-is to change
                                              * nothing else about a path that
                                              * already ran this way. */
    (void)HAL_RCC_ClockConfig(&clk, FLASH_LATENCY_1);
}

void board_clock_config(void)
{
    s_hse_ok = board_clock_try_hse();
    if (!s_hse_ok) {
        board_clock_fallback_hsi48();

        /* GH#38 requirement 3: a node transmitting at up to +/-3% clock error
         * can corrupt an otherwise healthy bus with error frames, so going
         * CAN-quiet is the neighbourly failure, not merely the safe one --
         * suppress Tx on BOTH dialects rather than just flagging the wire
         * data as suspect. Mechanism: the existing public dialect switches
         * (can_n2k_set_n2k_enabled()/set_rvc_enabled(), can_n2k.h) rather
         * than a new suppression path in can_n2k.c -- can_n2k_publish()
         * already no-ops per-dialect on these flags (can_n2k.c), so this
         * reuses a seam that already exists instead of adding one. Safe to
         * call this early: main.c calls board_init() (which reaches this)
         * BEFORE can_n2k_init(), and can_n2k_init() never resets these
         * flags, so the false set here is not clobbered by the port coming
         * up afterwards. A flag-only alternative (publish anyway, mark the
         * telemetry payload untrustworthy) was rejected: nothing in the N2K/
         * RV-C wire schemas defines an "ignore my bit-timing" signal, and a
         * downstream node that does not honour a novel diagnostic field
         * would still see frames arriving at the wrong bit rate -- actually
         * not transmitting removes the corruption risk categorically rather
         * than trusting every listener to opt in to distrust. */
        can_n2k_set_n2k_enabled(false);
        can_n2k_set_rvc_enabled(false);
    }

    /* USB clock: HSI48, independent of whichever SYSCLK path above won --
     * USB tolerates CRS trim-when-present regardless (GH#38). If the fallback
     * above already ran, HSI48 is already on; this second bounded OscConfig
     * call is a harmless re-request (HAL treats "state already matches" as a
     * no-op for an oscillator that is not currently the requested-off one). */
    RCC_OscInitTypeDef hsi48 = {0};
    hsi48.OscillatorType = RCC_OSCILLATORTYPE_HSI48;
    hsi48.HSI48State      = RCC_HSI48_ON;
    hsi48.PLL.PLLState    = RCC_PLL_NONE;
    (void)HAL_RCC_OscConfig(&hsi48);

    RCC_PeriphCLKInitTypeDef pclk = {0};
    pclk.PeriphClockSelection = RCC_PERIPHCLK_USB;
    pclk.UsbClockSelection    = RCC_USBCLKSOURCE_HSI48;
    (void)HAL_RCCEx_PeriphCLKConfig(&pclk);

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

bool board_clock_running_on_hse(void) { return s_hse_ok; }

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

    /* PB13 enable/feature input, polled + debounced in dio.c. Pull-up assumed
     * by convention with the confirmed DIP inputs above; not itself bench-
     * confirmed (board.h CTRL_IN_PIN comment). */
    GPIO_InitTypeDef ci = {0};
    ci.Mode = GPIO_MODE_INPUT;
    ci.Pull = GPIO_PULLUP;
    ci.Pin  = CTRL_IN_PIN;
    HAL_GPIO_Init(CTRL_IN_PORT, &ci);

    /* Status/DIO outputs: PA9 (OUT_LAMP_OR_LED_PIN, "busiest") + PB14
     * (STATUS_OUT_B_PINS). Lamp-vs-LED role split is bench-pending (M2,
     * IO_COVERAGE) — dio.c drives these as dio_out_a/dio_out_b without
     * guessing which is which. Fail-safe default: off (LOW) at boot, same
     * convention as field_drive's start-at-0%. PA0 is deliberately left
     * unclaimed here (see board.h STATUS_OUT_A_PINS comment: it was formerly
     * grouped with PA15, which §0.6 V7 resolved as the EEPROM /WP line, not a
     * status output — there is no fact tying PA0 to either output channel). */
    GPIO_InitTypeDef out = {0};
    out.Mode  = GPIO_MODE_OUTPUT_PP;
    out.Pull  = GPIO_NOPULL;
    out.Speed = GPIO_SPEED_FREQ_LOW;
    out.Pin = OUT_LAMP_OR_LED_PIN; HAL_GPIO_Init(GPIOA, &out);
    out.Pin = STATUS_OUT_B_PINS;   HAL_GPIO_Init(GPIOB, &out);
    HAL_GPIO_WritePin(GPIOA, OUT_LAMP_OR_LED_PIN, GPIO_PIN_RESET);
    HAL_GPIO_WritePin(GPIOB, STATUS_OUT_B_PINS,   GPIO_PIN_RESET);

    /* EEPROM /WP: PA15, GPIO output (§0.6 V7). Default HIGH (write-protected)
     * at boot; eeprom24c16.c drives it LOW only for the duration of a write. */
    GPIO_InitTypeDef wp = {0};
    wp.Mode  = GPIO_MODE_OUTPUT_PP;
    wp.Pull  = GPIO_NOPULL;
    wp.Speed = GPIO_SPEED_FREQ_LOW;
    wp.Pin   = EEPROM_WP_PIN;
    HAL_GPIO_Init(EEPROM_WP_PORT, &wp);
    HAL_GPIO_WritePin(EEPROM_WP_PORT, EEPROM_WP_PIN, GPIO_PIN_SET);

    /* USB PA11/PA12 handled by the USB HAL/PCD init (fixed pins). */
}
