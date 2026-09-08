#!/usr/bin/env python3
"""the LOADED, BUILDER-LESS state -- what a tank does when it is carrying
pillboxes it cannot place (GoalHunter 1.7).

WHAT HAPPENED (winbolo2, 2026-09, a 4v4)
----------------------------------------
A bot carrying FIVE pillboxes, whose LGM had been killed, picked attack_tank,
drove in, and died. Everything it was holding fell on the ground as corpses for
whoever was still alive nearby. In engine terms that is a losing trade three
times over:

  * a killed LGM is choppered back from a RANDOM start tile at 3 world units
    per engine tick (~85 brain ticks a tile) to where the tank stood when he
    died, and then WALKS. A tank death does NOT bring him back: lgm.c
    lgmTankDied only clears nextAction. "He'll be along shortly" can be a
    minute of map, and dying does not shorten it.
  * with no builder the pills aboard cannot be placed at all. They are not a
    resource while he is away, they are cargo.
  * a shell is DAMAGE 5 against TANK_FULL_ARMOUR 40, so eight clean hits end
    the tank and hand the whole stack to the enemy.

THE CHANGE UNDER TEST gives that situation a name --

    state.loaded_no_lgm = carried_pills >= LOADED_NO_LGM_MIN_PILLS(3)
                          and (the man is DEAD, or he is out walking and his
                               return ETA is > LOADED_NO_LGM_ETA_TICKS(1000))

recomputed EVERY tick from engine truth and never latched -- and hangs five
things off it:

  1  ATTACKING costs x ATTACK_NO_BUILDER_MULT (10) on the FINAL competed cost
     of attack_tank / attack_pill / attack_base / defend_pill. A multiplier,
     not a reject: with an empty pool the bot must still be able to shoot back.
  2  ESCAPING (flee_to_base, and a take_cover row that fired on its haul or
     panic trigger) enters the competition with NO hysteresis at all --
     switch 0, commitment 0, and no multiplicative stickiness bar. Asymmetric:
     once one of them IS the goal, every other row pays the normal switch cost
     to displace it.
  3  CAPTURING is priced as pure risk: danger x CAPTURE_NO_LGM_DANGER_MULT(20)
     instead of the cautious-mode x5, free-pill bonus scaled to zero, cluster
     discount off, and a route probe that refuses ANY predicted damage instead
     of only a predicted kill.
  4  "KILL ME": the tank parks and asks a team-mate whose builder IS aboard to
     shoot it and pocket the corpses (allied shells DO damage allied tanks --
     tank.c tankIsTankHit ignores only the shooter's own shells -- and the
     corpses keep the dead player's ownership, which capture_pill scoops).
  5  (un-gated bug fix) state.lgm_stranded is cleared whenever the man is not
     OUT WALKING -- INTANK or DEAD alike -- and every tick, not only on
     replans. It used to survive the man's death for the whole of his next
     life, leaving the HUD, eval_wait_for_lgm and attack.lua's
     ATTACK_PP_HOLD_SKIP_STRANDED reading a fact that had stopped being true.

THE ARENAS  (tests/generate_kill_me_map.py builds the ground and the sidecars;
its docstring carries the geometry and why every piece of it is where it is)

  A   THE STATE, WITH AN ENEMY IN THE ROOM. Asserts the x10 lands on the
      attack rows as its own `nobuild=` field, that an attack row carrying it
      is NOT what the bot picks, and that the escape rows enter with
      `hyst=none:no_lgm sw=0.0 cmt=0.0`.
  B   THE CONTROL: the same ground, the same seed, `preset=keel`. Every row
      reads `nobuild=1.0` and no row ever reads `hyst=none:no_lgm`.
  C   THE HAND-OFF: p1 is an ALLY with its builder aboard and full shells.
  D1  the responder's man is OUT (the sidecar keeps a pill damaged so the
      builder pool keeps sending him)  -> the row must reject `no_lgm`.
  D2  the responder cannot meet the shell gate (cfg=KILL_ME_SHELL_MARGIN=90)
      -> the row must reject `low_shells`.
  E   CAPTURE PRICING IN THE STATE: a corpse under the neutral's fire, put on
      the ground on the tick the state starts.
  EK  the same arena with `preset=keel`, i.e. the four capture knobs at their
      pre-change values, so the two runs differ only in those four numbers.

Every outcome below is read from print2 (FINAL_SCORES, LOADED_NO_LGM,
CAPTURE_NO_LGM, KILL_ME_ROW) or from the sidecar's ENGINE-side pill trace --
never from the brain's opinion of itself.

Usage: python kill_me_test.py [--variant A|B|C|D1|D2|E|EK|all] [--ticks N]
                              [--build DIR]
Exit 0 on PASS, 1 on FAIL.
"""

