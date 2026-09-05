#!/usr/bin/env python3
"""
A CONTESTED pill take turns blitzers into suiciders -- ALL of them when the
party is big, exactly ONE of them when it is only two tanks (GoalHunter 1.7,
author's rule of 2026-09-05).

WHAT IS UNDER TEST (constants.lua BLITZ_CONTESTED_RANGE /
BLITZ_CONTESTED_ALL_SUICIDERS, squad.lua blitz_contested_enemy /
blitz_designate_all_suiciders, attack.lua blitz_contested_check at both GO
sites and on the commander's replans, comms.lua's `bsu ... c` verb)

    The take of a pill is CONTESTED when a live HOSTILE TANK is seen within
    BLITZ_CONTESTED_RANGE (9) tiles, euclidean, of that pill.  That is the take
    that usually gets undone -- we kill the pill, the defender's LGM walks
    straight back out and repairs it while our survivors reload and back off --
    so on a contested take the commander designates blitzers as temporary
    pill_suiciders, regardless of BLITZ_MIN_SUICIDERS (which is 0 by default,
    i.e. nothing else designates anybody).  HOW MANY depends on the party:

      party >= BLITZ_CONTESTED_ALL_MIN_PARTY (3)   EVERY member, commander
                                                   included.
      party <  that (a two-tank take)              only
                                                   BLITZ_CONTESTED_SMALL_PARTY_SUICIDERS
                                                   (1) of them, soldiers first,
                                                   so the commander survives to
                                                   finish the pill.

THREE ARENAS (tests/generate_blitz_contested_map.py) on one piece of terrain.
A and B differ in ONE TILE -- where the enemy tank is parked.  A and C differ
only in PARTY SIZE -- same enemy tile, two attackers instead of three.  Every
arena has one enemy-owned 15 HP pill (a HARD take, so a blitz commander is
elected and a call opens) and one motionless enemy on an unreachable island.

  A  CONTESTED, BIG PARTY (three attackers with `blitz=3/4`, enemy 7.0 tiles
     from the pill)
     A1 the commander fired a real GO (BLITZ_GO / BLITZ_GO_ENROUTE);
     A2 it logged BLITZ_CONTESTED naming an enemy inside
        BLITZ_CONTESTED_RANGE, at or after that GO, on the same pill, and the
        line reads `party=3 -> all suiciders`;
     A3 ALL THREE attackers -- the commander included -- show
        `blitz_suicider=true ... reason=blitz_contested` with
        `src=blitz_contested` on their [role] line, and each is either the
        commander that self-designated or a bot that received `bsu ... c`;
     A4 the designation is TEMPORARY: every designee later reverts with
        `blitz_suicider=false reason=<one of the known expiry reasons>`.

  C  CONTESTED, SMALL PARTY (TWO attackers with `blitz=2/4`, the SAME enemy
     tile as A -- 7.0 tiles from the pill)
     C1 the commander fired a real GO with party=2;
     C2 it logged exactly the small-party form, `party=2 -> 1 suicider
        (1 designated)`, and the designee is a SOLDIER -- the name list never
        contains `self`;
     C3 that soldier, and only it, shows `blitz_suicider=true ...
        reason=blitz_contested`, arriving on a `bsu ... c` addressed to it;
     C4 the COMMANDER never became a suicider of any kind -- which is the
        whole point of the rule: a two-tank take keeps one tank alive.

  B  CONTROL (the same enemy moved to 13.9 tiles from the pill)
     B1 a GO fired here too, so the arena really did run the same experiment;
     B2 the enemy was VISIBLE from the take the whole time -- asserted from the
        attackers' own ENGINE_DUMP object lists, so "no BLITZ_CONTESTED" cannot
        be explained by the bots never seeing it -- and every sighting was
        further than BLITZ_CONTESTED_RANGE from the pill;
     B3 NO BLITZ_CONTESTED line anywhere;
     B4 nobody ever became a suicider at all (BLITZ_MIN_SUICIDERS is 0), and
        every BLITZ_GO line reads contested{no}.

Usage: python blitz_contested_test.py [A|B|C] [--ticks N] [--build DIR]
                                      [--seed N] [--no-asap]
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
FOE_BRAIN = HERE / "brains" / "idle.lua"
sys.path.insert(0, str(HERE))
from generate_blitz_contested_map import (   # noqa: E402
    VARIANTS, CONTESTED, PILL, FOE_SPAWN, BLITZ_CONTESTED_RANGE, BLITZ_PARTY,
    BLITZ_CONTESTED_ALL_MIN_PARTY, BLITZ_CONTESTED_SMALL_PARTY_SUICIDERS,
    BRAIN_VIEW_HALF, PILL_HP, allies, foe_player, n_allies, spawns,
    euclid, cheb)

PORTS = {"A": 50232, "B": 50233, "C": 50234}
SEEDS = {"A": 42, "B": 42, "C": 42}
# ENGINE ticks.  The brain thinks every other one, so this is ~1250 brain
# ticks.  Measured on this arena (seed 42): GO at brain t=310, the pill dead
# and every designation expired by brain t=479 -- so ~2.5x the whole
# experiment, which keeps each arena well under a minute headless.
DEFAULT_TICKS = 2500
def blitz_token(variant):
    return "blitz=%d/%d" % BLITZ_PARTY[variant]

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so all of
# these are used with re.search, never re.match.
TICK_RE = re.compile(r"^===TICK (\d+)===", re.M)
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")

# attack.lua, the two GO sites that designate.  Both carry the contested label.
GO_RE = re.compile(
    r"BLITZ_GO t=(\d+) ready=\d+/\d+ party=(\d+)/(\d+) .*?contested\{([^}]*)\} -> (\S+)")
GO_ENROUTE_RE = re.compile(
    r"BLITZ_GO_ENROUTE t=(\d+) set_inwait=(\d+) .*?contested\{([^}]*)\}")
# squad.lua blitz_designate_all_suiciders -- the line this whole test is about.
# Two shapes, one line: `-> all suiciders` (party >= ALL_MIN_PARTY) and
# `-> N suicider(s)` (the small-party rule).  The optional trailing
# `upgrade=small->all` marks a party that GREW past ALL_MIN_PARTY mid-take.
CONTESTED_RE = re.compile(
    r"BLITZ_CONTESTED t=(\d+) pill=#(\d+) enemy=p(\d+) dist=([\d.]+) "
    r"party=(\d+) -> (all suiciders|\d+ suiciders?) \((\d+) designated\) "
    r"designate=\[(.*?)\](?: upgrade=(\S+))?")
# squad.lua's role resolution, one line per CHANGE of is_pill_suicider.
ROLE_RE = re.compile(
    r"\[role\] t=(\d+) suicider=(\w+) src=(\S+) harasser=\w+ forced=\S+ "
    r"suicider_map=\S+(.*)$", re.M)
SU_ON_RE = re.compile(r"blitz_suicider=true pill=#(\d+) by=p(\d+) reason=(\S+)")
SU_OFF_RE = re.compile(r"blitz_suicider=false reason=(\S+)")
# comms.lua -- the designation as it arrived on the wire.
RX_RE = re.compile(
    r"BLITZ_RX bsu from p(\d+) pill=(\d+) -> p(\d+) why=(\w+)( \(US\))? t=(\d+)")
# init.lua's per-tick raw BrainInfo dump: ty0 = OBJECT_TANK, info bit 0 =
# OBJECT_HOSTILE.  This is the ENGINE's own answer to "could this bot see it".
OBJ_RE = re.compile(r"ty0#(\d+)@\((\d+),(\d+)\)info=0x([0-9A-Fa-f]+)")

# squad.lua update(): every reason a blitz-suicider designation can end.
EXPIRY_REASONS = {"noblitz", "pill_gone", "timeout", "left_take",
                  "commander_gone", "death_respawn"}


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    root = build_dir / "debug_sessions"
    if not root.exists():
        return None
    cands = [d for d in root.iterdir() if d.is_dir() and d.name.endswith("_" + label)]
    return max(cands, key=lambda d: d.stat().st_mtime) if cands else None


def play(variant, ticks, build_dir, seed):
    """Run one arena; return {bot: print2 text} for the three attackers."""
    mapfile = HERE / f"blitz_contested_{variant}.map"
    final = HERE / f"blitz_contested_{variant}_final.json"
    stderr = HERE / f"blitz_contested_{variant}_stderr.txt"
    # Short label on purpose: the recorder truncates the session directory name.
    label = f"blitzcon_{variant}_test"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable, str(HERE / "generate_blitz_contested_map.py"),
                    variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    # Every attacker gets the same party-size token, and its MIN is the number
    # of attackers, so no GO path fires until the WHOLE party is in and the GO
    # under test really is a party-of-N one.
    n_al = n_allies(variant)
    arg = f"{BRAIN}[{blitz_token(variant)}]"
    init = ",".join([f"{b}={arg}" for b in allies(variant)]
                    + [f"{foe_player(variant)}={FOE_BRAIN}"])
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby",
           # OPEN: full 40/40 loadout, so the party can actually shoot a 15 HP
           # pill down, and it leaves -teams alone (TOURNAMENT would override
           # it).  "<N>,1" is the contiguous-block form: bots 0..N-1 are the
           # allied party, bot N is the enemy.
           "-gametype", "open", "-teams", f"{n_al},1",
           "-bots", str(n_al + 1), "-brain", str(BRAIN),
           "-bot-init", init,
           "-allow-unsafe-brains",
           # yesfull: the whole arena is known from tick 0 -- the test is about
           # what happens at GO, not about finding the pill.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", str(seed), "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        print("FAIL: no debug session produced (is the cwd on a drive with "
              ">50 GB free? -brain-debug silently records nothing otherwise)")
        return None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed - see {crashes[0]}")
        print(Path(crashes[0]).read_text(errors="ignore")[:1500])
        return None
    logs = {}
    for b in allies(variant):
        log = sess / f"print2_bot{b}.log"
        if not log.exists():
            print(f"FAIL: no print2_bot{b}.log under {sess}")
            return None
        logs[b] = log.read_text(encoding="utf-8", errors="replace")
    print(f"  session: {sess.name}")
    return logs


def sanity(logs):
    for b, text in logs.items():
        for line in text.splitlines():
            if LUA_ERR_RE.search(line):
                print(f"FAIL: Lua error in bot{b}'s print2 log: {line.strip()}")
                return False
        if not TICK_RE.search(text):
            print(f"FAIL: bot{b} never thought (no ===TICK=== markers)")
            return False
    return True


def collect(text):
    """Everything the checks need out of one bot's log."""
    out = {"go": [], "contested": [], "su_on": [], "su_off": [], "rx": [],
           "foe_sightings": []}
    for m in GO_RE.finditer(text):
        out["go"].append(dict(t=int(m.group(1)), party=int(m.group(2)),
                              bmin=int(m.group(3)), contested=m.group(4),
                              sub=m.group(5), kind="GO"))
    for m in GO_ENROUTE_RE.finditer(text):
        out["go"].append(dict(t=int(m.group(1)), party=int(m.group(2)),
                              bmin=0, contested=m.group(3),
                              sub="charge", kind="GO_ENROUTE"))
    out["go"].sort(key=lambda g: g["t"])
    for m in CONTESTED_RE.finditer(text):
        out["contested"].append(dict(
            t=int(m.group(1)), pid=int(m.group(2)), enemy=int(m.group(3)),
            dist=float(m.group(4)), party=int(m.group(5)), rule=m.group(6),
            n=int(m.group(7)), names=m.group(8).split(), upgrade=m.group(9)))
    for m in ROLE_RE.finditer(text):
        t, is_su, src, extra = int(m.group(1)), m.group(2), m.group(3), m.group(4)
        on = SU_ON_RE.search(extra)
        off = SU_OFF_RE.search(extra)
        if is_su == "true":
            out["su_on"].append(dict(t=t, src=src,
                                     pid=int(on.group(1)) if on else None,
                                     by=int(on.group(2)) if on else None,
                                     reason=on.group(3) if on else None,
                                     line=m.group(0).strip()))
        else:
            out["su_off"].append(dict(t=t, src=src,
                                      reason=off.group(1) if off else None,
                                      line=m.group(0).strip()))
    for m in RX_RE.finditer(text):
        out["rx"].append(dict(frm=int(m.group(1)), pid=int(m.group(2)),
                              to=int(m.group(3)), why=m.group(4),
                              us=bool(m.group(5)), t=int(m.group(6))))
    # ENGINE_DUMP: distinct hostile-tank tiles this bot was handed by the
    # engine.  Deduped -- the idler never moves, so one entry is the norm.
    seen = set()
    for m in OBJ_RE.finditer(text):
        if int(m.group(4), 16) & 1:          # OBJECT_HOSTILE
            seen.add((int(m.group(1)), int(m.group(2)), int(m.group(3))))
    out["foe_sightings"] = sorted(seen)
    return out


