# arm-none-eabi toolchain for STM32F072 (Cortex-M0).
# SPDX-License-Identifier: GPL-3.0-or-later
set(CMAKE_SYSTEM_NAME Generic)
set(CMAKE_SYSTEM_PROCESSOR arm)

set(TOOL arm-none-eabi)
set(CMAKE_C_COMPILER   ${TOOL}-gcc)
set(CMAKE_ASM_COMPILER ${TOOL}-gcc)
set(CMAKE_CXX_COMPILER ${TOOL}-g++)
set(CMAKE_OBJCOPY      ${TOOL}-objcopy)
set(CMAKE_SIZE         ${TOOL}-size)
set(CMAKE_TRY_COMPILE_TARGET_TYPE STATIC_LIBRARY)

set(CPU_FLAGS "-mcpu=cortex-m0 -mthumb -mfloat-abi=soft")
set(CMAKE_C_FLAGS_INIT   "${CPU_FLAGS} -ffunction-sections -fdata-sections -Wall -Wextra")
set(CMAKE_ASM_FLAGS_INIT "${CPU_FLAGS} -x assembler-with-cpp")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${CPU_FLAGS} -Wl,--gc-sections --specs=nano.specs")

set(CMAKE_FIND_ROOT_PATH_MODE_PROGRAM NEVER)
set(CMAKE_FIND_ROOT_PATH_MODE_LIBRARY ONLY)
set(CMAKE_FIND_ROOT_PATH_MODE_INCLUDE ONLY)
