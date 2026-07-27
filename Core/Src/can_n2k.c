/*
 * can_n2k.c — bxCAN HAL glue for NMEA2000 Tx (GH#18, deliverable #9;
 * contract: can_n2k.h). Thin by design: every timing/priority/state
 * decision lives in the pure control/n2k_addrclaim.c and control/n2k_sched.c
 * modules (host-tested, see control/test/test_n2k_addrclaim.c and
 * test_n2k_sched.c); this file only drives them from real bxCAN registers
 * and HAL_GetTick(), and pushes the frames they produce at the wire.
 *
 * Polling-only (no CAN IRQ) — same "never block" contract as usb_cdc.c: a
 * software Tx ring decouples "the scheduler decided N frames are due this
 * cycle" from "the bxCAN hardware only has 3 mailboxes", and a full ring or
 * a full mailbox set drops-and-counts rather than waiting. Rx is drained
 * with a bounded loop, never an unbounded one.
 *
 * SPDX-License-Identifier: MIT
 */
#include "can_n2k.h"
#include "board.h"
#include "err_budget.h"
#include "version.h"
#include "n2k_addrclaim.h"
#include "n2k_sched.h"

/* ---- device identity (const-ish table next to the claim) ------------------ *
 * NAME + product info live here, not in control/, because they mix a real
 * hardware fact (the 96-bit UID, board.h/RM0091) with protocol identity that
 * is Core's business to own (n2k_addrclaim.c takes a NAME as a parameter and
 * stays hardware-agnostic). */

/* Preferred initial source address. Arbitrary: this device sets the NAME's
 * Arbitrary-Address-Capable bit (build_name() below) and steps off it on
 * contention (n2k_addrclaim.c), so the exact starting value only affects who
 * wins an early race, never correctness. [SPEC-SIGNOFF: no verified standard
 * default-address table for this device class is in this repo; a bench/
 * analyzer check against a real N2K network is the eventual falsification
 * step, CAN_INTEGRATION.md's own "verify on real hardware" caveat.] */
#define N2K_PREFERRED_ADDR    34u

/* NMEA 2000 Device Class 35 "Electrical Generation" / Device Function 140
 * "Alternator" (within Industry Group 4 marine, N2K_PROP_INDUSTRY from
 * n2k_encode.h, which IS settled). [SPEC-SIGNOFF]: these two numeric codes
 * are reproduced from the public NMEA2000 Appendix B / ISO 11783-2 device
 * class & function tables (the same ones every open N2K stack, e.g.
 * ttlappalainen/NMEA2000, hard-codes) from general familiarity, not from a
 * spec document checked into this repo — verify against the official list
 * (or simply how a real Cerbo/analyzer categorizes the device) before
 * treating this as authoritative. This is Tx-only telemetry
 * (CAN_INTEGRATION.md §0): getting it wrong miscategorizes the device on a
 * third-party display, it cannot reach the field-drive path. */
#define N2K_DEVICE_CLASS      35u
#define N2K_DEVICE_FUNCTION   140u

/* 126998 Configuration Information: installation description strings are
 * normally set by the installer (not yet exposed via config_protocol.c), so
 * they transmit empty rather than a made-up placeholder (n2k_encode_126998
 * treats NULL as "empty string", not an error). mfg_info is static text. */
static const char N2K_MFG_INFO[] = "WS500-OpenFW (MIT, open source)";

static n2k_product_info_t s_product;      /* filled at init: serial is runtime */
static char                s_serial[25];  /* 96-bit UID as 24 hex chars + NUL,
                                           * same convention as usb_cdc.c's
                                           * USB serial-number descriptor */

static void build_serial(void)
{
    static const char HEX[] = "0123456789ABCDEF";
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    int n = 0;
    for (int w = 0; w < 3; w++) {
        const uint32_t v = uid[w];
        for (int nib = 7; nib >= 0; nib--)
            s_serial[n++] = HEX[(v >> (nib * 4)) & 0xFu];
    }
    s_serial[n] = '\0';
}

static void build_product_info(void)
{
    /* [SPEC-SIGNOFF]: n2k_version/load_equiv are not measured/certified
     * values (see the field-by-field notes below) -- this device has no
     * NMEA2000 certification to report against. */
    s_product.n2k_version   = 2100u;   /* plausible current-era DB version;
                                        * not verified against a spec doc */
    s_product.product_code  = 0u;      /* no NMEA-assigned product code --
                                        * same honest-uncertain stance as
                                        * usb_cdc.c's pid.codes TEST VID/PID */
    s_product.model_id      = "WS500-OpenFW";
    s_product.sw_version    = FW_VERSION_STRING;
    s_product.model_version = "WS500";
    s_product.serial        = s_serial;
    s_product.cert_level    = 0u;      /* not NMEA2000 certified (a fact) */
    s_product.load_equiv    = 1u;      /* 50 mA bus-draw estimate for a
                                        * typical CAN transceiver, not a
                                        * measured value -- bench-pending */
}

