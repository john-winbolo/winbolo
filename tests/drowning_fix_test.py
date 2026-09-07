#!/usr/bin/env python3
"""drowning FIX -- the two knobs from the 2026-09-06 drowning analysis.

WHAT THIS FILE IS FOR
---------------------
tests/drowning_test.py already covers the two knobs from the 2026-09-05
incident (CLIFF_RAY_CORNER_CHECK, CLIFF_STOP_MASK_ALL_GOALS) on arenas A and B.
This file covers the two that came out of the 2026-09-06 tally of 110 drownings
(28% of all deaths in 2v2 Oil Rig), and it adds arenas rather than touching
those:

  C.CLIFF_BRAKE_STICKY            arena A3
      Both cliff guards are re-decided from scratch every tick off a heading
      ray.  While the tank turns, sub-tile jitter moves the ray a couple of
      brads and it misses the sea tile it hit last tick -- and on that tick
      navigate runs normally and returns KEY_FASTER.  Net forward creep with a
      brake that "fired": 20260906_184924_1_drown13 bot0 t=29523 drowned at
      FULL armour with nobody near it, having braked on 5 of its last 23 ticks.
      ON, a guard that names a deep-sea tile latches "no forward throttle" for
      C.CLIFF_BRAKE_STICKY_TICKS brain ticks, applied at M.steer's single key
      exit point.  Turn keys pass through, so the tank keeps rotating out.

  C.PF_NEXTSTEP_FOOT_SEA_RULE     arena A4
      brainPathfinderDijkstraNextStep's two 8-neighbour fallbacks picked a
      neighbour by min(g_land, g_boat) with no on-foot passability test and no
      diagonal-corner rule, while the A* expansion (brain_pathfinder.c:1514)
      and the Dijkstra edge builder (:2024) both refuse a deep-sea step and a
      deep-sea corner cut for a boatless tank.  `nav next=` named a deep-sea
      tile on 14 of the 28 recorded drownings.  ON, both fallbacks -- and the
      traced-chain step, which enters on whichever boat layer is cheaper at the
      DESTINATION and can hand a boatless tank a boat-route step -- apply the
      same rule, and a boatless tank's descent reads the LAND layer's g only.

Both knobs default ON and both are false in PRESETS.keel, so every arena runs
twice: once on the defaults and once on `preset=keel`, and the difference
between the two runs is the only thing either arena claims to measure.

THE ARENAS (tests/generate_drowning_fix_map.py builds the ground; its docstring
carries the geometry)

  A3  THE NOTCH LEDGE.  Arena A's proven two-tile staircase descending
      south-west -- sea due south, land to the south-west, tank hugging a
      column boundary, which is the geometry that makes the brake fire at all
      -- ending in a ONE-TILE-WIDE ledge with deep sea both north AND south of
      every tile, and the map's only base at its tip.  Every wobble off due
      west puts a sea tile on the diagonal.
      Asserted:
        * the tank never stands on a deep-sea tile and never dies, read from
          the ENGINE (game.map_tile under the tank, in the sidecar's trace);
        * IT STILL CAPTURES THE BASE.  This is the assertion that matters most
          for this knob: a sticky "no forward throttle" latch is exactly the
          kind of rule that pins a tank at a shoreline forever, and a ledge
          with water on both sides is the shape it would pin on.
        * with the defaults CLIFF_STICKY latches at least once, and with
          preset=keel the line never appears -- which is what ties the numbers
          below to the knob and not to the arena.
      Reported, not asserted: the closest the tank's centre ever gets to the
      first sea tile a brake named, in world units, for each config.  That is
      the creep, measured on the ground.

  A4  THE BOAT-SEEDED LEDGE.  A one-tile-wide grass ledge running south to the
      base with deep sea on BOTH flanks, and a line of BOAT tiles in the water
      cardinally adjacent to it.  The Dijkstra is seeded AT THE TANK with the
      tank's own boat state (init.lua:2077), so a boatless tank only reaches
      boat-layer nodes through a TT_BOAT tile -- on Oil Rig those are
      everywhere (every death leaves one), here they are placed.  Without them
      deep sea is a wall on the land layer, every sea tile carries g =
      COST_INF, and neither the old code nor the new one could name it.  The
      base is on the SAME landmass: grass costs 2 and open sea afloat costs
      2.5, so walking beats sailing and the traced chain stays on land.
      Asserted:
        * with the defaults, NO `nav: dij ... next=` line ever names a deep-sea
          tile while the tank is on land -- that is the fix, stated directly;
        * the tank never stands on a deep-sea tile, never dies, and still
          captures the base;
        * if the keel control DOES name deep sea (the control reproduces the
          bug), the defaults must also print at least one PF_SEA_VETO.
      If the keel control names no deep-sea next step the arena has not
      reproduced the bug; the test says so in as many words and does not
      pretend otherwise.

WHAT THESE ARENAS DO NOT CLAIM
------------------------------
Neither arena is expected to DROWN the preset=keel control, and the run output
says plainly whether it did.  The same thing was true of arenas A and B (see
the note in tests/drowning_test.py): the scenario API has no hook that places a
tank -- src/server/scenario.c scBuildGameTable exposes game.tank to READ one
and game.set_tile to rewrite the ground, and that is all -- so every approach
has to fall out of a drive, and a drive that reaches a shoreline under a
working cliff brake usually survives it.  What is proved here is that each rule
FIRES on ground shaped like the incident, that it changes the measured
behaviour, and that it does not freeze the tank or cost it its goal.

Usage: python drowning_fix_test.py [--variant A3|A4|all] [--ticks N]
                                   [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import argparse
import glob
import math
import os
import re
import subprocess
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
from asap import asap_args                                  # noqa: E402
import generate_drowning_fix_map as G                       # noqa: E402

VARIANTS = ("A3", "A4")
# One port per (variant, config) so two runs never collide.  50420+ is this
# file's block; drowning_test.py owns 50291-50294.
PORTS = {("A3", "def"): 50420, ("A3", "keel"): 50421,
         ("A4", "def"): 50422, ("A4", "keel"): 50423}
# ENGINE ticks.  A3's walk down the staircase and out along the ledge finishes
# well under 2000; the rest is the tank milling on the shoreline, which is
# where the brakes come from.  A4's walk south is shorter but the interesting
# part is the whole time it spends beside the water.
TICKS = {"A3": 8000, "A4": 8000}
# One bot each.  A4 was tried with two allied bots, to force the live-obstacle
# veer by having them queue down the one-tile ledge; they mined each other on
# the spawn block and never reached the base, which measured the arena instead
# of the knob.  See generate_drowning_fix_map.py for the rest of that note.
BOTS = {"A3": 1, "A4": 1}
CONFIGS = {"def": "", "keel": "preset=keel"}
ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

DEEP_SEA_ENGINE = 255               # what game.map_tile reports for deep sea
T_DEEPSEA = 10                      # constants.lua, and the .map nibble
P0 = 0

# print2 lines this test reads.
#   CLIFF_BRAKE t=85 goal=capture_base tile_mx/my=(129,121) dist_wu=256 ...
#   CLIFF_STICKY t=85 tile=(129,121) left=8
#   nav: dij (130,121)->(119,132) next=(129,121)
#   PF_SEA_VETO t=85 from=(130,121) rejected=(129,122) picked=(129,121)
BRAKE_RE = re.compile(
    r"CLIFF_BRAKE t=(\d+) goal=(\S+) tile_mx/my=\((\d+),(\d+)\) ")
STICKY_RE = re.compile(
    r"CLIFF_STICKY t=(\d+) tile=\((-?\d+),(-?\d+)\) left=(\d+)")
NAV_RE = re.compile(
    r"nav: dij \((\d+),(\d+)\)->\((\d+),(\d+)\) next=\((\d+),(\d+)\)")
VETO_RE = re.compile(
    r"PF_SEA_VETO t=(\d+) from=\((-?\d+),(-?\d+)\) rejected=\((-?\d+),(-?\d+)\) "
    r"picked=\((-?\d+),(-?\d+)\)")


# ── the sidecar's engine-side trace ──────────────────────────────────────
# tick wx wy dir mx my terrain dead base_owner boat
def read_trace(build_dir, variant):
    path = build_dir / f"drowning_fix_{variant}_trace.log"
    rows = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 10:
                rows.append([int(p) for p in parts])
    return rows


def deaths(rows):
    """Rising edges of the `dead` column: one per death, not one per tick."""
    return [r for i, r in enumerate(rows)
            if r[7] == 1 and (i == 0 or rows[i - 1][7] == 0)]


def wet_rows(rows, after_tick):
    """Rows on which the tank is over deep sea WITHOUT a boat -- i.e. rows on
    which the engine is about to drown it (tank.c's drowning branch is exactly
    onBoat == FALSE and mapGetPos == DEEP_SEA).

    The boat column is what makes this honest rather than approximate: the
    start square HAS to be deep sea (starts.c startsIsValidSquare) and the tank
    sits on it AFLOAT until the sidecar fills the pond in, so a plain
    "terrain == deep sea" test flags the spawn every single run.  `after_tick`
    is kept as a second belt for the same reason."""
    return [r for r in rows
            if r[6] == DEEP_SEA_ENGINE and r[9] == 0 and r[0] > after_tick]


def captured_tick(rows):
    for r in rows:
        if r[8] == P0:
            return r[0]
    return None


def closest_wu(rows, tile, after_tick):
    """Closest the tank's CENTRE ever gets to the centre of `tile`, in world
    units, over the rows after `after_tick`.  This is the creep measured on the
    ground: a tank that brakes and then creeps forward between brakes ends up
    nearer the tile it braked for than one whose throttle stayed off."""
    if not tile:
        return None
    cx = tile[0] * 256 + 128
    cy = tile[1] * 256 + 128
    best = None
    for r in rows:
        if r[0] <= after_tick or r[7] == 1:
            continue
        d = math.hypot(r[1] - cx, r[2] - cy)
        if best is None or d < best:
            best = d
    return best


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def run_sim(variant, config, ticks, build_dir):
    """Runs one (arena, config); returns (trace_rows, print2_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_drowning_fix_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"drownfix_{variant}_{config}"
    trace = build_dir / f"drowning_fix_{variant}_trace.log"
    stderr = HERE / f"drowning_fix_{variant}_{config}_stderr.txt"
    for p in (trace, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    tokens = CONFIGS[config]
    if len(tokens) > ARG_MAX:
        return None, (f"the -bot-init token string is {len(tokens)} bytes, over "
                      f"the {ARG_MAX}-byte BRAIN_INIT_ARG limit")
    nbots = BOTS[variant]
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"drowning_fix_{variant}.map"),
           "-port", str(PORTS[(variant, config)]), "-nolobby",
           "-gametype", "open", "-bots", str(nbots), "-brain", str(BRAIN),
           "-bot-init", (f"0={BRAIN}[{tokens}]" if nbots == 1
                         else f"0-{nbots - 1}={BRAIN}[{tokens}]"),
           # yesfull: the arenas are about STEERING and PATHING, not about
           # finding a base on a map that has exactly one thing on it.
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "1", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(600, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000])
    # Every bot's log, concatenated: on A4 both allies walk the same ledge and
    # either of them can be the one whose fallback names a sea tile.
    texts = []
    for b in range(nbots):
        p = sess / f"print2_bot{b}.log"
        if not p.exists():
            return None, f"no print2_bot{b}.log under {sess}"
        texts.append(p.read_text(errors="ignore"))
    return read_trace(build_dir, variant), "\n".join(texts)


