"""Generate 'Survival' — circular co-op survival scenario map.

Rings (center 128,128), sized to sit near Schism Toy III's ~55x55
footprint (the spawn-tiering rules put a floor on the bot ring: 10
bases must be >9 Chebyshev apart, so the island can't get much
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
                   - 10 DEAD neutral pills parked between the bot bases;
                     the scenario loads one into each wave-1 tank
                   - 10 bot bases at r=26 (36 deg apart, owners 15..6 -> each
                     wave bot is pulled to the outer start at its angle)
  r > 29           open sea; 10 bot starts at r=33 (one per bot base angle)

Everything stays inside the unmined 21..235 zone (max extent 128+33=161).
Run-encoding follows mapProcessRun exactly (nibble 0-7 = that many+1
different tiles follow; 8-15 = run of len-6 same tiles).
"""
import math

DST = r"D:\Development\winbolo2\data\maps\Survival.map"

DEEP = 0xFF
SWAMP, ROAD, FOREST, GRASS = 2, 4, 5, 7

C = 128
R_LAKE = 2.5
R_LAND = 29
R_HSTART, R_HBASE = 2, 6
R_BBASE, R_BSTART = 26, 33
R_PILL = 8               # 6 dead defender pills, one beyond each base
R_PILL_OUT = 26          # 10 dead neutral pills for the wave-1 tanks

def pol(r, deg):
    a = math.radians(deg)
    return (C + int(round(r * math.cos(a))), C + int(round(r * math.sin(a))))

human_angles = [k * 60 for k in range(6)]
bot_angles   = [k * 36 for k in range(10)]

human_starts = [pol(R_HSTART, a) for a in human_angles]
human_bases  = [pol(R_HBASE,  a) for a in human_angles]      # owners 0..5
bot_bases    = [pol(R_BBASE,  a) for a in bot_angles]        # owners 15..6
bot_starts   = [pol(R_BSTART, a) for a in bot_angles]
pills = ([pol(R_PILL, a) for a in human_angles] +        # 1..6: base 10+k's pill
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
    if r >= R_BBASE - 2:
        return GRASS if h % 10 < 8 else FOREST        # light cover out wide
    # main belt: decent forest for fort building + a few swamp accents
    m = h % 100
    if m < 34:
        return FOREST
    if m < 38:
        return SWAMP
    return GRASS

grid = [[terrain(x, y) for y in range(256)] for x in range(256)]

# ring road at the human perimeter (r ~ 10) for looks + mobility
for deg10 in range(0, 3600):
    x, y = pol(10, deg10 / 10.0)
    grid[x][y] = ROAD

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

all_bases = bot_bases + human_bases   # file order: outer 1..10, center 11..16
for i, s in enumerate(human_starts + bot_starts):
    assert grid[s[0]][s[1]] == DEEP, f"start {i} not in deep sea: {s}"
    near = [bi for bi, b in enumerate(all_bases) if cheb(s, b) <= 9]
    if i < 6:
        # Humans are pinned to the puddle by on_choose_start; with the
        # base ring hugging the puddle every human start sees several
        # CENTER bases (harmless — the hook decides placement), but it
        # must never see an OUTER base.
        assert all(bi >= 10 for bi in near), f"human start {i} sees outer bases {near}"
    else:
        # Each bot start pairs with exactly its own base so the engine
        # fallback stays correct for bots.
        assert near == [i - 6], f"bot start {i} sees bases {near}, want {[i-6]}"
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
    owner = (15 - i) if i < 10 else (i - 10)          # outer 15..6, center 0..5
    out += bytes([x, y, owner, 90, 90, 90])
def out_dir(x, y, inward):
    ang = math.degrees(math.atan2(y - C, x - C))
    if inward:
        ang += 180
    return int(round(((ang + 360) % 360) / 22.5)) % 16
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