/* This device's 64-bit ISO NAME (n2k_addrclaim.h bit layout). */
static uint64_t build_name(void)
{
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    n2k_name_fields_t f = {0};
    /* Unique Number: low 21 bits of UID word 0. Not collision-proof across
     * the whole NAME address space, but sufficient per-unit identity --
     * there is exactly one WS500 to distinguish right now (PROJECT_PLAN
     * §0.6), and NAME uniqueness only matters for arbitration among devices
     * actually sharing this bus. */
    f.unique_number          = uid[0] & 0x1FFFFFu;
    f.manufacturer_code      = N2K_PROP_MFG_CODE;   /* n2k_encode.h [SPEC-SIGNOFF] */
    f.device_instance_lower  = 0u;
    f.device_instance_upper  = 0u;
    f.device_function        = N2K_DEVICE_FUNCTION;
    f.device_class           = N2K_DEVICE_CLASS;
    f.system_instance        = 0u;
    f.industry_group         = N2K_PROP_INDUSTRY;   /* 4 = marine */
    f.arbitrary_addr_capable = true;                /* n2k_addrclaim.c relies
                                                      * on this to step off a
                                                      * contested address */
    return n2k_name_build(&f);
}

/* ---- driver state ----------------------------------------------------------- */

static CAN_HandleTypeDef s_hcan;
static bool               s_ok;
static n2k_ac_t            s_ac;
static n2k_sched_t         s_sched;
static bool               s_bus_off_latched;   /* edge-detect for CAN_FLAG_BOF */
static uint32_t           s_bus_off_events;
static uint32_t           s_tx_dropped;        /* ring-full or mailbox-full */

/* Software Tx ring: decouples "the scheduler produced N frames this cycle"
 * (up to N2K_SCHED_BATCH, sized for 126996's 20-frame worst case plus
 * headroom) from bxCAN's 3 hardware mailboxes, which can't possibly drain a
 * multi-frame fast-packet burst in one shot. Drained every can_n2k_poll()
 * call, which runs far more often than the 10 ms control tick that produces
 * bursts (main.c's for(;;) calls can_n2k_poll() unconditionally every
 * iteration), so steady-state occupancy stays low; sized generously (64) so
 * a single worst-case burst doesn't overflow it before the next drain. */
#define N2K_TX_RING      64u
#define N2K_SCHED_BATCH  32u
#define N2K_AC_BATCH      4u   /* at most 1 claim + 1 ISO-Request reply/cycle */

static n2k_frame_t s_tx_ring[N2K_TX_RING];
static uint16_t    s_tx_w, s_tx_r;

static void can_tx_enqueue(const n2k_frame_t *frames, int n)
{
    int i;
    if (!s_ok || n <= 0) return;
    for (i = 0; i < n; i++) {
        const uint16_t next_w = (uint16_t)((s_tx_w + 1u) % N2K_TX_RING);
        if (next_w == s_tx_r) {           /* ring full: drop and count, never
                                           * block (usb_cdc.c house style) */
            s_tx_dropped++;
            continue;
        }
        s_tx_ring[s_tx_w] = frames[i];
        s_tx_w = next_w;
    }
}

/* Push ring contents into free hardware mailboxes. HAL_CAN_AddTxMessage()
 * only writes mailbox registers (stm32f0xx_hal_can.c) — no wait loop — so
 * this never blocks; a real register-level failure (should not happen given
 * the free-mailbox check) just leaves the frame in the ring for next time. */
static void can_drain_tx_ring(void)
{
    CAN_TxHeaderTypeDef h = {0};
    h.IDE = CAN_ID_EXT;
    h.RTR = CAN_RTR_DATA;
    h.TransmitGlobalTime = DISABLE;

    while (s_tx_r != s_tx_w && HAL_CAN_GetTxMailboxesFreeLevel(&s_hcan) > 0u) {
        const n2k_frame_t *fr = &s_tx_ring[s_tx_r];
        uint32_t mailbox;
        h.ExtId = fr->id;
        h.DLC   = fr->dlc;
        if (HAL_CAN_AddTxMessage(&s_hcan, &h, fr->data, &mailbox) != HAL_OK) break;
        errb_ok(ERRB_CAN);
        s_tx_r = (uint16_t)((s_tx_r + 1u) % N2K_TX_RING);
    }
}

/* Drain Rx FIFO0 into the address-claim state machine. Bounded loop: the
 * hardware FIFO is 3 deep, +5 spare covers a burst arriving mid-drain
 * without ever looping on a jammed/flooded bus (never block). Full control-
 * Rx (BMS/DVCC ceilings) is OUT OF SCOPE here -- TODO(GH#10) -- only the two
 * PGNs address-claim needs are admitted by the Rx filters (can_n2k_init). */
