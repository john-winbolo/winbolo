#!/usr/bin/env python3
"""
Refuel TOP-OFF candidacy -- a tank above the low watermarks but below full is
allowed to go back for a top-off, and the existing ramp decides whether it is
worth it (GoalHunter 1.7, author's rule of 2026-09-06).

WHAT IS UNDER TEST (goals.lua refuel_need, constants.lua
REFUEL_TOPOFF_CANDIDATE)

    "20 [shells] is a good number to be like 'you're full enough, go do stuff
    unless it's worth the cost to keep recharging'."

    Before the flag a refuel row only EXISTED while the tank was at/below
    ARMOUR_LOW (15) or SHELLS_LOW (20).  With REFUEL_TOPOFF_CANDIDATE on it
    exists anywhere below the dynamic full targets, and the quadratic ramp in
    refuel_shape prices it:

        fill = min((sh - 20)/(sh_target - 20), (arm - 15)/(arm_target - 15))
        mult = 1 + fill^2 * (REFUEL_FULL_COST_MULT 8 - 1) * scarcity

    Below the low lines nothing changes: fill is 0, mult is 1.00, and the
    deficit bonus prices refuel exactly as it did before.

WHAT THE ARENA HAS TO DO (tests/generate_refuel_topoff_map.py has the details)

    The obvious "park on a pad and watch it fill" arena proves nothing: a tank
    that is ALREADY holding refuel_at_base tops off to 40 today, flag or no
    flag, because build_eval_queue keeps pool 1 alive for the active goal and
    the goal finishes at armour_target/shell_target.  The flag only decides
    what happens to a tank that has LOST the goal and is above both low lines.

    So the arena drains a base under the tank at exactly 30 shells and holds it
    empty until the tank gives up and drives off, then refills it:

        flag on  -> the 30/40 tank gets a refuel row at base x mult, wins the
                    (empty) competition, drives back and tops off to 40
        flag off -> no row at all; the tank stays on 30 shells for good

    There are NO pills anywhere -- a single live pill would turn
    build_eval_queue's combat_ahead on, widening the refuel gate to
    `shells <= SHELLS_COMBAT (30)`, and a 30-shell tank would be a candidate
    with the flag OFF too.  The bot is given cfg=REFUEL_BASELINE_SHELLS=40 so
    state.shell_target is the ordinary 40 in an arena with nothing to shoot at.

THE THREE RUNS
    A   arena A, flag ON (the default).  The tank must come back and reach 40.
    A2  arena A, cfg=REFUEL_TOPOFF_CANDIDATE=false.  The tank must never go
        back: no refuel_at_base goal after the lapse, shells never above 30.
    B   arena B, flag ON, plus an allied idle tank.  Same fill, more scarcity
        (4.00 vs 2.50, see the generator's scarcity_for), so the same fill has
        to price STRICTLY higher than in A.

CHECKS (all three runs)
    1. No Lua error in the print2 log; the tank thought; it spawned with 0
       shells (the tournament loadout applied).
    2. Phase 1: shells climb to PHASE1_SHELLS (30) and then sit there while the
       base is held empty, and the tank leaves the base tile.
    3. Every REFUEL_SHAPE row is arithmetically self-consistent: the printed
       fill is the min of the two printed ratios and the printed mult is
       1 + fill^2 * (FULL_MULT - 1) * scarcity.  (Author's rule: the whole
       formula has to be hand-computable from the line alone.)
    4. Every row tagged `topoff=on` really is a top-off: strictly above
       SHELLS_LOW and strictly below the shell target.
  A only
    5. A refuel_at_base goal is set AFTER the brain has DROPPED that goal (the
       empty base is blocked as base_useless), shells rise past 30 and reach
       40, and the multiplier rises with fill along the way.  The lapse, not
       the drain, is the line that matters: while refuel_at_base is installed
       pool 1 stays queued whatever the flag says.
    6. Reports why the tank finally left the pad (REFUEL_RELEASE, or the goal
       that beat refuel).
  A2 only
    7. No refuel_at_base goal after the lapse, no `topoff=on` row anywhere,
       shells never exceed the phase-1 plateau, and REFUEL_FINALIZE reports
       'NO PARTIAL' -- pool 1 is not even queued.
  B only
    8. Scarcity is strictly higher than A's, and at every fill value both runs
       priced, B's multiplier is strictly higher.

Usage: python refuel_topoff_test.py [--ticks N] [--build DIR] [--no-asap]
                                    [--only A|A2|B]
Exit 0 on pass.
"""
import os
import re
import subprocess
import sys
from pathlib import Path

