#!/usr/bin/env python3
"""Refuel target stickiness + stock-aware refuel pricing test.

Field incident 20260902_120856: the bot had two of its own bases thirteen
tiles apart, both reachable, both priced within a few tens of each other.  A
moving enemy tank kept crossing the CONTESTED_BASE_RANGE disk of first one and
then the other, the two scores swapped every replan, and -- because pool 1
publishes exactly ONE row, so the previous target has no pool entry left for
the goal layer's switch bar to hold onto -- the refuel target changed with it.
The tank drove back and forth and died having reached neither base.  In the
same block, a base holding four shells priced exactly the same as one holding
ninety: REFUEL_MIN_STOCK is a threshold REJECT and anything above it was free.

Two changes came out of that, and this test covers both.

  1. REFUEL TARGET STICKINESS (goals.lua refuel_target_hold).  pool 1's winner
     IS the refuel target, so the hold lives there.  The held target is kept
     unless it drops out of the viable set, we dock on it, the refuel
     completes, REFUEL_TARGET_HOLD_MAX_TICKS (3000) expires, or a rival costs
     less than REFUEL_TARGET_SWITCH_RATIO (0.6) x the held target's CURRENT
     cost AND at least REFUEL_TARGET_MIN_HOLD_TICKS (150) have passed.

  2. STOCK-AWARE PRICING (goals.lua refuel_shortfall).  A candidate with a
     FRESH stock observation is charged for the shells it cannot supply toward
     the tank's live target, clamped at REFUEL_SHORTFALL_CAP (400).  HOW that
     surcharge is derived is goals.lua's business and has already changed
     twice, so this test matches the chip's SHAPE and SIGN and prints whatever
     pricing detail the row carries -- it does not re-derive the formula.

THE ARENA (tests/generate_refuel_stickiness_map.py)

One map, two sidecars.  Everything is written between x 110..142 and y 110..142,
symmetric about (126,126) so mapRead's recenter is a no-op.  Three bands:

    x 110..111   PATROL ISLAND (grass)
    x 112        MOAT (deep sea, full height)
    x 113..142   MAIN FIELD (grass) -- the bot's world

    (126,127)    the bot's start: a one-tile deep-sea pond (a start square has
                 to be deep sea, starts.c startsIsValidSquare)
    (120,131)    BASE_NEAR, mdist 10 from the spawn
    (120,118)    BASE_FAR,  mdist 15 -- thirteen tiles apart, the incident's
                 geometry, and priced within about ten of each other
    (110,112)    a scripted opponent's start pond, ON THE ISLAND; it drives
                 south off it and paces the lane y=116..133 forever
                 (tests/brains/patrol_ns.lua)

The opponent lives across an uncrossable moat on purpose.  contested_penalty
measures mdist and does not care whether the enemy can be reached, so it still
swings each base's contested term by ~40 in ANTIPHASE (level with BASE_FAR the
near base reads exactly 0, and vice versa) -- four times the ~10 of travel that
separates the bases, so the RAW cheapest base flips every lap.  attack_tank
DOES care: an unreachable tank costs INF and never wins a pool, so the bot
never abandons refuel to go hunting, which on an earlier cut dragged it fifteen
tiles and swamped the contested jitter with travel-cost swings.  The moat is
uncrossable for good -- the spawn boat strands the moment the tank drives onto
grass, and there is no river anywhere, so no boat can ever be built.

Both variants run TOURNAMENT + -ranked, the only combination that puts 0 shells
in the tank on a map with a scenario sidecar (without -ranked,
serverSimApplyScenarioCommit stamps gameScripted, which hands out the full open
loadout).  That matters twice: the bot has to WANT to refuel, and the opponent
holds no shells either, so it cannot kill the bot, shoot a base down, or anger
anything.  It is pure motion.

TWO VARIANTS -- same map, different sidecar stock:

  A  STICKINESS.  Both bases fully stocked, so the scores stay comparable and
     the patrol keeps swapping the raw order all run.
     PASS: the hold actually got asked the question (at least one
     REFUEL_TARGET_HOLD line naming a cheaper rival -- without that the test
     would pass vacuously), genuine target CHANGES (the "#old -> #new" switch
     shape) stay at most 1 per refuel trip and at most 2 across the whole run,
     the tank REACHES one of the two base tiles, and its shells go from 0 to
     above SHELLS_LOW.  Arrival is read from the tank's own position: the hold
     no longer releases on docking, so there is no log line for it, and the
     position is what the test meant all along.

  B  STOCK-AWARE.  Identical, except the NEARER base is pinned at 6 shells --
     the smallest value that survives the low_stock REJECT (REFUEL_MIN_STOCK
     is 5; at 4 the row is thrown out upstream and never priced at all, so
     there would be nothing to assert on).
     PASS: the near base's row carries a positive "short sh<n> -> ... +N" stock
     term within the cap, the far (full) base's rows carry the "covers need"
     zero instead, the bot's chosen refuel target is the FAR base and the near
     one is never adopted, and the tank arrives on the far base's tile and
     fills up there.

Companion to tests/base_steal_test.py (same print2-log harness pattern).

Usage: python refuel_stickiness_test.py [--variant A|B|ALL] [--ticks N]
                                        [--build DIR]
Exit 0 only if every variant passes.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

# The brain's log lines carry non-ASCII arithmetic (-> >= x, em dashes) and get
# quoted verbatim in failure messages. A Windows console defaults to cp1252 and
# would raise UnicodeEncodeError mid-report, turning a useful FAIL into a
# traceback.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
FOE_BRAIN = HERE / "brains" / "patrol_ns.lua"

sys.path.insert(0, str(HERE))
from generate_refuel_stickiness_map import (      # noqa: E402
    SPAWN, BASE_NEAR, BASE_FAR, FOE_SPAWN, PATROL_X, PATROL_Y0, PATROL_Y1,
    NEAR_SHELLS_B, FULL_STOCK, REFUEL_TARGET_SWITCH_RATIO,
    REFUEL_SHORTFALL_CAP, mdist)

PORTS = {"A": 50120, "B": 50121}
TICKS = {"A": 3000, "B": 1500}
SHELLS_LOW = 20            # constants.lua -- "the tank is resupplied" line

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so these are
# used with re.search, never re.match.  The logs are UTF-8 and full of
# non-ASCII arithmetic (-> >= x <-> em dash); read them with encoding="utf-8".
#
#   REFUEL_TARGET_SWITCH t=14 [finalize] -> base#0 cost=60 (no held target)
ADOPT_RE = re.compile(
    r"REFUEL_TARGET_SWITCH t=(\d+) \[(\w+)\] → base#(\S+) "
    r"cost=(-?[\d.]+) \((.*)\)")
#   REFUEL_TARGET_SWITCH t=653 [finalize] #1 -> #0 -- rival 49 < 0.60x203=122 (held 150t)
CHANGE_RE = re.compile(
    r"REFUEL_TARGET_SWITCH t=(\d+) \[(\w+)\] #(\S+) → #(\S+) — "
    r"rival ([\d.]+) < ([\d.]+)×([\d.]+)=([\d.]+) \(held (\d+)t\)")
#   REFUEL_TARGET_HOLD t=1238 [finalize] keep #1 cost=70 vs rival #0 cost=61 -- min hold 150t (135/150)
HOLD_MIN_RE = re.compile(
    r"REFUEL_TARGET_HOLD t=(\d+) \[(\w+)\] keep #(\S+) cost=([\d.]+) "
    r"vs rival #(\S+) cost=([\d.]+) — min hold (\d+)t \((\d+)/(\d+)\)")
#   REFUEL_TARGET_HOLD t=1303 [finalize] keep #1 cost=70 -- rival #0 61 >= 0.60x70=42
HOLD_RATIO_RE = re.compile(
    r"REFUEL_TARGET_HOLD t=(\d+) \[(\w+)\] keep #(\S+) cost=([\d.]+) — "
    r"rival #(\S+) ([\d.]+) ≥ ([\d.]+)×([\d.]+)=([\d.]+)")
#   REFUEL_TARGET_HOLD t=253 RELEASED -- docked at (120,131) (the held base) after 239t
# LEGACY.  goals.lua no longer releases the hold on docking, so this matches
# nothing against a current brain.  It is kept only so trips() still segments
# an older log correctly; nothing asserts on it.
REL_DOCK_RE = re.compile(
    r"REFUEL_TARGET_HOLD t=(\d+) RELEASED — docked at \((\d+),(\d+)\)")
#   REFUEL_TARGET_HOLD t=900 RELEASED -- refuel complete (arm 40/40 sh 25/25), held #1 for 300t
REL_DONE_RE = re.compile(
    r"REFUEL_TARGET_HOLD t=(\d+) RELEASED — refuel complete "
    r"\(arm (\d+)/(\d+) sh (\d+)/(\d+)\), held #(\S+) for (\d+)t")
#   REFUEL_CAND base#0 @(120,131) OK score=344.3 travel=15.3 ...
CAND_RE = re.compile(
    r"REFUEL_CAND base#(\S+) @\((\d+),(\d+)\) OK score=([\d.]+) "
    r"travel=([\d.]+) danger=([\d.]+)×([\d.]+)×([\d.]+)\[desp\] "
    r"stale=(\d+) contest=(\d+) dep=(\d+)(.*) obs_sh=(-?\d+) obs_arm=(-?\d+)")
#   ... stock{obs sh6, need sh25, short sh19 -> frac 0.76 x (trip 15 x 0.6
#                                                 + 60) = +53}
# The chip has changed shape twice while this test has existed: the pricing
# started as a flat per-unit sum ("-> +228"), became the frac/trip form above,
# and the "arm<n>" halves were dropped once obs_armour turned out to disagree
# with itself 5x (bases.c:1581 hands the brain armour/5 in the per-tick close-
# base item, while EVENT_BASE_STOCK sends it unscaled). So the arm fields are
# OPTIONAL here and everything between the arrow and the "+N" is free text.
# What this test asserts is the SHAPE and the SIGN -- the row really does say
# the base cannot cover the top-up, and really does charge for it -- and it
# reports whatever pricing detail the row carries rather than re-deriving a
# formula that lives in goals.lua and is free to move again.
SHORT_RE = re.compile(
    r"stock\{obs sh(\d+)(?: arm\d+)?, need sh(\d+)(?: arm\d+)?, "
    r"short sh(\d+)(?: arm\d+)? →([^}]*?)\+([\d.]+)\}")
#   ... stock{obs sh90 covers need sh25 -> 0}  (the "this base can supply the
#   whole top-up" shape).  Earlier cuts wrote "obs 90/90 covers need 25/0" and
#   "obs sh90 arm90 covers need sh25 arm0", hence the tolerant middle.
COVERS_RE = re.compile(r"stock\{obs [^}]*covers need [^}]*→ 0\}")
#   ENGINE_DUMP t=411 self=(120,118) dir=171 spd=4 arm=40 sh=3
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\) dir=\d+ spd=\d+ "
    r"arm=(\d+) sh=(\d+)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def play(variant, ticks, build_dir):
    """Run one variant and return the bot's print2 log text, or None."""
    label = f"refuel_stick_test_{variant}"
    mapfile = HERE / f"refuel_stickiness_{variant}.map"
    snap = HERE / f"refuel_stickiness_{variant}_snap.jsonl"
    final = HERE / f"refuel_stickiness_{variant}_final.json"
    stderr = HERE / f"refuel_stickiness_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable,
                    str(HERE / "generate_refuel_stickiness_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (snap, final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby",
           # TOURNAMENT + -ranked is the only combination that puts 0 shells in
           # the tank on a scenario map -- see the generator's header.
           "-gametype", "tournament", "-ranked",
           "-bots", "2", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN},1={FOE_BRAIN}",
           "-allow-unsafe-brains",
           # yesfull: the whole arena is known from tick 0 - the test is about
           # which base is CHOSEN, not about finding them.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-snapjson", str(snap), "-snapinterval", "100",
           "-nowinbolonet", "-quiet", "-threads", "1"]
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 5))

    sess = newest_session(build_dir, label)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed - see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return None
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return None
    print(f"  session: {sess.name}")
    return log.read_text(encoding="utf-8", errors="replace")


