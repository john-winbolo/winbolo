#!/usr/bin/env python3
"""Sea-pill harvest test — capture_pill's DEEP-SEA branch (GoalHunter 1.7).

Field incident 20260901_000042_1_loss_b6 bot2 t=22735: three dead pills sat in
the first two deep-sea columns off the east shore of DH-Oil Rig and the pool
rejected all three every 50 ticks with `reject=deepsea_no_boat`.  Nothing in
the brain knew how to get afloat.

capture_pill now prices an ENTRANCE into the pills' own body of water and runs
a substate chain in front of the pickup:

  entrance_plan -> (refuel_mines) -> (seek_trees) -> approach_F
                -> lay_mine -> detonate -> build_boat -> board -> pickup

with an entrance LADDER: an existing boat, else an existing river (build the
boat straight into it, 20 trees, no mine), else a mined shore tile cardinally
adjacent to that water (21 trees + 1 mine + shells).

Variants (see tests/generate_sea_pills_map.py for the arenas):
  A  full loadout, no hostiles      -> mine/crater/river/boat, 3 pills taken
  B  hostile pill with a clear line -> REJECT pills_covered_by_pill#N, no mine
  C  same pill behind a breakwater  -> executes like A
  D  0 mines, base stocks mines     -> refuel_mines leg first, then A
  E  0 trees                        -> harvest leg first; the mine NEVER goes
                                       down before 21 trees + 1 mine are held
  F  the only forest is a row a live
     hostile pill COVERS end to end  -> REJECT no_safe_trees, no mine, no LGM
  G  existing river in the shore    -> no mine at all; boat built into the
                                       river; 3 pills taken
  H  forest in a corner two HOSTILE
     bases own by influence, covered
     by nothing                     -> the harvest RUNS; 3 pills taken. The
                                       par2 t=23410 shape: the retired
                                       influence gate refused this wood, the
                                       coverage gate funds it.

Usage: python sea_pills_test.py [--variant A|B|B2|C|D|E|F|G|H|ALL] [--ticks N]
                                [--build DIR] [--jobs N]

Variants run two at a time by default (--jobs, capped at 2): each one is a full
WinBoloDS process and the sim is CPU bound, so more workers make every run
slower rather than the set faster. Output is buffered per variant and printed
whole, in order, once everything finishes.
Exit 0 on PASS, 1 on FAIL.
"""

import concurrent.futures as cf
import json
import math
import os
import re
import subprocess
import sys
import time
from pathlib import Path

# -asap by default: ticks run back-to-back instead of one per 20 ms of wall
# clock. Same seed -> byte-identical game, just faster. --no-asap (or
# WINBOLO_ASAP=0) puts this run back on the 20 ms live-game pacing.
from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
from generate_sea_pills_map import (            # noqa: E402
    SEA_PILLS, ISOLATED_POND, RIVER_TILE, FIELD_X1, SEA_PILL_TREES_TOTAL,
    SEA_TREES_PER_FOREST, hostile_pill_for, sea_pills_for,
    gametype_for, VARIANTS)

PILLBOX_RANGE_TILES = 8.0   # pillbox.h PILLBOX_RANGE 2048 wu, inclusive
SEA_PILL_MIN_SHELLS = 3     # constants.lua — reserved to detonate the mine

# Engine terrain values (src/bolo/public/global.h). A mined tile is its base
# terrain + MINE_SUBTRACT(8), so 10..15 are the mined forms of SWAMP..GRASS.
T_BUILDING, T_RIVER, T_CRATER, T_ROAD, T_FOREST, T_GRASS = 0, 1, 3, 4, 5, 7
T_BOAT = 9
MINE_START, MINE_END, MINE_SUBTRACT = 10, 15, 8
TNAME = {0: "BUILDING", 1: "RIVER", 2: "SWAMP", 3: "CRATER", 4: "ROAD",
         5: "FOREST", 6: "RUBBLE", 7: "GRASS", 8: "HALFBUILDING", 9: "BOAT",
         255: "DEEP_SEA"}


def tname(v):
    if MINE_START <= v <= MINE_END:
        return TNAME.get(v - MINE_SUBTRACT, str(v - MINE_SUBTRACT)) + "+MINE"
    return TNAME.get(v, str(v))