# ── the checks ───────────────────────────────────────────────────────────
def sea_tiles(variant):
    """The arena's deep-sea tiles, straight off the generated grid, so
    'is that next step in the water' is answered from the ground."""
    grid = G.spots(variant)["grid"]
    return {(x, y) for y in range(G.MAP_SIZE) for x in range(G.MAP_SIZE)
            if grid[y][x] is G.DEEP_SEA}


def land_tiles(variant):
    grid = G.spots(variant)["grid"]
    return {(x, y) for y in range(G.MAP_SIZE) for x in range(G.MAP_SIZE)
            if grid[y][x] is not G.DEEP_SEA}


def wet_next_steps(text, variant, spawn):
    """`nav: dij` lines whose NEXT tile is deep sea while the tank's own tile
    is land -- the boatless-tank-told-to-drive-into-the-sea line.  The spawn
    pond is excluded: it is deep sea by construction and the tank is afloat on
    it for the first few dozen ticks."""
    sea = sea_tiles(variant)
    land = land_tiles(variant)
    out = []
    for m in NAV_RE.finditer(text):
        sx, sy, _dx, _dy, nx, ny = (int(g) for g in m.groups())
        if (sx, sy) in land and (nx, ny) in sea and (nx, ny) != tuple(spawn):
            out.append(((sx, sy), (nx, ny)))
    return out


