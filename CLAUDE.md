# WS500-OpenFW — Claude Code project guide

Clean-slate open firmware (MIT) for the Wakespeed WS500 alternator regulator
(STM32F072RB, Cortex-M0). **Master tracker: `docs/PROJECT_PLAN.md`** — read it
before planning any work; it is the single source of truth for milestones,
verified hardware facts (§0.6 V1–V8), and safety rules (§5).

## Hard rules

- **Exactly one WS500 exists, installed and live on a 48 V system.** Every
  hardware-facing action follows the staged ladder in PROJECT_PLAN §5
  (readings → config → bench flash). No custom firmware touches the unit
  before M1 (proven DFU backup/restore) and the §8 virtual gauntlet are green.
- **No GPL code in-tree, ever** (PROJECT_PLAN §0.5). The VSR upstream source is
  reference/validation only. Dependency additions require a license check
  (approved set: CMSIS Apache-2.0, STM32 HAL BSD-3, NMEA2000 MIT, Unity MIT).
- **Spec wins over code.** `docs/CONTROL_SPEC_NEXTGEN.md` + `docs/PROFILE_SPEC_LFP.md`
  are authoritative; legacy patterns are not carried forward.
- Hardware facts follow the evidence precedence in §0.6: bench > stock binary >
  upstream STM32 prototype > AVR source. Cite provenance when adding facts.

## Build & test

- Firmware: CMake + ARM GCC (`cmake -B build && cmake --build build`); deps
  vendored via `scripts/fetch_deps.sh`.
- Pure control core: `control/` (HAL-free) + native unit tests in `control/test/`.
- SIL gauntlet: `sim/` — 12 scenarios/148 checks, runs in CI (`sil` job).
- Renode whole-firmware emulation: `renode/` (see its README), CI `emulation` job.
- CI is the arbiter: `.github/workflows/build.yml` (tests, sil, firmware, emulation).

## Delegation policy (PM mode)

The main session acts as PM/orchestrator and delegates via subagents, using the
tier mapping in PROJECT_PLAN-derived planning (rule of thumb below). Roles are
defined in `.claude/agents/`.

- **Fable (main session / `safety-reviewer`)** — decisions, binary/disassembly
  interpretation, control-loop numerics, §7 safety architecture, debugging
  multi-layer failures (e.g. Renode × HAL × CI), and ANY session interacting
  with the live unit.
- **Sonnet (`implementer`)** — spec-driven implementation: drivers (INA226,
  EEPROM, stator RPM, DIO), CAN encoders, test-fw, SIL scenarios, client
  app/CLI, telemetry. The RE facts are settled; cite the V-item you build on.
- **Haiku (`doc-writer`)** — doc extraction from existing text, changelog/
  version/issue bookkeeping, log/CSV decoding, mechanical edits.

**Review gate:** any change touching the field-drive path, fault ladder,
rotor clamp, or safe-state code gets a Fable-tier review (`safety-reviewer`)
before commit, regardless of who wrote it.
