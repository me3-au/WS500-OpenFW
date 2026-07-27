# ws500ctl

Python CLI client for **WS500-OpenFW** (GH#17, deliverable #8 secondary
client). Speaks the USB-CDC JSON-lines config protocol implemented in
`control/Inc/config_msg.h`, under the compatibility contract in
[`docs/VERSIONING.md`](../../docs/VERSIONING.md).

**bench-pending**: this client has never been run against the physical WS500
unit and there is exactly one, installed and live on a 48 V system
(`docs/PROJECT_PLAN.md` §0.6/§5). Do not point it at real hardware without
following the staged access ladder in `PROJECT_PLAN.md` §5. The firmware side
of the wire it speaks is CI-proven against the same grammar
(`control/test/test_protocol.c`); this package's own test suite proves the
client side against an in-memory simulation of that grammar
(`tests/mock_serial.py`), not against a real device.

## Install

```sh
pip install -e ".[test]"     # editable install + pytest, from tools/ws500ctl/
```

Requires Python 3.10+ and `pyserial`. No other runtime dependency (stdlib
`argparse`/`json` for everything else).

## Usage

```
ws500ctl --port <PORT|auto> [--baud N] [--timeout S] [--retries N] <command> [args...]
```

`--port` is required for every command except `import`/`flash` (stubs) and
`--version`/`--help`. `--port auto` scans pyserial's port list for VID:PID
`1209:0001` — the pid.codes "Test PID", a development placeholder shared by
many hobbyist boards (see `ws500ctl/proto.py`'s `DEFAULT_VID`/`DEFAULT_PID`
comment). **This will change to an assigned VID:PID before release**; more
than one matching port is treated as ambiguous and refused rather than
guessed at.

| Command | What it does |
|---|---|
| `info` | Print the hello handshake: firmware version, git hash, protocol version, schema version, capability flags. |
| `get [-o FILE]` | Fetch the live config (`cfg-get`) — the PROFILE_SPEC §7 document exactly as received (schema_version/fw stamps preserved, unknown keys untouched). Prints to stdout, or writes to `FILE` with `-o`. |
| `set -i FILE [--dry-run]` | Read a JSON document from `FILE` and send it (`cfg-set`). Refuses locally if the file's `schema_version` doesn't match the firmware's (see below). `--dry-run` validates and prints what would be sent without opening a write — nothing touches the wire. On success prints `ok: generation=N unknown_keys=K`; on firmware rejection prints the validator's own named error code verbatim. |
| `telem [--follow] [--json]` | One `telem-get` snapshot, or `--follow` to poll at 1 Hz until Ctrl-C (the wire has no push/streaming mode — `--follow` is this client re-requesting once a second, not a firmware feature). Human-readable table by default; `--json` prints the raw reply line. |
| `ping` | Round-trip time of a fresh hello exchange, in milliseconds. |
| `import` | **Stub** (`TODO(GH#17)`) — planned: translate an archived stock `$CPx`/`$SCx` config dump into a native PROFILE_SPEC §7 document, reviewed as a diff before `set`. See `docs/DECISION_6A_CONFIG_STRATEGY.md`. |
| `flash` | **Stub** (`TODO(GH#30)`) — planned: USB DFU firmware update via the STM32 ROM bootloader. See `docs/FLASH_AND_RECOVERY.md`. Gated by `PROJECT_PLAN.md` §5 like every other hardware-facing action. |

Every command that talks to the firmware checks its required capability
(`cfg` for `get`/`set`, `telem` for `telem`) against the hello's `caps` list
before doing anything else, and refuses clearly if it's absent — this client
never falls back to sniffing a version number for that (VERSIONING.md §2:
"feature sync is capability-driven, not version-sniffed").

### Examples

```sh
ws500ctl --port auto info
ws500ctl --port COM5 get -o my-config.json
ws500ctl --port COM5 set -i my-config.json --dry-run
ws500ctl --port COM5 set -i my-config.json
ws500ctl --port /dev/ttyACM0 telem --follow
ws500ctl --port COM5 ping
```

## Exit codes

| Code | Meaning | Raised by |
|---|---|---|
| 0 | Success | |
| 1 | Unexpected/internal error, or a stub command (`import`, `flash`) | `WS500ProtocolError`, stub commands |
| 3 | Protocol version gate: firmware's `proto` is outside this build's supported range | `WS500ProtoRangeError` (VERSIONING.md §2, "client refuses politely") |
| 4 | Required capability absent from the hello's `caps` list | `WS500CapabilityError` |
| 5 | Firmware answered `{"t":"err",...}` | `WS500FirmwareError` — `.code` carries the validator's own error name (e.g. `CFG_ERR_RANGE_V_BULK`) or one of the message layer's five (`CFG_ERR_JSON`/`MSG`/`TYPE`/`SCHEMA`/`STORE`), printed verbatim, never re-worded |
| 6 | Local config file's `schema_version` doesn't match the connected firmware's | `WS500SchemaError` |
| 7 | Port could not be opened / autodiscovery ambiguous or empty / link died / no reply within timeout+retries | `WS500ConnectionError`, `WS500TimeoutError` |
| 8 | Local input file (`set -i`) missing or not valid JSON | `WS500InputError` |
| 130 | Interrupted with Ctrl-C (outside a `telem --follow` loop, which treats Ctrl-C as a normal stop and exits 0) | |

