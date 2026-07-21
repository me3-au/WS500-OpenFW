# WS500 Hardware Specification (reverse-engineered)

**Status:** first pass — MCU and peripheral inventory confirmed; pin-level map still open.
**Provenance:** every fact below was derived by *black-box analysis of the compiled
firmware image* `WS500-2.6.1.dfu` (a DFU/DfuSe binary), plus public Wakespeed datasheets
and the published Communications & Configuration Guide v2.6.1. **No source code was read
to produce this document.** This is a factual hardware/interface description only — it
contains no control algorithms, no code, and no software expression from the original
firmware. It is intended as the clean-side input for an independent reimplementation.

---

## 1. Firmware image facts (DfuSe container)

| Field | Value | Meaning |
|---|---|---|
| Container format | DfuSe v1 (`DfuSe` prefix, `UFD` suffix) | ST's DFU File Manager output |
| USB idVendor | `0x0483` | STMicroelectronics |
| USB idProduct | `0xDF11` | STM32 **system DFU bootloader** |
| Target name | `ST...` | ST-generated |
| Load address | `0x08000000` | STM32 internal flash base (no offset → app owns the vector table) |
| Payload size | 114 032 bytes (~111 KB) | Flash footprint |

## 2. MCU identification

**Conclusion: STMicroelectronics STM32F072 (Arm Cortex-M0), 128 KB flash / 16 KB SRAM.**
High confidence. Evidence:

- **Vendor/bootloader:** DFU `idVendor 0x0483` + `idProduct 0xDF11` = STM32 built-in DFU.
- **Core = Cortex-M0:** vector table has SVC(11), PendSV(14), SysTick(15) populated but
  slots 4–6 (MemManage/BusFault/UsageFault) and 12–13 all zero — those faults don't exist
  on M0. Initial MSP `0x20003FB0`, reset handler `0x08000151`.
- **RAM = 16 KB:** initial stack pointer `0x20003FB0` sits just under `0x20004000`
  (16 KB above SRAM base `0x20000000`).
- **Flash ≥ 128 KB:** image occupies `0x08000000`–`0x0801BD70` (~111 KB used).
- **Family = STM32F0xx, built on ST Cube HAL:** string `../Src/stm32f0xx_hal_msp.c`.
- **Crystal-less USB:** the **CRS** (Clock Recovery System) peripheral is referenced —
  present only on F0x2/F0x8 parts, used to trim the internal HSI48 against USB SOF frames.
  CRS + USB + bxCAN + 16 KB RAM + 128 KB flash uniquely fits **STM32F072xB**
  (e.g. STM32F072CB / RB / VB — package TBD from board photo).

Runner-up parts ruled out: F103 (Cortex-M3, would populate fault vectors, no CRS, and
USB/CAN share SRAM); F070 (no bxCAN); F091 (32 KB RAM, not 16 KB).

## 3. Memory map

| Region | Address | Notes |
|---|---|---|
| Flash | `0x08000000` + 128 KB | App + vector table at base. Some pages used for config storage (FLASH_IF referenced → in-app flash writes; see §5). |
| SRAM | `0x20000000` + 16 KB | Stack top `0x20003FB0`. |
| Peripherals | `0x40000000` / `0x48000000` | Standard STM32F0 map (see §4). |

## 4. Peripheral inventory (from base-address literals in the image)

Reference counts are how often each peripheral's base address appears as a literal in the
binary — a rough proxy for how heavily it's used, not an exact usage map.

