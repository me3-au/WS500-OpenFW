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
 * Channel->signal binding RECOVERED FROM FIRMWARE (measurement routine at
 * 0x08014230; conversion constants decoded):
 *   [0] PA1  = NTC thermistor, Beta 3950, clamp -40..160C  (alternator-class temp)
 *   [1] PA2  = NTC thermistor, Beta 3950, clamp -40..160C  (alternator-class temp)
 *   [2] PA3  = NTC thermistor, Beta 3380, clamp -40..140C  (battery-class temp)
 *   [3] PC5  = analog VOLTAGE, Vref 3.3, divider 34.33:1   (battery voltage)
 * (Alt-vs-battery split between PA1/PA2 is a harness detail; both are temp.)
 * NOTE: shunt CURRENT and alternator VOLTAGE are NOT on this ADC scan -> they
 * arrive via CAN and/or I2C (still to be traced).                              */
#define ADC_PORT_A             GPIOA
#define ADC_PINS_A             (GPIO_PIN_1 | GPIO_PIN_2 | GPIO_PIN_3)
#define ADC_PORT_C             GPIOC
#define ADC_PINS_C             (GPIO_PIN_5)

#define ADC_SCAN_LEN           7U      /* total channels per scan */
#define ADC_OVERSAMPLE         4U      /* averaged in software (matches stock) */
#define ADC_EXT_COUNT          4U      /* external channels [0..3] */

/* Logical signal -> ADC buffer slot (from firmware). */
#define SENSOR_CH_TEMP_A1      0U      /* PA1: NTC Beta 3950 (-40..160C) */
#define SENSOR_CH_TEMP_A2      1U      /* PA2: NTC Beta 3950 (-40..160C) */
#define SENSOR_CH_TEMP_BATT    2U      /* PA3: NTC Beta 3380 (-40..140C) */
#define SENSOR_CH_VBAT         3U      /* PC5: voltage, divider 34.33:1 */
#define SENSOR_SLOT_TEMPSENSOR 4U
#define SENSOR_SLOT_VREFINT    5U
#define SENSOR_SLOT_VBAT_INT   6U

/* Recovered scaling constants (hardware facts). */
#define SENSOR_NTC_BETA_ALT    3950.0f
#define SENSOR_NTC_BETA_BATT   3380.0f
#define SENSOR_VBAT_DIVIDER    34.3333f
#define SENSOR_VREF_NOMINAL_V  3.3f

/* ---- Stator / RPM input (manual wire 8, Yellow) --------------------------- *
 * AC frequency from the alternator stator, measured via TIM2 input capture
 * (stock uses handle 0x200029E0 -> TIM2, diffs successive CNT reads for period). */
#define STATOR_TIM             TIM2

/* ---- Current shunt + bus voltage: TI INA2xx over I2C (CONFIRMED) ----------- *
 * Single 500A/50mV shunt (manual wires 12/13, Purple/Grey), at battery OR
 * alternator per $CCN ShuntAtBat. Read by a TI INA226/INA228/INA238 current/
 * power monitor at I2C 7-bit addr 0x40. Firmware auto-detects the variant via
 * DIE_ID (reg 0xFF: 0x2260/0x2280/0x2380) + 'TI' manuf id (reg 0xFE=0x5449).
 * INA gives BOTH current (SHUNT_V reg 0x01) and local bus voltage (BUS_V 0x02),
 * which is why neither is on the internal ADC. See ina2xx.h. */
#define INA_I2C_ADDR7          0x40U     /* 7-bit; addr<<1 = 0x80 on the bus */
#define INA_REG_CONFIG         0x00U
#define INA_REG_SHUNT_V        0x01U     /* -> current (with shunt) */
#define INA_REG_BUS_V          0x02U     /* -> DC volts at shunt (batt or alt) */
#define INA_REG_POWER          0x03U
#define INA_REG_CURRENT        0x04U
#define INA_REG_CALIBRATION    0x05U
#define INA_REG_MANUF_ID       0xFEU     /* 0x5449 = 'TI' */
#define INA_REG_DIE_ID         0xFFU     /* 0x2260=INA226 0x2280=INA228 0x2380=INA238 */
#define INA_DIEID_INA226       0x2260U
#define INA_DIEID_INA228       0x2280U
#define INA_DIEID_INA238       0x2380U
#define SHUNT_FULL_SCALE_A     500.0f    /* 500A / 50mV default shunt */
#define SHUNT_FULL_SCALE_MV    50.0f

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

/* ---- Digital I/O (from HAL_GPIO_ReadPin/WritePin call-site decode) --------- *
 * OUTPUTS (driven 0/1): PA0, PA9 (busiest), PA15, PB14
 *   -> Lamp / Feature-Out (manual wire 2) + status LED(s). Exact split: usage TODO.
 * INPUTS (read):        PA4, PA5, PB0, PB1 (read once) ; PB13 (polled)
 *   -> PA4/PA5/PB0/PB1 = battery-capacity DIP switches (schema BC_Index "0 = use
 *      DIP switches"); PB13 = Enable/Ignition or Feature-In (manual wires 1/3).
 * Also configured but static in MX_GPIO_Init: PC13-15, PB3-5, PC4, PC10.
 * Labels below are best-inference; confirm the exact function<->pin by tracing
 * each pin's usage (which gates charging = Enable, which drives the lamp, etc.). */
#define DIP_PORT_A             GPIOA
#define DIP_PINS_A             (GPIO_PIN_4 | GPIO_PIN_5)   /* battery-capacity DIP */
#define DIP_PORT_B             GPIOB
#define DIP_PINS_B             (GPIO_PIN_0 | GPIO_PIN_1)
#define ENABLE_IN_PORT         GPIOB
#define ENABLE_IN_PIN          GPIO_PIN_13  /* Enable/Ignition or Feature-In: confirm */
#define OUT_PA0_PIN            GPIO_PIN_0   /* GPIOA - LED/feature-out: confirm */
#define OUT_LAMP_PORT          GPIOA
#define OUT_LAMP_PIN           GPIO_PIN_9   /* busiest output - Lamp/Feature-Out?: confirm */
#define OUT_PA15_PIN           GPIO_PIN_15  /* GPIOA */
#define OUT_PB14_PORT          GPIOB
#define OUT_PB14_PIN           GPIO_PIN_14

void board_init(void);        /* clocks + all peripheral GPIO/init */
void board_clock_config(void);

#endif /* WS500_BOARD_H */
