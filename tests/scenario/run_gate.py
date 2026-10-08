#!/usr/bin/env python3
"""The scenario gate: run every arena in tests/scenario/ and stop on a failure.

Each arena is a pair -- tests/scenario/<name>.scenario.lua and
tests/scenario/maps/<name>.map -- written against the scenario API on main
(docs/SCENARIO_API.md) with the compat prelude in tests/scenario/
scenario_compat.lua in front of it.

WHAT A RUN IS. For each arena the runner

  1. makes a working directory of its own,
  2. copies the map into it,
  3. writes <name>.scenario.lua beside the map: the prelude's head, the
     arena's own text, then the prelude's tail, in that order, as one file,
     because the map's own discovery reads one file and the sandbox has no
     require and no dofile to load a second,
  4. runs WinBoloDS on that map,
  5. reads the verdict off standard output.

THE VERDICT. game.end_round's text goes to the lobby and not to the console,
so the arena says its answer with game.log:

    VERDICT PASS <name> <why>
    VERDICT FAIL <name> <why>

A line game.log is handed is dropped silently past 128 bytes, so the prelude
cuts one rather than losing it. An arena that says nothing before its tick
limit is a FAIL with the reason "no verdict".

PER-ARENA FLAGS. An arena states what it needs on a GATE line of its own,
anywhere in the file (most put it beside the verdict, at the foot):

    -- GATE: ticks=6000 bots=2 gametype=tournament ai=yesfull

Anything it leaves out takes the default below. An arena marked

    -- GATE: skip=<reason>
    -- GATE: expect=fail <reason>

is reported as SKIP, or as an expected failure that does not turn the gate
red. Both are debts; keep the reason short and say what would repay it.

TEAMS AND SETTINGS. allybots=N passes -allybots N, so the -bots are seated
on team N rather than on no team. setting=id=value passes -setting id=value
for one of the script's own lobby settings; two or more are separated by
commas.

A SHIPPED SCRIPT UNDER TEST. An arena that tests a mod rather than a bot
names the file on its GATE line, from the repository root:

    -- GATE: include=data/mods/RuleRoulette.scenario.lua

The runner writes that file, unchanged, after the prelude's head and before
the arena's own text, so the arena sees the script's globals and its
`scenario` table and can wrap them. Two or more are separated by commas.

USAGE, from the repo root, where <dir> is the build directory holding
WinBoloDS (left out, it is build-own):

    python3 tests/scenario/run_gate.py --build <dir>
    python3 tests/scenario/run_gate.py --build <dir> --only heat_pill
    python3 tests/scenario/run_gate.py --build <dir> --jobs 1

Exit 0 when every arena that was meant to pass passed, 1 otherwise.
"""

import argparse
import atexit
import os
import re
import shutil
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
MAPS = os.path.join(HERE, "maps")
BRAINS = os.path.join(ROOT, "tests", "brains")
PRELUDE = os.path.join(HERE, "scenario_compat.lua")
SCRIPT_MARK = "--@@SCRIPT@@"

DEFAULTS = {
    "ticks": "6000",
    "bots": "1",
    "gametype": "open",
    "ai": "yesfull",
    "limit": "20",
    "mines": None,
    "seed": "42",
    "script": None,
    "include": None,
    "allybots": None,
    "setting": None,
}

# The verdict helper a script= arena gets in place of the compat prelude. It
# says its verdict once; a second call is ignored, so a check that runs every
# second cannot print PASS and then FAIL.
SCRIPT_VERDICT = """GATE_SAID = false
function verdict(ok, why)
  if GATE_SAID then return end
  GATE_SAID = true
  game.log(string.sub(string.format("VERDICT %s %s %s",
    ok and "PASS" or "FAIL", GATE_NAME, tostring(why or "")), 1, 120))
end
"""

PER_TEST_TIMEOUT_S = 10 * 60

