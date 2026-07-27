# WS500-OpenFW — STM32F072xB. SPDX-License-Identifier: MIT
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
    # PROJECT_PLAN §7 robustness layer (R0-R6). Firmware-only by construction:
    # every one of these files touches STM32 registers or the vector table, so
    # none of them is ever compiled into the native/SIL builds (which glob
    # control/ and sim/ only — see .github/workflows/build.yml).
    Core/Src/safe_state.c
    Core/Src/crash_record.c
    Core/Src/fault_handlers.c
    Core/Src/watchdog.c
    Core/Src/integrity.c
    Core/Src/pvd.c
    Core/Src/err_budget.c
    Core/Src/sensors.c
    Core/Src/ina2xx.c
    Core/Src/eeprom24c16.c
    Core/Src/stator_rpm.c
    Core/Src/dio.c
    Core/Src/config_protocol.c
    Core/Src/config_store_glue.c
    Core/Src/cfg_stream.c
    # GH#35 (#20): CDC-ACM port + 1 Hz telemetry pump. Self-written class on
    # the vendored BSD-3 HAL PCD driver — ST's USB Device middleware is
    # SLA0044 (ST-silicon-only) and therefore not vendorable here (CLAUDE.md).
    Core/Src/usb_cdc.c
    Core/Src/telem_stream.c
    Core/Src/can_n2k.c
    control/Src/control.c
    control/Src/arbitration.c
    control/Src/field.c
    control/Src/limits.c
    control/Src/faults.c
    control/Src/thermal.c
    control/Src/telemetry.c
    control/Src/telemetry_json.c
    control/Src/n2k_encode.c
    control/Src/n2k_addrclaim.c
    control/Src/n2k_sched.c
    control/Src/rvc_encode.c
    control/Src/rvc_sched.c
    control/Src/bms_rx.c
    control/Src/config_codec.c
    control/Src/config_validate.c
    control/Src/config_store.c
    control/Src/config_json.c
    control/Src/config_doc.c
    control/Src/config_msg.c
)

# --- test-fw: bring-up test firmware (deliverable #14, PROJECT_PLAN §4/M2) --
# A SECOND, separate build target sharing board.c and the production drivers
# rather than duplicating them (test-fw/README.md). Deliberately narrower than
# APP_SRC above: no control/ engine, no config/telemetry pipeline, no
# can_n2k.c (the full NMEA2000/RV-C stack) — test-fw's console is its own
# operator interface. It DOES share the §7-style robustness trio (safe_state/
# crash_record/fault_handlers/watchdog) so "never a bare while(1)" and the
# field-off funnel hold here too, not just in the main firmware.
set(TESTFW_SRC
    test-fw/main.c
    test-fw/console.c
    test-fw/field_guard.c
    test-fw/can_test.c
    test-fw/i2c_scan.c
    Core/Src/board.c
    Core/Src/field_drive.c
    Core/Src/safe_state.c
    Core/Src/crash_record.c
    Core/Src/fault_handlers.c
    Core/Src/watchdog.c
    Core/Src/err_budget.c
    Core/Src/sensors.c
    Core/Src/ina2xx.c
    Core/Src/eeprom24c16.c
    Core/Src/stator_rpm.c
    Core/Src/dio.c
    Core/Src/usb_cdc.c
    # PURE (HAL-free) — only linked for cfg_json_fmt_g()/cfg_json_scan_num(),
    # the firmware's one shared float formatter/parser (newlib-nano has no
    # float printf/scanf, see console.c's file header). Nothing else from
    # control/ is needed: test-fw carries no control-core state.
    control/Src/config_json.c
)

# --- Vendored deps you must add (see Drivers/README.md) ---
#   STM32Cube HAL (BSD-3): Drivers/STM32F0xx_HAL_Driver, Drivers/CMSIS
#   plus startup_stm32f072xb.s and system_stm32f0xx.c.
file(GLOB HAL_SRC CONFIGURE_DEPENDS Drivers/STM32F0xx_HAL_Driver/Src/*.c)
list(FILTER HAL_SRC EXCLUDE REGEX "_template\\.c$")
set(STARTUP  Drivers/CMSIS/Device/ST/STM32F0xx/Source/Templates/gcc/startup_stm32f072xb.s)
set(SYSTEM   Drivers/CMSIS/Device/ST/STM32F0xx/Source/Templates/system_stm32f0xx.c)

add_executable(${PROJECT_NAME}.elf ${APP_SRC} ${HAL_SRC} ${STARTUP} ${SYSTEM})
add_executable(ws500-testfw.elf ${TESTFW_SRC} ${HAL_SRC} ${STARTUP} ${SYSTEM})

set(WS500_TARGETS ${PROJECT_NAME}.elf ws500-testfw.elf)

foreach(tgt ${WS500_TARGETS})
    # Stem without ".elf" (e.g. "ws500-openfw" from "ws500-openfw.elf") — kept
    # identical to the pre-existing naming (${PROJECT_NAME}.bin/.map, NOT
    # ${PROJECT_NAME}.elf.bin) so .github/workflows/build.yml's artifact paths
    # for the main target are unchanged.
    get_filename_component(stem ${tgt} NAME_WE)

    target_compile_definitions(${tgt} PRIVATE STM32F072xB USE_HAL_DRIVER)
    target_include_directories(${tgt} PRIVATE
        Core/Inc
        control/Inc
        Drivers/STM32F0xx_HAL_Driver/Inc
        Drivers/CMSIS/Device/ST/STM32F0xx/Include
        Drivers/CMSIS/Include
    )
    target_link_options(${tgt} PRIVATE
        -T${LINKER_SCRIPT} -Wl,-Map=${stem}.map,--cref)
    # libm's logf references __errno from libc; group them so single-pass ld resolves it
    target_link_libraries(${tgt} PRIVATE -Wl,--start-group c m -Wl,--end-group)
endforeach()

# Post-build: raw binary, then patch the §7 R4 image CRC into its last four
# bytes (the `.fw_crc` section the linker script places last), then report size.
#
# The CRC is patched into the .bin ONLY — an .elf loaded straight onto a target
# by a debugger or into Renode keeps the 0xFFFFFFFF placeholder, which
# integrity.c reports as INTEGRITY_CRC_UNPATCHED rather than as corruption.
# That is the intended behaviour (see integrity.c's header); the .bin is what
# DFU/updates actually flash, and it is what M4 verifies. test-fw does NOT
# link integrity.c (out of scope for this small bring-up target — see
# test-fw/README.md), so the CRC is patched for consistency with the main
# image but nothing on that target reads it back; harmless either way.
#
# find_package(Python3) is deliberately not used: the script is stdlib-only and
# CI already runs python3 for the docs lint.
find_program(PYTHON3_EXE NAMES python3 python REQUIRED)
foreach(tgt ${WS500_TARGETS})
    get_filename_component(stem ${tgt} NAME_WE)
    add_custom_command(TARGET ${tgt} POST_BUILD
        COMMAND ${CMAKE_OBJCOPY} -O binary $<TARGET_FILE:${tgt}> ${stem}.bin
        COMMAND ${PYTHON3_EXE} ${CMAKE_SOURCE_DIR}/scripts/embed_crc.py ${stem}.bin
        COMMAND ${CMAKE_SIZE} $<TARGET_FILE:${tgt}>)
endforeach()
