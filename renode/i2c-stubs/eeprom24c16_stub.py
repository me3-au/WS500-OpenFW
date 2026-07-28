# eeprom24c16_stub.py -- Renode I2C device stub for the 24C16-class config
# EEPROM, PROJECT_PLAN 8.2 "I2C device stubs" deliverable.
# SPDX-License-Identifier: MIT
#
# Same rationale and DummyI2CSlave technique as ina226_stub.py in this
# directory (read that file's header for the "why DummyI2CSlave" background,
# and its WIRE-PROTOCOL MODEL section for why this file also does NOT queue a
# reply eagerly off an address-only write -- the same bug was found and fixed
# here too, see below) -- this stub backs Core/Src/eeprom24c16.c's
# HAL_I2C_Mem_Read/Write traffic with a real 2 KB byte store instead of a
# NACK/timeout, and stays attached to I2C2 because that already matches what
# eeprom24c16.c drives today (no GH#36-style bus mismatch on this device --
# see board.h / eeprom24c16.c file header).
#
# DEVICE-ADDRESS MODEL. A 24C16 folds its block-select bits into the 7-bit
# address itself: eeprom_dev_addr() in eeprom24c16.c computes
# `0xA0 | ((word_addr>>8)&7)<<1` (8-bit form) == 0x50 | block (7-bit), one
# distinct I2C address per 256-byte block (PROJECT_PLAN 0.6 V7). Renode's I2C bus model
# dispatches by exact address match, so faithfully modelling this needs EIGHT
# DummyI2CSlave instances (0x50..0x57), not one -- see mc_setup_eeprom24c16_stub
# below, called once per block from ws500.robot. All eight share the ONE
# 2048-byte backing store defined at module scope so a write through block
# 3's slave is visible to a later read through block 3's slave again (the
# guard below keeps that store across repeated `include`s of this file, e.g.
# once per Robot test case's fresh machine).
#
# WIRE-PROTOCOL MODEL -- probed directly against Renode 1.16.1's
# STM32F7_I2C, same finding as ina226_stub.py: HAL_I2C_Mem_Write's address and
# data phases arrive as TWO SEPARATE DataReceived calls (`Write([word_addr])`
# then `Write([byte0, byte1, ...])`, no address byte repeated), not one
# combined call. And DummyI2CSlave has a `ReadRequested(count)` event -- fired
# exactly when the bus wants bytes, with the count it wants -- so a reply is
# built and enqueued THEN, for whatever word address the most recent
# address-only write named, rather than queued speculatively off the address
# write itself (which the earlier version of this file did, and which is what
# broke ina226_stub.py's register values -- see that file for the full
# post-mortem). `ReadRequested`'s count also means an exact-length reply is
# possible now, instead of the previous version's "queue the whole rest of
# the block and hope the master reads a prefix of it" workaround.
EEPROM_SIZE_BYTES = 2048
EEPROM_BLOCK_SIZE  = 256    # device-address block-select granularity (board.h)

if 'eeprom24c16_data' not in globals():
    # 0xFF = the typical erased-cell state of an unwritten EEPROM byte; not a
    # claim about this specific chip's POR content (never bench-read), just a
    # conventional, clearly-not-accidentally-zero fill so a read of untouched
    # space is visibly "blank" rather than indistinguishable from a real 0x00
    # config value.
    eeprom24c16_data = bytearray(b'\xFF' * EEPROM_SIZE_BYTES)


class Eeprom24C16BlockStub:
    """Backs ONE block's DummyI2CSlave (0x50 | block). `block` (0..7) plus the
    write's own word-address byte together give the full 11-bit byte offset
    into the shared eeprom24c16_data store.
    """

    def __init__(self, dummy, block):
        self.dummy = dummy
        self.block = block
        # The word address named by the most recent address-only write, valid
        # until either a data write or a ReadRequested consumes it (see the
        # WIRE-PROTOCOL MODEL note above for why this can't resolve earlier).
        self.pending_addr = None

    def write(self, data):
        if len(data) < 1:
            self.dummy.Log(LogLevel.Warning, "EEPROM stub: empty write, ignored")
            return

        if self.pending_addr is None:
            # Address phase of a new transaction.
            word_off, rest = data[0], data[1:]
            if len(rest) == 0:
                self.pending_addr = word_off    # wait for ReadRequested or a data write
            else:
                self._store(word_off, rest)     # combined [addr, byte...] in one call
        else:
            # Data phase of a write that started with a prior address-only
            # write: no address byte here, per the WIRE-PROTOCOL MODEL note.
            word_off = self.pending_addr
            self.pending_addr = None
            self._store(word_off, data)

    def _store(self, word_off, payload):
        # HAL_I2C_Mem_Write: word address + up to a page (16 bytes,
        # eeprom24c16.c chunks at EEPROM_PAGE_SIZE boundaries so this driver
        # never sends more than that, but wrap defensively anyway for the
        # same reason the read side does -- RM0091's own internal
        # word-address counter wraps WITHIN the block, not past it).
        for i, b in enumerate(payload):
            eeprom24c16_data[(self.block << 8) | ((word_off + i) % EEPROM_BLOCK_SIZE)] = b
        self.dummy.Log(LogLevel.Info,
                        "EEPROM stub: block %d wrote %d byte(s) @ word 0x%02x"
                        % (self.block, len(payload), word_off))

    def read_requested(self, count):
        word_off = self.pending_addr
        self.pending_addr = None
        if word_off is None:
            self.dummy.Log(LogLevel.Warning,
                            "EEPROM stub: block %d read requested with no addressed word"
                            % self.block)
            self.dummy.EnqueueResponseBytes(bytearray(count))
            return

        # Exactly `count` bytes, wrapping WITHIN the block boundary -- the
        # real chip's internal word-address counter is 8 bits and free-runs
        # within the current block regardless of how many bytes the master
        # clocks out (PROJECT_PLAN 0.6 V7).
        reply = bytearray(
            eeprom24c16_data[(self.block << 8) | ((word_off + i) % EEPROM_BLOCK_SIZE)]
            for i in range(count))
        self.dummy.EnqueueResponseBytes(reply)
        self.dummy.Log(LogLevel.Info,
                        "EEPROM stub: block %d read %d byte(s) @ word 0x%02x"
                        % (self.block, count, word_off))


def mc_setup_eeprom24c16_stub(path, block):
    """Monitor command (from `include`ing this file):
        setup_eeprom24c16_stub "<path>" <block>
    where <path> is one block's DummyI2CSlave sysbus path (e.g.
    "sysbus.i2c2.eeprom0") and <block> (0..7) is that slave's block index --
    call this once per block, eight times total, all sharing this module's
    one eeprom24c16_data store.
    """
    dummy = monitor.Machine[path]
    stub = Eeprom24C16BlockStub(dummy, block)
    dummy.DataReceived += stub.write
    dummy.ReadRequested += stub.read_requested
