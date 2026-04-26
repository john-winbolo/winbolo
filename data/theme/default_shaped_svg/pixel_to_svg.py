"""
Pixel tile -> SVG shaper.

Detects 8-connected blobs of similar color and emits SVG paths in three
fitting styles: crisp (axis-aligned polygon), smoothed (chamfered corners),
and organic (cubic bezier curves).

Each color becomes its own <g> layer so they can be reordered later.
"""

from __future__ import annotations

import math
from dataclasses import dataclass, field
from pathlib import Path
from typing import Iterable

import numpy as np
from PIL import Image


# ---------------------------------------------------------------------------
# Tunable parameters
# ---------------------------------------------------------------------------

@dataclass
class Params:
    # Two pixels are considered the "same color" for clumping if their RGB
    # Euclidean distance is <= color_tolerance. 0 = exact match.
    color_tolerance: float = 0.0

    # Pixels whose max(R,G,B) <= black_threshold are treated as "black-ish".
    # All such pixels merge into a single black class regardless of their
    # tiny RGB variations -- handy for noisy near-blacks like (0, 8, 0).
    black_threshold: int = 32

    # When black_merge is True, all black-ish pixels become one logical color
    # using black_canonical_color as their representative.
    black_merge: bool = True
    black_canonical_color: tuple[int, int, int] = (0, 0, 0)

    # Diagonal connectivity for blob detection. True = 8-connected.
    diagonal_connect: bool = True

    # Smoothed-style chamfer: how far in from each corner (in pixel units,
    # 0..0.5). 0.0 = no chamfer (== crisp). 0.5 = max chamfer.
    chamfer: float = 0.35

    # Organic-style curve tension. 0 = straight, 1 = very curvy.
    curve_tension: float = 0.5

    # Output scale. Each source pixel becomes scale x scale output pixels.
    scale: int = 10

    # Layer ordering. Layers later in the SVG paint on top.
    #   "luminance_asc"  -> darkest first (darkest on bottom, brightest on top)
    #   "luminance_desc" -> brightest first (brightest on bottom, darkest on top)
    #   "frequency_asc"  -> rarest colors first (rarest on bottom)
    #   "frequency_desc" -> most common first (most common on bottom)
    #   "manual"         -> use color_priority below
    layer_order: str = "luminance_asc"

    # Optional explicit priority: maps RGB hex like "#ff00aa" -> integer.
    # Lower number = lower in stack (painted earlier). Colors not in this
    # map fall back to the layer_order rule and are placed after the
    # explicitly ordered ones.
    color_priority: dict[str, int] = field(default_factory=dict)

    # Background detection. When True, the most common color label among the
    # tile's outer border pixels is treated as the background: it's painted
    # first as a full tile-sized rectangle and excluded from blob smoothing
    # so the tile's outer edge stays crisp regardless of style.
    detect_background: bool = True

    # Optional explicit override (RGB hex). If set, this color is forced as
    # the background, skipping the auto-detection. Useful when you know the
    # background but auto-detection picks something else.
    background_color: str | None = None

    # Fatten foreground shapes so adjacent same-region pixels overlap and
    # don't leave background-colored gaps between them. Specified in source
    # pixels (so 0.25 means a quarter-pixel wider all the way around).
    # Applied to every non-background layer.
    fatten: float = 0.25

    # Transparency handling. WinBolo's stock PNGs use a magic-green RGB
    # (0, 255, 0) as the colorkey; real RGBA themes use alpha. We treat
    # both as "no pixel here" so the SVG keeps that area transparent
    # instead of painting a green tile.
    alpha_threshold: int = 128                # alpha < this -> transparent
    magic_green_transparent: bool = True
    magic_green_color: tuple[int, int, int] = (0, 255, 0)

    # How to fatten:
    #   "stroke"   -> add an SVG stroke of the same color as the fill. Cheap,
    #                 also softens sharp outer corners. Width = fatten * 2 *
    #                 scale (so `fatten` is the per-side outset in pixels).
    #   "geometry" -> push each polygon vertex outward along its corner normal
    #                 before fitting. Preserves sharp corners better but is a
    #                 bit slower and can self-intersect at very thin features.
    fatten_mode: str = "stroke"


# ---------------------------------------------------------------------------
# Color quantization & blob detection
# ---------------------------------------------------------------------------

