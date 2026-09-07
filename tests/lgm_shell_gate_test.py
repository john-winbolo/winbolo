#!/usr/bin/env python3
"""the shell gate — the builder pool will not step the man out in front of a
round that is already in the air (GoalHunter 1.7).

THE RULE (author, 2026-09-06, verbatim)
---------------------------------------
"if we know we can predict shells for at most 63 ticks, let's do that, and if
any shell will kill our builder (predict the builder for 63 ticks also) then we
should HARD STOP sending it out right then. shells and builder are very
predictable so this is worth doing. I realize a tank can impact reality quicker
than 63 ticks but it's a good start."

At the moment the pool would DISPATCH -- after every other gate has passed, and
only then, so it is paid for once a tick at most -- danger.lgm_shell_gate flies
every shell the brain can see forward tick by tick for up to
LGM_SHELL_PREDICT_TICKS (63 ENGINE ticks, the longest a shell can live:
shells.c shellLifeTicks = 1 + 8 x range - 6 at a pillbox's PILLBOX_FIRE_DISTANCE
of 8.5 tiles), walks the man forward beside it on the SAME sim the trip is
priced with, and refuses the tick outright if the engine's own LGM kill rule
would fire. It is a HARD STOP, not a score term, and it refuses THE TICK, not
the job: the row stays a candidate and goes the moment the round has landed.

WHAT ACTUALLY KILLS THE MAN, and why the gate looks for an ENDING rather than a
crossing (src/bolo/lgm.c lgmDeathCheckAtPosition:1264, reached from shells.c
only when a shell EXPLODES -- the collision path at line 368 and the
end-of-life path at line 452):

    solid explosion tile (wall / live pill)  -> the man dies only if his TILE
                                                is the explosion tile
    anywhere else                            -> he dies within
                                                MAP_SQUARE_MIDDLE (128 WU)

A shell flying THROUGH him does not touch him. So a shell has three endings the
gate tests for: it reaches a wall or a live pill, it reaches OUR HULL (within
TANK_HIT_RADIUS, 112 WU -- a shell never hits its own owner), or it runs out of
life, which is a known tick because the engine's `length` now reaches the brain.

TWO RUNS, ONE TOKEN APART (tests/generate_lgm_shell_gate_map.py builds the
ground; its docstring carries the geometry and why every piece of it is where
it is). A sealed 3x3 room walled three deep, a NEUTRAL pillbox four tiles west
behind a moat shelling the tank all run, and OUR damaged pill in the room for
the man to walk to.

  A  THE GATE REFUSES, AND THEN LETS HIM GO. At least one BP_DENY
     reason=shell_will_hit, every one of them naming whose round it is, how
     many ENGINE ticks until it lands and which of the three endings it is --
     and then the SAME row dispatched again within a few ticks, once that round
     has landed. That second half is the whole character of the rule: "not this
     tick", never "not this job".

     As measured on 2026-09-06 at -seed 42 -brain-lua-seed 42: refused at
     t=1080 (`shell from pill at +3t, tank`) and dispatched at t=1083; refused
     at t=2898 (`+21t`) and dispatched at t=2918. 24 dispatches in the run.

     (Those figures moved on 2026-09-06 when perception.lua's friendly-fire
     repair guard was fixed -- it bit-tested a shell's `info`, which is an ENUM
     (shells.h SHELLS_BRAIN_FRIENDLY 0 / HOSTILE 1 / NEUTRAL 2), with the
     bitfield predicate a TANK's info byte wants, so every round this arena's
     NEUTRAL pillbox fired was booked as our own team's friendly fire and our
     pill went unrepairable for REPAIR_FRIENDLY_FIRE_REJECT_TICKS after each
     one. The earlier 26/t=3070/t=3280 run had the same bug; it survived it
     only because the pre-alarm defend_pill ladder happened to park the tank
     off-centre, which left ~900-tick gaps in the mis-stamping.)

  B  THE CONTROL, one token different (cfg=BUILDER_POOL_SHELL_GATE=false).
     `shell_will_hit` must never appear -- that is what makes A's refusals
     evidence about the RULE rather than about a run that happened to print
     something. B must also still dispatch, and the arena must be shown to be
     genuinely lethal: the man is killed by a round at least once, read from
     the brain's own per-tick jsonl (man_status LGM_DEAD) and from the `lgm=dead`
     chip the pool prints on the errand that ends that way.

WHAT THIS TEST DOES NOT CLAIM, because it is not true and the author said so
first: that the gate keeps the man alive. It only knows about rounds ALREADY IN
THE AIR. A pillbox that reloads while the man is out fires a round the gate was
never shown, and the man dies to it -- which happens in BOTH arenas here (twice
in A, once in B at this seed). The two death counts are PRINTED rather than
asserted against each other, because one run's difference between one death and
two is not evidence of anything. What IS asserted is the mechanism: the gate
fires, it names a real round, and it costs nothing but a few ticks.

Every outcome is read from print2, from the brain's per-tick jsonl, or from the
sidecar's ENGINE-side pill trace -- never from the brain's opinion of itself.

Usage: python lgm_shell_gate_test.py [--variant A|B|all] [--ticks N] [--build DIR]
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
import generate_lgm_shell_gate_map as G   # noqa: E402

PORTS = {"A": 50281, "B": 50282}
# ENGINE ticks. The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks. The run needs
# room for the tank to come ashore (it sits on its pond for a while: it has no
# goal it can afford until the pool starts feeding it errands) and then for a
# few dozen errands.
TICKS = {"A": 8000, "B": 8000}

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# WHY THE cfg= TOKENS.
#   BUILDER_POOL_UNDER_FIRE_TICKS=0 -- the pool's OWN under-fire clock wants
#     BUILDER_POOL_UNDER_FIRE_TICKS (100) brain ticks with nothing inbound
#     before the man may leave, and this arena is a tank being shelled every 50
#     engine ticks: the clock never runs out, nothing is ever dispatched, and
#     the shell gate is never reached. Turning it off is what puts the question
#     to the NEW gate instead of to the old one.
#   PILL_REPOSITION_ENABLED=false -- without it the bot decides its own damaged
#     pill is badly placed and SHOOTS IT DOWN to move it (goal
#     capture_pill/gather/reposition_shoot); a measured run watched the armour
#     walk 11 -> 3 in the sidecar trace while the pool sat on `mode_owned`.
# Both are in BOTH arenas. The single token that differs is the gate itself.
COMMON = "cfg=BUILDER_POOL_UNDER_FIRE_TICKS=0;cfg=PILL_REPOSITION_ENABLED=false"
TOKENS = {
    "A": COMMON,
    "B": COMMON + ";cfg=BUILDER_POOL_SHELL_GATE=false",
}
GAMETYPE = {"A": "open", "B": "open"}

# ── print2 lines (builder_pool.lua) ───────────────────────────────────────
# BP_DENY t=3070 job=topup target=(127,125) reason=shell_will_hit (shell from
#   pill at +15t, tank) elig=mode_owned (gather) score=100 trip=82 trees=21/1 res=5
DENY_SHELL_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) "
    r"reason=shell_will_hit \(shell from (\S+) at \+(\d+)t, (\w+)\)")
DENY_ANY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(.*?) elig=")
DISP_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)")
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) ")
# BP_ABORT t=3083 job=topup target=(127,125) why=no_change+lgm_dead took=8t
#   lgm=dead (pill friendly hp 11 -> 11, LGM KILLED ...)
LGM_DEAD_RE = re.compile(
    r"BP_(?:DONE|ABORT) t=(\d+) job=(\S+) target=\((\d+),(\d+)\).*? lgm=dead")

# The gate's own horizon, in ENGINE ticks. A refusal that names a tick offset
# outside 1..this is the gate lying about its own window.
PREDICT_TICKS = 63
# Brain ticks the SAME row may take to be dispatched after a refusal before we
# call it "refused and then never went". The round it named lands within
# PREDICT_TICKS/2 brain ticks, and the pool re-asks every tick; measured at 5
# and 2 on the two refusals of the reference run.
REDISPATCH_GRACE = 120

LGM_INTANK, LGM_DEAD, LGM_MOVING = 0, 1, 2      # constants.lua 223-225


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
    """The sidecar's engine-side view of OUR pill, one row per CHANGE:

        [(sim_tick, x, y, armour, owner, in_tank), ...]

    Written by the scenario sidecar, so "the repair actually landed" is asked of
    the ENGINE and not of the brain's opinion of itself."""
    path = build_dir / f"lgm_shell_gate_{variant}_trace.log"
    seq = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 6:
                seq.append(tuple(int(p) for p in parts[:6]))
    return seq


