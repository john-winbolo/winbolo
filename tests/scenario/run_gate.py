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

USAGE, from the repo root:

    C:\\Python310\\python.exe tests/scenario/run_gate.py
    C:\\Python310\\python.exe tests/scenario/run_gate.py --only heat_pill
    C:\\Python310\\python.exe tests/scenario/run_gate.py --jobs 1
    C:\\Python310\\python.exe tests/scenario/run_gate.py --build build-own

Exit 0 when every arena that was meant to pass passed, 1 otherwise.
"""

import argparse
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
}

PER_TEST_TIMEOUT_S = 10 * 60
GATE_RE = re.compile(r"^\s*--\s*GATE:\s*(.*)$")
VERDICT_RE = re.compile(r"^VERDICT\s+(PASS|FAIL)\s+(\S+)\s*(.*)$")


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


class Job(object):
    def __init__(self, name, port, opts, head, tail, log_dir):
        self.name = name
        self.port = port
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
        with open(out, "w", encoding="utf-8", newline="\n") as f:
            # What the runner knows and the arena does not: its own name, and
            # the tick the server will stop at. The prelude's deadline is
            # taken off the second so an arena that only states a check still
            # gets asked before the round is cut off.
            f.write('GATE_NAME = "%s"\nGATE_TICKS = %s\n\n'
                    % (self.name, self.opts["ticks"]))
            f.write(self.head)
            f.write("\n-- ===== the arena =====\n")
            f.write(body)
            f.write("\n-- ===== the prelude's tail =====\n")
            f.write(self.tail)

    def command(self, ds):
        o = self.opts
        cmd = [ds,
               "-map", os.path.join(self.work, self.name + ".map"),
               "-port", str(self.port),
               "-nolobby",
               "-gametype", o["gametype"],
               "-bots", str(o["bots"]),
               "-brain", os.path.join(ROOT, "brains", "GoalHunter_1.7",
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
        return cmd

    def start(self, ds, build_dir):
        self.prepare()
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
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = VERDICT_RE.match(line.strip())
                if m:
                    self.verdict, _, self.why = m.group(1), m.group(2), m.group(3)
                    return
        self.verdict = None
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
    ap.add_argument("--port-base", type=int, default=50300)
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

    pending, skipped, expected = [], [], {}
    port = args.port_base
    for name in arena_names(args.only, args.skip):
        opts = gate_options(name)
        if opts.get("skip") and not args.run_skipped:
            skipped.append((name, opts["skip"]))
            continue
        if opts.get("expect"):
            expected[name] = opts["expect"]
        pending.append(Job(name, port, opts, head, tail, log_dir))
        port += 1

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
            print("  ....  %-30s port %d  running=%d"
                  % (j.name, j.port, len(running)))
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
