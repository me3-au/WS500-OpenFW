# WS500 Hardware Specification (reverse-engineered)

**Status:** second pass — MCU, package, full pin map, timer configuration, and sensing
chain confirmed by static disassembly of the stock binary (2026-07-23; see
`PROJECT_PLAN.md` §0.6 V1–V8 for the evidence trail). Remaining opens are board-level
only (§7).
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

**Conclusion: STMicroelectronics STM32F072RB, LQFP64 package (Arm Cortex-M0),
128 KB flash / 16 KB SRAM.** Confirmed — stock binary disassembly 2026-07-23 (§0.6 V1).
Evidence:

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
  CRS + USB + bxCAN + 16 KB RAM + 128 KB flash uniquely fits **STM32F072xB**.
- **Package = RB (64-pin LQFP64)** — resolved from the stock binary (2026-07-23, §0.6 V1):
  the RCC AHBENR writes clock **only GPIOA/GPIOB/GPIOC** (GPIOD/GPIOE are never enabled),
  and **PC4/PC5/PC10 are actively configured** — pins that do not exist on the 48-pin
  F072CB. This refutes both the earlier 48-pin and 100-pin guesses.

Runner-up parts ruled out: F103 (Cortex-M3, would populate fault vectors, no CRS, and
USB/CAN share SRAM); F070 (no bxCAN); F091 (32 KB RAM, not 16 KB).

## 3. Memory map

| Region | Address | Notes |
|---|---|---|
| Flash | `0x08000000` + 128 KB | App + vector table at base. **NOT used for config storage** — flash unlock keys (0x45670123/0xCDEF89AB) and KEYR/CR/SR write code are entirely absent from the image, so the stock firmware never programs its own flash (stock binary disassembly 2026-07-23, §0.6 V7). Config persists off-chip (I²C2 device; see §6c). |
| SRAM | `0x20000000` + 16 KB | Stack top `0x20003FB0`. |
| Peripherals | `0x40000000` / `0x48000000` | Standard STM32F0 map (see §4). |

## 4. Peripheral inventory (from base-address literals in the image)

Reference counts are how often each peripheral's base address appears as a literal in the
binary — a rough proxy for how heavily it's used, not an exact usage map.

| Peripheral | Refs | Likely role on the WS500 |
|---|---|---|
| **TIM1** (advanced timer) | 9 | **Alternator field-drive PWM — confirmed** (stock binary disassembly 2026-07-23, §0.6 V2): `MX_TIM1_Init` sets **PSC=326, ARR=1024** from a 48 MHz timer clock → **143.2 Hz PWM, 10-bit duty**. CH1 (PA8) + CH3/CH3N (PB15) configured. **BDTR BKE=0 — the break input (BKIN) is NOT used**; field fault cutoff is a software MOE-clear. |
| **TIM2** | 1 | **Free-running 32-bit timebase @ ~979.6 kHz (1.02 µs/tick) — confirmed** (§0.6 V2). **NOT input capture**: the stator signal enters on PA10 via EXTI rising edge, and software diffs successive `TIM2->CNT` reads for the period (see §6c). |
| TIM15 / TIM16 / TIM17 | 6 each | HAL clock-dispatch chain only — **not in the active IRQ set** (see note below); no proven active I/O. |
| TIM3 / **TIM7** | 1–2 | **TIM7 = system tick — confirmed** (§0.6 V1: TIM7 is one of only six live IRQs). TIM3 role unproven. |
| **ADC** (12-bit) | 2 | **Analog sensing:** battery voltage, alternator voltage, shunt-amp output (current), alt & battery temperature. Channel→signal mapping TBD. |
| **bxCAN** | 2 | CAN bus — RV-C / NMEA2000 / J1939 / Victron. |
| **USB_FS** + **CRS** | 2 + 1 | USB CDC **virtual COM port** (config channel; string "WS500 Virtual ComPort"). Crystal-less via CRS/HSI48. |
| **I2C1** + **I2C2** | 3 each | Two I²C buses. **I²C1 hosts the INA226 @ 0x40** (§0.6 V3). **I²C2** runs an interrupt-driven ~300-byte device — prime config-store suspect, address TBD (§0.6 V7). |
| **IWDG** | 5 | Independent watchdog (safety). |
| **EXTI / SYSCFG** | 2–3 | External interrupt lines (candidate: tach edge, fault/enable inputs). |
| **RCC** | 20 | Clock tree (expected). |
| **PWR / FLASH_IF / DMA1 / CRC / DBGMCU** | 1–3 | Power ctrl, DMA, CRC, debug. FLASH_IF base is referenced by linked HAL code, but **no flash-write path exists in the image** (no unlock keys — §0.6 V7): config is NOT in internal flash. |
| GPIOA–GPIOC | 2–13 | I/O. GPIOB most-referenced (13). Full pin map in §6b. (GPIOD/GPIOE base literals appear via HAL tables but those ports are **never clocked** — consistent with the LQFP64 package.) |

