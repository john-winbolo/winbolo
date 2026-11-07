#!/usr/bin/env python3
"""The dedicated server's host bot capacity, run for real.

Starts WinBoloDS with a lobby (no tracker, no WinBolo.net, 127.0.0.1 and a
test port) three times, each with WINBOLO_CPU_SPEED_FILE in a scratch
directory, and stops it with the console command "quit":

  1. -botcapacity 3       logs "Bot capacity: override 3" and runs no test,
                          so no store file is written.
  2. a fresh store        the lobby runs the 2 s CPU speed test, logs the
                          capacity "measured now", and the store file exists.
  3. the same store       a second start reads the stored result and logs
                          "stored result", with no new test.

Usage: ds_bot_capacity_test.py [build_dir]   (default: build-own)
The build dir is the DS's working directory, so its Brains/ is the one used.
"""

import os
import queue
import shutil
import subprocess
import sys
import tempfile
import threading
import time
from pathlib import Path

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent
PORT = 27531
WAIT_S = 60


def find_ds(build_dir):
    for name in ("WinBoloDS.exe", "WinBoloDS", "LinBoloDS"):
        for sub in ("", "Release"):
            p = build_dir / sub / name
            if p.exists():
                return p
    return None


def run_ds(ds, build_dir, store, extra, want, label):
    """Starts the DS, waits for a line with every text in want, then sends
    quit. Returns (ok, the whole output)."""
    env = dict(os.environ, WINBOLO_CPU_SPEED_FILE=str(store))
    cmd = [str(ds), "-inbuilt", "-port", str(PORT),
           "-addr", "127.0.0.1", "-gametype", "open", "-ai", "yes",
           "-notracker", "-nowinbolonet"] + extra
    proc = subprocess.Popen(cmd, cwd=str(build_dir), env=env,
                            stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT)
    lines = queue.Queue()

    def reader():
        for raw in proc.stdout:
            lines.put(raw.decode("utf-8", errors="replace").rstrip("\r\n"))
        lines.put(None)

    threading.Thread(target=reader, daemon=True).start()
    out = []
    found = None
    deadline = time.time() + WAIT_S
    while time.time() < deadline:
        try:
            line = lines.get(timeout=0.5)
        except queue.Empty:
            continue
        if line is None:
            break
        out.append(line)
        if line.startswith("Bot capacity:"):
            found = line
            break
    ok = found is not None and all(w in found for w in want)
    try:
        proc.stdin.write(b"quit\n")
        proc.stdin.flush()
    except OSError:
        pass
    try:
        proc.wait(timeout=30)
    except subprocess.TimeoutExpired:
        print(f"FAIL [{label}]: the DS did not stop after quit")
        ok = False
    while True:
        try:
            line = lines.get(timeout=2)
        except queue.Empty:
            break
        if line is None:
            break
        out.append(line)
    print(f"[{label}] {found if found else '(no Bot capacity line)'}")
    if not ok:
        print(f"FAIL [{label}]: wanted a 'Bot capacity:' line with {want}")
        print("\n".join(out[-40:]))
    return ok, "\n".join(out)


def main():
    build_dir = Path(sys.argv[1]) if len(sys.argv) > 1 else ROOT / "build-own"
    build_dir = build_dir.resolve()
    ds = find_ds(build_dir)
    if ds is None:
        print(f"FAIL: WinBoloDS not found under {build_dir}")
        return 1
    scratch = Path(tempfile.mkdtemp(prefix="wb_botcap_"))
    store = scratch / "cpu_speed.json"
    failures = 0
    try:
        ok, _ = run_ds(ds, build_dir, store, ["-botcapacity", "3"],
                       ["override 3"], "override")
        failures += not ok
        if store.exists():
            print("FAIL [override]: -botcapacity still wrote a store file")
            failures += 1

        ok, text = run_ds(ds, build_dir, store, [], ["measured now"],
                          "fresh store")
        failures += not ok
        if not store.exists():
            print("FAIL [fresh store]: no store file after the test")
            failures += 1

        # "stored result" is only logged by the branch that read the store
        # and started no test.
        ok, _ = run_ds(ds, build_dir, store, [], ["stored result"], "stored")
        failures += not ok
    finally:
        shutil.rmtree(scratch, ignore_errors=True)
    print("PASS" if failures == 0 else f"FAIL: {failures}")
    return 0 if failures == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
