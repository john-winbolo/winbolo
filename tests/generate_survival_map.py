"""Generate 'Survival' — circular co-op survival scenario map.

WARNING: data/maps/Survival.map is NOT byte-reproducible from this
script any more — the shipped file carries later hand edits (a few
extra ring-road tiles, some swamp turned back to grass, river inside
the puddle). Running this OVERWRITES those. It is kept as the readable
statement of the layout and as the starting point for a from-scratch
rebuild; incremental changes are made against the shipped file.

Rings (center 128,128), sized to sit near Schism Toy III's ~55x55
footprint (the spawn-tiering rules put a floor on the ocean start ring:
starts must be >9 Chebyshev from any base, so the island can't get much
smaller than this ~67x67):
  r <= 2.5         inner deep-sea puddle (5 tiles across), 6 human starts
                   at r=2 (60 deg apart) — the scenario's on_choose_start
                   hook pins humans here and everyone else to the outer
                   ring, so no base needs to sit within tiering range
  2.5 < r <= 29    land: grass + 10-fold-symmetric forest clumps
                   - 6 human bases at r=6 (~3 tiles off the puddle edge,
                     owners = slots 0..5)
                   - 6 DEAD pills at r=8, one just beyond each human base,
                     owners = slots 0..5 (the defenders' starting pills —
                     dead, so they must be scooped, placed and repaired)
                   - 10 DEAD neutral pills parked out at r=26, the ring
                     the horde's bases used to sit on; the scenario deals
                     one to each wave-1 tank
                   - 4 bot bases at r=13, just outside the ring road, on
                     the 0/72/180/252 deg spokes (owners 15/13/10/8 — the
                     slot whose ocean start shares the spoke). The horde
                     used to hold a full ring of 10 out at r=26; those
                     never got fought over, so the ring was cut down to
                     the two that had been pushed forward plus a matching
                     pair due east and due west.
  r > 29           open sea; 10 bot starts at r=33 (one per 36 deg spoke —
                   still one per wave slot, four of which own a base)

Everything stays inside the unmined 21..235 zone (max extent 128+33=161).
Run-encoding follows mapProcessRun exactly (nibble 0-7 = that many+1
different tiles follow; 8-15 = run of len-6 same tiles).
"""
import math

# Repo-relative output (this script lives in <repo>/tests/), so the
# generator works identically from any clone (winbolo, winbolo2, winbolo3).
import os
DST = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "data", "maps", "Survival.map"))

DEEP = 0xFF
SWAMP, ROAD, FOREST, GRASS = 2, 4, 5, 7

C = 128
R_LAKE = 2.5
R_LAND = 29
R_HSTART, R_HBASE = 2, 6
R_BBASE, R_BSTART = 25, 33   # horde bases ring the shore (r=25), starts at r=33
R_PILL = 8               # 6 dead defender pills, one beyond each base
R_PILL_OUT = 26          # 10 dead neutral pills for the wave-1 tanks

def pol(r, deg):
    a = math.radians(deg)
    return (C + int(round(r * math.cos(a))), C + int(round(r * math.sin(a))))

human_angles = [k * 60 for k in range(6)]
bot_angles   = [k * 36 for k in range(10)]

human_starts = [pol(R_HSTART, a) for a in human_angles]
human_bases  = [pol(R_HBASE,  a) for a in human_angles]      # owners 0..5
# The horde's bases all push in close — just outside the ring road — so
# their bots treat the collision zone with the core as home turf. Only
# four spokes carry one: 0 (east), 72, 180 (west), 252. Point-symmetric
# through the island center, and mirror-symmetric about the 36/216 spoke.
# Each is owned by the slot whose ocean start sits on the SAME spoke
# (on_choose_start pins slot p to start 22-p, and slot 15-i starts on
# spoke i), so its bot comes ashore aimed at its own base.
# 8 of the 10 spokes carry a horde base (point-symmetric: skip 4 and 9).
# The influence tail grows ~20 tiles from each, so the ring's claim meets
# the defenders' core claim and forms a front line around the island.
FORWARD_SPOKES = [0, 1, 2, 3, 5, 6, 7, 8]   # zero-based index into bot_angles
bot_bases  = [pol(R_BBASE, bot_angles[i]) for i in FORWARD_SPOKES]
bot_owners = [15 - i for i in FORWARD_SPOKES]                # 15, 13, 10, 8
bot_starts   = [pol(R_BSTART, a) for a in bot_angles]
pills = ([pol(R_PILL, a) for a in human_angles] +        # 1..6: base 4+k's pill
         [pol(R_PILL_OUT, a + 18) for a in bot_angles])  # 7..16: wave-1 carry

# --- terrain -----------------------------------------------------------
# 10-fold rotational symmetry: hash polar cells of (r, angle mod 36 deg).
def cell_hash(rb, ab):
    h = (rb * 73856093) ^ (ab * 19349663)
    h = (h ^ (h >> 13)) * 0x5bd1e995 & 0xFFFFFFFF
    return (h ^ (h >> 15)) & 0xFFFF

