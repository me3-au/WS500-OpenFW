"""
set.py -- `ws500ctl set`: validate and send a PROFILE_SPEC Sec 7 config
document (cfg-set).

Enforces VERSIONING.md Sec 3's client-side half of schema compatibility BEFORE
anything reaches the wire: the firmware is the schema authority and accepts
writes only in its native schema_version (config_doc.c cfg_doc_parse), so a
client that already knows the file is the wrong schema should say so directly
rather than spend a round trip being told CFG_ERR_SCHEMA. The client migrates
config files FORWARD ONLY (older file -> newer schema); that migration itself
is not implemented yet (TODO GH#17) -- this command detects the mismatch and
points at that gap rather than silently sending a document the firmware would
reject anyway.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import argparse
import json
import sys

from .. import proto, session


def add_subparser(subparsers: "argparse._SubParsersAction") -> None:
    sp = subparsers.add_parser("set", help="write a config document (cfg-set)")
    sp.add_argument(
        "-i",
        "--input",
        metavar="FILE",
        required=True,
        help="JSON document to send (as produced by `ws500ctl get`)",
    )
    sp.add_argument(
        "--dry-run",
        action="store_true",
        help="validate and print what would be sent; do not send it",
    )
    sp.set_defaults(func=run)


def _load_doc(path: str) -> dict:
    try:
        with open(path, "r", encoding="utf-8") as f:
            text = f.read()
    except OSError as e:
        raise proto.WS500InputError(f"cannot read {path}: {e}") from e
    try:
        doc = json.loads(text)
    except json.JSONDecodeError as e:
        raise proto.WS500InputError(f"{path} is not valid JSON: {e}") from e
    if not isinstance(doc, dict):
        raise proto.WS500InputError(f"{path} must contain a JSON object")
    return doc


def _check_schema(doc: dict, fw_schema: int) -> None:
    """VERSIONING.md Sec 3. An ABSENT schema_version is a legitimate
    hand-written overlay edit (config_doc.c tolerates it); a WRONG one is
    refused here, before it is ever sent."""
    sv = doc.get("schema_version")
    if sv is None:
        return
    if not isinstance(sv, int):
        raise proto.WS500InputError('"schema_version" must be an integer')
    if sv == fw_schema:
        return
    if sv < fw_schema:
        raise proto.WS500SchemaError(
            f"this file is schema {sv}; the firmware speaks schema {fw_schema}. "
            "ws500ctl migrates config files FORWARD only (VERSIONING.md Sec 3) "
            "-- the `migrate` subcommand that does this is not implemented yet "
            "(TODO GH#17). Re-export the file with `ws500ctl get` against this "
            'firmware, or remove "schema_version" from the file to send it as '
            "a hand-written overlay edit."
        )
    raise proto.WS500SchemaError(
        f"this file is schema {sv}; the connected firmware speaks schema "
        f"{fw_schema} -- the file is from NEWER firmware than what is "
        f"connected. Update the firmware, or use a ws500ctl/firmware pair that "
        f"both speak schema {sv}."
    )


def run(args: argparse.Namespace) -> int:
    doc = _load_doc(args.input)

    with session.open_client(args) as client:
        hello = client.hello()
        _check_schema(doc, hello.schema)

        if args.dry_run:
            body = json.dumps(doc)
            print(
                f"DRY RUN: would send cfg-set with {len(body)} bytes of "
                '"cfg"; nothing was sent.'
            )
            print(json.dumps(doc, indent=2))
            return 0

        reply = client.cfg_set(doc)

    unknown = reply.get("unknown_keys")
    print(f"ok: generation={reply.get('generation')} unknown_keys={unknown}")
    if unknown:
        print(
            f"note: {unknown} key(s) in the document were not recognised by "
            "this firmware and were ignored, not stored (config_msg.h's "
            "unknown-key policy) -- re-run `ws500ctl get` to see what was "
            "actually adopted.",
            file=sys.stderr,
        )
    return 0
