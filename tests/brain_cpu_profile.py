#!/usr/bin/env python3
"""GoalHunter brain per-tick CPU profiler / measurement harness.

Runs WinBoloDS headless on DH-Oil Rig (2v2) for N ticks in a chosen mode,
then reports per-bot think-time distributions, the recorder's own phase
costs, and (when the mode enables the Lua phase profiler) a per-section
breakdown of where the brain's tick goes.

MODES (all: DH-Oil Rig, 2v2 GoalHunter_1.7, -seed/-brain-lua-seed/-brain-tier
pinned so every mode does identical work and only the debug output differs)

  prod            opt/ brain, BRAIN_DEBUG_MODE off, no Lua profiler.  This is
                  production.  Nothing in the build reports think time in this
                  configuration, so it needs the temporary [[GHPROBE]] snippet
                  (see "GROUND-TRUTH PROBE" below).
  nomark          like prod, but against brains/GH17_nomark/ — a copy of the
                  opt/ tree with the unconditional `opt(string.format(...))`
                  phase markers stripped.  prod - nomark = what those markers
                  cost in production.  Build it with:
                    cp -r brains/GoalHunter_1.7 brains/GH17_nomark
                    rm brains/GH17_nomark/opt/*.lua
                    build/Release/lua_strip.exe --strip "opt("                         brains/GH17_nomark/opt brains/GoalHunter_1.7/opt/*.lua
  prodprof        opt/ brain + -brain-profile-log: writes optimize.log and
                  performance.ticks.log (per-think phase sections, capacity
                  tier, alloc_n/alloc_kb).  The only per-phase breakdown the
                  build can produce — but BRAIN_PROFILE itself adds ~35 %, so
                  read the SHARES, not the absolute ms.
  debug           root brain, -brain-debug, all four debug streams on.  This
                  is what a -brain-debug recorded game costs.
  debug-noprint2  as debug, minus print2_bot<N>.log
  debug-nojsonl   as debug, minus brain_p<N>.jsonl / player<N>.jsonl
  debug-noviz     as debug, minus visualizer overlay emit + record
  debug-nopool    as debug, minus the pool-breakdown JSON capture
  debug-bare      as debug, all four off = root brain code with no streams.
                  debug minus debug-bare = total debug-stream cost;
                  debug minus debug-no<X> = stream X's cost.
  debugprof       BROKEN as a "profile the debug brain" mode: -brain-profile-log
                  splices opt/ into the brain path (servermain.c:2070-2095) and
                  wins, so this measures the same thing as prodprof.  There is
                  no way to get a per-phase breakdown of the ROOT brain.

GROUND-TRUTH PROBE (prod / nomark only)
  brain.lastThinkMs is published to Lua by the host every tick
  (luabrainshandler.c:1593, from bot_manager.c:909) and costs nothing to read.
  Insert this immediately after `function Brain.think(info)` in BOTH
  brains/GoalHunter_1.7/init.lua and brains/GoalHunter_1.7/opt/init.lua, run,
  then `git checkout` both files:

    do local _p = _G._GHP
       if _p == nil then
         _p = { n = 0 }; _G._GHP = _p
         _p.f = io.open("ghprobe_bot" .. tostring(info.player_number or 0)
                        .. ".txt", "w")
       end
       _p.n = _p.n + 1
       _p[_p.n] = (_G.brain and _G.brain.lastThinkMs) or 0
       if _p.n >= 500 and _p.f then
         _p.f:write(table.concat(_p, "
", 1, _p.n), "
")
         _p.f:flush(); _p.n = 0
       end
    end

  Append `.. "," .. collectgarbage("count")` to the stored value to also sample
  the LuaJIT heap; the parser keeps the ms column either way, and a heap DROP
  across a slow think identifies it as a GC pause rather than brain work.

USAGE
  py tests/brain_cpu_profile.py --mode debug --ticks 12000
  py tests/brain_cpu_profile.py --mode core  --ticks 12000 --tag B --keep
  py tests/brain_cpu_profile.py --mode all   --ticks 12000 --tag A
  py tests/brain_cpu_profile.py --report debug_sessions/<dir> --ticks 12000
  py tests/brain_cpu_profile.py --summary

  --ticks are GAME ticks; the server runs 2 per 50 Hz server tick, so 12000
  ticks = 6000 thinks per bot = ~120 s wall.  --keep retains the .btr and
  performance.ticks.log (~100 MB per 12000-tick prodprof run).

CONTENTION: every run records whether another WinBoloDS.exe / BrainTest.exe was
alive (tasklist) before and after, and prints it.  Medians move ~5-8 % between
a contended and a quiet machine; the >15 ms tail is almost entirely contention
and GC, so compare modes pairwise within one batch.

Run from the repo root of a worktree whose build/ points at the built
binaries.  Writes into ./debug_sessions/ and ./cpuprof_out/ under the cwd.
Nothing is written into the shared build tree.
"""

