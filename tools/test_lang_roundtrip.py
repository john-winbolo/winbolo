#!/usr/bin/env python3
# test_lang_roundtrip.py
#
# Verifies that data/lang/en.txt round-trips byte-for-byte against the C
# string table in src/gui/sdl3/lang.c (langTable[]). This catches the
# class of bug where the loader silently strips trailing spaces from
# values — several real labels end in a space ("Map Name: ", etc.) so
# any whitespace tampering shows up here immediately.
#
# Mirrors the C loader's per-line parse rules:
#   * Strip a UTF-8 BOM on the first line.
#   * Strip ONLY \r\n from the end of the raw line; never spaces.
#   * Skip leading horizontal whitespace before checking comment/blank.
#   * Split on the first '='. Trim the key. Leave the value verbatim.
#   * Apply \n / \t / \r / \\ / \" unescapes to the value.
#
# Exit code 0 on clean, 1 on any mismatch.

import sys
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(REPO_ROOT / "tools"))

from dump_lang_en import parse_lang_c, LANG_C_PATH  # noqa: E402

EN_TXT_PATH = REPO_ROOT / "data" / "lang" / "en.txt"


def unescape_value(raw):
    """Mirror of unescapeValue() in src/gui/sdl3/lang.c."""
    out = []
    i = 0
    n = len(raw)
    while i < n:
        c = raw[i]
        if c == "\\" and i + 1 < n:
            nxt = raw[i + 1]
            if nxt == "n":
                out.append("\n")
                i += 2
                continue
            if nxt == "t":
                out.append("\t")
                i += 2
                continue
            if nxt == "r":
                out.append("\r")
                i += 2
                continue
            if nxt == "\\":
                out.append("\\")
                i += 2
                continue
            if nxt == "\"":
                out.append("\"")
                i += 2
                continue
        out.append(c)
        i += 1
    return "".join(out)


def parse_en_txt_like_c_loader(path):
    """Parse data/lang/en.txt the same way the C loader does.

    Returns (entries_by_name, errors). Header lines (name/author/notes)
    are returned in `meta`. Body lines use the symbolic ID name as the
    key and the unescaped value as the value.
    """
    text = path.read_text(encoding="utf-8")
    # Strip a single BOM at the start of the file (the C loader strips
    # it from the first line — equivalent for files written by Python's
    # write_text, which never inject one mid-file).
    if text.startswith("﻿"):
        text = text[1:]

    entries = {}
    meta = {}
    errors = []
    in_header = True
    for n, raw_line in enumerate(text.splitlines(), start=1):
        # splitlines() already strips \r\n, matching the C loader's
        # post-strip state. No spaces are stripped — values keep them.
        # Skip leading horizontal whitespace for the comment/blank check.
        i = 0
        while i < len(raw_line) and raw_line[i] in (" ", "\t"):
            i += 1
        if i == len(raw_line) or raw_line[i] == "#":
            continue

        eq = raw_line.find("=")
        if eq < 0:
            errors.append((n, f"no '=' in line: {raw_line!r}"))
            continue

        key = raw_line[:eq].strip()
        value = raw_line[eq + 1:]  # NOT stripped — verbatim

        if in_header and key in ("name", "author", "notes"):
            meta[key] = value
            continue
        in_header = False

        if key in entries:
            errors.append((n, f"duplicate key {key!r}"))
        entries[key] = unescape_value(value)

    return entries, meta, errors


def main():
    en_entries, en_meta, en_errors = parse_en_txt_like_c_loader(EN_TXT_PATH)

    if en_errors:
        for n, msg in en_errors:
            print(f"FAIL: {EN_TXT_PATH.name}:{n}: {msg}", file=sys.stderr)
        return 1

    c_id_to_text = parse_lang_c(LANG_C_PATH)

    # Map symbolic name -> C string by reading the lang.h enum.
    # Re-using dump_lang_en's parser keeps the test honest.
    from dump_lang_en import parse_lang_h, LANG_H_PATH
    id_to_name, _sections = parse_lang_h(LANG_H_PATH)
    name_to_id = {name: num for num, name in id_to_name.items()}

    matched = 0
    mismatches = []
    only_in_en = []
    only_in_c = []

    for name, en_value in en_entries.items():
        if name not in name_to_id:
            only_in_en.append(name)
            continue
        num = name_to_id[name]
        if num not in c_id_to_text:
            only_in_en.append(name)
            continue
        c_value = c_id_to_text[num]
        if c_value != en_value:
            mismatches.append((name, num, en_value, c_value))
        else:
            matched += 1

    for num, c_value in c_id_to_text.items():
        name = id_to_name.get(num)
        if name and name not in en_entries:
            only_in_c.append((num, name))

    if mismatches:
        print(f"FAIL: {len(mismatches)} value(s) differ between en.txt and lang.c:",
              file=sys.stderr)
        for name, num, en_v, c_v in mismatches:
            print(f"  {name} (id {num}):", file=sys.stderr)
            print(f"    en.txt = {en_v!r}", file=sys.stderr)
            print(f"    lang.c = {c_v!r}", file=sys.stderr)

    if only_in_en:
        print(f"warning: {len(only_in_en)} ID(s) in en.txt without a "
              f"matching lang.c entry:", file=sys.stderr)
        for name in only_in_en:
            print(f"  {name}", file=sys.stderr)

    if only_in_c:
        # These would be langTable[] entries with no #define in lang.h
        # (or that en.txt simply doesn't list). Reported but not failing
        # — the dump generator already warns about orphans.
        print(f"warning: {len(only_in_c)} lang.c entry(ies) have no "
              f"counterpart in en.txt:", file=sys.stderr)
        for num, name in only_in_c:
            print(f"  {num} {name}", file=sys.stderr)

    print(f"round-trip: {matched} match(es), {len(mismatches)} mismatch(es)",
          file=sys.stderr)
    return 1 if mismatches else 0


if __name__ == "__main__":
    sys.exit(main())
