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
- **Documentation standard (CONTRIBUTING.md) is binding**: docs live inline-first
  (why-comments at the decision site → file header → public-header doc comments →
  docs/ only for cross-module material); use the house markers (`[SPEC-SIGNOFF]`,
  `[SPEC-GAP]`, `EXPECTED-GAP`, `bench-pending`, `TODO(GH#n)`). CI runs
  `scripts/docs_lint.py` as the mechanical floor — run it before committing.

## Build & test

- Firmware: CMake + ARM GCC (`cmake -B build && cmake --build build`); deps
  vendored via `scripts/fetch_deps.sh`.
- Pure control core: `control/` (HAL-free) + native unit tests in `control/test/`.
- SIL gauntlet: `sim/` — 12 scenarios/148 checks, runs in CI (`sil` job).
- Renode whole-firmware emulation: `renode/` (see its README), CI `emulation` job.
- CI is the arbiter: `.github/workflows/build.yml` (tests, sil, firmware, emulation).

## Delegation policy (PM mode) — Fable-conserving (revised 2026-07-27)

The main session acts as PM/orchestrator and delegates via subagents. Roles are
defined in `.claude/agents/`. **The owner is conserving Fable for another
project: run the PM session itself on Opus (or Sonnet) by default.** Fable is
by-exception, not by-default.

- **Fable — only for** (batch these into single sittings where possible):
  the `safety-reviewer` gate below; `[SPEC-SIGNOFF]` constant sign-off batches;
  control-loop numerics / spec-gap derivation (GH#34); binary/disassembly
  interpretation; ANY session interacting with the live unit; and multi-layer
  debugging (e.g. Renode × HAL × CI) only after an Opus attempt has stalled.
- **Opus (main PM session default)** — orchestration, agent-report review,
  test/CI gates, commits, tracker sync; code review of everything NOT on the
  safety path; modules with tricky fault/concurrency semantics.
- **Sonnet (`implementer`)** — spec-driven implementation: drivers, CAN
  encoders/driver, test-fw, SIL scenarios, client app/CLI, telemetry. The RE
  facts are settled; cite the V-item you build on.
- **Haiku (`doc-writer`)** — doc extraction, changelog/version/issue
  bookkeeping, log/CSV decoding, mechanical edits.

**Review gate (unchanged, the one mandatory Fable use):** any change touching
the field-drive path, fault ladder, rotor clamp, or safe-state code gets a
Fable-tier review (`safety-reviewer`) before commit, regardless of author.
Everything else: Opus review suffices.