import argparse
import json
import os
import re
import shutil
import statistics
import subprocess
import sys
import time
from pathlib import Path

from asap import asap_args, asap_mode, pacing_line, take_asap_flag

ROOT = Path(__file__).resolve().parent.parent
DS = ROOT / "debug_sessions"
OUT = ROOT / "cpuprof_out"

MAP = "data/maps/DH-Oil Rig.map"
# WINBOLO_BRAIN_DIR lets a before/after comparison point the same harness at a
# pristine copy of the brain (e.g. brains/GH17_base) without editing this file.
# WINBOLO_DS_EXE does the same for the server binary when the C side changed and
# the build lives outside the shared build/ junction (e.g. build-wt).
BRAIN_DIR = os.environ.get("WINBOLO_BRAIN_DIR", "brains/GoalHunter_1.7")
DS_EXE = os.environ.get("WINBOLO_DS_EXE", "build/WinBoloDS.exe")
ROOT_BRAIN = BRAIN_DIR + "/init.lua"
OPT_BRAIN = BRAIN_DIR + "/opt/init.lua"

# Flags shared by every mode.  -brain-no-budget-kill + -brain-lua-seed give a
# deterministic run; it also pins the capacity tier at 10 (target_ms becomes
# 1000) so every mode measures the same worst-case work, not a self-throttled
# one.  -threads 4 matches the reference 2v2 recording.
BASE = [
    "-map", MAP,
    "-gametype", "tournament", "-ai", "yes", "-nolobby", "-quitonwin",
    "-notracker", "-nowinbolonet", "-dontsendlog", "-noinput",
    "-bots", "4", "-threads", "4",
    "-teams", "2,2",
    "-brain-no-budget-kill", "-brain-lua-seed", "42",
    "-brain-tier", "10",
    "-allow-unsafe-brains",
]
# -asap is now the DEFAULT (see tests/asap.py); asap_args() is spliced into the
# command line in run_mode().  It runs ticks back-to-back instead of one per
# 20 ms of wall clock.  It does not change what a think does (the -asap games
# are byte-identical by -snapjson), it just removes the idle time between
# ticks, so a 12000-tick run takes ~15 s instead of ~120 s.  That makes it
# practical to INTERLEAVE before/after runs, which matters: this machine drifts
# ~20% over an hour of back-to-back measurement, enough to swamp the effects
# being measured if A and B are an hour apart.
#
# --no-asap (or WINBOLO_ASAP=0) goes back to 20 ms pacing.  Do that when the
# number you want is "what does this cost in a REAL game" with the idle time
# in the denominator (e.g. eff_hz / the braindbg_perf ms-per-tick windows read
# against the 50 Hz budget); the per-think distributions themselves are
# pacing-independent, which is the whole point of measuring them.
# -brain-tier 10 is redundant with -brain-no-budget-kill (a 1000 ms budget
# already drives the derived tier to 10) but makes the pin explicit, so every
# mode does the SAME amount of work and the only difference is debug output.
# bot_manager.c:194-204 explains why an unpinned tier makes runs incomparable.

