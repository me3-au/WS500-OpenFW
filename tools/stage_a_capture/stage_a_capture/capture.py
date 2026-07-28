"""
capture.py -- automates docs/STAGE_A_RUNSHEET.md Procedure 1 ("USB `$` protocol
readout") against the live, installed WS500. This is the one Stage-A procedure
that talks to the unit over the wire, so it carries two hard safety/privacy
properties that are structural, not policy:

  1. READ-ONLY BY CONSTRUCTION. `ALLOWED_COMMAND_RE` is the only gate a command
     ever passes through before being written to the port (`send_command()`).
     Every command this script ever sends is drawn from `build_command_sequence()`,
     a hardcoded, fixed-order list -- there is no code path that accepts an
     operator-typed or file-sourced command string. Even so, `send_command()`
     re-checks every string against the whitelist before writing bytes, so a
     future edit that accidentally introduces a second call site (or a typo
     that turns `$RCP:1@` into something else) fails loudly instead of writing
     to the port. `$RAS:`/`$RCP:n` are documented read-only queries
     (Wakespeed-Communications-and-Configuration-Guide-v2.6.1-1.pdf, "Receiving
     data FROM the regulator" / "$RAS:"/"$RCP:n" on p.57-58); nothing in
     "Sending data TO the regulator" (`$CPx`, `$SCx`, `$CCx`, `$CDD`, `$MSR`,
     `$EDB`, `$RBT`, `$FRM`) is reachable from this module at all -- those verbs
     don't appear anywhere in this file's source, not even in a comment showing
     how one *could* be sent.
  2. PASSWORD REDACTION. The `NPC;` reply line carries the regulator's
     configured password in plain text unless it was set hidden (leading `.`,
     Comms & Config Guide p.38). `redact_line()` blanks that field
     unconditionally on every line written to the *redacted* copy; the raw
     copy is left untouched as the operator's own private archive. Two files,
     always, printed with their paths and which is which -- see `capture()`.

Everything else (terminator waiting, timeouts, Ctrl-C handling) exists because
this runs against a live, in-service 48 V charging system
(docs/SAFETY.md / PROJECT_PLAN.md Sec5) -- see STAGE_A_RUNSHEET.md Sec0's
"STOP immediately if..." list, which this script's timeout/no-retry behavior
deliberately mirrors rather than "helpfully" retrying against a non-responsive
unit.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import re
import sys
import time
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional, Protocol, runtime_checkable

# ---------------------------------------------------------------------------
# Wire constants
# ---------------------------------------------------------------------------

# Baud is accepted but not meaningful over the WS500's USB CDC-ACM port (the
# same fact ws500ctl/proto.py and test-fw/README.md note for the native
# protocol's port); 115200 matches the Comms & Config Guide's own PuTTY
# example (p.22) so a raw terminal session and this script agree on paper.
DEFAULT_BAUD = 115200
DEFAULT_CMD_TIMEOUT_S = 5.0
DEFAULT_PASSIVE_SECONDS = 180.0
DEFAULT_PREAMBLE_SECONDS = 2.0

# The read-only query verbs, and ONLY these -- Comms & Config Guide p.57-58.
# Every command byte-string this module can ever transmit must fully match
# this pattern; send_command() enforces it unconditionally.
ALLOWED_COMMAND_RE = re.compile(r"^\$(RAS:@|RCP:[0-8]@)$")


class NotAllowedCommandError(RuntimeError):
    """Raised if anything ever asks to send a command outside the read-only
    whitelist. This must never be reachable from build_command_sequence()'s
    output; if it fires, something has been miswired and the safe response is
    to crash loudly, not to send the string anyway."""


def assert_allowed(cmd: str) -> None:
    """Raise NotAllowedCommandError unless `cmd` is exactly one of the
    documented read-only queries. This is the single choke point every
    outgoing command passes through (send_command() calls it before any
    bytes reach the port)."""
    if not ALLOWED_COMMAND_RE.match(cmd):
        raise NotAllowedCommandError(
            f"refusing to send {cmd!r} -- not one of the read-only queries "
            f"this tool is allowed to send (STAGE_A_RUNSHEET.md Procedure 1: "
            f"$RAS:, $RCP:0..8 only)"
        )


def build_command_sequence() -> list[str]:
    """The fixed, in-order command list Procedure 1 sends: $RAS: first, then
    $RCP:1@ through $RCP:8@ (the full stored Charge Profile Entry table),
    then $RCP:0@ (the currently-selected entry, redundant but cheap per the
    runsheet). This is the ONLY place command strings are constructed --
    capture() just iterates this list. Returning a fresh list each call
    avoids a shared-mutable-default footgun; callers are free to mutate their
    copy without affecting the next call."""
    seq = ["$RAS:@"]
    seq.extend(f"$RCP:{n}@" for n in range(1, 9))
    seq.append("$RCP:0@")
    for cmd in seq:
        assert_allowed(cmd)  # belt-and-suspenders: this list IS the whitelist's contents
    return seq


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------


@runtime_checkable
class SerialLike(Protocol):
    """The narrow slice of pyserial's Serial this module needs -- same
    structural-typing trick as ws500ctl/proto.py's SerialLike, so tests drive
    this module against an in-memory mock with no real serial port anywhere
    near it."""

    def write(self, data: bytes) -> int: ...

    def read(self, size: int = 1) -> bytes: ...

    def close(self) -> None: ...


def open_port(port: str, baud: int = DEFAULT_BAUD, timeout: float = 0.2) -> SerialLike:
    """Open a real serial port via pyserial. Imported lazily so nothing in
    this module needs pyserial installed to run the mock-backed test suite
    (mirrors ws500ctl.proto.WS500Client.open's same lazy-import trade)."""
    try:
        import serial  # type: ignore[import-not-found]
    except ImportError as e:
        raise RuntimeError(
            "pyserial is not installed (`pip install pyserial`) -- see "
            "tools/stage_a_capture/README.md"
        ) from e
    return serial.Serial(port=port, baudrate=baud, timeout=timeout)


class TerminatorTimeoutError(TimeoutError):
    """No AOK;/NAK; terminator arrived within the command's timeout budget.
    Procedure 1's own pass/fail rule (STAGE_A_RUNSHEET.md) says not to retry
    aggressively against a live unit -- this exception is meant to propagate
    all the way to main() and stop the session, not be swallowed and retried."""


def _read_line(conn: SerialLike, deadline: float) -> Optional[str]:
    """Read one newline-terminated line, or None once `deadline`
    (time.monotonic()) passes first. Mirrors ws500ctl.proto's own permissive
    read loop: a byte that fails to decode as UTF-8 is replaced, not raised,
    because a stray byte of line noise on a live USB CDC port must not crash
    a bench session."""
    buf = bytearray()
    while True:
        remaining = deadline - time.monotonic()
        if remaining <= 0:
            return None
        chunk = conn.read(1)
        if not chunk:
            continue  # pyserial's per-call read timeout, not our real deadline
        if chunk == b"\n":
            return buf.decode("utf-8", errors="replace")
        if chunk != b"\r":
            buf.extend(chunk)


def read_until_terminator(conn: SerialLike, timeout_s: float) -> list[str]:
    """Read lines from `conn` until one starts with `AOK;` or `NAK;`
    (inclusive), or `timeout_s` elapses with no terminator seen. Blank lines
    are kept (they're part of the raw archive) but don't reset the deadline.
    Raises TerminatorTimeoutError on timeout -- see that class's docstring
    for why this does not retry internally."""
    deadline = time.monotonic() + timeout_s
    lines: list[str] = []
    while True:
        line = _read_line(conn, deadline)
        if line is None:
            raise TerminatorTimeoutError(
                f"no AOK;/NAK; terminator within {timeout_s}s "
                f"({len(lines)} line(s) captured before timing out)"
            )
        lines.append(line)
        stripped = line.strip()
        if stripped.startswith("AOK;") or stripped.startswith("NAK;"):
            return lines


def drain_preamble(conn: SerialLike, seconds: float) -> list[str]:
    """Capture whatever the port pushes unsolicited for `seconds` before the
    first command is sent -- Procedure 1 step 3: "a passive capture alone is
    a useful cross-check even before you send anything." Never raises on
    timeout; an empty result (nothing arrived) is a normal outcome here."""
    deadline = time.monotonic() + seconds
    lines: list[str] = []
    while time.monotonic() < deadline:
        line = _read_line(conn, deadline)
        if line is None:
            return lines
        lines.append(line)
    return lines


def capture_passive_stream(conn: SerialLike, seconds: float) -> list[str]:
    """Let the unsolicited AST;/DST; status stream run for `seconds` after
    the queries finish (Procedure 1 step 6). Same non-raising timeout
    behavior as drain_preamble -- this phase is expected to end in a plain
    "time's up", not an error."""
    return drain_preamble(conn, seconds)


# ---------------------------------------------------------------------------
# Redaction
# ---------------------------------------------------------------------------

REDACTED_MARKER = "[REDACTED-BY-stage_a_capture]"


def redact_line(line: str) -> str:
    """Blank the Password field of an `NPC;` reply line
    (Comms & Config Guide p.38: "NPC;, Enable BLE?, Name, Password, ,
    DeviceID, DOM"), unconditionally -- even if the device already shows
    "****" for a hidden (leading-dot) password, this still normalizes it,
    so the redacted copy never depends on trusting the *device's* redaction
    policy. Every other line passes through unchanged. Applied to every line
    written to the redacted copy, not just ones the caller thinks might be
    NPC; -- see capture()'s "why not more targeted" note."""
    if not line.strip().startswith("NPC;"):
        return line
    parts = line.split(",")
    # Comms & Config Guide field order: NPC;(0) EnableBLE(1) Name(2) Password(3) ...
    if len(parts) > 3:
        parts[3] = f" {REDACTED_MARKER}"
    return ",".join(parts)


# ---------------------------------------------------------------------------
# Session
# ---------------------------------------------------------------------------


@dataclass
class CaptureResult:
    raw_path: Path
    redacted_path: Path
    lines_captured: int
    commands_sent: list[str] = field(default_factory=list)


def _timestamp() -> str:
    return time.strftime("%Y%m%d_%H%M%S")


def capture(
    conn: SerialLike,
    outdir: Path,
    *,
    cmd_timeout_s: float = DEFAULT_CMD_TIMEOUT_S,
    preamble_s: float = DEFAULT_PREAMBLE_SECONDS,
    passive_s: float = DEFAULT_PASSIVE_SECONDS,
    stamp: Optional[str] = None,
) -> CaptureResult:
    """Run Procedure 1 end to end against an already-open `conn` and write
    the two archive files. Steps mirror STAGE_A_RUNSHEET.md Procedure 1
    1-7 exactly:

      3. preamble  -- capture whatever's already flowing (drain_preamble)
      4. $RAS:     -- send, wait for AOK;/NAK;
      5. $RCP:1..8 -- send each in turn, wait for AOK;/NAK; before the next
         $RCP:0    -- redundant-but-cheap per the runsheet
      6. passive   -- let AST; run for `passive_s`
      7. close     -- caller's responsibility (this function does not close
                      `conn`; capture_procedure1() below does, via `with`)

    Every line, from every phase, is appended to BOTH the raw and the
    redacted file as it arrives (not buffered to the end) -- if the port
    dies mid-session, whatever was captured is already on disk in both
    forms, not lost to an in-memory buffer.
    """
    outdir.mkdir(parents=True, exist_ok=True)
    stamp = stamp or _timestamp()
    raw_path = outdir / f"ws500_stage_a_{stamp}_raw.log"
    redacted_path = outdir / f"ws500_stage_a_{stamp}_redacted.log"

    lines_captured = 0
    commands_sent: list[str] = []

    with raw_path.open("w", encoding="utf-8", newline="\n") as raw_f, redacted_path.open(
        "w", encoding="utf-8", newline="\n"
    ) as red_f:

        def emit(batch: list[str]) -> None:
            nonlocal lines_captured
            for line in batch:
                raw_f.write(line + "\n")
                red_f.write(redact_line(line) + "\n")
                lines_captured += 1
            raw_f.flush()
            red_f.flush()

        emit(drain_preamble(conn, preamble_s))

        for cmd in build_command_sequence():
            send_command(conn, cmd)
            commands_sent.append(cmd)
            emit(read_until_terminator(conn, cmd_timeout_s))

        emit(capture_passive_stream(conn, passive_s))

    return CaptureResult(
        raw_path=raw_path,
        redacted_path=redacted_path,
        lines_captured=lines_captured,
        commands_sent=commands_sent,
    )


def send_command(conn: SerialLike, cmd: str) -> None:
    """Write one command's bytes to `conn`. Calls assert_allowed() first,
    unconditionally -- this is the one function in the whole module that
    ever calls conn.write(), and it never does so without the whitelist
    check running first, regardless of what called it or why."""
    assert_allowed(cmd)
    conn.write(cmd.encode("ascii"))


def capture_procedure1(
    port: str,
    outdir: Path,
    *,
    baud: int = DEFAULT_BAUD,
    cmd_timeout_s: float = DEFAULT_CMD_TIMEOUT_S,
    preamble_s: float = DEFAULT_PREAMBLE_SECONDS,
    passive_s: float = DEFAULT_PASSIVE_SECONDS,
) -> CaptureResult:
    """Open `port`, run capture(), and guarantee the port is closed
    afterward -- including on Ctrl-C, which is the documented clean-abort
    path (STAGE_A_RUNSHEET.md Sec0: nothing here writes to the unit, so an
    interrupted session just means an incomplete log, not a half-sent
    command)."""
    conn = open_port(port, baud=baud)
    try:
        return capture(
            conn,
            outdir,
            cmd_timeout_s=cmd_timeout_s,
            preamble_s=preamble_s,
            passive_s=passive_s,
        )
    finally:
        conn.close()


# ---------------------------------------------------------------------------
# CLI
# ---------------------------------------------------------------------------


def _build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="stage_a_capture",
        description=(
            "Automates docs/STAGE_A_RUNSHEET.md Procedure 1: read-only USB `$` "
            "protocol capture of the WS500's full status/config dump. Sends only "
            "$RAS: and $RCP:0..8 -- see capture.py's module docstring for the "
            "read-only guarantee. --port is required and NOT auto-discovered: "
            "the stock firmware's app-mode USB descriptor isn't documented "
            "anywhere in this repo yet (STAGE_A_RUNSHEET.md Sec5 'Candidate "
            "additions') -- find the COM port in Device Manager first."
        ),
    )
    p.add_argument("--port", required=True, help="e.g. COM5 (Windows) or /dev/ttyUSB0")
    p.add_argument("--baud", type=int, default=DEFAULT_BAUD)
    p.add_argument(
        "--cmd-timeout",
        type=float,
        default=DEFAULT_CMD_TIMEOUT_S,
        help="seconds to wait for AOK;/NAK; after each command (default: %(default)s)",
    )
    p.add_argument(
        "--preamble-seconds",
        type=float,
        default=DEFAULT_PREAMBLE_SECONDS,
        help="seconds to passively listen before sending the first command (default: %(default)s)",
    )
    p.add_argument(
        "--passive-seconds",
        type=float,
        default=DEFAULT_PASSIVE_SECONDS,
        help="seconds to let the AST; stream run after the queries finish (default: %(default)s)",
    )
    p.add_argument(
        "--outdir",
        type=Path,
        default=Path("."),
        help="directory to write the two log files into (default: current directory)",
    )
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = _build_argparser().parse_args(argv)
    print(f"stage_a_capture: opening {args.port} @ {args.baud} baud ...", file=sys.stderr)
    try:
        result = capture_procedure1(
            args.port,
            args.outdir,
            baud=args.baud,
            cmd_timeout_s=args.cmd_timeout,
            preamble_s=args.preamble_seconds,
            passive_s=args.passive_seconds,
        )
    except KeyboardInterrupt:
        # Clean abort (STAGE_A_RUNSHEET.md Sec0): the port is already closed
        # by capture_procedure1's finally-block by the time we get here.
        print("\nstage_a_capture: interrupted -- port closed, partial log kept.", file=sys.stderr)
        return 130
    except (NotAllowedCommandError, TerminatorTimeoutError, RuntimeError) as e:
        print(f"stage_a_capture: {e}", file=sys.stderr)
        return 1

    print(f"stage_a_capture: {result.lines_captured} line(s) captured.")
    print(f"  RAW (private archive, may contain the plaintext password):")
    print(f"    {result.raw_path.resolve()}")
    print(f"  REDACTED (safe to attach to an issue / commit if you must):")
    print(f"    {result.redacted_path.resolve()}")
    print(
        "  Do not commit the RAW copy -- *.log is already gitignored, but this "
        "is belt-and-suspenders, not the mechanism: see docs/STAGE_A_RUNSHEET.md "
        "Procedure 1's privacy/security note."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
