#!/usr/bin/env python3
# Copyright (c) 1998-2026 John Morrison.
# SPDX-License-Identifier: GPL-3.0-or-later
"""Run scenario arenas from any checkout and keep each arena's round log.

The scenario gate (tests/scenario/run_gate.py) runs the server with no -log,
so an arena leaves no .wbv to draw with WbvRender. A checkout whose run_gate.py
has --replay-dir needs nothing more. This runs the run_gate.py of a checkout
that has not got it, without changing that checkout: it loads the runner,
adds "-log <dir>/<arena>" to each server's command, and runs it.

    python -I tools/arena_replay.py --repo <checkout> --replay-dir <dir> \\
        -- --build <build dir> --jobs 1 --only <arena> [<arena> ...]

Everything after "--" goes to run_gate.py as it is. The arenas, the map files
and the brains are the checkout's own; the server is the one in --build.
Each arena's log lands in <dir>/<arena>/<timestamp>_<map>.wbv.
"""

import argparse
import importlib.util
import os
import sys


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--repo", required=True,
                    help="the checkout whose tests/scenario/run_gate.py runs")
    ap.add_argument("--replay-dir", required=True,
                    help="each arena's .wbv goes in <dir>/<arena>/")
    ap.add_argument("rest", nargs=argparse.REMAINDER,
                    help="-- then the run_gate.py arguments")
    args = ap.parse_args()
    rest = args.rest[1:] if args.rest[:1] == ["--"] else args.rest

    gate = os.path.join(os.path.abspath(args.repo), "tests", "scenario",
                        "run_gate.py")
    if not os.path.isfile(gate):
        sys.exit("no run_gate.py at %s" % gate)
    replay_dir = os.path.abspath(args.replay_dir)

    spec = importlib.util.spec_from_file_location("run_gate", gate)
    rg = importlib.util.module_from_spec(spec)
    sys.modules["run_gate"] = rg
    spec.loader.exec_module(rg)

    plain = rg.Job.command

    def command(self, ds):
        cmd = plain(self, ds)
        if "-log" not in cmd:
            # A folder, never a file: -log <file> writes over one file.
            d = os.path.join(replay_dir, self.name)
            os.makedirs(d, exist_ok=True)
            cmd += ["-log", d]
        return cmd

    rg.Job.command = command
    sys.argv = [gate] + rest
    return rg.main()


if __name__ == "__main__":
    sys.exit(main())
