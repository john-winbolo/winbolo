#!/usr/bin/env python3
"""
Generate the take-cover arena (companion to tests/take_cover_test.py).

Field incident 20260901_000042_1_loss_b6 bot3 t=16625: the tank sat one tile
from a forest INSIDE an angry hostile pill's fire for 130+ ticks. Every goal
that could have moved it was unavailable — defend_pill produced no bid (heat
blocked), attack_tank was gated by pill_crossfire, repair needed the LGM, and
the haul flee's destination picker rejected every base as "empty" because a
full tank needs nothing. The brain had no goal whose job was simply "stand
somewhere less lethal". take_cover (pool 14) is that goal.

Arena — the smallest shape that makes take_cover the only sensible answer:

    y=119        x=120..132   grass strip, ONE NEUTRAL pillbox PER TILE (13)
    y=120..121   DEEP SEA     moat.  There is no river anywhere on the map, so
                              no boat can ever be built and the pill strip is
                              permanently unreachable — attack_pill and
                              capture_pill therefore cost INF and never compete.
    y=122..134   x=108..144   our grass field
    (126,127)    a ONE-TILE deep-sea pond — the tank start.  8 tiles south of
                              the strip: deep inside the pill DANGER disk
                              (PILL_RANGE_MAP 10) but right at the edge of
                              PILLBOX_RANGE (8), so only the single pill
                              directly north can actually shoot.
    (123,133)    our base, full stock.  The tank spawns full, so refuel does
                              not bid until the pills have actually hurt it.

Deliberately NO friendly pillbox.  An earlier cut put one at (126,133) to
supply the `cover` term, and it wrecked the arena twice over: the reposition
pool decided a lone pill 14 tiles from base was badly placed and outbid
take_cover at cost 59, and once the bot started shooting its own pill down the
pill's anger tripled the cover term mid-run.  With no team pill on the map,
cover is 0 everywhere and safety reduces to -W_EXPO * threat.pill_at — which
is exactly the axis this test is about.  (The cover term is exercised by
tests/take_cover_scan_check.py instead, against a real session.)

The pond is not decoration.  startsIsValidSquare (starts.c) requires a start
square to be DEEP SEA — a start written on land is skipped and
startsScatterFind spirals outward to the nearest sea tile instead.  A first
cut of this arena put the start on grass and the spiral dropped the tank into
the moat, afloat, where take_cover correctly refuses to bid (in_boat: cover is
a land concept) and the test measured nothing.  One sea tile in the middle of
the field puts the tank exactly where the incident needs it: on bad ground,
one step from land, and ashore in a few ticks.

Why NEUTRAL pills rather than an enemy player's: neutral pillboxes shoot at
every tank in range (pillbox.c pillsUpdate skips only allies and the owner),
they need no second player in the game, and threat.lua counts hostile and
neutral pills identically.

Why THIRTEEN of them: threat.pill_at has to clear
TAKE_COVER_BAD_GROUND_PILL_AT (30) WITHOUT anger — and anger is a brain-side
model, bumped only when the brain SEES a pill take damage, which a one-bot
arena cannot arrange deterministically.  A calm pill contributes at most
PILL_DANGER_BASE (8) x prox x terrain_mult, and C.CROSSFIRE_MULTIPLIER_ENABLED
is FALSE so overlapping pills only ADD — no small cluster gets near 30.  A
solid row does: 13 of them put ~78 on the spawn tile while all but one sit
past PILLBOX_RANGE, so the ground is unambiguously bad and the tank is barely
being shot at while it leaves.

Expected: take_cover's bad_ground trigger fires as soon as the tank is ashore,
the scan picks the nearest tile out of the danger disk (~3 south), and the bot
drives there and holds.

IMPORTANT: mapRead RECENTERS off-center maps (bolo_map.c): the terrain
bounding-box midpoint is shifted to (126,126) — but only when BOTH axes need
shifting (`if (addX != 0 && addY != 0)`).  Written tiles here span x=108..144,
y=119..134, whose integer midpoints are both 126, so the recenter is a no-op
and in-game coordinates match this file.  main() asserts it.

Usage:
    python3 tests/generate_take_cover_map.py [output_path]
    Default output: tests/take_cover.map
"""