def base_tiles(text):
    """base id -> tile, learned from the REFUEL_CAND rows themselves."""
    out = {}
    for m in CAND_RE.finditer(text):
        out[m.group(1)] = (int(m.group(2)), int(m.group(3)))
    return out


def trips(text):
    """Split the run into refuel trips.

    A trip opens on an adopt ("-> base#N (why)") and closes on the next
    RELEASED line; a trip still open at the end of the log closes there.
    Against a current brain the only release is "refuel complete" (the
    docked-release rule is gone), so a trip now spans the whole fill-up
    instead of ending the moment the tank touches the pad -- which makes the
    per-trip switch budget below a STRICTER test than it used to be, not a
    looser one.  Returns a list of dicts with the adopt, the genuine target
    CHANGES inside it, and how it ended."""
    events = []
    for m in ADOPT_RE.finditer(text):
        events.append((int(m.group(1)), "adopt", m))
    for m in CHANGE_RE.finditer(text):
        events.append((int(m.group(1)), "change", m))
    for m in REL_DOCK_RE.finditer(text):
        events.append((int(m.group(1)), "dock", m))
    for m in REL_DONE_RE.finditer(text):
        events.append((int(m.group(1)), "done", m))
    events.sort(key=lambda e: (e[0], {"adopt": 0, "change": 1,
                                      "dock": 2, "done": 3}[e[1]]))
    out, cur = [], None
    for t, kind, m in events:
        if kind == "adopt":
            if cur:
                out.append(cur)
            cur = {"t0": t, "id": m.group(3), "why": m.group(5),
                   "changes": [], "end": None, "endkind": None}
        elif kind == "change":
            if cur is None:                      # a change with no adopt seen
                cur = {"t0": t, "id": m.group(3), "why": "(implicit)",
                       "changes": [], "end": None, "endkind": None}
            cur["changes"].append(m)
        elif cur is not None:
            cur["end"], cur["endkind"] = t, kind
            out.append(cur)
            cur = None
    if cur:
        out.append(cur)
    return out


