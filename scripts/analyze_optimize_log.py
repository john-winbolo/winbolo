#!/usr/bin/env python3
"""Analyze optimize.log produced by the GoalHunter brain.

Usage:
    python analyze_optimize_log.py [path/to/optimize.log] [--session N] [--top N]

The log is append-only, so multiple BrainTest sessions accumulate. Each session
boundary is detected by ===TICK 1=== blocks (two bots both start there). By
default the latest session is analyzed; use --session to pick an earlier one
(0 = oldest, -1 = latest).

Outputs a per-section summary:
    - Tick total distribution (worst, mean, p95)
    - Per-phase timings (threat.update, update_pool_cache, dij sched, ...)
    - Pool 6 cache miss diagnostics (count, distance tiers, diff/spot histogram)
    - Slow candidate breakdown (which pool, which subsystem dominates)
    - GC pressure proxies (if collectgarbage("count") deltas are present)
"""

from __future__ import annotations

import argparse
import re
import statistics
import sys
from collections import Counter, defaultdict
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

DEFAULT_LOG = Path(__file__).resolve().parent.parent / "build" / "optimize.log"

# Single-line patterns we extract.
TICK_HDR_RE      = re.compile(r"^===TICK (\d+)===\s*$")
PHASE_LINE_RE    = re.compile(r"^\[(\d+(?:\.\d+)?)ms\]\s+(.*?)\s+done\s+([\d.]+)\s+ms")
INNER_TIME_RE    = re.compile(r"^\[\d+(?:\.\d+)?ms\]\s+([A-Za-z _.+/]+?)\s+([\d.]+)\s+ms\b")
TICK_TOTAL_RE    = re.compile(r"\b(?:STARTUP\s+)?TICK TOTAL\s+([\d.]+)\s+ms")
END_TICK_RE      = re.compile(r"\bEND tick=(\d+)\s+total=([\d.]+)\s+ms")
DIAG_SLOW_CAND   = re.compile(
    r"\[diag\] slow cand pool=(\d+) id=(\S+) total=([\d.]+) raw=([\d.]+) "
    r"adj=([\d.]+) smart=([\d.]+) cost=([-\d.]+) obj=\((-?\d+),(-?\d+)\) hp=(\S+)")
DIAG_POOL6_CAND  = re.compile(
    r"\[diag\] pool6 cand id=(\S+) diff=([\d.]+) spot=([\d.]+) "
    r"just_evaluated=(\S+) force_detailed=(\S+)")
DIAG_THREAT      = re.compile(
    r"\[diag\] threat REBUILD pills=(\d+) clear=([\d.]+) stamp=([\d.]+)")
DIAG_OCCL        = re.compile(r"\[diag\] threat REBUILD occl=([\d.]+)")
DIAG_TERRAIN_FAC = re.compile(
    r"\[diag\] rebuild_terrain_factors: configure=([\d.]+) ms rebuild=([\d.]+) ms")


@dataclass
class Tick:
    """One tick's accumulated stats. Multiple bots' tick=N entries are kept
    separate (one Tick per BEGIN..END block)."""
    n: int
    line_start: int
    total_ms: float | None = None
    phases: dict[str, float] = field(default_factory=dict)
    pool6_cands: list[dict] = field(default_factory=list)
    slow_cands: list[dict] = field(default_factory=list)
    threat_clear_ms: float | None = None
    threat_stamp_ms: float | None = None
    threat_occl_ms: float | None = None
    threat_pill_count: int | None = None


@dataclass
class Session:
    """A contiguous run of ticks. Separated when we see two consecutive
    TICK 1 entries that share a bot (rare) or by line-distance heuristic."""
    start_line: int
    ticks: list[Tick] = field(default_factory=list)


