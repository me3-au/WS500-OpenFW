/*
 * board.h — WS500 (STM32F072xB) pin map and board constants.
 *
 * Single source of truth for the recovered hardware interface. Every pin/AF here
 * was extracted by black-box disassembly of the stock DFU image (see
 * docs/WS500_HARDWARE_SPEC.md). No control logic was taken from the original firmware.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_BOARD_H
#define WS500_BOARD_H

#include "stm32f0xx_hal.h"

/* ---- Clock: crystal-less USB, 48 MHz from HSI48 + CRS ---------------------- */
#define BOARD_SYSCLK_HZ        48000000UL

/* ---- Field-drive PWM (alternator field) ----------------------------------- *
 * PA8  = TIM1_CH1  (AF2)  — primary field PWM output
 * PB15 = TIM1_CH3N (AF2)  — complementary/aux (confirm role vs field-driver topology)
 * BKIN: NOT used by stock (BDTR.BKE=0, no break-AF pin — binary disasm 2026-07-23,
 * PROJECT_PLAN §0.6 V1+V2), and NOT routed on the board. Field fault cutoff is a
 * software MOE-clear (stock-proven); our driver keeps break DISABLED — re-enable
 * only if a fault comparator is ever wired to a break-capable pin.               */
#define FIELD_TIM              TIM1
#define FIELD_TIM_CH           TIM_CHANNEL_1
#define FIELD_PWM_PORT         GPIOA
#define FIELD_PWM_PIN          GPIO_PIN_8
#define FIELD_PWM_AF           GPIO_AF2_TIM1
#define FIELD_PWMN_PORT        GPIOB
#define FIELD_PWMN_PIN         GPIO_PIN_15
#define FIELD_PWMN_AF          GPIO_AF2_TIM1
/* PWM timer resolution; 0..FIELD_PWM_MAX = 0..100% duty. Also sets the PWM period:
 * with PSC in field_drive.c giving a 146.8 kHz timer clock, 1024 -> 143.2 Hz,
 * mirroring the stock WS500 (10-bit field duty). See PROJECT_PLAN §0.6 V2. */
#define FIELD_PWM_MAX          1024U

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
 * 0x08014230; conversion constants decoded; §0.6 V4/V8):
 *   [0] PA1  = NTC thermistor, Beta 3950, clamp -40..160C  (alternator/probe temp)
 *   [1] PA2  = NTC thermistor, Beta 3950, clamp -40..160C  (alternator/probe temp)
 *   [2] PA3  = NTC thermistor, Beta 3380, clamp -40..140C  (FET/DRIVER temp — NOT
 *              battery: drives the stock 125C over-temp fault 0x4029; battery temp
 *              for temp-comp arrives via CAN/BMS. §0.6 V8)
 *   [3] PC5  = analog VOLTAGE, Vref 3.3, divider 34.3333:1 (battery V, FS ~113.3V)
 * (Which of PA1/PA2 is ATS vs the second probe is a harness detail — bench.)
 * 10k NTC pull-up (ratio from FW; exact resistor values are board facts).
 * NOTE: shunt CURRENT and shunt-side bus VOLTAGE are NOT on this ADC scan ->
 * they come from the INA226 on I2C1 @0x40 (§0.6 V3).                            */
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
#define SENSOR_CH_TEMP_FET     2U      /* PA3: NTC Beta 3380 (-40..140C) — FET/driver temp (§0.6 V8) */
#define SENSOR_CH_VBAT         3U      /* PC5: voltage, divider 34.3333:1 */
#define SENSOR_SLOT_TEMPSENSOR 4U
#define SENSOR_SLOT_VREFINT    5U
#define SENSOR_SLOT_VBAT_INT   6U

/* Recovered scaling constants (hardware facts). */
#define SENSOR_NTC_BETA_ALT    3950.0f
#define SENSOR_NTC_BETA_FET    3380.0f  /* PA3 FET/driver sensor (§0.6 V8) */
#define SENSOR_VBAT_DIVIDER    34.3333f
#define SENSOR_VREF_NOMINAL_V  3.3f

/* ---- Stator / RPM input (manual wire 8, Yellow) --------------------------- *
 * PA10 = EXTI10 rising-edge input (NOT a timer capture channel). TIM2 is a plain
 * free-running 32-bit counter used as the timebase (stock: ~979.6 kHz, 1.02 us/
 * tick); the EXTI ISR reads TIM2->CNT on each stator edge and RPM comes from the
 * software CNT difference. Stock binary disasm 2026-07-23, §0.6 V1+V2 — this
 * corrects the earlier "TIM2 input capture" description.                        */
