"""
import_wizard.py -- `ws500ctl import`: STUB, not implemented in this
deliverable.

Planned shape (docs/DECISION_6A_CONFIG_STRATEGY.md, "Decision B" courtesy 1):
parse an archived stock `$CPx`/`$SCx` config dump file (the Stage-A capture
artifact -- this reads a FILE, it does not speak `$` over the wire; the
firmware never will, decision #6a) and propose the nearest equivalent native
PROFILE_SPEC Sec 7 document. Human-reviewed as a diff, never applied
automatically (VERSIONING.md Sec 3's "shown to the user as a diff, never
silent" applies here too, even though this is a one-time cross-format
translation rather than a same-schema migration).

This module is registered with argparse (so `ws500ctl import --help` exists
and the command surface is discoverable) but implements nothing.

TODO(GH#17): implement the $-dump parser, the native-profile proposal logic,
and the diff review presented to the user before anything is sent with `set`.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import sys


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser(
        "import", help="(not yet implemented) translate a stock $-dump config file"
    )
    sp.add_argument("file", nargs="?", help="archived $CPx/$SCx dump (reserved, unused)")
    sp.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    print(
        "ws500ctl import: not yet implemented (TODO GH#17) -- see "
        "docs/DECISION_6A_CONFIG_STRATEGY.md for the planned design.",
        file=sys.stderr,
    )
    return 1
