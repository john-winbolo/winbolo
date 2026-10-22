#!/usr/bin/env python3
"""Generate the isometric test sprites in data/svg/.

Every sprite is built from simple low-poly solids (boxes, prisms and
cylinders) and drawn through a 2:1 dimetric camera:

    screen x = 16 + 16 * (x - y)
    screen y = 24 +  8 * (x + y) - 16 * z

at density 1, with x east, y south and z up, all in cell units, and the
origin at the centre of the cell's ground diamond. World north (-y) is
screen up-right. The camera looks along -(1, 1, 1), so a face is visible
when its normal points along +(1, 1, 1). Light comes from the upper left
of the screen: tops get the lightest tone, faces that turn towards screen
left the middle tone and faces that turn towards screen right the darkest.

Standing cells are 32x32 at density 1 and are written at density 4
(128x128). Face and band textures are 32x16, written as 128x64.

Team colours are sampled from the classic top-down sprites in the same
folder so the two sets stay in step.

Usage: tools/gen_iso_sprites.py [--out DIR] [--sheet PATH]
"""

import argparse
import math
import os
import random
from collections import Counter

from PIL import Image, ImageDraw

DENSITY = 4
CELL = 32 * DENSITY
FACE_W = 32 * DENSITY
FACE_H = 16 * DENSITY
VIEW = (1.0, 1.0, 1.0)

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SVG_DIR = os.path.join(REPO, "data", "svg")


# ---------------------------------------------------------------------------
# Colour helpers
# ---------------------------------------------------------------------------

def mix(c, other, t):
    return tuple(int(round(a + (b - a) * t)) for a, b in zip(c, other))


def scale(c, f):
    return tuple(max(0, min(255, int(round(a * f)))) for a in c)


def tones(base):
    """Top, lit-left and shaded-right tones for one base colour."""
    return (mix(base, (255, 255, 255), 0.25), scale(base, 0.85),
            scale(base, 0.6))


def sample_colours(name, skip=()):
    """Opaque colours of a classic sprite, most common first.

    Near-black pixels and any colour in skip are left out.
    """
    path = os.path.join(SVG_DIR, name + ".png")
    im = Image.open(path).convert("RGBA")
    counts = Counter()
    for y in range(im.height):
        for x in range(im.width):
            r, g, b, a = im.getpixel((x, y))
            if a < 128 or r + g + b < 60:
                continue
            if (r, g, b) in skip:
                continue
            counts[(r, g, b)] += 1
    return [c for c, _ in counts.most_common()]


def sample_dominant(name, skip=()):
    return sample_colours(name, skip)[0]


class Palette:
    """Colours taken from the classic sprites."""

    def __init__(self):
        # Tank sprites share white treads and a grey body; the colour
        # left once those are removed is the team colour. The self tank
        # has nothing else, so its dominant non-black colour (white) is
        # its hull colour.
        self.tank_self = sample_dominant("tank_self_00")
        shared = set(sample_colours("tank_self_00"))
        self.tank_good = sample_dominant("tank_good_00", shared)
        self.tank_evil = sample_dominant("tank_evil_00", shared)
        self.tread = [c for c in sample_colours("tank_self_00")
                      if c != self.tank_self][0]

        # Pillboxes: dominant colour is the shared stone body, the team
        # colour is what is left after removing it.
        self.pill_stone = sample_dominant("pillbox_good_15")
        self.pill_good = sample_dominant("pillbox_good_15",
                                         {self.pill_stone})
        self.pill_evil = sample_dominant("pillbox_evil_15",
                                         {self.pill_stone})

        # Bases: yellow pad shared by all three, team colour on the ring.
        self.base_pad = sample_dominant("base_good")
        self.base_good = sample_dominant("base_good", {self.base_pad})
        self.base_evil = sample_dominant("base_evil", {self.base_pad})
        self.base_neutral = sample_dominant("base_neutral", {self.base_pad})

        lgm = sample_colours("lgm_frame0")
        self.lgm_body = lgm[0]
        self.lgm_head = lgm[1] if len(lgm) > 1 else (255, 0, 0)
        chute = sample_colours("lgm_helicopter")
        self.chute_a = chute[0]
        self.chute_b = chute[1] if len(chute) > 1 else (247, 247, 247)

        bricks = sample_colours("building_single")
        self.brick_mortar = bricks[0]
        self.brick = bricks[1]
        self.brick_light = bricks[2]

        self.boat = (150, 100, 55)


