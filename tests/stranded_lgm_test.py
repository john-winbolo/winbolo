#!/usr/bin/env python3
"""the stranded-LGM freeze -- a tank that has flagged its own man unreachable
must not sit still for ever waiting for him (GoalHunter 1.7).

WHAT FROZE (winbolo2, 2026-09-07, a 1v1 on Everard Island against 1.6)
----------------------------------------------------------------------
The 1.7 bot sat on tile (85,140) from engine tick 10899 to the end of a 180000
tick game: 87510 engine ticks in one place, alive throughout, armour never
changing, nothing printed. Three things were true at once and each one was
waiting on the other two:

  1  THE MAN WAS STRANDED. He went out on a builder-pool errand and ended up
     three tiles west of the tank in a spot his walk home could not leave.
     lgm.c lgmReturn walks a STRAIGHT LINE at the tank with a per-axis slide
     and has no re-route; the brain's own walk sim is the same straight line,
     returned -1, and goals.lua set state.lgm_stranded.

  2  attack_pill HELD IN plan_position WAITING FOR HIM. attack.lua's hold is
     "the LGM will return and we resume" -- and it had no timeout and no
     stranded check. It held for 79k ticks and printed nothing.

  3  rescue_lgm WAS SUPPRESSED BY A FIELD THAT ONLY MOVING COULD CLEAR. The
     rescue's fire gate was perc.under_fire, i.e. danger.danger_at(our tile)
     > 0 -- the STATIC pill-danger stamp. A CALM hostile or neutral pill
     stamps PILL_DANGER_BASE over a PILL_RANGE_MAP (9 tile) disk whether or
     not it ever fires, so a tank parked inside that disk reads "under fire"
     for ever, in silence, with a full armour bar. The builder pool had
     already written down why that reading is wrong for its own gate and uses
     danger.tank_fire_age instead; the rescue still used the field.

  The rescue waited for the field to clear. The field cleared only if the tank
  moved. The tank's only goal waited for the man. The man waited for the tank.

THE FIX, AS TWO KNOBS (both default true, both `false` in PRESETS.keel)
  RESCUE_LGM_SUPPRESS_BY_FIRE_AGE  rescue_lgm's fire gate reads
      danger.tank_fire_age -- something actually hit us, or a hostile round's
      closest approach lands inside SWERVE_HIT_RADIUS_WU -- and goes quiet
      BUILDER_POOL_UNDER_FIRE_TICKS after the last such event.
  ATTACK_PP_HOLD_SKIP_STRANDED     attack_pill does not hold in plan_position
      for a man the brain has already flagged state.lgm_stranded.

TWO ARENAS, TWO TOKENS APART, ON IDENTICAL GROUND
(tests/generate_stranded_lgm_map.py builds it and its docstring carries the
geometry and why every piece of it is where it is; the short version is a
forest room the neutral pillbox can never see into, a one-tile-wide gate the
sidecar turns to RIVER the moment the engine says the man is on the far side,
and exactly one open tile beyond that gate, occupied by a live pill, so the
TANK can never follow him across.)

  A  THE FIX. The gate opens on the static field: at least one RESCUE_GATE
     line with under_fire=true (the old rule would have suppressed) and
     fire_age=nil (nothing has ever been fired at us) and suppress=false.
     rescue_lgm is then actually picked, the tank goes and gets him, and
     man_status comes back to 0 -- read from the brain's own per-tick jsonl.
     No stationary spell over STUCK_ENGINE_TICKS while alive.

  B  THE CONTROL, two tokens different, same ground, same seed. The same
     moment reads mode=static, under_fire=true, suppress=true; rescue_lgm is
     never picked at all; attack_pill prints PP_HOLD_LGM with stranded=true
     and a held= count that keeps growing; and the tank does not move a tile
     for the rest of the run. That is the Everard freeze, in eight thousand
     ticks instead of a hundred and eighty thousand.

WHAT MAKES THIS EVIDENCE AND NOT A COINCIDENCE. Both arenas flood the same
gate column on the same ENGINE tick (the sidecar's trigger is the engine's own
report that our pill reached full armour, and the runs have not diverged that
early), so the two runs are handed an identical stranding and differ only in
what the brain does about it. Every outcome below is read from print2, from
the brain's per-tick jsonl, or from the sidecar's ENGINE-side pill trace --
never from the brain's opinion of itself.

Usage: python stranded_lgm_test.py [--variant A|B|all] [--ticks N] [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import json
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

sys.path.insert(0, str(HERE))
import generate_stranded_lgm_map as G   # noqa: E402

PORTS = {"A": 50480, "B": 50481}
# ENGINE ticks. The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are half as many brain ticks. The decisive
# moment is inside the first 600; the rest is there so "the tank never moved
# again" has room to be false if it wants to be.
TICKS = {"A": 8000, "B": 8000}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# WHY THE cfg= TOKEN THEY SHARE.
#   PILL_REPOSITION_ENABLED=false -- without it the bot decides its own damaged
#     pill is badly placed and SHOOTS IT DOWN to move it, then drives over the
#     corpse and pockets it (measured: armour 15 -> 0 between sim t=664 and
#     t=1028, `in_tank 1` at t=1118). The arena's only errand vanishes with it
#     and the man never goes anywhere. Nothing in this test is about
#     reposition, and it is off in BOTH arenas.
COMMON = "cfg=PILL_REPOSITION_ENABLED=false"
TOKENS = {
    "A": COMMON,
    "B": COMMON + ";cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false"
                  ";cfg=ATTACK_PP_HOLD_SKIP_STRANDED=false",
}

# ── print2 lines ─────────────────────────────────────────────────────────
# goals.lua, printed once per CHANGE of the gate's reading while the man is
# flagged stranded:
#   RESCUE_GATE t=253 stranded=1 mode=fire_age fire_age=nil why=nil
#     shot_by_tank=false under_fire=true suppress=false
GATE_RE = re.compile(
    r"RESCUE_GATE t=(\d+) stranded=1 mode=(\w+) fire_age=(\S+) why=(\S+) "
    r"shot_by_tank=(\w+) under_fire=(\w+) suppress=(\w+)")
# attack.lua, once per 50 brain ticks while the plan_position hold holds:
#   PP_HOLD_LGM t=258 pill=(126,130) man_status=2 stranded=true held=150
HOLD_RE = re.compile(
    r"PP_HOLD_LGM t=(\d+) pill=\((\d+),(\d+)\) man_status=(\d+) "
    r"stranded=(\w+) held=(\d+)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)")

LGM_INTANK, LGM_DEAD, LGM_MOVING = 0, 1, 2      # constants.lua

# A stationary spell longer than this, while alive, is the freeze. In ENGINE
# ticks, so it reads against the Everard figure (87510) directly. The measured
# runs: arena B never moved again from brain tick 132 (7736 engine ticks and
# still going at the tick limit); arena A's worst spell was 1866.
STUCK_ENGINE_TICKS = 3000


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
    """The sidecar's engine-side view: [(sim_tick, x, y, armour, owner,
    in_tank), ...] plus the `# flood <tick> tank=(x,y)` marker, returned
    separately. Written by the scenario sidecar, so "the man got across and the
    ground closed behind him" is asked of the ENGINE."""
    path = build_dir / f"stranded_lgm_{variant}_trace.log"
    seq, flood = [], None
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if line.startswith("# flood"):
                m = re.match(r"# flood (\d+) tank=\((\d+),(\d+)\)", line)
                if m:
                    flood = (int(m.group(1)), int(m.group(2)), int(m.group(3)))
                continue
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 6:
                seq.append(tuple(int(p) for p in parts[:6]))
    return seq, flood


