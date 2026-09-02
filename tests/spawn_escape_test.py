#!/usr/bin/env python3
"""Spawn-escape test: a respawn under a hostile pillbox must not be a death.

Field incident 20260902_120856 blocks 3-4.  Every respawn puts the tank on a
BOAT at a fixed start square -- starts.c startsIsValidSquare requires a start to
be DEEP SEA -- and once the enemy owned a pillbox covering that square the boat
was shot out from under the tank.  One shell is enough (tank.c clears onBoat on
any hit) and the tank drowned the next tick: cause 1 LAST_DEATH_BY_DEEPSEA,
killer == self, 61 to 800 frames after respawn, over and over.

goals.spawn_escape_tick is the fix.  init.lua arms state._spawn_escape_arm on a
fresh tank; for the next SPAWN_ESCAPE_WINDOW_TICKS (250) the escape asks whether
the tile the tank woke up on is under live hostile/neutral pill coverage or has
shells inbound, and if so picks the nearest tile OUT of all coverage that is
passable and ray-clear, biased toward the nearest friendly base with stock, sets
goal kind "spawn_escape" and DENIES goal selection until the tank is clear,
ashore, arrived, or SPAWN_ESCAPE_MAX_TICKS (600) are up.

THE ARENA (tests/generate_spawn_escape_map.py)

Both variants write the same landscape, symmetric about (126,126) so mapRead's
recenter is a no-op:

    x 124..128, y 110..120   north peninsula, with a NEUTRAL PILLBOX on its
                             south tip at (126,120), reload 100 =
                             PILLBOX_ATTACK_NORMAL, the slowest the engine
                             allows and what an unshot pill runs at anyway
    (126,127)                THE ONLY START -- deep sea, 7 tiles south of the
                             pill: inside both its 8-tile fire range and its
                             9-tile coverage disk, so every respawn lands under
                             the gun
    x 112..140, y 136..142   south continent, with the bot's own full BASE at
                             (126,141) -- 21 tiles from the pill, 14 from the
                             start

The run is TOURNAMENT + -ranked, the only combination that puts 0 shells in the
tank on a map with a scenario sidecar (without -ranked,
serverSimApplyScenarioCommit stamps gameScripted, which hands out the full open
loadout).  An empty magazine is load-bearing three times: the tank cannot shoot
variant B's walls down, cannot anger the pill into a faster reload, and has a
real reason to refuel once the escape lets go.

The coverage disk is one tile WIDER than the pill can shoot, so (126,129) is
covered but safe and (126,130) is the first uncovered tile: the escape travels
3 tiles to clear coverage but only 2 to leave the line of fire.

TWO VARIANTS -- they differ only in how the tank's first death is arranged, and
that difference is the whole point:

  A  FAR DEATH.  Open water all the way south.  The sidecar drowns the tank
     once, thirteen tiles out, by turning the tile under its tracks into deep
     sea for a few ticks and then putting the ground back.  The respawn is a
     13-tile jump, so init.lua's RESPAWN_DETECTED path sees it.  A exercises
     the escape end to end: arm, pick, drive, release, refuel.

  B  SAME-SQUARE DEATH -- the regression that once made the whole feature
     inert.  The start is sealed into a one-tile-wide water channel pointing
     straight up the pillbox's barrel; the boat is sunk where it floats and the
     tank respawns ONE TILE from where it died.  init.lua's respawn detector
     needs a jump of more than RESPAWN_CACHE_WIPE_DIST (12) tiles, so it cannot
     see this at all -- and for a while that meant the exact case the escape was
     written for armed nothing: a 3000-tick run contained no RESPAWN_DETECTED,
     no SPAWN_ESCAPE_ARM and no SPAWN_ESCAPE lines whatsoever.  Arming now also
     fires on the dead-tick -> live-tick EDGE (via=dead_edge), which has no
     distance condition.  B is what keeps that true.

Neither variant asserts a particular via NAME.  The dead edge runs first, so it
is what normally wins on both; the position-jump arm is a fallback for a missed
dead tick.  What each variant asserts is the property that distinguishes it: A
that the jump gate saw the death, B that it did not and the escape armed anyway.

PASS, variant A, requires all five:
  1. a RESPAWN_DETECTED whose jump really is over RESPAWN_CACHE_WIPE_DIST, a
     SPAWN_ESCAPE_ARM within 15 frames of it, and a real SPAWN_ESCAPE
     ("to=(x,y)", not a REJECT) within 15 frames of that arm -- with the pick's
     own cost arithmetic reconciling against the terms it prints;
  2. no death in the 600 frames after that SPAWN_ESCAPE -- checked twice, from
     the -snapjson death counter and from the absence of a second
     RESPAWN_DETECTED in the window;
  3. a SPAWN_ESCAPE_DONE with why=clear / landed / arrived, never why=timeout;
  4. the bot then goes and refuels: a GOAL_CHANGE to refuel_at_base (or
     flee_to_base) after the DONE, and its shells actually rise;
  5. no brain_crash_*.log and no Lua error text in the print2 log.

PASS, variant B, requires all five:
  1. the tank really did die, and NO RESPAWN_DETECTED fired up to the escape --
     a same-square death has to stay invisible to the jump gate, or the arena
     has stopped being the case this variant is for;
  2. a SPAWN_ESCAPE_ARM anyway, and not via=respawn_jump;
  3. a real SPAWN_ESCAPE ("to=(x,y)") within 15 frames of that arm, its cost
     arithmetic reconciling;
  4. no death in the 600 frames after it, and a SPAWN_ESCAPE_DONE that is not
     why=timeout;
  5. no crash, no Lua error.

Companion to tests/take_cover_test.py (same print2-log harness pattern).

Usage: python spawn_escape_test.py [--variant A|B|ALL] [--ticks N] [--port N]
                                   [--build DIR]
  --port  override the variant's default (A 50120, B 50122) when something
          else on the machine already has it.
Exit 0 only if every variant passes.
"""

