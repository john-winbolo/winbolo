#!/usr/bin/env python3
"""
LGM wall-shot kill repro (field incident 20260901_182558_1_par1b bot2 t=8410).

A tank two tiles south of a wall shoots it while its own builder, dispatched
north-and-a-tile-west, is pinned flush against the wall's south edge sliding
west.  The movement gates never let the man into the wall square, but
lgmDeathCheckAtPosition maps him with a -1/-2 world-unit nudge ((x-1)>>8,
(y-2)>>8); when the pin remainder lands 0-1 wu above the boundary the check
places him INSIDE the wall square, and "explosion on a solid square kills the
man on that square" fires.  He dies to his own tank's wall shot while standing
on open ground.  PR #262 switches the death check to the movement code's raw
>>8 mapping.

Mechanics the brain relies on (src/bolo/lgm.c, tank.c):

  * The man walks 16 wu/tick.  Sent (own_x - 1, FOREST_ROW) from two rows
    south of the wall the step is (xAdd, yAdd) = (-2, -16), so he pins at
    world-y = BOUNDARY + (start_y mod 16), where BOUNDARY = (WALL_ROW+1)*256
    is the wall's south edge.  He then slides west along the wall at 2 wu/tick
    at that SAME y until he is straight south of his target and gives up
    (lgmMoveAway: y blocked + xAdd == 0 -> LGM_STATE_RETURN).  A man sent
    straight north (K=0) gives up the very tick he touches the wall -- no
    window -- which is why K=1.
  * While pinned, his death square is ((x-1)>>8, (y-2)>>8): the WALL square
    whenever start_y mod 16 is 0 or 1.  The tank fires straight north the whole
    time; every shell explodes on the wall square and runs the same-square man
    check.  The wall takes 5 shells to rubble (and a shot wall is still solid),
    so the builder REPAIRS it every REBUILD_EVERY (4) shells with the in-game
    BUILDMODE_BUILD action -- no scenario scripting, so this runs on main.
  * The tank's own residual-speed mover (tank.c Step 2) advances 6 wu the
    first tick the residual reaches TANK_MIN_MOVE_SPEED, so a 2-frame throttle
    tap from rest is exactly one 6 wu creep north.  Creeping 6 wu per trial
    walks start_y through every residue of the tank's starting parity within
    8 trials -- and {0,1} contains one residue of each parity, so the lethal
    band is always reached.
  * info.speed is a stale snapshot for a stopped bot (it never returns to 0),
    so "stopped" is detected from the tank's world position, never from speed.
  * info.man_x/man_y are the wire snapshot of the man, quantized to 16 wu, so
    the brain cannot observe the pin residue directly (a pinned man always
    reads y == BOUNDARY).  The tank's own info.tankx/tanky are raw, and the
    man is spawned at exactly the tank's world position (lgmNewPrimaryRequest
    -> tankGetWorld), so the residue is start_y mod 16 and every trial is
    keyed by that.  The PIN line only confirms he really pinned at the wall.
    Because that wire position also carries the display mapping's -2, in
    exactly the lethal case the brain (and BrainTest) see the pinned man
    standing IN the wall row -- the DEATH line's sq=(126,122) -- which is
    the picture the par1b session showed.

Report (lgm_wallshot_report.txt in the game's working directory), one line per
event so the verdict is hand-checkable:

  PARKED / CREEP / DISPATCH   tank position, trial number, start_y
  REBUILD / REBUILT           the builder repaired the wall (shells since last,
                              trees left)
  PIN                         the man reached the wall row (quantized man pos)
  MAN                         every man position change (raw info.man_x/y)
  DEATH / HOME                trial outcome, keyed by the start residue, with
                              the tank's shell count (a pin only counts as
                              exercised if the tank had ammo)
  STATUS                      heartbeat every 250 frames

A -brain-debug session (debug_sessions/<TS>_1_wallshot_rN) is written per
game, so any death can be loaded in BrainTest.

Verdict:
  default            PASS = no trial dies (and the lethal band was pinned at
                     least once, else INCONCLUSIVE).  The patched engine.
  --expect-death     PASS = every lethal-band pin died and no other pin did.
                     Run against an unpatched engine to prove the repro bites.

Usage:
    python3 tests/generate_lgm_wallshot_map.py
    python3 tests/lgm_wallshot_test.py --repeat 8 [--exe-root D:/Development/winbolo2]
    python3 tests/lgm_wallshot_test.py --repeat 8 --expect-death --exe-root <unpatched>
"""