# ---------------------------------------------------------------------------
# Geometry
# ---------------------------------------------------------------------------

def project(p):
    x, y, z = p
    sx = 16 + 16 * (x - y)
    sy = 24 + 8 * (x + y) - 16 * z
    return (sx * DENSITY, sy * DENSITY)


def sub(a, b):
    return (a[0] - b[0], a[1] - b[1], a[2] - b[2])


def dot(a, b):
    return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]


def centroid(pts):
    n = len(pts)
    return (sum(p[0] for p in pts) / n, sum(p[1] for p in pts) / n,
            sum(p[2] for p in pts) / n)


def newell(pts):
    nx = ny = nz = 0.0
    for i, a in enumerate(pts):
        b = pts[(i + 1) % len(pts)]
        nx += (a[1] - b[1]) * (a[2] + b[2])
        ny += (a[2] - b[2]) * (a[0] + b[0])
        nz += (a[0] - b[0]) * (a[1] + b[1])
    length = math.sqrt(nx * nx + ny * ny + nz * nz) or 1.0
    return (nx / length, ny / length, nz / length)


class Solid:
    """A convex solid: a list of faces and one base colour per face.

    Normals are computed from the points and turned to face away from the
    solid's centre, so faces can be listed in any winding.
    """

    def __init__(self, faces, colour, layer=0, outline=True):
        self.faces = []
        allpts = [p for f in faces for p in f]
        self.centre = centroid(allpts)
        for f in faces:
            n = newell(f)
            if dot(n, sub(centroid(f), self.centre)) < 0:
                n = (-n[0], -n[1], -n[2])
            self.faces.append((f, n, colour))
        self.layer = layer
        self.outline = outline

    def depth(self):
        return dot(self.centre, VIEW)


def prism(base_pts, z0, z1, colour, layer=0):
    """Vertical prism over a convex ground polygon."""
    bottom = [(x, y, z0) for x, y in base_pts]
    top = [(x, y, z1) for x, y in base_pts]
    faces = [top, bottom]
    n = len(base_pts)
    for i in range(n):
        j = (i + 1) % n
        faces.append([bottom[i], bottom[j], top[j], top[i]])
    return Solid(faces, colour, layer)


def rect(cx, cy, w, l):
    hw, hl = w / 2, l / 2
    return [(cx - hw, cy - hl), (cx + hw, cy - hl), (cx + hw, cy + hl),
            (cx - hw, cy + hl)]


def circle(cx, cy, r, n=12, phase=0.0):
    return [(cx + r * math.cos(phase + 2 * math.pi * i / n),
             cy + r * math.sin(phase + 2 * math.pi * i / n))
            for i in range(n)]


def rotate2(pts, angle):
    """Rotate ground points clockwise (seen from above) by angle radians.

    With y pointing south, (0, -1) turned by 90 degrees becomes (1, 0).
    """
    c, s = math.cos(angle), math.sin(angle)
    return [(x * c - y * s, x * s + y * c) for x, y in pts]


def face_tone(colour, normal):
    top, lit, shaded = tones(colour)
    if normal[2] > 0.5:
        return top
    # Screen x of the normal: negative turns towards screen left.
    if normal[0] - normal[1] < 0:
        return lit
    return shaded


def outline_image(im, factor=0.45):
    """Add a 1 pixel darker border around the opaque pixels of im."""
    w, h = im.size
    src = im.load()
    out = im.copy()
    dst = out.load()
    for y in range(h):
        for x in range(w):
            if src[x, y][3] != 0:
                continue
            for dx, dy in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                nx, ny = x + dx, y + dy
                if 0 <= nx < w and 0 <= ny < h and src[nx, ny][3] != 0:
                    r, g, b, _ = src[nx, ny]
                    dst[x, y] = scale((r, g, b), factor) + (255,)
                    break
    return out


