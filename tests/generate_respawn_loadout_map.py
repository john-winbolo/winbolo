"""Generate 'RespawnLoadoutTest' — a pillbox death trap for the
per-slot spawn-loadout regression test.

Purpose: prove that a scenario bot spawned with game.spawn_bot(..., "open")
comes back with the FULL open loadout on EVERY respawn, not just its first
life. To measure that we need a bot that (a) spends its ammunition, (b) dies,
(c) always respawns in the same place, and (d) never breaks the trap that is
killing it — with none of it depending on how well the AI drives.

Geometry (centre 128,128), deliberately minimal:

  Chebyshev r <= 6   GRASS island (13x13). Nothing else — no forest, because
                     utilIsTankInTrees hides a tank from pillboxes and would
                     make the kill rate depend on where it parked.
  x = 124 and x = 132, y in {125, 128, 131}
                     6 NEUTRAL, FULL-ARMOUR pillboxes at full fire rate
                     (speed = PILLBOX_MAX_FIRERATE = 6), four tiles west and
                     four tiles east of the start. Every one of them is well
                     inside PILLBOX_RANGE (2048 world units = 8 tiles) of the
                     start, so the tank is under crossfire from six guns and
                     dies in a few hundred ticks.

                     They are in COLUMNS, not a ring, on purpose. The probe
                     brain (tests/brains/fire_north_in_place.lua) parks on the
                     start and fires due north to empty its magazine; with the
                     pillboxes 4 tiles off that lane to either side, not one
                     shell can reach them. The trap therefore survives the
                     whole run instead of being slowly dismantled by the very
                     tank it is supposed to keep killing. (The scenario
                     re-checks every pillbox's armour at each respawn and fails
                     loudly if this ever stops holding.)
  Chebyshev r >  6   DEEP SEA. Nowhere to run, and the northward shells expire
                     harmlessly out at sea.

  ONE start, dead centre at (128,128) — the map's only start, so every engine
  respawn pick puts the tank straight back between the two columns.

  ZERO bases. Two reasons: (1) with no bases the TOURNAMENT loadout is
  0 shells / 0 mines / 0 trees (gameTypeGetItems: percent of neutral bases
  over a base count floored at 1), which is the maximum possible contrast
  against the OPEN loadout of 40/40/40 the bot must receive — a respawn that
  silently falls back to the sim rules reads as a stark 0; and (2) no bases
  means no refuelling, so a loadout observed at a respawn is the loadout the
  engine handed out, not something the tank topped up afterwards.
  serverSimWinningOwner returns NEUTRAL when the base count is 0, so the
  engine's all-bases sweep can never end the round early.

Run-encoding follows mapProcessRun exactly (nibble 0-7 = that many+1
different tiles follow; 8-15 = run of len-6 same tiles), same as
generate_survival_map.py.
"""
import os

DST = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)),
                                    "..", "data", "maps",
                                    "RespawnLoadoutTest.map"))

DEEP = 0xFF
GRASS = 7

C = 128
R_LAND = 6          # Chebyshev radius of the island
COL_DX = 4          # pillbox columns, this far west and east of the start
COL_YS = (-3, 0, 3) # three pillboxes per column

PILL_ARMOUR = 15    # full health
PILL_SPEED = 6      # PILLBOX_MAX_FIRERATE — clamped floor, fastest reload
PILL_NEUTRAL = 0xFF # neutral pills shoot every tank (pillsUpdate skips only
                    # the owner and its allies)

PILLBOX_RANGE_TILES = 8.0   # PILLBOX_RANGE 2048 >> TANK_SHIFT_MAPSIZE

# --- terrain -----------------------------------------------------------
grid = [[DEEP] * 256 for _ in range(256)]
for x in range(C - R_LAND, C + R_LAND + 1):
    for y in range(C - R_LAND, C + R_LAND + 1):
        grid[x][y] = GRASS

# --- structures --------------------------------------------------------
pills = [(C + dx, C + dy) for dx in (-COL_DX, COL_DX) for dy in COL_YS]

bases = []          # deliberately none — see the module docstring

starts = [(C, C)]   # the only start, dead centre between the columns

for p in pills:
    assert grid[p[0]][p[1]] != DEEP, f"pill in water: {p}"
for s in starts:
    assert grid[s[0]][s[1]] != DEEP, f"start in water: {s}"
    assert s not in pills, f"start on a pillbox: {s}"
    for p in pills:
        d = ((s[0] - p[0]) ** 2 + (s[1] - p[1]) ** 2) ** 0.5
        assert d < PILLBOX_RANGE_TILES - 0.5, \
            f"start {s} is {d:.2f} tiles from pill {p} — outside pillbox range"
        # The northward firing lane must stay clear: no pillbox may sit in
        # (or beside) the column of tiles the probe shoots along.
        assert abs(p[0] - s[0]) >= 3, \
            f"pill {p} is only {abs(p[0]-s[0])} tiles off the firing lane at {s}"


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
out.append(len(bases))
out.append(len(starts))
for (x, y) in pills:
    out += bytes([x, y, PILL_NEUTRAL, PILL_ARMOUR, PILL_SPEED])
for (x, y, owner) in bases:
    out += bytes([x, y, owner, 90, 90, 90])
for (x, y) in starts:
    # Facing byte is irrelevant: the probe brain turns itself to north on the
    # first ticks of every life and holds there.
    out += bytes([x, y, 0])

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
print(f"wrote {DST}: {len(out)} bytes, {rows} runs, "
      f"{len(pills)} pills, {len(bases)} bases, {len(starts)} starts")
