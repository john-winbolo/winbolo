#!/usr/bin/env python3
# lobby_reset_scan.py
#
# Report file-scope static variables in the lobby dialog sources that no
# reset path ever writes back to a known state.
#
# The lobby dialog keeps its UI state in file-scope statics. Those statics
# outlive a single lobby: the dialog is torn down and re-entered within one
# process, so any static that no reset path clears carries stale state into
# the next lobby. This script lists the ones nothing clears, so the set can
# be reviewed and kept from growing.
#
# A static counts as reset if its name is written inside the body of
# imguiLobbyFrameReset() or of any function named lobby<...>Reset,
# lobby<...>End, or lobby<...>Abort. The bodies are treated as one region,
# so a symbol cleared in lobbyReelEnd() counts as reset even though
# imguiLobbyFrameReset() does not touch it; no call graph is built.
#
# Statics that are unreset on purpose are listed separately, with a reason,
# rather than dropped silently — see ALLOWLIST_NAMES and ALLOWLIST_TYPES.
#
# This is a regex heuristic, not a C parser. Known limits:
#
#   * File scope is decided by the static keyword sitting at column 0.
#     Every file-scope static in these sources is written that way; a
#     block-scope static indented inside a function is not reported.
#   * A static function pointer (static void (*fp)(void) = NULL;) is
#     misfiled as a function declaration and skipped, because the check
#     for "is this a function" is a ( ahead of the first = or ;.
#   * A declaration whose identifier is not on the same line as the
#     static keyword is not parsed.
#   * Writes are matched as NAME =, NAME[...] =, NAME.field =, a compound
#     assignment, memset/SDL_memset of NAME, or a NAME.clear()/.reset()/
#     .assign()/.store() call. A write through a pointer or a reference
#     bound elsewhere is not seen, so a symbol can be reported as
#     never-reset while something does in fact clear it.
#   * Comments and string literals are blanked before matching, so a name
#     mentioned only in prose does not count as a write.
#
# Usage:
#   python3 tools/lobby_reset_scan.py                    (default target set)
#   python3 tools/lobby_reset_scan.py --verbose          (also list reset statics)
#   python3 tools/lobby_reset_scan.py path/to/file.cpp   (scan named files)
#
# Exit code 0 when every never-reset static is allowlisted, 1 otherwise.

import argparse
import bisect
import fnmatch
import re
import sys
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parent.parent
LOBBY_CPP_PATH = REPO_ROOT / "src" / "gui" / "sdl3" / "dialogs" / "imgui_lobby.cpp"
LOBBY_DIR_PATH = REPO_ROOT / "src" / "gui" / "sdl3" / "dialogs" / "lobby"

# Statics that no reset path is expected to touch. Entries are matched
# against the variable name; a trailing * makes the entry a family.
ALLOWLIST_NAMES = (
    ("s_lobbySplitOffsetMap",
     "settings-backed splitter position, persists across sessions"),
    ("s_lobbySplitOffsetRecap",
     "settings-backed splitter position, persists across sessions"),
    ("s_lobbySplitOffsetInit",
     "one-shot latch guarding the settings-backed splitter load"),
    ("s_icons",
     "icon and tank texture cache, keyed on the renderer and valid "
     "process-wide"),
    ("s_settings",
     "settings header open state, deliberately carried across rounds by "
     "lobbySettingsPostGameEdge"),
    ("s_brains",
     "sticky bot-brain pick and the brain about.txt metadata cache, "
     "process-scoped by design"),
    ("s_chooserTabs",
     "map browsers' folder, selection and remembered tab, deliberately "
     "kept across sessions"),
    ("s_wbnMaps",
     "WinBolo.net folder listing and search cache behind the chooser tab "
     "that keeps its position, deliberately kept across sessions"),
    ("s_recapPost",
     "in-flight comment post, left running because a POST cannot be "
     "cancelled and freeing it would block the round-start path"),
)

# Statics whose declared type makes them non-assignable. Matched on the
# type text so new ones are covered as they appear.
ALLOWLIST_TYPES = (
    ("std::mutex", "not assignable; guards a region rather than holding state"),
    ("std::atomic", "torn down explicitly, not cleared by assignment"),
    ("std::thread", "torn down explicitly by join/detach, not by assignment"),
)

RESET_FUNC_RE = re.compile(r"\b(imguiLobbyFrameReset|lobby\w*(?:Reset|End|Abort))\s*\(")

IDENT_RE = re.compile(r"[A-Za-z_]\w*")

# NAME, optionally followed by subscripts and member access, then an
# assignment operator. == / != / <= / >= do not match.
ACCESS_CHAIN = (
    r"(?:\s*\[[^\[\]]*(?:\[[^\[\]]*\][^\[\]]*)*\]"
    r"|\s*(?:\.|->)\s*\w+)*"
)
ASSIGN_OP = r"(?:<<=|>>=|\+=|-=|\*=|/=|%=|&=|\|=|\^=|=(?!=))"


