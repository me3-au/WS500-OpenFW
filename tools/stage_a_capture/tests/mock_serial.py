"""
mock_serial.py -- in-memory stand-in for a real serial port, implementing
stage_a_capture.capture.SerialLike (write/read/close). Deterministic and
scripted: a command's reply bytes are queued the instant write() sees the
matching command bytes, so tests don't depend on real timing beyond the
short, real deadlines the timeout tests deliberately exercise.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations


class MockSerial:
    def __init__(self, script: dict[bytes, bytes] | None = None, preamble: bytes = b""):
        """`script` maps an exact outgoing command's bytes to the reply bytes
        the fake device "sends back" the instant that command is written.
        `preamble` is queued immediately, before any command is sent --
        simulating whatever is already flowing on the wire at connect time."""
        self.script = script or {}
        self._buf = bytearray(preamble)
        self.writes: list[bytes] = []
        self.closed = False

    def write(self, data: bytes) -> int:
        self.writes.append(bytes(data))
        reply = self.script.get(bytes(data))
        if reply is not None:
            self._buf.extend(reply)
        return len(data)

    def read(self, size: int = 1) -> bytes:
        if not self._buf:
            return b""
        chunk = bytes(self._buf[:size])
        del self._buf[:size]
        return chunk

    def close(self) -> None:
        self.closed = True
