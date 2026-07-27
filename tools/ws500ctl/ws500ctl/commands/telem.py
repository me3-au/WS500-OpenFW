"""
telem.py -- `ws500ctl telem`: one telem-get snapshot, or --follow to poll at
1 Hz until Ctrl-C.

The wire is PULL-based: telemetry_json.h emits exactly one {"t":"telem",...}
line per {"t":"telem-get"} request (config_msg.h has no push/streaming mode).
So --follow is this client issuing a fresh request once a second, not a
firmware streaming feature -- worth knowing if this is ever compared against
the CAN/NMEA2000 telemetry stream, which really is continuous.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import time

from .. import format, session

POLL_INTERVAL_S = 1.0


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser("telem", help="read live telemetry (telem-get)")
    sp.add_argument("--follow", action="store_true", help="poll at 1 Hz until Ctrl-C")
    sp.add_argument(
        "--json", action="store_true", help="print raw JSON lines instead of a table row"
    )
    sp.set_defaults(func=run)


def _print_one(t: dict, as_json: bool) -> None:
    print(format.telem_json(t) if as_json else format.telem_text(t))


def run(args: argparse.Namespace) -> int:
    with session.open_client(args) as client:
        if not args.follow:
            _print_one(client.telem_get(), args.json)
            return 0
        try:
            while True:
                t0 = time.monotonic()
                _print_one(client.telem_get(), args.json)
                elapsed = time.monotonic() - t0
                if elapsed < POLL_INTERVAL_S:
                    time.sleep(POLL_INTERVAL_S - elapsed)
        except KeyboardInterrupt:
            # A user-initiated stop of a follow loop is a normal exit, not an
            # error -- unlike an unrequested Ctrl-C elsewhere, which cli.main()
            # reports as exit 130.
            return 0
