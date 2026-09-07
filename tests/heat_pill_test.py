#!/usr/bin/env python3
"""heat a FRIENDLY pillbox mid-fight -- attack_tank interrupts the duel to make
our own pill angry, then goes back to the duel (GoalHunter 1.7, 2026-09-06).

THE RULE
--------
While the bot is on an `attack_tank` goal against enemy tank E, a friendly pill
P that already sits CLOSER to E than we do is a second gun in position -- but
only if it is angry.  So the bot breaks off for a few ticks, shoots P exactly as
many times as it takes to drive P's reload period to maximum, and resumes the
fight.  attack.lua's heat_pill_select / heat_pill_steer / heat_pill_fire do the
deciding; steering.lua's tank_combat_steer calls it AFTER both of its disengage
returns (the pillbox-crossfire break-off and the armour/shells flee check) and
BEFORE the close/engage split, so a volley can neither start nor continue on a
tick the existing code has already decided we must evade.

WHAT MAKES THIS TESTABLE AT ALL: A PILL'S ANGER IS A NUMBER THE ENGINE KEEPS
---------------------------------------------------------------------------
`speed` IS the reload period.  PILLBOX_ATTACK_NORMAL (100) is calm,
PILLBOX_MAX_FIRERATE (6) is as angry as a pill gets
(src/bolo/internal/pillbox.h:46-48).  It is not additive: every DAMAGING shell
does `speed /= 2` floored at 6 (src/bolo/pillbox.c:504-511), so the ladder from
calm is

        100 -> 50 -> 25 -> 12 -> 6          exactly FOUR hits

and our own shell counts, because shells.c:658-661 calls pillsDamagePos on any
shell landing on a pill tile with no ownership test whatsoever -- which is also
why each hit costs the pill an armour point and why the rule has an HP floor.

The scenario sidecar reads that field straight off the engine
(game.pill(n).speed, src/server/scenario.c:262) and writes one trace row per
CHANGE.  So the headline assertion of arena A is not "the brain printed that it
heated a pill"; it is "the ENGINE's reload period walked 100 -> 50 -> 25 -> 12
-> 6 while the pill lost exactly four armour, and the control run one token
away shows the same pill at 15 armour and speed 100".

THE HEALTH CAP (Andrew, 2026-09-06: "proportionally less shots to heat up
depending how hurt the pill is").  Every heat shell costs the pill an armour
point, so how many the volley may fire is capped by how much armour it has to
spend -- attack.lua heat_allowed_shots, mirrored in the generator:

    allowed = round(HEAT_MAX_HITS x (hp - MIN_HP) / (PILLS_MAX_HEALTH - MIN_HP))
    hp    15  14  13  12  11  10   9   8   7   6   5   4
    allow  4   4   3   3   2   2   2   1   1   0   0   0

and the volley fires min(need, allowed).  This REPLACED the old per-shot
"hp >= 5" test, so it has two visible outcomes and the arenas take one each:
allowed == 0 is `SKIP:hp` (arena B), and a partial allowance is the exit reason
`hp_cap` (arena B2) -- the volley finished its quota but stopped short of max
heat.  HEAT_MAX_HITS is no longer a knob either: it is the length of the
engine's ladder, a module-local constant.

BOTH RULES ARE STILL IN THE BUILD, so the new one can be benched against the
one it replaced.  C.ATTACK_TANK_HEAT_CAP_MODE is a string:

    "proportional"  the default, the table above
    "floor"         no proportional cap; the only limit is that a shot must
                    not take the pill under MIN_HP, so allowed = hp - MIN_HP
                    (hp 15/10 -> 4, 8 -> 3, 7 -> 2, 6 -> 1, 5 -> 0), and a
                    volley cut short by it exits `hp_floor`, not `hp_cap`.
                    Entry refuses with SKIP:hp only below the floor -- a pill
                    exactly AT 5 is admitted and simply affords nothing, which
                    reads SKIP:no_shots_needed.

Every HEAT_PILL line carries the mode as a `cap{...}` chip, and every arena
asserts the chip matches the mode it was written for -- so a cfg token that
silently failed to reach the brain fails the arena instead of quietly changing
what it measures.  Arenas B and F are the bench: SAME GROUND, SAME 6-ARMOUR
PILL, one token apart.

The BRAIN never sees `speed`.  It carries a proxy, world.lua's pill.anger in
[0,1]: +C.PILL_ANGER_BUMP (0.3333) per OBSERVED armour drop, decaying
1/C.PILL_ANGER_DECAY (3000) a brain tick.  One observed drop == one halving, so
shots_needed = 4 - round(anger / 0.3333).  Arena D is built on the one place
the two can be pulled apart: a health RISE never bumps, so the sidecar can bump
the PROXY to saturation by knocking one armour off and putting it straight back
-- three times -- while the ENGINE's speed never moves off 100.

EIGHT ARENAS (tests/generate_heat_pill_map.py builds the ground; its docstring
carries the geometry and why every tile of it is where it is).  Our tank in a
sealed room, a scripted IDLE enemy on a pond that is never filled in so it
respawns on the same tile forever, and one friendly pill.

  A   ENTER, FIRE, MAX, RESUME.  At least one `HEAT_PILL ... -> ENTER`, a run
      of HEAT_SHOT lines counting the pill's OBSERVED armour drop (not our key
      presses), a `HEAT_EXIT ... -> maxed`, the engine's speed ladder and the
      four-point armour drop in the sidecar trace, and the fight with E picked
      straight back up afterwards.
  AK  THE CONTROL, one token different (cfg=ATTACK_TANK_HEAT_PILL=false, which
      is what PRESETS.keel sets).  Same ground, same seed.  No HEAT_PILL and no
      HEAT_SHOT anywhere, and the ENGINE saw the pill at 15 armour and speed
      100 for the whole run.  Without this, arena A's ladder could be any
      stray shell.
  B   THE CAP AT ZERO.  P written into the map at 6 armour -- the HIGHEST
      health whose allowance is still 0, so the arena sits on the edge of the
      cap and not on a nearly-dead pill.  `SKIP:hp`, never an ENTER, and the
      engine's reload period flat at 100.
  B2  THE CAP IN BETWEEN, and the reason the rule exists.  Arena A's ground
      with the pill written in at 10 armour: `ENTER` with need{4} allow{2 of 4}
      shots_needed{2}, two hits, `HEAT_EXIT ... -> hp_cap` rather than `maxed`,
      and the ENGINE's reload period walking 100 -> 50 -> 25 and STOPPING --
      two halvings short of PILLBOX_MAX_FIRERATE, armour 10 -> 8, and never
      below 25 for the whole run.  Beside arena A's full 100 -> 50 -> 25 -> 12
      -> 6 on identical ground, that is the cap, measured off the engine.
  C   NOT CLOSER.  P at the far west end, further from E than the tank can get
      from anywhere in the room.  `SKIP:not_closer`, never an ENTER.
  D   ALREADY HOT.  The sidecar saturates the brain's proxy with drop-and-
      restore pairs and keeps re-topping it.  `SKIP:already_hot`, never an
      ENTER -- and the trace shows the engine's speed flat at 100 the whole
      time, which is the proxy and the truth visibly disagreeing.
  E   NO LINE OF SIGHT.  P sealed in a pocket behind three rows of BUILDING.
      `SKIP:no_los`, never an ENTER.
  F   THE RULE THE CAP REPLACED, on arena B's ground and arena B's pill with
      cfg=ATTACK_TANK_HEAT_CAP_MODE=floor -- one token between the two runs.
      Where the proportional cap refuses 6 armour outright, the floor rule
      spends it: `ENTER` with cap{floor} need{4} allow{1 of 4}
      shots_needed{1}, one hit, `HEAT_EXIT ... -> hp_floor`, and the ENGINE's
      reload period going 100 -> 50 and stopping while the armour goes 6 -> 5.
      Left standing on exactly MIN_HP, which is the whole of the older rule.

Every outcome is read from print2 or from the sidecar's ENGINE-side trace --
never from the brain's opinion of itself.

Usage: python heat_pill_test.py [--variant A|AK|B|B2|C|D|E|F|all] [--ticks N]
                                [--build DIR] [--no-asap]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import os
import re
import subprocess
import sys
import time
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
# The ROOT copy, never opt/: opt/ is stripped and carries none of the
# BRAIN_DEBUG_MODE print2 lines this test reads.
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
import generate_heat_pill_map as G   # noqa: E402

PORTS = {"A": 50360, "AK": 50361, "B": 50362, "B2": 50366, "C": 50363,
         "D": 50364, "E": 50365, "F": 50367}
# ENGINE ticks.  The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks.  The run
# needs room for our tank to come ashore (it sits on its pond a while), find E,
# and then for a volley -- which is 4 shots at TANK_RELOAD_TIME (13) engine
# ticks apart plus aiming, i.e. under a hundred ticks once it starts.
# ARENA B2 IS DELIBERATELY SHORT.  Its volley leaves the pill on 8 armour,
# which still buys 1 more halving, and its proxy anger at 0.66, under
# C.ATTACK_TANK_HEAT_MAX_FRAC -- so once C.ATTACK_TANK_HEAT_RETRY_TICKS (150
# brain ticks) has run out from the exit at brain tick ~157, the bot would
# quite correctly open a SECOND volley and walk the engine's period on from 25
# to 12.  That is right behaviour and wrong evidence: the arena is about where
# ONE capped volley stops.  600 engine ticks is 300 brain ticks, against a
# re-entry no earlier than brain 307.  (check_ladder's "never went below 25"
# would catch it anyway; the budget is so the pass is not a coincidence.)
# THE hp-6 PAIR (B and F) ARE SHORT FOR A DIFFERENT REASON.  On this ground the
# bot eventually decides its own back-line pillbox is badly placed and SHOOTS
# IT DOWN to move it -- goal capture_pill, substate reposition_shoot, logging
# `reposition pill#0@(126,125) cat=back` and measured starting at brain tick
# ~300 in both.  Everywhere else that is suppressed with
# cfg=PILL_REPOSITION_ENABLED=false, but B and F need three other tokens
# already and a fourth does not fit in BotInitSlot.arg's 127 bytes.  So instead
# the pair stops at 500 engine ticks -- 250 brain ticks, a fifty-tick margin --
# and each has all its evidence by brain tick 142.  A repositioning bot would
# be caught anyway: it drives the ENGINE's reload period down, which is
# precisely what both arenas assert never happens.
TICKS = {"A": 4000, "AK": 4000, "B": 500, "B2": 600, "C": 3000, "D": 3000,
         "E": 1200, "F": 500}

ARG_MAX = 127                       # BotInitSlot.arg[128], luabrainshandler.h

# WHY THE cfg= TOKENS -- both are in EVERY arena, so the single token that ever
# differs is the rule itself.
#   PILL_REPOSITION_ENABLED=false -- without it the bot can decide its own pill
#     is badly placed and SHOOT IT DOWN to move it, which forges exactly the
#     armour drops arenas A and AK are measuring.  (The geometry already keeps
#     a friendly base inside PILL_FIRE_RANGE of every pill, which is the real
#     guard; this is the belt to that pair of braces.)
#   HEAT_REQUIRE_ENEMY_RANGE=0 -- defend_pill has its OWN, much older heat-up
#     rung (C.HEAT_PILL_SHOTS, steering.lua defend_pill_steer) that fires
#     shells at a friendly pill for a different reason.  It needs a hostile
#     tank visible within HEAT_REQUIRE_ENEMY_RANGE (10) of the pill, and this
#     arena has one 4.1 tiles away.  Left on, it could produce the very ladder
#     arena A is here to attribute to attack_tank, and could break arena AK's
#     silence.  Zero makes that rung unreachable and leaves the new one alone.
#
# Arena B carries a THIRD token, BUILDER_POOL_TREES_TOPUP=99, which is the
# price in wood of one top-up errand: 99 against a tank's 40 trees means the
# pool refuses the row on tree_reserve and the man never walks.  It is there
# because THE BOT REPAIRS THE PILL ARENA B IS BUILT ON.  Its pill is written
# into the map at 4 armour -- one under the floor -- and the engine trace of
# the first run read `286 126 125 15` at sim tick 286: the builder pool had
# topped it up and the gate was looking at a 15-hp pill.  The sidecar puts the
# armour straight back down every tick, and the ENGINE-side trace shows a flat
# 4 for the whole run because of it -- but the brain samples the world once a
# frame, and a repair that lands and is undone inside one tick is still a tick
# on which the brain saw 15 and said ENTER.  The only way to close that window
# is for the repair never to happen.
COMMON = "cfg=PILL_REPOSITION_ENABLED=false;cfg=HEAT_REQUIRE_ENEMY_RANGE=0"
# ARENAS B AND F STAND ON THE SAME GROUND WITH THE SAME 6-ARMOUR PILL AND
# DIFFER BY EXACTLY ONE TOKEN -- the cap mode -- because that contrast IS the
# pair: the proportional cap refuses the pill outright, while the floor rule it
# replaced fires once and leaves it standing on the floor.  Keeping them one
# token apart costs them PILL_REPOSITION_ENABLED=false, which no longer fits;
# TICKS explains what is done about that instead.
HP6 = "cfg=HEAT_REQUIRE_ENEMY_RANGE=0;cfg=BUILDER_POOL_TREES_TOPUP=99"
TOKENS = {
    "A": COMMON,
    "AK": COMMON + ";cfg=ATTACK_TANK_HEAT_PILL=false",
    "B": HP6,
    "B2": COMMON + ";cfg=BUILDER_POOL_TREES_TOPUP=99",
    "C": COMMON,
    "D": COMMON,
    "E": COMMON,
    "F": HP6 + ";cfg=ATTACK_TANK_HEAT_CAP_MODE=floor",
}
GAMETYPE = "open"

# ── print2 lines (attack.lua heat_pill_select / heat_pill_steer /
#    heat_pill_fire).  Every line is prefixed "<file>\t<lineno>\t[Nms] ", so
#    these are used with re.search, never re.match. ─────────────────────────
#
# HEAT_PILL t=114 pill=#0 d_pill_enemy{3.2} d_us_enemy{5.0} range{2.1/7.0}
#   los{true} hp{15/5} anger{0.00/0.75} cap{proportional} need{4}
#   allow{4 of 4} shots_needed{4} -> ENTER
#     cap   = C.ATTACK_TANK_HEAT_CAP_MODE, "proportional" or "floor"
#     need  = shells to reach PILLBOX_MAX_FIRERATE from the anger proxy
#     allow = the HEALTH CAP under that mode, "{allowed of HEAT_MAX_HITS}"
#     shots_needed = min(need, allow) -- what actually gets fired
HEAT_PILL_RE = re.compile(
    r"HEAT_PILL t=(\d+) pill=#(\S+) d_pill_enemy\{([\d.]+)\} "
    r"d_us_enemy\{([\d.]+)\} range\{([\d.]+)/([\d.]+)\} los\{(\S+)\} "
    r"hp\{(\d+)/(\d+)\} anger\{([\d.]+)/([\d.]+)\} cap\{(\w+)\} "
    r"need\{(\d+)\} allow\{(\d+) of (\d+)\} shots_needed\{(\d+)\} "
    r"-> (\S+)")
# Field indices into a HEAT_PILL_RE tuple, named so a future field cannot
# silently shift a check onto the wrong column.
HP_T, HP_ID, HP_DPE, HP_DUE, HP_RNG, HP_MAXRNG, HP_LOS = 0, 1, 2, 3, 4, 5, 6
HP_HP, HP_MINHP, HP_ANGER, HP_MAXFRAC, HP_CAP = 7, 8, 9, 10, 11
HP_NEED, HP_ALLOW, HP_MAXHITS, HP_SHOTS, HP_VERDICT = 12, 13, 14, 15, 16
# HEAT_SHOT t=613 pill=#0 0/4 anger=0.00 hp=15 corr=+0 fire=true
HEAT_SHOT_RE = re.compile(
    r"HEAT_SHOT t=(\d+) pill=#(\S+) (\d+)/(\d+) anger=([\d.]+) hp=(\d+) "
    r"corr=([+-]\d+) fire=(\w+)")
# HEAT_EXIT t=188 pill=#0 hits{4/4} need{4} fired{4} hp{11} anger{1.00} -> maxed
HEAT_EXIT_RE = re.compile(
    r"HEAT_EXIT t=(\d+) pill=#(\S+) hits\{(-?\d+)/(\d+)\} need\{(\d+)\} "
    r"fired\{(-?\d+)\} hp\{(\S+)\} anger\{([\d.]+)\} -> (\S+)")
HX_T, HX_ID, HX_HITS, HX_SHOTS, HX_NEED = 0, 1, 2, 3, 4
HX_FIRED, HX_HP, HX_ANGER, HX_REASON = 5, 6, 7, 8
# steering.lua tank_combat_steer, the evidence the fight is running.
TANK_TARGET_RE = re.compile(
    r"TANK_TARGET t=(\d+) id=(\S+) ghost=(\w+) @\((\d+),(\d+)\)")
TANK_ENGAGE_RE = re.compile(r"TANK_ENGAGE t=(\d+) tgt#(\S+?) ")
TANK_FIRE_RE = re.compile(r"TANK_FIRE t=(\d+) id=(\S+) ghost=(\w+) ")

# Every reason heat_pill_select can print, so a typo in either file shows up as
# an unknown reason rather than as a silent miss.
SKIP_REASONS = ("not_closer", "out_of_range", "hp", "already_hot",
                "no_shots_needed", "retry_wait", "low_shells", "no_los")
# `hp_cap` and `hp_floor` are the SAME event under the two cap modes -- the
# volley fired everything its armour allowed and stopped short of max heat --
# named for whichever rule did the stopping.  Only one of them can appear in
# any given run, and check_volley asserts which.
EXIT_REASONS = ("maxed", "hp_cap", "hp_floor", "misses", "out_of_shells",
                "pill_gone", "out_of_range", "lost_los", "timeout")

# The arena each SKIP variant is built to produce, and how many armour points
# a full volley costs the pill.
SKIP_EXPECT = {"B": "hp", "C": "not_closer", "D": "already_hot",
               "E": "no_los"}


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_trace(build_dir, variant):
    """The sidecar's ENGINE-side view of our pill, one row per CHANGE:

        [(sim_tick, x, y, armour, owner, in_tank, speed), ...]

    `speed` IS the engine's anger (scenario.c:262): PILLBOX_ATTACK_NORMAL (100)
    calm down to PILLBOX_MAX_FIRERATE (6).  Asking the engine, not the brain,
    is the whole point of the column."""
    path = build_dir / f"heat_pill_{variant}_trace.log"
    seq = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 7:
                seq.append(tuple(int(p) for p in parts[:7]))
    return seq


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if ds is None:
        # Another agent may be relinking it right now.  Wait once, then give up.
        time.sleep(60)
        ds = find_ds(build_dir)
    if ds is None:
        return None, (f"WinBoloDS not found under {build_dir} (waited 60s in "
                      f"case a build was relinking it)")
    subprocess.run([sys.executable,
                    str(HERE / "generate_heat_pill_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"heatpill_{variant}"
    final = HERE / f"heat_pill_{variant}_final.json"
    stderr = HERE / f"heat_pill_{variant}_stderr.txt"
    trace = build_dir / f"heat_pill_{variant}_trace.log"
    for p in (final, stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    tokens = TOKENS[variant]
    if len(tokens) > ARG_MAX:
        return None, (f"the -bot-init token string is {len(tokens)} bytes, over "
                      f"the {ARG_MAX}-byte BRAIN_INIT_ARG limit -- it would be "
                      f"truncated mid-token and the last cfg= silently ignored:"
                      f"\n  {tokens}")
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"heat_pill_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby",
           "-gametype", GAMETYPE,
           "-bots", "1", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(900, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000])
    ours = sess / "print2_bot0.log"
    if not ours.exists():
        return None, f"no print2_bot0.log under {sess}"
    return sess, ours.read_text(errors="ignore")


def lua_errors(text):
    return [ln for ln in text.splitlines()
            if "attempt to " in ln or "stack traceback" in ln
            or (".lua:" in ln and "Error" in ln)][:5]


def dumps(text, needle, n=6, tail=False):
    lines = [l.strip() for l in text.splitlines() if needle in l]
    for ln in (lines[-n:] if tail else lines[:n]):
        print("   " + ln)


def reason_histogram(rows):
    """{reason: count} over the verdict column of HEAT_PILL_RE matches.

    NOTE the counts are DECISIONS PRINTED, not ticks: heat_pill_select keeps
    state._heat_pill_seen and prints only when a pill's
    reason|shots_needed|los key changes, so a gate that refuses for 400 ticks
    running shows up once.  Nothing here may assert a count proportional to
    the length of the run."""
    out = {}
    for r in rows:
        v = r[HP_VERDICT]
        out[v] = out.get(v, 0) + 1
    return out


def show_trace(trace, n=14):
    for row in trace[:n]:
        print(f"       t={row[0]:<6} armour={row[3]:<3} owner={row[4]:<4} "
              f"in_tank={row[5]} speed={row[6]}")
    if len(trace) > n:
        print(f"       ... {len(trace) - n} more row(s)")


def combat_running(text, after=None):
    """Ticks on which tank_combat_steer had a REAL (non-ghost) target."""
    out = []
    for (t, _id, ghost, _mx, _my) in TANK_TARGET_RE.findall(text):
        if ghost == "false" and (after is None or int(t) > after):
            out.append(int(t))
    return out


# ── arenas A and B2: enter, fire, stop, resume ───────────────────
# The two differ ONLY in the pill's starting armour, and therefore only in the
# health cap: A's pill is at 15 and buys all four halvings, so its volley ends
# `maxed` with the engine at PILLBOX_MAX_FIRERATE; B2's is at 10 and buys two,
# so the SAME volley ends `hp_cap` with the engine stopped at 25.  One checker,
# parameterised on the cap, so the contrast is asserted and not merely
# described.
def check_volley(variant):
    _pill, hp0 = G.pill_of(variant)
    mode = G.cap_mode_of(variant)              # proportional | floor
    allow = G.heat_allowed_shots(hp0, mode)    # the health cap at entry
    need = G.HEAT_MAX_HITS                     # a CALM pill needs all four
    shots = min(need, allow)                   # what the volley may fire
    # Cut short, the exit is named for the rule that cut it: `hp_cap` under the
    # proportional cap, `hp_floor` under the rule it replaced.
    want_reason = ("maxed" if shots >= need
                   else ("hp_floor" if mode == G.FLOOR else "hp_cap"))
    floor = G.ladder_speed(shots)              # where the engine's speed stops

    def _check(sess, text, build_dir):
        decisions = HEAT_PILL_RE.findall(text)
        hshots = HEAT_SHOT_RE.findall(text)
        exits = HEAT_EXIT_RE.findall(text)

        # 0. THE ARENA IS RUNNING.  heat_pill_select is only reached from
        #    tank_combat_steer, past both disengage returns, with a REAL target
        #    -- so a HEAT_PILL line at all is proof the fight got that far.
        if not decisions:
            print("FAIL (0): not one HEAT_PILL line, so the bot never reached "
                  "the heat block: either attack_tank never won a replan, or "
                  "one of tank_combat_steer's two disengage returns fired every "
                  "tick, or the only target was a GHOST. What it did instead:")
            dumps(text, "GOAL ", 8)
            dumps(text, "eval_attack_tank", 6)
            return 1
        hist = reason_histogram(decisions)
        unknown = [k for k in hist
                   if k != "ENTER" and k.replace("SKIP:", "") not in SKIP_REASONS]
        if unknown:
            print(f"FAIL (0): heat_pill_select printed verdict(s) {unknown} "
                  f"that are not ENTER or one of the eight documented skips.")
            return 1
        print(f"  0 OK: the heat block ran on {len(decisions)} decision(s): "
              + ", ".join(f"{k}x{v}" for k, v in sorted(hist.items())))

        # 1. IT OPENED, AND IT ASKED FOR THE RIGHT NUMBER OF SHELLS.  need is
        #    the anger proxy's answer, allow is the health cap, shots_needed is
        #    the min of the two -- which is where the new rule is visible.
        enters = [d for d in decisions if d[HP_VERDICT] == "ENTER"]
        if not enters:
            print("FAIL (1): every decision was a SKIP -- the gate never opened "
                  "on ground built for it. The reasons, with the numbers that "
                  "produced them:")
            dumps(text, "HEAT_PILL", 8)
            return 1
        e0 = enters[0]
        print(f"  1 OK: {len(enters)} ENTER(s); the first at t={e0[HP_T]} on "
              f"pill #{e0[HP_ID]} -- d(P,E)={e0[HP_DPE]} < "
              f"d(us,E)={e0[HP_DUE]}, {e0[HP_RNG]}/{e0[HP_MAXRNG]} tiles of "
              f"gun, los={e0[HP_LOS]}, hp {e0[HP_HP]}, anger "
              f"{e0[HP_ANGER]}<{e0[HP_MAXFRAC]}, cap={e0[HP_CAP]}, "
              f"need={e0[HP_NEED]} "
              f"allow={e0[HP_ALLOW]} of {e0[HP_MAXHITS]} -> "
              f"shots_needed={e0[HP_SHOTS]}")
        got = (e0[HP_CAP], int(e0[HP_HP]), int(e0[HP_NEED]),
               int(e0[HP_ALLOW]), int(e0[HP_MAXHITS]), int(e0[HP_SHOTS]))
        want = (mode, hp0, need, allow, G.HEAT_MAX_HITS, shots)
        if got != want:
            print(f"FAIL (1): the first ENTER reads cap={got[0]} hp={got[1]} "
                  f"need={got[2]} allow={got[3]} of {got[4]} "
                  f"shots_needed={got[5]}. This arena stages a CALM pill on "
                  f"{hp0} armour in {mode} mode, whose allowance is "
                  f"{allow} -- so it should read cap={want[0]} hp={want[1]} "
                  f"need={want[2]} allow={want[3]} of {want[4]} "
                  f"shots_needed={want[5]}.")
            if got[0] != want[0]:
                print(f"         The cap chip is the giveaway: the token "
                      f"cfg=ATTACK_TANK_HEAT_CAP_MODE={mode} did not reach the "
                      f"brain (check the run's `[cfg]` echo line).")
            return 1

        # 2. IT FIRED, and it counted HITS rather than key presses.
        volley = [x for x in hshots if int(x[0]) >= int(e0[HP_T])]
        if not volley:
            print(f"FAIL (2): ENTER at t={e0[HP_T]} but not one HEAT_SHOT after "
                  f"it -- the volley never produced a firing tick.")
            return 1
        aimed = [x for x in volley if x[7] == "true"]
        print(f"  2 OK: {len(volley)} HEAT_SHOT tick(s) after the first ENTER, "
              f"{len(aimed)} of them with the gun actually on the pill; the "
              f"hits/shots column walks "
              + " ".join(sorted({f"{x[2]}/{x[3]}" for x in volley},
                                key=lambda z: int(z.split('/')[0]))))

        # 3. IT STOPPED, AND FOR THE RIGHT REASON.  `maxed` means the pill
        #    reached PILLBOX_MAX_FIRERATE; `hp_cap` means its armour ran the
        #    volley short of that.  Which one appears is the whole difference
        #    between these two arenas.
        bad = [x for x in exits if x[HX_REASON] not in EXIT_REASONS]
        if bad:
            print(f"FAIL (3): HEAT_EXIT printed reason(s) "
                  f"{[b[HX_REASON] for b in bad]} that heat_pill_steer cannot "
                  f"produce.")
            return 1
        ends = [x for x in exits if x[HX_REASON] == want_reason]
        if not ends:
            why = sorted({x[HX_REASON] for x in exits}) or ["(none at all)"]
            print(f"FAIL (3): no `HEAT_EXIT ... -> {want_reason}`. The volley "
                  f"started and then ended some other way: {', '.join(why)}.")
            if "timeout" in why:
                print("         `timeout` is C.ATTACK_TANK_HEAT_MAX_TICKS: the "
                      "tank never got its aim inside the fire gate before the "
                      "volley's ceiling. Rotating onto the pill is the slow "
                      "part.")
            if want_reason == "maxed" and ("hp_cap" in why or "hp_floor" in why):
                print("         A health stop on a pill this arena stages at "
                      "FULL armour means the allowance arithmetic and this "
                      "test disagree about the table.")
            if want_reason != "maxed" and "maxed" in why:
                print(f"         `maxed` means the volley ran the full ladder "
                      f"on a pill whose armour was only supposed to buy "
                      f"{shots} of it -- the {mode} allowance did not bite.")
            if want_reason == "hp_floor" and "hp_cap" in why:
                print("         `hp_cap` is the PROPORTIONAL mode's name for "
                      "this stop, so the floor token did not take effect.")
            if want_reason == "hp_cap" and "hp_floor" in why:
                print("         `hp_floor` is the OLD rule's name for this "
                      "stop -- this arena should be running the default "
                      "proportional cap.")
            if not exits:
                print("         NO HEAT_EXIT AT ALL, which is its own finding: "
                      "a volley whose GOAL is replaced (a replan, or the target "
                      "dying) leaves goal._heat_pid behind with it and vanishes "
                      "without an exit line and without stamping the retry "
                      "latch.")
            dumps(text, "HEAT_EXIT", 8)
            return 1
        m0 = ends[0]
        if int(m0[HX_HITS]) < int(m0[HX_SHOTS]):
            print(f"FAIL (3): the `{want_reason}` exit at t={m0[HX_T]} reports "
                  f"hits{{{m0[HX_HITS]}/{m0[HX_SHOTS]}}} -- fewer hits than "
                  f"shells asked for, so the reason and the hit counter "
                  f"disagree.")
            return 1
        if (int(m0[HX_SHOTS]), int(m0[HX_NEED])) != (shots, need):
            print(f"FAIL (3): the exit at t={m0[HX_T]} reports "
                  f"hits{{../{m0[HX_SHOTS]}}} need{{{m0[HX_NEED]}}}; this "
                  f"arena's volley is {shots} shell(s) against a need of "
                  f"{need}.")
            return 1
        print(f"  3 OK: HEAT_EXIT t={m0[HX_T]} pill #{m0[HX_ID]} hits "
              f"{m0[HX_HITS]}/{m0[HX_SHOTS]} against a need of {m0[HX_NEED]}, "
              f"{m0[HX_FIRED]} shell(s) fired, pill left on hp {m0[HX_HP]}, "
              f"proxy anger {m0[HX_ANGER]} -> {want_reason} "
              f"({len(exits)} exit(s) in the run: "
              + ", ".join(sorted({x[HX_REASON] for x in exits})) + ")")

        # 4. THE ENGINE AGREES.  Everything above is the brain's own account;
        #    this is the reload period the engine keeps, read straight off
        #    pillbox memory by the sidecar.
        rc = check_ladder(read_trace(build_dir, variant), "4", shots, floor)
        if rc:
            return rc

        # 5. "A FEW TICKS", NOT "INSTEAD OF THE FIGHT".  The rule is an
        #    interrupt: the goal is still attack_tank and the duel has to
        #    resume.
        resumed = combat_running(text, after=int(m0[HX_T]))
        if not resumed:
            print(f"FAIL (5): after the volley ended at t={m0[HX_T]} the bot "
                  f"never picked the fight back up -- no tank_combat tick with "
                  f"a REAL target afterwards. Heating is supposed to interrupt "
                  f"the duel, not replace it.")
            dumps(text, "TANK_TARGET", 6, tail=True)
            return 1
        engage = [int(t) for (t, _i) in TANK_ENGAGE_RE.findall(text)
                  if int(t) > int(m0[HX_T])]
        fire = [int(t) for (t, _i, _g) in TANK_FIRE_RE.findall(text)
                if int(t) > int(m0[HX_T])]
        print(f"  5 OK: THE DUEL RESUMED -- tank_combat was back on a real "
              f"sighting {len(resumed)} tick(s) after the exit, first at "
              f"t={resumed[0]} ({resumed[0] - int(m0[HX_T])} brain tick(s) "
              f"later); {len(engage)} TANK_ENGAGE and {len(fire)} TANK_FIRE "
              f"line(s) followed")
        if want_reason == "maxed":
            print(f"PASS ({variant}): the bot broke off the duel, walked its "
                  f"own pillbox down the engine's reload ladder to "
                  f"PILLBOX_MAX_FIRERATE in exactly {shots} hits, and went "
                  f"back to the duel.")
        elif mode == G.FLOOR:
            print(f"PASS ({variant}): under the OLD floor rule a pill on {hp0} "
                  f"armour fired {shots} shell(s) -- all its armour allows "
                  f"without going under MIN_HP ({G.HEAT_MIN_HP}) -- leaving "
                  f"the engine's reload period at {floor} and the pill "
                  f"standing on exactly the floor. The proportional cap on "
                  f"this same ground (arena B, one token away) refuses it "
                  f"outright.")
        else:
            print(f"PASS ({variant}): a pill on {hp0} armour bought only "
                  f"{shots} of the engine's {need} halvings, the volley stopped "
                  f"there with the reload period at {floor} instead of "
                  f"{G.PILLBOX_MAX_FIRERATE}, and the duel resumed.")
        return 0

    return _check


def check_ladder(trace, step, want_drops, want_floor):
    """The engine-authoritative half of a volley arena, read off pillbox memory
    by the sidecar rather than off the brain's own account of itself.

    `want_drops` halvings of the reload period from PILLBOX_ATTACK_NORMAL, the
    last of them landing on `want_floor`, and an armour drop of exactly the
    same size across the same window -- because every damaging shell costs the
    pill one armour and halves its period, so the two counts are the same
    count.  For arena A that is 4 drops ending at PILLBOX_MAX_FIRERATE; for
    arena B2, whose pill can only afford two, it is 2 ending at 25, and the
    period must never go below 25 for the whole run."""
    if not trace:
        print(f"FAIL ({step}): the sidecar wrote no trace rows at all -- its "
              f"OURS list and the generator's geometry have drifted apart, or "
              f"the scenario file was never loaded.")
        return 1
    if trace[0][6] != G.PILLBOX_ATTACK_NORMAL:
        print(f"FAIL ({step}): the pill's first traced speed is {trace[0][6]}, "
              f"not PILLBOX_ATTACK_NORMAL ({G.PILLBOX_ATTACK_NORMAL}) -- it "
              f"did not start calm, so the ladder proves nothing.")
        show_trace(trace)
        return 1
    stolen = [r for r in trace if r[4] != 0 or r[5] != 0]
    if stolen:
        print(f"FAIL ({step}): the pill changed hands or was picked up at sim "
              f"t={stolen[0][0]} (owner={stolen[0][4]} in_tank={stolen[0][5]}) "
              f"-- an armour drop on a pill somebody took is not a heat volley.")
        show_trace(trace)
        return 1

    # THE VOLLEY STOPPED WHERE ITS ALLOWANCE RAN OUT.  This is the assertion
    # the health cap lives or dies on: a capped volley must not reach the
    # floor a full one does, at ANY point in the run.
    below = [r for r in trace if r[6] < want_floor]
    if below:
        print(f"FAIL ({step}): the ENGINE saw the reload period reach "
              f"{below[0][6]} at sim t={below[0][0]}, past this arena's "
              f"expected stop at {want_floor}. The pill's health only paid for "
              f"{want_drops} halving(s), so something fired more shells at it "
              f"than the cap allowed. Trace:")
        show_trace(trace)
        return 1

    end = next((i for i, r in enumerate(trace) if r[6] <= want_floor), None)
    if end is None:
        print(f"FAIL ({step}): the ENGINE never saw this pill's reload period "
              f"reach {want_floor}. The brain may have printed a volley, but "
              f"no shell of ours ever landed on the pill. Trace:")
        show_trace(trace)
        return 1
    speeds = [r[6] for r in trace[:end + 1]]
    drops = [(speeds[i], speeds[i + 1]) for i in range(len(speeds) - 1)
             if speeds[i + 1] < speeds[i]]
    if len(drops) != want_drops:
        print(f"FAIL ({step}): the engine's speed took {len(drops)} downward "
              f"step(s) to reach {want_floor}, not the {want_drops} this "
              f"arena's health cap allows. Every damaging shell halves the "
              f"period, so anything else means a different number of shells "
              f"landed. Ladder: " + " -> ".join(str(x) for x in speeds))
        show_trace(trace)
        return 1
    for (a, b) in drops:
        want = max(G.PILLBOX_MAX_FIRERATE, a // 2)
        if abs(b - want) > 1:
            print(f"FAIL ({step}): a speed step went {a} -> {b}; the engine "
                  f"halves ({a}//2 = {a // 2}, floored at "
                  f"{G.PILLBOX_MAX_FIRERATE}). Ladder: "
                  + " -> ".join(str(x) for x in speeds))
            return 1
    armour_drop = trace[0][3] - trace[end][3]
    if armour_drop != want_drops:
        print(f"FAIL ({step}): the pill lost {armour_drop} armour over the "
              f"volley, not {want_drops}. Each damaging shell costs it exactly "
              f"one (pillbox.c:485-487), so the two counts have to agree. "
              f"Trace:")
        show_trace(trace)
        return 1
    tail = ("PILLBOX_ATTACK_NORMAL to PILLBOX_MAX_FIRERATE"
            if want_floor <= G.PILLBOX_MAX_FIRERATE
            else f"PILLBOX_ATTACK_NORMAL to {want_floor}, which is "
                 f"{want_drops} of the engine's {G.HEAT_MAX_HITS} halvings and "
                 f"never went below it")
    print(f"  {step} OK: THE ENGINE AGREES -- reload period "
          + " -> ".join(str(x) for x in speeds)
          + f" ({tail}), armour {trace[0][3]} -> {trace[end][3]} "
            f"({armour_drop} points, one a hit), between sim t={trace[0][0]} "
            f"and t={trace[end][0]}; the pill stayed ours and out of any tank "
            f"throughout")
    return 0


# ── arena AK: the control ────────────────────────────────────────────────
def check_AK(sess, text, build_dir):
    decisions = HEAT_PILL_RE.findall(text)
    shots = HEAT_SHOT_RE.findall(text)
    exits = HEAT_EXIT_RE.findall(text)

    if decisions or shots or exits:
        print(f"FAIL (1): with cfg=ATTACK_TANK_HEAT_PILL=false the heat block "
              f"still ran ({len(decisions)} HEAT_PILL, {len(shots)} HEAT_SHOT, "
              f"{len(exits)} HEAT_EXIT) -- the flag does not turn the rule off, "
              f"so arena A says nothing about the rule.")
        dumps(text, "HEAT_PILL", 4)
        dumps(text, "HEAT_SHOT", 4)
        return 1
    print("  1 OK: not one HEAT_PILL / HEAT_SHOT / HEAT_EXIT line -- the flag "
          "really does switch the rule off (this is PRESETS.keel's value)")

    # 2. AND IT IS A CONTROL, not a run where nothing happened: the bot has to
    #    have fought the same fight arena A interrupts.
    fights = combat_running(text)
    if not fights:
        print("FAIL (2): the bot never reached tank_combat with a real target "
              "here either, so this run is not a control for anything -- the "
              "two arenas have to differ ONLY in the flag.")
        dumps(text, "GOAL ", 8)
        return 1
    print(f"  2 OK: the SAME fight ran -- {len(fights)} tank_combat tick(s) on "
          f"a real sighting, first at t={fights[0]}")

    # 3. THE ENGINE SAW AN UNTOUCHED PILL.
    trace = read_trace(build_dir, "AK")
    if not trace:
        print("FAIL (3): the sidecar wrote no trace rows at all -- it should "
              "write at least the opening state.")
        return 1
    bad = [r for r in trace
           if r[3] != G.PILLS_MAX_HEALTH or r[6] != G.PILLBOX_ATTACK_NORMAL
           or r[4] != 0 or r[5] != 0]
    if bad:
        print(f"FAIL (3): the ENGINE saw the pill move off "
              f"{G.PILLS_MAX_HEALTH} armour / speed "
              f"{G.PILLBOX_ATTACK_NORMAL} at sim t={bad[0][0]} "
              f"(armour={bad[0][3]} speed={bad[0][6]} owner={bad[0][4]} "
              f"in_tank={bad[0][5]}). Something in this arena shoots our own "
              f"pillbox for a reason that is not the rule under test, and "
              f"arena A's ladder cannot be attributed to attack_tank. Trace:")
        show_trace(trace)
        return 1
    print(f"  3 OK: the ENGINE saw the pill at {G.PILLS_MAX_HEALTH} armour and "
          f"speed {G.PILLBOX_ATTACK_NORMAL} (PILLBOX_ATTACK_NORMAL, i.e. dead "
          f"calm) for all {len(trace)} traced state(s) of the run -- nothing "
          f"else on this ground touches that pill")
    print("PASS (AK): one token different, no heat block, and an engine-side "
          "pillbox that never changed.")
    return 0


# ── arenas B / C / D / E: one gate each ──────────────────────────────────
def check_skip(variant):
    want = SKIP_EXPECT[variant]

    def _check(sess, text, build_dir):
        decisions = HEAT_PILL_RE.findall(text)
        if not decisions:
            print("FAIL (0): not one HEAT_PILL line, so the bot never reached "
                  "the heat block and the gate was never asked. What it did "
                  "instead:")
            dumps(text, "GOAL ", 8)
            dumps(text, "eval_attack_tank", 6)
            return 1
        hist = reason_histogram(decisions)
        print(f"  0 OK: the heat block ran on {len(decisions)} decision(s): "
              + ", ".join(f"{k}x{v}" for k, v in sorted(hist.items())))

        enters = [d for d in decisions if d[HP_VERDICT] == "ENTER"]
        if enters:
            e = enters[0]
            print(f"FAIL (1): the gate OPENED at t={e[HP_T]} on pill "
                  f"#{e[HP_ID]} -- d(P,E)={e[HP_DPE]} d(us,E)={e[HP_DUE]} "
                  f"range={e[HP_RNG]}/{e[HP_MAXRNG]} los={e[HP_LOS]} "
                  f"hp={e[HP_HP]} anger={e[HP_ANGER]}/{e[HP_MAXFRAC]} "
                  f"need={e[HP_NEED]} allow={e[HP_ALLOW]} of {e[HP_MAXHITS]} "
                  f"shots={e[HP_SHOTS]}. This arena is built so that `{want}` "
                  f"rejects it every time.")
            return 1
        print(f"  1 OK: the gate never opened -- {len(decisions)} decision(s), "
              f"no ENTER")

        wrong_mode = [d for d in decisions
                      if d[HP_CAP] != G.cap_mode_of(variant)]
        if wrong_mode:
            print(f"FAIL (1): a decision at t={wrong_mode[0][HP_T]} reports "
                  f"cap{{{wrong_mode[0][HP_CAP]}}}, not this arena's "
                  f"{G.cap_mode_of(variant)} -- the run is not in the mode it "
                  f"was written for.")
            return 1
        got = [d for d in decisions if d[HP_VERDICT] == "SKIP:" + want]
        if not got:
            print(f"FAIL (2): `SKIP:{want}` never appeared. The bot did reach "
                  f"the gate and did refuse, but for other reasons "
                  f"({', '.join(sorted(hist))}) -- so this arena is not "
                  f"actually testing the clause it was built for. First few "
                  f"decisions:")
            dumps(text, "HEAT_PILL", 6)
            return 1
        g0 = got[0]
        print(f"  2 OK: {len(got)} x SKIP:{want}; the first at "
              f"t={g0[HP_T]} on pill #{g0[HP_ID]} -- d(P,E)={g0[HP_DPE]} "
              f"d(us,E)={g0[HP_DUE]} range={g0[HP_RNG]}/{g0[HP_MAXRNG]} "
              f"los={g0[HP_LOS]} hp={g0[HP_HP]}/{g0[HP_MINHP]} "
              f"anger={g0[HP_ANGER]}/{g0[HP_MAXFRAC]} need={g0[HP_NEED]} "
              f"allow={g0[HP_ALLOW]} of {g0[HP_MAXHITS]} "
              f"shots_needed={g0[HP_SHOTS]}")

        # The gate that fires is the FIRST failing one in heat_pill_select's
        # ladder, so `los{?}` is the honest answer whenever an earlier gate
        # rejected: assert the line says so rather than inventing a LOS test.
        if want != "no_los":
            liars = [d for d in got if d[HP_LOS] != "?"]
            if liars:
                print(f"FAIL (2): a SKIP:{want} line at t={liars[0][HP_T]} "
                      f"reports los{{{liars[0][HP_LOS]}}}, but {want} is checked "
                      f"BEFORE line of sight -- the line claims a test that "
                      f"never ran.")
                return 1
            print(f"  3 OK: every SKIP:{want} line reports los{{?}} -- the "
                  f"honest answer when an earlier gate rejected first")
        else:
            liars = [d for d in got if d[HP_LOS] != "false"]
            if liars:
                print(f"FAIL (3): a SKIP:no_los line at t={liars[0][HP_T]} "
                      f"reports los{{{liars[0][HP_LOS]}}} rather than false.")
                return 1
            print(f"  3 OK: every SKIP:no_los line reports los{{false}} -- the "
                  f"test really did run and really did reject")

        trace = read_trace(build_dir, variant)
        rc = check_skip_trace(variant, trace)
        if rc:
            return rc
        print(f"PASS ({variant}): the gate was asked {len(decisions)} time(s) "
              f"and refused; `{want}` is why.")
        return 0

    return _check


def check_skip_trace(variant, trace):
    """The engine's side of a SKIP arena: nobody heated anything."""
    if not trace:
        print("FAIL (4): the sidecar wrote no trace rows at all.")
        return 1
    hot = [r for r in trace if r[6] < G.PILLBOX_ATTACK_NORMAL]
    if hot:
        print(f"FAIL (4): the ENGINE saw the pill's reload period drop to "
              f"{hot[0][6]} at sim t={hot[0][0]} -- something heated it even "
              f"though the gate refused every time. Trace:")
        show_trace(trace)
        return 1
    if variant == "D":
        # The point of arena D, stated as engine truth: the proxy saturated,
        # the reload period never moved.  Nothing else in tests/ can show that.
        armours = sorted({r[3] for r in trace})
        print(f"  4 OK: THE PROXY AND THE TRUTH DISAGREE, exactly as staged -- "
              f"the brain read anger >= {G.HEAT_MAX_FRAC} and refused, while "
              f"the ENGINE held the reload period at "
              f"{G.PILLBOX_ATTACK_NORMAL} (PILLBOX_ATTACK_NORMAL) for all "
              f"{len(trace)} traced state(s). The sidecar's drop-and-restore "
              f"pairs moved only armour, over {armours}")
    else:
        print(f"  4 OK: the ENGINE held the reload period at "
              f"{G.PILLBOX_ATTACK_NORMAL} (PILLBOX_ATTACK_NORMAL) for all "
              f"{len(trace)} traced state(s) -- nothing heated this pill")
    return 0


