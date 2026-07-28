# iwdg_stub.py -- Renode IWDG (independent watchdog) model for the SS7 R1
# checkpoint-kicked-watchdog starvation test (PROJECT_PLAN SS8.2, issue #25/#27).
# SPDX-License-Identifier: MIT
#
# Renode ships no IWDG model for the F0 family; the stock platform maps IWDG
# as a silent Tag region (writes logged, reads 0, no reset -- see
# ws500f072.repl / renode/README.md "KNOWN GAP -- IWDG"). That is harmless for
# the boot-liveness and NMI fault-path tests, which never depend on the
# watchdog actually firing, but it means the platform cannot prove
# watchdog.c's core safety claim: "a genuinely stopped loop always [resets]"
# (watchdog.c file header). This stub closes exactly that gap: a real,
# virtual-time-driven countdown that calls Machine.RequestReset() when it is
# not kicked in time, register-compatible with watchdog.c's actual
# IWDG->KR/PR/RLR/WINR/SR usage (RM0091 SS21).
#
# ATTACHMENT. Registered directly over the existing IWDG Tag region -- probed
# against Renode 1.16.1: a `Python.PythonPeripheral` declared at the same
# address as an unregistered `Tag` region takes priority with no
# `sysbus Unregister` needed first (Tags are not named sysbus entries the way
# `sysbus.flash` is, so there is nothing to unregister). See
# `setup_iwdg_stub_machine` in ws500.robot for the attach line.
#
# TIMING MODEL. Genuinely time-driven, not access-reactive: a PythonPeripheral
# script only runs when something touches the bus, which is exactly wrong for
# a peripheral whose entire job is noticing the ABSENCE of a bus access. This
# uses `Machine.ScheduleAction(TimeInterval, callback, name)` -- confirmed
# present and working against Renode 1.16.1 by direct interactive probing
# (not in any shipped example; `Machine.RequestReset()` likewise) -- to arm a
# one-shot deadline every time the counter is (re)loaded. An EPOCH counter
# invalidates stale deadlines instead of cancelling them (no cancel API was
# found on `IClockSource`/`Machine`): every (re)load bumps the epoch and
# captures it in the deadline closure, so a deadline that fires after a later
# kick already moved the epoch forward is a silent no-op instead of a
# spurious reset.
#
# LSI MODEL. Nominal 40 kHz only (STM32F072 datasheet; real silicon is
# 30..60 kHz over voltage/temperature -- watchdog.c's own file header derives
# its margins against that full range, not a single number). Modelling the
# drift would need a virtual-time jitter source with no V-item backing a
# specific distribution, so this stub deliberately uses the same nominal
# figure watchdog.c's derivation comment quotes ("100.8 ms nominal") rather
# than inventing a distribution. That means this test proves the
# RequestReset() wiring and watchdog.c's register programming are correct
# together, not the worst-case timing margin across the LSI's spec range --
# that stays a bench item (renode/README.md).
#
# NOT MODELLED: the WINR early-refresh-rejects-and-resets behaviour (RM0091
# SS21.4.4, "window watchdog" style early-kick catch). watchdog.c's own 8 ms
# software floor (WDG_MIN_KICK_INTERVAL_MS) already sits comfortably above
# the hardware window's worst-case minimum (see watchdog.c's derivation
# block), so omitting it does not risk a false negative on the starvation
# test this stub exists for -- and adding it would only introduce a second,
# untested way for a timing mismatch between this model and real silicon to
# trip a CI test that is not investigating that behaviour. If the
# window-reject path ever needs its own CI coverage, it is a small addition
# to `_reload()` below (compare elapsed-since-last-reload against WINR).
IWDG_REG_KR   = 0x00
IWDG_REG_PR   = 0x04
IWDG_REG_RLR  = 0x08
IWDG_REG_SR   = 0x0C
IWDG_REG_WINR = 0x10

# TEST-ONLY control register, not part of the real IWDG (RM0091's IWDG block
# is 0x00-0x10; this is deliberately far outside that so real firmware traffic
# can never reach it by accident). ws500.robot's starvation test writes here
# directly (`sysbus WriteDoubleWord <iwdg-base>+0x100 1`, same technique the
# existing NMI fault test already uses to poke SCB->ICSR) to make the stub
# start silently swallowing KR=0xAAAA kicks -- indistinguishable, from the
# watchdog's point of view, from a hung checkpoint task that has stopped
# calling watchdog_service(), which is exactly the SS7 R1 failure mode this
# test exists to prove is caught. Pausing the CPU itself was tried first and
# rejected: probed against Renode 1.16.1, `sysbus.cpu Pause` is a transient
# pause-then-immediately-resume (used internally for atomic register access),
# not a sustained halt, so the firmware kept kicking right through it.
IWDG_TEST_REG_STARVE = 0x100

IWDG_KEY_RELOAD = 0xAAAA
IWDG_KEY_UNLOCK = 0x5555
IWDG_KEY_START  = 0xCCCC

IWDG_LSI_HZ = 40000.0                     # nominal, see LSI MODEL above
# RM0091 SS21.4.2: PR[2:0] = 0..6 -> /4../256; 7 is reserved but behaves as /256
# on ST silicon (undocumented but harmless -- never written by watchdog.c).
IWDG_PR_DIVIDERS = (4, 8, 16, 32, 64, 128, 256, 256)

