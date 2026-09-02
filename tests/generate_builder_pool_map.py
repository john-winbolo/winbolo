#!/usr/bin/env python3
"""
Generate the builder-pool arenas (companion to tests/builder_pool_test.py).

Four variants, one canvas.  All of them exist to answer the question the plan
opens with: the tank's goal is RIGHT and the man is idle, so who spends him?

  A  FIRE-EXCHANGE GATING.  Our own worn pill sits five tiles off the line
     while the tank works a hostile pill.  Assert nothing is dispatched on any
     tick whose eligibility verdict reads `fire_exchange:<substate>`, that
     those ticks really had a candidate to hold back, and that the errand that
     did run reproduces from its own chips and landed.  This is
     20260901_160325_1_par2 bot2 t=21471 at test scale.

  B  THE repair_pill SPLIT (plan section 7).  An empty field, our base, and one
     badly worn friendly pill EIGHTEEN tiles away -- well outside the leash.
     repair_pill as a tank goal therefore means what the plan says it means:
     "relocate until the repair becomes leash-reachable".  Assert the tank does
     drive, that the pool-5 row flips to `REJECT builder_can (leash 8, eta N)`
     the moment it is in range, and that the MAN then finishes it without the
     tank closing the last few tiles itself.

  B2 UNDER-FIRE CLOCK.  A moat with no river anywhere makes a scripted shooter
     PERMANENTLY unreachable -- so attack_tank and kill_lgm price at INF and
     cannot pull the tank into a fight, leaving it on idle-ish goals -- while
     its shells still fly over the water onto OUR TANK, with a worn pill four
     tiles away.  Assert the dispatch is denied `under_fire(<age>t)` on ticks
     that had a live candidate.  The shooter aims at the TANK, not at the pill:
     this is danger.tank_fire_age, a different signal from the pill's
     last_hit_tick that the repair hold uses.  (The under-fire clock is tested
     AFTER the mode gate, so it can only ever be the named reason when the
     owner is idle-ish -- hence the unreachable shooter.)

  C  TWO ALLIES, ONE PILL.  Two GoalHunter 1.7 bots equidistant from one worn
     pill, both able to reach it with the man.  Assert exactly one BP_DISPATCH
     across the pair and that the loser's row says ally_repairing.

  D  RESERVATION.  A far take with wall shields planned, and our worn pill near
     the standoff.  Assert b.reserve_eta is DECLARED for that take, that it is
     the real tank-travel ETA (it moves as the tank drives rather than sitting
     on a constant), and that no side-quest was ever launched into a
     reservation it did not fit.  The arena does NOT force the deferral and the
     check says so where it does not happen -- see the note in check_D: in a
     one-bot arena repair_pill (30 flat) or defend_pill's ARRIVED rung (40)
     outbids attack_pill for any damaged pill in range, so the tank goes and
     fixes it through the feeder, which is exempt from the reservation by
     construction.

WHY A DAMAGED PILL AND NOT A CORPSE in A / C / D
------------------------------------------------
The plan's incident is a dead blocker, and the pool does have a `rebuild` row
for exactly that.  But a corpse on open ground inside the leash is ALSO a
capture_pill candidate for the TANK, and capture prices at ~5 + path against
attack_pill's 30 + travel -- so in a two-goal arena the tank simply drives over
and picks the corpse up, which is correct play and measures nothing about the
builder pool.  There is no scenario-API lever that makes a pill un-capturable
without also making it unreachable for the LGM.

A damaged-but-alive pill is not capturable at all (filter_capture_pill rejects
`alive`), so the tank pool leaves it alone and the only thing that can fix it
is the man.  The eligibility stack, the reservation, the claim and the
dispatch are identical for `topup` and `rebuild` -- they differ only in the
value constant -- so the mechanism under test is the same one.  The `rebuild`
row is exercised by the DH-Oil Rig smoke run, where corpses appear with the
tank genuinely too busy to fetch them.

MAP FACTS THAT BITE
-------------------
  * mapRead recenters the terrain bounding box midpoint to (126,126), so every
    variant asserts its own midpoint is already (126,126) and no shift happens.
  * a start square must be DEEP SEA (starts.c startsIsValidSquare) or the
    engine spirals off to find one -- hence the one-tile ponds.
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40), which is what lets
    these arenas test wood-gated behaviour without a harvest first.
  * EVERY worn pill needs a friendly base within PILL_FIRE_RANGE (8) of it.
    The pill-reposition pool scores a lone pill far from every base as badly
    placed and bids capture_pill/reposition_shoot on it -- i.e. the bot shoots
    its OWN pill down to move it, which ends the experiment. A base inside the
    pill's fire range gives it a job (PILL_REPOSITION_BASE_PROTECT_W) and
    settles it. This is the same trap tests/generate_defend_repair_map.py
    documents, and it cost this file a run to rediscover.

Usage:
    python3 tests/generate_builder_pool_map.py [variant] [output_path]
    variant: A | B | C | D   (default: all four)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256
PILLS_MAX_HEALTH = 15

# ── Variant A: stolen blocker ────────────────────────────────────────────
# Field is 37x29, midpoint (126,126).  The tank starts west, the hostile pill
# is east, and our worn pill sits north of the line between them.
A_FIELD = (108, 144, 112, 140)
A_SPAWN = (114, 126)           # one-tile pond, west end
A_BASE = (110, 126)            # full stock: no refuel bid while the tank is full
A_GUARD_BASE = (121, 118)      # the reposition guard: 6 tiles from our worn pill,
                               # inside PILL_FIRE_RANGE, so the pill has a job and
                               # the reposition pool stops bidding to shoot it down
A_FOE_PILL = (134, 126)        # the take.  Owned by the scripted idler (team 1).
A_OUR_PILL = (124, 121)        # worn.  ~5 tiles off the standoff the take picks,
                               # i.e. inside BUILDER_POOL_LEASH (8) from it, and
                               # 5 north of the firing line so the tank's own
                               # shells never cross it.
A_OUR_HP = 6                   # 9 missing = 3 trees; well over
                               # BUILDER_POOL_TOPUP_MIN_MISSING (4)
A_FOE_SPAWN = (140, 138)       # the idler: it owns the target pill and does
                               # nothing else.  Far corner so it never fights.

# ── Variant B: the repair_pill split ─────────────────────────────────────
# Empty field, no enemies at all: the only thing on it that needs doing is one
# badly worn pill, 18 tiles from the spawn -- more than twice the leash, so the
# pool cannot even see it as a candidate at first and repair_pill is the only
# goal that can act. The tank drives; the pool takes over when it is in range.
B_FIELD = (108, 144, 112, 140)
B_SPAWN = (114, 126)
B_BASE = (110, 126)
B_OUR_PILL = (132, 126)        # 18 tiles east of the spawn
B_OUR_HP = 3                   # 12 missing = 3 trees
B_GUARD_BASE = (134, 122)      # 2+4 = 6 tiles from the pill: the reposition guard

# ── Variant B2: shells on the tank, and no way to shoot back ─────────────
# The moat trick from tests/generate_defend_repair_map.py: a 3-row band of DEEP
# SEA across the whole width, and NO river anywhere on the map, so no boat can
# ever be built and the shooter is PERMANENTLY unreachable.  attack_tank and
# kill_lgm therefore price at INF and cannot pull the tank into a fight -- but
# shells fly over water, so the shooter can still hit us.
#
# That is the point.  The under-fire clock is checked AFTER the mode gate, so
# it can only ever BE the reason a dispatch is denied when the owner is
# idle-ish.  A tank that can fight back picks attack_tank, the mode gate fires
# first, and the clock never gets a turn.  An unreachable shooter leaves the
# bot on explore / take_cover / none -- idle-ish -- with shells landing on it
# and a worn pill four tiles away that it must nonetheless not send the man to.
B2_FIELD = (108, 144, 112, 127)
B2_MOAT_Y = (128, 130)
B2_STRIP = (108, 144, 131, 140)
B2_SPAWN = (120, 126)          # one-tile pond, 5 tiles north of the shooter
B2_BASE = (114, 120)
B2_OUR_PILL = (124, 126)       # worn, 4 tiles east of the spawn: inside the
                               # leash from the moment the tank is ashore
B2_OUR_HP = 5
B2_GUARD_BASE = (126, 122)     # 2+4 = 6 tiles from the pill: the reposition guard
B2_FOE_SPAWN = (120, 132)      # the shooter's pond, across the moat and ONE TILE
                               # from its standoff. It has to be in position and
                               # firing before our bot has finished beaching:
                               # the arena needs a live side-quest candidate AND
                               # shells in the air at the same moment, and a
                               # four-tile repair is over in ~150 ticks
B2_FOE_STANDOFF = (120, 131)   # 5 tiles from our spawn -- inside a tank shell's
                               # ~7.1-tile reach, and it aims at wherever our
                               # tank actually is, not at a fixed tile

# ── Variant C: two allies, one pill ──────────────────────────────────────
# Both bots spawn INSIDE the leash of the pill so neither has to drive to it --
# the experiment is the race for the man, not a race of tanks. The pill belongs
# to slot 0, so it reads "friendly" to bot 0 and "allied" to the ally; both are
# candidates (the engine's pillsRepairPos has no ownership test), which is
# exactly why the claim has to arbitrate.
C_FIELD = (108, 144, 112, 140)
C_SPAWN_1 = (120, 126)         # 6 tiles west of the pill
C_SPAWN_2 = (132, 126)         # 6 tiles east: same trip, opposite side
C_OUR_PILL = (126, 126)
C_OUR_HP = 5
C_BASE_1 = (114, 126)
C_BASE_2 = (138, 126)
C_GUARD_BASE = (126, 120)      # 6 tiles from the pill: the reposition guard

# ── Variant D: reservation ───────────────────────────────────────────────
# The take is FAR east, so the tank spends a long stretch in plan_position /
# approach with a live reserve_eta, and our worn pill sits near the STANDOFF
# rather than near the spawn.  While the tank is far away the pill is outside
# the leash and there is no row at all; as the tank closes, the pill enters the
# leash at exactly the moment reserve_eta has shrunk to "the walls are due
# shortly" -- so the trip does not fit and the row must defer.
D_FIELD = (108, 144, 108, 144)
D_SPAWN = (112, 126)
D_BASE = (110, 130)
D_FOE_PILL = (140, 126)
D_OUR_PILL = (127, 124)
D_OUR_HP = 11                  # only 4 missing, on purpose. repair_pill prices at
                               # REPAIR_BASE_COST(30) + max(0, path - missing x
                               # REPAIR_DAMAGE_BONUS(10)); at 11 missing the bonus
                               # cancels the whole cross-map drive and the tank goes
                               # and fixes it before ever starting the take, which
                               # leaves nothing for the reservation to defer.
D_GUARD_BASE = (124, 119)      # 3+5 = 8 from our pill; 23 from the foe pill, so
                               # refuelling there is never under its fire
D_FOE_SPAWN = (142, 142)

VARIANTS = ("A", "B", "B2", "C", "D")


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(t, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = tile


def pond(t, sq):
    t[sq[1]][sq[0]] = DEEP_SEA


def build(variant):
    """Returns terrain, pills, bases, starts for the variant."""
    t = blank()
    if variant == "A":
        fill(t, A_FIELD)
        pond(t, A_SPAWN)
        pond(t, A_FOE_SPAWN)
        pills = [(A_FOE_PILL[0], A_FOE_PILL[1], 0, PILLS_MAX_HEALTH, 50),
                 (A_OUR_PILL[0], A_OUR_PILL[1], 0, A_OUR_HP, 50)]
        bases = [(A_BASE[0], A_BASE[1], 0, 90, 90, 90),
                 (A_GUARD_BASE[0], A_GUARD_BASE[1], 0, 90, 90, 90)]
        starts = [(A_SPAWN[0], A_SPAWN[1], 4),        # ours, facing east
                  (A_FOE_SPAWN[0], A_FOE_SPAWN[1], 0)]
    elif variant == "B":
        fill(t, B_FIELD)
        pond(t, B_SPAWN)
        pills = [(B_OUR_PILL[0], B_OUR_PILL[1], 0, B_OUR_HP, 50)]
        bases = [(B_BASE[0], B_BASE[1], 0, 90, 90, 90),
                 (B_GUARD_BASE[0], B_GUARD_BASE[1], 0, 90, 90, 90)]
        starts = [(B_SPAWN[0], B_SPAWN[1], 4)]
    elif variant == "B2":
        fill(t, B2_FIELD)
        fill(t, B2_STRIP)
        pond(t, B2_SPAWN)
        pond(t, B2_FOE_SPAWN)
        pills = [(B2_OUR_PILL[0], B2_OUR_PILL[1], 0, B2_OUR_HP, 50)]
        bases = [(B2_BASE[0], B2_BASE[1], 0, 90, 90, 90),
                 (B2_GUARD_BASE[0], B2_GUARD_BASE[1], 0, 90, 90, 90)]
        starts = [(B2_SPAWN[0], B2_SPAWN[1], 0),
                  (B2_FOE_SPAWN[0], B2_FOE_SPAWN[1], 4)]
    elif variant == "C":
        fill(t, C_FIELD)
        pond(t, C_SPAWN_1)
        pond(t, C_SPAWN_2)
        pills = [(C_OUR_PILL[0], C_OUR_PILL[1], 0, C_OUR_HP, 50)]
        bases = [(C_BASE_1[0], C_BASE_1[1], 0, 90, 90, 90),
                 (C_BASE_2[0], C_BASE_2[1], 0, 90, 90, 90),
                 (C_GUARD_BASE[0], C_GUARD_BASE[1], 0, 90, 90, 90)]
        starts = [(C_SPAWN_1[0], C_SPAWN_1[1], 4),
                  (C_SPAWN_2[0], C_SPAWN_2[1], 12)]
    else:  # D
        fill(t, D_FIELD)
        pond(t, D_SPAWN)
        pond(t, D_FOE_SPAWN)
        pills = [(D_FOE_PILL[0], D_FOE_PILL[1], 0, PILLS_MAX_HEALTH, 50),
                 (D_OUR_PILL[0], D_OUR_PILL[1], 0, D_OUR_HP, 50)]
        bases = [(D_BASE[0], D_BASE[1], 0, 90, 90, 90),
                 (D_GUARD_BASE[0], D_GUARD_BASE[1], 0, 90, 90, 90)]
        starts = [(D_SPAWN[0], D_SPAWN[1], 4),
                  (D_FOE_SPAWN[0], D_FOE_SPAWN[1], 0)]
    return t, pills, bases, starts


def write(variant, output):
    terrain, pills, bases, starts = build(variant)

    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"variant {variant}: start ({sx},{sy}) must be deep sea "
            f"(starts.c startsIsValidSquare)")
    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, (
            f"variant {variant}: pill ({px},{py}) is in the sea")

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
        for x, y, dr in starts:
            f.write(struct.pack('BBB', x, y, dr))
        f.write(encode_map_runs(terrain))
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) "
          f"[variant {variant}: {len(pills)} pill(s), {len(bases)} base(s), "
          f"{len(starts)} start(s)]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"builder_pool_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
