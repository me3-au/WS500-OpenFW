---
name: Hardware fact / correction
about: Report or correct a reverse-engineered hardware fact, with provenance
title: 'HW fact: '
labels: hardware-re
assignees: ''
---

<!-- This project's hardware interface is reverse-engineered (docs/WS500_HARDWARE_SPEC.md,
     docs/IO_COVERAGE.md). Facts are only as good as their provenance — a claim without
     evidence is a lead, not a fact. Both are welcome; just label which this is. -->

**The fact (or correction)**
<!-- One precise statement. e.g. "PA3 is the FET-temperature NTC input, not battery temp."
     If correcting, quote the current claim and where it's documented. -->

**Source** (pick one or more)
- [ ] Binary disassembly of the stock DFU image
- [ ] Bench measurement (scope/DMM/logic analyzer on real hardware)
- [ ] Upstream VSR source (reference only — cite file/line; no code gets copied)
- [ ] Datasheet / reference manual / schematic
- [ ] Other (explain)

**Evidence**
<!-- Whatever lets someone else verify it: disassembly addresses + relevant register
     values, scope capture / measurement numbers + setup, VSR file:line, datasheet
     section, photos. Note: bench measurements on the WS500 are gated by the project
     safety rules (docs/PROJECT_PLAN.md §5) — say which access stage the measurement
     was taken under. -->

**Confidence**
<!-- Confirmed (two independent sources) / measured once / inferred / hearsay -->

**What it affects**
<!-- e.g. Core/Inc/board.h constant X, sensors scaling, a spec section, a §0.6
     verification item -->
