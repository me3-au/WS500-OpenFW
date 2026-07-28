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
# NOT @-prefixed: this is embedded as a literal filename string inside a
# `LoadPlatformDescriptionFromString "... filename: \"...\" ..."` command
# (Renode's own DSL for a Python.PythonPeripheral's script path), not passed
# as a monitor-command argument -- the `@` resolution marker only applies to
# the latter. Plain ${CURDIR}-anchored absolute path, confirmed working
# against Renode 1.16.1 by direct probing (a bare relative path here fails
# with "Could not find source file for the script").
${IWDG_STUB}    ${CURDIR}${/}iwdg-stub${/}iwdg_stub.py
# These two are loaded via the monitor's `include "<path>"` command (like
# tests/platforms/Renesas_DA14592.robot's echo-i2c-peripheral.py), not a
# `filename:` field inside a LoadPlatformDescriptionFromString string — a
# different code path than ${IWDG_STUB}'s, so it is not assumed to need the
# same forward-slash treatment (see Create I2C Machine below if it turns out
# it does).
${INA226_STUB}    ${CURDIR}${/}i2c-stubs${/}ina226_stub.py
${EEPROM_STUB}     ${CURDIR}${/}i2c-stubs${/}eeprom24c16_stub.py

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

Create Watchdog Machine
    [Documentation]    Same BOOT0=0 flash alias as Create Fault Machine (a
    ...    watchdog-triggered reset needs it for exactly the same reason a
    ...    software one does), plus the §7 R1 IWDG model attached over the
    ...    platform's inert IWDG Tag region (renode/README.md "KNOWN GAP —
    ...    IWDG"; renode/iwdg-stub/iwdg_stub.py has the full rationale). No
    ...    `sysbus Unregister` is needed for the IWDG swap: probed directly
    ...    against Renode 1.16.1, a Python.PythonPeripheral declared at the
    ...    same address as an unregistered Tag region simply takes priority.
    Execute Command             using sysbus
    Execute Command             mach create "ws500"
    Execute Command             machine LoadPlatformDescription ${PLATFORM}
    Execute Command             sysbus Unregister sysbus.flash
    Execute Command             machine LoadPlatformDescriptionFromString "flash: Memory.MappedMemory @ { sysbus 0x08000000; sysbus 0x00000000 } { size: 0x20000 }"
    # Two escaping layers had to be worked out by hand, both confirmed by
    # direct failure, not guessed:
    #  (1) Robot Framework's own text escaping collapses a single `\"` down
    #      to a bare `"` before Execute Command ever sees it, so `\\"` is
    #      needed to hand Renode a literal `\"`.
    #  (2) Renode's own `.repl`-style string literal (the quoted value of
    #      `filename:`) treats backslash as an escape leader itself, so a
    #      raw Windows path (`C:\Users\...`) inside it is a syntax error —
    #      it must be forward-slash. `$IWDG_STUB` (not `${IWDG_STUB}`) passes
    #      the variable as a real Python value into Evaluate instead of first
    #      substituting it as text, which sidesteps a THIRD layer of escaping
    #      that textual substitution would add here.
    ${IWDG_STUB_FS}=    Evaluate    $IWDG_STUB.replace(chr(92), '/')
    Execute Command             machine LoadPlatformDescriptionFromString "iwdg: Python.PythonPeripheral @ sysbus 0x40003000 { size: 0x400; initable: true; filename: \\"${IWDG_STUB_FS}\\" }"
    Execute Command             logLevel -1 sysbus.iwdg
    Execute Command             sysbus LoadELF ${ELF}
    Execute Command             sysbus.cpu LogFunctionNames true "ctrl_tick Error_Handler"
    # Same reason as Create Fault Machine: Machine.RequestReset() (what
    # iwdg_stub.py calls) kills LogFunctionNames exactly like SYSRESETREQ does
    # — confirmed by direct probing, not assumed — so the post-reset liveness
    # check needs an address hook captured now, before the reset wipes the
    # symbol store.
    ${a}=    Execute Command    sysbus GetSymbolAddress "ctrl_tick"
    ${a}=    Evaluate           $a.strip()
    Set Test Variable    ${TICK_ADDR}    ${a}

