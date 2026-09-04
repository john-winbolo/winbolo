#!/usr/bin/env python3
"""
attack_pill steal handshake -- commitment-aware pricing, targeted requests, and
a yield block that ends when the stealer leaves (GoalHunter 1.7, author's rules
of 2026-09-03).

WHAT IS UNDER TEST (goals.lua sync_ally_claimed_rejects / steal_competed_cost /
steal_drain_requests, init.lua's stq reply + /info extra cost advert,
constants.lua STEAL_COMPETED_FRESH_AGE / STEAL_COMPETED_MAX_AGE /
STEAL_YIELD_RELEASE_GRACE)

    Field incident 20260903_193428_1, bot3, t=1266-1270: p4 sent bot3 two steal
    requests back to back (`stq 4 3 188`, `stq 0 3 379`) while its own goal was
    attack_pill #5 and stayed #5; bot3 yielded both and then carried pills #0
    and #4 as ally_claimed by p4 for the full STEAL_YIELD_BLOCK (300 ticks,
    still up at t=1487) for takes p4 never went near.  Three changes:

    1. COMMITMENT-AWARE PRICE.  Both sides of the handshake now trade the
       COMPETED total -- the number that bot's own goal selection used, i.e. the
       raw pool cost after phase weight, influence, hysteresis, commitment and
       history -- instead of the raw cost_cache cost.  Bot3's goal selection had
       priced pill #0 at 223.6 against a raw of 423, and quoting the raw handed
       the pill to a challenger at 379.  It is used for the holder's sta reply,
       the holder's advertised `cost=` (tagged `cq=c` on the wire), and both
       sides' dual-hold decisions; where no competed total exists (the normal
       challenger case -- an ally-claimed row is REJECTed out of every pool) the
       raw cost is used and the line says `(raw)`.

    2. ONE TARGETED REQUEST PER REPLAN.  An stq now goes out only for the row
       goal selection would actually pick if the claim were lifted -- it has to
       beat the pool-6 row that DID compete and the goal that won the replan --
       and at most one leaves per replan.  Rows that fail print STEAL_REQ HOLD
       with the reason and burn no cooldown.

    3. YIELD RELEASE.  A yield block ends the moment the stealer's own advert
       shows it on something else, instead of running the full 300 ticks.

CHECKS (two arenas, see tests/generate_steal_handshake_map.py)

    DUEL arena -- one pill, both bots exactly 13 tiles from it:
      A1 no bot sends more than one stq in a tick, and never two inside one
         GOAL_REPLAN_INTERVAL;
      A2 every stq that IS sent passes its own gate: the STEAL_REQ SEND line's
         est_competed beats the winner it names (or names no winner, which
         means nothing won that replan and the row is the only candidate);
      A3 the DECISION lines carry units, and at least one reads an ally cost
         tagged `(competed)` -- i.e. the advert's `cq=c` made it across;
      B  at least one STEAL_YIELD_RELEASED naming what the stealer went to,
         fired inside STEAL_YIELD_BLOCK and no sooner than
         STEAL_YIELD_RELEASE_GRACE, and the released row is priced again
         afterwards (it stops being an ally_claimed REJECT);
      C  at least one sta/str reply quotes a COMPETED cost that is BELOW the
         raw cost_cache cost for the same pill -- the commitment discount
         actually on the wire, which is the half of the incident that decided
         it -- and the challenger's DECISION for a pill it does not hold reads
         `they_hold` at least once.

    BUSY arena -- same contested pill plus one 5 tiles from bot1 and 31 from
    bot0, so bot1 always has a cheaper take of its own (the incident's shape):
      D  bot1 wants to ask for the contested pill and is stopped: at least one
         STEAL_REQ HOLD, every HOLD names a reason, and no stq is ever sent for
         a pill while the bot's own winner that replan was a different one.

Usage: python steal_handshake_test.py [duel|busy] [--ticks N] [--build DIR]
                                      [--no-asap]
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
from generate_steal_handshake_map import (   # noqa: E402
    VARIANTS, PILL_A, PILL_B, BOT0_SPAWN, BOT1_SPAWN, ALIVE_HP,
    STEAL_YIELD_BLOCK, STEAL_YIELD_RELEASE_GRACE, GOAL_REPLAN_INTERVAL, mdist)

PORTS = {"duel": 50186, "busy": 50187}
DEFAULT_TICKS = 4000
ALLIES = (0, 1)                 # the two GoalHunter 1.7 bots; 2 is the opponent

# Every print2 line is prefixed with "<file>\t<lineno>\t[Nms] ", so all of these
# are used with re.search, never re.match.
TICK_RE = re.compile(r"^===TICK (\d+)===", re.M)
LUA_ERR_RE = re.compile(r"(?:stack traceback|attempt to (?:index|call|compare|perform)"
                        r"|\.lua:\d+: )")
# goals.lua steal_drain_requests: the per-replan verdict on every queued request.
SEND_RE = re.compile(
    r"STEAL_REQ t=(\d+) SEND pill=#(\d+) to=p(\d+) our_cost=([\d.-]+)\((\w+)\) "
    r"raw=([\d.-]+) ally_cost=(\S+) est_competed=([\d.-]+) shape=(.*?) winner=(.*?) "
    r"\((\d+) candidate")
HOLD_RE = re.compile(
    r"STEAL_REQ t=(\d+) HOLD pill=#(\d+) to=p(\d+) our_cost=([\d.-]+)\((\w+)\) "
    r"raw=([\d.-]+) est_competed=([\d.-]+) -- (.+)$", re.M)
# goals.lua sync_ally_claimed_rejects: edge-triggered, so no tick of its own --
# the enclosing ===TICK n=== block supplies it.
DECISION_RE = re.compile(
    r"SYNC_P6 pid=(\d+) DECISION ally=p(\d+) ally_cost=(\S+?)\((\w+)\) "
    r"our_cost=([\d.-]+)\((\w+)\) raw=([\d.-]+) frac=([\d.]+) we_hold=(\w+) "
    r"force_engaging=(\w+) -> (\S+) \[(.*?)\]$", re.M)
# goals.lua sync_ally_claimed_rejects, entry dump: also edge-triggered.
ENTRY_RE = re.compile(
    r"SYNC_P6 pid=(\d+) mx=\S+ my=\S+ cost=(\S+) _reject_in=(\S+) ")
# init.lua, the holder's answer to an stq.
REPLY_RE = re.compile(
    r"STEAL_REPLY t=(\d+) pill=#(\d+) to=p(\d+) (\w+) \[(\w+)\] "
    r"our_cost=(\S+?)\((\w+?)(?:,age=(\d+))?\) raw=(\S+) their_cost=(\S+) sub=(\S*)")
YIELD_RE = re.compile(r"STEAL_YIELD t=(\d+) pill=#(\d+) to=p(\d+) \((.*?)\)")
RELEASE_RE = re.compile(
    r"STEAL_YIELD_RELEASED t=(\d+) pill=#(\d+) to=p(\d+) reason=(.*?) "
    r"after (\d+)t \(block was (\d+)\)")
# goals.lua goal_selection's per-replan winner dump, used to cross-check that a
# request was never sent for a row the bot would not have picked.
FINAL_RE = re.compile(r"FINAL_SCORES t=(\d+) pool_size=(\d+) cur=(\S+)")
CAND_RE = re.compile(r"^\s*\[(\d+)\] (\w+)@(\d+),(\d+) total=([\d.-]+) ", re.M)


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


def play(variant, ticks, build_dir):
    """Run one arena; return {bot: print2 text} for the two allies, or None."""
    mapfile = HERE / f"steal_handshake_{variant}.map"
    final = HERE / f"steal_handshake_{variant}_final.json"
    stderr = HERE / f"steal_handshake_{variant}_stderr.txt"
    # Short label on purpose: the recorder truncates the session directory
    # name, and "steal_handshake_duel_test" came back as
    # "..._1_steal_handshake_duel_te", which no endswith() would ever match.
    label = f"steal_{variant}_test"

    ds = find_ds(build_dir)
    if not ds:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return None
    subprocess.run([sys.executable, str(HERE / "generate_steal_handshake_map.py"),
                    variant], check=True, stdout=subprocess.DEVNULL)
    for p in (final, stderr):
        if p.exists():
            try:
                p.unlink()
            except PermissionError:
                print(f"FAIL: {p.name} is locked - a previous WinBoloDS run is "
                      f"still going. Wait for it to exit, then retry.")
                return None

    env = dict(os.environ, WINBOLO_BRAINDBG_LABEL=label)
    cmd = [str(ds), "-map", str(mapfile), "-port", str(PORTS[variant]),
           "-nolobby",
           # OPEN: full 40/40 loadout (both bots can actually shoot a pill
           # down), and it leaves -teams alone. "2,1" is the contiguous-block
           # form: bots 0+1 are ALLIES, bot 2 is the scripted opponent.
           "-gametype", "open", "-teams", "2,1",
           "-bots", "3", "-brain", str(BRAIN),
           "-bot-init", f"0={BRAIN},1={BRAIN},2={FOE_BRAIN}",
           "-allow-unsafe-brains",
           # yesfull: the whole arena is known from tick 0 - the test is about
           # how the contested pill is NEGOTIATED, not about finding it.
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
    for b in ALLIES:
        log = sess / f"print2_bot{b}.log"
        if not log.exists():
            print(f"FAIL: no print2_bot{b}.log under {sess}")
            return None
        logs[b] = log.read_text(encoding="utf-8", errors="replace")
    print(f"  session: {sess.name}")
    return logs


def tick_blocks(text):
    """[(tick, block_text)] in log order."""
    out = []
    marks = list(TICK_RE.finditer(text))
    for i, m in enumerate(marks):
        end = marks[i + 1].start() if i + 1 < len(marks) else len(text)
        out.append((int(m.group(1)), text[m.start():end]))
    return out


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
    """Everything the checks need out of one bot's log, with ticks attached to
    the edge-triggered SYNC_P6 lines from their enclosing tick block."""
    out = {"send": [], "hold": [], "decision": [], "entry": [], "reply": [],
           "yield": [], "release": [], "winner": {}}
    for m in SEND_RE.finditer(text):
        out["send"].append(dict(
            t=int(m.group(1)), pid=int(m.group(2)), to=int(m.group(3)),
            cost=float(m.group(4)), units=m.group(5), raw=float(m.group(6)),
            ally=m.group(7), est=float(m.group(8)), shape=m.group(9),
            winner=m.group(10).strip(), n=int(m.group(11))))
    for m in HOLD_RE.finditer(text):
        out["hold"].append(dict(
            t=int(m.group(1)), pid=int(m.group(2)), to=int(m.group(3)),
            cost=float(m.group(4)), units=m.group(5), raw=float(m.group(6)),
            est=float(m.group(7)), why=m.group(8).strip()))
    for m in REPLY_RE.finditer(text):
        out["reply"].append(dict(
            t=int(m.group(1)), pid=int(m.group(2)), to=int(m.group(3)),
            verb=m.group(4), verdict=m.group(5), cost=m.group(6),
            units=m.group(7), age=m.group(8), raw=m.group(9),
            their=m.group(10), sub=m.group(11)))
    for m in YIELD_RE.finditer(text):
        out["yield"].append(dict(t=int(m.group(1)), pid=int(m.group(2)),
                                 to=int(m.group(3)), why=m.group(4)))
    for m in RELEASE_RE.finditer(text):
        out["release"].append(dict(
            t=int(m.group(1)), pid=int(m.group(2)), to=int(m.group(3)),
            reason=m.group(4), after=int(m.group(5)), block=int(m.group(6))))
    for tick, block in tick_blocks(text):
        for m in DECISION_RE.finditer(block):
            out["decision"].append(dict(
                t=tick, pid=int(m.group(1)), ally=int(m.group(2)),
                ally_cost=m.group(3), ally_units=m.group(4),
                cost=float(m.group(5)), units=m.group(6), raw=float(m.group(7)),
                frac=float(m.group(8)), we_hold=(m.group(9) == "true"),
                engaging=(m.group(10) == "true"), verdict=m.group(11),
                why=m.group(12)))
        for m in ENTRY_RE.finditer(block):
            out["entry"].append(dict(t=tick, pid=int(m.group(1)),
                                     cost=m.group(2), reject=m.group(3)))
        fm = FINAL_RE.search(block)
        if fm:
            cands = [(int(c.group(1)), c.group(2), (int(c.group(3)), int(c.group(4))),
                      float(c.group(5))) for c in CAND_RE.finditer(block)]
            top = next((c for c in cands if c[0] == 1), None)
            if top:
                out["winner"][int(fm.group(1))] = dict(kind=top[1], tile=top[2],
                                                       total=top[3])
    return out


# ---------------------------------------------------------------------------
# DUEL checks
# ---------------------------------------------------------------------------
def check_duel(logs):
    data = {b: collect(t) for b, t in logs.items()}
    for b in ALLIES:
        d = data[b]
        print(f"  bot{b}: {len(d['send'])} stq sent, {len(d['hold'])} held, "
              f"{len(d['reply'])} repl(y/ies), {len(d['yield'])} yield(s), "
              f"{len(d['release'])} release(s), {len(d['decision'])} DECISION line(s)")

    # -- A1: at most one request per tick, and never two inside one replan.
    for b in ALLIES:
        sends = data[b]["send"]
        per_tick = {}
        for s in sends:
            per_tick.setdefault(s["t"], []).append(s)
        for t, group in per_tick.items():
            if len(group) > 1:
                print(f"FAIL: bot{b} sent {len(group)} stq at t={t} "
                      f"(pills {[g['pid'] for g in group]}); at most one per replan")
                return 1
        ticks = sorted(per_tick)
        for a, c in zip(ticks, ticks[1:]):
            if c - a < GOAL_REPLAN_INTERVAL:
                print(f"FAIL: bot{b} sent stq at t={a} and again at t={c}, "
                      f"{c - a} ticks apart -- inside one "
                      f"GOAL_REPLAN_INTERVAL ({GOAL_REPLAN_INTERVAL})")
                return 1
    total_sends = sum(len(data[b]["send"]) for b in ALLIES)
    if total_sends == 0:
        print("FAIL: no steal request was ever sent -- the duel arena did not "
              "produce a negotiation, so nothing here is under test")
        return 1
    print(f"  A1: {total_sends} request(s) total, never more than one per bot "
          f"per replan")

    # -- A2: every request that went out passed its own gate.  Checked twice
    #        over: against the winner the SEND line itself names, and
    #        independently against that replan's FINAL_SCORES dump, so a wrong
    #        number in the line cannot make the check pass.
    for b in ALLIES:
        for s in data[b]["send"]:
            w = data[b]["winner"].get(s["t"])
            if s["winner"].startswith("none"):
                # "nothing won this replan" -- the pool really has to be empty,
                # which is what an ally claim on the only pill looks like.
                if w is not None:
                    print(f"FAIL: bot{b} t={s['t']} SEND says no goal won the "
                          f"replan, but FINAL_SCORES has "
                          f"{w['kind']}@{w['tile']} at {w['total']:.1f}")
                    return 1
                continue
            wm = re.search(r"([\d.-]+)$", s["winner"])
            if not wm:
                print(f"FAIL: bot{b} t={s['t']} SEND line has an unreadable "
                      f"winner field: {s['winner']!r}")
                return 1
            if s["est"] >= float(wm.group(1)):
                print(f"FAIL: bot{b} t={s['t']} sent stq for pill #{s['pid']} "
                      f"with est_competed={s['est']:.1f} >= the winner it names "
                      f"({s['winner']}) -- the gate should have held it")
                return 1
            if w and s["est"] >= w["total"]:
                print(f"FAIL: bot{b} t={s['t']} sent stq for pill #{s['pid']} "
                      f"(est {s['est']:.1f}) although FINAL_SCORES that replan "
                      f"picked {w['kind']}@{w['tile']} at {w['total']:.1f}")
                return 1
    print("  A2: every request sent beat the goal that won its replan (checked "
          "against both the SEND line and that tick's FINAL_SCORES)")

    # -- A3: the DECISION lines carry units, and the ally's competed price
    #        made it across the wire at least once.
    dec = [d for b in ALLIES for d in data[b]["decision"]]
    if not dec:
        print("FAIL: no SYNC_P6 DECISION line at all -- no ally ever claimed "
              "the contested pill, so the handshake never ran")
        return 1
    for d in dec:
        for k in ("units", "ally_units"):
            if d[k] not in ("raw", "competed"):
                print(f"FAIL: DECISION line has unit tag {d[k]!r}, expected "
                      f"raw or competed: {d}")
                return 1
    competed_ally = [d for d in dec if d["ally_units"] == "competed"]
    if not competed_ally:
        print("FAIL: no DECISION line ever read an ally cost tagged (competed) "
              "-- the holder's cq=c advert never reached the challenger")
        return 1
    ex = competed_ally[0]
    print(f"  A3: {len(dec)} DECISION line(s), all unit-tagged; "
          f"{len(competed_ally)} read the ally's COMPETED price "
          f"(first: t={ex['t']} pill #{ex['pid']} ally=p{ex['ally']} "
          f"ally_cost={ex['ally_cost']}(competed) vs our "
          f"{ex['cost']:.1f}({ex['units']}))")

    # -- B: a yield block ended early because the stealer went elsewhere.
    rel = [(b, r) for b in ALLIES for r in data[b]["release"]]
    if not rel:
        print(f"FAIL: no STEAL_YIELD_RELEASED anywhere -- every yield ran the "
              f"full STEAL_YIELD_BLOCK ({STEAL_YIELD_BLOCK})")
        return 1
    for b, r in rel:
        if not re.match(r"stealer_(target=\d+ kind=\S+|goal=\S+)$", r["reason"]):
            print(f"FAIL: bot{b} t={r['t']} release reason {r['reason']!r} does "
                  "not name what the stealer went to")
            return 1
        if r["after"] >= STEAL_YIELD_BLOCK:
            print(f"FAIL: bot{b} t={r['t']} released after {r['after']}t, which "
                  f"is not early -- STEAL_YIELD_BLOCK is {r['block']}")
            return 1
        if r["after"] < STEAL_YIELD_RELEASE_GRACE:
            print(f"FAIL: bot{b} t={r['t']} released after only {r['after']}t, "
                  f"inside STEAL_YIELD_RELEASE_GRACE "
                  f"({STEAL_YIELD_RELEASE_GRACE}) -- the stealer had not been "
                  "given a replan to pick the pill up")
            return 1
    # ...and the row really is priceable again afterwards.
    repriced = 0
    for b, r in rel:
        later = [e for e in data[b]["entry"]
                 if e["pid"] == r["pid"] and r["t"] < e["t"] <= r["t"] + STEAL_YIELD_BLOCK
                 and e["reject"] != "ally_claimed"]
        if later:
            repriced += 1
    if not repriced:
        print("FAIL: no released row was ever seen priced again inside the "
              "block it beat -- the release did not actually re-open the pill")
        return 1
    b, r = rel[0]
    print(f"  B: {len(rel)} release(s), all naming the stealer's new goal and "
          f"all inside [{STEAL_YIELD_RELEASE_GRACE}, {STEAL_YIELD_BLOCK}); "
          f"{repriced} of them show the row priced again afterwards "
          f"(first: bot{b} t={r['t']} pill #{r['pid']} to p{r['to']} "
          f"reason={r['reason']} after {r['after']}t)")

    # -- C: the commitment discount was on the wire.
    discounted = []
    for b in ALLIES:
        for rp in data[b]["reply"]:
            if rp["units"] != "competed":
                continue
            try:
                c, raw = float(rp["cost"]), float(rp["raw"])
            except ValueError:
                continue
            if c < raw:
                discounted.append((b, rp))
    if not discounted:
        print("FAIL: no sta/str reply ever quoted a COMPETED cost below the raw "
              "cost_cache cost -- the commitment discount never reached the "
              "wire, which is the half of the incident that decided it")
        return 1
    b, rp = discounted[0]
    print(f"  C: {len(discounted)} repl(y/ies) quoted a competed price below "
          f"the raw one (first: bot{b} t={rp['t']} pill #{rp['pid']} {rp['verb']} "
          f"[{rp['verdict']}] our_cost={rp['cost']}(competed,age={rp['age']}) vs "
          f"raw={rp['raw']}, sub={rp['sub']})")
    they_hold = [d for d in dec if not d["we_hold"] and d["why"].startswith("they_hold")]
    if not they_hold:
        print("FAIL: no DECISION ever read `they_hold` -- a challenger never "
              "once declined to ask, so the band was never the thing deciding")
        return 1
    print(f"     and {len(they_hold)} DECISION(s) read `they_hold` "
          f"(challenger not meaningfully cheaper than the holder's price)")
    return 0


# ---------------------------------------------------------------------------
# BUSY checks
# ---------------------------------------------------------------------------
def check_busy(logs):
    data = {b: collect(t) for b, t in logs.items()}
    for b in ALLIES:
        d = data[b]
        print(f"  bot{b}: {len(d['send'])} stq sent, {len(d['hold'])} held, "
              f"{len(d['decision'])} DECISION line(s)")

    holds = [(b, h) for b in ALLIES for h in data[b]["hold"]]
    if not holds:
        print("FAIL: no STEAL_REQ HOLD anywhere -- the busy arena never "
              "produced a request for a row the bot would not have picked, so "
              "the gate was never exercised")
        return 1
    for b, h in holds:
        if not h["why"]:
            print(f"FAIL: bot{b} t={h['t']} HOLD line gives no reason")
            return 1
    print(f"  D: {len(holds)} request(s) held. Reasons seen:")
    seen = {}
    for b, h in holds:
        seen.setdefault(re.sub(r"[\d.]+", "N", h["why"]), (b, h))
    for _, (b, h) in sorted(seen.items()):
        print(f"       bot{b} t={h['t']} pill #{h['pid']} to p{h['to']} "
              f"our_cost={h['cost']:.1f}({h['units']}) est={h['est']:.1f} "
              f"-- {h['why']}")

    # -- Nothing that WAS sent contradicts the replan that sent it: the pill
    #    asked for must not be one the bot's own winner that tick was elsewhere
    #    at a lower competed total.
    for b in ALLIES:
        for s in data[b]["send"]:
            w = data[b]["winner"].get(s["t"])
            if w and s["est"] >= w["total"]:
                print(f"FAIL: bot{b} t={s['t']} sent stq for pill #{s['pid']} "
                      f"(est {s['est']:.1f}) although its replan winner was "
                      f"{w['kind']}@{w['tile']} at {w['total']:.1f}")
                return 1
    sent = sum(len(data[b]["send"]) for b in ALLIES)
    print(f"  D: {sent} request(s) were sent, none of them for a row that lost "
          f"its own replan")
    return 0


def run_one(variant, ticks, build_dir):
    print(f"=== steal handshake / {variant} ({ticks} ticks; contested pill "
          f"{PILL_A} hp={ALIVE_HP}, {mdist(BOT0_SPAWN, PILL_A)} tiles from each "
          f"bot" + (f"; bot1 also has {PILL_B} at "
                    f"{mdist(BOT1_SPAWN, PILL_B)} tiles"
                    if variant == "busy" else "") + ")")
    logs = play(variant, ticks, build_dir)
    if logs is None:
        return 1
    if not sanity(logs):
        return 1
    rc = check_duel(logs) if variant == "duel" else check_busy(logs)
    if rc == 0:
        if variant == "duel":
            print("PASS: the handshake priced on competed costs, sent at most "
                  "one targeted request per replan, and ended its yield blocks "
                  "as soon as the stealer went elsewhere.")
        else:
            print("PASS: a challenger committed to its own take never asked for "
                  "the ally-claimed pill it would not have picked.")
    return rc


def main():
    args = sys.argv[1:]
    take_asap_flag(args)
    variants = list(VARIANTS)
    if args and args[0].lower() in VARIANTS:
        variants = [args.pop(0).lower()]
    ticks = DEFAULT_TICKS
    build_dir = DEFAULT_BUILD
    i = 0
    while i < len(args):
        if args[i] == "--ticks" and i + 1 < len(args):
            ticks = int(args[i + 1]); i += 2
        elif args[i] == "--build" and i + 1 < len(args):
            build_dir = Path(args[i + 1]); i += 2
        else:
            i += 1
    print(pacing_line())
    rc = 0
    for v in variants:
        rc |= run_one(v, ticks, build_dir)
    return rc


if __name__ == "__main__":
    sys.exit(main())
