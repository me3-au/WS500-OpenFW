/*
 * usb_cdc.c — minimal CDC-ACM device on the STM32F072 USB FS peripheral
 * (GH#35, deliverable #20). Contract and the never-block rules: usb_cdc.h.
 *
 * MIDDLEWARE DECISION (GH#35): this is a self-written CDC-ACM class + EP0
 * control state machine (MIT) layered on the ALREADY-VENDORED BSD-3 HAL PCD
 * driver (stm32f0xx_hal_pcd.c — part of the STM32Cube HAL that
 * scripts/fetch_deps.sh pins, license-approved in CLAUDE.md). ST's own USB
 * Device middleware (STM32_USB_Device_Library) was rejected: STM32CubeF0
 * ships it under SLA0044 ("Ultimate Liberty"), which restricts use to ST
 * silicon — not permissive, so it fails the repo's license rule outright.
 * What the middleware would have provided is exactly the bounded scope written
 * here instead: fixed descriptors, the EP0 request machine, and three data
 * endpoints. The HAL PCD layer keeps all PMA/BTABLE/toggle-bit register
 * handling in code that is already vendored, licensed, and compiled.
 *
 * Endpoint map (fixed, single-configuration):
 *   EP0  IN/OUT  64 B control          PMA 0x040 (TX) / 0x080 (RX)
 *   0x81 IN      64 B bulk  (device→host data)      PMA 0x0C0
 *   0x01 OUT     64 B bulk  (host→device data)      PMA 0x100
 *   0x82 IN       8 B interrupt (CDC notifications, mandatory but idle)
 *                                                    PMA 0x140
 * BTABLE sits at PMA 0; the first allocation starts at 0x40 so the 8-byte-per-
 * endpoint table can never collide. Total PMA use 0x148 of 1024 bytes.
 *
 * VID/PID are pid.codes' TEST pair (0x1209/0x0001) — honest for a development
 * firmware that has no assigned identity, and CDC-ACM binds by CLASS code on
 * every modern OS so enumeration does not depend on the IDs. Registering a
 * permanent pid.codes PID before any release is an open GH#35 item.
 *
 * RENODE/CI NOTE: in the emulation job every register write below lands in a
 * silent Tag and every read returns 0 (renode/README.md). This whole file was
 * audited against that: HAL_PCD_Init/PMAConfig/Start contain no status-bit
 * waits (verified in stm32f0xx_{hal_pcd,ll_usb}.c — the only `while` is the
 * ISR's pending-CTR drain, which exits instantly on a 0 read), and the USB
 * interrupt never fires there, so the port is simply dead and the firmware
 * runs on. The emulation job passing IS the no-blocking-init proof.
 *
 * SPDX-License-Identifier: MIT
 */
#include "usb_cdc.h"
#include "stm32f0xx_hal.h"
#include <string.h>

/* ---- sizing ---------------------------------------------------------------- */

#define CDC_EP_MPS      64u     /* FS bulk max packet size */
#define CDC_EP_OUT      0x01u
#define CDC_EP_IN       0x81u
#define CDC_EP_CMD      0x82u
#define CDC_CMD_MPS     8u

/*
 * RX ring: 256 B = four bulk packets. Correctness never depends on this size —
 * the OUT endpoint is re-armed only while a full 64-byte packet fits, so when
 * the ring is full the endpoint NAKs and the HOST queues (USB's own flow
 * control). 256 matches config_poll()'s per-iteration drain quota
 * (CONFIG_POLL_CHUNKS × CONFIG_POLL_BYTES = 4 × 64), so one poll can always
 * empty everything the ring can hold: steady-state bulk throughput is bounded
 * by the main loop, not the buffer.
 *
 * TX ring: 3072 B = CFG_MSG_LINE_MAX. The largest reply the firmware emits is
 * the cfg-get document (~2.6 KB), produced synchronously inside one
 * config_poll() call — faster than the ISR can drain it — and cfg_proto_tx's
 * contract says "doesn't fit → DROPPED". Sizing the ring to the protocol's own
 * line bound (which test_protocol.c pins against the real emitter, so it
 * cannot rot) guarantees a full worst-case reply is never dropped merely
 * because it was produced in one burst. Cost: 3.3 KB of the part's 16 KB RAM
 * for both rings — the price of a config surface that works; the .map in CI
 * tracks the total.
 */