import struct
import sys
from pathlib import Path

GRASS = 7
DEEP_SEA = None  # background sentinel (unwritten cells read as deep sea)

MAP_SIZE = 256
NEUTRAL = 0xFF

# ── Geometry (the test runner imports these for its assertions) ──────────
PILL_STRIP = (120, 132, 119)          # x0, x1 inclusive, y
FIELD = (108, 144, 122, 134)          # x0, x1, y0, y1 inclusive
NEUTRAL_PILLS = [(x, 119) for x in range(120, 133)]
BASE = (123, 133)
SPAWN = (126, 127)                    # the one-tile deep-sea pond
POND = [(126, 127)]
# The tile the scan is expected to choose: ring-3 south, the nearest tile
# outside every neutral pill's danger disk.  NOT asserted — which tile wins is
# the scan's business — but the generator checks its danger, so the arena
# really does offer an escape.
EXPECT_PICK = (126, 130)

# ── Brain-side constants this arena is designed against ──────────────────
PILL_RANGE_MAP = 10                   # constants.lua M.PILL_RANGE_MAP
PILL_DANGER_BASE = 8                  # constants.lua M.PILL_DANGER_BASE
PILL_DANGER_EDGE_FALLOFF = 0.5        # constants.lua M.PILL_DANGER_EDGE_FALLOFF
PILLBOX_RANGE = 8                     # tiles a pill can actually SHOOT
# threat.lua's per-tile terrain factor is 16/TERRAIN_SPEED[tile]; grass is 12.
GRASS_TERRAIN_MULT = 16.0 / 12.0
BAD_GROUND = 30                       # constants.lua TAKE_COVER_BAD_GROUND_PILL_AT


def pill_at(mx, my, pills=None, terrain_mult=GRASS_TERRAIN_MULT):
    """threat.pill_at for CALM, full-health pills on flat open ground —
    gh_threat.c stamp_pill, with no trees to hide in and no walls to occlude
    (this arena has neither):

        penalty(p) = PILL_DANGER_BASE
                     * (1 - EDGE_FALLOFF * d/PILL_RANGE_MAP)   [d <= RANGE_MAP]
        pill_at    = sum(penalty) * terrain_mult

    No coverage multiplier: C.CROSSFIRE_MULTIPLIER_ENABLED is FALSE, so
    overlapping pills only add.  Accurate to a few percent (the C side caches
    prox per integer offset); used to DESIGN the arena and to report what the
    brain's own printed number should be near — never asserted equal.
    tests/take_cover_scan_check.py reuses it on a real session's pill table.
    """
    if pills is None:
        pills = NEUTRAL_PILLS
    total = 0.0
    for (px, py) in pills:
        d = ((px - mx) ** 2 + (py - my) ** 2) ** 0.5
        if d <= PILL_RANGE_MAP:
            total += PILL_DANGER_BASE * (
                1.0 - PILL_DANGER_EDGE_FALLOFF * (d / PILL_RANGE_MAP))
    return total * terrain_mult


def n_pills_in_shooting_range(mx, my, pills=None):
    """How many pills can actually put a shell on this tile."""
    if pills is None:
        pills = NEUTRAL_PILLS
    return sum(1 for (px, py) in pills
               if ((px - mx) ** 2 + (py - my) ** 2) ** 0.5 <= PILLBOX_RANGE)


def make_map():
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    x0, x1, y = PILL_STRIP
    for x in range(x0, x1 + 1):
        t[y][x] = GRASS
    fx0, fx1, fy0, fy1 = FIELD
    for yy in range(fy0, fy1 + 1):
        for xx in range(fx0, fx1 + 1):
            t[yy][xx] = GRASS
    # The start pond: DEEP_SEA is the unwritten background, so "digging" it is
    # just clearing the tile back to None.
    for (px, py) in POND:
        t[py][px] = DEEP_SEA
    return t


