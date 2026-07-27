"""
session.py -- turns parsed CLI args into an open WS500Client (GH#17).

Kept out of cli.py (so command modules can share it without importing the
argparse wiring) and out of proto.py (so the transport stays free of anything
shaped like an argparse.Namespace -- proto.py is exercised directly in
tests/test_proto.py with no argparse involved at all).

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse

from . import proto


def resolve_port(args: argparse.Namespace) -> str:
    """--port auto scans by VID:PID (proto.discover_port); anything else is
    used as-is. --port is required for every command that reaches here."""
    if args.port is None:
        raise proto.WS500ConnectionError(
            "--port is required (use --port auto to scan by VID:PID)"
        )
    if args.port == "auto":
        return proto.discover_port()
    return args.port


def open_client(args: argparse.Namespace) -> proto.WS500Client:
    """Resolve --port and open a real serial connection. Commands use this via
    `with session.open_client(args) as client:` so the port is always closed,
    even on an exception."""
    port = resolve_port(args)
    return proto.WS500Client.open(
        port, baudrate=args.baud, timeout=args.timeout, retries=args.retries
    )