#define CDC_RX_RING     256u
#define CDC_TX_RING     3072u

/* ---- device state ---------------------------------------------------------- */

static PCD_HandleTypeDef s_pcd;
static bool              s_ok;          /* init succeeded; port is live */
static volatile bool     s_configured;  /* SET_CONFIGURATION(1) seen */
static volatile bool     s_dtr;         /* host asserted DTR */

/* EP0 control-transfer state. Only the IN data stage needs continuation state:
 * the config descriptor (67 B) is the one transfer above one packet. */
typedef enum {
    EP0_IDLE = 0,
    EP0_DATA_IN,      /* streaming a descriptor/value to the host */
    EP0_DATA_OUT,     /* expecting SET_LINE_CODING's 7 data bytes */
    EP0_STATUS_IN,    /* we sent/queued the ZLP status */
    EP0_STATUS_OUT    /* host owes us the ZLP status */
} ep0_state_t;

static volatile ep0_state_t s_ep0_state;
static const uint8_t       *s_ep0_ptr;   /* remaining IN data (flash/static) */
static volatile uint16_t    s_ep0_rem;
static uint8_t              s_ep0_buf[64];   /* string descriptors, GET_* replies */

/* CDC line coding, 115200 8N1 default. Stored on SET, echoed on GET — the
 * values gate nothing (there is no UART behind this port). */
static uint8_t s_line_coding[7] = { 0x00, 0xC2, 0x01, 0x00, 0x00, 0x00, 0x08 };

/* ---- rings (SPSC: ISR is RX producer / TX consumer, main is the opposite) -- */

static uint8_t           s_rx_ring[CDC_RX_RING];
static volatile uint16_t s_rx_w, s_rx_r;
static volatile bool     s_rx_paused;        /* OUT ep left NAK: ring was full */
static uint8_t           s_rx_pkt[CDC_EP_MPS];

static uint8_t           s_tx_ring[CDC_TX_RING];
static volatile uint16_t s_tx_w, s_tx_r;
static volatile bool     s_tx_busy;          /* a bulk IN transfer is in flight */
static volatile uint16_t s_tx_last_len;      /* for the terminating ZLP */
static uint8_t           s_tx_pkt[CDC_EP_MPS];
static uint32_t          s_tx_dropped;

static inline uint16_t rx_used(void) { return (uint16_t)((s_rx_w - s_rx_r + CDC_RX_RING) % CDC_RX_RING); }
static inline uint16_t rx_free(void) { return (uint16_t)(CDC_RX_RING - 1u - rx_used()); }
static inline uint16_t tx_used(void) { return (uint16_t)((s_tx_w - s_tx_r + CDC_TX_RING) % CDC_TX_RING); }
static inline uint16_t tx_free(void) { return (uint16_t)(CDC_TX_RING - 1u - tx_used()); }

/* Main-loop side masks the USB IRQ around anything the ISR also touches
 * (endpoint arming, the busy/paused flags). Cheap, and the ISR itself never
 * needs the lock. */
static inline void cdc_lock(void)   { NVIC_DisableIRQ(USB_IRQn); __DSB(); __ISB(); }
static inline void cdc_unlock(void) { NVIC_EnableIRQ(USB_IRQn); }

/* ---- descriptors ----------------------------------------------------------- */

#define CDC_VID  0x1209u   /* pid.codes */
#define CDC_PID  0x0001u   /* pid.codes TEST PID — see file header */

static const uint8_t DESC_DEVICE[18] = {
    18, 0x01,                   /* bLength, DEVICE */
    0x00, 0x02,                 /* bcdUSB 2.00 */
    0x02, 0x00, 0x00,           /* class CDC, no subclass/protocol at device level */
    64,                         /* bMaxPacketSize0 */
    (uint8_t)(CDC_VID & 0xFFu), (uint8_t)(CDC_VID >> 8),
    (uint8_t)(CDC_PID & 0xFFu), (uint8_t)(CDC_PID >> 8),
    0x00, 0x01,                 /* bcdDevice 1.00 — FW identity travels in the
                                 * hello handshake, not here (VERSIONING §2) */
    1, 2, 3,                    /* iManufacturer, iProduct, iSerialNumber */
    1                           /* bNumConfigurations */
};