def read_jsonl(build_dir, sess):
    """The brain's per-tick rows, as dicts. [] if the log is missing.

    NOT under the debug session directory: _G.DEBUG_SESSION_DIR is set by
    BrainTest and by nothing else, so under headless WinBoloDS logger.lua falls
    back to <cwd>/player0_<YYYYmmdd_HHMMSS>.jsonl -- i.e. straight into build/.
    The session directory is named <the same stamp>_<n>_<label>, both being
    strftime at startup, so the two are paired on the stamp; the fallback covers
    the second in which the two clocks could disagree.

    Fields used here: t (brain tick, the SAME clock as BP_DISPATCH's t=) and
    lgm (info.man_status: 0 in tank, 1 dead, 2 out on a mission)."""
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


def man_death_ticks(rows):
    """Brain ticks on which man_status ENTERS LGM_DEAD. Edges, not samples, so a
    man who stays dead for 200 ticks counts once."""
    out, prev = [], None
    for r in rows:
        cur = r.get("lgm")
        if cur == LGM_DEAD and prev != LGM_DEAD:
            out.append(r["t"])
        prev = cur
    return out


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_lgm_shell_gate_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"lgmshell_{variant}"
    final = HERE / f"lgm_shell_gate_{variant}_final.json"
    stderr = HERE / f"lgm_shell_gate_{variant}_stderr.txt"
    trace = build_dir / f"lgm_shell_gate_{variant}_trace.log"
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
    cmd = [str(ds), "-map", str(HERE / f"lgm_shell_gate_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby",
           "-gametype", GAMETYPE[variant],
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


def repair_landed(step, trace):
    """The engine saw our pill's armour reach full at least once -- so the man
    really did arrive and really did work, and the errands the gate is gating
    were not empty round trips."""
    rows = [r for r in trace if (r[1], r[2]) == G.OURPILL]
    if not rows:
        print(f"FAIL ({step}): the sidecar traced no pill at {G.OURPILL} -- its "
              f"OURS list and the generator's geometry have drifted apart.")
        return 1
    full = next((r for r in rows if r[3] >= G.PILLS_MAX_HEALTH), None)
    if full is None:
        print(f"FAIL ({step}): the engine never saw {G.OURPILL} reach full "
              f"armour, so no errand ever completed and the gate was never "
              f"gating anything real. Trace: "
              + ", ".join(f"{t}:a={a}" for t, _x, _y, a, _o, _k in rows[:12]))
        return 1
    stolen = [r for r in rows if r[4] != 0]
    if stolen:
        print(f"FAIL ({step}): {G.OURPILL} changed hands at sim t={stolen[0][0]}"
              f" -- the arena lost its errand.")
        return 1
    print(f"  {step} OK: THE ERRAND IS REAL -- the engine saw {G.OURPILL} reach "
          f"{full[3]}/{G.PILLS_MAX_HEALTH} armour at sim t={full[0]}, and it "
          f"stayed ours across all {len(rows)} traced change(s)")
    return 0


# ── arena A: the gate refuses, and then lets him go ──────────────────────
def check_A(sess, text, build_dir):
    denies = DENY_SHELL_RE.findall(text)
    disps = DISP_RE.findall(text)
    pool = POOL_RE.findall(text)

    if not pool:
        print("FAIL (0): the builder pool never ran at all.")
        return 1
    live = sum(1 for (_t, _o, _e, c, _k) in pool if int(c) >= 1)
    print(f"  0 OK: the pool ran on {len(pool)} tick(s), {live} of them with a "
          f"candidate on the table")

    if not disps:
        print("FAIL (1): the man was never dispatched at all, so the gate was "
              "never reached -- it only runs on a row that has passed every "
              "other test.")
        dumps(text, "BP_DENY", tail=True)
        return 1
    print(f"  1 OK: {len(disps)} dispatch(es) -- the gate was consulted on "
          f"every one of them")

    if not denies:
        print("FAIL (2): `shell_will_hit` never appeared. Either no round was "
              "ever going to land on the man at a moment the pool was ready, "
              "or the gate is not running. The arena is built to make the "
              "first unlikely (see generate_lgm_shell_gate_map.py: dozens of "
              "dispatch decisions against a pillbox firing every "
              f"{G.NP_SPEED} engine ticks). Last few denies:")
        dumps(text, "BP_DENY", tail=6)
        return 1

    for (t, job, mx, my, src, off, how) in denies:
        off = int(off)
        if not (1 <= off <= PREDICT_TICKS):
            print(f"FAIL (2): a refusal at t={t} names +{off}t, outside the "
                  f"gate's own 1..{PREDICT_TICKS} engine-tick window -- the "
                  f"line and LGM_SHELL_PREDICT_TICKS disagree.")
            return 1
        if how not in ("tank", "expiry", "wall", "unknown_life"):
            print(f"FAIL (2): a refusal at t={t} names ending '{how}', which is "
                  f"not one of the three the gate can produce.")
            return 1
    print(f"  2 OK: {len(denies)} refusal(s), each naming a round, an ENGINE "
          f"tick offset inside 1..{PREDICT_TICKS} and one of the three endings:"
          + "".join(f"\n       t={t} {job}@({mx},{my}) shell from {src} "
                    f"at +{off}t, ends on {how}"
                    for (t, job, mx, my, src, off, how) in denies))

    # 3. "NOT THIS TICK", NOT "NOT THIS JOB". Each refused row has to be
    #    dispatched again shortly after -- that is the half of the rule that
    #    stops the gate from being a permanent block on a shelled lane.
    for (t, job, mx, my, src, off, how) in denies:
        t = int(t)
        later = [d for d in disps
                 if int(d[0]) >= t and (int(d[2]), int(d[3])) == (int(mx), int(my))
                 and int(d[0]) - t <= REDISPATCH_GRACE]
        if not later:
            print(f"FAIL (3): the row at ({mx},{my}) was refused at t={t} and "
                  f"never dispatched again within {REDISPATCH_GRACE} brain "
                  f"ticks. The gate is supposed to refuse THE TICK, not the "
                  f"job -- the round it named lands in {off} engine ticks and "
                  f"the row should go straight after.")
            dumps(text, "BP_DISPATCH", tail=6)
            return 1
        print(f"  3 OK: refused at t={t} (round lands in {off} engine ticks), "
              f"and the SAME row went at t={later[0][0]} -- "
              f"{int(later[0][0]) - t} brain tick(s) later")

    rc = repair_landed("4", read_trace(build_dir, "A"))
    if rc:
        return rc

    deaths = man_death_ticks(read_jsonl(build_dir, sess))
    chips = LGM_DEAD_RE.findall(text)
    print(f"  5 --: the man was killed {len(deaths)} time(s) in this arena "
          f"(jsonl LGM_DEAD at {deaths}; {len(chips)} `lgm=dead` chip(s) on the "
          f"pool's own errand records). NOT an assertion: the gate only knows "
          f"about rounds ALREADY IN THE AIR, so a pillbox that reloads while "
          f"the man is out still kills him -- 'I realize a tank can impact "
          f"reality quicker than 63 ticks but it's a good start.'")
    print("PASS (A): the gate refused the dispatch while a named round was "
          "inbound, and let the same row go once it had landed.")
    return 0


# ── arena B: the control ─────────────────────────────────────────────────
def check_B(sess, text, build_dir):
    denies = DENY_SHELL_RE.findall(text)
    disps = DISP_RE.findall(text)

    if denies:
        print(f"FAIL (1): `shell_will_hit` appeared {len(denies)} time(s) with "
              f"cfg=BUILDER_POOL_SHELL_GATE=false -- the flag does not turn the "
              f"gate off, so arena A's refusals say nothing about the rule.")
        dumps(text, "shell_will_hit", 4)
        return 1
    print("  1 OK: `shell_will_hit` never appeared -- the flag really does "
          "switch the gate off, so A's refusals are the rule and not the arena")

    if not disps:
        print("FAIL (2): nothing was dispatched here either, so this run is not "
              "a control for anything -- the two arenas have to differ ONLY in "
              "the gate.")
        dumps(text, "BP_DENY", tail=6)
        return 1
    print(f"  2 OK: {len(disps)} dispatch(es), the same errands arena A was "
          f"gating")

    rc = repair_landed("3", read_trace(build_dir, "B"))
    if rc:
        return rc

    # 4. THE ARENA IS LETHAL. Without this, A's refusals could be refusals about
    #    rounds that were never going to hurt anybody.
    deaths = man_death_ticks(read_jsonl(build_dir, sess))
    chips = LGM_DEAD_RE.findall(text)
    if not deaths and not chips:
        print("FAIL (4): with the gate off the man was never killed, so this "
              "ground is not actually dangerous to him and arena A was "
              "refusing rounds that could not have connected. Move the "
              "neutral pillbox closer, or lengthen the run.")
        return 1
    print(f"  4 OK: THE GROUND IS LETHAL -- with the gate off the man was "
          f"killed {len(deaths)} time(s) (jsonl LGM_DEAD at {deaths}), and the "
          f"pool closed {len(chips)} errand(s) with an `lgm=dead` chip. So the "
          f"rounds arena A refused to walk into were rounds that kill.")
    print("PASS (B): one token different, no refusals, and the man dies to the "
          "shelling this arena is made of.")
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