# A script= arena is one chunk, and Lua allows 200 locals in one. The gate
# stops above this many, a few short of the cap, so the message comes before
# the loader error does.
TOP_LEVEL_LOCALS_MAX = 195
TOP_LOCAL_RE = re.compile(r"^local\s+(function\s+)?([^=]*)")


def top_level_locals(text):
    """The locals a Lua file declares at its top level: every line that starts
    "local" in the first column, one for "local function f", and one per name
    before the "=" for "local a, b = ...". A rough count, which is all the
    check above needs: the top level of these files starts in column 0 and
    everything inside a block is indented."""
    n = 0
    for line in text.splitlines():
        m = TOP_LOCAL_RE.match(line)
        if m is None:
            continue
        if m.group(1):
            n += 1
            continue
        names = m.group(2).split("--", 1)[0]
        n += len([x for x in names.split(",") if x.strip()])
    return n
GATE_RE = re.compile(r"^\s*--\s*GATE:\s*(.*)$")
VERDICT_RE = re.compile(r"^VERDICT\s+(PASS|FAIL)\s+(\S+)\s*(.*)$")
PORT_RE = re.compile(r"listening on UDP port (\d+)")
BIND_FAILED_RE = re.compile(r"bind\(\) failed on port (\d+)")


def load_prelude():
    with open(PRELUDE, encoding="utf-8") as f:
        text = f.read()
    if SCRIPT_MARK not in text:
        sys.exit("scenario_compat.lua has no %s marker" % SCRIPT_MARK)
    head, tail = text.split(SCRIPT_MARK, 1)
    return head, tail


def arena_names(only, skip):
    out = []
    for f in sorted(os.listdir(HERE)):
        if not f.endswith(".scenario.lua"):
            continue
        name = f[: -len(".scenario.lua")]
        if only and not any(s in name for s in only):
            continue
        if any(s in name for s in skip):
            continue
        if not os.path.exists(os.path.join(MAPS, name + ".map")):
            print("  ----  %-30s no map in tests/scenario/maps" % name)
            continue
        out.append(name)
    return out


def gate_options(name):
    """The GATE line an arena states, over the defaults."""
    opts = dict(DEFAULTS)
    opts["skip"] = None
    opts["expect"] = None
    path = os.path.join(HERE, name + ".scenario.lua")
    with open(path, encoding="utf-8", errors="replace") as f:
        for line in f:
            m = GATE_RE.match(line)
            if not m:
                continue
            # A GATE line is read left to right. A `key=value` word starts a
            # new key; every word after it belongs to that key's value, so a
            # reason may be a whole sentence. Two things follow, and both
            # were bugs before they were rules: a reason may contain an `=`
            # (a cfg=X=false token, say) without being read as a new key, and
            # a reason written across two GATE lines is joined rather than
            # having its first half quietly dropped.
            last = None
            for word in m.group(1).split():
                if last in ("skip", "expect"):
                    opts[last] += " " + word
                elif "=" in word:
                    k, v = word.split("=", 1)
                    if k in ("skip", "expect") and opts.get(k):
                        opts[k] += " " + v      # a second line continues it
                    else:
                        opts[k] = v
                    last = k
                elif last is not None:
                    opts[last] += " " + word
    return opts


# ── The test brains ───────────────────────────────────────────
#
# An arena names a scripted opponent's brain the way the drivers did, by path:
# "../tests/brains/idle.lua". This host takes a NAME instead -- one directory
# under a brains parent, holding init.lua -- and refuses a value with a
# separator in it (scenario_lua.c scnResolveOpBrain). The prelude maps the
# path to its stem; the stem then has to be findable, which is what this does.
#
# brainListParents (brain_list.c) searches "brains" and "Brains" against the
# working directory before it looks anywhere else, and every arena's server
# runs with cwd=<build>, so <build>/Brains/<stem>/init.lua is on the path it
# already searches.
#
# <build>/Brains is not ours. CMake stages GoalHunter into it beside the
# binary, so only the stems staged here are removed at the end, and the
# directory itself only when this run made it. Copies, not links: this runs on
# Windows too.


