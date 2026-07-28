"""
test_cli.py -- end-to-end: run the CLI against a fixture file and check
the two output files it writes (default paths, and explicit -o/-r), plus
error handling for a missing input file.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import json
import shutil
from pathlib import Path

from stock_config_import.cli import main

FIXTURES = Path(__file__).parent / "fixtures"


def test_cli_writes_default_output_paths_next_to_input(tmp_path):
    src = tmp_path / "capture.txt"
    shutil.copy(FIXTURES / "synthetic_48v_battleborn.txt", src)

    rc = main([str(src)])
    assert rc == 0

    out_json = tmp_path / "capture.txt.proposed.json"
    out_report = tmp_path / "capture.txt.report.txt"
    assert out_json.is_file()
    assert out_report.is_file()

    doc = json.loads(out_json.read_text())
    assert doc["global"]["cells_series"] == 16
    assert "hunter2" not in out_report.read_text()


def test_cli_respects_explicit_output_paths(tmp_path):
    src = tmp_path / "capture.txt"
    shutil.copy(FIXTURES / "synthetic_minimal_sparse.txt", src)
    out_json = tmp_path / "custom.json"
    out_report = tmp_path / "custom.txt"

    rc = main([str(src), "-o", str(out_json), "-r", str(out_report)])
    assert rc == 0
    assert out_json.is_file()
    assert out_report.is_file()


def test_cli_reports_error_for_missing_input(tmp_path, capsys):
    rc = main([str(tmp_path / "does_not_exist.txt")])
    assert rc == 1
    captured = capsys.readouterr()
    assert "no such file" in captured.err
