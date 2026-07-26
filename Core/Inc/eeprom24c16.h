/*
 * eeprom24c16.h — 24C16-class config EEPROM (2 KB) on I2C2, 7-bit family 0x50.
 *
 * RAW byte-addressed driver only: read/write raw bytes at a byte offset within
 * the 2 KB device. No record format — the config schema (magic/CRC framing,
 * field layout) is a pending decision (GH#28); this driver is deliberately
 * agnostic to it, the same separation ina2xx.c keeps from control.h.
 *
 * Facts (stock binary disasm 2026-07-24, PROJECT_PLAN §0.6 V7): 24C16-class
 * device, 2 KB / 8 x 256-byte blocks / 16-byte pages; the 8-bit device address
 * on the wire is COMPUTED per access (block-select bits folded into the fixed
 * 0xA0 family nibble), not a literal constant — see board.h EEPROM_* defines.
 * Write-protected by PA15 (/WP), driven low only for the duration of a write.
 *
 * BUS CAVEAT (mirrors ina2xx.c, GH#36): V7 places BOTH the EEPROM and the
 * INA226 on I2C2 (PB10/PB11); ina2xx.c has not yet been flipped off I2C1
 * pending a bench scope of PB6/7 vs PB10/11. This driver follows V7's
 * resolved call (I2C2) since that is the newer, EEPROM-specific finding —
 * re-confirm both together at the same bench session.
 *
 * SPDX-License-Identifier: MIT
 */
#ifndef WS500_EEPROM24C16_H
#define WS500_EEPROM24C16_H

#include <stdint.h>
#include <stdbool.h>

/* Bring up I2C2 and leave /WP protected (HIGH). Returns false if the I2C2
 * peripheral fails to configure. */
bool eeprom24c16_init(void);

/* Raw read, chunked internally at 256-byte block boundaries (§0.6 V7). Returns
 * false on an out-of-range request (addr+len > EEPROM_SIZE_BYTES) or after
 * exhausting retries on any chunk; buf may hold a partial result up to the
 * point of failure in that case. */
bool eeprom24c16_read(uint16_t addr, uint8_t *buf, uint16_t len);

/* Raw write, chunked internally at 16-byte page boundaries with a bounded
 * ~7 ms write-cycle wait per page (§0.6 V7; HAL_Delay stands in for the
 * stock's RTOS osDelay — this firmware is bare-metal). /WP is driven low for
 * the duration of the call and restored high before returning, success or
 * failure. This call BLOCKS the caller for roughly
 * EEPROM_WRITE_CYCLE_MS x (pages touched) — call it from config-write paths
 * (e.g. config_protocol.c on a config change), never from the 10 ms control
 * loop. */
bool eeprom24c16_write(uint16_t addr, const uint8_t *buf, uint16_t len);

/* Health (§7 R6 style, mirrors ina2xx.c): false once the consecutive-failure
 * budget is exhausted; self-recovers on the next successful transaction. The
 * cumulative failure counter never resets (telemetry/diagnostics). */
bool     eeprom24c16_ok(void);
uint32_t eeprom24c16_xact_failures(void);

#endif /* WS500_EEPROM24C16_H */
