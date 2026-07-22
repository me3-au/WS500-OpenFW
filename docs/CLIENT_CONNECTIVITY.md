# Client Connectivity — Programming / Firmware / Monitoring across platforms

Decision record for deliverable #8 (client app). Resolves how PC / Mac / iOS /
Android each do the three client functions over the v1 hardware's transports
(USB CDC + STM32 ROM DFU; CAN). Aligns with CONTROL_SPEC §0.1 (BLE/Wi-Fi =
⟦future-hw⟧) and §6.7 (USB DFU update, no wireless on v1).

## Functions ↔ transport

| Function | Transport on v1 |
|---|---|
| **Programming** (config read/write) | USB **CDC** (virtual COM), `$`/JSON protocol |
| **Firmware** (flash) | USB **DFU** — STM32 system bootloader in ROM (unbrickable) |
| **Monitoring** (live telemetry) | USB **CDC** stream; also **CAN/NMEA2000** out |

## Platform capability matrix

| Platform | USB serial/DFU access | Program | Firmware | Monitor |
|---|---|---|---|---|
| Windows PC | Native (COM; DFU=WinUSB) | ✅ | ✅ | ✅ |
| macOS | Native (`/dev/tty.usbmodem*`; DFU=libusb) | ✅ | ✅ | ✅ |
| Android | USB-Host API + **OTG cable** | ✅ | ✅ (fiddly) | ✅ |
| **iOS / iPadOS** | ❌ blocked for 3rd-party apps (MFi-only) | ❌ | ❌ | ⚠️ via CAN only |

**iOS constraint:** Apple restricts USB accessories to MFi-certified hardware
(security chip + program membership + External Accessory framework). Generic
CDC/DFU is unavailable; unchanged by USB-C models. No 3rd-party USB serial on iOS.

## Decision

1. **Primary: one WebSerial/WebUSB web app** (single codebase, zero install).
   - WebSerial → programming + monitoring; WebUSB + WebDFU → firmware.
   - Runs on **Chrome/Edge: Windows, macOS, Android**. Static hosting (e.g. Pages).
   - **Not iOS** — WebKit implements neither WebSerial nor WebUSB, and all iOS
     browsers are WebKit.
2. **Secondary: native `ws500ctl` CLI** (Python + pyserial + a DFU lib) for
   PC/Mac scripting, CI, and a robust flash path. Basis for deliverable #8.
3. **iOS:**
   - **Monitoring** via CAN/NMEA2000 → Victron GX (Cerbo) → VRM app (regulator
     already telemeters N2K, §8). Works through the ecosystem, not over a cable.
   - **Programming/firmware** only via ⟦future-hw⟧ wireless (BLE/Wi-Fi) or a
     USB→Wi-Fi/BLE **bridge** (ESP32/Pi gateway). Out of scope for v1.

## v1 scope summary

- **USB (PC/Mac/Android):** all three functions.
- **CAN:** monitoring everywhere including iOS (via GX/MFD).
- **iOS direct USB:** not supported (hardware/OS limit) — full iOS support is a
  future-hw wireless deliverable.

## Open items

- WebDFU reliability on Windows (WinUSB driver association) — validate; CLI is the
  fallback.
- Android firmware-flash UX over WebUSB — confirm on a target device.
- Bridge/gateway (USB→BLE/Wi-Fi) — evaluate only if iOS direct support is required
  before wireless hardware exists.
