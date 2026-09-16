#!/usr/bin/env python3
"""Shared -asap plumbing for the scenario tests.

`WinBoloDS -asap` (src/server/servermain.c) runs game ticks back-to-back
instead of one per SERVER_TICK_LENGTH (20 ms) of wall clock.  Same-seed games
are byte-identical to timer-paced runs -- proven with -snapinterval 1 series --
because the sim advances on ticks, not on the clock; -asap only removes the
idle gap between ticks.  A 12000-tick production game drops from ~120 s to
~12 s, and ~67 s with -brain-debug on (the recorder's I/O is the new floor).

The one part of a bot game that IS wall-clock driven is the brain's capacity
tier: bot_manager.c picks it from lastThinkMs / targetMs, so a busier machine
can tier the brain down and a different tier is a different brain.  Every
scenario test that cares already neutralises this -- -brain-no-budget-kill
hands out the 1000 ms slow-mo budget, which pins the derived tier at 10, and
sea_pills additionally pins WINBOLO_BRAIN_TIER=10.  The handful of tests
without either (aim, boat_diagonal, cliff_staircase, corner_cut, seek_trees,
respawn_loadout) are the ones to re-check under --no-asap if a result ever
looks pacing-shaped; measured 2026-09-02 they are identical in both modes.

Every scenario test here therefore runs with -asap by DEFAULT.  Two ways back
to timer pacing when you want the old behaviour (e.g. reproducing a report from
a live game, or checking that a result is not an artefact of the pacing):

    python tests/take_cover_test.py --no-asap      # this run only
    WINBOLO_ASAP=0 python tests/take_cover_test.py # whole shell / CI job

The CLI switch wins over the environment variable.  `--asap` is accepted too so
a WINBOLO_ASAP=0 shell can force one test back on.

Test-side usage (three lines):

    from asap import take_asap_flag, asap_args, pacing_line
    ...
    use_asap = take_asap_flag(args)   # strips --asap/--no-asap from `args`
    print(pacing_line())              # header line: which pacing ran
    cmd = [ds, ...] + asap_args()     # [] or ["-asap"]

take_asap_flag() records the decision in this module, so asap_args() and
pacing_line() need no plumbing through run()/run_one() signatures.

WHAT IS NOT SAFE UNDER -asap (things to keep out of a test):
  * wall-clock deltas used as a stand-in for game time -- 6000 ticks is no
    longer ~120 s.  Measure ticks (snapjson "tick", print2 "t=") instead.
  * sleeping until the game is expected to have reached tick N.
  * polling -snapjson / a session dir *while* the server runs; under -asap the
    whole game can be over before the first poll.
  * expecting the brain-debug recorder's 15-minute wall-clock block roll
    (BRAINDBG_BLOCK_MS in src/server/server_lifecycle.c) to split a long run
    into debug_sessions/<ts>_1_<label>, <ts>_2_<label>, ...  Under -asap even
    a 12000-tick run finishes inside one block, so only `_1_` exists.  Every
    test here globs `*<label>*` and takes the newest, which is correct either
    way, but a test that hard-codes `_2_` would break.  (The same wall clock
    names the dir to the second, so back-to-back runs of one test are now
    close enough together that this only just still holds -- if a test ever
    gets fast enough to start twice inside one second, two runs would land in
    the same session dir.)
  * anything outside these tests that ends the game on a wall-clock DURATION
    rather than a tick count -- tests/bots/water_stuck_regression.sh runs the
    server for N seconds and then kills it, which under -asap would be a much
    longer game.  It is a shell harness, left timer-paced.
Timeouts on the server subprocess are safe to leave alone: -asap only makes
the run finish sooner, so an existing wall-clock timeout gains slack.
"""

import os

__all__ = ["take_asap_flag", "asap_args", "asap_enabled", "asap_mode",
           "pacing_line"]


def _env_default():
    """WINBOLO_ASAP unset -> on.  0/no/off/false -> off.  Anything else -> on."""
    v = os.environ.get("WINBOLO_ASAP")
    if v is None:
        return True
    return v.strip().lower() not in ("0", "no", "off", "false", "")


_USE_ASAP = _env_default()


def take_asap_flag(args):
    """Strip --asap / --no-asap out of `args` IN PLACE and return the decision.

    `args` is the usual `sys.argv[1:]` list every test hand-parses; removing the
    switches here keeps the tests that reject unknown arguments happy.  Call it
    before the test's own argument loop.
    """
    global _USE_ASAP
    _USE_ASAP = _env_default()
    kept = []
    for a in args:
        if a == "--no-asap":
            _USE_ASAP = False
        elif a == "--asap":
            _USE_ASAP = True
        else:
            kept.append(a)
    args[:] = kept
    return _USE_ASAP


def asap_enabled():
    return _USE_ASAP


def asap_args():
    """The server flags to splice into the WinBoloDS command line."""
    return ["-asap"] if _USE_ASAP else []


def asap_mode():
    return "asap" if _USE_ASAP else "timer"


def pacing_line(prefix="  "):
    """One-line header saying which pacing this run used."""
    if _USE_ASAP:
        return (prefix + "pacing: -asap (ticks run back-to-back; "
                "--no-asap or WINBOLO_ASAP=0 for 20 ms/tick)")
    return prefix + "pacing: timer-paced (20 ms/tick, the live-game rate)"
