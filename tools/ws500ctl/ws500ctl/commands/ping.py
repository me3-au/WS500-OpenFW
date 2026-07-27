"""
ping.py -- `ws500ctl ping`: hello round-trip time.
SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse

from .. import session


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser("ping", help="measure hello round-trip time")
    sp.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    with session.open_client(args) as client:
        rtt_s = client.ping()
    print(f"{rtt_s * 1000:.1f} ms")
    return 0
