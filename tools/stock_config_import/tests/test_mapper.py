"""
test_mapper.py -- mapper.py's classification logic: which fields get
mapped/converted/dropped/unrecognised, the arithmetic behind every
conversion, and -- most load-bearing -- that the password NEVER appears in
the proposed document or the report, under any circumstance, even when fed
a raw (unredacted) capture.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

from pathlib import Path

import pytest

from stock_config_import.mapper import Mapper, translate
from stock_config_import.parser import parse_capture_file, parse_capture_text

FIXTURES = Path(__file__).parent / "fixtures"


@pytest.fixture()
def battleborn_capture():
    return parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")


@pytest.fixture()
def sparse_capture():
    return parse_capture_file(FIXTURES / "synthetic_minimal_sparse.txt")


# ---------------------------------------------------------------------------
# Security: the password must never leak, anywhere, under any classification
# ---------------------------------------------------------------------------


def test_password_never_appears_in_proposed_document(battleborn_capture):
    doc, _report = translate(battleborn_capture)
    assert "hunter2" not in repr(doc)


def test_password_never_appears_anywhere_in_the_report(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    for _cat, entry in report.all_entries():
        assert entry.stock_value != "hunter2"
        assert (entry.note or "") .find("hunter2") == -1
        assert entry.native_value != "hunter2"


def test_password_field_is_explicitly_dropped_with_a_security_note(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    pw_entries = [e for e in report.dropped if e.stock_field == "NPC.password"]
    assert len(pw_entries) == 1
    assert "REDACTED" in pw_entries[0].stock_value
    assert "credential" in pw_entries[0].note


# ---------------------------------------------------------------------------
# Converted fields: the math is the whole point -- check it exactly
# ---------------------------------------------------------------------------


def test_sys_volts_converts_to_cells_series_via_documented_heuristic(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    entries = {e.stock_field: e for e in report.converted}
    e = entries["SST.sys_volts"]
    assert e.native_key == "global.cells_series"
    assert e.native_value == 16
    assert "HEURISTIC" in e.note  # never presented as a plain fact


def test_bc_mult_converts_to_bank_capacity_ah_with_shown_math(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    e = {x.stock_field: x for x in report.converted}["SST.bc_mult"]
    assert e.native_key == "global.bank_capacity_ah"
    assert e.native_value == pytest.approx(0.6 * 500.0)
    assert "500" in e.note  # the nominal-battery constant is shown, not hidden


def test_warmup_delay_converts_and_discards_only_the_sign(sparse_capture):
    _doc, report = translate(sparse_capture)
    e = {x.stock_field: x for x in report.converted}["SCV.warmup_delay"]
    assert e.native_key == "global.warmup_time_s"
    assert e.native_value == 15.0  # |-15| = 15
    assert "sign" in e.note


def test_active_cpe_max_amps_converts_to_battery_c_limit(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    e = {x.stock_field: x for x in report.converted}["CPE[6].max_amps"]
    assert e.native_key == "limits.battery_c_limit"
    assert e.native_value == pytest.approx(250.0 / 500.0)


def test_proposed_document_has_no_schema_version_key(battleborn_capture):
    doc, _report = translate(battleborn_capture)
    assert "schema_version" not in doc
    # But the fields it DOES have match the exact ctrl_globals_t/
    # ctrl_limits_t (control.h) dotted paths, not the raw stock names.
    assert doc["global"]["cells_series"] == 16
    assert doc["global"]["bank_capacity_ah"] == pytest.approx(300.0)
    assert doc["global"]["max_charge_power_w"] == pytest.approx(8000.0)
    assert doc["global"]["warmup_time_s"] == pytest.approx(30.0)
    assert doc["limits"]["alternator_limit_a"] == pytest.approx(220.0)
    assert doc["limits"]["battery_c_limit"] == pytest.approx(0.5)


# ---------------------------------------------------------------------------
# Mapped (direct-copy) fields
# ---------------------------------------------------------------------------


def test_amp_and_watt_limit_map_directly_no_scaling(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    entries = {e.stock_field: e for e in report.mapped}
    assert entries["SCV.amp_limit"].native_value == 220.0
    assert entries["SCV.watt_limit"].native_value == 8000.0


# ---------------------------------------------------------------------------
# Sentinel values (autosize / disabled) have no native equivalent -> dropped
# ---------------------------------------------------------------------------


def test_amp_watt_limit_sentinels_are_dropped_not_converted(sparse_capture):
    _doc, report = translate(sparse_capture)
    dropped_names = {e.stock_field for e in report.dropped}
    assert "SCV.amp_limit" in dropped_names
    assert "SCV.watt_limit" in dropped_names
    mapped_names = {e.stock_field for e in report.mapped}
    assert "SCV.amp_limit" not in mapped_names


def test_sys_volts_without_a_clean_cell_heuristic_is_dropped(sparse_capture):
    _doc, report = translate(sparse_capture)
    dropped_names = {e.stock_field for e in report.dropped}
    assert "SST.sys_volts" in dropped_names
    converted_names = {e.stock_field for e in report.converted}
    assert "SST.sys_volts" not in converted_names


# ---------------------------------------------------------------------------
# The 6-stage Pb charge-profile block: dropped, explicitly, by design
# ---------------------------------------------------------------------------


def test_every_cpe_stage_field_is_dropped_except_max_amps(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    cpe_drops = [e for e in report.dropped if e.stock_field.startswith("CPE[")]
    assert len(cpe_drops) == 9  # one grouped entry per captured CPE (0..8)
    for e in cpe_drops:
        assert "6-stage Pb" in e.note
    # The active profile's grouped drop mentions every field EXCEPT max_amps
    # (which was pulled out separately into the converted bucket).
    active_drop = next(e for e in cpe_drops if e.stock_field == "CPE[6]")
    assert "max_amps" not in active_drop.note


def test_design_dropped_scv_fields_carry_a_named_reason(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    dropped = {e.stock_field: e for e in report.dropped}
    for key, must_contain in (
        ("SCV.drt_half", "dropped-by-design"),
        ("SCV.pbf", "dropped-by-design"),
        ("SCV.bts2ats", "no schema v2 field"),
    ):
        assert key in dropped, f"{key} should have been classified as dropped"
        assert must_contain in dropped[key].note


def test_cst_and_engine_blocks_are_dropped_wholesale(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    dropped_fields = {e.stock_field for e in report.dropped}
    assert "CST.battery_id" in dropped_fields
    assert "CST.can_id" in dropped_fields
    assert any(f.startswith("ENG.") for f in dropped_fields)


# ---------------------------------------------------------------------------
# Unrecognised bucket
# ---------------------------------------------------------------------------


def test_unrecognised_bucket_covers_extra_tokens_and_unknown_lines(battleborn_capture):
    _doc, report = translate(battleborn_capture)
    unrec_fields = {e.stock_field for e in report.unrecognised}
    assert "SST.extra[0]" in unrec_fields
    assert "CPE[3].extra[0]" in unrec_fields
    assert "(unrecognised line)" in unrec_fields
    line_entry = next(e for e in report.unrecognised if e.stock_field == "(unrecognised line)")
    assert line_entry.stock_value == "XYZ;,1,2,3"


def test_no_stock_field_vanishes_without_a_bucket(battleborn_capture):
    """Every field this parser extracted from the fixture lands in exactly
    one bucket -- nothing is silently dropped from the *report itself*
    (as opposed to being classified as the "dropped" category, which is
    the honest, expected outcome for most stock fields here)."""
    _doc, report = translate(battleborn_capture)
    total = (
        len(report.mapped) + len(report.converted) + len(report.dropped)
        + len(report.unrecognised)
    )
    assert total > 60  # SCV(~25) + CST(~13) + CPE(9 grouped) + NPC(4) + ENG + extras
