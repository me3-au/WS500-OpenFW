"""
flash.py -- `ws500ctl flash`: STUB, not implemented in this deliverable.

Planned shape (docs/CLIENT_CONNECTIVITY.md, docs/FLASH_AND_RECOVERY.md): drive
the STM32 ROM DFU bootloader (unbrickable, entered via the documented pin/
button sequence or a "dfu-handoff" capability request over this same CDC
protocol once that capability lands) to flash a new firmware image, verify it,
and hand back the config-preserving story FLASH_AND_RECOVERY.md describes --
this is firmware update, a fundamentally different transport (USB DFU, not
USB CDC JSON-lines) from everything else in this package, hence its own
subcommand and module rather than a proto.py method.

PROJECT_PLAN.md Sec 5's staged access ladder gates ANY hardware-facing action
on this repo, including exercising this command against the one physical
unit -- do not wire this up against real hardware without that sign-off, even
after the implementation below exists.

This module is registered with argparse (so `ws500ctl flash --help` exists)
but implements nothing.

TODO(GH#30): implement DFU device discovery (WinUSB/libusb), image transfer,
verification, and the config-preserving update flow.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import sys


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser(
        "flash", help="(not yet implemented) flash firmware via USB DFU"
    )
    sp.add_argument("file", nargs="?", help="firmware image (.bin/.dfu, reserved, unused)")
    sp.set_defaults(func=run)


def run(args: argparse.Namespace) -> int:
    print(
        "ws500ctl flash: not yet implemented (TODO GH#30) -- see "
        "docs/FLASH_AND_RECOVERY.md for the planned design.",
        file=sys.stderr,
    )
    return 1
