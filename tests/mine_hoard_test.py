#!/usr/bin/env python3
"""
MINE-HOARD EVICTION -- the pool-1 mine surcharge is waived while the base can
still supply the tank (GoalHunter 1.7, author's rule of 2026-09-06).

WHAT IS UNDER TEST (goals.lua refuel_shape, constants.lua
REFUEL_MINE_HOARD_NEEDS_SUPPLY)

    A tank PARKED on a base is charged an exponential mine-hoard staying cost:

        mines_over = mines - REFUEL_MINE_FREE(5)
        mine_cost  = REFUEL_MINE_HOARD_WEIGHT(8)
                     x (REFUEL_MINE_HOARD_BASE(1.3)^mines_over - 1)

    -- 891.64 at 23 mines.  It is an EVICTION lever, "don't sit on a pad just
    to load mines", but it used to be charged whatever the tank still needed.
    Incident 20260905_231835 bot2 t=67674: 35 armour, 20 shells, 23 mines on
    base #4 priced the pad at 48 + 891.6 = 939.6, defend_pill took the goal at
    187, and the tank left short of BOTH targets purely because of its mines --
    then off the pad the same base priced 48 again and it hopped back.

    THE RULE.  The surcharge is WAIVED (charged 0) while the tank is still
    below a target (armour < armour_target or shells < shell_target) AND the
    base it is standing on still holds REFUEL_MIN_STOCK(5) of that same supply.
    At both targets, or on a base that has run dry of everything the tank still
    needs, it is charged in full and still evicts.

THE ARENA (tests/generate_mine_hoard_map.py has the full derivation)

    A tank is staged PARKED on its own base holding armour 40 / shells 20 /
    mines 23.  Shells 20 sits exactly on SHELLS_LOW, which makes every other
    term of the refuel shape neutral -- fill 0, mult 1.00, deficit bonus 0,
    urgency 1.00, scarcity 1.00 -- so the printed final cost IS the cached cost
    and the mine surcharge is the only moving part.

    A   the pad is restocked and can still feed the tank.
    B   the pad is held at (0, 0, 90): nothing left but mines.

THE FOUR RUNS
    A    arena A, default (the fix).
    AK   arena A, cfg=REFUEL_MINE_HOARD_NEEDS_SUPPLY=false (the KEEL control).
    B    arena B, default.
    BK   arena B, keel.

CHECKS
  every run
    1. No Lua error; the bot thought; every cfg token this file passed was
       actually applied (BRAIN_INIT_ARG is a 127-char buffer and overflows
       SILENTLY -- that bug cost an afternoon on 2026-09-06).
    2. The tank spawned at the TOURNAMENT loadout (armour 40, 0 shells,
       0 mines) and reached the staged state on the pad.
    3. Every mines{} chip is arithmetically self-consistent: the printed raw
       term equals 8 x (1.3^(printed mines - 5) - 1), and the printed final
       cost equals cached - deficit, times mult, plus the charged part of the
       chip.  (Author's rule: the whole line has to be hand-computable.)
    4. The chip's waive decision matches the rule, read off the chip's own
       printed inputs: waived iff the knob is on AND (below armour target with
       base armour >= 5 OR below shell target with base shells >= 5).
  A vs AK  (the fix)
    5. Paired by tick over every tick BOTH runs are parked on the pad with the
       same armour/shells/mines: A charges nothing and AK charges the full
       891.6-shaped term, and AK's final cost is exactly A's plus that term.
    6. A never leaves the pad before reaching its shell target, and reaches it
       (engine-authoritative shell count).  AK leaves the pad while still at
       the staged 20 shells -- and the test prints the mine count and the cost
       it left at.
  B vs BK  (the safety half)
    7. Once the pad is dry (ENGINE_DUMP's own base=arm0/sh0 reading), NO chip
       is waived in either run, and both tanks leave the pad.

Usage: python mine_hoard_test.py [--ticks N] [--build DIR] [--no-asap]
                                 [--only A|AK|B|BK]
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
sys.path.insert(0, str(HERE))
from generate_mine_hoard_map import (      # noqa: E402
    BASE, FAR_BASE, OUR_PILL, BOT_SPAWN, STAGE_SHELLS, STAGE_MINES,
    SHELLS_LOW, SHELL_TARGET, ENGINE_FULL_ARMOUR, ARENA_FULL_ARMOUR,
    REFUEL_MINE_FREE, REFUEL_MINE_HOARD_BASE, REFUEL_MINE_HOARD_WEIGHT,
    REFUEL_MIN_STOCK, REFUEL_FULL_COST_MULT, BRAIN_INIT_ARG_MAX,
    FULL_STOCK, base_armour_brain, mine_cost_for, mult_for)

PORT = 50300                  # 50300+; other tests use 50124 / 50291-50294
LABEL = "mine_hoard_test"
DEFAULT_TICKS = 2600          # ENGINE ticks; the brain thinks every second one

# The arena's cfg.  See the generator header for why each token is here; the
# whole string must stay under BRAIN_INIT_ARG_MAX or the tail is dropped in
# silence.
CFG_TOKENS = [
    "cfg=REFUEL_BASELINE_SHELLS=40",
    "cfg=SHELLS_LOW=20",
    "cfg=TANK_FULL_ARMOUR=45",
]
KEEL_TOKEN = "cfg=REFUEL_MINE_HOARD_NEEDS_SUPPLY=false"

RUNS = ("A", "AK", "B", "BK")

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so these are
# used with re.search, never re.match.
DUMP_RE = re.compile(
    r"ENGINE_DUMP t=(\d+) self=\((\d+),(\d+)\) dir=\d+ spd=\d+ "
    r"arm=(\d+) sh=(\d+) mn=(\d+) .*? base=(?:arm(\d+)/sh(\d+)/mn(\d+)|none)")
# goals.lua goal_selection, pool-1 shaping block (BRAIN_DEBUG_MODE only).
SHAPE_RE = re.compile(
    r"REFUEL_SHAPE t=(\d+) base=\((\d+),(\d+)\) cached=([\d.-]+) - defic\{([\d.-]+)\} "
    r"= ([\d.-]+) x mult\{([\d.]+)\} \[fill\{([\d.]+)\} = "
    r"\(sh (\d+)-(\d+)\)/\((\d+)-(\d+)\) & \(arm (\d+)-(\d+)\)/\((\d+)-(\d+)\), min; "
    r"1 \+ fill\^2 x \([\d.]+\[FULL_MULT\]-1\) x scar\{([\d.]+)\}\]"
    # `why` carries [MINE_HOARD_WEIGHT]-style tags, so it holds ']' itself:
    # match it lazily and let the " = <final> topoff=" tail close it.
    r"(?P<mine> \+ mines\{(?P<head>[^}]*)\} \[(?P<why>.*?)\])?"
    r" = (?P<final>[\d.-]+) topoff=(?P<topoff>on|off)")
# The chip's own derivation, re-parsed so the test recomputes the exponential
# from the numbers the reader sees rather than from anything it was told.
CHIP_RE = re.compile(
    r"^([\d.]+) = ([\d.]+)\[MINE_HOARD_WEIGHT\] x \(([\d.]+)\[MINE_HOARD_BASE\]"
    r"\^\((\d+) mines - (\d+)\[MINE_FREE\]\) - 1\); "
    r"(WAIVED|CHARGED)[^:]*: "
    r"arm (\d+)/(\d+) \((below|at)\), sh (\d+)/(\d+) \((below|at)\); "
    r"base under us arm=(\S+) sh=(\S+), MIN_STOCK=(\d+)$")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)(?: #(-?\d+))? \((\d+),(\d+)\)")
CFG_OK_RE = re.compile(r"\[cfg\] ([A-Z_]+)=(\S+) \(init_arg\)")
CFG_BAD_RE = re.compile(r"\[cfg\] BAD TOKEN '([^']*)'")
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")


class Dump(object):
    """One ENGINE_DUMP: what the ENGINE says, not what the brain thinks."""

    __slots__ = ("tick", "pos", "arm", "sh", "mn",
                 "base_arm", "base_sh", "base_mn")

    def __init__(self, m):
        self.tick = int(m.group(1))
        self.pos = (int(m.group(2)), int(m.group(3)))
        self.arm = int(m.group(4))
        self.sh = int(m.group(5))
        self.mn = int(m.group(6))
        self.base_arm = int(m.group(7)) if m.group(7) is not None else None
        self.base_sh = int(m.group(8)) if m.group(8) is not None else None
        self.base_mn = int(m.group(9)) if m.group(9) is not None else None

    @property
    def on_pad(self):
        return self.pos == BASE

    @property
    def pad_dry(self):
        """The pad has nothing the tank could want but mines."""
        return (self.base_arm == 0 and self.base_sh == 0)


class Chip(object):
    """The mines{...} chip, unpacked from its own printed derivation."""

    __slots__ = ("head", "why", "raw", "weight", "expbase", "mines", "free",
                 "verdict", "arm", "arm_target", "arm_rel", "sh", "sh_target",
                 "sh_rel", "b_arm", "b_sh", "min_stock", "charged")

    def __init__(self, head, why):
        m = CHIP_RE.match(why)
        if not m:
            raise ValueError("chip derivation did not parse: %r" % why)
        self.head = head
        self.why = why
        self.raw = float(m.group(1))
        self.weight = float(m.group(2))
        self.expbase = float(m.group(3))
        self.mines = int(m.group(4))
        self.free = int(m.group(5))
        self.verdict = m.group(6)
        self.arm = int(m.group(7))
        self.arm_target = int(m.group(8))
        self.arm_rel = m.group(9)
        self.sh = int(m.group(10))
        self.sh_target = int(m.group(11))
        self.sh_rel = m.group(12)
        self.b_arm = None if m.group(13) == "none" else int(m.group(13))
        self.b_sh = None if m.group(14) == "none" else int(m.group(14))
        self.min_stock = int(m.group(15))
        # The head is the number the cost path actually added.
        self.charged = 0.0 if self.head.startswith("0 of ") else float(head)

    @property
    def waived(self):
        return self.verdict == "WAIVED"


class Shape(object):
    """One REFUEL_SHAPE row, already unpacked."""

    __slots__ = ("tick", "base", "cached", "defic", "base_cost", "mult",
                 "fill", "sh", "sh_low", "sh_target", "arm", "arm_low",
                 "arm_target", "scar", "final", "topoff", "chip", "text")

    def __init__(self, m):
        self.tick = int(m.group(1))
        self.base = (int(m.group(2)), int(m.group(3)))
        self.cached = float(m.group(4))
        self.defic = float(m.group(5))
        self.base_cost = float(m.group(6))
        self.mult = float(m.group(7))
        self.fill = float(m.group(8))
        self.sh = int(m.group(9))
        self.sh_low = int(m.group(10))
        self.sh_target = int(m.group(11))
        self.arm = int(m.group(13))
        self.arm_low = int(m.group(14))
        self.arm_target = int(m.group(15))
        self.scar = float(m.group(17))
        self.final = float(m.group("final"))
        self.topoff = m.group("topoff") == "on"
        self.chip = (Chip(m.group("head"), m.group("why"))
                     if m.group("mine") else None)
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


def cfg_for(run):
    toks = list(CFG_TOKENS)
    if run.endswith("K"):
        toks.append(KEEL_TOKEN)
    return toks


def play(run, ticks, build_dir):
    """Run one of A / AK / B / BK and return (log text, cfg tokens), or None."""
    variant = run[0]
    mapfile = HERE / f"mine_hoard_{variant}.map"
    final = HERE / f"mine_hoard_{run}_final.json"
    stderr = HERE / f"mine_hoard_{run}_stderr.txt"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable,
                    str(HERE / "generate_mine_hoard_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    toks = cfg_for(run)
    cfg = ";".join(toks)
    if len(cfg) > BRAIN_INIT_ARG_MAX:
        print(f"FAIL[{run}]: the cfg string is {len(cfg)} chars and "
              f"BRAIN_INIT_ARG is a {BRAIN_INIT_ARG_MAX + 1}-byte buffer "
              f"(luabrainshandler.h) -- the tail would be dropped in silence")
        return None
    init = f"0={BRAIN}[{cfg}],1={FOE_BRAIN}"

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=f"{LABEL}_{run}")
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORT),
           "-nolobby",
           # TOURNAMENT + -ranked is the only combination that puts 0 shells
           # and 0 mines in the tank on a scenario map -- see the generator.
           "-gametype", "tournament", "-ranked",
           "-bots", "2", "-brain", str(BRAIN),
           "-bot-init", init,
           "-allow-unsafe-brains",
           # yesfull: the whole arena is known from tick 0 -- the test is about
           # what the pad COSTS, not about finding it.
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
    return log.read_text(encoding="utf-8", errors="replace"), toks


def parse(text):
    dumps = [Dump(m) for m in DUMP_RE.finditer(text)]
    shapes = []
    for m in SHAPE_RE.finditer(text):
        try:
            shapes.append(Shape(m))
        except ValueError as e:
            print(f"FAIL: {e}")
            return None, None, None
    goals = [(m.group(1), (int(m.group(3)), int(m.group(4))))
             for m in GOAL_RE.finditer(text)]
    return dumps, shapes, goals


def check_cfg(tag, text, toks):
    """Check 1b: every token we passed really landed."""
    bad = CFG_BAD_RE.findall(text)
    if bad:
        print(f"FAIL[{tag}]: the brain rejected a cfg token: {bad[0]!r}. "
              f"BRAIN_INIT_ARG is a 128-byte buffer (luabrainshandler.h) and "
              f"truncates in silence -- shorten the arena's cfg.")
        return 1
    applied = dict(CFG_OK_RE.findall(text))
    rc = 0
    for t in toks:
        name, val = t[len("cfg="):].split("=", 1)
        if applied.get(name) != val:
            print(f"FAIL[{tag}]: cfg {name}={val} was not applied "
                  f"(brain reports {applied.get(name)!r})")
            rc = 1
    return rc


def check_common(tag, text, toks, dumps, shapes):
    """Checks 1-4.  Returns rc (0 = pass)."""
    rc = check_cfg(tag, text, toks)
    if LUA_ERR_RE.search(text):
        for line in text.splitlines():
            if LUA_ERR_RE.search(line):
                print(f"FAIL[{tag}]: lua error in the brain log: {line[:200]}")
                return 1
    if not dumps:
        print(f"FAIL[{tag}]: no ENGINE_DUMP lines -- the bot never thought")
        return 1
    first, last = dumps[0], dumps[-1]
    print(f"  [{tag}] tank: t={first.tick} {first.pos} "
          f"arm={first.arm} sh={first.sh} mn={first.mn}"
          f"  ->  t={last.tick} {last.pos} arm={last.arm} sh={last.sh} "
          f"mn={last.mn}")
    if (first.arm, first.sh, first.mn) != (ENGINE_FULL_ARMOUR, 0, 0):
        print(f"FAIL[{tag}]: spawn loadout was arm={first.arm} sh={first.sh} "
              f"mn={first.mn}, wanted {ENGINE_FULL_ARMOUR}/0/0 -- the "
              f"TOURNAMENT loadout did not apply (is -ranked still on the "
              f"command line?)")
        rc = 1

    # The staged state has to actually happen, on the pad.
    staged = [d for d in dumps if d.on_pad and d.arm == ENGINE_FULL_ARMOUR
              and d.sh == STAGE_SHELLS and d.mn == STAGE_MINES]
    if not staged:
        print(f"FAIL[{tag}]: the tank never stood on the pad at "
              f"arm={ENGINE_FULL_ARMOUR}/sh={STAGE_SHELLS}/mn={STAGE_MINES} -- "
              f"the staging never completed, so nothing below means anything")
        return 1
    print(f"  [{tag}] staged on the pad at t={staged[0].tick} "
          f"({len(staged)} think(s) in that exact state)")

    if not shapes:
        print(f"FAIL[{tag}]: no REFUEL_SHAPE lines")
        return 1

    # Check 3+4: every chip re-derived, and its verdict re-decided, from its
    # OWN printed numbers.
    chipped = [s for s in shapes if s.chip]
    print(f"  [{tag}] {len(shapes)} REFUEL_SHAPE rows, {len(chipped)} with a "
          f"mines chip")
    for s in shapes:
        want = s.cached - s.defic
        if abs(want - s.base_cost) > 0.06:
            print(f"FAIL[{tag}] t={s.tick}: cached {s.cached} - defic "
                  f"{s.defic} = {want:.1f}, printed {s.base_cost}")
            return 1
        charged = s.chip.charged if s.chip else 0.0
        want = s.base_cost * s.mult + charged
        if abs(want - s.final) > 0.11:
            print(f"FAIL[{tag}] t={s.tick}: {s.base_cost} x {s.mult} + "
                  f"{charged} = {want:.1f}, printed final {s.final}\n  "
                  f"{s.text}")
            return 1
    for s in chipped:
        c = s.chip
        if (c.weight, c.expbase, c.free) != (
                float(REFUEL_MINE_HOARD_WEIGHT), REFUEL_MINE_HOARD_BASE,
                REFUEL_MINE_FREE):
            print(f"FAIL[{tag}] t={s.tick}: the chip printed constants "
                  f"{c.weight}/{c.expbase}/{c.free}, arena expects "
                  f"{REFUEL_MINE_HOARD_WEIGHT}/{REFUEL_MINE_HOARD_BASE}/"
                  f"{REFUEL_MINE_FREE}")
            return 1
        want = mine_cost_for(c.mines)
        if abs(want - c.raw) > 0.06:
            print(f"FAIL[{tag}] t={s.tick}: {REFUEL_MINE_HOARD_WEIGHT} x "
                  f"({REFUEL_MINE_HOARD_BASE}^({c.mines}-{REFUEL_MINE_FREE})"
                  f" - 1) = {want:.2f}, chip printed {c.raw}")
            return 1
        # the chip's own inputs have to agree with the rest of the row
        if (c.sh, c.sh_target, c.arm, c.arm_target) != (
                s.sh, s.sh_target, s.arm, s.arm_target):
            print(f"FAIL[{tag}] t={s.tick}: the chip says arm {c.arm}/"
                  f"{c.arm_target} sh {c.sh}/{c.sh_target}, the row says "
                  f"arm {s.arm}/{s.arm_target} sh {s.sh}/{s.sh_target}")
            return 1
        if (c.arm_rel == "below") != (c.arm < c.arm_target) or \
           (c.sh_rel == "below") != (c.sh < c.sh_target):
            print(f"FAIL[{tag}] t={s.tick}: the chip's below/at tags do not "
                  f"match its own numbers: {c.why}")
            return 1
        # THE RULE, re-decided from the chip's printed inputs.
        knob = "NEEDS_SUPPLY off" not in c.why
        supplied = ((c.arm < c.arm_target and (c.b_arm or 0) >= c.min_stock)
                    or (c.sh < c.sh_target and (c.b_sh or 0) >= c.min_stock))
        want_waived = knob and supplied
        if c.waived != want_waived:
            print(f"FAIL[{tag}] t={s.tick}: the rule says "
                  f"waived={want_waived} (knob={knob}, supplied={supplied}) "
                  f"but the chip says {c.verdict}\n  {c.why}")
            return 1
        if c.min_stock != REFUEL_MIN_STOCK:
            print(f"FAIL[{tag}] t={s.tick}: chip MIN_STOCK={c.min_stock}, "
                  f"arena expects {REFUEL_MIN_STOCK}")
            return 1
        if c.charged != (0.0 if c.waived else c.raw):
            print(f"FAIL[{tag}] t={s.tick}: chip head {c.head!r} does not "
                  f"match verdict {c.verdict} / raw {c.raw}")
            return 1
    return rc


def left_pad_tick(dumps):
    """First tick the tank is OFF the pad after it first stood on it."""
    arrived = False
    for d in dumps:
        if d.on_pad:
            arrived = True
        elif arrived:
            return d.tick
    return None


def check_A(dumps_a, shapes_a, dumps_k, shapes_k):
    """Checks 5-6: the fix, against its keel control."""
    rc = 0
    # 5. Paired by tick, over every tick both runs are parked on the pad in the
    #    same state.  Nothing else about the two runs is allowed to differ.
    da = {d.tick: d for d in dumps_a}
    dk = {d.tick: d for d in dumps_k}
    sa = {s.tick: s for s in shapes_a}
    sk = {s.tick: s for s in shapes_k}
    paired = []
    for t in sorted(set(sa) & set(sk)):
        a, k = da.get(t), dk.get(t)
        if not a or not k or not a.on_pad or not k.on_pad:
            continue
        if (a.arm, a.sh, a.mn) != (k.arm, k.sh, k.mn):
            continue
        ra, rk = sa[t], sk[t]
        if ra.chip is None or rk.chip is None:
            continue
        paired.append((t, ra, rk))
        if not ra.chip.waived:
            print(f"FAIL[A]: t={t} the default run CHARGED the surcharge on a "
                  f"pad that can still supply it:\n  {ra.chip.why}")
            return 1
        if rk.chip.waived:
            print(f"FAIL[AK]: t={t} the keel control WAIVED the surcharge -- "
                  f"cfg={KEEL_TOKEN} did not take:\n  {rk.chip.why}")
            return 1
        if abs((ra.final + rk.chip.raw) - rk.final) > 0.11:
            print(f"FAIL[A/AK]: t={t} keel final {rk.final} != default "
                  f"{ra.final} + surcharge {rk.chip.raw}")
            return 1
    # REFUEL_SHAPE only prints on a REPLAN, and the default run replans rarely
    # (it is committed to refuel), so the overlap is a handful of ticks, not
    # hundreds.  Three is the floor: fewer than that and the arena has drifted
    # and nothing was really compared.
    if len(paired) < 3:
        print(f"FAIL[A/AK]: only {len(paired)} tick(s) had both runs parked on "
              f"the pad in the same state with a mines chip -- nothing was "
              f"actually compared")
        return 1
    print(f"  [A/AK] {len(paired)} paired tick(s) on the pad, same "
          f"armour/shells/mines in both:")
    for t, ra, rk in paired:
        d = da[t]
        print(f"    t={t} arm={d.arm} sh={d.sh} mn={d.mn}: "
              f"default {ra.final} ({ra.chip.head}) vs "
              f"keel {rk.final} ({rk.chip.head}) "
              f"-- delta {rk.final - ra.final:.1f}, chip raw {rk.chip.raw}")

    # 6. Behaviour.  Engine-authoritative shells and position.
    left_a = left_pad_tick(dumps_a)
    reached = next((d for d in dumps_a if d.on_pad and d.sh >= SHELL_TARGET),
                   None)
    if reached is None:
        print(f"FAIL[A]: the default run never reached its shell target "
              f"({SHELL_TARGET}) on the pad -- best was "
              f"{max(d.sh for d in dumps_a if d.on_pad)}")
        rc = 1
    else:
        print(f"  [A] reached sh={SHELL_TARGET} on the pad at t={reached.tick}")
        if left_a is not None and left_a < reached.tick:
            print(f"FAIL[A]: the default run left the pad at t={left_a}, "
                  f"before reaching its shell target at t={reached.tick}")
            rc = 1

    left_k = left_pad_tick(dumps_k)
    if left_k is None:
        print("NOTE[AK]: the keel control never left the pad in this run -- "
              "the pricing difference is proved (check 5) but the behavioural "
              "half is not.")
    else:
        at = next((d for d in dumps_k if d.tick == left_k), None)
        row = max((s for s in shapes_k if s.tick <= left_k and s.chip),
                  key=lambda s: s.tick, default=None)
        print(f"  [AK] left the pad at t={left_k} with sh={at.sh} "
              f"mn={at.mn} (target {SHELL_TARGET}); last pad price "
              f"{row.final if row else '?'} of which "
              f"{row.chip.raw if row else '?'} was mines")
        if at.sh >= SHELL_TARGET:
            print(f"FAIL[AK]: the keel control left the pad only after "
                  f"reaching its shell target -- the control did not "
                  f"reproduce the incident")
            rc = 1
    return rc


def check_B(tag, dumps, shapes):
    """Check 7: a pad with nothing but mines still evicts, waiver or not."""
    dry = [d for d in dumps if d.pad_dry and d.on_pad]
    if not dry:
        print(f"FAIL[{tag}]: the pad never read dry under the tank "
              f"(base=arm0/sh0) -- phase 4 never happened")
        return 1
    t0 = dry[0].tick
    print(f"  [{tag}] pad read dry under the tank at t={t0}")
    pad_rows = [s for s in shapes if s.tick >= t0 and s.base == BASE]
    chipped = [s for s in pad_rows if s.chip]
    bad = [s for s in chipped if s.chip.waived]
    if bad:
        print(f"FAIL[{tag}]: the surcharge was WAIVED at t={bad[0].tick} on a "
              f"pad holding nothing the tank needs:\n  {bad[0].chip.why}")
        return 1
    if chipped:
        print(f"  [{tag}] {len(chipped)} pad row(s) priced after the pad went "
              f"dry, all CHARGED -- e.g. t={chipped[0].tick} "
              f"{chipped[0].chip.head}")
    else:
        print(f"  [{tag}] no pool-1 row was priced at the pad at all after it "
              f"went dry ({len(pad_rows)} pad row(s)). nearest_resupply_base "
              f"rejects an empty base upstream, so the eviction there is the "
              f"pre-existing empty/low_stock reject rather than the surcharge; "
              f"what this run proves is that the waiver cannot rescue a dry "
              f"pad.")
    after = [d for d in dumps if d.tick > t0]
    off = [d for d in after if not d.on_pad]
    if not off:
        print(f"FAIL[{tag}]: the tank never left the dry pad "
              f"({len(after)} think(s) after it went dry)")
        return 1
    print(f"  [{tag}] left the dry pad at t={off[0].tick} -> {off[-1].pos}")
    return 0


def main():
    args = sys.argv[1:]
    take_asap_flag(args)                # strips the flag; asap.py holds it
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
            only = args[i + 1].upper().split(","); i += 2
        else:
            i += 1
    build_dir = build_dir.resolve()

    print("MINE-HOARD EVICTION TEST -- the pool-1 mine surcharge is waived "
          "while the pad can still supply the tank")
    print(f"  pad {BASE}, second base {FAR_BASE}, pill {OUR_PILL}, spawn "
          f"{BOT_SPAWN}")
    print(f"  staged: arm {ENGINE_FULL_ARMOUR}/{ARENA_FULL_ARMOUR}, sh "
          f"{STAGE_SHELLS}/{SHELL_TARGET}, mn {STAGE_MINES} -> surcharge "
          f"{REFUEL_MINE_HOARD_WEIGHT} x ({REFUEL_MINE_HOARD_BASE}^"
          f"({STAGE_MINES}-{REFUEL_MINE_FREE}) - 1) = "
          f"{mine_cost_for(STAGE_MINES):.2f}")
    print(f"  at {STAGE_SHELLS} shells the rest of the shape is neutral: fill "
          f"0.00, mult {mult_for(STAGE_SHELLS):.2f}, deficit 0, pad armour "
          f"{base_armour_brain(FULL_STOCK)} >= MIN_STOCK {REFUEL_MIN_STOCK}")
    print(f"  build={build_dir} ticks={ticks}")
    print(pacing_line())

    runs = [r for r in RUNS if only is None or r in only]
    data = {}
    rc = 0
    for run in runs:
        print(f"\n=== run {run} "
              f"({'keel control' if run.endswith('K') else 'default'}, arena "
              f"{run[0]}) ===")
        out = play(run, ticks, build_dir)
        if out is None:
            return 1
        text, toks = out
        dumps, shapes, _goals = parse(text)
        if dumps is None:
            return 1
        rc |= check_common(run, text, toks, dumps, shapes)
        data[run] = (dumps, shapes)

    if {"A", "AK"} <= set(data):
        print("\n=== A vs AK: the fix against its keel control ===")
        rc |= check_A(data["A"][0], data["A"][1],
                      data["AK"][0], data["AK"][1])
    if "B" in data:
        print("\n=== B: a pad with nothing but mines still evicts ===")
        rc |= check_B("B", *data["B"])
    if "BK" in data:
        rc |= check_B("BK", *data["BK"])

    print("\nPASS" if rc == 0 else "\nFAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