def stage_brains(build_dir):
    """Every tests/brains/*.lua as <build>/Brains/<stem>/init.lua."""
    parent = os.path.join(build_dir, "Brains")
    made_parent = not os.path.isdir(parent)
    if made_parent:
        os.makedirs(parent)
    staged = []
    for f in sorted(os.listdir(BRAINS)):
        if not f.endswith(".lua"):
            continue
        stem = f[: -len(".lua")]
        if stem == "GoalHunter":       # never ours to write or remove
            continue
        d = os.path.join(parent, stem)
        mine = not os.path.isdir(d)
        if mine:
            os.makedirs(d)
            staged.append(d)
        shutil.copy(os.path.join(BRAINS, f), os.path.join(d, "init.lua"))
    return parent, made_parent, staged


def unstage_brains(parent, made_parent, staged):
    """Remove what stage_brains made, and nothing else."""
    for d in staged:
        shutil.rmtree(d, ignore_errors=True)
    if made_parent:
        try:
            os.rmdir(parent)
        except OSError:
            pass               # something else arrived in it; leave it alone


class Job(object):
    def __init__(self, name, opts, head, tail, log_dir):
        self.name = name
        self.port = None         # read off the server's own listening line
        self.bind_failed = False
        self.opts = opts
        self.head = head
        self.tail = tail
        self.log_path = os.path.join(log_dir, name + ".log")
        self.work = None
        self.proc = None
        self.log = None
        self.t0 = None
        self.rc = None
        self.dt = None
        self.verdict = None      # "PASS" / "FAIL" / None
        self.why = ""

    def prepare(self):
        self.work = tempfile.mkdtemp(prefix="scngate_" + self.name + "_")
        shutil.copy(os.path.join(MAPS, self.name + ".map"),
                    os.path.join(self.work, self.name + ".map"))
        with open(os.path.join(HERE, self.name + ".scenario.lua"),
                  encoding="utf-8") as f:
            body = f.read()
        out = os.path.join(self.work, self.name + ".scenario.lua")
        if self.opts.get("script"):
            self.prepare_with_script(out, body)
            return
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            # What the runner knows and the arena does not: its own name, the
            # tick the server will stop at, and the game type it is started
            # with. The prelude's deadline is taken off the second so an arena
            # that only states a check still gets asked before the round is
            # cut off. The third is what the prelude declares as scenario.game,
            # so the table and the -gametype flag name the same game.
            f.write('GATE_NAME = "%s"\nGATE_TICKS = %s\n'
                    'GATE_GAMETYPE = "%s"\n\n'
                    % (self.name, self.opts["ticks"], self.opts["gametype"]))
            f.write(self.head)
            for inc in (self.opts.get("include") or "").split(","):
                inc = inc.strip()
                if not inc:
                    continue
                with open(os.path.join(ROOT, inc), encoding="utf-8") as g:
                    f.write("\n-- ===== included: %s =====\n" % inc)
                    f.write(g.read())
            f.write("\n-- ===== the arena =====\n")
            f.write(body)
            f.write("\n-- ===== the prelude's tail =====\n")
            f.write(self.tail)

    def prepare_with_script(self, out, body):
        """An arena that tests a real game script, run as it ships.

        GATE script=<path> names a script under the checkout, such as
        data/mods/PillboxTag.scenario.lua. The file is written as: the GATE
        globals, a verdict() helper, the script's own text, then the arena.
        There is no compat prelude: the prelude hands every hook `game` as its
        first argument, and a real script's hooks do not take it. Because the
        arena is in the same chunk as the script, it can read the script's
        top-level locals and wrap its hooks (keep the old function in a local
        and call it from the new one). Keep the arena's own top-level locals
        few: Lua allows 200 in one chunk and a big script uses many of them.
        The gate counts them first and stops with a clear message when the
        two together come near the cap, rather than letting every script=
        arena fail with a loader error.
        """
        with open(os.path.join(ROOT, self.opts["script"]),
                  encoding="utf-8") as f:
            script = f.read()
        n = top_level_locals(script) + top_level_locals(body)
        if n > TOP_LEVEL_LOCALS_MAX:
            sys.exit("%s: %s and the arena declare %d top-level locals "
                     "together; Lua allows 200 in one chunk and the gate "
                     "stops above %d. Fold some of the script's locals into "
                     "a table." % (self.name, self.opts["script"], n,
                                   TOP_LEVEL_LOCALS_MAX))
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            f.write('GATE_NAME = "%s"\nGATE_TICKS = %s\n'
                    'GATE_GAMETYPE = "%s"\n\n'
                    % (self.name, self.opts["ticks"], self.opts["gametype"]))
            f.write(SCRIPT_VERDICT)
            f.write("\n-- ===== the script: %s =====\n" % self.opts["script"])
            f.write(script)
            f.write("\n-- ===== the arena =====\n")
            f.write(body)
            f.write("\n")

    def command(self, ds):
        o = self.opts
        cmd = [ds,
               "-map", os.path.join(self.work, self.name + ".map"),
               "-port", "0",
               "-nolobby",
               "-gametype", o["gametype"],
               "-bots", str(o["bots"]),
               "-brain", os.path.join(ROOT, "brains", "GoalHunter",
                                      "init.lua"),
               "-ai", o["ai"],
               "-seed", str(o["seed"]),
               "-ticks", str(o["ticks"]),
               "-asap",
               "-brain-no-budget-kill",
               "-brain-lua-seed", str(o["seed"]),
               "-nowinbolonet",
               "-threads", "1"]
        if o.get("limit") is not None:
            cmd += ["-limit", str(o["limit"])]
        if o.get("mines") is not None:
            cmd += ["-mines", str(o["mines"])]
        # -nolobby seats every -bots bot on no team; allybots=N puts them
        # all on team N, which a script that tests a team needs.
        if o.get("allybots") is not None:
            cmd += ["-allybots", str(o["allybots"])]
        # setting=id=value[,id=value] picks a value for one of the script's
        # own lobby settings, as -setting does.
        for st in (o.get("setting") or "").split(","):
            if st.strip():
                cmd += ["-setting", st.strip()]
        return cmd

    def start(self, ds, build_dir):
        self.prepare()
        self.bind_failed = False
        self.log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        cmd = self.command(ds)
        self.log.write("[gate] " + " ".join(cmd) + "\n")
        self.log.flush()
        self.t0 = time.time()
        self.proc = subprocess.Popen(cmd, cwd=build_dir, stdout=self.log,
                                     stderr=subprocess.STDOUT)

    def poll(self):
        rc = self.proc.poll()
        if rc is None:
            if time.time() - self.t0 > PER_TEST_TIMEOUT_S:
                self.proc.kill()
                self.proc.wait()
                self.log.write("\n[gate] TIMEOUT after %ds\n"
                               % PER_TEST_TIMEOUT_S)
                rc = -1
            else:
                return False
        self.rc = rc
        self.dt = time.time() - self.t0
        self.log.close()
        self.read_verdict()
        if self.work and os.path.isdir(self.work):
            shutil.rmtree(self.work, ignore_errors=True)
        return True

    def read_verdict(self):
        self.bind_failed = False
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            for line in f:
                s = line.strip()
                # Read before the verdict test: a server that never bound
                # never played the round, so there is no verdict to find and
                # the reason has to come off this line instead.
                if "bind() failed on port" in s:
                    self.bind_failed = True
                    m = BIND_FAILED_RE.search(s)
                    if m:
                        self.port = int(m.group(1))
                m = PORT_RE.search(s)
                if m:
                    self.port = int(m.group(1))
                m = VERDICT_RE.match(s)
                if m:
                    self.verdict, _, self.why = m.group(1), m.group(2), m.group(3)
                    return
        self.verdict = None
        if self.bind_failed:
            self.why = ("the server could not bind port %d" % self.port
                        if self.port is not None
                        else "the server could not bind its port")
        else:
            self.why = "no verdict (rc=%s)" % self.rc

    def tail_lines(self, n=4):
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            lines = [l.rstrip() for l in f if l.strip()]
        return "\n".join("      " + l for l in lines[-n:])