MODES = {
    #  name            brain        extra flags
    "prod":           (OPT_BRAIN,  []),
    "prodprof":       (ROOT_BRAIN, ["-brain-profile-log"]),
    "debug":          (ROOT_BRAIN, ["-brain-debug"]),
    "debug-noprint2": (ROOT_BRAIN, ["-brain-debug", "-bd-noprint2"]),
    "debug-nojsonl":  (ROOT_BRAIN, ["-brain-debug", "-bd-nojsonl"]),
    "debug-noviz":    (ROOT_BRAIN, ["-brain-debug", "-bd-noviz"]),
    "debug-nopool":   (ROOT_BRAIN, ["-brain-debug", "-bd-nopool"]),
    "debug-bare":     (ROOT_BRAIN, ["-brain-debug", "-bd-noprint2", "-bd-nojsonl",
                                    "-bd-noviz", "-bd-nopool"]),
    "debugprof":      (ROOT_BRAIN, ["-brain-debug", "-brain-profile-log",
                                    "-bd-noprint2", "-bd-nojsonl", "-bd-noviz",
                                    "-bd-nopool"]),
    # Ablation: opt/ brain with the ~112 unconditional `opt(string.format(..))`
    # phase-marker statements stripped out (build it with
    #   build/Release/lua_strip.exe --strip "opt(" brains/GH17_nomark/opt \
    #       brains/GoalHunter_1.7/opt/*.lua
    # after copying brains/GoalHunter_1.7 -> brains/GH17_nomark).
    # Measures what the profiler markers cost in PRODUCTION, where they format
    # their strings before optimize.lua's BRAIN_PROFILE check discards them.
    "nomark":         ("brains/GH17_nomark/init.lua", []),
}
# -brain-profile-log rewrites the brain path to opt/ itself unless -brain-debug
# also asks for the base path, so "prodprof" passes the root path on purpose.

ORDER = ["prod", "nomark", "prodprof", "debug", "debug-bare", "debug-noprint2",
         "debug-nojsonl", "debug-noviz", "debug-nopool", "debugprof"]
CORE = ["prod", "nomark", "prodprof", "debug", "debug-bare"]


# ---------------------------------------------------------------- contention

def other_winbolo_procs():
    """PIDs of WinBoloDS/BrainTest processes not started by us."""
    try:
        out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"],
                             capture_output=True, text=True, timeout=30).stdout
    except Exception:
        return []
    hits = []
    for line in out.splitlines():
        low = line.lower()
        if "winbolods.exe" in low or "braintest.exe" in low:
            hits.append(line.split(",")[0].strip('"') + " " + line.split(",")[1].strip('"'))
    return hits


# ------------------------------------------------------------------- running

