"""
test_proto.py -- ws500ctl.proto against tests/mock_serial.py's firmware
simulation (GH#17). No hardware, no real serial port.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import pytest

from ws500ctl import proto

from .fixtures import default_cfg_doc
from .mock_serial import MockSerial


def client(ms: MockSerial, **kw) -> proto.WS500Client:
    kw.setdefault("timeout", 0.2)
    kw.setdefault("retries", 2)
    return proto.WS500Client(ms, **kw)


# ---- hello + proto-range gate (VERSIONING Sec 2) ---------------------------


def test_hello_happy_path():
    ms = MockSerial(fw="0.1.0-dev", git="deadbee", fw_proto=1, fw_schema=1, caps=["cfg", "telem"])
    c = client(ms)
    info = c.hello()
    assert info.fw == "0.1.0-dev"
    assert info.git == "deadbee"
    assert info.proto == 1
    assert info.schema == 1
    assert info.caps == ["cfg", "telem"]
    # the request itself announces the client's own max-supported proto
    assert ms.requests[-1] == {"t": "hello", "proto": proto.CLIENT_PROTO_MAX}


@pytest.mark.parametrize("fw_proto", [0, 2])
def test_hello_proto_out_of_range_is_refused(fw_proto):
    ms = MockSerial(fw_proto=fw_proto)
    c = client(ms)
    with pytest.raises(proto.WS500ProtoRangeError) as ei:
        c.hello()
    assert ei.value.exit_code == 3


def test_hello_proto_in_range_is_accepted():
    ms = MockSerial(fw_proto=1)
    info = client(ms).hello()
    assert info.proto == 1


def test_hello_missing_optional_fields():
    ms = MockSerial(fw=None, git=None)
    info = client(ms).hello()
    assert info.fw is None
    assert info.git is None


# ---- capability gating -------------------------------------------------


def test_cfg_get_requires_cfg_cap():
    ms = MockSerial(caps=["telem"])  # no "cfg"
    c = client(ms)
    with pytest.raises(proto.WS500CapabilityError) as ei:
        c.cfg_get()
    assert ei.value.exit_code == 4


def test_telem_get_requires_telem_cap():
    ms = MockSerial(caps=["cfg"])  # no "telem"
    c = client(ms)
    with pytest.raises(proto.WS500CapabilityError) as ei:
        c.telem_get()
    assert ei.value.exit_code == 4


def test_require_cap_fetches_hello_implicitly():
    ms = MockSerial(caps=["cfg"])
    c = client(ms)
    assert c.hello_info is None
    doc = c.cfg_get()
    assert c.hello_info is not None  # hello happened as a side effect
    assert isinstance(doc, dict)


# ---- cfg-get / cfg-set round trip --------------------------------------


def test_cfg_get_returns_the_fixture_document():
    fixture = default_cfg_doc()
    ms = MockSerial(cfg_doc=fixture)
    doc = client(ms).cfg_get()
    assert doc == fixture
    assert doc["schema_version"] == 1
    assert doc["primitives"]["v_bulk"] == 3.60
    assert doc["profiles"][6]["reserved"] is True
    assert doc["limits"]["field"]["rotor_v_max"] is None  # NAN -> null


def test_cfg_set_round_trip_ok():
    ms = MockSerial()
    c = client(ms)
    doc = c.cfg_get()
    doc["global"]["ramp_w_per_s"] = 150.0
    reply = c.cfg_set(doc)
    assert reply["t"] == "ok"
    assert reply["generation"] == 8  # started at 7 in MockSerial
    assert reply["unknown_keys"] == 0
    assert ms.saves[-1]["global"]["ramp_w_per_s"] == 150.0


def test_cfg_set_unknown_top_level_key_is_counted():
    ms = MockSerial()
    c = client(ms)
    doc = default_cfg_doc()
    doc["engine"] = {"idle_rpm": 800}  # config_doc.h deviation 1: no schema-v1 home
    reply = c.cfg_set(doc)
    assert reply["unknown_keys"] == 1


# ---- error replies surface with the firmware's own code --------------


def test_firmware_error_reply_propagates_named_code():
    ms = MockSerial(fw_schema=1)
    c = client(ms)
    with pytest.raises(proto.WS500FirmwareError) as ei:
        c.cfg_set({"schema_version": 99})
    assert ei.value.code == "CFG_ERR_SCHEMA"
    assert ei.value.exit_code == 5
    assert ei.value.key == "schema_version"


def test_unknown_message_type_is_cfg_err_msg():
    ms = MockSerial()
    c = client(ms)
    # request() itself does not interpret "err" replies -- only the typed
    # calls (cfg_get/cfg_set/telem_get/hello) raise WS500FirmwareError.
    reply = c.request({"t": "not-a-real-type"})
    assert reply["t"] == "err"
    assert reply["code"] == "CFG_ERR_MSG"


# ---- outgoing line-length guard (client-side, before the wire) --------


def test_outgoing_oversize_line_is_refused_client_side():
    ms = MockSerial()
    c = client(ms)
    c.hello()  # establishes caps so cfg_set doesn't also send a hello
    before = ms.write_count
    huge = default_cfg_doc()
    huge["global"]["pad"] = "x" * (proto.LINE_MAX + 500)
    with pytest.raises(proto.WS500ProtocolError):
        c.cfg_set(huge)
    # nothing was written for the oversize request -- the guard fires first
    assert ms.write_count == before


# ---- oversize line arriving FROM the simulated firmware -----------------


def test_incoming_oversize_line_reply():
    ms = MockSerial()
    ms._handle_line(b'{"t":"cfg-set","cfg":{"pad":"' + b"x" * (proto.LINE_MAX + 100) + b'"}}')
    out = bytes(ms._tx).decode("utf-8")
    assert '"code":"CFG_ERR_JSON"' in out
    assert "line longer" in out


# ---- garbage bytes on the wire are skipped, not fatal -------------------


def test_garbage_line_before_a_real_reply_is_skipped():
    ms = MockSerial()
    ms.inject_noise("WS500 booting, please stand by...")
    c = client(ms)
    info = c.hello()  # the noise line precedes the real hello reply
    assert info.proto == 1


def test_garbage_bytes_without_trailing_newline_do_not_hang():
    ms = MockSerial()
    # Not terminated with \n on purpose: it merges into the next real line's
    # bytes, so the FIRST line the client parses is garbage+json glued
    # together (invalid JSON) and gets skipped; the retry's resend produces a
    # second, clean reply that succeeds.
    ms._tx.extend(b"\xff\xfe stray bytes")
    c = client(ms, timeout=0.05, retries=2)
    info = c.hello()
    assert info.proto == 1


# ---- timeout + retries ---------------------------------------------------


def test_timeout_retries_then_raises():
    ms = MockSerial(silent=True)
    c = client(ms, timeout=0.05, retries=2)
    with pytest.raises(proto.WS500TimeoutError) as ei:
        c.hello()
    assert ei.value.exit_code == 7
    assert ms.write_count == 3  # 1 initial attempt + 2 retries


def test_zero_retries_means_one_attempt():
    ms = MockSerial(silent=True)
    c = client(ms, timeout=0.05, retries=0)
    with pytest.raises(proto.WS500TimeoutError):
        c.hello()
    assert ms.write_count == 1


# ---- ping -----------------------------------------------------------------


def test_ping_returns_a_nonnegative_duration():
    ms = MockSerial()
    rtt = client(ms).ping()
    assert rtt >= 0.0


# ---- port autodiscovery ----------------------------------------------------


class _FakePortInfo:
    def __init__(self, device, vid, pid):
        self.device = device
        self.vid = vid
        self.pid = pid


def test_discover_port_single_match():
    ports = [
        _FakePortInfo("COM3", 0x0403, 0x6001),
        _FakePortInfo("COM5", proto.DEFAULT_VID, proto.DEFAULT_PID),
    ]
    assert proto.discover_port(ports=ports) == "COM5"


def test_discover_port_no_match_raises():
    ports = [_FakePortInfo("COM3", 0x0403, 0x6001)]
    with pytest.raises(proto.WS500ConnectionError):
        proto.discover_port(ports=ports)


def test_discover_port_ambiguous_raises():
    ports = [
        _FakePortInfo("COM5", proto.DEFAULT_VID, proto.DEFAULT_PID),
        _FakePortInfo("COM7", proto.DEFAULT_VID, proto.DEFAULT_PID),
    ]
    with pytest.raises(proto.WS500ConnectionError):
        proto.discover_port(ports=ports)