TRANSPARENT_LABEL = -2  # distinct from -1 (unassigned sentinel)


def quantize(arr: np.ndarray, params: Params) -> np.ndarray:
    """Return an int label image where same label == same logical color.

    arr can be HxWx3 (RGB) or HxWx4 (RGBA). Transparent pixels (alpha
    below the threshold, or RGB matching magic_green_color when
    magic_green_transparent is on) get TRANSPARENT_LABEL and are
    excluded from later blob/colour processing.
    """
    h, w, channels = arr.shape
    rgb = arr[..., :3].reshape(-1, 3).astype(np.int32)
    if channels == 4:
        alpha = arr[..., 3].reshape(-1).astype(np.int32)
    else:
        alpha = np.full(len(rgb), 255, dtype=np.int32)

    transparent_mask = alpha < params.alpha_threshold
    if params.magic_green_transparent:
        mg = params.magic_green_color
        transparent_mask |= (
            (rgb[:, 0] == mg[0])
            & (rgb[:, 1] == mg[1])
            & (rgb[:, 2] == mg[2])
        )

    flat = rgb

    # Identify black-ish pixels first if requested.
    if params.black_merge:
        black_mask = (flat.max(axis=1) <= params.black_threshold) & ~transparent_mask
    else:
        black_mask = np.zeros(len(flat), dtype=bool)

    labels = np.full(len(flat), -1, dtype=np.int32)
    labels[transparent_mask] = TRANSPARENT_LABEL
    representatives: list[tuple[int, int, int]] = []

    # All blacks share label 0.
    if black_mask.any():
        labels[black_mask] = 0
        representatives.append(params.black_canonical_color)

    # Cluster the remaining pixels by tolerance (single-pass greedy).
    tol_sq = params.color_tolerance ** 2
    cluster_centers: list[np.ndarray] = []
    cluster_labels: list[int] = []
    next_label = len(representatives)

    non_black_idx = np.where(~black_mask & ~transparent_mask)[0]
    for i in non_black_idx:
        px = flat[i]
        if tol_sq <= 0:
            t = (int(px[0]), int(px[1]), int(px[2]))
            # Look for an exact match.
            found = -1
            for j, c in enumerate(cluster_centers):
                if int(c[0]) == t[0] and int(c[1]) == t[1] and int(c[2]) == t[2]:
                    found = cluster_labels[j]
                    break
            if found == -1:
                cluster_centers.append(px.copy())
                cluster_labels.append(next_label)
                representatives.append(t)
                labels[i] = next_label
                next_label += 1
            else:
                labels[i] = found
        else:
            # Find nearest existing center within tolerance.
            best = -1
            best_d = tol_sq + 1
            for j, c in enumerate(cluster_centers):
                d = float(((px - c) ** 2).sum())
                if d <= tol_sq and d < best_d:
                    best = j
                    best_d = d
            if best == -1:
                cluster_centers.append(px.astype(np.float64).copy())
                cluster_labels.append(next_label)
                representatives.append((int(px[0]), int(px[1]), int(px[2])))
                labels[i] = next_label
                next_label += 1
            else:
                labels[i] = cluster_labels[best]

    label_img = labels.reshape(h, w)
    return label_img, representatives


def find_blobs(label_img: np.ndarray, diagonal: bool) -> list[dict]:
    """Return list of blobs: {label, color_idx, pixels: set[(y,x)]}."""
    h, w = label_img.shape
    visited = np.zeros_like(label_img, dtype=bool)
    blobs: list[dict] = []

    if diagonal:
        neighbors = [(-1, -1), (-1, 0), (-1, 1),
                     (0, -1),           (0, 1),
                     (1, -1),  (1, 0),  (1, 1)]
    else:
        neighbors = [(-1, 0), (1, 0), (0, -1), (0, 1)]

    for y in range(h):
        for x in range(w):
            if visited[y, x]:
                continue
            color = int(label_img[y, x])
            if color == TRANSPARENT_LABEL:
                visited[y, x] = True
                continue
            stack = [(y, x)]
            pixels: set[tuple[int, int]] = set()
            while stack:
                cy, cx = stack.pop()
                if visited[cy, cx]:
                    continue
                if int(label_img[cy, cx]) != color:
                    continue
                visited[cy, cx] = True
                pixels.add((cy, cx))
                for dy, dx in neighbors:
                    ny, nx = cy + dy, cx + dx
                    if 0 <= ny < h and 0 <= nx < w and not visited[ny, nx]:
                        if int(label_img[ny, nx]) == color:
                            stack.append((ny, nx))
            blobs.append({"color_idx": color, "pixels": pixels})
    return blobs