| Peripheral | Refs | Likely role on the WS500 |
|---|---|---|
| **TIM1** (advanced timer) | 9 | **Alternator field-drive PWM.** TIM1 has complementary outputs + a break input (hardware fault shutdown) — the natural choice for the field MOSFET driver. *Role inferred; confirm via pin map.* |
| **TIM2** | 1 | **Stator tach / RPM capture** — confirmed (§6c; handle reads CNT and diffs successive counts for period). |
| TIM15 / TIM16 / TIM17 | 6 each | High ref count is mostly a HAL clock-dispatch chain, not proven active I/O; possible aux PWM (e.g. Feature-Out) — confirm on bench. |
| TIM3 / TIM7 | 1–2 | General-purpose timing / time base (TIM7 likely a tick). |
| **ADC** (12-bit) | 2 | **Analog sensing:** battery voltage, alternator voltage, shunt-amp output (current), alt & battery temperature. Channel→signal mapping TBD. |
| **bxCAN** | 2 | CAN bus — RV-C / NMEA2000 / J1939 / Victron. |
| **USB_FS** + **CRS** | 2 + 1 | USB CDC **virtual COM port** (config channel; string "WS500 Virtual ComPort"). Crystal-less via CRS/HSI48. |
| **I2C1** + **I2C2** | 3 each | Two I²C buses. Candidates: config/parameter EEPROM, external temp sensor, display/aux. TBD. |
| **IWDG** | 5 | Independent watchdog (safety). |
| **EXTI / SYSCFG** | 2–3 | External interrupt lines (candidate: tach edge, fault/enable inputs). |
| **RCC** | 20 | Clock tree (expected). |
| **PWR / FLASH_IF / DMA1 / CRC / DBGMCU** | 1–3 | Power ctrl, in-app flash programming (config persistence), DMA, CRC, debug. |
| GPIOA–GPIOE | 2–13 | I/O. GPIOB most-referenced (13). Pin functions TBD. |

Notably **absent**: no USART base addresses referenced (config is over USB CDC, not a
hardware UART), no SPI, no DAC. Do not assume those signals exist.

## 5. Software components identified (for your own rebuild)

These are libraries the original firmware links — useful because the **open-source** ones
you can reuse directly with zero provenance concern:

- **ST STM32Cube HAL for F0** (`stm32f0xx_hal_*`) — ST's BSD-3 HAL. Freely reusable.
- **ST USB Device library** (`usbd_conf.c`) — CDC/virtual-COM class. ST-licensed, reusable.
- **ttlappalainen NMEA2000 library** (`github.com/ttlappalainen/NMEA2000`) — Timo
  Lappalainen's widely used **open-source (MIT)** NMEA2000 stack. You can adopt the same
  library in a clean rebuild outright.

Implication: the USB, CAN transport, and NMEA2000 layers are effectively "already open."
The only part you must reimplement independently is the **regulation logic** (charge-stage
state machine, field-drive control loop, temperature compensation) — which is exactly the
expression you're intending to rewrite anyway.

## 6. Config / protocol interface (public — from the guide + WS500 Util schema)

- Text command protocol `$XXX:` / `$CPx:n` over the USB virtual COM port.
- All parameters stored **12 V-normalized**, scaled at runtime by an auto-detected system
  voltage multiplier (12–48 V/52 V).
- Current sense: external **500 A / 50 mV** shunt (configurable ratio), on battery or
  alternator side.
- CAN dialects: RV-C, NMEA2000, J1939, Victron VE.reg. Documented PGNs incl. 61444
  (engine RPM in), 127488 (engine params out), 127508 (DC status / CAN current in), 61443.
- Full field-level semantics already encoded in `ws_schema.json` in the WS500 Util project.

## 6b. Pin map (extracted by disassembling the HAL MSP-init routines)

Reconstructed from the `HAL_GPIO_Init` call sites (function at `0x08002AEC`) by decoding
the `GPIO_InitTypeDef` each one builds — pin mask, mode, and alternate-function number.
This is peripheral-init configuration only; no control logic was read. Alternate-function
numbers cross-checked against the STM32F072 datasheet AF table.

