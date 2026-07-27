"""
fixtures.py -- reference documents for the mock firmware (GH#17 test support).

`default_cfg_doc()` mirrors, field for field, the PROFILE_SPEC Sec 7 document
control/Src/config_doc.c's cfg_doc_emit() writes for the native test baseline
(control/test/test_config.h cfg_test_baseline(): 16S/300 Ah/8000 W bank). It is
not generated from the C source -- there is no build step wiring the two
together -- so if config_doc.c's emitted shape changes, this fixture needs a
matching hand-edit. Keeping it byte-for-shape faithful (primitive refs as
strings, -1/NaN as null, the full profile block including "reserved") is what
lets tests/test_proto.py exercise a get -> edit -> set round trip that looks
like the real wire, not a simplified stand-in.

`default_telem_line()` mirrors control/Inc/telemetry_json.h's line schema.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import copy

CFGT_CELLS = 16
CFGT_BANK_AH = 300.0
CFGT_MAX_W = 8000.0
CFGT_P_TAIL_W = 0.04 * CFGT_BANK_AH * CFGT_CELLS * 3.45  # 662.4, test_config.h

_AH_REVERT = 0.30 * CFGT_BANK_AH  # 90.0, cfgt_profile()'s fixed 0.30C default

_PROFILE_NAMES = [
    "Bulk, Float Norm",
    "Bulk, Float Low (Solar Priority)",
    "Bulk, Solar Finish",
    "Bulk Conservative, Float Low",
    "Bulk, Off",
    "Limp Home",
    "Fast Charge",
]


def _profile(
    pid: int,
    cv_target,
    exit_at_cv_entry: bool,
    rest_mode: str,
    rest_voltage,
    rest_power_cap_w: float,
    v_cell: float,
    hold_s: int,
    soc_pct,
    reserved: bool = False,
) -> dict:
    return {
        "id": pid,
        "name": _PROFILE_NAMES[pid - 1],
        "reserved": reserved,
        "cv_target": cv_target,
        "exit_at_cv_entry": exit_at_cv_entry,
        "rest": {
            "mode": rest_mode,
            "voltage": rest_voltage,
            "power_cap_w": rest_power_cap_w,
        },
        "revert": {
            "v_cell": v_cell,
            "hold_s": hold_s,
            "soc_pct": soc_pct,
            "ah": _AH_REVERT,
        },
    }


_BASELINE_PROFILES = [
    _profile(1, "v_bulk", False, "hold", "v_float", 1.00 * CFGT_MAX_W, 3.28, 30, 50),
    _profile(2, "v_bulk", False, "hold", "v_float_cons", 0.25 * CFGT_MAX_W, 3.22, 60, 40),
    _profile(3, "v_bulk", True, "zero", 0.0, 0.0, 3.25, 60, 50),
    _profile(4, "v_bulk_cons", False, "hold", "v_float_cons", 0.50 * CFGT_MAX_W, 3.25, 30, 50),
    _profile(5, "v_bulk", False, "zero", 0.0, 0.0, 3.25, 30, 50),
    _profile(6, "v_limp", False, "hold", "v_limp", 0.25 * CFGT_MAX_W, 0.0, 0, None),
    _profile(7, 0.0, False, "zero", 0.0, 0.0, 0.0, 0, None, reserved=True),
]


def default_cfg_doc(schema_version: int = 1, fw: str = "0.1.0-dev") -> dict:
    """A fresh copy every call -- tests mutate what they get back, and a
    shared mutable module-level dict would leak state between them."""
    doc = {
        "schema_version": schema_version,
        "fw": fw,
        "primitives": {
            "v_bulk": 3.60,
            "v_bulk_cons": 3.50,
            "v_float": 3.40,
            "v_float_cons": 3.35,
            "v_limp": 3.30,
        },
        "limits": {
            "battery_c_limit": 0.5,
            "wiring_limit_a": 250.0,
            "alternator_limit_a": 220.0,
            "field": {
                "rotor_rated_v": 12.0,
                "rotor_v_max": None,  # NAN -> null (globals.rotor_v_max, "use rated")
                "allow_full_field_48v": False,
            },
        },
        "global": {
            "cells_series": CFGT_CELLS,
            "bank_capacity_ah": CFGT_BANK_AH,
            "max_charge_power_w": CFGT_MAX_W,
            "ramp_w_per_s": 200.0,
            "p_tail_w": CFGT_P_TAIL_W,
            "t_tail_hold_s": 60,
            "t_vclamp_s": 5,
            "cv_hold_exit_min": 15,
            "t_charge_max_min": 480,
            "warmup_time_s": 30,
            "warmup_coolant_c": None,  # NAN -> null, "off"
            "soc_target_pct": None,  # -1 -> null, "off"
            "skip_bulk_vcell": 0.0,
            "skip_bulk_soc_pct": None,  # -1 -> null, "off"
            "limp_vcell": 3.30,
            "limp_power_cap_w": 0.25 * CFGT_MAX_W,
        },
        "profiles": copy.deepcopy(_BASELINE_PROFILES),
        "active_profile": 1,
    }
    return doc


def default_telem_line(**overrides) -> dict:
    """One telemetry_json.h snapshot. Field names/nesting match the header's
    documented LINE SCHEMA; values are plausible but arbitrary."""
    t = {
        "t": "telem",
        "up_ms": 123456,
        "state": 2,
        "standby": 0,
        "profile": 1,
        "field": 0.42,
        "bind": 1,
        "bind_w": 1500.0,
        "faults": 0,
        "sev": 0,
        "cells": CFGT_CELLS,
        "vbat": 54.4,
        "vcell": 3.40,
        "amps": 25.3,
        "watts": 1377.0,
        "alt_c": 65.0,
        "batt_c": 28.0,
        "rpm": 1800.0,
        "rpm_st": 1,
        "diag": {
            "resets": [1, 0, 0, 0, 0, 0, 0],
            "cause": 1,
            "boots": 5,
            "crashes": 0,
            "crash_kind": 0,
            "crash_pc": 0,
            "crc": "ok",
            "stack_used": 2048,
            "stack_free": 6144,
            "errb": 0,
            "errb_reinit": [0, 0, 0],
            "safe": 0,
        },
    }
    t.update(overrides)
    return t
