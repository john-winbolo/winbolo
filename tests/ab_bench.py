#!/usr/bin/env python3
"""
ab_bench -- play GoalHunter 1.7 against ITSELF with two different sets of
constants, one per side, and say which side came out ahead.

WHY THIS EXISTS
    Every behaviour change wants an answer to "is it actually better?", and
    until now that meant freezing a whole second brain tree (brains/GH17_keel,
    brains/GH17_a, ...) and benching tree against tree.  With the per-bot
    override tokens (init.lua, "preset=NAME" / "cfg=NAME=VALUE") one brain can
    play both sides of the same game:

        --a "preset=keel"   --b ""                      stock KEEL vs today
        --a "preset=keel"   --b "cfg=SQUAD_MAX_SIZE=3"  one knob at a time
        --a ""              --b "cfg=BLITZ_CONTESTED_RANGE=14"

    Side A is players 0..N-1, side B is players N..2N-1, and the score is
    written from B's point of view, so a POSITIVE mean means B (the change)
    beat A (the baseline):

        score = (bases_B - bases_A) + 0.5*(pills_B - pills_A)
                                    - 0.1*(deaths_B - deaths_A)

    taken off the LAST snapshot row of the game (or the finaljson end state,
    which is the same thing when the game ran to the tick limit).  -quitonwin
    stands, so a game that is genuinely won ends there and the end MINUTE is
    itself a result, reported next to the winner.  The same score is also read
    off the minute-15 row of the same snapshot stream, so a long game answers
    "was the 15-minute leader the eventual winner?" for free.

HOW FAST
    Everything is set for speed: the stripped opt/ brains (no print2, no viz,
    no jsonl), -asap, -quiet, no .wbv recording, one snapshot row per
    game-minute, -threads = min(12, tanks), and --jobs games at once (default
    12 // threads-per-game, so a 6v6 runs one at a time and a 2v2 three wide).
    -brain-no-budget-kill means a slow tick is never a kill, so the thread
    count only decides how long a tick takes, never what the bots decide.
    Per-game and total wall time are printed.

WHAT IS AND IS NOT CONTROLLED
    Both sides run the SAME brain files (the production opt/ tree), the same
    build, the same map, the same -seed and the same -brain-lua-seed, so the
    only difference between them is the init string.  What is NOT controlled is
    the map: sides start at different ends of it, so a single seed is a noisy
    measurement -- run several and read the mean and the W/L, never one game.

RESULT CACHE
    Every game is cached per (label, seed) under build/ab_bench/<label>/, where
    the label is derived from the A/B strings, the map and the shape unless
    --label says otherwise.  A re-run only fills the gaps, and --table
    re-prints everything cached without playing anything.

VERIFYING THE TOKENS ACTUALLY LANDED
    The bench runs the PRODUCTION tree, where print2 is stripped out, so the
    games themselves log nothing about which constants each side got.
    --verify-tokens plays 300 ticks of the same matchup on the SOURCE tree with
    -brain-debug (identical code -- opt/ is generated from it by lua_strip,
    which only removes the print2 emit), prints the [preset]/[cfg] lines each
    side emitted, and deletes the scratch session again.  Run it once whenever
    you change the init strings; the numbers mean nothing if a token was a typo.

Usage:
    python tests/ab_bench.py --a "preset=keel" --b "" --seeds 4242 1 2 \\
                            --minutes 90 --jobs 3 --map easter --per-side 6
    python tests/ab_bench.py --a "preset=keel" --b "" --verify-tokens
    python tests/ab_bench.py --a "preset=keel" --b "" --table
Run from the repo root.
"""
import argparse
import json
import re
import shutil
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
BUILD = REPO / "build"
OUT_ROOT = BUILD / "ab_bench"

# The PRODUCTION tree: what a real game runs, print2 and viz stripped out.
BRAIN = "brains/GoalHunter_1.7/opt/init.lua"
# The same code with the logging still in it -- only --verify-tokens uses it.
DEBUG_BRAIN = "brains/GoalHunter_1.7/init.lua"