static void can_drain_rx(void)
{
    CAN_RxHeaderTypeDef rh;
    uint8_t data[8];
    int i;
    for (i = 0; i < 8 && HAL_CAN_GetRxFifoFillLevel(&s_hcan, CAN_RX_FIFO0) > 0u; i++) {
        if (HAL_CAN_GetRxMessage(&s_hcan, CAN_RX_FIFO0, &rh, data) == HAL_OK &&
            rh.IDE == CAN_ID_EXT) {
            n2k_ac_rx(&s_ac, rh.ExtId, data, (size_t)rh.DLC);
        }
    }
}

/* Edge-triggered: count each bus-off EVENT once, not once per poll while the
 * bus happens to still be off (AutoBusOff recovers it in hardware; we are
 * only counting, per can_n2k.h / §7 R6). */
static void can_poll_bus_off(void)
{
    const bool boff = __HAL_CAN_GET_FLAG(&s_hcan, CAN_FLAG_BOF) != 0u;
    if (boff && !s_bus_off_latched) {
        can_n2k_note_bus_off();
        s_bus_off_latched = true;
    } else if (!boff) {
        s_bus_off_latched = false;
    }
}

/* ---- Rx filter: admit only PGN 60928 (Address Claim) / 59904 (ISO Request)
 * into FIFO0 -- everything else this driver doesn't yet understand (GH#10)
 * is left un-filtered-in, so bxCAN discards it in hardware. -------------- */

/* bxCAN 32-bit-scale filter register format (RM0091 §30.7.4, standard
 * silicon behaviour, not a WS500-specific fact): bits[31:3] = the 29-bit
 * extended CAN ID left-shifted by 3, bit2 = IDE, bit1 = RTR. We require
 * IDE=1 (extended) and RTR=0 (data frame) on every admitted frame. */
static void can_filter_pair(uint32_t id29, uint32_t mask29,
                            uint16_t *id_hi, uint16_t *id_lo,
                            uint16_t *mask_hi, uint16_t *mask_lo)
{
    const uint32_t idr  = (id29 << 3) | (uint32_t)CAN_ID_EXT;
    const uint32_t mskr = (mask29 << 3) | (uint32_t)CAN_ID_EXT | (uint32_t)CAN_RTR_REMOTE;
    *id_hi   = (uint16_t)(idr  >> 16);
    *id_lo   = (uint16_t)(idr  & 0xFFFFu);
    *mask_hi = (uint16_t)(mskr >> 16);
    *mask_lo = (uint16_t)(mskr & 0xFFFFu);
}

/* Mask covering ID bits 16-24: the PF byte (bits16-23) plus the DP bit
 * (bit24) that together identify a PDU1 PGN regardless of source address
 * (bits0-7, "don't care") or destination/PS byte (bits8-15, "don't care" --
 * a claim or request can be broadcast OR addressed to whichever address we
 * currently hold). Matches n2k_can_id()'s bit placement (n2k_encode.h). */
#define N2K_FILTER_PGN_MASK   0x01FF0000u

/* ---- public API (contract: can_n2k.h) --------------------------------- */