/* One configuration: standard 2-interface CDC-ACM (comm + data), 67 bytes. */
static const uint8_t DESC_CONFIG[67] = {
    /* configuration */
    9, 0x02, 67, 0, 2, 1, 0,
    0xC0,                       /* self-powered: this device runs off the bank */
    1,                          /* 2 mA — a token draw for the transceiver */
    /* interface 0: communications class, ACM */
    9, 0x04, 0, 0, 1, 0x02, 0x02, 0x01, 0,
    /* CDC header functional, bcdCDC 1.10 */
    5, 0x24, 0x00, 0x10, 0x01,
    /* call management: none handled, data interface 1 */
    5, 0x24, 0x01, 0x00, 0x01,
    /* ACM functional: line coding + control line state supported */
    4, 0x24, 0x02, 0x02,
    /* union: control interface 0, subordinate 1 */
    5, 0x24, 0x06, 0x00, 0x01,
    /* notification endpoint 0x82, interrupt, 8 B, 16 ms */
    7, 0x05, CDC_EP_CMD, 0x03, CDC_CMD_MPS, 0x00, 0x10,
    /* interface 1: data class */
    9, 0x04, 1, 0, 2, 0x0A, 0x00, 0x00, 0,
    /* data endpoints: bulk OUT 0x01, bulk IN 0x81, 64 B */
    7, 0x05, CDC_EP_OUT, 0x02, CDC_EP_MPS, 0x00, 0x00,
    7, 0x05, CDC_EP_IN,  0x02, CDC_EP_MPS, 0x00, 0x00,
};

static const uint8_t DESC_LANGID[4] = { 4, 0x03, 0x09, 0x04 };   /* en-US */

static const char STR_MANUFACTURER[] = "WS500-OpenFW";
static const char STR_PRODUCT[]      = "WS500 Regulator";

/* Build a UTF-16LE string descriptor from ASCII into s_ep0_buf. */
static uint16_t str_desc_ascii(const char *s)
{
    uint16_t n = 2;
    while (*s != '\0' && n + 2u <= sizeof s_ep0_buf) {
        s_ep0_buf[n++] = (uint8_t)*s++;
        s_ep0_buf[n++] = 0;
    }
    s_ep0_buf[0] = (uint8_t)n;
    s_ep0_buf[1] = 0x03;
    return n;
}

/* Serial number: the 96-bit device UID as 24 hex chars, so every WS500 keeps a
 * stable COM-port identity across replugs. Read only during enumeration, which
 * cannot happen in the Renode job (no host), so the Tag'd system-memory read
 * there is unreachable. */
static uint16_t str_desc_serial(void)
{
    static const char HEX[] = "0123456789ABCDEF";
    const volatile uint32_t *uid = (const volatile uint32_t *)UID_BASE;
    uint16_t n = 2;
    for (int w = 0; w < 3; w++) {
        const uint32_t v = uid[w];
        for (int nib = 7; nib >= 0; nib--) {
            s_ep0_buf[n++] = (uint8_t)HEX[(v >> (nib * 4)) & 0xFu];
            s_ep0_buf[n++] = 0;
        }
    }
    s_ep0_buf[0] = (uint8_t)n;
    s_ep0_buf[1] = 0x03;
    return n;
}

/* ---- EP0 helpers ----------------------------------------------------------- */

static void ep0_stall(void)
{
    (void)HAL_PCD_EP_SetStall(&s_pcd, 0x80u);
    (void)HAL_PCD_EP_SetStall(&s_pcd, 0x00u);
    s_ep0_state = EP0_IDLE;
}

/* Start an IN data stage: clamp to the host's wLength, send the first packet;
 * DataInStage continues the rest. `src` must stay valid until the transfer
 * completes (flash constants or the static s_ep0_buf — both do). */
