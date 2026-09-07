#!/usr/bin/env python3
"""defend_pill ALARM MODE -- the goal is REJECTED unless an alarm holds, and it
dies the tick the alarm does (GoalHunter 1.7, C.DEFEND_ALARM_MODE).

THE RULE (Andrew, 2026-09-06).  defend_pill stays VISIBLE in the goal pool but
is REJECTED, with the failing condition as the reject reason, unless an ALARM
holds for that pill.  The alarm is on ONLY while ALL of:

  1. a hostile tank is VISIBLE RIGHT NOW -- this tick's perception, not a
     remembered sighting and not a ghost -- within DEFEND_ALARM_ENEMY_TILES (11)
     of the pill;
  2. AND either (a) the pill took damage FROM AN ENEMY inside
     DEFEND_ALARM_WINDOW_TICKS (250 = 5 s), or (b) a wall / hostile pill went up
     inside DEFEND_ALARM_BUILD_RADIUS (4) of it inside that window WITH a
     HOSTILE LGM seen in the same stamp ("if we know the LGM is an enemy,
     that's sufficient" -- no ally build claim is consulted);
  3. AND we are MORE than DEFEND_ALARM_MIN_DIST (9) tiles from the pill;
  4. AND the pill is not already WELL DEFENDED (C.DEFEND_ALARM_WELL_DEFENDED,
     added 2026-09-07).  Andrew: "Let's just reject the ones that are
     well_defended / No need to raise that alarm if it's well_defended."  The
     KEEL evaluator's own gate: R = ceil(their_team / our_team), held when
     foes_near <= allies_near * R, both counted within
     DEFEND_WELL_DEFENDED_RADIUS (10) euclidean tiles of the pill, the bidder
     excluded, allies = allied tanks at the pill PLUS allies whose broadcast
     goal is a defend/repair response aimed there.

The moment any condition stops holding the row goes back to REJECTED and a bot
standing on that goal DROPS it -- no hysteresis, no commitment, no grace.  The
cost while it holds is

    max(DEFEND_ALARM_MIN_COST 50, 100 - hits x 10) + partial Dijkstra

where the Dijkstra half is the path to the pill priced only as far as the first
tile within DEFEND_ALARM_DIJ_STOP_TILES (9) of it.

SEVEN RUNS.  tests/generate_defend_alarm_map.py builds the ground (its docstring
carries the geometry and, in particular, WHY every enemy position is 6 tiles
from the pill and not 11); tests/defend_alarm.scenario.lua drives the arenas
and writes the engine-side trace each one is checked against.

  A  DAMAGE ALARM, AND THE DROP.  Our tank starts 16 tiles west of our pill P.
     A scripted shooter parks 6 tiles south of P, across a moat it can never be
     reached over, and shells it.
       * BEFORE the first hit, with the shooter already sitting in the ring,
         the row must read REJECT alarm_off:no_trigger -- condition 1 alone is
         not an alarm.
       * Once the shells land the alarm must come ON, and its printed cost must
         re-derive: base 100, minus 10 per counted hit, floored at 50, plus the
         al_dij half, and the parts must add up to the total the line claims.
       * defend_pill must actually become the bot's goal.
       * Then the scenario REMOVES the shooter, and the alarm has to die of
         no_enemy_near within a handful of ticks of the despawn -- NOT five
         seconds later when the damage window would have expired.  That gap,
         measured against the engine's own despawn tick, is the whole point of
         condition 1 being a live sighting.
       * And the goal has to change off defend_pill right after.

  B  BUILD ALARM.  A scripted enemy parks 6 tiles NORTH of P on our side of the
     moat and sends its LGM to wall (126,122) -- 4 tiles from P, the OUTER ring
     of the stamp.  It never fires a shell, so trigger 2a can never arm: the
     only thing that can raise this alarm is the build.  Asserted on the
     DEFEND_ALARM_WATCH line naming the tile and the terrain it changed from,
     on a hostile LGM having been seen in the stamp, and on the alarm coming ON
     with trigger=build (never trigger=damage).

  B2 THE CONTROL, and it proves the opposite thing to the one this arena
     originally proved.  Same ground, but the enemy is told to PARK and never
     sends its man, and the SCENARIO flips the same tile to a wall itself with
     game.set_tile.  So: an enemy tank in the ring, a real terrain change inside
     the stamp, and NO hostile LGM ever seen there.  The alarm must NOT fire.
     Without this, B only says "something built and the alarm went off".

  C  TOO CLOSE.  Arena A's shooter, but our tank starts 6 tiles from P.
     Conditions 1 and 2 both hold and the row must still reject
     alarm_off:too_close, on every tick the engine says we were inside 9 tiles.
     (The bot is free to drive away and alarm afterwards -- that is correct
     behaviour, so the assertion is scoped to the ticks it was actually close.)

  D  KEEL CONTROL, one token different: cfg=DEFEND_ALARM_MODE=false.  The old
     evaluator must be what runs -- its HEAT_GATE ladder lines present, its
     `(base{250}+dij{..})*m{..}` desc shape printed in FINAL_SCORES -- and not
     one DEFEND_ALARM_* line anywhere.  This is what makes A-C evidence about
     the new rule rather than about a brain that would have defended anyway.

  W  WELL DEFENDED (condition 4).  Arena A's shooter and our far observer, plus
     four more parked tanks: two ALLIES sitting 6 and 5.7 tiles from the pill
     and two filler FOES sitting 18 tiles away across the moat, making the
     teams 3v3 so R = ceil(3/3) = 1.  Conditions 1-3 all hold -- an enemy is
     visible at the pill, it is shelling it, and we are 16 tiles out -- so
     condition 4 is the only thing that can reject the row.  Every rejected row
     must read alarm_off:well_defended(foes 1 <= allies 2 x 1), its arithmetic
     must close, its ally count must match the ENGINE's own tank positions, the
     alarm must never come ON and defend_pill must never become the goal.

  WC THE CONTROL for W.  The same six-tank 3v3 game with exactly one thing
     changed: the two allies park 14 and 18 tiles out instead of 6 and 5.7.
     allies_near is then 0, the pill is not held, and the alarm must fire just
     as it does in arena A -- with not one well_defended reject anywhere.
     Note that removing ONE of the two near allies would NOT be a control: at
     R=1 a single ally still covers a single foe (1 <= 1 x 1).

WHAT IS READ FROM WHERE.  The brain's own reasoning comes from print2 (the
DEFEND_ALARM_ON / OFF / WATCH lines and FINAL_SCORES).  Everything those lines
are checked AGAINST -- when the pill was hit, when the shooter was removed, when
the wall tile changed, where our tank was -- comes from the scenario sidecar's
engine-side trace, which knows nothing about goals.lua.

Usage: python defend_alarm_test.py [--variant A|B|B2|C|D|W|WC|all] [--ticks N]
                                   [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "defend_alarm.map"

sys.path.insert(0, str(HERE))
import generate_defend_alarm_map as G   # noqa: E402

VARIANTS = ["A", "B", "B2", "C", "D", "W", "WC"]
# Ports 50340+ -- 50124, 50291-50294, 50300+ and 50320+ are taken by other
# tests in this directory and two servers on one port is a silent hang.
# W/WC sit at 50440+ rather than 50345 so a future arena can be slotted in
# beside the originals without renumbering these.
PORTS = {"A": 50340, "B": 50341, "B2": 50342, "C": 50343, "D": 50344,
         "W": 50440, "WC": 50441}
# ENGINE ticks.  The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks.
# W/WC get more than A: four extra tanks have to join, drive off their ponds
# and park before the count they exist to produce is even true.
TICKS = {"A": 2600, "B": 2600, "B2": 2600, "C": 2000, "D": 2000,
         "W": 3000, "WC": 3000}

# Knobs the assertions are written against, imported from the generator so one
# edit moves the arena and the arithmetic together.
BASE = G.ALARM_BASE_COST
DISC = G.ALARM_HIT_DISCOUNT
FLOOR = G.ALARM_MIN_COST
MIN_DIST = G.ALARM_MIN_DIST
WINDOW = G.ALARM_WINDOW_TICKS
STOP = G.ALARM_DIJ_STOP_TILES
OUR_PILL = G.OUR_PILL
FOE_WALL = G.FOE_WALL

# How soon after the engine removed the shooter the alarm has to be off, in
# BRAIN ticks.  The goal-validity hook runs every tick, so this is slack for
# the tick the brain happened to be on, not for a mechanism.  It is far below
# the 250-tick damage window on purpose: a drop that took the window would be
# the bug this arena exists to catch.
DESPAWN_SLACK = 25
# Brain ticks after the alarm dies within which the goal must have changed.
GOAL_CHANGE_SLACK = 40

NO_CAPTURE = "cfg=CAPTURE_PILL_BASE_COST=1e30"
# The reposition pool would otherwise bid to shoot our own pill down and move
# it, which puts our own shells on the pill the alarm is counting hits on.
NO_REPOS = "cfg=PILL_REPOSITION_ENABLED=false"
KEEL_ALARM_OFF = "cfg=DEFEND_ALARM_MODE=false"
TOKENS = {
    "A":  ";".join([NO_CAPTURE, NO_REPOS]),
    "B":  ";".join([NO_CAPTURE, NO_REPOS]),
    "B2": ";".join([NO_CAPTURE, NO_REPOS]),
    "C":  ";".join([NO_CAPTURE, NO_REPOS]),
    "D":  ";".join([NO_CAPTURE, NO_REPOS, KEEL_ALARM_OFF]),
    "W":  ";".join([NO_CAPTURE, NO_REPOS]),
    "WC": ";".join([NO_CAPTURE, NO_REPOS]),
}
ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# ── print2 lines ──────────────────────────────────────────────────────────
# goals.lua
#   DEFEND_ALARM_ON t=812 pill#3@(126,126) cost=131 = net{80 = 100 - 2x10} +
#     dij{51 stop9t 7/16} enemy=#1@6.0t trigger=damage 2 hits in 250t dist=16.0
ON_RE = re.compile(
    r"DEFEND_ALARM_ON t=(\d+) pill#(\S+)@\((\d+),(\d+)\) cost=([\d.]+) = "
    r"net\{([\d.]+) = (\d+) - (\d+)x(\d+)([^}]*)\} \+ dij\{([\d.]+) ([^}]*)\} "
    r"enemy=#(\S+)@([\d.]+)t trigger=(.*?) dist=([\d.]+)")
# goals.lua, the pool's side of a rejected row
#   DEFEND_ALARM_OFF t=700 pill#3@(126,126) REJECT alarm_off:no_trigger
#     enemy=#1@6.0t trigger=(dmg 0 in 250t, build none) dist=16.0 (min 9)
# The reason is matched non-greedily up to the ` enemy=` anchor rather than as
# one \S+ token: condition 4's reason carries its own arithmetic and reads
# `well_defended(foes 1 <= allies 2 x 1)`, spaces and all.  (The whitespace-free
# rule the line's own comment states is about the `enemy=` field, which has no
# such anchor after it.)
ROW_OFF_RE = re.compile(
    r"DEFEND_ALARM_OFF t=(\d+) pill#(\S+)@\((\d+),(\d+)\) REJECT (.+?) "
    r"enemy=(\S+) trigger=\((.*?)\) dist=([\d.]+) \(min (\d+)\)")
# Condition 4's reason, so the test can re-derive foes <= allies x R and check
# the ally count against the ENGINE's word for where the allies were.
WD_RE = re.compile(r"well_defended\(foes (\d+) <= allies (\d+) x (\d+)\)")
# init.lua, the goal-validity hook DROPPING a live goal
#   DEFEND_ALARM_OFF t=931 pill@(126,126) reason=no_enemy_near -- alarm ...
HOOK_OFF_RE = re.compile(
    r"DEFEND_ALARM_OFF t=(\d+) pill@\((\d+),(\d+)\) reason=(\S+)")
# perception.lua
#   DEFEND_ALARM_WATCH t=555 pill@(126,126) BUILD (126,122) halfwall
#     (was tt=7) lgm_seen=12t ago within r=4 win=250
WATCH_RE = re.compile(
    r"DEFEND_ALARM_WATCH t=(\d+) pill@\((\d+),(\d+)\) BUILD \((\d+),(\d+)\) "
    r"(\S+) \(was tt=(\d+)\) lgm_seen=(\S+)")
# init.lua TICK_COST t=123 ms=1.0 ... goal=defend_pill sub=nil
TICK_RE = re.compile(r"TICK_COST t=(\d+) .*? goal=(\S+)")
# goals.lua FINAL_SCORES rows carry the pool desc verbatim
DESC_RE = re.compile(r"desc=(.*)$")
# the keel evaluator's own ladder
HEAT_GATE_RE = re.compile(r"HEAT_GATE t=(\d+) pill@\((\d+),(\d+)\)")
KEEL_DESC_RE = re.compile(r"\(base\{[\d.]+\}\+dij\{[\d.]+\}\)\*m\{[\d.]+\}")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_trace(build_dir, variant):
    """The scenario sidecar's engine-side view, as a list of (kind, [ints...]).

    Kinds: hp, foe, bot, wall, spawn, despawn, settile.  All ticks are SIM
    ticks -- twice the brain's clock, which every comparison below converts
    for explicitly."""
    path = build_dir / f"defend_alarm_{variant}_trace.log"
    rows = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            try:
                rows.append((parts[0], [int(p) for p in parts[1:]]))
            except ValueError:
                pass
    return rows


def run_sim(variant, ticks, build_dir):
    """Runs one arena; returns (session_dir, print2_text, trace) or (None, msg,
    None)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}", None
    subprocess.run([sys.executable, str(HERE / "generate_defend_alarm_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"defend_alarm_{variant}"
    final = HERE / f"defend_alarm_{variant}_final.json"
    stderr = HERE / f"defend_alarm_{variant}_stderr.txt"
    trace_log = build_dir / f"defend_alarm_{variant}_trace.log"
    for p in (final, stderr, trace_log):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then "
                              f"retry."), None

    tokens = TOKENS[variant]
    assert len(tokens) <= ARG_MAX, (
        f"arena {variant}'s token string is {len(tokens)} bytes; BotInitSlot.arg "
        f"is {ARG_MAX} and a longer one is truncated MID-TOKEN")

    env = dict(os.environ,
               WINBOLO_BRAINDBG_LABEL=label,
               DEFEND_ALARM_VARIANT=variant)
    cmd = [str(ds), "-map", str(MAP), "-port", str(PORTS[variant]),
           "-nolobby", "-gametype", "open", "-bots", "1",
           "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           # yesfull: the arena is tiny and the test is about WHEN the alarm
           # holds, not about discovering the map.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(300, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)"), None
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- see " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000]), None
    log = sess / "print2_bot0.log"
    if not log.exists():
        return None, f"no print2_bot0.log under {sess}", None
    return sess, log.read_text(errors="ignore"), read_trace(build_dir, variant)


def goal_at(goals, tick):
    """The bot's goal on the latest TICK_COST line at or before `tick`."""
    last = None
    for t, g in goals:
        if t <= tick:
            last = g
        else:
            break
    return last


def check_on_line(m):
    """Re-derive an ON line's cost from its own chips.

    The standing rule for these lines is that EVERY factor is on the line, so a
    reader can hand-check the total without opening constants.lua.  If the parts
    do not close, the line is lying and everything below is reading a number
    nobody can reproduce.  Returns (ok, msg, fields)."""
    (t, pid, px, py, cost, net, base, hits, disc, floored, dij, dijchip,
     eid, edist, trig, dist) = m.groups()
    t, cost, net = int(t), float(cost), float(net)
    base, hits, disc = int(base), int(hits), int(disc)
    dij, dist, edist = float(dij), float(dist), float(edist)
    f = dict(t=t, pill=(int(px), int(py)), cost=cost, net=net, hits=hits,
             dij=dij, dist=dist, edist=edist, trig=trig, floored=bool(floored),
             dijchip=dijchip, enemy=eid)
    if base != BASE or disc != DISC:
        return False, (f"t={t}: the line prints base={base} discount={disc}, "
                       f"but the arena is built for {BASE}/{DISC}"), f
    raw = base - hits * disc
    want = max(FLOOR, raw)
    if abs(net - want) > 0.51:
        return False, (f"t={t}: net{{{net}}} does not follow from "
                       f"max({FLOOR}, {base} - {hits}x{disc}) = {want}"), f
    if raw < FLOOR and not floored:
        return False, (f"t={t}: {base} - {hits}x{disc} = {raw} is under the "
                       f"{FLOOR} floor but the line does not say it floored"), f
    if abs(cost - (net + dij)) > 0.51:
        return False, (f"t={t}: cost={cost} but net{{{net}}} + dij{{{dij}}} "
                       f"= {net + dij}"), f
    if dist <= MIN_DIST:
        return False, (f"t={t}: the alarm is ON at dist={dist}, which is "
                       f"inside DEFEND_ALARM_MIN_DIST({MIN_DIST}) -- "
                       f"condition 3 should have rejected it"), f
    if f"stop{STOP}t" not in dijchip and f"disc{STOP}t" not in dijchip:
        return False, (f"t={t}: dij chip {dijchip!r} names neither the "
                       f"stop{STOP}t path walk nor the disc{STOP}t "
                       f"fallback -- the partial Dijkstra is unaccounted for"), f
    return True, "", f


# ══════════════════════════════════════════════════════════════════════════
def arena_A(text, trace):
    ons = [m for m in (ON_RE.search(l) for l in text.splitlines()) if m]
    rowoffs = [m for m in (ROW_OFF_RE.search(l) for l in text.splitlines()) if m]
    # The goal-validity hook's line and the pool's rejected-row line share the
    # DEFEND_ALARM_OFF prefix on purpose (they are the same statement from two
    # places); "REJECT" is what tells them apart.
    hookoffs = [m for m in (HOOK_OFF_RE.search(li) for li in text.splitlines()
                            if "REJECT" not in li) if m]
    goals = [(int(t), g) for (t, g) in TICK_RE.findall(text)]

    hp_rows = [r for k, r in trace if k == "hp"]
    despawns = [r for k, r in trace if k == "despawn"]
    print(f"  engine: {len(hp_rows)} pill-armour change(s)"
          + (f", first {hp_rows[0]}, last {hp_rows[-1]}" if hp_rows else "")
          + (f"; shooter removed at sim t={despawns[0][0]}" if despawns
             else "; shooter was NEVER removed"))
    print(f"  brain: {len(ons)} ALARM_ON, {len(rowoffs)} row-REJECT, "
          f"{len(hookoffs)} goal-drop line(s)")

    if not despawns:
        print("FAIL (A): the scenario never removed the shooter, so the "
              "condition-1 half of this arena did not run. Check that the pill "
              "was hit at all (hp rows above) -- the despawn is timed off the "
              "first hp drop.")
        return 1
    despawn_sim = despawns[0][0]
    despawn_brain = despawn_sim // 2

    # ── A1: condition 1 alone is not an alarm ────────────────────────────
    first_hit_sim = None
    for i, r in enumerate(hp_rows):
        if i > 0 and r[1] < hp_rows[i - 1][1]:
            first_hit_sim = r[0]
            break
    if first_hit_sim is None:
        print("FAIL (A): the engine never saw our pill's armour DROP, so "
              f"trigger 2a never armed. Armour trace: {hp_rows[:8]}")
        return 1
    pre = [m for m in rowoffs
           if int(m.group(1)) < first_hit_sim // 2
           and m.group(5) in ("alarm_off:no_trigger", "alarm_off:no_enemy_near")]
    if not pre:
        reasons = sorted({m.group(5) for m in rowoffs})
        print(f"FAIL (A1): before the first hit (sim t={first_hit_sim}) no row "
              f"was rejected no_trigger or no_enemy_near. Reasons seen across "
              f"the run: {reasons}")
        return 1
    print(f"  A1 OK: before the first hit (sim t={first_hit_sim}, brain "
          f"t={first_hit_sim // 2}) the row was REJECTED "
          f"{pre[-1].group(5)} -- an enemy in the ring is not an alarm on its "
          f"own ({len(pre)} such line(s))")

    # ── A2: the alarm comes on, and its cost re-derives ──────────────────
    if not ons:
        print("FAIL (A2): the pill was shelled with an enemy 6 tiles from it "
              "and our tank 16 tiles away, and the alarm never came ON. Row "
              f"rejects seen: {sorted({m.group(5) for m in rowoffs})}")
        return 1
    dmg_ons = [m for m in ons if "damage" in m.group(15)]
    if not dmg_ons:
        print("FAIL (A2): the alarm came ON but never on the DAMAGE trigger: "
              f"{sorted({m.group(15) for m in ons})}")
        return 1
    for m in dmg_ons:
        ok, msg, f = check_on_line(m)
        if not ok:
            print(f"FAIL (A2): the ALARM_ON line does not add up -- {msg}")
            return 1
    f0 = check_on_line(dmg_ons[0])[2]
    print(f"  A2 OK: ALARM ON at brain t={f0['t']} -- "
          f"max({FLOOR}, {BASE} - {f0['hits']}x{DISC}) = {f0['net']:.0f} "
          f"+ al_dij {f0['dij']:.0f} [{f0['dijchip']}] = {f0['cost']:.0f}, "
          f"enemy {f0['enemy']} at {f0['edist']:.1f}t, us at {f0['dist']:.1f}t; "
          f"all {len(dmg_ons)} damage-trigger line(s) re-derive")

    # ── A3: it actually became the goal ──────────────────────────────────
    on_ticks = [int(m.group(1)) for m in dmg_ons]
    defend_ticks = [t for (t, g) in goals if g == "defend_pill"
                    and on_ticks[0] - 10 <= t <= despawn_brain + 5]
    if not defend_ticks:
        near = [(t, g) for (t, g) in goals
                if on_ticks[0] - 10 <= t <= despawn_brain + 5]
        print(f"FAIL (A3): the alarm was ON from brain t={on_ticks[0]} but "
              f"defend_pill never became the goal before the despawn "
              f"(brain t={despawn_brain}). Goals in that window: {near[:14]}")
        return 1
    print(f"  A3 OK: defend_pill was the goal on {len(defend_ticks)} tick(s) "
          f"between the alarm and the despawn (first t={defend_ticks[0]})")

    # ── A4: and it dies when the enemy does, not when the window does ────
    drops = [m for m in hookoffs if int(m.group(1)) >= despawn_brain - 2]
    if not drops:
        print(f"FAIL (A4): the shooter was removed at sim t={despawn_sim} "
              f"(brain t={despawn_brain}) and no DEFEND_ALARM_OFF goal-drop "
              f"line followed. Drops seen: "
              f"{[(int(m.group(1)), m.group(4)) for m in hookoffs][:8]}")
        return 1
    d = drops[0]
    gap = int(d.group(1)) - despawn_brain
    if d.group(4) != "no_enemy_near":
        print(f"FAIL (A4): the goal was dropped at brain t={d.group(1)} for "
              f"{d.group(4)}, not no_enemy_near -- the despawn is supposed to "
              f"take condition 1 out from under it.")
        return 1
    if gap > DESPAWN_SLACK:
        print(f"FAIL (A4): the shooter vanished at brain t={despawn_brain} but "
              f"the goal was not dropped until t={d.group(1)} ({gap} ticks "
              f"later, slack is {DESPAWN_SLACK}). Condition 1 is a LIVE "
              f"sighting; a drop that takes the {WINDOW}-tick damage window is "
              f"the bug this arena exists to catch.")
        return 1
    print(f"  A4 OK: shooter removed at brain t={despawn_brain}, goal dropped "
          f"at t={d.group(1)} reason={d.group(4)} -- {gap} tick(s) later, "
          f"against a {WINDOW}-tick damage window that had not expired")

    # ── A5: and the bot moved on ─────────────────────────────────────────
    after = [(t, g) for (t, g) in goals
             if int(d.group(1)) <= t <= int(d.group(1)) + GOAL_CHANGE_SLACK]
    changed = [(t, g) for (t, g) in after if g != "defend_pill"]
    if not changed:
        print(f"FAIL (A5): the goal was dropped at t={d.group(1)} but the bot "
              f"was still on defend_pill for the next {GOAL_CHANGE_SLACK} "
              f"ticks: {after[:14]}")
        return 1
    print(f"  A5 OK: goal changed to {changed[0][1]} at t={changed[0][0]}, "
          f"{changed[0][0] - int(d.group(1))} tick(s) after the drop")
    return 0


def arena_B(text, trace):
    watches = [m for m in (WATCH_RE.search(l) for l in text.splitlines()) if m]
    ons = [m for m in (ON_RE.search(l) for l in text.splitlines()) if m]
    rowoffs = [m for m in (ROW_OFF_RE.search(l) for l in text.splitlines()) if m]
    walls = [r for k, r in trace if k == "wall"]
    hp_rows = [r for k, r in trace if k == "hp"]

    print(f"  engine: wall tile {FOE_WALL} terrain trace {walls}")
    print(f"  engine: {len(hp_rows)} pill-armour change(s) {hp_rows[:4]}")
    print(f"  brain: {len(watches)} BUILD watch, {len(ons)} ALARM_ON, "
          f"{len(rowoffs)} row-REJECT line(s)")

    # The arena is only meaningful if the man really did build.
    built = [r for r in walls[1:] if r[3] in (0, 8)]   # T_BUILDING / T_HALFBUILD
    if not built:
        print(f"FAIL (B): the enemy LGM never put a wall on {FOE_WALL} -- the "
              f"engine's terrain trace for that tile is {walls}. Nothing in "
              f"this arena can be about builds until it does.")
        return 1
    print(f"  B0 OK: the engine saw {FOE_WALL} become terrain "
          f"{built[0][3]} at sim t={built[0][0]}")

    # Trigger 2a must never have armed, or B proves nothing about builds.
    dropped = [r for i, r in enumerate(hp_rows)
               if i > 0 and r[1] < hp_rows[i - 1][1]]
    if dropped:
        print(f"FAIL (B): the pill took damage at sim t={dropped[0][0]} -- the "
              f"waller is not supposed to fire, and with trigger 2a armed this "
              f"arena no longer says anything about builds.")
        return 1

    stamp_watch = [m for m in watches
                   if (int(m.group(4)), int(m.group(5))) == tuple(FOE_WALL)]
    if not stamp_watch:
        print(f"FAIL (B1): the engine walled {FOE_WALL} but perception never "
              f"reported it inside the build stamp. Watch lines: "
              f"{[(m.group(1), m.group(4), m.group(5), m.group(6)) for m in watches][:6]}")
        return 1
    w = stamp_watch[0]
    if w.group(8) in ("never", "never "):
        print(f"FAIL (B1): the build at {FOE_WALL} was seen, but with NO "
              f"hostile LGM in the stamp (lgm_seen={w.group(8)}) -- that is the "
              f"B2 control's outcome, not B's.")
        return 1
    print(f"  B1 OK: brain t={w.group(1)} -- BUILD ({w.group(4)},{w.group(5)}) "
          f"{w.group(6)} (was terrain {w.group(7)}), hostile LGM seen "
          f"{w.group(8)} in the same stamp")

    build_ons = [m for m in ons if "build" in m.group(15)]
    if not build_ons:
        print(f"FAIL (B2): the build was detected with a hostile LGM in the "
              f"stamp and the alarm never came ON for it. ALARM_ON triggers "
              f"seen: {sorted({m.group(15) for m in ons})}; rejects: "
              f"{sorted({m.group(5) for m in rowoffs})}")
        return 1
    for m in build_ons:
        ok, msg, _f = check_on_line(m)
        if not ok:
            print(f"FAIL (B2): the ALARM_ON line does not add up -- {msg}")
            return 1
    f0 = check_on_line(build_ons[0])[2]
    if f0["hits"] != 0:
        print(f"FAIL (B2): the build-trigger alarm counted {f0['hits']} hits, "
              f"but nothing ever shot this pill -- the damage discount is "
              f"reading something it should not.")
        return 1
    print(f"  B2 OK: ALARM ON at brain t={f0['t']} on the BUILD trigger "
          f"({f0['trig']}) -- {BASE} - 0x{DISC} = {f0['net']:.0f} + al_dij "
          f"{f0['dij']:.0f} [{f0['dijchip']}] = {f0['cost']:.0f}; "
          f"{len(build_ons)} such line(s), all re-derive")
    return 0


def arena_B2(text, trace):
    watches = [m for m in (WATCH_RE.search(l) for l in text.splitlines()) if m]
    ons = [m for m in (ON_RE.search(l) for l in text.splitlines()) if m]
    rowoffs = [m for m in (ROW_OFF_RE.search(l) for l in text.splitlines()) if m]
    walls = [r for k, r in trace if k == "wall"]
    settile = [r for k, r in trace if k == "settile"]

    print(f"  engine: wall tile {FOE_WALL} terrain trace {walls}, "
          f"set_tile at {settile}")
    print(f"  brain: {len(watches)} BUILD watch, {len(ons)} ALARM_ON, "
          f"{len(rowoffs)} row-REJECT line(s)")

    if not settile:
        print("FAIL (B2): the scenario never ran set_tile, so the control's "
              "build never happened.")
        return 1
    if not [r for r in walls[1:] if r[3] == 0]:
        print(f"FAIL (B2): set_tile ran at sim t={settile[0][0]} but the "
              f"engine's terrain trace for {FOE_WALL} never showed a wall: "
              f"{walls}. The control cannot say anything until the tile "
              f"really changes.")
        return 1
    print(f"  B2-0 OK: the script walled {FOE_WALL} at sim t={settile[0][0]} "
          f"with no LGM anywhere near it")

    # An enemy tank HAS to be in the ring, or the control passes for the wrong
    # reason (no_enemy_near instead of no_trigger).
    with_enemy = [m for m in rowoffs if not m.group(6).startswith("NONE")]
    if not with_enemy:
        print(f"FAIL (B2): no rejected row ever had an enemy in the ring, so "
              f"this run proves nothing about the LGM attribution -- it would "
              f"have rejected on condition 1 regardless. Rejects: "
              f"{[(m.group(1), m.group(5), m.group(6)) for m in rowoffs][:8]}")
        return 1

    bad = [m for m in ons if "build" in m.group(15)]
    if bad:
        m = bad[0]
        print(f"FAIL (B2): the alarm fired on a BUILD trigger at brain "
              f"t={m.group(1)} ({m.group(15)}) even though no hostile LGM was "
              f"ever in the stamp -- the OBJECT_HOSTILE attribution is not "
              f"gating trigger 2b.")
        return 1
    lgm_watch = [m for m in watches
                 if (int(m.group(4)), int(m.group(5))) == tuple(FOE_WALL)
                 and not m.group(8).startswith("never")]
    if lgm_watch:
        print(f"FAIL (B2): perception reported a hostile LGM in the stamp "
              f"({lgm_watch[0].group(8)}) on a run where the enemy never sent "
              f"its man out.")
        return 1
    after = [m for m in rowoffs if int(m.group(1)) >= settile[0][0] // 2]
    print(f"  B2-1 OK: the wall went up with an enemy tank in the ring and no "
          f"hostile LGM in the stamp, and the alarm stayed OFF -- "
          f"{len(after)} row(s) rejected after the build, reasons "
          f"{sorted({m.group(5) for m in after})}")
    return 0


def arena_C(text, trace):
    ons = [m for m in (ON_RE.search(l) for l in text.splitlines()) if m]
    rowoffs = [m for m in (ROW_OFF_RE.search(l) for l in text.splitlines()) if m]
    bots = [r for k, r in trace if k == "bot"]
    hp_rows = [r for k, r in trace if k == "hp"]

    def bot_dist_at(sim_t):
        """Euclidean tiles from our tank to the pill at a SIM tick, from the
        engine's own trace -- not from anything the brain said."""
        last = None
        for t, x, y in bots:
            if t <= sim_t:
                last = (x, y)
            else:
                break
        if last is None:
            return None
        return ((last[0] - OUR_PILL[0]) ** 2
                + (last[1] - OUR_PILL[1]) ** 2) ** 0.5

    dropped = [r for i, r in enumerate(hp_rows)
               if i > 0 and r[1] < hp_rows[i - 1][1]]
    print(f"  engine: {len(bots)} bot-tile change(s), first {bots[0] if bots else None}; "
          f"{len(dropped)} hit(s) on the pill")
    print(f"  brain: {len(ons)} ALARM_ON, {len(rowoffs)} row-REJECT line(s)")

    if not dropped:
        print("FAIL (C): the pill was never hit, so conditions 1 and 2 never "
              "both held and there was nothing for condition 3 to be the only "
              "thing refusing.")
        return 1

    close = [m for m in rowoffs
             if (bot_dist_at(int(m.group(1)) * 2) or 99) <= MIN_DIST]
    if not close:
        print(f"FAIL (C): no rejected row was printed on a tick the ENGINE "
              f"says our tank was within {MIN_DIST} tiles of the pill -- the "
              f"bot left before the arena could test anything. First bot "
              f"tiles: {bots[:8]}")
        return 1
    # The printed reason carries the numbers -- "alarm_off:too_close(5.8<=9)" --
    # so match on the condition prefix, not the whole string.
    # too_close is the ONLY answer left on a close tick where the OTHER two
    # conditions are both satisfied -- and whether they were is on the line
    # itself, so the check does not have to guess from a time window.  (The
    # shooter dies to our own pill partway through this arena; after that,
    # rejecting no_enemy_near while the damage window is still open is exactly
    # right, and an assertion keyed to the window would call it a failure.)
    def conds_1_and_2(m):
        if m.group(6).startswith("NONE"):
            return False
        trig = m.group(7)
        dm = re.search(r"dmg (\d+) in", trig)
        if dm and int(dm.group(1)) > 0:
            return True
        return "build none" not in trig and "NOT SEEN" not in trig
    wrong = [m for m in close
             if conds_1_and_2(m)
             and not m.group(5).startswith("alarm_off:too_close")]
    if wrong:
        m = wrong[0]
        print(f"FAIL (C): at brain t={m.group(1)} the engine says we were "
              f"{bot_dist_at(int(m.group(1)) * 2):.1f} tiles from the pill, the "
              f"line says an enemy was in the ring ({m.group(6)}) and a trigger "
              f"was armed ({m.group(7)}), so condition 3 is the only one left "
              f"to refuse -- the row should have rejected too_close and "
              f"rejected {m.group(5)} instead.")
        return 1
    tc = [m for m in close if m.group(5).startswith("alarm_off:too_close")
          and conds_1_and_2(m)]
    if not tc:
        print(f"FAIL (C): not one row read alarm_off:too_close while we were "
              f"inside {MIN_DIST} tiles. Reasons on close ticks: "
              f"{sorted({m.group(5) for m in close})}")
        return 1
    bad_on = [m for m in ons
              if (bot_dist_at(int(m.group(1)) * 2) or 99) <= MIN_DIST]
    if bad_on:
        m = bad_on[0]
        print(f"FAIL (C): the alarm was ON at brain t={m.group(1)} while the "
              f"engine says we were "
              f"{bot_dist_at(int(m.group(1)) * 2):.1f} tiles from the pill, "
              f"inside DEFEND_ALARM_MIN_DIST({MIN_DIST}).")
        return 1
    m0 = tc[0]
    print(f"  C OK: {len(tc)} row(s) rejected alarm_off:too_close while the "
          f"engine had us inside {MIN_DIST} tiles (first brain t={m0.group(1)}, "
          f"engine distance {bot_dist_at(int(m0.group(1)) * 2):.1f}, brain's own "
          f"dist={m0.group(8)}), enemy {m0.group(6)} in the ring and "
          f"trigger ({m0.group(7)}) armed -- and NOT ONE ALARM_ON on any tick "
          f"we were that close")
    return 0


def arena_D(text, trace):
    # The three lines alarm mode prints, and ONLY those: the brain also echoes
    # the cfg=DEFEND_ALARM_MODE=false token itself at startup, which mentions
    # the knob without being the evaluator saying anything.
    alarm_lines = [li for li in text.splitlines()
                   if ("DEFEND_ALARM_ON " in li or "DEFEND_ALARM_OFF " in li
                       or "DEFEND_ALARM_WATCH " in li)]
    heat = HEAT_GATE_RE.findall(text)
    keel_descs = [li for li in text.splitlines()
                  if "desc=" in li and "defend" in li and KEEL_DESC_RE.search(li)]
    hp_rows = [r for k, r in trace if k == "hp"]
    dropped = [r for i, r in enumerate(hp_rows)
               if i > 0 and r[1] < hp_rows[i - 1][1]]

    print(f"  engine: {len(dropped)} hit(s) on the pill "
          f"(same shooter as arena A)")
    print(f"  brain: {len(alarm_lines)} DEFEND_ALARM_* line(s), "
          f"{len(heat)} HEAT_GATE line(s), "
          f"{len(keel_descs)} keel-shaped defend desc(s)")

    if alarm_lines:
        print(f"FAIL (D): cfg=DEFEND_ALARM_MODE=false and the brain still "
              f"printed {len(alarm_lines)} alarm line(s). First: "
              f"{alarm_lines[0][:180]}")
        return 1
    if not keel_descs:
        descs = [li.strip()[:150] for li in text.splitlines()
                 if "desc=" in li and "defend" in li][:6]
        print(f"FAIL (D): no defend row printed the keel evaluator's "
              f"(base{{..}}+dij{{..}})*m{{..}} shape, so this run is not "
              f"evidence that the old path is what ran. defend descs seen: "
              f"{descs}")
        return 1
    if not dropped:
        print("FAIL (D): the pill was never hit, so the keel evaluator was "
              "never asked the question arena A asks the new one.")
        return 1
    sample = KEEL_DESC_RE.search(keel_descs[0]).group(0)
    print(f"  D OK: with DEFEND_ALARM_MODE=false the OLD evaluator ran and the "
          f"new one is silent -- {len(heat)} HEAT_GATE ladder line(s) and "
          f"{len(keel_descs)} defend row(s) priced {sample}, zero "
          f"DEFEND_ALARM_* lines")
    return 0


def wd_engine_allies(trace, sim_tick):
    """How many TEAM-0 extras the ENGINE had within DEFEND_WELL_DEFENDED_RADIUS
    of the pill at `sim_tick`.

    Read off the sidecar's `xpos` rows (tick, slot, team, mx, my), which are
    emitted on every tile change and know nothing about goals.lua.  This is what
    the brain's own allies_near number is checked against -- comparing the
    brain's count to the brain's count would be checking the code against
    itself.  Our observer is not among the extras and is 16 tiles out anyway;
    the engine excludes the bidder from its own object list regardless
    (players.c playersGetBrainTanksInRect skips myPlayerNum)."""
    pos = {}
    for kind, r in trace:
        if kind != "xpos" or r[0] > sim_tick:
            continue
        pos[r[1]] = (r[2], r[3], r[4])       # team, mx, my
    n = 0
    for team, mx, my in pos.values():
        if team != 0:
            continue
        if G.edist((mx, my), OUR_PILL) <= G.WELL_DEFENDED_RADIUS:
            n += 1
    return n


def arena_W(text, trace):
    """WELL DEFENDED -- the pill is already held, so no alarm is raised.

    Conditions 1-3 all hold (arena A's shooter is visible at the pill and
    shelling it, and we are 16 tiles away), so condition 4 is the only thing
    that can reject the row.  Two allies sit 6 tiles from the pill, teams are
    3v3 so R = ceil(3/3) = 1, and 1 foe <= 2 allies x 1 means held."""
    ons = [m for m in (ON_RE.search(li) for li in text.splitlines()) if m]
    rowoffs = [m for m in (ROW_OFF_RE.search(li) for li in text.splitlines())
               if m]
    goals = [(int(t), g) for (t, g) in TICK_RE.findall(text)]

    hp_rows = [r for k, r in trace if k == "hp"]
    dropped = [r for i, r in enumerate(hp_rows)
               if i > 0 and r[1] < hp_rows[i - 1][1]]
    extras = [r for k, r in trace if k == "extra"]
    near_end = wd_engine_allies(trace, 10 ** 9)
    print(f"  engine: {len(extras)} extra tank(s) spawned, "
          f"{len(dropped)} hit(s) on the pill, "
          f"{near_end} team-0 extra(s) parked within "
          f"{G.WELL_DEFENDED_RADIUS} tiles of it")
    wd_offs = [m for m in rowoffs if m.group(5).startswith(
        "alarm_off:well_defended")]
    print(f"  brain: {len(ons)} ALARM_ON, {len(rowoffs)} row-REJECT line(s), "
          f"{len(wd_offs)} of them well_defended")

    # ── W0: the arena actually assembled ─────────────────────────────────
    if len(extras) != 4:
        print(f"FAIL (W): the scenario spawned {len(extras)} extra tank(s), "
              f"not 4 -- without both allies AND both filler foes the count "
              f"and the team ratio are not what this arena is about. "
              f"extra rows: {extras}")
        return 1
    if near_end != 2:
        print(f"FAIL (W): the engine had {near_end} allied extra(s) parked "
              f"within {G.WELL_DEFENDED_RADIUS} tiles of the pill, not 2. "
              f"They must have failed to reach their park tiles -- check the "
              f"xpos rows in the trace.")
        return 1
    if not dropped:
        print("FAIL (W): the pill was never hit, so trigger 2a never armed "
              "and the row would have been rejected no_trigger long before "
              "condition 4 was ever reached.")
        return 1

    # ── W1: the reject, with its arithmetic re-derived ───────────────────
    # Scoped to rows where the brain itself was outside MIN_DIST: inside it,
    # condition 3 rejects FIRST and correctly, and the bot is free to drive
    # wherever it likes.
    far_wd = [m for m in wd_offs if float(m.group(8)) > MIN_DIST]
    if not far_wd:
        reasons = sorted({m.group(5) for m in rowoffs})
        print(f"FAIL (W1): with two allies parked at the pill and an enemy "
              f"shelling it, no row was rejected well_defended while we were "
              f"outside {MIN_DIST} tiles. Reasons seen: {reasons}")
        return 1
    for m in far_wd:
        wd = WD_RE.search(m.group(5))
        if not wd:
            print(f"FAIL (W1): a well_defended reject did not carry its "
                  f"numbers: {m.group(0)[:200]}")
            return 1
        foes, allies, ratio = (int(wd.group(1)), int(wd.group(2)),
                               int(wd.group(3)))
        if not (foes <= allies * ratio):
            print(f"FAIL (W1): the row claims well_defended but its own "
                  f"numbers say otherwise: foes {foes} > allies {allies} x "
                  f"R {ratio}. Line: {m.group(0)[:200]}")
            return 1
        if ratio != 1:
            print(f"FAIL (W1): teams are 3v3 so R must be ceil(3/3) = 1, but "
                  f"the row printed R={ratio}. Either a filler foe never "
                  f"joined or a tank died and left the teams uneven. "
                  f"Line: {m.group(0)[:200]}")
            return 1
        eng = wd_engine_allies(trace, int(m.group(1)) * 2)
        if allies != eng:
            print(f"FAIL (W1): at brain t={m.group(1)} the brain counted "
                  f"{allies} ally/allies at the pill but the ENGINE had "
                  f"{eng} parked within {G.WELL_DEFENDED_RADIUS} tiles. "
                  f"Line: {m.group(0)[:200]}")
            return 1
    m0 = far_wd[0]
    print(f"  W1 OK: {len(far_wd)} row(s) rejected {m0.group(5)} while we were "
          f"{m0.group(8)} tiles out (> {MIN_DIST}) with enemy {m0.group(6)} in "
          f"the ring and trigger ({m0.group(7)}) armed -- every one re-derives, "
          f"and its ally count matches the engine's own tank positions")

    # ── W2: and so the alarm never fires, and we never take the goal ─────
    if ons:
        print(f"FAIL (W2): the pill was held and the alarm still came ON "
              f"{len(ons)} time(s). First: {ons[0].group(0)[:200]}")
        return 1
    took = [(t, g) for (t, g) in goals if g == "defend_pill"]
    if took:
        print(f"FAIL (W2): the row was rejected every time and the bot still "
              f"took defend_pill, first at brain t={took[0][0]} "
              f"({len(took)} tick(s))")
        return 1
    print(f"  W2 OK: not one ALARM_ON in {len(rowoffs)} rejected row(s), and "
          f"defend_pill was never the goal on any of "
          f"{len(goals)} TICK_COST line(s)")
    return 0


def arena_WC(text, trace):
    """THE CONTROL -- the same 3v3 game with the two allies parked FAR away.

    allies_near is 0, so the pill is not held and the alarm has to fire exactly
    as it does in arena A.  Without this, arena W only says "the alarm did not
    fire in a six-tank game"."""
    ons = [m for m in (ON_RE.search(li) for li in text.splitlines()) if m]
    rowoffs = [m for m in (ROW_OFF_RE.search(li) for li in text.splitlines())
               if m]
    hp_rows = [r for k, r in trace if k == "hp"]
    dropped = [r for i, r in enumerate(hp_rows)
               if i > 0 and r[1] < hp_rows[i - 1][1]]
    extras = [r for k, r in trace if k == "extra"]
    near_end = wd_engine_allies(trace, 10 ** 9)
    wd_offs = [m for m in rowoffs if m.group(5).startswith(
        "alarm_off:well_defended")]
    print(f"  engine: {len(extras)} extra tank(s) spawned, "
          f"{len(dropped)} hit(s) on the pill, "
          f"{near_end} team-0 extra(s) within {G.WELL_DEFENDED_RADIUS} tiles "
          f"of it")
    print(f"  brain: {len(ons)} ALARM_ON, {len(rowoffs)} row-REJECT line(s), "
          f"{len(wd_offs)} of them well_defended")

    if len(extras) != 4:
        print(f"FAIL (WC): the scenario spawned {len(extras)} extra tank(s), "
              f"not 4, so this is not the same game as arena W with one thing "
              f"changed. extra rows: {extras}")
        return 1
    if near_end != 0:
        print(f"FAIL (WC): the control needs ZERO allies within "
              f"{G.WELL_DEFENDED_RADIUS} tiles of the pill and the engine had "
              f"{near_end}. The allies parked on the wrong tiles.")
        return 1
    if not dropped:
        print("FAIL (WC): the pill was never hit, so the alarm could not have "
              "fired for reasons that have nothing to do with condition 4.")
        return 1
    if wd_offs:
        print(f"FAIL (WC): no ally is within {G.WELL_DEFENDED_RADIUS} tiles of "
              f"the pill and {len(wd_offs)} row(s) were still rejected "
              f"well_defended. First: {wd_offs[0].group(0)[:200]}")
        return 1
    if not ons:
        print(f"FAIL (WC): with the allies parked far away the pill is not "
              f"held, so the alarm should fire exactly as in arena A -- and it "
              f"never did. Rejects seen: "
              f"{sorted({m.group(5) for m in rowoffs})}")
        return 1
    for m in ons:
        ok, msg, _f = check_on_line(m)
        if not ok:
            print(f"FAIL (WC): the ALARM_ON line does not add up -- {msg}")
            return 1
    f0 = check_on_line(ons[0])[2]
    print(f"  WC OK: move the same two allies out to 14 and 18 tiles and the "
          f"pill stops being held -- the alarm came ON at brain t={f0['t']} "
          f"(cost {f0['cost']:.0f}, {len(ons)} such line(s), all re-derive) "
          f"and NOT ONE row was rejected well_defended")
    return 0


CHECKS = {"A": arena_A, "B": arena_B, "B2": arena_B2, "C": arena_C,
          "D": arena_D, "W": arena_W, "WC": arena_WC}


def main():
    argv = sys.argv[1:]
    # Strips --asap/--no-asap from argv IN PLACE and records the decision in
    # the asap module; the return value is the decision, not the list.
    take_asap_flag(argv)
    variant = "all"
    ticks = None
    build_dir = DEFAULT_BUILD
    i = 0
    while i < len(argv):
        if argv[i] == "--variant" and i + 1 < len(argv):
            variant = argv[i + 1]; i += 2
        elif argv[i] == "--ticks" and i + 1 < len(argv):
            ticks = int(argv[i + 1]); i += 2
        elif argv[i] == "--build" and i + 1 < len(argv):
            build_dir = Path(argv[i + 1]); i += 2
        else:
            i += 1
    todo = VARIANTS if variant == "all" else [variant]
    for v in todo:
        if v not in CHECKS:
            print(f"unknown variant {v!r}; pick from {VARIANTS} or 'all'")
            return 1

    print("defend_pill ALARM MODE: an alarm is a precondition, not a bid")
    print(pacing_line())
    rc = 0
    for v in todo:
        n = ticks or TICKS[v]
        print(f"\n=== arena {v} ({n} engine ticks, port {PORTS[v]}, "
              f"tokens {TOKENS[v]}) ===")
        sess, text, trace = run_sim(v, n, build_dir)
        if sess is None:
            print(f"FAIL ({v}): {text}")
            rc = 1
            continue
        print(f"  session: {sess.name}")
        r = CHECKS[v](text, trace)
        if r:
            rc = 1
        else:
            print(f"  PASS ({v})")
    print()
    print("PASS: alarm mode holds" if rc == 0 else "FAIL")
    return rc


if __name__ == "__main__":
    sys.exit(main())
