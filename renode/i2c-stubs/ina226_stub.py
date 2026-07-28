# ina226_stub.py -- Renode I2C device stub for the TI INA226 current/bus-voltage
# monitor, PROJECT_PLAN 8.2 "I2C device stubs" deliverable.
# SPDX-License-Identifier: MIT
#
# Renode ships no INA226 model (renode/README.md "I2C stub adequacy" risk), so
# Core/Src/ina2xx.c's HAL_I2C_Mem_Read/Write calls currently NACK/time out
# against an unpopulated bus. This attaches to Renode's built-in
# `Mocks.DummyI2CSlave` (a real II2CPeripheral the stock Renode distribution
# ships for exactly this "scriptable I2C responder" job -- see
# tests/platforms/Renesas_DA14592.robot's echo-i2c-peripheral.py in the Renode
# tree for the base pattern) and answers with plausible fixed values, so
# ina2xx.c's poll-then-read gate and its shunt/bus scaling math both actually
# execute under emulation instead of falling straight to the NAN fallback path.
#
# --- BUS CAVEAT (GH#36) -----------------------------------------------------
# ina2xx.c currently brings up I2C1 (PB6/PB7). The bench-confirmed stock-
# binary evidence (PROJECT_PLAN 0.6 V7) says the real board runs the
# INA226 on I2C2 (PB10/PB11) instead. That flip is bench-gated and not made
# here -- this stub is attached to i2c1 in ws500.robot to match whichever bus
# the CURRENT firmware actually drives, so the test stays meaningful today.
# When GH#36 flips the driver to I2C2, move the
# `Mocks.DummyI2CSlave @ i2c1 0x40` attachment line to i2c2 alongside it --
# this script itself does not encode a bus, so nothing else here changes.
# -----------------------------------------------------------------------------
#
# Register map (verified from the stock binary, PROJECT_PLAN 0.6 V3, mirrored
# in board.h): 0x00 CONFIG, 0x01 SHUNT_V (signed, 2.5 uV/LSB), 0x02 BUS_V
# (1.25 mV/LSB), 0x05 CALIBRATION, 0x06 Mask/Enable (CVRF = bit 3, read clears
# it). All registers are 16-bit BIG-ENDIAN on the wire (ina2xx.c's
# ina_read16/ina_write16 do the byte-order swap; this stub mirrors that).
#
# WIRE-PROTOCOL MODEL -- rewritten after the FIRST version (address+data
# combined into one `Write([reg, hi, lo])` call, and an eager reply queued on
# every address-only write "in case a Read() follows") was probed directly
# against Renode 1.16.1's STM32F7_I2C and found wrong on BOTH counts:
#   1. HAL_I2C_Mem_Write's address and data phases arrive as TWO SEPARATE
#      DataReceived calls -- `Write([reg])` then `Write([hi, lo])`, with no
#      register byte repeated in the second call. Treating the first call as
#      a complete transaction (as the original version did) meant the address
#      byte was silently misread as a phantom READ SETUP -- for the wrong
#      register no less, since it also mis-parsed the second call as
#      `reg=hi, payload=[lo]` -- and the real CONFIG value was NEVER stored.
#   2. Eagerly calling EnqueueResponseBytes() on every address-only write (so
#      it would be ready "in case" a Read() followed) queued bytes that a
#      write transaction never consumes. Those bytes did not evaporate --
#      DummyI2CSlave's queue is FIFO across the WHOLE peripheral, so they sat
#      there and were wrongly served to the NEXT unrelated Read(), shifting
#      every subsequent register value by one. This is what actually happened
#      in this firmware's own boot sequence (probed directly): SHUNT_V's read
#      returned the stale MASK_ENABLE reply (0x0008 -> 0.2 A) and BUS_V's read
#      returned the stale SHUNT_V reply (2000 counts -> 2.5 V) -- silently
#      plausible-looking numbers, wrong ones, and nothing in a log said so.
#      A stub that answers with the WRONG data is worse than one that NACKs.
#
# The fix uses DummyI2CSlave's `ReadRequested(count)` event (reflected off the
# live object -- not documented in any shipped example) instead of queuing
# speculatively. It fires exactly when the bus actually wants bytes, with the
# byte count it wants, so the reply is built and enqueued at that moment, for
# whichever register the most recent address-only write named -- never before
# it is certain a read is what's happening, and never for the wrong register.
INA_REG_CONFIG      = 0x00
INA_REG_SHUNT_V      = 0x01
INA_REG_BUS_V        = 0x02
INA_REG_CALIBRATION  = 0x05
INA_REG_MASK_ENABLE  = 0x06
INA226_MASKEN_CVRF   = 1 << 3