import glob
import json
import os
import re
import subprocess
import sys
from pathlib import Path

# The brain's log lines carry non-ASCII arithmetic and get quoted verbatim in
# failure messages. A Windows console defaults to cp1252 and would raise
# UnicodeEncodeError mid-report, turning a useful FAIL into a traceback.
try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
from generate_spawn_escape_map import (           # noqa: E402
    SPAWN, PILL, BASE, EXPECT_PICK, KILL_DIST, RESPAWN_CACHE_WIPE_DIST,
    SPAWN_ESCAPE_MAX_TICKS, PILL_RANGE_MAP, PILL_FIRE_RANGE,
    CHANNEL_X, CHANNEL_Y, VARIANTS, edist, mdist)

PORTS = {"A": 50120, "B": 50122}
TICKS = {"A": 4000, "B": 3000}

# How close after the arm the escape has to speak up, in brain ticks.  The
# brain runs spawn_escape_tick every tick while armed, so this is generous; it
# exists to catch "it eventually fired, 200 ticks later".
ESCAPE_LATENCY = 15
# The window over which the tank has to survive after the escape fires.
SURVIVE_WINDOW = SPAWN_ESCAPE_MAX_TICKS   # 600

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so these are
# used with re.search, never re.match.  The logs are UTF-8; read them with
# encoding="utf-8".
#
#   RESPAWN_DETECTED t=531 jump=13 prev=(126,140) now=(126,127)
RESPAWN_RE = re.compile(
    r"RESPAWN_DETECTED t=(\d+) jump=(\d+) prev=\((\d+),(\d+)\) "
    r"now=\((\d+),(\d+)\)")
#   SPAWN_ESCAPE_ARM t=531 at=(126,127) via=respawn_jump|info.newtank
ARM_RE = re.compile(
    r"SPAWN_ESCAPE_ARM t=(\d+) at=\((\d+),(\d+)\) via=(\S+)")
#   SPAWN_ESCAPE t=531 from=(126,127) covered_by=1[#0] lof=1 to=(126,130)
#     cost=26 (travel 3 x 3.0 + base_dist 11 x 1.5) why=pill_coverage
ESCAPE_RE = re.compile(
    r"SPAWN_ESCAPE t=(\d+) from=\((\d+),(\d+)\) covered_by=(\d+)\[([^\]]*)\] "
    r"lof=(\d+) to=\((\d+),(\d+)\) cost=([\d.]+) "
    r"\(travel (\d+) x ([\d.]+) \+ base_dist (\d+) x ([\d.]+)\) why=(\S+)")
