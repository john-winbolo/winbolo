#!/usr/bin/env python3
"""Read the PT2_V2_DOLL_MEM lines (cfg=PT2_V2_DOLL_MEM_LOG=true) of a DS
stderr file and print the doll cache's bytes by part, over time, at the
peak and at the end. Optional: draw a PNG of the bytes over time (PIL).

Usage: doll_mem.py STDERR [--png OUT.png] [--csv OUT.csv]
"""
import argparse
import collections
import sys

KINDS = ["ray", "pt", "pl", "flt"]
POK = ["ray", "point", "cwm", "filt", "post", "plan", "alev", "aws"]
LEVEL = {"ray": "D1", "cwm": "D1c", "point": "D2", "filt": "D2", "alev": "D4", "aws": "D4",
         "post": "D5", "plan": "D5"}
SIZEOF_POBJ = 192


def parse(path):
    rows = []
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        for ln in f:
            if not ln.startswith("PT2_V2_DOLL_MEM "):
                continue
            d = {}
            for tok in ln.split()[1:]:
                if "=" in tok:
                    k, v = tok.split("=", 1)
                    d[k] = v
            rows.append(d)
    return rows


def num(d, k, i=None):
    v = d.get(k)
    if v is None:
        return 0.0
    if i is not None:
        p = v.split(",")
        return float(p[i]) if i < len(p) else 0.0
    try:
        return float(v)
    except ValueError:
        return 0.0


