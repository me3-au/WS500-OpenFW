"""
get.py -- `ws500ctl get`: fetch the PROFILE_SPEC Sec 7 config document
(cfg-get), stamped with the firmware's own schema_version/fw (config_doc.c
cfg_doc_emit) and never stripped here.
SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import json
import sys

from .. import session


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser("get", help="read the live config document (cfg-get)")
    sp.add_argument(
        "-o", "--output", metavar="FILE", help="write the document to FILE instead of stdout"
    )
    sp.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    with session.open_client(args) as client:
        doc = client.cfg_get()

    # Pretty-printed for readability, but nothing is added, removed or
    # reinterpreted: json.loads() preserved member order and every key the
    # firmware sent (including ones this client doesn't recognise), so this
    # is "the document exactly as received", just not byte-identical to the
    # wire's compact single-line form.
    text = json.dumps(doc, indent=2)
    if args.output:
        with open(args.output, "w", encoding="utf-8", newline="\n") as f:
            f.write(text + "\n")
        print(f"wrote {args.output} ({len(text) + 1} bytes)", file=sys.stderr)
    else:
        print(text)
    return 0