def encode_map_runs(terrain):
    """Nibble-run encode; one run per CONTIGUOUS non-deep span (follows
    mapProcessRun: length nibble 0-7 = that many +1 literal nibbles; 8-15 =
    run of len-6 copies of the next nibble)."""
    MAP_RUN_SAME = 6
    runs = bytearray()
    for y in range(MAP_SIZE):
        x = 0
        while x < MAP_SIZE:
            if terrain[y][x] is DEEP_SEA:
                x += 1
                continue
            startx = x
            while x < MAP_SIZE and terrain[y][x] is not DEEP_SEA:
                x += 1
            endx = x
            data = bytearray()
            i = startx
            while i < endx:
                run_start = i
                while (i < endx and terrain[y][i] == terrain[y][run_start]
                       and (i - run_start) < 9):
                    i += 1
                run_len = i - run_start
                tile = terrain[y][run_start]
                if run_len >= 2:
                    data.append(((run_len + MAP_RUN_SAME) << 4) | (tile & 0x0F))
                else:
                    data.append((0 << 4) | (tile & 0x0F))
            runs.append(4 + len(data))
            runs.append(y)
            runs.append(startx)
            runs.append(endx)
            runs.extend(data)
    runs.extend(b'\x04\xFF\xFF\xFF')
    return bytes(runs)


def main():
    output = sys.argv[1] if len(sys.argv) > 1 else str(
        Path(__file__).parent / "take_cover.map")
    terrain = make_map()

    # Recenter sanity: only written tiles define the bounding box, and its
    # integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))
    assert terrain[SPAWN[1]][SPAWN[0]] is DEEP_SEA, (
        "the start square must be deep sea (starts.c startsIsValidSquare)")
    for (px, py) in NEUTRAL_PILLS:
        assert terrain[py][px] is not DEEP_SEA, f"pill ({px},{py}) needs land"

    # The spawn must be unambiguously bad ground (with slack for the few
    # percent the model is off by), the escape unambiguously not, and the
    # tank must not be under enough fire to die before it can leave.
    assert pill_at(*SPAWN) >= BAD_GROUND * 1.5, (
        f"spawn pill_at is only {pill_at(*SPAWN):.0f} — bad_ground may not fire")
    assert pill_at(*EXPECT_PICK) < BAD_GROUND * 0.5, (
        f"pick {EXPECT_PICK} still reads pill_at {pill_at(*EXPECT_PICK):.0f}")
    assert n_pills_in_shooting_range(*SPAWN) <= 2, (
        "too many pills can shell the spawn — the tank dies before it moves")

    # Pill record: x, y, owner, armour, speed.
    #   NEUTRAL (0xFF) = shoots every tank in range, belongs to nobody.
    #   owner 0        = the bot's player (the only player, slot 0).
    # armour 15 = PILLS_MAX_HEALTH (alive); speed 50 = normal reload.
    pills = [(x, y, NEUTRAL, 15, 50) for (x, y) in NEUTRAL_PILLS]
    bases = [(BASE[0], BASE[1], 0, 90, 90, 90)]   # ours, full stock
    starts = [(SPAWN[0], SPAWN[1], 8)]            # dir 8 = south, away from the strip

    with open(output, 'wb') as f:
        f.write(b'BMAPBOLO')
        f.write(struct.pack('B', 1))
        f.write(struct.pack('B', len(pills)))
        f.write(struct.pack('B', len(bases)))
        f.write(struct.pack('B', len(starts)))
        for x, y, owner, armour, speed in pills:
            f.write(struct.pack('BBBBB', x, y, owner, armour, speed))
        for x, y, owner, armour, shells, mines in bases:
            f.write(struct.pack('BBBBBB', x, y, owner, armour, shells, mines))
        for x, y, d in starts:
            f.write(struct.pack('BBB', x, y, d))
        f.write(encode_map_runs(terrain))

    print(f"Wrote {output} ({Path(output).stat().st_size} bytes)")
    print(f"  {len(NEUTRAL_PILLS)} neutral pills at y={PILL_STRIP[2]} across the"
          f" moat, no team pill, base {BASE}, spawn pond {SPAWN},"
          f" expected cover tile {EXPECT_PICK}")
    print(f"  modelled threat.pill_at: spawn {pill_at(*SPAWN):.0f}"
          f" (bad ground is >= {BAD_GROUND}),"
          f" pick {pill_at(*EXPECT_PICK):.0f};"
          f" {n_pills_in_shooting_range(*SPAWN)} pill(s) can shell the spawn")


if __name__ == '__main__':
    main()
