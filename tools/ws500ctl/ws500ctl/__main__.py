"""
__main__.py -- `python -m ws500ctl` entry point.
SPDX-License-Identifier: MIT
"""
from __future__ import annotations

import sys

from .cli import main

if __name__ == "__main__":
    sys.exit(main())
