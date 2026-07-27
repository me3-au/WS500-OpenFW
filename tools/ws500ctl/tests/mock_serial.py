"""
mock_serial.py -- an in-memory stand-in for pyserial's Serial, backed by a
small simulation of the FIRMWARE side of the wire grammar
(control/Inc/config_msg.h, config_doc.h, config_validate.h).

ws500ctl.proto.WS500Client only needs .write(bytes) / .read(size) / .close()
(proto.SerialLike), so this class can hand the transport layer scripted
firmware behaviour without any real serial port, USB stack, or hardware --
matching how config_msg.h's own host is injected so the firmware side is
native-tested with no USB stack near it either.

This is a SIMULATION, not a re-implementation: it does not run the real
validator (ctrl_config_validate() has ~40 named rules) or the real bounded
JSON reader. It reproduces enough of the observable wire behaviour --
hello/caps, cfg-get/cfg-set/telem-get shapes, the named error codes a client
must be able to handle, line framing, and the timeout case -- to exercise
ws500ctl's transport and command logic. It is not a substitute for
control/test/test_protocol.c, which is the actual grammar oracle.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import json
from typing import Optional

from .fixtures import default_cfg_doc, default_telem_line

LINE_MAX = 3072  # mirrors config_msg.h CFG_MSG_LINE_MAX, see proto.py's own note

_KNOWN_TOP_KEYS = {
    "schema_version",
    "fw",
    "primitives",
    "limits",
    "global",
    "profiles",
    "active_profile",
}


class MockSerial:
    """Firmware-side simulator. Configure via the constructor for most tests;
    `inject_noise` / `push_raw_line` / `silent` cover the transport-edge cases
    (garbage bytes, oversize lines, a device that never answers)."""

    def __init__(
        self,
        *,
        fw: Optional[str] = "0.1.0-dev",
        git: Optional[str] = "deadbee",
        fw_proto: int = 1,
        fw_schema: int = 1,
        caps: Optional[list] = None,
        cfg_doc: Optional[dict] = None,
        telem_line: Optional[dict] = None,
        silent: bool = False,
        force_cfg_set_error: Optional[dict] = None,
    ) -> None:
        self.fw = fw
        self.git = git
        self.fw_proto = fw_proto
        self.fw_schema = fw_schema
        self.caps = list(caps) if caps is not None else ["cfg", "telem"]
        self.cfg_doc = cfg_doc if cfg_doc is not None else default_cfg_doc(fw_schema, fw or "")
        self.telem_line_obj = telem_line if telem_line is not None else default_telem_line()
        self.silent = silent  # simulate "no device answers" for timeout tests
        # Test hook: when set, every cfg-set gets this error verbatim instead
        # of being processed -- for exercising WS500FirmwareError paths that
        # a client-side check (e.g. set.py's schema pre-check) would
        # otherwise intercept before the "wire" is ever reached.
        self.force_cfg_set_error = force_cfg_set_error

        self._rx = bytearray()  # bytes written by the client, awaiting a newline
        self._tx = bytearray()  # bytes queued for the client's next read()

        self.write_count = 0  # number of write() calls -- lets tests count retries
        self.requests: list = []  # every parsed request object, in arrival order
        self.saves: list = []  # every cfg dict a cfg-set actually stored
        self.generation = 7  # non-zero, so an "ok" reply can't fake a save (test_protocol.c mirrors this)
        self.closed = False

    # ---- SerialLike surface ---------------------------------------------

    def write(self, data: bytes) -> int:
        self.write_count += 1
        if self.silent:
            return len(data)  # bytes accepted; the "device" never answers
        self._rx.extend(data)
        while b"\n" in self._rx:
            line, _, rest = bytes(self._rx).partition(b"\n")
            self._rx = bytearray(rest)
            self._handle_line(line)
        return len(data)

    def read(self, size: int = 1) -> bytes:
        if not self._tx:
            return b""  # pyserial's own per-call-timeout return value
        n = min(size, len(self._tx))
        out = bytes(self._tx[:n])
        del self._tx[:n]
        return out

    def close(self) -> None:
        self.closed = True

    # ---- test hooks -------------------------------------------------------

    def inject_noise(self, text: str) -> None:
        """Queue a line of non-protocol text (a boot banner, stray debug
        print, ...) ahead of whatever reply comes next. Must be a complete
        line -- real UART noise this client needs to tolerate is line-shaped
        too (config_msg.h answers per newline), just not JSON-with-a-"t"."""
        if not text.endswith("\n"):
            text += "\n"
        self._tx.extend(text.encode("utf-8", errors="replace"))

    def push_raw_line(self, text: str) -> None:
        """Queue an exact reply line, bypassing request handling entirely --
        used for shapes the firmware would only ever emit in response to
        something (e.g. testing the client's own oversize-line guard would
        require an already-broken client, so this simulates the REPLY
        instead)."""
        if not text.endswith("\n"):
            text += "\n"
        self._tx.extend(text.encode("utf-8", errors="replace"))

    # ---- simulated firmware -----------------------------------------------

    def _reply(self, obj: dict) -> None:
        self._tx.extend(json.dumps(obj, separators=(",", ":")).encode("utf-8"))
        self._tx.extend(b"\n")

    def _err(self, code: str, detail: Optional[str] = None, **extra) -> None:
        obj = {"t": "err", "code": code}
        if detail is not None:
            obj["detail"] = detail
        obj.update(extra)
        self._reply(obj)

    def _handle_line(self, raw: bytes) -> None:
        # config_msg.c: a line over CFG_MSG_LINE_MAX is dropped to the
        # newline and answered with CFG_ERR_JSON, same as any other
        # malformed-JSON line from this simulator's point of view.
        if len(raw) > LINE_MAX:
            self._err("CFG_ERR_JSON", "line longer than the protocol allows")
            return
        try:
            obj = json.loads(raw.decode("utf-8"))
        except Exception:
            self._err("CFG_ERR_JSON", "malformed JSON")
            return
        if not isinstance(obj, dict) or not isinstance(obj.get("t"), str):
            self._err("CFG_ERR_MSG", 'message has no string "t" member')
            return

        self.requests.append(obj)
        t = obj["t"]
        if t == "hello":
            self._do_hello()
        elif t == "cfg-get":
            self._do_cfg_get()
        elif t == "cfg-set":
            self._do_cfg_set(obj)
        elif t == "telem-get":
            self._do_telem_get()
        else:
            self._err("CFG_ERR_MSG", "unknown message type")

    def _do_hello(self) -> None:
        reply = {"t": "hello", "proto": self.fw_proto, "schema": self.fw_schema, "caps": list(self.caps)}
        if self.fw:
            reply["fw"] = self.fw
        if self.git:
            reply["git"] = self.git
        self._reply(reply)

    def _do_cfg_get(self) -> None:
        if "cfg" not in self.caps:
            self._err("CFG_ERR_MSG", "unknown message type")
            return
        self._reply({"t": "cfg", "cfg": self.cfg_doc})

    def _do_cfg_set(self, obj: dict) -> None:
        if "cfg" not in self.caps:
            self._err("CFG_ERR_MSG", "unknown message type")
            return
        cfg = obj.get("cfg")
        if not isinstance(cfg, dict):
            self._err("CFG_ERR_MSG", 'cfg-set carries no "cfg" object')
            return
        if self.force_cfg_set_error is not None:
            self._err(**self.force_cfg_set_error)
            return
        sv = cfg.get("schema_version")
        if sv is not None and sv != self.fw_schema:
            self._err(
                "CFG_ERR_SCHEMA",
                "config schema_version is not the one this firmware speaks",
                key="schema_version",
            )
            return
        # Simplified unknown-key count: top-level keys only (the real
        # firmware counts every unrecognised member at any depth --
        # config_msg.h's docstring; this simulator's callers only need to see
        # SOME non-zero count come back, not the exact figure).
        unknown = sum(1 for k in cfg if k not in _KNOWN_TOP_KEYS)
        self.cfg_doc = cfg
        self.saves.append(cfg)
        self.generation += 1
        self._reply({"t": "ok", "generation": self.generation, "unknown_keys": unknown})

    def _do_telem_get(self) -> None:
        if "telem" not in self.caps:
            self._err("CFG_ERR_MSG", "unknown message type")
            return
        self._reply(dict(self.telem_line_obj))
