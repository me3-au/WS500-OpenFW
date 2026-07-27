"""
proto.py -- serial JSON-lines transport + protocol client (GH#17).

Implements the CLIENT side of two documents:

  - docs/VERSIONING.md Sec 2, the hello handshake: the client announces the
    highest protocol version it speaks, the firmware always answers honestly
    (it never refuses a hello), and the CLIENT is the one that "refuses
    politely" outside its supported range -- that refusal is WS500ProtoRangeError
    below. Feature enablement is capability-driven (the "caps" list), never a
    version comparison beyond that one range check.
  - control/Inc/config_msg.h, the wire grammar: newline-delimited JSON, one
    object per line in both directions, <= CFG_MSG_LINE_MAX (3072) bytes.
    hello / cfg-get / cfg-set / telem-get map onto the methods below; error
    replies ({"t":"err","code":...}) surface as WS500FirmwareError with the
    validator's own enum name in `.code`, never re-worded.

This module knows nothing about argparse or files -- that is cli.py /
session.py / commands/*.py. It also knows nothing about pyserial beyond the
narrow read/write/close surface in SerialLike, which is deliberate: the whole
class is exercised in tests/ against an in-memory mock, exactly the way the
firmware's own protocol layer is host-injected and tested with no USB stack
anywhere near it (config_msg.h's own docstring makes the same trade).

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import json
import time
from dataclasses import dataclass, field
from typing import Any, Optional, Protocol, runtime_checkable

# ---------------------------------------------------------------------------
# Wire constants -- mirror control/Inc/config_msg.h. Keep these in lock-step
# with that header; there is no build-time check tying the two together
# (Python has no access to the C headers), so a firmware-side bump to
# CFG_PROTO_VERSION or CFG_MSG_LINE_MAX must be echoed here by hand.
# ---------------------------------------------------------------------------

LINE_MAX = 3072  # CFG_MSG_LINE_MAX (control/Inc/config_msg.h)

# This client's supported protocol_version RANGE (VERSIONING Sec 1/Sec 2). A
# hello reply outside [CLIENT_PROTO_MIN, CLIENT_PROTO_MAX] is refused by the
# client -- the firmware never refuses a hello, so somebody has to.
CLIENT_PROTO_MIN = 1
CLIENT_PROTO_MAX = 1  # also the value sent as the hello request's "proto"

DEFAULT_BAUDRATE = 115200
DEFAULT_TIMEOUT_S = 2.0
DEFAULT_RETRIES = 2

CAP_CFG = "cfg"
CAP_TELEM = "telem"

# The USB VID:PID the firmware currently enumerates as. 0x1209:0x0001 is the
# pid.codes "Test PID" (https://pid.codes/1209/0001/) -- a placeholder for
# development only, shared by many hobbyist boards, so autodiscovery must
# treat more than one match as ambiguous rather than guessing.
# NOTE: this WILL change to an assigned VID:PID before release; update here
# (and nowhere else -- nothing else in this package hardcodes it) when it does.
DEFAULT_VID = 0x1209
DEFAULT_PID = 0x0001


# ---------------------------------------------------------------------------
# Exceptions. Every one carries `exit_code`, the code the CLI layer returns to
# the shell, so a command handler never re-derives it from the error type --
# cli.py just does `except WS500Error as e: ... return e.exit_code`.
# ---------------------------------------------------------------------------


class WS500Error(Exception):
    """Base for every error this package raises deliberately (as opposed to
    an unexpected bug, which cli.py reports separately)."""

    exit_code = 1


class WS500ConnectionError(WS500Error):
    """The port could not be opened, autodiscovery could not resolve to
    exactly one port, or the link died mid-exchange."""

    exit_code = 7


class WS500TimeoutError(WS500Error):
    """No complete reply line arrived within `timeout`, after `retries`
    resends of the same request."""

    exit_code = 7


class WS500ProtoRangeError(WS500Error):
    """The firmware's hello "proto" is outside [CLIENT_PROTO_MIN,
    CLIENT_PROTO_MAX]. VERSIONING.md Sec 2: "client refuses politely" -- this
    is that refusal."""

    exit_code = 3


class WS500CapabilityError(WS500Error):
    """A command needs a capability flag that is absent from the hello's
    "caps" list (VERSIONING.md Sec 2: feature sync is capability-driven)."""

    exit_code = 4


class WS500FirmwareError(WS500Error):
    """The firmware answered {"t":"err",...}. `code` is the validator's or
    protocol layer's own enum name verbatim (config_validate.h cfg_err_name(),
    or one of the five CFG_ERR_{JSON,MSG,TYPE,SCHEMA,STORE} the message layer
    adds) -- callers should match on `.code`, not on the message text."""

    exit_code = 5

    def __init__(
        self,
        code: str,
        detail: Optional[str] = None,
        key: Optional[str] = None,
        profile: Optional[int] = None,
        unknown_keys: Optional[int] = None,
    ) -> None:
        self.code = code
        self.detail = detail
        self.key = key
        self.profile = profile
        self.unknown_keys = unknown_keys
        msg = f"firmware rejected the request: {code}"
        if detail:
            msg += f" ({detail})"
        if key:
            msg += f" [key={key}]"
        if profile:
            msg += f" [profile={profile}]"
        super().__init__(msg)


class WS500SchemaError(WS500Error):
    """A config file's schema_version does not match the firmware's native
    schema (VERSIONING.md Sec 3). Raised client-side, before a cfg-set is ever
    sent -- the firmware would reject it too (CFG_ERR_SCHEMA), but there is no
    reason to spend a round trip finding that out."""

    exit_code = 6


class WS500InputError(WS500Error):
    """A local file (the `set` input) could not be read or is not valid JSON."""

    exit_code = 8


class WS500ProtocolError(WS500Error):
    """A reply did not parse as the expected message shape, or an outgoing
    message would violate the wire's own rules (e.g. over LINE_MAX)."""

    exit_code = 1


