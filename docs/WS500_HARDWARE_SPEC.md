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
| TIM15 / TIM16 / TIM17 | 6 each | Additional PWM / timebase / input capture (candidate: stator tach / RPM capture). |
| TIM2 / TIM3 / TIM7 | 1–2 | General-purpose timing, delays, scheduling. |
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

**The four analog channels (PA1/PA2/PA3/PC5) carry: battery voltage, alternator voltage,
shunt-amplifier output (current), and temperature** — but *which pin is which* cannot be
determined from the firmware alone. Resolve by tracing the board, or by injecting known
voltages on each pin in an emulator/rig. (Note the WS500 also exposes alt + battery temp;
one or both temp inputs may use additional analog channels or an I²C sensor — verify.)

**ADC acquisition facts (confirmed from firmware):**
- 7-channel scan, DMA to buffer `0x2000288C`, **oversampled ×4 and averaged**
  (averaging routine at `0x08014242`; ADC handle at `0x200028C4`).
- Fixed ascending scan order fixes the buffer layout:
  `[0]=PA1(IN1) [1]=PA2(IN2) [2]=PA3(IN3) [3]=PC5(IN15)` (external signals),
  `[4]/[5]/[6]` = internal temp-sensor / VREFINT / VBAT (calibration).
- Per-channel engineering-unit scaling (divider ratio → V, shunt ratio → A, thermistor
  → °C) is reached through a pointer in the measurement struct, **not** by literal buffer
  address — recovering it requires full decompilation of the measurement routine and
  crosses into measurement logic. **Recommended instead:** bind each channel by injecting
  a known voltage per pin (emulator or bench) and observing the reported value. Four
  injections → unambiguous PA1/PA2/PA3/PC5 → signal map.

Field-drive note: TIM1_CH1 (PA8) is the natural main field PWM; the complementary
CH3N (PB15) usage should be confirmed against the field-driver circuit (single-ended vs.
half-bridge, P-type vs. N-type switching). TIM1's break input (BKIN) provides hardware
over-current/fault cutoff of the field — worth replicating in new firmware.

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
