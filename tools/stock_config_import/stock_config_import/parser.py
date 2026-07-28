"""
parser.py -- parses an archived stock `$RAS:`/`$RCP:n` capture (the Stage-A
artifact, docs/STAGE_A_RUNSHEET.md Procedure 1) into structured records.

PURE text parsing: this module reads a FILE, it never opens a serial port
and never sends anything to a device -- Decision #6a
(docs/DECISION_6A_CONFIG_STRATEGY.md) is explicit that the client-side
translator "reads a file," full stop.

Wire format reference: `docs/Wakespeed-Communications-and-Configuration-
Guide-v2.6.1-1.pdf` ("Receiving data FROM the regulator", p.25-43),
Appendix D (p.115, `CPE`/Charge Profile Entry field layout). That PDF is a
gitignored reference doc (PROJECT_PLAN.md Sec0), so the field-position
tables below are the parser's own record of what each reply line's comma
-separated tokens mean; there is no machine-readable schema to import them
from.

Field-count drift is expected, not a bug: the Comms & Config Guide's own
revision notes (page 3, "2.6.1 Notes") show the CST; and SST; layouts have
grown fields release over release (e.g. "CAN Alternator Data Instance"
added after "BitRate" for CAN-based current sensing). A raw capture from
older or newer firmware than the schema tables below WILL have a different
token count than what's here. This parser is deliberately tolerant of that:
- fewer tokens than expected -> the missing trailing fields are recorded as
  "not present in this capture" (older firmware / feature never enabled),
  not an error.
- more tokens than expected -> the extra trailing tokens are kept as
  `extra_fields`, positionally labelled but unnamed, and mapper.py routes
  them to the "unrecognised" report bucket. This is deliberate: an
  unexpectedly long reply line is exactly how a future firmware revision
  that has grown a field this parser doesn't know about yet would surface
  -- see the STAGE_A_RUNSHEET.md task brief's "the unrecognised bucket is
  the interesting one."

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import re
from dataclasses import dataclass, field
from pathlib import Path
from typing import Optional

# ---------------------------------------------------------------------------
# Field-position tables. Every list below is `field name` in the order the
# Comms & Config Guide documents the tokens AFTER the leading "TAG;" token
# (e.g. "SST;"), spacer/blank fields included by name so positions line up
# with the manual's own field-numbering. A spacer field's value is always
# empty in the wire format; it is kept in the table (rather than skipped)
# so `zip(FIELDS, tokens)` stays positionally correct even when a record is
# shorter than the table -- see the module docstring on field-count drift.
# ---------------------------------------------------------------------------

# Comms & Config Guide p.41-42.
SST_FIELDS = [
    "version",
    "_spacer1",
    "derate_mode",
    "system_options",
    "_spacer2",
    "cp_index",
    "bc_mult",
    "sys_volts",
    "_spacer3",
    "alt_cap",
    "cap_rpms",
    "_spacer4",
    "ahs",
    "whs",
    "_spacer5",
    "forced_tm",
    "required_sensor_flag",
    "_spacer6",
    "ble_readonly_state",
]

# Comms & Config Guide p.39-40.
SCV_FIELDS = [
    "lockout",
    "bts2ats",
    "rev_amp",
    "sv_ovr",
    "bc_ovr",
    "cp_ovr",
    "_spacer1",
    "alt_temp_set",
    "drt_norm",
    "drt_small",
    "drt_half",
    "pbf",
    "_spacer2",
    "amp_limit",
    "watt_limit",
    "_spacer3",
    "alt_poles",
    "drive_ratio",
    "shunt_ratio",
    "_spacer4",
    "idle_rpm",
    "tach_min_field",
    "warmup_delay",
    "required_sensor",
    "dc_disconnected_vbat",
    "feature_in_mod",
    "trigger_half_power_rpm",
    "ignore_sensor",
    "feature_out_mod",
    "_spacer5",
    "bms_amp_cap",
    "promiscuous_mode",
]

# Comms & Config Guide p.38. EnableBLE may be blank on plain (non-Pro) WS500.
NPC_FIELDS = ["enable_ble", "name", "password", "_spacer1", "device_id", "dom"]

# Comms & Config Guide p.32-34.
CST_FIELDS = [
    "battery_id",
    "id_override",
    "instance",
    "priority",
    "_spacer1",
    "enable_nmea2000",
    "enable_ose",
    "_spacer2",
    "allow_rbm",
    "is_rbm",
    "shunt_at_bat",
    "_spacer3",
    "rbm_id",
    "ignoring_rbm",
    "enable_alt_can",
    "_spacer4",
    "can_id",
    "_spacer5",
    "engine_id",
    "bit_rate",
    "aggregate_bms",
    "can_alternator_data_instance",
    "_spacer6",
    "enable_dvcc",
    "dvcc_active",
    "_spacer7",
    "can_tx_err",
    "can_rx_err",
]

# Comms & Config Guide Appendix D (p.115-117) + the CPE; reply layout on
# p.36-37 -- both describe the same 34-value record; Appendix D's struct
# comments are what name each one here.
CPE_FIELDS = [
    "n",
    "acpt_vbat",
    "acpt_time",
    "acpt_exit",
    "res1",
    "_spacer1",
    "oc_amps",
    "oc_time",
    "oc_vbat",
    "oc_aexit",
    "_spacer2",
    "float_vbat",
    "float_amps",
    "float_time",
    "float_resume_a",
    "float_resume_ah",
    "float_resume_v",
    "_spacer3",
    "pf_time",
    "pf_resume",
    "pf_resume_ah",
    "_spacer4",
    "equal_vbat",
    "equal_amps",
    "equal_time",
    "equal_exit",
    "_spacer5",
    "bat_comp",
    "comp_min",
    "min_charge",
    "max_charge",
    "_spacer6",
    "rdc_volts",
    "rdc_low_temp",
    "rdc_high_temp",
    "rdc_amps",
    "_spacer7",
    "float_soc",
    "_spacer8",
    "max_amps",
    "_spacer9",
    "pf_vbat",
    "max_bat_volts",
]

# Record types this parser recognises by their leading "TAG;" token. DST;/
# DCV;/ENG; are parsed generically (see _parse_generic) because every one of
# their fields is dropped by mapper.py anyway (DC-DC converter config /
# engine white-space config -- not part of the two-stage LFP schema, and
# this WS500 install has no DC-DC converter per WS500_HARDWARE_SPEC.md); a
# named field table for them would be dead weight.
KNOWN_PREFIXES = ("SST;", "SCV;", "NPC;", "CST;", "CPE;", "DST;", "DCV;", "ENG;")

# Telemetry/protocol-framing tags this parser deliberately ignores (not
# config, not an error) -- see the elif chain in parse_capture_text().
_IGNORED_PREFIXES = ("AST;", "AOK;", "NAK;", "FLT;", "DBG;", "RST;")

# A line that LOOKS like a reply ("TAG;" at the start, tag = 2-6 caps) but
# whose tag isn't in KNOWN_PREFIXES or _IGNORED_PREFIXES is exactly how a
# firmware revision newer than the Comms & Config Guide this parser was
# written against would surface a reply type we've never seen -- see
# `unrecognised_lines` and the module docstring's "field-count drift" note.
_STATUS_LINE_RE = re.compile(r"^[A-Z]{2,6};")


@dataclass
class Record:
    """One parsed reply line: `tag` (e.g. "SST"), `fields` (name -> raw
    string value, spacers included as empty strings), and `extra_fields`
    (positional list of tokens beyond the known table's length -- see the
    module docstring's "field-count drift" note)."""

    tag: str
    fields: dict[str, str]
    extra_fields: list[str] = field(default_factory=list)
    raw_line: str = ""


@dataclass
class StockCapture:
    """Everything this parser found in one capture file."""

    sst: Optional[Record] = None
    scv: Optional[Record] = None
    npc: Optional[Record] = None
    cst: Optional[Record] = None
    cpe: dict[int, Record] = field(default_factory=dict)  # keyed by profile n (0..8)
    dst: Optional[Record] = None
    dcv: Optional[Record] = None
    eng: Optional[Record] = None
    unrecognised_lines: list[str] = field(default_factory=list)  # unknown "TAG;" entirely


def _split_tokens(line: str) -> list[str]:
    """Split a reply line on commas, dropping the leading "TAG;" token and
    stripping whitespace from every field -- the Comms & Config Guide's own
    format note (p.26): "double commas (separated by a space) between major
    'sections', this is to simplify manual reading of the strings."""
    parts = line.split(",")
    return [p.strip() for p in parts[1:]]  # parts[0] is "TAG;"


def _parse_named(tag: str, line: str, field_names: list[str]) -> Record:
    tokens = _split_tokens(line)
    fields: dict[str, str] = {}
    for i, name in enumerate(field_names):
        fields[name] = tokens[i] if i < len(tokens) else ""
    extra = tokens[len(field_names):]
    return Record(tag=tag, fields=fields, extra_fields=extra, raw_line=line)


def _parse_generic(tag: str, line: str) -> Record:
    """DST;/DCV;/ENG; -- keep every token, but only as an ordinal list (see
    KNOWN_PREFIXES docstring for why no named table exists for these)."""
    tokens = _split_tokens(line)
    fields = {f"field_{i}": v for i, v in enumerate(tokens)}
    return Record(tag=tag, fields=fields, raw_line=line)


def parse_capture_text(text: str) -> StockCapture:
    """Parse the full text of a Stage-A capture file (raw or redacted --
    this parser reads whatever the archived file contains; it doesn't care
    which stage_a_capture output it was, and it never needs the password
    field, so a redacted capture parses identically to a raw one). Unknown
    text (boot banners, `AST;` telemetry lines, blank lines, partial
    fragments) is skipped, not fatal -- a Stage-A raw log is a continuous
    capture "from connection onward" per the runsheet, so it is expected to
    contain plenty of lines this importer has no use for.
    """
    capture = StockCapture()
    for raw_line in text.splitlines():
        line = raw_line.strip()
        if not line:
            continue

        if line.startswith("SST;"):
            capture.sst = _parse_named("SST", line, SST_FIELDS)
        elif line.startswith("SCV;"):
            capture.scv = _parse_named("SCV", line, SCV_FIELDS)
        elif line.startswith("NPC;"):
            capture.npc = _parse_named("NPC", line, NPC_FIELDS)
        elif line.startswith("CST;"):
            capture.cst = _parse_named("CST", line, CST_FIELDS)
        elif line.startswith("CPE;"):
            rec = _parse_named("CPE", line, CPE_FIELDS)
            try:
                n = int(rec.fields.get("n", ""))
            except ValueError:
                capture.unrecognised_lines.append(line)
                continue
            capture.cpe[n] = rec
        elif line.startswith("DST;"):
            capture.dst = _parse_generic("DST", line)
        elif line.startswith("DCV;"):
            capture.dcv = _parse_generic("DCV", line)
        elif line.startswith("ENG;"):
            capture.eng = _parse_generic("ENG", line)
        elif line.startswith(_IGNORED_PREFIXES):
            continue  # telemetry / protocol framing, not config -- not our concern
        elif _STATUS_LINE_RE.match(line):
            # Tag-shaped ("XYZ;...") but not one of KNOWN_PREFIXES or
            # _IGNORED_PREFIXES -- a genuinely new reply type. This is
            # deliberately recorded, not skipped: see _STATUS_LINE_RE.
            capture.unrecognised_lines.append(line)
        # else: not tag-shaped at all (boot banner, partial/torn fragment,
        # general noise) -- not recorded, there is nothing structured to
        # report about it.

    return capture


def parse_capture_file(path: Path) -> StockCapture:
    """Read `path` as UTF-8 (invalid bytes replaced, matching the
    permissiveness stage_a_capture.capture and ws500ctl.proto both apply to
    a live serial link -- an archived log can carry the same line noise)
    and parse it."""
    text = path.read_text(encoding="utf-8", errors="replace")
    return parse_capture_text(text)