# ---------------------------------------------------------------------------
# Transport
# ---------------------------------------------------------------------------


@runtime_checkable
class SerialLike(Protocol):
    """The narrow slice of pyserial's Serial that WS500Client needs. Anything
    satisfying this (a real serial.Serial, or tests/mock_serial.py's
    MockSerial) can be handed to WS500Client directly -- structural typing, no
    inheritance required, so the mock owes pyserial nothing but these three
    methods."""

    def write(self, data: bytes) -> int: ...

    def read(self, size: int = 1) -> bytes: ...

    def close(self) -> None: ...


@dataclass
class HelloInfo:
    """Parsed hello reply (VERSIONING.md Sec 2)."""

    fw: Optional[str]
    git: Optional[str]
    proto: int
    schema: int
    caps: list = field(default_factory=list)

    def has_cap(self, name: str) -> bool:
        return name in self.caps


def discover_port(
    vid: int = DEFAULT_VID, pid: int = DEFAULT_PID, ports: Optional[list] = None
) -> str:
    """Scan serial ports for the WS500's VID:PID and return the single
    match's device path (`--port auto`). `ports` is normally left None, which
    pulls the live list from pyserial's serial.tools.list_ports; tests inject
    a canned list of simple objects with .device/.vid/.pid instead, so this
    function never needs a real USB stack to be exercised.

    Raises WS500ConnectionError if there is not EXACTLY one match -- silently
    picking one of several candidates would be a worse failure mode than
    asking the user to pass --port explicitly.
    """
    if ports is None:
        try:
            from serial.tools import list_ports  # type: ignore[import-not-found]
        except ImportError as e:
            raise WS500ConnectionError(
                "pyserial is not installed (`pip install pyserial`)"
            ) from e
        ports = list(list_ports.comports())

    matches = [p for p in ports if getattr(p, "vid", None) == vid and getattr(p, "pid", None) == pid]
    if not matches:
        raise WS500ConnectionError(
            f"no serial port found with VID:PID {vid:04x}:{pid:04x} -- pass --port explicitly"
        )
    if len(matches) > 1:
        found = ", ".join(getattr(m, "device", repr(m)) for m in matches)
        raise WS500ConnectionError(
            f"multiple ports match VID:PID {vid:04x}:{pid:04x} ({found}) -- pass --port explicitly"
        )
    return matches[0].device