void can_n2k_init(void)
{
    CAN_FilterTypeDef filt = {0};
    uint16_t hi, lo, mhi, mlo;

    s_hcan.Instance = CAN;
    /* 250 kbit/s from PCLK1 = 48 MHz (board_clock_config(): APB1CLKDivider
     * = DIV1, so PCLK1 = HCLK = 48 MHz, board.c). tq/bit = PCLK1 /
     * (Prescaler * bitrate) = 48e6 / (12 * 250000) = 16 tq/bit; SJW 1 +
     * TS1 13 + TS2 2 = 1+13+2 = 16 tq, sample point (1+13)/16 = 87.5% --
     * the J1939/N2K-recommended sample point. */
    s_hcan.Init.Prescaler            = 12u;
    s_hcan.Init.Mode                 = CAN_MODE_NORMAL;
    s_hcan.Init.SyncJumpWidth        = CAN_SJW_1TQ;
    s_hcan.Init.TimeSeg1             = CAN_BS1_13TQ;
    s_hcan.Init.TimeSeg2             = CAN_BS2_2TQ;
    s_hcan.Init.TimeTriggeredMode    = DISABLE;
    s_hcan.Init.AutoBusOff           = ENABLE;    /* hardware auto-recovery;
                                                   * can_n2k_note_bus_off()
                                                   * only counts, §7 R6 */
    s_hcan.Init.AutoWakeUp           = DISABLE;   /* this device never sleeps */
    s_hcan.Init.AutoRetransmission   = ENABLE;    /* standard CAN behaviour */
    s_hcan.Init.ReceiveFifoLocked    = DISABLE;   /* newest-message-wins beats
                                                   * a latched overrun flag
                                                   * for the low-traffic PGNs
                                                   * we admit */
    s_hcan.Init.TransmitFifoPriority = DISABLE;   /* ID-priority order (bxCAN
                                                   * default) is exactly what
                                                   * J1939/N2K priority means */

    if (HAL_CAN_Init(&s_hcan) != HAL_OK) { s_ok = false; return; }

    can_filter_pair(n2k_can_id(0u, N2K_PGN_ADDR_CLAIM, 0u, 0u),
                    N2K_FILTER_PGN_MASK, &hi, &lo, &mhi, &mlo);
    filt.FilterIdHigh         = hi;
    filt.FilterIdLow          = lo;
    filt.FilterMaskIdHigh     = mhi;
    filt.FilterMaskIdLow      = mlo;
    filt.FilterFIFOAssignment = CAN_FILTER_FIFO0;
    filt.FilterBank           = 0u;
    filt.FilterMode           = CAN_FILTERMODE_IDMASK;
    filt.FilterScale          = CAN_FILTERSCALE_32BIT;
    filt.FilterActivation     = CAN_FILTER_ENABLE;
    filt.SlaveStartFilterBank = 14u;   /* single-CAN part; HAL still wants a
                                       * value (stm32f0xx_hal_can.h) */
    if (HAL_CAN_ConfigFilter(&s_hcan, &filt) != HAL_OK) { s_ok = false; return; }

    can_filter_pair(n2k_can_id(0u, N2K_PGN_ISO_REQUEST, 0u, 0u),
                    N2K_FILTER_PGN_MASK, &hi, &lo, &mhi, &mlo);
    filt.FilterIdHigh     = hi;
    filt.FilterIdLow      = lo;
    filt.FilterMaskIdHigh = mhi;
    filt.FilterMaskIdLow  = mlo;
    filt.FilterBank       = 1u;
    if (HAL_CAN_ConfigFilter(&s_hcan, &filt) != HAL_OK) { s_ok = false; return; }

    if (HAL_CAN_Start(&s_hcan) != HAL_OK) { s_ok = false; return; }

    build_serial();
    build_product_info();
    {
        const uint64_t name = build_name();
        n2k_ac_init(&s_ac, name, (uint8_t)N2K_PREFERRED_ADDR);
        n2k_sched_init(&s_sched, name, &s_product, NULL, NULL, N2K_MFG_INFO);
    }
    s_ok = true;   /* set last: poll()/publish() no-op until every piece of
                   * state they touch has actually been initialised */
}

void can_n2k_poll(void)
{
    n2k_frame_t ac_out[N2K_AC_BATCH];
    int n;

    if (!s_ok) return;   /* port never came up; rest of the firmware runs on */

    can_drain_rx();

    n = n2k_ac_tick(&s_ac, HAL_GetTick(), ac_out, N2K_AC_BATCH);
    if (n > 0) can_tx_enqueue(ac_out, n);

    can_drain_tx_ring();
    can_poll_bus_off();
}

void can_n2k_publish(const ctrl_telemetry_t *t)
{
    n2k_frame_t out[N2K_SCHED_BATCH];
    int n;

    if (!s_ok || !t) return;

    n = n2k_sched_tick(&s_sched, HAL_GetTick(), n2k_ac_ready(&s_ac),
                       n2k_ac_address(&s_ac), t, out, N2K_SCHED_BATCH);
    if (n > 0) can_tx_enqueue(out, n);
}

/* §7 R6 bus-off accounting. Contract: can_n2k.h. Deliberately does NOT try to
 * force recovery — bxCAN's automatic bus-off recovery does that in hardware
 * (AutoBusOff, can_n2k_init()), and a software "fix" would only mask how
 * often it is happening. */
void can_n2k_note_bus_off(void)
{
    if (s_bus_off_events != UINT32_MAX) s_bus_off_events++;
    (void)errb_fail(ERRB_CAN);   /* REINIT rung is a no-op here: see above */
}

uint32_t can_n2k_bus_off_count(void) { return s_bus_off_events; }

uint32_t can_n2k_tx_dropped_count(void) { return s_tx_dropped; }

/* HAL MSP hook, called from inside HAL_CAN_Init: clock plumbing only. GPIO
 * AF for PB8/PB9 is already configured by board_init() (board.c) before
 * can_n2k_init() runs; this driver is polling-only (no CAN IRQ), so there is
 * no NVIC setup here (mirrors usb_cdc.c's HAL_PCD_MspInit comment density). */
void HAL_CAN_MspInit(CAN_HandleTypeDef *hcan)
{
    (void)hcan;
    __HAL_RCC_CAN1_CLK_ENABLE();
}
