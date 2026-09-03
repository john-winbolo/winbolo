#!/usr/bin/env python3
"""Blocked-shot-line re-aim bot test.

Generates the blocked-aim arena (generate_blocked_aim_map.py: a flat 41x41
grass field, a NEUTRAL 7-armour target pill at (126,126), a partial ring of
OUR OWN live pills in the annulus 2-3 tiles around it with the due-west
corridor left open, and the bot spawning at (136,136), diametrically opposite
that corridor), runs one GoalHunter_1.7 bot and asserts it still takes the
pill.  The generator's docstring carries the full geometry rationale; the
test imports its constants so the two cannot drift.

The abort under test fires on the tank's LIVE HULL HEADING while it is still
driving in — `shot_path_obstacle_count(info, goal, world)` with no aim
override — not on the planned line, which the standoff scan already screens.
Pre-fix that is CHARGE_ABORT_OBSTACLE -> clear_attack_goal("shot path
blocked: ..."), or SANITY_PILL_REPLANS_MAX blocked replans escalating to
SANITY_ABANDON.

Our ring pills are made PERMANENT by tests/blocked_aim.scenario.lua, which
puts DEEP SEA back under each at round setup (mapRead paves ROAD under every
map-file pill).  Without that the bot drives over its own pills and
repositions them and the arena stops blocking anything — measured, the lone
blocker of an earlier layout was gone by tick 1249.

STATUS: this arena DISCRIMINATES.  Measured on the same arena, same seed:
  fixed brain    target taken tick 3249, 0 aborts, 0 abandons, 1 SANITY_BAN  PASS
  baseline brain target NEVER taken (armour 7 at tick 5000), 1 SANITY_ABANDON,
                 5 SANITY_BAN                                                FAIL

It did not discriminate at first, and the reason was a SECOND bug that this
arena flushed out: attack_shield.M.scan never honoured
state.banned_pill_angles.  The 5-degree sweep skips banned buckets, but the
shield scan then nudges the standoff several degrees along the standoff circle
and lands back inside the bucket the take just banned -- so two of the three
"spots tried" before SANITY_ABANDON were the SAME spot (20260901_032030 bot0:
banned 140..150 at t=506, deg=146.25 adopted at t=510, banned again at t=531;
146.25 is not a multiple of 5, which is the tell).  That is why thickening the
ring from 6 to 8 to 10 pills never produced a third distinct try.  With the
shield scan filtering banned angles, one ban is enough to reach a genuinely
different spot and the take completes.

--require-ladder is the STRICT MODE: it additionally demands that a blocked
shot line was actually exercised, so a green result cannot come from the arena
having quietly stopped blocking anything.  Use it whenever the result is going
to be quoted as evidence.

PASS requires ALL of:
  1. the TARGET pill ends up dead (armour 0) or captured by our bot;
  2. NO clear_attack_goal with a "shot path blocked" reason — in print2 that
     is the CHARGE_ABORT_OBSTACLE / SHOOT_PILL_ABORT_OBSTACLE tag each such
     clear is emitted next to (attack.lua ~5978 / ~6552);
  3. NO SANITY_ABANDON and NO BLITZ_GO_ABANDON at all.  The baseline brain
     abandons at t=556 here and the pill is still standing at tick 5000; the
     fixed brain reaches a genuinely different spot on its first ban and never
     abandons.
  4. STRICT MODE (--require-ladder only): at least one re-aim/blocked-spot
     tag, so a green run proves the blocked-line ladder actually ran rather
     than the arena having quietly stopped blocking anything.  Use this
     whenever the result is going to be used as evidence — see STATUS below,
     which is why strict mode currently reports FAIL on the fixed brain.

Also REPORTED (not part of PASS unless --require-ladder): the fixed brain's
own telemetry — CHARGE_REAIM / SHOOT_PILL_REAIM / BLITZ_GO_REAIM /
SANITY_REAIM / SHOOT_PILL_REAIM_RELINE, the blocked-spot replans
(CHARGE_BLOCKED_AT_SPOT / SHOOT_PILL_BLOCKED / BLITZ_GO_BLOCKED /
BLITZ_RECOMMIT), and the plan_position replan traffic (SANITY_REPLAN /
SANITY_BAN / PP_TO_APPROACH).  None of these exist in the pre-fix brain.

Notes on the evidence streams:
  * Pill state comes from -finaljson / -snapjson (a jsonl).  Pills are keyed
    by INDEX (0 = target, 1.. = ours) because a captured pill moves off its
    tile — see generate_blocked_aim_map.py, which writes them in that order.
  * GoalHunter's per-tick behaviour trace (player0.jsonl) is gated on
    _G._JSONL_LOGGER_ENABLED, which only BrainTest's debug-modules panel
    sets, so WinBoloDS never writes it for player 0.  The test picks it up if
    it is there and otherwise works from print2_bot0.log, which carries every
    tag the PASS conditions need.
  * clear_attack_goal's own "[clear_attack_goal] <reason>" line goes through
    Lua print() -> wb_log, which is silent unless WINBOLO_LOG is set.  Pass
    --lua-log to turn that on (WINBOLO_LOG=lua=debug) and have the reason
    strings themselves checked too; it is off by default because it is
    noisy and slow.

Ownership: set from the map file's pill owner bytes (0xFF -> NEUTRAL target,
0 -> our ring), the same mechanism tests/generate_water_pills_map.py uses, and
re-pinned by the sidecar's on_setup.  The test reports how many ring pills
were actually standing, at the first snapshot and at the end.

Companion to tests/cliff_staircase_test.py and tests/water_pills_test.py
(same harness pattern).

Usage: python blocked_aim_test.py [--ticks N] [--build DIR] [--lua-log]
                                  [--require-ladder] [--simple]
Exit 0 on PASS, 1 on FAIL.
"""

