/*
 * sensors.c — ADC1 7-channel oversampled acquisition + scaling (scaling = TODO).
 *
 * Acquisition mirrors the stock design (fact): 7-channel 12-bit scan, DMA
 * circular, x4 software averaging (F072 has no HW oversampler — §0.6 V4).
 * Channel->signal binding + scaling constants are recovered from the stock
 * binary (board.h); the NTC divider R values still need bench confirmation.
 *
 * SPDX-License-Identifier: MIT
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

/*
 * HAL_ADC_MspInit — ADC1 clock enable + DMA1 Channel1 bring-up/link, called by
 * the HAL from inside HAL_ADC_Init() below. Owned here (not board.c) to match
 * the pattern the other peripheral drivers already use for their own MSP
 * hooks: usb_cdc.c defines HAL_PCD_MspInit, can_n2k.c defines HAL_CAN_MspInit
 * — board.c only does GPIO/AF, each driver brings up its own clock+DMA.
 *
 * GAP FOUND while building test-fw (deliverable #14, PROJECT_PLAN §4/M2):
 * this callback did not exist anywhere in the tree before this change. ADC1's
 * and DMA1's clocks were never enabled, and hdma_adc1 above was declared but
 * never configured or linked to hadc1 — so HAL_ADC_Start_DMA() below would
 * have failed immediately (NULL DMA_Handle) on real hardware, and the ADC
 * scan this whole module exists to run would never actually happen. The stale
 * comment that used to sit on sensors_init() ("NOTE: HAL_ADC_MspInit (in
 * board.c) must enable ADC1 + DMA1 clocks") already said this was owed; it
 * had just never been written. Flagged for review since it changes shared
 * code both firmware targets link.
 *
 * ADC1 is hard-wired to DMA1 Channel1 on the F0 family (RM0091 Table 28 — no
 * DMAMUX on this part) — a silicon fact, not a board-specific one.
 */
void HAL_ADC_MspInit(ADC_HandleTypeDef *hadc)
{
    (void)hadc;
    __HAL_RCC_ADC1_CLK_ENABLE();
    __HAL_RCC_DMA1_CLK_ENABLE();

    hdma_adc1.Instance                 = DMA1_Channel1;
    hdma_adc1.Init.Direction           = DMA_PERIPH_TO_MEMORY;
    hdma_adc1.Init.PeriphInc           = DMA_PINC_DISABLE;
    hdma_adc1.Init.MemInc              = DMA_MINC_ENABLE;
    hdma_adc1.Init.PeriphDataAlignment = DMA_PDATAALIGN_HALFWORD;  /* 12-bit
                                        * right-aligned samples fit a half-word */
    hdma_adc1.Init.MemDataAlignment    = DMA_MDATAALIGN_HALFWORD;
    hdma_adc1.Init.Mode                = DMA_CIRCULAR;  /* matches
                                        * ContinuousConvMode/DMAContinuousRequests
                                        * below: the scan free-runs into s_dma */
    hdma_adc1.Init.Priority            = DMA_PRIORITY_LOW;
    HAL_DMA_Init(&hdma_adc1);

    __HAL_LINKDMA(hadc, DMA_Handle, hdma_adc1);
}

void sensors_init(void)
{
    /* PA1/PA2/PA3/PC5 are already set to analog mode by board_init(); channel
     * config (IN1/IN2/IN3/IN15 + internal temp/vref/vbat), scan direction and
     * the DMA target buffer are configured below. */
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
    /* hdma_adc1 is configured and linked to hadc1 inside HAL_ADC_MspInit()
     * above, called by HAL_ADC_Init() a few lines up — nothing left to wire
     * here. */
    HAL_ADCEx_Calibration_Start(&hadc1);
    HAL_ADC_Start_DMA(&hadc1, (uint32_t*)s_dma, ADC_OVERSAMPLE * ADC_SCAN_LEN);

    /* Current/bus-voltage monitor at the shunt (INA226, hardwired @ I2C1 0x40 —
     * §0.6 V3: no variant auto-detect in stock). */
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

void sensors_read(sensor_readings_t *out, ctrl_batt_temp_src_t batt_temp_src)
{
    const float vdda = sensors_vdda();                 /* mV */
    const float lsb  = vdda / 4095.0f / 1000.0f;       /* V per count at the pin */

    /* Battery voltage: PC5 through the recovered 34.33:1 divider (fact). */
    out->vbat_pack_v = sensors_raw(SENSOR_CH_VBAT) * lsb * SENSOR_VBAT_DIVIDER;

    /* Temperatures (stock binary disasm 2026-07-23, PROJECT_PLAN §0.6 V8):
     * PA1/PA2 = beta-3950 alternator/probe NTCs (which is which: bench);
     * PA3 = beta-3380 FET/driver-stage over-temp sensor (stock 125C fault) —
     * NOT battery. */
    const float ntc_a1 = ntc_temp_c(SENSOR_CH_TEMP_A1, SENSOR_NTC_BETA_ALT);
    const float ntc_a2 = ntc_temp_c(SENSOR_CH_TEMP_A2, SENSOR_NTC_BETA_ALT);
    out->driver_temp_c = ntc_temp_c(SENSOR_CH_TEMP_FET, SENSOR_NTC_BETA_FET);

    /* GH#40: batt_temp_src (config, default NONE — see control.h
     * ctrl_batt_temp_src_t for why the default must stay NONE) routes
     * whichever of PA1/PA2 it names to batt_temp_c; the OTHER channel keeps
     * reporting as an alternator/probe temp exactly as before. NONE reports
     * batt_temp_c NAN and both channels stay alternator-side, unchanged from
     * the pre-GH#40 behavior. */
    switch (batt_temp_src) {
    case CTRL_BATT_TEMP_ADC_A:
        out->alt_temp_c  = NAN;
        out->alt_temp2_c = ntc_a2;
        out->batt_temp_c = ntc_a1;
        break;
    case CTRL_BATT_TEMP_ADC_B:
        out->alt_temp_c  = ntc_a1;
        out->alt_temp2_c = NAN;
        out->batt_temp_c = ntc_a2;
        break;
    case CTRL_BATT_TEMP_NONE:
    default:
        out->alt_temp_c  = ntc_a1;
        out->alt_temp2_c = ntc_a2;
        out->batt_temp_c = NAN;
        break;
    }

    /* Charge current + local bus voltage: INA2xx at the shunt (battery or
     * alternator side per ShuntAtBat) — NOT the internal ADC. */
    out->amps_batt = ina2xx_current_a();
    out->bus_v     = ina2xx_bus_v();
}

/* DMA/ADC IRQ handlers to be routed from stm32f0xx_it.c as needed. */