Create I2C Machine
    [Documentation]    Same as Create Machine, plus the §8.2 "I2C device
    ...    stubs" deliverable: an INA226 on I2C1 0x40 and an eight-block
    ...    24C16 EEPROM on I2C2 0x50-0x57, so Core/Src/ina2xx.c and
    ...    Core/Src/eeprom24c16.c run against something that answers instead
    ...    of NACKing (renode/i2c-stubs/ has the full protocol rationale,
    ...    including a real bug this harness caught and fixed).
    Execute Command             using sysbus
    Execute Command             mach create "ws500"
    Execute Command             machine LoadPlatformDescription ${PLATFORM}
    Execute Command             machine LoadPlatformDescriptionFromString "inaslave: Mocks.DummyI2CSlave @ i2c1 0x40"
    Execute Command             include "${INA226_STUB}"
    Execute Command             setup_ina226_stub "sysbus.i2c1.inaslave"
    # Eight DummyI2CSlave instances, one per 24C16 block-select address
    # (0x50..0x57) — see eeprom24c16_stub.py's DEVICE-ADDRESS MODEL note for
    # why one instance cannot cover all eight.
    FOR    ${block}    IN RANGE    8
        ${addr}=    Evaluate    hex(0x50 + $block)
        Execute Command    machine LoadPlatformDescriptionFromString "eeprom${block}: Mocks.DummyI2CSlave @ i2c2 ${addr}"
    END
    Execute Command             include "${EEPROM_STUB}"
    FOR    ${block}    IN RANGE    8
        Execute Command    setup_eeprom24c16_stub "sysbus.i2c2.eeprom${block}" ${block}
    END
    Execute Command             sysbus LoadELF ${ELF}
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

IWDG Starvation Resets The Regulator
    [Documentation]    §7 R1 + the §8.2 "watchdog starvation" item: prove that
    ...    when the checkpoint-kicked IWDG stops being fed, the MCU resets
    ...    rather than continuing to run with the field energised, and the
    ...    regulator comes back up healthy afterwards. Renode ships no IWDG
    ...    model at all (renode/README.md "KNOWN GAP — IWDG"), so this uses
    ...    renode/iwdg-stub/iwdg_stub.py — a real virtual-time-driven
    ...    countdown, armed off whatever PR/RLR/WINR watchdog_init() actually
    ...    programs, not a value this test invents.
    Create Watchdog Machine
    Create Log Tester           15
    Register Failing Log String    Entering function Error_Handler

    Start Emulation

    # Liveness first, same reasoning as the other two cases: without this, a
    # firmware that never reached watchdog_init() would "pass" by never
    # arming anything.
    Wait For Log Entry          Entering function ctrl_tick    timeout=15
    Wait For Log Entry          Entering function ctrl_tick    timeout=5

    # STARVATION. IWDG_TEST_REG_STARVE (iwdg_stub.py) sits at 0x40003100 —
    # 0x100 past the IWDG's real 0x00-0x10 register range, so real firmware
    # traffic can never reach it by accident. Writing it makes the stub start
    # silently swallowing further KR=0xAAAA kicks. This stands in for a hung
    # checkpoint task (a wedged I2C/CAN/comms service, say): watchdog_service()
    # itself sees nothing wrong — HAL_GetTick() keeps advancing normally and
    # every checkpoint still looks fresh — it is the kick ITSELF that goes
    # missing, exactly like a real stuck peripheral driver would produce.
    # sysbus.cpu is deliberately NOT paused to simulate this: that was tried
    # first and rejected (see iwdg_stub.py's header) because Renode 1.16.1's
    # `sysbus.cpu Pause` is a transient pause-then-resume, not a sustained
    # halt, so the firmware kept right on kicking through it.
    Execute Command              sysbus WriteDoubleWord 0x40003100 1

    # The stub logs this the instant its scheduled deadline fires with no
    # intervening kick — i.e. exactly watchdog_init()'s own programmed period
    # (§7 R1: PR=4/RLR=62 → 100.8 ms nominal) after the last real kick.
    Wait For Log Entry          requesting reset    timeout=30

    # RE-ARM OBSERVABILITY, same technique as the NMI fault-path test above:
    # Machine.RequestReset() kills LogFunctionNames exactly like SYSRESETREQ
    # does — confirmed by direct probing for THIS reset path too (zero
    # "Entering function" lines survive it) — so address hooks stand in.
    Execute Command              pause
    Execute Command              sysbus.cpu AddHook ${TICK_ADDR} "self.ErrorLog('WDGPOSTRESET ctrl_tick alive')"
    Execute Command              start

    # The reboot succeeded and the loop is cycling again, not a one-shot
    # entry — the same "field not left energised while hung" claim as the NMI
    # test, proven the same way: the regulator is provably alive AFTER a
    # provable reset, never spinning through one.
    Wait For Log Entry          WDGPOSTRESET ctrl_tick alive    timeout=20
    Wait For Log Entry          WDGPOSTRESET ctrl_tick alive    timeout=10
    Should Not Be In Log        Entering function Error_Handler    timeout=1