def read_jsonl(build_dir, sess):
    """The brain's per-tick rows, as dicts. [] if the log is missing.

    NOT under the debug session directory: _G.DEBUG_SESSION_DIR is set by
    BrainTest and by nothing else, so under headless WinBoloDS logger.lua falls
    back to <cwd>/player0_<YYYYmmdd_HHMMSS>.jsonl -- i.e. straight into build/.
    The session directory is named <the same stamp>_<n>_<label>.

    Fields used here: t (brain tick, the same clock as the print2 t=), mx/my
    (the tank's tile) and lgm (info.man_status)."""
    stamp = "_".join(sess.name.split("_")[:2])
    cand = build_dir / f"player0_{stamp}.jsonl"
    if not cand.exists():
        floor = os.path.getmtime(sess) - 5
        pool = [p for p in build_dir.glob("player0_*.jsonl")
                if os.path.getmtime(p) >= floor]
        if not pool:
            return []
        cand = max(pool, key=os.path.getmtime)
    rows = []
    for line in cand.read_text(errors="ignore").splitlines():
        if not line.startswith('{"type":"tick"'):
            continue
        try:
            rows.append(json.loads(line))
        except ValueError:
            pass
    return rows


def spells(rows, after_brain_tick=0):
    """Runs of consecutive brain ticks on the same tile, as
    (start, end, (mx,my)) in BRAIN ticks. The tank is alive for all of them --
    a dead tank has no tile in this arena's jsonl."""
    out, cur = [], None
    for r in rows:
        if r["t"] < after_brain_tick:
            continue
        k = (r.get("mx"), r.get("my"))
        if cur and cur[2] == k:
            cur[1] = r["t"]
        else:
            if cur:
                out.append(tuple(cur))
            cur = [r["t"], r["t"], k]
    if cur:
        out.append(tuple(cur))
    return out


