"""
info.py -- `ws500ctl info`: print the hello handshake contents.
SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse

from .. import format, session


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser(
        "info", help="print firmware/protocol identity from the hello handshake"
    )
    sp.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    with session.open_client(args) as client:
        hello = client.hello()
    print(format.hello_text(hello))
    return 0
