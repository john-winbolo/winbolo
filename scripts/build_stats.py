#!/usr/bin/env python3
"""Post-match aggregates for the GoalHunter build design.

    python scripts/build_stats.py build/debug_sessions/<session> [bot ...]

Reads print2_bot<N>.log (-brain-debug). Per bot and totals:
  * panic builds, and how many fired with NO enemy within 15, NO shell within
    3 and NO angry pill covering us (should be 0 -- the bug the design fixes)
  * offensive builds, vetoes (a veto with both distances > 8 = veto broken)
  * dispatches by trigger reason (vuln / imdanger / coverage / close)
  * give-ups by reason
  * spacing class at placement time (clear should dominate, orthogonal rare)
  * placement trips: set/ended by reason; harvest set / resumed / dropped by reason
"""
import re, sys, glob, os
from collections import Counter, defaultdict

RE_T = re.compile(r"\bt=(\d+)")

def parse(path):
    st = defaultdict(Counter)
    last_scores = {}          # tick -> dict of imdanger facts
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            msg = line.rstrip("\n").split("\t")[-1]
            msg = re.sub(r"^\[[\d.]+ms\] ", "", msg)
            m = RE_T.search(msg)
            t = int(m.group(1)) if m else None
            if msg.startswith("       imdanger="):
                # odds(... ; Ne Na Nskip), shells{..}(N near), expo{..}(pill_at=X)
                sh = re.search(r"shells\{[^}]*\}\((\d+) near\)", msg)
                od = re.search(r"; (\d+)e (\d+)a (\d+)skip", msg)
                ex = re.search(r"expo\{([+-][\d.]+)\}", msg)
                last_scores["cur"] = {
                    "shells": int(sh.group(1)) if sh else 0,
                    "enemies": int(od.group(1)) if od else 0,
                    "expo": float(ex.group(1)) if ex else 0.0,
                }
            elif msg.startswith("BUILD_TRIGGER"):
                kind = re.search(r"kind=(\w+)", msg).group(1)
                st["build_trigger"][kind] += 1
                if kind == "panic":
                    s = last_scores.get("cur", {})
                    if s.get("enemies", 0) == 0 and s.get("shells", 0) == 0 and s.get("expo", 0.0) >= 0.0:
                        st["panic_no_threat"]["count"] += 1
                        st["panic_no_threat_ticks"][t] += 1
            elif msg.startswith("BUILD_VETO"):
                st["veto"]["offensive"] += 1
                ds = float(re.search(r"d_self=([\d.]+)", msg).group(1))
                de = float(re.search(r"d_enemy=([\d.]+)", msg).group(1))
                if ds > 8 or de > 8:
                    st["veto_out_of_range"]["count"] += 1
            elif msg.startswith("DISPATCH t="):
                st["dispatch"][re.search(r"reason=(\w+)", msg).group(1)] += 1
            elif msg.startswith("PLACE_GIVEUP"):
                st["giveup"][re.search(r"reason=(\w+)", msg).group(1)] += 1
            elif msg.startswith("SPACING"):
                cls = re.search(r"class=(\w+)", msg).group(1)
                src = "emergency" if "(emergency search)" in msg else "guard_spot"
                st["spacing_" + src][cls] += 1
            elif msg.startswith("HARVEST_SET"):
                st["harvest"]["set"] += 1
            elif msg.startswith("HARVEST_RESUME"):
                st["harvest"]["resume"] += 1
            elif msg.startswith("HARVEST_DROP"):
                st["harvest_drop"][re.search(r"reason=(\w+)", msg).group(1)] += 1
            elif msg.startswith("PLACE_TRIP_SET"):
                st["trip_set"][re.search(r"origin=(\w+)", msg).group(1)] += 1
            elif msg.startswith("PLACE_TRIP_END"):
                st["trip_end"][re.search(r"reason=(\w+)", msg).group(1)] += 1
    return st

def show(name, st):
    print(f"== {name}")
    def row(label, c):
        if not c: print(f"  {label:<22} -"); return
        print(f"  {label:<22} " + "  ".join(f"{k}={v}" for k, v in sorted(c.items(), key=lambda kv: (-kv[1], str(kv[0])))))
    row("build triggers", st["build_trigger"])
    n = st["panic_no_threat"]["count"]
    print(f"  {'panic w/ no threat':<22} {n}" + (f"  ticks={sorted(st['panic_no_threat_ticks'])[:10]}" if n else "   (0 = good)"))
    row("offensive vetoes", st["veto"])
    print(f"  {'veto out of range':<22} {st['veto_out_of_range']['count']}   (0 = good)")
    row("dispatch by trigger", st["dispatch"])
    row("give-ups", st["giveup"])
    row("spacing guard-spot", st["spacing_guard_spot"])
    row("spacing emergency", st["spacing_emergency"])
    row("trips set (origin)", st["trip_set"])
    row("trips ended", st["trip_end"])
    row("harvest", st["harvest"])
    row("harvest drops", st["harvest_drop"])

def main():
    if len(sys.argv) < 2:
        print(__doc__); sys.exit(1)
    d = sys.argv[1]
    bots = sys.argv[2:]
    logs = sorted(glob.glob(os.path.join(d, "print2_bot*.log")))
    if bots:
        logs = [l for l in logs if re.search(r"bot(\d+)\.log$", l).group(1) in bots]
    total = defaultdict(Counter)
    for l in logs:
        st = parse(l)
        show(os.path.basename(l), st)
        for k, c in st.items():
            total[k].update(c)
    if len(logs) > 1:
        show("TOTAL", total)

if __name__ == "__main__":
    main()