#   SPAWN_ESCAPE t=... from=(x,y) ... REJECT why=not_covered (...)
REJECT_RE = re.compile(
    r"SPAWN_ESCAPE t=(\d+) from=\((\d+),(\d+)\).*REJECT why=(\S+)")
#   SPAWN_ESCAPE_DONE t=606 why=clear at=(126,130) coverage=0 ticks=75
DONE_RE = re.compile(
    r"SPAWN_ESCAPE_DONE t=(\d+) why=(\S+) at=\((\d+),(\d+)\) "
    r"coverage=(\d+) ticks=(\d+)")
#   ENGINE_DUMP t=411 self=(126,141) dir=136 spd=4 arm=40 sh=3
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\) dir=\d+ spd=\d+ "
    r"arm=(\d+) sh=(\d+)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")

GOOD_DONE = ("clear", "landed", "arrived")
REFUEL_GOALS = ("refuel_at_base", "flee_to_base")
# init.lua arms the escape from the dead-tick -> live-tick edge
# (via=dead_edge), which has no distance condition, and keeps the old
# RESPAWN_DETECTED position-jump arm (via=respawn_jump) as a fallback for a
# missed dead tick.  The dead edge runs first, so it is what normally wins on
# BOTH variants -- which is why neither variant asserts a particular via NAME.
# What each variant asserts is the property that distinguishes it: A that the
# jump gate saw the death, B that it did NOT and the escape armed anyway.
VIA_JUMP = "respawn_jump"


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_snap(path):
    """[(server_tick, tx, ty, deaths, alive)] for the bot, in order."""
    out = []
    if not path.exists():
        return out
    for line in path.read_text(errors="ignore").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            s = json.loads(line)
        except ValueError:
            continue
        for t in s.get("tanks", []):
            if t.get("player") == 0:
                out.append((s.get("tick"), t.get("tx"), t.get("ty"),
                            t.get("deaths"), t.get("alive")))
    return out


# The dedicated server prints this and exits when it cannot take the port.  It
# is worth spotting by name: the symptom downstream is an empty debug_sessions
# glob, whose default explanation ("is the drive full?") sends you looking in
# entirely the wrong place.
BIND_FAIL_RE = re.compile(r"bind\(\) failed on port (\d+)|Error creating network transport")


def play(variant, ticks, build_dir, port=None):
    """Run one variant; return (print2 text, snapshot rows) or (None, None)."""
    label = f"spawn_escape_test_{variant}"
    mapfile = HERE / f"spawn_escape_{variant}.map"
    final = HERE / f"spawn_escape_{variant}_final.json"
    snap = HERE / f"spawn_escape_{variant}_snap.jsonl"
    stderr = HERE / f"spawn_escape_{variant}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None, None
    subprocess.run([sys.executable,
                    str(HERE / "generate_spawn_escape_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (final, snap, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None, None

    port = port or PORTS[variant]
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(port),
           "-nolobby",
           # TOURNAMENT + -ranked is the only combination that puts 0 shells in
           # the tank on a scenario map -- see the generator's header.  The
           # empty magazine keeps the tank from shooting variant B's walls
           # down, keeps the pillbox at its slow reload, and gives variant A's
           # last assertion (it goes and refuels) something to measure.
           "-gametype", "tournament", "-ranked",
           "-bots", "1", "-brain", str(BRAIN),
           # yesfull: the whole arena is known from tick 0 - the test is about
           # reacting to a covered spawn, not about discovering the pillbox.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           # 100-tick snapshots: fine enough that a death inside the 600-frame
           # survival window cannot hide between two samples.
           "-snapjson", str(snap), "-snapinterval", "100",
           "-nowinbolonet", "-quiet", "-threads", "1"]
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 5))

    sess = newest_session(build_dir, label)
    if not sess:
        # Say WHY before falling back to the generic explanation.  A port the
        # server could not bind looks identical from here to a run that never
        # recorded anything.
        head = stderr.read_text(errors="ignore")[:2000] if stderr.exists() else ""
        m = BIND_FAIL_RE.search(head)
        if m:
            print(f"FAIL: the dedicated server could not take port {port} "
                  "-- something else is using it, so the run never started. "
                  f"Retry, or pass --port with a free one. Server said:")
            for line in head.splitlines():
                if BIND_FAIL_RE.search(line):
                    print("   " + line.strip())
            return None, None
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return None, None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed - see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return None, None
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return None, None
    print(f"  session: {sess.name}")
    return log.read_text(encoding="utf-8", errors="replace"), read_snap(snap)


