/*
 * can_test.c — bxCAN bring-up + the two bench self-tests. Contract: can_test.h.
 *
 * Bit timing is COPIED from Core/Src/can_n2k.c's documented derivation, not
 * re-derived independently (one place should own "how do we compute bxCAN
 * timing for this board's clock tree", and can_n2k.c already got it right):
 * 250 kbit/s from PCLK1 = 48 MHz (board_clock_config(): APB1CLKDivider=DIV1,
 * so PCLK1 = HCLK = 48 MHz). Prescaler=12 -> tq/bit = 48e6/(12*250000) = 16
 * tq/bit; SJW 1 + TS1 13 + TS2 2 = 16 tq total, sample point (1+13)/16 =
 * 87.5% — the J1939/N2K-recommended point, and a reasonable default for any
 * dialect this test-fw might see on a bench CAN bus.
 *
 * GPIO (PB8 RX / PB9 TX, AF4) is already configured by board_init(); this
 * file only needs the CAN1 peripheral clock (HAL_CAN_MspInit below, same
 * split can_n2k.c and usb_cdc.c use: each driver owns its own MSP hook).
 *
 * SPDX-License-Identifier: MIT
 */
#include "can_test.h"
#include "board.h"
#include "stm32f0xx_hal.h"
#include <string.h>

/* Distinctive test frame: extended ID + an 8-byte ASCII payload that reads
 * cleanly on a bus analyzer, so a human watching the wire can recognise it
 * without decoding anything. */
#define CAN_TEST_ID     0x1ABCDEu
static const uint8_t CAN_TEST_PAYLOAD[8] = "WS500TFW";

/* Bounded windows. Loopback is internal to the peripheral (arrives within a
 * tick or two); echo needs a real bus round-trip, so it gets a larger but
 * still bounded window. Both are short enough that a FAILing echo test (the
 * expected outcome with no bus wired) never meaningfully delays anything —
 * the console and watchdog checkpoints keep running normally throughout,
 * since this is a tick-polled state machine, not a blocking wait. */
#define CAN_TEST_LOOP_TIMEOUT_MS   200u
#define CAN_TEST_ECHO_TIMEOUT_MS  2000u

typedef enum { CT_IDLE = 0, CT_RUNNING } ct_run_t;

static CAN_HandleTypeDef s_hcan;
static bool              s_can_up;
static ct_run_t          s_run;
static bool              s_is_echo;
static uint32_t          s_deadline_ms;
static bool              s_last_pass;
static bool              s_last_was_echo;
static bool              s_have_result;

/* HAL MSP hook, called from inside HAL_CAN_Init(): clock plumbing only, same
 * split can_n2k.c uses (this file is the only CAN-owning module in test-fw,
 * so there is exactly one definition to link, not a duplicate-symbol risk). */
void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
    (void)hcan;
    __HAL_RCC_CAN1_CLK_ENABLE();
}

/* Tear down whatever CAN configuration is live. Called before reconfiguring
 * for a new test (loop<->echo need different Mode bits) and after a test
 * concludes, so the bus is never left transmitting/listening between
 * commands — a bench operator running `can loop` twice in a row should not
 * find a stale mailbox or filter state from the first run. */
static void can_test_teardown(void)
{
    if (!s_can_up) return;
    (void)HAL_CAN_Stop(&s_hcan);
    (void)HAL_CAN_DeInit(&s_hcan);
    s_can_up = false;
}

/* Bring up bxCAN in the requested mode with a single "admit everything"
 * filter (mask=0 on bank 0, FIFO0) — this is a bench diagnostic tool, not
 * the production N2K/RV-C Rx path, so there is no reason to filter anything
 * out; can_test_poll() decides what counts as a match. */
