"""
test_report.py -- render_text() output shape and the same password-leak
guarantee re-checked at the rendered-text layer (belt and suspenders on
top of test_mapper.py's structural checks).

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

from pathlib import Path

from stock_config_import.mapper import translate
from stock_config_import.parser import parse_capture_file
from stock_config_import.report import render_text

FIXTURES = Path(__file__).parent / "fixtures"


def test_render_text_has_all_four_section_headers():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    _doc, report = translate(cap)
    text = render_text(report, source_path="fixtures/synthetic_48v_battleborn.txt")
    for header in ("## CONVERTED", "## MAPPED", "## DROPPED", "## UNRECOGNISED"):
        assert header in text


def test_render_text_carries_the_synthetic_fixture_warning():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    _doc, report = translate(cap)
    text = render_text(report, source_path="x")
    assert "SYNTHETIC-FIXTURE WARNING" in text


def test_render_text_never_contains_the_password():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    _doc, report = translate(cap)
    text = render_text(report, source_path="x")
    assert "hunter2" not in text
    assert "REDACTED" in text


def test_render_text_shows_the_conversion_arithmetic():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    _doc, report = translate(cap)
    text = render_text(report, source_path="x")
    # BC Mult -> bank_capacity_ah's math must be visible, not just the result.
    assert "0.6" in text and "500" in text and "300" in text
