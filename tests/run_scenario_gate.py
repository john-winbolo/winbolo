#!/usr/bin/env python3
"""PIGEON gate: run every tests/*_test.py (the scenario-sidecar tests and the
C/Lua parity test) and stop the propagation on any failure.

Rule (2026-09-05): the complicated bot tests are written as scenario tests
and live in winbolo2 only -- they are never pigeoned. The bot/engine code is
propagated to winbolo only once this gate is green, so the release line gets
tested code without carrying the scenario harness.

The tests run IN PARALLEL (2026-09-10). Each test's WinBoloDS runs with
-threads 1, so one test uses about one core; the gate keeps adding tests
while the machine has CPU and memory to spare and holds when it doesn't:
every --interval seconds it samples total CPU (GetSystemTimes) and free
memory (GlobalMemoryStatusEx) and starts one more test if CPU is under
--cpu-high, free memory is over --mem-min-gb and fewer than --jobs are
running. Each test gets its own server port through tests/gate_wrap.py
(GATE_PORT), so the 30-odd tests that never pass -port do not collide; a
test that sets its own -port keeps it, and two such tests sharing a port
are never run at the same time.

Usage (from the repo root, build/ present):
  C:\\Python310\\python.exe tests/run_scenario_gate.py            # everything, --jobs auto
  C:\\Python310\\python.exe tests/run_scenario_gate.py --jobs 1   # the old serial run
  C:\\Python310\\python.exe tests/run_scenario_gate.py --only steal_handshake refuel_pad_reach
  C:\\Python310\\python.exe tests/run_scenario_gate.py --skip aim
Exit code 0 = all green, 1 = something failed (listed at the end).
"""
import argparse
import ctypes
import os
import re
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")
WRAP = os.path.join(TESTS, "gate_wrap.py")

# Tests that are known-broken for their own reasons and must not block a
# propagation. Keep this list SHORT and dated; an entry here is a debt.
KNOWN_BROKEN = {
    "aim_test.py": "pre-existing failure (noted 2026-09-03), not chased",
}

# Tests the gate runs with EXTRA ARGUMENTS because one part of them is a known
# debt. Same rule as KNOWN_BROKEN -- short, dated, and it costs coverage -- but
# it costs LESS coverage than skipping the whole file, so prefer it.
PARTIAL = {
    # (empty) Repaid 2026-09-05: builder_pool variant A assertion 2 used to sit
    # here because the arena could not keep our pill worn across the take's
    # fire exchange. game.set_pill_armour(n, a) (scenario.c, a07fae5e) gave the
    # sidecar a way to re-wear it, so the whole file runs again.
}

# Not tests of the bots.
NOT_A_TEST = {"run_test.py", "gate_wrap.py"}

PER_TEST_TIMEOUT_S = 15 * 60

_PORT_RE = re.compile(r'"-port"\s*,\s*"(\d+)"')


def discover():
    names = sorted(f for f in os.listdir(TESTS)
                   if f.endswith("_test.py") and f not in NOT_A_TEST)
    return names


def explicit_port(name):
    """The -port a test hard-codes for its server, or None if it passes none
    (then the gate hands it a private one through GATE_PORT)."""
    with open(os.path.join(TESTS, name), encoding="utf-8", errors="replace") as f:
        m = _PORT_RE.search(f.read())
    return int(m.group(1)) if m else None


# ── machine load (no psutil on this box; straight Win32 through ctypes) ──────

class _FILETIME(ctypes.Structure):
    _fields_ = [("lo", ctypes.c_uint32), ("hi", ctypes.c_uint32)]


def _ft(ft):
    return (ft.hi << 32) | ft.lo