def summarise(data, AL):
    for b in AL:
        d = data[b]
        print(f"  bot{b}: {len(d['go'])} GO, {len(d['contested'])} "
              f"BLITZ_CONTESTED, {len(d['su_on'])} suicider-on, "
              f"{len(d['su_off'])} suicider-off, {len(d['rx'])} bsu rx, "
              f"{len(d['foe_sightings'])} distinct hostile-tank tile(s) seen")


def foe_tiles(data, AL):
    """Every distinct hostile-tank tile any attacker was handed, with its
    distance from the pill."""
    out = {}
    for b in AL:
        for (tid, x, y) in data[b]["foe_sightings"]:
            out[(tid, x, y)] = euclid((x, y), PILL)
    return out


# ---------------------------------------------------------------------------
# A: CONTESTED
# ---------------------------------------------------------------------------
def check_A(logs):
    AL = allies("A")
    data = {b: collect(t) for b, t in logs.items()}
    summarise(data, AL)

    # -- A1: a real GO happened.  Without one there is nothing to contest.
    gos = [(b, g) for b in AL for g in data[b]["go"]]
    if not gos:
        print("FAIL: no BLITZ_GO anywhere -- the three attackers never formed "
              "a blitz on the pill, so nothing in this arena is under test")
        return 1
    cmdrs = sorted({b for b, _ in gos})
    b0, g0 = gos[0]
    print(f"  A1: {len(gos)} GO line(s) from bot(s) {cmdrs} "
          f"(first: bot{b0} {g0['kind']} t={g0['t']} party={g0['party']} "
          f"-> {g0['sub']})")

    # -- A2: the commander declared the take contested, naming an enemy inside
    #        BLITZ_CONTESTED_RANGE, at or after its own GO.
    cons = [(b, c) for b in AL for c in data[b]["contested"]]
    if not cons:
        print(f"FAIL: no BLITZ_CONTESTED line -- the enemy is "
              f"{euclid(PILL, FOE_SPAWN['A']):.1f} tiles from the pill, well "
              f"inside BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE}), so the "
              f"take should have flipped the whole party")
        return 1
    for b, c in cons:
        if c["dist"] > BLITZ_CONTESTED_RANGE:
            print(f"FAIL: bot{b} t={c['t']} called the take contested on an "
                  f"enemy {c['dist']:.1f} tiles away, past "
                  f"BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE})")
            return 1
        if c["enemy"] != foe_player("A"):
            print(f"FAIL: bot{b} t={c['t']} named p{c['enemy']} as the "
                  f"contesting tank; the only enemy in this arena is "
                  f"p{foe_player('A')}")
            return 1
        own_go = [g for g in data[b]["go"]]
        if not own_go:
            print(f"FAIL: bot{b} logged BLITZ_CONTESTED at t={c['t']} without "
                  f"ever firing a GO -- only a commander at GO (or replanning "
                  f"during its own live take) may designate")
            return 1
        if c["t"] < min(g["t"] for g in own_go):
            print(f"FAIL: bot{b} logged BLITZ_CONTESTED at t={c['t']}, before "
                  f"its first GO at t={min(g['t'] for g in own_go)}")
            return 1
        if c["n"] <= 0:
            print(f"FAIL: bot{b} t={c['t']} declared the take contested but "
                  f"designated nobody")
            return 1
        # The party here is 3, at BLITZ_CONTESTED_ALL_MIN_PARTY, so it is the
        # ALL rule that must have fired -- not the small-party one.
        if c["party"] < BLITZ_CONTESTED_ALL_MIN_PARTY:
            print(f"FAIL: bot{b} t={c['t']} reports party={c['party']}; this "
                  f"arena runs {n_allies('A')} attackers with "
                  f"{blitz_token('A')}, so the whole party should be in the "
                  f"take before GO")
            return 1
        if c["rule"] != "all suiciders":
            print(f"FAIL: bot{b} t={c['t']} party={c['party']} (>= "
                  f"BLITZ_CONTESTED_ALL_MIN_PARTY "
                  f"{BLITZ_CONTESTED_ALL_MIN_PARTY}) took the "
                  f"{c['rule']!r} rule; a party this big designates everyone")
            return 1
        if "self" not in c["names"]:
            print(f"FAIL: bot{b} t={c['t']} designated [{' '.join(c['names'])}]"
                  f" -- the all rule includes the COMMANDER itself, which logs "
                  f"as `self`")
            return 1
    b, c = cons[0]
    print(f"  A2: bot{b} t={c['t']} pill#{c['pid']} enemy=p{c['enemy']} "
          f"dist={c['dist']:.1f} (<= {BLITZ_CONTESTED_RANGE}) "
          f"party={c['party']} -> {c['rule']} ({c['n']} designated) "
          f"designate=[{' '.join(c['names'])}]")

    # -- A3: every one of the three, the commander included, is a suicider and
    #        says the contested rule is why.
    missing = []
    for b in AL:
        on = [s for s in data[b]["su_on"] if s["reason"] == "blitz_contested"]
        if not on:
            missing.append(b)
            continue
        s = on[0]
        if s["src"] != "blitz_contested":
            print(f"FAIL: bot{b} t={s['t']} has reason=blitz_contested but "
                  f"src={s['src']!r}; the DECISION breakdown reads src, so the "
                  f"two must agree")
            return 1
        # Either it designated itself (it is the commander) or a `bsu ... c`
        # named it -- there is no third way in.
        self_designated = s["by"] == b
        got_rx = [r for r in data[b]["rx"]
                  if r["us"] and r["why"] == "contested" and r["to"] == b]
        if not self_designated and not got_rx:
            print(f"FAIL: bot{b} t={s['t']} became a contested suicider "
                  f"(by=p{s['by']}) with no `bsu ... c` addressed to it and no "
                  f"self-designation")
            return 1
        who = "self-designated (commander)" if self_designated else (
            f"bsu from p{got_rx[0]['frm']} t={got_rx[0]['t']}")
        print(f"  A3: bot{b} t={s['t']} suicider=true src={s['src']} "
              f"pill=#{s['pid']} by=p{s['by']} reason={s['reason']} -- {who}")
    if missing:
        print(f"FAIL: bot(s) {missing} never became a contested suicider; the "
              f"rule designates EVERY member of the party, commander included")
        return 1

    # -- A4: it is TEMPORARY.  Every designee lets go again, naming a reason
    #        from squad.update's list.
    for b in AL:
        first_on = min(s["t"] for s in data[b]["su_on"]
                       if s["reason"] == "blitz_contested")
        off = [s for s in data[b]["su_off"] if s["t"] > first_on]
        if not off:
            print(f"FAIL: bot{b} was still a blitz suicider at the end of the "
                  f"run -- the designation is supposed to expire with the take")
            return 1
        bad = [s for s in off if s["reason"] not in EXPIRY_REASONS]
        if bad:
            print(f"FAIL: bot{b} t={bad[0]['t']} reverted with reason="
                  f"{bad[0]['reason']!r}, which is not one of "
                  f"{sorted(EXPIRY_REASONS)}")
            return 1
        print(f"  A4: bot{b} reverted at t={off[0]['t']} "
              f"(reason={off[0]['reason']}), {off[0]['t'] - first_on} ticks "
              f"after it was designated")
    return 0