import os
import re
import sys
import json
import glob
import subprocess
from pathlib import Path

# -asap by default: ticks run back-to-back instead of one per 20 ms of wall
# clock. Same seed -> byte-identical game, just faster. --no-asap (or
# WINBOLO_ASAP=0) puts this run back on the 20 ms live-game pacing.
from asap import asap_args, pacing_line, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"
MAP = HERE / "blocked_aim.map"
FINAL = HERE / "blocked_aim_final.json"
SNAP = HERE / "blocked_aim_snap.jsonl"
STDERR = HERE / "blocked_aim_stderr.log"
LABEL = "_blocked_aim_test"

# Geometry comes straight from the generator so the two can never drift
# (in-game == file coords; the arena is already centred so mapCenter's shift
# is (0,0)).  Pill order in the map file is target first, then ours.
sys.path.insert(0, str(HERE))
import generate_blocked_aim_map as gen        # noqa: E402

TARGET = gen.TARGET
RING = gen.RING
START = gen.START
TARGET_IDX = 0
OUR_IDXS = list(range(1, 1 + len(RING)))
OUR_PLAYER = gen.OUR_PLAYER

# A clear_attack_goal("shot path blocked: ...") is always emitted right after
# one of these print2 tags, so they are the log-visible form of condition 2.
BLOCKED_TAGS = ("CHARGE_ABORT_OBSTACLE", "SHOOT_PILL_ABORT_OBSTACLE")
ABANDON_TAGS = ("SANITY_ABANDON", "BLITZ_GO_ABANDON")
# The fixed brain's blocked-line ladder: re-aim onto a corner of the target,
# then re-plan the firing spot.  None of these tags exist pre-fix.
REAIM_RE = re.compile(r"\b([A-Z][A-Z0-9_]*_REAIM(?:_RELINE)?)\b")
LADDER_TAGS = ("SANITY_BLOCKED", "CHARGE_BLOCKED_AT_SPOT",
               "SHOOT_PILL_BLOCKED",
               "BLITZ_GO_BLOCKED", "BLITZ_RECOMMIT")