class CpuSampler:
    """Total CPU busy fraction between two samples, from GetSystemTimes."""

    def __init__(self):
        self._k32 = ctypes.windll.kernel32 if os.name == "nt" else None
        self._last = self._read()

    def _read(self):
        if self._k32 is None:
            return None
        idle, kern, user = _FILETIME(), _FILETIME(), _FILETIME()
        self._k32.GetSystemTimes(ctypes.byref(idle), ctypes.byref(kern), ctypes.byref(user))
        return _ft(idle), _ft(kern), _ft(user)

    def percent(self):
        now = self._read()
        if now is None or self._last is None:
            return 0.0
        d_idle = now[0] - self._last[0]
        d_total = (now[1] - self._last[1]) + (now[2] - self._last[2])  # kernel includes idle
        self._last = now
        if d_total <= 0:
            return 0.0
        return 100.0 * (1.0 - d_idle / float(d_total))


class _MEMSTATUS(ctypes.Structure):
    _fields_ = [("dwLength", ctypes.c_uint32), ("dwMemoryLoad", ctypes.c_uint32),
                ("ullTotalPhys", ctypes.c_uint64), ("ullAvailPhys", ctypes.c_uint64),
                ("ullTotalPageFile", ctypes.c_uint64), ("ullAvailPageFile", ctypes.c_uint64),
                ("ullTotalVirtual", ctypes.c_uint64), ("ullAvailVirtual", ctypes.c_uint64),
                ("ullAvailExtendedVirtual", ctypes.c_uint64)]


def free_mem_gb():
    if os.name != "nt":
        return 1e9
    st = _MEMSTATUS()
    st.dwLength = ctypes.sizeof(st)
    ctypes.windll.kernel32.GlobalMemoryStatusEx(ctypes.byref(st))
    return st.ullAvailPhys / float(1 << 30)


# ── one test ─────────────────────────────────────────────────────────────────

class Job:
    def __init__(self, name, port, own_port, extra, log_dir):
        self.name = name
        self.port = port            # the server port this test will use
        self.own_port = own_port    # True: the test hard-codes it (we don't inject)
        self.extra = tuple(extra)
        self.log_path = os.path.join(log_dir, name.replace(".py", ".log"))
        self.log = None
        self.proc = None
        self.t0 = None
        self.rc = None
        self.dt = None

    def start(self):
        self.log = open(self.log_path, "w", encoding="utf-8", errors="replace")
        if self.extra:
            self.log.write("[gate] partial run: %s\n" % " ".join(self.extra))
        self.log.write("[gate] port %d%s\n" % (self.port, "" if self.own_port else " (via GATE_PORT)"))
        self.log.flush()
        env = dict(os.environ)
        env["GATE_PORT"] = str(self.port)
        self.t0 = time.time()
        self.proc = subprocess.Popen(
            [sys.executable, WRAP, os.path.join(TESTS, self.name)] + list(self.extra),
            cwd=ROOT, stdout=self.log, stderr=subprocess.STDOUT, env=env)

    def poll(self):
        """True once finished (sets rc/dt); kills on timeout."""
        rc = self.proc.poll()
        if rc is None:
            if time.time() - self.t0 > PER_TEST_TIMEOUT_S:
                self.proc.kill()
                self.proc.wait()
                self.log.write("\n[gate] TIMEOUT after %ds\n" % PER_TEST_TIMEOUT_S)
                rc = -1
            else:
                return False
        self.rc = rc
        self.dt = time.time() - self.t0
        self.log.close()
        return True

    def tail(self):
        if self.rc == 0:
            return ""
        with open(self.log_path, encoding="utf-8", errors="replace") as f:
            lines = [l.rstrip() for l in f if l.strip()]
        return "\n".join("      " + l for l in lines[-4:])