# ---------------------------------------------------------------------------
# B: CONTROL
# ---------------------------------------------------------------------------
def check_B(logs):
    AL = allies("B")
    data = {b: collect(t) for b, t in logs.items()}
    summarise(data, AL)

    # -- B1: the same experiment really did run.
    gos = [(b, g) for b in AL for g in data[b]["go"]]
    if not gos:
        print("FAIL: no BLITZ_GO in the CONTROL arena either -- with no GO, "
              "'no contested designation' proves nothing")
        return 1
    b0, g0 = gos[0]
    print(f"  B1: {len(gos)} GO line(s) (first: bot{b0} {g0['kind']} "
          f"t={g0['t']} party={g0['party']} -> {g0['sub']})")

    # -- B2: the enemy was SEEN, and every sighting was out of range.  Without
    #        this the arena would pass just as happily with no enemy at all.
    tiles = foe_tiles(data, AL)
    if not tiles:
        print("FAIL: no attacker was ever handed a hostile tank by the engine "
              "-- this arena is testing blindness, not the range gate")
        return 1
    for (tid, x, y), d in sorted(tiles.items()):
        if d <= BLITZ_CONTESTED_RANGE:
            print(f"FAIL: hostile tank p{tid} was seen at ({x},{y}), only "
                  f"{d:.1f} tiles from the pill {PILL} -- inside "
                  f"BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE}), so this "
                  f"is not a control")
            return 1
    shown = ", ".join(f"p{tid}@({x},{y}) d={d:.1f}"
                      for (tid, x, y), d in sorted(tiles.items()))
    print(f"  B2: the enemy was visible to the party and never inside range: "
          f"{shown} (BLITZ_CONTESTED_RANGE {BLITZ_CONTESTED_RANGE}, brain view "
          f"+/-{BRAIN_VIEW_HALF})")

    # -- B3: nothing declared the take contested.
    cons = [(b, c) for b in AL for c in data[b]["contested"]]
    if cons:
        b, c = cons[0]
        print(f"FAIL: bot{b} t={c['t']} logged BLITZ_CONTESTED for an enemy "
              f"{c['dist']:.1f} tiles away; the range is "
              f"{BLITZ_CONTESTED_RANGE}")
        return 1
    print("  B3: no BLITZ_CONTESTED line anywhere")

    # -- B4: and so nobody is a suicider -- BLITZ_MIN_SUICIDERS is 0, this map
    #        is not in PILL_SUICIDER_MAPS, and no token forces the role, so a
    #        single suicider anywhere is the contested rule leaking.
    for b in AL:
        if data[b]["su_on"]:
            s = data[b]["su_on"][0]
            print(f"FAIL: bot{b} t={s['t']} became a suicider "
                  f"(src={s['src']}, reason={s['reason']}) in the control "
                  f"arena: {s['line']}")
            return 1
        rx = [r for r in data[b]["rx"] if r["why"] == "contested"]
        if rx:
            print(f"FAIL: bot{b} received a contested `bsu` at t={rx[0]['t']} "
                  f"from p{rx[0]['frm']} in the control arena")
            return 1
    for b, g in gos:
        if g["contested"] != "no":
            print(f"FAIL: bot{b} {g['kind']} t={g['t']} reads "
                  f"contested{{{g['contested']}}}; the control's takes are all "
                  f"uncontested")
            return 1
    print("  B4: nobody became a suicider, no contested `bsu` was sent, and "
          "every GO line reads contested{no}")
    return 0


