"""
report.py -- renders an ImportReport (mapper.py) as a human-readable text
summary: mapped / converted / dropped / unrecognised, per the task brief's
"emit a human-readable summary alongside the JSON" requirement. This is the
document a human reads before ever running `ws500ctl set` with the proposed
JSON -- Decision #6a is explicit the import is "human-reviewed, never
automatic."

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

from .mapper import ImportReport, MappingEntry

_SECTION_BLURB = {
    "mapped": "Copied as-is -- same units, same meaning in both schemas.",
    "converted": "Value transformed (unit/normalization math shown below) -- "
    "review the arithmetic, not just the result.",
    "dropped": "No OpenFW equivalent, explicitly -- nothing was silently "
    "discarded; every line below says why.",
    "unrecognised": "Did not match this importer's known field tables at all "
    "-- this is how a manual gap or a newer-firmware field gets found. "
    "Worth a look before trusting the rest of the parse.",
}


def _render_section(title: str, entries: list[MappingEntry]) -> list[str]:
    lines = [f"## {title.upper()} ({len(entries)})", "", _SECTION_BLURB[title], ""]
    if not entries:
        lines.append("  (none)")
    for e in entries:
        lines.append(f"- {e.stock_field}")
        if e.stock_value is not None:
            lines.append(f"    stock value : {e.stock_value}")
        if e.native_key is not None:
            lines.append(f"    -> {e.native_key} = {e.native_value!r}")
        lines.append(f"    {e.note}")
    lines.append("")
    return lines


def render_text(report: ImportReport, *, source_path: str) -> str:
    """Full report, in the order a reviewer should read it: converted first
    (needs the closest look -- math to verify), mapped second (should be
    boring), dropped third (the bulk, but each line is self-explanatory),
    unrecognised last (the interesting tail -- see this module's docstring
    and the "unrecognised" blurb above)."""
    total = len(report.mapped) + len(report.converted) + len(report.dropped) + len(
        report.unrecognised
    )
    lines = [
        "WS500-OpenFW stock-config import report",
        f"source: {source_path}",
        "",
        "SYNTHETIC-FIXTURE WARNING: if this report was produced from a "
        "hand-written test fixture rather than a real Stage-A capture, "
        "treat every line below as illustrative only -- see "
        "tools/stock_config_import/tests/fixtures/README.md.",
        "",
        f"{total} stock field(s) examined: "
        f"{len(report.converted)} converted, {len(report.mapped)} mapped, "
        f"{len(report.dropped)} dropped, {len(report.unrecognised)} unrecognised.",
        "",
        "Nothing in this report has been applied to any device. The "
        "accompanying JSON file is a proposal to review by hand, then merge "
        "(diff-reviewed) into a real config document before `ws500ctl set` "
        "-- see docs/DECISION_6A_CONFIG_STRATEGY.md ('human-reviewed, never "
        "automatic').",
        "",
    ]
    lines += _render_section("converted", report.converted)
    lines += _render_section("mapped", report.mapped)
    lines += _render_section("dropped", report.dropped)
    lines += _render_section("unrecognised", report.unrecognised)
    return "\n".join(lines)
