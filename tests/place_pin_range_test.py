#!/usr/bin/env python3
"""A placement is pinned under attack_tank only in shooting range (GoalHunter 1.7).

Field incident 20260903_105448 bot2 t=34774.  Four pills aboard, the carry
discount maxed at -300, the pool-8 placement row priced 1.0 -- and
goal_selection overwrote it with 2163 because "a NORMAL place_pill_strategic
must never out-rank an attack_tank".  The attack_tank row it lost to cost 2162
and was for an enemy FIFTEEN tiles away across water this tank could not
cross.  Nothing was placed again and the bot died carrying all four.

WHAT IS UNDER TEST (goals.lua goal_selection, C.PLACE_PIN_ENEMY_RANGE)
    The pin applies only while a hostile tank is within PLACE_PIN_ENEMY_RANGE
    (7 tiles = TANK_COMBAT_ENGAGE_RANGE, gun range) of OUR tank, measured on
    perception.nearest_hostile_tank.  Outside it the placement competes on its
    own cost and the carry pressure decides.  The EMERGENCY build
    (goal._place_forced) stays exempt at any range.  The verdict is stamped on
    the place row's desc so FINAL_SCORES reproduces its own number:

        pin{atk 2162.0+1, enemy 5t<=7}      the row was raised to atk + 1
        nopin{enemy 15t>7, atk 2162.0}      the row was left alone

THE ARENAS (tests/generate_place_pin_map.py) -- one open grass canvas, one
scripted patrol tank (tests/brains/patrol_ns.lua, which never fires), and
three healthy pills handed to our tank at sim tick 200 to make the carry
pressure real.  The lane is the only difference:
    far    lane x=112, 24 tiles from our base -- the incident's shape: a
           hostile tank that is real and priced, and much too far to shoot us.
    near   lane x=133, 3 tiles from our base -- inside gun range of everywhere
           our tank works.

CHECKS (both variants)
    1. Every chip is internally consistent with the constant: a `pin` chip
       always reports a distance <= PLACE_PIN_ENEMY_RANGE, a `nopin` chip
       always reports one greater than it (or no visible enemy at all).
    2. Every chip's distance matches the nearest HOSTILE tank in that tick's
       own ENGINE_DUMP -- the gate reads what the tank can see, not something
       else.
    3. A pinned row's cost really is the cheapest attack_tank cost + 1 -- the
       number FINAL_SCORES prints reproduces from the chip.
CHECK (far)
    4. At least one `nopin` chip, and a pill actually goes into the ground: a
       pillbox stands on a tile that had none at the start.  Under the old
       rule that far patrol tank pinned every placement instead.
CHECK (near)
    5. The patrol really does come inside the range, and on every tick where
       it is inside it no UNFORCED placement is dispatched.  A `near` run may
       carry no pin verdict at all, and that is not a failure -- see the note
       below.  (An emergency
       build -- goal._place_forced, PLACE_PILL_GATE forced=true -- is exempt
       from the pin by design and is reported, not failed: with
       OFF_BUILD_THREAT_RANGE at 8 tiles, an enemy inside the 7-tile pin range
       usually routes the placement to that exempt path before the pin is even
       consulted.)

Usage: python place_pin_range_test.py [--variant far|near|ALL]
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
PATROL = HERE / "brains" / "patrol_ns.lua"
sys.path.insert(0, str(HERE))
from generate_place_pin_map import (         # noqa: E402
    SPARE_PILLS, GIVE_TICK, FOE_SPAWN, OUR_BASE, PLACE_PIN_ENEMY_RANGE,
    VARIANTS, mdist)

PORTS = {"far": 50146, "near": 50147}
DEFAULT_TICKS = 4000

# goals.lua FINAL_SCORES, one line per candidate. The pin chip is appended to
# the row's desc, so it rides along on the same line as the total it explains.
SCORE_RE = re.compile(
    r"\[(\d+)\] (\S+)@(\d+),(\d+) total=(-?[\d.]+) base=(-?[\d.]+) .*?desc=(.*)")
HEAD_RE = re.compile(r"FINAL_SCORES t=(\d+) pool_size=(\d+)")
PIN_RE = re.compile(r" pin\{atk (-?[\d.]+)\+1, enemy (\d+)t<=(\d+)\}")
NOPIN_RE = re.compile(r" nopin\{enemy (\d+)t>(\d+), atk (-?[\d.]+)\}")
NOPIN_NONE_RE = re.compile(r" nopin\{no visible enemy tank, atk (-?[\d.]+)\}")
DISPATCH_RE = re.compile(
    r"PLACE_PILL_GATE t=(\d+) target=\((\d+),(\d+)\).*? forced=(\w+)")
# ENGINE_DUMP's OBJ list: ty0 = TANK, info bit 0x1 = OBJECT_HOSTILE. Same list
# perception.lua walks to fill nearest_hostile_tank, so the distance the chip
# prints has to come out of it.
SELF_RE = re.compile(r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\)")
FOE_RE = re.compile(r"ty0#\d+@\((\d+),(\d+)\)info=0x([0-9a-fA-F]+)")
TRIPEND_RE = re.compile(r"PLACE_TRIP_END t=(\d+) reason=(\S+)")
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
    label = f"place_pin_{variant}"
    mapfile = HERE / f"place_pin_{variant}.map"
    final = HERE / f"place_pin_{variant}_final.json"
    stderr = HERE / f"place_pin_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        out.append(f"FAIL: WinBoloDS not found under {build_dir}")
        return None, None
    subprocess.run([sys.executable, str(HERE / "generate_place_pin_map.py"),
                    "--variant", variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                out.append(f"FAIL: {p.name} is locked -- a previous WinBoloDS "
                           f"run is still going.")
                return None, None

    init = f"0={BRAIN},1={PATROL}"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label,
               WINBOLO_BRAIN_TIER="10")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby",
           # open: placing a pill costs wood and a tournament tank has none.
           "-gametype", "open",
           "-bots", "2", "-brain", str(BRAIN), "-bot-init", init,
           "-allow-unsafe-brains",
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


def place_rows(text):
    """[(tick, total, chip_kind, dist, range, atk, raw)] for every
    place_pill_strategic row FINAL_SCORES printed that carries a pin verdict."""
    rows = []
    tick = None
    for line in text.splitlines():
        h = HEAD_RE.search(line)
        if h:
            tick = int(h.group(1))
            continue
        m = SCORE_RE.search(line)
        if not m or m.group(2) != "place_pill_strategic":
            continue
        total, desc = float(m.group(5)), m.group(7)
        p = PIN_RE.search(desc)
        if p:
            rows.append((tick, total, "pin", int(p.group(2)),
                         int(p.group(3)), float(p.group(1)), desc))
            continue
        n = NOPIN_RE.search(desc)
        if n:
            rows.append((tick, total, "nopin", int(n.group(1)),
                         int(n.group(2)), float(n.group(3)), desc))
            continue
        nn = NOPIN_NONE_RE.search(desc)
        if nn:
            rows.append((tick, total, "nopin", None, None,
                         float(nn.group(1)), desc))
    return rows


def enemy_dist(text):
    """{brain tick: manhattan distance to the nearest HOSTILE tank}, read off
    the same OBJ list perception.lua walks. Ticks with no hostile in the list
    map to None."""
    out = {}
    for line in text.splitlines():
        m = SELF_RE.search(line)
        if not m:
            continue
        t, sx, sy = int(m.group(1)), int(m.group(2)), int(m.group(3))
        best = None
        for f in FOE_RE.finditer(line):
            if int(f.group(3), 16) & 0x1:      # OBJECT_HOSTILE
                d = abs(int(f.group(1)) - sx) + abs(int(f.group(2)) - sy)
                if best is None or d < best:
                    best = d
        out[t] = best
    return out


def check(variant, text, fd, out):
    if LUA_ERR_RE.search(text):
        bad = next(ln for ln in text.splitlines() if LUA_ERR_RE.search(ln))
        out.append(f"FAIL: Lua error in the print2 log: {bad.strip()}")
        return 1
    if not DUMP_RE.search(text):
        out.append("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return 1
    rows = place_rows(text)
    seen = enemy_dist(text)
    in_range = {t for t, d in seen.items()
                if d is not None and d <= PLACE_PIN_ENEMY_RANGE}
    closest = min((d for d in seen.values() if d is not None), default=None)
    out.append(f"  nearest hostile tank over the run: closest {closest} tile(s); "
               f"{len(in_range)} of {len(seen)} tick(s) inside "
               f"PLACE_PIN_ENEMY_RANGE {PLACE_PIN_ENEMY_RANGE}")
    if not rows and variant == "far":
        out.append("FAIL: no place_pill_strategic row ever carried a pin "
                   "verdict -- the placement and an attack_tank row never met "
                   "in one pool with the placement the cheaper of the two, so "
                   "the pin question was never asked. First few FINAL_SCORES "
                   "place rows:")
        n = 0
        for line in text.splitlines():
            m = SCORE_RE.search(line)
            if m and m.group(2) == "place_pill_strategic":
                out.append("   " + line.strip()[:200])
                n += 1
                if n >= 3:
                    break
        return 1
    pins = [r for r in rows if r[2] == "pin"]
    nopins = [r for r in rows if r[2] == "nopin"]
    out.append(f"  {len(rows)} place row(s) with a pin verdict: {len(pins)} "
               f"pin, {len(nopins)} nopin")
    if pins:
        r = pins[0]
        out.append(f"    e.g. t={r[0]} total={r[1]:.1f}  pin{{atk {r[5]}+1, "
                   f"enemy {r[3]}t<={r[4]}}}")
    if nopins:
        r = nopins[0]
        out.append(f"    e.g. t={r[0]} total={r[1]:.1f}  nopin{{enemy "
                   f"{r[3] if r[3] is not None else 'none'}t>"
                   f"{r[4] if r[4] is not None else PLACE_PIN_ENEMY_RANGE}, "
                   f"atk {r[5]}}}")

    # -- 1. the chips agree with the constant -------------------------------
    bad = [r for r in pins if r[3] > PLACE_PIN_ENEMY_RANGE
           or r[4] != PLACE_PIN_ENEMY_RANGE]
    bad += [r for r in nopins
            if (r[3] is not None and r[3] <= PLACE_PIN_ENEMY_RANGE)
            or (r[4] is not None and r[4] != PLACE_PIN_ENEMY_RANGE)]
    if bad:
        r = bad[0]
        out.append(f"FAIL (1): t={r[0]} a `{r[2]}` chip reports enemy {r[3]} "
                   f"tiles against range {r[4]}, which contradicts "
                   f"PLACE_PIN_ENEMY_RANGE {PLACE_PIN_ENEMY_RANGE}. "
                   f"desc={r[6][-90:]}")
        return 1
    out.append(f"  1 OK: every pin chip reports <= {PLACE_PIN_ENEMY_RANGE} "
               f"tiles and every nopin chip more (or no visible enemy), "
               f"against the same range on every row")

    # -- 2. ...and with what the tank could actually see that tick -----------
    off = [r for r in rows if r[0] in seen and seen[r[0]] != r[3]]
    if off:
        r = off[0]
        out.append(f"FAIL (2): t={r[0]} the chip says the nearest hostile tank "
                   f"is {r[3]} tiles away, but that tick's ENGINE_DUMP puts it "
                   f"at {seen[r[0]]}. The gate is not reading what the tank "
                   f"sees.")
        return 1
    out.append("  2 OK: every chip's distance matches the nearest hostile "
               "tank in that tick's own ENGINE_DUMP")

    # -- 3. a pinned cost reproduces from its own chip ----------------------
    off = [r for r in pins if abs(r[1] - (r[5] + 1.0)) > 0.051]
    if off:
        r = off[0]
        out.append(f"FAIL (3): t={r[0]} the row totals {r[1]:.1f} but its chip "
                   f"says attack_tank {r[5]} + 1 = {r[5] + 1:.1f}. The printed "
                   f"number no longer reproduces from the printed chips.")
        return 1
    if pins:
        out.append(f"  3 OK: all {len(pins)} pinned row(s) total exactly the "
                   f"cheapest attack_tank cost + 1")
    else:
        out.append("  3 -- no pinned row in this variant, nothing to reproduce")

    started = set(SPARE_PILLS)
    ground = [(pb.get("tx"), pb.get("ty")) for pb in (fd or {}).get("pillboxes", [])
              if not pb.get("in_tank")]
    fresh = [t for t in ground if t not in started]
    gates = DISPATCH_RE.findall(text)

    if variant == "far":
        if not nopins:
            out.append(f"FAIL (4): not one `nopin` chip. The patrol lane is "
                       f"{mdist(FOE_SPAWN[chr(102) + 'ar'], OUR_BASE)}+ tiles "
                       f"from our base, so every pin question here should "
                       f"answer `no`.")
            return 1
        if not fresh:
            out.append(f"FAIL (4): no pill was ever put into the ground. "
                       f"Pills still lying on their start tiles: {ground}; the "
                       f"tank was handed {len(SPARE_PILLS)} at sim tick "
                       f"{GIVE_TICK} and nothing within "
                       f"{PLACE_PIN_ENEMY_RANGE} tiles could stop it.")
            return 1
        ends = TRIPEND_RE.findall(text)
        out.append(f"  4 OK: {len(nopins)} nopin verdict(s) and {len(fresh)} "
                   f"pill(s) placed on new ground: {fresh}"
                   + (f"; PLACE_TRIP_END reasons "
                      f"{sorted({e[1] for e in ends})}" if ends else ""))
        out.append("PASS (far): an enemy that cannot shoot us no longer "
                   "cancels the carry pressure -- the placement competed on "
                   "its own cost and the pills went into the ground.")
        return 0

    # -- 5. near: nothing casual is placed while the patrol is in range -----
    if not in_range:
        out.append(f"FAIL (5): the patrol tank never came within "
                   f"{PLACE_PIN_ENEMY_RANGE} tiles of our tank (closest "
                   f"{closest}), so the arena never asked the question.")
        return 1
    casual = [g for g in gates if int(g[0]) in in_range and g[3] == "false"]
    if casual:
        g = casual[0]
        out.append(f"FAIL (5): t={g[0]} an UNFORCED placement was dispatched "
                   f"at ({g[1]},{g[2]}) with a hostile tank "
                   f"{seen.get(int(g[0]))} tiles away -- inside "
                   f"PLACE_PIN_ENEMY_RANGE {PLACE_PIN_ENEMY_RANGE}, where the "
                   f"pin is supposed to hold it back. "
                   f"{len(casual)} such dispatch(es).")
        return 1
    forced = [g for g in gates if int(g[0]) in in_range and g[3] == "true"]
    out.append(f"  5 OK: {len(in_range)} tick(s) with a hostile tank inside "
               f"{PLACE_PIN_ENEMY_RANGE} tiles and not one unforced placement "
               f"dispatched on any of them"
               + (f" ({len(forced)} EMERGENCY build(s) did fire, which is the "
                  f"documented exemption)" if forced else "")
               + (f"; {len(pins)} row(s) carried the pin verdict itself"
                  if pins else
                  "; the pin branch itself was never reached -- with "
                  "OFF_BUILD_THREAT_RANGE (8) wider than the pin range (7), an "
                  "enemy this close routes the placement to the exempt "
                  "emergency build first"))
    out.append("PASS (near): an enemy in gun range still stops a casual "
               "placement -- the rule is intact where it was meant to apply.")
    return 0


def run_one(variant, ticks, build_dir):
    out = []
    text, fd = play(variant, ticks, build_dir, out)
    if text is None:
        return 1, out
    return check(variant, text, fd, out), out


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
    print(f"=== placement pin range ({ticks} sim ticks; "
          f"PLACE_PIN_ENEMY_RANGE={PLACE_PIN_ENEMY_RANGE}, "
          f"{len(SPARE_PILLS)} pills handed over at sim tick {GIVE_TICK})")
    rc = 0
    for v in (list(VARIANTS) if variant == "ALL" else [variant]):
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