# ---------------------------------------------------------------------------
# Boundary tracing (Moore-Neighbor) for a blob's outer outline.
# Each pixel becomes a unit square at integer coordinates [x, x+1] x [y, y+1].
# We trace the polygon outline of the union of those squares.
# ---------------------------------------------------------------------------

def blob_to_polygons(pixels: set[tuple[int, int]]) -> list[list[tuple[float, float]]]:
    """
    Return a list of polygons. Each polygon is a closed list of (x, y) vertices.

    Method: for each pixel, walk its 4 edges. Each edge oriented so the filled
    region is on its LEFT. Then chain edges into rings.

    Diagonal pinch handling: when two same-color pixels touch only at a corner
    (i.e., A on (y,x) and (y+1,x+1) with the other two diagonal cells NOT in
    the blob), the naive edge-walker produces two squares meeting at a single
    point and chains them as separate rings. We fix that by detecting these
    "saddle" corners and splicing the rings: at such a corner, instead of a
    pinch, we bridge the same-color regions through the corner so the blob
    becomes a single connected polygon.

    We do this at the chaining stage by routing through saddle vertices: when
    a chained ring reaches a saddle vertex, we choose the outgoing edge that
    keeps the same-color region together rather than splitting it.
    """
    if not pixels:
        return []

    # First, identify saddle corners. A vertex (vx, vy) on the integer grid is
    # surrounded by 4 cells: NW=(vy-1, vx-1), NE=(vy-1, vx), SW=(vy, vx-1), SE=(vy, vx).
    # A saddle for THIS blob occurs when NW and SE are in pixels but NE and SW
    # are not, OR when NE and SW are in pixels but NW and SE are not.
    # At a saddle, we want the two same-color cells joined; the pixel-edge graph
    # will have the vertex appearing twice (in-degree 2, out-degree 2), and we
    # need to pick the right pairing.
    saddles: dict[tuple[int, int], str] = {}  # vertex -> 'nw_se' or 'ne_sw'

    # Vertices range over all pixel corners.
    ys = [y for (y, x) in pixels]
    xs = [x for (y, x) in pixels]
    if not ys:
        return []

    for y in range(min(ys), max(ys) + 2):
        for x in range(min(xs), max(xs) + 2):
            nw = (y - 1, x - 1) in pixels
            ne = (y - 1, x) in pixels
            sw = (y, x - 1) in pixels
            se = (y, x) in pixels
            if nw and se and not ne and not sw:
                saddles[(x, y)] = "nw_se"
            elif ne and sw and not nw and not se:
                saddles[(x, y)] = "ne_sw"

    # Collect oriented boundary edges. Each edge keeps fill on its LEFT.
    # We tag each edge with the "side" of the pixel it came from so we can
    # disambiguate at saddles.
    # Edge format: (from, to, owner_pixel) where owner_pixel is the (y,x) of
    # the pixel whose side this edge belongs to. The owner tells us which
    # of the two same-color cells at a saddle this edge is leaving/entering.
    edges_from: dict[tuple[float, float], list[tuple[tuple[float, float], tuple[int, int]]]] = {}

    def add_edge(a, b, owner):
        edges_from.setdefault(a, []).append((b, owner))

    for (y, x) in pixels:
        if (y - 1, x) not in pixels:
            add_edge((x + 1, y), (x, y), (y, x))         # top edge: right-to-left
        if (y + 1, x) not in pixels:
            add_edge((x, y + 1), (x + 1, y + 1), (y, x)) # bottom: left-to-right
        if (y, x - 1) not in pixels:
            add_edge((x, y), (x, y + 1), (y, x))         # left: top-to-bottom
        if (y, x + 1) not in pixels:
            add_edge((x + 1, y + 1), (x + 1, y), (y, x)) # right: bottom-to-top

    # Index edges by their incoming pixel-owner too, so at a saddle we can pick
    # the outgoing edge whose owner is the SAME pixel as our incoming edge's
    # destination cell. Concretely: edges arrive at the saddle vertex from one
    # of the two same-color cells, and we want to leave via an edge that's a
    # side of the OTHER same-color cell -- this is what stitches them together.
    rings: list[list[tuple[float, float]]] = []

    # Keep edges as a multimap we consume.
    while edges_from:
        # Pick any starting vertex with outgoing edges.
        start = next(iter(edges_from))
        if not edges_from[start]:
            del edges_from[start]
            continue

        ring = [start]
        current = start
        prev_owner = None  # the pixel that owned the most recent traversed edge

        while True:
            outs = edges_from.get(current)
            if not outs:
                break

            chosen_idx = 0
            if current in saddles and len(outs) >= 2 and prev_owner is not None:
                # At a saddle, prefer an outgoing edge whose owner is the
                # DIAGONAL same-color cell relative to prev_owner. That keeps
                # both diagonal cells inside the same ring.
                py, px = prev_owner
                kind = saddles[current]
                if kind == "nw_se":
                    # diagonal pair: (vy-1, vx-1) and (vy, vx)
                    if (py, px) == (current[1] - 1, current[0] - 1):
                        target = (current[1], current[0])
                    else:
                        target = (current[1] - 1, current[0] - 1)
                else:  # ne_sw
                    # diagonal pair: (vy-1, vx) and (vy, vx-1)
                    if (py, px) == (current[1] - 1, current[0]):
                        target = (current[1], current[0] - 1)
                    else:
                        target = (current[1] - 1, current[0])
                for i, (_, owner) in enumerate(outs):
                    if owner == target:
                        chosen_idx = i
                        break

            nxt, owner = outs.pop(chosen_idx)
            if not outs:
                del edges_from[current]
            prev_owner = owner
            ring.append(nxt)
            current = nxt
            if current == start and edges_from.get(start) is None:
                break
            if current == start:
                # Closed ring; we still allow continuing if more edges leave
                # start, but for a clean ring we stop here.
                break

        if len(ring) > 1 and ring[0] == ring[-1]:
            ring.pop()
        if len(ring) >= 3:
            rings.append(ring)

    return rings


