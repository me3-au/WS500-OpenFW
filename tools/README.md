# tools/

Python tooling for WS500-OpenFW. Each subdirectory is an independent,
individually-installable package with its own `pyproject.toml`, README, and
`pytest` suite — see each one for full detail; this file is the index.

| Tool | What it does | Talks to |
|---|---|---|
| [`stage_a_capture/`](stage_a_capture/) | Automates `docs/STAGE_A_RUNSHEET.md` Procedure 1: read-only USB `$`-protocol capture of the stock WS500's full status/config dump | the live WS500, over USB CDC (stock firmware, `$RAS:`/`$RCP:n` only) |
| [`stock_config_import/`](stock_config_import/) | Translates an archived stock capture (the file `stage_a_capture` produces) into a proposed native PROFILE_SPEC §7 JSON config, with a mapped/converted/dropped/unrecognised report | nothing — reads a file, per Decision #6a |
| [`ws500ctl/`](ws500ctl/) | CLI client for **OpenFW's own** native JSON-lines config/telemetry protocol (`get`/`set`/`telem`/`info`/`ping`) | a WS500 running **WS500-OpenFW**, not stock firmware |

## Why two tools for "Stage A", not one

`stage_a_capture` and `stock_config_import` are deliberately separate
packages with a file in between, not one tool that captures-then-imports
in a single run:

- **Different safety class.** `stage_a_capture` is the one piece of
  tooling in this repo that opens a serial port to the live, installed,
  in-service unit (`docs/SAFETY.md` / `PROJECT_PLAN.md` §5's staged access
  ladder). `stock_config_import` never does — it reads a file, full stop,
  and can be run, tested, and iterated on all day with zero risk to the
  unit. Keeping them separate means the translator's logic (which is where
  most of the interesting, evolving mapping decisions live) can be
  developed and tested without ever touching hardware, or even having
  hardware available.
- **The archive is the point.** `docs/STAGE_A_RUNSHEET.md` Procedure 1
  frames the raw capture itself as *"the Stage-A artifact"* — the
  permanent record of what the stock unit's configuration actually was,
  independent of whatever any importer does with it later. A combined
  tool would make the intermediate file feel disposable; it isn't.
- **Decision #6a says so.** `docs/DECISION_6A_CONFIG_STRATEGY.md`: *"the
  client app translates stock dumps"* — a translator that reads an
  archived *file*, as opposed to something that speaks live to a device,
  is exactly what was decided, and `stock_config_import`'s tests hold it
  to that (no serial import anywhere in that package, on purpose).

## Full absolute-path example, start to finish

```sh
# 1. Capture (talks to the live unit -- read STAGE_A_RUNSHEET.md Procedure 1 first)
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stage_a_capture
pip install -e ".[test]"
python -m stage_a_capture.capture --port COM5 --outdir C:\Users\adren\stage-a-logs

# 2. Translate the archive (no hardware involved from here on)
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stock_config_import
pip install -e ".[test]"
python -m stock_config_import C:\Users\adren\stage-a-logs\ws500_stage_a_<timestamp>_redacted.log

# 3. Review C:\Users\adren\stage-a-logs\ws500_stage_a_<timestamp>_redacted.log.report.txt
#    by hand, merge whatever you accept into a real config document, then:
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\ws500ctl
pip install -e ".[test]"
ws500ctl --port <OpenFW-unit-port> set -i merged.json --dry-run
```

Step 3 only applies once a WS500 is actually running WS500-OpenFW
(Stage C, gated on M1 + the §8 virtual gauntlet, per `PROJECT_PLAN.md` §5)
— today, step 2's output is reviewed on paper, not sent anywhere.

## Testing all three

Each package's own README has the exact command; in short, from each
`tools/<name>/` directory: `pip install -e ".[test]" && pytest -v`. There
is no shared top-level test runner — each package is independently
installable and tested by design (see "Why two tools" above; `ws500ctl`
predates both and follows the same pattern).