**Active interrupt set (confirmed — stock binary disassembly 2026-07-23, §0.6 V1):** only
**six** IRQs are live: **EXTI4_15** (stator edge + digital inputs), **DMA1_CH1** (ADC),
**ADC**, **TIM7** (system tick), **CAN**, **USB**. I²C and everything else is serviced by
poll/DMA — do not assume other interrupt-driven peripherals exist.

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
| **Analog sense #1** | **PA1** | ADC_IN1, analog | High — β3950 NTC (alternator/probe; which of PA1/PA2 is ATS → bench) |
| **Analog sense #2** | **PA2** | ADC_IN2, analog | High — β3950 NTC (alternator/probe; which of PA1/PA2 is ATS → bench) |
| **Analog sense #3** | **PA3** | ADC_IN3, analog | High — β3380 NTC, **FET/driver over-temp sensor** (§0.6 V8) |
| **Analog sense #4** | **PC5** | ADC_IN15, analog | High — battery voltage, 34.3333:1 divider |
| **Stator / RPM input** | **PA10** | **EXTI rising-edge input** (not a timer channel); ISR diffs TIM2 free-running CNT for period | High — stock binary disassembly 2026-07-23 (§0.6 V1/V2) |
| **DIP switches (battery-capacity code)** | **PA4, PA5, PA6, PA7 + PB0, PB1, PB2** | GPIO inputs, **all pull-up** | High — stock binary disassembly 2026-07-23 (§0.6 V1); PA7/PB2 were previously undocumented |
| Enable / feature input | PB13 | GPIO input, polled | High (pin); Enable vs Feature-In label → bench |
| **CAN RX** | **PB8** | CAN_RX, AF4 | High |
| **CAN TX** | **PB9** | CAN_TX, AF4 | High |
| I²C1 SCL / SDA | PB6 / PB7 | AF1, open-drain, pull-up | High |
| I²C2 SCL / SDA | PB10 / PB11 | AF1, open-drain, pull-up | High |
| USB DM / DP | PA11 / PA12 | USB FS (fixed) | High |
| Digital I/O (LED/status/fault out, misc) | PC13, PC14, PC15, PA0, PA9, PA15, PB3, PB4, PB5, PB14, PC4, PC10 | GPIO in/out | Medium — pins seen in `MX_GPIO_Init`; individual functions not yet resolved |

**Analog channel binding — RECOVERED FROM FIRMWARE** (measurement routine at
`0x08014230`; conversion constants decoded from the per-slot scaling calls):

| Slot | Pin | Conversion (fn) | Constants | Signal |
|---|---|---|---|---|
| 0 | PA1 | thermistor `0x800f050` | Beta **3950**, clamp −40…**160 °C** | NTC temp (alternator-class) |
| 1 | PA2 | thermistor `0x800f050` | Beta **3950**, clamp −40…**160 °C** | NTC temp (alternator-class) |
| 2 | PA3 | thermistor `0x800f050` | Beta **3380**, clamp −40…**140 °C** | NTC temp — **FET/driver over-temp sensor** (drives the 125 °C fault `0x4029`), **not battery** (§0.6 V8) |
| 3 | PC5 | linear `0x801003a` | Vref **3.3**, divider **34.3333:1** (FS ≈ 113.3 V) | **Battery voltage** |