SEA_PLAN_RE = re.compile(
    r"SEA_PLAN t=(\d+) cluster=(\d+) n=(\d+) entrance=(\S+) comp=(\d+) "
    r"S=\((\d+),(\d+)\) F=(\S+) mine=(\S+) boat=(\S+) "
    r"trees=(\d+)/(\d+) short=(\d+) need_tiles=(\d+) forest_ok=(\d+) "
    r"mines=(\d+)/(\d+) shells=(\d+)/(\d+) dropped=\[([^\]]*)\] "
    r"covwater=(\d+) refuel=(\S+) travel=(\S+) legs=(\S+)/(\S+) "
    r"boat_path=(\S+) cost=(\S+)")
SEA_SUB_RE = re.compile(r"SEA_SUB t=(\d+) from=(\S+) to=(\S+) reason=(.*)")
SEA_REJECT_RE = re.compile(
    r"SEA_REJECT t=(\d+) cluster=(\d+) n=(\d+) pills=\[([^\]]*)\] reason=(\S+) \((.*)\)")
SEA_ABORT_RE = re.compile(r"SEA_ABORT t=(\d+) reason=(\S+)")
SEA_SHOT_RE = re.compile(r"SEA_SHOT t=(\d+) #(\d+) at S=\((\d+),(\d+)\)")
SEA_DISPATCH_RE = re.compile(r"SEA_DISPATCH t=(\d+) action=(\S+) S=\((\d+),(\d+)\)")
TREES_RE = re.compile(r"ENGINE_DUMP t=(\d+) .*? tr=(\d+)")
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\).*?arm=(\d+) sh=(\d+) mn=(\d+) "
    r"tr=(\d+).*?boat=(\w+)")
CARRY_RE = re.compile(r"ENGINE_DUMP t=(\d+) .*? carry=(\d+)")
CAPTURE_CAND_RE = re.compile(
    r"CAPTURE_CAND t=(\d+) id=(\S+) @\((\d+),(\d+)\).*?reject=(\S+)")
LAY_MINE_TREES_RE = re.compile(r"trees (\d+)/(\d+) and mines (\d+)/(\d+)")
# goals.lua sea_count_safe_forest, one line per scan:
#   SEA_FOREST t=160 S=(136,128) tank=(132,130) R=12 threats=0 ok=25 covered=0
#     nearest=(132,129) infl_at_nearest=92
# infl is REPORTED ONLY - the harvest no longer reads it. It is the number the
# retired SEA_TREES_MIN_INFLUENCE gate used to test, kept so variant H can show
# a harvest going ahead on ground that gate called enemy territory.
SEA_FOREST_RE = re.compile(
    r"SEA_FOREST t=(\d+) S=\((\d+),(\d+)\) tank=\((\d+),(\d+)\) R=(\d+) "
    r"threats=(\d+) ok=(\d+) covered=(\d+) nearest=(\S+) infl_at_nearest=(\S+)")


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


def read_trace(path):
    """{(x,y): [(tick, value), ...]} from the sidecar's terrain log."""
    out = {}
    if not path.exists():
        return out
    for line in path.read_text(errors="ignore").splitlines():
        if line.startswith("#") or not line.strip():
            continue
        parts = line.split()
        if len(parts) != 5:
            continue
        tick, x, y, old, new = (int(p) for p in parts)
        out.setdefault((x, y), []).append((tick, new))
    return out


