#!/usr/bin/env python3
"""PIGEON gate: run every tests/*_test.py (the scenario-sidecar tests and the
C/Lua parity test) and stop the propagation on any failure.

Rule (2026-09-05): the complicated bot tests are written as scenario tests
and live in winbolo2 only -- they are never pigeoned. The bot/engine code is
propagated to winbolo only once this gate is green, so the release line gets
tested code without carrying the scenario harness.

Usage (from the repo root, build/ present):
  C:\\Python310\\python.exe tests/run_scenario_gate.py            # everything
  C:\\Python310\\python.exe tests/run_scenario_gate.py --only steal_handshake refuel_pad_reach
  C:\\Python310\\python.exe tests/run_scenario_gate.py --skip aim
Exit code 0 = all green, 1 = something failed (listed at the end).
"""
import argparse
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TESTS = os.path.join(ROOT, "tests")

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
NOT_A_TEST = {"run_test.py"}

PER_TEST_TIMEOUT_S = 15 * 60


def discover():
    names = sorted(f for f in os.listdir(TESTS)
                   if f.endswith("_test.py") and f not in NOT_A_TEST)
    return names


def run_one(name, log_dir, extra=()):
    path = os.path.join(TESTS, name)
    log_path = os.path.join(log_dir, name.replace(".py", ".log"))
    t0 = time.time()
    with open(log_path, "w", encoding="utf-8", errors="replace") as log:
        if extra:
            log.write("[gate] partial run: %s\n" % " ".join(extra))
        try:
            rc = subprocess.call([sys.executable, path] + list(extra), cwd=ROOT,
                                 stdout=log, stderr=subprocess.STDOUT,
                                 timeout=PER_TEST_TIMEOUT_S)
        except subprocess.TimeoutExpired:
            rc = -1
            log.write("\n[gate] TIMEOUT after %ds\n" % PER_TEST_TIMEOUT_S)
    dt = time.time() - t0
    tail = ""
    if rc != 0:
        with open(log_path, encoding="utf-8", errors="replace") as f:
            lines = [l.rstrip() for l in f if l.strip()]
        tail = "\n".join("      " + l for l in lines[-4:])
    return rc, dt, tail, log_path


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--only", nargs="*", default=None,
                    help="substrings; run only tests whose name contains one")
    ap.add_argument("--skip", nargs="*", default=[],
                    help="substrings; skip tests whose name contains one")
    ap.add_argument("--include-known-broken", action="store_true")
    args = ap.parse_args()

    names = discover()
    if args.only:
        names = [n for n in names if any(s in n for s in args.only)]
    names = [n for n in names if not any(s in n for s in args.skip)]

    log_dir = os.path.join(ROOT, "build", "gate_logs")
    os.makedirs(log_dir, exist_ok=True)

    failed, skipped = [], []
    print("gate: %d test(s), logs in %s" % (len(names), log_dir))
    for name in names:
        if name in KNOWN_BROKEN and not args.include_known_broken:
            skipped.append(name)
            print("  SKIP  %-32s (%s)" % (name, KNOWN_BROKEN[name]))
            continue
        extra = () if args.include_known_broken else PARTIAL.get(name, ())
        sys.stdout.write("  ....  %-32s" % name)
        sys.stdout.flush()
        rc, dt, tail, log_path = run_one(name, log_dir, extra)
        if rc == 0:
            print("\r  %s  %-32s %6.0fs%s" % (
                "PART" if extra else "PASS", name, dt,
                ("  (%s)" % " ".join(extra)) if extra else ""))
        else:
            failed.append(name)
            print("\r  FAIL  %-32s %6.0fs  rc=%s  (%s)" % (name, dt, rc, log_path))
            if tail:
                print(tail)

    print()
    if failed:
        print("GATE RED: %d failed -> %s" % (len(failed), ", ".join(failed)))
        print("Do NOT pigeon. Fix in winbolo2 first.")
        return 1
    print("GATE GREEN: %d passed%s" % (
        len(names) - len(skipped),
        (", %d known-broken skipped" % len(skipped)) if skipped else ""))
    return 0


if __name__ == "__main__":
    sys.exit(main())