Every non-zero, non-130 exit is a subclass of `WS500Error` in `ws500ctl/proto.py`;
`cli.py`'s `main()` prints `e` and returns `e.exit_code` — there is exactly one
place that maps exceptions to exit codes.

## Relationship to `docs/VERSIONING.md`

This client implements the client half of the compatibility standard end to
end:

- **Handshake (§2):** every command opens with a `{"t":"hello","proto":1}`
  request (`proto.CLIENT_PROTO_MAX`). A firmware `proto` outside
  `[CLIENT_PROTO_MIN, CLIENT_PROTO_MAX]` (currently `[1, 1]`) is refused with
  exit code 3 — the firmware itself never refuses a hello (§2: "a client that
  cannot be told what it is talking to cannot explain itself to a user
  standing in an engine room").
- **Capability-driven features (§2):** nothing in this client compares
  version numbers beyond that one range gate. `set`/`get` need `"cfg"` in
  `caps`, `telem` needs `"telem"`; new firmware features arrive as new
  capability strings, never as a version bump this client has to know about.
- **Config compatibility (§3):** the firmware is the schema authority. `set`
  checks the input file's `schema_version` against the firmware's *before*
  sending anything: equal is sent as-is; absent is sent as a hand-written
  overlay edit (VERSIONING.md §3's "new fields... with safe defaults" /
  config_doc.h's "parsing is an overlay" — an overlay document only touches
  the keys it names); older is refused locally and points at the
  not-yet-implemented `migrate` path (`TODO(GH#17)`, "the client owns file
  migrations"); newer is refused outright (this firmware cannot speak it).
  This is a client-side pre-check to save a round trip — the firmware would
  reject a genuine mismatch itself with `CFG_ERR_SCHEMA`, and that path is
  exercised too (`tests/test_cli.py::test_firmware_error_exit_code`).

## Protocol/schema knowledge that is duplicated, not shared

Python has no access to the C headers, so `ws500ctl/proto.py`'s
`LINE_MAX = 3072` and `CLIENT_PROTO_MIN`/`CLIENT_PROTO_MAX = 1` mirror
`control/Inc/config_msg.h`'s `CFG_MSG_LINE_MAX`/`CFG_PROTO_VERSION` by hand —
there is no build-time check tying the two together. A firmware-side bump to
either must be echoed here. Likewise `tests/fixtures.py`'s baseline document
mirrors `control/test/test_config.h`'s `cfg_test_baseline()` (16S / 300 Ah /
8000 W) and `control/Inc/config_doc.h`'s documented §7 deviations (no
`engine`/`limits.belt`/`limits.warmup`/`global.topbalance_days` — schema-v1
has no field for them; `rest.power_cap_w` not `power_cap_pct`; `revert.ah`
not `ah_frac_c`; profile 6's Limp Home is an ordinary `rest` block, not a
separate `limp` block) — a hand-edit, not a generated artifact.

## Testing

```sh
pip install -e ".[test]"
pytest -v
```

No hardware, no real serial port: `tests/mock_serial.py`'s `MockSerial`
implements `ws500ctl.proto.SerialLike` (`write`/`read`/`close`) and
simulates enough of the *observable* firmware wire behaviour — hello/caps,
`cfg-get`/`cfg-set`/`telem-get` shapes, the named error codes, line framing,
timeouts — to exercise this client's transport and command logic end to end.
It is a simulation, not a re-implementation: it does not run the real ~40-rule
validator or the real bounded JSON reader, and it is not a substitute for
`control/test/test_protocol.c`, which is the actual grammar oracle.

`tests/test_proto.py` exercises `ws500ctl.proto` directly (transport, hello
gate, capability gate, error propagation, retries/timeouts, port
autodiscovery). `tests/test_cli.py` exercises `ws500ctl.cli` end to end with
`session.open_client` monkeypatched onto a `MockSerial`-backed client (exit
codes, `--dry-run`, schema-mismatch both directions, stub subcommands,
`--version`).

## Layout

```
tools/ws500ctl/
  pyproject.toml          setuptools + pytest config, console_scripts entry point
  README.md               this file
  ws500ctl/
    __init__.py            package docstring, __version__
    __main__.py             `python -m ws500ctl`
    proto.py               wire constants, exceptions, SerialLike, WS500Client, discover_port
    session.py              argparse.Namespace -> open WS500Client
    cli.py                  argparse wiring, dispatch, exit-code mapping
    format.py               human-readable rendering for `info`/`telem`
    commands/
      __init__.py
      info.py               `info`
      get.py                `get`
      set.py                `set`
      telem.py              `telem`
      ping.py                `ping`
      import_wizard.py      `import` (stub, TODO(GH#17))
      flash.py              `flash` (stub, TODO(GH#30))
  tests/
    __init__.py
    fixtures.py             default_cfg_doc()/default_telem_line() reference documents
    mock_serial.py          MockSerial, the in-memory firmware simulation
    test_proto.py           proto.py unit tests
    test_cli.py             cli.py end-to-end tests
```