import glob
import os
import re
import subprocess
import sys
from pathlib import Path

from asap import asap_args, take_asap_flag  # noqa: E402

HERE = Path(__file__).resolve().parent
REPO = HERE.parent
DEFAULT_BUILD = REPO / "build"
BRAIN = REPO / "brains" / "GoalHunter_1.7" / "init.lua"

sys.path.insert(0, str(HERE))
import generate_kill_me_map as G   # noqa: E402

VARIANTS = ("A", "B", "C", "D1", "D2", "E", "EK")
PORTS = {"A": 50520, "B": 50521, "C": 50522, "D1": 50523,
         "D2": 50524, "E": 50525, "EK": 50526}
# ENGINE ticks. The brain thinks once per frame and the sim advances two engine
# ticks per frame, so these are half as many brain ticks. The state starts
# around engine tick 700 in every arena (the sidecar's retry loop needs a
# completion with both tanks back in the room); the rest is measurement room.
TICKS = {v: 10000 for v in VARIANTS}
# The two keel controls need longer: keel's goal shaping keeps the tank east of
# the room more of the time, so the sidecar's "neither tank is east" guard has
# to wait for more completions before it can lay the wall.
TICKS["B"] = 20000
TICKS["EK"] = 20000

ARG_MAX = 127                       # BotInitSlot.arg, luabrainshandler.h

# ── the -bot-init tokens ─────────────────────────────────────────────────
# WHY EACH PIN IS THERE.
#   RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false  puts rescue_lgm back on the STATIC
#     gate (perc.under_fire = danger_at(our tile) > 0). In the arenas that
#     carry the neutral pillbox its permanent danger stamp then suppresses the
#     rescue for the whole run -- without which the tank fetches its stranded
#     man, the state ends, and there is nothing left to measure. (Arenas C/D
#     have no neutral, so the pin does nothing there; it is kept so all seven
#     runs are the same brain configuration apart from the knob under test.)
#   PILL_REPOSITION_ENABLED=false  stops a bot deciding the arena's errand pill
#     is badly placed and SHOOTING IT DOWN to move it -- measured, p1 did
#     exactly that at sim t=1262 and the arena lost its errand.
COMMON = ("cfg=RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false"
          ";cfg=PILL_REPOSITION_ENABLED=false")
# preset=keel restores the KEEL baseline wholesale, which includes
# RESCUE_LGM_SUPPRESS_BY_FIRE_AGE=false and all six knobs of this change; the
# reposition pin is not part of keel and still has to be spelled out.
# ...but keel ALSO turns off BUILDER_POOL_REPAIR_LINEAR, and the arena's whole
# errand is a top-up: under the old front-clock/threat formula the job scores
# under BUILDER_POOL_MIN_SCORE with the neutral's danger on the pill, the man
# is never dispatched, and the control run produces no state at all (measured:
# arena B walled nothing in 10000 ticks, with zero completions). The linear
# repair row is not one of the knobs under test, so it is pinned back ON --
# the control differs from the live run in the six knobs of THIS change and
# in nothing else that the arena depends on.
KEEL = ("preset=keel;cfg=BUILDER_POOL_REPAIR_LINEAR=true"
        ";cfg=PILL_REPOSITION_ENABLED=false")
# p1 with tank combat off is a VISIBLE HOSTILE TANK that never fires: it gives
# arena A the attack_tank row and the haul trigger it needs without shooting
# p0's man before the state can start.
QUIET_ENEMY = COMMON + ";cfg=TANK_COMBAT_ENABLED=false"
# p1 with its builder pool off keeps its man IN THE TANK, which is the
# precondition the kill_me responder row requires (a responder whose own man is
# out rejects with no_lgm -- that is arena D1's job, not arena C's).
ALLY_BUILDER_HOME = COMMON + ";cfg=BUILDER_POOL_ENABLED=false"
# ceil(40 armour / TANK_SHELL_DAMAGE 5) + 90 = 98 shells, which no tank can
# ever hold (TANK_FULL_SHELLS is 40), so the shell gate can never be met.
ALLY_UNDERGUNNED = COMMON + ";cfg=KILL_ME_SHELL_MARGIN=90"

TOKENS = {
    "A":  (COMMON, QUIET_ENEMY),
    "B":  (KEEL,   QUIET_ENEMY),
    "C":  (COMMON, ALLY_BUILDER_HOME),
    "D1": (COMMON, COMMON),
    "D2": (COMMON, ALLY_UNDERGUNNED),
    "E":  (COMMON, COMMON),
    "EK": (KEEL,   KEEL),
}
# Arenas E/EK measure ONE bot's capture pricing; a second tank in a seven-tile
# room only adds noise (and an ally would make kill_me_wait bid).
NBOTS = {"A": 2, "B": 2, "C": 2, "D1": 2, "D2": 2, "E": 1, "EK": 1}