def collapse_collinear(ring: list[tuple[float, float]]) -> list[tuple[float, float]]:
    """Remove vertices that lie on a straight line between their neighbors."""
    if len(ring) < 3:
        return ring
    out = []
    n = len(ring)
    for i in range(n):
        a = ring[(i - 1) % n]
        b = ring[i]
        c = ring[(i + 1) % n]
        # Cross product of (b-a) x (c-b)
        cross = (b[0] - a[0]) * (c[1] - b[1]) - (b[1] - a[1]) * (c[0] - b[0])
        if cross != 0:
            out.append(b)
    if len(out) < 3:
        return ring
    return out


def offset_ring(ring: list[tuple[float, float]], distance: float) -> list[tuple[float, float]]:
    """
    Push each vertex outward by `distance` along the bisector of its corner.
    The ring is assumed to be wound such that "outward" = the direction that
    expands the polygon. Our tracer winds rings with fill on the LEFT of each
    edge, which is counter-clockwise in screen-space (y-down), so the outward
    normal of each edge is to the right of its direction.
    """
    if distance == 0 or len(ring) < 3:
        return list(ring)

    n = len(ring)

    # Per-edge outward unit normals (right of edge direction in y-down space).
    edge_normals = []
    for i in range(n):
        ax, ay = ring[i]
        bx, by = ring[(i + 1) % n]
        ex, ey = bx - ax, by - ay
        length = math.hypot(ex, ey)
        if length == 0:
            edge_normals.append((0.0, 0.0))
            continue
        # Right-of-direction normal in y-down: (ey, -ex)/length
        edge_normals.append((ey / length, -ex / length))

    new_ring: list[tuple[float, float]] = []
    for i in range(n):
        # Vertex i is between edge (i-1) and edge i.
        n1 = edge_normals[(i - 1) % n]
        n2 = edge_normals[i]
        bx, by = (n1[0] + n2[0]), (n1[1] + n2[1])
        blen = math.hypot(bx, by)
        if blen < 1e-9:
            # 180-degree turn (shouldn't happen after collapse_collinear, but
            # guard anyway). Use either edge normal directly.
            ox, oy = n2
        else:
            # Miter scaling: divide by cos(half-angle) to reach outset distance.
            # cos(half-angle) = (bisector dot edge_normal) / |bisector|. Since
            # both edge normals are unit length, |bx,by|/2 = cos(half-angle).
            cos_half = blen / 2.0
            cos_half = max(cos_half, 0.2)  # cap miter to avoid spikes at sharp corners
            scale = 1.0 / cos_half
            ox, oy = (bx / blen) * scale, (by / blen) * scale
        new_ring.append((ring[i][0] + ox * distance, ring[i][1] + oy * distance))

    return new_ring