def dumps(text):
    return [(int(m.group(1)), int(m.group(2)), int(m.group(3)),
             int(m.group(4)), int(m.group(5))) for m in DUMP_RE.finditer(text)]


def report_common(text, snap):
    """Shared reporting + the checks both variants must pass.  Returns the
    ENGINE_DUMP samples, or None on failure."""
    print(f"  arena: start {SPAWN}, neutral pill {PILL} "
          f"{edist(PILL, SPAWN):.0f} tiles away (fire range {PILL_FIRE_RANGE}, "
          f"coverage disk {PILL_RANGE_MAP}), base {BASE} "
          f"{mdist(SPAWN, BASE)} tiles away")
    for line in text.splitlines():
        if LUA_ERR_RE.search(line):
            print(f"FAIL: Lua error in the print2 log: {line.strip()}")
            return None
    d = dumps(text)
    if not d:
        print("FAIL: no ENGINE_DUMP lines -- the bot never thought")
        return None
    if d[0][4] != 0:
        print(f"FAIL: the tank spawned with {d[0][4]} shells, not 0 -- the "
              "TOURNAMENT loadout did not apply (is -ranked still on the "
              "command line? a scenario map without it is stamped "
              "gameScripted, which hands out the full open loadout)")
        return None
    deaths_end = snap[-1][3] if snap else None
    print(f"  tank: t={d[0][0]} at ({d[0][1]},{d[0][2]}) arm={d[0][3]} "
          f"sh={d[0][4]} -> t={d[-1][0]} at ({d[-1][1]},{d[-1][2]}) "
          f"arm={d[-1][3]} sh={d[-1][4]}; deaths in the whole run: "
          f"{deaths_end}")
    return d


def show_arms_and_escapes(text):
    """Print the arm / pick / reject picture and hand back the parsed lists."""
    arms = list(ARM_RE.finditer(text))
    escapes = list(ESCAPE_RE.finditer(text))
    rejects = list(REJECT_RE.finditer(text))
    respawns = list(RESPAWN_RE.finditer(text))
    print(f"  RESPAWN_DETECTED x{len(respawns)}"
          + ("" if not respawns else ": " + ", ".join(
              f"t={r.group(1)} jump={r.group(2)} "
              f"({r.group(3)},{r.group(4)})->({r.group(5)},{r.group(6)})"
              for r in respawns[:4])))
    print(f"  SPAWN_ESCAPE_ARM x{len(arms)}"
          + ("" if not arms else ": " + ", ".join(
              f"t={a.group(1)} at=({a.group(2)},{a.group(3)}) via={a.group(4)}"
              for a in arms[:4])))
    print(f"  SPAWN_ESCAPE: {len(escapes)} pick(s), {len(rejects)} reject(s)"
          + ("" if not rejects
             else " (" + ", ".join(sorted({r.group(4) for r in rejects})) + ")"))
    return arms, escapes, rejects, respawns


def check_escape_line(e0):
    """Report one SPAWN_ESCAPE pick and check its own arithmetic closes."""
    print(f"  escape picked: t={e0.group(1)} from "
          f"({e0.group(2)},{e0.group(3)}) covered_by={e0.group(4)}"
          f"[{e0.group(5)}] lof={e0.group(6)} -> ({e0.group(7)},{e0.group(8)}) "
          f"cost={e0.group(9)} (travel {e0.group(10)} x {e0.group(11)} + "
          f"base_dist {e0.group(12)} x {e0.group(13)}) why={e0.group(14)}")
    print(f"    modelled cheapest uncovered tile was {EXPECT_PICK} "
          "(reported only; which tile wins is the scan's business)")
    want = (int(e0.group(10)) * float(e0.group(11))
            + int(e0.group(12)) * float(e0.group(13)))
    if abs(float(e0.group(9)) - want) > 0.6:
        print(f"FAIL: the SPAWN_ESCAPE line quotes cost {e0.group(9)} but its "
              f"own terms add up to {want:.1f}.")
        return False
    if int(e0.group(4)) < 1:
        print("FAIL: the escape fired on a tile it says nothing covers.")
        return False
    return True