static void ep0_send(const uint8_t *src, uint16_t len, uint16_t wlength)
{
    if (len > wlength) len = wlength;
    s_ep0_ptr   = src;
    s_ep0_rem   = len;
    s_ep0_state = EP0_DATA_IN;

    const uint16_t chunk = (len > CDC_EP_MPS) ? CDC_EP_MPS : len;
    s_ep0_ptr += chunk;
    s_ep0_rem  = (uint16_t)(len - chunk);
    (void)HAL_PCD_EP_Transmit(&s_pcd, 0x80u, (uint8_t *)(uintptr_t)src, chunk);
}

static void ep0_status_in(void)
{
    s_ep0_state = EP0_STATUS_IN;
    (void)HAL_PCD_EP_Transmit(&s_pcd, 0x80u, NULL, 0u);
}

static void ep0_status_out(void)
{
    s_ep0_state = EP0_STATUS_OUT;
    (void)HAL_PCD_EP_Receive(&s_pcd, 0x00u, NULL, 0u);
}

/* ---- bulk data plumbing ---------------------------------------------------- */

/* Start the next bulk IN transfer if idle. ISR context, or main with the USB
 * IRQ masked. Sends ≤ one packet; a full-size final packet is chased with a
 * ZLP so the host's read completes instead of waiting for more. */
static void tx_kick(void)
{
    if (!s_configured || s_tx_busy) return;

    uint16_t n = tx_used();
    if (n == 0u) {
        if (s_tx_last_len == CDC_EP_MPS) {       /* terminate the transfer */
            s_tx_last_len = 0u;
            s_tx_busy     = true;
            (void)HAL_PCD_EP_Transmit(&s_pcd, CDC_EP_IN, NULL, 0u);
        }
        return;
    }
    if (n > CDC_EP_MPS) n = CDC_EP_MPS;

    uint16_t r = s_tx_r;
    for (uint16_t i = 0; i < n; i++) {
        s_tx_pkt[i] = s_tx_ring[r];
        r = (uint16_t)((r + 1u) % CDC_TX_RING);
    }
    s_tx_r        = r;
    s_tx_last_len = n;
    s_tx_busy     = true;
    (void)HAL_PCD_EP_Transmit(&s_pcd, CDC_EP_IN, s_tx_pkt, n);
}

/* Arm bulk OUT reception iff a whole packet fits — this NAK-based flow control
 * is what makes RX correctness independent of ring size (usb_cdc.h). */
static void rx_arm(void)
{
    if (rx_free() >= CDC_EP_MPS) {
        s_rx_paused = false;
        (void)HAL_PCD_EP_Receive(&s_pcd, CDC_EP_OUT, s_rx_pkt, CDC_EP_MPS);
    } else {
        s_rx_paused = true;
    }
}

/* ---- SETUP dispatch (ISR context via HAL_PCD_SetupStageCallback) ----------- */

static void setup_get_descriptor(uint16_t wvalue, uint16_t wlength)
{
    const uint8_t type = (uint8_t)(wvalue >> 8);
    const uint8_t idx  = (uint8_t)(wvalue & 0xFFu);

    switch (type) {
    case 0x01:   /* DEVICE */
        ep0_send(DESC_DEVICE, sizeof DESC_DEVICE, wlength);
        return;
    case 0x02:   /* CONFIGURATION */
        ep0_send(DESC_CONFIG, sizeof DESC_CONFIG, wlength);
        return;
    case 0x03:   /* STRING */
        switch (idx) {
        case 0:  ep0_send(DESC_LANGID, sizeof DESC_LANGID, wlength);        return;
        case 1:  ep0_send(s_ep0_buf, str_desc_ascii(STR_MANUFACTURER), wlength); return;
        case 2:  ep0_send(s_ep0_buf, str_desc_ascii(STR_PRODUCT), wlength);      return;
        case 3:  ep0_send(s_ep0_buf, str_desc_serial(), wlength);                return;
        default: break;
        }
        break;
    default:     /* qualifier/BOS/other: an FS-only device stalls these */
        break;
    }
    ep0_stall();
}

