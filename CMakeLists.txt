# WS500-OpenFW — STM32F072xB. SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.20)
project(ws500-openfw C ASM)

set(CMAKE_C_STANDARD 11)
set(LINKER_SCRIPT ${CMAKE_SOURCE_DIR}/STM32F072xB_FLASH.ld)

# --- Application sources (this repo) ---
#   Core/  = drivers + BSP + app (HAL-facing)
#   control/ = pure control core (no HAL; host/Renode testable)
set(APP_SRC
    Core/Src/main.c
    Core/Src/board.c
    Core/Src/field_drive.c
    Core/Src/sensors.c
    Core/Src/ina2xx.c
    Core/Src/config_protocol.c
    Core/Src/can_n2k.c
    control/Src/control.c
    control/Src/arbitration.c
    control/Src/field.c
)

# --- Vendored deps you must add (see Drivers/README.md) ---
#   STM32Cube HAL (BSD-3): Drivers/STM32F0xx_HAL_Driver, Drivers/CMSIS
#   plus startup_stm32f072xb.s and system_stm32f0xx.c.
file(GLOB HAL_SRC CONFIGURE_DEPENDS Drivers/STM32F0xx_HAL_Driver/Src/*.c)
list(FILTER HAL_SRC EXCLUDE REGEX "_template\\.c$")
set(STARTUP  Drivers/CMSIS/Device/ST/STM32F0xx/Source/Templates/gcc/startup_stm32f072xb.s)
set(SYSTEM   Drivers/CMSIS/Device/ST/STM32F0xx/Source/Templates/system_stm32f0xx.c)

add_executable(${PROJECT_NAME}.elf ${APP_SRC} ${HAL_SRC} ${STARTUP} ${SYSTEM})

target_compile_definitions(${PROJECT_NAME}.elf PRIVATE STM32F072xB USE_HAL_DRIVER)
target_include_directories(${PROJECT_NAME}.elf PRIVATE
    Core/Inc
    control/Inc
    Drivers/STM32F0xx_HAL_Driver/Inc
    Drivers/CMSIS/Device/ST/STM32F0xx/Include
    Drivers/CMSIS/Include
)
target_link_options(${PROJECT_NAME}.elf PRIVATE
    -T${LINKER_SCRIPT} -Wl,-Map=${PROJECT_NAME}.map,--cref)
# libm's logf references __errno from libc; group them so single-pass ld resolves it
target_link_libraries(${PROJECT_NAME}.elf PRIVATE -Wl,--start-group c m -Wl,--end-group)

add_custom_command(TARGET ${PROJECT_NAME}.elf POST_BUILD
    COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${PROJECT_NAME}.elf> ${PROJECT_NAME}.bin
    COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${PROJECT_NAME}.elf>)
