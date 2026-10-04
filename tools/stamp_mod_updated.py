#!/usr/bin/env python3
# stamp_mod_updated.py
#
# Keeps the `updated` field of every shipped scenario script current. The
# field is the time the script's content last changed, written into the
# script's own scenario table as an ISO 8601 UTC time to the minute:
#
#   scenario = {
#     name    = "Mac Bolo Rules",
#     author  = "WinBolo",
#     updated = "2026-10-03T20:47Z",
#     ...
#   }
#
# It is not the file's modified time on disk, which a copy or a download
# moves. Lobbies and the DS log show it beside the author so that two
# editions of a mod with one name can be told apart (docs/SCENARIO_API.md,
# "author and updated").
#
# The shipped scripts are every *.lua under data/mods/ and every
# *.scenario.lua under data/maps/ that git tracks, plus any new one there
# that git does not track yet. tests/scenario fixtures are not shipped and
# are never touched.
#
# Usage (from anywhere; the paths are the repo's):
#   python tools/stamp_mod_updated.py           stamp what changed
#   python tools/stamp_mod_updated.py --check   verify; exit 1 on a failure
#   python tools/stamp_mod_updated.py --init    add missing fields
#   ... [FILE ...]                              only these shipped files
#
# Stamp (no flag): every shipped file whose content differs from git HEAD,
# staged or not, and every untracked one, gets updated = now (UTC). A file
# with no updated line gets one, after its api line, and an author line
# (--author, default "WinBolo") when it has none either. Run it before
# committing a change to a shipped script.
#
# --init: every shipped file that lacks the fields gets them, with updated
# taken from the last commit that touched the file (git log -1 %cI, turned
# to UTC). Files that have them are left alone.
#
# --check, the rule, chosen so that it needs no clock and no tolerance:
#
#   1. Each shipped file states an author and an updated time in the one
#      form, and the time is not more than a day in the future.
#   2. A file whose content differs from HEAD must change its updated line
#      in that same difference (git diff HEAD -U0).
#   3. Otherwise, the last commit that touched the file (merges left out)
#      must have changed its updated line too (git show -U0).
#
# So a commit that changes a shipped script without restamping it fails the
# check from that commit on, until a later commit restamps it. A commit that
# touches the file only to restamp it passes. A shallow clone whose history
# stops at the last commit shows the file as added whole, which passes 3.
# Outside a git checkout the check says so and passes: it has nothing to
# compare against.
#
# Not wired into CTest, as tools/dump_lang_en.py --check is not; run it by
# hand or from a pre-commit hook.

import argparse
import datetime as dt
import re
import subprocess
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent

UPDATED_RE = re.compile(r'^(\s*updated\s*=\s*)"([^"]*)"', re.M)
AUTHOR_RE = re.compile(r'^\s*author\s*=\s*"([^"]*)"', re.M)
API_RE = re.compile(r'^(\s*)api(\s*)=', re.M)
TABLE_RE = re.compile(r'^scenario\s*=\s*\{', re.M)
FORM_RE = re.compile(r'^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}Z$')
DIFF_UPDATED_RE = re.compile(r'^[+-]\s*updated\s*=', re.M)


def git(*args, check=True):
    """Runs git in the repo and answers its stdout as text."""
    out = subprocess.run(["git", *args], cwd=REPO_ROOT, capture_output=True,
                         text=True, encoding="utf-8", errors="replace")
    if check and out.returncode != 0:
        raise RuntimeError("git %s: %s" % (" ".join(args), out.stderr.strip()))
    return out.stdout


def in_git_checkout():
    try:
        return git("rev-parse", "--is-inside-work-tree").strip() == "true"
    except (RuntimeError, OSError):
        return False


def shipped_files():
    """Every shipped script, as repo-relative POSIX paths, sorted."""
    found = set()
    for pattern in ("data/mods/*.lua", "data/maps/*.scenario.lua"):
        for p in REPO_ROOT.glob(pattern):
            found.add(p.relative_to(REPO_ROOT).as_posix())
    return sorted(found)