def strip_comments_and_literals(text):
    """Blank out comments and string/char literal bodies.

    Returns a string of the same length as the input, with every blanked
    character replaced by a space and newlines preserved, so offsets and
    line numbers still line up with the original.
    """
    out = list(text)
    i = 0
    n = len(text)
    while i < n:
        c = text[i]
        if c == "/" and i + 1 < n and text[i + 1] == "/":
            while i < n and text[i] != "\n":
                out[i] = " "
                i += 1
        elif c == "/" and i + 1 < n and text[i + 1] == "*":
            out[i] = " "
            out[i + 1] = " "
            i += 2
            while i < n:
                if text[i] == "*" and i + 1 < n and text[i + 1] == "/":
                    out[i] = " "
                    out[i + 1] = " "
                    i += 2
                    break
                if text[i] != "\n":
                    out[i] = " "
                i += 1
        elif c in "\"'":
            quote = c
            i += 1
            while i < n and text[i] != quote:
                if text[i] == "\\" and i + 1 < n:
                    out[i] = " "
                    if text[i + 1] != "\n":
                        out[i + 1] = " "
                    i += 2
                    continue
                if text[i] != "\n":
                    out[i] = " "
                i += 1
            i += 1
        else:
            i += 1
    return "".join(out)


def line_starts(text):
    """Offsets of the first character of each line, for offset -> line."""
    starts = [0]
    for m in re.finditer(r"\n", text):
        starts.append(m.end())
    return starts


def line_of(starts, offset):
    return bisect.bisect_right(starts, offset)


def split_declarators(text):
    """Split a declaration body at top-level commas.

    Commas inside (), [], {} or <> belong to an initializer or a template
    argument list, not to a second declarator.
    """
    parts = []
    depth = 0
    current = []
    for ch in text:
        if ch in "([{<":
            depth += 1
        elif ch in ")]}>":
            depth = max(0, depth - 1)
        if ch == "," and depth == 0:
            parts.append("".join(current))
            current = []
        else:
            current.append(ch)
    parts.append("".join(current))
    return [p for p in parts if p.strip()]


def declarator_name(segment):
    """Last identifier before the first [, =, { or end of the segment."""
    cut = len(segment)
    for ch in "[={":
        pos = segment.find(ch)
        if pos != -1:
            cut = min(cut, pos)
    names = IDENT_RE.findall(segment[:cut])
    return names[-1] if names else None


def parse_statics(code, starts):
    """Return the file-scope static variables declared in code.

    Static functions and named constants (static const / static constexpr)
    are not state and are left out.
    """
    found = []
    for m in re.finditer(r"(?m)^static\b", code):
        line_no = line_of(starts, m.start())
        eol = code.find("\n", m.start())
        if eol == -1:
            eol = len(code)
        line = code[m.start():eol]
        body = line[len("static"):]

        first_stop = len(body)
        for ch in "=;":
            pos = body.find(ch)
            if pos != -1:
                first_stop = min(first_stop, pos)
        paren = body.find("(")
        if paren != -1 and paren < first_stop:
            continue  # function declaration or definition

        if re.match(r"\s*(?:const|constexpr)\b", body):
            continue  # named constant, not state

        tag = re.match(r"\s*(?:struct|union|enum|class)\b", body)
        brace = body.find("{")
        if tag and brace != -1:
            # A type defined inline: the declarator follows the closing
            # brace, which is normally several lines down.
            open_at = m.start() + len("static") + brace
            depth = 0
            k = open_at
            while k < len(code):
                if code[k] == "{":
                    depth += 1
                elif code[k] == "}":
                    depth -= 1
                    if depth == 0:
                        break
                k += 1
            end = code.find(";", k)
            if end == -1:
                continue
            decl = code[k + 1:end]
            base_type = " ".join(body[:brace].split())
            for segment in split_declarators(decl):
                name = declarator_name(segment)
                if name:
                    found.append({
                        "name": name,
                        "type": base_type,
                        "line": line_no,
                    })
            continue

        decl = body.split(";", 1)[0]
        segments = split_declarators(decl)
        if not segments:
            continue

        base_type = None
        for index, segment in enumerate(segments):
            name = declarator_name(segment)
            if name is None:
                continue
            if index == 0:
                pos = segment.rfind(name)
                base_type = " ".join(segment[:pos].split())
            found.append({
                "name": name,
                "type": base_type or "",
                "line": line_no,
            })
    return found