import argparse
import os
import subprocess
import sys
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
sys.path.insert(0, str(HERE))
from generate_lgm_wallshot_map import WALL_ROW, FOREST_ROW, STOP_ROW, POND  # noqa: E402

MAP_PATH = HERE / "lgm_wallshot.map"

REPORT = "lgm_wallshot_report.txt"
TICKS = 12000             # engine ticks (2 per brain frame): park + 12 trials plus repairs
TRIALS_PER_GAME = 12      # the 6 wu creep visits all 8 residues of the start
                          # parity in 8 trials; ~1 shell a trial from 40
K = 1                     # harvest target one tile WEST: the man slides
LETHAL = {0, 1}           # pin residues (man_y - BOUNDARY) the old check kills

BRAIN_TEMPLATE = r'''
-- GENERATED by tests/lgm_wallshot_test.py -- do not edit.
--
-- Shoot-north wall-shot brain.  Park two rows south of the wall, then per
-- trial: creep the tank 6 wu north (a fresh start_y residue), send the man to
-- harvest one tile west beyond the wall, and hold fire on the wall square
-- dead ahead while he is out near the wall.  Log everything to
-- lgm_wallshot_report.txt.  Frames (think calls) are the time unit; the
-- engine runs two ticks per frame.
local K          = __K__
local WALL_ROW   = __WALL_ROW__
local FOREST_ROW = __FOREST_ROW__
local STOP_ROW   = __STOP_ROW__
local TRIALS     = __TRIALS__
local BOUNDARY   = (WALL_ROW + 1) * 256   -- wall's south edge (row 123 top)

-- The wall the tank fires at takes BUILDING_LIFE+1 = 5 shells to go from a
-- fresh BUILDING to RUBBLE (shell 1 -> HALFBUILDING life 4, shells 2..4 ->
-- life 1, shell 5 -> RUBBLE).  A HALFBUILDING is still man-speed 0, so the
-- man pins and the death still fires on it; only RUBBLE lets him through.
-- So the builder REPAIRS the wall before it rubbles: after REBUILD_EVERY
-- shells (< 5) it is dispatched with BUILDMODE_BUILD onto the wall square,
-- which resets it to a full BUILDING for 1 tree.  No scenario scripting --
-- this is the in-game LGM build action, so the test runs on plain main.
-- The tank is gametype "open", which starts it with a full tree store.
local REBUILD_EVERY = 4

local tick = 0
local phase = "drive"          -- drive -> align -> dispatched -> (rebuild) -> creep -> align
local sub, sub_t = nil, 0      -- creep sub-phase: accel -> coast -> brake
local creep_y = -1
local trials, deaths = 0, 0
local shots = 0                -- shells landed on the wall since the last repair
local shot_this_trial = false
local last_status = 0
local last_mx, last_my = -1, -1
local dispatch_tick = 0
local start_y = -1
local pin_y = nil
local pin_tick = 0
local still = 0                -- frames the tank has not moved
local ptx, pty = -1, -1
local brake = 0

local function report(line)
  local f, err = io.open("lgm_wallshot_report.txt", "a")
  if f then f:write(line .. "\n"); f:close()
  else io.stderr:write("lgm_wallshot: report open failed: " .. tostring(err) .. "\n") end
end

local function res_of() return start_y % 16 end   -- pin residue = start_y mod 16 (see header)

local function start_creep(ty)
  phase, sub, sub_t, creep_y = "creep", "accel", 0, ty
end

-- End a harvest trial: repair the wall first if it is close to rubble,
-- otherwise creep to the next residue.  (The rebuild phase waits for the man,
-- so a death trial's man is repaired-with once he respawns into the tank.)
local function finish_trial(ty)
  if shots >= REBUILD_EVERY then phase = "rebuild"
  else start_creep(ty) end
end

brain = {}
function brain.think(info)
  tick = tick + 1
  local hold, tap = 0, 0
  local bld = nil

  local tx, ty = info.tankx or 0, info.tanky or 0
  local mx = math.floor(tx / 256)
  local my = math.floor(ty / 256)
  local dir = info.direction or 0
  local spd = info.speed or 0
  local man = info.man_status or 0
  local north = (dir <= 2 or dir >= 254)

  if tx == ptx and ty == pty then still = still + 1 else still = 0 end
  ptx, pty = tx, ty

  if tick % 250 == 1 then
    report(string.format("STATUS t=%d phase=%s/%s txy=(%d,%d) sq=(%d,%d) dir=%d spd=%d boat=%s man=%d trials=%d deaths=%d",
                         tick, phase, tostring(sub), tx, ty, mx, my, dir, spd, tostring(info.inboat), man, trials, deaths))
  end

  -- Keep the nose pointed north (tap inside the last few bradians so a held
  -- key does not oscillate past 0).
  if not north then
    local key = (dir > 128) and KEY_TURNRIGHT or KEY_TURNLEFT
    if dir >= 246 or dir <= 10 then tap = tap + key else hold = hold + key end
  end

  if phase == "drive" then
    if my > STOP_ROW + 3 and north then
      -- full throttle while afloat (a boat needs speed to beach)
      if info.inboat or spd < 12 then hold = hold + KEY_FASTER end
    elseif my > STOP_ROW then
      -- beached and closing: bleed the exit speed off early so the tank
      -- does not coast on into the wall row
      if spd > 6 then hold = hold + KEY_SLOWER
      elseif spd < 4 then hold = hold + KEY_FASTER end
    else
      hold = hold + KEY_SLOWER
      brake = brake + 1
      if brake >= 4 and still >= 5 then
        phase = "align"
        report(string.format("PARKED t=%d txy=(%d,%d) sq=(%d,%d)", tick, tx, ty, mx, my))
      end
    end

  elseif phase == "creep" and man == 0 then
    -- One 6 wu step north: throttle 2 frames (engine speed 1.0), coast until
    -- the residual mover fires, then brake to rest.
    if my <= WALL_ROW + 1 then
      phase = "align"                        -- at the wall already; no creep
    elseif sub == "accel" then
      hold = hold + KEY_FASTER
      sub_t = sub_t + 1
      if sub_t >= 2 then sub, sub_t = "coast", 0 end
    elseif sub == "coast" then
      sub_t = sub_t + 1
      if ty ~= creep_y then sub, sub_t = "brake", 0
      elseif sub_t > 20 then sub, sub_t = "accel", 0 end
    else -- brake
      hold = hold + KEY_SLOWER
      sub_t = sub_t + 1
      if sub_t >= 4 and still >= 5 then
        phase = "align"
        report(string.format("CREEP t=%d moved=%d txy=(%d,%d)", tick, creep_y - ty, tx, ty))
      end
    end

  elseif phase == "align" and man == 0 then
    hold = hold + KEY_SLOWER
    if trials >= TRIALS then
      -- sweep done: sit still
    elseif still >= 5 and north then
      bld = { x = mx - K, y = FOREST_ROW, action = BUILDMODE_FARM }
      phase = "dispatched"
      trials = trials + 1
      dispatch_tick = tick
      start_y = ty
      pin_y = nil
      shot_this_trial = false
      last_mx, last_my = -1, -1
      report(string.format("DISPATCH t=%d trial=%d start_y=%d res=%d txy=(%d,%d) target=(%d,%d) shells=%d trees=%d",
                           tick, trials, ty, ty % 16, tx, ty, mx - K, FOREST_ROW, info.shells or -1, info.trees or -1))
    end

  elseif phase == "dispatched" then
    hold = hold + KEY_SLOWER
    if man == 2 and info.man_y then
      local man_my = math.floor(info.man_y / 256)
      -- Pin detection uses the boundary band, NOT the row: the brain's man
      -- view is the wire position, which carries the display mapping's -2,
      -- so in exactly the lethal case (true y == BOUNDARY) he reads as
      -- standing in the WALL row (y == BOUNDARY-16, row 122).  That is the
      -- same picture BrainTest paints for the par1b death.
      if pin_y == nil and info.man_y < BOUNDARY + 16 and info.man_y >= BOUNDARY - 16 then
        pin_y = info.man_y
        pin_tick = tick
        report(string.format("PIN t=%d trial=%d man=(%d,%d) sq=(%d,%d) res=%d after=%d",
                             tick, trials, info.man_x, info.man_y,
                             math.floor(info.man_x / 256), man_my, res_of(), tick - dispatch_tick))
      end
      -- Fire a short burst from the pin frame while he is in the tank's own
      -- column: a shell exploding on the wall square ahead then runs the
      -- same-square man check against him.  One explosion is enough, so
      -- ~1-2 shells a trial keeps 40 shells good for the whole sweep.
      -- (Firing the whole time he was out burned 16 a trial: dry by trial 3.)
      local man_mx = math.floor(info.man_x / 256)
      if pin_y ~= nil and tick - pin_tick < 10 and man_mx == mx and north then
        hold = hold + KEY_SHOOT
        -- One burst puts exactly one shell on the wall square; count it once.
        if not shot_this_trial then shots = shots + 1; shot_this_trial = true end
      end
      if info.man_x ~= last_mx or info.man_y ~= last_my then
        report(string.format("MAN t=%d x=%d y=%d (sq %d,%d)", tick,
                             info.man_x, info.man_y,
                             math.floor(info.man_x / 256), math.floor(info.man_y / 256)))
        last_mx, last_my = info.man_x, info.man_y
      end
    end
    if last_status == 2 and man == 1 then
      deaths = deaths + 1
      report(string.format("DEATH t=%d trial=%d res=%d start_y=%d after=%d last=(%d,%d) sq=(%d,%d) shells=%d",
                           tick, trials, res_of(), start_y, tick - dispatch_tick,
                           last_mx, last_my, math.floor(last_mx / 256), math.floor(last_my / 256), info.shells or -1))
      finish_trial(ty)
    elseif last_status ~= 0 and man == 0 then
      report(string.format("HOME t=%d trial=%d res=%d start_y=%d dead_so_far=%d shells=%d",
                           tick, trials, res_of(), start_y, deaths, info.shells or -1))
      finish_trial(ty)
    elseif tick - dispatch_tick > 1500 then
      report(string.format("TIMEOUT t=%d trial=%d man=%d res=%d", tick, trials, man, res_of()))
      finish_trial(ty)
    end

  elseif phase == "rebuild" and man == 0 then
    -- Repair the wall the tank has been chipping at, before it rubbles.  Send
    -- the man onto the wall square with BUILDMODE_BUILD; on a HALFBUILDING that
    -- costs 1 tree and resets it to a full BUILDING.  No firing here.
    hold = hold + KEY_SLOWER
    if still >= 5 and north then
      bld = { x = mx, y = WALL_ROW, action = BUILDMODE_BUILD }
      phase = "rebuild_wait"
      dispatch_tick = tick
      report(string.format("REBUILD t=%d after_shots=%d txy=(%d,%d) target=(%d,%d) trees=%d",
                           tick, shots, tx, ty, mx, WALL_ROW, info.trees or -1))
    end

  elseif phase == "rebuild_wait" then
    hold = hold + KEY_SLOWER
    if last_status ~= 0 and man == 0 then
      shots = 0
      report(string.format("REBUILT t=%d trees=%d", tick, info.trees or -1))
      start_creep(ty)
    elseif tick - dispatch_tick > 1500 then
      report(string.format("REBUILD_TIMEOUT t=%d man=%d trees=%d", tick, man, info.trees or -1))
      shots = 0
      start_creep(ty)
    end
  end

  if info.events then
    for _, ev in ipairs(info.events) do
      if ev.type == EVENT_LGM_LOST and ev.data then
        report(string.format("EV_LGM_LOST t=%d victim=%s killer=%s",
                             tick, tostring(ev.data[1]), tostring(ev.data[2])))
      end
    end
  end

  last_status = man
  return { holdkeys = hold, tapkeys = tap, build = bld }
end

return brain
'''


