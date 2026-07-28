# stock_config_import

Translates an archived stock WS500 `$RAS:`/`$RCP:n` capture (the Stage-A
artifact — see `tools/stage_a_capture`) into a proposed PROFILE_SPEC §7
JSON overlay, plus a human-readable **mapped / converted / dropped /
unrecognised** report. This is the "one-time import tool" courtesy from
`docs/DECISION_6A_CONFIG_STRATEGY.md` (Decision #6a, "clean break"): **the
client app translates stock dumps; the firmware never speaks the `$`
protocol at all.**

**UNVALIDATED against real hardware output**: we have no real Stage-A
capture yet (`docs/STAGE_A_RUNSHEET.md` Procedure 1 hasn't run — see
`tools/stage_a_capture`). Every fixture this tool's tests run against is
**synthetic**, hand-built from the documented reply grammar, not captured
from the installed unit — see `tests/fixtures/README.md`. Drop a real
capture in once one exists; the parser reads any file with the same line
shape, real or synthetic, identically.

## Why a clean break instead of a lossy mapping

The stock 6-stage Pb charge-profile model (Bulk → Acceptance → Overcharge
→ Float → Post-Float, plus Equalize) has **no equivalent** in the
two-stage CHARGE/REST LFP profile engine this firmware implements
(`docs/PROFILE_SPEC_LFP.md`). `PROJECT_PLAN.md`'s "Design philosophy" is
explicit that this is deliberate: *"we deliberately ditch the legacy
surface (6-stage Pb machine, absorption stage, DIP switches, small-alt/half
modes, RFM/PBF/Feature-IN RPM conflicts, 12V/500Ah normalization)."* A
translator that pretended otherwise would be lossy in both directions and
would silently misrepresent what the regulator will actually do to a
battery — the worst property for a charging device
(`docs/DECISION_6A_CONFIG_STRATEGY.md`'s own stated reasoning).

So this tool does not attempt a 1:1 config migration. It converts the
small set of stock fields that **do** have a faithful native equivalent,
and explicitly, individually, reports everything else as dropped — with a
reason, not silently.

## What actually converts (and why)

| Stock field | Native field | How |
|---|---|---|
| `SCV.amp_limit` | `limits.alternator_limit_a` | direct copy — not normalized to the nominal 12V/500Ah battery (Comms & Config Guide p.62), same unit |
| `SCV.watt_limit` | `global.max_charge_power_w` | direct copy, same reason |
| `SST.sys_volts` | `global.cells_series` | **heuristic**, always flagged as such: 1x/2x/4x → 4S/8S/16S at an assumed 3V/cell nominal step. A 48V pack is commonly 15S *or* 16S (Comms & Config Guide p.9) — confirm before trusting this one |
| `SST.bc_mult` | `global.bank_capacity_ah` | `\|BC Mult\| × 500 Ah` (the stock normalization constant, Appendix D `BAT_AMPHR_NOMINAL`) |
| `SCV.warmup_delay` | `global.warmup_time_s` | `\|Warmup Delay\|` — same unit (seconds); the *sign* (disables the stock "Fast Ramp" feature) has no OpenFW equivalent and is discarded |
| `CPE[active].max_amps` | `limits.battery_c_limit` | `MaxAmps / 500` (same 500Ah normalization) — taken only from the profile the capture shows as currently active |

Every one of these is reported as **CONVERTED** (with the arithmetic shown
in the report) or **MAPPED** (plain copy) — see `stock_config_import/mapper.py`
for the full reasoning behind each choice, cited against `control/Inc/control.h`
and `control/Inc/config_doc.h`, which are the actual schema-v2 authority
(not the PROFILE_SPEC §7 sketch, which `config_doc.h`'s own header comment
says predates several of these fields).

## What's dropped, and why

Everything else — every per-stage voltage/current/time setpoint in every
Charge Profile Entry, the DIP-switch overrides, derate/small-alt/half-power
modes, PBF/RFM white-space shaping, CAN bus identity, the device
name/ID/password, DC-DC converter config (not present on this hardware
per `WS500_HARDWARE_SPEC.md`) — is dropped, each with a named, specific
reason. Nothing is silently discarded: run the tool and read the
`DROPPED` section of the report.

**The password is never carried over, full stop** — not redacted-and-kept,
*dropped entirely*, and the report itself never echoes the plaintext value
even if you feed it a raw (unredacted) capture. There is no config field
for a device password in the JSON schema in the first place.

## Install

```sh
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stock_config_import
pip install -e ".[test]"
```

Pure standard library — no runtime dependency (unlike `stage_a_capture` and
`ws500ctl`, this tool never opens a serial port, so it needs no `pyserial`).

## Usage

```sh
python -m stock_config_import path\to\capture.log
```

or, if installed as a console script:

```sh
stock-config-import path\to\capture.log
```

Writes `capture.log.proposed.json` (the overlay) and `capture.log.report.txt`
(the human-readable summary) next to the input by default; override with
`-o`/`-r`. Example against one of this repo's own synthetic fixtures:

```sh
python -m stock_config_import tools\stock_config_import\tests\fixtures\synthetic_48v_battleborn.txt -o C:\Users\adren\proposed.json -r C:\Users\adren\report.txt
```

**The output is a proposal, never applied automatically.** Per Decision #6a:
review the report, review the JSON, hand-merge what you accept into a real
config document, and only then `ws500ctl set -i merged.json --dry-run`
before a real `set`.

### The proposed JSON has no `"schema_version"` key, on purpose

Per `docs/VERSIONING.md` §3 and `ws500ctl`'s own `set` command ("absent is
sent as a hand-written overlay edit"), an absent `schema_version` marks
this as a partial overlay meant to be merged onto a known-good baseline —
not a self-sufficient document. That's exactly what this tool's output is:
a handful of confidently-converted fields, nothing more.

## Testing

```sh
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stock_config_import
pip install -e ".[test]"
pytest -v
```

31 tests across four files:

- `tests/test_parser.py` — field extraction against the synthetic
  fixtures; tolerant handling of a shorter-than-current-schema `CST;` line
  (lifted verbatim from the Comms & Config Guide's own older-firmware
  example, p.57 — real field-count drift, not a fabricated edge case);
  extra-token capture; detection of a wholly unrecognised reply tag.
- `tests/test_mapper.py` — every conversion's exact arithmetic; sentinel
  values (`-1`/`0` = autosize/disabled) correctly routed to `dropped`
  instead of converted; the entire 6-stage Pb charge-profile block dropped
  with a named reason except the one field that does convert; and —
  repeatedly, from several angles — that the password never appears in the
  proposed document or the report.
- `tests/test_report.py` — rendered-text shape and the same password-leak
  check re-verified at the text layer.
- `tests/test_cli.py` — end-to-end file writing, default and explicit
  output paths, missing-input error handling.

## Layout

```
tools/stock_config_import/
  pyproject.toml
  README.md                   this file
  stock_config_import/
    __init__.py
    parser.py                  $RAS:/$RCP:n text -> StockCapture (field-position tables)
    mapper.py                   StockCapture -> (proposed JSON, ImportReport)
    report.py                   ImportReport -> human-readable text
    cli.py                       argparse wiring, file I/O
    __main__.py                  `python -m stock_config_import`
  tests/
    __init__.py
    fixtures/
      README.md                 SYNTHETIC — read this first
      synthetic_48v_battleborn.txt
      synthetic_minimal_sparse.txt
    test_parser.py
    test_mapper.py
    test_report.py
    test_cli.py
```
