# stage_a_capture

Automates `docs/STAGE_A_RUNSHEET.md` **Procedure 1** ("USB `$` protocol
readout (full config + status dump)") against the live, installed WS500 —
the last thing on the V1 critical path that is currently entirely manual.
Every step this script performs is **observation only**: it opens the USB
CDC port, logs everything received from connection onward, sends the three
documented read-only queries in sequence, waits for each reply's terminator,
then lets the passive `AST;` stream run for a configurable window.

**bench-pending**: this tool has never been run against the physical WS500.
Do not point it at real hardware without reading `docs/SAFETY.md` and
`docs/STAGE_A_RUNSHEET.md` Procedure 1 first — this script automates the
mechanics of that procedure, it does not replace reading it.

## Why Python + pyserial, not the PowerShell `SerialPort` method

`docs/STAGE_A_RUNSHEET.md` documents the already-proven PowerShell
`System.IO.Ports.SerialPort` method as the reference procedure, and that
remains the manual fallback (see "Relationship to the manual procedure"
below). This tool uses Python + `pyserial` instead, for one reason:
`tools/ws500ctl` already commits the project to a Python + pyserial stack
for the native-protocol CLI (`docs/CLIENT_CONNECTIVITY.md` decision #8:
"native `ws500ctl` CLI (Python + pyserial + a DFU lib)"). Building this
tool the same way means one Python environment covers both the Stage-A
capture and the eventual native-firmware tooling, and it gets a proper
pytest suite (`tests/`) with no serial port anywhere near it — see
"Testing" below.

**Honest dependency story**: this needs `pyserial` installed
(`pip install -e ".[test]"` from this directory pulls it in). It is not
stdlib. If you'd rather not install anything, `docs/STAGE_A_RUNSHEET.md`
Procedure 1's own PowerShell steps are the documented, dependency-free
alternative — see "Relationship to the manual procedure" below.

## Install

```sh
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stage_a_capture
pip install -e ".[test]"
```

Requires Python 3.10+.

## Usage

1. Connect the WS500's USB port. Find its COM port in Windows Device
   Manager — the app-mode USB descriptor isn't documented anywhere in this
   repo yet (`STAGE_A_RUNSHEET.md` §5 "Candidate additions"), so `--port`
   is required and there is **no** `--port auto` (unlike `ws500ctl`, which
   knows its own firmware's VID:PID).
2. Run:

```sh
python -m stage_a_capture.capture --port COM5 --outdir C:\Users\adren\stage-a-logs
```

or, if installed as a console script:

```sh
stage-a-capture --port COM5 --outdir C:\Users\adren\stage-a-logs
```

3. It prints the two output file paths when done (or on Ctrl-C, whatever
   was captured up to that point is already on disk in both forms —
   see "Read-only guarantee" below).

| Flag | Default | Meaning |
|---|---|---|
| `--port` | *(required)* | e.g. `COM5` |
| `--baud` | `115200` | accepted but not meaningful over USB CDC (same fact `ws500ctl` and `test-fw/README.md` note for the native protocol's port) |
| `--cmd-timeout` | `5.0` s | how long to wait for `AOK;`/`NAK;` after each command before giving up |
| `--preamble-seconds` | `2.0` s | passive listen window before the first command, to catch whatever's already flowing |
| `--passive-seconds` | `180.0` s (a few minutes) | how long to let the unsolicited `AST;` stream run after the queries finish |
| `--outdir` | `.` | where to write the two log files |

**Unexpected result → stop, do not retry.** If a command times out
(`--cmd-timeout` elapses with no `AOK;`/`NAK;`), the tool exits with an
error rather than resending — `STAGE_A_RUNSHEET.md` Procedure 1's own
pass/fail rule says not to retry aggressively against a live unit;
disconnect, reconnect, and try once more by hand instead.

## Read-only guarantee

This tool can only ever send `$RAS:@` and `$RCP:0@`..`$RCP:8@` — the
documented read-only queries (Comms & Config Guide p.57-58). This is
enforced structurally, not by convention:

- `build_command_sequence()` is the **only** place command strings are
  constructed, and it returns a fixed, hardcoded, in-order list. There is
  no argparse flag, no config file, no interactive prompt anywhere in this
  tool that accepts an operator-typed or file-sourced command string.
- `send_command()` — the **only** function in the module that calls
  `conn.write()` — re-checks every string against
  `ALLOWED_COMMAND_RE` (`^\$(RAS:@|RCP:[0-8]@)$`) before writing a single
  byte, unconditionally. A future edit that introduced a second call site,
  or a typo that mangled a command string, fails loudly
  (`NotAllowedCommandError`) instead of writing to the port.
- None of the write-command verbs (`$CPx`, `$SCx`, `$CCx`, `$CDD`, `$MSR`,
  `$EDB`, `$RBT`, `$FRM`) appear anywhere in `capture.py`'s source — not
  even in a comment showing how one *could* be sent. `tests/test_capture.py`
  exercises the whitelist against a list of real write commands drawn
  from the Comms & Config Guide and asserts every one is refused.

## Password redaction

The `NPC;` reply line carries the regulator's configured password in
plain text unless it was set hidden (Comms & Config Guide p.38: a leading
`.` on the password). This tool writes **two** files every run:

- `ws500_stage_a_<timestamp>_raw.log` — the complete, unredacted capture.
  This is **your own private archive**; it is not safe to attach to a
  public issue or commit. `*.log` is already gitignored in this repo, but
  treat that as belt-and-suspenders, not the mechanism.
- `ws500_stage_a_<timestamp>_redacted.log` — byte-for-byte identical
  except every `NPC;` line's Password field is replaced with
  `[REDACTED-BY-stage_a_capture]`, unconditionally — even if the device
  already showed `****` for a hidden password, this still normalizes it
  rather than trusting the device's own redaction policy. **This is the
  copy that's safe to attach to an issue.**

Both paths are printed at the end of the run, clearly labeled which is
which.

## Relationship to the manual procedure

`docs/STAGE_A_RUNSHEET.md` Procedure 1's own step-by-step PowerShell
instructions remain the documented fallback — this script is a
convenience, not a new single point of failure at the bench. If this tool
misbehaves, fall back to the manual PowerShell `SerialPort` steps; they
don't depend on Python or this package at all.

## Testing

```sh
cd C:\Users\adren\AppDev\Wakespeed\WS500-OpenFW\tools\stage_a_capture
pip install -e ".[test]"
pytest -v
```

No hardware, no real serial port: `tests/mock_serial.py`'s `MockSerial`
implements `stage_a_capture.capture.SerialLike` (`write`/`read`/`close`)
and answers scripted commands with canned reply bytes. Tests cover: every
documented write command is refused by the whitelist; the fixed command
sequence never varies; password redaction on both a visible and an
already-hidden password, and on non-`NPC;` lines (no-op); terminator
detection on `AOK;` and `NAK;`; timeout behavior when no terminator
arrives; and a full scripted end-to-end run asserting the exact bytes
written to the port and the contents of both output files.

## Layout

```
tools/stage_a_capture/
  pyproject.toml
  README.md               this file
  stage_a_capture/
    __init__.py
    capture.py             whitelist, redaction, terminator handling, session, CLI
  tests/
    __init__.py
    mock_serial.py          MockSerial, the in-memory fake port
    test_capture.py
```