def check_A3(res, ticks, out):
    ok = True
    fill_by = 200                      # the pond is filled well before this
    for cfg in ("def", "keel"):
        rows, text = res[cfg]
        d = deaths(rows)
        wet = wet_rows(rows, fill_by)
        cap = captured_tick(rows)
        brakes = BRAKE_RE.findall(text)
        stickies = STICKY_RE.findall(text)
        first_brake_tile = ((int(brakes[0][2]), int(brakes[0][3]))
                            if brakes else None)
        near = closest_wu(rows, first_brake_tile, fill_by)
        out.append(
            f"  A3/{cfg:4s} rows={len(rows)} deaths={len(d)} wet_rows={len(wet)} "
            f"captured={cap} CLIFF_BRAKE={len(brakes)} CLIFF_STICKY={len(stickies)}")
        if first_brake_tile:
            out.append(f"           first braked sea tile {first_brake_tile}, "
                       f"closest the tank ever came to its centre: "
                       f"{near:.0f} wu ({near / 256.0:.2f} tiles)"
                       if near is not None else
                       f"           first braked sea tile {first_brake_tile}, "
                       f"no post-fill rows to measure against")
        if not rows:
            out.append(f"    FAIL A3/{cfg}: no engine trace at all")
            ok = False
            continue
        if d:
            out.append(f"    FAIL A3/{cfg}: the tank DIED (first at tick {d[0][0]}, "
                       f"tile ({d[0][4]},{d[0][5]}))")
            ok = False
        if wet:
            out.append(f"    FAIL A3/{cfg}: the tank stood on DEEP SEA at tick "
                       f"{wet[0][0]}, tile ({wet[0][4]},{wet[0][5]})")
            ok = False
        if cap is None:
            out.append(f"    FAIL A3/{cfg}: the base was never captured -- on the "
                       f"defaults that is the sticky latch pinning the tank on "
                       f"the ledge, which is the whole risk of the knob")
            ok = False
    # The knob, isolated: the line belongs to the default and to nothing else.
    n_def = len(STICKY_RE.findall(res["def"][1]))
    n_keel = len(STICKY_RE.findall(res["keel"][1]))
    if n_def < 1:
        out.append("    FAIL A3: CLIFF_BRAKE_STICKY never latched on the "
                   "defaults -- the arena did not exercise the knob")
        ok = False
    if n_keel != 0:
        out.append(f"    FAIL A3: preset=keel printed {n_keel} CLIFF_STICKY "
                   f"lines; it must print none, or the count above is not the "
                   f"knob's")
        ok = False
    return ok


