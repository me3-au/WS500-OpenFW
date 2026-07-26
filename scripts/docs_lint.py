#!/usr/bin/env python3
# docs_lint.py — mechanical floor for the documentation standard
# (CONTRIBUTING.md "Documentation standard"). Fails CI when:
#   1. a tracked .c/.h under Core/, control/, sim/ lacks an
#      SPDX-License-Identifier line or an opening file-header comment;
#   2. a public header prototype has no doc comment within the 3 lines above it;
#   3. (warning only, never fails) a TODO/FIXME lacks a GH#n / M<n> / bench ref.
# The lint is the floor, not the standard — it checks presence, humans check *why*.
# SPDX-License-Identifier: MIT
import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SCAN_DIRS = ["Core", "control", "sim"]
HEADER_DIRS = ["Core/Inc", "control/Inc"]

# Prototype heuristic: a top-level line ending in ');' whose first token looks
# like a type, excluding preprocessor/typedef/struct lines. Multi-line
# prototypes are joined first.
PROTO_RE = re.compile(r"^[A-Za-z_][A-Za-z0-9_ \t\*]*\([^;{]*\)\s*;\s*$")
SKIP_START = ("#", "typedef", "}", "extern \"C\"")
TODO_RE = re.compile(r"\b(TODO|FIXME)\b(?P<rest>.*)")
TODO_OK = re.compile(r"GH#\d+|\bM\d\b|bench", re.IGNORECASE)

fails, warns = [], []


# Vendor-derived templates keep their upstream license header, not ours.
EXCLUDE = {"Core/Inc/stm32f0xx_hal_conf.h"}


def check_file_header(path, lines):
    head = "".join(lines[:40])
    if "SPDX-License-Identifier" not in head:
        fails.append(f"{path}: missing SPDX-License-Identifier in first 40 lines")
    if not (lines and lines[0].lstrip().startswith(("/*", "//"))):
        fails.append(f"{path}: missing opening file-header comment block")


def is_commented(lines, i):
    """Doc comment within the 3 non-blank lines above line i?"""
    seen = 0
    for j in range(i - 1, -1, -1):
        s = lines[j].strip()
        if not s:
            continue
        seen += 1
        if s.startswith(("/*", "*", "//")) or s.endswith("*/"):
            return True
        if seen >= 3:
            return False
    return False


def join_prototypes(lines):
    """Yield (start_index, joined_logical_line) for top-level statements."""
    buf, start = "", None
    for i, raw in enumerate(lines):
        s = raw.rstrip("\n")
        if start is None:
            if not s.strip() or s.lstrip() != s:  # blank or indented → not top-level
                continue
            start, buf = i, s.strip()
        else:
            buf += " " + s.strip()
        if buf.endswith((";", "{", "*/")) or buf.startswith(SKIP_START):
            yield start, buf
            start, buf = None, ""
        elif len(buf) > 500:
            start, buf = None, ""


def check_header_api(path, lines):
    for i, joined in join_prototypes(lines):
        if joined.startswith(SKIP_START):
            continue
        if PROTO_RE.match(joined) and not is_commented(lines, i):
            fails.append(f"{path}:{i + 1}: public prototype without a doc comment: "
                         f"{joined[:70]}")


def check_todos(path, lines):
    for i, line in enumerate(lines):
        m = TODO_RE.search(line)
        if m and not TODO_OK.search(line):
            warns.append(f"{path}:{i + 1}: TODO/FIXME without GH#/M<n>/bench ref")


def main():
    for d in SCAN_DIRS:
        for path in sorted((ROOT / d).rglob("*")):
            if path.suffix not in (".c", ".h"):
                continue
            rel = path.relative_to(ROOT).as_posix()
            if rel in EXCLUDE:
                continue
            lines = path.read_text(encoding="utf-8", errors="replace").splitlines()
            check_file_header(rel, lines)
            check_todos(rel, lines)
            if any(rel.startswith(h) for h in HEADER_DIRS) and path.suffix == ".h":
                check_header_api(rel, lines)

    for w in warns:
        print(f"WARN  {w}")
    for f in fails:
        print(f"FAIL  {f}")
    print(f"docs_lint: {len(fails)} failure(s), {len(warns)} warning(s)")
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