import clr
clr.AddReference("Renode")
from Antmicro.Renode.Time import TimeInterval

# Module-scope state, like eeprom24c16_stub.py's backing store: `include`d
# fresh per Robot test case (new machine), but must survive the repeated
# IsInit calls a single test case's own resets produce.
if 'iwdg_state' not in globals():
    iwdg_state = {'pr': 0, 'rlr': 0xFFF, 'unlocked': False, 'started': False,
                   'epoch': 0, 'starved': False}


def _period_ms(state):
    divider = IWDG_PR_DIVIDERS[state['pr'] & 0x7]
    tick_ms = 1000.0 * divider / IWDG_LSI_HZ
    return (state['rlr'] + 1) * tick_ms


def _reload(peripheral, machine):
    """(Re)arm the countdown: bump the epoch and schedule a deadline that
    fires iff nothing reloads again before it does (see EPOCH note above)."""
    iwdg_state['epoch'] += 1
    my_epoch = iwdg_state['epoch']
    period_ms = _period_ms(iwdg_state)

    def _deadline(elapsed):
        if iwdg_state['epoch'] != my_epoch:
            return          # a later kick already superseded this deadline
        peripheral.Log(LogLevel.Info,
                        "IWDG stub: %.1f ms elapsed with no kick -- "
                        "requesting reset" % period_ms)
        machine.RequestReset()

    machine.ScheduleAction(TimeInterval.FromMilliseconds(period_ms),
                            _deadline, "iwdg_stub_deadline")


if request.IsInit:
    # POR defaults (RM0091 SS21.4): PR=0 (/4), RLR=0xFFF. NOT re-armed here --
    # a real IWDG only starts counting once KR=0xCCCC is written (or, with
    # the WDG_SW option byte this firmware deliberately does not use, from
    # reset -- SS7 R1 "deferred"), so `started` resets to False here too: a
    # fresh boot has not re-armed the watchdog yet. `starved` also resets --
    # it is a test-only condition standing in for a hung checkpoint task, not
    # a persistent hardware fault, so a clean reboot must come up healthy
    # (this is what lets the test assert the post-reset ctrl_tick loop keeps
    # running instead of starving again immediately). `epoch` is the one
    # exception -- see the EPOCH note in the file header for why it must NOT
    # reset here.
    iwdg_state['pr'] = 0
    iwdg_state['rlr'] = 0xFFF
    iwdg_state['unlocked'] = False
    iwdg_state['started'] = False
    iwdg_state['starved'] = False
elif request.IsRead:
    # SR (PVU/RVU/WVU) always reads clear: watchdog_init()'s iwdg_wait_ready()
    # bounds its poll anyway (never an unbounded wait, SS7 R6), but there is no
    # reason to make it spend that budget against a stub that can apply
    # updates instantly. All other registers are write-only on real silicon;
    # reading them back is not something watchdog.c ever does.
    request.Value = 0
elif request.IsWrite:
    if request.Offset == IWDG_REG_KR:
        key = request.Value & 0xFFFF
        if key == IWDG_KEY_START:
            iwdg_state['started'] = True
            self.Log(LogLevel.Info, "IWDG stub: started (KR=0xCCCC)")
            _reload(self, self.GetMachine())
        elif key == IWDG_KEY_UNLOCK:
            iwdg_state['unlocked'] = True
        elif key == IWDG_KEY_RELOAD:
            iwdg_state['unlocked'] = False
            if iwdg_state['starved']:
                # Test-induced starvation (IWDG_TEST_REG_STARVE): the real
                # KR=0xAAAA transaction still lands (this is not a bus fault),
                # it is just silently dropped instead of re-arming the
                # deadline -- exactly what a hung checkpoint task looks like
                # from the watchdog's side.
                self.Log(LogLevel.Info,
                          "IWDG stub: kick SUPPRESSED (test starvation active)")
            elif iwdg_state['started']:
                self.Log(LogLevel.Info, "IWDG stub: kicked (KR=0xAAAA)")
                _reload(self, self.GetMachine())
        else:
            self.Log(LogLevel.Warning,
                      "IWDG stub: unexpected KR value 0x%04x" % key)
    elif request.Offset == IWDG_REG_PR:
        if iwdg_state['unlocked']:
            iwdg_state['pr'] = request.Value & 0x7
    elif request.Offset == IWDG_REG_RLR:
        if iwdg_state['unlocked']:
            iwdg_state['rlr'] = request.Value & 0xFFF
    elif request.Offset == IWDG_REG_WINR:
        # WINR write also performs a reload on real silicon (RM0091 SS21.4.4;
        # watchdog_init()'s own comment says the same) -- and in this
        # firmware's boot sequence it is what arms the FIRST real-period
        # countdown, since PR/RLR are written between KR=0xCCCC (POR-default
        # period) and the loop's first KR=0xAAAA.
        if iwdg_state['started']:
            self.Log(LogLevel.Info, "IWDG stub: WINR write, reload")
            _reload(self, self.GetMachine())
    elif request.Offset == IWDG_TEST_REG_STARVE:
        iwdg_state['starved'] = bool(request.Value)
        self.Log(LogLevel.Info,
                  "IWDG stub: TEST starvation %s"
                  % ("ARMED" if iwdg_state['starved'] else "cleared"))
    else:
        self.Log(LogLevel.Warning,
                  "IWDG stub: write to unexpected offset 0x%x" % request.Offset)
