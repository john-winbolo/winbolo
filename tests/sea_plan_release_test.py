#!/usr/bin/env python3
"""The sea-harvest plan ends with the goal (GoalHunter 1.7).

Field incident 20260903_105448 bot2.  A deep-sea harvest was planned at
~t=15000; its LGM died at t=28997.  Nothing retired `state._sea_live`, so for
the remaining twenty minutes builder_pool.tree_reserve went on adding the
plan's 21 trees -- `BUILDER_POOL ... (base 4 + pills 16 + goal 0 + sea 21)`
against 21 trees -- and every repair the bot's own goal asked for came back
`BP_DENY ... reason=tree_reserve(41,21,1)`.  The bot stood beside three
damaged friendly pills and fixed none of them.

WHAT IS UNDER TEST (goals.lua M.sea_update, the goal-change release)
    The plan lives on `state` only so that the capture_pill goal OBJECT being
    rebuilt every replan does not restart the chain at entrance_plan.  That is
    a same-goal concern, so the plan -- and its tree reserve -- now end the
    tick the goal stops being that harvest.  T = 0, no grace window.  Both
    readers (the `sea` term of the reserve, and builder.decide's
    `sea_trees_reserved` rung) are keyed on the live plan, so both see it go
    on the same tick.

THE ARENA (tests/generate_sea_release_map.py)
    The sea_pills variant-A peninsula, plus a WORN friendly pill five tiles
    from our base and four spare pills parked far out at sea.  Variant
    `release` hands the WHOLE RAFT to the tank at sim tick 400, mid-chain --
    "somebody else got there first".  The pills are in a tank, so
    filter_capture_pill drops them and the committed capture goal is
    invalidated, which is the only lever that reliably ends a goal this brain
    is committed to (a cheaper rival does not: a placement priced 2.5 against
    the harvest's 5.0 still lost by 230 once the switch flat and the
    commitment were added).  No enemy tank is involved, so none of the
    harvest's own safety gates can be what retires the plan.  Variant
    `control` hands over nothing and must still take all three.

CHECKS (release)
    1. No Lua error, the bot thought, and a sea plan really went live (the
       reserve's `sea` term was the plan's 21 for a stretch of ticks).
    2. A `SEA_RELEASE ... reason=goal_changed` line exists, and it lands on
       the FIRST tick the goal was not that harvest -- every BUILDER_POOL
       verdict between the plan going live and that tick reads
       owner=capture_pill.
    3. From that tick on the reserve's `sea` term is 0 -- the same tick, not
       the next one.  The tick before it was still the plan's 21.
    4. The printed reserve still reproduces from its own chips on every
       verdict line -- base + pills + goal + sea == res -- so the number the
       `sea` term dropped out of is the number the pool actually charged rows
       against.  (What the tank then does with the freed wood -- the repair
       the frozen reserve used to refuse -- is tests/reserve_seeded_test.py.)

CHECKS (control)
    5. All three sea pills are taken, exactly as sea_pills variant A takes
       them: releasing the plan on a goal change must not cost the harvest
       that never changes goal.

Usage: python sea_plan_release_test.py [--variant release|control|ALL]
                                       [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import json
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
sys.path.insert(0, str(HERE))
from generate_sea_release_map import GIVE_TICK, VARIANTS  # noqa: E402
from generate_sea_pills_map import SEA_PILLS  # noqa: E402

PORTS = {"release": 50141, "control": 50142}
DEFAULT_TICKS = 6000
SEA_PILL_TREES_TOTAL = 21          # constants.lua (LGM_COST_BOAT 20 + MINE 1)

# builder_pool.lua, one verdict line per tick:
#   BUILDER_POOL t=123 owner=capture_pill/sea_mine/lay_mine elig=yes cands=1
#     ok=0 trees=40/res=46 (base 4 + pills 0 + goal 21 + sea 21) reserve_eta=...
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=.*? cands=(\d+) ok=(\d+) "
    r"trees=(\d+)/res=(\d+) \(base (\d+) \+ pills (\d+) \+ goal (\d+) "
    r"\+ sea (\d+)\)")
# goals.lua sea_update, the goal-change release:
#   SEA_RELEASE t=200 reason=goal_changed goal=place_pill_strategic sub=lay_mine
#     cluster=1 trees_need=21 -- ...
RELEASE_RE = re.compile(
    r"SEA_RELEASE t=(\d+) reason=(\S+) goal=(\S+) sub=(\S+) cluster=(\S+) "
    r"trees_need=(\d+)")
DISPATCH_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)(\s+seeded_by=\S+)?")
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(\S+)")
DUMP_RE = re.compile(r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\)")
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare"
                        r"|perform|concatenate))")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    root = build_dir / "debug_sessions"
    if not root.is_dir():
        return None
    cands = [d for d in root.iterdir() if d.is_dir() and label in d.name]
    return max(cands, key=lambda d: d.stat().st_mtime) if cands else None


def play(variant, ticks, build_dir, out):
    label = f"sea_release_{variant}"
    mapfile = HERE / f"sea_release_{variant}.map"
    final = HERE / f"sea_release_{variant}_final.json"
    stderr = HERE / f"sea_release_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        out.append(f"FAIL: WinBoloDS not found under {build_dir}")
        return None, None
    subprocess.run([sys.executable, str(HERE / "generate_sea_release_map.py"),
                    "--variant", variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                out.append(f"FAIL: {p.name} is locked -- a previous WinBoloDS "
                           f"run is still going.")
                return None, None

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label,
               WINBOLO_BRAIN_TIER="10")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby", "-gametype", "open",
           "-bots", "1", "-brain", str(BRAIN),
           # yesfull: the arena is small and the test is about WHEN the plan is
           # dropped, not about discovering the shore.
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        out.append("FAIL: no debug session produced (is the cwd on a drive "
                   "with >50 GB free? -brain-debug records nothing otherwise)")
        return None, None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        out.append(f"FAIL: brain crashed -- see {crashes[0]}")
        out.append(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return None, None
    log = sess / "print2_bot0.log"
    if not log.exists():
        out.append(f"FAIL: no print2_bot0.log under {sess}")
        return None, None
    out.append(f"  session: {sess.name}")
    fd = json.load(open(final)) if final.exists() else None
    return log.read_text(encoding="utf-8", errors="replace"), fd


def collected_count(fd):
    """Sea pills that are no longer lying dead where they started."""
    if not fd:
        return None
    n = 0
    for (x, y) in SEA_PILLS:
        lying = any((pb.get("tx"), pb.get("ty")) == (x, y)
                    and not pb.get("in_tank") and (pb.get("armor") or 0) == 0
                    for pb in fd.get("pillboxes", []))
        if not lying:
            n += 1
    return n


def check_release(text, fd, out):
    # One tuple per verdict line: tick, goal kind, `sea` chip, trees in hand,
    # reserve total, `base` chip, `pills` chip, `goal` chip.
    pool = [(int(m.group(1)), m.group(2).split("/")[0], int(m.group(10)),
             int(m.group(5)), int(m.group(6)),
             int(m.group(7)), int(m.group(8)), int(m.group(9)))
            for m in POOL_RE.finditer(text)]
    releases = RELEASE_RE.findall(text)
    dispatches = DISPATCH_RE.findall(text)
    denies = DENY_RE.findall(text)

    if LUA_ERR_RE.search(text):
        bad = next(ln for ln in text.splitlines() if LUA_ERR_RE.search(ln))
        out.append(f"FAIL: Lua error in the print2 log: {bad.strip()}")
        return 1
    if not DUMP_RE.search(text):
        out.append("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return 1
    if not pool:
        out.append("FAIL: no BUILDER_POOL verdict lines at all")
        return 1

    # -- 1. the plan really went live --------------------------------------
    live = [p for p in pool if p[2] > 0]
    if not live:
        out.append("FAIL (1): the reserve's `sea` term was never non-zero -- "
                   "no sea plan was ever live, so there is nothing to release. "
                   "Check the harvest ran at all (SEA_PLAN / SEA_SUB lines).")
        return 1
    first_live, last_live = live[0][0], live[-1][0]
    seas = sorted({p[2] for p in live})
    out.append(f"  sea plan live on {len(live)} verdict tick(s) "
               f"t={first_live}..{last_live}, reserve `sea` term {seas} "
               f"(SEA_PILL_TREES_TOTAL={SEA_PILL_TREES_TOTAL})")

    # -- 2. the release line, on the first non-harvest tick -----------------
    goal_changed = [r for r in releases if r[1] == "goal_changed"]
    if not goal_changed:
        kinds = sorted({p[1] for p in pool if p[0] >= first_live})
        out.append("FAIL (2): no `SEA_RELEASE ... reason=goal_changed` line. "
                   f"Goals seen while the plan was live: {', '.join(kinds)}. "
                   f"If capture_pill is the only one, the give at sim tick "
                   f"{GIVE_TICK} did not move the goal and the arena, not the "
                   f"brain, is what failed.")
        return 1
    rel_tick = int(goal_changed[0][0])
    out.append(f"  SEA_RELEASE t={rel_tick} reason=goal_changed "
               f"goal={goal_changed[0][2]} sub={goal_changed[0][3]} "
               f"cluster={goal_changed[0][4]} "
               f"trees_need={goal_changed[0][5]}")
    early = [p for p in pool
             if first_live <= p[0] < rel_tick and p[1] != "capture_pill"]
    if early:
        out.append(f"FAIL (2): the goal was already {early[0][1]} at t="
                   f"{early[0][0]}, {rel_tick - early[0][0]} tick(s) before "
                   f"the plan was released. T = 0 means the release is on the "
                   f"FIRST such tick, not a later one.")
        return 1
    out.append(f"  2 OK: the goal was capture_pill on every one of the "
               f"{len([p for p in pool if first_live <= p[0] < rel_tick])} "
               f"verdict tick(s) before t={rel_tick}, and became "
               f"{goal_changed[0][2]} on it")

    # -- 3. the reserve reads sea 0 on that same tick -----------------------
    at = [p for p in pool if p[0] == rel_tick]
    if not at:
        out.append(f"FAIL (3): no BUILDER_POOL verdict on the release tick "
                   f"{rel_tick} -- cannot say what the reserve read.")
        return 1
    if at[0][2] != 0:
        out.append(f"FAIL (3): on the release tick t={rel_tick} the reserve "
                   f"still read sea {at[0][2]}. The release has to be visible "
                   f"to the pool on the SAME tick (sea_update runs before "
                   f"builder_pool.update).")
        return 1
    before = [p for p in pool if p[0] < rel_tick]
    prev = before[-1] if before else None
    still = [p for p in pool if p[0] > rel_tick and p[2] > 0]
    out.append(f"  3 OK: reserve `sea` {prev[2] if prev else '?'} at t="
               f"{prev[0] if prev else '?'} -> 0 at t={rel_tick} "
               f"(res {prev[4] if prev else '?'} -> {at[0][4]}, trees "
               f"{at[0][3]})"
               + (f"; a LATER plan went live again at t={still[0][0]}"
                  if still else "; no plan went live again"))

    # -- 4. the printed reserve reproduces from its printed chips ----------
    # base + pills + goal + sea == res, on every line. The release moves the
    # `sea` chip to 0; the TOTAL moves by less than trees_need whenever another
    # chip moves on the same tick (here the raft lands in the tank, so `pills`
    # goes 0 -> PILL_PLACE_TREE_COST), and the sum is what says so.
    bad = [q for q in pool if q[5] + q[6] + q[7] + q[2] != q[4]]
    if bad:
        b = bad[0]
        out.append(f"FAIL (4): t={b[0]} the reserve chips do not add up: "
                   f"base {b[5]} + pills {b[6]} + goal {b[7]} + sea {b[2]} "
                   f"= {b[5] + b[6] + b[7] + b[2]}, but res={b[4]}")
        return 1
    need = int(goal_changed[0][5])
    out.append(f"  4 OK: base+pills+goal+sea == res on all {len(pool)} verdict "
               f"line(s); across the release res {prev[4]} (base {prev[5]} + "
               f"pills {prev[6]} + goal {prev[7]} + sea {prev[2]}) -> "
               f"{at[0][4]} (base {at[0][5]} + pills {at[0][6]} + goal "
               f"{at[0][7]} + sea {at[0][2]}), the plan's {need} trees gone")
    out.append("PASS (release): the plan and its 21-tree reserve ended on the "
               "first tick the goal was not the harvest, and the "
               "wood it was holding went back to the tank.")
    return 0


def check_control(text, fd, out):
    if LUA_ERR_RE.search(text):
        bad = next(ln for ln in text.splitlines() if LUA_ERR_RE.search(ln))
        out.append(f"FAIL: Lua error in the print2 log: {bad.strip()}")
        return 1
    got = collected_count(fd)
    out.append(f"  sea pills taken: {got}/{len(SEA_PILLS)}")
    releases = RELEASE_RE.findall(text)
    out.append(f"  SEA_RELEASE lines: {len(releases)}"
               + (f" (first t={releases[0][0]} reason={releases[0][1]})"
                  if releases else ""))
    if got != len(SEA_PILLS):
        out.append(f"FAIL (5): only {got}/{len(SEA_PILLS)} sea pills were "
                   f"taken. The control tank is never asked to change goal, so "
                   f"the harvest must run exactly as sea_pills variant A runs "
                   f"it.")
        return 1
    out.append("PASS (control): the tank that is left alone still harvests the "
               "whole raft -- releasing the plan on a goal change costs the "
               "harvest that never changes goal nothing.")
    return 0


def run_one(variant, ticks, build_dir):
    out = []
    text, fd = play(variant, ticks, build_dir, out)
    if text is None:
        return 1, out
    rc = (check_release if variant == "release" else check_control)(text, fd, out)
    return rc, out


def main():
    args = sys.argv[1:]
    take_asap_flag(args)
    variant, ticks, build = "ALL", DEFAULT_TICKS, DEFAULT_BUILD
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
    print(pacing_line(""))
    print(f"=== sea-plan release ({ticks} sim ticks; the release variant has "
          f"the raft handed to the tank at sim tick {GIVE_TICK})")
    todo = list(VARIANTS) if variant == "ALL" else [variant]
    rc = 0
    for v in todo:
        r, lines = run_one(v, ticks, build)
        print(f"-- variant {v} " + "-" * 46)
        for line in lines:
            print(line)
        rc |= r
    return rc


if __name__ == "__main__":
    try:
        sys.exit(main())
    except subprocess.TimeoutExpired:
        print("FAIL: run timed out")
        sys.exit(1)
