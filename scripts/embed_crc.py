#!/usr/bin/env python3
# embed_crc.py — patch the PROJECT_PLAN §7 R4 image CRC into a firmware binary.
#
# The linker script (STM32F072xB_FLASH.ld) places a single-word `.fw_crc`
# section LAST in flash, so `objcopy -O binary` always ends the image with that
# word. This script therefore needs no map-file parsing, no `nm`, and no
# toolchain at all: CRC-32 everything except the final four bytes, and write
# the result there, little-endian.
#
#     crc = zlib.crc32(image[:-4])        # standard reflected CRC-32
#     image[-4:] = crc.to_bytes(4, "little")
#
# integrity.c recomputes exactly this range at boot with the STM32's hardware
# CRC unit (configured REV_IN=word / REV_OUT=1 / final XOR, which is what makes
# the peripheral agree with zlib) and compares against the stored word.
#
# Idempotent: re-running on an already-patched image recomputes the same value
# from the same unchanged prefix. An unpatched image carries 0xFFFFFFFF, which
# integrity.c reports as UNPATCHED rather than as corruption.
#
# Usage:  embed_crc.py <image.bin> [--check]
#   --check verifies an existing patch instead of writing (exit 1 on mismatch).
#
# SPDX-License-Identifier: MIT
import sys
import zlib
from pathlib import Path

CRC_SLOT_BYTES = 4
PLACEHOLDER = 0xFFFFFFFF


def main(argv):
    args = [a for a in argv[1:] if not a.startswith("--")]
    check_only = "--check" in argv[1:]
    if len(args) != 1:
        print("usage: embed_crc.py <image.bin> [--check]", file=sys.stderr)
        return 2

    path = Path(args[0])
    data = bytearray(path.read_bytes())
    if len(data) <= CRC_SLOT_BYTES:
        print(f"embed_crc: {path} is too small to hold a CRC slot",
              file=sys.stderr)
        return 1

    crc = zlib.crc32(bytes(data[:-CRC_SLOT_BYTES])) & 0xFFFFFFFF
    stored = int.from_bytes(data[-CRC_SLOT_BYTES:], "little")

    if check_only:
        if stored == crc:
            print(f"embed_crc: OK  0x{crc:08X}  ({len(data)} bytes)")
            return 0
        what = "unpatched" if stored == PLACEHOLDER else "MISMATCH"
        print(f"embed_crc: {what}  stored=0x{stored:08X} computed=0x{crc:08X}",
              file=sys.stderr)
        return 1

    data[-CRC_SLOT_BYTES:] = crc.to_bytes(CRC_SLOT_BYTES, "little")
    path.write_bytes(bytes(data))
    print(f"embed_crc: {path.name}  {len(data)} bytes  CRC-32 0x{crc:08X}"
          f"  (was 0x{stored:08X})")
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
