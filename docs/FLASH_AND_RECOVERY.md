# WS500-OpenFW — Flash / Update / Rollback / Backup / Recovery

> **Extracted from PROJECT_PLAN.md §6 on 2026-07-26.**
> Authoritative details, updates, and the ongoing tracker remain in PROJECT_PLAN.md.
>
> **V7 correction (from PROJECT_PLAN §0.6):** the config store is a **24C16-class 2 KB serial EEPROM at 7-bit 0x50**, write-protected by **PA15 = /WP** (driven low around writes), on **I²C2**. This resolves the earlier uncertainty in §6 — config is stored externally, not in internal flash.

---

## Chip Facts

**STM32F072xB** = 128 KB single-bank flash → no A/B slots (not worth halving flash).

---

## Unbrickable Floor: ROM DFU Bootloader

The ST **system DFU bootloader in ROM** cannot be erased.

**DFU entry is already available and documented (confirmed 2026-07-24):** the manual's firmware-upgrade procedure — **press-and-hold the reset button** — enters the ST ROM DFU bootloader (the stock `.dfu` carries VID 0x0483 / PID 0xDF11, ST's system-bootloader IDs; the device re-enumerates as "STM32 BOOTLOADER"). So we do **not** need to locate a BOOT0 access point to enter DFU — that IO_COVERAGE open item is resolved for the entry path (BOOT0 still relevant only as a fallback if the app ever won't hand off).

**M1 hook:** the read-only full-flash backup + RDP check + clean DFU exit is the concrete M1 rehearsal; needs dfu-util or STM32CubeProgrammer (not yet installed). Discipline: **read/upload only, never write/erase, never touch RDP** (disabling RDP mass-erases the stock image).

---

## Backup First

Full SWD flash readout of the stock unit before anything. ⚠️ If RDP ≥1 is set, readout is blocked and disabling it mass-erases — then the stock DFU image file we already hold *is* the backup; prove it restores before relying on it.

---

## Rollback

**Rollback = restore stock image via DFU** (rehearsed procedure).

---

## Update Path

**Stock ROM DFU** (`dfu-util`), driven by `ws500ctl`. A CRC-checked, config-preserving custom bootloader is a *later* nice-to-have, not a dependency.

---

## Config Survives Updates

Last flash page(s), outside the app image, CRC + version; `ws500ctl` exports/imports as text.

**Configuration storage note:** as confirmed in PROJECT_PLAN §0.6 V7 (2026-07-24), config lives in an **external I²C EEPROM (@0x50 on I²C2, 24C16-class, /WP = PA15)**, not internal flash pages. The update mechanism must preserve this EEPROM across flash rewrites.

---

## No Flash Protections, Ever (Decided)

Our firmware **never sets RDP or WRP**. The chip stays fully readable/reflashable via SWD and DFU — recovery is never locked out. (The *stock* unit may still ship with RDP set, affecting only the backup step above.)
