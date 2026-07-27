"""
commands -- one module per `ws500ctl <subcommand>` (GH#17). Each module
exposes `add_subparser(subparsers)` (registers itself with argparse and sets
`func=run` on the resulting namespace) and `run(args) -> int` (the exit code).
cli.py only knows the list of modules; it does not know their contents.

SPDX-License-Identifier: MIT
"""