def now_utc():
    return dt.datetime.now(dt.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def to_utc_minute(iso):
    """git's %cI (2026-10-03T01:08:08+10:00) as 2026-10-02T15:08Z."""
    t = dt.datetime.fromisoformat(iso.strip())
    return t.astimezone(dt.timezone.utc).strftime("%Y-%m-%dT%H:%MZ")


def valid_updated(s):
    if not FORM_RE.match(s):
        return False
    try:
        dt.datetime.strptime(s, "%Y-%m-%dT%H:%MZ")
    except ValueError:
        return False
    return True


def table_start(text):
    m = TABLE_RE.search(text)
    return m.start() if m else -1


def read(rel):
    # newline="" keeps the file's own line endings on the way back out.
    with open(REPO_ROOT / rel, encoding="utf-8", newline="") as f:
        return f.read()


def write(rel, text):
    with open(REPO_ROOT / rel, "w", encoding="utf-8", newline="") as f:
        f.write(text)


def set_fields(text, updated, author):
    """text with its scenario table's updated set to updated, adding the
    updated line, and an author line when there is none, after the api
    line. None when the file has no scenario table or no api line to place
    them after."""
    start = table_start(text)
    if start < 0:
        return None
    m = UPDATED_RE.search(text, start)
    if m:
        return text[:m.start(2)] + updated + text[m.end(2):]
    api = API_RE.search(text, start)
    if api is None:
        return None
    eol = text.find("\n", api.end())
    if eol < 0:
        return None
    nl = "\r\n" if text[eol - 1:eol] == "\r" else "\n"
    indent = api.group(1)
    # The keys line up the way the table's own do: padded to the width the
    # api key was padded to, or one space after each where it was not.
    if len(api.group(2)) > 1:
        width = max(len("api") + len(api.group(2)), len("updated") + 1)
        pad = lambda key: key.ljust(width)
    else:
        pad = lambda key: key + " "
    lines = ""
    if AUTHOR_RE.search(text, start) is None:
        lines += '%s%s= "%s",%s' % (indent, pad("author"), author, nl)
    lines += '%s%s= "%s",%s' % (indent, pad("updated"), updated, nl)
    return text[:eol + 1] + lines + text[eol + 1:]


def changed_vs_head(rel):
    """True for a file that differs from HEAD or that git does not track."""
    if git("ls-files", "--", rel).strip() == "":
        return True
    return subprocess.run(["git", "diff", "--quiet", "HEAD", "--", rel],
                          cwd=REPO_ROOT).returncode != 0


def stamp(files, author):
    when = now_utc()
    n = 0
    for rel in files:
        if not changed_vs_head(rel):
            continue
        text = read(rel)
        out = set_fields(text, when, author)
        if out is None:
            print("%s: no scenario table with an api line; not stamped" % rel)
            continue
        if out != text:
            write(rel, out)
            print("%s: updated = %s" % (rel, when))
            n += 1
    print("%d file%s stamped" % (n, "" if n == 1 else "s"))
    return 0


def init(files, author):
    n = 0
    for rel in files:
        text = read(rel)
        start = table_start(text)
        if start >= 0 and UPDATED_RE.search(text, start):
            continue
        iso = git("log", "-1", "--format=%cI", "--", rel).strip()
        when = to_utc_minute(iso) if iso else now_utc()
        out = set_fields(text, when, author)
        if out is None:
            print("%s: no scenario table with an api line; skipped" % rel)
            continue
        write(rel, out)
        print("%s: updated = %s" % (rel, when))
        n += 1
    print("%d file%s given the fields" % (n, "" if n == 1 else "s"))
    return 0


def check(files):
    if not in_git_checkout():
        print("stamp_mod_updated: not a git checkout; nothing to check against")
        return 0
    soon = (dt.datetime.now(dt.timezone.utc) +
            dt.timedelta(days=1)).strftime("%Y-%m-%dT%H:%MZ")
    bad = 0
    for rel in files:
        text = read(rel)
        start = max(table_start(text), 0)
        a = AUTHOR_RE.search(text, start)
        u = UPDATED_RE.search(text, start)
        if a is None or a.group(1).strip() == "":
            print("%s: FAIL: no author" % rel)
            bad += 1
        if u is None or not valid_updated(u.group(2)):
            print("%s: FAIL: no updated time in the form YYYY-MM-DDTHH:MMZ"
                  % rel)
            bad += 1
            continue
        if u.group(2) > soon:
            print("%s: FAIL: updated %s is in the future" % (rel, u.group(2)))
            bad += 1
        if git("ls-files", "--", rel).strip() == "":
            continue  # new: rule 1 is all there is to hold it to
        diff = git("diff", "-U0", "HEAD", "--", rel)
        if diff:
            if not DIFF_UPDATED_RE.search(diff):
                print("%s: FAIL: changed since HEAD without a new updated "
                      "time; run tools/stamp_mod_updated.py" % rel)
                bad += 1
            continue
        last = git("log", "-1", "--no-merges", "--format=%H", "--",
                   rel).strip()
        if not last:
            continue
        shown = git("show", "--format=", "-U0", last, "--", rel)
        if not DIFF_UPDATED_RE.search(shown):
            print("%s: FAIL: commit %s changed it without a new updated time"
                  % (rel, last[:12]))
            bad += 1
    print("%d shipped script%s checked, %d failure%s"
          % (len(files), "" if len(files) == 1 else "s", bad,
             "" if bad == 1 else "s"))
    return 1 if bad else 0


def main():
    ap = argparse.ArgumentParser(
        description="Keep the updated field of the shipped scenario scripts "
                    "current (see the header of this file for the rule).")
    mode = ap.add_mutually_exclusive_group()
    mode.add_argument("--check", action="store_true",
                      help="verify the rule; exit 1 on a failure")
    mode.add_argument("--init", action="store_true",
                      help="add missing fields from git history")
    ap.add_argument("--author", default="WinBolo",
                    help="author written where a file has none")
    ap.add_argument("files", nargs="*",
                    help="shipped files to limit the run to")
    args = ap.parse_args()

    files = shipped_files()
    if args.files:
        wanted = set()
        for f in args.files:
            try:
                wanted.add(Path(f).resolve().relative_to(REPO_ROOT).as_posix())
            except ValueError:
                print("%s: not a shipped script; skipped" % f)
        files = [f for f in files if f in wanted]
        missing = wanted - set(files)
        for f in sorted(missing):
            print("%s: not a shipped script; skipped" % f)

    if args.check:
        return check(files)
    if not in_git_checkout():
        print("stamp_mod_updated: not a git checkout")
        return 1
    if args.init:
        return init(files, args.author)
    return stamp(files, args.author)


if __name__ == "__main__":
    sys.exit(main())