try:
    sys.stdout.reconfigure(encoding="utf-8", errors="replace")
except (AttributeError, ValueError):        # pragma: no cover - old Pythons
    pass

from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
FOE_BRAIN = HERE / "brains" / "patrol_ns.lua"
ALLY_BRAIN = HERE / "brains" / "idle.lua"
sys.path.insert(0, str(HERE))
from generate_refuel_topoff_map import (      # noqa: E402
    BASE, BOT_SPAWN, PHASE1_SHELLS, SHELLS_LOW, SHELL_TARGET,
    REFUEL_FULL_COST_MULT, scarcity_for, mult_for, mdist)

PORT = 50124
LABEL = "refuel_topoff_test"
DEFAULT_TICKS = 3000          # ENGINE ticks; the brain thinks every second one

# The bot's arena config.  REFUEL_BASELINE_SHELLS pins state.shell_target at 40
# (see the generator); it is a property of the arena, not of the flag.
# 2026-09-06: the defaults moved (REFUEL_TOPOFF_CANDIDATE off, SHELLS_LOW 19),
# so the arena pins both: the test is about the flag's ON behaviour at the
# 20-shell low line its numbers were derived for.
CFG_ON = "cfg=REFUEL_BASELINE_SHELLS=40;cfg=SHELLS_LOW=20;cfg=REFUEL_TOPOFF_CANDIDATE=true"
CFG_OFF = CFG_ON + ";cfg=REFUEL_TOPOFF_CANDIDATE=false"

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so these are
# used with re.search, never re.match.
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\) dir=\d+ spd=\d+ "
    r"arm=(\d+) sh=(\d+)")
# goals.lua goal_selection, pool-1 shaping block (BRAIN_DEBUG_MODE only): the
# whole multiplier, factor by factor.
SHAPE_RE = re.compile(
    r"REFUEL_SHAPE t=(\d+) base=\((\d+),(\d+)\) cached=([\d.]+) - defic\{([\d.]+)\} "
    r"= ([\d.-]+) x mult\{([\d.]+)\} \[fill\{([\d.]+)\} = "
    r"\(sh (\d+)-(\d+)\)/\((\d+)-(\d+)\) & \(arm (\d+)-(\d+)\)/\((\d+)-(\d+)\), min; "
    r"1 \+ fill\^2 x \([\d.]+\[FULL_MULT\]-1\) x scar\{([\d.]+)\}\]"
    r"(?: \+ mines\{[\d.-]+\})? = ([\d.-]+) topoff=(on|off)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")
RELEASE_RE = re.compile(r"REFUEL_RELEASE t=(\d+) arm=(\d+)/(\d+) sh=(\d+)/(\d+)")
NOPARTIAL_RE = re.compile(r"REFUEL_FINALIZE t=(\d+) NO PARTIAL")
TICK_RE = re.compile(r"^===TICK (\d+)===", re.M)
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")


class Shape(object):
    """One REFUEL_SHAPE row, already unpacked."""

    __slots__ = ("tick", "cached", "defic", "base_cost", "mult", "fill",
                 "sh", "sh_target", "arm", "arm_target", "scar", "final",
                 "topoff", "text")

    def __init__(self, m):
        self.tick = int(m.group(1))
        self.cached = float(m.group(4))
        self.defic = float(m.group(5))
        self.base_cost = float(m.group(6))
        self.mult = float(m.group(7))
        self.fill = float(m.group(8))
        self.sh = int(m.group(9))
        self.sh_target = int(m.group(11))
        self.arm = int(m.group(13))
        self.arm_target = int(m.group(15))
        self.scar = float(m.group(17))
        self.final = float(m.group(18))
        self.topoff = m.group(19) == "on"
        self.text = m.group(0)


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    for root in (build_dir / "debug_sessions", REPO / "debug_sessions"):
        if not root.exists():
            continue
        cands = [d for d in root.iterdir()
                 if d.is_dir() and d.name.endswith("_" + label)]
        if cands:
            return max(cands, key=lambda d: d.stat().st_mtime)
    return None


