"""
test_capture.py -- exercises the read-only guarantee, password redaction,
terminator handling, and the fixed command sequence of
stage_a_capture.capture. No real serial port anywhere: tests/mock_serial.py's
MockSerial stands in.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import time

import pytest

from stage_a_capture import capture as cap
from tests.mock_serial import MockSerial


# ---------------------------------------------------------------------------
# Read-only whitelist
# ---------------------------------------------------------------------------


def test_allowed_commands_pass():
    for cmd in cap.build_command_sequence():
        cap.assert_allowed(cmd)  # must not raise


@pytest.mark.parametrize(
    "cmd",
    [
        "$CPA:7 14.5,200,40,0@",  # write: accept-phase profile edit
        "$SCN:1,MainsAlt,5555@",  # write: name/password
        "$SCO:3,3,1,0,0@",  # write: overrides
        "$CCN:1,1,70,1,1@",  # write: CAN config
        "$MSR: 1234@",  # factory reset
        "$FRM:B@",  # force regulator mode -- touches the field
        "$RBT:@",  # reboot-to-apply
        "$EDB:@",  # enable debug
        "$RCP:9@",  # out-of-range profile index
        "$RAS:",  # missing the trailing '@' this module always sends
        "RAS:@",  # missing the leading '$'
        "",
    ],
)
def test_disallowed_commands_are_refused(cmd):
    with pytest.raises(cap.NotAllowedCommandError):
        cap.assert_allowed(cmd)


def test_build_command_sequence_is_fixed_order():
    seq = cap.build_command_sequence()
    assert seq == ["$RAS:@"] + [f"$RCP:{n}@" for n in range(1, 9)] + ["$RCP:0@"]
    # Two calls must not share mutable state.
    seq.append("$MUTATED@")
    assert "$MUTATED@" not in cap.build_command_sequence()


def test_send_command_refuses_before_writing_any_bytes():
    conn = MockSerial()
    with pytest.raises(cap.NotAllowedCommandError):
        cap.send_command(conn, "$MSR: 1234@")
    assert conn.writes == []  # nothing reached the port


def test_send_command_writes_allowed_command():
    conn = MockSerial(script={b"$RAS:@": b"AOK;\n"})
    cap.send_command(conn, "$RAS:@")
    assert conn.writes == [b"$RAS:@"]


# ---------------------------------------------------------------------------
# Redaction
# ---------------------------------------------------------------------------


def test_redact_line_blanks_visible_password():
    line = "NPC;,1,MainsAlt,hunter2,,ABCD1234,2024-01-01"
    redacted = cap.redact_line(line)
    assert "hunter2" not in redacted
    assert cap.REDACTED_MARKER in redacted
    # Every other field survives untouched.
    assert redacted.split(",")[2] == "MainsAlt"
    assert redacted.split(",")[5] == "ABCD1234"


def test_redact_line_blanks_already_hidden_password_too():
    # Firmware shows **** for a leading-dot password; we still normalize it
    # rather than trust the device's own redaction policy.
    line = "NPC;,1,MainsAlt,****,,ABCD1234,2024-01-01"
    redacted = cap.redact_line(line)
    assert "****" not in redacted
    assert cap.REDACTED_MARKER in redacted


def test_redact_line_leaves_non_npc_lines_alone():
    line = "AST;,1.23,,54.400,25.3,25.3,1377.0"
    assert cap.redact_line(line) == line


def test_redact_line_leaves_short_or_malformed_lines_alone():
    assert cap.redact_line("NPC;") == "NPC;"
    assert cap.redact_line("") == ""


# ---------------------------------------------------------------------------
# Terminator handling
# ---------------------------------------------------------------------------


def test_read_until_terminator_stops_at_aok():
    conn = MockSerial(preamble=b"SST;,AREG2.6.1\nSCV;,0,0\nAOK;\nSCV;,ignored\n")
    lines = cap.read_until_terminator(conn, timeout_s=2.0)
    assert lines == ["SST;,AREG2.6.1", "SCV;,0,0", "AOK;"]


def test_read_until_terminator_stops_at_nak():
    conn = MockSerial(preamble=b"NAK;\n")
    lines = cap.read_until_terminator(conn, timeout_s=2.0)
    assert lines == ["NAK;"]


def test_read_until_terminator_times_out():
    conn = MockSerial(preamble=b"SST;,AREG2.6.1\n")  # no AOK;/NAK; ever arrives
    with pytest.raises(cap.TerminatorTimeoutError):
        cap.read_until_terminator(conn, timeout_s=0.05)


def test_drain_preamble_never_raises_on_empty_port():
    conn = MockSerial()
    t0 = time.monotonic()
    lines = cap.drain_preamble(conn, seconds=0.05)
    assert lines == []
    assert time.monotonic() - t0 >= 0.04  # actually waited out the window


# ---------------------------------------------------------------------------
# Full procedure, scripted end to end
# ---------------------------------------------------------------------------


def _cpe_reply(n: int) -> bytes:
    return f"CPE;,{n},14.1,360,0,0,,0,0,0,0,,13.4,-1,0,-10,0,12.8,50,0.0\nAOK;\n".encode()


def test_capture_sends_exact_sequence_and_writes_two_files(tmp_path):
    script = {b"$RAS:@": b"SST;,AREG2.6.1\nNPC;,1,MainsAlt,hunter2,,ABCD1234,2024-01-01\nAOK;\n"}
    for n in range(1, 9):
        script[f"$RCP:{n}@".encode()] = _cpe_reply(n)
    script[b"$RCP:0@"] = _cpe_reply(1)

    conn = MockSerial(script=script, preamble=b"AST;,0.01,,54.0\n")
    result = cap.capture(
        conn,
        tmp_path,
        cmd_timeout_s=2.0,
        preamble_s=0.05,
        passive_s=0.05,
        stamp="TESTSTAMP",
    )

    assert result.commands_sent == cap.build_command_sequence()
    assert conn.writes == [cmd.encode("ascii") for cmd in cap.build_command_sequence()]

    assert result.raw_path.name == "ws500_stage_a_TESTSTAMP_raw.log"
    assert result.redacted_path.name == "ws500_stage_a_TESTSTAMP_redacted.log"

    raw_text = result.raw_path.read_text()
    redacted_text = result.redacted_path.read_text()

    # The raw copy is the operator's own unredacted archive.
    assert "hunter2" in raw_text
    # The redacted copy never carries the plaintext password.
    assert "hunter2" not in redacted_text
    assert cap.REDACTED_MARKER in redacted_text
    # Everything else is identical between the two copies.
    assert "SST;,AREG2.6.1" in raw_text and "SST;,AREG2.6.1" in redacted_text
    assert "AST;,0.01,,54.0" in raw_text and "AST;,0.01,,54.0" in redacted_text


def test_capture_never_sends_a_write_command_even_if_script_offers_one(tmp_path):
    # Defense in depth: even if a MockSerial were primed to answer a write
    # command helpfully, capture() has no code path that would ever send one.
    script = {b"$RAS:@": b"AOK;\n"}
    for n in range(1, 9):
        script[f"$RCP:{n}@".encode()] = _cpe_reply(n)
    script[b"$RCP:0@"] = _cpe_reply(1)
    script[b"$MSR: 1234@"] = b"AOK;\n"  # a write command the mock WOULD answer

    conn = MockSerial(script=script)
    cap.capture(conn, tmp_path, cmd_timeout_s=2.0, preamble_s=0.0, passive_s=0.0)

    assert b"$MSR: 1234@" not in conn.writes
    for w in conn.writes:
        cap.assert_allowed(w.decode("ascii"))  # every single write passes the whitelist