def parse(path: Path) -> list[Session]:
    sessions: list[Session] = []
    cur_session: Session | None = None
    cur_tick: Tick | None = None

    # Sessions are bounded by TICK 0 entries (Brain.open). Each session
    # has 2 TICK 0 entries (one per bot), then each tick number from 1
    # upward. New session = TICK 0 right after a regular tick (>0). We
    # also start a new session on the very first TICK 0 we see.
    last_tick_n: int | None = None
    in_tick_zero_run = False  # currently consuming the bot-pair TICK 0 block

    with path.open("r", encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            m = TICK_HDR_RE.match(line)
            if m:
                tn = int(m.group(1))
                if tn == 0:
                    # Start a new session unless we're still in the same
                    # TICK 0 run (e.g., bot 2's Brain.open right after bot 1).
                    if not in_tick_zero_run:
                        cur_session = Session(start_line=lineno)
                        sessions.append(cur_session)
                        in_tick_zero_run = True
                else:
                    in_tick_zero_run = False
                if cur_session is None:
                    cur_session = Session(start_line=lineno)
                    sessions.append(cur_session)
                cur_tick = Tick(n=tn, line_start=lineno)
                cur_session.ticks.append(cur_tick)
                last_tick_n = tn
                continue

            if cur_tick is None:
                continue

            # END tick=N total=X
            m = END_TICK_RE.search(line)
            if m:
                cur_tick.total_ms = float(m.group(2))
                continue

            # TICK TOTAL X ms (per-bot total — same as END but earlier in block)
            m = TICK_TOTAL_RE.search(line)
            if m and cur_tick.total_ms is None:
                cur_tick.total_ms = float(m.group(1))

            # Phase timings: "<phase> done <ms> ms"
            m = PHASE_LINE_RE.search(line)
            if m:
                phase = m.group(2).strip()
                # collapse names like "  W.update done" → "W.update"
                cur_tick.phases[phase] = float(m.group(3))
                continue

            # Diagnostic lines
            m = DIAG_SLOW_CAND.search(line)
            if m:
                cur_tick.slow_cands.append({
                    "pool":  int(m.group(1)),
                    "id":    m.group(2),
                    "total": float(m.group(3)),
                    "raw":   float(m.group(4)),
                    "adj":   float(m.group(5)),
                    "smart": float(m.group(6)),
                    "cost":  float(m.group(7)),
                    "mx":    int(m.group(8)),
                    "my":    int(m.group(9)),
                    "hp":    m.group(10),
                })
                continue

            m = DIAG_POOL6_CAND.search(line)
            if m:
                cur_tick.pool6_cands.append({
                    "id":             m.group(1),
                    "diff":           float(m.group(2)),
                    "spot":           float(m.group(3)),
                    "just_evaluated": m.group(4) == "true",
                    "force_detailed": m.group(5) == "true",
                })
                continue

            m = DIAG_THREAT.search(line)
            if m:
                cur_tick.threat_pill_count = int(m.group(1))
                cur_tick.threat_clear_ms   = float(m.group(2))
                cur_tick.threat_stamp_ms   = float(m.group(3))
                continue

            m = DIAG_OCCL.search(line)
            if m:
                cur_tick.threat_occl_ms = float(m.group(1))
                continue

    return [s for s in sessions if s.ticks]


def percentile(xs: list[float], p: float) -> float:
    if not xs:
        return 0.0
    xs2 = sorted(xs)
    k = max(0, min(len(xs2) - 1, int(round((p / 100) * (len(xs2) - 1)))))
    return xs2[k]


def fmt_ms(x: float | None) -> str:
    return "  -  " if x is None else f"{x:5.2f}"


def hist(values: list[float], bins: list[float], label_fmt: str = "{:>6.2f}") -> str:
    """Tiny ASCII histogram. bins = right-edges; values <= bin go in that bin."""
    if not values:
        return "(no samples)"
    counts = [0] * len(bins)
    overflow = 0
    for v in values:
        placed = False
        for i, b in enumerate(bins):
            if v <= b:
                counts[i] += 1
                placed = True
                break
        if not placed:
            overflow += 1
    total = len(values)
    width = 30
    lines = []
    for i, b in enumerate(bins):
        bar = "#" * int(round(counts[i] * width / total)) if total else ""
        lines.append(f"  <={label_fmt.format(b)}  {counts[i]:5}  {bar}")
    if overflow:
        bar = "#" * int(round(overflow * width / total))
        lines.append(f"   >{label_fmt.format(bins[-1])}  {overflow:5}  {bar}")
    return "\n".join(lines)


def report_session(s: Session) -> None:
    ticks = s.ticks
    if not ticks:
        return
    print(f"\n========== Session starting at line {s.start_line}: "
          f"{len(ticks)} bot-ticks, max tick={max(t.n for t in ticks)} ==========")

    # Per-bot totals
    totals = [t.total_ms for t in ticks if t.total_ms is not None]
    if totals:
        print(f"\nTICK TOTAL: count={len(totals)}  "
              f"max={max(totals):.2f}  mean={statistics.mean(totals):.2f}  "
              f"p50={percentile(totals,50):.2f}  p95={percentile(totals,95):.2f}  "
              f"p99={percentile(totals,99):.2f}  min={min(totals):.2f}")
        # Worst 10 ticks
        worst = sorted(((t.total_ms, t.n) for t in ticks if t.total_ms is not None),
                       reverse=True)[:10]
        print("  Worst 10 ticks:")
        for ms, n in worst:
            print(f"    tick {n:5}: {ms:6.2f} ms")

    # Phase timing summary (only those that appear)
    phase_totals: dict[str, list[float]] = defaultdict(list)
    for t in ticks:
        for ph, ms in t.phases.items():
            phase_totals[ph].append(ms)
    if phase_totals:
        print("\nPhase timings (ms): worst-10 mean of each phase, sorted by p95")
        rows = []
        for ph, vals in phase_totals.items():
            rows.append((ph, percentile(vals, 95), max(vals),
                         statistics.mean(vals), len(vals)))
        rows.sort(key=lambda r: r[1], reverse=True)
        print(f"    {'phase':<32} {'p95':>7} {'max':>7} {'mean':>7} {'count':>7}")
        for ph, p95, mx, mean, n in rows[:20]:
            print(f"    {ph:<32} {p95:7.2f} {mx:7.2f} {mean:7.2f} {n:7}")

    # Threat rebuild diagnostics
    rebuilds = [t for t in ticks if t.threat_pill_count is not None]
    if rebuilds:
        clears = [t.threat_clear_ms for t in rebuilds if t.threat_clear_ms is not None]
        stamps = [t.threat_stamp_ms for t in rebuilds if t.threat_stamp_ms is not None]
        occls  = [t.threat_occl_ms  for t in rebuilds if t.threat_occl_ms  is not None]
        print(f"\nThreat REBUILD events: {len(rebuilds)}")
        print(f"  pills/build: max={max(t.threat_pill_count for t in rebuilds)}  "
              f"mean={statistics.mean(t.threat_pill_count for t in rebuilds):.1f}")
        if stamps:
            print(f"  stamp:  max={max(stamps):.2f}  mean={statistics.mean(stamps):.2f}")
        if occls:
            print(f"  occl:   max={max(occls):.2f}  mean={statistics.mean(occls):.2f}")
        if clears:
            print(f"  clear:  max={max(clears):.2f}  mean={statistics.mean(clears):.2f}")

    # Pool 6 cache misses
    p6 = [c for t in ticks for c in t.pool6_cands]
    if p6:
        misses = [c for c in p6 if c["just_evaluated"]]
        hits   = [c for c in p6 if not c["just_evaluated"]]
        # Tick distribution of misses
        miss_by_tick_bucket: Counter[str] = Counter()
        for t in ticks:
            for c in t.pool6_cands:
                if c["just_evaluated"]:
                    if   t.n <  50:  miss_by_tick_bucket["t<50"]      += 1
                    elif t.n < 150:  miss_by_tick_bucket["50<=t<150"] += 1
                    elif t.n < 500:  miss_by_tick_bucket["150<=t<500"]+= 1
                    else:             miss_by_tick_bucket["t>=500"]   += 1
        print(f"\nPool 6 diagnostic ({len(p6)} entries; "
              f"misses={len(misses)} hits={len(hits)}, "
              f"force_detailed mostly={'true' if sum(c['force_detailed'] for c in p6) > len(p6)/2 else 'false'}):")
        for k in ("t<50", "50<=t<150", "150<=t<500", "t>=500"):
            print(f"  misses {k:<12} {miss_by_tick_bucket.get(k, 0)}")
        if misses:
            diffs = [c["diff"] for c in misses]
            spots = [c["spot"] for c in misses]
            print(f"  miss diff: max={max(diffs):.2f}  mean={statistics.mean(diffs):.2f}  "
                  f"p95={percentile(diffs,95):.2f}")
            print(f"  miss spot: max={max(spots):.2f}  mean={statistics.mean(spots):.2f}  "
                  f"p95={percentile(spots,95):.2f}")
            # Per-pill miss frequency (top 5)
            per_id: Counter[str] = Counter()
            for c in misses:
                per_id[c["id"]] += 1
            print("  Misses per pill id (top 8):")
            for pid, cnt in per_id.most_common(8):
                print(f"    id={pid:>4}  {cnt} misses")

    # Slow candidate breakdown
    sc = [c for t in ticks for c in t.slow_cands]
    if sc:
        print(f"\nSlow candidates (total>0.5ms): {len(sc)} entries")
        # Where does the time go (raw vs adj vs smart vs everything-else)?
        raws    = [c["raw"]   for c in sc]
        adjs    = [c["adj"]   for c in sc]
        smarts  = [c["smart"] for c in sc]
        totals_ = [c["total"] for c in sc]
        others  = [c["total"] - c["raw"] - c["adj"] - c["smart"] for c in sc]
        print(f"  raw    p95={percentile(raws,95):.2f}  mean={statistics.mean(raws):.2f}")
        print(f"  adj    p95={percentile(adjs,95):.2f}  mean={statistics.mean(adjs):.2f}")
        print(f"  smart  p95={percentile(smarts,95):.2f}  mean={statistics.mean(smarts):.2f}")
        print(f"  other  p95={percentile(others,95):.2f}  mean={statistics.mean(others):.2f}  "
              "<- downstream pool-specific work")
        print(f"  total  p95={percentile(totals_,95):.2f}  mean={statistics.mean(totals_):.2f}  "
              f"max={max(totals_):.2f}")

        # By pool
        per_pool: dict[int, list[float]] = defaultdict(list)
        for c in sc:
            per_pool[c["pool"]].append(c["total"])
        print("  By pool:")
        for pool, vals in sorted(per_pool.items()):
            print(f"    pool {pool}: count={len(vals):4} mean={statistics.mean(vals):5.2f} "
                  f"max={max(vals):5.2f} p95={percentile(vals,95):5.2f}")


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__,
                                  formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("path", nargs="?", default=str(DEFAULT_LOG),
                    help=f"optimize.log path (default: {DEFAULT_LOG})")
    ap.add_argument("--session", type=int, default=-1,
                    help="Session index (0=oldest, -1=latest, default: -1). "
                         "Use 'all' to report every session.")
    ap.add_argument("--all", action="store_true", help="Report all sessions.")
    args = ap.parse_args()

    p = Path(args.path)
    if not p.exists():
        print(f"error: {p} not found", file=sys.stderr)
        sys.exit(1)

    sessions = parse(p)
    if not sessions:
        print(f"No sessions parsed from {p}.")
        sys.exit(0)

    print(f"Found {len(sessions)} session(s) in {p}")
    print(f"  total bot-ticks across sessions: {sum(len(s.ticks) for s in sessions)}")

    if args.all:
        for s in sessions:
            report_session(s)
    else:
        idx = args.session if args.session >= 0 else len(sessions) + args.session
        if not (0 <= idx < len(sessions)):
            print(f"error: session index {args.session} out of range "
                  f"(0..{len(sessions) - 1})", file=sys.stderr)
            sys.exit(1)
        report_session(sessions[idx])


if __name__ == "__main__":
    main()
