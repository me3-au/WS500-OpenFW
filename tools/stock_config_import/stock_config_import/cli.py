"""
cli.py -- `python -m stock_config_import <capture-file>`. Parses an
archived Stage-A `$RAS:`/`$RCP:n` capture, proposes a PROFILE_SPEC Sec7
JSON overlay, and writes both the JSON and a human-readable report.
Never touches a serial port, never talks to a device -- see parser.py's
module docstring.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import json
import sys
from pathlib import Path
from typing import Optional

from .mapper import translate
from .parser import parse_capture_file
from .report import render_text


def _build_argparser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="stock_config_import",
        description=(
            "Translate an archived stock $RAS:/$RCP:n capture (the Stage-A "
            "artifact, docs/STAGE_A_RUNSHEET.md Procedure 1) into a proposed "
            "PROFILE_SPEC Sec7 JSON overlay, plus a mapped/converted/dropped/"
            "unrecognised report. Reads a file; never opens a serial port "
            "(docs/DECISION_6A_CONFIG_STRATEGY.md: the client app translates "
            "stock dumps, the firmware never speaks $-protocol at all)."
        ),
    )
    p.add_argument("capture", type=Path, help="path to a Stage-A raw or redacted capture log")
    p.add_argument(
        "-o", "--out-json", type=Path, default=None,
        help="where to write the proposed JSON overlay "
        "(default: <capture>.proposed.json next to the input)",
    )
    p.add_argument(
        "-r", "--out-report", type=Path, default=None,
        help="where to write the human-readable report "
        "(default: <capture>.report.txt next to the input)",
    )
    return p


def main(argv: Optional[list[str]] = None) -> int:
    args = _build_argparser().parse_args(argv)

    if not args.capture.is_file():
        print(f"stock_config_import: no such file: {args.capture}", file=sys.stderr)
        return 1

    out_json = args.out_json or args.capture.with_suffix(args.capture.suffix + ".proposed.json")
    out_report = args.out_report or args.capture.with_suffix(args.capture.suffix + ".report.txt")

    capture = parse_capture_file(args.capture)
    doc, report = translate(capture)

    out_json.write_text(json.dumps(doc, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    out_report.write_text(render_text(report, source_path=str(args.capture)), encoding="utf-8")

    total = len(report.mapped) + len(report.converted) + len(report.dropped) + len(
        report.unrecognised
    )
    print(
        f"stock_config_import: {total} field(s) examined -- "
        f"{len(report.converted)} converted, {len(report.mapped)} mapped, "
        f"{len(report.dropped)} dropped, {len(report.unrecognised)} unrecognised."
    )
    print(f"  proposed overlay : {out_json.resolve()}")
    print(f"  report           : {out_report.resolve()}")
    print(
        "  Review both by hand before merging into a real config and "
        "sending with `ws500ctl set` -- this proposal is never applied "
        "automatically."
    )
    return 0


if __name__ == "__main__":
    sys.exit(main())