def render(solids, extras=None):
    """Draw solids back to front into a standing cell.

    Solids are drawn by layer, then by distance along the view direction.
    Each solid gets its own 1 pixel dark outline so parts stay distinct.
    extras(draw) may add flat details after a given layer; it is a list of
    (layer, function) pairs.
    """
    extras = extras or []
    canvas = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
    order = sorted(solids, key=lambda s: (s.layer, s.depth()))
    layers = sorted({s.layer for s in solids} | {l for l, _ in extras})
    for layer in layers:
        for s in order:
            if s.layer != layer:
                continue
            part = Image.new("RGBA", (CELL, CELL), (0, 0, 0, 0))
            d = ImageDraw.Draw(part)
            for pts, n, colour in s.faces:
                if dot(n, VIEW) <= 1e-6:
                    continue
                d.polygon([project(p) for p in pts],
                          fill=face_tone(colour, n) + (255,))
            if s.outline:
                part = outline_image(part)
            canvas.alpha_composite(part)
        for l, fn in extras:
            if l == layer:
                fn(canvas)
    return canvas


# ---------------------------------------------------------------------------
# Tanks
# ---------------------------------------------------------------------------

def tank_solids(hull, tread, angle, lift=0.0, size=1.0):
    k = size

    def ground(pts):
        return rotate2([(x * k, y * k) for x, y in pts], angle)

    z = lambda v: lift + v * k
    solids = [
        # Treads either side of the hull.
        prism(ground(rect(-0.25, 0.02, 0.1, 0.72)), z(0.0), z(0.15),
              tread, 0),
        prism(ground(rect(0.25, 0.02, 0.1, 0.72)), z(0.0), z(0.15),
              tread, 0),
        # Hull.
        prism(ground(rect(0.0, 0.02, 0.4, 0.66)), z(0.03), z(0.21),
              hull, 0),
        # Turret.
        prism(ground(circle(0.0, 0.08, 0.16, 10)), z(0.21), z(0.33),
              hull, 1),
        # Barrel, pointing along the facing.
        prism(ground(rect(0.0, -0.24, 0.07, 0.48)), z(0.24), z(0.3),
              scale(hull, 0.8), 1),
    ]
    return solids


def boat_solids(angle):
    # Pointed bow towards the facing, square stern.
    outline = [(0.0, -0.55), (0.3, -0.25), (0.3, 0.42), (-0.3, 0.42),
               (-0.3, -0.25)]
    return [prism(rotate2(outline, angle), 0.0, 0.1, PAL.boat, -1)]


def make_tank(colour, frame, boat=False):
    angle = math.radians(frame * 22.5)
    if boat:
        solids = boat_solids(angle) + tank_solids(colour, PAL.tread, angle,
                                                  lift=0.1, size=0.85)
    else:
        solids = tank_solids(colour, PAL.tread, angle)
    return render(solids)


# ---------------------------------------------------------------------------
# Pillboxes
# ---------------------------------------------------------------------------

