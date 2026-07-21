# WS500 I/O Coverage Map

Status of the reverse-engineered hardware I/O. Confidence: ✅ confirmed from firmware ·
🟡 partial / inferred · 🔴 not yet determined · 🔵 needs board/schematic (not in firmware).

MCU: **STM32F072xB** (Cortex-M0, 128 KB/16 KB, HSI48+CRS). Package **TBD** (confirm from
board): GPIOD/GPIOE base addresses are referenced in init code, which would imply a ≥64/100-
pin part, but no GPIOD/GPIOE *pin function* is actually mapped (highest mapped port is GPIOC).
See `WS500_HARDWARE_SPEC.md` for the derivation of every item below.

## Confirmed I/O ✅

| Function | Pin / peripheral | Notes |
|---|---|---|
| Field-drive PWM (primary) | PA8 — TIM1_CH1 (AF2) | 1 kHz-class PWM to alternator field |
| Field-drive PWM (aux/compl) | PB15 — TIM1_CH3N (AF2) | role vs driver topology 🔵 |
| Field fault cutoff | TIM1 BKIN (break input) | hardware field-off on fault line |
| Alt temp (ATS) | ADC PA1 **or** PA2 — NTC β3950, −40…160 °C | which of PA1/PA2 🔵 |
| Regulator internal temp | the other of PA1/PA2 — NTC β3950 | inferred 🟡 |
| Battery temp (BTS) | ADC PA3 — NTC β3380, −40…140 °C | |
| Battery voltage | ADC PC5 (IN15) — 34.33:1 divider | |
| ADC calibration | internal temp / VREFINT / VBAT (slots 4–6) | 7-ch scan, ×4 oversample |
| Charge current | **INA2xx I²C @ 0x40**, SHUNT_V reg 0x01 | INA226/228/238 auto-detect |
| Bus voltage at shunt | INA2xx BUS_V reg 0x02 | batt or alt per ShuntAtBat |
| Stator / RPM | TIM2 input capture | exact pin 🔴 |
| CAN (RV-C/N2K/J1939/Victron) | PB8 = RX, PB9 = TX (AF4) | |
| I²C1 | PB6 = SCL, PB7 = SDA (AF1) | hosts the INA2xx |
| I²C2 | PB10 = SCL, PB11 = SDA (AF1) | device(s) 🔴 |
| USB FS (CDC config) | PA11 = DM, PA12 = DP | `$XXX:` protocol channel |
| Watchdog | IWDG | safety |
| ADC DMA | DMA1 | circular scan |

## Partial / not yet determined

| Function | Status | What's needed |
|---|---|---|
| Battery-capacity DIP switches | ✅ PA4, PA5, PA6 (+ PB0/PB1 likely) | confirmed — read in sequence & packed into a binary code (BC_Index) |
| Control input PB13 | 🟡 confirmed input, polled, gates a branch | = Enable/Ignition **or** Feature-In; splitting the two reads control logic → bench |
| Status outputs PA0/PA9/PA15/PB14 | 🟡 confirmed outputs, driven 0/1 by state | Lamp/Feature-Out (wire 2) + status LEDs; which is which → bench |
| Aux timers TIM3 / TIM7 / TIM15-17 | 🟡 mostly a HAL clock-dispatch chain | confirm if any drives Feature-Out PWM / a tick; TIM7 likely a time base |
| Secondary I²C devices 0x0C / 0x10 / 0x4C | 🔴 identity unknown | read their register access; candidates: EEPROM, GPIO/temp expander, variant sensor |
| Config storage location | 🟡 internal flash likely (FLASH_IF used) | confirm flash page vs I²C EEPROM |
| Digital I/O pin roster | 🟡 pins known, functions not | assign each of the 12 seen GPIO pins |

## Needs board / schematic (not recoverable from firmware) 🔵

| Item | Why |
|---|---|
| MCU package (48/64/100-pin) | GPIOE use ⇒ likely 100-pin, but confirm |
| Field-driver topology (P-type/N-type, gate driver, MOSFET) | analog circuit, not in code |
| Analog front-end resistor values | divider *ratio* (34.33) is in FW; exact R's are board |
| Which PA1/PA2 = ATS vs internal | both identical β3950 in FW |
| SWD header (PA13/PA14) + BOOT0 access | for flashing/recovery (PROJECT_PLAN M1) |
| RDP readout-protection level | determines if stock flash can be backed up via SWD |

## Summary

**Complete:** the full sensing + actuation chain — field PWM (+fault cutoff), all
temperatures, battery voltage, current & bus voltage (INA2xx), RPM, CAN, USB config, I²C1.
This is enough to build and bench the core regulator.

**Remaining firmware traces (in priority order):**
1. Digital I/O *labels* — pins are found (in: PA4/PA5/PB0/PB1 DIP, PB13 enable/feature;
   out: PA0/PA9/PA15/PB14). Still need which input = Enable vs Feature-In, and which
   output = Lamp vs status LED (trace each pin's usage).
2. Stator input pin (which TIM2 channel).
3. Secondary I²C devices (0x0C/0x10/0x4C) + config-storage location.
4. Confirm aux-timer roles (Feature-Out PWM? tick?).

**Board-dependent items** (field-driver topology, package, SWD/BOOT0, RDP) are best closed
with a photo or a multimeter on a real unit — they cannot come from the binary.
