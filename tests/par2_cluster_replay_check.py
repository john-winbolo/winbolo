#!/usr/bin/env python3
"""Offline re-pricing of the par2 incident under the new rules.

Reads a RECORDED session's print2 log and recomputes, from the numbers that
session already printed, what the goal pool WOULD have looked like with the
three changes in place:

  Part 1  damage-source scariness  - a healthy team pill taking pillbox strays
          gets no ARRIVED-watch bid (DEFEND_WATCH_MIN_HP_FRAC, 2/3)
  Part 2  dead-pill cluster discount  - cost / min(n, CAPTURE_CLUSTER_DIVISOR_MAX),
          floored at CAPTURE_CLUSTER_MIN_COST
  Part 3  hostile-territory guard  - x (1 + CAPTURE_CLUSTER_GUARD_MULT * k),
          capped at CAPTURE_CLUSTER_GUARD_MAX

Nothing is simulated.  Every input is a number the run already logged:

  CAPTURE_CAND  ->  which pills were dead, where  ->  n and the cluster
  ENGINE_DUMP   ->  the full pillbox table with hostility bits  ->  which live
                    hostile/neutral pills are even in RANGE of a cluster tile
  HEAT_GATE     ->  which team pill was bidding the watch, at what HP
  FINAL_SCORES  ->  the pool as it actually stood: each row's base and total

The one thing this cannot replay offline is the LINE OF FIRE half of the guard
test (goals.lua runs cpf.simulate_shot against the live map, and the recorded
log does not carry the terrain).  So the guard count k is reported as a RANGE:
every live hostile/neutral pill whose euclidean distance to some cluster tile
is within PILLBOX_RANGE (+ the heated margin) is a CANDIDATE guard, and the
table prints the re-priced pool for every k from 0 to that many.  Read the row
for the k the real line-of-fire test would return.

Usage:
  python par2_cluster_replay_check.py [SESSION_DIR] [--bot 3] [--ticks 17930,18080,18130]

Default session: build/debug_sessions/20260901_160325_1_par2, bot 3, the three
ticks of the incident.  Prints a table; exits 0 if it could parse everything.
"""

import re
import sys
from pathlib import Path

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_SESSION = REPO / "build" / "debug_sessions" / "20260901_160325_1_par2"
DEFAULT_TICKS = [17930, 18080, 18130]

# constants.lua, the block this change added plus the ones it leans on
CLUSTER_RADIUS = 3            # CAPTURE_CLUSTER_RADIUS   (Manhattan, single-linkage)
DIVISOR_MAX = 6               # CAPTURE_CLUSTER_DIVISOR_MAX
MIN_COST = 5                  # CAPTURE_CLUSTER_MIN_COST
GUARD_MULT = 0.75             # CAPTURE_CLUSTER_GUARD_MULT (per covering pill)
GUARD_MAX = 4.0               # CAPTURE_CLUSTER_GUARD_MAX
WATCH_MIN_HP_FRAC = 2.0 / 3.0  # DEFEND_WATCH_MIN_HP_FRAC
PILLS_MAX_HEALTH = 15
PILLBOX_RANGE_WU = 2048       # PILLBOX_RANGE_WU
COVER_MARGIN_HOT_WU = 256     # SEA_COVER_MARGIN_HOT_WU (a heated pill's extra tile)
IMMINENT_PATH_COST = 30       # IMMINENT_CAPTURE_PATH_COST - the pool clamps a
IMMINENT_FLOOR = 5            # capture priced at or under it to this floor

CAND_RE = re.compile(
    r"CAPTURE_CAND t=(\d+) id=(\d+) @\((\d+),(\d+)\) hp=(\d+) owner=(\w+) "
    r"in_tank=(\w+) carrier=(\S+) synth=(\S+) last_seen=(\d+) reject=(\S+)")
DUMP_RE = re.compile(r"ENGINE_DUMP t=(\d+) .*?\| OBJ=(.*?) \| EVT=")
PILL_OBJ_RE = re.compile(r"ty2#(\d+)@\((\d+),(\d+)\)info=0x([0-9a-fA-F]+)")
SCORES_HEAD_RE = re.compile(r"FINAL_SCORES t=(\d+) pool_size=(\d+) cur=(\S+)")
# Rows are emitted directly under their FINAL_SCORES header, so the parser
# tracks "which header am I under" - and MUST drop that tracking at every
# header, including headers for ticks we are not interested in, or a later
# pool's rows get filed under the last tick we did want.
SCORES_ROW_RE = re.compile(
    r"\[(\d+)\] (\w+)@(\d+),(\d+) total=([\d.eE+-]+) base=([\d.eE+-]+) "
    r"pen=([\d.eE+-]+).*?desc=(.*)$")