REPLAN_TAGS = ("SANITY_REPLAN", "SANITY_BAN", "PP_TO_APPROACH")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{LABEL}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def matching_lines(text, needles):
    """Every line of `text` containing any of `needles`, in order."""
    out = []
    for line in text.splitlines():
        for n in needles:
            if n in line:
                out.append(line.strip())
                break
    return out


def pill_at_index(record, idx):
    pbs = record.get("pillboxes") or []
    return pbs[idx] if idx < len(pbs) else None


def read_brain_jsonl(sess):
    """GoalHunter's per-tick behaviour trace, when the host wrote one.  Returns
    (path, [tick records]) or (None, []).  Under WinBoloDS the player-0 trace
    is normally absent (see the module docstring), so this is best-effort."""
    if not sess:
        return None, []
    cands = sorted(glob.glob(str(sess / "player*.jsonl"))
                   + glob.glob(str(sess / "brain_p*.jsonl")))
    if not cands:
        return None, []
    path = Path(cands[0])
    recs = []
    for line in path.read_text(errors="ignore").splitlines():
        line = line.strip()
        if not line:
            continue
        try:
            d = json.loads(line)
        except ValueError:
            continue
        if d.get("type") == "tick":
            recs.append(d)
    return path, recs


def run(ticks, build_dir, lua_log, require_ladder, simple):
    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    gen_cmd = [sys.executable, str(HERE / "generate_blocked_aim_map.py")]
    if simple:
        gen_cmd.append("--simple")
    subprocess.run(gen_cmd, check=True, stdout=subprocess.DEVNULL)
    if not (HERE / "blocked_aim.scenario.lua").exists():
        print("FAIL: blocked_aim.scenario.lua is missing — without it the "
              "blocker pill is reachable and the arena stops blocking")
        return 1
    # Clear the previous run's outputs.  On Windows these stay locked while an
    # earlier WinBoloDS is still alive, so say that plainly instead of dying
    # with a PermissionError traceback.
    for p in (FINAL, SNAP, STDERR):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked — a previous WinBoloDS run "
                      f"is still running. Wait for it to exit, then retry.")
                return 1

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=LABEL)
    if lua_log:
        # Routes the brain's Lua print() (and therefore the literal
        # "[clear_attack_goal] <reason>" lines) into the captured stderr.
        env["WINBOLO_LOG"] = "lua=debug"
    cmd = [str(ds), "-map", str(MAP), "-port", "50046", "-nolobby",
           "-gametype", "open", "-bots", "1", "-brain", str(BRAIN),
           # yesfull: both pills and the whole (tiny) arena are known from
           # tick 0 — the test is about aiming past a known blocker, not fog.
           "-ai", "yesfull",
           "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(FINAL),
           "-snapjson", str(SNAP), "-snapinterval", "250",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(STDERR, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(240, ticks // 40))

    sess = newest_session(build_dir)
    if not sess:
        print("FAIL: no debug session produced")
        return 1
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        print(f"FAIL: brain crashed — see {crashes[0]}")
        return 1
    log = sess / "print2_bot0.log"
    if not log.exists():
        print(f"FAIL: no print2_bot0.log under {sess}")
        return 1
    text = log.read_text(errors="ignore")
    if not FINAL.exists():
        print("FAIL: no final-state JSON produced")
        return 1
    final = json.load(open(FINAL))

    # ── geometry check: the arena really loaded the way the generator said ──
    tgt_final = pill_at_index(final, TARGET_IDX)
    snaps = []
    if SNAP.exists():
        for line in SNAP.read_text(errors="ignore").splitlines():
            line = line.strip()
            if line:
                try:
                    snaps.append(json.loads(line))
                except ValueError:
                    pass
    first = snaps[0] if snaps else final
    tgt_first = pill_at_index(first, TARGET_IDX)
    if tgt_first is None or pill_at_index(first, OUR_IDXS[-1]) is None:
        print(f"FAIL: expected {1 + len(RING)} pillboxes in the state JSON, "
              f"got {len(first.get('pillboxes') or [])}")
        return 1
    print(f"  target pill:  ({tgt_first.get('tx')},{tgt_first.get('ty')}) "
          f"owner={tgt_first.get('owner')} armour={tgt_first.get('armor')}"
          f"  (neutral = hostile to us)")

    def standing(rec):
        """How many of OUR ring pills are still deployed, alive and on their
        original tile — i.e. still blocking shot lines."""
        n = 0
        for i, want in zip(OUR_IDXS, RING):
            p = pill_at_index(rec, i)
            if (p and not p.get("in_tank") and (p.get("armor") or 0) > 0
                    and (p.get("tx"), p.get("ty")) == want
                    and p.get("owner") == OUR_PLAYER):
                n += 1
        return n

    print(f"  our ring:     {standing(first)}/{len(RING)} standing at tick "
          f"{first.get('tick')}, {standing(final)}/{len(RING)} at the end")
    if standing(first) < len(RING):
        print(f"  WARNING: the ring was already incomplete at the first "
              f"snapshot — check ownership and the scenario sidecar")
    # The ring only has to hold long enough for the take to be planned and
    # driven; crossfire (our shells at the target, and its return fire) eats
    # it afterwards.  Report when it thinned so a null result is diagnosable.
    for s in snaps:
        if standing(s) < len(RING):
            print(f"  NOTE: ring first thinned at tick {s.get('tick')} "
                  f"({standing(s)}/{len(RING)} left) — blocked lines only "
                  f"exist while pills are standing")
            break

    # ── 1. target dead or ours ──
    def taken(p):
        return p is not None and ((p.get("armor") or 0) == 0
                                  or p.get("owner") == OUR_PLAYER)
    taken_at = None
    for s in snaps:
        if taken(pill_at_index(s, TARGET_IDX)):
            taken_at = s.get("tick")
            break
    target_taken = taken_at is not None or taken(tgt_final)

    # ── 2. no "shot path blocked" abort ──
    blocked_lines = matching_lines(text, BLOCKED_TAGS)
    if lua_log and STDERR.exists():
        blocked_lines += matching_lines(STDERR.read_text(errors="ignore"),
                                        ("shot path blocked",))

    # ── 3. no abandon ──
    abandon_lines = matching_lines(text, ABANDON_TAGS)

    # ── the fixed brain's blocked-line ladder (report; PASS gate only under
    #    --require-ladder) ──
    reaim_lines = [ln for ln in text.splitlines() if REAIM_RE.search(ln)]
    reaim_tags = sorted({m for ln in reaim_lines for m in REAIM_RE.findall(ln)})
    ladder_lines = matching_lines(text, LADDER_TAGS)
    ladder_counts = {t: sum(1 for ln in ladder_lines if t in ln)
                     for t in LADDER_TAGS}
    ladder_ran = bool(reaim_lines) or bool(ladder_lines)
    replan_lines = matching_lines(text, REPLAN_TAGS)
    replan_counts = {t: sum(1 for ln in replan_lines if t in ln)
                     for t in REPLAN_TAGS}
    jsonl_path, jsonl_recs = read_brain_jsonl(sess)
    pp_ticks = []
    if jsonl_recs:
        prev = None
        for r in jsonl_recs:
            cur = (r.get("goal"), r.get("gsub"))
            if prev is not None and cur != prev and cur[1] == "plan_position":
                pp_ticks.append(r.get("t"))
            prev = cur

    print(f"  session:      {sess}")
    print(f"  target taken: {target_taken}"
          + (f" (dead/ours by tick {taken_at})" if taken_at is not None else "")
          + (f"  final: owner={tgt_final.get('owner')} "
             f"armour={tgt_final.get('armor')} "
             f"in_tank={tgt_final.get('in_tank')}" if tgt_final else ""))
    print(f"  shot-path aborts:   {len(blocked_lines)}  "
          f"({'/'.join(BLOCKED_TAGS)})")
    print(f"  abandons:           {len(abandon_lines)}  "
          f"({'/'.join(ABANDON_TAGS)})")
    print(f"  re-aims observed:   {len(reaim_lines)}"
          + (f"  tags={reaim_tags}" if reaim_tags else "  (no *_REAIM tags)"))
    print(f"  blocked-spot ladder: " +
          "  ".join(f"{t}={n}" for t, n in ladder_counts.items()))
    print(f"  replan traffic:     " +
          "  ".join(f"{t}={n}" for t, n in replan_counts.items()))
    if jsonl_path:
        print(f"  brain jsonl:        {jsonl_path} "
              f"({len(jsonl_recs)} ticks, {len(pp_ticks)} plan_position entries)")
    else:
        print("  brain jsonl:        none written "
              "(player-0 trace is BrainTest-only) — using print2 + snapjson")

    # STRICT: no abandon at all.  An earlier revision of this test allowed a
    # transient abandon (one the take later recovered from), because the fixed
    # brain still burned all three SANITY_PILL_REPLANS_MAX tries here.  That
    # turned out to be a SECOND, separate bug: attack_shield.M.scan did not
    # honour state.banned_pill_angles, so it nudged the standoff straight back
    # into the bucket the take had just banned and two of the three "spots
    # tried" were the SAME spot (20260901_032030 bot0: banned 140..150 at
    # t=506, deg=146.25 adopted at t=510, banned again t=531).  With the shield
    # scan filtering banned angles the brain finds a genuinely different spot on
    # the first ban and never abandons, so the criterion is back to strict.
    ok = (target_taken and not blocked_lines and not abandon_lines
          and (ladder_ran or not require_ladder))
    if ok:
        print("PASS: bot took the pill without ever declaring the shot path "
              "blocked or abandoning the take."
              + (f"  (blocked-line ladder ran: {len(reaim_lines)} re-aim(s), "
                 f"{len(ladder_lines)} blocked-spot replan(s))"
                 if ladder_ran else ""))
        return 0

    print("FAIL")
    if not target_taken:
        print("  - the target pill was never killed or captured")
        if tgt_final:
            print(f"    final target: {json.dumps(tgt_final)}")
    if require_ladder and not ladder_ran:
        print("  - no re-aim / blocked-spot tag was ever emitted: the run "
              "never exercised a blocked shot line, so a green result would "
              "prove nothing (check the blocker is still standing above)")
    for ln in blocked_lines[:10]:
        print(f"  - shot-path abort: {ln}")
    if len(blocked_lines) > 10:
        print(f"    ... and {len(blocked_lines) - 10} more")
    for ln in abandon_lines[:10]:
        print(f"  - abandon: {ln}")
    if len(abandon_lines) > 10:
        print(f"    ... and {len(abandon_lines) - 10} more")
    for ln in ladder_lines[:6]:
        print(f"  - ladder: {ln}")
    for ln in replan_lines[:10]:
        print(f"  - replan: {ln}")
    if len(replan_lines) > 10:
        print(f"    ... and {len(replan_lines) - 10} more")
    return 1


def main():
    ticks = 5000
    build = DEFAULT_BUILD
    lua_log = False
    require_ladder = False
    simple = False
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
    print(pacing_line(""))
    i = 0
    while i < len(args):
        if args[i] == "--ticks":
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build":
            build = Path(args[i + 1]); i += 2
        elif args[i] == "--lua-log":
            lua_log = True; i += 1
        elif args[i] == "--require-ladder":
            require_ladder = True; i += 1
        elif args[i] == "--simple":
            simple = True; i += 1
        else:
            i += 1
    try:
        sys.exit(run(ticks, build, lua_log, require_ladder, simple))
    except subprocess.TimeoutExpired:
        print("FAIL: run timed out")
        sys.exit(1)


if __name__ == "__main__":
    main()
