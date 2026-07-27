"""
test_cli.py -- ws500ctl.cli dispatch and exit codes against a mocked
transport (GH#17). session.open_client is monkeypatched so no real serial
port / pyserial install is exercised; the mocked client still speaks the same
proto.WS500Client the real one does, so command logic (schema checks,
dry-run, capability gating, output formatting) is tested end to end.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import json

import pytest

from ws500ctl import cli, proto, session

from .fixtures import default_cfg_doc
from .mock_serial import MockSerial


def use_mock(monkeypatch, ms: MockSerial) -> None:
    def _fake_open_client(args):
        return proto.WS500Client(ms, timeout=args.timeout, retries=args.retries)

    monkeypatch.setattr(session, "open_client", _fake_open_client)


# ---- info -------------------------------------------------------------


def test_info_prints_hello_fields(monkeypatch, capsys):
    ms = MockSerial(fw="1.2.3")
    use_mock(monkeypatch, ms)
    rc = cli.main(["--port", "MOCK", "info"])
    out = capsys.readouterr().out
    assert rc == 0
    assert "1.2.3" in out
    assert "protocol   : 1" in out
    assert "schema     : 1" in out


# ---- get ----------------------------------------------------------------


def test_get_writes_file(monkeypatch, capsys, tmp_path):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    out_file = tmp_path / "cfg.json"
    rc = cli.main(["--port", "MOCK", "get", "-o", str(out_file)])
    assert rc == 0
    written = json.loads(out_file.read_text(encoding="utf-8"))
    assert written == ms.cfg_doc
    assert "wrote" in capsys.readouterr().err


def test_get_prints_to_stdout_by_default(monkeypatch, capsys):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    rc = cli.main(["--port", "MOCK", "get"])
    out = capsys.readouterr().out
    assert rc == 0
    assert json.loads(out) == ms.cfg_doc


# ---- set: dry-run sends nothing -----------------------------------------


def test_set_dry_run_sends_nothing(monkeypatch, capsys, tmp_path):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    doc = default_cfg_doc()
    doc["global"]["ramp_w_per_s"] = 321.0
    f = tmp_path / "in.json"
    f.write_text(json.dumps(doc), encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f), "--dry-run"])
    out = capsys.readouterr().out

    assert rc == 0
    assert "DRY RUN" in out
    assert ms.saves == []  # nothing was stored
    assert all(r.get("t") != "cfg-set" for r in ms.requests)  # nothing was even sent


def test_set_real_send_ok(monkeypatch, capsys, tmp_path):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    doc = default_cfg_doc()
    doc["global"]["ramp_w_per_s"] = 321.0
    f = tmp_path / "in.json"
    f.write_text(json.dumps(doc), encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f)])
    out = capsys.readouterr().out

    assert rc == 0
    assert "ok: generation=" in out
    assert ms.saves and ms.saves[-1]["global"]["ramp_w_per_s"] == 321.0


# ---- set: schema mismatch, both directions -------------------------------


def test_set_schema_older_than_firmware_is_refused(monkeypatch, capsys, tmp_path):
    ms = MockSerial(fw_schema=1)
    use_mock(monkeypatch, ms)
    doc = default_cfg_doc()
    doc["schema_version"] = 0  # older than the firmware's schema 1
    f = tmp_path / "in.json"
    f.write_text(json.dumps(doc), encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f)])
    err = capsys.readouterr().err

    assert rc == 6
    assert "migrate" in err
    assert "GH#17" in err
    assert ms.saves == []


def test_set_schema_newer_than_firmware_is_refused(monkeypatch, capsys, tmp_path):
    ms = MockSerial(fw_schema=1)
    use_mock(monkeypatch, ms)
    doc = default_cfg_doc()
    doc["schema_version"] = 2  # newer than the firmware's schema 1
    f = tmp_path / "in.json"
    f.write_text(json.dumps(doc), encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f)])
    err = capsys.readouterr().err

    assert rc == 6
    assert "NEWER firmware" in err
    assert ms.saves == []


def test_set_missing_schema_version_is_an_overlay_edit(monkeypatch, tmp_path):
    ms = MockSerial(fw_schema=1)
    use_mock(monkeypatch, ms)
    f = tmp_path / "in.json"
    f.write_text(json.dumps({"global": {"ramp_w_per_s": 111.0}}), encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f)])
    assert rc == 0
    assert ms.saves and ms.saves[-1]["global"]["ramp_w_per_s"] == 111.0


# ---- set: invalid input file ----------------------------------------------


def test_set_invalid_json_input_exit_code(monkeypatch, capsys, tmp_path):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    f = tmp_path / "bad.json"
    f.write_text("{not json", encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f)])
    assert rc == 8
    assert "not valid JSON" in capsys.readouterr().err


def test_set_missing_input_file_exit_code(monkeypatch, capsys, tmp_path):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    missing = tmp_path / "nope.json"

    rc = cli.main(["--port", "MOCK", "set", "-i", str(missing)])
    assert rc == 8


# ---- protocol gate / capability / firmware-error exit codes -------------


def test_proto_gate_exit_code(monkeypatch, capsys):
    ms = MockSerial(fw_proto=2)
    use_mock(monkeypatch, ms)
    rc = cli.main(["--port", "MOCK", "info"])
    assert rc == 3
    assert "protocol" in capsys.readouterr().err.lower()


def test_capability_missing_exit_code(monkeypatch, capsys):
    ms = MockSerial(caps=["telem"])  # no "cfg"
    use_mock(monkeypatch, ms)
    rc = cli.main(["--port", "MOCK", "get"])
    assert rc == 4
    assert "cfg" in capsys.readouterr().err


def test_firmware_error_exit_code(monkeypatch, capsys, tmp_path):
    ms = MockSerial(force_cfg_set_error={"code": "CFG_ERR_RANGE_V_BULK", "detail": "out of range"})
    use_mock(monkeypatch, ms)
    doc = default_cfg_doc()
    del doc["schema_version"]  # send as an overlay edit, past the client-side check
    f = tmp_path / "in.json"
    f.write_text(json.dumps(doc), encoding="utf-8")

    rc = cli.main(["--port", "MOCK", "set", "-i", str(f)])
    err = capsys.readouterr().err
    assert rc == 5
    assert "CFG_ERR_RANGE_V_BULK" in err


def test_timeout_exit_code(monkeypatch, capsys):
    ms = MockSerial(silent=True)
    use_mock(monkeypatch, ms)
    rc = cli.main(["--port", "MOCK", "--timeout", "0.05", "--retries", "1", "ping"])
    assert rc == 7


def test_missing_port_exit_code(capsys):
    # Deliberately NOT monkeypatched: exercises the real session.resolve_port.
    rc = cli.main(["info"])
    assert rc == 7
    assert "--port" in capsys.readouterr().err


# ---- telem --------------------------------------------------------------


def test_telem_single_snapshot_json(monkeypatch, capsys):
    ms = MockSerial()
    use_mock(monkeypatch, ms)
    rc = cli.main(["--port", "MOCK", "telem", "--json"])
    out = capsys.readouterr().out
    assert rc == 0
    line = json.loads(out.strip())
    assert line["t"] == "telem"


def test_telem_follow_stops_on_ctrl_c(monkeypatch, capsys):
    ms = MockSerial()
    use_mock(monkeypatch, ms)

    calls = {"n": 0}

    def fake_sleep(_seconds):
        calls["n"] += 1
        raise KeyboardInterrupt

    monkeypatch.setattr("ws500ctl.commands.telem.time.sleep", fake_sleep)

    rc = cli.main(["--port", "MOCK", "telem", "--follow", "--json"])
    out = capsys.readouterr().out
    assert rc == 0
    assert calls["n"] == 1
    assert len(out.strip().splitlines()) == 1  # one snapshot printed before the interrupt


# ---- stub subcommands ------------------------------------------------------


def test_import_stub(capsys):
    rc = cli.main(["import"])
    assert rc == 1
    assert "GH#17" in capsys.readouterr().err


def test_flash_stub(capsys):
    rc = cli.main(["flash"])
    assert rc == 1
    assert "GH#30" in capsys.readouterr().err


# ---- --version ------------------------------------------------------------


def test_version_flag(capsys):
    with pytest.raises(SystemExit) as ei:
        cli.main(["--version"])
    assert ei.value.code == 0
    assert "ws500ctl" in capsys.readouterr().out