static void setup_set_configuration(uint16_t wvalue)
{
    if ((wvalue & 0xFFu) == 1u) {
        (void)HAL_PCD_EP_Open(&s_pcd, CDC_EP_OUT, CDC_EP_MPS, EP_TYPE_BULK);
        (void)HAL_PCD_EP_Open(&s_pcd, CDC_EP_IN,  CDC_EP_MPS, EP_TYPE_BULK);
        (void)HAL_PCD_EP_Open(&s_pcd, CDC_EP_CMD, CDC_CMD_MPS, EP_TYPE_INTR);
        s_configured  = true;
        s_tx_busy     = false;
        s_tx_last_len = 0u;
        rx_arm();
        tx_kick();               /* flush anything queued while unconfigured */
        ep0_status_in();
    } else if ((wvalue & 0xFFu) == 0u) {
        s_configured = false;
        s_dtr        = false;
        (void)HAL_PCD_EP_Close(&s_pcd, CDC_EP_OUT);
        (void)HAL_PCD_EP_Close(&s_pcd, CDC_EP_IN);
        (void)HAL_PCD_EP_Close(&s_pcd, CDC_EP_CMD);
        ep0_status_in();
    } else {
        ep0_stall();
    }
}

static void setup_standard(uint8_t bm, uint8_t req, uint16_t wvalue,
                           uint16_t windex, uint16_t wlength)
{
    switch (req) {
    case 0x06:   /* GET_DESCRIPTOR (device recipient only) */
        if ((bm & 0x1Fu) == 0u) { setup_get_descriptor(wvalue, wlength); return; }
        break;
    case 0x05:   /* SET_ADDRESS — HAL defers the DADDR write to after the
                  * status stage completes (stm32f0xx_hal_pcd.c). */
        (void)HAL_PCD_SetAddress(&s_pcd, (uint8_t)(wvalue & 0x7Fu));
        ep0_status_in();
        return;
    case 0x09:   /* SET_CONFIGURATION */
        setup_set_configuration(wvalue);
        return;
    case 0x08:   /* GET_CONFIGURATION */
        s_ep0_buf[0] = s_configured ? 1u : 0u;
        ep0_send(s_ep0_buf, 1u, wlength);
        return;
    case 0x00:   /* GET_STATUS */
        s_ep0_buf[0] = ((bm & 0x1Fu) == 0u) ? 0x01u : 0x00u;   /* self-powered */
        s_ep0_buf[1] = 0u;
        ep0_send(s_ep0_buf, 2u, wlength);
        return;
    case 0x01:   /* CLEAR_FEATURE: ENDPOINT_HALT is the only one that matters */
        if ((bm & 0x1Fu) == 2u) (void)HAL_PCD_EP_ClrStall(&s_pcd, (uint8_t)windex);
        ep0_status_in();
        return;
    case 0x03:   /* SET_FEATURE — nothing supported, but ACK per spec */
        ep0_status_in();
        return;
    case 0x0A:   /* GET_INTERFACE — no alternate settings */
        s_ep0_buf[0] = 0u;
        ep0_send(s_ep0_buf, 1u, wlength);
        return;
    case 0x0B:   /* SET_INTERFACE — alt 0 only */
        if (wvalue == 0u) { ep0_status_in(); return; }
        break;
    default:
        break;
    }
    ep0_stall();
}

static void setup_class(uint8_t bm, uint8_t req, uint16_t wvalue, uint16_t wlength)
{
    switch (req) {
    case 0x20:   /* SET_LINE_CODING: 7 data bytes follow on EP0 OUT */
        if ((bm & 0x80u) == 0u && wlength == sizeof s_line_coding) {
            s_ep0_state = EP0_DATA_OUT;
            (void)HAL_PCD_EP_Receive(&s_pcd, 0x00u, s_ep0_buf, sizeof s_line_coding);
            return;
        }
        break;
    case 0x21:   /* GET_LINE_CODING */
        if ((bm & 0x80u) != 0u) {
            memcpy(s_ep0_buf, s_line_coding, sizeof s_line_coding);
            ep0_send(s_ep0_buf, sizeof s_line_coding, wlength);
            return;
        }
        break;
    case 0x22:   /* SET_CONTROL_LINE_STATE: bit0 = DTR (the telemetry gate) */
        if ((bm & 0x80u) == 0u) {
            s_dtr = (wvalue & 0x0001u) != 0u;
            ep0_status_in();
            return;
        }
        break;
    default:
        break;
    }
    ep0_stall();
}