# ---------------------------------------------------------------------------
# Three rendering styles -> SVG path "d" strings
# ---------------------------------------------------------------------------

def path_crisp(rings: list[list[tuple[float, float]]], scale: float) -> str:
    """Straight-edge polygon. Just M/L/Z."""
    parts = []
    for ring in rings:
        ring = collapse_collinear(ring)
        if len(ring) < 3:
            continue
        x0, y0 = ring[0]
        parts.append(f"M{x0 * scale:g} {y0 * scale:g}")
        for x, y in ring[1:]:
            parts.append(f"L{x * scale:g} {y * scale:g}")
        parts.append("Z")
    return " ".join(parts)


def path_smoothed(rings: list[list[tuple[float, float]]], scale: float, chamfer: float) -> str:
    """Chamfer each corner: cut a small bit off each vertex.

    For each consecutive triple (a, b, c), replace b with two points: one
    `chamfer` of the way from b toward a, and one `chamfer` of the way from
    b toward c. This works on the vertex-collapsed (only true corners) ring.
    """
    parts = []
    chamfer = max(0.0, min(0.499, chamfer))
    for ring in rings:
        ring = collapse_collinear(ring)
        n = len(ring)
        if n < 3:
            continue

        new_pts = []
        for i in range(n):
            a = ring[(i - 1) % n]
            b = ring[i]
            c = ring[(i + 1) % n]
            # Distance from b along each side, clamped to half the side length
            # so adjacent chamfers don't cross.
            la = math.dist(a, b)
            lc = math.dist(b, c)
            ta = min(chamfer, 0.499) * la
            tc = min(chamfer, 0.499) * lc
            if la == 0 or lc == 0:
                new_pts.append((b, b))
                continue
            ax = b[0] + (a[0] - b[0]) * (ta / la)
            ay = b[1] + (a[1] - b[1]) * (ta / la)
            cx = b[0] + (c[0] - b[0]) * (tc / lc)
            cy = b[1] + (c[1] - b[1]) * (tc / lc)
            new_pts.append(((ax, ay), (cx, cy)))

        # Emit: from each vertex, line to the next vertex's "incoming" chamfer
        # point, then quadratic curve through the corner to its "outgoing"
        # chamfer point.
        first_in = new_pts[0][0]
        parts.append(f"M{first_in[0] * scale:g} {first_in[1] * scale:g}")
        for i in range(n):
            cur_out = new_pts[i][1]
            corner = ring[i]
            # Quadratic curve through the actual corner.
            parts.append(
                f"Q{corner[0] * scale:g} {corner[1] * scale:g} "
                f"{cur_out[0] * scale:g} {cur_out[1] * scale:g}"
            )
            nxt_in = new_pts[(i + 1) % n][0]
            parts.append(f"L{nxt_in[0] * scale:g} {nxt_in[1] * scale:g}")
        parts.append("Z")
    return " ".join(parts)