# ---------------------------------------------------------------------------
# C: CONTESTED, but a party of TWO -> exactly ONE suicider, and it is the soldier
# ---------------------------------------------------------------------------
def check_C(logs):
    AL = allies("C")
    data = {b: collect(t) for b, t in logs.items()}
    summarise(data, AL)

    # -- C1: a real GO happened, with the two-tank party.
    gos = [(b, g) for b in AL for g in data[b]["go"]]
    if not gos:
        print("FAIL: no BLITZ_GO anywhere -- the two attackers never formed a "
              "blitz on the pill, so nothing in this arena is under test")
        return 1
    b0, g0 = gos[0]
    print(f"  C1: {len(gos)} GO line(s) (first: bot{b0} {g0['kind']} "
          f"t={g0['t']} party={g0['party']} -> {g0['sub']})")

    # -- C2: the take was declared contested and took the SMALL-party rule --
    #        `party=2 -> 1 suicider (1 designated)` -- and the designee is a
    #        SOLDIER, never the commander (which would log as `self`).
    cons = [(b, c) for b in AL for c in data[b]["contested"]]
    if not cons:
        print(f"FAIL: no BLITZ_CONTESTED line -- the enemy is "
              f"{euclid(PILL, FOE_SPAWN['C']):.1f} tiles from the pill, well "
              f"inside BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE}); a "
              f"two-tank party still designates "
              f"{BLITZ_CONTESTED_SMALL_PARTY_SUICIDERS} of its members")
        return 1
    want = BLITZ_CONTESTED_SMALL_PARTY_SUICIDERS
    want_rule = f"{want} suicider" + ("" if want == 1 else "s")
    designees = []
    for b, c in cons:
        if c["dist"] > BLITZ_CONTESTED_RANGE:
            print(f"FAIL: bot{b} t={c['t']} called the take contested on an "
                  f"enemy {c['dist']:.1f} tiles away, past "
                  f"BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE})")
            return 1
        if c["enemy"] != foe_player("C"):
            print(f"FAIL: bot{b} t={c['t']} named p{c['enemy']} as the "
                  f"contesting tank; the only enemy here is "
                  f"p{foe_player('C')}")
            return 1
        if c["party"] != n_allies("C"):
            print(f"FAIL: bot{b} t={c['t']} reports party={c['party']}; this "
                  f"arena runs {n_allies('C')} attackers with "
                  f"{blitz_token('C')}, and party 2 is what the small-party "
                  f"rule is about")
            return 1
        if c["rule"] != want_rule:
            print(f"FAIL: bot{b} t={c['t']} party={c['party']} (< "
                  f"BLITZ_CONTESTED_ALL_MIN_PARTY "
                  f"{BLITZ_CONTESTED_ALL_MIN_PARTY}) took the {c['rule']!r} "
                  f"rule; a party this small designates {want_rule}")
            return 1
        if c["n"] != want:
            print(f"FAIL: bot{b} t={c['t']} designated {c['n']} member(s), "
                  f"not {want}: designate=[{' '.join(c['names'])}]")
            return 1
        if "self" in c["names"]:
            print(f"FAIL: bot{b} t={c['t']} designated ITSELF "
                  f"(designate=[{' '.join(c['names'])}]); the small-party rule "
                  f"picks SOLDIERS first and only falls back to the commander "
                  f"when they cannot cover the number -- with one soldier "
                  f"available they always can, and the commander has to stay "
                  f"alive to finish the pill")
            return 1
        if c["upgrade"]:
            print(f"FAIL: bot{b} t={c['t']} logged {c['upgrade']}; the party "
                  f"never grows in this arena, so nothing should upgrade")
            return 1
        pn = int(c["names"][0].lstrip("p"))
        if pn == b or pn not in AL:
            print(f"FAIL: bot{b} t={c['t']} designated p{pn}, which is not one "
                  f"of its soldiers (attackers are {list(AL)}, and bot{b} is "
                  f"the commander)")
            return 1
        designees.append((b, pn, c))
    b, pn, c = designees[0]
    print(f"  C2: bot{b} t={c['t']} pill#{c['pid']} enemy=p{c['enemy']} "
          f"dist={c['dist']:.1f} party={c['party']} -> {c['rule']} "
          f"({c['n']} designated) designate=[{' '.join(c['names'])}] "
          f"-- p{pn} is a SOLDIER, not the commander (bot{b})")

    # -- C3: that soldier, and only it, is a contested suicider, and the
    #        designation arrived on a `bsu ... c` addressed to it.
    su_bots = [x for x in AL
               if [t for t in data[x]["su_on"] if t["reason"] == "blitz_contested"]]
    expect = sorted({q for _, q, _ in designees})
    if su_bots != expect:
        print(f"FAIL: contested suiciders were bot(s) {su_bots}, expected "
              f"exactly {expect} (the designee(s) named on the "
              f"BLITZ_CONTESTED line(s))")
        return 1
    for x in su_bots:
        t = [t for t in data[x]["su_on"] if t["reason"] == "blitz_contested"][0]
        if t["src"] != "blitz_contested":
            print(f"FAIL: bot{x} t={t['t']} has reason=blitz_contested but "
                  f"src={t['src']!r}; the DECISION breakdown reads src, so the "
                  f"two must agree")
            return 1
        rx = [r for r in data[x]["rx"]
              if r["us"] and r["why"] == "contested" and r["to"] == x]
        if not rx:
            print(f"FAIL: bot{x} t={t['t']} became a contested suicider "
                  f"(by=p{t['by']}) with no `bsu ... c` addressed to it -- a "
                  f"SOLDIER can only be designated over the wire")
            return 1
        print(f"  C3: bot{x} t={t['t']} suicider=true src={t['src']} "
              f"pill=#{t['pid']} by=p{t['by']} reason={t['reason']} -- bsu "
              f"from p{rx[0]['frm']} t={rx[0]['t']}")

    # -- C4: the COMMANDER never became a suicider at all.  BLITZ_MIN_SUICIDERS
    #        is 0 and nothing else designates here, so one would be the rule
    #        leaking past the party-size gate.
    cmdrs = sorted({b for b, _, _ in designees})
    for x in cmdrs:
        if data[x]["su_on"]:
            t = data[x]["su_on"][0]
            print(f"FAIL: bot{x} commanded a two-tank contested take and still "
                  f"became a suicider at t={t['t']} (src={t['src']}, "
                  f"reason={t['reason']}): {t['line']}")
            return 1
    print(f"  C4: commander(s) {cmdrs} never became a suicider -- the two-tank "
          f"take kept a tank alive")

    # -- C5: and every panel/DECISION label says WHICH rule applied.
    labelled = [(x, g) for x in AL for g in data[x]["go"]
                if g["contested"] != "no"]
    for x, g in labelled:
        if "rule=small" not in g["contested"]:
            print(f"FAIL: bot{x} {g['kind']} t={g['t']} reads "
                  f"contested{{{g['contested']}}}; a two-tank contested take "
                  f"must label itself rule=small")
            return 1
    if labelled:
        x, g = labelled[0]
        print(f"  C5: bot{x} {g['kind']} t={g['t']} "
              f"contested{{{g['contested']}}}")
    return 0


