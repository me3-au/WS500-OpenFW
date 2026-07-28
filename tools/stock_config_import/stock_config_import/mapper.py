"""
mapper.py -- proposes a PROFILE_SPEC Sec7 JSON overlay from a parsed
StockCapture, and produces the mapped/converted/dropped/unrecognised report
that makes the proposal auditable. Decision #6a
(docs/DECISION_6A_CONFIG_STRATEGY.md) governs every choice here: the stock
6-stage Pb charge-profile model, DIP-switch overrides, derate/small-alt/
half-power modes, and RFM/PBF white-space shaping are **deliberately not
carried forward** (PROJECT_PLAN.md "Design philosophy" -- "we deliberately
ditch the legacy surface"). This module's job is to say so explicitly for
every field it declines to map, not to silently drop them.

Native schema authority: `control/Inc/control.h` (`ctrl_globals_t`,
`ctrl_limits_t`) and `control/Inc/config_doc.h`'s documented deviations from
the PROFILE_SPEC Sec7 sketch are the ground truth for "does this field exist
in schema v2" -- not the Sec7 sketch itself, which config_doc.h's own header
comment says predates several of these fields. See PROPOSAL_NOTES in each
mapping function for the specific citation.

Every value this module writes into the proposed JSON document is either a
DIRECT copy of a stock field (native units already match) or a CONVERTED
value with the arithmetic shown in the report -- this module never invents
a number. Fields with no defensible mapping are DROPPED, always with a
reason.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

from dataclasses import dataclass, field
from typing import Any, Optional

from .parser import CPE_FIELDS, StockCapture

# BAT_AMPHR_NOMINAL (Comms & Config Guide Appendix D, p.117): every stock
# Amp/Ah value in a Charge Profile Entry is normalized against a 500 Ah
# "12v" nominal battery and scaled by the Battery Capacity Multiplier at
# runtime. Converting a CPE amp value to a real-world number means undoing
# that normalization -- see _convert_battery_c_limit().
STOCK_NOMINAL_BATTERY_AH = 500.0

# SST;'s SysVolt field (Comms & Config Guide p.42) is a multiplier against a
# 12V nominal step, not a cell count. There is no chemistry-independent way
# to go from "system is nominally 4x = 48V" to "this pack has 16 LFP cells
# in series" -- 48V packs are sold as both 15S and 16S LFP
# (Comms & Config Guide p.9, "48v vs. 52v vs. 56v"). This table is a
# heuristic (3V/cell nominal step), not a fact, and every value it produces
# is reported as CONVERTED with that caveat spelled out, never silently
# trusted.
_SYSVOLT_TO_CELLS_HEURISTIC = {1.0: 4, 2.0: 8, 4.0: 16}


@dataclass
class MappingEntry:
    """One row of the import report."""

    stock_field: str
    stock_value: Optional[str]
    native_key: Optional[str]
    native_value: Optional[Any]
    note: str


@dataclass
class ImportReport:
    mapped: list[MappingEntry] = field(default_factory=list)
    converted: list[MappingEntry] = field(default_factory=list)
    dropped: list[MappingEntry] = field(default_factory=list)
    unrecognised: list[MappingEntry] = field(default_factory=list)

    def all_entries(self) -> list[tuple[str, MappingEntry]]:
        out: list[tuple[str, MappingEntry]] = []
        for cat in ("mapped", "converted", "dropped", "unrecognised"):
            out.extend((cat, e) for e in getattr(self, cat))
        return out


def _to_float(s: Optional[str]) -> Optional[float]:
    if s is None:
        return None
    s = s.strip()
    if not s:
        return None
    try:
        return float(s)
    except ValueError:
        return None


def _set_nested(doc: dict, dotted_key: str, value: Any) -> None:
    parts = dotted_key.split(".")
    node = doc
    for p in parts[:-1]:
        node = node.setdefault(p, {})
    node[parts[-1]] = value


class Mapper:
    """Stateful only in that it accumulates the report as it walks a single
    StockCapture; construct a fresh one per capture (see translate())."""

    def __init__(self, capture: StockCapture):
        self.capture = capture
        self.report = ImportReport()

    # ---- top-level entry point ------------------------------------------

    def run(self) -> ImportReport:
        """Walk every field this parser knows about and file it into
        exactly one report bucket. Building the proposed JSON document
        itself is a separate, later step (see module-level translate()) so
        this method has one job: classify every field, faithfully."""
        self._map_scv_limits()
        self._map_sst_cells_and_capacity()
        self._map_scv_warmup()
        self._map_npc()
        self._map_cpe_all()
        self._map_scv_remaining()
        self._map_cst_all()
        self._map_generic_block(self.capture.dst, "DST", "DC-DC converter status/config")
        self._map_generic_block(self.capture.dcv, "DCV", "DC-DC converter status/config")
        self._map_generic_block(self.capture.eng, "ENG", "engine white-space (RFM) config")
        self._map_extra_fields()
        self._map_unrecognised_lines()
        return self.report

    # ---- individual mappings ----------------------------------------------

    def _map_scv_limits(self) -> None:
        """SCV;'s Amp Limit / Watt Limit are NOT normalized against the
        500Ah/12V nominal battery (Comms & Config Guide p.62: "There is no
        adjustment made to this value based on system voltage or selection
        of system battery size -- the values declared will be used
        directly"), so they copy straight across into
        ctrl_limits_t.alternator_limit_a / ctrl_globals_t.max_charge_power_w
        (control.h) -- same units (A, W), same meaning (a static rating of
        the installed system)."""
        if self.capture.scv is None:
            return
        f = self.capture.scv.fields

        amp_raw = f.get("amp_limit", "")
        amp_val = _to_float(amp_raw)
        if amp_val is not None and amp_val > 0:
            self.report.mapped.append(
                MappingEntry(
                    "SCV.amp_limit", amp_raw, "limits.alternator_limit_a", amp_val,
                    "Direct copy -- Amp Limit is not normalized to the 12V/500Ah "
                    "nominal battery (Comms & Config Guide p.62), same unit (A) as "
                    "ctrl_limits_t.alternator_limit_a.",
                )
            )
        else:
            self.report.dropped.append(
                MappingEntry(
                    "SCV.amp_limit", amp_raw, None, None,
                    "Stock value is a sentinel (-1 = auto-size from a live "
                    "self-test, 0 = unregulated) with no OpenFW equivalent -- "
                    "alternator_limit_a is a static, user-entered rating "
                    "(control.h ctrl_limits_t). Measure or look up the "
                    "alternator's real rated current and set it by hand.",
                )
            )

        watt_raw = f.get("watt_limit", "")
        watt_val = _to_float(watt_raw)
        if watt_val is not None and watt_val > 0:
            self.report.mapped.append(
                MappingEntry(
                    "SCV.watt_limit", watt_raw, "global.max_charge_power_w", watt_val,
                    "Direct copy -- System Watts Cap is not normalized to the "
                    "nominal battery either, same unit (W) as "
                    "ctrl_globals_t.max_charge_power_w.",
                )
            )
        else:
            self.report.dropped.append(
                MappingEntry(
                    "SCV.watt_limit", watt_raw, None, None,
                    "Stock value is a sentinel (-1 = auto-size, 0 = disabled / "
                    "hard-coded 15kW internal cap) with no OpenFW equivalent -- "
                    "set max_charge_power_w from the real system design "
                    "(belt/engine/wiring capacity), not this sentinel.",
                )
            )

    def _map_sst_cells_and_capacity(self) -> None:
        """SysVolt -> cells_series is a HEURISTIC (see
        _SYSVOLT_TO_CELLS_HEURISTIC's docstring), always reported as
        CONVERTED with the assumption spelled out, never as a plain mapped
        fact. BC Mult -> bank_capacity_ah is exact arithmetic (the stock
        normalization constant, Appendix D BAT_AMPHR_NOMINAL=500) and is a
        faithful conversion, not a heuristic."""
        if self.capture.sst is None:
            return
        f = self.capture.sst.fields

        sv_raw = f.get("sys_volts", "")
        sv_val = _to_float(sv_raw)
        if sv_val is not None and sv_val in _SYSVOLT_TO_CELLS_HEURISTIC:
            cells = _SYSVOLT_TO_CELLS_HEURISTIC[sv_val]
            self.report.converted.append(
                MappingEntry(
                    "SST.sys_volts", sv_raw, "global.cells_series", cells,
                    f"HEURISTIC, confirm before trusting: SysVolt={sv_raw} "
                    f"(Comms & Config Guide p.42's Nx-nominal-voltage table) "
                    f"assumed as a {cells}S LFP pack (3V/cell nominal step: "
                    f"1x=12V->4S, 2x=24V->8S, 4x=48V->16S). This install's own "
                    "48V bank is 16S (docs/PROJECT_PLAN.md installed-unit "
                    "note) -- but a 15S 48V-nominal pack is equally common "
                    "(Comms & Config Guide p.9), so verify the real series-cell "
                    "count before accepting this value.",
                )
            )
        else:
            self.report.dropped.append(
                MappingEntry(
                    "SST.sys_volts", sv_raw, None, None,
                    f"No standard LFP cell-count heuristic for a "
                    f"SysVolt multiplier of {sv_raw!r} (only 1x/2x/4x, i.e. "
                    "12V/24V/48V, have one here) -- e.g. a fractional 32V "
                    "(2.67x) or 42V (3.5x) system has no clean series-cell "
                    "mapping. Set cells_series directly from the battery's "
                    "own datasheet.",
                )
            )

        bc_raw = f.get("bc_mult", "")
        bc_val = _to_float(bc_raw)
        if bc_val is not None and bc_val != 0:
            ah = abs(bc_val) * STOCK_NOMINAL_BATTERY_AH
            self.report.converted.append(
                MappingEntry(
                    "SST.bc_mult", bc_raw, "global.bank_capacity_ah", ah,
                    f"bank_capacity_ah = |BC Mult| x {STOCK_NOMINAL_BATTERY_AH:g} Ah "
                    f"(Appendix D BAT_AMPHR_NOMINAL) = |{bc_raw}| x "
                    f"{STOCK_NOMINAL_BATTERY_AH:g} = {ah:g} Ah. The sign of BC "
                    "Mult (negative = 'don't let CAN override this') has no "
                    "OpenFW equivalent and is discarded; only the magnitude "
                    "is used.",
                )
            )
        else:
            self.report.dropped.append(
                MappingEntry(
                    "SST.bc_mult", bc_raw, None, None,
                    "BC Mult is 0 or unparseable -- no capacity was actually "
                    "configured on the stock unit (DIP-switch default). Set "
                    "bank_capacity_ah from the battery's own nameplate Ah "
                    "rating instead.",
                )
            )

    def _map_scv_warmup(self) -> None:
        """Warmup Delay (seconds, hold-off before RAMP) maps in unit and
        intent to ctrl_globals_t.warmup_time_s (seconds, "hold-off period
        when regulator is 1st powered on" -- control.h). The stock field's
        SIGN selects a separate behavior (negative = disable 'Fast Ramp',
        Comms & Config Guide p.64) that has no OpenFW equivalent; only the
        magnitude converts."""
        if self.capture.scv is None:
            return
        raw = self.capture.scv.fields.get("warmup_delay", "")
        val = _to_float(raw)
        if val is not None and val != 0:
            self.report.converted.append(
                MappingEntry(
                    "SCV.warmup_delay", raw, "global.warmup_time_s", abs(val),
                    f"warmup_time_s = |Warmup Delay| = |{raw}| = {abs(val):g} s. "
                    "The sign (negative = disable the stock 'Fast Ramp' "
                    "feature, Comms & Config Guide p.64) is discarded -- "
                    "OpenFW's ramp_w_per_s is the only ramp-rate control and "
                    "has no fast/slow toggle to carry it to.",
                )
            )
        else:
            self.report.dropped.append(
                MappingEntry(
                    "SCV.warmup_delay", raw, None, None,
                    "Value is 0 or unparseable -- nothing to convert.",
                )
            )

    def _map_npc(self) -> None:
        if self.capture.npc is None:
            return
        f = self.capture.npc.fields
        # SECURITY: never let the plaintext password reach the report or
        # the proposed document, even if the capture file handed to this
        # tool is the *raw*, unredacted stage_a_capture output. Redact
        # unconditionally, the same way stage_a_capture.capture.redact_line
        # does for its own output files.
        self.report.dropped.append(
            MappingEntry(
                "NPC.password", "[REDACTED-BY-stock_config_import]", None, None,
                "Never carried into the proposal, regardless of whether the "
                "captured value was plaintext or already hidden ('.'-prefixed) "
                "-- there is no config field for a device password in the "
                "PROFILE_SPEC Sec7 schema, and even if there were, this tool "
                "must not be the thing that echoes a credential into a "
                "reviewed-and-possibly-committed JSON file.",
            )
        )
        for key, label, why in (
            ("name", "NPC.name", "regulator display name"),
            ("device_id", "NPC.device_id", "semi-unique hardware device ID"),
            ("dom", "NPC.dom", "manufacture date (WS500 Pro only)"),
        ):
            if f.get(key, ""):
                self.report.dropped.append(
                    MappingEntry(
                        label, f[key], None, None,
                        f"No schema v2 field for a {why} -- "
                        "config_doc.h's PROFILE_SPEC deviations list (the "
                        "authoritative record of what schema v1/v2 actually "
                        "store) has no device-identity block.",
                    )
                )
        if f.get("enable_ble", ""):
            self.report.dropped.append(
                MappingEntry(
                    "NPC.enable_ble", f["enable_ble"], None, None,
                    "WS500 Pro-only Bluetooth toggle; docs/CLIENT_CONNECTIVITY.md "
                    "v1 scope is USB CDC + CAN only, no wireless -- not "
                    "applicable to this install (§0.6 hardware facts) or to "
                    "the v1 schema.",
                )
            )

    def _map_cpe_all(self) -> None:
        """The 6-stage Pb charge-profile model (Bulk -> Acceptance ->
        Overcharge -> Float -> Post-Float, plus a separate Equalize mode)
        has no equivalent in the two-stage CHARGE/REST LFP profile engine
        (docs/PROFILE_SPEC_LFP.md; PROJECT_PLAN.md "Design philosophy":
        "we deliberately ditch the legacy surface (6-stage Pb machine,
        absorption stage ... ) and adopt smarter methods (two-stage
        CHARGE/REST ...)"). Every per-stage voltage/current/time setpoint in
        every captured CPE is dropped for that reason -- ONE exception:
        MaxAmps, which is genuinely just a C-rate cap normalized to the
        500Ah nominal battery, and DOES have a faithful OpenFW target
        (ctrl_limits_t.battery_c_limit). That one field, from whichever CPE
        the capture shows as currently active, is converted separately
        below; every other field of every CPE (including that same active
        one) is dropped here.
        """
        if not self.capture.cpe:
            return

        active_n = self._active_cpe_index()
        active_rec = self.capture.cpe.get(active_n) if active_n is not None else None
        if active_rec is None and 0 in self.capture.cpe:
            active_rec = self.capture.cpe[0]  # $RCP:0's "currently selected" snapshot
            active_n = 0

        if active_rec is not None:
            self._convert_battery_c_limit(active_rec, active_n)
        else:
            self.report.dropped.append(
                MappingEntry(
                    "CPE.max_amps (active profile)", None, None, None,
                    "Could not identify which CPE was active in this capture "
                    "(no SST.cp_index and no $RCP:0 entry present) -- "
                    "battery_c_limit was not proposed; set it directly from "
                    "the battery's C-rate spec.",
                )
            )

        named_fields = [k for k in CPE_FIELDS if k != "n" and not k.startswith("_spacer")]
        for n in sorted(self.capture.cpe):
            rec = self.capture.cpe[n]
            drop_fields = [k for k in named_fields if k in rec.fields]
            if n == active_n:
                drop_fields = [k for k in drop_fields if k != "max_amps"]
            if not drop_fields:
                continue
            self.report.dropped.append(
                MappingEntry(
                    f"CPE[{n}]", None, None, None,
                    f"{len(drop_fields)} field(s) dropped -- 6-stage Pb "
                    "charge-profile parameters, no LFP two-stage equivalent "
                    f"(see this function's docstring): {', '.join(drop_fields)}.",
                )
            )

    def _active_cpe_index(self) -> Optional[int]:
        if self.capture.sst is None:
            return None
        raw = self.capture.sst.fields.get("cp_index", "")
        try:
            return int(float(raw))
        except ValueError:
            return None

    def _convert_battery_c_limit(self, rec, n) -> None:
        raw = rec.fields.get("max_amps", "")
        val = _to_float(raw)
        if val is None or val <= 0:
            self.report.dropped.append(
                MappingEntry(
                    f"CPE[{n}].max_amps", raw, None, None,
                    "0 (stock default: disabled) or unparseable -- no cap was "
                    "actually configured on the stock unit, nothing to "
                    "convert. Set battery_c_limit from the battery's own "
                    "max-charge-C-rate spec.",
                )
            )
            return
        c_rate = val / STOCK_NOMINAL_BATTERY_AH
        self.report.converted.append(
            MappingEntry(
                f"CPE[{n}].max_amps", raw, "limits.battery_c_limit", c_rate,
                f"MaxAmps is normalized to the nominal 500Ah battery (Appendix "
                f"D): battery_c_limit = MaxAmps / {STOCK_NOMINAL_BATTERY_AH:g} "
                f"= {raw} / {STOCK_NOMINAL_BATTERY_AH:g} = {c_rate:g} C. Taken "
                f"from CPE #{n}, the profile this capture shows as active.",
            )
        )

    _SCV_DROP_REASONS = {
        "bts2ats": "2nd-alternator-temp-sensor repurposing -- single-alternator "
        "install here (PROJECT_PLAN.md §0.6), and no schema v2 field exists for it.",
        "rev_amp": "shunt polarity correction is an INA226 driver-level "
        "calibration constant (V3/V7), not a JSON profile field.",
        "sv_ovr": "system-voltage auto-detect/override has no schema field -- "
        "OpenFW has no DIP-switch equivalent to override in the first place.",
        "bc_ovr": "DIP-switch battery-capacity override -- OpenFW has no DIP "
        "switches (PROJECT_PLAN.md §0.5); bank_capacity_ah is set directly.",
        "cp_ovr": "DIP-switch charge-profile override -- same reason as bc_ovr.",
        "alt_temp_set": "alternator thermal governor setpoint is not yet a "
        "PROFILE_SPEC Sec7 field in schema v2 (config_doc.h has no alt-temp-"
        "target key) -- SPEC-GAP, not carried over.",
        "drt_norm": "Normal-mode de-rate fraction -- PROJECT_PLAN.md design "
        "philosophy deliberately drops derate-mode logic entirely.",
        "drt_small": "Small-Alt-Mode de-rate -- 'small-alt/half modes' named "
        "explicitly as dropped-by-design (PROJECT_PLAN.md Design philosophy).",
        "drt_half": "Half-Power-Mode de-rate -- same dropped-by-design reason.",
        "pbf": "Pull-Back Factor (idle field-drive capping) -- RFM/PBF named "
        "explicitly as dropped-by-design (PROJECT_PLAN.md Design philosophy); "
        "the manual itself (p.61) recommends White Space (RFM) over PBF, and "
        "RFM is dropped too (see ENG; below).",
        "alt_poles": "stator poles is stator_rpm.c bench-config, not a JSON "
        "profile field (docs/STAGE_A_RUNSHEET.md Procedure 4 territory).",
        "drive_ratio": "engine/alternator pulley ratio -- same as alt_poles.",
        "shunt_ratio": "INA226 shunt calibration is a driver-level constant "
        "(hardware fact, V3), not a user profile field.",
        "idle_rpm": "idle-RPM field-drive-capping basis -- PBF-family feature, "
        "dropped by design (see pbf above).",
        "tach_min_field": "Tach-Mode minimum field-drive floor -- Tach Mode "
        "itself has no OpenFW equivalent.",
        "required_sensor": "stock 'which sensors are critical' bitmask -- "
        "OpenFW's fault ladder handles missing sensors in firmware, not as a "
        "configurable bitmask (see docs/CONTROL_SPEC_NEXTGEN.md §7/§9).",
        "dc_disconnected_vbat": "DC-Disconnect CV fallback voltage -- OpenFW's "
        "BMS-disconnect handling is fixed fault-ladder behavior, not a "
        "configurable target voltage yet (SPEC-GAP).",
        "feature_in_mod": "Feature-IN port repurposing -- IO_COVERAGE.md still "
        "has this pin's role as a bench-open item, and even once resolved this "
        "is a wiring/IO config, not a charge-profile field.",
        "trigger_half_power_rpm": "RPM-triggered Half-Power -- Half-Power mode "
        "is dropped by design (see drt_half above).",
        "ignore_sensor": "stock 'which sensors to ignore' bitmask -- see "
        "required_sensor above.",
        "feature_out_mod": "Feature-OUT port repurposing -- IO config, not a "
        "profile field; see feature_in_mod above.",
        "bms_amp_cap": "static BMS current cap set by hand -- OpenFW instead "
        "takes bms_ccl_w live from the BMS each cycle (ctrl_ceilings_t, "
        "CONTROL_SPEC §6.3), so a stale, separately-configured cap would be "
        "the wrong model, not merely an unmapped field.",
        "promiscuous_mode": "auto-restart-on-hard-fault -- deliberately absent: "
        "OpenFW's fail-safe posture treats a hard fault as needing operator "
        "attention, not silent self-clearing.",
        "lockout": "stock password-gated config lockout levels -- Decision #6a "
        "means the firmware never speaks the $-protocol at all, so there is "
        "nothing here to lock out.",
    }

    def _map_scv_remaining(self) -> None:
        if self.capture.scv is None:
            return
        f = self.capture.scv.fields
        already_handled = {
            "amp_limit", "watt_limit", "warmup_delay",
        }
        for key, reason in self._SCV_DROP_REASONS.items():
            if key in already_handled or key not in f:
                continue
            self.report.dropped.append(
                MappingEntry(f"SCV.{key}", f[key], None, None, reason)
            )

    def _map_cst_all(self) -> None:
        if self.capture.cst is None:
            return
        f = self.capture.cst.fields
        reason = (
            "CAN bus identity/config (battery/charger instance, priority, "
            "RBM role, alt-CAN protocol selection, bit rate, DVCC, error "
            "counters) is not part of the PROFILE_SPEC Sec7 charge-profile "
            "schema -- OpenFW's own CAN Tx identity is a fixed part of the "
            "firmware build (docs/CAN_INTEGRATION.md §2), not a per-install "
            "JSON field in schema v1/v2 yet."
        )
        for key, val in f.items():
            if key.startswith("_spacer") or not val:
                continue
            self.report.dropped.append(MappingEntry(f"CST.{key}", val, None, None, reason))

    def _map_generic_block(self, rec, tag: str, what: str) -> None:
        if rec is None:
            return
        for key, val in rec.fields.items():
            if not val:
                continue
            self.report.dropped.append(
                MappingEntry(
                    f"{tag}.{key}", val, None, None,
                    f"{what} -- not applicable to this install "
                    "(WS500_HARDWARE_SPEC.md: no DC-DC converter hardware) "
                    "and/or not part of the PROFILE_SPEC Sec7 schema.",
                )
            )

    def _map_extra_fields(self) -> None:
        """Tokens beyond a record's documented field-position table -- see
        parser.py's module docstring on field-count drift. This is the
        parser-level half of "the unrecognised bucket is the interesting
        one": a newer firmware revision growing a field this tool's tables
        don't know about surfaces here, positionally, not silently merged
        into the wrong named field."""
        for label, rec in (
            ("SST", self.capture.sst),
            ("SCV", self.capture.scv),
            ("NPC", self.capture.npc),
            ("CST", self.capture.cst),
        ):
            if rec is None:
                continue
            for i, val in enumerate(rec.extra_fields):
                self.report.unrecognised.append(
                    MappingEntry(
                        f"{label}.extra[{i}]", val, None, None,
                        "Token beyond this parser's documented field count for "
                        f"{label};. Either a newer firmware revision than "
                        "Comms & Config Guide v2.6.1 has grown a field here, "
                        "or the capture is malformed -- worth a look before "
                        "trusting the rest of this record's parse.",
                    )
                )
        for n, rec in self.capture.cpe.items():
            for i, val in enumerate(rec.extra_fields):
                self.report.unrecognised.append(
                    MappingEntry(
                        f"CPE[{n}].extra[{i}]", val, None, None,
                        "Token beyond CPE;'s documented 34-field layout "
                        "(Appendix D) -- see the note above.",
                    )
                )

    def _map_unrecognised_lines(self) -> None:
        for line in self.capture.unrecognised_lines:
            self.report.unrecognised.append(
                MappingEntry(
                    "(unrecognised line)", line, None, None,
                    "Line did not parse as any known reply type at its "
                    "documented position, or its leading token wasn't "
                    "recognised at all -- possibly a status string this "
                    "importer's parser.py has no table for yet.",
                )
            )


def translate(capture: StockCapture) -> tuple[dict, ImportReport]:
    """Translate a parsed StockCapture into (proposed JSON overlay,
    ImportReport). The returned document deliberately has NO
    "schema_version" key: per VERSIONING.md Sec3 / ws500ctl's own `set`
    command ("absent is sent as a hand-written overlay edit"), an absent
    schema_version marks this as a partial overlay to be merged onto a
    known-good baseline, not a self-sufficient document -- and every key
    this module writes is one whose native shape it is confident about
    regardless of exactly which schema_version the connected firmware
    speaks."""
    report = Mapper(capture).run()
    doc: dict[str, Any] = {}
    for entry in (*report.mapped, *report.converted):
        if entry.native_key is not None:
            _set_nested(doc, entry.native_key, entry.native_value)
    return doc, report