def path_organic(rings: list[list[tuple[float, float]]], scale: float, tension: float) -> str:
    """Cubic-bezier Catmull-Rom-ish curve through every corner.

    Smooths the silhouette aggressively. Tension controls how curvy.
    """
    parts = []
    t = max(0.0, min(1.0, tension)) * 0.5  # cap at 0.5 to avoid wild loops
    for ring in rings:
        ring = collapse_collinear(ring)
        n = len(ring)
        if n < 3:
            continue
        # Use midpoints of each edge as on-curve anchors and the corners as
        # control points. This is a classic "round corners of a polygon" trick:
        # M m0 ; for each i: Q corner_i  m_i+1.
        mids = []
        for i in range(n):
            a = ring[i]
            b = ring[(i + 1) % n]
            mids.append((a[0] + (b[0] - a[0]) * (0.5 + 0),
                         a[1] + (b[1] - a[1]) * (0.5 + 0)))

        # When tension is low we want the curve to hug the polygon (less
        # rounding). We blend the corner with the next midpoint.
        # Easiest: scale the "control point" toward the midpoint of mids.
        def blended_corner(corner, m_in, m_out):
            mid_of_mids = ((m_in[0] + m_out[0]) * 0.5,
                           (m_in[1] + m_out[1]) * 0.5)
            # tension=1 => pure corner; tension=0 => midpoint (no rounding bias)
            k = 1.0 - 2.0 * t  # t=0.5 -> k=0 (max round), t=0 -> k=1 (corner)
            return (mid_of_mids[0] + (corner[0] - mid_of_mids[0]) * k,
                    mid_of_mids[1] + (corner[1] - mid_of_mids[1]) * k)

        m0 = mids[0]
        parts.append(f"M{m0[0] * scale:g} {m0[1] * scale:g}")
        for i in range(n):
            corner = ring[(i + 1) % n]
            m_in = mids[i]
            m_out = mids[(i + 1) % n]
            ctrl = blended_corner(corner, m_in, m_out)
            parts.append(
                f"Q{ctrl[0] * scale:g} {ctrl[1] * scale:g} "
                f"{m_out[0] * scale:g} {m_out[1] * scale:g}"
            )
        parts.append("Z")
    return " ".join(parts)


# ---------------------------------------------------------------------------
# SVG assembly
# ---------------------------------------------------------------------------

def rgb_to_hex(rgb: tuple[int, int, int]) -> str:
    return "#{:02x}{:02x}{:02x}".format(*rgb)


def detect_background_label(
    label_img: np.ndarray,
    representatives: list[tuple[int, int, int]],
    explicit_hex: str | None,
) -> int | None:
    """Pick a label index to treat as the background.

    Default: take all border pixels, find the most common label.
    Override: if explicit_hex matches a representative color, use that label.
    """
    if explicit_hex is not None:
        target = explicit_hex.lower().lstrip("#")
        for idx, rgb in enumerate(representatives):
            if rgb_to_hex(rgb)[1:].lower() == target:
                return idx
        return None  # explicit color wasn't found in the image

    h, w = label_img.shape
    if h < 2 or w < 2:
        return None

    border_labels = []
    border_labels.extend(label_img[0, :].tolist())
    border_labels.extend(label_img[-1, :].tolist())
    border_labels.extend(label_img[1:-1, 0].tolist())
    border_labels.extend(label_img[1:-1, -1].tolist())

    if not border_labels:
        return None

    counts: dict[int, int] = {}
    for lbl in border_labels:
        counts[int(lbl)] = counts.get(int(lbl), 0) + 1
    # Modal label; tie-break by lower index for determinism.
    modal = max(counts.items(), key=lambda kv: (kv[1], -kv[0]))[0]
    # Transparent border -> no background fill (let it stay transparent).
    if modal == TRANSPARENT_LABEL:
        return None
    return modal


