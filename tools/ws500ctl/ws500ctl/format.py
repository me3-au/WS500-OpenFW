"""
format.py -- human-readable rendering for `info` and `telem` (GH#17).

Deliberately thin: the wire's enum-valued telemetry fields (state, standby,
profile, bind, sev, rpm_st, ...) travel as the control core's own integers
(telemetry_json.h) and pretty-printing them is explicitly the client's job
(decision #6a-B's "the client owns pretty-printing" applied to telemetry, not
just config). This module prints the raw numbers rather than guessing at a
label table that would drift from control/Inc/telemetry.h -- a follow-up can
add a name table once one is needed for real use.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import json

from .proto import HelloInfo


def hello_text(info: HelloInfo) -> str:
    lines = [
        f"firmware   : {info.fw or '(unknown)'}",
        f"git        : {info.git or '(none)'}",
        f"protocol   : {info.proto}",
        f"schema     : {info.schema}",
        f"caps       : {', '.join(info.caps) if info.caps else '(none)'}",
    ]
    return "\n".join(lines)


# (wire key, column label) -- order is display order. Kept to the top-level
# scalar fields; "diag" is a nested block meant for --json, not the table.
TELEM_COLUMNS = [
    ("up_ms", "up_ms"),
    ("state", "state"),
    ("standby", "standby"),
    ("profile", "profile"),
    ("field", "field"),
    ("bind", "bind"),
    ("bind_w", "bind_w"),
    ("faults", "faults"),
    ("sev", "sev"),
    ("cells", "cells"),
    ("vbat", "vbat"),
    ("vcell", "vcell"),
    ("amps", "amps"),
    ("watts", "watts"),
    ("alt_c", "alt_c"),
    ("batt_c", "batt_c"),
    ("rpm", "rpm"),
    ("rpm_st", "rpm_st"),
]


def telem_text(t: dict) -> str:
    return "  ".join(f"{label}={t.get(key)}" for key, label in TELEM_COLUMNS)


def telem_json(t: dict) -> str:
    return json.dumps(t, separators=(",", ":"))
