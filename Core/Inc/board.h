/*
 * board.h — WS500 (STM32F072xB) pin map and board constants.
 *
 * Single source of truth for the recovered hardware interface. Every pin/AF here
 * was extracted by black-box disassembly of the stock DFU image (see
 * docs/WS500_HARDWARE_SPEC.md). No control logic was taken from the original firmware.
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#ifndef WS500_BOARD_H
#define WS500_BOARD_H

#include "stm32f0xx_hal.h"

/* ---- Clock: crystal-less USB, 48 MHz from HSI48 + CRS ---------------------- */
#define BOARD_SYSCLK_HZ        48000000UL

/* ---- Field-drive PWM (alternator field) ----------------------------------- *
 * PA8  = TIM1_CH1  (AF2)  — primary field PWM output
 * PB15 = TIM1_CH3N (AF2)  — complementary/aux (confirm role vs field-driver topology)
 * TIM1 break input (BKIN) provides hardware fault cutoff of the field.          */
#define FIELD_TIM              TIM1
#define FIELD_TIM_CH           TIM_CHANNEL_1
#define FIELD_PWM_PORT         GPIOA
#define FIELD_PWM_PIN          GPIO_PIN_8
#define FIELD_PWM_AF           GPIO_AF2_TIM1
#define FIELD_PWMN_PORT        GPIOB
#define FIELD_PWMN_PIN         GPIO_PIN_15
#define FIELD_PWMN_AF          GPIO_AF2_TIM1
/* PWM timer resolution. Pick freq in field_drive.c; 0..FIELD_PWM_MAX = 0..100%. */
#define FIELD_PWM_MAX          1000U

/* ---- Analog sensing (ADC1, 7-ch scan, x4 oversample+average) --------------- *
 * External inputs, in ascending channel order (== DMA buffer index order):
 *   [0] PA1  = ADC_IN1
 *   [1] PA2  = ADC_IN2
 *   [2] PA3  = ADC_IN3
 *   [3] PC5  = ADC_IN15
 * Internal (calibration), appended by the scan:
 *   [4] temp sensor  [5] VREFINT  [6] VBAT
 *
 * NOTE: which external pin carries VBat / VAlt / shunt-current / temperature is
 * set by board wiring and is NOT yet known. Bind by signal injection, then map
 * the SENSOR_CH_* indices below to the right slot.                              */
#define ADC_PORT_A             GPIOA
#define ADC_PINS_A             (GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3)
#define ADC_PORT_C             GPIOC
#define ADC_PINS_C             (GPIO_PIN_5)

#define ADC_SCAN_LEN           7U      /* total channels per scan */
#define ADC_OVERSAMPLE         4U      /* averaged in software (matches stock) */
#define ADC_EXT_COUNT          4U      /* external channels [0..3] */

/* Logical signal -> ADC buffer slot. *** PLACEHOLDER — verify by injection *** */
#define SENSOR_CH_VBAT         0U      /* TODO confirm (PA1?) */
#define SENSOR_CH_VALT         1U      /* TODO confirm (PA2?) */
#define SENSOR_CH_SHUNT        2U      /* TODO confirm (PA3?) */
#define SENSOR_CH_TEMP         3U      /* TODO confirm (PC5?) */
#define SENSOR_SLOT_TEMPSENSOR 4U
#define SENSOR_SLOT_VREFINT    5U
#define SENSOR_SLOT_VBAT_INT   6U

/* ---- CAN (bxCAN): RV-C / NMEA2000 / J1939 / Victron ------------------------ *
 * PB8 = CAN_RX (AF4), PB9 = CAN_TX (AF4)                                        */
#define CAN_PORT               GPIOB
#define CAN_RX_PIN             GPIO_PIN_8
#define CAN_TX_PIN             GPIO_PIN_9
#define CAN_AF                 GPIO_AF4_CAN

/* ---- I2C ------------------------------------------------------------------- *
 * I2C1: PB6=SCL, PB7=SDA (AF1)   I2C2: PB10=SCL, PB11=SDA (AF1) — open-drain    */
#define I2C1_PORT              GPIOB
#define I2C1_SCL_PIN           GPIO_PIN_6
#define I2C1_SDA_PIN           GPIO_PIN_7
#define I2C2_PORT              GPIOB
#define I2C2_SCL_PIN           GPIO_PIN_10
#define I2C2_SDA_PIN           GPIO_PIN_11
#define I2C_AF                 GPIO_AF1_I2C1   /* AF1 for both on this part */

/* ---- USB FS (CDC virtual COM — config channel) ---------------------------- *
 * PA11 = DM, PA12 = DP (fixed pins on F072; no AF config needed by HAL)         */
#define USB_PORT               GPIOA
#define USB_DM_PIN             GPIO_PIN_11
#define USB_DP_PIN             GPIO_PIN_12

/* ---- Digital I/O (function not yet resolved — LEDs/enable/status/fault) ---- *
 * Seen configured in the stock MX_GPIO_Init: PC13 PC14 PC15 PA0 PA9 PA15
 * PB3 PB4 PB5 PB14 PC4 PC10. Assign as you identify them.                       */

void board_init(void);        /* clocks + all peripheral GPIO/init */
void board_clock_config(void);

#endif /* WS500_BOARD_H */
