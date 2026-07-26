*** Comments ***
# WS500-OpenFW whole-firmware liveness test  (PROJECT_PLAN §8.2, issue #25).
#
# renode-test injects the Renode Robot keyword library automatically, so this
# file needs no Settings/Resource import (mirrors Renode's own stock tests,
# e.g. tests/platforms/STM32F072b.robot).
#
# LIVENESS OBSERVABLE
#   The firmware has no console UART, so we do not watch a serial line. Instead
#   we enable the CPU's function-name logging (needs the ELF's .symtab symbols)
#   and assert on log entries:
#     * PRIMARY   — "Entering function ctrl_tick" appears  -> main() reached the
#                   10 ms control loop and called ctrl_tick at least once.
#     * ITERATION — it appears a SECOND time               -> the for(;;) loop
#                   actually iterates (a fresh SysTick-gated tick), not just a
#                   one-shot entry.
#     * SAFETY    — "Entering function Error_Handler" is registered as a FAILING
#                   log string, so diverging into Error_Handler (field_drive_off
#                   + for(;;){}) fails the test immediately, and a trailing
#                   Should Not Be In Log guards the window after iteration.

*** Variables ***
${ELF}          @build/ws500-openfw.elf
${PLATFORM}     @renode/ws500f072.repl

*** Keywords ***
Create Machine
    Execute Command             using sysbus
    Execute Command             mach create "ws500"
    Execute Command             machine LoadPlatformDescription ${PLATFORM}
    Execute Command             sysbus LoadELF ${ELF}
    # Info-level "Entering function <name> at 0x..." for just these two symbols.
    Execute Command             sysbus.cpu LogFunctionNames true "ctrl_tick Error_Handler"

*** Test Cases ***
Firmware Boots And Control Loop Iterates
    Create Machine
    Create Log Tester           15
    # Landing in the fatal handler is an immediate, unambiguous failure.
    Register Failing Log String    Entering function Error_Handler

    Start Emulation

    # PRIMARY liveness: init completed and the loop body ran.
    Wait For Log Entry          Entering function ctrl_tick    timeout=15
    # ITERATION: the loop re-enters ctrl_tick on the next 10 ms tick.
    Wait For Log Entry          Entering function ctrl_tick    timeout=5
    # SAFETY net for the trailing window (the failing-string check above also
    # fires the instant Error_Handler is ever entered during any wait).
    Should Not Be In Log        Entering function Error_Handler    timeout=1