def check_survival(snap):
    """The death count must be flat for SURVIVE_WINDOW server frames from the
    moment the tank is alive again.  The snapshot stream runs on SERVER ticks
    and the print2 log on brain ticks, so the window is anchored on the
    snapshot where the tank is alive after its first death rather than by
    converting one clock to the other."""
    if not snap:
        print("FAIL: no snapshot stream to measure survival from.")
        return False
    alive_again = next(((tk, d) for (tk, _x, _y, d, alive) in snap
                        if alive and d and d >= 1), None)
    if alive_again is None:
        print("FAIL: the snapshot stream never shows the tank alive after a "
              "death, so there is no survival window to measure.")
        return False
    t0, d0_count = alive_again
    window = [(tk, d) for (tk, _x, _y, d, _a) in snap
              if t0 <= tk <= t0 + SURVIVE_WINDOW]
    print(f"  survival window: server ticks {t0}..{t0 + SURVIVE_WINDOW}, "
          f"{len(window)} snapshot(s), death count "
          + " -> ".join(str(d) for _tk, d in window))
    rose = [(tk, d) for tk, d in window if d > d0_count]
    if rose:
        print(f"FAIL: the death count went {d0_count} -> {rose[0][1]} at "
              f"server tick {rose[0][0]}, inside the {SURVIVE_WINDOW}-frame "
              "window after the escape. The escape did not save the tank.")
        return False
    alld = [d for (_tk, _x, _y, d, _a) in snap]
    print(f"  deaths across the whole run: {alld[0]} -> {alld[-1]} (only the "
          "window above is asserted on; a later death is the bot's own "
          "business once it is rearmed)")
    return True


def check_done(text, t_esc):
    """The escape has to LET GO, and for a good reason.  Returns the match."""
    dones = [m for m in DONE_RE.finditer(text) if int(m.group(1)) >= t_esc]
    if not dones:
        print("FAIL: no SPAWN_ESCAPE_DONE after the escape armed -- it never "
              "released the goal.")
        return None
    d0 = dones[0]
    print(f"  escape released: t={d0.group(1)} why={d0.group(2)} at "
          f"({d0.group(3)},{d0.group(4)}) coverage={d0.group(5)} "
          f"ticks={d0.group(6)}")
    if d0.group(2) not in GOOD_DONE:
        print(f"FAIL: the escape ended why={d0.group(2)}; it has to end "
              f"{'/'.join(GOOD_DONE)}. why=timeout means it held the goal for "
              f"the full SPAWN_ESCAPE_MAX_TICKS ({SPAWN_ESCAPE_MAX_TICKS}) "
              "without ever getting out of the coverage.")
        return None
    if int(d0.group(5)) != 0 and d0.group(2) == "clear":
        print(f"FAIL: it released why=clear on a tile it says has coverage "
              f"{d0.group(5)}.")
        return None
    return d0