def write_brain(dirpath: Path, k: int) -> Path:
    dirpath.mkdir(parents=True, exist_ok=True)
    lua = (BRAIN_TEMPLATE
           .replace("__K__", str(k))
           .replace("__WALL_ROW__", str(WALL_ROW))
           .replace("__FOREST_ROW__", str(FOREST_ROW))
           .replace("__STOP_ROW__", str(STOP_ROW))
           .replace("__TRIALS__", str(TRIALS_PER_GAME)))
    p = dirpath / "init.lua"
    p.write_text(lua, encoding="utf-8", newline="\n")
    return p


def run_game(exe_root: Path, brain_path: Path, label: str, ticks: int, port: int,
             exe: Path = None):
    # Single-bot arena, run from the BUILD dir: -ai yesfull, -quiet,
    # -threads 1, -brain-debug (arms the unsandboxed io the report needs and
    # writes a BrainTest-loadable session).  The brain uses no math.random and
    # is trivially light, so it needs no seed or budget flags -- only the
    # engine -seed, which makes the whole game deterministic.
    build_dir = exe_root / "build"
    report = build_dir / REPORT
    if report.exists():
        report.unlink()
    env = dict(os.environ)
    env["WINBOLO_BRAINDBG_LABEL"] = label
    cmd = [
        str(exe or (build_dir / "WinBoloDS.exe")),
        "-map", str(MAP_PATH),
        "-port", str(port), "-nolobby",
        "-gametype", "open",
        "-bots", "1",
        "-brain", str(brain_path),
        "-ai", "yesfull",
        "-limit", "20",
        "-brain-debug", "-seed", "42", "-ticks", str(ticks),
        "-nowinbolonet", "-quiet", "-threads", "1",
    ]
    t0 = time.time()
    r = subprocess.run(cmd, cwd=str(build_dir), env=env,
                       capture_output=True, text=True,
                       timeout=max(300, ticks // 20))
    lines = report.read_text(encoding="utf-8").splitlines() if report.exists() else []
    return r.returncode, lines, time.time() - t0


def summarize(lines):
    """Per trial: the start residue (DISPATCH), whether it pinned, whether it died."""
    import re
    dispatched = {}  # trial -> start residue
    shells = {}      # trial -> shells in the tank at dispatch
    pins = {}        # trial -> start residue, only trials that reached the wall
    dead = {}        # trial -> start residue
    for l in lines:
        m = re.search(r"trial=(\d+)", l)
        if not m:
            continue
        tr = int(m.group(1))
        r = re.search(r"\bres=(-?\d+)", l)
        res = int(r.group(1)) if r else None
        if l.startswith("DISPATCH"):
            dispatched[tr] = res
            sm = re.search(r"shells=(\d+)", l)
            shells[tr] = int(sm.group(1)) if sm else -1
        elif l.startswith("PIN"):
            pins[tr] = dispatched.get(tr, res)
        elif l.startswith("DEATH"):
            dead[tr] = res
    return dispatched, pins, dead, shells


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repeat", type=int, default=1)
    ap.add_argument("--expect-death", action="store_true",
                    help="unpatched engine: PASS iff every lethal-band pin "
                         "died and no other pin did")
    ap.add_argument("--exe-root", default=str(REPO))
    ap.add_argument("--ticks", type=int, default=TICKS)
    ap.add_argument("--exe", default=None,
                    help="WinBoloDS.exe to run instead of <exe-root>/build/WinBoloDS.exe "
                         "(e.g. a side copy built from the unpatched lgm.c); the game "
                         "still runs from <exe-root>/build")
    args = ap.parse_args()

    exe_root = Path(args.exe_root).resolve()
    assert (exe_root / "build" / "WinBoloDS.exe").exists(), exe_root
    assert MAP_PATH.exists(), "run tests/generate_lgm_wallshot_map.py first"
    brains_dir = Path(os.environ.get("TEMP", str(HERE))) / "lgm_wallshot_brains" / f"K{K}"
    bp = write_brain(brains_dir, K)

    ok = True
    for i in range(args.repeat):
        rc, lines, dt = run_game(exe_root, bp, f"wallshot_r{i}", args.ticks, 27650 + i,
                                 Path(args.exe).resolve() if args.exe else None)
        dispatched, pins, dead, shells = summarize(lines)
        # a pin only counts as exercised if the tank had ammo to volley it
        pins = {t: r for t, r in pins.items() if shells.get(t, 0) > 0}
        pin_res = sorted(set(pins.values()))
        dead_res = sorted(set(dead.values()))
        print(f"run {i+1}/{args.repeat}: {len(dispatched)} dispatches, "
              f"{len(pins)} pinned at residues {pin_res}; deaths at residues "
              f"{dead_res or 'NONE'} ({len(dead)} of {len(dispatched)}) "
              f"({dt:.0f}s rc={rc})")
        if not dispatched:
            print("  FAIL: no dispatch at all -- harness problem")
            sys.exit(2)
        if not pins:
            print("  FAIL: the man never pinned against the wall with ammo in the tank -- harness problem")
            sys.exit(2)
        lethal_pinned = sorted(LETHAL & set(pin_res))
        if not lethal_pinned:
            print(f"  INCONCLUSIVE: the residue sweep never pinned in the lethal "
                  f"band {sorted(LETHAL)} with ammo (armed pins: {pin_res})")
            sys.exit(2)
        if args.expect_death:
            stray = [r for r in dead_res if r not in LETHAL]
            missed = [r for r in lethal_pinned if r not in dead_res]
            if stray:
                print(f"  FAIL: a SAFE residue killed the man: {stray}")
                ok = False
            elif missed:
                print(f"  FAIL: lethal residue(s) {missed} pinned but did not kill")
                ok = False
            else:
                print(f"  REPRODUCED: exactly the lethal residues {dead_res} "
                      "killed; every safe residue survived")
        else:
            if dead_res:
                print(f"  FAIL: the man died at residues {dead_res} -- the "
                      "death check still maps a pinned man into the wall")
                ok = False
            else:
                print(f"  PASS: survived every pin, lethal band {lethal_pinned} included")

    print("\nOVERALL:", "PASS" if ok else "FAIL")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
