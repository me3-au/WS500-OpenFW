# What & why

<!-- One logical change per PR. Link the issue if there is one. -->

# Checklist

- [ ] **CI green** — native control-core tests + ARM firmware build both pass.
- [ ] **No GPL-derived code** — nothing copied or adapted from the VSR upstream or any
      other GPL source (reference/validation use is fine; expression is not).
- [ ] **New dependencies license-checked** — permissive only (approved set in
      CONTRIBUTING.md), or N/A.
- [ ] **Control-core changes have unit tests** in `control/test/`, or N/A.
- [ ] **Safety-critical constants unchanged** — rotor duty clamp, compiled max-duty cap,
      fault thresholds, fail-safe defaults — or the change is explicitly justified below
      against `docs/CONTROL_SPEC_NEXTGEN.md`.
- [ ] **Specs updated** if behavior changed (`CONTROL_SPEC_NEXTGEN.md` /
      `PROFILE_SPEC_LFP.md`), or N/A.
- [ ] **Hardware facts carry provenance** (disassembly / bench / VSR reference /
      datasheet), or N/A.
- [ ] **Documentation is inline and self-explanatory** per the CONTRIBUTING
      "Documentation standard": why-comments at the decision site, doc comments on
      new public prototypes, house markers used (`[SPEC-SIGNOFF]`, `TODO(GH#n)`, …);
      `scripts/docs_lint.py` passes (CI enforces).

# Safety-critical justification (if applicable)

<!-- Only if the safety checklist item is not "unchanged": what changed, why it is safe,
     and how it was verified without hardware. -->
