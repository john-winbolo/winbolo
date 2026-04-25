"""generate_themes.py — produce SVG themes from data/svg/*.png.

Generates three theme directories in data/theme/:

  stock_svg/                — pixel-faithful: each opaque pixel
                              becomes its own <rect> in the SVG.
                              Largest output, most precise.
  stock_svg_pixelgrouped/   — same colour neighbours merged into a
                              single <path> region (flood fill on
                              same-rgba quads).  Smaller files, same
                              visible result.
  stock_svg_ingamerotate/   — same as stock_svg but only emits the
                              N-facing variant (_00) of rotation
                              groups (tanks, boats, shells).  The
                              game engine is expected to rotate the
                              sprite in-game for the other 15 (or
                              256) directions.

Usage (from repo root):
    python tools/generate_themes.py [--input data/svg]
                                    [--out   data/theme]

The PNGs are read in their native resolution; the SVG dimensions
match exactly so the existing tileLoaderBuildSheet path that scales
SVGs to a target tile size produces pixel-equivalent output.
"""

from __future__ import annotations

import argparse
import os
import re
from collections import deque
from typing import Iterable

from PIL import Image  # type: ignore

# Filenames that are part of a rotation group — only the _00 variant
# is emitted into stock_svg_ingamerotate.  All other names are emitted
# unchanged.
#
# Pattern: <prefix>_<NN>.png  where NN is 00..15.
ROTATION_GROUPS = (
    "tank_evil",
    "tank_evilboat",
    "tank_good",
    "tank_goodboat",
    "tank_self",
    "tank_selfboat",
    "shell",
)


def is_rotation_variant(filename: str) -> tuple[str, int] | None:
    """If filename is <group>_NN.png from a rotation group, return
    (group, NN); else None."""
    m = re.match(r"(.+?)_(\d{2})\.png$", filename)
    if not m:
        return None
    prefix, num_str = m.group(1), m.group(2)
    if prefix not in ROTATION_GROUPS:
        return None
    return prefix, int(num_str)


def rgba_pixels(im: Image.Image) -> list[list[tuple[int, int, int, int]]]:
    """Read pixels as [y][x] = (r, g, b, a).  Returns full-alpha
    pixels only via lookup; alpha=0 pixels are kept so callers can
    skip them."""
    im = im.convert("RGBA")
    w, h = im.size
    px = im.load()
    return [[px[x, y] for x in range(w)] for y in range(h)]


def emit_pixelwise_svg(im: Image.Image) -> str:
    """One <rect> per opaque pixel.  Faithful but verbose."""
    w, h = im.size
    grid = rgba_pixels(im)
    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
        f'shape-rendering="crispEdges">'
    ]
    for y in range(h):
        for x in range(w):
            r, g, b, a = grid[y][x]
            if a == 0:
                continue
            if a == 255:
                parts.append(
                    f'<rect x="{x}" y="{y}" width="1" height="1" '
                    f'fill="#{r:02x}{g:02x}{b:02x}"/>'
                )
            else:
                parts.append(
                    f'<rect x="{x}" y="{y}" width="1" height="1" '
                    f'fill="#{r:02x}{g:02x}{b:02x}" '
                    f'fill-opacity="{a / 255:.3f}"/>'
                )
    parts.append("</svg>")
    return "\n".join(parts)