CHECKS = {
    "A": check_volley("A"),
    "AK": check_AK,
    "B": check_skip("B"),
    "B2": check_volley("B2"),
    "C": check_skip("C"),
    "D": check_skip("D"),
    "E": check_skip("E"),
    "F": check_volley("F"),
}


def run(variant, ticks, build_dir):
    print(f"=== arena {variant} ({ticks} engine ticks, tokens: "
          f"{TOKENS[variant]}) ===")
    sess, text = run_sim(variant, ticks, build_dir)
    if sess is None:
        print(f"FAIL: {text}")
        return 1
    print(f"  session: {sess.name}")
    errs = lua_errors(text)
    if errs:
        print("FAIL: Lua errors in our bot's log:")
        for e in errs:
            print("   " + e.strip())
        return 1
    return CHECKS[variant](sess, text, build_dir)


def main():
    variant, ticks, build = "all", None, DEFAULT_BUILD
    args = sys.argv[1:]
    take_asap_flag(args)
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1]; i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    variants = list(CHECKS) if variant == "all" else variant.split(",")
    unknown = [v for v in variants if v not in CHECKS]
    if unknown:
        print(f"unknown arena(s): {', '.join(unknown)} "
              f"(have: {', '.join(CHECKS)})")
        sys.exit(2)
    rc = 0
    for v in variants:
        try:
            rc |= run(v, ticks or TICKS[v], build)
        except subprocess.TimeoutExpired:
            print(f"FAIL ({v}): run timed out")
            rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