def run_one(variant, ticks, build_dir, seed):
    foe = FOE_SPAWN[variant]
    d = euclid(PILL, foe)
    rule = ("all" if n_allies(variant) >= BLITZ_CONTESTED_ALL_MIN_PARTY
            else "small")
    verdict = ("CONTESTED, rule=" + rule) if variant in CONTESTED else "CONTROL"
    print(f"=== blitz contested / {variant} ({verdict}; "
          f"seed {seed}, {ticks} ticks; pill {PILL} hp={PILL_HP} owned by "
          f"p{foe_player(variant)}; enemy at {foe}, {d:.1f} tiles from the "
          f"pill (range {BLITZ_CONTESTED_RANGE}), view cheb {cheb(PILL, foe)} "
          f"(<= {BRAIN_VIEW_HALF}); {n_allies(variant)} attackers "
          f"{spawns(variant)} with {blitz_token(variant)})")
    logs = play(variant, ticks, build_dir, seed)
    if logs is None:
        return 1
    if not sanity(logs):
        return 1
    rc = {"A": check_A, "B": check_B, "C": check_C}[variant](logs)
    if rc == 0:
        if variant == "A":
            print("PASS: an enemy tank inside BLITZ_CONTESTED_RANGE of the "
                  "pill turned every member of a THREE-tank blitz -- commander "
                  "included -- into a temporary suicider, and the designation "
                  "expired with the take.")
        elif variant == "C":
            print("PASS: the same enemy at the same distance, but a TWO-tank "
                  "party: exactly one member was designated, it was the "
                  "soldier, and the commander stayed a normal tank.")
        else:
            print("PASS: the same enemy, plainly visible but past "
                  "BLITZ_CONTESTED_RANGE, left the take uncontested and "
                  "designated nobody.")
    return rc


def main():
    args = sys.argv[1:]
    take_asap_flag(args)
    variants = list(VARIANTS)
    if args and args[0].upper() in VARIANTS:
        variants = [args.pop(0).upper()]
    ticks = DEFAULT_TICKS
    build_dir = DEFAULT_BUILD
    seed = None
    i = 0
    while i < len(args):
        if args[i] == "--ticks" and i + 1 < len(args):
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build" and i + 1 < len(args):
            build_dir = Path(args[i + 1]); i += 2
        elif args[i] == "--seed" and i + 1 < len(args):
            seed = int(args[i + 1]); i += 2
        else:
            i += 1
    print(pacing_line())
    rc = 0
    for v in variants:
        rc |= run_one(v, ticks, build_dir, seed if seed is not None else SEEDS[v])
    return rc


if __name__ == "__main__":
    sys.exit(main())
