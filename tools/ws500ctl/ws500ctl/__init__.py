"""
ws500ctl -- Python CLI client for WS500-OpenFW (GH#17, deliverable #8 secondary
client). Speaks the USB-CDC JSON-lines config protocol implemented in
control/Inc/config_msg.h, under the compatibility contract in
docs/VERSIONING.md: a hello handshake with a proto-range gate, capability-driven
feature enablement (never version sniffing beyond that gate), and client-owned
config-file migrations (decision #6a-B: "the app translates the file").

Pure stdlib + pyserial (the only runtime dependency). Python 3.10+.

bench-pending: this client has never been run against the physical unit. The
firmware side of the wire is CI-proven against the same grammar this package
implements (control/test/test_protocol.c); see tools/ws500ctl/README.md.

SPDX-License-Identifier: MIT
"""
from __future__ import annotations

__version__ = "0.1.0-dev"

__all__ = ["__version__"]