def run_one(variant, ticks, build_dir, port):
    # Output is BUFFERED, not printed: variants run in parallel and their
    # lines would otherwise interleave into nonsense. main() prints each
    # block whole, in canonical order.
    out = []

    def emit(*a):
        out.append(" ".join(str(x) for x in a))

    ds = find_ds(build_dir)
    if not ds:
        emit(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1, out
    subprocess.run([sys.executable, str(HERE / "generate_sea_pills_map.py"),
                    "--variant", variant],
                   check=True, stdout=subprocess.DEVNULL)
    mapfile = HERE / f"sea_pills_{variant}.map"
    final = HERE / f"sea_pills_{variant}_final.json"
    snap = HERE / f"sea_pills_{variant}_snap.jsonl"
    stderr = HERE / f"sea_pills_{variant}_stderr.txt"
    trace = build_dir / f"sea_pills_terrain_{variant}.log"
    label = f"sea_pills_{variant}"
    # Windows holds a just-exited process's files open for a moment, and with
    # variants running concurrently that moment lands on a sibling's cleanup.
    # Retry briefly; only a file still locked after that is a real "previous run
    # still going".
    for p in (final, snap, stderr, trace):
        for attempt in range(20):
            if not p.exists():
                break
            try:
                p.unlink()
                break
            except PermissionError:
                if attempt == 19:
                    emit(f"FAIL: {p.name} is still locked after 5s — a previous "
                         f"WinBoloDS run is probably still going.")
                    return 1, out
                time.sleep(0.25)

    # A map with a sidecar boots as gameScripted, which hands a -bots tank the
    # OPEN loadout (40/40/40) whatever -gametype says. The variants that need an
    # empty tank therefore run with -bots 0 and let the sidecar spawn its own
    # bot with an explicit loadout mode (game.spawn_bot arms sim->spawnLoadout
    # before tankCreate). -noemptyreset keeps the round alive until it lands.
    scripted_spawn = gametype_for(variant) != "open"
    # The brain's capacity tier is driven by wall-clock lastThinkMs, so under
    # load it tiers down and plays differently from the same seed — running two
    # variants at once was enough to change which pills the bot fetched. Pin it
    # so the test measures the brain, not the machine.
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label,
               WINBOLO_BRAIN_TIER="10")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(port), "-nolobby",
           "-gametype", gametype_for(variant),
           "-bots", "0" if scripted_spawn else "1",
           "-brain", str(BRAIN),
           # yesfull: the whole (tiny) arena is known from tick 0 — the test is
           # about getting afloat, not about fog.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-snapjson", str(snap), "-snapinterval", str(SNAP_INTERVAL),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    if scripted_spawn:
        cmd.append("-noemptyreset")
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 20))

    sess = newest_session(build_dir, label)
    if not sess:
        emit("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1, out
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        emit(f"FAIL: brain crashed — see {crashes[0]}")
        emit(Path(crashes[0]).read_text(errors="ignore")[:2000])
        return 1, out
    # The scripted-spawn variants land in whatever slot spawn_bot picked (it
    # fills from the TOP down), so find the log rather than assuming bot0.
    logs = sorted(sess.glob("print2_bot*.log"), key=lambda p2: p2.stat().st_size)
    if not logs:
        emit(f"FAIL: no print2_bot*.log under {sess}")
        return 1, out
    log = logs[-1]
    emit(f"  brain log: {log.name}")
    text = log.read_text(errors="ignore")

    plans = SEA_PLAN_RE.findall(text)
    subs = SEA_SUB_RE.findall(text)
    rejects = SEA_REJECT_RE.findall(text)
    aborts = SEA_ABORT_RE.findall(text)
    shots = SEA_SHOT_RE.findall(text)
    dispatches = SEA_DISPATCH_RE.findall(text)
    tr = read_trace(trace)

    emit(f"  SEA_PLAN lines: {len(plans)}   SEA_SUB: {len(subs)}   "
          f"SEA_REJECT: {len(rejects)}   SEA_ABORT: {len(aborts)}   "
          f"SEA_SHOT: {len(shots)}   SEA_DISPATCH: {len(dispatches)}")
    if plans:
        for line in text.splitlines():
            if "SEA_PLAN t=" in line:
                emit("  first plan: " + line.split("SEA_PLAN ", 1)[1].strip())
                break
    for r in dict.fromkeys((r[4], r[5]) for r in rejects):
        emit(f"  reject: {r[0]} — {r[1][:150]}")
    for s in subs:
        emit(f"  SEA_SUB t={s[0]} {s[1]} -> {s[2]}  ({s[3][:90]})")
    for a in aborts:
        emit(f"  SEA_ABORT t={a[0]} reason={a[1]}")

    # ── Terrain trace at the entrance ──────────────────────────────────
    ent_S = (int(plans[0][5]), int(plans[0][6])) if plans else None
    if ent_S and ent_S in tr:
        seq = [(t, tname(v)) for (t, v) in tr[ent_S]]
        emit(f"  terrain at S{ent_S}: " +
              " -> ".join(f"{n}@{t}" for (t, n) in seq))
    any_mine_laid = any(MINE_START <= v <= MINE_END
                        for hist in tr.values() for (_, v) in hist)
    boats = sorted({xy for xy, hist in tr.items()
                    if any(v == T_BOAT for (_, v) in hist)})
    emit(f"  mined tile ever seen: {any_mine_laid}    boats appeared at: {boats}")

    # -- Pills collected, and WHEN --------------------------------------
    pills_here = sea_pills_for(variant)

    def collected_in(snapshot):
        n = 0
        for (x, y) in pills_here:
            lying = False
            for pb in snapshot.get("pillboxes", []):
                if (pb.get("tx"), pb.get("ty")) == (x, y)                         and not pb.get("in_tank") and (pb.get("armor") or 0) == 0:
                    lying = True
            if not lying:
                n += 1
        return n

    collected = 0
    per_pill = {}
    if final.exists():
        fd = json.load(open(final))
        collected = collected_in(fd)
        for (x, y) in pills_here:
            lying = any((pb.get("tx"), pb.get("ty")) == (x, y)
                        and not pb.get("in_tank") and (pb.get("armor") or 0) == 0
                        for pb in fd.get("pillboxes", []))
            per_pill[(x, y)] = not lying
    # First snapshot at which the WHOLE cluster was out of the water. Snapshots
    # land every SNAP_INTERVAL sim ticks, so this is the completion tick to that
    # resolution; the brain tick is half the sim tick.
    done_tick = None
    if snap.exists():
        for line in snap.read_text(errors="ignore").splitlines():
            if not line.strip():
                continue
            sn = json.loads(line)
            if collected_in(sn) >= len(pills_here):
                done_tick = sn.get("tick")
                break
    emit(f"  sea pills collected: {collected}/{len(pills_here)}"
          + (f"   COMPLETED by sim tick {done_tick} (brain tick ~{done_tick // 2})"
             if done_tick is not None else ""))

    # -- What the whole cluster COST ------------------------------------
    mine_dispatches = [d2 for d2 in dispatches if d2[1] == "MINE"]
    boat_dispatches = [d2 for d2 in dispatches if d2[1].startswith("BUILD")]
    # Trees the HARVEST cost, which means trees spent BEFORE the cluster came
    # out of the water. What the tank does with its wood afterwards is not this
    # test's business: from 2026-09-03 the sea plan (and its 21-tree reserve)
    # is released the tick the goal stops being that harvest, so the tank goes
    # on to DEPLOY the pills it just fetched -- 4 wood a piece. That used to be
    # impossible (builder.decide bailed `sea_trees_reserved` on the retired
    # plan forever) and it is the whole point of the change, so counting those
    # placements against the boat's budget would assert the bug back in.
    trees = [(int(m.group(1)), int(m.group(2))) for m in TREES_RE.finditer(text)]
    harvest_end = (done_tick // 2) if done_tick is not None else None
    trees_spent, trees_after = 0, 0
    for (ta, a), (_, b) in zip(trees, trees[1:]):
        if b < a:
            if harvest_end is None or ta <= harvest_end:
                trees_spent += a - b
            else:
                trees_after += a - b
    emit(f"  cluster cost: {len(mine_dispatches)} mine dispatch(es), "
          f"{len(boat_dispatches)} boat build(s), {trees_spent} trees spent"
          + (f" (+{trees_after} after the cluster was aboard, brain tick > "
             f"{harvest_end})" if trees_after else ""))
    for (x, y), got in sorted(per_pill.items()):
        emit(f"    pill ({x},{y}): {'collected' if got else 'LEFT at sea'}")

    # Loadout + refuel trace: what the tank started with, and what it had when
    # the mine went down.
    dumps = DUMP_RE.findall(text)
    start = dumps[0] if dumps else None
    if start:
        emit(f"  start loadout: armour={start[3]} shells={start[4]} "
              f"mines={start[5]} trees={start[6]}")
    lay = next((s2 for s2 in subs if s2[2] == "lay_mine"), None)
    refuel_span = None
    rin = next((int(s2[0]) for s2 in subs if s2[2] == "refuel_mines"), None)
    rout = None
    if rin is not None:
        rout = next((int(s2[0]) for s2 in subs
                     if int(s2[0]) > rin and s2[1] == "refuel_mines"), None)
        if rout:
            refuel_span = rout - rin
            emit(f"  refuel leg: t={rin}..{rout} ({refuel_span} brain ticks)")

    carries = CARRY_RE.findall(text)
    final_carry = int(carries[-1][1]) if carries else None
    emit(f"  pills carried at the end: {final_carry}")

    # Never inside a hostile pill's firing circle while afloat.
    hp = hostile_pill_for(variant)
    closest_afloat = None
    if hp:
        for d2 in dumps:
            if d2[7] == "true":
                dist = math.hypot(int(d2[1]) - hp[0], int(d2[2]) - hp[1])
                if closest_afloat is None or dist < closest_afloat:
                    closest_afloat = dist
        emit(f"  closest the boat ever came to hostile pill {hp}: "
              + (f"{closest_afloat:.2f} tiles" if closest_afloat is not None
                 else "never afloat"))

    ok, why = check(variant, plans, subs, rejects, aborts, dispatches,
                    tr, any_mine_laid, boats, collected,
                    done_tick, mine_dispatches, boat_dispatches, trees_spent,
                    per_pill, dumps, closest_afloat, text, refuel_span, shots,
                    final_carry)
    if ok:
        emit(f"PASS ({variant}): {why}")
        return 0, out
    emit(f"FAIL ({variant}): {why}")
    return 1, out


def _ordered_terrain(tr, xy):
    """The values seen at a tile, in tick order."""
    return [v for (_, v) in tr.get(xy, [])]


def _flood_sequence_ok(vals):
    """The mine -> crater -> river -> boat progression, judged on FIRST
    appearance. Later oscillation is normal and not a failure: a tank that
    drives off a boat turns the tile back into river (it takes the boat with
    it), and a second trip builds another one. A step may also go unobserved
    (the tick sampling can miss CRATER when the flood lands between samples),
    but the firsts that ARE observed must be in order."""
    rank = {}
    for v in range(MINE_START, MINE_END + 1):
        rank[v] = 1
    rank[T_CRATER] = 2
    rank[T_RIVER] = 3
    rank[T_BOAT] = 4
    firsts = {}
    for i, v in enumerate(vals):
        r = rank.get(v)
        if r is not None and r not in firsts:
            firsts[r] = i
    order = [firsts[r] for r in sorted(firsts)]
    return all(a < b for a, b in zip(order, order[1:])), firsts


def check(variant, plans, subs, rejects, aborts, dispatches,
          tr, any_mine_laid, boats, collected,
          done_tick, mine_dispatches, boat_dispatches, trees_spent,
          per_pill, dumps, closest_afloat, text, refuel_span, shots,
          final_carry):
    sub_targets = [s[2] for s in subs]
    reject_reasons = [r[4] for r in rejects]
    # (ok, covered, influence-at-the-nearest-tile) per forest scan. Influence is
    # "-" when the scan found no tile at all.
    forest_scans = [(int(m[7]), int(m[8]),
                     int(m[10]) if m[10] not in ("-", "nil") else None)
                    for m in SEA_FOREST_RE.findall(text)]

    if variant == "H":
        # H exists to prove the gate CHANGED, so it has to show the old gate
        # would have said no. Influence is reported, never read: if the wood the
        # scan settled on is on ground cpf.influence_at calls ours, the retired
        # SEA_TREES_MIN_INFLUENCE test would have funded this harvest too and
        # the arena is measuring nothing.
        if not forest_scans:
            return False, ("no SEA_FOREST scan lines - the harvest never had to "
                           "count wood, so this variant proves nothing")
        infls = [f[2] for f in forest_scans if f[2] is not None]
        if infls and min(infls) > 0:
            return False, ("every forest tile the scan settled on sits on ground "
                           f"cpf.influence_at calls OURS (lowest {min(infls)} > 0). "
                           "The retired SEA_TREES_MIN_INFLUENCE gate would have "
                           "funded this too - move the hostile bases closer or "
                           "our base further away.")
        if max((f[1] for f in forest_scans), default=0) > 0:
            return False, ("a forest tile read as covered by a pill - variant H "
                           "must have no pillbox coverage anywhere")

    if variant in ("A", "C", "D", "E", "G", "H"):
        if not plans:
            return False, "no SEA_PLAN line — the deep-sea branch never priced the cluster"
        entrance = plans[0][3]
        S = (int(plans[0][5]), int(plans[0][6]))
        want_entrance = "existing_river" if variant in ("G", "H") else "mine_crater"
        if entrance != want_entrance:
            return False, f"entrance was {entrance}, expected {want_entrance}"
        # Connectivity: the entrance must belong to the pills' water, never the
        # land-locked pond.
        for pond in ISOLATED_POND:
            if abs(S[0] - pond[0]) + abs(S[1] - pond[1]) <= 1:
                return False, (f"entrance S={S} is cardinally adjacent to the "
                               f"land-locked pond {pond} — the connectivity gate failed")
        if variant in ("G", "H"):
            if S != RIVER_TILE:
                return False, f"expected the existing river tile {RIVER_TILE}, got S={S}"
            # The FIRST trip must use the river: no mine before the tank is
            # afloat. Later trips legitimately fall back to a crater — boarding
            # CONSUMES the boat (the tank takes it), which turns the tile back
            # to river and then the river tile itself is gone once that boat
            # sails away, so a second load has no free entrance left.
            first_board = next((i for i, s2 in enumerate(sub_targets)
                                if s2 == "board"), None)
            if first_board is None:
                return False, "never boarded"
            if "lay_mine" in sub_targets[:first_board]:
                return False, ("entered lay_mine before boarding even though an "
                               "existing river was available")
            if RIVER_TILE not in boats:
                return False, f"no boat was ever built on the river tile {RIVER_TILE}"
        else:
            if S[0] != FIELD_X1:
                return False, (f"entrance S={S} is not on the shore column x={FIELD_X1}")
            if not any_mine_laid:
                return False, "no mine was ever laid"
            ok, firsts = _flood_sequence_ok(_ordered_terrain(tr, S))
            if not ok:
                return False, (f"terrain at S={S} did not progress "
                               f"mine -> crater -> river -> boat "
                               f"(first-seen indices by stage {firsts})")
            vals = _ordered_terrain(tr, S)
            if T_RIVER not in vals:
                return False, f"S={S} never flooded to RIVER"
            if T_BOAT not in vals:
                return False, f"S={S} never became a BOAT"
        if S not in boats:
            return False, f"no boat was ever built at the entrance {S}"
        if variant == "G" and "seek_trees" not in sub_targets:
            return False, "the seek_trees leg never ran (trees started at 0)"
        if variant == "D":
            if "refuel_mines" not in sub_targets:
                return False, "the refuel_mines leg never ran (mines started at 0)"
            # STRICT tournament: the tank spawns with nothing at all, so the
            # leg has to fetch BOTH the mine and the shells that set it off.
            if dumps:
                s0 = dumps[0]
                if int(s0[4]) != 0 or int(s0[5]) != 0 or int(s0[6]) != 0:
                    return False, (f"strict loadout expected 0/0/0, got shells="
                                   f"{s0[4]} mines={s0[5]} trees={s0[6]}")
            lay_t = next((int(x[0]) for x in subs if x[2] == "lay_mine"), None)
            if lay_t is None:
                return False, "never reached lay_mine"
            at_lay = None
            for d2 in dumps:
                if int(d2[0]) <= lay_t:
                    at_lay = d2
                else:
                    break
            if at_lay is None:
                return False, "no ENGINE_DUMP at the moment the mine went down"
            sh, mn = int(at_lay[4]), int(at_lay[5])
            if mn < 1:
                return False, f"the mine went down with mines={mn}"
            if sh < SEA_PILL_MIN_SHELLS:
                return False, (f"the mine went down with shells={sh}, below the "
                               f"{SEA_PILL_MIN_SHELLS} reserved to detonate it")
            if not shots:
                return False, "the mine was never shot — the shells were not used"
        if variant in ("D", "E", "H") and "seek_trees" not in sub_targets:
            return False, "the seek_trees leg never ran (trees started at 0)"
        if variant == "E":
            lm = [s for s in subs if s[2] == "lay_mine"]
            if not lm:
                return False, "never reached lay_mine"
            for s in lm:
                m = LAY_MINE_TREES_RE.search(s[3])
                if not m:
                    return False, f"lay_mine transition did not report its resources: {s[3]}"
                have, need, mhave, mneed = (int(g) for g in m.groups())
                if have < need or have < SEA_PILL_TREES_TOTAL:
                    return False, (f"the mine went down with trees {have}/{need} "
                                   f"(need >= {SEA_PILL_TREES_TOTAL})")
                if mhave < mneed:
                    return False, f"the mine went down with mines {mhave}/{mneed}"
            order_ok = sub_targets.index("seek_trees") < sub_targets.index("lay_mine")
            if not order_ok:
                return False, "lay_mine came before the harvest leg"
        if collected < len(sea_pills_for(variant)):
            return False, (f"only {collected}/{len(sea_pills_for(variant))} sea pills were "
                           f"collected within one game minute")
        if done_tick is None:
            return False, "the cluster was never all out of the water in a snapshot"
        # ONE boat trip takes the whole cluster: one mine, one boat, and the
        # trees for them. More than that means the bot went ashore mid-trip and
        # paid for a second entrance — the thing the collect phase exists to
        # prevent, and what turned "three free pills" into 3 mines and 60 trees.
        if len(mine_dispatches) > 1:
            return False, (f"{len(mine_dispatches)} mines laid for one cluster "
                           f"(expected exactly 1)")
        if len(boat_dispatches) > 1:
            return False, (f"{len(boat_dispatches)} boats built for one cluster "
                           f"(expected exactly 1)")
        budget = SEA_PILL_TREES_TOTAL + SEA_TREES_PER_FOREST
        if trees_spent > budget:
            return False, (f"{trees_spent} trees spent on one cluster "
                           f"(expected <= {budget} = 21 + one harvest overshoot)")
        return True, (f"entrance={entrance} S={S}, boat built, "
                      f"{collected}/{len(sea_pills_for(variant))} pills taken by brain tick "
                      f"~{done_tick // 2} "
                      f"({len(mine_dispatches)} mine, {len(boat_dispatches)} boat, "
                      f"{trees_spent} trees)"
                      + (", refuel leg ran" if variant == "D" else "")
                      + (", harvest ran on wood two hostile bases own by "
                         "influence (lowest infl "
                         + str(min([f[2] for f in forest_scans
                                    if f[2] is not None] or [0]))
                         + ")" if variant == "H" else "")
                      + (", harvest leg ran with the mine held back until "
                         "21 trees were in hand" if variant == "E" else ""))

    if variant == "B":
        # ONE member (the middle pill, exactly 8.0 tiles from the hostile pill)
        # is inside its firing circle; the outer two at 9.2 tiles are not. The
        # raft must SPLIT: two pills fetched, one left, and the boat must never
        # enter the circle on the way.
        hp = hostile_pill_for("B")
        covered_pill = SEA_PILLS[1]
        want_taken = [SEA_PILLS[0], SEA_PILLS[2]]
        member_rejects = [r for r in reject_reasons
                          if r.startswith("covered_by_pill#")]
        if not member_rejects and f"covered_by_pill#" not in text:
            return False, ("no per-pill covered_by_pill reject — the raft was "
                           f"not split (rejects seen: {sorted(set(reject_reasons))})")
        for xy in want_taken:
            if not per_pill.get(xy):
                d = math.hypot(xy[0] - hp[0], xy[1] - hp[1])
                return False, (f"pill {xy} ({d:.1f} tiles from the hostile pill, "
                               f"outside its {PILLBOX_RANGE_TILES}-tile circle) "
                               f"was not collected")
        if per_pill.get(covered_pill):
            return False, (f"pill {covered_pill} sits 8.0 tiles from the hostile "
                           f"pill — inside PILLBOX_RANGE — and must be left alone")
        if closest_afloat is not None and closest_afloat <= PILLBOX_RANGE_TILES:
            return False, (f"the boat came within {closest_afloat:.2f} tiles of "
                           f"the hostile pill (its circle is {PILLBOX_RANGE_TILES})")
        # The split raft is worth exactly 2, and the per-pill check above
        # already says WHICH 2 came out of the water (a placed pill is alive
        # somewhere else, so it still reads as collected). Carry is only a
        # ceiling: more than 2 would mean the covered member was fetched
        # anyway. FEWER is fine and now normal -- since the sea plan is
        # released with the goal (2026-09-03) the tank can afford to deploy
        # what it fetched instead of driving around with it.
        if final_carry is not None and final_carry > 2:
            return False, (f"the tank ended carrying {final_carry} pills; the "
                           f"split raft is worth at most 2")
        if done_tick is None:
            done_tick = 0
        return True, (f"raft SPLIT: {want_taken[0]} and {want_taken[1]} taken, "
                      f"{covered_pill} left (covered_by_pill#), boat never closer "
                      f"than {closest_afloat:.2f} tiles to the hostile pill")

    if variant == "B2":
        covered = [r for r in reject_reasons if r.startswith("pills_covered_by_pill#")]
        if not covered:
            return False, ("no pills_covered_by_pill reject — with every member "
                           f"inside the circle the whole cluster must go "
                           f"(rejects seen: {sorted(set(reject_reasons))})")
        if any_mine_laid:
            return False, "a mine was laid despite every member being covered"
        if "lay_mine" in sub_targets:
            return False, "entered lay_mine despite every member being covered"
        if boats:
            return False, f"a boat was built at {boats} despite every member being covered"
        if any(per_pill.values()):
            return False, "a covered pill was collected"
        return True, f"whole cluster rejected with {covered[0]}, no mine, no boat"

    if variant == "F":
        if "no_safe_trees" not in reject_reasons:
            return False, ("expected REJECT no_safe_trees; got "
                           f"{sorted(set(reject_reasons))}")
        # ...and for the right reason: the scan must have FOUND the forest and
        # thrown it out as covered, not merely failed to see any.
        covered_seen = max((f[1] for f in forest_scans), default=0)
        ok_seen = max((f[0] for f in forest_scans), default=0)
        if covered_seen == 0:
            return False, ("the reject fired but no forest tile was ever counted "
                           "as covered_by_pill - the arena is exercising the "
                           f"wrong gate (SEA_FOREST scans: {forest_scans[:3]})")
        if any_mine_laid:
            return False, "a mine was laid even though the wood is not obtainable"
        if "lay_mine" in sub_targets:
            return False, "entered lay_mine even though the wood is not obtainable"
        if dispatches:
            return False, f"the LGM was dispatched ({dispatches[0]}) with no fundable plan"
        return True, (f"rejected no_safe_trees ({covered_seen} forest tile(s) "
                      f"covered by a pill, {ok_seen} safe), no mine, no LGM trip")

    return False, f"unknown variant {variant}"


# ONE GAME MINUTE for every variant. -ticks counts SIM ticks and the brain runs
# at half that rate, so 6000 sim ticks == 3000 brain ticks == 60 s of play. The
# arena is sized so a full mine -> crater -> river -> boat -> three-pickup cycle
# fits inside it with room to spare; if a variant needs more than a minute here,
# that is a bug in the plan, not a budget to raise.
DEFAULT_TICKS = {v: 6000 for v in VARIANTS}
SNAP_INTERVAL = 100     # completion-tick resolution
PORTS = {"A": 50061, "B": 50062, "B2": 50068, "C": 50063, "D": 50064,
         "E": 50065, "F": 50066, "G": 50067, "H": 50069}


# Two at a time, and no more. Each variant is a full WinBoloDS process playing
# a game to completion; the machine has other work to do and the runs are only
# independent because every variant owns its port, its map, its logs and its
# debug-session label. Raising this is not a free speed-up — the sim is CPU
# bound and over-subscribing just makes every run slower and the wall-clock
# worse. MAX_JOBS is a hard ceiling, not a default.
MAX_JOBS = 2


def main():
    variant = "ALL"
    ticks = None
    build = DEFAULT_BUILD
    jobs = MAX_JOBS
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1].upper(); i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        elif args[i] == "--jobs":
            jobs = max(1, min(MAX_JOBS, int(args[i + 1]))); i += 2
        else:
            i += 1
    todo = list(VARIANTS) if variant == "ALL" else [variant]

    def one(v):
        try:
            return run_one(v, ticks or DEFAULT_TICKS[v], build, PORTS[v])
        except subprocess.TimeoutExpired:
            return 1, [f"FAIL ({v}): run timed out"]
        except Exception as e:                     # noqa: BLE001
            return 1, [f"FAIL ({v}): {type(e).__name__}: {e}"]

    results = {}
    t0 = time.time()
    if len(todo) == 1 or jobs == 1:
        for v in todo:
            results[v] = one(v)
            print(f"[{time.time() - t0:6.1f}s] {v} done", flush=True)
    else:
        with cf.ThreadPoolExecutor(max_workers=jobs) as pool:
            futures = {pool.submit(one, v): v for v in todo}
            for fut in cf.as_completed(futures):
                v = futures[fut]
                results[v] = fut.result()
                print(f"[{time.time() - t0:6.1f}s] {v} done", flush=True)

    rc = 0
    for v in todo:
        r, lines = results[v]
        print(f"-- variant {v} ({gametype_for(v)}) " + "-" * 40)
        for line in lines:
            print(line)
        rc |= r
    print(f"-- {len(todo)} variant(s) in {time.time() - t0:.1f}s "
          f"({jobs} at a time) " + "-" * 20)
    sys.exit(rc)


if __name__ == "__main__":
    main()