def run_A(text, snap):
    """Far death -> the respawn is a big jump -> arm via=respawn_jump."""
    d = report_common(text, snap)
    if d is None:
        return 1
    arms, escapes, rejects, respawns = show_arms_and_escapes(text)

    # -- 1. The jump path fired, and the escape spoke up right after. -------
    if not respawns:
        print("FAIL: no RESPAWN_DETECTED line at all. The sidecar's scripted "
              f"drowning (>= {KILL_DIST} tiles from the start, which clears "
              f"RESPAWN_CACHE_WIPE_DIST = {RESPAWN_CACHE_WIPE_DIST}) did not "
              "happen, or the tank never got that far from its start. Look for "
              "SPAWN_ESCAPE_ARENA lines in the server output.")
        return 1
    if not escapes:
        print("FAIL: the escape never picked a tile. Rejects seen:")
        for r in rejects[:5]:
            print("   " + r.group(0).strip())
        return 1
    t_resp = int(respawns[0].group(1))
    armed = [a for a in arms if 0 <= int(a.group(1)) - t_resp <= ESCAPE_LATENCY]
    if not armed:
        print(f"FAIL: RESPAWN_DETECTED at t={t_resp} but no SPAWN_ESCAPE_ARM "
              f"within {ESCAPE_LATENCY} frames of it.")
        return 1
    if int(respawns[0].group(2)) <= RESPAWN_CACHE_WIPE_DIST:
        print(f"FAIL: RESPAWN_DETECTED reports jump={respawns[0].group(2)}, "
              f"which is not over RESPAWN_CACHE_WIPE_DIST "
              f"({RESPAWN_CACHE_WIPE_DIST}) -- this variant is supposed to be "
              "the FAR death, the one the position-jump path can see.")
        return 1
    print(f"  armed via={armed[0].group(4)} (either path is fine here; what "
          f"makes A distinct is that the jump gate saw the death at all)")
    picks = [e for e in escapes
             if 0 <= int(e.group(1)) - int(armed[0].group(1)) <= ESCAPE_LATENCY]
    if not picks:
        print(f"FAIL: armed at t={armed[0].group(1)} but the nearest "
              f"SPAWN_ESCAPE pick is t={escapes[0].group(1)}, more than "
              f"{ESCAPE_LATENCY} frames later.")
        return 1
    e0 = picks[0]
    t_esc = int(e0.group(1))
    if not check_escape_line(e0):
        return 1

    # -- 2. It survived the window. -----------------------------------------
    later = [r for r in respawns[1:]
             if int(r.group(1)) - t_esc <= SURVIVE_WINDOW]
    if later:
        print(f"FAIL: a second RESPAWN_DETECTED at t={later[0].group(1)}, only "
              f"{int(later[0].group(1)) - t_esc} frames after the escape -- the "
              "tank died again inside the survival window.")
        return 1
    if not check_survival(snap):
        return 1
    print(f"  no second respawn within {SURVIVE_WINDOW} frames of the escape")

    # -- 3. It let go for a good reason. ------------------------------------
    d0 = check_done(text, t_esc)
    if d0 is None:
        return 1

    # -- 4. ...and then went and did the thing it woke up needing. ----------
    t_done = int(d0.group(1))
    tail = text[d0.end():]
    kinds_after = [m.group(1) for m in GOAL_RE.finditer(tail)]
    refuel_after = [k for k in kinds_after if k in REFUEL_GOALS]
    print("  goals after the release: "
          + ", ".join(list(dict.fromkeys(kinds_after))[:8]))
    if not refuel_after:
        print(f"FAIL: the bot never chose {' or '.join(REFUEL_GOALS)} after "
              "the escape released, even though it woke up with 0 shells and "
              f"its own full base is {mdist(SPAWN, BASE)} tiles away.")
        return 1
    post = [x for x in d if x[0] > t_done]
    peak = max((x[4] for x in post), default=0)
    print(f"  shells after the release: {post[0][4] if post else '?'} -> "
          f"peak {peak}")
    if peak <= 0:
        print("FAIL: the bot's shells never rose after the escape -- it chose "
              "refuel but never actually reached stock.")
        return 1
    on_base = [x for x in post if (x[1], x[2]) == BASE]
    print(f"  ticks standing on the base {BASE} after the release: "
          f"{len(on_base)}")

    print(f"PASS: the respawn under pill coverage armed the escape "
          f"(via={armed[0].group(4)}), it drove out (why={d0.group(2)}) "
          "without dying, and the bot then refuelled.")
    return 0