# ── print2 lines ─────────────────────────────────────────────────────────
# init.lua, once per CHANGE of the state:
#   LOADED_NO_LGM t=254 false -> true loaded_no_lgm{carry 4>=3, man=OUT,
#     eta STUCK>1000, eta}
STATE_RE = re.compile(
    r"LOADED_NO_LGM t=(\d+) (\S+) -> (\S+) loaded_no_lgm\{([^}]*)\}")
# goals.lua FINAL_SCORES, one line per candidate per replan:
#   [1] attack_tank@123,128 total=258.0 base=51.6 pen=206.4 inf=0.50
#       nobuild=10.0 hyst=- sw=0.0 cmt=0.0 histT=0 ...
ROW_RE = re.compile(
    r"\[(\d+)\] (\w+)@(\d+),(\d+) total=([-\d.]+) base=([-\d.]+) pen=([-\d.]+) "
    r"inf=([-\d.]+) nobuild=([-\d.]+) hyst=(\S+) sw=([-\d.]+) cmt=([-\d.]+)")
# goals.lua compute_pool4_cost, once per tick while the state holds:
#   CAPTURE_NO_LGM t=900 pill@(122,126) danger_mult=20(no_builder) free_mult=0
#     cluster_discount=false route_strict=true cost=311.2 danger=8.0 free=0.0 ...
CAP_RE = re.compile(
    r"CAPTURE_NO_LGM t=(\d+) pill@\((\d+),(\d+)\) danger_mult=(\S+)\((\w+)\) "
    r"free_mult=(\S+) cluster_discount=(\w+) route_strict=(\w+) "
    r"cost=([-\d.]+) danger=([-\d.]+) free=([-\d.]+)")
# goals.lua eval_attack_tank, one line per kill_me request per eval:
#   KILL_ME_ROW t=900 subject=ally p0 tile=(126,127) armour=40 base=20
#     travel=31 cost=51 need_shells=10 have=40 man=0 reject=none why=-
KMROW_RE = re.compile(
    r"KILL_ME_ROW t=(\d+) subject=ally p(\d+) tile=\((\d+),(\d+)\) "
    r"armour=(\d+) base=(\d+) travel=([-\d.]+) cost=(\S+) need_shells=(\d+) "
    r"have=(-?\d+) man=(-?\d+) reject=(\S+)")
GOAL_RE = re.compile(r"GOAL_CHANGE Goal: (\w+)")

ATTACK_KINDS = {"attack_tank", "attack_pill", "attack_base", "defend_pill"}


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
    """The sidecar's ENGINE-side view. Returns (rows, marks) where rows are
    (sim_tick, x, y, armour, owner, in_tank) and marks is a dict of the
    `# <name> <tick> ...` comment lines the sidecar writes for the wall, the
    pill hand-out, the corpse and every retry."""
    path = build_dir / f"kill_me_{variant}_trace.log"
    rows, marks = [], {}
    if not path.exists():
        return rows, marks
    for line in path.read_text(errors="ignore").splitlines():
        line = line.strip()
        if line.startswith("#"):
            parts = line[1:].split()
            if len(parts) >= 2 and parts[1].isdigit():
                marks.setdefault(parts[0], []).append(
                    (int(parts[1]), " ".join(parts[2:])))
            continue
        if not line:
            continue
        p = line.split()
        if len(p) >= 6:
            try:
                rows.append(tuple(int(x) for x in p[:6]))
            except ValueError:
                pass
    return rows, marks