def emit_grouped_svg(im: Image.Image) -> str:
    """Flood-fill same-colour 4-connected neighbours into a single
    <path> per region.  Each path is a series of M / h / v ops
    that traces the region's outline as a single polygon.

    For the simplest correct output, we instead emit a <path> made
    from horizontal-strip <rect> ops via 'd' commands: each region
    is rendered as a series of unit-height row strips concatenated.
    Small enough output for sprite-sized PNGs (<= ~32×32) and avoids
    the complexity of polygon outlining."""
    w, h = im.size
    grid = rgba_pixels(im)
    visited = [[False] * w for _ in range(h)]

    parts = [
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'width="{w}" height="{h}" viewBox="0 0 {w} {h}" '
        f'shape-rendering="crispEdges">'
    ]

    for sy in range(h):
        for sx in range(w):
            if visited[sy][sx]:
                continue
            seed = grid[sy][sx]
            if seed[3] == 0:
                visited[sy][sx] = True
                continue

            # Flood-fill 4-connected same-rgba region.
            region: list[tuple[int, int]] = []
            queue: deque[tuple[int, int]] = deque([(sx, sy)])
            visited[sy][sx] = True
            while queue:
                cx, cy = queue.popleft()
                region.append((cx, cy))
                for nx, ny in (
                    (cx - 1, cy),
                    (cx + 1, cy),
                    (cx, cy - 1),
                    (cx, cy + 1),
                ):
                    if 0 <= nx < w and 0 <= ny < h and not visited[ny][nx]:
                        if grid[ny][nx] == seed:
                            visited[ny][nx] = True
                            queue.append((nx, ny))

            # Emit region as a list of horizontal-strip rects.  Group
            # consecutive cells in the same row into one rect.
            r, g, b, a = seed
            color = f"#{r:02x}{g:02x}{b:02x}"
            opacity_attr = (
                "" if a == 255 else f' fill-opacity="{a / 255:.3f}"'
            )
            # Bucket by row, sorted by x.
            rows: dict[int, list[int]] = {}
            for x, y in region:
                rows.setdefault(y, []).append(x)

            # Build the rects for this region as a single <g>.
            parts.append(f'<g fill="{color}"{opacity_attr}>')
            for y, xs in sorted(rows.items()):
                xs.sort()
                run_start = xs[0]
                run_end = xs[0]
                for x in xs[1:]:
                    if x == run_end + 1:
                        run_end = x
                    else:
                        parts.append(
                            f'<rect x="{run_start}" y="{y}" '
                            f'width="{run_end - run_start + 1}" height="1"/>'
                        )
                        run_start = x
                        run_end = x
                parts.append(
                    f'<rect x="{run_start}" y="{y}" '
                    f'width="{run_end - run_start + 1}" height="1"/>'
                )
            parts.append("</g>")

    parts.append("</svg>")
    return "\n".join(parts)


def write_text(path: str, text: str) -> None:
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8") as f:
        f.write(text)


def process_one(
    png_path: str,
    name: str,
    out_pixelwise_dir: str,
    out_grouped_dir: str,
    out_rotate_dir: str,
) -> None:
    im = Image.open(png_path)

    pixelwise = emit_pixelwise_svg(im)
    grouped = emit_grouped_svg(im)

    write_text(os.path.join(out_pixelwise_dir, name + ".svg"), pixelwise)
    write_text(os.path.join(out_grouped_dir, name + ".svg"), grouped)

    # Rotate variant: only emit N-facing (_00) members of rotation
    # groups; everything non-rotation goes through unchanged.
    rot = is_rotation_variant(name + ".png")
    if rot is None:
        write_text(os.path.join(out_rotate_dir, name + ".svg"), pixelwise)
    elif rot[1] == 0:
        write_text(os.path.join(out_rotate_dir, name + ".svg"), pixelwise)
    # else: skip — caller will rotate _00 in-game.


def main() -> None:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--input", default="data/svg")
    ap.add_argument("--out", default="data/theme")
    args = ap.parse_args()

    out_pix = os.path.join(args.out, "stock_svg")
    out_grp = os.path.join(args.out, "stock_svg_pixelgrouped")
    out_rot = os.path.join(args.out, "stock_svg_ingamerotate")

    pngs = sorted(
        f for f in os.listdir(args.input) if f.lower().endswith(".png")
    )
    print(f"Found {len(pngs)} PNGs in {args.input}")
    for fn in pngs:
        name = os.path.splitext(fn)[0]
        process_one(
            os.path.join(args.input, fn),
            name,
            out_pix,
            out_grp,
            out_rot,
        )

    # Print summary of what landed in each theme.
    for d in (out_pix, out_grp, out_rot):
        n = len(
            [f for f in os.listdir(d) if f.lower().endswith(".svg")]
        ) if os.path.isdir(d) else 0
        print(f"  {d}: {n} SVG file(s)")


if __name__ == "__main__":
    main()
