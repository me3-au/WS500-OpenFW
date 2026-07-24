# WS500 I/O Coverage Map

Status of the reverse-engineered hardware I/O. Confidence: ✅ confirmed from firmware ·
🟡 partial / inferred · 🔴 not yet determined · 🔵 needs board/schematic (not in firmware).
Items marked "(§0.6 Vn)" were settled by **stock binary disassembly 2026-07-23** — see
`PROJECT_PLAN.md` §0.6 for the evidence table.

MCU: **STM32F072RB, LQFP64** (Cortex-M0, 128 KB/16 KB, HSI48+CRS). Package **confirmed**
(§0.6 V1): only GPIOA/B/C are ever clocked (GPIOD/E never), and PC4/PC5/PC10 are
configured — pins absent on the 48-pin CB — refuting both the 48- and 100-pin guesses.
See `WS500_HARDWARE_SPEC.md` for the derivation of every item below.

## Confirmed I/O ✅

| Function | Pin / peripheral | Notes |
|---|---|---|
| Field-drive PWM (primary) | PA8 — TIM1_CH1 (AF2) | **143.2 Hz, TIM1 PSC=326 ARR=1024, 10-bit duty** (§0.6 V2; replaces the earlier unsourced "1 kHz-class") |
| Field-drive PWM (aux/compl) | PB15 — TIM1_CH3N (AF2) | role vs driver topology 🔵 |
| Field fault cutoff | **software MOE-clear** (no BKIN) | stock sets `BDTR.BKE=0`, no break-AF pin configured (§0.6 V1+V2) — the earlier "✅ TIM1 BKIN hardware cutoff" is **refuted**; BKIN stays only as an optional improvement if a fault comparator is ever wired 🔵 |
| Alt temp (ATS) | ADC PA1 **or** PA2 — NTC β3950, −40…160 °C | which of PA1/PA2 🔵 |
| Second β3950 probe channel | the other of PA1/PA2 — NTC β3950 | alternator/probe-class (§0.6 V8); harness role 🔵 |
| **FET/driver temp** (was mislabeled "battery temp / BTS") | ADC PA3 — NTC β3380, −40…140 °C | **internal FET/driver over-temp sensor, 125 °C fault `0x4029`** (§0.6 V8); battery temp for temp-comp arrives via **CAN/BMS**, not this pin |
| Battery voltage | ADC PC5 (IN15) — 34.3333:1 divider | 3.3 V Vref → ≈113.3 V full-scale |
| ADC calibration | internal temp / VREFINT / VBAT (slots 4–6) | 7-ch scan, 12-bit, **×4 software averaging** (no HW oversampler on F072 — §0.6 V4); 10 kΩ NTC pull-up |
| Charge current | **INA226 (only), hardwired, I²C1 @ 0x40** — SHUNT_V reg 0x01 | regs 0x01/0x02/0x06, 16-bit big-endian; CALIBRATION computed at runtime from the configured shunt (§0.6 V3). The earlier "INA226/228/238 auto-detect via die-ID" claim is **refuted** — no ID register is ever read |
| Bus voltage at shunt | INA226 BUS_V reg 0x02 | batt or alt per ShuntAtBat; conversion-ready gated via Mask/Enable reg 0x06 |
| Stator / RPM | **PA10 — EXTI rising edge** + TIM2 free-running 32-bit counter @ ~979.6 kHz as timebase | software CNT-diff per edge, **not** TIM2 input capture (§0.6 V1+V2; was 🔴) |
| Battery-capacity DIP switches | **PA4, PA5, PA6, PA7 + PB0, PB1, PB2** — all pull-up | §0.6 V1; PA7 and PB2 were previously undocumented |
| System tick | **TIM7** | §0.6 V1 |
| Active IRQ set | **EXTI4_15, DMA1_CH1, ADC, TIM7, CAN, USB** — only these six | §0.6 V1; everything else is polled/DMA |
| CAN (RV-C/N2K/J1939/Victron) | PB8 = RX, PB9 = TX (AF4) | |
| I²C1 | PB6 = SCL, PB7 = SDA (AF1) | hosts the INA226 @ 0x40 |
| I²C2 | PB10 = SCL, PB11 = SDA (AF1) | device address 🔴 — see config storage below |
| USB FS (CDC config) | PA11 = DM, PA12 = DP | `$XXX:` protocol channel |
| Watchdog | IWDG | safety |
| ADC DMA | DMA1 | circular scan |

## Partial / not yet determined

| Function | Status | What's needed |
|---|---|---|
| Control input PB13 | 🟡 confirmed input, polled, gates a branch | = Enable/Ignition **or** Feature-In; splitting the two reads control logic → bench |
| Status outputs PA0/PA9/PA15/PB14 | 🟡 confirmed outputs, driven 0/1 by state | Lamp/Feature-Out (wire 2) + status LEDs; which is which → bench |
| Aux timers TIM3 / TIM15-17 | 🟡 HAL clock-dispatch chain only; none in the active IRQ set (§0.6 V1) | confirm if any drives Feature-Out PWM |
| Config storage location | 🔨 **NOT internal flash** (no flash-write code in the image) and **NOT EEPROM @ 0x50** (no 0xA0 transactions) — both refuted (§0.6 V7) | prime suspect: the interrupt-driven ~300-byte device on **I²C2** — extract its 7-bit address + boot-read/config-write pattern from the driver at `0x8014050`/`0x8014084` |
| Digital I/O pin roster | 🟡 pins known, functions not | assign each of the remaining seen GPIO pins (PC13-15, PB3-5, PC4, PC10) |

> **Struck (refuted by §0.6 V3):** the "secondary I²C devices 0x0C / 0x10 / 0x4C" row —
> those values were transfer length/size arguments misread as device addresses; no such
> devices exist.

## Needs board / schematic (not recoverable from firmware) 🔵

| Item | Why |
|---|---|
| Field-driver topology (P-type/N-type, gate driver, MOSFET) | analog circuit, not in code |
| Analog front-end resistor values | divider *ratio* (34.3333) + 10 kΩ NTC pull-up are in FW; exact R's are board |
| Which PA1/PA2 = ATS vs second probe | both identical β3950 in FW |
| Battery Temp Sense (harness wire 9) landing point | PA3 is the internal FET sensor (§0.6 V8), so the BTS wire's digitizer is unresolved — possibly the other β3950 channel |
| SWD header (PA13/PA14) + BOOT0 access | for flashing/recovery (PROJECT_PLAN M1); DFU entry itself is via press-and-hold reset (manual) |
| RDP readout-protection level | determines if stock flash can be backed up via SWD |

## Summary

**Complete:** the full sensing + actuation chain — field PWM (143.2 Hz; software fault
cutoff), all temperatures (incl. the corrected PA3 = FET/driver sensor), battery voltage,
current & bus voltage (INA226), RPM (PA10 EXTI + TIM2 timebase), DIP bank, system tick,
CAN, USB config, I²C1. This is enough to build and bench the core regulator.

**Remaining firmware traces (in priority order):**
1. Config store: identify the I²C2 device address + read/write pattern (§0.6 V7).
2. Digital I/O *labels* — pins are found (in: PB13 enable/feature; out:
   PA0/PA9/PA15/PB14). Still need which input = Enable vs Feature-In, and which
   output = Lamp vs status LED (trace each pin's usage, or bench).
3. Confirm remaining aux-timer roles (Feature-Out PWM?).

**Board-dependent items** (field-driver topology, exact resistor values, PA1-vs-PA2 probe
identity, SWD/BOOT0, RDP) are best closed with a photo or a multimeter on a real unit —
they cannot come from the binary.