MAP_ALIASES = {
    "oilrig":    "data/maps/DH-Oil Rig.map",
    "easter":    "data/maps/Easter Island III.map",
    "slugfest7": "data/maps/Slugfest VII.map",
}
DEFAULT_MAP = "easter"
DEFAULT_PER_SIDE = 6
DEFAULT_MINUTES = 90
SNAP_INTERVAL = 6000            # one snapshot row per game-minute
TICKS_PER_MINUTE = 6000
MIN15_TICK = 15 * TICKS_PER_MINUTE   # the short bench's measuring point
MAX_THREADS = 12                # the machine has 12 logical CPUs
PORT_BASE = 50400
PORT_TOP = 50460
# The bench runs -brain-no-budget-kill, so a slow tick is never a kill; the
# thread count only decides how long a tick takes.


# ---------------------------------------------------------------------------
# map handling
# ---------------------------------------------------------------------------
def resolve_map(arg):
    """--map value -> (absolute path, short name). Accepts an alias or a path."""
    name = arg or DEFAULT_MAP
    rel = MAP_ALIASES.get(name.lower(), name)
    p = Path(rel)
    if not p.is_absolute():
        p = REPO / rel
    if not p.exists():
        raise SystemExit(
            f"map not found: {arg!r} -> {p}\n"
            f"  aliases: {', '.join(sorted(MAP_ALIASES))}\n"
            f"  or pass a path, e.g. --map \"data/maps/DH-Oil Rig.map\"")
    short = next((k for k, v in MAP_ALIASES.items() if v == rel), p.stem)
    return p, short


def map_starts(path):
    """The map file's start count (BMAPBOLO header byte 11)."""
    with open(path, "rb") as f:
        head = f.read(12)
    if len(head) < 12 or head[:8] != b"BMAPBOLO":
        raise SystemExit(f"{path} is not a BMAPBOLO map file")
    return head[11]


# ---------------------------------------------------------------------------
# the game
# ---------------------------------------------------------------------------
def side_players(per_side):
    return tuple(range(per_side)), tuple(range(per_side, 2 * per_side))


def bot_range(lo, hi):
    return str(lo) if lo == hi else f"{lo}-{hi}"


def bot_init(a_arg, b_arg, per_side, brain):
    """-bot-init value. An EMPTY init string means no [..] suffix at all --
    that bot runs plain stock constants."""
    A, B = side_players(per_side)

    def spec(players, arg):
        r = bot_range(players[0], players[-1])
        return f"{r}={brain}[{arg}]" if arg else f"{r}={brain}"

    return f"{spec(A, a_arg)},{spec(B, b_arg)}"