def dumps(text):
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3)),
             int(m.group(4)), int(m.group(5))) for m in DUMP_RE.finditer(text)]


def report_common(text):
    """Shared reporting + the checks every variant must pass.  Returns the
    ENGINE_DUMP samples, or None on failure."""
    if LUA_ERR_RE.search(text):
        for line in text.splitlines():
            if LUA_ERR_RE.search(line):
                print(f"FAIL: Lua error in the print2 log: {line.strip()}")
                return None
    d = dumps(text)
    if not d:
        print("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return None
    ids = base_tiles(text)
    print("  bases seen by the brain: "
          + ", ".join(f"#{k}={v}" for k, v in sorted(ids.items())))
    if len(ids) < 2:
        print("FAIL: the brain never priced both bases as refuel candidates")
        return None
    print(f"  tank: t={d[0][0]} at ({d[0][1]},{d[0][2]}) arm={d[0][3]} "
          f"sh={d[0][4]} -> t={d[-1][0]} at ({d[-1][1]},{d[-1][2]}) "
          f"arm={d[-1][3]} sh={d[-1][4]}; peak shells {max(x[4] for x in d)}")
    if d[0][4] != 0:
        print(f"FAIL: the tank spawned with {d[0][4]} shells, not 0 -- the "
              "TOURNAMENT loadout did not apply (is -ranked still on the "
              "command line? a scenario map without it is stamped gameScripted, "
              "which hands out the full open loadout)")
        return None
    return d


def run_A(text):
    """Variant A: the hold absorbs the flapping and the tank docks."""
    d = report_common(text)
    if d is None:
        return 1

    holds = [m for m in HOLD_MIN_RE.finditer(text)] + \
            [m for m in HOLD_RATIO_RE.finditer(text)]
    hold_min = len(HOLD_MIN_RE.findall(text))
    hold_ratio = len(HOLD_RATIO_RE.findall(text))
    tr = trips(text)
    changes = sum(len(t["changes"]) for t in tr)

    print(f"  REFUEL_TARGET_HOLD lines: {len(holds)} "
          f"({hold_min} inside the {'min-hold window'}, {hold_ratio} on the "
          f"{REFUEL_TARGET_SWITCH_RATIO} ratio bar)")
    print(f"  refuel trips: {len(tr)}")
    for t in tr:
        end = f"{t['endkind']}@{t['end']}" if t["end"] else "still open at end"
        print(f"    t={t['t0']} adopt #{t['id']} ({t['why']}) -> {end}; "
              f"{len(t['changes'])} genuine target change(s)"
              + "".join(f"\n      t={c.group(1)} #{c.group(3)} -> #{c.group(4)}"
                        f" (rival {c.group(5)} < {c.group(6)}x{c.group(7)}"
                        f"={c.group(8)}, held {c.group(9)}t)"
                        for c in t["changes"]))

    # -- 1. The arena has to have ASKED the question.  A hold line naming a
    #       cheaper rival is the raw pool winner flipping and the hold refusing
    #       to follow it; with none, the test proved nothing.
    if not holds:
        print("FAIL: not one REFUEL_TARGET_HOLD line -- no rival ever became "
              "the cheaper base, so the hold was never exercised. The patrol "
              f"({FOE_SPAWN} -> lane x={PATROL_X} y={PATROL_Y0}..{PATROL_Y1}) "
              "is not swinging the contested term; check the REFUEL_CAND rows' "
              "contest= field.")
        return 1

    # -- 2. Genuine target changes stay bounded.
    worst = max((len(t["changes"]) for t in tr), default=0)
    if worst > 1:
        bad = [t for t in tr if len(t["changes"]) > 1][0]
        print(f"FAIL: the trip adopted at t={bad['t0']} changed target "
              f"{len(bad['changes'])} times. At most one change per refuel "
              "trip is allowed -- more than that is the flapping this test "
              "exists to catch.")
        return 1
    if changes > 2:
        print(f"FAIL: {changes} genuine target changes across the run; at most "
              "2 are allowed.")
        return 1
    print(f"  genuine target changes: {changes} across the run "
          f"(max {worst} in any one trip; the budget is 1 per trip and 2 total)")

    # -- 3. It actually got there.  A hold that never delivers the tank to a
    #       base is the incident with extra steps.
    #
    #       Measured from the TANK's own position, not from a hold-release
    #       line.  goals.lua used to release the hold on docking and print
    #       "RELEASED - docked at (x,y)"; that rule is gone (arriving is not
    #       finishing -- a 20-minute smoke found 121 of 121 releases were
    #       docked ones, so every trip's hold ended the moment the tank rolled
    #       onto a pad and the next replan was free to leave again).  Standing
    #       on the tile is the thing this test actually cares about, and it
    #       survives any future change to when the hold lets go.
    docked = [x for x in d if (x[1], x[2]) in (BASE_NEAR, BASE_FAR)]
    if not docked:
        print(f"FAIL: the tank never reached either base tile "
              f"({BASE_NEAR} or {BASE_FAR}). Positions sampled: "
              + ", ".join(f"({x[1]},{x[2]})@{x[0]}" for x in d[::200][:10]))
        return 1
    first = docked[0]
    print(f"  first arrival on a base tile: t={first[0]} at "
          f"({first[1]},{first[2]}); {len(docked)} tick(s) parked on a base")

    peak = max(x[4] for x in d)
    if peak <= SHELLS_LOW:
        print(f"FAIL: the tank's shells peaked at {peak}; it started at 0 and "
              f"has to climb above SHELLS_LOW ({SHELLS_LOW}) for the refuel to "
              "count as having happened.")
        return 1
    first_full = next(x for x in d if x[4] > SHELLS_LOW)
    print(f"  shells crossed {SHELLS_LOW} at t={first_full[0]} on "
          f"({first_full[1]},{first_full[2]}), peak {peak}")

    print("PASS: the refuel target was held through the contested swing "
          f"({len(holds)} hold(s)), changed at most once per trip, and the "
          "tank docked and refilled.")
    return 0


def run_B(text):
    """Variant B: the nearly-empty near base is priced out of the trip."""
    d = report_common(text)
    if d is None:
        return 1
    ids = base_tiles(text)
    tile_of = ids
    id_near = next((k for k, v in ids.items() if v == BASE_NEAR), None)
    id_far = next((k for k, v in ids.items() if v == BASE_FAR), None)
    if id_near is None or id_far is None:
        print(f"FAIL: could not find both {BASE_NEAR} and {BASE_FAR} among the "
              f"priced bases {ids}")
        return 1
    print(f"  BASE_NEAR {BASE_NEAR} is base#{id_near} (pinned at "
          f"{NEAR_SHELLS_B} shells); BASE_FAR {BASE_FAR} is base#{id_far} "
          f"(full, {FULL_STOCK})")

    # -- 1. The near base's row carries a real shortfall, the far one's none.
    near_rows, far_rows = [], []
    for m in CAND_RE.finditer(text):
        (near_rows if m.group(1) == id_near else
         far_rows if m.group(1) == id_far else []).append(m)
    if not near_rows or not far_rows:
        print(f"FAIL: {len(near_rows)} near row(s) and {len(far_rows)} far "
              "row(s) -- both bases have to be priced for the comparison to "
              "mean anything.")
        return 1
    shorts = [SHORT_RE.search(m.group(12)) for m in near_rows]
    shorts = [s for s in shorts if s]
    if not shorts:
        print("FAIL: not one REFUEL_CAND row for the near base carried a "
              "'short ... -> +N' stock term. Either the stock observation went "
              "stale (the sidecar re-asserts it every 150 ticks so it should "
              "not) or refuel_shortfall priced it at zero. Sample row:")
        print("   " + near_rows[0].group(0))
        return 1
    s0 = shorts[0]
    got = float(s0.group(5))
    detail = s0.group(4).strip()
    print(f"  near base stock term: obs sh{s0.group(1)}, need sh{s0.group(2)}, "
          f"short sh{s0.group(3)} -> +{got:.0f} "
          f"(on {len(shorts)}/{len(near_rows)} of its rows)")
    if detail:
        print(f"    priced as: {detail} +{got:.0f}")
    if int(s0.group(3)) <= 0:
        print("FAIL: the row says the near base can cover the whole shell "
              "top-up; with 6 shells against a 25-shell target it cannot.")
        return 1
    if got <= 0:
        print("FAIL: the near base's stock term is not positive -- a base that "
              "cannot supply what we came for has to cost something.")
        return 1
    if got > REFUEL_SHORTFALL_CAP + 0.5:
        print(f"FAIL: the near base's stock term is +{got:.0f}, above "
              f"REFUEL_SHORTFALL_CAP ({REFUEL_SHORTFALL_CAP}).")
        return 1
    if any(SHORT_RE.search(m.group(12)) for m in far_rows):
        print("FAIL: the FULL base also quoted a shortfall -- it holds "
              f"{FULL_STOCK} shells and should cover the whole top-up.")
        return 1
    covers = sum(1 for m in far_rows if COVERS_RE.search(m.group(12)))
    print(f"  far base stock term: 0 on {covers}/{len(far_rows)} rows "
          "(\"covers need\")")

    # -- 2. ...and it actually changed the answer: the chosen target is the FAR
    #       base, even though the near one is five tiles closer.
    adopts = list(ADOPT_RE.finditer(text))
    if not adopts:
        print("FAIL: pool 1 never adopted a refuel target.")
        return 1
    picks = [a.group(3) for a in adopts]
    print(f"  refuel targets adopted, in order: "
          + ", ".join(f"#{p}@{tile_of.get(p)}" for p in picks))
    if picks[0] != id_far:
        print(f"FAIL: the first refuel target was base#{picks[0]} "
              f"{tile_of.get(picks[0])}; with the near base "
              f"{mdist(SPAWN, BASE_NEAR)} tiles away holding {NEAR_SHELLS_B} "
              f"shells and the far one {mdist(SPAWN, BASE_FAR)} tiles away "
              "holding a full magazine, the far base has to win.")
        return 1
    if any(p == id_near for p in picks):
        print(f"FAIL: base#{id_near} (the nearly-empty near base) was adopted "
              "as a refuel target at some point in the run.")
        return 1

    # -- 3. And the tank drove there and filled up ON that tile.
    on_far = [x for x in d if (x[1], x[2]) == BASE_FAR]
    on_near = [x for x in d if (x[1], x[2]) == BASE_NEAR]
    print(f"  ticks standing on BASE_FAR {BASE_FAR}: {len(on_far)}; "
          f"on BASE_NEAR {BASE_NEAR}: {len(on_near)}")
    if not on_far:
        print("FAIL: the tank never reached the full base's tile.")
        return 1
    print(f"  shells while on BASE_FAR: {min(x[4] for x in on_far)} -> "
          f"{max(x[4] for x in on_far)}")
    if max(x[4] for x in on_far) <= SHELLS_LOW:
        print(f"FAIL: the tank stood on the full base but its shells never "
              f"got above SHELLS_LOW ({SHELLS_LOW}).")
        return 1

    print("PASS: the nearly-empty near base was priced with a real shortfall, "
          "the bot drove past it to the full base, and refilled there.")
    return 0


def run_one(variant, ticks, build_dir):
    print(f"=== variant {variant} "
          f"({'stickiness' if variant == 'A' else 'stock-aware pricing'}), "
          f"{ticks} ticks ===")
    text = play(variant, ticks, build_dir)
    if text is None:
        return 1
    return run_A(text) if variant == "A" else run_B(text)


def main():
    which = "ALL"
    ticks = None
    build = DEFAULT_BUILD
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            which = args[i + 1].upper(); i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    variants = ["A", "B"] if which == "ALL" else [which]
    rc = 0
    for v in variants:
        try:
            rc |= run_one(v, ticks or TICKS[v], build)
        except subprocess.TimeoutExpired:
            print(f"FAIL: variant {v} timed out")
            rc = 1
        print()
    print("OVERALL: " + ("PASS" if rc == 0 else "FAIL"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
