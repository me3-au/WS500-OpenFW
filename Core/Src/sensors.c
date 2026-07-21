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

void sensors_read(reg_inputs_t *out)
{
    const float vdda = sensors_vdda();                 /* mV */
    const float lsb  = vdda / 4095.0f / 1000.0f;       /* V per count at the pin */

    /* ---- TODO: real scaling once channel binding + front-end are confirmed ---- *
     * Each external pin is a divided/amplified version of a physical quantity.
     *   vbat  = pin_volts(SENSOR_CH_VBAT)  * VBAT_DIVIDER;
     *   valt  = pin_volts(SENSOR_CH_VALT)  * VALT_DIVIDER;
     *   amps  = (pin_volts(SENSOR_CH_SHUNT) - SHUNT_OFFSET) * SHUNT_A_PER_V;
     *   temp  = thermistor_to_c(pin_volts(SENSOR_CH_TEMP));
     * Determine the *_DIVIDER / SHUNT_* / thermistor curve by injection or board trace. */
    #define PIN_V(slot)  (sensors_raw(slot) * lsb)

    out->vbat        = PIN_V(SENSOR_CH_VBAT);   /* placeholder: raw pin volts */
    out->valt        = PIN_V(SENSOR_CH_VALT);
    out->amps        = 0.0f;                     /* until shunt scaling known */
    out->alt_temp_c  = NAN;
    out->batt_temp_c = NAN;
    out->rpm         = 0.0f;                     /* from stator capture / CAN */
    out->bms_charge_ok = true;                   /* until CAN BMS wired */

    #undef PIN_V
}

/* DMA/ADC IRQ handlers to be routed from stm32f0xx_it.c as needed. */