def run_B(text, snap):
    """Same-square death -> the jump gate cannot see it -> arm via=newtank."""
    d = report_common(text, snap)
    if d is None:
        return 1
    print(f"  channel: x={CHANNEL_X}, y {CHANNEL_Y[0]}..{CHANNEL_Y[1]}; every "
          f"tile of it is within the pill's {PILL_FIRE_RANGE}-tile reach and "
          f"at most {max(mdist(SPAWN, (CHANNEL_X, y)) for y in range(CHANNEL_Y[0], CHANNEL_Y[1] + 1))} "
          f"tiles from the start (jump gate is {RESPAWN_CACHE_WIPE_DIST})")
    arms, escapes, rejects, respawns = show_arms_and_escapes(text)

    # -- 0. It has to have died in there at all. ----------------------------
    if not snap or max((x[3] or 0) for x in snap) < 1:
        print("FAIL: the tank never died. The channel is supposed to be a dead "
              "end under the pillbox's gun -- check the SPAWN_ESCAPE_ARENA "
              "death lines in the server output, and that the pill is still "
              "neutral and alive.")
        return 1

    # -- 1./2./3. The arm, and that it came the only way it can here. -------
    if not escapes:
        print("FAIL: the escape never picked a tile. THIS IS THE REGRESSION "
              "THIS VARIANT EXISTS FOR: a tank whose boat is shot out from "
              "under it AT its own start square respawns one tile away, which "
              f"is invisible to RESPAWN_CACHE_WIPE_DIST ({RESPAWN_CACHE_WIPE_DIST}"
              "), so only a distance-independent arm can save it. Arms seen: "
              + (", ".join(f"t={a.group(1)} via={a.group(4)}" for a in arms)
                 or "NONE")
              + "; rejects seen: "
              + (", ".join(sorted({r.group(4) for r in rejects})) or "NONE"))
        return 1
    e0 = escapes[0]
    t_esc = int(e0.group(1))
    before = [a for a in arms if int(a.group(1)) <= t_esc]
    if not before:
        print(f"FAIL: a SPAWN_ESCAPE pick at t={t_esc} with no preceding "
              "SPAWN_ESCAPE_ARM line.")
        return 1
    a0 = before[-1]
    t_arm = int(a0.group(1))
    print(f"  arm that produced the escape: t={t_arm} "
          f"at=({a0.group(2)},{a0.group(3)}) via={a0.group(4)} "
          f"(+{t_esc - t_arm} to the pick)")
    if a0.group(4) == VIA_JUMP:
        print(f"FAIL: the arm says via={VIA_JUMP}. On this arena the tank dies "
              "inside the channel and respawns at most "
              f"{max(mdist(SPAWN, (CHANNEL_X, y)) for y in range(CHANNEL_Y[0], CHANNEL_Y[1] + 1))} "
              "tiles away, so the position-jump path cannot legitimately fire "
              "here -- if it did, the arena has stopped being a same-square "
              "case and nothing is covering the distance-independent arm any "
              "more.")
        return 1
    early = [r for r in respawns if int(r.group(1)) <= t_esc]
    if early:
        print(f"FAIL: RESPAWN_DETECTED fired at t={early[0].group(1)} with "
              f"jump={early[0].group(2)} before the escape. A same-square death "
              "must not clear the jump gate; this arena is no longer testing "
              f"the {VIA_NEWTANK} path.")
        return 1
    print("  no RESPAWN_DETECTED before the escape -- the jump gate never saw "
          "this death, which is exactly the regression case")
    if t_esc - t_arm > ESCAPE_LATENCY:
        print(f"FAIL: armed at t={t_arm} but the escape only picked a tile at "
              f"t={t_esc}, more than {ESCAPE_LATENCY} frames later.")
        return 1
    if not check_escape_line(e0):
        return 1

    # -- 4. It survived, and let go for a good reason. ----------------------
    if not check_survival(snap):
        return 1
    if check_done(text, t_esc) is None:
        return 1

    print(f"PASS: a boat sunk ON the start square armed the escape "
          f"(via={a0.group(4)}, the jump gate never fired), and it drove the "
          "tank out of the coverage without dying again.")
    return 0


def run_one(variant, ticks, build_dir, port=None):
    name = ("far death, the jump gate can see it" if variant == "A"
            else "same-square death, the jump gate cannot see it")
    print(f"=== variant {variant} ({name}), {ticks} ticks"
          + (f", port {port}" if port else "") + " ===")
    text, snap = play(variant, ticks, build_dir, port)
    if text is None:
        return 1
    return run_A(text, snap) if variant == "A" else run_B(text, snap)


def main():
    which = "ALL"
    ticks = None
    port = None
    build = DEFAULT_BUILD
    args = sys.argv[1:]
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            which = args[i + 1].upper(); i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--port":
            port = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    variants = list(VARIANTS) if which == "ALL" else [which]
    rc = 0
    for v in variants:
        try:
            rc |= run_one(v, ticks or TICKS[v], build, port)
        except subprocess.TimeoutExpired:
            print(f"FAIL: variant {v} timed out")
            rc = 1
        print()
    print("OVERALL: " + ("PASS" if rc == 0 else "FAIL"))
    sys.exit(rc)


if __name__ == "__main__":
    main()