def find_reset_bodies(code):
    """Return [(function name, body text)] for every reset definition."""
    bodies = []
    for m in RESET_FUNC_RE.finditer(code):
        depth = 0
        i = m.end() - 1
        while i < len(code):
            if code[i] == "(":
                depth += 1
            elif code[i] == ")":
                depth -= 1
                if depth == 0:
                    break
            i += 1
        j = i + 1
        while j < len(code) and code[j] in " \t\r\n":
            j += 1
        if j >= len(code) or code[j] != "{":
            continue  # a prototype or a call, not a definition
        depth = 0
        k = j
        while k < len(code):
            if code[k] == "{":
                depth += 1
            elif code[k] == "}":
                depth -= 1
                if depth == 0:
                    break
            k += 1
        bodies.append((m.group(1), code[j:k + 1]))
    return bodies


def writes_to(region, name):
    escaped = re.escape(name)
    patterns = (
        r"\b" + escaped + r"\b" + ACCESS_CHAIN + r"\s*" + ASSIGN_OP,
        r"\b(?:SDL_)?memset\s*\(\s*&?\s*" + escaped + r"\b",
        r"\b" + escaped + r"\b\s*\.\s*(?:clear|reset|assign|store)\s*\(",
    )
    return any(re.search(p, region) for p in patterns)


def allowlist_reason(entry):
    for pattern, reason in ALLOWLIST_NAMES:
        if fnmatch.fnmatchcase(entry["name"], pattern):
            return reason
    for needle, reason in ALLOWLIST_TYPES:
        if needle in entry["type"]:
            return reason
    return None


def rel(path):
    try:
        return str(path.relative_to(REPO_ROOT))
    except ValueError:
        return str(path)


def default_targets():
    targets = []
    if LOBBY_CPP_PATH.is_file():
        targets.append(LOBBY_CPP_PATH)
    if LOBBY_DIR_PATH.is_dir():
        targets.extend(sorted(LOBBY_DIR_PATH.glob("*.cpp")))
    return targets


def scan(path):
    """Return (never_reset, allowlisted, reset) for one file."""
    code = strip_comments_and_literals(path.read_text(encoding="utf-8", errors="replace"))
    starts = line_starts(code)
    bodies = find_reset_bodies(code)

    never_reset = []
    allowlisted = []
    reset = []
    for entry in parse_statics(code, starts):
        clearers = [fn for fn, body in bodies if writes_to(body, entry["name"])]
        if clearers:
            entry["clearers"] = sorted(set(clearers))
            reset.append(entry)
            continue
        reason = allowlist_reason(entry)
        if reason:
            entry["reason"] = reason
            allowlisted.append(entry)
        else:
            never_reset.append(entry)
    return never_reset, allowlisted, reset


def main(argv=None):
    parser = argparse.ArgumentParser(
        description="List file-scope statics in the lobby sources that no "
                    "reset path assigns.")
    parser.add_argument("paths", nargs="*",
                        help="source files to scan (default: the lobby sources)")
    parser.add_argument("--verbose", action="store_true",
                        help="also list the statics that are reset, and where")
    args = parser.parse_args(argv)

    if args.paths:
        targets = [Path(p).resolve() for p in args.paths]
    else:
        targets = default_targets()

    missing = [p for p in targets if not p.is_file()]
    for path in missing:
        print("error: no such file: %s" % rel(path), file=sys.stderr)
    if missing:
        return 1
    if not targets:
        print("error: no source files to scan", file=sys.stderr)
        return 1

    total_never = 0
    total_allowed = 0
    total_reset = 0

    print("Never-reset file-scope statics")
    print("==============================")
    for path in targets:
        never_reset, allowlisted, reset = scan(path)
        total_allowed += len(allowlisted)
        total_reset += len(reset)
        total_never += len(never_reset)

        print("")
        print(rel(path))
        if not never_reset:
            print("  (none)")
        for entry in never_reset:
            print("  %s:%-6d %-32s %s"
                  % (rel(path), entry["line"], entry["name"], entry["type"]))
        print("  %d never-reset" % len(never_reset))

        if allowlisted:
            print("")
            print("  Allowlisted (unreset by design)")
            for entry in allowlisted:
                print("    %s:%-6d %-32s %s"
                      % (rel(path), entry["line"], entry["name"], entry["reason"]))

        if args.verbose:
            print("")
            print("  Reset elsewhere")
            if not reset:
                print("    (none)")
            for entry in reset:
                print("    %s:%-6d %-32s %s"
                      % (rel(path), entry["line"], entry["name"],
                         ", ".join(entry["clearers"])))

    print("")
    print("Total: %d never-reset, %d allowlisted, %d reset, across %d file(s)"
          % (total_never, total_allowed, total_reset, len(targets)))

    return 0 if total_never == 0 else 1


if __name__ == "__main__":
    sys.exit(main())