The Beta values (3950 / 3380 are standard NTC part constants) and the −40/160/140 °C
clamps make the signal *type* of each channel unambiguous. Which of PA1/PA2 is the primary
alternator sensor vs. a second probe input is a harness detail (both are temperature).
**PA3's role is resolved (stock binary disassembly 2026-07-23, §0.6 V8):** its result
(`0x200003E0`) feeds a **125 °C over-temp fault (`0x4029`)**, an over-temp status
classifier, and telemetry — and nothing feeds a `V_target += k·(Tref−T)` temp-comp term.
It is the **internal FET/driver-stage temperature**, not the harness Battery Temp Sense;
battery temperature for temp-compensation arrives via **CAN/BMS** (agrees with the
upstream VSR's β3380 = internal-FET-sensor split).
Scaled results are written to a measurement-global cluster at `0x200003D8..0x200003F4`.

**Not on this ADC scan: shunt CURRENT and shunt-side bus VOLTAGE.** The WS500 must acquire
current (500 A/50 mV shunt) — but it is not among the four analog channels here.
**Resolved (stock binary disassembly 2026-07-23, §0.6 V3):** both arrive from the
**INA226 on I²C1 @ 0x40** (see §6c).

**ADC acquisition facts (confirmed from firmware):**
- 7-channel scan, **12-bit** (FS 4095), DMA to buffer `0x2000288C`, **×4 software
  averaging** (averaging routine at `0x08014242`; ADC handle at `0x200028C4`). The F072
  has **no hardware oversampler** — the averaging is done in software (§0.6 V4).
- Fixed ascending scan order fixes the buffer layout:
  `[0]=PA1(IN1) [1]=PA2(IN2) [2]=PA3(IN3) [3]=PC5(IN15)` (external signals),
  `[4]/[5]/[6]` = internal temp-sensor / VREFINT / VBAT (calibration).
- Per-channel engineering-unit scaling was **recovered by disassembly** of the measurement
  routine at `0x08014230` (see the binding table above): PA1/PA2/PA3 are NTC temperatures
  (Beta 3950/3950/3380, **10 kΩ pull-up**, Beta/Steinhart via `logf`), PC5 is battery
  voltage (linear, **34.3333:1** divider, 3.3 V Vref → ≈113.3 V full-scale). These scaling
  constants are hardware facts (thermistor Beta, resistor-divider ratio). Exact resistor
  values (vs. the ratio) remain board-level facts — bench item.

Field-drive note (updated — stock binary disassembly 2026-07-23, §0.6 V1/V2): TIM1_CH1
(PA8) is the main field PWM at **143.2 Hz (PSC=326, ARR=1024, 10-bit duty)**; the
complementary CH3N (PB15) usage should be confirmed against the field-driver circuit
(single-ended vs. half-bridge, P-type vs. N-type switching — board-level, bench item).
**TIM1's break input (BKIN) is NOT used by the stock firmware**: `BDTR.BKE=0` and no
break-AF pin is configured — the stock field fault cutoff is a **software MOE-clear**.
Our firmware keeps BKIN only as an *optional* improvement, valid only if a fault
comparator is ever wired to a break-capable pin (it is not routed in stock).

## 6c. External inputs — Product Manual harness pinout ↔ firmware

Cross-referenced from the WS500 Product Manual (10.21.24) harness pinout (page 10) with
the firmware acquisition paths. This is the authoritative physical-input list.

| Wire (manual) | Signal | Digitized by | Notes |
|---|---|---|---|
| 4  Alternator Temp Sense | NTC thermistor | ADC PA1 or PA2 (Beta 3950, −40..160°C) | ATS |
| 9  Battery Temp Sense | NTC thermistor | **NOT PA3** (PA3 = internal FET temp, §0.6 V8); digitizer unresolved — possibly the other β3950 channel (PA1/PA2) → bench | earlier "BTS = PA3" label refuted |
| —  (regulator internal FET/driver temp) | NTC thermistor | **ADC PA3** (Beta 3380, −40..140°C; 125 °C fault `0x4029`) | confirmed (§0.6 V8) |
| 11/10  Voltage Sense +/− | battery DC voltage | ADC PC5 (34.3333:1 divider) | Kelvin sense |
| 6  Power/Alt Positive | powers regulator + alt voltage | external monitor / derived | see below |
| 12/13  Current Sense +/− (Purple/Grey) | shunt ±50 mV | **external, not internal ADC** | see below |
| 8  Stator (Yellow) | AC frequency | **PA10 EXTI rising edge** + TIM2 free-running timebase (software CNT diff) — *not* TIM2 input capture (§0.6 V1/V2) | RPM |
| 1  Ignition / Enable | digital | GPIO | |
| 3  Feature-In (White) | digital/config | GPIO | multi-function |
| 2  Lamp / Feature-Out (Orange) | output | GPIO | |

**Single current input — CONFIRMED: TI INA226 (only) on I²C1 @ 0x40, hardwired.**
There is exactly one shunt (500 A/50 mV), selectable at the battery or alternator via
`$CCN ShuntAtBat`. Being a differential millivolt signal it is not readable by the
single-ended STM32F072 ADC and is absent from the 7-channel scan; it is digitized by a
**TI INA226** current-and-power monitor at **I²C 7-bit address 0x40** on **I²C1**.

Evidence (stock binary disassembly 2026-07-23, §0.6 V3): the reader at `0x800B530` reads
registers **`0x06` Mask/Enable** (conversion-ready gate), **`0x02` BUS_V** (→ local DC
voltage), and **`0x01` SHUNT_V** (→ current), all **16-bit big-endian**, on I²C1 @ 0x40.
The **CALIBRATION register value is computed at runtime from the configured shunt ratio**
— it is not a magic constant.

> **Correction (2026-07-23):** an earlier revision of this document claimed the firmware
> auto-detects an INA226/INA228/INA238 variant via DIE_ID/manufacturer-ID registers, and
> that a secondary I²C device cluster existed at 0x0C/0x10/0x4C. **Both claims are
> refuted by the disassembly (§0.6 V3):** the die-IDs `0x2260`/`0x2280`/`0x2380` and mfr
> `0x5449` do not exist anywhere in the image, no ID register (0xFF/0x3F) is ever read,
> and there is no detection table; the "0x0C/0x10/0x4C addresses" were transfer
> length/size arguments misread as device addresses. The hardware is **INA226 only,
> hardwired**.

Implication for the rebuild: the INA226 supplies **both charge current (shunt V) and the
bus voltage at the shunt location** (battery or alternator side per ShuntAtBat) — which is
why neither is on the internal ADC. Reuse a standard open INA226 driver over I²C; derive
the CALIBRATION value from the configured shunt (500 A/50 mV default), not a copied
constant.

**Config storage (narrowed — §0.6 V7, in progress):** NOT internal flash (no flash-write
code in the image) and NOT an EEPROM @ 0x50 (no 0xA0 transactions). The prime suspect is
the **I²C2 device** — a second inited bus running an interrupt-driven ~300-byte
peripheral (driver at `0x8014050`/`0x8014084`); its 7-bit address and boot-read/
config-write behavior are still to be extracted.

Non-ADC input handles (corrected): `0x20002910` = CAN, `0x200029A0` = TIM1 (field),
`0x200029E0` = TIM2 (**free-running 32-bit timebase @ ~979.6 kHz** — software reads CNT
at [handle→0x24] and diffs successive counts against PA10/EXTI stator edges; not input
capture). Only `0x20002954` is I²C1.

## 7. Open items — needed to complete the hardware spec

These are the remaining facts a clean-side implementer needs. All obtainable without
reading the original control-logic source:

1. ~~**Pin map.**~~ **RESOLVED** — full pin map extracted from the stock binary
   (all 17 `HAL_GPIO_Init` sites + RCC AHBENR decoded, 2026-07-23, §0.6 V1); see §6b.
   Remaining pin-level opens are label-only (which of PA1/PA2 = ATS vs second probe;
   PB13 = Enable vs Feature-In; which output = Lamp vs LED) → bench.
2. ~~**Package / pin count.**~~ **RESOLVED — STM32F072RB, LQFP64** (2026-07-23, §0.6 V1:
   only GPIOA/B/C clocked; PC4/PC5/PC10 configured, which don't exist on the 48-pin CB).
3. **Field-drive topology** — high-side vs low-side switch for P-type/N-type alternators;
   gate driver + MOSFET part numbers (board photo / schematic trace). Still open — bench.
4. **Analog front-end resistor values** — the *ratios* (34.3333:1 divider, 10 kΩ NTC
   pull-up) are recovered from firmware; exact resistor values are board facts. Bench.
5. **Config storage device** — narrowed (§0.6 V7): NOT internal flash, NOT EEPROM @ 0x50;
   prime suspect is the interrupt-driven ~300-byte device on **I²C2** — its 7-bit address
   and read/write pattern still to be extracted from the I²C2 driver.

---

### Provenance note (keep for the record)

This document was produced solely from (a) the compiled DFU binary as a black box and
(b) public documentation. It records hardware and interface **facts**. It deliberately
excludes the original firmware's control algorithms, code structure, and any expressive
content. The independent reimplementation must derive its regulation logic from control
theory and the *published* charge-profile behavior — not from this file and not from the
original source.
