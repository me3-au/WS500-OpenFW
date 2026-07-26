*** Comments ***
# WS500-OpenFW whole-firmware tests  (PROJECT_PLAN §8.2, issues #25 and #27).
#
# renode-test injects the Renode Robot keyword library automatically, so this
# file needs no Settings/Resource import (mirrors Renode's own stock tests,
# e.g. tests/platforms/STM32F072b.robot). It also gives every test case a
# default `Reset Emulation` setup, so each case starts from a clean emulation
# and may create its own "ws500" machine.
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
#                   log string, so diverging into Error_Handler fails the test
#                   immediately, and a trailing Should Not Be In Log guards the
#                   window after iteration.
#
# TEST 2 — the §7 R3 fault path. See renode/README.md "Fault-path test" for the
# emulator facts behind the injection method (they were established by probing
# Renode 1.16.1 directly, not assumed) and for what this test can and cannot
# prove.

*** Variables ***
# ${CURDIR}-anchored so resolution is independent of Renode's working directory
# (renode-test does not run Renode from the repo root — first CI run proved it).
${ELF}          @${CURDIR}${/}..${/}build${/}ws500-openfw.elf
${PLATFORM}     @${CURDIR}${/}ws500f072.repl

*** Keywords ***
Create Machine
    Execute Command             using sysbus
    Execute Command             mach create "ws500"
    Execute Command             machine LoadPlatformDescription ${PLATFORM}
    Execute Command             sysbus LoadELF ${ELF}
    # Info-level "Entering function <name> at 0x..." for just these two symbols.
    Execute Command             sysbus.cpu LogFunctionNames true "ctrl_tick Error_Handler"

Create Fault Machine
    [Documentation]    Same machine as Create Machine, plus the BOOT0=0 flash
    ...    alias a software reset needs, and the §7 R3 symbol set.
    Execute Command             using sysbus
    Execute Command             mach create "ws500"
    Execute Command             machine LoadPlatformDescription ${PLATFORM}
    # BOOT ALIAS. On the real STM32F072 with BOOT0=0 the flash is mirrored at
    # 0x00000000, and that mirror is where a Cortex-M0 fetches the initial SP
    # and PC after ANY reset (the M0 has no VTOR). Renode's stock stm32f072
    # platform maps flash at 0x08000000 only, so without this the CPU halts
    # with "PC does not lay in memory" the instant NVIC_SystemReset() lands —
    # verified directly against Renode 1.16.1. Registering the SAME memory at
    # both addresses (rather than sysbus Redirect, which is not executable) is
    # what makes the mirror behave like the hardware's.
    Execute Command             sysbus Unregister sysbus.flash
    Execute Command             machine LoadPlatformDescriptionFromString "flash: Memory.MappedMemory @ { sysbus 0x08000000; sysbus 0x00000000 } { size: 0x20000 }"
    Execute Command             sysbus LoadELF ${ELF}
    Execute Command             sysbus.cpu LogFunctionNames true "ctrl_tick enter_safe_state fault_nmi_stage2 fault_hardfault_stage2 Error_Handler"
    # Capture the hook addresses NOW, while the symbol store exists — the
    # SYSRESETREQ machine reset wipes it (which is also why LogFunctionNames
    # dies at reset and cannot be re-armed; verified on Renode 1.16.1).
    ${a}=    Execute Command    sysbus GetSymbolAddress "ctrl_tick"
    ${a}=    Evaluate           $a.strip()
    Set Test Variable    ${TICK_ADDR}    ${a}
    # Error_Handler is deliberately NOT hooked: since the §7 rework nothing
    # references it and --gc-sections removes it from the image entirely (no
    # symbol to resolve). The meaningful post-reset failure mode is the fault
    # path RE-firing — a crash loop — so that is what the failing guard hooks.
    ${b}=    Execute Command    sysbus GetSymbolAddress "fault_hardfault_stage2"
    ${b}=    Evaluate           $b.strip()
    Set Test Variable    ${HF2_ADDR}    ${b}

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