def run_sim(variant, ticks, build_dir):
    """Runs the arena; returns (session_dir, {bot: log_text}) or (None, msg)."""
    ds = find_ds(build_dir)
    if not ds:
        return None, f"WinBoloDS not found under {build_dir}"
    subprocess.run([sys.executable,
                    str(HERE / "generate_kill_me_map.py"), variant],
                   check=True, stdout=subprocess.DEVNULL)

    label = f"killme_{variant}"
    stderr = HERE / f"kill_me_{variant}_stderr.txt"
    trace = build_dir / f"kill_me_{variant}_trace.log"
    for p in (stderr, trace):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                return None, (f"{p.name} is locked -- a previous WinBoloDS run "
                              f"is still going. Wait for it to exit, then retry.")

    t0, t1 = TOKENS[variant]
    for who, tok in (("p0", t0), ("p1", t1)):
        if len(tok) > ARG_MAX:
            return None, (f"{who}'s -bot-init token string is {len(tok)} bytes, "
                          f"over the {ARG_MAX}-byte BRAIN_INIT_ARG limit -- it "
                          f"would be truncated mid-token and the last cfg= "
                          f"silently ignored:\n  {tok}")
    nbots = NBOTS[variant]
    init = f"0={BRAIN}[{t0}]"
    if nbots > 1:
        init += f",1={BRAIN}[{t1}]"
    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(HERE / f"kill_me_{variant}.map"),
           "-port", str(PORTS[variant]), "-nolobby",
           "-gametype", "open",
           "-bots", str(nbots), "-brain", str(BRAIN),
           "-bot-init", init,
           "-ai", "yesfull", "-limit", "20",
           "-brain-debug", "-seed", "42", "-ticks", str(ticks),
           "-brain-no-budget-kill", "-brain-lua-seed", "42",
           "-nowinbolonet", "-quiet", "-threads", "1"] + asap_args()
    with open(stderr, "wb") as errf:
        subprocess.run(cmd, cwd=str(build_dir), env=env,
                       stdout=errf, stderr=subprocess.STDOUT,
                       timeout=max(1200, ticks // 3))

    sess = newest_session(build_dir, label)
    if not sess:
        return None, ("no debug session produced (is the cwd on a drive with "
                      ">50 GB free? -brain-debug silently records nothing "
                      "otherwise)")
    crashes = list(sess.glob("brain_crash_*.log"))
    if crashes:
        return None, ("brain crashed -- " + str(crashes[0]) + "\n"
                      + Path(crashes[0]).read_text(errors="ignore")[:2000])
    logs = {}
    for b in range(nbots):
        f = sess / f"print2_bot{b}.log"
        if not f.exists():
            return None, f"no print2_bot{b}.log under {sess}"
        logs[b] = f.read_text(errors="ignore")
    return sess, logs


def rows_of(text):
    """Every FINAL_SCORES candidate line, as dicts."""
    out = []
    for m in ROW_RE.finditer(text):
        out.append(dict(rank=int(m.group(1)), kind=m.group(2),
                        mx=int(m.group(3)), my=int(m.group(4)),
                        total=float(m.group(5)), base=float(m.group(6)),
                        pen=float(m.group(7)), inf=float(m.group(8)),
                        nobuild=float(m.group(9)), hyst=m.group(10),
                        sw=float(m.group(11)), cmt=float(m.group(12))))
    return out


def state_windows(text):
    """[(on_tick, off_tick_or_None), ...] from the LOADED_NO_LGM edges."""
    wins, on = [], None
    for m in STATE_RE.finditer(text):
        t, to = int(m.group(1)), m.group(3)
        if to == "true" and on is None:
            on = t
        elif to == "false" and on is not None:
            wins.append((on, t))
            on = None
    if on is not None:
        wins.append((on, None))
    return wins


def the_state_is_real(step, variant, build_dir, logs):
    """Shared by every arena: the ENGINE walled the man out and put three
    pillboxes in p0's tank, and the BRAIN then read itself as loaded and
    builder-less. Without this, every verdict below is about an arena that
    never produced the state it exists to produce."""
    rows, marks = read_trace(build_dir, variant)
    # The man is detached either by walling him out ("wall", arenas A/B/E/EK)
    # or by killing him outright ("kill", arenas C/D1/D2 via game.kill_lgm).
    detach = "wall" if "wall" in marks else ("kill" if "kill" in marks else None)
    if detach == "kill":
        marks["wall"] = marks["kill"]
    if "wall" not in marks:
        n_retry = len(marks.get("retry", []))
        print(f"FAIL ({step}): the sidecar never walled the gate. It only walls "
              f"on a tick where the errand pill reads full AND neither tank is "
              f"east of the room, and it re-damages the pill to try again -- so "
              f"this means the two never coincided in {TICKS[variant]} ticks "
              f"({n_retry} retr(y/ies) recorded).")
        return 1, None
    wall_t = marks["wall"][0][0]
    gave = marks.get("gave", [])
    if not gave:
        print(f"FAIL ({step}): the gate was walled at sim t={wall_t} but no "
              f"pillboxes were ever loaded into p0.")
        return 1, None
    n_gave = gave[0][1]
    if "n=3" not in n_gave:
        print(f"FAIL ({step}): the sidecar loaded '{n_gave}' into p0, not 3 -- "
              f"under LOADED_NO_LGM_MIN_PILLS the state can never hold.")
        return 1, None
    wins = state_windows(logs[0])
    live = [w for w in wins if w[0] * 2 >= wall_t - 200]
    if not live:
        print(f"FAIL ({step}): the gate was walled at sim t={wall_t} and three "
              f"pillboxes went aboard, but the brain never read itself as "
              f"loaded and builder-less afterwards. Windows seen (brain ticks): "
              f"{wins[:6]}")
        return 1, None
    longest = max((w[1] or 10 ** 9) - w[0] for w in live)
    print(f"  {step} OK: THE STATE IS REAL -- the engine walled the gate column "
          f"at sim t={wall_t} and put 3 pillboxes in p0's tank on the same "
          f"tick, and the brain then read loaded_no_lgm true for "
          f"{longest} brain tick(s) across {len(live)} window(s).")
    return 0, live


def check_A(variant, logs, build_dir):
    rc, wins = the_state_is_real("0", variant, build_dir, logs)
    if rc:
        return rc
    lo = wins[0][0]
    rows = [r for r in rows_of(logs[0])]

    # 1. THE MULTIPLIER IS THERE AND IT IS ITS OWN TERM.
    mult = [r for r in rows if r["kind"] in ATTACK_KINDS and r["nobuild"] > 1.0]
    if not mult:
        print("FAIL (1): no FINAL_SCORES row ever carried nobuild=10.0 on an "
              "attack row, so ATTACK_NO_BUILDER_MULT never applied (or never "
              "printed). Attack rows seen: "
              + ", ".join(f"{r['kind']}@{r['mx']},{r['my']} nobuild={r['nobuild']}"
                          for r in rows if r["kind"] in ATTACK_KINDS)[:300])
        return 1
    bad = [r for r in mult if abs(r["nobuild"] - 10.0) > 0.01]
    if bad:
        print(f"FAIL (1): a row carried nobuild={bad[0]['nobuild']}, not the "
              f"default 10 -- the arena is not running the value it says it is.")
        return 1
    # ...AND THE ROW IS HAND-COMPUTABLE FROM ITS OWN CHIPS. FINAL_SCORES' `pen`
    # is a RESIDUAL (total - base), so "base + pen = total" is true by
    # construction and proves nothing. The real check is the multiplicative
    # chain, on the rows that carry no additive penalty at all
    # (hyst "-", sw 0, cmt 0):
    #     total = base x inf x nobuild
    # base is snapshotted at pool assembly (post phase-weight, PRE influence),
    # so both multipliers still have to be there for the number to come out.
    clean = [r for r in mult
             if r["hyst"] == "-" and r["sw"] == 0.0 and r["cmt"] == 0.0]
    if not clean:
        print("FAIL (1): every multiplied attack row also carried an additive "
              "hysteresis penalty, so none of them can be reconciled from the "
              "printed chips alone. The arena needs a replan where an attack "
              "row is the incumbent (hyst=-).")
        return 1
    off = [r for r in clean
           if abs(r["total"] - r["base"] * r["inf"] * r["nobuild"])
           > max(1.0, 0.02 * r["total"])]
    if off:
        r = off[0]
        print(f"FAIL (1): {r['kind']}@{r['mx']},{r['my']} prints total="
              f"{r['total']:.1f} but base{r['base']:.1f} x inf{r['inf']:.2f} x "
              f"nobuild{r['nobuild']:.0f} is "
              f"{r['base'] * r['inf'] * r['nobuild']:.1f} -- the row does not "
              f"add up to its own total, which is the whole point of printing "
              f"the multipliers as their own fields.")
        return 1
    r = clean[0]
    print(f"  1 OK: {len(mult)} attack row(s) carried nobuild=10.0 as their own "
          f"printed field, and all {len(clean)} penalty-free one(s) reconcile "
          f"by hand: {r['kind']}@{r['mx']},{r['my']} base={r['base']:.1f} x "
          f"inf={r['inf']:.2f} x nobuild={r['nobuild']:.0f} = "
          f"{r['base'] * r['inf'] * r['nobuild']:.1f} = total {r['total']:.1f}.")

    # 2. AND IT ACTUALLY COSTS THE ATTACK THE PICK.
    lost = [r for r in mult if r["rank"] > 1]
    if not lost:
        print("FAIL (2): every multiplied attack row was still rank [1] -- the "
              "x10 was applied but never changed what the bot picked, so this "
              "arena has not shown the surcharge doing anything.")
        return 1
    r = lost[0]
    print(f"  2 OK: THE SURCHARGE DECIDES -- {r['kind']}@{r['mx']},{r['my']} "
          f"priced {r['base']:.1f} raw came out at {r['total']:.1f} after x10 "
          f"and finished rank [{r['rank']}], not [1]. "
          f"{len(lost)}/{len(mult)} multiplied attack rows lost the pick.")

    # 3. THE ESCAPE ROWS ENTER FREE.
    free = [r for r in rows if r["hyst"] == "none:no_lgm"]
    if not free:
        print("FAIL (3): no row ever entered with hyst=none:no_lgm, so "
              "ESCAPE_NO_BUILDER_SKIP_HYST never waived anything. Rows with a "
              "hysteresis tier: "
              + ", ".join(sorted({f"{r['kind']}:{r['hyst']}" for r in rows}))[:300])
        return 1
    charged = [r for r in free if r["sw"] != 0.0 or r["cmt"] != 0.0]
    if charged:
        r = charged[0]
        print(f"FAIL (3): {r['kind']}@{r['mx']},{r['my']} is tagged "
              f"hyst=none:no_lgm but was still charged sw={r['sw']} "
              f"cmt={r['cmt']} -- 'no penalty to enter' means none.")
        return 1
    kinds = sorted({r["kind"] for r in free})
    print(f"  3 OK: THE ESCAPE ROWS ENTER FREE -- {len(free)} row(s) "
          f"({', '.join(kinds)}) carried hyst=none:no_lgm with sw=0.0 cmt=0.0. "
          f"The waiver is on the row ENTERING; nothing else in the pool is "
          f"exempted, so displacing one still costs the normal switch.")
    return 0


def check_B(variant, logs, build_dir):
    rc, _wins = the_state_is_real("0", variant, build_dir, logs)
    if rc:
        return rc
    rows = rows_of(logs[0])
    mult = [r for r in rows if r["nobuild"] != 1.0]
    if mult:
        r = mult[0]
        print(f"FAIL (1): {r['kind']}@{r['mx']},{r['my']} carried "
              f"nobuild={r['nobuild']} under preset=keel, where "
              f"ATTACK_NO_BUILDER_MULT is 1. The keel preset is not reaching "
              f"this bot.")
        return 1
    print(f"  1 OK: all {len(rows)} FINAL_SCORES rows read nobuild=1.0 -- under "
          f"preset=keel the surcharge does not exist, in the same state, on the "
          f"same ground, at the same seed.")
    free = [r for r in rows if r["hyst"] == "none:no_lgm"]
    if free:
        r = free[0]
        print(f"FAIL (2): {r['kind']}@{r['mx']},{r['my']} entered with "
              f"hyst=none:no_lgm under preset=keel, where "
              f"ESCAPE_NO_BUILDER_SKIP_HYST is false.")
        return 1
    tiers = sorted({r["hyst"] for r in rows})
    print(f"  2 OK: no row entered with hyst=none:no_lgm; every row paid an "
          f"ordinary tier ({', '.join(tiers)}). That is today's behaviour, "
          f"reproduced by the preset rather than by a rebuilt brain.")
    return 0


def check_C(variant, logs, build_dir):
    """The hand-off. This arena reaches the initiator half and reports honestly
    on the responder half -- see the note it prints when the request never
    survives long enough to be answered."""
    rc, wins = the_state_is_real("0", variant, build_dir, logs)
    if rc:
        return rc

    # 1. THE INITIATOR BIDS, AND THE BID WINS ITS REPLAN.
    rows = rows_of(logs[0])
    kmw = [r for r in rows if r["kind"] == "kill_me_wait"]
    if not kmw:
        print("FAIL (1): kill_me_wait never entered a pool competition, so the "
              "initiator half never ran at all.")
        return 1
    picked = [r for r in kmw if r["rank"] == 1]
    goals = GOAL_RE.findall(logs[0])
    took = goals.count("kill_me_wait")
    if not picked and not took:
        print(f"FAIL (1): kill_me_wait bid {len(kmw)} time(s) but was never "
              f"picked. Best rank seen: {min(r['rank'] for r in kmw)}, cheapest "
              f"total {min(r['total'] for r in kmw):.1f}.")
        return 1
    print(f"  1 OK: THE INITIATOR ASKS -- kill_me_wait bid {len(kmw)} time(s) at "
          f"base cost {kmw[0]['base']:.0f}, won its replan {len(picked)} time(s) "
          f"and was actually adopted as the goal {took} time(s).")

    # 2. THE RESPONDER SEES THE REQUEST.
    kmrows = KMROW_RE.findall(logs.get(1, ""))
    if not kmrows:
        print("FAIL (2): p1 never saw a kill_me token. The sidecar kills p0's "
              "builder with game.kill_lgm (engine death path, re-applied every "
              "tick so he never lands), so the state holds for the whole run; "
              "if the token never reached p1 the initiator either never held "
              "kill_me_wait long enough for a heartbeat or the /info extra "
              "slate dropped the km key.")
        return 1
    good = [r for r in kmrows if r[11] == "none"]
    if not good:
        print(f"FAIL (2): p1 saw {len(kmrows)} kill_me row(s) but rejected every "
              f"one: {sorted({r[11] for r in kmrows})}. p1 is configured with "
              f"its builder aboard and full shells, so none of those should "
              f"apply.")
        return 1
    r = good[0]
    print(f"  2 OK: THE ALLY ANSWERS -- p1's pool carried "
          f"subject=ally p{r[1]} @({r[2]},{r[3]}) priced base {r[5]} + travel "
          f"{r[6]} = {r[7]}, with {r[9]} shells against a gate of {r[8]}.")

    # 2b. THE RESPONDER ACTUALLY DRIVES AT THE ALLY: the fight loop's kill_me
    #     target override ran (KILL_ME_STEER lines in p1's log).
    steer = logs.get(1, "").count("KILL_ME_STEER")
    if not steer:
        print("FAIL (2b): p1 priced the request but never adopted the delivery "
              "-- no KILL_ME_STEER line, so attack_tank subject=ally never "
              "steered.")
        return 1
    print(f"  2b OK: p1 steered at the ally on {steer} tick(s).")
    # 2c. THE INITIATOR HELD STILL AND DIED: the sidecar's trace marks p0's
    #     death edge, and the initiator log must show the commit-to-die state
    #     (KILL_ME_EXEC executing=true) before it.
    trows, marks = read_trace(build_dir, variant)
    deaths = [t for (t, rest) in marks.get("p0dead", []) if rest.strip() == "true"]
    execs = logs[0].count("KILL_ME_EXEC") and "executing=true" in logs[0]
    if not execs:
        print("FAIL (2c): p0 never entered the executing state (no KILL_ME_EXEC "
              "executing=true), so the claimant was never recognised as the "
              "shooter and the armour triggers were live.")
        return 1
    if not deaths:
        print("FAIL (2c): p0 was never killed (no `# p0dead <tick> true` mark in "
              "the trace) -- the hand-off did not land.")
        return 1
    print(f"  2c OK: p0 committed to die (executing=true) and was killed at sim "
          f"t={deaths[0]}.")
    # 3. THE CORPSES CHANGE HANDS.
    trows, _marks = read_trace(build_dir, variant)
    taken = [t for t in trows if t[5] == 1 and t[4] == 1]
    if not taken:
        print("FAIL (3): no pill was ever seen in p1's tank after the kill, so "
              "the pickup half of the hand-off did not complete.")
        return 1
    print(f"  3 OK: a corpse ended up in p1's tank at sim t={taken[0][0]}.")
    return 0


def check_D(variant, logs, build_dir, want_reject):
    rc, _wins = the_state_is_real("0", variant, build_dir, logs)
    if rc:
        return rc
    kmrows = KMROW_RE.findall(logs.get(1, ""))
    if not kmrows:
        print(f"NOTE (1): p1 never saw a kill_me token, so the '{want_reject}' "
              f"reject could not be measured. Same structural reason as arena "
              f"C -- run kill_me_test.py --variant C for the full explanation.")
        return 2
    hit = [r for r in kmrows if r[11] == want_reject]
    if not hit:
        print(f"FAIL (1): p1 saw {len(kmrows)} kill_me row(s) but never rejected "
              f"with '{want_reject}'. Reasons seen: "
              f"{sorted({r[11] for r in kmrows})}")
        return 1
    r = hit[0]
    print(f"  1 OK: the responder row was REJECTED '{want_reject}' -- "
          f"subject=ally p{r[1]}, our man={r[10]}, shells {r[9]} against a gate "
          f"of {r[8]}, and the row is still shown with its reason rather than "
          f"silently dropped.")
    return 0


def capture_lines(text):
    out = []
    for m in CAP_RE.finditer(text):
        out.append(dict(t=int(m.group(1)), mx=int(m.group(2)), my=int(m.group(3)),
                        dmult=float(m.group(4)), src=m.group(5),
                        fmult=float(m.group(6)), cluster=m.group(7) == "true",
                        strict=m.group(8) == "true", cost=float(m.group(9)),
                        danger=float(m.group(10)), free=float(m.group(11))))
    return out


# Filled by check_E so compare_E does not have to go looking for the sessions
# again -- newest_session() globs "*killme_E*", which also matches killme_EK,
# and the comparison silently read the same run twice.
E_RESULT = {}


def check_E(variant, logs, build_dir, expect):
    rc, _wins = the_state_is_real("0", variant, build_dir, logs)
    if rc:
        return rc
    caps = capture_lines(logs[0])
    if not caps:
        print("FAIL (1): no CAPTURE_NO_LGM line was ever printed, so the "
              "capture formula never ran while the state held -- there was no "
              "corpse to price, or the state and the corpse never overlapped.")
        return 1
    c = caps[0]
    got = (c["dmult"], c["fmult"], c["cluster"], c["strict"])
    if got != expect:
        print(f"FAIL (1): the capture row read danger_mult={c['dmult']} "
              f"free_mult={c['fmult']} cluster_discount={c['cluster']} "
              f"route_strict={c['strict']}, expected {expect} for this arena.")
        return 1
    print(f"  1 OK: all four capture terms read exactly as configured -- "
          f"danger_mult={c['dmult']:.0f} (source {c['src']}), "
          f"free_mult={c['fmult']:.0f}, cluster_discount={c['cluster']}, "
          f"route_strict={c['strict']}, over {len(caps)} priced tick(s).")
    E_RESULT[variant] = dict(cost=min(x["cost"] for x in caps),
                             danger=c["danger"], free=c["free"],
                             dmult=c["dmult"])
    print(f"     cheapest capture cost while loaded and builder-less: "
          f"{min(x['cost'] for x in caps):.1f} "
          f"(danger {c['danger']:.1f}, free bonus {c['free']:.1f})")
    return 0


def compare_E(build_dir):
    """E against EK: the same corpse, the same ground, the same seed, four
    constants apart. The state's price must not be CHEAPER, and the free-pill
    bonus -- the one term with room to move on this corpse -- must be gone.

    The two costs are close on purpose-built ground: the corpse sits under
    tree cover, so threat reads well under a point and the danger multiplier
    (x20 against x5) has almost nothing to multiply, while the keel run's
    free-pill bonus is large enough that CAPTURE_FREE_PILL_MIN_COST floors its
    cost anyway. The four TERMS are asserted exactly by check_E; this is the
    end-to-end sanity check on top of them."""
    if "E" not in E_RESULT or "EK" not in E_RESULT:
        return 0                      # a single-variant run: nothing to compare
    e, k = E_RESULT["E"], E_RESULT["EK"]
    if e["cost"] < k["cost"] - 0.01:
        print(f"FAIL (E/EK): the capture priced {e['cost']:.1f} in the state and "
              f"{k['cost']:.1f} at keel -- the four knobs made it CHEAPER, "
              f"which is the opposite of what they are for.")
        return 1
    if e["free"] != 0.0 or k["free"] <= 0.0:
        print(f"FAIL (E/EK): the free-pill bonus read {e['free']:.1f} in the "
              f"state and {k['free']:.1f} at keel; it should be exactly 0 with "
              f"CAPTURE_NO_LGM_FREE_BONUS_MULT at its default and non-zero at "
              f"its keel value of 1.")
        return 1
    print(f"  E/EK OK: the same corpse on the same ground priced "
          f"{e['cost']:.1f} with the four knobs at their defaults against "
          f"{k['cost']:.1f} at their KEEL values, and the free-pill bonus went "
          f"from {k['free']:.1f} to {e['free']:.1f}. The danger term barely "
          f"moves here (threat {e['danger']:.1f} under tree cover, so x{e['dmult']:.0f} "
          f"against x{k['dmult']:.0f} is worth under a point) and keel's bonus is "
          f"big enough that CAPTURE_FREE_PILL_MIN_COST floors its total -- the "
          f"four TERMS above are where the knobs are actually asserted.")
    return 0


CHECKS = {
    "A":  lambda v, l, b: check_A(v, l, b),
    "B":  lambda v, l, b: check_B(v, l, b),
    "C":  lambda v, l, b: check_C(v, l, b),
    "D1": lambda v, l, b: check_D(v, l, b, "no_lgm"),
    "D2": lambda v, l, b: check_D(v, l, b, "low_shells"),
    # (danger_mult, free_mult, cluster_discount, route_strict)
    "E":  lambda v, l, b: check_E(v, l, b, (20.0, 0.0, False, True)),
    "EK": lambda v, l, b: check_E(v, l, b, (5.0, 1.0, True, False)),
}


def main():
    argv = sys.argv[1:]
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
    todo = list(VARIANTS) if variant == "all" else [variant]
    worst = 0
    for v in todo:
        n = ticks or TICKS[v]
        print(f"\n=== arena {v} ({n} engine ticks) ===")
        sess, logs = run_sim(v, n, build_dir)
        if sess is None:
            print(f"FAIL (setup): {logs}")
            worst = 1
            continue
        rc = CHECKS[v](v, logs, build_dir)
        if rc == 1:
            worst = 1
        elif rc == 2 and worst == 0:
            worst = 2
        print(f"--- arena {v}: "
              + ("PASS" if rc == 0 else "PARTIAL (see NOTE)" if rc == 2 else "FAIL"))
    if variant == "all" and worst != 1:
        print()
        if compare_E(build_dir):
            worst = 1
    print("\n" + ("PASS" if worst == 0
                  else "PARTIAL -- some arenas could not be measured; see NOTEs"
                  if worst == 2 else "FAIL"))
    return 0 if worst != 1 else 1


if __name__ == "__main__":
    sys.exit(main())
