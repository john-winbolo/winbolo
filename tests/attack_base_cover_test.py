#!/usr/bin/env python3
"""attack_base pricing test (GoalHunter 1.7 pool 7).

Two defects, one arena (tests/generate_attack_base_cover_map.py):

  1. DOUBLE CHARGE.  step_eval_queue's per-candidate cost for pool 7 is
     path + stale + armour-aware markup + threat.  finalize_pools then took
     that number and added the markup and the threat to it AGAIN, and printed
     the whole candidate cost under the label `path=`.  Field session
     20260902_000405_1_16v17 bot2 t=22561: base #9 competed at 210 with a
     candidate cost of 130.
  2. NO FRIENDLY-COVER CREDIT.  refresh_base_steal rejects a base we could only
     reach down a hostile pillbox's line of fire.  There was no mirror: a base
     sitting under two of OUR pillboxes was priced exactly like one in the
     middle of their half.

The arena puts two untouched hostile bases the SAME 8 tiles from the tank, on
flat grass, differing only in that two of our pills cover one of them.

PASS requires all four:
  1. every pool-7 candidate row's printed arithmetic closes:
     cand == path + stale + base + threat (so the markup is charged ONCE);
  2. the covered base carries an fcover chip of ATTACK_BASE_FRIENDLY_COVER_MULT
     ^ 2 = 0.64 with n=2, and the bare base carries none;
  3. the covered base's markup is exactly 0.64x the bare base's -- i.e. the
     discount lands on the engage half and nothing else;
  4. the covered base WINS pool 7 on a tick where the two path costs are within
     a few tiles of each other, and the FINAL_SCORES row for it reports
     cost == cand (no second charge at finalize).

Usage: python attack_base_cover_test.py [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

# -asap by default: ticks run back-to-back instead of one per 20 ms of wall
# clock. Same seed -> byte-identical game, just faster. --no-asap (or
# WINBOLO_ASAP=0) puts this run back on the 20 ms live-game pacing.
from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "attack_base_cover.map"
FINAL = HERE / "attack_base_cover_final.json"
STDERR = HERE / "attack_base_cover_stderr.txt"
LABEL = "attack_base_cover_test"

sys.path.insert(0, str(HERE))
from generate_attack_base_cover_map import (      # noqa: E402
    BASE_COVERED, BASE_BARE, COVER_MULT)

EXPECT_MULT = COVER_MULT ** 2       # two pills cover the covered base
PATH_TOL = 12                       # cost units: ~6 tiles of grass

# goals.lua P7_CANDS t=123 top2 | #1@(118,126) cand=115 = path 64 + stale 0
#   + base 51 + threat 0 fcover{x0.64 n=2} WINNER ; #2@(134,126) cand=144 = ...
P7_LINE_RE = re.compile(r"P7_CANDS t=(\d+) top\d+ \| (.*)")
P7_ROW_RE = re.compile(
    r"#(\S+)@\((\d+),(\d+)\) cand=(-?[\d.]+) = path (-?[\d.]+) \+ stale (-?[\d.]+) "
    r"\+ base (-?[\d.]+) \+ threat (-?[\d.]+)(?: fcover\{x([\d.]+) n=(\d+)\})?"
    r"( STALE\([^)]*\))?(?: (WINNER))?")
# FINAL_SCORES desc: attack_base#1@(118,126) cost=115 (cand=115 = path{64}
#   +stale{0} +base{80 x hp 17/17 x fcover{0.64 n=2} = 51} +threat{...})
# `cand~=` (with the tilde) marks a winner with no cost_cache row -- the terms
# are then a re-read at the decision tick and are NOT expected to sum, so those
# rows are matched but skipped by the arithmetic check below.
FINAL_RE = re.compile(
    r"attack_base#(\S+)@\((\d+),(\d+)\) cost=(-?[\d.]+) \(cand~?=(-?[\d.]+) = "
    r"path\{(-?[\d.]+)\} \+stale\{(-?[\d.]+)\} \+base\{.*?= (-?[\d.]+)\} "
    r"\+threat\{.*?= (-?[\d.]+)\}\)")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{LABEL}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def run(ticks, build_dir):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    subprocess.run(
        [sys.executable, str(HERE / "generate_attack_base_cover_map.py")],
        check=True, stdout=subprocess.DEVNULL)
    for p in (FINAL, STDERR):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked -- a previous WinBoloDS run is "
                      f"still going.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    cmd = [str(ds), "-map", str(MAP), "-port", "50054", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(FINAL),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(STDERR, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 10))

    sess = newest_session(build_dir)
    if not sess:
        print("FAIL: no debug session produced")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed -- see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:2000])
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")

    # ── parse every P7_CANDS line into rows keyed by base TILE ────────
    lines = []           # (tick, {tile: row})
    for tick, body in P7_LINE_RE.findall(text):
        rows = {}
        for m in P7_ROW_RE.finditer(body):
            (bid, bx, by, cand, path, stale, base, threat,
             fcm, fcn, stale_mark, winner) = m.groups()
            rows[(int(bx), int(by))] = dict(
                id=bid, cand=float(cand), path=float(path),
                stale=float(stale), base=float(base), threat=float(threat),
                fcov=float(fcm) if fcm else None,
                fcov_n=int(fcn) if fcn else 0,
                stale_row=bool(stale_mark),
                winner=bool(winner))
        if rows:
            lines.append((int(tick), rows))
    print(f"  session: {sess.name}")
    print(f"  P7_CANDS lines: {len(lines)}")
    if not lines:
        print("FAIL: no P7_CANDS diagnostic lines at all -- pool 7 never "
              "produced a candidate. Are the two bases actually hostile? "
              "(grep the stderr for ATTACK_BASE_COVER)")
        return 1

    both = [(t, r) for (t, r) in lines
            if BASE_COVERED in r and BASE_BARE in r]
    print(f"  ...of which {len(both)} list BOTH bases")
    if not both:
        seen = sorted({tile for _, r in lines for tile in r})
        print(f"FAIL: no P7_CANDS line lists both {BASE_COVERED} and "
              f"{BASE_BARE}. Tiles seen: {seen}")
        return 1

    # ── 1. the arithmetic closes on every row ─────────────────────────
    bad = []
    n_rows = 0
    for t, rows in lines:
        for tile, r in rows.items():
            n_rows += 1
            if r["stale_row"]:
                # The row says so itself: its components come from a later
                # re-evaluation than the cost it is carrying.
                continue
            want = r["path"] + r["stale"] + r["base"] + r["threat"]
            if abs(want - r["cand"]) > 1.0:
                bad.append((t, tile, r, want))
    if bad:
        t, tile, r, want = bad[0]
        print(f"FAIL (1): {len(bad)}/{n_rows} pool-7 rows do not add up -- "
              f"t={t} base@{tile} cand={r['cand']} but "
              f"path {r['path']} + stale {r['stale']} + base {r['base']} + "
              f"threat {r['threat']} = {want}")
        return 1
    print(f"  1 OK: cand == path + stale + base + threat on all {n_rows} "
          f"pool-7 rows (the markup is charged once)")

    # ── 2 + 3. the cover chip, and where the discount landed ──────────
    t0, r0 = both[0]
    cov, bare = r0[BASE_COVERED], r0[BASE_BARE]
    print(f"  first line with both (t={t0}):")
    print(f"    covered {BASE_COVERED}: cand={cov['cand']:.0f} "
          f"path={cov['path']:.0f} base={cov['base']:.0f} "
          f"threat={cov['threat']:.0f} fcover={cov['fcov']} n={cov['fcov_n']}")
    print(f"    bare    {BASE_BARE}: cand={bare['cand']:.0f} "
          f"path={bare['path']:.0f} base={bare['base']:.0f} "
          f"threat={bare['threat']:.0f} fcover={bare['fcov']}")
    if cov["fcov"] is None or abs(cov["fcov"] - EXPECT_MULT) > 0.01 \
       or cov["fcov_n"] != 2:
        print(f"FAIL (2): the covered base should carry fcover "
              f"{{x{EXPECT_MULT:.2f} n=2}}; it reports "
              f"{cov['fcov']} n={cov['fcov_n']}")
        return 1
    if bare["fcov"] is not None:
        print(f"FAIL (2): the BARE base picked up an fcover chip "
              f"({bare['fcov']} n={bare['fcov_n']}) -- one of our pills is "
              f"reaching it or its approach line after all")
        return 1
    print(f"  2 OK: fcover{{x{cov['fcov']:.2f} n={cov['fcov_n']}}} on the "
          f"covered base only")

    if bare["base"] <= 0:
        print(f"FAIL (3): the bare base's markup is {bare['base']} -- nothing "
              f"to compare the discount against")
        return 1
    ratio = cov["base"] / bare["base"]
    if abs(ratio - EXPECT_MULT) > 0.02:
        print(f"FAIL (3): markup ratio covered/bare = {cov['base']:.1f}/"
              f"{bare['base']:.1f} = {ratio:.3f}, expected {EXPECT_MULT:.2f}. "
              f"The discount is not landing cleanly on the engage half.")
        return 1
    print(f"  3 OK: markup {cov['base']:.1f} / {bare['base']:.1f} = "
          f"{ratio:.2f} = ATTACK_BASE_FRIENDLY_COVER_MULT^2")

    # ── 4. and it actually wins when the drive is comparable ──────────
    fair = [(t, r) for (t, r) in both
            if abs(r[BASE_COVERED]["path"] - r[BASE_BARE]["path"]) <= PATH_TOL]
    print(f"  {len(fair)}/{len(both)} of those lines have the two path costs "
          f"within {PATH_TOL}")
    won = [(t, r) for (t, r) in fair if r[BASE_COVERED]["winner"]]
    if not fair:
        print(f"FAIL (4): the tank never scored the two bases from comparable "
              f"distances, so the arena proved nothing about the discount. "
              f"Path spreads seen: "
              f"{[round(r[BASE_COVERED]['path'] - r[BASE_BARE]['path']) for _, r in both][:10]}")
        return 1
    if not won:
        t, r = fair[0]
        print(f"FAIL (4): the covered base never won pool 7 on a fair tick. "
              f"e.g. t={t}: covered cand={r[BASE_COVERED]['cand']:.0f} "
              f"(path {r[BASE_COVERED]['path']:.0f}) vs bare "
              f"cand={r[BASE_BARE]['cand']:.0f} "
              f"(path {r[BASE_BARE]['path']:.0f})")
        return 1
    t, r = won[0]
    print(f"  4a OK: covered base wins pool 7 at t={t} "
          f"({r[BASE_COVERED]['cand']:.0f} vs {r[BASE_BARE]['cand']:.0f}, "
          f"paths {r[BASE_COVERED]['path']:.0f} / {r[BASE_BARE]['path']:.0f}) "
          f"-- {len(won)}/{len(fair)} fair lines")

    # ...and the row the competition actually saw was not charged twice.
    finals = FINAL_RE.findall(text)
    if not finals:
        print("FAIL (4): no attack_base FINAL_SCORES/desc row to check the "
              "finalize step against.")
        return 1
    mismatched = []
    for (bid, bx, by, cost, cand, path, stale, base, threat) in finals:
        if abs(float(cost) - float(cand)) > 1.0:
            mismatched.append((bid, bx, by, cost, cand))
    if mismatched:
        m = mismatched[0]
        print(f"FAIL (4): {len(mismatched)}/{len(finals)} attack_base winner "
              f"rows report cost != cand -- base#{m[0]}@({m[1]},{m[2]}) "
              f"cost={m[3]} cand={m[4]}. finalize is charging the markup and "
              f"the threat a second time.")
        return 1
    f0 = finals[0]
    print(f"  4b OK: {len(finals)} attack_base winner row(s), cost == cand on "
          f"all of them (e.g. base#{f0[0]}@({f0[1]},{f0[2]}) cost={f0[3]} "
          f"cand={f0[4]} = path {f0[5]} + stale {f0[6]} + base {f0[7]} + "
          f"threat {f0[8]})")

    print("PASS: pool 7 charges its markup once, and a base under our own "
          "pillboxes is priced as the cheaper errand it is.")
    return 0


def main():
    ticks = 2000
    build = DEFAULT_BUILD
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    try:
        sys.exit(run(ticks, build))
    except subprocess.TimeoutExpired:
        print("FAIL: run timed out")
        sys.exit(1)


if __name__ == "__main__":
    main()
