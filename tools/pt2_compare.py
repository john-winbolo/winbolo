#!/usr/bin/env python3
"""Summarise pill take v1 vs v2 side-by-side lines (PT2_CMP) from print2 logs.

The GoalHunter brain writes one PT2_CMP line each time v1's shield scan runs
on a debug run (constants PT2_SIDE_BY_SIDE_LOG). This script reads one or more
print2.log files, or directories that hold them, and prints:

  * how many takes were compared, and the agreement rate (diff=same);
  * a count of each kind of difference (so, chain, none_v1, none_v2, none_both);
  * the reasons v2 gives for rejecting v1's point (why=...);
  * v2's cost in microseconds (mean, median, 95th percentile, max).

Usage:
  python tools/pt2_compare.py <print2.log or dir> [...] [--rows]

--rows also prints each compared line in a short table.
"""
import os
import re
import sys
import statistics

FIELD = re.compile(r"(\w+)=(\S+)")


def find_logs(paths):
    for p in paths:
        if os.path.isdir(p):
            for root, _dirs, files in os.walk(p):
                for f in files:
                    if f.startswith("print2") and f.endswith(".log"):
                        yield os.path.join(root, f)
        else:
            yield p


def parse_line(line):
    i = line.find("PT2_CMP ")
    if i < 0:
        return None
    body = line[i:].rstrip("\r\n")
    parts = body.split(" | ")
    rec = {"raw": body}
    for name, part in zip(("head", "v1", "v2", "tail"), parts):
        d = {}
        for k, v in FIELD.findall(part):
            d[k] = v
        rec[name] = d
    return rec


def pct(values, q):
    if not values:
        return 0
    s = sorted(values)
    k = min(len(s) - 1, int(round(q * (len(s) - 1))))
    return s[k]


def main(argv):
    rows = "--rows" in argv
    paths = [a for a in argv if a != "--rows"]
    if not paths:
        print(__doc__)
        return 2
    recs = []
    for f in find_logs(paths):
        with open(f, encoding="utf-8", errors="replace") as fh:
            for line in fh:
                r = parse_line(line)
                if r:
                    r["file"] = f
                    recs.append(r)
    if not recs:
        print("no PT2_CMP lines found")
        return 1
    diffs, whys, costs = {}, {}, []
    for r in recs:
        t = r.get("tail", {})
        diffs[t.get("diff", "?")] = diffs.get(t.get("diff", "?"), 0) + 1
        why = t.get("why", "-")
        whys[why.split(":")[0]] = whys.get(why.split(":")[0], 0) + 1
        try:
            costs.append(int(t.get("cost_us", "0")))
        except ValueError:
            pass
    n = len(recs)
    same = diffs.get("same", 0)
    print("compared takes: %d" % n)
    print("agreement (diff=same): %d/%d = %.1f%%" % (same, n, 100.0 * same / n))
    print("differences:")
    for k in sorted(diffs, key=lambda k: -diffs[k]):
        print("  %-10s %d" % (k, diffs[k]))
    print("v2 verdict on v1's point (why):")
    for k in sorted(whys, key=lambda k: -whys[k]):
        print("  %-16s %d" % (k, whys[k]))
    if costs:
        print("v2 cost us: mean %.0f  median %.0f  p95 %d  max %d" % (
            statistics.mean(costs), statistics.median(costs), pct(costs, 0.95), max(costs)))
    if rows:
        print()
        print("%-8s %-10s %-16s %-16s %-6s %-6s %-6s %-10s %s" % (
            "tick", "pill", "v1 so", "v2 so", "v1cov", "v2have", "kills", "diff", "why"))
        for r in recs:
            h, a, b, t = r["head"], r["v1"], r["v2"], r["tail"]
            print("%-8s %-10s %-16s %-16s %-6s %-6s %-6s %-10s %s" % (
                h.get("t", "?"), h.get("pill", "?"), a.get("so", "-"), b.get("so", "-"),
                a.get("cover", "-"), b.get("have", "-"), b.get("kills", "-"),
                t.get("diff", "?"), t.get("why", "-")))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
