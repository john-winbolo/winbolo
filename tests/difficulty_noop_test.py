#!/usr/bin/env python3
"""Difficulty no-op invariant (GoalHunter_1.7).

The difficulty feature must leave HARD (and preset=keel) bit-for-bit identical
to the pre-feature brain. Two things guarantee that, and this test asserts both
statically so a later default tweak can't silently break it (the file's own rule
is "a knob whose default equals its keel value needs no keel pin" -- so instead
of a PRESETS.keel pin, we assert the default IS the no-op here):

  1. MODE_LEVELS.default.hard is an EMPTY table (Hard applies zero overrides).
  2. Every NEW handicap knob's module default equals its no-op value, so even
     before any bundle applies, the knob changes nothing.

Run: C:\\Python310\\python.exe tests/difficulty_noop_test.py
"""
import re
import sys
from pathlib import Path

CONST = Path(__file__).resolve().parent.parent / "brains" / "GoalHunter_1.7" / "constants.lua"

# knob -> expected no-op default (the value at which the knob does nothing)
NOOP = {
    "BLITZ_ENABLED": "true",
    "AIM_ERROR_BRADS": "0",
    "FIRE_HOLD_TICKS": "0",
    "REACTION_DELAY_TICKS": "0",
    "AHEAD_PILL_FRAC": "0",
    "AHEAD_BASE_FRAC": "0",
    "BEHIND_ATTACK_MULT": "1.0",
    "AHEAD_BLITZ_ONLY": "false",
    "OUTNUMBERED_DISENGAGE": "false",
}


def main():
    src = CONST.read_text(encoding="utf-8", errors="replace")
    errs = []

    # 1. every new knob's default == its no-op
    for knob, want in NOOP.items():
        m = re.search(r"(?m)^\s*M\." + re.escape(knob) + r"\s*=\s*([^\s;-]+)", src)
        if not m:
            errs.append(f"{knob}: not declared as M.{knob} = <default> in constants.lua")
            continue
        got = m.group(1).rstrip(",")
        # normalise numeric forms (0 == 0.0, 1 == 1.0)
        def num(x):
            try:
                return float(x)
            except ValueError:
                return None
        if num(want) is not None and num(got) is not None:
            if num(got) != num(want):
                errs.append(f"{knob}: default {got} != no-op {want}")
        elif got != want:
            errs.append(f"{knob}: default {got!r} != no-op {want!r}")

    # 2. MODE_LEVELS.default.hard must be empty {}
    #    Find the default = { ... hard = {...} ... } block and pull hard's braces.
    mh = re.search(r"hard\s*=\s*\{(.*?)\}", src, re.S)
    if not mh:
        errs.append("MODE_LEVELS: no `hard = { ... }` entry found")
    else:
        body = mh.group(1).strip()
        # strip Lua comments/whitespace; anything left is a real override
        body_no_comments = re.sub(r"--.*", "", body).strip()
        if body_no_comments:
            errs.append(f"MODE_LEVELS.default.hard is NOT empty -- Hard would not be bit-for-bit: {body_no_comments[:120]!r}")

    if errs:
        print("FAIL (difficulty no-op invariant):")
        for e in errs:
            print("  - " + e)
        return 1
    print("PASS: hard bundle empty and all %d new-knob defaults are no-ops "
          "(Hard = pre-feature brain, no keel pin needed)." % len(NOOP))
    return 0


if __name__ == "__main__":
    sys.exit(main())
