#!/usr/bin/env python3
"""Read the pill take v2 doll cache event log (PT2_V2_DOLL_EVENT_LOG).

Each bot writes <debug session dir>/dl_p<player>.bin: 48-byte records, one
per store lookup (ray, filter answer, ring point, plan), per map or pill
change a sync saw, per ring (hostile pill X) state at a sync, per sync and
per job reset. Log only: the bot does the same with the knob off.

Usage:
  dl_events.py summary <dir or files>       live table per doll level and source
  dl_events.py list <files> [filters]       one line per record
  dl_events.py counts <files> [filters]     counts by level / result / reason
  dl_events.py table <dir> --out live       live.md, live.json, live.png (model vs live)
  dl_events.py at <session dir> --player N --tick T [--pill K]
      what the BrainTest doll layers (pt2_d1..pt2_d5, pt2_dchg) draw at tick T
      for bot N: the same records pt2_viz.lua takes (the last run of each
      source, the last sync), counts per result, and per not-kept record the
      reason, the change behind it and its distance from the goal pill and the
      pills X. T is the BrainTest HUD "Tick" (the brain tick, engine tick / 2).
      K is the pill_id_label pill number (from the run's snap.jsonl, else the
      bot's brain log); with K also the pill's earlier evaluates, the last one
      all kept, and the changes since.
Filters: --tick A:B  --player N  --src take|dbl  --lvl ray|flt|pt|pl|ring|chg|pill|sync|reset
         --res hit|hitrun|drop|newkey|cold|first|tank|gone  --pill SQ (goal pill square)
         --x SQ (hostile pill X square)  --sq SQ (part square)  --limit N
Squares are y*256+x (map squares).
"""
import argparse
import glob
import json
import os
import sys

import numpy as np

DT = np.dtype([("tick", "<u4"), ("ev", "<u4"), ("us", "<f4"), ("ov", "<f4"), ("nv", "<f4"),
               ("bytes", "<u4"), ("asq", "<u2"), ("xsq", "<u2"), ("osq", "<u2"), ("deg10", "<u2"),
               ("csq", "<u2"), ("ckind", "<u2"), ("p", "u1"), ("src", "u1"), ("lvl", "u1"),
               ("res", "u1"), ("code", "u1"), ("flags", "u1"), ("depth", "u1"), ("field", "u1"),
               ("run", "<u4")])
assert DT.itemsize == 48

LVL = {1: "chg", 2: "pill", 3: "sync", 4: "reset", 5: "ring", 10: "ray", 11: "flt", 12: "pt", 13: "pl"}
LVL_ID = {v: k for k, v in LVL.items()}
RES = ["hit", "hitrun", "drop", "newkey", "cold", "first", "tank", "gone"]
RES_ID = {v: i for i, v in enumerate(RES)}
DC = ["none", "terrain", "forest", "base", "pill_on_square", "pill_reach", "pill_box", "margin", "box",
      "centre_pill", "slot_pill", "filter_square", "filter_slot", "filter_reach", "wsig_drop", "max_bytes",
      "drop_plans", "square"]
COLD = {0: "first_job", 1: "open", 2: "time", 3: "age", 4: "rules", 5: "lost", 6: "busy", 7: "nojob",
        8: "nomap", 9: "other", 20: "wsig_change"}
SRC = {0: "take", 1: "dbl"}
PO_KIND = {}  # filled from the C enum order if needed
# drop code -> the study's cause class
CLASS = {1: "terrain", 2: "forest", 3: "base", 4: "pill", 5: "pill", 6: "pill", 7: "terrain", 8: "terrain",
         9: "pill", 10: "pill", 11: "terrain", 12: "pill", 13: "pill", 14: "rules", 15: "rules", 16: "rules",
         17: "terrain"}

# the study's rule B rows (cache_parts/parts.md, slices.md): kept, put together, rebuild, work kept
MODEL = {
    "D1 ray": dict(kept=95.8, reasm=0.0, rebuild=4.2, work=95.9, pill=0.2, terrain=0.3, rules=3.7),
    "D2 ring point": dict(kept=87.1, reasm=5.2, rebuild=7.6, work=91.6, pill=5.0, rules=2.5),
    "D3 slice": dict(kept=82.9, reasm=None, rebuild=None, work=83.1),
    "D4 pill X ring": dict(kept=76.4, reasm=13.3, rebuild=10.2, work=89.7, terrain=7.7),
    "D5 plan": dict(kept=67.2, reasm=11.2, rebuild=21.6, work=78.7, pill=14.1, base=0.4, terrain=3.5,
                    forest=1.1, rules=2.5),
}


def load_fields(path):
    names = {}
    fp = path + ".fields"
    if os.path.exists(fp):
        for ln in open(fp):
            a = ln.split(None, 1)
            if len(a) == 2:
                names[int(a[0])] = a[1].strip()
    return names


def files_of(args):
    out = []
    for a in args:
        if os.path.isdir(a):
            out += sorted(glob.glob(os.path.join(a, "dl_p*.bin")))
        else:
            out += glob.glob(a)
    return out


def load(path):
    r = np.fromfile(path, dtype=DT)
    # the job index per (player, src): a RESET record starts a new job
    job = np.zeros(len(r), dtype=np.int32)
    for s in (0, 1):
        m = (r["src"] == s)
        rs = (r["lvl"] == 4) & m
        c = np.cumsum(rs)
        job[m] = c[m]
    return r, job