class WS500Client:
    """One connection to a WS500 running WS500-OpenFW. Wraps a SerialLike and
    speaks the newline-delimited JSON grammar in config_msg.h.

    Construct directly with any SerialLike (tests do this with MockSerial);
    use `WS500Client.open()` for a real serial port.
    """

    def __init__(
        self,
        connection: SerialLike,
        *,
        timeout: float = DEFAULT_TIMEOUT_S,
        retries: int = DEFAULT_RETRIES,
    ) -> None:
        self._conn = connection
        self.timeout = timeout
        self.retries = retries
        self.hello_info: Optional[HelloInfo] = None

    @classmethod
    def open(
        cls,
        port: str,
        *,
        baudrate: int = DEFAULT_BAUDRATE,
        timeout: float = DEFAULT_TIMEOUT_S,
        retries: int = DEFAULT_RETRIES,
    ) -> "WS500Client":
        """Open a real serial port via pyserial. pyserial is imported here,
        lazily, so nothing in this module needs it installed to run the
        mock-backed test suite."""
        try:
            import serial  # type: ignore[import-not-found]
        except ImportError as e:
            raise WS500ConnectionError(
                "pyserial is not installed (`pip install pyserial`)"
            ) from e
        try:
            conn = serial.Serial(port=port, baudrate=baudrate, timeout=timeout)
        except Exception as e:  # serial.SerialException, OSError, ...
            raise WS500ConnectionError(f"could not open {port}: {e}") from e
        return cls(conn, timeout=timeout, retries=retries)

    def close(self) -> None:
        try:
            self._conn.close()
        except Exception:
            pass

    def __enter__(self) -> "WS500Client":
        return self

    def __exit__(self, *exc_info: Any) -> None:
        self.close()

    # ---- line I/O -----------------------------------------------------

    def _send_line(self, obj: dict) -> None:
        try:
            # allow_nan=False: config_json.h's reader has no NaN/Infinity
            # literal (non-finite values travel as JSON null, config_doc.c
            # emit_i8/rd_f32); a document that still has a raw float('nan') in
            # it at send time is a client bug, not something to paper over.
            line = json.dumps(obj, separators=(",", ":"), allow_nan=False)
        except ValueError as e:
            raise WS500ProtocolError(f"refusing to send a non-finite value: {e}") from e
        data = (line + "\n").encode("utf-8")
        if len(data) > LINE_MAX:
            raise WS500ProtocolError(
                f"message is {len(data)} bytes, over the {LINE_MAX}-byte line "
                "limit (config_msg.h CFG_MSG_LINE_MAX) -- refusing to send it"
            )
        try:
            self._conn.write(data)
        except Exception as e:
            raise WS500ConnectionError(f"write failed: {e}") from e

    def _read_line(self, deadline: float) -> Optional[str]:
        """Read one newline-terminated line, or None once `deadline`
        (time.monotonic()) passes first. A byte that fails to decode as UTF-8
        is replaced rather than raised -- a stray byte of line noise on a USB
        CDC port must not crash the client; config_json.h's own reader is
        similarly permissive about raw bytes >= 0x80 inside strings."""
        buf = bytearray()
        while True:
            remaining = deadline - time.monotonic()
            if remaining <= 0:
                return None
            try:
                chunk = self._conn.read(1)
            except Exception as e:
                raise WS500ConnectionError(f"read failed: {e}") from e
            if not chunk:
                # pyserial's read() returns b"" on its own per-call timeout;
                # our deadline (checked above) is the real budget.
                continue
            if chunk == b"\n":
                return buf.decode("utf-8", errors="replace")
            if chunk != b"\r":
                buf.extend(chunk)
            if len(buf) > LINE_MAX + 256:
                # The firmware would have dropped a line this long to
                # CFG_ERR_JSON on its own side; seeing one back means the
                # transport is producing garbage, not a real reply.
                raise WS500ProtocolError("reply line grew past the protocol's line limit")

    def _recv_reply(self, deadline: float) -> dict:
        """Read lines until one parses as a JSON object with a string "t"
        member. Anything else -- boot-banner text, a truncated fragment, plain
        noise -- is skipped rather than treated as fatal, up to `deadline`."""
        while True:
            line = self._read_line(deadline)
            if line is None:
                raise WS500TimeoutError("no reply within timeout")
            line = line.strip()
            if not line:
                continue
            try:
                obj = json.loads(line)
            except json.JSONDecodeError:
                continue  # noise on the wire; keep waiting for the real reply
            if not isinstance(obj, dict) or not isinstance(obj.get("t"), str):
                continue
            return obj

    def request(self, obj: dict) -> dict:
        """Send one request line and return the parsed reply object as-is
        (including {"t":"err",...} -- callers that care use `_raise_if_err`).
        On timeout, resends the SAME request up to `self.retries` more times
        before giving up."""
        last_exc: Optional[WS500TimeoutError] = None
        for _attempt in range(self.retries + 1):
            self._send_line(obj)
            deadline = time.monotonic() + self.timeout
            try:
                return self._recv_reply(deadline)
            except WS500TimeoutError as e:
                last_exc = e
                continue
        assert last_exc is not None
        raise last_exc

    @staticmethod
    def _raise_if_err(reply: dict) -> dict:
        if reply.get("t") == "err":
            raise WS500FirmwareError(
                code=reply.get("code", "CFG_ERR_UNKNOWN"),
                detail=reply.get("detail"),
                key=reply.get("key"),
                profile=reply.get("profile"),
                unknown_keys=reply.get("unknown_keys"),
            )
        return reply

    # ---- protocol calls -------------------------------------------------

    def hello(self) -> HelloInfo:
        """{"t":"hello","proto":<max>} -> HelloInfo. Enforces the proto-range
        gate (VERSIONING.md Sec 2) and caches the result for require_cap()."""
        reply = self._raise_if_err(self.request({"t": "hello", "proto": CLIENT_PROTO_MAX}))
        if reply.get("t") != "hello":
            raise WS500ProtocolError(f"expected a hello reply, got {reply.get('t')!r}")
        proto_v = reply.get("proto")
        if not isinstance(proto_v, int):
            raise WS500ProtocolError('hello reply has no integer "proto"')
        if not (CLIENT_PROTO_MIN <= proto_v <= CLIENT_PROTO_MAX):
            raise WS500ProtoRangeError(
                f"firmware speaks protocol {proto_v}; this ws500ctl build supports "
                f"{CLIENT_PROTO_MIN}-{CLIENT_PROTO_MAX} only. Use the ws500ctl "
                "release matching this firmware (docs/VERSIONING.md Sec 2/Sec 4)."
            )
        schema_v = reply.get("schema")
        info = HelloInfo(
            fw=reply.get("fw"),
            git=reply.get("git"),
            proto=proto_v,
            schema=schema_v if isinstance(schema_v, int) else -1,
            caps=list(reply.get("caps") or []),
        )
        self.hello_info = info
        return info

    def require_cap(self, name: str) -> None:
        """Raise WS500CapabilityError unless `name` is in the hello's caps
        list, fetching a hello first if this is the first call this session
        (VERSIONING.md Sec 2: feature sync is capability-driven, never
        version-sniffed)."""
        if self.hello_info is None:
            self.hello()
        assert self.hello_info is not None
        if not self.hello_info.has_cap(name):
            raise WS500CapabilityError(
                f'firmware does not advertise capability "{name}" '
                f"(caps={self.hello_info.caps}) -- this command is unavailable "
                "on this firmware build."
            )

    def cfg_get(self) -> dict:
        """{"t":"cfg-get"} -> the PROFILE_SPEC Sec 7 document (as `dict`)."""
        self.require_cap(CAP_CFG)
        reply = self._raise_if_err(self.request({"t": "cfg-get"}))
        if reply.get("t") != "cfg" or not isinstance(reply.get("cfg"), dict):
            raise WS500ProtocolError('cfg-get reply is missing a "cfg" object')
        return reply["cfg"]

    def cfg_set(self, cfg_doc: dict) -> dict:
        """{"t":"cfg-set","cfg":cfg_doc} -> {"generation":N,"unknown_keys":K}.
        Raises WS500FirmwareError on a named rejection -- this layer does not
        pre-validate the document beyond the wire's own bounds; the firmware
        is the schema/range authority (VERSIONING.md Sec 3)."""
        self.require_cap(CAP_CFG)
        reply = self._raise_if_err(self.request({"t": "cfg-set", "cfg": cfg_doc}))
        if reply.get("t") != "ok":
            raise WS500ProtocolError(f"unexpected cfg-set reply: {reply}")
        return reply

    def telem_get(self) -> dict:
        """{"t":"telem-get"} -> one telemetry_json.h snapshot (as `dict`)."""
        self.require_cap(CAP_TELEM)
        reply = self._raise_if_err(self.request({"t": "telem-get"}))
        if reply.get("t") != "telem":
            raise WS500ProtocolError(f"unexpected telem-get reply: {reply}")
        return reply

    def ping(self) -> float:
        """Round-trip time of a fresh hello exchange, in seconds."""
        t0 = time.monotonic()
        self.hello()
        return time.monotonic() - t0
