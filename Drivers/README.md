# Drivers (add these — not vendored here)

The build expects the STM32Cube HAL for F0, dropped in beside this file:

```
Drivers/
├── STM32F0xx_HAL_Driver/   (Inc/ + Src/)
├── CMSIS/
│   ├── Device/ST/STM32F0xx/  (Include/, Source/Templates/{system_stm32f0xx.c,
│   │                          gcc/startup_stm32f072xb.s})
│   └── Include/
```

Get them from **STM32CubeF0** (BSD-3-Clause):
<https://github.com/STMicroelectronics/STM32CubeF0> — copy `Drivers/STM32F0xx_HAL_Driver`
and `Drivers/CMSIS` verbatim. Add a `Core/Inc/stm32f0xx_hal_conf.h` (start from the HAL
template and enable the ADC, CAN, I2C, TIM, USB/PCD, RCC, GPIO, CORTEX, DMA modules).

The **NMEA2000** layer (`can_n2k.c`) uses <https://github.com/ttlappalainen/NMEA2000>
(MIT) plus a bxCAN driver shim — add under `Middlewares/` and extend CMakeLists.

Both are open-source and freely redistributable; they carry no Wakespeed IP.