| Signal | Pin | Peripheral / mode | Confidence |
|---|---|---|---|
| **Field-drive PWM (primary)** | **PA8** | TIM1_CH1, AF2, push-pull | High |
| **Field-drive PWM (compl./aux)** | **PB15** | TIM1_CH3N, AF2 | High |
| **Analog sense #1** | **PA1** | ADC_IN1, analog | High (pin); role TBD |
| **Analog sense #2** | **PA2** | ADC_IN2, analog | High (pin); role TBD |
| **Analog sense #3** | **PA3** | ADC_IN3, analog | High (pin); role TBD |
| **Analog sense #4** | **PC5** | ADC_IN15, analog | High (pin); role TBD |
| **CAN RX** | **PB8** | CAN_RX, AF4 | High |
| **CAN TX** | **PB9** | CAN_TX, AF4 | High |
| I²C1 SCL / SDA | PB6 / PB7 | AF1, open-drain, pull-up | High |
| I²C2 SCL / SDA | PB10 / PB11 | AF1, open-drain, pull-up | High |
| USB DM / DP | PA11 / PA12 | USB FS (fixed) | High |
| Digital I/O (LED/enable/status/fault) | PC13, PC14, PC15, PA0, PA9, PA15, PB3, PB4, PB5, PB14, PC4, PC10 | GPIO in/out | Medium — pins seen in `MX_GPIO_Init`; individual functions not yet resolved |

**Analog channel binding — RECOVERED FROM FIRMWARE** (measurement routine at
`0x08014230`; conversion constants decoded from the per-slot scaling calls):

| Slot | Pin | Conversion (fn) | Constants | Signal |
|---|---|---|---|---|
| 0 | PA1 | thermistor `0x800f050` | Beta **3950**, clamp −40…**160 °C** | NTC temp (alternator-class) |
| 1 | PA2 | thermistor `0x800f050` | Beta **3950**, clamp −40…**160 °C** | NTC temp (alternator-class) |
| 2 | PA3 | thermistor `0x800f050` | Beta **3380**, clamp −40…**140 °C** | NTC temp (battery-class) |
| 3 | PC5 | linear `0x801003a` | Vref **3.3**, divider **34.33:1** (FS ≈ 113 V) | **Battery voltage** |

The Beta values (3950 / 3380 are standard NTC part constants) and the −40/160/140 °C
clamps make the signal *type* of each channel unambiguous. Which of PA1/PA2 is the primary
alternator sensor vs. a second temp input is a harness detail (both are temperature).
Scaled results are written to a measurement-global cluster at `0x200003D8..0x200003F4`.

**Not on this ADC scan: shunt CURRENT and alternator VOLTAGE.** The WS500 must acquire
current (500 A/50 mV shunt) and likely alternator voltage — but neither is among the four
analog channels here. They therefore arrive via **CAN** (the config is heavily CAN-BMS/
shunt oriented) and/or one of the two **I²C** buses (an external current-sense / ADC IC).
Trace next: the CAN Rx handlers (PGN 127508 DC status, Victron/RV-C shunt) and the I²C1/
I²C2 transaction sites.

**ADC acquisition facts (confirmed from firmware):**
- 7-channel scan, DMA to buffer `0x2000288C`, **oversampled ×4 and averaged**
  (averaging routine at `0x08014242`; ADC handle at `0x200028C4`).
- Fixed ascending scan order fixes the buffer layout:
  `[0]=PA1(IN1) [1]=PA2(IN2) [2]=PA3(IN3) [3]=PC5(IN15)` (external signals),
  `[4]/[5]/[6]` = internal temp-sensor / VREFINT / VBAT (calibration).
- Per-channel engineering-unit scaling was **recovered by disassembly** of the measurement
  routine at `0x08014230` (see the binding table above): PA1/PA2/PA3 are NTC temperatures
  (Beta 3950/3950/3380), PC5 is battery voltage (34.33:1 divider). These scaling constants
  are hardware facts (thermistor Beta, resistor-divider ratio).

Field-drive note: TIM1_CH1 (PA8) is the natural main field PWM; the complementary
CH3N (PB15) usage should be confirmed against the field-driver circuit (single-ended vs.
half-bridge, P-type vs. N-type switching). TIM1's break input (BKIN) provides hardware
over-current/fault cutoff of the field — worth replicating in new firmware.

## 6c. External inputs — Product Manual harness pinout ↔ firmware

Cross-referenced from the WS500 Product Manual (10.21.24) harness pinout (page 10) with
the firmware acquisition paths. This is the authoritative physical-input list.

