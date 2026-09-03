#!/usr/bin/env python3
"""
Refuel low-stock markup -- a friendly base that is short of a resource the
tank needs is 10% dearer (GoalHunter 1.7, author's rule of 2026-09-03).

WHAT IS UNDER TEST (goals.lua refuel_low_stock_mult, constants.lua
REFUEL_LOW_ARMOUR_MULT / REFUEL_LOW_SHELLS_MULT):

    For each resource the tank NEEDS (below its armour / shell target), a
    refuel candidate whose FRESH observed stock of that resource is below
    REFUEL_MIN_STOCK (5) has its whole cost multiplied by 1.10.  Both short
    at once compounds to x1.21.  It is applied in both refuel scoring paths
    (nearest_resupply_base and the pool-1 block of step_eval_queue) and shows
    up as a `lowarm{x1.10}` / `lowsh{x1.10}` chip on the REFUEL_CAND row and
    in the pool-1 formula panel.  It is a markup on the short base, never a
    discount on the full one.

WHY THE TANK HAS TO NEED TWO THINGS
    The low_stock REJECT upstream drops any base that can supply NOTHING the
    tank needs.  A tank that needs only shells therefore never sees a
    shells-short base priced -- the reject eats it -- and the markup is for
    the base that gets PAST the reject.  So the arena makes the tank need
    both: TOURNAMENT + -ranked puts 0 shells in it, and a hidden mine on the
    only tile through a deep-sea barrier knocks its armour from 40 to 30.
    BASE_NEAR is pinned at armour 4 (below REFUEL_MIN_STOCK) and shells 6
    (at/above it): it can supply shells, so it survives the reject, and it is
    short of armour, so it is marked up.  BASE_FAR is full and pays nothing.
    The arena, and every number above, is documented in
    tests/generate_refuel_lowstock_map.py.

CHECKS
    1. No Lua error in the print2 log; the tank thought; it spawned with 0
       shells (the tournament loadout applied); its armour dropped below 40
       at some point (the mine fired).
    2. BEFORE the mine (armour still 40) no REFUEL_CAND row for BASE_NEAR
       carries a lowarm chip -- armour is not needed yet, so nothing may be
       marked up.  (Rows for BASE_NEAR may not exist at all before the mine:
       with only shells needed, the shells-6 base is priced but that is fine
       either way.)
    3. AFTER the mine every priced REFUEL_CAND row for BASE_NEAR carries
       exactly ` lowarm{x1.10}` and no lowsh chip; every row for BASE_FAR
       carries neither.  At least one such row must exist for each base.
    4. The pick follows the priced costs: on every tick where GOAL_CHANGE
       sets refuel_at_base and both bases were priced in that same tick,
       the chosen base is the one with the LOWER REFUEL_CAND score.  The
       test does not demand a fixed winner -- with only a 10% markup the
       near base, five tiles closer, may well still win -- it reports which
       one did.

Usage: python refuel_lowstock_test.py [--ticks N] [--build DIR] [--no-asap]
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
from generate_refuel_lowstock_map import (      # noqa: E402
    SPAWN, BASE_NEAR, BASE_FAR, GAP, NEAR_SHELLS, NEAR_ARMOUR, FULL_STOCK,
    REFUEL_LOW_ARMOUR_MULT, MINE_DAMAGE, mdist)

PORT = 50121
LABEL = "refuel_lowstock_test"
DEFAULT_TICKS = 1500

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so these are
# used with re.search, never re.match.
# Pool 1's per-candidate trace (goals.lua step_eval_queue, BRAIN_DEBUG_MODE
# only): one line per base the refuel pool priced, with the whole term
# breakdown and the low-stock chip in group 6.
CAND_RE = re.compile(
    r"REFUEL_P1 t=(\d+) base#(\S+) @\((\d+),(\d+)\) OK score=([\d.]+) = (.*?) "
    r"\| need arm=(\d+)/(\d+) sh=(\d+)/(\d+) obs_sh=(\S+) obs_arm=(\S+)")
LOWARM_RE = re.compile(r" lowarm\{x([\d.]+)\}")
LOWSH_RE = re.compile(r" lowsh\{x([\d.]+)\}")
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


def play(ticks, build_dir):
    """Run the arena and return bot 0's print2 log text, or None."""
    mapfile = HERE / "refuel_lowstock.map"
    final = HERE / "refuel_lowstock_final.json"
    stderr = HERE / "refuel_lowstock_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable, str(HERE / "generate_refuel_lowstock_map.py")],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORT),
           "-nolobby",
           # TOURNAMENT + -ranked is the only combination that puts 0 shells in
           # the tank on a scenario map -- see the generator's header.
           "-gametype", "tournament", "-ranked",
           # hidden mines (the default, made explicit): the bot must NOT see
           # the mine in the gap, or it would route around... there is no
           # around, but it would try.
           "-hiddenmines", "yes",
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

    sess = newest_session(build_dir, LABEL)
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


