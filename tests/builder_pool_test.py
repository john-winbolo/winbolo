#!/usr/bin/env python3
"""builder pool — LGM side-quests as a parallel track (GoalHunter 1.7).

Field incident 20260901_160325_1_par2 bot2, t=21071-21661: bot2 places its own
blocker p15 at (125,114) for a take on pill #10; p15 dies to return fire at
21471; for the next 190 ticks the LGM sits IN THE TANK with 13 trees, six tiles
from the corpse, while the tank (correctly) finishes the take.  At 21661 a 1.6
bot drives over p15 and owns it.  Four trees would have made it a live friendly
pill -- undriveable, race over.  The tank goal was right; the architecture lost
the pill, because builder dispatch was slaved to the tank's goal.

builder_pool.lua is the second arbiter: the goal pool spends the TANK, this one
spends the MAN.  Five things have to be true for it to be worth having, and
each variant here checks one of them.

  A  FIRE-EXCHANGE GATING.  The tank works a hostile pill while our own worn
     pill sits five tiles off the line.  Nothing may be dispatched on any tick
     whose eligibility verdict reads `fire_exchange:<substate>` -- shells
     detonating on the tank splash the man at departure and at return, which
     are the two ends of the errand -- and the ticks that were held back have
     to have had a real candidate on them, or the assertion proves only that
     the pool was empty.  The dispatch that DOES happen must reproduce from its
     own chips and the pill must come back up.

     NOTE: in this arena the repair lands BEFORE the take begins, because
     repair_pill (30 flat, with the damage bonus cancelling the path) outbids
     attack_pill from the spawn, so the dispatch here is a seeded one.  The
     "hold, then go in the next travel window" sequence is variant D's job.

  B  THE repair_pill SPLIT.  One badly worn pill 18 tiles away -- outside the
     leash, so repair_pill as a TANK goal means "relocate until the repair
     becomes leash-reachable" and nothing else.  The tank must drive, the
     pool-5 row must flip to `REJECT builder_can (leash 8, eta N)` the moment
     it is in range, and the MAN must finish it.  One executor, two feeders.

  B2 UNDER-FIRE CLOCK.  Bad ground (the take_cover pill strip) plus a scripted
     shooter putting shells on OUR TANK, with a worn pill inside the leash.
     The dispatch must be denied `under_fire(<age>t)` while they are landing.
     This is danger.tank_fire_age (armour dropped / a hostile shell will
     connect with us), NOT the pill's last_hit_tick that the repair hold uses
     -- two different interlocks, two different constants.

  C  ALLY CLAIMS.  Two GoalHunter 1.7 bots, one worn pill between them, both
     able to reach it.  Exactly one dispatch across the pair; the loser's row
     must say ally_repairing.

  D  RESERVATION.  A far take with walls planned, and our worn pill near the
     standoff.  b.reserve_eta must be DECLARED for that take, must be the real
     tank-travel ETA (it has to move as the tank drives, not sit on a constant),
     and no side-quest may ever be launched into a reservation it does not fit.

     NOTE: this arena does not FORCE the deferral, and the check says so where
     it does not happen.  In a one-bot arena a damaged friendly pill inside the
     leash is never left alone long enough for a take to be running over it:
     repair_pill prices at REPAIR_BASE_COST(30) with the damage bonus
     cancelling the drive, defend_pill's ARRIVED rung at 40, and one of them
     outbids attack_pill every time -- so the tank goes and fixes it through
     the feeder, which is exempt from the reservation by construction.

     A 40k-tick DH-Oil Rig run produced no deferral either, for a structural
     reason worth writing down: the reservation is tested AFTER the mode gate,
     and every goal that declares a reserve_eta is a goal whose builder mode
     already owns the man -- so mode_owned answers first and the reservation
     never gets a turn.  It is the backstop for the narrow case where a
     travel-class substate lets a side-quest through while walls are still
     due.  The invariant (nothing ever launches into a reservation it does not
     fit) is what is asserted here, and it is cheap to keep true.

Every assertion is read from print2, from the -finaljson, or from the sidecar's
own engine-side HP trace -- never from the brain's opinion of itself where the
engine can be asked instead.

Usage: python builder_pool_test.py [--variant A|B|B2|C|D|all] [--ticks N]
                                   [--build DIR]
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

sys.path.insert(0, str(HERE))
import generate_builder_pool_map as G   # noqa: E402

# constants.lua
LEASH = 8
UNDER_FIRE_TICKS = 100
RESERVE_MARGIN = 40

PORTS = {"A": 50071, "B": 50072, "B2": 50073, "C": 50074, "D": 50075}
TICKS = {"A": 9000, "B": 4000, "B2": 4000, "C": 5000, "D": 12000}

# builder_pool.lua
# BUILDER_POOL t=11 owner=explore/infrastructure elig=yes cands=0 ok=0 trees=40/res=4 ...
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) "
    r"trees=(\d+)/res=(\d+).*?reserve_eta=(\S+) under_fire=(\S+) job=(\S+)")
# BP_DISPATCH t=453 job=topup target=(126,126) [seeded_by=x ]score=... eta=36 trip=92 ...
DISP_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)(.*?) score=(-?[\d.]+) "
    r"\(val (-?[\d.]+) - trip (-?[\d.]+) - danger (-?[\d.]+)\) "
    r"eta=(\S+) trip=(\S+) trees=(\d+)-(\d+) front=(-?\d+)")
# BP_DENY t=267 job=topup target=(126,126) reason=discovery:friendly_fire score=93 trip=178 ...
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(.*?) elig=(.*?) "
    r"score=(-?[\d.]+) trip=(\S+) trees=(\d+)/(\d+) res=(\d+)")
DONE_RE = re.compile(
    r"BP_DONE t=(\d+) job=(\S+) target=\((\d+),(\d+)\) outcome=(\S+) took=(\d+)t")
ABORT_RE = re.compile(
    r"BP_ABORT t=(\d+) job=(\S+) target=\((\d+),(\d+)\) why=(\S+) took=(\d+)t")
# init.lua TICK_COST ... goal=attack_pill sub=shoot_pill
TICK_RE = re.compile(r"TICK_COST t=(\d+) .*? goal=(\S+) sub=(\S+)")
# goals.lua REPAIR_SPLIT t=812 pill#3@(132,126) hp=3 REJECT builder_can (leash 8, eta 96) -- ...
SPLIT_RE = re.compile(
    r"REPAIR_SPLIT t=(\d+) pill#(\S+)@\((\d+),(\d+)\) hp=(\d+) "
    r"REJECT (builder_can \(leash \d+, eta \d+\))")


def find_ds(build_dir):
    for c in (build_dir / "WinBoloDS.exe", build_dir / "WinBoloDS",
              build_dir / "Release" / "WinBoloDS.exe"):
        if c.exists():
            return c
    return None


def newest_session(build_dir, label):
    dirs = glob.glob(str(build_dir / "debug_sessions" / f"*{label}*"))
    return Path(max(dirs, key=os.path.getmtime)) if dirs else None


def read_hp(build_dir, variant):
    path = build_dir / f"builder_pool_{variant}_hp.log"
    seq = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            a, b = line.split()[:2]
            seq.append((int(a), int(b)))
    return seq


def run_sim(variant, ticks, build_dir):
    """Runs the arena and returns (session_dir, [bot log texts]) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable, str(HERE / "generate_builder_pool_map.py"),
                    variant], check=True, stdout=subprocess.DEVNULL)

    label = f"builder_pool_{variant}"
    final = HERE / f"builder_pool_{variant}_final.json"
    stderr = HERE / f"builder_pool_{variant}_stderr.txt"
    hp = build_dir / f"builder_pool_{variant}_hp.log"
    for p in (final, stderr, hp):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"builder_pool_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby", "-gametype", "open",
           "-bots", "1", "-brain", str(BRAIN),
           # yesfull: these arenas are tiny and the experiment is about WHEN the
           # man is spent, not about discovering the map.
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(600, ticks // 5))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000])
    # Sidecar-spawned bots land in whatever slot the server had free, which is
    # routinely 15 rather than 1 -- glob rather than counting from zero.
    logs = []
    for p in sorted(sess.glob("print2_bot*.log"),
                    key=lambda q: int(re.search(r"(\d+)", q.name).group(1))):
        logs.append(p.read_text(errors="ignore"))
    if not logs:
        return None, f"no print2_bot*.log under {sess}"
    return sess, logs


def lua_errors(text):
    bad = [ln for ln in text.splitlines()
           if "attempt to " in ln or "stack traceback" in ln
           or ".lua:" in ln and "Error" in ln]
    return bad[:5]


# ── variant A ─────────────────────────────────────────────────────────────
def check_A(sess, logs, build_dir):
    text = logs[0]
    pool = POOL_RE.findall(text)
    disp = DISP_RE.findall(text)
    deny = DENY_RE.findall(text)
    done = DONE_RE.findall(text)
    ticks = TICK_RE.findall(text)
    target = G.A_OUR_PILL

    print(f"  BUILDER_POOL verdicts: {len(pool)}; dispatches: {len(disp)}; "
          f"denials: {len(deny)}; completions: {len(done)}")

    # 0. the arena worked at all: the bot really took on the hostile pill and
    #    really got into a shooting exchange over it.
    fire_verdicts = [(int(t), e) for (t, _o, e, _c, _ok, _tr, _rs, _re, _uf, _j)
                     in pool if e.startswith("no: fire_exchange:")]
    subs = sorted({s for (_t, g, s) in ticks if g == "attack_pill"})
    print(f"  attack_pill substates seen: {', '.join(subs) or '(none)'}")
    if not fire_verdicts:
        print("FAIL (0): the bot never reached a fire-exchange substate with a "
              "live builder pool, so the gating half of this test never ran. "
              "Check that the idler owns the pill at "
              f"{G.A_FOE_PILL} (grep the stderr for BUILDER_POOL_A).")
        return 1
    print(f"  0 OK: {len(fire_verdicts)} tick(s) with a fire-exchange verdict, "
          f"e.g. t={fire_verdicts[0][0]} {fire_verdicts[0][1]}")

    # 1. nothing was dispatched on any of those ticks.
    fire_set = {t for (t, _e) in fire_verdicts}
    bad = [d for d in disp if int(d[0]) in fire_set]
    if bad:
        print(f"FAIL (1): {len(bad)} dispatch(es) during a fire exchange, e.g. "
              f"t={bad[0][0]} job={bad[0][1]} target=({bad[0][2]},{bad[0][3]}). "
              "The man left the tank while shells were flying.")
        return 1
    print(f"  1 OK: no dispatch on any of the {len(fire_set)} fire-exchange ticks")

    # 2. ...and it was the SUBSTATE CLASS that stopped them, with a real
    #    candidate on the table -- otherwise assertion 1 proves only that the
    #    pool was empty. A denial line's `elig=` field carries the pool-wide
    #    verdict even when the row also had a local problem of its own.
    fe_deny = [d for d in deny if d[5].startswith("fire_exchange:")]
    cands_during_fire = [(int(t), int(c)) for (t, _o, e, c, _ok, _tr, _rs, _re,
                                               _uf, _j) in pool
                         if e.startswith("no: fire_exchange:") and int(c) > 0]
    if not cands_during_fire:
        print("FAIL (2): every fire-exchange tick had ZERO candidates, so "
              "assertion 1 proves nothing -- there was nothing to hold back. "
              "The worn pill has to be inside the leash while the take runs.")
        return 1
    print(f"  2 OK: {len(cands_during_fire)} fire-exchange tick(s) had a live "
          f"candidate and none of them went (e.g. t={cands_during_fire[0][0]}, "
          f"{cands_during_fire[0][1]} candidate(s))"
          + (f"; {len(fe_deny)} BP_DENY line(s) name it, first t={fe_deny[0][0]} "
             f"reason={fe_deny[0][4]} elig={fe_deny[0][5]}" if fe_deny else ""))

    # 3. it DID go in a travel-class window, on the pill we planted.
    ours = [d for d in disp if (int(d[2]), int(d[3])) == target]
    if not ours:
        print(f"FAIL (3): the man was never dispatched to our worn pill at "
              f"{target}. Dispatches seen: "
              + (", ".join(f"{d[1]}@({d[2]},{d[3]})" for d in disp) or "none"))
        tail = [ln for ln in text.splitlines() if "BP_DENY" in ln][-6:]
        for ln in tail:
            print("   " + ln.strip())
        return 1
    d0 = ours[0]
    verdict = {int(t): e for (t, _o, e, _c, _ok, _tr, _rs, _re, _uf, _j) in pool}
    v0 = verdict.get(int(d0[0]), "?")
    seeded = "seeded_by" in d0[4]
    # A seeded job legitimately fires on a tick the stack said no to: it is the
    # tank's own repair goal handing its work to the one executor, and that
    # goal IS the thing that owns the man (see builder_pool.seed_job). Any
    # OTHER dispatch has to have been allowed outright.
    if not seeded and not v0.startswith("yes"):
        print(f"FAIL (3): the dispatch at t={d0[0]} happened on a tick whose "
              f"eligibility verdict was '{v0}', and it was not seeded by a "
              "feeder -- an ordinary side-quest may only fire when the stack "
              "says yes.")
        return 1
    # ...and the row's arithmetic closes: score == val - trip - danger.
    score, val, trip_c, dgr_c = (float(d0[5]), float(d0[6]),
                                 float(d0[7]), float(d0[8]))
    if abs(score - (val - trip_c - dgr_c)) > 0.51:
        print(f"FAIL (3): the dispatch row does not add up -- score={score} but "
              f"{val} - {trip_c} - {dgr_c} = {val - trip_c - dgr_c:.1f}")
        return 1
    print(f"  3 OK: dispatched at t={d0[0]} job={d0[1]} target=({d0[2]},{d0[3]}) "
          f"eta={d0[9]} trip={d0[10]} front={d0[13]}; "
          f"score {score:.0f} = val {val:.0f} - trip {trip_c:.0f} - danger {dgr_c:.0f}; "
          f"verdict '{v0}'"
          + (" (seeded by a feeder)" if seeded else ""))

    # 4. the engine agrees the pill came back up.
    hp = read_hp(build_dir, "A")
    rises = [(t, v) for i, (t, v) in enumerate(hp) if i > 0 and v > hp[i - 1][1]]
    if not rises:
        print("FAIL (4): our pill's armour never went back up. Either the man "
              "died on the way or the repair never completed. HP trace: "
              + ", ".join(f"{t}:{v}" for t, v in hp[:16]))
        for ln in [l for l in text.splitlines() if "BP_ABORT" in l][:4]:
            print("   " + ln.strip())
        return 1
    print(f"  4 OK: pill armour rose {len(rises)} time(s), first at sim "
          f"t={rises[0][0]} to {rises[0][1]}/{G.PILLS_MAX_HEALTH}"
          + (f"; BP_DONE outcome={done[0][4]} took={done[0][5]}t" if done else ""))

    print("PASS (A): the pool held the man through every fire-exchange tick "
          "that had somewhere to send him, and the errand it did run added up "
          "and landed.")
    return 0


# ── variant B ─────────────────────────────────────────────────────────────
def check_B(sess, logs, build_dir):
    text = logs[0]
    pool = POOL_RE.findall(text)
    disp = DISP_RE.findall(text)
    done = DONE_RE.findall(text)
    ticks = TICK_RE.findall(text)
    split = SPLIT_RE.findall(text)
    target = G.B_OUR_PILL

    kinds = sorted({g for (_t, g, _s) in ticks})
    print(f"  goals seen: {', '.join(kinds) or '(none)'}; verdicts: {len(pool)}; "
          f"dispatches: {len(disp)}; REPAIR_SPLIT lines: {len(split)}")

    # 0. the tank did the half of the split that is its job: relocate.
    relocate = [int(t) for (t, g, _s) in ticks if g == "repair_pill"]
    if not relocate:
        print(f"FAIL (0): repair_pill never won the tank pool, so the tank "
              f"never relocated toward the pill at {target} and the split had "
              "nothing to hand over. Goals seen: " + ", ".join(kinds))
        return 1
    print(f"  0 OK: repair_pill held the tank for {len(relocate)} tick(s) "
          f"(t={relocate[0]}..{relocate[-1]}) -- the relocate half of the split")

    # 1. ...and stood down the moment the man could walk it.
    ours_split = [s for s in split if (int(s[2]), int(s[3])) == target]
    if not ours_split:
        print(f"FAIL (1): the pool-5 row never flipped to REJECT builder_can "
              f"for {target}, so the tank goal never handed off -- it would "
              "have driven all the way onto the pill itself.")
        return 1
    s0 = ours_split[0]
    if int(s0[0]) < relocate[0]:
        print(f"FAIL (1): the split fired at t={s0[0]}, before the relocate "
              f"even started at t={relocate[0]} -- that is not a hand-off.")
        return 1
    print(f"  1 OK: split at t={s0[0]} hp={s0[4]} REJECT {s0[5]}")

    # 2. the man finished it.
    ours = [d for d in disp if (int(d[2]), int(d[3])) == target]
    if not ours:
        print(f"FAIL (2): the man was never sent to {target}, so the hand-off "
              "went to nobody.")
        for ln in [l for l in text.splitlines() if "BP_DENY" in l][-8:]:
            print("   " + ln.strip())
        return 1
    d0 = ours[0]
    verdict = {int(t): (o, e) for (t, o, e, _c, _ok, _tr, _rs, _re, _uf, _j) in pool}
    owner, v = verdict.get(int(d0[0]), ("?", "?"))
    seeded = "seeded_by" in d0[4]
    print(f"  2 OK: dispatched at t={d0[0]} job={d0[1]} target=({d0[2]},{d0[3]}) "
          f"owner={owner} verdict='{v}' trees={d0[11]}-{d0[12]} "
          f"({'seeded by the feeder' if seeded else 'as an ordinary side-quest'})")

    hp = read_hp(build_dir, "B")
    rises = [(t, val) for i, (t, val) in enumerate(hp) if i > 0 and val > hp[i - 1][1]]
    if not rises:
        print("FAIL (3): the pill's armour never rose. HP trace: "
              + ", ".join(f"{t}:{v}" for t, v in hp[:16]))
        return 1
    print(f"  3 OK: pill armour rose {len(rises)} time(s), first at sim "
          f"t={rises[0][0]} to {rises[0][1]}/{G.PILLS_MAX_HEALTH}"
          + (f"; BP_DONE outcome={done[0][4]}" if done else ""))
    print("PASS (B): the tank relocated until the repair was leash-reachable, "
          "the pool-5 row stood down with its reason, and the man finished it.")
    return 0


# ── variant B2 ────────────────────────────────────────────────────────────
def check_B2(sess, logs, build_dir):
    text = logs[0]
    pool = POOL_RE.findall(text)
    disp = DISP_RE.findall(text)
    deny = DENY_RE.findall(text)

    # under_fire= is "never" or "<n>t"
    hot = [(int(t), uf) for (t, _o, _e, _c, _ok, _tr, _rs, _re, uf, _j) in pool
           if uf != "never" and int(uf.rstrip("t")) < UNDER_FIRE_TICKS]
    print(f"  verdicts: {len(pool)}; ticks with a fresh fire stamp: {len(hot)}; "
          f"dispatches: {len(disp)}; denials: {len(deny)}")
    if not hot:
        print("FAIL (0): the tank was never recorded as under fire, so the "
              "clock this variant tests never started. Did the scripted "
              "shooter spawn and get in range? (grep the stderr for "
              "shell_tank_then_flee / BUILDER_POOL_B2)")
        shots = [ln for ln in text.splitlines() if "under_fire=" in ln][:3]
        for ln in shots:
            print("   " + ln.strip())
        return 1
    print(f"  0 OK: {len(hot)} tick(s) inside BUILDER_POOL_UNDER_FIRE_TICKS "
          f"({UNDER_FIRE_TICKS}), first t={hot[0][0]} age={hot[0][1]}")

    # The clock is checked AFTER the mode gate, so it can only ever BE the
    # named reason on a tick whose owner is idle-ish. What has to be true is
    # that such ticks exist, that the pool had somewhere to send the man on
    # them, and that it did not.
    uf_verdicts = [(int(t), e, int(c)) for (t, _o, e, c, _ok, _tr, _rs, _re,
                                            _uf, _j) in pool
                   if e.startswith("no: under_fire(")]
    uf_live = [v for v in uf_verdicts if v[2] > 0]
    if not uf_live:
        print("FAIL (1): no tick was ever denied on under_fire WITH a candidate "
              "on the table, so the clock never actually held anything back. "
              f"({len(uf_verdicts)} under_fire verdict(s), all with 0 "
              "candidates.) Either the shooter never landed anything while the "
              "worn pill was in the leash, or the mode gate got there first on "
              "every shelled tick -- check that the moat really does make the "
              "shooter unreachable, so attack_tank cannot bid.")
        for d in deny[:8]:
            print(f"   t={d[0]} {d[1]}@({d[2]},{d[3]}) reason={d[4]} elig={d[5]}")
        return 1
    uf_deny = [d for d in deny if d[4].startswith("under_fire(")
               or d[5].startswith("under_fire(")]
    print(f"  1 OK: {len(uf_live)} tick(s) denied on under_fire with a live "
          f"candidate, first t={uf_live[0][0]} '{uf_live[0][1]}' "
          f"({uf_live[0][2]} candidate(s))"
          + (f"; {len(uf_deny)} BP_DENY line(s) name it, first t={uf_deny[0][0]}"
             if uf_deny else ""))

    hot_set = {t for (t, _a) in hot}
    bad = [d for d in disp if int(d[0]) in hot_set]
    if bad:
        print(f"FAIL (2): {len(bad)} dispatch(es) while the tank was under "
              f"fire, e.g. t={bad[0][0]} job={bad[0][1]}")
        return 1
    print(f"  2 OK: no dispatch on any of the {len(hot_set)} under-fire ticks")
    print("PASS (B2): shells on the tank kept the man aboard -- the sustained "
          "clock, not a single-tick danger reading, is what held him.")
    return 0


# ── variant C ─────────────────────────────────────────────────────────────
def check_C(sess, logs, build_dir):
    if len(logs) < 2:
        print(f"FAIL (0): only {len(logs)} bot log(s) under {sess}; this "
              "variant needs two GoalHunter 1.7 bots. Check the sidecar's "
              "spawn_bot (grep the stderr for BUILDER_POOL_C).")
        return 1
    target = G.C_OUR_PILL
    per_bot = []
    for i, text in enumerate(logs):
        d = [x for x in DISP_RE.findall(text) if (int(x[2]), int(x[3])) == target]
        dn = DENY_RE.findall(text)
        ally = [x for x in dn if x[4].startswith("ally_repairing")]
        per_bot.append((d, dn, ally))
        print(f"  bot{i}: dispatches to {target}: {len(d)}; denials: {len(dn)}; "
              f"ally_repairing: {len(ally)}")

    total = sum(len(d) for (d, _dn, _a) in per_bot)
    if total == 0:
        print(f"FAIL (1): neither bot ever dispatched to the pill at {target}.")
        for i, text in enumerate(logs):
            for ln in [l for l in text.splitlines() if "BP_DENY" in l][:5]:
                print(f"   bot{i}: " + ln.strip())
        return 1
    if total > 1:
        # Two dispatches are only a failure if BOTH were live at once. A second
        # trip after the first completed is ordinary work, not a duplicate.
        windows = []
        for i, text in enumerate(logs):
            for x in per_bot[i][0]:
                windows.append((int(x[0]), i))
        windows.sort()
        overlap = [(a, b) for a, b in zip(windows, windows[1:])
                   if a[1] != b[1] and (b[0] - a[0]) < 200]
        if overlap:
            print(f"FAIL (1): both bots had the man out for {target} within "
                  f"200 ticks of each other ({overlap[0]}). The claim did not "
                  "de-conflict them.")
            return 1
        print(f"  1 OK: {total} dispatch(es) total, never two bots at once "
              f"({[w for w in windows]})")
    else:
        print(f"  1 OK: exactly one dispatch across both bots")

    if not any(len(a) for (_d, _dn, a) in per_bot):
        print("FAIL (2): no bot ever logged ally_repairing, so the loser was "
              "stopped by something other than the claim. Denial reasons seen:")
        for i, (_d, dn, _a) in enumerate(per_bot):
            print(f"   bot{i}: " + ", ".join(sorted({x[4] for x in dn}))[:200])
        return 1
    for i, (_d, _dn, a) in enumerate(per_bot):
        if a:
            print(f"  2 OK: bot{i} stood down with reason={a[0][4]} at t={a[0][0]}")
    print("PASS (C): one pill, two willing bots, one man spent -- the bpj claim "
          "picked a winner deterministically and the loser said why.")
    return 0


# ── variant D ─────────────────────────────────────────────────────────────
def check_D(sess, logs, build_dir):
    text = logs[0]
    pool = POOL_RE.findall(text)
    disp = DISP_RE.findall(text)
    deny = DENY_RE.findall(text)
    done = DONE_RE.findall(text)
    target = G.D_OUR_PILL

    # reserve_eta prints as "<n>(<why>)" or "-(-)": the value and the reason
    # set_mode had for declaring it, so the number is attributable.
    res = []
    for (t, _o, _e, c, _ok, _tr, _rs, re_, _uf, _j) in pool:
        m = re.match(r"(-|\d+)\((\S+)\)$", re_)
        if m and m.group(1) != "-":
            res.append((int(t), int(m.group(1)), m.group(2), int(c)))
    print(f"  verdicts: {len(pool)} ({len(res)} with a live reserve_eta); "
          f"dispatches: {len(disp)}; denials: {len(deny)}")

    # 0. set_mode declares the reservation, and says which goal wants the man.
    if not res:
        print("FAIL (0): b.reserve_eta was never set, so there was no "
              "reservation at all. set_mode sets it for a take with walls "
              "planned, a live sea plan, a placement, a take-blocker drop and "
              "the two repair feeders -- none of those ever ran.")
        return 1
    whys = sorted({w for (_t, _v, w, _c) in res})
    walls = [r for r in res if r[2] == "walls_at_standoff"]
    print(f"  0 OK: reserve_eta live on {len(res)} tick(s); declared by: "
          f"{', '.join(whys)}")
    if not walls:
        print("FAIL (0): none of them came from walls_at_standoff, so the "
              "attack_pill-with-walls case -- the one the plan's worked "
              "example is about -- never ran in this arena.")
        return 1
    # It is the real tank-travel estimate, not a constant: it has to move as
    # the tank drives. Measured over EVERY live reservation, not just the
    # walls ones -- a wall shield is only planned in the last stretch of an
    # approach, so its own window can legitimately be a couple of ticks wide.
    # (cpf.estimate_tank_travel_ticks, ~21 ticks per tile of drive.)
    vals = sorted({v for (_t, v, _w, _c) in res})
    wvals = sorted({v for (_t, v, _w, _c) in walls})
    if len(vals) < 3:
        print(f"FAIL (0): reserve_eta only ever took {len(vals)} distinct "
              f"value(s) ({vals}) -- that is a constant, not a travel estimate.")
        return 1
    print(f"  0b OK: reserve_eta ranged {min(vals)}..{max(vals)} over "
          f"{len(vals)} distinct values -- it tracks the drive, as a "
          f"tank-travel ETA must (walls_at_standoff on {len(walls)} tick(s), "
          f"values {wvals})")

    # 1. THE INVARIANT. Whatever the arena happens to throw up, a side-quest
    #    must never have been launched into a live reservation it did not fit.
    #    A SEEDED job is exempt by construction (the reserving goal IS the one
    #    asking), and those carry seeded_by= on the dispatch line.
    res_at = {t: (v, w) for (t, v, w, _c) in res}
    violations = []
    for d in disp:
        if "seeded_by" in d[4]:
            continue
        rv = res_at.get(int(d[0]))
        trip = d[10]
        if rv and trip != "-" and int(trip) + RESERVE_MARGIN > rv[0]:
            violations.append((d[0], trip, rv))
    if violations:
        v = violations[0]
        print(f"FAIL (1): a side-quest launched into a reservation it did not "
              f"fit -- t={v[0]} trip={v[1]} + margin {RESERVE_MARGIN} > "
              f"reserve_eta {v[2][0]} ({v[2][1]})")
        return 1
    print(f"  1 OK: none of the {len(disp)} dispatch(es) violated a live "
          f"reservation")

    # 2. ...and where a deferral DID happen, its numbers close.
    res_deny = [d for d in deny if d[4].startswith("reserve(")]
    if res_deny:
        m = re.match(r"reserve\((\d+) < trip (\d+)\)", res_deny[0][4])
        if not m:
            print(f"FAIL (2): reserve denial is not in the documented shape: "
                  f"{res_deny[0][4]}")
            return 1
        eta, need = int(m.group(1)), int(m.group(2))
        trip = res_deny[0][7]
        if trip != "-" and need != int(trip) + RESERVE_MARGIN:
            print(f"FAIL (2): the denial does not reproduce from its own chips "
                  f"-- trip={trip} + margin {RESERVE_MARGIN} = "
                  f"{int(trip) + RESERVE_MARGIN}, but the row says trip {need}")
            return 1
        if need <= eta:
            print(f"FAIL (2): the row deferred even though the trip fitted "
                  f"({need} <= {eta})")
            return 1
        print(f"  2 OK: {len(res_deny)} deferral(s), first t={res_deny[0][0]} "
              f"reserve_eta={eta} < trip {need} (= walk {trip} + margin "
              f"{RESERVE_MARGIN})")
    else:
        # Not a failure, and worth saying why. In a ONE-BOT arena a damaged
        # friendly pill inside the leash is never left alone long enough for a
        # take to be running over it: repair_pill prices at REPAIR_BASE_COST
        # (30) with the damage bonus cancelling the drive, and defend_pill's
        # ARRIVED rung at 40, so one of them outbids attack_pill and the tank
        # simply goes and fixes it -- through the feeder, which is exempt.
        # A 40k-tick DH-Oil Rig run produced none either, and the reason is
        # structural rather than arena-specific: the reservation is tested
        # AFTER the mode gate, and every goal that declares a reserve_eta is a
        # goal whose builder mode already owns the man, so mode_owned answers
        # first. The reservation is the backstop for the narrow case where a
        # travel-class substate lets a side-quest through while walls are
        # still due. The invariant above is what holds either way.
        print("  2 -- no reserve(...) deferral occurred; the invariant in 1 is "
              "what is asserted here. The reservation sits BEHIND the mode "
              "gate, and the goals that declare one are the same goals whose "
              "builder mode already owns the man -- so mode_owned answers "
              "first and the reservation rarely gets a turn.")

    # 3. the arena still did its job end to end.
    ours = [d for d in disp if (int(d[2]), int(d[3])) == target]
    if not ours:
        print(f"FAIL (3): the man was never sent to {target} at all.")
        for ln in [l for l in text.splitlines() if "BP_DENY" in l][-6:]:
            print("   " + ln.strip())
        return 1
    d0 = ours[0]
    hp = read_hp(build_dir, "D")
    rises = [(t, v) for i, (t, v) in enumerate(hp) if i > 0 and v > hp[i - 1][1]]
    if not rises:
        print("FAIL (3): the pill's armour never rose. HP trace: "
              + ", ".join(f"{t}:{v}" for t, v in hp[:16]))
        return 1
    print(f"  3 OK: dispatched at t={d0[0]} trip={d0[10]}; pill armour rose at "
          f"sim t={rises[0][0]} to {rises[0][1]}/{G.PILLS_MAX_HEALTH}"
          + (f"; BP_DONE outcome={done[0][4]}" if done else ""))
    print("PASS (D): the reservation is declared from the real tank-travel ETA, "
          "tracks the drive, and was never violated.")
    return 0


CHECKS = {"A": check_A, "B": check_B, "B2": check_B2, "C": check_C, "D": check_D}


def run(variant, ticks, build_dir):
    print(f"=== variant {variant} ({ticks} ticks) ===")
    sess, logs = run_sim(variant, ticks, build_dir)
    if sess is None:
        print(f"FAIL: {logs}")
        return 1
    print(f"  session: {sess.name}")
    for i, text in enumerate(logs):
        errs = lua_errors(text)
        if errs:
            print(f"FAIL: Lua errors in bot{i}'s log:")
            for e in errs:
                print("   " + e.strip())
            return 1
    return CHECKS[variant](sess, logs, build_dir)


def main():
    variant, ticks, build = "all", None, DEFAULT_BUILD
    args = sys.argv[1:]
    take_asap_flag(args)      # consumes --asap / --no-asap
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
    variants = list(CHECKS) if variant == "all" else [variant]
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
