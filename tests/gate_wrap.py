#!/usr/bin/env python3
"""Gate wrapper: run one tests/*_test.py with its WinBoloDS on a private port.

run_scenario_gate.py runs tests in parallel, and most tests launch
WinBoloDS.exe without a -port, so they would all bind the dedicated
server's default port and collide. Rather than editing every test, the
gate launches each one through this wrapper: it patches subprocess.Popen
(which call/run/check_output all go through) so that any WinBoloDS
command line that carries no -port gets "-port <GATE_PORT>" appended,
then runs the test in this process with runpy. A test that sets its own
-port is left alone (the gate serialises those on their port).

Usage (the gate does this; you don't need to):
  python gate_wrap.py <test.py> [args...]      with GATE_PORT in the env
"""
import os
import runpy
import subprocess
import sys

_PORT = os.environ.get("GATE_PORT")


def _is_ds(exe):
    name = os.path.basename(str(exe)).lower()
    return name in ("winbolods.exe", "winbolods")


def _inject(args):
    if not _PORT or not isinstance(args, (list, tuple)) or not args:
        return args
    if not _is_ds(args[0]):
        return args
    if "-port" in [str(a) for a in args]:
        return args
    return list(args) + ["-port", _PORT]


_real_popen_init = subprocess.Popen.__init__


def _popen_init(self, args, *a, **kw):
    _real_popen_init(self, _inject(args), *a, **kw)


subprocess.Popen.__init__ = _popen_init


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: gate_wrap.py <test.py> [args...]\n")
        return 2
    target = sys.argv[1]
    sys.argv = [target] + sys.argv[2:]
    sys.path.insert(0, os.path.dirname(os.path.abspath(target)))
    runpy.run_path(target, run_name="__main__")
    return 0


if __name__ == "__main__":
    sys.exit(main())