def worst_spell(rows, after_brain_tick=0):
    sp = spells(rows, after_brain_tick)
    if not sp:
        return 0, None
    w = max(sp, key=lambda s: s[1] - s[0])
    return (w[1] - w[0]) * 2, w          # x2: brain ticks -> engine ticks


def print_spells(rows, after_brain_tick, n=6):
    sp = sorted(spells(rows, after_brain_tick),
                key=lambda s: s[1] - s[0], reverse=True)[:n]
    print("       %-9s %-9s %-9s %s" % ("brain t0", "brain t1", "engine", "tile"))
    for (a, b, k) in sp:
        print("       %-9d %-9d %-9d (%s,%s)" % (a, b, (b - a) * 2, k[0], k[1]))


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_stranded_lgm_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"strandedlgm_{variant}"
    final = HERE / f"stranded_lgm_{variant}_final.json"
    stderr = HERE / f"stranded_lgm_{variant}_stderr.txt"
    trace = build_dir / f"stranded_lgm_{variant}_trace.log"
    for p in (final, stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    tokens = TOKENS[variant]
    if len(tokens) > ARG_MAX:
        return None, (f"the -bot-init token string is {len(tokens)} bytes, over "
                      f"the {ARG_MAX}-byte BRAIN_INIT_ARG limit -- it would be "
                      f"truncated mid-token and the last cfg= silently ignored:"
                      f"\n  {tokens}")
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"stranded_lgm_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby",
           "-gametype", "open",
           "-bots", "1", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(900, ticks // 4))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000])
    ours = sess / "print2_bot0.log"
    if not ours.exists():
        return None, f"no print2_bot0.log under {sess}"
    return sess, ours.read_text(errors="ignore")


def lua_errors(text):
    return [ln for ln in text.splitlines()
            if "attempt to " in ln or "stack traceback" in ln
            or (".lua:" in ln and "Error" in ln)][:5]


def dumps(text, needle, n=6, tail=False):
    lines = [l.strip() for l in text.splitlines() if needle in l]
    for ln in (lines[-n:] if tail else lines[:n]):
        print("   " + ln)


def the_stranding_is_real(step, variant, build_dir):
    """Shared by both arenas: the ENGINE says the repair landed (so the man was
    on the far tile) and the sidecar then cut the ground behind him. Without
    this every verdict below is about an arena that never stranded anybody."""
    trace, flood = read_trace(build_dir, variant)
    rows = [r for r in trace if (r[1], r[2]) == G.OURPILL]
    if not rows:
        print(f"FAIL ({step}): the sidecar traced no pill at {G.OURPILL} -- its "
              f"OURS list and the generator's geometry have drifted apart.")
        return 1, None
    full = next((r for r in rows if r[3] >= G.PILLS_MAX_HEALTH), None)
    if full is None:
        print(f"FAIL ({step}): the engine never saw {G.OURPILL} reach full "
              f"armour, so the man never made the trip and nothing was ever "
              f"stranded. Trace: "
              + ", ".join(f"{t}:a={a}" for t, _x, _y, a, _o, _k in rows[:12]))
        return 1, None
    stolen = [r for r in rows if r[4] != 0 or r[5] != 0]
    if stolen:
        print(f"FAIL ({step}): {G.OURPILL} changed hands or was pocketed at sim "
              f"t={stolen[0][0]} -- the arena lost its errand.")
        return 1, None
    if flood is None:
        print(f"FAIL ({step}): the sidecar never flooded the gate column. It "
              f"only floods on a tick where the pill reads full AND the tank is "
              f"west of x={G.GATE[0][0]}, and it re-damages the pill to try "
              f"again -- so this means the two never coincided in "
              f"{TICKS[variant]} ticks.")
        return 1, None
    print(f"  {step} OK: THE STRANDING IS REAL -- the engine saw {G.OURPILL} "
          f"reach {full[3]}/{G.PILLS_MAX_HEALTH} at sim t={full[0]} (so the man "
          f"was standing on it), and the sidecar turned the gate column "
          f"x={G.GATE[0][0]} to river at sim t={flood[0]} with the tank at "
          f"({flood[1]},{flood[2]}), west of it. The pill stayed ours and out "
          f"of anyone's tank across all {len(rows)} traced change(s).")
    return 0, flood


def check_A(sess, text, build_dir):
    rc, flood = the_stranding_is_real("0", "A", build_dir)
    if rc:
        return rc
    flood_brain = flood[0] // 2

    gates = GATE_RE.findall(text)
    if not gates:
        print("FAIL (1): rescue_lgm's gate never printed a RESCUE_GATE line, so "
              "the man was never flagged stranded and the arena measured "
              "nothing.")
        return 1
    for (t, mode, _fa, _why, _sbt, _uf, _sup) in gates:
        if mode != "fire_age":
            print(f"FAIL (1): the gate at t={t} read mode={mode}, not "
                  f"fire_age -- RESCUE_LGM_SUPPRESS_BY_FIRE_AGE is not on in "
                  f"the arena that is supposed to be testing it.")
            return 1
    print(f"  1 OK: the man was flagged stranded and the gate was asked about "
          f"it -- {len(gates)} RESCUE_GATE reading(s), every one of them on "
          f"the fire_age rule.")

    # THE WHOLE POINT, IN ONE LINE: the static field says "under fire", the
    # sustained clock says nothing has ever been fired at us, and the gate
    # sides with the clock. That combination is the Everard freeze, and here it
    # opens instead of closing.
    decisive = [g for g in gates
                if g[5] == "true" and g[2] == "nil" and g[6] == "false"]
    if not decisive:
        print("FAIL (2): no RESCUE_GATE line had under_fire=true (the OLD rule "
              "would have suppressed) together with fire_age=nil (nothing has "
              "ever hit us or been inbound) and suppress=false. Either the "
              "neutral pillbox is not stamping danger over the tank's room, or "
              "it is actually shooting at it -- both of which the arena's "
              "geometry is built to prevent. Gate lines seen:")
        for g in gates[:8]:
            print(f"   t={g[0]} mode={g[1]} fire_age={g[2]} under_fire={g[5]} "
                  f"suppress={g[6]}")
        return 1
    d = decisive[0]
    print(f"  2 OK: THE GATE OPENS ON THE STATIC FIELD -- at t={d[0]} the man "
          f"was stranded, perc.under_fire was true (the old rule's whole "
          f"input), danger.tank_fire_age was nil (nothing has ever been fired "
          f"at this tank) and the rescue was NOT suppressed. "
          f"{len(decisive)}/{len(gates)} gate readings were of that shape.")

    goals = GOAL_RE.findall(text)
    if "rescue_lgm" not in goals:
        print("FAIL (3): the gate opened but rescue_lgm was never actually "
              "picked as the goal.")
        dumps(text, "GOAL_CHANGE", tail=6)
        return 1
    print(f"  3 OK: rescue_lgm was picked ({goals.count('rescue_lgm')} "
          f"GOAL_CHANGE line(s)) -- the override the freeze suppressed.")

    rows = read_jsonl(build_dir, sess)
    if not rows:
        print("FAIL (4): no per-tick jsonl to read man_status from.")
        return 1
    back = [r["t"] for r in rows
            if r["t"] > flood_brain and r.get("lgm") == LGM_INTANK]
    if not back:
        print(f"FAIL (4): man_status never returned to 0 after the gate was "
              f"flooded at sim t={flood[0]} (brain t~{flood_brain}). The rescue "
              f"fired but the man was never actually fetched.")
        return 1
    print(f"  4 OK: THE MAN CAME HOME -- man_status back to LGM_INTANK at brain "
          f"t={back[0]}, {back[0] - flood_brain} brain tick(s) after the ground "
          f"closed behind him, and it read 0 on {len(back)} tick(s) after that. "
          f"(The tank crosses the river the man cannot: MAP_SPEED_TRIVER 3 "
          f"against MAP_MANSPEED_TRIVER 0.)")

    stuck, w = worst_spell(rows, flood_brain)
    if stuck >= STUCK_ENGINE_TICKS:
        print(f"FAIL (5): the tank still sat on one tile for {stuck} engine "
              f"ticks after the stranding (tile ({w[2][0]},{w[2][1]}), brain "
              f"ticks {w[0]}..{w[1]}), over the {STUCK_ENGINE_TICKS}-tick line.")
        print_spells(rows, flood_brain)
        return 1
    print(f"  5 OK: the longest stationary spell after the stranding is {stuck} "
          f"engine ticks (tile ({w[2][0]},{w[2][1]})), under the "
          f"{STUCK_ENGINE_TICKS}-tick line. Spells:")
    print_spells(rows, flood_brain)
    print("PASS (A): the man was stranded inside a calm pillbox's danger stamp, "
          "the rescue gate read the CLOCK instead of the FIELD and opened, "
          "rescue_lgm fired, and the man was back in the tank.")
    return 0


def check_B(sess, text, build_dir):
    rc, flood = the_stranding_is_real("0", "B", build_dir)
    if rc:
        return rc
    flood_brain = flood[0] // 2

    gates = GATE_RE.findall(text)
    if not gates:
        print("FAIL (1): no RESCUE_GATE line -- the man was never flagged "
              "stranded, so this control proves nothing.")
        return 1
    static = [g for g in gates if g[1] == "static"]
    if not static:
        print("FAIL (1): every gate reading was mode=fire_age -- the "
              "cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false token did not land, "
              "so this is not the control it claims to be.")
        return 1
    print(f"  1 OK: the man was flagged stranded here too -- {len(gates)} "
          f"RESCUE_GATE reading(s), {len(static)} of them on the old static "
          f"rule the tokens ask for.")
    frozen = [g for g in static if g[5] == "true" and g[6] == "true"]
    if not frozen:
        print("FAIL (2): the OLD rule never suppressed the rescue in this "
              "arena, so arena A's opening is not evidence about the change. "
              "Gate lines:")
        for g in gates[:8]:
            print(f"   t={g[0]} mode={g[1]} fire_age={g[2]} under_fire={g[5]} "
                  f"suppress={g[6]}")
        return 1
    f = frozen[0]
    print(f"  2 OK: THE OLD RULE SUPPRESSES -- at t={f[0]} the same stranding, "
          f"on the same ground, read under_fire=true from the static field with "
          f"fire_age={f[2]} (nothing has ever been fired at this tank) and "
          f"suppressed the rescue anyway.")

    goals = GOAL_RE.findall(text)
    if "rescue_lgm" in goals:
        print(f"FAIL (3): rescue_lgm was picked {goals.count('rescue_lgm')} "
              f"time(s) with the old rule on -- the control leaked.")
        return 1
    print("  3 OK: rescue_lgm was never picked at all in the control.")

    holds = [h for h in HOLD_RE.findall(text) if h[4] == "true"]
    if not holds:
        print("FAIL (4): attack_pill never printed PP_HOLD_LGM with "
              "stranded=true, so the second half of the deadlock -- the take "
              "holding in plan_position for a man who is not coming back -- "
              "never happened here.")
        dumps(text, "PP_HOLD_LGM", tail=6)
        return 1
    longest = max(int(h[5]) for h in holds)
    print(f"  4 OK: attack_pill HELD FOR A MAN IT KNEW WAS STRANDED -- "
          f"{len(holds)} PP_HOLD_LGM line(s) with stranded=true on pill "
          f"({holds[0][1]},{holds[0][2]}), the longest reaching held="
          f"{longest} brain ticks.")

    rows = read_jsonl(build_dir, sess)
    if not rows:
        print("FAIL (5): no per-tick jsonl to read the tank's tile from.")
        return 1
    back = [r["t"] for r in rows
            if r["t"] > flood_brain and r.get("lgm") == LGM_INTANK]
    if back:
        print(f"FAIL (5): man_status returned to 0 at brain t={back[0]} even "
              f"with the old rule on -- the arena did not actually strand him.")
        return 1
    stuck, w = worst_spell(rows, flood_brain)
    if stuck < STUCK_ENGINE_TICKS:
        print(f"FAIL (5): the longest stationary spell is only {stuck} engine "
              f"ticks, under the {STUCK_ENGINE_TICKS}-tick line -- the control "
              f"did not freeze, so arena A's result is not a difference this "
              f"change made. Spells:")
        print_spells(rows, flood_brain)
        return 1
    print(f"  5 OK: THE FREEZE -- the tank sat on tile ({w[2][0]},{w[2][1]}) "
          f"for {stuck} engine ticks (brain {w[0]}..{w[1]}) and the man never "
          f"came home. That is the Everard shape, at 1/11th the length.")
    print_spells(rows, flood_brain)
    print("PASS (B): two tokens back, on the same ground and the same seed, "
          "the static field suppresses the rescue, the take holds for a man it "
          "knows is stranded, and the tank stops moving.")
    return 0


CHECKS = {"A": check_A, "B": check_B}


def run(variant, ticks, build_dir):
    print(f"=== arena {variant} ({ticks} engine ticks, tokens: "
          f"{TOKENS[variant]}) ===")
    sess, text = run_sim(variant, ticks, build_dir)
    if sess is None:
        print(f"FAIL: {text}")
        return 1
    print(f"  session: {sess.name}")
    errs = lua_errors(text)
    if errs:
        print("FAIL: Lua errors in our bot's log:")
        for e in errs:
            print("   " + e.strip())
        return 1
    return CHECKS[variant](sess, text, build_dir)


def main():
    variant, ticks, build = "all", None, DEFAULT_BUILD
    args = sys.argv[1:]
    take_asap_flag(args)
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--variant":
            variant = args[i + 1]; i += 2
        elif args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        else:
            i += 1
    variants = list(CHECKS) if variant == "all" else variant.split(",")
    unknown = [v for v in variants if v not in CHECKS]
    if unknown:
        print(f"unknown arena(s): {', '.join(unknown)} "
              f"(have: {', '.join(CHECKS)})")
        sys.exit(2)
    rc = 0
    for v in variants:
        try:
            rc |= run(v, ticks or TICKS[v], build)
        except subprocess.TimeoutExpired:
            print(f"FAIL ({v}): run timed out")
            rc = 1
    sys.exit(rc)


if __name__ == "__main__":
    main()