def run(text):
    # -- 1. sanity
    if LUA_ERR_RE.search(text):
        for line in text.splitlines():
            if LUA_ERR_RE.search(line):
                print(f"FAIL: Lua error in the print2 log: {line.strip()}")
                return 1
    dumps = [(int(m.group(1)), int(m.group(2)), int(m.group(3)),
              int(m.group(4)), int(m.group(5))) for m in DUMP_RE.finditer(text)]
    if not dumps:
        print("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return 1
    print(f"  tank: t={dumps[0][0]} at ({dumps[0][1]},{dumps[0][2]}) "
          f"arm={dumps[0][3]} sh={dumps[0][4]} -> t={dumps[-1][0]} at "
          f"({dumps[-1][1]},{dumps[-1][2]}) arm={dumps[-1][3]} sh={dumps[-1][4]}")
    if dumps[0][4] != 0:
        print(f"FAIL: the tank spawned with {dumps[0][4]} shells, not 0 -- the "
              "TOURNAMENT loadout did not apply (is -ranked still on the "
              "command line?)")
        return 1
    hit = next((d for d in dumps if d[3] < 40), None)
    if hit is None:
        print(f"FAIL: the tank's armour never dropped below 40 -- it never "
              f"crossed the mine at {GAP} (it has to, to reach either base). "
              f"Last position ({dumps[-1][1]},{dumps[-1][2]}).")
        return 1
    mine_tick = hit[0]
    print(f"  mine at {GAP} hit before t={mine_tick}: armour {hit[3]} "
          f"(expected {40 - MINE_DAMAGE}); armour is now NEEDED")

    # -- per-tick candidate rows
    rows = []          # (tick, id, tile, score, chips)
    goals = []         # (tick, kind, id, tile)
    for m in CAND_RE.finditer(text):
        rows.append((int(m.group(1)), m.group(2),
                     (int(m.group(3)), int(m.group(4))),
                     float(m.group(5)), m.group(6),
                     int(m.group(7)) < int(m.group(8))))   # armour needed?
    for tick, block in tick_blocks(text):
        for m in GOAL_RE.finditer(block):
            goals.append((tick, m.group(1), m.group(2),
                          (int(m.group(3)), int(m.group(4)))))
    ids = {}
    for r in rows:
        ids[r[1]] = r[2]
    id_near = next((k for k, v in ids.items() if v == BASE_NEAR), None)
    id_far = next((k for k, v in ids.items() if v == BASE_FAR), None)
    print("  bases priced by the brain: "
          + ", ".join(f"#{k}={v}" for k, v in sorted(ids.items())))
    if id_near is None or id_far is None:
        print(f"FAIL: could not find both {BASE_NEAR} and {BASE_FAR} among the "
              f"priced bases {ids}")
        return 1
    print(f"  BASE_NEAR {BASE_NEAR} is base#{id_near} (pinned armour "
          f"{NEAR_ARMOUR}, shells {NEAR_SHELLS}); BASE_FAR {BASE_FAR} is "
          f"base#{id_far} (full, {FULL_STOCK}/{FULL_STOCK})")

    # -- 2. before the mine: nothing marked up
    pre_near = [r for r in rows if r[0] < mine_tick and r[1] == id_near]
    bad = [r for r in pre_near if LOWARM_RE.search(r[4])]
    if bad:
        print(f"FAIL: at t={bad[0][0]}, before the mine (armour still 40), the "
              f"near base already carried a lowarm chip: {bad[0][4].strip()}")
        return 1
    print(f"  before the mine: {len(pre_near)} near-base row(s), none marked up")

    # -- 3. after the mine: near marked up (armour only), far not
    post = [r for r in rows if r[0] >= mine_tick]
    near = [r for r in post if r[1] == id_near]
    far = [r for r in post if r[1] == id_far]
    if not near or not far:
        print(f"FAIL: after the mine {len(near)} near row(s) and {len(far)} far "
              "row(s) -- both bases have to be priced for the comparison to "
              "mean anything.")
        return 1
    # The markup is "for a resource we NEED": once the tank has refilled its
    # armour (it does, at the full base) the near base is no longer short of
    # anything needed and the chip must go away again. So: chip iff needed.
    needed_rows = [r for r in near if r[5]]
    if not needed_rows:
        print("FAIL: no near-base row after the mine while armour was still "
              "needed -- nothing to check the markup on")
        return 1
    for r in near:
        m = LOWARM_RE.search(r[4])
        if r[5] and not m:
            print(f"FAIL: t={r[0]} near base row has no lowarm chip although "
                  f"armour is needed and its stock is {NEAR_ARMOUR}: "
                  f"'{r[4].strip()}' (obs may have gone stale -- the sidecar "
                  "re-asserts it every 150 ticks)")
            return 1
        if m and not r[5]:
            print(f"FAIL: t={r[0]} near base row carries lowarm although armour "
                  f"is NOT needed any more: '{r[4].strip()}'")
            return 1
        if m and abs(float(m.group(1)) - REFUEL_LOW_ARMOUR_MULT) > 1e-6:
            print(f"FAIL: lowarm chip says x{m.group(1)}, constants.lua says "
                  f"x{REFUEL_LOW_ARMOUR_MULT}")
            return 1
        if LOWSH_RE.search(r[4]):
            print(f"FAIL: t={r[0]} near base row carries a lowsh chip; with "
                  f"{NEAR_SHELLS} shells (>= REFUEL_MIN_STOCK) it is not "
                  "short of shells")
            return 1
    for r in far:
        if LOWARM_RE.search(r[4]) or LOWSH_RE.search(r[4]):
            print(f"FAIL: t={r[0]} the FULL base carries a low-stock chip: "
                  f"'{r[4].strip()}'")
            return 1
    print(f"  after the mine: {len(needed_rows)} near-base rows while armour was "
          f"needed, all carry lowarm{{x{REFUEL_LOW_ARMOUR_MULT:.2f}}} and no "
          f"lowsh; {len(near) - len(needed_rows)} after the refill carry none; "
          f"{len(far)} far-base rows carry no chip")

    # -- 4. the pick follows the priced costs
    refuels = [g for g in goals if g[1] == "refuel_at_base"]
    if not refuels:
        print("FAIL: the bot never set a refuel_at_base goal")
        return 1
    checked = 0
    for tick, _, gid, tile in refuels:
        same = [r for r in rows if r[0] == tick]
        have = {r[1]: r[3] for r in same}
        if id_near in have and id_far in have:
            checked += 1
            cheapest = min(have, key=have.get)
            if gid != cheapest:
                print(f"FAIL: t={tick} refuel target is base#{gid} {tile} but "
                      f"base#{cheapest} was priced cheaper that tick "
                      f"({have[cheapest]:.0f} vs {have[gid]:.0f})")
                return 1
    first = refuels[0]
    who = "NEAR" if first[2] == id_near else "FAR" if first[2] == id_far else "?"
    print(f"  first refuel target: base#{first[2]} {first[3]} = {who} base at "
          f"t={first[0]}; pick matched the cheaper priced row on {checked} "
          f"goal-set tick(s) where both were priced")
    print("PASS: the armour-short base was marked up x"
          f"{REFUEL_LOW_ARMOUR_MULT:.2f} once armour was needed, the full base "
          "was not, and the pick followed the priced costs.")
    return 0


def main():
    args = sys.argv[1:]
    use_asap = take_asap_flag(args)     # noqa: F841 (strips the flag)
    ticks = DEFAULT_TICKS
    build_dir = DEFAULT_BUILD
    i = 0
    while i < len(args):
        if args[i] == "--ticks" and i + 1 < len(args):
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build" and i + 1 < len(args):
            build_dir = Path(args[i + 1]); i += 2
        else:
            i += 1
    print(pacing_line())
    print(f"=== refuel low-stock markup ({ticks} ticks; near base "
          f"{mdist(SPAWN, BASE_NEAR)} tiles, far base "
          f"{mdist(SPAWN, BASE_FAR)} tiles from the spawn)")
    text = play(ticks, build_dir)
    if text is None:
        return 1
    return run(text)


if __name__ == "__main__":
    sys.exit(main())