/* ---- HAL PCD callbacks (all ISR context) ----------------------------------- */

void HAL_PCD_SetupStageCallback(PCD_HandleTypeDef *hpcd)
{
    const uint8_t *s = (const uint8_t *)hpcd->Setup;
    const uint8_t  bm      = s[0];
    const uint8_t  req     = s[1];
    const uint16_t wvalue  = (uint16_t)(s[2] | ((uint16_t)s[3] << 8));
    const uint16_t windex  = (uint16_t)(s[4] | ((uint16_t)s[5] << 8));
    const uint16_t wlength = (uint16_t)(s[6] | ((uint16_t)s[7] << 8));

    switch (bm & 0x60u) {
    case 0x00u: setup_standard(bm, req, wvalue, windex, wlength); break;
    case 0x20u: setup_class(bm, req, wvalue, wlength);            break;
    default:    ep0_stall();                                      break;
    }
}

void HAL_PCD_DataInStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    (void)hpcd;
    if (epnum == 0u) {
        if (s_ep0_state == EP0_DATA_IN && s_ep0_rem > 0u) {
            const uint16_t chunk = (s_ep0_rem > CDC_EP_MPS) ? CDC_EP_MPS
                                                            : s_ep0_rem;
            const uint8_t *src = s_ep0_ptr;
            s_ep0_ptr += chunk;
            s_ep0_rem  = (uint16_t)(s_ep0_rem - chunk);
            (void)HAL_PCD_EP_Transmit(&s_pcd, 0x80u, (uint8_t *)(uintptr_t)src, chunk);
        } else if (s_ep0_state == EP0_DATA_IN) {
            ep0_status_out();      /* data stage done — accept the host's ZLP */
        }
        /* EP0_STATUS_IN completing needs no action (HAL applies any deferred
         * address itself); the next SETUP resets the state machine. */
        return;
    }
    if ((epnum & 0x7Fu) == (CDC_EP_IN & 0x7Fu)) {
        s_tx_busy = false;
        tx_kick();
    }
}

void HAL_PCD_DataOutStageCallback(PCD_HandleTypeDef *hpcd, uint8_t epnum)
{
    if (epnum == 0u) {
        if (s_ep0_state == EP0_DATA_OUT) {       /* SET_LINE_CODING payload */
            memcpy(s_line_coding, s_ep0_buf, sizeof s_line_coding);
            ep0_status_in();
        }
        return;
    }
    if (epnum == (CDC_EP_OUT & 0x7Fu)) {
        uint16_t n = (uint16_t)HAL_PCD_EP_GetRxCount(hpcd, CDC_EP_OUT);
        if (n > CDC_EP_MPS) n = CDC_EP_MPS;      /* cannot happen; belt+braces */
        uint16_t w = s_rx_w;
        for (uint16_t i = 0; i < n; i++) {       /* space guaranteed by rx_arm() */
            s_rx_ring[w] = s_rx_pkt[i];
            w = (uint16_t)((w + 1u) % CDC_RX_RING);
        }
        s_rx_w = w;
        rx_arm();
    }
}

void HAL_PCD_ResetCallback(PCD_HandleTypeDef *hpcd)
{
    s_configured  = false;
    s_dtr         = false;
    s_tx_busy     = false;
    s_rx_paused   = false;
    s_tx_last_len = 0u;
    s_ep0_state   = EP0_IDLE;
    (void)HAL_PCD_EP_Open(hpcd, 0x00u, 64u, EP_TYPE_CTRL);
    (void)HAL_PCD_EP_Open(hpcd, 0x80u, 64u, EP_TYPE_CTRL);
}

/* Suspend = host gone quiet (or cable pulled on a self-powered device): drop
 * the DTR gate so telemetry stops streaming into a void. */
void HAL_PCD_SuspendCallback(PCD_HandleTypeDef *hpcd)
{
    (void)hpcd;
    s_dtr = false;
}

/* HAL MSP hook, called from inside HAL_PCD_Init: clock + interrupt plumbing.
 * Lowest IRQ priority on purpose — comms must never delay the control tick or
 * the field-drive/stator interrupts. */
