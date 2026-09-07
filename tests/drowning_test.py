#!/usr/bin/env python3
"""drowning -- the tank may not drive into deep sea (GoalHunter 1.7 steering).

THE INCIDENT
------------
20260905_231835_*_oilrig_2v2_repair_seed1, bot 3, engine tick 71330.  The tank
sat at world (36104,36093) = tile (141,140) -- eight world units inside column
141 and three above row 141 -- with DEEP SEA at (141,141) directly south of it
and land at (140,141) to the south-west, heading wobbling between 133 and 135.
It drove in.  Two things had to line up:

  1. THE BRAKE RAY CORNER-CUT THE KILLER TILE.  steering.lua's global cliff
     brake walks the heading ray in 64 wu hops and only looks up the tile each
     sample LANDS in.  At dir >= 134 the first sample jumped from (141,140) to
     (140,141) -- land -- and never tested (141,141) or (140,140), the two tiles
     it cut across.  It fired at dir <= 133 and missed at 134-135, so the brake
     flickered with the tank's 2-brad wobble.

  2. A STOPPED TANK FALLS THROUGH THE GUARD.  The brake is gated on
     CLIFF_MIN_SPEED, so once it had braked the tank to a standstill the guard
     stopped running and navigate's single KEY_FASTER moved it 6 wu south.
     Tanks have no reverse gear (tank.c tankAccel clamps speed at 0): it drove
     in, it did not slide.

THE TWO KNOBS UNDER TEST, both ON by default and both false in PRESETS.keel:

  C.CLIFF_RAY_CORNER_CHECK      the ray also tests the two tiles a both-axes
                                sample hop cut across
  C.CLIFF_STOP_MASK_ALL_GOALS   M.steer clears KEY_FASTER and sets KEY_SLOWER
                                when the tile ahead -- or a corner-cut tile --
                                is deep sea, for EVERY goal (the same mask has
                                always existed for attack_tank alone)

FOUR RUNS, TWO ARENAS, EACH RUN TWICE: once on the defaults and once on
`preset=keel`.  tests/generate_drowning_map.py builds the ground and its
docstring carries the geometry.

  A  THE STAIRCASE SHORELINE.  A two-tile-wide staircase of grass descending
     south-west through open deep sea, spawn block at the top, a NEUTRAL BASE at
     the bottom, and nothing else on the map.  Standing on the east tile of any
     step IS the incident: sea due south, land to the south-west, the tank
     hugging the column boundary on a south-west heading.  Asserted:

       * the tank never stands on a deep-sea tile and never dies -- read from
         the ENGINE (game.map_tile under the tank, in the sidecar's trace), not
         from the brain's opinion of itself;
       * it captures the base, so "it stayed dry" is not the answer a tank that
         refused to move would also give;
       * with the defaults the corner rule FIRES: at least one CLIFF_BRAKE with
         corner=true, i.e. a brake on a tile the old ray never looked at;
       * with the defaults the stop mask BITES: at least one CLIFF_STOP_MASK
         that actually changed the keys, i.e. a KEY_FASTER aimed at adjacent
         deep sea that was vetoed;
       * with preset=keel neither line appears at all, which is what proves the
         two counts above belong to the knobs and not to the arena.

  B  WATER IN FRONT, THE ONLY WAY OUT BEHIND -- the DEADLOCK guard.  A corridor
     running east to a NEUTRAL BASE and a forty-tile ring the tank has no reason
     to use.  Two tiles short of the base, and 192 wu short of the boundary, the
     sidecar turns the whole next column into deep sea: goal across the water,
     route behind.  A rule that strips KEY_FASTER at a shoreline could pin a
     tank there forever, so this asserts the opposite -- it turns round, walks
     the ring and captures the base -- and that it never enters the water doing
     it.

WHAT THESE ARENAS DO NOT CLAIM, because it is not true
------------------------------------------------------
NEITHER ARENA DROWNS THE preset=keel CONTROL.  That was the intent and it did
not survive contact with the measurements, so it is written down rather than
dressed up:

  * Arena A, seeds 1 to 5: the keel tank captures the base at engine tick 810
    and never touches the water either.  Its ray misses the corner tiles 182
    times over the 8000-tick run (that is the count of corner=true brakes the
    DEFAULT logs, every one of which keel does not fire, alongside 137 vetoed
    KEY_FASTERs) -- but the plain brake still fires on the cardinal headings in
    between, and on this geometry that flicker is enough to keep it out.  The incident's tank was additionally
    pinned by a committed u-turn fighting the evasive turn; this staircase has
    the tank driving THROUGH, and it never gets pinned.
  * Arena B, sweeping the warning distance at seed 1: at 160 wu -- inside the
    stopping distance of a tank cruising grass at speed 48 -- BOTH the default
    and keel slide in and drown, which measures momentum and not the knobs.  At
    192 and 224 wu both stop, both turn round, and the traces are byte
    identical.  There is no distance in between at which the mask casts the
    deciding vote: whenever the water is dead ahead the brake's evasive turn
    gets the tank away before its speed reaches 0, and the mask only exists for
    the tick where speed IS 0.

So what is proved here is: the two rules fire, on ground shaped like the
incident, without the tank drowning, freezing, or failing to reach its goal --
and that they are the only difference between the two configurations.  What is
NOT proved is a reproduction of the death itself.  Reproducing that needs the
tank pinned between an evasive turn and a committed u-turn, and the scenario API
has no hook that places a tank (src/server/scenario.c scBuildGameTable exposes
game.tank to READ one and game.set_tile to rewrite the ground, and that is all),
so the pin has to fall out of a drive -- which is the piece that did not come.

Usage: python drowning_test.py [--variant A|B|all] [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import argparse
import glob
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
from asap import asap_args, pacing_line, take_asap_flag   # noqa: E402
import generate_drowning_map as G                          # noqa: E402

VARIANTS = ("A", "B")
# One port per (variant, config): the two configs of a variant are separate
# server runs and must not collide if somebody runs them back to back.
PORTS = {("A", "def"): 50291, ("A", "keel"): 50292,
         ("B", "def"): 50293, ("B", "keel"): 50294}
# ENGINE ticks.  Arena A's walk down the staircase finishes around 850 and the
# tank then mills about on the shoreline, which is where most of the corner
# brakes come from; arena B has to drive the ring, which lands around 2500.
TICKS = {"A": 8000, "B": 5000}
CONFIGS = {"def": "", "keel": "preset=keel"}
ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

DEEP_SEA = 255                      # global.h, as game.map_tile reports it
NEUTRAL = 255                       # ...and as the base owner byte reports it
P0 = 0

# steering.lua print2 lines this test reads.
#   CLIFF_BRAKE t=85 goal=capture_base tile_mx/my=(129,121) dist_wu=256
#     corner=false corner_chk=true dir=192 spd=64 tank=(130,121) scan_wu=512
#   CLIFF_STOP_MASK t=85 goal=capture_base tile_mx/my=(129,121) corner=false
#     ahead_mx/my=(129,121) dir=192 spd=64 keys=10->10 flag=true
BRAKE_RE = re.compile(
    r"CLIFF_BRAKE t=(\d+) goal=(\S+) tile_mx/my=\((\d+),(\d+)\) "
    r"dist_wu=(\d+) corner=(\w+) corner_chk=(\w+) dir=(\d+) spd=(-?\d+) "
    r"tank=\((\d+),(\d+)\)")
MASK_RE = re.compile(
    r"CLIFF_STOP_MASK t=(\d+) goal=(\S+) tile_mx/my=\((\d+),(\d+)\) "
    r"corner=(\w+) ahead_mx/my=\((\d+),(\d+)\) dir=(\d+) spd=(-?\d+) "
    r"keys=(\d+)->(\d+) flag=(\w+)")


# ── the sidecar's engine-side trace ──────────────────────────────────────
# Written by drowning_<V>.scenario.lua into the BUILD directory (the server's
# cwd), one row per tile change plus a pulse every 50 ticks:
#     tick wx wy dir mx my terrain dead base_owner [notch_cut]
# `terrain` is game.map_tile UNDER THE TANK, so terrain == DEEP_SEA on any row
# is the drowning itself, read from the ground and not inferred.
def read_trace(build_dir, variant):
    path = build_dir / f"drowning_{variant}_trace.log"
    rows = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 9:
                rows.append([int(p) for p in parts])
    return rows


def deaths(rows):
    """Rising edges of the `dead` column: one per death, not one per tick."""
    out = []
    for i, r in enumerate(rows):
        if r[7] == 1 and (i == 0 or rows[i - 1][7] == 0):
            out.append(r)
    return out


def wet_rows(rows, after_tick):
    """Rows on which the tank's own tile is deep sea.

    `after_tick` skips the spawn pond: the start square HAS to be deep sea
    (starts.c startsIsValidSquare) and the tank sits on it, afloat, until the
    sidecar fills it in."""
    return [r for r in rows if r[6] == DEEP_SEA and r[0] > after_tick]


def captured_tick(rows):
    for r in rows:
        if r[8] == P0:
            return r[0]
    return None


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
                    str(HERE / "generate_drowning_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"drowning_{variant}_{config}"
    trace = build_dir / f"drowning_{variant}_trace.log"
    stderr = HERE / f"drowning_{variant}_{config}_stderr.txt"
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
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"drowning_{variant}.map"),
           "-port", str(PORTS[(variant, config)]), "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           # yesfull: the arenas are about STEERING, not about finding a base
           # on a map that has exactly one thing on it.
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
    ours = sess / "print2_bot0.log"
    if not ours.exists():
        return None, f"no print2_bot0.log under {sess}"
    rows = read_trace(build_dir, variant)
    if not rows:
        return None, (f"the sidecar wrote no trace -- is "
                      f"tests/drowning_{variant}.scenario.lua beside the .map?")
    return rows, ours.read_text(errors="ignore")


def lua_errors(text):
    return [ln for ln in text.splitlines()
            if "attempt to " in ln or "stack traceback" in ln
            or (".lua:" in ln and "Error" in ln)][:5]


def brakes(text):
    return BRAKE_RE.findall(text)


def masks(text):
    """Every CLIFF_STOP_MASK line, and the subset that actually changed keys.

    The mask logs on every tick it finds water ahead; on most of those the
    brake has already returned KEY_SLOWER and there is no KEY_FASTER left to
    strip (keys=N->N).  Only the lines where the two differ are the rule
    casting a vote."""
    all_m = MASK_RE.findall(text)
    bit_m = [m for m in all_m if m[9] != m[10]]
    return all_m, bit_m


def sidecar_matches_map(variant):
    """The sidecar is a static .lua and the map is generated; a coordinate that
    drifts between them makes the arena silently do nothing.  Checked, not
    trusted -- an earlier revision of this test cut a notch one column away
    from the tank and filled a pond that was not the pond."""
    src = (HERE / f"drowning_{variant}.scenario.lua").read_text(errors="ignore")
    spots = G.spots(variant)

    def const(name):
        m = re.search(r"^local %s = \{ *(\d+), *(\d+) *\}" % name, src, re.M)
        return (int(m.group(1)), int(m.group(2))) if m else None

    def num(name):
        m = re.search(r"^local %s = (\d+)" % name, src, re.M)
        return int(m.group(1)) if m else None

    problems = []
    if const("SPAWN") != spots["spawn"]:
        problems.append(f"SPAWN {const('SPAWN')} != generated {spots['spawn']}")
    if variant == "B":
        if num("NOTCH_X") != spots["notch"][0]:
            problems.append(
                f"NOTCH_X {num('NOTCH_X')} != generated {spots['notch'][0]}")
        if num("TRIGGER_X") != spots["trigger"][0]:
            problems.append(
                f"TRIGGER_X {num('TRIGGER_X')} != generated {spots['trigger'][0]}")
        rows = re.search(r"^local MAIN_ROWS = \{([^}]*)\}", src, re.M)
        got = [int(v) for v in rows.group(1).replace(" ", "").split(",") if v]
        if got != spots["main_rows"]:
            problems.append(f"MAIN_ROWS {got} != generated {spots['main_rows']}")
    return problems


# ── arena A ──────────────────────────────────────────────────────────────
def check_A(runs):
    ok = True
    spots = G.spots("A")
    for cfg in ("def", "keel"):
        rows, text = runs[cfg]
        errs = lua_errors(text)
        if errs:
            print(f"  FAIL [{cfg}] lua errors in the brain log:")
            for e in errs:
                print("    " + e)
            ok = False
        wet = wet_rows(rows, 400)      # the pond is filled long before 400
        dead = deaths(rows)
        cap = captured_tick(rows)
        allm, bitm = masks(text)
        corner = [b for b in brakes(text) if b[5] == "true"]
        print(f"  [{cfg}] deaths={len(dead)} deep-sea rows={len(wet)} "
              f"base captured at t={cap} | brakes={len(brakes(text))} "
              f"corner-brakes={len(corner)} mask lines={len(allm)} "
              f"mask vetoes={len(bitm)}")
        if wet:
            print(f"  FAIL [{cfg}] the tank stood on deep sea: {wet[0]}")
            ok = False
        if dead:
            print(f"  FAIL [{cfg}] the tank died: {dead[0]}")
            ok = False
        if cap is None:
            print(f"  FAIL [{cfg}] the base at {spots['base']} was never "
                  f"captured -- a tank that refuses to move also never drowns, "
                  f"so staying dry only counts if it got there")
            ok = False

        if cfg == "def":
            if not corner:
                print("  FAIL [def] no CLIFF_BRAKE with corner=true -- the "
                      "corner rule never fired, so this run did not exercise "
                      "C.CLIFF_RAY_CORNER_CHECK at all")
                ok = False
            if not bitm:
                print("  FAIL [def] no CLIFF_STOP_MASK changed the keys -- "
                      "C.CLIFF_STOP_MASK_ALL_GOALS never vetoed a KEY_FASTER, "
                      "so this run did not exercise it either")
                ok = False
        else:
            if corner or allm:
                print(f"  FAIL [keel] preset=keel logged {len(corner)} corner "
                      f"brakes and {len(allm)} mask lines -- both knobs are "
                      f"supposed to be false, so the control is not a control")
                ok = False

    # The whole point of a control: the two configs must differ ONLY here.
    d_rows, d_text = runs["def"]
    k_rows, _ = runs["keel"]
    dc = len([b for b in brakes(d_text) if b[5] == "true"])
    print(f"  [both] the corner rule fired {dc} times on the defaults and 0 "
          f"times on preset=keel; keel captured at "
          f"t={captured_tick(k_rows)} and, on this arena, ALSO never entered "
          f"the water -- see the module docstring: arena A is not a drowning "
          f"control and does not pretend to be one")
    return ok


# ── arena B ──────────────────────────────────────────────────────────────
def check_B(runs):
    ok = True
    spots = G.spots("B")
    for cfg in ("def", "keel"):
        rows, text = runs[cfg]
        errs = lua_errors(text)
        if errs:
            print(f"  FAIL [{cfg}] lua errors in the brain log:")
            for e in errs:
                print("    " + e)
            ok = False
        cut = [r for r in rows if len(r) > 9 and r[9] == 1]
        wet = wet_rows(rows, 300)
        dead = deaths(rows)
        cap = captured_tick(rows)
        gap = (spots["notch"][0] * 256 - cut[0][1]) if cut else None
        print(f"  [{cfg}] notch cut at t={cut[0][0] if cut else None} with "
              f"{gap} wu of warning | deaths={len(dead)} deep-sea rows={len(wet)} "
              f"base captured at t={cap}")
        if not cut:
            print("  FAIL the notch was never cut -- the tank never stood in "
                  "the trigger column slowly enough. The trace logs every tick "
                  "the tank spends in that column (mx == TRIGGER_X); read it "
                  "for which of the three conditions was missing.")
            ok = False
            continue
        if wet:
            print(f"  FAIL [{cfg}] the tank stood on deep sea: {wet[0]}")
            ok = False
        if dead:
            print(f"  FAIL [{cfg}] the tank died: {dead[0]}")
            ok = False
        if cap is None or cap <= cut[0][0]:
            print(f"  FAIL [{cfg}] the base was not captured AFTER the notch "
                  f"was cut (cut t={cut[0][0]}, captured t={cap}) -- this is "
                  f"the deadlock the arena exists to rule out: a tank that can "
                  f"no longer accelerate at the shoreline and never takes the "
                  f"long way round")
            ok = False
    print("  [both] arena B is the DEADLOCK guard, not a drowning control: at "
          "160 wu of warning both configurations slide in (momentum, not the "
          "knobs) and at 192 and 224 both turn round -- see the module "
          "docstring")
    return ok


CHECKS = {"A": check_A, "B": check_B}


def run(variant, ticks, build_dir):
    print(f"\n=== arena {variant} ===")
    problems = sidecar_matches_map(variant)
    if problems:
        print("  FAIL the scenario sidecar and the generated map disagree:")
        for p in problems:
            print("    " + p)
        return False
    runs = {}
    for cfg in ("def", "keel"):
        rows, text = run_sim(variant, cfg, ticks, build_dir)
        if rows is None:
            print(f"  FAIL [{cfg}] {text}")
            return False
        runs[cfg] = (rows, text)
    return CHECKS[variant](runs)


def main():
    argv = list(sys.argv[1:])
    take_asap_flag(argv)          # strips --asap / --no-asap IN PLACE
    ap = argparse.ArgumentParser()
    ap.add_argument("--variant", default="all")
    ap.add_argument("--ticks", type=int, default=None)
    ap.add_argument("--build", default=str(DEFAULT_BUILD))
    args = ap.parse_args(argv)

    build_dir = Path(args.build).resolve()
    variants = VARIANTS if args.variant == "all" else (args.variant,)
    print("drowning test -- the tank may not drive into deep sea")
    print(pacing_line())
    ok = True
    for v in variants:
        ok = run(v, args.ticks or TICKS[v], build_dir) and ok
    print("\n" + ("PASS" if ok else "FAIL"))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