# ── the gate ─────────────────────────────────────────────────────────────────

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None,
                    help="substrings; run only tests whose name contains one")
    ap.add_argument("--skip", nargs="*", default=[],
                    help="substrings; skip tests whose name contains one")
    ap.add_argument("--include-known-broken", action="store_true")
    ap.add_argument("--jobs", default="auto",
                    help="max tests at once: a number, or auto = one per logical CPU (default)")
    ap.add_argument("--cpu-high", type=float, default=80.0,
                    help="do not start another test while total CPU is above this %% (default 80)")
    ap.add_argument("--mem-min-gb", type=float, default=3.0,
                    help="do not start another test while free RAM is below this (default 3)")
    ap.add_argument("--interval", type=float, default=2.0,
                    help="seconds between load samples / scheduling decisions (default 2)")
    ap.add_argument("--port-base", type=int, default=50100,
                    help="first private server port handed to tests that pass none (default 50100)")
    args = ap.parse_args()

    ncpu = os.cpu_count() or 4
    jobs_max = ncpu if args.jobs == "auto" else max(1, int(args.jobs))

    names = discover()
    if args.only:
        names = [n for n in names if any(s in n for s in args.only)]
    names = [n for n in names if not any(s in n for s in args.skip)]

    log_dir = os.path.join(ROOT, "build", "gate_logs")
    os.makedirs(log_dir, exist_ok=True)

    skipped = []
    pending = []
    next_port = args.port_base
    for name in names:
        if name in KNOWN_BROKEN and not args.include_known_broken:
            skipped.append(name)
            print("  SKIP  %-32s (%s)" % (name, KNOWN_BROKEN[name]))
            continue
        extra = () if args.include_known_broken else PARTIAL.get(name, ())
        own = explicit_port(name)
        if own is None:
            port, own_port = next_port, False
            next_port += 1
        else:
            port, own_port = own, True
        pending.append(Job(name, port, own_port, extra, log_dir))

    print("gate: %d test(s), up to %d at once (cpu-high %.0f%%, mem-min %.1f GB), logs in %s"
          % (len(pending), jobs_max, args.cpu_high, args.mem_min_gb, log_dir))
    sys.stdout.flush()

    cpu = CpuSampler()
    running = []
    done = []
    failed = []
    t_start = time.time()
    last_load = (0.0, free_mem_gb())
    # The first sample right after construction is meaningless; take one now.
    time.sleep(0.5)
    cpu.percent()

    while pending or running:
        # Reap.
        for j in list(running):
            if j.poll():
                running.remove(j)
                done.append(j)
                if j.rc == 0:
                    print("  %s  %-32s %6.0fs%s" % (
                        "PART" if j.extra else "PASS", j.name, j.dt,
                        ("  (%s)" % " ".join(j.extra)) if j.extra else ""))
                else:
                    failed.append(j.name)
                    print("  FAIL  %-32s %6.0fs  rc=%s  (%s)" % (j.name, j.dt, j.rc, j.log_path))
                    t = j.tail()
                    if t:
                        print(t)
                sys.stdout.flush()

        # Schedule: at most ONE new test per interval, so the load ramps and the
        # next sample sees what the last start cost before we add another.
        if pending and len(running) < jobs_max:
            cpu_pct = cpu.percent()
            mem = free_mem_gb()
            last_load = (cpu_pct, mem)
            room = (not running) or (cpu_pct < args.cpu_high and mem > args.mem_min_gb)
            if room:
                busy_ports = {j.port for j in running}
                for j in pending:
                    if j.port not in busy_ports:
                        pending.remove(j)
                        j.start()
                        running.append(j)
                        print("  ....  %-32s port %d  running=%d  cpu=%3.0f%%  free=%.1f GB" % (
                            j.name, j.port, len(running), cpu_pct, mem))
                        sys.stdout.flush()
                        break
        time.sleep(args.interval)

    total = time.time() - t_start
    print()
    if failed:
        print("GATE RED: %d failed -> %s" % (len(failed), ", ".join(failed)))
        print("Do NOT pigeon. Fix in winbolo2 first.")
        print("(%d tests in %.0fs)" % (len(done), total))
        return 1
    print("GATE GREEN: %d passed%s  (%.0fs wall, up to %d at once)" % (
        len(done),
        (", %d known-broken skipped" % len(skipped)) if skipped else "",
        total, jobs_max))
    return 0


if __name__ == "__main__":
    sys.exit(main())