WATCH_RE = re.compile(
    r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\) WATCH (\S+) \((\w+)\).*? hp=(\d+) "
    r"anger=([\d.]+) shells=(\d+) hit_age=(\S+) sight_age=(\S+) setup_age=(\S+)")


def mdist(a, b):
    return abs(a[0] - b[0]) + abs(a[1] - b[1])


def edist(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def build_clusters(tiles_by_id, radius):
    """goals.lua build_pill_clusters: greedy single-linkage over SORTED ids."""
    clusters = []
    by_pill = {}
    for pid in sorted(tiles_by_id):
        t = tiles_by_id[pid]
        home = None
        for ci, cl in enumerate(clusters):
            if any(mdist(t, u) <= radius for u in cl["tiles"]):
                home = ci
                break
        if home is None:
            clusters.append({"ids": [], "tiles": []})
            home = len(clusters) - 1
        clusters[home]["ids"].append(pid)
        clusters[home]["tiles"].append(t)
        by_pill[pid] = home
    return clusters, by_pill


def guard_mult(k):
    return min(GUARD_MAX, 1.0 + GUARD_MULT * k)


def cluster_price(pre, n, k):
    """The new tail of compute_pool4_cost, exactly as written there."""
    div = min(n, DIVISOR_MAX)
    c = pre / div
    if c < MIN_COST:
        c = MIN_COST
    return c * guard_mult(k), div


def parse(logtext, ticks):
    """Everything the recomputation needs, per tick of interest."""
    out = {t: {"dead": {}, "pills": {}, "rows": [], "watches": [], "cur": None}
           for t in ticks}
    cur_tick = None
    for line in logtext.splitlines():
        m = CAND_RE.search(line)
        if m and int(m.group(1)) in out:
            t = int(m.group(1))
            if m.group(11) == "nil" and int(m.group(5)) == 0 \
                    and m.group(7) == "false":
                out[t]["dead"][int(m.group(2))] = (int(m.group(3)), int(m.group(4)))
            continue
        m = DUMP_RE.search(line)
        if m and int(m.group(1)) in out:
            t = int(m.group(1))
            for pm in PILL_OBJ_RE.finditer(m.group(2)):
                # info bit 0 = OBJECT_HOSTILE, bit 1 = OBJECT_NEUTRAL (brain.h)
                info = int(pm.group(4), 16)
                out[t]["pills"][int(pm.group(1))] = {
                    "tile": (int(pm.group(2)), int(pm.group(3))),
                    "hostile": bool(info & 1), "neutral": bool(info & 2)}
            continue
        m = SCORES_HEAD_RE.search(line)
        if m:
            t = int(m.group(1))
            if t in out:
                out[t]["cur"] = m.group(3)
                cur_tick = t
            else:
                cur_tick = None
            continue
        m = WATCH_RE.search(line)
        if m and int(m.group(1)) in out:
            out[int(m.group(1))]["watches"].append(m.groups())
            continue
        m = SCORES_ROW_RE.search(line)
        if m:
            # Rows follow their FINAL_SCORES header; the header set cur_tick.
            t = cur_tick
            if t is not None:
                out[t]["rows"].append({
                    "rank": int(m.group(1)), "kind": m.group(2),
                    "mx": int(m.group(3)), "my": int(m.group(4)),
                    "total": float(m.group(5)), "base": float(m.group(6)),
                    "pen": float(m.group(7)), "desc": m.group(8)})
    return out


def report_tick(t, d):
    print(f"\n=== t={t}  (the run picked: {d['cur']}) " + "=" * 24)
    if not d["rows"]:
        print("  no FINAL_SCORES rows at this tick - nothing to recompute")
        return False

    # ── the heap ─────────────────────────────────────────────────────────
    clusters, by_pill = build_clusters(d["dead"], CLUSTER_RADIUS)
    if not clusters:
        print("  no dead pills logged at this tick")
        return False
    heap = max(clusters, key=lambda c: len(c["ids"]))
    n = len(heap["ids"])
    print(f"  dead pills: {len(d['dead'])} -> {len(clusters)} cluster(s); "
          f"largest n={n} ids={heap['ids']} tiles={heap['tiles']}")

    # ── candidate guards, by RANGE only (see the module docstring) ────────
    live = {pid: p for pid, p in d["pills"].items()
            if (p["hostile"] or p["neutral"]) and pid not in d["dead"]}
    cands = []
    for pid, p in sorted(live.items()):
        best = min(edist(p["tile"], u) for u in heap["tiles"])
        calm = best * 256 <= PILLBOX_RANGE_WU
        hot = best * 256 <= PILLBOX_RANGE_WU + COVER_MARGIN_HOT_WU
        if hot:
            cands.append((pid, p["tile"],
                          "neutral" if p["neutral"] else "hostile", best,
                          "calm+hot" if calm else "hot only"))
    print(f"  live hostile/neutral pills: "
          f"{[(pid, p['tile'], 'neutral' if p['neutral'] else 'hostile') for pid, p in sorted(live.items())]}")
    if cands:
        for pid, tile, own, dd, band in cands:
            print(f"    guard candidate #{pid} {tile} ({own}) nearest cluster "
                  f"tile {dd:.2f} tiles -> in range ({band})")
    else:
        print("    no live pill is even in RANGE of a cluster tile -> k=0")
    kmax = len(cands)

    # ── the capture row, re-priced ───────────────────────────────────────
    cap = None
    for r in d["rows"]:
        if r["kind"] == "capture_pill" and (r["mx"], r["my"]) in heap["tiles"]:
            cap = r
            break
    if cap is None:
        print("  no capture_pill row targeting the heap at this tick")
        return False
    pre = cap["base"]
    shape = (cap["total"] / cap["base"]) if cap["base"] else 1.0
    print(f"  capture row as logged: base={pre:.1f} total={cap['total']:.1f} "
          f"(pool shaping x{shape:.2f})")
    print(f"    -> {cap['desc'][:96]}")
    print(f"  re-priced: base / min({n},{DIVISOR_MAX}) = /{min(n, DIVISOR_MAX)}"
          f", floored at {MIN_COST}, then x(1 + {GUARD_MULT} k) capped {GUARD_MAX}")
    print(f"    {'k':>2}  {'guard x':>8}  {'new base':>9}  {'IMMINENT':>9}  {'new total':>10}")
    new_totals = {}
    for k in range(0, kmax + 1):
        c, div = cluster_price(pre, n, k)
        clamped = IMMINENT_FLOOR if c <= IMMINENT_PATH_COST else c
        tot = clamped * shape
        new_totals[k] = tot
        print(f"    {k:>2}  {guard_mult(k):>8.2f}  {c:>9.1f}  {clamped:>9.1f}  "
              f"{tot:>10.1f}")

    # ── the defend row, re-gated ─────────────────────────────────────────
    dropped = []
    for w in d["watches"]:
        wt, wx, wy, cost, block, hp = (int(w[0]), int(w[1]), int(w[2]),
                                       w[3], w[4], int(w[5]))
        frac = hp / PILLS_MAX_HEALTH
        if block == "taking_damage" and frac >= WATCH_MIN_HP_FRAC:
            dropped.append(((wx, wy), hp, frac))
    if dropped:
        print("  defend watch bids that the source gate REMOVES, assuming the "
              "damage attributes to pillbox strays (src=npill/epill):")
        for tile, hp, frac in dropped:
            print(f"    pill@{tile} hp={hp}/{PILLS_MAX_HEALTH} "
                  f"({frac:.2f} >= {WATCH_MIN_HP_FRAC:.2f}) -> NO watch bid")
    else:
        print("  no watch bid at this tick is removed by the source gate")
    dropped_tiles = {t for t, _, _ in dropped}

    # ── the pool, before and after ───────────────────────────────────────
    for k in range(0, kmax + 1):
        rows = []
        for r in d["rows"]:
            if r["kind"] == "capture_pill" and (r["mx"], r["my"]) in heap["tiles"]:
                rows.append((new_totals[k], r["kind"] + "@heap"))
                continue
            if r["kind"] == "defend_pill":
                m = re.search(r"defend#\d+@\((\d+),(\d+)\)", r["desc"])
                if m and (int(m.group(1)), int(m.group(2))) in dropped_tiles:
                    continue   # no bid at all now
            rows.append((r["total"], f"{r['kind']}@{r['mx']},{r['my']}"))
        rows.sort()
        winner = rows[0] if rows else (None, "-")
        order = ", ".join(f"{nm} {tv:.0f}" for tv, nm in rows[:4])
        print(f"  k={k}: winner would be {winner[1]} ({winner[0]:.0f})   "
              f"top4: {order}")
    return True


def main():
    args = sys.argv[1:]
    session = DEFAULT_SESSION
    bot = 3
    ticks = list(DEFAULT_TICKS)
    i = 0
    while i < len(args):
        if args[i] == "--bot":
            bot = int(args[i + 1]); i += 2
        elif args[i] == "--ticks":
            ticks = [int(x) for x in args[i + 1].split(",")]; i += 2
        else:
            session = Path(args[i]); i += 1
    log = session / f"print2_bot{bot}.log"
    if not log.exists():
        print(f"no such log: {log}")
        return 1
    print(f"session {session.name}, bot {bot}, ticks {ticks}")
    text = log.read_text(errors="ignore")
    data = parse(text, ticks)
    ok = True
    for t in ticks:
        ok = report_tick(t, data[t]) and ok
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