#define STATOR_PORT            GPIOA
#define STATOR_PIN             GPIO_PIN_10
#define STATOR_EXTI_IRQn       EXTI4_15_IRQn   /* PA10 -> EXTI line 10 */
#define STATOR_TIM             TIM2            /* free-running timebase, not capture */
#define STATOR_TIMEBASE_HZ     979600UL        /* stock tick rate (~1.02 us/tick) */

/* ---- Current shunt + bus voltage: TI INA226 over I2C1 (CONFIRMED) ---------- *
 * Single 500A/50mV shunt (manual wires 12/13, Purple/Grey), at battery OR
 * alternator per $CCN ShuntAtBat. Read by a TI INA226 (ONLY — hardwired, no
 * variant auto-detect: stock binary disasm 2026-07-23, §0.6 V3) at I2C1 7-bit
 * addr 0x40. Stock reads regs 0x06 (Mask/Enable, conversion-ready gate), 0x02
 * (bus V) and 0x01 (shunt V), 16-bit big-endian; CALIBRATION is computed at
 * runtime from the configured shunt ratio, never a copied constant.
 * INA gives BOTH current (SHUNT_V reg 0x01) and local bus voltage (BUS_V 0x02),
 * which is why neither is on the internal ADC. See ina2xx.h. */
#define INA_I2C_ADDR7          0x40U     /* 7-bit; addr<<1 = 0x80 on the bus */
#define INA_REG_CONFIG         0x00U
#define INA_REG_SHUNT_V        0x01U     /* -> current (with shunt) */
#define INA_REG_BUS_V          0x02U     /* -> DC volts at shunt (batt or alt) */
#define INA_REG_POWER          0x03U     /* unused: power computed in software (ina2xx.c) */
#define INA_REG_CURRENT        0x04U     /* unused: current computed from SHUNT_V */
#define INA_REG_CALIBRATION    0x05U     /* left at POR 0 — see ina2xx.c */
#define INA_REG_MASK_ENABLE    0x06U     /* conversion-ready gate (stock uses this) */
/* No ID-register defines: neither the stock firmware nor our driver reads
 * MANUF_ID/DIE_ID — the "INA226/228/238 auto-detect" was fiction (§0.6 V3). */
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

/* ---- Digital I/O (from HAL_GPIO_ReadPin/WritePin call-site + usage decode) -- *
 * DIP SWITCHES (battery-capacity code) — CONFIRMED (§0.6 V1): PA4, PA5, PA6, PA7
 *   + PB0, PB1, PB2 — seven inputs, ALL pull-up (PA7/PB2 were previously
 *   undocumented). Read in sequence and packed into a binary value (bit0=PA4...);
 *   this is the BC_Index DIP bank (schema: "0 = use DIP switches").
 * CONTROL INPUT: PB13 — polled, gates a control branch -> Enable/Ignition or
 *   Feature-In (manual wire 1 / 3). Exact label needs control-logic trace/bench.
 * STATUS OUTPUTS (driven 0/1 by state): PA0, PA9 (busiest), PA15, PB14 ->
 *   Lamp/Feature-Out (manual wire 2) + status LED(s). Which is which: bench.
 * Also static in MX_GPIO_Init: PC13-15, PB3-5, PC4, PC10.                        */
#define DIP_PORT               GPIOA
#define DIP_PINS               (GPIO_PIN_4 | GPIO_PIN_5 | GPIO_PIN_6 | GPIO_PIN_7)  /* batt-cap, PA4=LSB; pull-up */
#define DIP_EXTRA_PORT         GPIOB
#define DIP_EXTRA_PINS         (GPIO_PIN_0 | GPIO_PIN_1 | GPIO_PIN_2)  /* more DIP bits; pull-up */
#define CTRL_IN_PORT           GPIOB
#define CTRL_IN_PIN            GPIO_PIN_13  /* Enable/Ignition or Feature-In */
#define STATUS_OUT_A_PINS      (GPIO_PIN_0 | GPIO_PIN_9 | GPIO_PIN_15) /* GPIOA LEDs/lamp */
#define STATUS_OUT_B_PINS      (GPIO_PIN_14)                          /* GPIOB */
#define OUT_LAMP_OR_LED_PIN    GPIO_PIN_9   /* GPIOA, busiest output */

void board_init(void);        /* clocks + all peripheral GPIO/init */
void board_clock_config(void);

#endif /* WS500_BOARD_H */