void HAL_PCD_MspInit(PCD_HandleTypeDef *hpcd)
{
    (void)hpcd;
    __HAL_RCC_USB_CONFIG(RCC_USBCLKSOURCE_HSI48);   /* CRS-trimmed, board.c */
    __HAL_RCC_USB_CLK_ENABLE();
    HAL_NVIC_SetPriority(USB_IRQn, 3, 0);
    HAL_NVIC_EnableIRQ(USB_IRQn);
}

/* Claimed vector (startup weak-aliases it to Default_Handler otherwise, which
 * fault_handlers.c treats as an unexpected IRQ). */
void USB_IRQHandler(void)
{
    HAL_PCD_IRQHandler(&s_pcd);
}

/* ---- public API ------------------------------------------------------------ */

void usb_cdc_init(void)
{
    s_pcd.Instance                 = USB;
    s_pcd.Init.dev_endpoints       = 8;
    s_pcd.Init.speed               = PCD_SPEED_FULL;
    s_pcd.Init.ep0_mps             = EP_MPS_64;
    s_pcd.Init.phy_itface          = PCD_PHY_EMBEDDED;
    s_pcd.Init.Sof_enable          = 0;
    s_pcd.Init.low_power_enable    = 0;
    s_pcd.Init.lpm_enable          = 0;
    s_pcd.Init.battery_charging_enable = 0;

    if (HAL_PCD_Init(&s_pcd) != HAL_OK) {
        /* Port stays disabled; firmware is fully functional without it
         * (usb_cdc.h explains why this is not an err-budget domain). */
        s_ok = false;
        return;
    }

    /* PMA layout per the file-header map. Bookkeeping only — no bus access. */
    (void)HAL_PCDEx_PMAConfig(&s_pcd, 0x00u,      PCD_SNG_BUF, 0x040u);
    (void)HAL_PCDEx_PMAConfig(&s_pcd, 0x80u,      PCD_SNG_BUF, 0x080u);
    (void)HAL_PCDEx_PMAConfig(&s_pcd, CDC_EP_IN,  PCD_SNG_BUF, 0x0C0u);
    (void)HAL_PCDEx_PMAConfig(&s_pcd, CDC_EP_OUT, PCD_SNG_BUF, 0x100u);
    (void)HAL_PCDEx_PMAConfig(&s_pcd, CDC_EP_CMD, PCD_SNG_BUF, 0x140u);

    s_ok = (HAL_PCD_Start(&s_pcd) == HAL_OK);
}

bool usb_cdc_ok(void)         { return s_ok; }
bool usb_cdc_configured(void) { return s_configured; }
bool usb_cdc_link_up(void)    { return s_configured && s_dtr; }

int usb_cdc_read(uint8_t *buf, int cap)
{
    if (buf == NULL || cap <= 0) return 0;

    int      n = 0;
    uint16_t r = s_rx_r;
    while (n < cap && r != s_rx_w) {
        buf[n++] = s_rx_ring[r];
        r = (uint16_t)((r + 1u) % CDC_RX_RING);
    }
    s_rx_r = r;

    /* If reception stalled on a full ring, the drain above may have made room. */
    if (s_rx_paused && s_ok) {
        cdc_lock();
        if (s_rx_paused) rx_arm();
        cdc_unlock();
    }
    return n;
}

int usb_cdc_write(const uint8_t *buf, int len)
{
    if (buf == NULL || len <= 0) return 0;
    if (!s_ok || !s_configured) {
        s_tx_dropped += (uint32_t)len;   /* nobody listening — cheap honesty */
        return 0;
    }

    int      n = 0;
    uint16_t w = s_tx_w;
    while (n < len && tx_free() > 0u) {
        s_tx_ring[w] = buf[n++];
        w = (uint16_t)((w + 1u) % CDC_TX_RING);
        s_tx_w = w;
    }
    if (n < len) s_tx_dropped += (uint32_t)(len - n);

    if (n > 0) {
        cdc_lock();
        tx_kick();
        cdc_unlock();
    }
    return n;
}

uint32_t usb_cdc_tx_dropped(void) { return s_tx_dropped; }