I2C Stubs Answer Real Sensor And Config Reads
    [Documentation]    §8.2 "I2C device stubs": prove the INA226/EEPROM stubs
    ...    are not just present but actually exercised, AND that the values
    ...    that reach the firmware are correct — not merely present. The
    ...    second half matters: an earlier version of this harness answered
    ...    every register with a plausible-looking but WRONG value (a queued-
    ...    reply-timing bug, see renode/i2c-stubs/ina226_stub.py's
    ...    WIRE-PROTOCOL MODEL section) and nothing here would have caught it
    ...    without reading the driver's own computed result back out of RAM.
    Create I2C Machine
    Create Log Tester           15
    Register Failing Log String    Entering function Error_Handler
    # Any address neither stub answers means a real driver transaction NACKed
    # or timed out — the exact failure mode this deliverable exists to close.
    Register Failing Log String    Unknown slave at address

    Start Emulation

    # EEPROM FIRST, deliberately. The Log Tester's cursor only ever moves
    # forward: config_store_glue.c's lazy cfg_store_load() reads through
    # eeprom24c16_read() during boot, strictly BEFORE main()'s for(;;) loop
    # ever calls ctrl_tick — confirmed by direct probing, every "EEPROM stub"
    # line in the log precedes the first "Entering function ctrl_tick" one.
    # Waiting for ctrl_tick first (as this test originally did) moves the
    # cursor past the EEPROM traffic, and it can never be found afterward —
    # not because it did not happen, but because the tester was told to stop
    # looking for it before it started looking.
    Wait For Log Entry          EEPROM stub: block    timeout=15

    Wait For Log Entry          Entering function ctrl_tick    timeout=15
    Wait For Log Entry          Entering function ctrl_tick    timeout=5

    # INA226. ina2xx.c's ina_sample() runs on every ina2xx_current_a()/
    # ina2xx_bus_v() call (sensors.c calls both every 10 ms tick), gated on
    # the stub's CVRF bit, so by now the driver's own cache — s_current_a /
    # s_bus_v, static file-scope symbols in ina2xx.c, confirmed resolvable
    # and cross-checked against build/ws500-openfw.map's addresses — holds
    # whatever it actually computed from real wire bytes. Reading it directly
    # (not just checking the stub logged a poll) is what proves the VALUE is
    # right, not just that a transaction happened.
    ${cur_addr}=    Execute Command    sysbus GetSymbolAddress "s_current_a"
    ${cur_raw}=      Execute Command    sysbus ReadDoubleWord ${cur_addr}
    ${current_a}=    Evaluate    struct.unpack('<f', struct.pack('<I', int($cur_raw.strip(), 0)))[0]    struct
    Should Be True    abs(${current_a} - 50.0) < 0.01

    ${bus_addr}=     Execute Command    sysbus GetSymbolAddress "s_bus_v"
    ${bus_raw}=      Execute Command    sysbus ReadDoubleWord ${bus_addr}
    ${bus_v}=        Evaluate    struct.unpack('<f', struct.pack('<I', int($bus_raw.strip(), 0)))[0]    struct
    Should Be True    abs(${bus_v} - 24.0) < 0.01

    Should Not Be In Log        Entering function Error_Handler    timeout=1