def check_A4(res, ticks, out):
    ok = True
    fill_by = 200
    spawn = G.spots("A4")["spawn"]
    wet_next = {}
    for cfg in ("def", "keel"):
        rows, text = res[cfg]
        d = deaths(rows)
        wet = wet_rows(rows, fill_by)
        cap = captured_tick(rows)
        wn = wet_next_steps(text, "A4", spawn)
        wet_next[cfg] = wn
        vetoes = VETO_RE.findall(text)
        out.append(
            f"  A4/{cfg:4s} rows={len(rows)} deaths={len(d)} wet_rows={len(wet)} "
            f"captured={cap} nav_next_into_sea={len(wn)} PF_SEA_VETO={len(vetoes)}")
        if wn:
            out.append(f"           first: on land at {wn[0][0]}, told next="
                       f"{wn[0][1]} which is deep sea")
        if not rows:
            out.append(f"    FAIL A4/{cfg}: no engine trace at all")
            ok = False
            continue
        if d:
            out.append(f"    FAIL A4/{cfg}: the tank DIED (first at tick {d[0][0]}, "
                       f"tile ({d[0][4]},{d[0][5]}))")
            ok = False
        if wet:
            out.append(f"    FAIL A4/{cfg}: the tank stood on DEEP SEA at tick "
                       f"{wet[0][0]}, tile ({wet[0][4]},{wet[0][5]})")
            ok = False
        if cap is None:
            out.append(f"    FAIL A4/{cfg}: the base was never captured")
            ok = False
    # The fix, stated directly: on the defaults a boatless tank is never told
    # to step into the water.  This holds whether or not the control reproduces.
    if wet_next["def"]:
        out.append(f"    FAIL A4: with the defaults, {len(wet_next['def'])} "
                   f"`nav: dij` lines still named a deep-sea next step for a "
                   f"tank on land -- PF_NEXTSTEP_FOOT_SEA_RULE did not hold")
        ok = False
    # The control.  If keel does not reproduce, say so; do not dress it up.
    if wet_next["keel"]:
        n = len(VETO_RE.findall(res["def"][1]))
        out.append(f"  A4 control: preset=keel named a deep-sea next step "
                   f"{len(wet_next['keel'])} times -- the bug reproduces here.")
        if n < 1:
            out.append("    FAIL A4: the control reproduced the bug but the "
                       "defaults printed no PF_SEA_VETO, so the difference "
                       "above is not the rule's doing")
            ok = False
    else:
        out.append("  A4 control: preset=keel named NO deep-sea next step in "
                   "this arena, so it does not reproduce the recorded bug. "
                   "What the arena still shows is that the rule is live (the "
                   "PF_SEA_VETO count above) and that turning it on costs the "
                   "tank neither its goal nor its life.")
    return ok


CHECKS = {"A3": check_A3, "A4": check_A4}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="all", choices=list(VARIANTS) + ["all"])
    ap.add_argument("--ticks", type=int, default=None)
    ap.add_argument("--build", default=str(DEFAULT_BUILD))
    args = ap.parse_args()

    build_dir = Path(args.build).resolve()
    variants = list(VARIANTS) if args.variant == "all" else [args.variant]
    out, ok = [], True
    for v in variants:
        ticks = args.ticks or TICKS[v]
        res = {}
        for cfg in ("def", "keel"):
            print(f"running arena {v} / {cfg} ({ticks} ticks)...", flush=True)
            rows, text = run_sim(v, cfg, ticks, build_dir)
            if rows is None:
                out.append(f"    FAIL {v}/{cfg}: {text}")
                ok = False
                res = None
                break
            res[cfg] = (rows, text)
        if res is None:
            continue
        out.append(f"arena {v}:")
        ok = CHECKS[v](res, ticks, out) and ok

    print()
    for line in out:
        print(line)
    print()
    print("PASS" if ok else "FAIL")
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