| Wire (manual) | Signal | Digitized by | Notes |
|---|---|---|---|
| 4  Alternator Temp Sense | NTC thermistor | ADC PA1 or PA2 (Beta 3950, −40..160°C) | ATS |
| 9  Battery Temp Sense | NTC thermistor | ADC PA3 (Beta 3380, −40..140°C) | BTS |
| —  (regulator internal temp) | NTC thermistor | ADC — other of PA1/PA2 (Beta 3950) | inferred |
| 11/10  Voltage Sense +/− | battery DC voltage | ADC PC5 (34.33:1 divider) | Kelvin sense |
| 6  Power/Alt Positive | powers regulator + alt voltage | external monitor / derived | see below |
| 12/13  Current Sense +/− (Purple/Grey) | shunt ±50 mV | **external, not internal ADC** | see below |
| 8  Stator (Yellow) | AC frequency | **TIM2** input capture | RPM |
| 1  Ignition / Enable | digital | GPIO | |
| 3  Feature-In (White) | digital/config | GPIO | multi-function |
| 2  Lamp / Feature-Out (Orange) | output | GPIO | |

**Single current input — CONFIRMED: TI INA2xx current/power monitor on I²C @ 0x40.**
There is exactly one shunt (500 A/50 mV), selectable at the battery or alternator via
`$CCN ShuntAtBat`. Being a differential millivolt signal it is not readable by the
single-ended STM32F072 ADC and is absent from the 7-channel scan; it is digitized by a
**TI INA226 / INA228 / INA238** current-and-power monitor at **I²C 7-bit address 0x40**.

Evidence (from disassembly): the I²C access functions read the INA register map at 0x40 —
`0x00` CONFIG, `0x01` SHUNT_V (→ current), `0x02` BUS_V (→ local DC voltage), `0x03` POWER,
`0xFF` DIE_ID; the firmware auto-detects the variant against die-IDs `0x2260` (INA226),
`0x2280` (INA228), `0x2380` (INA238) and the `'TI'` (0x5449) manufacturer ID (register
0xFE) — all present in a detection table in the image.

Implication for the rebuild: the INA supplies **both charge current (shunt V) and the bus
voltage at the shunt location** (battery or alternator side per ShuntAtBat) — which is why
neither is on the internal ADC. Reuse a standard open INA2xx driver over I²C; set the
calibration register from the configured shunt (500 A/50 mV default). A secondary I²C
device cluster (7-bit 0x0C / 0x10 / 0x4C) also exists — likely other-variant peripherals
(EEPROM / expander / aux sensor), not part of the core sensing chain.

Non-ADC input handles (corrected): `0x20002910` = CAN, `0x200029A0` = TIM1 (field),
`0x200029E0` = TIM2 (stator/RPM capture — reads CNT at [handle→0x24], diffs successive
counts). Only `0x20002954` is I²C1.

## 7. Open items — needed to complete the hardware spec

These are the remaining facts a clean-side implementer needs. All obtainable without
reading the original control-logic source:

1. **Pin map (highest priority).** Which GPIO = field PWM (TIM1 channel), which ADC
   channels = VBat / VAlt / shunt / temps, which pins = CAN TX/RX, tach input, enable/
   fault I/O, I²C SDA/SCL. → Extract by disassembling the HAL `GPIO_Init` / `TIM`/`ADC`
   MSP-init calls in the binary (peripheral-init only, not the control loop), **or** from
   clear board photos of a physical unit.
2. **Package / pin count** (CB=48-pin LQFP, RB=64-pin, VB=100-pin) — from a board photo.
3. **Field-drive topology** — high-side vs low-side switch for P-type/N-type alternators;
   gate driver + MOSFET part numbers (board photo / schematic trace).
4. **Analog front-end scaling** — divider ratios and shunt-amp gain (board trace, or infer
   from ADC-to-engineering-unit constants — treat those constants as facts to verify, not
   as code to copy).
5. **Config storage location** — internal flash page vs external I²C EEPROM.

---

### Provenance note (keep for the record)

This document was produced solely from (a) the compiled DFU binary as a black box and
(b) public documentation. It records hardware and interface **facts**. It deliberately
excludes the original firmware's control algorithms, code structure, and any expressive
content. The independent reimplementation must derive its regulation logic from control
theory and the *published* charge-profile behavior — not from this file and not from the
original source.