def parts(d):
    """The job's bytes by part (no instrument bytes)."""
    p = collections.OrderedDict()
    if d.get("src") == "none":
        return p
    p["ray_arena"] = num(d, "ar_cb")
    # the slot arenas: the bytes the stored standoff points and plans
    # reach, and the rest (evaluate scratch, chunk ends)
    pr, lr = num(d, "pt_reach"), num(d, "pl_reach")
    p["slot: points kept"] = pr
    p["slot: plans kept"] = lr
    p["slot: dropped+scratch"] = max(0.0, num(d, "spar_cb") - pr - lr)
    p["par_arena"] = num(d, "par_cb")
    hm = 0
    for m in ["rays", "sqm", "sqf", "pts", "pls", "flt"]:
        hm += num(d, m + "_b")
    p["hash_maps"] = hm
    po = 0
    for k in POK:
        n = num(d, "o_" + k, 0)
        # the 9th number: the part bytes (Nov 27, PT2_V2_DOLL_POBJ_SMALL); else n x sizeof
        nb = num(d, "o_" + k, 8) if len(d.get("o_" + k, "").split(",")) > 8 else n * (num(d, "sizeof_pobj") or SIZEOF_POBJ)
        po += nb + num(d, "o_" + k, 1) + num(d, "o_" + k, 3)
    p["pobj"] = po
    p["pobj_map+cap"] = num(d, "om_b") + num(d, "capobj") * 8
    p["slot_tables"] = num(d, "cx_b") + num(d, "fr_b") + num(d, "pccjob")
    p["fixed"] = num(d, "dsnap") * 2 + num(d, "dlb_bits") + num(d, "pills_cap") + num(d, "sizeof_job")
    return p


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("stderr")
    ap.add_argument("--png")
    ap.add_argument("--csv")
    a = ap.parse_args()
    rows = parse(a.stderr)
    if not rows:
        print("no PT2_V2_DOLL_MEM lines")
        return 1
    # the newest line per (player, src) at each sample time
    times = sorted({int(num(r, "t")) for r in rows})
    last = {}
    series = []
    ri = 0
    rows_t = sorted(rows, key=lambda r: num(r, "t"))
    for t in times:
        while ri < len(rows_t) and num(rows_t[ri], "t") <= t:
            r = rows_t[ri]
            key = (r.get("p"), r.get("src"))
            if r.get("src") == "none":
                for s in ("take", "dbl"):
                    last.pop((r.get("p"), s), None)
            else:
                last[key] = r
            last[(r.get("p"), "_proc")] = r
            ri += 1
        tot = collections.OrderedDict()
        for (pl, src), r in last.items():
            if src == "_proc":
                continue
            for k, v in parts(r).items():
                tot[k] = tot.get(k, 0) + v
        zomb = sum(num(r, "zomb") for (pl, src), r in last.items() if src == "_proc")
        ws = max(num(r, "ws") for r in last.values())
        hc = max(num(r, "heap_commit") for r in last.values())
        ha = max(num(r, "heap_alloc") for r in last.values())
        lua = sum(num(r, "lua_kb") * 1024 for (pl, src), r in last.items() if src == "_proc")
        side = sum(num(r, "dm_side_b") for (pl, src), r in last.items() if src != "_proc")
        series.append((t, tot, zomb, ws, hc, ha, lua, side))
    MB = 1 / (1 << 20)
    print("t  doll_parts_MB  zombie_MB  ws_MB  heap_commit_MB  heap_alloc_MB  lua_MB  instrument_MB")
    for t, tot, zomb, ws, hc, ha, lua, side in series[:: max(1, len(series) // 60)]:
        print("%6d %8.1f %7.1f %7.1f %7.1f %7.1f %6.1f %6.1f" % (t, sum(tot.values()) * MB, zomb * MB, ws * MB,
                                                               hc * MB, ha * MB, lua * MB, side * MB))
    if a.csv:
        with open(a.csv, "w") as f:
            keys = list(series[-1][1].keys()) or ["ray_arena"]
            f.write("t," + ",".join(keys) + ",zombie,ws,heap_commit,heap_alloc,lua,instrument\n")
            for t, tot, zomb, ws, hc, ha, lua, side in series:
                f.write("%d,%s,%.0f,%.0f,%.0f,%.0f,%.0f,%.0f\n" % (t, ",".join("%.0f" % tot.get(k, 0) for k in keys),
                                                                    zomb, ws, hc, ha, lua, side))
    if a.png:
        draw(series, a.png)
    return 0


def draw(series, out):
    from PIL import Image, ImageDraw
    W, H, L, R, T, B = 1400, 700, 90, 260, 40, 60
    im = Image.new("RGB", (W, H), "white")
    g = ImageDraw.Draw(im)
    keys = []
    for s in series:
        for k in s[1]:
            if k not in keys:
                keys.append(k)
    keys.append("zombie")
    cols = [(31, 119, 180), (255, 127, 14), (44, 160, 44), (214, 39, 40), (148, 103, 189), (140, 86, 75),
            (227, 119, 194), (127, 127, 127), (188, 189, 34), (23, 190, 207), (255, 215, 0), (0, 0, 0)]
    tmax = max(s[0] for s in series) or 1
    ymax = max(max(sum(s[1].values()) + s[2], s[3]) for s in series) * 1.05 or 1
    MB = 1 / (1 << 20)

    def X(t):
        return L + (W - L - R) * t / tmax

    def Y(v):
        return H - B - (H - T - B) * v / ymax
    for i in range(len(series) - 1):
        t0, t1 = series[i][0], series[i + 1][0]
        base = 0
        for ki, k in enumerate(keys):
            v = series[i][2] if k == "zombie" else series[i][1].get(k, 0)
            g.rectangle([X(t0), Y(base + v), X(t1), Y(base)], fill=cols[ki % len(cols)])
            base += v
    pts = [(X(s[0]), Y(s[3])) for s in series]
    g.line(pts, fill="black", width=2)
    pts = [(X(s[0]), Y(s[4])) for s in series]
    g.line(pts, fill=(90, 90, 90), width=1)
    pk = max(series, key=lambda s: sum(s[1].values()) + s[2])
    g.line([X(pk[0]), T, X(pk[0]), H - B], fill="red", width=1)
    g.text((X(pk[0]) + 4, T + 2), "doll peak t=%d (%.0f MB)" % (pk[0], (sum(pk[1].values()) + pk[2]) * MB), fill="red")
    step = 100 if ymax * MB < 1200 else 200
    v = 0
    while v <= ymax * MB:
        g.line([L - 5, Y(v / MB), L, Y(v / MB)], fill="black")
        g.text((10, Y(v / MB) - 6), "%d MB" % v, fill="black")
        v += step
    for tt in range(0, int(tmax) + 1, 5000):
        g.text((X(tt) - 15, H - B + 8), "%d" % tt, fill="black")
    g.text((L, H - 25), "brain ticks (50 per second, 15 game-min); stacked = doll bytes by part, all 4 players, take + dbl jobs", fill="black")
    g.line([L, H - B, W - R, H - B], fill="black")
    g.line([L, T, L, H - B], fill="black")
    ly = T
    for ki, k in enumerate(keys):
        g.rectangle([W - R + 15, ly, W - R + 30, ly + 12], fill=cols[ki % len(cols)])
        g.text((W - R + 36, ly), k, fill="black")
        ly += 20
    g.line([W - R + 15, ly + 6, W - R + 30, ly + 6], fill="black", width=2)
    g.text((W - R + 36, ly), "process working set", fill="black")
    ly += 20
    g.line([W - R + 15, ly + 6, W - R + 30, ly + 6], fill=(90, 90, 90), width=1)
    g.text((W - R + 36, ly), "process heap commit", fill="black")
    g.text((L, 10), "PT2 v2 doll cache memory over time", fill="black")
    im.save(out)


if __name__ == "__main__":
    sys.exit(main())
