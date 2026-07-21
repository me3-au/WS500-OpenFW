/*
 * sensors.c — ADC1 7-channel oversampled acquisition + scaling (scaling = TODO).
 *
 * Acquisition mirrors the stock design (fact): 7-channel scan, DMA circular, x4
 * software oversample+average. Channel->signal binding and the raw->engineering
 * scaling are NOT yet known and must be resolved by signal injection (see README).
 *
 * SPDX-License-Identifier: GPL-3.0-or-later
 */
#include "sensors.h"
#include "board.h"
#include "ina2xx.h"
#include <math.h>

static ADC_HandleTypeDef hadc1;
static DMA_HandleTypeDef hdma_adc1;

/* Circular DMA target: ADC_OVERSAMPLE frames x ADC_SCAN_LEN channels. */
static volatile uint16_t s_dma[ADC_OVERSAMPLE * ADC_SCAN_LEN];
static uint16_t s_avg[ADC_SCAN_LEN];

/* STM32F0 factory calibration (facts, from the datasheet memory map). */
#define VREFINT_CAL_ADDR  ((uint16_t*)0x1FFFF7BA)  /* @3.3V, 12-bit */
#define VREFINT_CAL_VREF  3300.0f

void sensors_init(void)
{
    /* NOTE: HAL_ADC_MspInit (in board.c) must enable ADC1 + DMA1 clocks and set
     * PA1/PA2/PA3/PC5 to analog. Configure IN1,IN2,IN3,IN15 + internal temp/
     * vref/vbat channels here, scan ascending, DMA circular into s_dma. */
    hadc1.Instance = ADC1;
    hadc1.Init.ClockPrescaler = ADC_CLOCK_SYNC_PCLK_DIV4;
    hadc1.Init.Resolution = ADC_RESOLUTION_12B;
    hadc1.Init.DataAlign = ADC_DATAALIGN_RIGHT;
    hadc1.Init.ScanConvMode = ADC_SCAN_DIRECTION_FORWARD;
    hadc1.Init.EOCSelection = ADC_EOC_SINGLE_CONV;
    hadc1.Init.ContinuousConvMode = ENABLE;
    hadc1.Init.DMAContinuousRequests = ENABLE;
    hadc1.Init.SamplingTimeCommon = ADC_SAMPLETIME_239CYCLES_5;
    HAL_ADC_Init(&hadc1);

    ADC_ChannelConfTypeDef ch = {0};
    const uint32_t chans[ADC_SCAN_LEN] = {
        ADC_CHANNEL_1, ADC_CHANNEL_2, ADC_CHANNEL_3, ADC_CHANNEL_15,
        ADC_CHANNEL_TEMPSENSOR, ADC_CHANNEL_VREFINT, ADC_CHANNEL_VBAT
    };
    for (unsigned i = 0; i < ADC_SCAN_LEN; i++) {
        ch.Channel = chans[i];
        ch.Rank = ADC_RANK_CHANNEL_NUMBER;
        HAL_ADC_ConfigChannel(&hadc1, &ch);
    }
    (void)hdma_adc1;  /* wired in board.c MspInit */
    HAL_ADCEx_Calibration_Start(&hadc1);
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)s_dma, ADC_OVERSAMPLE * ADC_SCAN_LEN);

    /* Current/bus-voltage monitor at the shunt (INA226/228/238 @ I2C 0x40). */
    ina2xx_init(SHUNT_FULL_SCALE_A, SHUNT_FULL_SCALE_MV);
}

void sensors_update(void)
{
    for (unsigned c = 0; c < ADC_SCAN_LEN; c++) {
        uint32_t acc = 0;
        for (unsigned f = 0; f < ADC_OVERSAMPLE; f++)
            acc += s_dma[f * ADC_SCAN_LEN + c];
        s_avg[c] = (uint16_t)(acc / ADC_OVERSAMPLE);
    }
}

uint16_t sensors_raw(unsigned slot) { return slot < ADC_SCAN_LEN ? s_avg[slot] : 0; }

float sensors_vdda(void)
{
    uint16_t vref = s_avg[SENSOR_SLOT_VREFINT];
    if (!vref) return 3300.0f;
    return VREFINT_CAL_VREF * (float)(*VREFINT_CAL_ADDR) / (float)vref; /* mV */
}

/* NTC Beta model: 1/T = 1/T0 + (1/B)*ln(R/R0), T0=25C, R0=10k assumed.
 * Front-end assumed a divider pull-up to Vref; adjust R_FIXED/R0 to the board. */
static float ntc_temp_c(unsigned slot, float beta)
{
    const float raw = (float)sensors_raw(slot);
    if (raw <= 0.0f || raw >= 4095.0f) return NAN;      /* open/short */
    const float ratio = raw / (4095.0f - raw);          /* R_ntc / R_fixed */
    const float R0_over_Rf = 1.0f;                       /* TODO: set from board */
    const float invT = 1.0f/298.15f + (1.0f/beta) * logf(ratio * R0_over_Rf);
    return (1.0f/invT) - 273.15f;
}

void sensors_read(reg_inputs_t *out)
{
    const float vdda = sensors_vdda();                 /* mV */
    const float lsb  = vdda / 4095.0f / 1000.0f;       /* V per count at the pin */

    /* Battery voltage: PC5 through the recovered 34.33:1 divider (fact). */
    out->vbat = sensors_raw(SENSOR_CH_VBAT) * lsb * SENSOR_VBAT_DIVIDER;

    /* Temperatures: NTC Beta model with the recovered Beta values (fact).
     * Confirm R_FIXED/R0 and which of PA1/PA2 is the alternator sensor. */
    out->alt_temp_c  = ntc_temp_c(SENSOR_CH_TEMP_A1, SENSOR_NTC_BETA_ALT);
    out->batt_temp_c = ntc_temp_c(SENSOR_CH_TEMP_BATT, SENSOR_NTC_BETA_BATT);

    /* Charge current + local bus voltage come from the INA2xx monitor at the shunt
     * (battery or alternator side per ShuntAtBat) — NOT the internal ADC. */
    out->amps        = ina2xx_current_a();
    out->valt        = ina2xx_bus_v();   /* bus V at the shunt location */

    out->rpm         = 0.0f;        /* from TIM2 stator capture (see STATOR_TIM) */
    out->bms_charge_ok = true;      /* until CAN BMS wired */
}

/* DMA/ADC IRQ handlers to be routed from stm32f0xx_it.c as needed. */
