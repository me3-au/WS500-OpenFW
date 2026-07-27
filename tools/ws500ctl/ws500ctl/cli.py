"""
cli.py -- ws500ctl argument parsing and dispatch (GH#17, deliverable #8
secondary client).

Global flags (--port/--baud/--timeout/--retries) are shared by every
subcommand; session.py turns them into an open WS500Client so this module
stays pure argparse plumbing. Exit codes are owned by the exceptions in
proto.py (WS500Error.exit_code) -- this module's only job on failure is to
print the message and propagate that code, documented in
tools/ws500ctl/README.md.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import sys
from typing import Optional

from . import __version__, proto
from .commands import flash, get, import_wizard, info, ping
from .commands import set as set_cmd
from .commands import telem

_SUBCOMMANDS = (info, get, set_cmd, telem, ping, import_wizard, flash)


def build_parser() -> argparse.ArgumentParser:
    p = argparse.ArgumentParser(
        prog="ws500ctl",
        description="CLI client for WS500-OpenFW (USB-CDC JSON-lines config protocol).",
    )
    p.add_argument("--version", action="version", version=f"%(prog)s {__version__}")
    p.add_argument(
        "--port",
        default=None,
        help='serial port (e.g. COM5, /dev/ttyACM0), or "auto" to scan by VID:PID',
    )
    p.add_argument(
        "--baud",
        type=int,
        default=proto.DEFAULT_BAUDRATE,
        help=f"serial baud rate (default {proto.DEFAULT_BAUDRATE})",
    )
    p.add_argument(
        "--timeout",
        type=float,
        default=proto.DEFAULT_TIMEOUT_S,
        help=f"reply timeout in seconds (default {proto.DEFAULT_TIMEOUT_S})",
    )
    p.add_argument(
        "--retries",
        type=int,
        default=proto.DEFAULT_RETRIES,
        help=f"retries on timeout before giving up (default {proto.DEFAULT_RETRIES})",
    )

    sub = p.add_subparsers(dest="command", required=True)
    for mod in _SUBCOMMANDS:
        mod.add_subparser(sub)
    return p


def main(argv: Optional[list] = None) -> int:
    parser = build_parser()
    args = parser.parse_args(argv)
    try:
        return args.func(args)
    except proto.WS500Error as e:
        print(f"ws500ctl: error: {e}", file=sys.stderr)
        return e.exit_code
    except KeyboardInterrupt:
        print(file=sys.stderr)
        return 130


if __name__ == "__main__":
    sys.exit(main())