def make_pillbox(team, state):
    f = state / 15.0
    rng = random.Random(1000 + state)
    darken = 0.5 + 0.5 * f
    stone = scale(PAL.pill_stone, darken)
    band = scale(team, darken)
    height = 0.14 + 0.26 * f
    radius = 0.36
    n = 12
    phase = math.pi / n
    solids = [
        prism(circle(0, 0, radius, n, phase), 0.0, height, stone, 0),
    ]
    if state > 0:
        # Low cupola in the middle of the roof, shrinking with damage.
        solids.append(prism(circle(0, 0, 0.11, 8, math.pi / 8), height,
                            height + 0.03 + 0.05 * f, stone, 2))

    def roof_ring(canvas):
        d = ImageDraw.Draw(canvas)
        colour = tones(band)[0] + (255,)
        segs = 16
        r0, r1 = 0.17, 0.29
        for i in range(segs):
            if state < 15 and rng.random() < (15 - state) / 25.0:
                continue          # broken pieces of the team ring
            a0 = 2 * math.pi * i / segs
            a1 = 2 * math.pi * (i + 1) / segs
            pts = [(r0 * math.cos(a0), r0 * math.sin(a0), height),
                   (r1 * math.cos(a0), r1 * math.sin(a0), height),
                   (r1 * math.cos(a1), r1 * math.sin(a1), height),
                   (r0 * math.cos(a1), r0 * math.sin(a1), height)]
            d.polygon([project(p) for p in pts], fill=colour)

    def details(canvas):
        d = ImageDraw.Draw(canvas)
        # Firing slots on the four compass sides; only those facing the
        # camera are drawn.
        slot_z0 = height - 0.16
        slot_z1 = height - 0.09
        if state > 0 and slot_z0 > 0.01:
            for a in (0, 90, 180, 270):
                ang = math.radians(a)
                nx, ny = math.sin(ang), -math.cos(ang)
                if nx + ny <= 0:
                    continue
                tx, ty = -ny, nx
                r = radius * math.cos(math.pi / n) + 0.005
                cx, cy = nx * r, ny * r
                w = 0.09
                pts = [(cx - tx * w, cy - ty * w, slot_z0),
                       (cx + tx * w, cy + ty * w, slot_z0),
                       (cx + tx * w, cy + ty * w, slot_z1),
                       (cx - tx * w, cy - ty * w, slot_z1)]
                d.polygon([project(p) for p in pts], fill=(20, 16, 20, 255))
        # Cracks on the visible side, more of them as damage grows.
        cracks = (15 - state + 2) // 3
        for _ in range(cracks):
            a = rng.uniform(60, 210)
            ang = math.radians(a)
            nx, ny = math.sin(ang), -math.cos(ang)
            r = radius * 0.98
            zt = height - 0.02
            pts = [(nx * r, ny * r, zt)]
            zz = zt
            for _ in range(3):
                zz -= rng.uniform(0.03, 0.07)
                ang += math.radians(rng.uniform(-12, 12))
                nx, ny = math.sin(ang), -math.cos(ang)
                pts.append((nx * r, ny * r, max(zz, 0.01)))
            d.line([project(p) for p in pts], fill=(25, 20, 18, 255),
                   width=DENSITY // 2 + 1)
        if state == 0:
            # Rubble on the top of an empty pillbox.
            for _ in range(6):
                x = rng.uniform(-0.25, 0.25)
                y = rng.uniform(-0.25, 0.25)
                p = project((x, y, height))
                s = DENSITY
                d.rectangle([p[0] - s, p[1] - s // 2, p[0] + s, p[1] + s // 2],
                            fill=scale(stone, 0.55) + (255,))

    return render(solids, [(0, roof_ring), (1, details)])


# ---------------------------------------------------------------------------
# Bases
# ---------------------------------------------------------------------------

def make_base(ring_colour):
    pad = prism(rect(0, 0, 0.84, 0.84), 0.0, 0.08, PAL.base_pad, 0)
    hub = prism(circle(0, 0, 0.1, 8, math.pi / 8), 0.08, 0.14,
                PAL.base_pad, 2)
    solids = [pad, hub]

    def ring(canvas):
        d = ImageDraw.Draw(canvas)
        top = tones(ring_colour)[0] + (255,)
        n = 16
        r0, r1 = 0.22, 0.32
        z = 0.08
        for i in range(n):
            a0 = 2 * math.pi * i / n
            a1 = 2 * math.pi * (i + 1) / n
            pts = [(r0 * math.cos(a0), r0 * math.sin(a0), z),
                   (r1 * math.cos(a0), r1 * math.sin(a0), z),
                   (r1 * math.cos(a1), r1 * math.sin(a1), z),
                   (r0 * math.cos(a1), r0 * math.sin(a1), z)]
            d.polygon([project(p) for p in pts], fill=top)

    return render(solids, [(1, ring)])


# ---------------------------------------------------------------------------
# Builder
# ---------------------------------------------------------------------------

def figure_solids(frame, lift=0.0):
    """A small two-legged figure facing the camera (towards +x, +y)."""
    body = PAL.lgm_body
    legs = scale(body, 0.6)
    # Walking stride along the facing direction.
    stride = {0: 0.0, 1: 0.05, 2: -0.05}[frame]
    fx, fy = 0.707, 0.707          # facing towards the camera
    sx, sy = 0.707, -0.707         # to the figure's left on screen

    def at(off_side, off_fwd, w, l):
        cx = sx * off_side + fx * off_fwd
        cy = sy * off_side + fy * off_fwd
        return [(cx + px, cy + py) for px, py in
                rotate2(rect(0, 0, w, l), math.radians(-45))]

    z = lambda v: lift + v
    return [
        prism(at(-0.05, stride, 0.05, 0.05), z(0.0), z(0.16), legs, 0),
        prism(at(0.05, -stride, 0.05, 0.05), z(0.0), z(0.16), legs, 0),
        prism(at(0.0, 0.0, 0.16, 0.09), z(0.16), z(0.34), body, 1),
        prism(at(-0.1, 0.0, 0.04, 0.04), z(0.2), z(0.32), body, 1),
        prism(at(0.1, 0.0, 0.04, 0.04), z(0.2), z(0.32), body, 1),
        prism(circle(0, 0, 0.055, 8), z(0.34), z(0.44), PAL.lgm_head, 2),
    ]


def make_lgm(frame):
    return render(figure_solids(frame))


def make_helicopter():
    lift = 0.3
    solids = figure_solids(0, lift)
    # Parachute: alternating canopy panels around a low dome.
    n = 8
    r = 0.42
    zc = 1.12
    zr = 0.88
    for i in range(n):
        a0 = 2 * math.pi * i / n
        a1 = 2 * math.pi * (i + 1) / n
        p0 = (r * math.cos(a0), r * math.sin(a0), zr)
        p1 = (r * math.cos(a1), r * math.sin(a1), zr)
        top = (0.0, 0.0, zc)
        colour = PAL.chute_a if i % 2 == 0 else PAL.chute_b
        # Under side, so the canopy has some body seen from below.
        u0 = (r * 0.8 * math.cos(a0), r * 0.8 * math.sin(a0), zr + 0.05)
        u1 = (r * 0.8 * math.cos(a1), r * 0.8 * math.sin(a1), zr + 0.05)
        s = Solid([[p0, p1, top], [p0, p1, u1, u0], [u0, u1, top]], colour,
                  3, outline=False)
        solids.append(s)

    def strings(canvas):
        d = ImageDraw.Draw(canvas)
        hand = (0.0, 0.0, lift + 0.32)
        for a in (math.pi / 4, 3 * math.pi / 4, 5 * math.pi / 4,
                  7 * math.pi / 4):
            p = (r * math.cos(a), r * math.sin(a), zr)
            d.line([project(p), project(hand)], fill=(40, 40, 40, 255),
                   width=1)

    return render(solids, [(2, strings)])


# ---------------------------------------------------------------------------
# Face and band textures
# ---------------------------------------------------------------------------

def bricks(base, mortar, light, holes=0, seed=0):
    rng = random.Random(seed)
    im = Image.new("RGBA", (FACE_W, FACE_H), mortar + (255,))
    d = ImageDraw.Draw(im)
    s = DENSITY
    bw, bh = 8, 4
    for row in range(4):
        off = 0 if row % 2 == 0 else -bw // 2
        x = off
        while x < 32:
            x0 = max(x, 0) * s
            x1 = min(x + bw, 32) * s - s
            y0 = row * bh * s
            y1 = (row + 1) * bh * s - s
            tint = rng.uniform(0.9, 1.08)
            if x1 > x0:
                d.rectangle([x0, y0, x1, y1], fill=scale(base, tint) + (255,))
                d.rectangle([x0, y0, x1, y0 + s - 1],
                            fill=scale(light, tint) + (255,))
            x += bw
    for _ in range(holes):
        cx = rng.randrange(2, 30) * s
        cy = rng.randrange(2, 14) * s
        r = rng.randrange(2, 4) * s
        pts = [(cx + r * math.cos(a) * rng.uniform(0.6, 1.2),
                cy + r * math.sin(a) * rng.uniform(0.6, 1.2))
               for a in [i * math.pi / 4 for i in range(8)]]
        d.polygon(pts, fill=(18, 14, 14, 255))
    return im


def make_face_building():
    return bricks(PAL.brick, PAL.brick_mortar, PAL.brick_light, seed=1)


def make_face_shot_building():
    im = bricks(scale(PAL.brick, 0.7), scale(PAL.brick_mortar, 0.7),
                scale(PAL.brick_light, 0.7), holes=5, seed=2)
    d = ImageDraw.Draw(im)
    rng = random.Random(3)
    for _ in range(4):
        x = rng.randrange(0, FACE_W)
        y = 0
        pts = [(x, y)]
        while y < FACE_H:
            y += rng.randrange(4, 12)
            x += rng.randrange(-8, 9)
            pts.append((x, y))
        d.line(pts, fill=(18, 14, 14, 255), width=2)
    return im


def make_face_pill():
    stone = PAL.pill_stone
    im = Image.new("RGBA", (FACE_W, FACE_H), scale(stone, 0.55) + (255,))
    d = ImageDraw.Draw(im)
    s = DENSITY
    for row in range(2):
        off = 0 if row == 0 else -6
        x = off
        while x < 32:
            x0 = max(x, 0) * s
            x1 = min(x + 12, 32) * s - s
            y0 = row * 8 * s
            y1 = (row + 1) * 8 * s - s
            if x1 > x0:
                d.rectangle([x0, y0, x1, y1], fill=stone + (255,))
                d.rectangle([x0, y0, x1, y0 + s - 1],
                            fill=mix(stone, (255, 255, 255), 0.3) + (255,))
            x += 12
    # Firing slot across the middle.
    d.rectangle([11 * s, 6 * s, 21 * s - 1, 9 * s - 1], fill=(20, 16, 20, 255))
    return im


def make_face_base():
    pad = PAL.base_pad
    im = Image.new("RGBA", (FACE_W, FACE_H), scale(pad, 0.85) + (255,))
    d = ImageDraw.Draw(im)
    s = DENSITY
    d.rectangle([0, 0, FACE_W - 1, 2 * s - 1], fill=pad + (255,))
    d.rectangle([0, FACE_H - 2 * s, FACE_W - 1, FACE_H - 1],
                fill=scale(pad, 0.6) + (255,))
    # Hazard stripes.
    for i in range(-2, 10):
        x = i * 4 * s
        d.polygon([(x, 4 * s), (x + 2 * s, 4 * s), (x + 6 * s, 12 * s - 1),
                   (x + 4 * s, 12 * s - 1)], fill=scale(pad, 0.45) + (255,))
    for x in (2, 30):
        d.rectangle([(x - 1) * s, 7 * s, x * s + s - 1, 9 * s - 1],
                    fill=scale(pad, 0.35) + (255,))
    return im


def make_forest_band():
    bark = (101, 67, 33)
    im = Image.new("RGBA", (FACE_W, FACE_H), bark + (255,))
    d = ImageDraw.Draw(im)
    rng = random.Random(7)
    s = DENSITY
    x = 0
    while x < 32:
        w = rng.choice((1, 1, 2))
        d.rectangle([x * s, 0, (x + w) * s - 1, FACE_H - 1],
                    fill=scale(bark, 0.45) + (255,))
        if x + w + 1 < 32:
            d.rectangle([(x + w) * s, 0, (x + w) * s + s - 1, FACE_H - 1],
                        fill=mix(bark, (255, 255, 255), 0.15) + (255,))
        x += w + rng.randrange(3, 5)
    return im


# ---------------------------------------------------------------------------
# Output
# ---------------------------------------------------------------------------

def build_all():
    """Return [(family, name, image)] for every sprite in the contract."""
    out = []
    tanks = (("self", PAL.tank_self), ("good", PAL.tank_good),
             ("evil", PAL.tank_evil))
    for team, colour in tanks:
        for nn in range(16):
            out.append(("tank_" + team, "iso_tank_%s_%02d" % (team, nn),
                        make_tank(colour, nn)))
    for team, colour in tanks:
        for nn in range(16):
            out.append(("tank_%sboat" % team,
                        "iso_tank_%sboat_%02d" % (team, nn),
                        make_tank(colour, nn, boat=True)))
    for team, colour in (("good", PAL.pill_good), ("evil", PAL.pill_evil)):
        for nn in range(16):
            out.append(("pillbox_" + team, "iso_pillbox_%s_%02d" % (team, nn),
                        make_pillbox(colour, nn)))
    for team, colour in (("good", PAL.base_good), ("evil", PAL.base_evil),
                         ("neutral", PAL.base_neutral)):
        out.append(("base", "iso_base_" + team, make_base(colour)))
    for nn in range(3):
        out.append(("lgm", "iso_lgm_frame%d" % nn, make_lgm(nn)))
    out.append(("lgm", "iso_lgm_helicopter", make_helicopter()))
    out.append(("face", "iso_face_building", make_face_building()))
    out.append(("face", "iso_face_shot_building", make_face_shot_building()))
    out.append(("face", "iso_face_pill", make_face_pill()))
    out.append(("face", "iso_face_base", make_face_base()))
    out.append(("face", "iso_forest_band", make_forest_band()))
    return out


def diamond_backdrop(w, h, colour):
    """A density-1 sized tile with the ground diamond drawn in colour."""
    im = Image.new("RGBA", (w, h), (0, 0, 0, 0))
    d = ImageDraw.Draw(im)
    k = w / 32.0
    d.polygon([(16 * k, 16 * k), (32 * k, 24 * k), (16 * k, 32 * k),
               (0, 24 * k)], fill=colour + (255,))
    return im


def contact_sheet(items, path):
    grass = (40, 120, 40)
    water = (40, 70, 160)
    bg = (60, 60, 60)
    pad = 6
    label_h = 14
    families = []
    for fam, name, img in items:
        if not families or families[-1][0] != fam:
            families.append((fam, []))
        families[-1][1].append((name, img))

    rows = []
    for fam, entries in families:
        n = len(entries)
        w1 = max(img.width // DENSITY for _, img in entries)
        h1 = max(img.height // DENSITY for _, img in entries)
        w4 = max(img.width for _, img in entries)
        h4 = max(img.height for _, img in entries)
        width = pad + n * (w4 + pad)
        height = label_h + h1 + pad + h4 + pad + label_h
        row = Image.new("RGBA", (width, height), bg + (255,))
        d = ImageDraw.Draw(row)
        d.text((pad, 2), "%s  (1x above, 4x below)" % fam,
               fill=(255, 255, 255, 255))
        for i, (name, img) in enumerate(entries):
            x = pad + i * (w4 + pad)
            ground = grass if i % 2 == 0 else water
            small = img.resize((img.width // DENSITY, img.height // DENSITY),
                               Image.NEAREST)
            if img.height == img.width:
                tile1 = diamond_backdrop(small.width, small.height, ground)
                tile4 = diamond_backdrop(img.width, img.height, ground)
            else:
                tile1 = Image.new("RGBA", small.size, ground + (255,))
                tile4 = Image.new("RGBA", img.size, ground + (255,))
            tile1.alpha_composite(small)
            tile4.alpha_composite(img)
            row.alpha_composite(tile1, (x, label_h))
            row.alpha_composite(tile4, (x, label_h + h1 + pad))
            short = name.replace("iso_", "")
            short = short.split("_")[-1] if fam != "face" else short
            d.text((x, label_h + h1 + pad + h4 + 1), short,
                   fill=(220, 220, 220, 255))
        rows.append(row)
    width = max(r.width for r in rows)
    height = sum(r.height for r in rows) + pad * len(rows)
    sheet = Image.new("RGBA", (width, height), (30, 30, 30, 255))
    y = 0
    for r in rows:
        sheet.alpha_composite(r, (0, y))
        y += r.height + pad
    os.makedirs(os.path.dirname(path), exist_ok=True)
    sheet.save(path)


def main():
    global SVG_DIR, PAL
    ap = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    ap.add_argument("--out", default=SVG_DIR,
                    help="folder to write sprites to (default data/svg)")
    ap.add_argument("--sheet", default=None,
                    help="also write a contact sheet PNG to this path")
    args = ap.parse_args()
    PAL = Palette()
    items = build_all()
    os.makedirs(args.out, exist_ok=True)
    for _, name, img in items:
        img.save(os.path.join(args.out, name + ".png"))
    print("wrote %d sprites to %s" % (len(items), args.out))
    if args.sheet:
        contact_sheet(items, args.sheet)
        print("wrote contact sheet %s" % args.sheet)


PAL = None

if __name__ == "__main__":
    main()