def game_cmd(mapfile, seed, port, ticks, snap, final, a_arg, b_arg, per_side,
             brain=BRAIN, debug=False, threads=None):
    n = 2 * per_side
    cmd = [str(BUILD / "WinBoloDS.exe"),
           "-map", str(mapfile), "-gametype", "tournament", "-ai", "yes",
           "-nolobby", "-quitonwin", "-notracker", "-nowinbolonet",
           "-dontsendlog", "-noinput",
           "-bots", str(n), "-threads", str(threads or min(MAX_THREADS, n)),
           "-brain", brain,
           "-bot-init", bot_init(a_arg, b_arg, per_side, brain),
           "-teams", f"{per_side},{per_side}",
           "-port", str(port), "-seed", str(seed), "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final), "-quiet", "-asap"]
    if snap:
        cmd += ["-snapjson", str(snap), "-snapinterval", str(SNAP_INTERVAL)]
    if debug:
        cmd += ["-brain-debug"]
    return cmd


# ---------------------------------------------------------------------------
# measurement
# ---------------------------------------------------------------------------
def tally(d, per_side):
    """bases/pills/deaths as [A, B] out of one snapshot-shaped dict."""
    A, B = side_players(per_side)
    own = {"pills": [0, 0], "bases": [0, 0], "deaths": [0, 0]}
    for pb in d.get("pillboxes", []) or []:
        o = pb.get("owner", 255)
        if o in A:
            own["pills"][0] += 1
        elif o in B:
            own["pills"][1] += 1
    for bs in d.get("bases", []) or []:
        o = bs.get("owner", 255)
        if o in A:
            own["bases"][0] += 1
        elif o in B:
            own["bases"][1] += 1
    for tk in d.get("tanks", []) or []:
        pn, dth = tk.get("player"), tk.get("deaths")
        if dth is None:
            continue
        if pn in A:
            own["deaths"][0] += dth
        elif pn in B:
            own["deaths"][1] += dth
    return own


def score_of(own):
    """Positive = side B (the change) ahead of side A (the baseline)."""
    return round((own["bases"][1] - own["bases"][0])
                 + 0.5 * (own["pills"][1] - own["pills"][0])
                 - 0.1 * (own["deaths"][1] - own["deaths"][0]), 3)


def side_of(winner, tanks, per_side):
    """Map a finaljson `winner` onto 'A' / 'B' / None.

    The engine writes it as a SENTENCE naming the winning tanks, e.g.
    "Game Won! Winners: Kahlo Dali" -- not a player number -- so the names are
    resolved against the snapshot's own tanks[] (player -> name).  A plain
    integer is still accepted in case the format ever changes.
    """
    A, B = side_players(per_side)
    if winner is None:
        return None
    if isinstance(winner, (int, float)):
        w = int(winner)
        return "A" if w in A else "B" if w in B else None
    text = str(winner)
    if not text.strip():
        return None
    gotA = gotB = False
    for t in tanks or []:
        nm, pn = t.get("name"), t.get("player")
        if not nm or pn is None:
            continue
        if nm in text:
            if pn in A:
                gotA = True
            elif pn in B:
                gotB = True
    if gotA and not gotB:
        return "A"
    if gotB and not gotA:
        return "B"
    return None


def measure(snap, final, per_side):
    rows = []
    sp = Path(snap)
    if sp.exists():
        rows = [ln for ln in sp.read_text(encoding="utf-8",
                                          errors="replace").splitlines() if ln.strip()]
    fin = {}
    fp = Path(final)
    if fp.exists():
        try:
            fin = json.loads(fp.read_text(encoding="utf-8", errors="replace"))
        except ValueError:
            fin = {}
    last = json.loads(rows[-1]) if rows else {}
    if not last and not fin:
        return None
    # Minute 15 off the SAME stream, so a 90-minute game also answers "was the
    # 15-minute leader the eventual winner?".  The engine stamps its periodic
    # rows one tick SHORT of the interval (5999, 11999, ... for -snapinterval
    # 6000), so match the NEAREST row rather than an exact tick; a game that
    # ended before minute 15 legitimately has none.
    m15, best = None, None
    for ln in rows:
        d = json.loads(ln)
        gap = abs(d.get("tick", -1) - MIN15_TICK)
        if gap <= 10 and (best is None or gap < best):
            best, m15 = gap, tally(d, per_side)
    # The finaljson is the authoritative end state; fall back to the last
    # snapshot row when it is missing.
    end = fin if fin.get("tanks") else last
    own = tally(end, per_side)
    end_tick = fin.get("tick", last.get("tick", 0))
    winner = fin.get("winner", last.get("winner"))
    return {
        "outcome": side_of(winner, end.get("tanks"), per_side) or "none",
        "winner_raw": winner,
        "reason": fin.get("reason") or last.get("reason") or "",
        "end_tick": end_tick,
        "end_min": round((end_tick + 1) / float(TICKS_PER_MINUTE), 1),
        "basesA": own["bases"][0], "basesB": own["bases"][1],
        "pillsA": own["pills"][0], "pillsB": own["pills"][1],
        "deathsA": own["deaths"][0], "deathsB": own["deaths"][1],
        "score": score_of(own),
        "score15": score_of(m15) if m15 else None,
        "rows": len(rows),
    }


# ---------------------------------------------------------------------------
# running
# ---------------------------------------------------------------------------
def slug(s):
    s = re.sub(r"[^A-Za-z0-9]+", "_", s or "").strip("_")
    return s[:40] or "stock"


def default_label(a_arg, b_arg, map_short, per_side, minutes):
    return (f"{slug(a_arg)}__vs__{slug(b_arg)}__{slug(map_short)}"
            f"__{per_side}v{per_side}__{minutes}m")


def result_path(out, seed):
    return out / f"s{seed}.json"


def play(out, seed, port, mapfile, ticks, a_arg, b_arg, per_side):
    snap = out / f"s{seed}_snap.jsonl"
    final = out / f"s{seed}_final.json"
    log = out / f"s{seed}_stderr.txt"
    for p in (snap, final):
        if p.exists():
            p.unlink()
    t0 = time.time()
    with open(log, "wb") as errf:
        rc = subprocess.run(
            game_cmd(mapfile, seed, port, ticks, snap, final, a_arg, b_arg,
                     per_side),
            cwd=str(REPO), stdout=errf, stderr=subprocess.STDOUT,
            timeout=max(1800, ticks // 5)).returncode
    m = measure(snap, final, per_side)
    if m is None:
        return None, f"exit={rc}, no snapshot or finaljson written (see {log.name})"
    m["seconds"] = round(time.time() - t0, 1)
    m["seed"] = seed
    return m, None


def cell(m):
    tag = {"A": "WIN-A", "B": "WIN-B"}.get(m["outcome"], "-----")
    return (f"{tag}@{m['end_min']:g} b{m['basesA']}-{m['basesB']} "
            f"p{m['pillsA']}-{m['pillsB']}")


def run_round(out, seeds, jobs, mapfile, ticks, a_arg, b_arg, per_side, force):
    out.mkdir(parents=True, exist_ok=True)
    todo = [s for s in seeds if force or not result_path(out, s).exists()]
    if not todo:
        print("  (every requested seed is already cached; nothing to play)")
        return 0
    print(f"  {len(todo)} game(s) to play, {jobs} at a time ({ticks} ticks each)")
    fails = 0
    free = list(range(jobs))
    lock = threading.Lock()

    def one(seed):
        with lock:
            slot = free.pop()
        try:
            port = PORT_BASE + slot
            assert port <= PORT_TOP
            return (seed,) + play(out, seed, port, mapfile, ticks, a_arg,
                                  b_arg, per_side)
        finally:
            with lock:
                free.append(slot)

    with ThreadPoolExecutor(max_workers=jobs) as ex:
        # One bad game must never take the round down with it: an exception
        # raised here closes ex.map's generator, which CANCELS every job that
        # has not started yet.  Everything inside the loop is guarded.
        for seed, m, err in ex.map(one, todo):
            try:
                if err:
                    print(f"  FAIL s{seed}: {err}", flush=True)
                    fails += 1
                    continue
                result_path(out, seed).write_text(json.dumps(m, indent=1),
                                                  encoding="utf-8")
                m15 = ("%+.1f" % m["score15"]) if m["score15"] is not None else "n/a"
                print(f"  ok   s{seed}: {cell(m)}  score {m['score']:+.1f} "
                      f"(min15 {m15})  d {m['deathsA']}-{m['deathsB']}  "
                      f"reason={m['reason']}  [{m['seconds']}s]", flush=True)
            except Exception as e:                       # noqa: BLE001
                print(f"  ERROR handling s{seed}: {e!r}", flush=True)
                fails += 1
    return fails


def load_all(out):
    res = {}
    if not out.exists():
        return res
    for p in sorted(out.glob("s*.json")):
        if p.name.endswith("_final.json"):
            continue
        try:
            m = json.loads(p.read_text(encoding="utf-8"))
        except ValueError:
            continue
        res[m["seed"]] = m
    return res


def table_text(res, a_arg, b_arg):
    out = []
    if not res:
        return "  (nothing cached yet)\n"
    a_lbl = a_arg or "(stock)"
    b_lbl = b_arg or "(stock)"
    out.append(f"  A = {a_lbl}     B = {b_lbl}")
    out.append("  score is B minus A: positive = B ahead")
    out.append("  " + "seed".ljust(8) + "winner".ljust(9) + "end".ljust(8)
               + "bases".ljust(9) + "pills".ljust(9) + "deaths".ljust(10)
               + "score".ljust(8) + "min15".ljust(8) + "secs")
    out.append("  " + "-" * 76)
    scores, s15s, wins = [], [], {"A": 0, "B": 0, "none": 0}
    for s in sorted(res):
        m = res[s]
        wins[m["outcome"] if m["outcome"] in wins else "none"] += 1
        scores.append(m["score"])
        if m.get("score15") is not None:
            s15s.append(m["score15"])
        out.append("  " + str(s).ljust(8)
                   + {"A": "WIN-A", "B": "WIN-B"}.get(m["outcome"], "none").ljust(9)
                   + (f"{m['end_min']:g}m").ljust(8)
                   + f"{m['basesA']}-{m['basesB']}".ljust(9)
                   + f"{m['pillsA']}-{m['pillsB']}".ljust(9)
                   + f"{m['deathsA']}-{m['deathsB']}".ljust(10)
                   + f"{m['score']:+.1f}".ljust(8)
                   + (f"{m['score15']:+.1f}" if m.get("score15") is not None
                      else "n/a").ljust(8)
                   + f"{m.get('seconds', 0):g}")
    out.append("  " + "-" * 76)
    mean = sum(scores) / float(len(scores))
    m15line = ("   mean min15 %+.2f" % (sum(s15s) / float(len(s15s)))) if s15s else ""
    out.append(f"  {len(scores)} game(s)   mean score {mean:+.2f}{m15line}   "
               f"wins A {wins['A']} / B {wins['B']} / undecided {wins['none']}")
    if mean > 0:
        out.append("  -> B (the change) is ahead on this sample")
    elif mean < 0:
        out.append("  -> A (the baseline) is ahead on this sample")
    else:
        out.append("  -> dead level on this sample")
    return "\n".join(out) + "\n"


# ---------------------------------------------------------------------------
# --verify-tokens
# ---------------------------------------------------------------------------
def verify_tokens(mapfile, a_arg, b_arg, per_side, seed):
    """Play 300 ticks on the SOURCE tree with -brain-debug and show the
    [preset]/[cfg] lines each side emitted, then delete the scratch session.

    The bench itself runs opt/, where print2 is stripped, so this is the only
    way to SEE which constants a side got.  opt/ is generated from these exact
    files by lua_strip and the override block survives the strip untouched --
    only the print2 emit is removed -- so what lands here lands there.
    """
    import os
    label = "_abverify"
    scratch = BUILD / "ab_bench" / "_verify"
    scratch.mkdir(parents=True, exist_ok=True)
    final = scratch / "verify_final.json"
    log = scratch / "verify_stderr.txt"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = game_cmd(mapfile, seed, PORT_TOP, 300, None, final, a_arg, b_arg,
                   per_side, brain=DEBUG_BRAIN, debug=True,
                   threads=min(MAX_THREADS, 2 * per_side))
    print("  " + " ".join(f'"{c}"' if " " in c else c for c in cmd))
    with open(log, "wb") as errf:
        subprocess.run(cmd, cwd=str(REPO), env=env, stdout=errf,
                       stderr=subprocess.STDOUT, timeout=600)

    # The recorder writes debug_sessions/ under the process CWD, which for
    # this bench is the REPO root (relative brain paths must resolve to the
    # repo tree, not to build/Brains) -- so look in both places.
    cands = []
    for root in (REPO / "debug_sessions", BUILD / "debug_sessions"):
        if root.exists():
            cands += [d for d in root.iterdir()
                      if d.is_dir() and d.name.endswith("_" + label)]
    if not cands:
        print("  FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return 1
    sess = max(cands, key=lambda d: d.stat().st_mtime)
    A, B = side_players(per_side)
    rc = 0
    try:
        for name, players, arg in (("A", A, a_arg), ("B", B, b_arg)):
            bot = players[0]
            p = sess / f"print2_bot{bot}.log"
            if not p.exists():
                print(f"  FAIL: no print2_bot{bot}.log for side {name}")
                rc = 1
                continue
            text = p.read_text(encoding="utf-8", errors="replace")
            lines = sorted(set(re.findall(r"\[(?:cfg|preset)\][^\n\"]*", text)))
            print(f"  side {name} (bot{bot}, init {arg!r}):")
            if not lines:
                print("      (no [cfg]/[preset] line -- this side runs stock "
                      "constants)")
                if arg.strip():
                    print(f"      FAIL: side {name} was given {arg!r} but "
                          f"logged no override at all")
                    rc = 1
            for ln in lines:
                print("      " + ln.strip())
                if "IGNORED" in ln or "UNKNOWN" in ln or "BAD TOKEN" in ln:
                    print(f"      FAIL: side {name} had a token refused")
                    rc = 1
    finally:
        shutil.rmtree(sess, ignore_errors=True)
        for p in (final, log):
            if p.exists():
                p.unlink()
    print("  verify: %s" % ("OK" if rc == 0 else "FAILED"))
    return rc


# ---------------------------------------------------------------------------
def main():
    global PORT_BASE
    ap = argparse.ArgumentParser(
        formatter_class=argparse.RawDescriptionHelpFormatter,
        description=__doc__)
    ap.add_argument("--a", default="", help="side A's BRAIN_INIT_ARG (players "
                                            "0..N-1); empty = stock constants")
    ap.add_argument("--b", default="", help="side B's BRAIN_INIT_ARG (players "
                                            "N..2N-1); empty = stock constants")
    ap.add_argument("--seeds", nargs="*", default=["4242"],
                    help="world seeds; space or comma separated")
    ap.add_argument("--minutes", type=int, default=DEFAULT_MINUTES,
                    help="game length in game-minutes (15, 90, ...)")
    ap.add_argument("--jobs", type=int, default=0,
                    help="games in parallel (default: 12 // threads-per-game, "
                         "so a 6v6 runs 1 at a time and a 2v2 runs 3 wide)")
    ap.add_argument("--map", default=DEFAULT_MAP,
                    help="path, or one of: " + ", ".join(sorted(MAP_ALIASES)))
    ap.add_argument("--port-base", type=int, default=PORT_BASE,
                    help="first UDP port; use a different base per concurrently "
                         "running bench (games take base+slot)")
    ap.add_argument("--per-side", type=int, default=DEFAULT_PER_SIDE,
                    help="tanks per side (2 = 2v2, 6 = 6v6)")
    ap.add_argument("--label", default=None, help="cache directory name")
    ap.add_argument("--force", action="store_true", help="replay cached seeds")
    ap.add_argument("--table", action="store_true",
                    help="print the cached table and play nothing")
    ap.add_argument("--verify-tokens", action="store_true",
                    help="300-tick -brain-debug run: show the [preset]/[cfg] "
                         "lines each side got, then delete the session")
    args = ap.parse_args()
    PORT_BASE = args.port_base

    seeds = []
    for s in args.seeds:
        seeds += [int(x) for x in str(s).replace(",", " ").split()]
    if args.per_side < 1:
        raise SystemExit("--per-side must be at least 1")
    mapfile, map_short = resolve_map(args.map)
    starts = map_starts(mapfile)
    n = 2 * args.per_side
    ticks = args.minutes * TICKS_PER_MINUTE
    label = args.label or default_label(args.a, args.b, map_short,
                                        args.per_side, args.minutes)
    out = OUT_ROOT / label

    threads = min(MAX_THREADS, n)
    jobs = args.jobs if args.jobs and args.jobs > 0 else max(1, MAX_THREADS // threads)
    print(f"ab_bench: {mapfile.name}  {args.per_side}v{args.per_side} "
          f"({n} tanks, -threads {threads})  {args.minutes} min "
          f"({ticks} ticks)  jobs {jobs}")
    print(f"  A = players 0-{args.per_side - 1}   init {args.a!r}")
    print(f"  B = players {args.per_side}-{n - 1}   init {args.b!r}")
    print(f"  cache: {out}")
    if starts == 0:
        raise SystemExit(f"{mapfile.name} declares no start positions at all "
                         f"-- nothing can be placed on it")
    if starts < n:
        # NOT an error.  starts.c reuses starts and spiral-scatters to a free
        # deep-sea square nearby (startsGetStart / startsGetStartOpen ->
        # startsScatterFind), which is how the stock 8-start maps have always
        # held 16 players.  Worth saying out loud, because two tanks sharing a
        # start area do begin the game closer together than the map intends.
        print(f"  NOTE: {mapfile.name} has {starts} start position(s) for "
              f"{n} tanks -- the engine reuses starts and scatters to a free "
              f"square nearby (starts.c startsScatterFind), so sides do start "
              f"more crowded than the map was drawn for")
    else:
        print(f"  {starts} start position(s) for {n} tanks")

    if args.verify_tokens:
        return verify_tokens(mapfile, args.a, args.b, args.per_side,
                             seeds[0] if seeds else 4242)

    if not args.table:
        ds = BUILD / "WinBoloDS.exe"
        if not ds.exists():
            raise SystemExit(f"WinBoloDS not found at {ds}")
        t0 = time.time()
        fails = run_round(out, seeds, jobs, mapfile, ticks,
                          args.a, args.b, args.per_side, args.force)
        print("  total wall time %.1fs" % (time.time() - t0))
    else:
        fails = 0
    print()
    print(table_text(load_all(out), args.a, args.b))
    return 1 if fails else 0


if __name__ == "__main__":
    sys.exit(main())
