#!/usr/bin/env python3
# dump_lang_en.py
#
# Generates the canonical English translation file (data/lang/en.txt) and a
# parallel C lookup table (src/gui/sdl3/lang_names.inc) from the two
# sources of truth in the repo:
#
#   * src/gui/lang.h         — symbolic ID -> integer enum mapping
#   * src/gui/sdl3/lang.c    — integer enum -> English string mapping
#
# Both outputs are deterministic and idempotent: re-running with no
# source changes produces byte-identical files. The script does not
# write anywhere outside data/lang/en.txt and src/gui/sdl3/lang_names.inc.
#
# Usage:
#   python3 tools/dump_lang_en.py                          (run from repo root)
#   python3 tools/dump_lang_en.py --check                  (exit 1 if outputs would change)
#
# This is Phase 0 plumbing for the localization plan in plans/localize.md.
# Translators edit data/lang/<code>.txt files; they never touch the C table.

import argparse
import os
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
LANG_H_PATH = REPO_ROOT / "src" / "gui" / "lang.h"
LANG_C_PATH = REPO_ROOT / "src" / "gui" / "sdl3" / "lang.c"
EN_TXT_PATH = REPO_ROOT / "data" / "lang" / "en.txt"
NAMES_INC_PATH = REPO_ROOT / "src" / "gui" / "sdl3" / "lang_names.inc"

# Symbolic IDs that participate in localization. lang.h also carries
# Win32-style IDD_*, IDR_*, IDB_*, IDC_*, IDI_*, ID_* resource IDs which
# are non-localizable and intentionally excluded.
ID_PREFIXES = (
    "STR_",
    "STRERR_",
    "LGM_",
    "MESSAGE_",
    "NETERR_",
    "NET_STATUS_",
)


def parse_lang_h(path):
    """Return (id_to_name, sections).

    id_to_name maps the integer enum value -> symbolic name.
    sections is a list of (comment_text, [name, name, ...]) in source
    order, used to preserve the section grouping in data/lang/en.txt.
    """
    text = path.read_text(encoding="utf-8")
    lines = text.splitlines()

    define_re = re.compile(r"^\s*#define\s+([A-Z][A-Z0-9_]*)\s+(\d+)\s*$")
    comment_re = re.compile(r"^\s*/\*\s*(.+?)\s*\*/\s*$")

    id_to_name = {}
    sections = []  # list of (label, [names_in_order])
    current_label = None
    current_names = []

    def flush():
        if current_names:
            sections.append((current_label or "Strings", list(current_names)))

    for line in lines:
        m = comment_re.match(line)
        if m:
            label = m.group(1).strip()
            # Skip the boilerplate file header.
            if label.startswith("Copyright") or label.startswith("---"):
                continue
            if label.startswith("Name:") or label.startswith("Filename:"):
                continue
            # Treat short "/* foo */" lines as section labels.
            if len(label) <= 80 and not label.startswith("*"):
                flush()
                current_label = label
                current_names = []
            continue

        m = define_re.match(line)
        if not m:
            continue
        name, num = m.group(1), int(m.group(2))
        if not name.startswith(ID_PREFIXES):
            continue
        if num in id_to_name and id_to_name[num] != name:
            print(f"warning: duplicate id {num}: {id_to_name[num]} and {name}",
                  file=sys.stderr)
        id_to_name[num] = name
        current_names.append(name)

    flush()
    return id_to_name, sections


def parse_lang_c(path):
    """Return id_to_text mapping the integer id to its English text.

    Parses the langTable[] entries: { id, "text" },
    handling C string-literal escape sequences within the value.
    """
    text = path.read_text(encoding="utf-8")
    # Match: {<id>, "<string with escapes>"},
    # The string literal may contain \" and \\.
    entry_re = re.compile(
        r"\{\s*(\d+)\s*,\s*\"((?:[^\"\\]|\\.)*)\"\s*\}",
        re.MULTILINE,
    )

    id_to_text = {}
    for m in entry_re.finditer(text):
        num = int(m.group(1))
        raw = m.group(2)
        if num in id_to_text:
            print(f"warning: duplicate id {num} in lang.c", file=sys.stderr)
        id_to_text[num] = decode_c_string(raw)
    return id_to_text


def decode_c_string(s):
    """Convert a C string literal body (without the surrounding quotes)
    into the actual Python string. Handles \\n, \\t, \\\\, \\\", \\xHH.
    """
    out = []
    i = 0
    while i < len(s):
        c = s[i]
        if c == "\\" and i + 1 < len(s):
            nxt = s[i + 1]
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
            if nxt == "'":
                out.append("'")
                i += 2
                continue
            if nxt == "x":
                # \xHH — single byte. Consecutive \xHH\xHH... are a
                # UTF-8 sequence; collect all the bytes then decode.
                byte_str = bytearray()
                while i + 3 < len(s) and s[i] == "\\" and s[i + 1] == "x":
                    hex2 = s[i + 2:i + 4]
                    if len(hex2) != 2 or not all(ch in "0123456789abcdefABCDEF" for ch in hex2):
                        break
                    byte_str.append(int(hex2, 16))
                    i += 4
                out.append(byte_str.decode("utf-8", errors="replace"))
                continue
            # Unknown escape — keep as-is (warn-worthy but tolerated).
            out.append(c)
            out.append(nxt)
            i += 2
            continue
        out.append(c)
        i += 1
    return "".join(out)