def build_svg(
    label_img: np.ndarray,
    representatives: list[tuple[int, int, int]],
    blobs: list[dict],
    style: str,
    params: Params,
) -> str:
    h, w = label_img.shape
    out_w = w * params.scale
    out_h = h * params.scale

    # Detect background label, if requested.
    bg_label: int | None = None
    if params.detect_background or params.background_color is not None:
        bg_label = detect_background_label(
            label_img, representatives, params.background_color
        )

    # Group blobs by color for layering.
    by_color: dict[int, list[dict]] = {}
    for b in blobs:
        by_color.setdefault(b["color_idx"], []).append(b)

    # Compute layer ordering. Lower position = painted earlier (bottom).
    def luminance(rgb: tuple[int, int, int]) -> float:
        # Rec. 709 luma
        return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]

    color_indices = list(by_color.keys())

    def sort_key(idx: int):
        rgb = representatives[idx]
        hex_key = rgb_to_hex(rgb)
        # Manually-prioritized colors win and sort by their explicit value.
        if hex_key in params.color_priority:
            return (0, params.color_priority[hex_key], 0, idx)
        # Otherwise, fall through to the rule.
        if params.layer_order == "luminance_asc":
            return (1, luminance(rgb), 0, idx)
        if params.layer_order == "luminance_desc":
            return (1, -luminance(rgb), 0, idx)
        if params.layer_order == "frequency_asc":
            count = sum(len(b["pixels"]) for b in by_color[idx])
            return (1, count, 0, idx)
        if params.layer_order == "frequency_desc":
            count = sum(len(b["pixels"]) for b in by_color[idx])
            return (1, -count, 0, idx)
        if params.layer_order == "manual":
            # Anything not in color_priority goes last, in stable index order.
            return (2, 0, 0, idx)
        return (1, 0, 0, idx)

    color_indices.sort(key=sort_key)

    layer_svg = []

    # Emit the background first if we have one. It's just a tile-sized rect.
    # Smoothing/curving doesn't apply to it -- this keeps the tile's outer
    # edge perfectly square regardless of style.
    if bg_label is not None:
        bg_rgb = representatives[bg_label]
        bg_hex = rgb_to_hex(bg_rgb)
        layer_svg.append(
            f'  <g class="layer-bg layer-{bg_hex[1:]}" data-color="{bg_hex}" data-role="background">\n'
            f'    <rect x="0" y="0" width="{out_w}" height="{out_h}" fill="{bg_hex}"/>\n'
            f'  </g>'
        )

    for color_idx in color_indices:
        if color_idx == bg_label:
            continue  # already emitted as a flat rect
        color = representatives[color_idx]
        hex_color = rgb_to_hex(color)
        d_parts = []
        for blob in by_color[color_idx]:
            rings = blob_to_polygons(blob["pixels"])
            # Geometric fattening: outset each ring before path fitting.
            if params.fatten > 0 and params.fatten_mode == "geometry":
                rings = [offset_ring(r, params.fatten) for r in rings]
            if style == "crisp":
                d = path_crisp(rings, params.scale)
            elif style == "smoothed":
                d = path_smoothed(rings, params.scale, params.chamfer)
            elif style == "organic":
                d = path_organic(rings, params.scale, params.curve_tension)
            else:
                raise ValueError(f"unknown style {style}")
            if d:
                d_parts.append(d)
        if not d_parts:
            continue

        # Stroke fattening: same-color stroke around the path. Cheap and also
        # softens outer corners.
        stroke_attr = ""
        if params.fatten > 0 and params.fatten_mode == "stroke":
            stroke_w = params.fatten * 2 * params.scale
            stroke_attr = (
                f' stroke="{hex_color}" stroke-width="{stroke_w:g}"'
                f' stroke-linejoin="round" stroke-linecap="round"'
            )

        layer_svg.append(
            f'  <g class="layer-{hex_color[1:]}" data-color="{hex_color}">\n'
            f'    <path fill="{hex_color}" fill-rule="evenodd"{stroke_attr} d="{" ".join(d_parts)}"/>\n'
            f'  </g>'
        )

    svg = (
        f'<svg xmlns="http://www.w3.org/2000/svg" '
        f'viewBox="0 0 {out_w} {out_h}" width="{out_w}" height="{out_h}" '
        f'shape-rendering="geometricPrecision">\n'
        f'  <defs>\n'
        f'    <clipPath id="tile-bounds">\n'
        f'      <rect x="0" y="0" width="{out_w}" height="{out_h}"/>\n'
        f'    </clipPath>\n'
        f'  </defs>\n'
        f'  <g clip-path="url(#tile-bounds)">\n'
        + "\n".join(layer_svg)
        + "\n  </g>\n</svg>\n"
    )
    return svg


# ---------------------------------------------------------------------------
# Top-level convenience
# ---------------------------------------------------------------------------

def process(image_path: str | Path, params: Params | None = None) -> dict[str, str]:
    """Return {style: svg_text} for the three styles."""
    params = params or Params()
    img = Image.open(image_path).convert("RGBA")
    arr = np.array(img)
    label_img, reps = quantize(arr, params)
    blobs = find_blobs(label_img, params.diagonal_connect)
    return {
        style: build_svg(label_img, reps, blobs, style, params)
        for style in ("crisp", "smoothed", "organic")
    }