def find_ds(build_dir):
    for c in ("WinBoloDS.exe", "WinBoloDS",
              os.path.join("Release", "WinBoloDS.exe")):
        p = os.path.join(build_dir, c)
        if os.path.exists(p):
            return p
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None)
    ap.add_argument("--skip", nargs="*", default=[])
    ap.add_argument("--jobs", default="auto")
    ap.add_argument("--build", default="build-own")
    ap.add_argument("--run-skipped", action="store_true",
                    help="run the arenas marked skip as well")
    args = ap.parse_args()

    build_dir = args.build
    if not os.path.isabs(build_dir):
        build_dir = os.path.join(ROOT, build_dir)
    ds = find_ds(build_dir)
    if ds is None:
        sys.exit("no WinBoloDS under %s" % build_dir)

    ncpu = os.cpu_count() or 4
    jobs_max = ncpu if args.jobs == "auto" else max(1, int(args.jobs))

    head, tail = load_prelude()
    log_dir = os.path.join(build_dir, "scenario_gate_logs")
    if not os.path.isdir(log_dir):
        os.makedirs(log_dir)

    # Staged before the first arena starts and taken away however the run
    # ends, including a failing one.
    atexit.register(unstage_brains, *stage_brains(build_dir))

    pending, skipped, expected = [], [], {}
    for name in arena_names(args.only, args.skip):
        opts = gate_options(name)
        if opts.get("skip") and not args.run_skipped:
            skipped.append((name, opts["skip"]))
            continue
        if opts.get("expect"):
            expected[name] = opts["expect"]
        pending.append(Job(name, opts, head, tail, log_dir))

    print("gate: %d arena(s), up to %d at once, %d skipped, logs in %s"
          % (len(pending), jobs_max, len(skipped), log_dir))
    for name, why in skipped:
        print("  SKIP  %-30s %s" % (name, why))
    sys.stdout.flush()

    running, done, failed, xfail = [], [], [], []
    t_start = time.time()
    while pending or running:
        for j in list(running):
            if not j.poll():
                continue
            running.remove(j)
            done.append(j)
            expect = expected.get(j.name)
            if j.verdict == "PASS":
                if expect and expect.startswith("fail"):
                    # Red, like a FAIL, and said differently: the arena is
                    # fine and the mark on it is not.
                    failed.append(j.name)
                    print("  UPASS %-30s %6.1fs  (marked expect=fail -- "
                          "the mark is stale)" % (j.name, j.dt))
                else:
                    print("  PASS  %-30s %6.1fs  %s"
                          % (j.name, j.dt, j.why[:60]))
            elif expect and expect.startswith("fail"):
                xfail.append(j.name)
                print("  XFAIL %-30s %6.1fs  %s" % (j.name, j.dt, expect))
            else:
                failed.append(j.name)
                print("  FAIL  %-30s %6.1fs  %s" % (j.name, j.dt, j.why[:70]))
                t = j.tail_lines()
                if t:
                    print(t)
            sys.stdout.flush()

        while pending and len(running) < jobs_max:
            j = pending.pop(0)
            j.start(ds, build_dir)
            running.append(j)
            print("  ....  %-30s running=%d" % (j.name, len(running)))
            sys.stdout.flush()

        if running:
            time.sleep(0.5)

    total = time.time() - t_start
    print()
    passed = len(done) - len(failed) - len(xfail)
    print("ran %d arena(s) in %.0fs: %d passed, %d expected-fail, %d failed, "
          "%d skipped" % (len(done), total, passed, len(xfail), len(failed),
                          len(skipped)))
    if failed:
        print("GATE RED: " + ", ".join(failed))
        return 1
    print("GATE GREEN")
    return 0


if __name__ == "__main__":
    sys.exit(main())