static bool can_test_bring_up(uint32_t mode)
{
    s_hcan.Instance = CAN;
    s_hcan.Init.Prescaler            = 12u;              /* see file header */
    s_hcan.Init.Mode                 = mode;
    s_hcan.Init.SyncJumpWidth        = CAN_SJW_1TQ;
    s_hcan.Init.TimeSeg1             = CAN_BS1_13TQ;
    s_hcan.Init.TimeSeg2             = CAN_BS2_2TQ;
    s_hcan.Init.TimeTriggeredMode    = DISABLE;
    s_hcan.Init.AutoBusOff           = ENABLE;   /* hardware auto-recovery,
                                                  * same as can_n2k.c — an
                                                  * echo test with no other
                                                  * node present will drive
                                                  * toward bus-off during its
                                                  * retry storm, and that is
                                                  * expected/harmless here */
    s_hcan.Init.AutoWakeUp           = DISABLE;
    s_hcan.Init.AutoRetransmission   = ENABLE;
    s_hcan.Init.ReceiveFifoLocked    = DISABLE;
    s_hcan.Init.TransmitFifoPriority = DISABLE;

    if (HAL_CAN_Init(&s_hcan) != HAL_OK) return false;

    CAN_FilterTypeDef filt = {0};
    filt.FilterIdHigh         = 0x0000u;
    filt.FilterIdLow          = 0x0000u;
    filt.FilterMaskIdHigh     = 0x0000u;   /* mask 0 -> admit every frame */
    filt.FilterMaskIdLow      = 0x0000u;
    filt.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filt.FilterBank           = 0u;
    filt.FilterMode           = CAN_FILTERMODE_IDMASK;
    filt.FilterScale          = CAN_FILTERSCALE_32BIT;
    filt.FilterActivation     = CAN_FILTER_ENABLE;
    filt.SlaveStartFilterBank = 14u;   /* single-CAN part; HAL still wants a
                                       * value, mirrors can_n2k.c */
    if (HAL_CAN_ConfigFilter(&s_hcan, &filt) != HAL_OK) return false;

    if (HAL_CAN_Start(&s_hcan) != HAL_OK) return false;

    s_can_up = true;
    return true;
}

static bool can_test_start(bool echo)
{
    can_test_teardown();   /* clean slate — see its own comment */

    if (!can_test_bring_up(echo ? CAN_MODE_NORMAL : CAN_MODE_SILENT_LOOPBACK)) {
        s_can_up = false;
        return false;
    }

    CAN_TxHeaderTypeDef h = {0};
    h.ExtId = CAN_TEST_ID;
    h.IDE   = CAN_ID_EXT;
    h.RTR   = CAN_RTR_DATA;
    h.DLC   = (uint32_t)sizeof CAN_TEST_PAYLOAD;
    h.TransmitGlobalTime = DISABLE;

    uint32_t mailbox;
    if (HAL_CAN_AddTxMessage(&s_hcan, &h, CAN_TEST_PAYLOAD, &mailbox) != HAL_OK) {
        can_test_teardown();
        return false;
    }

    s_is_echo     = echo;
    s_deadline_ms = HAL_GetTick() + (echo ? CAN_TEST_ECHO_TIMEOUT_MS
                                           : CAN_TEST_LOOP_TIMEOUT_MS);
    s_run         = CT_RUNNING;
    return true;
}

void can_test_init(void)
{
    s_can_up        = false;
    s_run           = CT_IDLE;
    s_have_result   = false;
    s_last_pass     = false;
    s_last_was_echo = false;
}

bool can_test_start_loop(void) { return can_test_start(false); }
bool can_test_start_echo(void) { return can_test_start(true); }
bool can_test_busy(void)       { return s_run == CT_RUNNING; }

bool can_test_poll(void)
{
    if (s_run != CT_RUNNING) return false;

    /* Bounded drain, mirrors can_n2k.c's can_drain_rx(): the hardware FIFO is
     * 3 deep, a small spare covers a burst arriving mid-drain, never an
     * unbounded loop. */
    CAN_RxHeaderTypeDef rh;
    uint8_t             data[8];
    bool                matched = false;
    for (int i = 0; i < 8 && HAL_CAN_GetRxFifoFillLevel(&s_hcan, CAN_RX_FIFO0) > 0u; i++) {
        if (HAL_CAN_GetRxMessage(&s_hcan, CAN_RX_FIFO0, &rh, data) == HAL_OK &&
            rh.IDE == CAN_ID_EXT && rh.ExtId == CAN_TEST_ID &&
            rh.DLC == sizeof CAN_TEST_PAYLOAD &&
            memcmp(data, CAN_TEST_PAYLOAD, sizeof CAN_TEST_PAYLOAD) == 0) {
            matched = true;
            break;
        }
    }

    const bool timed_out = (int32_t)(HAL_GetTick() - s_deadline_ms) >= 0;
    if (!matched && !timed_out) return false;   /* still waiting */

    s_last_pass     = matched;
    s_last_was_echo = s_is_echo;
    s_have_result   = true;
    s_run           = CT_IDLE;
    can_test_teardown();   /* leave the bus idle between tests, see above */
    return true;
}

bool can_test_last_pass(void)     { return s_have_result && s_last_pass; }
bool can_test_last_was_echo(void) { return s_last_was_echo; }