def play(run, ticks, build_dir):
    """Run one of A / A2 / B and return bot 0's print2 log text, or None."""
    variant = "B" if run == "B" else "A"
    mapfile = HERE / f"refuel_topoff_{variant}.map"
    final = HERE / f"refuel_topoff_{run}_final.json"
    stderr = HERE / f"refuel_topoff_{run}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable,
                    str(HERE / "generate_refuel_topoff_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    cfg = CFG_OFF if run == "A2" else CFG_ON
    init = f"0={BRAIN}[{cfg}],1={FOE_BRAIN}"
    nbots = 2
    if variant == "B":
        init += f",2={ALLY_BRAIN}"
        nbots = 3

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=f"{LABEL}_{run}")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORT),
           "-nolobby",
           # TOURNAMENT + -ranked is the only combination that puts 0 shells in
           # the tank on a scenario map -- see the generator's header.
           "-gametype", "tournament", "-ranked",
           "-bots", str(nbots), "-brain", str(BRAIN),
           "-bot-init", init,
           "-allow-unsafe-brains",
           # yesfull: the whole arena is known from tick 0 - the test is about
           # whether a refuel row EXISTS and what it costs, not about finding
           # the base.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 5))

    sess = newest_session(build_dir, f"{LABEL}_{run}")
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed - see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return None
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return None
    print(f"  session: {sess.name}")
    return log.read_text(encoding="utf-8", errors="replace")


