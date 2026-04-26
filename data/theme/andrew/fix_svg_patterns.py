#!/usr/bin/env python3
"""
Post-processes <n>_inkscape.svg files into <n>.svg, fixing patterns
so they render in renderers that don't implement fill inheritance through
pattern boundaries (e.g. WinBolo).

What it does:
  1. Resolves <pattern xlink:href="#other"> chains so each pattern is
     self-contained.
  2. Pushes inherited fills down from <pattern> elements onto the shapes
     inside them, so renderers don't have to walk up the tree.

The Inkscape export pipeline writes <n>_inkscape.svg, this script
produces the WinBolo-friendly <n>.svg next to it. Re-running is safe
and idempotent --- the source file is the _inkscape.svg, and the output
file is always overwritten.

Skips master source files (tiles.svg, skin.svg, theme.svg) and files
larger than 50 KB.

Usage:
  python fix_svg_patterns.py                      # all *_inkscape.svg in cwd
  python fix_svg_patterns.py file_inkscape.svg
"""

import argparse
import re
import sys
import time
from pathlib import Path
from xml.etree import ElementTree as ET

SVG_NS = "http://www.w3.org/2000/svg"
XLINK_NS = "http://www.w3.org/1999/xlink"

ET.register_namespace("", SVG_NS)
ET.register_namespace("xlink", XLINK_NS)

NS = {"svg": SVG_NS, "xlink": XLINK_NS}

SHAPE_TAGS = {f"{{{SVG_NS}}}{t}" for t in ("rect", "path", "circle", "ellipse", "polygon", "polyline")}

MAX_SIZE_KB = 50
SKIP_NAMES = {"tiles.svg", "skin.svg", "theme.svg"}
SOURCE_SUFFIX = "_inkscape.svg"

# Inkscape on Windows can return from the CLI before all writes flush to
# disk. If we're called right after generation, poll briefly for files.
WAIT_TIMEOUT_S = 5
WAIT_INTERVAL_S = 0.1


def get_fill(elem):
    fill = elem.get("fill")
    if fill:
        return fill
    style = elem.get("style", "")
    m = re.search(r"fill\s*:\s*([^;]+)", style)
    if m:
        return m.group(1).strip()
    return None


def set_fill_if_missing(elem, color):
    """Add a fill to an element if it has none. Prefer style over attr.
    `fill="none"` is treated as an explicit choice (the transparent
    backdrop in many patterns), so we leave it alone."""
    if not color or color == "none":
        return
    if get_fill(elem) is not None:  # any explicit fill, including "none", wins
        return

    style = elem.get("style")
    if style:
        if re.search(r"fill\s*:", style):
            new_style = re.sub(r"fill\s*:\s*[^;]+", f"fill:{color}", style)
        else:
            new_style = f"fill:{color};" + style
        elem.set("style", new_style)
    else:
        elem.set("fill", color)


def find_by_id(root, target_id):
    for elem in root.iter():
        if elem.get("id") == target_id:
            return elem
    return None


def resolve_pattern_chain(pattern, root):
    """If pattern uses xlink:href to reference another pattern, copy its
    children in and inherit its fill."""
    href = pattern.get("href") or pattern.get(f"{{{XLINK_NS}}}href")
    if not href or not href.startswith("#"):
        return

    target = find_by_id(root, href[1:])
    if target is None:
        return

    for child in list(target):
        clone = ET.fromstring(ET.tostring(child))
        pattern.append(clone)

    target_fill = get_fill(target)
    if target_fill and not get_fill(pattern):
        existing_style = pattern.get("style", "")
        pattern.set("style", f"fill:{target_fill};{existing_style}")

    if "href" in pattern.attrib:
        del pattern.attrib["href"]
    if f"{{{XLINK_NS}}}href" in pattern.attrib:
        del pattern.attrib[f"{{{XLINK_NS}}}href"]


def push_fills_down(pattern):
    pattern_fill = get_fill(pattern)
    if not pattern_fill:
        return

    for elem in pattern.iter():
        if elem.tag in SHAPE_TAGS:
            set_fill_if_missing(elem, pattern_fill)


def fix_file(source: Path):
    if source.name in SKIP_NAMES:
        print(f"Skipping {source} (master source file)")
        return
    if not source.name.endswith(SOURCE_SUFFIX):
        print(f"Skipping {source} (not a {SOURCE_SUFFIX} file)")
        return

    size_kb = source.stat().st_size / 1024
    if size_kb > MAX_SIZE_KB:
        print(f"Skipping {source} ({size_kb:.1f} KB > {MAX_SIZE_KB} KB)")
        return

    out = source.with_name(source.name[: -len(SOURCE_SUFFIX)] + ".svg")

    print(f"Processing: {source}")
    tree = ET.parse(source)
    root = tree.getroot()

    patterns = root.findall(".//svg:pattern", NS)
    for pat in patterns:
        resolve_pattern_chain(pat, root)

    patterns = root.findall(".//svg:pattern", NS)
    for pat in patterns:
        push_fills_down(pat)

    tree.write(out, xml_declaration=True, encoding="UTF-8")
    print(f"  -> {out}")


def wait_for_files(pattern: str):
    """Poll for files matching pattern until the file set stabilizes
    (no new files added and no size changes for STABLE_FOR seconds)
    or timeout elapses. Inkscape on Windows lags flushing writes after
    the CLI exits."""
    STABLE_FOR = 0.3

    deadline = time.monotonic() + WAIT_TIMEOUT_S
    last_snapshot = None
    stable_since = None

    while time.monotonic() < deadline:
        # Snapshot: each file as (name, size). New files or growing files
        # mean things are still being written.
        snapshot = frozenset(
            (p.name, p.stat().st_size) for p in Path(".").glob(pattern)
        )

        if snapshot and snapshot == last_snapshot:
            if stable_since is None:
                stable_since = time.monotonic()
            elif time.monotonic() - stable_since >= STABLE_FOR:
                return [Path(name) for name, _ in snapshot]
        else:
            stable_since = None
            last_snapshot = snapshot

        time.sleep(WAIT_INTERVAL_S)

    # Timed out --- return whatever we've got
    return list(Path(".").glob(pattern))


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    p.add_argument("files", nargs="*", help=f"Source files (default: all *{SOURCE_SUFFIX} in cwd)")
    args = p.parse_args()

    if args.files:
        files = [Path(f) for f in args.files]
    else:
        files = wait_for_files(f"*{SOURCE_SUFFIX}")

    if not files:
        print(f"No *{SOURCE_SUFFIX} files found.", file=sys.stderr)
        sys.exit(1)

    for f in files:
        if not f.exists():
            print(f"Not found: {f}", file=sys.stderr)
            continue
        fix_file(f)


if __name__ == "__main__":
    main()