def terrain(x, y):
    dx, dy = x - C, y - C
    r = math.hypot(dx, dy)
    if r <= R_LAKE or r > R_LAND:
        return DEEP
    ang = math.degrees(math.atan2(dy, dx)) % 36.0     # 10-fold symmetry
    rb = int(r // 2)
    ab = int(ang * (r / 14.0))                        # ~constant arc length
    h = cell_hash(rb, ab)
    if r <= R_HBASE + 1:
        return GRASS                                  # clear human base ring
    if r >= 24:
        # Light cover out wide — kept at the historical belt boundary
        # (the old bot-base ring at r=26, minus the 2-tile skirt), NOT
        # tied to R_BBASE: the bases moving inward must not deforest
        # the island's midfield.
        return GRASS if h % 10 < 8 else FOREST
    # main belt: decent forest for fort building + a few swamp accents
    m = h % 100
    if m < 34:
        return FOREST
    if m < 38:
        return SWAMP
    return GRASS

grid = [[terrain(x, y) for y in range(256)] for x in range(256)]

# ring road at the human perimeter (r ~ 10) for looks + mobility.
# Bridge diagonal steps: the polar march can hop kitty-corner at the
# quadrant mirror points, leaving roads that only touch at corners —
# insert one orthogonal tile so the ring reads as connected.
px, py = None, None
for deg10 in range(0, 3601):
    x, y = pol(10, (deg10 % 3600) / 10.0)
    if px is not None and x != px and y != py:
        grid[x][py] = ROAD
    grid[x][y] = ROAD
    px, py = x, y

# clear pads under/around every structure
for (px, py) in human_bases + bot_bases + pills:
    for ox in range(-1, 2):
        for oy in range(-1, 2):
            if grid[px + ox][py + oy] != DEEP:
                grid[px + ox][py + oy] = GRASS
    grid[px][py] = GRASS

# --- constraint checks -------------------------------------------------
def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))

all_bases = bot_bases + human_bases   # file order: horde 1..8, center 9..14
for i, s in enumerate(human_starts + bot_starts):
    assert grid[s[0]][s[1]] == DEEP, f"start {i} not in deep sea: {s}"
    near = [bi for bi, b in enumerate(all_bases) if cheb(s, b) <= 9]
    if i < 6:
        # Humans are pinned to the puddle by on_choose_start; with the
        # base ring hugging the puddle every human start sees several
        # CENTER bases (harmless — the hook decides placement), but it
        # must never see a HORDE base.
        assert all(bi >= 4 for bi in near), f"human start {i} sees horde bases {near}"
    else:
        # Horde bases sit on the shore ring, so a bot start may be within 9
        # of the base on ITS OWN spoke. Placement is hook-driven
        # (on_choose_start), so the engine's tiering fallback is never
        # consulted -- report it, don't refuse it.
        if near:
            print(f"note: bot start {i} within 9 of horde base(s) {near} (hook-placed)")
for b in all_bases + pills:
    assert grid[b[0]][b[1]] != DEEP, f"structure in water: {b}"

# --- encode ------------------------------------------------------------
def encode_segment(codes):
    nib, i, n = [], 0, len(codes)
    while i < n:
        j = i
        while j < n and codes[j] == codes[i]:
            j += 1
        run = j - i
        if run >= 2:
            while run >= 2:
                c = min(9, run)
                if run - c == 1:
                    c -= 1                      # never strand a 1-tail
                nib += [c + 6, codes[i]]
                run -= c
            if run == 1:
                nib += [0, codes[i]]
            i = j
        else:
            grp, k = [], i
            while k < n and len(grp) < 8:
                if k + 1 < n and codes[k + 1] == codes[k]:
                    break
                grp.append(codes[k])
                k += 1
            nib += [len(grp) - 1] + grp
            i = k
    return nib

out = bytearray()
out += b"BMAPBOLO"
out.append(1)
out.append(len(pills))
out.append(len(all_bases))
out.append(len(human_starts) + len(bot_starts))
for i, (x, y) in enumerate(pills):
    owner = i if i < 6 else 0xFF                      # center: slot i; outer: neutral
    out += bytes([x, y, owner, 0, 50])                # DEAD, default speed
for i, (x, y) in enumerate(all_bases):
    n_bot = len(bot_bases)
    owner = bot_owners[i] if i < n_bot else (i - n_bot)  # horde spokes, center 0..5
    out += bytes([x, y, owner, 90, 90, 90])
def out_dir(x, y, inward):
    # Map-file start dirs count COUNTERclockwise from east in y-UP map
    # coords; our angle is computed in y-DOWN screen coords, so negate
    # it (the old formula skipped that and vertically mirrored every
    # off-axis start — tanks spawned facing open water).
    ang = math.degrees(math.atan2(y - C, x - C))
    if inward:
        ang += 180
    return int(round(((-ang) % 360) / 22.5)) % 16
for (x, y) in human_starts:
    out += bytes([x, y, out_dir(x, y, False)])        # humans face outward
for (x, y) in bot_starts:
    out += bytes([x, y, out_dir(x, y, True)])         # bots face inward

rows = 0
for y in range(256):
    x = 0
    while x < 256:
        if grid[x][y] == DEEP:
            x += 1
            continue
        x0 = x
        while x < 256 and grid[x][y] != DEEP:
            x += 1
        nib = encode_segment([grid[i][y] for i in range(x0, x)])
        if len(nib) % 2:
            nib.append(0)
        data = bytes([(nib[i] << 4) | nib[i + 1] for i in range(0, len(nib), 2)])
        out += bytes([len(data) + 4, y, x0, x]) + data
        rows += 1
out += bytes([4, 0xFF, 0xFF, 0xFF])

open(DST, "wb").write(bytes(out))
forest = sum(1 for x in range(256) for y in range(256) if grid[x][y] == FOREST)
land = sum(1 for x in range(256) for y in range(256) if grid[x][y] != DEEP)
print(f"wrote {DST}: {len(out)} bytes, {rows} runs, "
      f"{len(pills)} pills, {len(all_bases)} bases, 16 starts, "
      f"land={land} forest={forest} ({100*forest//land}%)")