def run_mode(mode, ticks, seed, port, tag):
    brain, extra = MODES[mode]
    label = f"{tag}_{mode}".replace("-", "")
    env = dict(os.environ)
    env["WINBOLO_BRAINDBG_LABEL"] = label

    before = {p.name for p in DS.iterdir()} if DS.is_dir() else set()
    # Brain.open runs before server_lifecycle publishes DEBUG_SESSION_DIR, so
    # metrics.open_files lands player0_ticks.log / player0_metrics.log (and a
    # stray optimize.log) in the CWD rather than the session dir.  Clear them
    # so what we collect afterwards belongs to this run only.
    for pat in ("ghprobe_bot*.txt", "player*_ticks.log", "player*_metrics.log",
                "optimize.log"):
        for f in ROOT.glob(pat):
            f.unlink()

    cmd = [str(ROOT / DS_EXE)] + BASE + asap_args() + [
        "-brain", brain, "-port", str(port), "-seed", str(seed),
        "-ticks", str(ticks)] + extra

    contention_before = other_winbolo_procs()
    t0 = time.time()
    proc = subprocess.run(cmd, cwd=str(ROOT), env=env,
                          capture_output=True, text=True)
    wall = time.time() - t0
    contention_after = other_winbolo_procs()

    after = {p.name for p in DS.iterdir()} if DS.is_dir() else set()
    new = sorted(after - before)
    sess = DS / new[-1] if new else None

    OUT.mkdir(exist_ok=True)
    probe = {}
    for f in sorted(ROOT.glob("ghprobe_bot*.txt")):
        bot = int(re.search(r"bot(\d+)", f.name).group(1))
        # Probe lines are "<ms>" or, when the heap sampler is enabled,
        # "<ms>,<gc_heap_kb>".  Keep the ms column either way.
        vals = [float(x.split(",")[0]) for x in f.read_text().split() if x]
        probe[bot] = vals
        dest = OUT / f"{tag}_{mode}_probe_bot{bot}.txt"
        shutil.move(str(f), str(dest))

    # player0_ticks.log = "<tick>\t<us_total>" straight from the brain's own
    # clock_us pair (init.lua us_total).  Finest-grained series available and
    # it survives into opt/, so keep it beside the probe.
    us_total = []
    pt = ROOT / "player0_ticks.log"
    if pt.is_file():
        for line in pt.read_text().splitlines():
            p = line.split("\t")
            if len(p) == 2:
                try:
                    us_total.append((int(p[0]), float(p[1])))
                except ValueError:
                    pass
        shutil.move(str(pt), str(OUT / f"{tag}_{mode}_player0_ticks.log"))
    for pat in ("player*_metrics.log", "optimize.log"):
        for f in ROOT.glob(pat):
            f.unlink()

    meta = {
        "mode": mode, "ticks": ticks, "seed": seed, "wall_s": round(wall, 1),
        "pacing": asap_mode(),
        "session": sess.name if sess else None,
        "cmd": " ".join(cmd[1:]),
        "contention_before": contention_before,
        "contention_after": contention_after,
        "exit": proc.returncode,
        "us_total_n": len(us_total),
        "stderr_tail": proc.stderr.splitlines()[-6:],
    }
    (OUT / f"{tag}_{mode}_meta.json").write_text(json.dumps(meta, indent=2))
    return sess, probe, meta, us_total


# ------------------------------------------------------------------ parsing

def pct(vals, p):
    if not vals:
        return None
    s = sorted(vals)
    i = min(len(s) - 1, int(round((p / 100.0) * (len(s) - 1))))
    return s[i]


def dist(vals):
    if not vals:
        return None
    return {
        "n": len(vals),
        "mean": round(statistics.fmean(vals), 3),
        "p50": round(pct(vals, 50), 3),
        "p90": round(pct(vals, 90), 3),
        "p99": round(pct(vals, 99), 3),
        "max": round(max(vals), 3),
    }


def parse_perf(sess):
    """braindbg_perf.log -> list of dicts (one per 250-tick window)."""
    f = sess / "braindbg_perf.log"
    if not f.is_file():
        return []
    rows = []
    for line in f.read_text().splitlines():
        if not line.startswith("PERF "):
            continue
        d = {}
        for kv in line.split()[1:]:
            k, _, v = kv.partition("=")
            d[k] = float(v)
        rows.append(d)
    return rows


def parse_ticks(sess, warmup=200):
    """performance.ticks.log -> per-bot think_total_ms, alloc, sections."""
    f = sess / "performance.ticks.log"
    if not f.is_file():
        return None
    per_bot = {}
    sections = {}       # name -> list of ms (summed over bots per tick record)
    subs = {}           # "parent/child" -> list of ms
    tickinfo = []       # (think_total_ms, tick, bot, top section, tier)
    for line in f.open(encoding="utf-8", errors="replace"):
        line = line.strip()
        if not line.startswith("{"):
            continue
        try:
            r = json.loads(line)
        except Exception:
            continue
        if r.get("tick", 0) < warmup:
            continue
        bot = r["bot"]
        d = r["data"]
        tot = d.get("think_total_ms")
        b = per_bot.setdefault(bot, {"think": [], "alloc_n": [], "alloc_kb": [],
                                     "tier": [], "last_ms": []})
        if tot is not None:
            b["think"].append(tot)
        for k in ("alloc_n", "alloc_kb", "tier", "last_ms"):
            if d.get(k) is not None:
                b[k].append(d[k])
        top = None
        for s in d.get("sections", []) or []:
            sections.setdefault(s["name"], []).append(s.get("ms") or 0.0)
            if top is None or (s.get("ms") or 0) > (top[1] or 0):
                top = (s["name"], s.get("ms") or 0)
            for sub in s.get("subs", []) or []:
                subs.setdefault(s["name"] + "/" + sub["name"], []).append(sub.get("ms") or 0.0)
        tickinfo.append((tot or 0.0, r.get("tick"), bot,
                         top[0] if top else "?", d.get("tier")))
    return {"per_bot": per_bot, "sections": sections, "subs": subs,
            "ticks": tickinfo}