def sqs(v):
    return "-" if v == 0xFFFF else "%d,%d" % (v % 256, v // 256)


def filt(r, a):
    m = np.ones(len(r), dtype=bool)
    if a.tick:
        lo, hi = a.tick.split(":")
        if lo:
            m &= r["tick"] >= int(lo)
        if hi:
            m &= r["tick"] <= int(hi)
    if a.player is not None:
        m &= r["p"] == a.player
    if a.src:
        m &= r["src"] == (0 if a.src == "take" else 1)
    if a.lvl:
        m &= r["lvl"] == LVL_ID[a.lvl]
    if a.res:
        m &= r["res"] == RES_ID[a.res]
    if a.pill is not None:
        m &= r["asq"] == a.pill
    if a.x is not None:
        m &= r["xsq"] == a.x
    if a.sq is not None:
        m &= (r["osq"] == a.sq) | (r["csq"] == a.sq)
    return m


def why(rec, fields):
    lv, res = int(rec["lvl"]), int(rec["res"])
    if lv in (10, 11, 12, 13, 5):
        if res in (2,) or (lv == 5 and res == 2):
            s = DC[rec["code"]] if rec["code"] < len(DC) else str(rec["code"])
            fl = []
            if rec["flags"] & 1:
                fl.append("child")
            if rec["flags"] & 2:
                fl.append("slot")
            return "%s sq=%s %g>%g%s" % (s, sqs(rec["csq"]), rec["ov"], rec["nv"],
                                         (" (" + ",".join(fl) + ")") if fl else "")
        if res == 3:
            return "field=%s %g>%g" % (fields.get(int(rec["field"]), str(rec["field"])), rec["ov"], rec["nv"])
        if res == 4:
            return "cold=" + COLD.get(int(rec["code"]), str(rec["code"]))
    if lv == 4:
        s = "reset=" + COLD.get(int(rec["code"]), str(rec["code"]))
        if rec["code"] == 20:
            s += " field=%s %g>%g" % (fields.get(int(rec["field"]), str(rec["field"])), rec["ov"], rec["nv"])
        return s
    if lv == 1:
        return "sq=%s kind=%d %g>%g" % (sqs(rec["csq"]), rec["field"], rec["ov"], rec["nv"])
    if lv == 2:
        return "pill %s>%s" % (sqs(int(rec["ov"]) if rec["ov"] >= 0 else 0xFFFF),
                               sqs(int(rec["nv"]) if rec["nv"] >= 0 else 0xFFFF))
    if lv == 3:
        return "changes=%g own=%g kB=%d" % (rec["ov"], rec["nv"], rec["bytes"])
    return ""


def cmd_list(a):
    n = 0
    for f in files_of(a.files):
        fields = load_fields(f)
        r, job = load(f)
        idx = np.nonzero(filt(r, a))[0]
        for i in idx:
            x = r[i]
            print("%7d p%d %-4s job%-3d run%-4d %-5s %-6s goal=%-7s X=%-7s sq=%-7s deg=%-5s us=%8.1f B=%-7d %s" % (
                x["tick"], x["p"], SRC.get(int(x["src"]), "?"), job[i], x["run"], LVL.get(int(x["lvl"]), "?"),
                RES[x["res"]] if x["res"] < len(RES) else "?", sqs(x["asq"]), sqs(x["xsq"]), sqs(x["osq"]),
                "-" if x["deg10"] == 0xFFFF else "%.1f" % (x["deg10"] / 10), x["us"], x["bytes"], why(x, fields)))
            n += 1
            if a.limit and n >= a.limit:
                return


def cmd_counts(a):
    tot = {}
    for f in files_of(a.files):
        fields = load_fields(f)
        r, job = load(f)
        r = r[filt(r, a)]
        for lv in np.unique(r["lvl"]):
            q = r[r["lvl"] == lv]
            for rs in np.unique(q["res"]):
                z = q[q["res"] == rs]
                key = (LVL.get(int(lv)), RES[rs] if lv >= 5 else "-")
                tot[key] = tot.get(key, 0) + len(z)
                if lv >= 5 and rs in (2, 3, 4):
                    for x in np.unique(z["code"] if rs != 3 else z["field"]):
                        nm = (DC[x] if rs == 2 else (COLD.get(int(x)) if rs == 4 else fields.get(int(x), str(x))))
                        k2 = key + (nm,)
                        tot[k2] = tot.get(k2, 0) + int(np.sum((z["code"] if rs != 3 else z["field"]) == x))
    for k in sorted(tot, key=lambda k: (k[0] or "", k[1], k[2] if len(k) > 2 else "")):
        print("%-6s %-7s %-22s %d" % (k[0], k[1], k[2] if len(k) > 2 else "", tot[k]))


def level_stats(recs):
    """recs: list of (r, job) per file. Returns {level: {src: stats}}."""
    out = {}

    def acc(name, src, d):
        o = out.setdefault(name, {}).setdefault(src, {})
        for k, v in d.items():
            if isinstance(v, dict):
                oo = o.setdefault(k, {})
                for kk, vv in v.items():
                    oo[kk] = oo.get(kk, 0) + vv
            else:
                o[k] = o.get(k, 0) + v

    for r, job in recs:
        for s in (0, 1):
            rs = r[r["src"] == s]
            js = job[r["src"] == s]
            sname = SRC[s]
            for lv, name in ((10, "D1 ray"), (11, "D2 filter"), (12, "D2 ring point"), (13, "D5 plan")):
                q = rs[(rs["lvl"] == lv) & (rs["res"] != 1)]
                if not len(q):
                    continue
                hit = q["res"] == 0
                miss = ~hit
                d = dict(lookups=len(q), hit=int(hit.sum()), us_hit=float(q["us"][hit].sum()),
                         us_miss=float(q["us"][miss].sum()), bytes=float(q["bytes"].sum()),
                         hitrun=int(np.sum((rs["lvl"] == lv) & (rs["res"] == 1))))
                res = {}
                for i, nm in enumerate(RES):
                    if i in (0, 1):
                        continue
                    c = int(np.sum(q["res"] == i))
                    if c:
                        res[nm] = c
                d["res"] = res
                cause = {}
                dz = q[q["res"] == 2]
                for c in np.unique(dz["code"]):
                    nm = CLASS.get(int(c), "other")
                    cause[nm] = cause.get(nm, 0) + int(np.sum(dz["code"] == c))
                cz = q[q["res"] == 4]
                for c in np.unique(cz["code"]):
                    nm = "cold_" + COLD.get(int(c), "other")
                    cause[nm] = cause.get(nm, 0) + int(np.sum(cz["code"] == c))
                nk = q[q["res"] == 3]
                if len(nk):
                    cause["new_key"] = len(nk)
                fz = int(np.sum(q["res"] == 5))
                if fz:
                    cause["first_use"] = fz
                tk = int(np.sum(q["res"] == 6)) + int(np.sum(q["res"] == 7))
                if tk:
                    cause["tank_or_gone"] = tk
                d["cause"] = cause
                d["us_cause"] = {}
                nkf = {}
                for x in np.unique(nk["field"]):
                    nkf[int(x)] = int(np.sum(nk["field"] == x))
                d["newkey_field"] = nkf
                acc(name, sname, d)
            # D3: 12 slices of 30 deg per (job, run, goal pill): kept = every point lookup in it hit
            q = rs[(rs["lvl"] == 12) & (rs["res"] != 1) & (rs["deg10"] != 0xFFFF)]
            qj = js[(rs["lvl"] == 12) & (rs["res"] != 1) & (rs["deg10"] != 0xFFFF)]
            if len(q):
                sl = (q["deg10"].astype(np.int64) // 300) % 12
                key = ((qj.astype(np.int64) * 1000003 + q["run"].astype(np.int64)) * 65536
                       + q["asq"].astype(np.int64)) * 16 + sl
                u, inv = np.unique(key, return_inverse=True)
                miss = np.zeros(len(u), dtype=np.int64)
                np.add.at(miss, inv, (q["res"] != 0).astype(np.int64))
                ush = np.zeros(len(u)); usm = np.zeros(len(u))
                np.add.at(ush, inv, np.where(q["res"] == 0, q["us"], 0))
                np.add.at(usm, inv, np.where(q["res"] != 0, q["us"], 0))
                acc("D3 slice", sname, dict(lookups=len(u), hit=int(np.sum(miss == 0)),
                                            us_hit=float(ush.sum()), us_miss=float(usm.sum()), res={}, cause={}))
            # D4: rings (A level per hostile pill slot) at each sync
            q = rs[rs["lvl"] == 5]
            if len(q):
                hit = q["res"] == 0
                cause = {}
                dz = q[~hit]
                for c in np.unique(dz["code"]):
                    nm = CLASS.get(int(c), "other")
                    cause[nm] = cause.get(nm, 0) + int(np.sum(dz["code"] == c))
                acc("D4 pill X ring", sname, dict(lookups=len(q), hit=int(hit.sum()), us_hit=0.0, us_miss=0.0,
                                                  res={"drop": int((~hit).sum())}, cause=cause))
            # world input changes (wsig): every ray, point, plan and filter answer dropped
            q = rs[(rs["lvl"] == 4) & (rs["code"] == 20)]
            wf = {}
            for x in np.unique(q["field"]):
                wf[int(x)] = int(np.sum(q["field"] == x))
            acc("wsig", sname, dict(lookups=len(q), hit=0, us_hit=0.0, us_miss=0.0, res={}, cause={}, newkey_field=wf))
            # jobs
            q = rs[(rs["lvl"] == 4) & (rs["code"] != 20)]
            cz = {}
            for c in np.unique(q["code"]):
                cz[COLD.get(int(c), "other")] = int(np.sum(q["code"] == c))
            acc("jobs", sname, dict(lookups=len(q), hit=0, us_hit=0.0, us_miss=0.0, res={}, cause=cz))
            q = rs[rs["lvl"] == 3]
            acc("syncs", sname, dict(lookups=len(q), hit=int(np.sum(q["field"] == 0)), us_hit=0.0,
                                     us_miss=float(q["us"].sum()), res={}, cause={}))
    return out


def merged(st, name):
    m = {}
    for s, d in st.get(name, {}).items():
        for k, v in d.items():
            if isinstance(v, dict):
                mm = m.setdefault(k, {})
                for kk, vv in v.items():
                    mm[kk] = mm.get(kk, 0) + vv
            else:
                m[k] = m.get(k, 0) + v
    return m


def pct(a, b):
    return 100.0 * a / b if b else 0.0


def row(name, d, model):
    n = d.get("lookups", 0)
    h = d.get("hit", 0)
    reb = n - h
    wk = pct(d.get("us_hit", 0), d.get("us_hit", 0) + d.get("us_miss", 0)) if d.get("us_hit", 0) + d.get("us_miss", 0) > 0 else None
    c = d.get("cause", {})
    cs = ", ".join("%s %.1f" % (k, pct(v, n)) for k, v in sorted(c.items(), key=lambda kv: -kv[1]) if v)
    return dict(level=name, lookups=n, kept=pct(h, n), rebuild=pct(reb, n), work=wk, causes=cs,
                model=model)


def fmt(v):
    return "-" if v is None else "%.1f" % v


def cmd_table(a):
    fl = files_of(a.files)
    recs = [load(f) for f in fl]
    fields = {}
    for f in fl:
        fields.update(load_fields(f))
    st = level_stats(recs)
    order = ["D1 ray", "D2 filter", "D2 ring point", "D3 slice", "D4 pill X ring", "D5 plan"]
    rows = []
    for nm in order:
        if nm in st:
            rows.append(row(nm, merged(st, nm), MODEL.get(nm)))
    bysrc = []
    for nm in order:
        for s in ("dbl", "take"):
            d = st.get(nm, {}).get(s)
            if d:
                bysrc.append(dict(row(nm, d, None), src=s))
    # new key fields, by level
    nkf = {}
    for nm in order:
        f = merged(st, nm).get("newkey_field", {})
        if f:
            nkf[nm] = {fields.get(int(k), str(k)): v for k, v in sorted(f.items(), key=lambda kv: -kv[1])}
    wsf = {fields.get(int(k), str(k)): v for k, v in sorted(merged(st, "wsig").get("newkey_field", {}).items(),
                                                            key=lambda kv: -kv[1])}
    jobs = merged(st, "jobs")
    sync = merged(st, "syncs")
    ticks = max((int(r["tick"].max()) - int(r["tick"].min())) for r, _ in recs if len(r)) if recs else 0
    size = sum(os.path.getsize(f) for f in fl)
    meta = dict(files=fl, ticks=ticks, bytes=size,
                mb_per_game_min=(size / 1e6) / (ticks / 3000.0) if ticks else None,
                jobs=jobs, syncs=sync)
    L = []
    L.append("# Doll cache: live vs model (rule B)\n")
    L.append("Files: %s  \nBrain ticks seen: %d (2 game ticks each; %.1f game min). Log size %.1f MB (%.1f MB per game minute, all bots).\n"
             % (", ".join(os.path.basename(f) for f in fl), ticks, ticks / 3000.0, size / 1e6,
                meta["mb_per_game_min"] or 0))
    L.append("Kept = looked up and found in the store (a first touch per run; a second find in the same run is "
             "not counted). Rebuild = every other lookup. Work kept = the C time of the kept parts (their build "
             "time when made) over that plus the time of the rebuilt parts.\n")
    L.append("| Level | Lookups | Kept % (model) | Rebuild % (model) | Work kept % (model) | Rebuild causes, % of lookups |")
    L.append("|---|---:|---:|---:|---:|---|")
    for x in rows:
        m = x["model"] or {}
        L.append("| %s | %s | %s (%s) | %s (%s) | %s (%s) | %s |" % (
            x["level"], "-" if x["lookups"] is None else x["lookups"], fmt(x["kept"]), fmt(m.get("kept")),
            fmt(x["rebuild"]), fmt(m.get("rebuild")), fmt(x["work"]), fmt(m.get("work")), x["causes"]))
    L.append("\n## By source\n")
    L.append("| Level | Source | Lookups | Kept % | Rebuild % | Work kept % | Rebuild causes, % of lookups |")
    L.append("|---|---|---:|---:|---:|---:|---|")
    for x in bysrc:
        L.append("| %s | %s | %d | %s | %s | %s | %s |" % (x["level"], x["src"], x["lookups"], fmt(x["kept"]),
                                                       fmt(x["rebuild"]), fmt(x["work"]), x["causes"]))
    L.append("\n## New key misses: the first key field that differed from the last key at the same place\n")
    for nm, f in nkf.items():
        L.append("- %s: %s" % (nm, ", ".join("%s %d" % (k, v) for k, v in list(f.items())[:12])))
    L.append("\n## World input changes (all stores of the job dropped), by the first field that changed\n")
    L.append("- %d drops: %s" % (merged(st, "wsig").get("lookups", 0),
                                ", ".join("%s %d" % kv for kv in wsf.items())))
    L.append("\n## Jobs\n")
    L.append("New jobs by reason: %s. Syncs: %d (%d with no change), %.1f s in sync.\n" % (
        ", ".join("%s %d" % kv for kv in sorted(jobs.get("cause", {}).items(), key=lambda kv: -kv[1])),
        sync.get("lookups", 0), sync.get("hit", 0), sync.get("us_miss", 0) / 1e6))
    out = a.out or "live"
    open(out + ".md", "w").write("\n".join(L) + "\n")
    json.dump(dict(meta=meta, rows=rows, wsig_fields=wsf, by_source=bysrc, newkey_fields=nkf, raw=st), open(out + ".json", "w"),
              indent=1, default=float)
    try:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        names = [x["level"] for x in rows if x["model"]]
        lv = [x for x in rows if x["model"]]
        fig, axs = plt.subplots(1, 2, figsize=(13, 4.8))
        for ax, key, title in ((axs[0], "kept", "Kept %"), (axs[1], "work", "Work kept %")):
            xs = np.arange(len(lv))
            mv = [x["model"].get(key) or 0 for x in lv]
            vv = [x[key] or 0 for x in lv]
            ax.bar(xs - 0.2, mv, 0.4, label="model (rule B)", color="#8da0cb")
            ax.bar(xs + 0.2, vv, 0.4, label="live", color="#fc8d62")
            for i in range(len(lv)):
                ax.text(xs[i] - 0.2, mv[i] + 1, "%.0f" % mv[i], ha="center", fontsize=8)
                ax.text(xs[i] + 0.2, vv[i] + 1, "%.0f" % vv[i], ha="center", fontsize=8)
            ax.set_xticks(xs)
            ax.set_xticklabels(names, rotation=20, fontsize=8)
            ax.set_ylim(0, 110)
            ax.set_title(title)
            ax.legend(fontsize=8)
        fig.suptitle("Doll cache: model vs live (%.1f game min)" % (ticks / 3000.0))
        fig.tight_layout()
        fig.savefig(out + ".png", dpi=110)
    except Exception as ex:  # no matplotlib: md and json only
        print("png skipped:", ex, file=sys.stderr)
    print("\n".join(L))


def cmd_mem(a):
    """the doll bytes per bot and source over the game (the SYNC records):
    the whole doll (kB), and of the slot arenas the stored and the dropped
    MB (the changed syncs only)"""
    fl = files_of(a.files)
    rows = []
    for f in fl:
        r = np.fromfile(f, dtype=DT)
        s = r[r["lvl"] == 3]
        for src in (0, 1):
            z = s[s["src"] == src]
            if not len(z):
                continue
            p = int(z["p"][0])
            mins = (z["tick"] // 3000).astype(int)
            for m in range(int(mins.max()) + 1):
                q = z[mins == m]
                if not len(q):
                    rows.append(dict(p=p, src=SRC[src], minute=m + 1, doll_mb=None, live_mb=None, dead_mb=None))
                    continue
                c = q[(q["field"] == 1) & (q["csq"] != 0xFFFF)]
                rows.append(dict(p=p, src=SRC[src], minute=m + 1, doll_mb=float(q["bytes"].max()) / 1024,
                                 live_mb=float(c["csq"].max()) if len(c) else None,
                                 dead_mb=float(c["ckind"].max()) if len(c) else None))
    L = ["# Doll memory per bot over the game (MB, the max in each game minute)\n",
         "Doll = the whole C store of the job. Stored / dropped = the slot arenas (points and plans): the bytes of "
         "the parts still in the store, and the bytes of parts dropped but not yet freed (changed syncs only).\n",
         "| player | source | " + " | ".join("min %d" % (m + 1) for m in range(max(r["minute"] for r in rows))) + " |",
         "|---|---|" + "---:|" * max(r["minute"] for r in rows)]
    keys = sorted(set((r["p"], r["src"]) for r in rows))
    nm = max(r["minute"] for r in rows)
    for p, src in keys:
        for fld, lab in (("doll_mb", "doll"), ("live_mb", "stored"), ("dead_mb", "dropped")):
            v = {r["minute"]: r[fld] for r in rows if r["p"] == p and r["src"] == src}
            L.append("| %d | %s %s | " % (p, src, lab) + " | ".join("-" if v.get(m + 1) is None else "%.0f" % v[m + 1]
                                                                for m in range(nm)) + " |")
    print("\n".join(L))
    if a.out:
        open(a.out + ".md", "w").write("\n".join(L) + "\n")
        json.dump(rows, open(a.out + ".json", "w"), indent=1)
        try:
            import matplotlib
            matplotlib.use("Agg")
            import matplotlib.pyplot as plt
            fig, ax = plt.subplots(1, 2, figsize=(13, 4.5))
            for p, src in keys:
                for k, fld in enumerate(("doll_mb", "dead_mb")):
                    v = [(r["minute"], r[fld]) for r in rows if r["p"] == p and r["src"] == src and r[fld] is not None]
                    if v:
                        ax[k].plot([x for x, _ in v], [y for _, y in v], marker="o", label="player %d %s" % (p, src))
            ax[0].set_title("Doll MB (max per game minute)")
            ax[1].set_title("Slot arenas: dropped, not freed MB")
            for x in ax:
                x.set_xlabel("game minute"); x.set_ylabel("MB"); x.grid(alpha=0.3)
            ax[0].legend(fontsize=7)
            fig.tight_layout()
            fig.savefig(a.out + ".png", dpi=110)
        except ImportError:
            pass


# ── at: what the BrainTest doll layers draw at one tick, and why ─────────

def sq_dist(a, b):
    if a == 0xFFFF or b == 0xFFFF:
        return None
    return ((a % 256 - b % 256) ** 2 + (a // 256 - b // 256) ** 2) ** 0.5


def fd(v):
    return "-" if v is None else "%.1f" % v


def at_pill_squares(sess, player, tick):
    """pill number (world.pills id = the engine pill index, 0-based, the
    pill_id_label number) -> (square, where from). From the run's
    snap.jsonl (the last snapshot at or before the engine tick 2 * tick),
    else the bot's brain log world records."""
    eng = 2 * tick
    for d in (sess, os.path.dirname(sess), os.path.dirname(os.path.dirname(sess))):
        p = os.path.join(d, "snap.jsonl")
        if not os.path.exists(p):
            continue
        best = None
        for ln in open(p):
            try:
                s = json.loads(ln)
            except ValueError:
                continue
            if s.get("tick", 1 << 30) <= eng and "pillboxes" in s:
                best = s
        if best:
            out = {}
            for i, pb in enumerate(best["pillboxes"]):
                out[i] = (pb["ty"] * 256 + pb["tx"], "snap.jsonl engine tick %d%s" % (
                    best["tick"], " (in a tank)" if pb.get("in_tank") else ""))
            return out
    for d in (sess, os.path.dirname(sess), os.path.dirname(os.path.dirname(sess))):
        for p in sorted(glob.glob(os.path.join(d, "player%d*.jsonl" % player))):
            best = None
            for ln in open(p):
                if '"type":"world"' not in ln[:20]:
                    continue
                s = json.loads(ln)
                if s.get("t", 0) <= tick or best is None:
                    best = s
            if best:
                return {pl["id"]: (pl["y"] * 256 + pl["x"], os.path.basename(p) + " world record")
                        for pl in best.get("pills", [])}
    return {}


def at_runs(q):
    """pt2_viz.lua dl_take: per source the records of the last run (a
    lookup record with another run id starts a new one) and of the last
    sync (a record of another tick starts a new one)."""
    run, sync = {}, {}
    for i in range(len(q)):
        x = q[i]
        s = int(x["src"])
        if x["lvl"] >= 10:
            e = run.get(s)
            if e is None or e["run"] != int(x["run"]):
                e = run[s] = dict(run=int(x["run"]), idx=[])
            e["idx"].append(i)
        else:
            e = sync.get(s)
            if e is None or e["tick"] != int(x["tick"]):
                e = sync[s] = dict(tick=int(x["tick"]), idx=[])
            e["idx"].append(i)
    return run, sync


def at_cause_change(r, i, fields):
    """the change record behind a dropped lookup r[i]: the last sync of its
    source at or before the lookup with a change on the cause square (a
    terrain/base change, or a pill change from or to that square); a
    world input drop: the last RESET record with code 20"""
    x = r[i]
    src, p, code, csq = int(x["src"]), int(x["p"]), int(x["code"]), int(x["csq"])
    m = (r["src"] == src) & (r["p"] == p) & (np.arange(len(r)) < i)
    if code == 14:   # wsig_drop
        z = np.nonzero(m & (r["lvl"] == 4) & (r["code"] == 20))[0]
        if len(z):
            y = r[z[-1]]
            return dict(tick=int(y["tick"]), what="world input %s %g>%g" % (
                fields.get(int(y["field"]), str(y["field"])), y["ov"], y["nv"]), sq=0xFFFF)
        return None
    if csq == 0xFFFF:
        return None
    if code in (4, 5, 6, 9, 10, 13):   # the pill change codes
        z = np.nonzero(m & (r["lvl"] == 2) & ((r["ov"] == csq) | (r["nv"] == csq)))[0]
        if len(z):
            y = r[z[-1]]
            fl = int(y["field"])
            who = "hostile" if fl & 1 else ("ally" if fl & 2 else "neutral/none")
            return dict(tick=int(y["tick"]), what="pill %s>%s (%s)" % (
                sqs(int(y["ov"]) if y["ov"] >= 0 else 0xFFFF), sqs(int(y["nv"]) if y["nv"] >= 0 else 0xFFFF), who),
                sq=csq)
    z = np.nonzero(m & (r["lvl"] == 1) & (r["csq"] == csq))[0]
    if len(z):
        y = r[z[-1]]
        return dict(tick=int(y["tick"]), what="square %s kind=%d terrain %g>%g" % (
            sqs(csq), y["field"], y["ov"], y["nv"]), sq=csq)
    return None


def at_reason(r, i, fields):
    x = r[i]
    res = int(x["res"])
    if res == 2:
        s = DC[x["code"]] if x["code"] < len(DC) else str(x["code"])
        fl = []
        if x["flags"] & 1:
            fl.append("via a child part")
        if x["flags"] & 2:
            fl.append("its slot")
        return "%s sq=%s%s" % (s, sqs(x["csq"]), (" (" + ", ".join(fl) + ")") if fl else "")
    if res == 3:
        return "new key: field %s %g>%g" % (fields.get(int(x["field"]), str(x["field"])), x["ov"], x["nv"])
    if res == 4:
        return "new job: " + COLD.get(int(x["code"]), str(x["code"]))
    return {5: "first use", 6: "tank inside its box", 7: "gone (the log had it live)"}.get(res, RES[res])


def cmd_at(a):
    sess = a.files[0]
    tick = int(a.tick)
    p = a.player if a.player is not None else 0
    f = sess if os.path.isfile(sess) else os.path.join(sess, "dl_p%d.bin" % p)
    if os.path.isfile(sess):
        sess = os.path.dirname(sess)
    if not os.path.exists(f):
        print("no event log %s (the game needs PT2_V2_DOLL_VIZ_RECORD or PT2_V2_DOLL_EVENT_LOG)" % f)
        return
    fields = load_fields(f)
    r, job = load(f)
    r = r[r["p"] == p] if len(r) and np.any(r["p"] == p) else r
    q_idx = np.nonzero(r["tick"] <= tick)[0]
    q = r[q_idx]
    pills = at_pill_squares(sess, p, tick)
    num_of = {v[0]: k for k, v in pills.items()}
    pn = lambda s: ("#%d" % num_of[s]) if s in num_of else "#?"
    resets = r[(r["lvl"] == 4) & (r["code"] != 20) & (r["tick"] <= tick)]
    print("Session %s, player %d, tick %d (BrainTest HUD Tick = brain tick = engine tick / 2 = %d)." % (
        sess, p, tick, 2 * tick))
    goal = None
    if a.pill is not None:
        if a.pill in pills:
            goal = pills[a.pill][0]
            print("Pill #%d (pill_id_label number) = square %s (from %s)." % (a.pill, sqs(goal), pills[a.pill][1]))
        else:
            print("Pill #%d: no square found (no snap.jsonl or brain log world record)." % a.pill)
    run, sync = at_runs(q)
    if not run:
        print("No doll lookup records at or before this tick: the layers are empty.")
        return
    print("\nThe layers draw the LAST run of each source (not the last run of a goal pill), and the last sync.")
    for s, e in sorted(run.items()):
        z = q[e["idx"]]
        ts = sorted(set(z["tick"].tolist()))
        goals = sorted(set(z["asq"].tolist()))
        jr = resets[resets["src"] == s]
        print("  %s: run %d at tick %s, %d lookups, goal pills %s; job since tick %s (%s)" % (
            SRC[s], e["run"], ",".join(map(str, ts)), len(z),
            ", ".join("%s %s" % (pn(g), sqs(g)) for g in goals),
            int(jr["tick"][-1]) if len(jr) else "?", COLD.get(int(jr["code"][-1]), "?") if len(jr) else "?"))
        for ev in sorted(set(z["ev"].tolist())):
            w = z[z["ev"] == ev]
            X = sorted(set(int(v) for v in w["xsq"] if v != 0xFFFF))
            print("    evaluate %d: goal %s %s, pills X %s, depth %s" % (
                ev, pn(int(w["asq"][0])), sqs(w["asq"][0]), ", ".join("%s %s" % (pn(v), sqs(v)) for v in X) or "-",
                sorted(set(w["depth"].tolist()))))
        if goal is not None and goal not in goals:
            print("    NOTE: pill #%d is not in this run: the layers show other goal pills" % a.pill)

    def recs_of(lvls, need_x=False):
        out = []
        for s, e in sorted(run.items()):
            for k in e["idx"]:
                x = q[k]
                if x["lvl"] in lvls and x["res"] != 1 and x["osq"] != 0xFFFF and (not need_x or x["xsq"] != 0xFFFF):
                    out.append(k)
        return out

    def counts(ks):
        out = []
        for s in sorted(set(int(q[k]["src"]) for k in ks)):
            c = {}
            for k in ks:
                if q[k]["src"] == s:
                    c[RES[q[k]["res"]]] = c.get(RES[q[k]["res"]], 0) + 1
            out.append("%s run at tick %s: %s" % (
                SRC[s], ",".join(map(str, sorted(set(int(q[k]["tick"]) for k in run[s]["idx"])))),
                ", ".join("%s %d" % (n, c[n]) for n in RES if n in c)))
        return "; ".join(out) or "none"

    def explain(ks, title):
        bad = [k for k in ks if q[k]["res"] not in (0, 1)]
        if not bad:
            return
        grp = {}
        for k in bad:
            x = q[k]
            key = (int(x["src"]), int(x["lvl"]), int(x["res"]), at_reason(q, k, fields))
            grp.setdefault(key, []).append(k)
        print("    not kept, by reason:")
        for (s, lv, rs, why), ks2 in sorted(grp.items(), key=lambda kv: (kv[0][0], -len(kv[1]))):
            k = ks2[0]
            x = q[k]
            line = "      %5d %-4s %-4s %-7s %s" % (len(ks2), SRC[s], LVL[lv], RES[rs], why)
            if rs == 2:
                ch = at_cause_change(r, int(q_idx[k]), fields)
                jr = resets[(resets["src"] == x["src"]) & (resets["tick"] <= x["tick"])]
                if ch:
                    dA = sq_dist(ch["sq"], int(x["asq"]))
                    Xs = sorted(set(int(q[j]["xsq"]) for j in run[int(x["src"])]["idx"]
                                    if q[j]["asq"] == x["asq"] and q[j]["xsq"] != 0xFFFF))
                    dX = ", ".join("%s %s" % (pn(v), fd(sq_dist(ch["sq"], v))) for v in Xs)
                    line += "\n            change: tick %d, %s" % (ch["tick"], ch["what"])
                    if ch["sq"] != 0xFFFF:
                        line += "; distance from goal %s: %s, from pills X: %s" % (pn(int(x["asq"])), fd(dA), dX or "-")
                    if len(jr) and ch["tick"] < int(jr["tick"][-1]):
                        line += ("\n            NOTE: that change is older than this job (new job at tick %d):"
                                 " the job never made this part, so read it as a new job miss" % int(jr["tick"][-1]))
                else:
                    line += "\n            change: not found in the log"
            print(line)

    print("\npt2_d1 rays (pill X to the point's square):")
    k1 = recs_of((10,), need_x=True)
    print("    %d lookups: %s" % (len(k1), counts(k1)))
    explain(k1, "D1")
    print("\npt2_d2 filter answers (outline) and ring points (fill):")
    k2f, k2p = recs_of((11,)), recs_of((12,))
    print("    filter %d lookups: %s" % (len(k2f), counts(k2f)))
    print("    ring points %d lookups: %s" % (len(k2p), counts(k2p)))
    explain(k2f + k2p, "D2")
    print("\npt2_d3 slices (12 x 30 deg around each goal pill; red when ANY point lookup in it is not kept):")
    sl = {}
    for k in k2p:
        x = q[k]
        if x["deg10"] == 0xFFFF or x["asq"] == 0xFFFF:
            continue
        key = (int(x["src"]), int(x["asq"]), (int(x["deg10"]) // 300) % 12)
        e = sl.setdefault(key, {})
        e[RES[x["res"]]] = e.get(RES[x["res"]], 0) + 1
    nk = sum(1 for e in sl.values() if set(e) <= {"hit"})
    print("    %d of %d slices kept (green)" % (nk, len(sl)))
    for (s, A, i), e in sorted(sl.items()):
        if set(e) <= {"hit"}:
            continue
        print("      %s goal %s slice %3d-%3d deg: %s" % (SRC[s], pn(A), i * 30, i * 30 + 30,
                                                         ", ".join("%s %d" % kv for kv in sorted(e.items()))))
    print("\npt2_d4 pill X rings (the last sync):")
    for s, e in sorted(sync.items()):
        z = [k for k in e["idx"] if q[k]["lvl"] == 5 and q[k]["asq"] != 0xFFFF]
        print("    %s sync at tick %d: %d of %d kept" % (SRC[s], e["tick"], sum(1 for k in z if q[k]["res"] == 0), len(z)))
        for k in z:
            x = q[k]
            if x["res"] != 0:
                print("      slot %s %s: %s sq=%s (distance %s)" % (pn(int(x["asq"])), sqs(x["asq"]),
                      DC[x["code"]] if x["code"] < len(DC) else x["code"], sqs(x["csq"]),
                      fd(sq_dist(int(x["csq"]), int(x["asq"])))))
    print("\npt2_d5 plans:")
    k5 = recs_of((13,))
    print("    %d lookups: %s" % (len(k5), counts(k5)))
    explain(k5, "D5")
    print("\npt2_dchg (the last sync):")
    for s, e in sorted(sync.items()):
        for k in e["idx"]:
            x = q[k]
            if x["lvl"] in (1, 2, 4) or (x["lvl"] == 3):
                print("    %s %d %-5s %s" % (SRC[s], x["tick"], LVL[int(x["lvl"])], why(x, fields)))

    if goal is None:
        return
    # the goal pill's last earlier evaluate with every lookup kept, and what changed since
    print("\nPill #%d %s: its evaluates up to tick %d (newest last; kept = every lookup kept):" % (a.pill, sqs(goal), tick))
    gq = q[(q["asq"] == goal) & (q["lvl"] >= 10) & (q["res"] != 1)]
    evs = []
    for ev in np.unique(gq["ev"]):
        w = gq[gq["ev"] == ev]
        X = tuple(sorted(set(int(v) for v in w["xsq"] if v != 0xFFFF)))
        evs.append(dict(ev=int(ev), tick=int(w["tick"][0]), src=int(w["src"][0]), run=int(w["run"][0]), X=X,
                        depth=tuple(sorted(set(w["depth"].tolist()))), n=len(w), kept=int(np.sum(w["res"] == 0))))
    evs.sort(key=lambda e: (e["tick"], e["ev"]))
    for e in evs[-12:]:
        print("    tick %5d %s run %-4d evaluate %-5d X %-26s depth %-7s kept %d/%d" % (
            e["tick"], SRC[e["src"]], e["run"], e["ev"], ",".join(pn(v) for v in e["X"]), ",".join(map(str, e["depth"])),
            e["kept"], e["n"]))
    if not evs:
        return
    last = evs[-1]
    prev = [e for e in evs[:-1] if e["kept"] == e["n"]]
    same = [e for e in prev if e["X"] == last["X"] and e["depth"] == last["depth"]]
    for lab, lst in (("any kind", prev), ("the same kind (same pills X and depth)", same)):
        if not lst:
            print("  last earlier all-kept evaluate, %s: none" % lab)
            continue
        e = lst[-1]
        print("  last earlier all-kept evaluate, %s: tick %d (%s run %d, evaluate %d, X %s)" % (
            lab, e["tick"], SRC[e["src"]], e["run"], e["ev"], ",".join(pn(v) for v in e["X"])))
    if prev and (prev[-1]["X"] != last["X"] or prev[-1]["depth"] != last["depth"]):
        print("  NOTE: the newest evaluate is another kind than the last all-kept one (pills X %s depth %s vs %s depth %s):"
              " its parts have other keys" % (",".join(pn(v) for v in last["X"]), ",".join(map(str, last["depth"])),
                                              ",".join(pn(v) for v in prev[-1]["X"]), ",".join(map(str, prev[-1]["depth"]))))
    kind = [e for e in evs[:-1] if e["X"] == last["X"] and e["depth"] == last["depth"]]
    t0 = kind[-1]["tick"] if kind else (prev[-1]["tick"] if prev else 0)
    print("  changes the %s syncs saw from tick %d (the last evaluate of the same kind) to %d:" % (
        SRC[last["src"]], t0, last["tick"]))
    cz = r[(r["src"] == last["src"]) & (r["tick"] > t0) & (r["tick"] <= last["tick"]) & np.isin(r["lvl"], (1, 2, 4))]
    if not len(cz):
        print("    none")
    for x in cz:
        d = ""
        if x["lvl"] == 1:
            d = " (distance from goal %s)" % fd(sq_dist(int(x["csq"]), goal))
        elif x["lvl"] == 2:
            d = " (distance from goal %s / %s)" % (fd(sq_dist(int(x["ov"]), goal) if x["ov"] >= 0 else None),
                                                   fd(sq_dist(int(x["nv"]), goal) if x["nv"] >= 0 else None))
        print("    %5d %-5s %s%s" % (x["tick"], LVL[int(x["lvl"])], why(x, fields), d))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("cmd", choices=["list", "counts", "summary", "table", "mem", "at"])
    ap.add_argument("files", nargs="+")
    ap.add_argument("--tick")
    ap.add_argument("--player", type=int)
    ap.add_argument("--src", choices=["take", "dbl"])
    ap.add_argument("--lvl", choices=list(LVL_ID))
    ap.add_argument("--res", choices=RES)
    ap.add_argument("--pill", type=int)
    ap.add_argument("--x", type=int)
    ap.add_argument("--sq", type=int)
    ap.add_argument("--limit", type=int)
    ap.add_argument("--out")
    a = ap.parse_args()
    if a.cmd == "list":
        cmd_list(a)
    elif a.cmd == "counts":
        cmd_counts(a)
    elif a.cmd == "mem":
        cmd_mem(a)
    elif a.cmd == "at":
        cmd_at(a)
    else:
        cmd_table(a)


if __name__ == "__main__":
    main()