def tick_blocks(text):
    """[(tick, block_text)] in log order."""
    out = []
    marks = list(TICK_RE.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        out.append((int(m.group(1)), text[m.start():end]))
    return out


def parse(text):
    dumps = [(int(m.group(1)), (int(m.group(2)), int(m.group(3))),
              int(m.group(4)), int(m.group(5))) for m in DUMP_RE.finditer(text)]
    shapes = [Shape(m) for m in SHAPE_RE.finditer(text)]
    goals = []
    for tick, block in tick_blocks(text):
        for m in GOAL_RE.finditer(block):
            goals.append((tick, m.group(1), (int(m.group(3)), int(m.group(4)))))
    return dumps, shapes, goals


def plateau_tick(dumps):
    """First brain tick at which the tank is holding PHASE1_SHELLS.

    Phase 1 is over when the base has been drained under the tank: the shell
    count stops climbing and sits on PHASE1_SHELLS.
    """
    for tick, _pos, _arm, sh in dumps:
        if sh >= PHASE1_SHELLS:
            return tick
    return None


def lapse_tick(goals, after):
    """First tick after `after` at which the bot's goal is NOT refuel_at_base.

    This -- not the moment the shells stop climbing -- is the line the flag is
    about.  While refuel_at_base is still installed build_eval_queue keeps pool
    1 alive whatever the flag says, and init.lua only finishes the goal at
    armour_target/shell_target, so a tank that never lost the goal proves
    nothing.  The empty base is what makes the brain drop it (base_useless ->
    blocked); everything asserted below happens strictly after that.
    """
    for tick, kind, _pos in goals:
        if tick > after and kind != "refuel_at_base":
            return tick
    return None


def check_common(tag, dumps, shapes, goals):
    """Checks 1-4. Returns (rc, plateau_tick, lapse_tick) -- rc 0 on pass."""
    if not dumps:
        print(f"FAIL[{tag}]: no ENGINE_DUMP lines -- the bot never thought")
        return 1, None, None
    first, last = dumps[0], dumps[-1]
    print(f"  [{tag}] tank: t={first[0]} at {first[1]} arm={first[2]} "
          f"sh={first[3]} -> t={last[0]} at {last[1]} arm={last[2]} "
          f"sh={last[3]}")
    if first[3] != 0:
        print(f"FAIL[{tag}]: the tank spawned with {first[3]} shells, not 0 -- "
              "the TOURNAMENT loadout did not apply (is -ranked still on the "
              "command line?)")
        return 1, None, None

    pt = plateau_tick(dumps)
    if pt is None:
        print(f"FAIL[{tag}]: the tank never reached {PHASE1_SHELLS} shells -- "
              "phase 1 never finished, so there is nothing to test")
        return 1, None, None
    at_plateau = [d for d in dumps if d[0] >= pt and d[3] == PHASE1_SHELLS]
    if len(at_plateau) < 20:
        print(f"FAIL[{tag}]: the tank held {PHASE1_SHELLS} shells for only "
              f"{len(at_plateau)} think(s) -- the sidecar did not hold the base "
              "empty (bases.c basesUpdateStock regenerates stock on its own)")
        return 1, None, None
    left = [d for d in at_plateau if d[1] != BASE]
    if not left:
        print(f"FAIL[{tag}]: the tank never left the base tile {BASE} while the "
              "base was empty -- the arena never puts it in the off-the-pad "
              "state the flag is about")
        return 1, None, None
    lapse = lapse_tick(goals, pt)
    if lapse is None:
        print(f"FAIL[{tag}]: the bot held refuel_at_base for the whole run after "
              f"t={pt} -- the arena never made it drop the goal, and while the "
              "goal is installed pool 1 stays alive whatever the flag says")
        return 1, None, None
    print(f"  [{tag}] phase 1: {PHASE1_SHELLS} shells from t={pt}, held for "
          f"{len(at_plateau)} think(s); dropped the refuel goal at t={lapse}; "
          f"off the pad from t={left[0][0]} at {left[0][1]}")

    if not shapes:
        print(f"FAIL[{tag}]: no REFUEL_SHAPE rows -- pool 1 never priced a base")
        return 1, None, None
    for s in shapes:
        want_fill = min((s.sh - SHELLS_LOW) / float(max(1, s.sh_target - SHELLS_LOW)),
                        (s.arm - 15) / float(max(1, s.arm_target - 15)))
        want_fill = max(0.0, want_fill) if (s.sh > SHELLS_LOW and s.arm > 15) else 0.0
        if abs(want_fill - s.fill) > 5e-3:
            print(f"FAIL[{tag}]: t={s.tick} the row prints fill {s.fill:.3f} but "
                  f"its own numbers give {want_fill:.3f}: {s.text}")
            return 1, None, None
        want_mult = 1.0 + s.fill * s.fill * (REFUEL_FULL_COST_MULT - 1.0) * s.scar
        if abs(want_mult - s.mult) > 5e-3:
            print(f"FAIL[{tag}]: t={s.tick} the row prints mult {s.mult:.3f} but "
                  f"1 + {s.fill:.3f}^2 x {REFUEL_FULL_COST_MULT - 1:.2f} x "
                  f"{s.scar:.2f} = {want_mult:.3f}: {s.text}")
            return 1, None, None
        if s.topoff and not (SHELLS_LOW < s.sh < s.sh_target
                             or 15 < s.arm < s.arm_target):
            print(f"FAIL[{tag}]: t={s.tick} row is tagged topoff=on but the tank "
                  f"is not between the low line and the target: {s.text}")
            return 1, None, None
    print(f"  [{tag}] {len(shapes)} REFUEL_SHAPE row(s), every fill and "
          f"multiplier reproduced from the row's own numbers")
    return 0, pt, lapse


def run_A(text, tag="A"):
    dumps, shapes, goals = parse(text)
    rc, pt, lapse = check_common(tag, dumps, shapes, goals)
    if rc:
        return rc, shapes

    # -- 5. it went back, and it filled
    back = [g for g in goals if g[0] > lapse and g[1] == "refuel_at_base"
            and g[2] == BASE]
    if not back:
        print(f"FAIL[{tag}]: after dropping the goal at t={lapse} the bot never set a "
              f"refuel_at_base goal on {BASE} again -- with "
              "REFUEL_TOPOFF_CANDIDATE on, a 30/40 tank with nothing else to "
              "do should have gone back for the top-off")
        return 1, shapes
    rising = [d for d in dumps if d[0] > lapse and d[3] > PHASE1_SHELLS]
    if not rising:
        print(f"FAIL[{tag}]: the tank re-took the refuel goal at t={back[0][0]} "
              f"but its shells never went above {PHASE1_SHELLS}")
        return 1, shapes
    topped = [d for d in rising if d[3] >= SHELL_TARGET]
    if not topped:
        print(f"FAIL[{tag}]: shells reached only {max(d[3] for d in rising)}, "
              f"never the target {SHELL_TARGET}")
        return 1, shapes
    print(f"  [{tag}] went back at t={back[0][0]}; shells "
          + " -> ".join(f"t={d[0]}:{d[3]}"
                        for d in [d for d in dumps if d[0] >= lapse][::40][:8])
          + f" ... reached {SHELL_TARGET} at t={topped[0][0]}")

    # the ramp climbs as it fills
    tops = [s for s in shapes if s.topoff and s.tick >= lapse]
    if len(tops) < 2:
        print(f"FAIL[{tag}]: only {len(tops)} topoff=on row(s) after the lapse; "
              "nothing to show the ramp with")
        return 1, shapes
    lo, hi = tops[0], tops[-1]
    if not (hi.fill > lo.fill and hi.mult > lo.mult):
        print(f"FAIL[{tag}]: the multiplier did not climb with the fill: "
              f"t={lo.tick} fill {lo.fill:.3f} mult {lo.mult:.3f} -> "
              f"t={hi.tick} fill {hi.fill:.3f} mult {hi.mult:.3f}")
        return 1, shapes
    print(f"  [{tag}] ramp: t={lo.tick} sh={lo.sh} fill {lo.fill:.3f} "
          f"mult x{lo.mult:.3f} cost {lo.final:.1f}  ->  t={hi.tick} sh={hi.sh} "
          f"fill {hi.fill:.3f} mult x{hi.mult:.3f} cost {hi.final:.1f} "
          f"(scarcity {hi.scar:.2f})")

    # -- 6. why it left
    rel = list(RELEASE_RE.finditer(text))
    if rel:
        m = rel[-1]
        print(f"  [{tag}] left the pad FULL: REFUEL_RELEASE t={m.group(1)} "
              f"arm={m.group(2)}/{m.group(3)} sh={m.group(4)}/{m.group(5)}")
    else:
        after = [g for g in goals if g[0] > topped[0][0]
                 and g[1] != "refuel_at_base"]
        who = after[0] if after else None
        print(f"  [{tag}] left the pad PRICED OUT: the next goal was "
              + (f"{who[1]} {who[2]} at t={who[0]}" if who else "(none seen)")
              + f"; last refuel row cost {hi.final:.1f} at x{hi.mult:.3f}")
    return 0, shapes


def run_A2(text):
    dumps, shapes, goals = parse(text)
    rc, pt, lapse = check_common("A2", dumps, shapes, goals)
    if rc:
        return rc, shapes

    # -- 7. it must not go back
    back = [g for g in goals if g[0] > lapse and g[1] == "refuel_at_base"]
    if back:
        print(f"FAIL[A2]: with cfg=REFUEL_TOPOFF_CANDIDATE=false the bot still "
              f"set refuel_at_base at t={back[0][0]} {back[0][2]} after dropping "
              f"the goal at t={lapse} -- the flag did not turn the candidate off")
        return 1, shapes
    over = [d for d in dumps if d[0] > lapse and d[3] > PHASE1_SHELLS]
    if over:
        print(f"FAIL[A2]: shells rose to {over[0][3]} at t={over[0][0]}, above "
              f"the phase-1 plateau {PHASE1_SHELLS} -- with the flag off the "
              "tank has no reason to be on a pad at all")
        return 1, shapes
    tops = [s for s in shapes if s.topoff]
    if tops:
        print(f"FAIL[A2]: {len(tops)} row(s) tagged topoff=on with the flag "
              f"off, first at t={tops[0].tick}: {tops[0].text}")
        return 1, shapes
    nop = [m for m in NOPARTIAL_RE.finditer(text) if int(m.group(1)) > lapse]
    print(f"  [A2] after the lapse: no refuel goal, shells stayed at "
          f"{PHASE1_SHELLS} for {len([d for d in dumps if d[0] > lapse])} "
          f"think(s), no topoff=on row, and REFUEL_FINALIZE reported "
          f"'NO PARTIAL' (refuel not queued) on {len(nop)} cycle(s)")
    if not nop:
        print("FAIL[A2]: pool 1 was still being queued after the lapse -- "
              "expected REFUEL_FINALIZE 'NO PARTIAL' once the tank was above "
              "both low lines")
        return 1, shapes
    return 0, shapes


def compare_A_B(shapes_a, shapes_b):
    """Check 8: same fill, strictly dearer under scarcity."""
    scar_a = {s.scar for s in shapes_a}
    scar_b = {s.scar for s in shapes_b}
    if len(scar_a) != 1 or len(scar_b) != 1:
        print(f"FAIL: scarcity was not constant within a run (A {sorted(scar_a)}, "
              f"B {sorted(scar_b)}) -- the comparison would not be like for like")
        return 1
    sa, sb = scar_a.pop(), scar_b.pop()
    want_a, want_b = scarcity_for("A"), scarcity_for("B")
    if abs(sa - want_a) > 1e-6 or abs(sb - want_b) > 1e-6:
        print(f"FAIL: scarcity A={sa:.2f} B={sb:.2f}, but the arena predicts "
              f"A={want_a:.2f} B={want_b:.2f} (see generate_refuel_topoff_map."
              "scarcity_for)")
        return 1
    if not sb > sa:
        print(f"FAIL: the allied tank did not raise scarcity ({sb:.2f} vs "
              f"{sa:.2f})")
        return 1

    by_a, by_b = {}, {}
    for s in shapes_a:
        by_a.setdefault(round(s.fill, 3), s)
    for s in shapes_b:
        by_b.setdefault(round(s.fill, 3), s)
    common = sorted(set(by_a) & set(by_b))
    common = [f for f in common if f > 0.0]
    if not common:
        print(f"FAIL: A and B never priced the same fill (A {sorted(by_a)}, "
              f"B {sorted(by_b)}) -- nothing to compare")
        return 1
    for f in common:
        a, b = by_a[f], by_b[f]
        if not b.mult > a.mult:
            print(f"FAIL: at fill {f:.3f} B's multiplier x{b.mult:.3f} is not "
                  f"above A's x{a.mult:.3f}")
            return 1
    f0 = common[0] if len(common) == 1 else round(
        min(common, key=lambda f: abs(f - 0.5)), 3)
    a, b = by_a[f0], by_b[f0]
    print(f"  [A vs B] scarcity {sa:.2f} -> {sb:.2f}; at fill {f0:.3f} "
          f"(sh {a.sh}/{a.sh_target}) the row goes x{a.mult:.3f} -> x{b.mult:.3f} "
          f"(cost {a.final:.1f} -> {b.final:.1f}); "
          f"{len(common)} shared fill value(s), all dearer in B")
    print(f"  [A vs B] hand-check at fill 0.50: "
          f"A 1 + 0.5^2 x 7 x {sa:.2f} = {mult_for(PHASE1_SHELLS, sa):.3f}, "
          f"B 1 + 0.5^2 x 7 x {sb:.2f} = {mult_for(PHASE1_SHELLS, sb):.3f}")
    return 0


def main():
    args = sys.argv[1:]
    use_asap = take_asap_flag(args)     # noqa: F841 (strips the flag)
    ticks = DEFAULT_TICKS
    build_dir = DEFAULT_BUILD
    only = None
    i = 0
    while i < len(args):
        if args[i] == "--ticks" and i + 1 < len(args):
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build" and i + 1 < len(args):
            build_dir = Path(args[i + 1]); i += 2
        elif args[i] == "--only" and i + 1 < len(args):
            only = args[i + 1].upper(); i += 2
        else:
            i += 1
    print(pacing_line())
    print(f"=== refuel top-off candidacy ({ticks} engine ticks per run; base "
          f"{mdist(BOT_SPAWN, BASE)} tiles from the spawn; phase-1 plateau "
          f"{PHASE1_SHELLS} shells, target {SHELL_TARGET}, "
          f"SHELLS_LOW {SHELLS_LOW})")

    shapes = {}
    for run in ("A", "A2", "B"):
        if only and run != only:
            continue
        print(f"--- run {run}: arena {'B' if run == 'B' else 'A'}, "
              f"REFUEL_TOPOFF_CANDIDATE {'off' if run == 'A2' else 'on'}")
        text = play(run, ticks, build_dir)
        if text is None:
            return 1
        if LUA_ERR_RE.search(text):
            for line in text.splitlines():
                if LUA_ERR_RE.search(line):
                    print(f"FAIL[{run}]: Lua error in the print2 log: "
                          f"{line.strip()}")
                    return 1
        rc, sh = (run_A2(text) if run == "A2" else run_A(text, run))
        if rc:
            return rc
        shapes[run] = sh

    if "A" in shapes and "B" in shapes:
        if compare_A_B(shapes["A"], shapes["B"]):
            return 1

    print("PASS: with REFUEL_TOPOFF_CANDIDATE on, a tank above the low lines "
          "and below full gets a refuel row priced by the top-off ramp and "
          "goes back for it; with the flag off the row does not exist and the "
          "tank stays where it is; and an allied tank makes the same fill cost "
          "strictly more.")
    return 0


if __name__ == "__main__":
    sys.exit(main())
