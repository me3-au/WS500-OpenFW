# Renode whole-firmware emulation harness

Deliverable #19 · issue #25 · **PROJECT_PLAN §8.2**.

This harness boots the real `ws500-openfw.elf` — HAL, drivers, control core, the
lot — on an emulated **STM32F072RB (Cortex-M0)** inside
[Renode](https://renode.io), with no physical WS500 attached. Where §8.1 (the
SIL gauntlet) exercises the *pure control core* against a plant model, §8.2
proves the *whole firmware image* survives its own bring-up and keeps its 10 ms
control loop turning on the actual target peripherals.

> ### Status: liveness case CI-green; fault case authored against a probed emulator
> The liveness test has been green in CI since 2026-07-26. The fault-path case
> (added 2026-07-27 with issue #27) was written against **Renode 1.16.1 run
> locally** — the two emulator behaviours it depends on were probed directly
> with a hand-built flash image (see [Fault-path test](#fault-path-test-project_plan-7-r3--issue-27--m3-exit-criterion)),
> and the Robot file parses under the same `TestSuiteBuilder` renode-test uses.
> What is still unproven locally is the firmware side: there is no ARM
> toolchain on the authoring machine, so the ELF the test loads has never been
> built here. **The first CI run is the real test of the fault case.**

## Files

| File | Role |
|------|------|
| `ws500f072.repl` | Platform. Thin overlay on Renode's stock `platforms/cpus/stm32f072.repl` (128 KB flash @ `0x08000000`, 16 KB SRAM @ `0x20000000` — matches the linker script). Documents the HSI48, BOOT0-mirror and IWDG gaps. |
| `ws500-openfw.resc` | Interactive boot script: create machine, load platform, `LoadELF`, `start`. Parameterised by `$elf` (default `build/ws500-openfw.elf`). |
| `ws500.robot` | The CI tests (Robot Framework): whole-firmware liveness, and the §7 R3 fault-path case described below. |
| `ws500-stock-trace.resc` | **Diagnostic, not CI.** Boots the *stock* binary and logs RCC/TIM1/ADC/I2C bus traffic for PROJECT_PLAN §0.6 **V6**. |

## Renode models used

The stock `stm32f072.repl` (→ `stm32f0.repl`) supplies everything the firmware
touches; **no peripheral had to be hand-written**. Notable choices:

- **RCC** — `Miscellaneous.STM32F0_RCC`. Mirrors `SWS←SW`, `HSERDY←HSEON`,
  `PLLRDY←PLLON`, `HSI14RDY←HSI14ON`. **Does not** implement HSI48 (see risks).
- **ADC** — `Analog.STM32F0_ADC` (+ `DMA.STM32G0DMA`, channel 1). Writing `ADEN`
  sets `ADRDY`; `ADCAL` is a tag that reads back 0, so
  `HAL_ADCEx_Calibration_Start`'s wait exits immediately (no hang predicted).
- **Timers** — `Timers.STM32_Timer` for TIM1 (field PWM) and TIM2/7 (timebase).
- **I2C** — `I2C.STM32F7_I2C` ×2. No INA226/EEPROM device model exists in
  Renode; see the I2C-stub risk.
- **CAN** — `CAN.STMCAN` (bxCAN). **EXTI** — `IRQControllers.STM32F4_EXTI`
  (stator pulse on PA10).
- **Stubbed as silent `Tag` regions** (writes logged, reads 0, never fault the
  bus): **IWDG**, **USB**, **CRS**, **PWR**, **SYSCFG/COMP**, **DBGMCU**. This is
  what lets `board_clock_config()`'s CRS setup and the IWDG init/refresh run
  without a real model.

## How liveness is asserted

The firmware exposes no console UART, so the test does not watch a serial line.
Instead it uses the CPU's **function-name logging** (which needs the ELF's
`.symtab` symbols — the build does not strip them):

```
sysbus.cpu LogFunctionNames true "ctrl_tick Error_Handler"
```

That emits `Entering function ctrl_tick at 0x...` on every entry. The Robot test
(`ws500.robot`) then:

1. `Create Log Tester 15` and `Register Failing Log String  Entering function Error_Handler`
   — any entry into the fatal handler (`field_drive_off()` + `for(;;){}`) fails
   the test **immediately**.
2. `Wait For Log Entry  Entering function ctrl_tick` — **primary**: `main()`
   reached the loop and called `ctrl_tick`.
3. `Wait For Log Entry  Entering function ctrl_tick` (again) — **iteration**: the
   `for(;;)` loop genuinely re-enters on the next SysTick-gated 10 ms tick, so
   we're proving the loop *runs*, not just that `main()` was entered once.
4. `Should Not Be In Log  Entering function Error_Handler  timeout=1` — trailing
   safety window.

Emulated time advances automatically while the CPU runs (SysTick is core, driven
by the platform clock), so `HAL_GetTick` / the 10 ms gate progress on their own.

## Fault-path test (PROJECT_PLAN §7 R3 · issue #27 · M3 exit criterion)

The second Robot case, **`Induced Fault Reaches Safe State And Reboots`**, is the
emulated half of M3's exit criterion: *"induced fault provably lands in safe
state + crash record + clean reboot"*. It boots the firmware, waits for control
loop liveness, injects a fault, and then asserts, in order:

| Assertion | What it proves |
|---|---|
| `Entering function fault_nmi_stage2` | the two-stage handler works: the **naked Thumb-1 stage-1** assembled correctly, decoded EXC_RETURN, re-pointed MSP and tail-called C |
| `Entering function enter_safe_state` | the **§7 R0 funnel** ran — TIM1 `MOE=0`, CCR 0, field pins driven low |
| `Resetting platform with SYSRESETREQ` | the firmware **rebooted rather than spun** (this line comes from Renode's NVIC model when AIRCR.SYSRESETREQ is written, so nothing else can produce it) |
| `Entering function ctrl_tick` ×2, after the reset | the reboot **succeeded** and the 10 ms control loop is turning again |

### Two platform facts this test needed, both established by probing Renode 1.16.1

Neither was assumed; each was checked by running a hand-built flash image
(vector table + a few Thumb opcodes poked in with `sysbus WriteDoubleWord`) on
the real emulator before the test was written.

1. **An undefined instruction does not produce a HardFault on the emulated
   Cortex-M0.** Renode dispatches it to **UsageFault (exception 6)**, which does
   not exist on ARMv6-M. Vector slot 0x18 is a reserved zero in the ST startup
   table, so the emulated CPU ends up executing address 0 and wandering — the
   firmware's HardFault handler is never reached. Setting `PC` to an unmapped
   address is no better: Renode halts the CPU (*"Trying to execute code outside
   RAM or ROM"*) without raising any exception at all.
   **Therefore the injection is `SCB->ICSR.NMIPENDSET` (write `0x80000000` to
   `0xE000ED04`)**, which the NVIC model implements exactly. NMI is a real §7 R3
   vector (SRAM parity) and shares the entire path — stage-1 asm, C stage,
   `enter_safe_state()`, crash record, `NVIC_SystemReset()` — with HardFault.
   The only step not covered is the CPU's own HardFault *dispatch*, which is
   silicon behaviour, not firmware behaviour.
2. **A software reset needs the BOOT0 flash mirror at `0x00000000`.** The
   Cortex-M0 has no VTOR, so after any reset it fetches SP/PC from address 0.
   The stock platform maps flash at `0x08000000` only, so `SYSRESETREQ` left the
   CPU halted. The test therefore re-registers the flash memory at **both**
   addresses before `LoadELF`, which is what the hardware does with BOOT0=0.
   (`sysbus Redirect` is not a substitute — the redirected region serves data
   reads but cannot be executed from, so vector fetches through it return 0.)
   Confirmed in the same probe: RAM contents **survive** the emulated reset, so
   the `.noinit` crash-record block behaves as it does on hardware.

### What this test cannot prove

- **The IWDG (§7 R1) is not emulated at all.** The platform maps IWDG as a
  silent `Tag` region: key/prescaler/reload/window writes are logged and
  discarded, `SR` reads back 0, and no reset is ever generated. So the harness
  can confirm the watchdog code *runs* (and that its bounded `SR` waits do not
  hang — which is a real risk on a stubbed peripheral), but it cannot confirm
  that starving the checkpoints resets the part, nor that the window rejects an
  early kick. Those need silicon: they are bench items for M3, and the
  `renode/README.md` gap list is the record of that. A hand-written IWDG model
  would be the alternative if starvation ever needs CI coverage.
- **The CPU's HardFault dispatch** — see fact 1 above. `HardFault_Handler`'s
  stage-1 body is byte-identical (same macro) to `NMI_Handler`'s, so what the
  test does not exercise is the emulator's, not the firmware's.
- **The PVD (§7 R5)**, because `PWR` is also a `Tag`: the threshold write goes
  nowhere and no EXTI 16 event can be generated.
- **The image CRC (§7 R4) is UNPATCHED in this environment by design.** The job
  loads the `.elf`; `scripts/embed_crc.py` patches the `.bin`. `integrity.c`
  reports `INTEGRITY_CRC_UNPATCHED` (a distinct, non-fatal status) rather than
  a mismatch, which is exactly why the boot-time check is report-only.

## Running it

Interactively (opens the Renode Monitor; `start` is issued by the script):

```
renode renode/ws500-openfw.resc
```

As the automated test (what CI runs):

```
renode-test renode/ws500.robot
```

Success prints `Tests finished successfully :)` and writes `robot_output.xml`,
`log.html`, `report.html`. The robot test anchors its paths to `${CURDIR}` (the
`renode/` directory), so it runs from anywhere; the interactive `.resc` scripts
still use repo-root-relative `@` paths, so run those from the **repo root**. The
ELF must be built first (`cmake --build build`).

Stock-firmware trace (V6 — supply your own out-of-tree image, do **not** commit
it):

```
renode -e "$bin=@'../New folder/fw_0x08000000.bin'; include @renode/ws500-stock-trace.resc"
```

**V6 was run and resolved 2026-07-26** (PROJECT_PLAN §0.6 V6 has the findings:
TIM1 143.2 Hz config, TIM7 tick, ADC DMA scan, and the I²C1→I²C2 rebind with
INA226 @0x40 + 24C16 EEPROM 0xA0-family traffic all observed live). Practical
notes from that run — the *stock* image, unlike ours, hard-checks HAL return
codes, so the bare platform is not enough:

- **`renode/v6-stubs/`** holds three PythonPeripheral stubs that were required:
  `rcc_v6.py` (RCC store-and-return with HSERDY/PLLRDY/SWS/HSI14/HSI48 ready-bit
  mirroring — the stock RCC model never raises HSERDY for the HSE+PLL path),
  `ramreg.py` (FLASH interface @0x40022000 so ACR latency readback matches), and
  `adc_v6.py` (ADC @0x40012400 with ADCAL/ADEN/ADRDY handshakes). Register them
  after `sysbus Unregister sysbus.rcc` / `sysbus.adc` via
  `machine LoadPlatformDescriptionFromString`.
- **Renode 1.16.1's PythonPeripheral request API is PascalCase**
  (`request.IsRead/IsWrite/Offset/Value`); the lowercase names in older docs
  throw.
- The stock firmware polls `uwTick` (0x200001EC) with PRIMASK set before the
  scheduler starts; under emulation the tick can starve. Workaround: stepped
  `emulation RunFor` interleaved with monitor writes advancing 0x200001EC.

## CI wiring

`.github/workflows/build.yml` gets a new **`emulation`** job that `needs:
firmware`. It downloads the `ws500-openfw` artifact (the ELF the `firmware` job
already builds and uploads) into `build/`, installs the pinned **Renode 1.16.1
Linux portable** release (which ships `renode-test`), installs the Robot test
Python deps, and runs `renode-test renode/ws500.robot` — which executes **every**
case in that file, so adding the fault-path test needed no workflow change. The
job passes **iff** all robot tests pass. The existing `tests`, `sil`, and
`firmware` jobs are untouched.

Why this shape rather than the alternatives:
- **vs. `container: antmicro/renode:latest`** — a job-level container runs
  `actions/checkout` / `download-artifact` *inside* the image, which needs
  Node 20 that the Renode image doesn't guarantee. Running on the host runner
  and installing the portable release avoids that entirely.
- **vs. `antmicro/renode-test-action`** — that composite action *builds Renode
  from source* every run (~30 min). The portable release is a fast, pinned
  binary. The action remains a clean fallback if the portable route needs
  Xvfb/deps we didn't anticipate.

## Bring-up risks (the first CI run settles these)

Listed worst-first. Each is a concrete "if red, here's why."

1. **RCC HSI48RDY absent (most likely to bite).** `STM32F0_RCC` leaves CR2
   bits 16/17 reserved, so `HSI48RDY` never reads 1. `board_clock_config()`
   **ignores** the `HAL_RCC_OscConfig` / `HAL_RCC_ClockConfig` return codes, so
   the expectation is a bounded ~2 ms `HAL_TIMEOUT` (gated on SysTick) and then
   execution continues on the reset HSI clock — survivable. **Risk:** if the
   vendored HAL build's HSI48 wait is *not* bounded, or SysTick isn't advancing
   yet at that point, this hangs before `ctrl_tick`. *Fallback if red:* a small
   custom RCC model (or a `.cs` register-init) that forces `HSI48RDY`; a plain
   `sysbus WriteDoubleWord` can't set a reserved bit.
2. **CAN (bxCAN) init handshake.** `HAL_CAN_Init` / `HAL_CAN_Start` wait on
   `INAK`/`SLAK` in `CAN_MSR`. If `STMCAN` doesn't mirror the `INRQ`→`INAK`
   transition, these hit their HAL timeouts (survivable if the return is
   ignored) or hang (if not). Unverified — CI will show it in the log.
3. **ADC / DMA scan.** Predicted OK (ADEN→ADRDY modelled; ADCAL reads 0 so
   calibration returns immediately; DMA channel 1 wired). **Risk:** the circular
   DMA request wiring or `HAL_ADC_Start_DMA` enable sequence differs enough to
   error — but that shouldn't gate liveness (the loop reads a buffer).
4. **I2C stub adequacy.** No INA226/EEPROM model; `ina2xx.c` uses a 5 ms bounded
   HAL timeout, so reads NACK/time out without stalling. **Risk:** if any I2C
   path does an *unbounded* poll (none seen), or config-store EEPROM access at
   init blocks, the loop stalls. Attach a mock device (see `.resc`) if red.
5. **Toolchain / harness plumbing.** (a) `LogFunctionNames` needs `ctrl_tick` /
   `Error_Handler` in `.symtab` — present unless the build strips (it doesn't).
   (b) `using "platforms/cpus/stm32f072.repl"` must resolve inside the portable
   Renode (it's on the default Monitor PATH). (c) The portable tarball must
   expose `renode-test` and its Python `requirements.txt`; if renode-test needs
   a virtual display, wrap it in `xvfb-run`.
