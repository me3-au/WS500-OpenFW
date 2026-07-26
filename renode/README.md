# Renode whole-firmware emulation harness

Deliverable #19 · issue #25 · **PROJECT_PLAN §8.2**.

This harness boots the real `ws500-openfw.elf` — HAL, drivers, control core, the
lot — on an emulated **STM32F072RB (Cortex-M0)** inside
[Renode](https://renode.io), with no physical WS500 attached. Where §8.1 (the
SIL gauntlet) exercises the *pure control core* against a plant model, §8.2
proves the *whole firmware image* survives its own bring-up and keeps its 10 ms
control loop turning on the actual target peripherals.

> ### Status: authored and CI-validated, **not yet locally run**
> Neither Renode nor an ARM toolchain is installed on the authoring machine, so
> these files were written against the **current** Renode platform library,
> Robot keyword set, and CI tooling (v1.16.1), but have **not** been executed
> locally. **The first CI run is the real test.** The harness is *expected* to
> work; the concrete things that run will confirm or expose are in
> [Bring-up risks](#bring-up-risks-the-first-ci-run-settles-these) below. Do not
> read a green checkmark here — read the CI log.

## Files

| File | Role |
|------|------|
| `ws500f072.repl` | Platform. Thin overlay on Renode's stock `platforms/cpus/stm32f072.repl` (128 KB flash @ `0x08000000`, 16 KB SRAM @ `0x20000000`, `_estack = 0x20004000` — matches the linker script). Documents the HSI48 gap. |
| `ws500-openfw.resc` | Interactive boot script: create machine, load platform, `LoadELF`, `start`. Parameterised by `$elf` (default `build/ws500-openfw.elf`). |
| `ws500.robot` | The CI liveness test (Robot Framework). |
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

## CI wiring

`.github/workflows/build.yml` gets a new **`emulation`** job that `needs:
firmware`. It downloads the `ws500-openfw` artifact (the ELF the `firmware` job
already builds and uploads) into `build/`, installs the pinned **Renode 1.16.1
Linux portable** release (which ships `renode-test`), installs the Robot test
Python deps, and runs `renode-test renode/ws500.robot`. The job passes **iff**
the robot test passes. The existing `tests`, `sil`, and `firmware` jobs are
untouched.

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