# Plausible fixed sample, chosen against this firmware's actual configured
# shunt (board.h: SHUNT_FULL_SCALE_A=500, SHUNT_FULL_SCALE_MV=50 ->
# R_shunt = 50mV/500A = 100 uOhm, ina2xx.c ina2xx_init()):
#   SHUNT_V raw = 2000 counts * 2.5 uV/LSB = 5.0 mV -> 5.0mV / 100uOhm = 50.0 A
#   (well inside the +-500 A full-scale range: 500A == 20000 counts == 50mV)
# BUS_V is the INA226's OWN chip-native register, capped by its 15-bit-plus-
# sign-free 1.25 mV/LSB encoding at ~40.96 V full scale (INA226 datasheet) --
# it is NOT necessarily the full battery-pack voltage: whether there is a
# VBUS-pin divider ahead of it is not established by any V-item in
# PROJECT_PLAN 0.6, and ina2xx.c applies no extra scaling of its own, so this
# stub does not invent one either. 19200 counts * 1.25 mV/LSB = 24.000 V is
# just a round, in-range, nonzero chip-native reading -- enough to prove
# ina2xx_bus_v() derives a finite, sane float from real wire bytes instead of
# NAN, which is this stub's whole job. Confirmed end-to-end (not just
# asserted): with this fix, ina2xx.c's OWN computed s_current_a/s_bus_v land
# at exactly 50.0 / 24.0 in the firmware's RAM, matching these constants'
# documented derivation -- the earlier, broken version silently produced
# 0.2 A / 2.5 V instead (see WIRE-PROTOCOL MODEL above).
SHUNT_V_RAW = 2000        # -> 50.0 A
BUS_V_RAW   = 19200       # -> 24.000 V (chip-native, see note above)


