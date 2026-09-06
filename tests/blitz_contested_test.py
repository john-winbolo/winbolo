#!/usr/bin/env python3
"""
A CONTESTED pill take designates exactly ONE blitzer a suicider -- for any
party of two or more (GoalHunter 1.7, author's rule of 2026-09-05).

THE RULE IS OFF BY DEFAULT since the evening of 2026-09-05: ten-seed benches
against stock KEEL had it LOSING even in this one-suicider form (2v2 DH-Oil Rig
KEEL 7-3, 6v6 Easter KEEL 6-3 over the nine that finished).  The mechanism was
kept rather than deleted, so arenas A, B and C opt in per bot with
`cfg=BLITZ_CONTESTED_ALL_SUICIDERS=true` and arena D checks the default.

WHAT IS UNDER TEST (constants.lua BLITZ_CONTESTED_RANGE /
BLITZ_CONTESTED_ALL_SUICIDERS / BLITZ_CONTESTED_SUICIDERS, squad.lua
blitz_contested_enemy / blitz_designate_contested, attack.lua
blitz_contested_check at both GO sites and on the commander's replans,
comms.lua's `bsu ... c` verb)

    The take of a pill is CONTESTED when a live HOSTILE TANK is seen within
    BLITZ_CONTESTED_RANGE (9) tiles, euclidean, of that pill.  That is the take
    that usually gets undone -- we kill the pill, the defender's LGM walks
    straight back out and repairs it while our survivors reload and back off --
    so on a contested take the commander designates
    BLITZ_CONTESTED_SUICIDERS (1) of the blitz a temporary pill_suicider,
    regardless of BLITZ_MIN_SUICIDERS (which is 0 by default, i.e. nothing else
    designates anybody).  SOLDIERS are picked first, uniformly at random over
    the seeded RNG, and the commander designates ITSELF only when the soldiers
    cannot cover the number -- so with a soldier available the commander stays
    a normal tank that can finish the pill and hold the ground.  The party size
    does NOT change the count: a party of three designates one, exactly as a
    party of two does.  (It used to designate EVERY blitzer; the 2026-09-05
    evening bench had that losing 7-3 in 2v2 against KEEL.)

FOUR ARENAS (tests/generate_blitz_contested_map.py) on one piece of terrain.
A and B differ in ONE TILE -- where the enemy tank is parked.  A and C differ
only in PARTY SIZE -- same enemy tile, two attackers instead of three.  Every
arena has one enemy-owned 15 HP pill (a HARD take, so a blitz commander is
elected and a call opens) and one motionless enemy on an unreachable island.

  A  CONTESTED, PARTY OF THREE (three attackers with `blitz=3/4`, enemy 7.0
     tiles from the pill)
     A1 the commander fired a real GO (BLITZ_GO / BLITZ_GO_ENROUTE);
     A2 it logged BLITZ_CONTESTED naming an enemy inside
        BLITZ_CONTESTED_RANGE, at or after that GO, on the same pill, and the
        line reads `party=3 -> 1 suicider` with ONE name in designate=[...],
        never `self`;
     A3 that ONE designee, and only it, shows `blitz_suicider=true ...
        reason=blitz_contested` with `src=blitz_contested` on its [role] line,
        arriving on a `bsu ... c` addressed to it;
     A4 the COMMANDER never became a suicider -- a bigger party does not pull
        it in;
     A5 every GO line labels the take `designated=1`;
     A6 the designation is TEMPORARY: the designee later reverts with
        `blitz_suicider=false reason=<one of the known expiry reasons>`.

  C  CONTESTED, PARTY OF TWO (TWO attackers with `blitz=2/4`, the SAME enemy
     tile as A -- 7.0 tiles from the pill)
     C1..C5 are A1..A5 with party=2: the SAME single designation, which is the
     point of running both -- the count does not follow the party size.

  D  DEFAULT-OFF CONTROL (byte-identical ground to A, and the SAME three
     attackers on the SAME `blitz=3/4` -- but WITHOUT the
     `cfg=BLITZ_CONTESTED_ALL_SUICIDERS=true` token A, B and C carry)
     D1 a GO fired here too, so the arena really did run the same experiment;
     D2 the enemy was inside BLITZ_CONTESTED_RANGE of the pill and VISIBLE --
        this take IS contested by geometry, which is what makes "nobody was
        designated" mean something;
     D3 NO BLITZ_CONTESTED line anywhere;
     D4 nobody ever became a suicider, and every BLITZ_GO line reads
        contested{off} -- NOT contested{no}. The two are different facts and
        squad.blitz_contested_label keeps them apart: "no" is the rule running
        and finding nobody in range (that is arena B), "off" is the rule never
        running at all.

  B  CONTROL (the same enemy moved to 13.9 tiles from the pill)
     B1 a GO fired here too, so the arena really did run the same experiment;
     B2 the enemy was VISIBLE from the take the whole time -- asserted from the
        attackers' own ENGINE_DUMP object lists, so "no BLITZ_CONTESTED" cannot
        be explained by the bots never seeing it -- and every sighting was
        further than BLITZ_CONTESTED_RANGE from the pill;
     B3 NO BLITZ_CONTESTED line anywhere;
     B4 nobody ever became a suicider at all (BLITZ_MIN_SUICIDERS is 0), and
        every BLITZ_GO line reads contested{no}.

Usage: python blitz_contested_test.py [A|B|C|D] [--ticks N] [--build DIR]
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
    VARIANTS, CONTESTED, RULE_ENABLED, PILL, FOE_SPAWN,
    BLITZ_CONTESTED_RANGE, BLITZ_PARTY, BLITZ_CONTESTED_SUICIDERS,
    BRAIN_VIEW_HALF, PILL_HP, allies, foe_player, n_allies, spawns,
    euclid, cheb)

PORTS = {"A": 50232, "B": 50233, "C": 50234, "D": 50235}
SEEDS = {"A": 42, "B": 42, "C": 42, "D": 42}

# The contested rule is OFF by default (constants.lua, 2026-09-05: ten-seed
# benches against stock KEEL had it losing 7-3 in 2v2 and 6-3 in 6v6 even in its
# one-suicider form).  The mechanism was kept, not deleted, so the arenas that
# TEST it opt in per bot with this token; variant D deliberately does not, and
# checks what the default now does.
RULE_TOKEN = ";cfg=BLITZ_CONTESTED_ALL_SUICIDERS=true"
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
# squad.lua blitz_designate_contested -- the line this whole test is about.
# `-> N suicider(s)` is BLITZ_CONTESTED_SUICIDERS, the number the take WANTS;
# designate=[...] is who it newly designated, which is empty (with a trailing
# `(already covered by ...)`) when a member was a suicider already.
CONTESTED_RE = re.compile(
    r"BLITZ_CONTESTED t=(\d+) pill=#(\d+) enemy=p(\d+) dist=([\d.]+) "
    r"party=(\d+) -> (\d+) suiciders? designate=\[(.*?)\]"
    r"(?: \(already covered by (.*?)\))?")
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
    arg = (f"{BRAIN}[{blitz_token(variant)}"
           f"{RULE_TOKEN if variant in RULE_ENABLED else ''}]")
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
        names = m.group(7).split()
        out["contested"].append(dict(
            t=int(m.group(1)), pid=int(m.group(2)), enemy=int(m.group(3)),
            dist=float(m.group(4)), party=int(m.group(5)),
            want=int(m.group(6)), n=len(names), names=names,
            covered=(m.group(8) or "").split()))
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
# A and C: CONTESTED.  Identical checks either side of the party size, because
# the rule does not vary with it: ONE designee, and it is a soldier.
# ---------------------------------------------------------------------------
def check_contested(variant, logs, tag):
    """The shared contested-arena checks.  Returns (data, designees) or None.

    `designees` is [(commander bot, designated bot, the BLITZ_CONTESTED line)].
    """
    AL = allies(variant)
    want = BLITZ_CONTESTED_SUICIDERS
    data = {b: collect(t) for b, t in logs.items()}
    summarise(data, AL)

    # -- 1: a real GO happened.  Without one there is nothing to contest.
    gos = [(b, g) for b in AL for g in data[b]["go"]]
    if not gos:
        print(f"FAIL: no BLITZ_GO anywhere -- the {n_allies(variant)} "
              f"attackers never formed a blitz on the pill, so nothing in this "
              f"arena is under test")
        return None
    b0, g0 = gos[0]
    print(f"  {tag}1: {len(gos)} GO line(s) from bot(s) "
          f"{sorted({b for b, _ in gos})} (first: bot{b0} {g0['kind']} "
          f"t={g0['t']} party={g0['party']} -> {g0['sub']})")

    # -- 2: the commander declared the take contested, naming an enemy inside
    #       BLITZ_CONTESTED_RANGE, at or after its own GO, and designated
    #       exactly BLITZ_CONTESTED_SUICIDERS of the party -- a SOLDIER, which
    #       is what a name list without `self` means.
    cons = [(b, c) for b in AL for c in data[b]["contested"]]
    if not cons:
        print(f"FAIL: no BLITZ_CONTESTED line -- the enemy is "
              f"{euclid(PILL, FOE_SPAWN[variant]):.1f} tiles from the pill, "
              f"well inside BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE}), "
              f"so the take should have designated {want} of the party")
        return None
    designees = []
    for b, c in cons:
        if c["dist"] > BLITZ_CONTESTED_RANGE:
            print(f"FAIL: bot{b} t={c['t']} called the take contested on an "
                  f"enemy {c['dist']:.1f} tiles away, past "
                  f"BLITZ_CONTESTED_RANGE ({BLITZ_CONTESTED_RANGE})")
            return None
        if c["enemy"] != foe_player(variant):
            print(f"FAIL: bot{b} t={c['t']} named p{c['enemy']} as the "
                  f"contesting tank; the only enemy in this arena is "
                  f"p{foe_player(variant)}")
            return None
        own_go = data[b]["go"]
        if not own_go:
            print(f"FAIL: bot{b} logged BLITZ_CONTESTED at t={c['t']} without "
                  f"ever firing a GO -- only a commander at GO (or replanning "
                  f"during its own live take) may designate")
            return None
        if c["t"] < min(g["t"] for g in own_go):
            print(f"FAIL: bot{b} logged BLITZ_CONTESTED at t={c['t']}, before "
                  f"its first GO at t={min(g['t'] for g in own_go)}")
            return None
        if c["party"] != n_allies(variant):
            print(f"FAIL: bot{b} t={c['t']} reports party={c['party']}; this "
                  f"arena runs {n_allies(variant)} attackers with "
                  f"{blitz_token(variant)}, so the whole party should be in "
                  f"the take before GO")
            return None
        if c["want"] != want:
            print(f"FAIL: bot{b} t={c['t']} party={c['party']} wanted "
                  f"{c['want']} suicider(s); BLITZ_CONTESTED_SUICIDERS is "
                  f"{want}, and the party size does not change it")
            return None
        if c["n"] > want:
            print(f"FAIL: bot{b} t={c['t']} designated {c['n']} member(s), "
                  f"more than {want}: designate=[{' '.join(c['names'])}]")
            return None
        if "self" in c["names"]:
            print(f"FAIL: bot{b} t={c['t']} designated ITSELF "
                  f"(designate=[{' '.join(c['names'])}]); the rule picks "
                  f"SOLDIERS first and only falls back to the commander when "
                  f"they cannot cover the number -- with {c['party'] - 1} "
                  f"soldier(s) available they always can, and the commander "
                  f"has to stay alive to finish the pill")
            return None
        for name in c["names"]:
            pn = int(name.lstrip("p"))
            if pn == b or pn not in AL:
                print(f"FAIL: bot{b} t={c['t']} designated p{pn}, which is not "
                      f"one of its soldiers (attackers are {list(AL)}, and "
                      f"bot{b} is the commander)")
                return None
            designees.append((b, pn, c))
    # One designation for the whole arena, no matter how many lines it took.
    picked = sorted({pn for _, pn, _ in designees})
    if len(picked) != want:
        print(f"FAIL: the arena designated {len(picked)} member(s) {picked} "
              f"in total; BLITZ_CONTESTED_SUICIDERS is {want}")
        return None
    b, pn, c = designees[0]
    print(f"  {tag}2: bot{b} t={c['t']} pill#{c['pid']} enemy=p{c['enemy']} "
          f"dist={c['dist']:.1f} (<= {BLITZ_CONTESTED_RANGE}) "
          f"party={c['party']} -> {c['want']} suicider "
          f"designate=[{' '.join(c['names'])}] -- p{pn} is a SOLDIER, not the "
          f"commander (bot{b})")

    # -- 3: that designee, and only it, is a contested suicider, and the
    #       designation arrived on a `bsu ... c` addressed to it.
    su_bots = [x for x in AL
               if [t for t in data[x]["su_on"] if t["reason"] == "blitz_contested"]]
    if su_bots != picked:
        print(f"FAIL: contested suiciders were bot(s) {su_bots}, expected "
              f"exactly {picked} (the designee(s) named on the "
              f"BLITZ_CONTESTED line(s))")
        return None
    for x in su_bots:
        s = [t for t in data[x]["su_on"] if t["reason"] == "blitz_contested"][0]
        if s["src"] != "blitz_contested":
            print(f"FAIL: bot{x} t={s['t']} has reason=blitz_contested but "
                  f"src={s['src']!r}; the DECISION breakdown reads src, so the "
                  f"two must agree")
            return None
        rx = [r for r in data[x]["rx"]
              if r["us"] and r["why"] == "contested" and r["to"] == x]
        if not rx:
            print(f"FAIL: bot{x} t={s['t']} became a contested suicider "
                  f"(by=p{s['by']}) with no `bsu ... c` addressed to it -- a "
                  f"SOLDIER can only be designated over the wire")
            return None
        print(f"  {tag}3: bot{x} t={s['t']} suicider=true src={s['src']} "
              f"pill=#{s['pid']} by=p{s['by']} reason={s['reason']} -- bsu "
              f"from p{rx[0]['frm']} t={rx[0]['t']}")

    # -- 4: the COMMANDER never became a suicider at all.  BLITZ_MIN_SUICIDERS
    #       is 0 and nothing else designates here, so one would be the rule
    #       pulling in the tank that has to survive the take.
    cmdrs = sorted({x for x, _, _ in designees})
    for x in cmdrs:
        if data[x]["su_on"]:
            s = data[x]["su_on"][0]
            print(f"FAIL: bot{x} commanded a contested take of "
                  f"{n_allies(variant)} and still became a suicider at "
                  f"t={s['t']} (src={s['src']}, reason={s['reason']}): "
                  f"{s['line']}")
            return None
    print(f"  {tag}4: commander(s) {cmdrs} never became a suicider -- the take "
          f"kept every tank but one a normal tank")

    # -- 5: and every panel/DECISION label reports the size of the designation.
    labelled = [(x, g) for x in AL for g in data[x]["go"]
                if g["contested"] != "no"]
    for x, g in labelled:
        if f"designated={want}" not in g["contested"]:
            print(f"FAIL: bot{x} {g['kind']} t={g['t']} reads "
                  f"contested{{{g['contested']}}}; a contested take must label "
                  f"itself designated={want}")
            return None
    if labelled:
        x, g = labelled[0]
        print(f"  {tag}5: bot{x} {g['kind']} t={g['t']} "
              f"contested{{{g['contested']}}}")
    return data, designees


def check_A(logs):
    got = check_contested("A", logs, "A")
    if got is None:
        return 1
    data, designees = got

    # -- A6: it is TEMPORARY.  The designee lets go again, naming a reason from
    #        squad.update's list.
    for x in sorted({pn for _, pn, _ in designees}):
        first_on = min(s["t"] for s in data[x]["su_on"]
                       if s["reason"] == "blitz_contested")
        off = [s for s in data[x]["su_off"] if s["t"] > first_on]
        if not off:
            print(f"FAIL: bot{x} was still a blitz suicider at the end of the "
                  f"run -- the designation is supposed to expire with the take")
            return 1
        bad = [s for s in off if s["reason"] not in EXPIRY_REASONS]
        if bad:
            print(f"FAIL: bot{x} t={bad[0]['t']} reverted with reason="
                  f"{bad[0]['reason']!r}, which is not one of "
                  f"{sorted(EXPIRY_REASONS)}")
            return 1
        print(f"  A6: bot{x} reverted at t={off[0]['t']} "
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
# C: CONTESTED with a party of TWO -- the SAME single designation as A, which
# is the whole point of running both: the count does not follow the party size.
# ---------------------------------------------------------------------------
def check_C(logs):
    return 0 if check_contested("C", logs, "C") is not None else 1


# ---------------------------------------------------------------------------
# D: DEFAULT-OFF CONTROL.  Same ground and same party as A; the only difference
# in the whole run is that D's attackers are NOT given
# cfg=BLITZ_CONTESTED_ALL_SUICIDERS=true.  A contested take on stock constants
# must designate nobody and say so.
# ---------------------------------------------------------------------------
def check_D(logs):
    AL = allies("D")
    data = {b: collect(t) for b, t in logs.items()}
    summarise(data, AL)

    # -- D1: the same experiment really did run.
    gos = [(b, g) for b in AL for g in data[b]["go"]]
    if not gos:
        print("FAIL: no BLITZ_GO in the default-off arena -- with no GO, "
              "'nobody was designated' proves nothing")
        return 1
    b0, g0 = gos[0]
    print(f"  D1: {len(gos)} GO line(s) (first: bot{b0} {g0['kind']} "
          f"t={g0['t']} party={g0['party']} -> {g0['sub']})")

    # -- D2: the take IS contested by geometry -- the enemy was seen, and seen
    #        INSIDE the range.  That is the opposite of arena B's check and it
    #        is what gives D4 its meaning: the rule had every reason to fire.
    tiles = foe_tiles(data, AL)
    if not tiles:
        print("FAIL: no attacker was ever handed a hostile tank by the engine "
              "-- this arena is testing blindness, not the default")
        return 1
    inside = [(k, d) for k, d in tiles.items() if d <= BLITZ_CONTESTED_RANGE]
    if not inside:
        print(f"FAIL: the enemy was never seen inside BLITZ_CONTESTED_RANGE "
              f"({BLITZ_CONTESTED_RANGE}) of the pill {PILL}, so this take is "
              f"not contested and designating nobody would be correct anyway. "
              f"Sightings: "
              + ", ".join(f"p{t}@({x},{y}) d={d:.1f}"
                          for (t, x, y), d in sorted(tiles.items())))
        return 1
    (tid, x, y), d = sorted(inside)[0]
    print(f"  D2: the enemy was visible INSIDE the range: p{tid}@({x},{y}) "
          f"d={d:.1f} <= {BLITZ_CONTESTED_RANGE} -- this take is contested by "
          f"geometry; only the switch stops the designation")

    # -- D3: nothing declared the take contested, because the rule never ran.
    cons = [(b, c) for b in AL for c in data[b]["contested"]]
    if cons:
        b, c = cons[0]
        print(f"FAIL: bot{b} t={c['t']} logged BLITZ_CONTESTED with the rule "
              f"OFF by default -- either the default flipped back to true in "
              f"constants.lua, or an opt-in token leaked into this arena")
        return 1
    print("  D3: no BLITZ_CONTESTED line anywhere")

    # -- D4: nobody is a suicider, and the GO lines say OFF rather than NO.
    for b in AL:
        if data[b]["su_on"]:
            s_ = data[b]["su_on"][0]
            print(f"FAIL: bot{b} t={s_['t']} became a suicider "
                  f"(src={s_['src']}, reason={s_['reason']}) with the "
                  f"contested rule off: {s_['line']}")
            return 1
        rx = [r for r in data[b]["rx"] if r["why"] == "contested"]
        if rx:
            print(f"FAIL: bot{b} received a contested `bsu` at t={rx[0]['t']} "
                  f"from p{rx[0]['frm']} with the rule off")
            return 1
    for b, g in gos:
        if g["contested"] != "off":
            print(f"FAIL: bot{b} {g['kind']} t={g['t']} reads "
                  f"contested{{{g['contested']}}}, expected contested{{off}}. "
                  f"'no' would mean the rule RAN and found nobody in range "
                  f"(arena B); with the switch off it must say so, or a log "
                  f"cannot tell a disabled rule from an uncontested take.")
            return 1
    print("  D4: nobody became a suicider, no contested `bsu` was sent, and "
          "every GO line reads contested{off}")
    return 0


def run_one(variant, ticks, build_dir, seed):
    foe = FOE_SPAWN[variant]
    d = euclid(PILL, foe)
    verdict = (f"CONTESTED, designates {BLITZ_CONTESTED_SUICIDERS}"
               if variant in CONTESTED and variant in RULE_ENABLED
               else "CONTESTED but the rule is OFF (default tokens)"
               if variant in CONTESTED else "CONTROL")
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
    rc = {"A": check_A, "B": check_B, "C": check_C, "D": check_D}[variant](logs)
    if rc == 0:
        if variant == "A":
            print("PASS: an enemy tank inside BLITZ_CONTESTED_RANGE of the "
                  "pill turned exactly ONE member of a THREE-tank blitz -- a "
                  "soldier, never the commander -- into a temporary suicider, "
                  "and the designation expired with the take.")
        elif variant == "C":
            print("PASS: the same enemy at the same distance, but a TWO-tank "
                  "party: the SAME single designation, again the soldier, and "
                  "the commander stayed a normal tank.")
        elif variant == "D":
            print("PASS: the same contested take on STOCK constants designated "
                  "nobody and labelled itself contested{off} -- the rule is off "
                  "by default and says which of the two 'no suicider' answers "
                  "this is.")
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
