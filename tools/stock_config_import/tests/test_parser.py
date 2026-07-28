"""
test_parser.py -- parser.py against the synthetic fixtures (see
tests/fixtures/README.md: no real Stage-A capture exists yet). Covers
correct field extraction, tolerant handling of shorter-than-documented
records (real field-count drift, lifted from the Comms & Config Guide's
own $RSS:CST@ example), extra-token capture, and detection of a wholly
unrecognised reply tag.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

from pathlib import Path

from stock_config_import.parser import (
    CPE_FIELDS,
    NPC_FIELDS,
    SCV_FIELDS,
    SST_FIELDS,
    parse_capture_file,
    parse_capture_text,
)

FIXTURES = Path(__file__).parent / "fixtures"


def test_parses_sst_scv_npc_cst_and_all_nine_cpe_entries():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")

    assert cap.sst is not None
    assert cap.sst.fields["version"] == "AREG2.6.1"
    assert cap.sst.fields["cp_index"] == "6"
    assert cap.sst.fields["sys_volts"] == "4"
    assert cap.sst.fields["bc_mult"] == "0.6"

    assert cap.scv is not None
    assert cap.scv.fields["amp_limit"] == "220"
    assert cap.scv.fields["watt_limit"] == "8000"
    assert cap.scv.fields["warmup_delay"] == "30"

    assert cap.npc is not None
    assert cap.npc.fields["name"] == "MainsAlt48"
    assert cap.npc.fields["password"] == "hunter2"  # parser itself does NOT redact

    assert cap.cst is not None
    assert cap.cst.fields["battery_id"] == "1"
    assert cap.cst.fields["can_id"] == "129"

    assert sorted(cap.cpe.keys()) == list(range(0, 9))
    assert cap.cpe[6].fields["acpt_vbat"] == "14.2"
    assert cap.cpe[6].fields["max_amps"] == "250"
    assert cap.cpe[0].fields["max_amps"] == "250"  # currently-selected == active #6


def test_every_field_table_has_no_duplicate_names():
    # A duplicate name would silently overwrite a field during parsing --
    # this locks in that every position table stays internally consistent.
    for name, table in (
        ("SST_FIELDS", SST_FIELDS),
        ("SCV_FIELDS", SCV_FIELDS),
        ("NPC_FIELDS", NPC_FIELDS),
        ("CPE_FIELDS", CPE_FIELDS),
    ):
        assert len(table) == len(set(table)), f"{name} has a duplicate field name"


def test_shorter_than_documented_cst_is_tolerated_not_an_error():
    # The fixture's CST; line is lifted verbatim from the Comms & Config
    # Guide's own older-firmware example (p.57) -- 21 tokens vs. the
    # current 28-field table. Missing trailing fields read as "" rather
    # than raising or misaligning the fields that ARE present.
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    assert cap.cst.fields["battery_id"] == "1"  # first field: correct regardless
    assert cap.cst.fields["can_tx_err"] == ""  # never arrived in this capture
    assert cap.cst.extra_fields == []  # short, not long -- no extras here


def test_extra_tokens_beyond_the_field_table_are_captured_positionally():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    assert cap.sst.extra_fields == ["99"]
    assert cap.cpe[3].extra_fields == ["77"]
    # A profile with no extra token has an empty list, not a missing key.
    assert cap.cpe[1].extra_fields == []


def test_wholly_unrecognised_reply_tag_is_recorded():
    cap = parse_capture_file(FIXTURES / "synthetic_48v_battleborn.txt")
    assert cap.unrecognised_lines == ["XYZ;,1,2,3"]


def test_ast_and_aok_lines_are_silently_ignored_not_unrecognised():
    text = "AST;,1.23,,54.4,25.3\nAOK;\nNAK;\n"
    cap = parse_capture_text(text)
    assert cap.unrecognised_lines == []
    assert cap.sst is None and cap.scv is None


def test_sparse_capture_leaves_unseen_sections_none():
    cap = parse_capture_file(FIXTURES / "synthetic_minimal_sparse.txt")
    assert cap.sst is not None
    assert cap.scv is not None
    assert cap.npc is None
    assert cap.cst is None
    assert cap.cpe == {}


def test_blank_and_noise_lines_do_not_crash_the_parser():
    text = "\n\n   \nnot a status line at all\n$RAS:@\nSST;,AREG2.6.1\n"
    cap = parse_capture_text(text)
    assert cap.sst is not None
    assert cap.sst.fields["version"] == "AREG2.6.1"
