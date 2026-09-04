#!/usr/bin/env python3
"""
Refuel-pad reach -- a pill that cannot shoot a tank parked on a base must not
price that base (GoalHunter 1.7, author's rule of 2026-09-03).

WHAT IS UNDER TEST (goals.lua refuel_pad_pill_reach / refuel_pad_chip,
constants.lua REFUEL_PAD_TANK_OFFSET):

    A pillbox fires when the euclidean distance from ITS tile centre to the
    TANK's world position is <= PILLBOX_RANGE 2048 wu = PILL_FIRE_RANGE 8
    tiles (pillbox.c pillsUpdate -> util.c utilIsItemInRange).  A tank parked
    on a base tile is at most half a tile diagonal -- REFUEL_PAD_TANK_OFFSET,
    0.7071 tiles -- from that tile's centre.  So a pill can shell a docked tank
    only when dist(pill tile, base tile) <= 8.7071.

    The brain's DANGER STAMP is deliberately one tile wider than that
    (PILL_RANGE_MAP 9), because it steers the DRIVE.  For REFUEL-BASE pricing
    only, in BOTH refuel scoring paths (nearest_resupply_base and the pool-1
    block of step_eval_queue), when NO live deployed hostile/neutral pill is
    within 8.7071 of the base tile the PILL layer comes off the base's danger
    (danger = threat.at - threat.pill_at) and only the enemy-tank layer is
    left.  If any pill does reach part of the tile the stamped value stands in
    full -- no fractional scaling; the tank does not choose where on the tile
    it stops.  Every row says which verdict applied and the numbers it
    compared: ` padsafe{no pill reaches the pad; nearest #0 at 9.0 > 8.71}` or
    ` padhit{#0 reaches: 8.0 <= 8.71}`, plus ` padcut{-N pill danger, ...}`
    when something was actually removed.

THE INCIDENT
    20260903_193428 bot2 t=1563: refuel candidate base#0 @(138,112) priced
    raw 9 + base 45 + danger 427 (x danger{1.33}) = 640.9 with the tank on 10
    armour two tiles away, and lost to base#5 twelve tiles off at 301.  The
    whole 427 came from pill#2 at (138,121) -- exactly 9.0 tiles from the base
    centre, on the rim of the stamp and angry, and physically unable to fire a
    single shell at that base.

THE ARENA (tests/generate_refuel_pad_reach_map.py, two variants)
    A neutral pillbox due south of BASE_NEAR, and a fully stocked BASE_FAR
    twelve tiles from the spawn and ~17 tiles from the pill as the control.
    Variant A puts the pill at 9.0 tiles -- inside the stamp, outside the pad
    threshold.  Variant B moves it one tile closer, to 8.0 -- inside both.
    TOURNAMENT + -ranked puts 0 shells in the tank so it has to refuel; there
    is no mine anywhere, so its armour stays full and neither base is ever
    short of anything.  The scripted opponent paces an island 30+ tiles away,
    past CONTESTED_BASE_RANGE and the enemy-tank threat radius, so threat.at()
    at either base is PURE pill danger and variant A's danger term must land on
    exactly 0.

CHECKS -- VARIANT A (dy = 9, padsafe)
    1. No Lua error; the tank thought; it spawned with 0 shells.
    2. Every REFUEL_P1 row for BASE_NEAR is padsafe, none is padhit, and at
       least one names the pill at 9.0 tiles against the 8.71 threshold.
    3. At least one BASE_NEAR row carries a padcut -- i.e. the stamp really did
       put pill danger on that base and the rule really did remove it.  Without
       this the test would pass on an arena where there was nothing to remove.
    4. Every padsafe BASE_NEAR row's danger term is 0 (the tank-layer
       remainder, and there is no enemy tank in reach of the base).
    5. BASE_FAR is padsafe throughout and its danger term is 0 too.
    6. BASE_NEAR is priced below BASE_FAR on every tick where both were priced.
    7. The tank refuels at BASE_NEAR: its shells rise while it stands there.

CHECKS -- VARIANT B (dy = 8, padhit)
    1. Same sanity.
    2. Every REFUEL_P1 row for BASE_NEAR is padhit (the pill at 8.0 <= 8.71)
       -- or padsafe with NO pill named, which is the honest answer after the
       bot has killed it.  No row may name a live pill as out of reach.
    3. Every padhit row's danger term is > 0 (the stamped value, unscaled) and
       no BASE_NEAR row carries a padcut.
    4. BASE_FAR stays padsafe -- one pill, two bases, two different verdicts.
    5. The pick follows the priced costs on every goal-set tick where both
       bases were priced.  The test does not demand a fixed winner; it reports
       which base won.

Usage: python refuel_pad_reach_test.py [A|B] [--ticks N] [--build DIR] [--no-asap]
       (no variant = run A then B)
Exit 0 on pass.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
FOE_BRAIN = HERE / "brains" / "patrol_ns.lua"
sys.path.insert(0, str(HERE))
from generate_refuel_pad_reach_map import (      # noqa: E402
    BOT_SPAWN, BASE_NEAR, BASE_FAR, PILL, PAD_THRESHOLD, PILL_FIRE_RANGE,
    REFUEL_PAD_TANK_OFFSET, PILL_RANGE_MAP, edist, mdist)

PORT = 50173
LABEL = "refuel_pad_reach_test"
DEFAULT_TICKS = 1500

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so these are
# used with re.search, never re.match.
# Pool 1's per-candidate trace (goals.lua step_eval_queue, BRAIN_DEBUG_MODE
# only): one line per base the refuel pool priced.  Group 8 is the danger term
# (danger_val x REFUEL_DANGER_WEIGHT x cautious mult); group 13 is the chip
# tail that carries the pad verdict.
ROW_RE = re.compile(
    r"REFUEL_P1 t=(\d+) base#(\S+) @\((\d+),(\d+)\) OK score=([\d.]+) = "
    r"max\(raw ([-\d.]+) \+ base ([-\d.]+) \+ danger ([-\d.]+) \+ "
    r"stale ([-\d.]+) \+ contest ([-\d.]+) \+ dep ([-\d.]+), ([-\d.]+)\)"
    r"(.*?) \| need arm=(\d+)/(\d+) sh=(\d+)/(\d+)")
PADSAFE_NAMED_RE = re.compile(
    r" padsafe\{no pill reaches the pad; nearest #(\S+) at ([\d.]+) > ([\d.]+)\}")
PADSAFE_NONE_RE = re.compile(r" padsafe\{no live hostile pill on the map\}")
PADHIT_RE = re.compile(r" padhit\{#(\S+) reaches: ([\d.]+) <= ([\d.]+)\}")
PADCUT_RE = re.compile(r" padcut\{-([\d.]+) pill danger, danger_val ([\d.]+)\}")
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\) dir=\d+ spd=\d+ "
    r"arm=(\d+) sh=(\d+)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")
TICK_RE = re.compile(r"^===TICK (\d+)===", re.M)
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    root = build_dir / "debug_sessions"
    if not root.exists():
        return None
    cands = [d for d in root.iterdir() if d.is_dir() and d.name.endswith("_" + label)]
    return max(cands, key=lambda d: d.stat().st_mtime) if cands else None


def play(variant, ticks, build_dir):
    """Run one variant's arena and return bot 0's print2 log text, or None."""
    mapfile = HERE / f"refuel_pad_reach_{variant}.map"
    final = HERE / f"refuel_pad_reach_{variant}_final.json"
    stderr = HERE / f"refuel_pad_reach_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable,
                    str(HERE / "generate_refuel_pad_reach_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=f"{LABEL}_{variant}")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORT),
           "-nolobby",
           # TOURNAMENT + -ranked is the only combination that puts 0 shells in
           # the tank on a scenario map -- see the generator's header.
           "-gametype", "tournament", "-ranked",
           "-bots", "2", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN},1={FOE_BRAIN}",
           "-allow-unsafe-brains",
           # yesfull: the whole arena is known from tick 0 - the test is about
           # how a base is PRICED, not about finding it.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 5))

    sess = newest_session(build_dir, f"{LABEL}_{variant}")
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


def tick_blocks(text):
    """[(tick, block_text)] in log order."""
    out = []
    marks = list(TICK_RE.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        out.append((int(m.group(1)), text[m.start():end]))
    return out


class Row(object):
    """One REFUEL_P1 candidate line, split into the bits this test reads."""

    def __init__(self, m):
        self.tick = int(m.group(1))
        self.id = m.group(2)
        self.tile = (int(m.group(3)), int(m.group(4)))
        self.score = float(m.group(5))
        self.raw = float(m.group(6))
        self.danger = float(m.group(8))
        self.chips = m.group(13)

    @property
    def padsafe_named(self):
        return PADSAFE_NAMED_RE.search(self.chips)

    @property
    def padsafe_none(self):
        return PADSAFE_NONE_RE.search(self.chips)

    @property
    def padhit(self):
        return PADHIT_RE.search(self.chips)

    @property
    def padcut(self):
        return PADCUT_RE.search(self.chips)


def parse(text):
    """(rows, dumps, goals) or None with a printed reason."""
    if LUA_ERR_RE.search(text):
        for line in text.splitlines():
            if LUA_ERR_RE.search(line):
                print(f"FAIL: Lua error in the print2 log: {line.strip()}")
                return None
    dumps = [(int(m.group(1)), (int(m.group(2)), int(m.group(3))),
              int(m.group(4)), int(m.group(5))) for m in DUMP_RE.finditer(text)]
    if not dumps:
        print("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return None
    if dumps[0][3] != 0:
        print(f"FAIL: the tank spawned with {dumps[0][3]} shells, not 0 -- the "
              "TOURNAMENT loadout did not apply (is -ranked still on the "
              "command line?)")
        return None
    rows = [Row(m) for m in ROW_RE.finditer(text)]
    if not rows:
        print("FAIL: no REFUEL_P1 rows -- the refuel pool never priced a base")
        return None
    goals = []
    for tick, block in tick_blocks(text):
        for m in GOAL_RE.finditer(block):
            goals.append((tick, m.group(1), m.group(2),
                          (int(m.group(3)), int(m.group(4)))))
    print(f"  tank: t={dumps[0][0]} at {dumps[0][1]} arm={dumps[0][2]} "
          f"sh={dumps[0][3]} -> t={dumps[-1][0]} at {dumps[-1][1]} "
          f"arm={dumps[-1][2]} sh={dumps[-1][3]}")
    return rows, dumps, goals


def split_bases(rows):
    """(near_rows, far_rows, id_near, id_far) or None."""
    ids = {}
    for r in rows:
        ids[r.id] = r.tile
    id_near = next((k for k, v in ids.items() if v == BASE_NEAR), None)
    id_far = next((k for k, v in ids.items() if v == BASE_FAR), None)
    print("  bases priced by the brain: "
          + ", ".join(f"#{k}={v}" for k, v in sorted(ids.items())))
    if id_near is None or id_far is None:
        print(f"FAIL: could not find both {BASE_NEAR} and {BASE_FAR} among the "
              f"priced bases {ids}")
        return None
    return ([r for r in rows if r.id == id_near],
            [r for r in rows if r.id == id_far], id_near, id_far)


def check_far_is_padsafe(far, variant, pill):
    """BASE_FAR is the control: ~17 tiles off, padsafe in both variants."""
    for r in far:
        if r.padhit:
            print(f"FAIL: t={r.tick} BASE_FAR {BASE_FAR} is "
                  f"{edist(pill, BASE_FAR):.2f} tiles from the pill "
                  f"{pill} -- far outside {PAD_THRESHOLD:.4f} -- yet it is "
                  f"priced padhit: '{r.chips.strip()}'")
            return False
        if not (r.padsafe_named or r.padsafe_none):
            print(f"FAIL: t={r.tick} BASE_FAR row carries no pad verdict at "
                  f"all: '{r.chips.strip()}'")
            return False
        if r.danger != 0.0:
            print(f"FAIL: t={r.tick} BASE_FAR danger term is {r.danger}, not 0 "
                  f"-- nothing in this arena can reach it: '{r.chips.strip()}'")
            return False
    print(f"  BASE_FAR: {len(far)} row(s), all padsafe with danger 0 "
          f"({edist(pill, BASE_FAR):.2f} tiles from the pill)")
    return True


# step_eval_queue pops TWO candidates per tick, so the two bases of a single
# replan land on ADJACENT ticks, never the same one.  Pair a row with the
# other base's nearest row within this many ticks to get one "round".
ROUND_WINDOW = 15


def rounds(near, far):
    """[(tick, near_score, far_score)] -- one entry per replan where both
    bases were priced within ROUND_WINDOW ticks of each other."""
    out = []
    for r in near:
        mate = min(far, key=lambda f: abs(f.tick - r.tick), default=None)
        if mate is not None and abs(mate.tick - r.tick) <= ROUND_WINDOW:
            out.append((r.tick, r.score, mate.score))
    return out


def check_cheaper(near, far):
    """BASE_NEAR must be priced below BASE_FAR in every round."""
    rr = rounds(near, far)
    if not rr:
        print(f"FAIL: the two bases were never priced within {ROUND_WINDOW} "
              "ticks of each other")
        return False
    for t, ns, fs in rr:
        if not ns < fs:
            print(f"FAIL: t={t} BASE_NEAR priced {ns:.1f}, not below "
                  f"BASE_FAR at {fs:.1f}")
            return False
    print(f"  BASE_NEAR was cheaper than BASE_FAR in all {len(rr)} priced "
          f"round(s) (e.g. t={rr[0][0]}: {rr[0][1]:.1f} vs {rr[0][2]:.1f})")
    return True


def check_refuelled_at_near(dumps):
    """Shells rise while the tank stands on BASE_NEAR."""
    at_near = [d for d in dumps if d[1] == BASE_NEAR]
    if not at_near:
        print(f"FAIL: the tank never stood on BASE_NEAR {BASE_NEAR}; last seen "
              f"at {dumps[-1][1]}")
        return False
    if at_near[-1][3] <= at_near[0][3]:
        print(f"FAIL: the tank stood on BASE_NEAR from t={at_near[0][0]} to "
              f"t={at_near[-1][0]} but its shells went {at_near[0][3]} -> "
              f"{at_near[-1][3]} -- it never refuelled there")
        return False
    print(f"  refuelled at BASE_NEAR: shells {at_near[0][3]} (t={at_near[0][0]})"
          f" -> {at_near[-1][3]} (t={at_near[-1][0]}) while standing on it")
    return True


def report_pick(goals, near, far, id_near, id_far):
    """The pick must follow the priced costs; report which base won."""
    refuels = [g for g in goals if g[1] == "refuel_at_base"]
    if not refuels:
        print("FAIL: the bot never set a refuel_at_base goal")
        return False
    # The pool re-prices on a ~50-tick cadence, so a goal set at tick t is
    # acting on the most recent row for each base at or before t.
    def latest(rows_, t):
        seen = [r for r in rows_ if r.tick <= t]
        return seen[-1] if seen else None

    checked = 0
    for tick, _, gid, tile in refuels:
        rn, rf = latest(near, tick), latest(far, tick)
        if rn is None or rf is None:
            continue
        checked += 1
        cheapest = id_near if rn.score < rf.score else id_far
        if gid != cheapest:
            print(f"FAIL: t={tick} refuel target is base#{gid} {tile} but "
                  f"base#{cheapest} was priced cheaper "
                  f"(#{id_near} {rn.score:.1f} @t={rn.tick} vs "
                  f"#{id_far} {rf.score:.1f} @t={rf.tick})")
            return False
    first = refuels[0]
    who = ("NEAR" if first[2] == id_near
           else "FAR" if first[2] == id_far else "?")
    print(f"  first refuel target: base#{first[2]} {first[3]} = {who} base at "
          f"t={first[0]}; pick matched the cheaper priced row on {checked} "
          f"goal-set tick(s) where both were priced")
    return True


def run_a(text):
    pill = PILL["A"]
    parsed = parse(text)
    if parsed is None:
        return 1
    rows, dumps, goals = parsed
    split = split_bases(rows)
    if split is None:
        return 1
    near, far, id_near, id_far = split

    # -- 2. every NEAR row is padsafe, and at least one names the pill at 9.0
    named = 0
    for r in near:
        if r.padhit:
            print(f"FAIL: t={r.tick} BASE_NEAR is padhit although the pill "
                  f"{pill} is {edist(pill, BASE_NEAR):.1f} tiles away, past "
                  f"the {PAD_THRESHOLD:.4f} threshold: '{r.chips.strip()}'")
            return 1
        m = r.padsafe_named
        if m:
            named += 1
            if abs(float(m.group(2)) - edist(pill, BASE_NEAR)) > 0.05:
                print(f"FAIL: t={r.tick} padsafe chip says the nearest pill is "
                      f"{m.group(2)} tiles away; the arena puts it at "
                      f"{edist(pill, BASE_NEAR):.1f}: '{r.chips.strip()}'")
                return 1
            if abs(float(m.group(3)) - PAD_THRESHOLD) > 0.01:
                print(f"FAIL: t={r.tick} padsafe chip's threshold is "
                      f"{m.group(3)}, not PILL_FIRE_RANGE {PILL_FIRE_RANGE} + "
                      f"REFUEL_PAD_TANK_OFFSET {REFUEL_PAD_TANK_OFFSET} = "
                      f"{PAD_THRESHOLD:.4f}")
                return 1
        elif not r.padsafe_none:
            print(f"FAIL: t={r.tick} BASE_NEAR row carries no pad verdict at "
                  f"all: '{r.chips.strip()}'")
            return 1
    if not named:
        print("FAIL: no BASE_NEAR row ever named the pill as out of reach -- "
              "was the pill alive at all?")
        return 1
    print(f"  BASE_NEAR: {len(near)} row(s), none padhit; {named} name the pill "
          f"at {edist(pill, BASE_NEAR):.1f} > {PAD_THRESHOLD:.2f}")

    # -- 3. the stamp really did put danger there, and the rule really cut it
    cuts = [(r, r.padcut) for r in near if r.padcut]
    if not cuts:
        print(f"FAIL: no BASE_NEAR row carries a padcut chip -- the "
              f"PILL_RANGE_MAP {PILL_RANGE_MAP} stamp never reached "
              f"{BASE_NEAR} ({edist(pill, BASE_NEAR):.1f} tiles from the pill), "
              "so there was nothing for the rule to remove and this arena "
              "proves nothing")
        return 1
    biggest = max(cuts, key=lambda c: float(c[1].group(1)))
    print(f"  pill layer removed: {len(cuts)} row(s) carry padcut, largest "
          f"-{biggest[1].group(1)} at t={biggest[0].tick} "
          f"(x REFUEL_DANGER_WEIGHT 20 = "
          f"{float(biggest[1].group(1)) * 20:.0f} cost that is now gone)")

    # -- 4. the danger term is the tank-only remainder, and there is no tank
    for r in near:
        if r.danger != 0.0:
            print(f"FAIL: t={r.tick} BASE_NEAR danger term is {r.danger}, not "
                  f"0 -- the pill layer was removed and the opponent is 30+ "
                  f"tiles away, so nothing may be left: '{r.chips.strip()}'")
            return 1
    print("  every BASE_NEAR danger term is 0 (tank-layer remainder; the "
          "opponent never gets within the 6-tile tank threat radius)")

    # -- 5/6/7
    if not check_far_is_padsafe(far, "A", pill):
        return 1
    if not check_cheaper(near, far):
        return 1
    if not report_pick(goals, near, far, id_near, id_far):
        return 1
    if not check_refuelled_at_near(dumps):
        return 1
    print("PASS (A): a pill 9.0 tiles from the base -- on the danger stamp's "
          f"rim but past the {PAD_THRESHOLD:.4f} pad threshold -- no longer "
          "prices the refuel, the base's danger term is 0, it is cheaper than "
          "the far base, and the tank refuelled there.")
    return 0


def run_b(text):
    pill = PILL["B"]
    parsed = parse(text)
    if parsed is None:
        return 1
    rows, dumps, goals = parsed
    split = split_bases(rows)
    if split is None:
        return 1
    near, far, id_near, id_far = split

    # -- 2. every NEAR row is padhit while the pill is alive.  A padsafe row
    #    naming NO pill is the honest answer after the bot has killed it; a
    #    padsafe row NAMING a pill would mean the reach test got it wrong.
    hits = 0
    for r in near:
        if r.padsafe_named:
            m = r.padsafe_named
            print(f"FAIL: t={r.tick} BASE_NEAR is padsafe against a named live "
                  f"pill #{m.group(1)} at {m.group(2)} tiles, but the arena's "
                  f"pill {pill} is {edist(pill, BASE_NEAR):.1f} tiles away -- "
                  f"inside {PAD_THRESHOLD:.4f}: '{r.chips.strip()}'")
            return 1
        m = r.padhit
        if m:
            hits += 1
            if abs(float(m.group(2)) - edist(pill, BASE_NEAR)) > 0.05:
                print(f"FAIL: t={r.tick} padhit chip says {m.group(2)} tiles; "
                      f"the arena puts the pill at "
                      f"{edist(pill, BASE_NEAR):.1f}: '{r.chips.strip()}'")
                return 1
            if abs(float(m.group(3)) - PAD_THRESHOLD) > 0.01:
                print(f"FAIL: t={r.tick} padhit chip's threshold is "
                      f"{m.group(3)}, not {PAD_THRESHOLD:.4f}")
                return 1
            if r.danger <= 0.0:
                print(f"FAIL: t={r.tick} BASE_NEAR is padhit but its danger "
                      f"term is {r.danger} -- the stamped value has to stand "
                      f"in full: '{r.chips.strip()}'")
                return 1
        elif not r.padsafe_none:
            print(f"FAIL: t={r.tick} BASE_NEAR row carries no pad verdict at "
                  f"all: '{r.chips.strip()}'")
            return 1
        if r.padcut:
            print(f"FAIL: t={r.tick} BASE_NEAR carries a padcut although the "
                  f"pill reaches the pad: '{r.chips.strip()}'")
            return 1
    if not hits:
        print("FAIL: no BASE_NEAR row was ever padhit -- the pill "
              f"{pill} is {edist(pill, BASE_NEAR):.1f} tiles away, inside "
              f"{PAD_THRESHOLD:.4f}")
        return 1
    dangers = sorted({r.danger for r in near if r.padhit})
    print(f"  BASE_NEAR: {len(near)} row(s), {hits} padhit at "
          f"{edist(pill, BASE_NEAR):.1f} <= {PAD_THRESHOLD:.2f}, none padsafe "
          f"against a live pill, no padcut; danger terms seen: "
          f"{', '.join('%.0f' % d for d in dangers)}")

    # -- 4/5.  One pill, two bases, two verdicts.
    if not check_far_is_padsafe(far, "B", pill):
        return 1
    if not report_pick(goals, near, far, id_near, id_far):
        return 1
    print("PASS (B): the same pill moved one tile closer (8.0 <= "
          f"{PAD_THRESHOLD:.4f}) is priced padhit, the base keeps its stamped "
          "danger in full, the far base stays padsafe, and the pick followed "
          "the priced costs.")
    return 0


def main():
    args = sys.argv[1:]
    use_asap = take_asap_flag(args)     # noqa: F841 (strips the flag)
    variants = []
    ticks = DEFAULT_TICKS
    build_dir = DEFAULT_BUILD
    i = 0
    while i < len(args):
        if args[i] == "--ticks" and i + 1 < len(args):
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build" and i + 1 < len(args):
            build_dir = Path(args[i + 1]); i += 2
        elif args[i].upper() in ("A", "B"):
            variants.append(args[i].upper()); i += 1
        else:
            i += 1
    if not variants:
        variants = ["A", "B"]
    print(pacing_line())
    rc = 0
    for v in variants:
        pill = PILL[v]
        print(f"=== refuel-pad reach, variant {v} ({ticks} ticks): neutral pill "
              f"{pill} is {edist(pill, BASE_NEAR):.1f} tiles from BASE_NEAR "
              f"{BASE_NEAR} (threshold {PAD_THRESHOLD:.4f}), "
              f"{edist(pill, BASE_FAR):.1f} from BASE_FAR {BASE_FAR}; spawn "
              f"{BOT_SPAWN} is mdist {mdist(BOT_SPAWN, BASE_NEAR)} / "
              f"{mdist(BOT_SPAWN, BASE_FAR)} from them")
        text = play(v, ticks, build_dir)
        if text is None:
            rc = 1
            continue
        rc = (run_a(text) if v == "A" else run_b(text)) or rc
    return rc


if __name__ == "__main__":
    sys.exit(main())
