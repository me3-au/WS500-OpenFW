# Contributing to WS500-OpenFW

Thanks for helping keep the WS500 hardware alive. Before anything else, two hard rules —
they are not negotiable and PRs that break them will be closed:

## Rule 1 — NO GPL code in-tree, ever

This project is MIT-licensed (see `docs/PROJECT_PLAN.md` §0.5). The GPL VSR upstream
(Al Thomason's "Very Smart Regulator", the WS500's open ancestor) is **reference and
validation material only**. Facts and algorithms may be learned from it; **expression may
not be copied**. Anything that would be a paste gets independently rewritten from the spec.
The same goes for the reverse-engineered stock firmware: hardware interface facts only,
never decompiled control logic.

Any **new dependency requires a license check** before it goes in. The approved set:

| Dependency | License |
|---|---|
| CMSIS | Apache-2.0 |
| STM32Cube HAL (F0) | BSD-3-Clause |
| ttlappalainen/NMEA2000 | MIT |
| Unity test framework | MIT |

GPL/LGPL libraries are ruled out (e.g. canboat). GPL *tools* that nothing links against
(`dfu-util`, `gcc`) are fine. Propose additions to the set in an issue first.

## Rule 2 — Hardware testing is safety-gated

**Exactly one WS500 exists for this project, it is irreplaceable, and it is installed on a
live 48 V system with a 12 V-class (4 Ω) rotor.** All hardware/bench work is gated by the
safety rules in `docs/PROJECT_PLAN.md` §5 (staged access ladder, virtual-first gauntlet,
proven DFU recovery before any flash). Do not ask contributors to flash anything; the
firmware is developed and CI-tested hardware-free by design.

Consequently, contributions must **never weaken**:

- the **rotor duty clamp** (≈25 % on the installed 48 V system — confirmed on hardware),
- the **compiled max-duty cap** (the bootstrap gate driver forbids sustained 100 % duty),
- the **fail-safe defaults** (ships field-OFF; field-open on uncertainty; TIM1 break;
  raw-signal safety comparators independent of the control path).

If a change touches any safety-critical constant or path, say so explicitly in the PR and
justify it against `docs/CONTROL_SPEC_NEXTGEN.md`.

## Building

**Firmware (ARM cross-build, same as CI):**

```sh
sh scripts/fetch_deps.sh        # vendors STM32Cube HAL/CMSIS (pinned, gitignored)
cmake -B build -DCMAKE_TOOLCHAIN_FILE=cmake/gcc-arm-none-eabi.cmake
cmake --build build             # -> build/ws500-openfw.elf / .bin
```

Requires `gcc-arm-none-eabi` and CMake. No hardware needed to build.

**Control-core unit tests (native, no hardware, no HAL):**

```sh
gcc -std=c11 -Wall -Wextra -I control/Inc control/Src/*.c control/test/*.c -lm -o control_tests
./control_tests
```

CI (`.github/workflows/build.yml`) runs both jobs on every push and PR. Both must be green.

## Architecture rules

- `control/` is the **pure control core**: C11, `float`, no dynamic allocation, **no HAL
  includes**. That purity is what makes it testable on a PC and in CI — keep it.
- Drivers (`Core/`) never include control types; `main.c` wires the two together.
- The authoritative behavior specs are `docs/CONTROL_SPEC_NEXTGEN.md` and
  `docs/PROFILE_SPEC_LFP.md`. If you change behavior, change the spec in the same PR.

## Documentation standard — inline first, self-explanatory, enforced

This is an open-source firmware for a safety-relevant device: a reader with no
access to the authors must be able to trust and modify the code. Documentation
lives **as close to the thing it describes as possible**, in this order:

1. **In the code line** — a comment stating a constraint the code cannot show
   (why, not what). If a value or decision needs a paragraph, the paragraph
   goes right there, not in a wiki.
2. **In the file header** — every `.c`/`.h` opens with a comment block: what
   the module is, the spec section it implements, and its boundary rules
   (e.g. "PURE — no HAL"). Plus the `SPDX-License-Identifier: MIT` line.
3. **In the public header** — every public function/type carries a doc comment
   at its prototype: contract, units, failure behavior. Header comments are
   the API reference; there is no separate one.
4. **In `docs/`** — only what genuinely spans modules (specs, decisions,
   plans). Every `docs/*.md` is registered in the PROJECT_PLAN §0 doc map.

House idioms — use these exact markers so they stay greppable:

| Marker | Meaning |
|---|---|
| `(PROJECT_PLAN §0.6 Vn)` / spec § cites | **Provenance** — every hardware fact and behavior rule cites its source |
| `[SPEC-SIGNOFF]` | an engineering-chosen constant awaiting spec review |
| `[SPEC-GAP]` | code enforces only sanity because the spec gives no range |
| `EXPECTED-GAP` (tests) | a characterized, review-gated known defect; the test asserts it still exists, so fixing the code forces the marker's removal |
| `BUS CAVEAT` / `bench-pending` | fact believed from RE/derivation, awaiting bench confirmation |
| `TODO(GH#n):` | deferred work — must reference an issue (or `M<n>`/`bench`) |

Mechanical floor: `scripts/docs_lint.py` runs in CI and fails the build if a
source file is missing its SPDX line or file-header block, or a public header
prototype has no doc comment. The lint is the floor, not the standard —
reviewers still judge whether comments explain *why*.

## Pull requests

- One logical change per PR.
- **CI must be green** (native tests + ARM firmware build).
- **Any control-core change needs a unit test** in `control/test/`.
- Hardware facts (pin map, peripheral config, board constants) need **provenance** —
  binary disassembly, bench measurement, upstream VSR source, or datasheet. Use the
  "hardware fact" issue template if you're reporting one rather than coding it.
- Fill in the PR template checklist honestly; "N/A" is a fine answer.
- By contributing you agree your work is licensed MIT and contains no GPL-derived code.

## Reporting issues

Use the issue templates. Suspected **unsafe-charging bugs** (over-voltage, over-duty,
fail-safe bypass) should go through private vulnerability reporting instead — see
`SECURITY.md`.
