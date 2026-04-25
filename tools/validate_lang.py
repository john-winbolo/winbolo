#!/usr/bin/env python3
# validate_lang.py
#
# Validate a translation file (lang/<code>.txt) against the canonical
# English source (lang/en.txt). Catches the failure modes a translator
# is likely to introduce:
#
#   * unknown ID (typo of a STR_/MESSAGE_/etc. name) — FAIL
#   * duplicate ID — FAIL
#   * placeholder-token multiset mismatch — FAIL
#   * missing ID (just-not-translated-yet) — WARN (runtime falls back
#     to English, so this is non-fatal)
#   * %s/%d in the value — WARN (Phase 3 converts the last holdouts;
#     once Phase 3 lands, tighten this to FAIL)
#
# Phase 6 will populate K_LENGTH_BUDGETS with per-ID character caps so
# the validator can flag UI overflows; the loop is wired up now and
# silently passes until the table has entries.
#
# Usage:
#   python3 tools/validate_lang.py lang/de.txt
#   python3 tools/validate_lang.py --en lang/en.txt lang/de.txt
#
# Exit code 0 on clean, 1 on any failure.

import argparse
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
DEFAULT_EN = REPO_ROOT / "lang" / "en.txt"

# Tokens substituted at render time. The validator counts occurrences
# per-ID; the per-string multiset must match (ordering may differ —
# that's the whole point of using named placeholders, see localize.md).
PLACEHOLDER_TOKENS = (
    "{player}", "{other}", "{number}",
    "{ACCEL}", "{BRAKE}", "{FIRE}",
    "{LEFT}", "{RIGHT}", "{MINE}",
    "{SCROLL_UP}", "{SCROLL_DOWN}", "{SCROLL_LEFT}", "{SCROLL_RIGHT}",
)

# TODO Phase 6: populate per-ID budgets, e.g.
#   "STR_DLGOPENING_OPTION1": 32,
# The check loop below is wired up now so Phase 6 only adds data.
K_LENGTH_BUDGETS = {}

PRINTF_RE = re.compile(r"%[-+0# ]*\d*(?:\.\d+)?[diouxXeEfgGsc%]")


def parse_lang_file(path):
    """Return (entries, errors).

    entries is an ordered list of (id_name, raw_value, line_num) tuples.
    errors is a list of (line_num, message) tuples for lines we
    couldn't make sense of (kept separate so the caller can surface
    them as failures).
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    entries = []
    errors = []
    inHeader = True
    for n, raw in enumerate(lines, start=1):
        line = raw
        if n == 1 and line.startswith("﻿"):
            line = line[1:]
        stripped = line.strip()
        if not stripped or stripped.startswith("#"):
            continue
        if "=" not in stripped:
            errors.append((n, f"line has no '=': {stripped!r}"))
            continue
        key, value = stripped.split("=", 1)
        key = key.strip()
        if inHeader and key in ("name", "author", "notes"):
            continue
        # Anything that isn't a known header key starts the body.
        inHeader = False
        entries.append((key, value, n))
    return entries, errors


def token_multiset(value):
    """Return {token: count} for known placeholder tokens in `value`."""
    out = {}
    for tok in PLACEHOLDER_TOKENS:
        c = value.count(tok)
        if c:
            out[tok] = c
    return out


def diff_multisets(a, b):
    """Return a sorted list of (token, a_count, b_count) for tokens
    where the counts differ."""
    keys = sorted(set(a) | set(b))
    return [(k, a.get(k, 0), b.get(k, 0)) for k in keys if a.get(k, 0) != b.get(k, 0)]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--en", default=str(DEFAULT_EN),
                        help=f"Path to canonical English file (default: {DEFAULT_EN.relative_to(REPO_ROOT)}).")
    parser.add_argument("target", help="Path to translation file to validate.")
    args = parser.parse_args()

    en_path = Path(args.en)
    target_path = Path(args.target)

    if not en_path.exists():
        print(f"error: english source not found: {en_path}", file=sys.stderr)
        return 1
    if not target_path.exists():
        print(f"error: target file not found: {target_path}", file=sys.stderr)
        return 1

    en_entries, en_errors = parse_lang_file(en_path)
    tg_entries, tg_errors = parse_lang_file(target_path)

    failures = 0
    warnings = 0

    if en_errors:
        print(f"warning: malformed lines in {en_path.name}:", file=sys.stderr)
        for n, msg in en_errors:
            print(f"  {en_path.name}:{n}: {msg}", file=sys.stderr)
        warnings += len(en_errors)

    if tg_errors:
        print(f"FAIL: malformed lines in {target_path.name}:", file=sys.stderr)
        for n, msg in tg_errors:
            print(f"  {target_path.name}:{n}: {msg}", file=sys.stderr)
        failures += len(tg_errors)

    en_by_id = {}
    for name, val, n in en_entries:
        # The English file may include the same key twice if the source
        # is malformed; that's an English-side bug, not the translator's.
        en_by_id[name] = val

    # Duplicate-ID detection in the target.
    seen = {}
    for name, val, n in tg_entries:
        if name in seen:
            print(f"FAIL: duplicate ID '{name}' in {target_path.name}: "
                  f"first at line {seen[name]}, again at line {n}",
                  file=sys.stderr)
            failures += 1
        else:
            seen[name] = n

    # Unknown IDs in target.
    for name, val, n in tg_entries:
        if name not in en_by_id:
            print(f"FAIL: unknown ID '{name}' at {target_path.name}:{n} "
                  f"(not present in {en_path.name})", file=sys.stderr)
            failures += 1

    # Missing IDs (warn only — runtime falls back to English).
    target_ids = set(seen)
    missing = sorted(set(en_by_id) - target_ids)
    if missing:
        print(f"warning: {len(missing)} ID(s) missing from {target_path.name} "
              f"(English fallback will apply):", file=sys.stderr)
        for name in missing:
            print(f"  {name}", file=sys.stderr)
        warnings += len(missing)

    # Placeholder multiset comparison (per-ID).
    for name, val, n in tg_entries:
        if name not in en_by_id:
            continue  # already failed above
        en_tokens = token_multiset(en_by_id[name])
        tg_tokens = token_multiset(val)
        diff = diff_multisets(en_tokens, tg_tokens)
        if diff:
            print(f"FAIL: placeholder mismatch in '{name}' at "
                  f"{target_path.name}:{n}", file=sys.stderr)
            for tok, ea, ta in diff:
                print(f"    {tok}: en={ea} target={ta}", file=sys.stderr)
            failures += 1

    # Printf-style format-specifier warnings (don't yet fail — Phase 3
    # converts the last %s/%d holdouts to named placeholders).
    for name, val, n in tg_entries:
        hits = PRINTF_RE.findall(val)
        if hits:
            print(f"warning: {target_path.name}:{n} '{name}' contains "
                  f"%-style format specifier(s) {hits}; should be a "
                  f"named placeholder once Phase 3 lands",
                  file=sys.stderr)
            warnings += 1

    # Length budgets (currently empty; Phase 6 populates the table).
    for name, val, n in tg_entries:
        budget = K_LENGTH_BUDGETS.get(name)
        if budget is None:
            continue
        if len(val) > budget:
            print(f"warning: {target_path.name}:{n} '{name}' exceeds "
                  f"length budget {len(val)}>{budget}", file=sys.stderr)
            warnings += 1

    print(f"{target_path.name}: {len(tg_entries)} entries, "
          f"{failures} failure(s), {warnings} warning(s)",
          file=sys.stderr)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
