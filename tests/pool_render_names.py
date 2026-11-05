#!/usr/bin/env python3
"""Check pool_render.lua's C_NAMES list against the C.NAME reads in the file.

goals.capture_pool_raw copies only the constants named in
pool_render.C_NAMES into each capture. A C.NAME read in the render that is
not in the list reads nil from a recorded block, so the recorded text would
differ from the old text. This check finds such a name before a game does.

Run from the repository root:  python tests/pool_render_names.py
Exit code 0 = the list and the reads agree.
"""
import re
import sys

PATH = "brains/GoalHunter/pool_render.lua"


def main():
    with open(PATH, encoding="utf-8") as f:
        text = f.read()
    start = text.find("M.C_NAMES = {")
    if start < 0:
        print("FAIL: no M.C_NAMES list in " + PATH)
        return 1
    end = text.find("}", start)
    listed = set(re.findall(r'"([A-Z0-9_]+)"', text[start:end]))
    body = text[:start]
    # Drop comments so a name in a comment does not count as a read.
    body = re.sub(r"--\[\[.*?\]\]", "", body, flags=re.S)
    body = re.sub(r"--[^\n]*", "", body)
    read = set(re.findall(r"(?<![\w.])C\.([A-Z][A-Z0-9_]*)", body))
    read |= set(re.findall(r'(?<![\w.])C\["([A-Z][A-Z0-9_]*)"\]', body))
    missing = sorted(read - listed)
    unused = sorted(listed - read)
    if missing:
        print("FAIL: read in the render but not in C_NAMES: " + ", ".join(missing))
    if unused:
        print("NOTE: in C_NAMES but not read: " + ", ".join(unused))
    if not missing:
        print("PASS: %d names, every C.NAME read is in C_NAMES" % len(listed))
    return 1 if missing else 0


if __name__ == "__main__":
    sys.exit(main())