def encode_lang_value(s):
    """Inverse of decode_c_string for the data/lang/<code>.txt format.

    Only \\, \n, \t are escaped; everything else (including UTF-8) is
    written verbatim. The parser splits on the first '=' so '=' itself
    needs no escaping.
    """
    out = []
    for c in s:
        if c == "\\":
            out.append("\\\\")
        elif c == "\n":
            out.append("\\n")
        elif c == "\t":
            out.append("\\t")
        else:
            out.append(c)
    return "".join(out)


def render_en_txt(id_to_name, sections, id_to_text):
    """Build the data/lang/en.txt content as a string."""
    out = []
    out.append("# WinBolo English language file")
    out.append("# Generated by tools/dump_lang_en.py — do not hand-edit.")
    out.append("# Re-run after editing src/gui/lang.h or src/gui/sdl3/lang.c.")
    out.append("")
    out.append("name=English")
    out.append("author=WinBolo")
    out.append("notes=Generated from src/gui/sdl3/lang.c — do not hand-edit.")
    out.append("")

    seen_ids = set()
    for label, names in sections:
        # Filter to names that actually have a string in lang.c.
        rows = []
        for name in names:
            # Find the id for this name.
            num = None
            for n_id, n_name in id_to_name.items():
                if n_name == name:
                    num = n_id
                    break
            if num is None or num in seen_ids:
                continue
            if num not in id_to_text:
                continue
            rows.append((name, id_to_text[num]))
            seen_ids.add(num)
        if not rows:
            continue
        out.append(f"# {label}")
        for name, value in rows:
            out.append(f"{name}={encode_lang_value(value)}")
        out.append("")

    # Catch any ids that were in lang.c but not assigned to a section.
    # (Anomaly: id present in C table with no symbolic name.)
    leftover = sorted(set(id_to_text) - seen_ids)
    if leftover:
        out.append("# Unsectioned / orphan strings")
        for num in leftover:
            name = id_to_name.get(num)
            if name is None:
                # Skip with a comment — translator can't address this string
                # because it has no symbolic ID.
                value = id_to_text[num]
                preview = encode_lang_value(value)[:60]
                out.append(f"# (no symbolic ID for {num}: {preview!r})")
                continue
            out.append(f"{name}={encode_lang_value(id_to_text[num])}")
        out.append("")

    # Trim a trailing empty line so we end with exactly one newline.
    while len(out) >= 2 and out[-1] == "" and out[-2] == "":
        out.pop()
    return "\n".join(out) + "\n"


def render_names_inc(id_to_name, id_to_text):
    """Build the lang_names.inc content as a string.

    Sorted alphabetically by symbolic name so the loader can use bsearch.
    """
    pairs = []
    for num, name in id_to_name.items():
        if num in id_to_text:
            pairs.append((name, num))
    pairs.sort(key=lambda p: p[0])

    out = []
    out.append("/* lang_names.inc — generated by tools/dump_lang_en.py.")
    out.append(" * Do not hand-edit; re-run the generator after changing")
    out.append(" * src/gui/lang.h or src/gui/sdl3/lang.c.")
    out.append(" *")
    out.append(" * Sorted alphabetically by name so the loader can bsearch.")
    out.append(" */")
    out.append("")
    out.append("typedef struct {")
    out.append("    const char *name;")
    out.append("    langid      id;")
    out.append("} LangNameEntry;")
    out.append("")
    out.append("static const LangNameEntry kLangNameTable[] = {")
    for name, num in pairs:
        out.append(f"    {{ \"{name}\", {name} }},")
    out.append("};")
    out.append("")
    out.append(f"#define K_LANG_NAME_TABLE_SIZE  {len(pairs)}")
    out.append("")
    return "\n".join(out)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--check", action="store_true",
                        help="Exit 1 if outputs would change instead of writing.")
    args = parser.parse_args()

    id_to_name, sections = parse_lang_h(LANG_H_PATH)
    id_to_text = parse_lang_c(LANG_C_PATH)

    en_txt = render_en_txt(id_to_name, sections, id_to_text)
    names_inc = render_names_inc(id_to_name, id_to_text)

    # Sanity report.
    in_h = set(id_to_name)
    in_c = set(id_to_text)
    only_h = sorted(in_h - in_c)
    only_c = sorted(in_c - in_h)
    if only_h:
        print(f"warning: {len(only_h)} ID(s) in lang.h have no entry in lang.c:",
              file=sys.stderr)
        for num in only_h:
            print(f"  {num}  {id_to_name[num]}", file=sys.stderr)
    if only_c:
        print(f"warning: {len(only_c)} ID(s) in lang.c have no symbolic name in lang.h:",
              file=sys.stderr)
        for num in only_c:
            preview = id_to_text[num][:60].replace("\n", "\\n")
            print(f"  {num}  {preview!r}", file=sys.stderr)

    print(f"lang.h: {len(in_h)} symbolic IDs", file=sys.stderr)
    print(f"lang.c: {len(in_c)} string entries", file=sys.stderr)
    print(f"matched: {len(in_h & in_c)}", file=sys.stderr)

    changed = False
    for path, content in [(EN_TXT_PATH, en_txt), (NAMES_INC_PATH, names_inc)]:
        path.parent.mkdir(parents=True, exist_ok=True)
        existing = path.read_text(encoding="utf-8") if path.exists() else None
        if existing != content:
            changed = True
            if args.check:
                print(f"would update: {path.relative_to(REPO_ROOT)}",
                      file=sys.stderr)
            else:
                path.write_text(content, encoding="utf-8")
                print(f"wrote: {path.relative_to(REPO_ROOT)}", file=sys.stderr)

    if args.check and changed:
        return 1
    return 0


if __name__ == "__main__":
    sys.exit(main())