SLOWPAT = re.compile(r"^\s*\[diag\]\s+(.*)$")


def parse_optimize(sess, limit=40):
    """optimize.log -> counts of each [diag] line shape."""
    f = sess / "optimize.log"
    if not f.is_file():
        return {}
    counts = {}
    for line in f.open(encoding="utf-8", errors="replace"):
        m = SLOWPAT.match(line)
        if not m:
            continue
        # collapse numbers so line shapes group
        shape = re.sub(r"-?\d+\.?\d*", "#", m.group(1))
        counts[shape] = counts.get(shape, 0) + 1
    return dict(sorted(counts.items(), key=lambda kv: -kv[1])[:limit])


def parse_session_sizes(sess):
    out = {}
    for f in sorted(sess.iterdir()):
        if f.is_file():
            out[f.name] = f.stat().st_size
    return out


# ------------------------------------------------------------------ report

def report(sess, probe, meta, warmup=200, topn=8, us_total=None):
    print("=" * 78)
    print(f"MODE {meta['mode']}   ticks={meta['ticks']} seed={meta['seed']} "
          f"wall={meta['wall_s']}s pacing={meta.get('pacing', '?')}")
    print(f"session: {sess.name if sess else '(none)'}")
    if meta["contention_before"] or meta["contention_after"]:
        print("CONTENTION: other WinBolo processes were running:")
        for p in sorted(set(meta["contention_before"] + meta["contention_after"])):
            print("   ", p)
    elif meta.get("contention_checked", True):
        print("CONTENTION: none (no other WinBoloDS.exe / BrainTest.exe)")
    else:
        print("CONTENTION: unknown (--report re-parses an old session; the "
              "contention state was recorded in its *_meta.json)")

    if probe:
        print("\n-- ground-truth per-think wall ms (host lastThinkMs, [[GHPROBE]]) --")
        print(f"{'bot':>4} {'n':>7} {'mean':>7} {'p50':>7} {'p90':>7} {'p99':>7} {'max':>8}")
        allv = []
        for bot in sorted(probe):
            v = [x for x in probe[bot][warmup // 2:] if x > 0]
            allv += v
            d = dist(v)
            if d:
                print(f"{bot:>4} {d['n']:>7} {d['mean']:>7.2f} {d['p50']:>7.2f} "
                      f"{d['p90']:>7.2f} {d['p99']:>7.2f} {d['max']:>8.2f}")
        d = dist(allv)
        if d:
            print(f"{'ALL':>4} {d['n']:>7} {d['mean']:>7.2f} {d['p50']:>7.2f} "
                  f"{d['p90']:>7.2f} {d['p99']:>7.2f} {d['max']:>8.2f}")

    if us_total:
        v = [us / 1000.0 for tk, us in us_total if tk >= warmup]
        d = dist(v)
        if d:
            print()
            print("-- player0_ticks.log: brain-internal us_total (bot 0, ms) --")
            print(f"   n={d['n']} mean={d['mean']:.3f} p50={d['p50']:.3f} "
                  f"p90={d['p90']:.3f} p99={d['p99']:.3f} max={d['max']:.3f}")

    if sess:
        rows = [r for r in parse_perf(sess) if r["t"] >= warmup]
        if rows:
            print("\n-- braindbg_perf.log (250-tick windows, ms/tick) --")
            for key in ("think_ms", "rec_snap", "rec_map", "rec_ovl", "rec_pool"):
                v = [r[key] for r in rows]
                print(f"  {key:<9} mean={statistics.fmean(v):7.3f}  "
                      f"p50={pct(v,50):7.3f}  max={max(v):7.3f}")
            ov = [r["ovl_cmds"] for r in rows]
            pb = [r["pool_B"] for r in rows]
            hz = [r["eff_hz"] for r in rows]
            print(f"  ovl_cmds  mean={statistics.fmean(ov):7.0f}/tick   "
                  f"pool_B mean={statistics.fmean(pb):7.0f}/tick")
            print(f"  eff_hz    mean={statistics.fmean(hz):7.2f}  min={min(hz):7.2f}"
                  f"   (50.0 = server keeping up)")

        t = parse_ticks(sess, warmup)
        if t and t["per_bot"]:
            print("\n-- performance.ticks.log: brain-measured think_total_ms/think --")
            print(f"{'bot':>4} {'n':>7} {'mean':>7} {'p50':>7} {'p90':>7} {'p99':>7} "
                  f"{'max':>8} {'tier':>5} {'allocKB':>8}")
            for bot in sorted(t["per_bot"]):
                b = t["per_bot"][bot]
                d = dist(b["think"])
                tier = statistics.fmean(b["tier"]) if b["tier"] else 0
                akb = statistics.fmean(b["alloc_kb"]) if b["alloc_kb"] else 0
                if d:
                    print(f"{bot:>4} {d['n']:>7} {d['mean']:>7.3f} {d['p50']:>7.3f} "
                          f"{d['p90']:>7.3f} {d['p99']:>7.3f} {d['max']:>8.3f} "
                          f"{tier:>5.1f} {akb:>8.1f}")

            print("\n-- phase sections (ms per think; mean / p90 / share) --")
            tot = sum(statistics.fmean(v) for v in t["sections"].values()) or 1.0
            rows_ = sorted(t["sections"].items(),
                           key=lambda kv: -statistics.fmean(kv[1]))
            for name, v in rows_:
                m = statistics.fmean(v)
                print(f"  {name:<22} mean={m:7.3f}  p90={pct(v,90):7.3f}  "
                      f"p99={pct(v,99):7.3f}  max={max(v):7.3f}  {100*m/tot:5.1f}%")
            print(f"  {'TOTAL(sections)':<22} mean={tot:7.3f}")

            print("\n-- top sub-sections --")
            subs = sorted(t["subs"].items(),
                          key=lambda kv: -statistics.fmean(kv[1]))[:18]
            for name, v in subs:
                print(f"  {name:<40} mean={statistics.fmean(v):7.3f}  "
                      f"p99={pct(v,99):7.3f}  max={max(v):7.3f}")

            print(f"\n-- {topn} slowest thinks (think_total_ms, dominant section) --")
            for tot_ms, tick, bot, top, tier in sorted(t["ticks"], reverse=True)[:topn]:
                print(f"  {tot_ms:8.3f} ms  tick={tick:<7} bot={bot} tier={tier} "
                      f"dominant={top}")

        diag = parse_optimize(sess)
        if diag:
            print("\n-- optimize.log [diag] line shapes (count) --")
            for shape, n in list(diag.items())[:12]:
                print(f"  {n:>7}  {shape[:90]}")

        sizes = parse_session_sizes(sess)
        if sizes:
            print("\n-- recorded bytes (session dir) --")
            tks = meta["ticks"]
            for name, sz in sorted(sizes.items(), key=lambda kv: -kv[1]):
                print(f"  {name:<28} {sz:>13,} B   {sz/max(tks,1):8.1f} B/tick")
    print()


def summary(tag=None):
    """One line per completed run: probe p50/p90 and perf think_ms."""
    metas = sorted(OUT.glob("*_meta.json"))
    print(f"{'run':<28} {'probe p50':>10} {'p90':>7} {'p99':>7} {'max':>8} "
          f"{'perf think':>11} {'ovl/t':>7} {'poolB/t':>9} {'contend':>8}")
    for m in metas:
        meta = json.loads(m.read_text())
        if tag and not m.name.startswith(tag):
            continue
        name = m.name[: -len("_meta.json")]
        vals = []
        for pf in OUT.glob(name + "_probe_bot*.txt"):
            vals += [float(x) for x in pf.read_text().split() if x and float(x) > 0][100:]
        d = dist(vals)
        think = ovl = poolb = None
        if meta.get("session"):
            rows = [r for r in parse_perf(DS / meta["session"]) if r["t"] >= 200]
            if rows:
                think = statistics.fmean([r["think_ms"] for r in rows])
                ovl = statistics.fmean([r["ovl_cmds"] for r in rows])
                poolb = statistics.fmean([r["pool_B"] for r in rows])
        cont = "yes" if (meta["contention_before"] or meta["contention_after"]) else "no"
        print(f"{name:<28} "
              f"{(f'{d[chr(112)+chr(53)+chr(48)]:.2f}' if d else '-'):>10} "
              f"{(f'{d[chr(112)+chr(57)+chr(48)]:.2f}' if d else '-'):>7} "
              f"{(f'{d[chr(112)+chr(57)+chr(57)]:.2f}' if d else '-'):>7} "
              f"{(f'{d[chr(109)+chr(97)+chr(120)]:.2f}' if d else '-'):>8} "
              f"{(f'{think:.2f}' if think else '-'):>11} "
              f"{(f'{ovl:.0f}' if ovl else '-'):>7} "
              f"{(f'{poolb:.0f}' if poolb else '-'):>9} "
              f"{cont:>8}")


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--mode", default="debug",
                    help="one of " + ", ".join(ORDER) + ", or 'all'")
    ap.add_argument("--ticks", type=int, default=6000)
    ap.add_argument("--seed", type=int, default=4242)
    ap.add_argument("--port", type=int, default=27801)
    ap.add_argument("--tag", default="run")
    ap.add_argument("--warmup", type=int, default=400,
                    help="ignore ticks below this (map exploration / JIT warmup)")
    ap.add_argument("--report", help="parse an existing session dir and exit")
    ap.add_argument("--summary", action="store_true")
    ap.add_argument("--keep", action="store_true",
                    help="keep the .btr / big streams (default: delete them)")
    ap.add_argument("--no-asap", dest="no_asap", action="store_true",
                    help="run the server at the live 20 ms/tick pacing instead "
                         "of -asap (same as WINBOLO_ASAP=0)")
    ap.add_argument("--asap", dest="force_asap", action="store_true",
                    help="force -asap on even under WINBOLO_ASAP=0 (the default)")
    a = ap.parse_args()
    # Feed the two switches through the shared resolver so the CLI beats the
    # WINBOLO_ASAP environment variable exactly as it does in the scenario tests.
    take_asap_flag((["--no-asap"] if a.no_asap else [])
                   + (["--asap"] if a.force_asap else []))
    print(pacing_line(""))

    OUT.mkdir(exist_ok=True)

    if a.summary:
        summary()
        return
    if a.report:
        sess = Path(a.report)
        meta = {"mode": "(reported)", "ticks": a.ticks, "seed": a.seed,
                "wall_s": 0, "pacing": "(from the run's *_meta.json)",
                "contention_before": [], "contention_after": [],
                "contention_checked": False}
        report(sess, {}, meta, a.warmup)
        return

    modes = (ORDER if a.mode == "all"
             else CORE if a.mode == "core"
             else [a.mode])
    for i, mode in enumerate(modes):
        if mode not in MODES:
            sys.exit("unknown mode " + mode)
        sess, probe, meta, us_total = run_mode(mode, a.ticks, a.seed, a.port + i, a.tag)
        report(sess, probe, meta, a.warmup, us_total=us_total)
        sys.stdout.flush()
        if sess and not a.keep:
            # the .btr and the jsonl streams are gigabytes; the numbers we
            # need are already parsed out.  Keep the small logs.
            for f in sess.iterdir():
                if f.suffix in (".btr", ".jsonl") or f.name.endswith("ticks.log"):
                    f.unlink()


if __name__ == "__main__":
    main()
