/*
 * usb_cdc.h — USB CDC-ACM virtual COM port on the F072's USB FS device
 * (PA11/PA12), the physical transport under cfg_stream.h (GH#35, #20).
 *
 * Byte pipes only: this module knows nothing about JSON, lines or the config
 * protocol — it enumerates as a CDC-ACM port ("WS500-OpenFW" / "WS500
 * Regulator") and moves octets between two static rings and the bulk
 * endpoints. cfg_stream.c binds the rings to the protocol seam.
 *
 * Hard rules this driver is written to (and why):
 *   - init NEVER blocks on USB readiness. The Renode CI platform stubs the USB
 *     block as a silent Tag (renode/README.md): every register read returns 0
 *     and no interrupt ever fires, so any wait-for-bit loop would hang the
 *     whole-firmware emulation job. There is none — enumeration is entirely
 *     interrupt-driven, and a USB block that never answers simply leaves the
 *     port dead while the regulator runs on unaffected.
 *   - the ISR does bounded work: copy ≤ one 64-byte packet per event, arm the
 *     next transfer, return. No HAL_Delay, no waiting, no protocol work.
 *   - a wedged or absent host can never stall the control loop: writes beyond
 *     the TX ring are dropped (counted), reads are non-blocking, and RX flow
 *     control is USB's own NAK (the OUT endpoint is only re-armed when a full
 *     packet fits, so the HOST waits, not us).
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_USB_CDC_H
#define WS500_USB_CDC_H

#include <stdint.h>
#include <stdbool.h>

/* Bring up the USB FS device and start enumeration. Non-blocking by contract
 * (see above); on any init failure the port stays disabled, usb_cdc_ok()
 * reports it, and nothing else in the firmware is affected. Call after
 * board_init() (HSI48+CRS are the USB clock, board.c). */
void usb_cdc_init(void);

/* False when init failed and the port was left disabled. Diagnostics only —
 * a dead comms port is never a charging fault (same reasoning as ERRB_EEPROM
 * in main.c; a USB err-budget domain would gate nothing, since every USB
 * failure mode already ends in "no traffic", which every caller tolerates). */
bool usb_cdc_ok(void);

/* True once the host has configured the device (SET_CONFIGURATION). */
bool usb_cdc_configured(void);

/* True while a terminal is actually attached: configured AND the host asserts
 * DTR (CDC SET_CONTROL_LINE_STATE). This is the telemetry-stream gate — DTR is
 * the one signal every serial-terminal and libserialport-style client raises
 * on open and drops on close. */
bool usb_cdc_link_up(void);

/* Drain up to `cap` received bytes into `buf`; returns the count (0 = none).
 * Non-blocking; called from the main loop (cfg_stream.c). */
int usb_cdc_read(uint8_t *buf, int cap);

/* Queue bytes toward the host. Best-effort: returns how many were accepted;
 * the rest are dropped and counted (usb_cdc_tx_dropped). Never blocks. */
int usb_cdc_write(const uint8_t *buf, int len);

/* Lifetime count of TX bytes dropped because the ring was full or the port was
 * not configured. A steadily climbing count with a live host means the TX ring
 * is undersized for the traffic — see usb_cdc.c for the sizing rationale. */
uint32_t usb_cdc_tx_dropped(void);

#endif /* WS500_USB_CDC_H */
