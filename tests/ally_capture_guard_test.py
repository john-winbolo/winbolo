#!/usr/bin/env python3
"""ally-capture guard — don't rebuild the corpse a teammate is coming to scoop
(GoalHunter 1.7).

THE GAP THIS CLOSES
-------------------
builder_pool.lua already refused to rebuild a dead pill that OUR OWN tank had
committed to (`our_target` in pill_blocked) and already honoured an ally's
BUILDER-JOB advert (`ally_repairing`, the bpj claim).  It never looked at the
third thing an ally can be doing with a corpse: DRIVING OVER TO PICK IT UP.
Four trees turn a corpse into a live friendly pill, which is undriveable — so
our LGM could walk out and make a teammate's pickup impossible, throwing away
both the trip and the kill that produced the corpse.  The tank-level repair
filter (goals.lua filter_repair_pill / pool 5) had the same blind spot.

Allies advertise that intent on the ordinary /info state slate:
`goal=capture_pill|pill_place` with `target=<pill id>`, or with `mx`/`my` when
the goal carries no object id.  builder_pool.ally_capture_on reads it.

THE AUTHOR'S RULE, in two halves that are deliberately different:
  MOVE-ON   the ally's LATEST slate names a different goal or a different pill
            -> the block ends THAT TICK, at any age.
  SILENCE   the ally stops talking altogether (killed, kicked, removed)
            -> the block ends BUILDER_POOL_ALLY_CAPTURE_TTL (175 brain ticks =
            7 s at 50 ticks/s) after its last message of any kind.

FOUR ARENAS, ONE PIECE OF GROUND (tests/generate_ally_capture_guard_map.py
writes four byte-identical .map files — the sidecar is found by map name, so
four sidecars need four names).  A grass DIAMOND of radius 8 around (126,126)
with one dead friendly pill at its centre and nothing else; our bot spawning at
the far east end of it with 40 trees and the LGM aboard.  The diamond, not a
square, because BUILDER_POOL_LEASH is measured in MANHATTAN distance — so every
land tile is inside the leash and "the man never went" can never mean "the
corpse fell out of the pool's reach".  Our own tank is kept off the corpse by
`cfg=CAPTURE_PILL_BASE_COST=1e30` (no map can stop a tank driving over a corpse
without also stopping the man) plus the distance; the generator's docstring has
the whole argument.

  A  THE BLOCK.  A real GoalHunter 1.7 ally six tiles north, with the corpse as
     the only thing on the map worth doing.  Assert its advert reaches us, our
     pool logs `ally_capturing`, our LGM is never dispatched to that tile, and
     the ENGINE says the ally scooped it (in_tank).  This is the `target=<id>`
     half of the matching rule.

  B  THE CONTROL.  Identical ground, one token different:
     `cfg=BUILDER_POOL_ALLY_CAPTURE_GUARD=false` on our bot.  The ally here is
     the scripted talker rather than a real 1.7, so the capture advert is live
     for the whole run and there is no footrace to muddy the result.  Assert our
     man IS dispatched and the engine-side armour rises off 0 — i.e. the arena
     can produce the bad behaviour, so A's silence means the guard and not some
     unrelated reason the pool was never going to fire.

  C  THE 7-SECOND EXPIRY.  The ally is tests/brains/advert_capture.lua, which
     only talks: `goal=capture_pill mx=126 my=126` once a second (the COORDINATE
     half of the matching rule).  The sidecar removes it mid-game, so the
     adverts simply stop — nothing ever tells our bot the slot is gone.  Assert
     the block survives and then ends with `silent=` at the TTL, and only then
     the rebuild lands.

  D  THE MOVE-ON.  The same talker, never removed: at think 600 it starts
     advertising `goal=explore` instead, on the same cadence.  Assert the block
     ends within a few ticks with `silent=` near ZERO — the ally is as fresh as
     it ever was, so only "the latest advert no longer names that pill" can have
     lifted it.

NOT COVERED HERE: goals.lua's copy of the guard (the pool-5 `ally_capturing`
INF row, logged as REPAIR_ALLY_CAPTURE).  It is a narrow backstop —
filter_repair_pill already drops a dead pill whenever OUR OWN capture_pill could
take it, so the pool-5 row only exists in the corner where capture rejects the
corpse for `blocked` or `stale` and an ally can still drive over.  These arenas
never reach that corner; the regex below is here so the line shows up in the run
output if one ever does.

Every outcome is read from the ENGINE (the sidecar's own pill trace) or from
print2 — never from the brain's opinion of itself where the engine can be asked.

Usage: python ally_capture_guard_test.py [--variant A|B|C|D|all] [--ticks N]
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

sys.path.insert(0, str(HERE))
import generate_ally_capture_guard_map as G   # noqa: E402

CORPSE = G.CORPSE
TTL = G.ALLY_CAPTURE_TTL          # C.BUILDER_POOL_ALLY_CAPTURE_TTL, brain ticks

PORTS = {"A": 50241, "B": 50242, "C": 50243, "D": 50244}
# ENGINE ticks.  The brain thinks once per 20 ms frame and the sim advances two
# engine ticks per frame, so these are ~half as many brain ticks.
TICKS = {"A": 3000, "B": 2000, "C": 2600, "D": 2400}

# Our bot's tokens, on every variant.
#   CAPTURE_PILL_BASE_COST  a corpse inside the leash is a ~20-cost capture_pill
#       for the TANK, so by default our own bot simply drives over and picks it
#       up — correct play, and it measures nothing about the builder pool (the
#       same trap generate_builder_pool_map.py documents at length).  1e30, not
#       merely large: the competition picks the CHEAPEST candidate, so a 999999
#       capture_pill still wins an otherwise empty pool.  Past 1e29 the goal is
#       "unaffordable" and drops out entirely, leaving the tank on explore.
#   PILL_REPOSITION_ENABLED  a bot with nothing to do bids reposition on any
#       pill it can see, shoots it down and starts moving it — which on this map
#       would mean shelling the very corpse under test.
#
# BRAIN_INIT_ARG IS 127 BYTES (BotInitSlot.arg in luabrainshandler.h). Past that
# the spec is TRUNCATED MID-TOKEN and the brain logs one "[cfg] BAD TOKEN" line
# and carries on with the default -- which cost this file a run. ARG_MAX below
# asserts it rather than leaving it to be rediscovered.
ARG_MAX = 127
OUR_CFG = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false"
GUARD_OFF = ";cfg=BUILDER_POOL_ALLY_CAPTURE_GUARD=false"

# ── print2 lines ──────────────────────────────────────────────────────────
# builder_pool.lua log_ally_capture, edge-triggered on (tile, ally).
BLOCK_RE = re.compile(
    r"BP_ALLY_CAPTURE t=(\d+) pill#(\S+)@\((\d+),(\d+)\) BLOCKED by p(\d+) "
    r"\(advert (\d+)t old, ttl (\d+)t\)")
RELEASE_RE = re.compile(
    r"BP_ALLY_CAPTURE t=(\d+) pill#(\S+)@\((\d+),(\d+)\) RELEASED \(was p(\S+)\) "
    r"silent=(-?\d+)t ttl=(\d+)t their_goal=(\S*) their_target=(\S*)")
# builder_pool.lua, unchanged shapes.
DISP_RE = re.compile(
    r"BP_DISPATCH t=(\d+) job=(\S+) target=\((\d+),(\d+)\)")
DENY_RE = re.compile(
    r"BP_DENY t=(\d+) job=(\S+) target=\((\d+),(\d+)\) reason=(.*?) elig=")
POOL_RE = re.compile(
    r"BUILDER_POOL t=(\d+) owner=(\S+) elig=(.*?) cands=(\d+) ok=(\d+) ")
# goals.lua, the pool-5 half of the same guard.
RSPLIT_RE = re.compile(
    r"REPAIR_ALLY_CAPTURE t=(\d+) pill#(\S+)@\((\d+),(\d+)\) hp=0 "
    r"REJECT (ally_capturing \(p\d+, \d+t\))")


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
    """The sidecar's engine-side view: [(tick, armour, in_tank, owner), ...]."""
    path = build_dir / f"ally_capture_guard_{variant}_trace.log"
    seq = []
    if path.exists():
        for line in path.read_text(errors="ignore").splitlines():
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            parts = line.split()
            if len(parts) >= 4:
                seq.append((int(parts[0]), int(parts[1]), int(parts[2]),
                            parts[3]))
    return seq


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, our_log_text) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_ally_capture_guard_map.py")],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"allycap_{variant}"
    final = HERE / f"ally_capture_guard_{variant}_final.json"
    stderr = HERE / f"ally_capture_guard_{variant}_stderr.txt"
    trace = build_dir / f"ally_capture_guard_{variant}_trace.log"
    for p in (final, stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    tokens = OUR_CFG + (GUARD_OFF if variant == "B" else "")
    if len(tokens) > ARG_MAX:
        return None, (f"the -bot-init token string is {len(tokens)} bytes, over "
                      f"the {ARG_MAX}-byte BRAIN_INIT_ARG limit -- it would be "
                      f"truncated mid-token and the last cfg= silently ignored:"
                      f"\n  {tokens}")
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"ally_capture_guard_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby", "-gametype", "open",
           "-bots", "1", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN}[{tokens}]",
           # The scripted ally in C/D lives under tests/, outside brains/.
           "-allow-unsafe-brains",
           # yesfull: a 17x17 pocket, and the experiment is about who is allowed
           # to spend the man, not about finding the pill.
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-finaljson", str(final),
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(600, ticks // 4))

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
    bad = [ln for ln in text.splitlines()
           if "attempt to " in ln or "stack traceback" in ln
           or (".lua:" in ln and "Error" in ln)]
    return bad[:5]


def arena_sane(text, variant):
    """Assertion 0 for every variant: the pool really did see the corpse as a
    rebuild candidate.  Without this, "the man never went" is worthless."""
    pool = POOL_RE.findall(text)
    with_cands = [(int(t), int(c)) for (t, _o, _e, c, _ok) in pool if int(c) > 0]
    if not pool:
        print("FAIL (0): no BUILDER_POOL verdict lines at all -- the pool never "
              "ran. Is BUILDER_POOL_ENABLED still true?")
        return False
    if not with_cands:
        print(f"FAIL (0): {len(pool)} pool verdicts and not one of them had a "
              "candidate, so nothing was ever held back. The corpse should be a "
              f"`rebuild` row from the first ticks: check the sidecar owned "
              f"pill 1 and left it at 0 armour (grep the stderr for "
              f"ALLY_CAPTURE_{variant}).")
        return False
    print(f"  0 OK: {len(pool)} pool verdicts, {len(with_cands)} with a live "
          f"candidate (first t={with_cands[0][0]})")
    return True


# ── variant A: the block ──────────────────────────────────────────────────
def check_A(sess, text, build_dir):
    if not arena_sane(text, "A"):
        return 1
    blocks = BLOCK_RE.findall(text)
    disp = DISP_RE.findall(text)
    deny = [d for d in DENY_RE.findall(text)
            if d[4].startswith("ally_capturing")]
    trace = read_trace(build_dir, "A")

    # 1. the ally's advert reached us and the pool named it.
    ours = [b for b in blocks if (int(b[2]), int(b[3])) == CORPSE]
    if not ours:
        print(f"FAIL (1): our pool never logged BP_ALLY_CAPTURE ... BLOCKED for "
              f"the corpse at {CORPSE}. Either the ally never advertised "
              "capture_pill on it, or the advert never reached us (the internal "
              "channel is ALLY-ONLY -- check the sidecar's set_team).")
        for ln in [l for l in text.splitlines() if "BP_DENY" in l][:6]:
            print("   " + ln.strip())
        return 1
    b0 = ours[0]
    print(f"  1 OK: blocked at t={b0[0]} by p{b0[4]} (advert {b0[5]}t old, "
          f"ttl {b0[6]}t)")
    if int(b0[6]) != TTL:
        print(f"FAIL (1): the brain says the ttl is {b0[6]} but this test was "
              f"written against {TTL}. Update TTL in "
              "generate_ally_capture_guard_map.py.")
        return 1

    # 2. ...and it is the reason the row lost, on the deny line the panel shows.
    if not deny:
        print("FAIL (2): no BP_DENY line named ally_capturing, so the row was "
              "stopped by something else (or never got as far as being the top "
              "row). Denial reasons seen: "
              + ", ".join(sorted({d[4] for d in DENY_RE.findall(text)}))[:300])
        return 1
    print(f"  2 OK: BP_DENY t={deny[0][0]} job={deny[0][1]} "
          f"reason={deny[0][4]}")

    # 3. the man never went. This is the whole point.
    went = [d for d in disp if (int(d[2]), int(d[3])) == CORPSE]
    if went:
        print(f"FAIL (3): the LGM was dispatched to the corpse anyway at "
              f"t={went[0][0]} job={went[0][1]} -- the guard did not hold.")
        return 1
    print(f"  3 OK: no BP_DISPATCH to {CORPSE} in "
          f"{len(disp)} dispatch(es) total")

    # 4. the ENGINE says the ally got it: in_tank, and the armour never rose.
    rose = [(t, a) for i, (t, a, _k, _o) in enumerate(trace)
            if i > 0 and a > trace[i - 1][1]]
    if rose:
        print(f"FAIL (4): the corpse's armour rose to {rose[0][1]} at sim "
              f"t={rose[0][0]} -- somebody rebuilt it. Trace: "
              + ", ".join(f"{t}:{a}/{k}" for t, a, k, _o in trace[:12]))
        return 1
    scooped = [(t, o) for (t, _a, k, o) in trace if k == 1]
    if not scooped:
        print("FAIL (4): nobody ever picked the corpse up, so the arena proved "
              "only that our man stayed home -- the ally has to actually get it "
              "or the block cost the team the pill. Engine trace: "
              + ", ".join(f"{t}:a{a}/t{k}/{o}" for t, a, k, o in trace[:12]))
        return 1
    print(f"  4 OK: engine says in_tank at sim t={scooped[0][0]} "
          f"(owner {scooped[0][1]}); armour never rose off 0")
    print("PASS (A): the ally advertised the scoop, our pool read it, the man "
          "stayed in the tank and the pill went to the teammate.")
    return 0


# ── variant B: the control ────────────────────────────────────────────────
def check_B(sess, text, build_dir):
    if not arena_sane(text, "B"):
        return 1
    blocks = BLOCK_RE.findall(text)
    disp = DISP_RE.findall(text)
    trace = read_trace(build_dir, "B")

    # 1. the guard really is off in this arena.
    if blocks:
        print(f"FAIL (1): BP_ALLY_CAPTURE fired at t={blocks[0][0]} even though "
              "this bot was given cfg=BUILDER_POOL_ALLY_CAPTURE_GUARD=false. "
              "The token did not land -- grep the log for '[cfg]'.")
        return 1
    cfg = [l for l in text.splitlines() if "BUILDER_POOL_ALLY_CAPTURE_GUARD" in l
           and "[cfg]" in l]
    if not cfg:
        print("FAIL (1): the bot never logged the cfg override, so this is not "
              "a control -- it may simply be running the default (guard ON) and "
              "passing the rest of the checks by accident.")
        return 1
    print(f"  1 OK: guard disabled for this bot -- {cfg[0].strip()[-90:]}")

    # 1b. ...and there really was a talker on the map to be ignored.  The
    #     sidecar writes its slot into the trace header, because a brain's own
    #     print() never reaches the headless server's stdout.
    #     That the talker's advert actually LANDS in our ally_state is proved by
    #     variant C's assertion 1, which runs the same brain from the same
    #     sidecar shape with the guard ON and gets blocked by it.
    hdr = [l for l in
           (build_dir / "ally_capture_guard_B_trace.log").read_text(
               errors="ignore").splitlines()
           if l.startswith("# ally slot=")]
    if not hdr:
        print("FAIL (1b): the scripted ally was never spawned, so there was no "
              "capture advert for the guard-off bot to ignore and this is not a "
              "control. Check the sidecar's spawn_bot (the newswire line reads "
              "ALLY_CAPTURE_B ...).")
        return 1
    print(f"  1b OK: {hdr[0].strip()}")

    # 2. with the guard off, the man goes.
    went = [d for d in disp if (int(d[2]), int(d[3])) == CORPSE]
    if not went:
        print(f"FAIL (2): the LGM was never dispatched to {CORPSE} even with "
              "the guard off, so variant A's silence proves nothing. Something "
              "else is stopping the row:")
        for ln in [l for l in text.splitlines() if "BP_DENY" in l][-6:]:
            print("   " + ln.strip())
        return 1
    print(f"  2 OK: BP_DISPATCH t={went[0][0]} job={went[0][1]} "
          f"target=({went[0][2]},{went[0][3]})")

    # 3. ...and the ENGINE agrees the corpse came back up.
    rose = [(t, a) for i, (t, a, _k, _o) in enumerate(trace)
            if i > 0 and a > trace[i - 1][1]]
    if not rose:
        print("FAIL (3): the corpse's armour never rose. The man was sent but "
              "the rebuild did not land -- he died on the way, he came home "
              "without spending the wood, or our own exploring tank drove over "
              "the corpse and picked it up first (in_tank with owner 0 in the "
              "trace below; if so, shorten the run). Engine trace: "
              + ", ".join(f"{t}:a{a}/t{k}/own{o}" for t, a, k, o in trace[:12]))
        return 1
    print(f"  3 OK: engine says armour rose to {rose[0][1]} at sim t={rose[0][0]}")
    print("PASS (B): with the guard off this arena reproduces the bug -- our "
          "man rebuilds the pill the ally was coming for. A's result is the "
          "guard, not the arena.")
    return 0


# ── variants C and D: the two ways a block ends ───────────────────────────
def _check_expiry(sess, text, build_dir, variant, want_silent_min,
                  want_silent_max, story):
    if not arena_sane(text, variant):
        return 1
    blocks = [b for b in BLOCK_RE.findall(text)
              if (int(b[2]), int(b[3])) == CORPSE]
    rels = [r for r in RELEASE_RE.findall(text)
            if (int(r[2]), int(r[3])) == CORPSE]
    disp = DISP_RE.findall(text)
    trace = read_trace(build_dir, variant)

    # 1. the scripted talker's advert landed and blocked us.
    if not blocks:
        print(f"FAIL (1): the block never started. tests/brains/"
              "advert_capture.lua sends `/info state goal=capture_pill "
              f"mx={CORPSE[0]} my={CORPSE[1]}` on the internal channel, which "
              "is ALLY-ONLY -- if the spawned bot is not on team 0 nothing is "
              "delivered at all. Check the sidecar's set_team, and the stderr "
              "for '[advert_capture]'.")
        return 1
    b0 = blocks[0]
    print(f"  1 OK: blocked at t={b0[0]} by p{b0[4]} (matched on the advert's "
          f"mx/my -- it carries no target id)")

    # 2. the block ended, once, and for the right reason.
    if not rels:
        print(f"FAIL (2): the block never ended within the run. Last BUILDER_POOL "
              "line: "
              + (([l for l in text.splitlines() if "BUILDER_POOL t=" in l]
                  or ["(none)"])[-1].strip()[:160]))
        return 1
    r0 = rels[0]
    silent = int(r0[5])
    print(f"  2 OK: released at t={r0[0]} silent={silent}t ttl={r0[6]}t "
          f"their_goal={r0[7] or '(none)'} their_target={r0[8] or '(none)'}")
    if not (want_silent_min <= silent <= want_silent_max):
        print(f"FAIL (2): expected the ex-blocker to have been silent "
              f"{want_silent_min}..{want_silent_max} ticks when the block "
              f"lifted, got {silent}. {story}")
        return 1
    held = int(r0[0]) - int(b0[0])
    print(f"     the block was held for {held} brain ticks")

    # 2b. our OWN tank must not have driven over the corpse in the meantime.
    #     It has nothing to do but explore, and explore sweeps every land tile
    #     sooner or later -- the arena buys time (spawn at the far end of the
    #     diamond) rather than making it impossible, because no terrain can.
    taken = [(t, o) for (t, _a, k, o) in trace if k == 1]
    if taken and taken[0][0] < int(r0[0]) * 2:
        print(f"FAIL (2b): the corpse went in_tank at sim t={taken[0][0]}, "
              f"before the block lifted at brain t={r0[0]} -- our own exploring "
              "tank drove over it and picked it up, so nothing after this "
              "measures the guard. Lower the sidecar's timing (REMOVE_AT in "
              f"variant C, SWITCH_AT in the talker's init for D) so the whole "
              "sequence finishes earlier in the sweep.")
        return 1

    # 3. and only THEN did the man go.
    went = [d for d in disp if (int(d[2]), int(d[3])) == CORPSE]
    if not went:
        print(f"FAIL (3): the man was never sent to {CORPSE} after the block "
              "lifted, so we cannot tell the release from a permanent refusal:")
        for ln in [l for l in text.splitlines() if "BP_DENY" in l][-6:]:
            print("   " + ln.strip())
        return 1
    if int(went[0][0]) < int(r0[0]):
        print(f"FAIL (3): the man was dispatched at t={went[0][0]}, BEFORE the "
              f"release at t={r0[0]} -- the block was not holding.")
        return 1
    print(f"  3 OK: BP_DISPATCH t={went[0][0]} job={went[0][1]}, "
          f"{int(went[0][0]) - int(r0[0])} tick(s) after the release")
    for m in RSPLIT_RE.findall(text)[:2]:
        print(f"     (pool-5 backstop also fired: REPAIR_ALLY_CAPTURE t={m[0]} "
              f"{m[4]})")

    rose = [(t, a) for i, (t, a, _k, _o) in enumerate(trace)
            if i > 0 and a > trace[i - 1][1]]
    if not rose:
        print("FAIL (3): the corpse's armour never rose. Engine trace: "
              + ", ".join(f"{t}:a{a}/t{k}" for t, a, k, _o in trace[:12]))
        return 1
    print(f"     engine says armour rose to {rose[0][1]} at sim t={rose[0][0]}")
    return 0


def check_C(sess, text, build_dir):
    # The removed bot's last /info state is at most `every` (50) thinks before
    # it vanished, and the guard is tested once per tick, so the release lands
    # on the first tick where age > TTL: silent is TTL+1 in the ideal case. The
    # window allows for the pool skipping a tick or two.
    rc = _check_expiry(
        sess, text, build_dir, "C", TTL, TTL + 20,
        "A value well UNDER the ttl means something other than the silence "
        "expiry lifted the block (did the slate get cleared?); a value well "
        "over it means the release was noticed late, which only happens if the "
        "corpse stopped being scored as a rebuild row for a while.")
    if rc:
        return rc
    print("PASS (C): the ally stopped talking and the block outlived it by "
          f"exactly the {TTL}-tick (7 s) budget, then the man went.")
    return 0


def check_D(sess, text, build_dir):
    # The talker is still sending once a second, so at the moment the block
    # lifts its last message is at most ~50 ticks old and typically ~0.
    rc = _check_expiry(
        sess, text, build_dir, "D", 0, 60,
        "The talker never stopped talking in this arena, so a `silent` value "
        f"near the ttl ({TTL}) would mean the block was ended by the EXPIRY "
        "rather than by the move-on -- i.e. the 'latest advert' test is not "
        "working and only the clock is.")
    if rc:
        return rc
    rels = [r for r in RELEASE_RE.findall(text)
            if (int(r[2]), int(r[3])) == CORPSE]
    if rels[0][7] != "explore":
        print(f"FAIL (4): at the release the ex-blocker's goal read "
              f"'{rels[0][7]}', expected 'explore' -- the move-on slate is not "
              "what ended the block.")
        return 1
    print(f"  4 OK: the ex-blocker's slate read goal=explore at the release")
    print("PASS (D): a fresh ally that changed its mind lifted the block the "
          "tick its new slate landed, with the 7-second clock nowhere near up.")
    return 0


CHECKS = {"A": check_A, "B": check_B, "C": check_C, "D": check_D}


def run(variant, ticks, build_dir):
    print(f"=== variant {variant} ({ticks} engine ticks) ===")
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
        print(f"unknown variant(s): {', '.join(unknown)} "
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