class Ina226Stub:
    """Backs a Mocks.DummyI2CSlave with INA226 register-read/write behaviour.

    One instance per attached slave (see mc_setup_ina226_stub below); `dummy`
    is the DummyI2CSlave peripheral object, used to log, to answer
    ReadRequested with the right register's bytes, and (via DataReceived) to
    track which register the master last addressed.
    """

    def __init__(self, dummy):
        self.dummy = dummy
        # CONFIG/CALIBRATION are write-then-maybe-read-back; store whatever
        # the driver last wrote so a read-back (never actually done by
        # ina2xx.c today, but harmless to support) is not just zeros.
        self.regs = {INA_REG_CONFIG: 0x0000, INA_REG_CALIBRATION: 0x0000}
        # The register named by the most recent address-only write, valid
        # until either a data-write consumes it or a read consumes it (both
        # clear it back to None -- see the WIRE-PROTOCOL MODEL note above for
        # why this cannot be resolved any earlier than that).
        self.pending_reg = None

    def write(self, data):
        if len(data) < 1:
            self.dummy.Log(LogLevel.Warning, "INA226 stub: empty write, ignored")
            return

        if self.pending_reg is None:
            # Address phase of a new transaction.
            reg, rest = data[0], data[1:]
            if len(rest) == 0:
                self.pending_reg = reg          # wait for ReadRequested or a data write
            elif len(rest) == 2:
                self._store(reg, rest)          # combined [reg, hi, lo] in one call
            else:
                self.dummy.Log(LogLevel.Warning,
                                "INA226 stub: unexpected write length %d to reg 0x%02x"
                                % (len(rest), reg))
        else:
            # Data phase of a write that started with a prior address-only
            # write: no register byte here, per the WIRE-PROTOCOL MODEL note.
            reg = self.pending_reg
            self.pending_reg = None
            if len(data) == 2:
                self._store(reg, data)
            else:
                self.dummy.Log(LogLevel.Warning,
                                "INA226 stub: unexpected data-phase length %d for reg 0x%02x"
                                % (len(data), reg))

    def _store(self, reg, payload):
        val = (payload[0] << 8) | payload[1]
        self.regs[reg] = val
        self.dummy.Log(LogLevel.Info,
                        "INA226 stub: wrote reg 0x%02x = 0x%04x" % (reg, val))

    def read_requested(self, count):
        reg = self.pending_reg
        self.pending_reg = None
        if reg is None:
            self.dummy.Log(LogLevel.Warning,
                            "INA226 stub: read requested with no addressed register")
            self.dummy.EnqueueResponseBytes(bytearray(count))
            return

        if reg == INA_REG_MASK_ENABLE:
            # Real silicon clears CVRF on this read and only re-sets it once
            # the next averaged conversion (~35 ms at this firmware's 16x/
            # 1.1ms config, see ina2xx.c) completes. This stub does not model
            # that cadence -- no V-item gives a conversion-timing fact to
            # emulate against -- so it re-arms CVRF immediately rather than
            # leaving it stuck clear, which would silently stall
            # ina2xx.c's poll-then-read gate forever under emulation. The
            # gate itself (poll 0x06, only proceed to 0x01/0x02 once CVRF is
            # seen set) is exactly what this exercises.
            val = INA226_MASKEN_CVRF
            self.dummy.Log(LogLevel.Info, "INA226 stub: CVRF poll -> ready")
        elif reg == INA_REG_SHUNT_V:
            val = SHUNT_V_RAW & 0xFFFF
            self.dummy.Log(LogLevel.Info,
                            "INA226 stub: SHUNT_V -> 0x%04x (50.0 A)" % val)
        elif reg == INA_REG_BUS_V:
            val = BUS_V_RAW & 0xFFFF
            self.dummy.Log(LogLevel.Info,
                            "INA226 stub: BUS_V -> 0x%04x (24.000 V)" % val)
        else:
            val = self.regs.get(reg, 0x0000)
            self.dummy.Log(LogLevel.Info,
                            "INA226 stub: read-back reg 0x%02x -> 0x%04x" % (reg, val))

        reply = bytearray([(val >> 8) & 0xFF, val & 0xFF])
        # ina2xx.c only ever reads 2 bytes (I2C_MEMADD_SIZE_8BIT, 16-bit
        # registers), but answer whatever length the bus actually asked for
        # rather than assuming -- pad/truncate defensively instead of
        # potentially stalling a request this stub did not anticipate.
        if count != len(reply):
            reply = (reply + bytearray(count))[:count]
        # bytearray, not a plain list: probed against Renode 1.16.1 -- a bare
        # Python list of ints crashes the WHOLE process inside
        # DummyI2CSlave.EnqueueResponseBytes ("Could not cast: System.Byte in
        # System.Int32"), because a plain `list` doesn't carry the CLR
        # IEnumerable<byte> typing EnqueueResponseBytes needs, unlike
        # `bytearray`/`bytes`, which IronPython maps onto that directly (see
        # the Renode-shipped tests/platforms/echo-i2c-peripheral.py, which
        # only ever re-enqueues a `data` array it received FROM the bus, never
        # a freshly-built Python list -- this stub is the first to build one).
        self.dummy.EnqueueResponseBytes(reply)


def mc_setup_ina226_stub(path):
    """Monitor command (from `include`ing this file): setup_ina226_stub "<path>"
    where <path> is the DummyI2CSlave's sysbus path, e.g. "sysbus.i2c1.ina226".
    """
    dummy = monitor.Machine[path]
    stub = Ina226Stub(dummy)
    dummy.DataReceived += stub.write
    dummy.ReadRequested += stub.read_requested