Induced Fault Reaches Safe State And Reboots
    [Documentation]    PROJECT_PLAN §7 R3 + the M3 exit criterion: an induced
    ...    fault must land in enter_safe_state(), write a crash record and
    ...    REBOOT — never spin with the field energized.
    Create Fault Machine
    Create Log Tester           30
    Register Failing Log String    Entering function Error_Handler

    Start Emulation

    # Establish liveness first: without this, a firmware that never booted
    # would "pass" the fault assertions by never getting anywhere at all.
    Wait For Log Entry          Entering function ctrl_tick    timeout=15
    Wait For Log Entry          Entering function ctrl_tick    timeout=5

    # INJECTION. Pend an NMI through SCB->ICSR.NMIPENDSET (0xE000ED04 bit 31)
    # with the CPU paused, exactly as Renode's own tests pause around monitor
    # pokes. NMI is the §7 R3 SRAM-parity vector and shares the entire fault
    # path with HardFault: naked stage-1 asm -> C stage -> enter_safe_state()
    # -> crash record -> NVIC_SystemReset().
    #
    # Why not an undefined instruction / a jump to a bad address: probed
    # against Renode 1.16.1 and neither works on this core model — an
    # undefined opcode is dispatched to UsageFault (exception 6), which does
    # not exist on a Cortex-M0 and whose vector-table slot is a reserved zero,
    # so the emulated CPU ends up executing address 0 instead of taking
    # HardFault; and setting PC to unmapped memory halts the emulated CPU
    # ("Trying to execute code outside RAM or ROM") without any exception at
    # all. Both are emulator artefacts, not firmware behaviour. See
    # renode/README.md.
    Execute Command             pause
    Execute Command             sysbus WriteDoubleWord 0xE000ED04 0x80000000
    Start Emulation

    # 1. The C stage of the two-stage handler ran (so the naked stage-1 asm
    #    assembled correctly, found the frame, and re-pointed MSP).
    Wait For Log Entry          Entering function fault_nmi_stage2    timeout=10
    # 2. The one safe-state funnel executed — field PWM off (§7 R0).
    Wait For Log Entry          Entering function enter_safe_state    timeout=10
    # 3. The firmware asked for a reset instead of spinning. This message comes
    #    from Renode's NVIC model when AIRCR.SYSRESETREQ is written, so it can
    #    only appear as a result of our NVIC_SystemReset().
    Wait For Log Entry          Resetting platform with SYSRESETREQ    timeout=10
    # RE-ARM OBSERVABILITY, the hard-won way. The CPU reset that SYSRESETREQ
    # triggers silently kills LogFunctionNames — verified on Renode 1.16.1: the
    # post-reset log shows a full normal boot (clock config, per-tick INA
    # polling) but zero "Entering function" lines, and re-issuing
    # LogFunctionNames does NOT revive it. Address hooks DO work post-reset
    # (probed on the failure snapshot: a ctrl_tick hook fired 25x in 100 ms of
    # virtual time), so the post-reset assertions use AddHook on the ELF
    # symbols instead. Race-safe: the reboot needs seconds of virtual time to
    # reach ctrl_tick, planting the hooks takes milliseconds of wall clock.
    Execute Command             pause
    Execute Command             sysbus.cpu AddHook ${TICK_ADDR} "self.ErrorLog('POSTRESET ctrl_tick alive')"
    Execute Command             sysbus.cpu AddHook ${HF2_ADDR} "self.ErrorLog('POSTRESET fault re-entered')"
    Register Failing Log String    POSTRESET fault re-entered
    Execute Command             start
    # 4. ...and came back: the control loop is alive again after the reboot —
    #    two hook firings = the loop is cycling, not a one-shot entry.
    Wait For Log Entry          POSTRESET ctrl_tick alive    timeout=20
    Wait For Log Entry          POSTRESET ctrl_tick alive    timeout=10
    Should Not Be In Log        POSTRESET fault re-entered    timeout=1
