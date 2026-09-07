#!/usr/bin/env python3
"""
Generate the attack_tank HEAT-A-FRIENDLY-PILL arenas, and their scenario
sidecars (companion to tests/heat_pill_test.py).

THE RULE UNDER TEST (GoalHunter 1.7, 2026-09-06)
------------------------------------------------
While the bot is on an `attack_tank` goal against enemy tank E, it interrupts
the fight to HEAT a FRIENDLY pillbox P: it shoots P exactly as many times as it
takes to drive P's reload period to maximum anger, so that P -- which is
already sitting closer to E than we are -- reloads at PILLBOX_MAX_FIRERATE and
hoses E while we go back to fighting.

THE ENGINE FACTS THIS GEOMETRY IS BUILT ON (cited, not guessed)
--------------------------------------------------------------
  * A pill's "anger" IS its reload period, the `speed` field.
    PILLBOX_ATTACK_NORMAL = 100 is calm, PILLBOX_MAX_FIRERATE = 6 is as angry
    as a pill gets                       (src/bolo/internal/pillbox.h:46-48).
  * Anger is NOT additive: every DAMAGING shell does `speed /= 2`, floored at
    6                                       (src/bolo/pillbox.c:504-511).  The
    ladder from calm is 100 -> 50 -> 25 -> 12 -> 6: exactly FOUR hits.  That
    ladder, read off the ENGINE through the scenario sidecar's `pill(n).speed`
    (src/server/scenario.c:262), is what the test asserts on -- not the brain's
    opinion of itself.
  * OUR OWN shell damages and angers our OWN pill.  The server calls
    pillsDamagePos(..., TRUE, TRUE, owner) for any shell landing on a pill tile
    with NO ownership test at all      (src/bolo/shells.c:658-661).  Each hit
    also costs the pill 1 armour -- which is the whole reason for the HP floor
    (C.ATTACK_TANK_HEAT_MIN_HP = 5).
  * Cool-down gives back +1 speed every PILLBOX_COOLDOWN_TIME (32) ticks
    (src/bolo/pillbox.c:347-355).  Our volley is 4 shots at TANK_RELOAD_TIME
    (13) engine ticks apart = 39 ticks, so at most one +1 lands inside it and
    the next halving eats it again (51/2 = 25, 26/2 = 13, 13/2 = 6).  The test
    therefore asserts "four downward steps, ending at 6", not four exact
    values.
  * The BRAIN cannot read `speed`.  It uses world.lua's PROXY, pill.anger in
    [0,1]: +C.PILL_ANGER_BUMP (0.3333) per OBSERVED armour DROP, capped at 1,
    decaying elapsed/C.PILL_ANGER_DECAY (3000)   (world.lua:285-291).  One
    observed drop == one halving, so need = 4 - round(anger/0.3333).
    A health RISE does not bump.  Arena D is built entirely on that asymmetry.

THE HEALTH CAP (Andrew, 2026-09-06: "proportionally less shots to heat up
depending how hurt the pill is").  Every heat shell costs the pill an armour
point, so how many the volley may fire is capped by how much armour it has to
spend.  attack.lua heat_allowed_shots, mirrored here as heat_allowed_shots():

    allowed = round(HEAT_MAX_HITS x (hp - MIN_HP) / (PILLS_MAX_HEALTH - MIN_HP))
    hp    15  14  13  12  11  10   9   8   7   6   5   4
    allow  4   4   3   3   2   2   2   1   1   0   0   0

and shots_to_fire = min(need, allowed).  allowed == 0 is the whole of
SKIP:hp -- it REPLACES the old per-shot "hp >= 5" test, so a pill at 6 now
fires nothing at all rather than one-and-stop.  The cap is taken ONCE at entry,
so a pill at 10 fires exactly 2 even though its health is 8 by the end.
ALLOWED_TABLE above is that table, asserted against the mirror at import; note
the rule says "floor" but its own worked table sends hp 7 to 1, which floor
does not, so both sides round.

EIGHT ARENAS
------------
  A   DEFAULT / ENTER.  Every gate passes: P is ours, alive at 15 hp, calm,
      inside our gun range with clear line of sight, and closer to E than we
      are.  Expect ENTER, a run of HEAT_SHOT, HEAT_EXIT -> maxed, the engine's
      speed ladder in the trace, and the fight with E resumed afterwards.
  AK  THE CONTROL, one token different (cfg=ATTACK_TANK_HEAT_PILL=false, which
      is what PRESETS.keel sets).  Same ground, same seed.  No HEAT_PILL and
      no HEAT_SHOT line may appear at all, and the ENGINE must show P
      untouched: armour 15, speed 100.  That is what makes arena A's ladder
      evidence about the RULE rather than about a bot that happens to shoot
      its own pillbox.
  B   THE CAP AT ZERO.  Identical ground, P written into the map at 6 armour
      -- the HIGHEST health whose allowance is still 0, so the arena sits on
      the edge of the cap rather than on a nearly-dead pill.  Expect SKIP:hp,
      never ENTER, engine speed flat at 100.
  B2  THE CAP IN BETWEEN, and the point of the whole rule.  Identical ground to
      A, P written in at 10 armour: allowance 2 against a need of 4.  Expect
      ENTER with need{4} allow{2 of 4} shots_needed{2}, two hits, and
      `HEAT_EXIT ... -> hp_cap` rather than `maxed` -- and the ENGINE's reload
      period walking 100 -> 50 -> 25 and STOPPING there, two halvings short of
      PILLBOX_MAX_FIRERATE, with the armour down 10 -> 8.  Arena A's full
      100 -> 50 -> 25 -> 12 -> 6 beside it is the contrast the cap is for.
  C   NOT CLOSER.  P moved to the FAR WEST end of the room, so P is further
      from E than we are from every tile our tank can stand on.  Expect
      SKIP:not_closer, never ENTER.  `not_closer` is the FIRST gate in
      heat_pill_select, so it is the reason whatever else is true.
  D   ALREADY HOT.  Identical ground to A; the sidecar pre-heats the BRAIN's
      proxy without touching the engine: it knocks P's armour DOWN by one and
      puts it straight back, three times, then re-tops it every
      D_REHEAT_PERIOD ticks.  Three observed drops = 0.9999 anger, and the
      decay (1/3000 a brain tick) never gets it back under
      C.ATTACK_TANK_HEAT_MAX_FRAC (0.75) before the next top-up.  Expect
      SKIP:already_hot, never ENTER -- and, as a bonus the engine can prove,
      P's `speed` stays at 100 the whole time: set_pill_armour does not halve
      anything, so this arena's "hot" pill is hot only in the proxy.
  E   NO LINE OF SIGHT.  P sits in a sealed POCKET above the tank's corridor,
      behind THREE rows of BUILDING.  A shot from the tank to P must cross
      those rows, so shot_path_clear (max_walls = 0) rejects it.  Expect
      SKIP:no_los, never ENTER.
  F   THE OTHER CAP MODE, benched on arena B's ground against arena B's pill.
      Same 6-armour pill, one cfg token apart: with
      C.ATTACK_TANK_HEAT_CAP_MODE=floor there is no proportional cap at all,
      only "a shot must not take the pill under MIN_HP", so 6 armour buys
      exactly 1 shell where the proportional cap buys none.  Expect ENTER with
      cap{floor} allow{1 of 4}, one hit, `HEAT_EXIT ... -> hp_floor`, and the
      ENGINE going 100 -> 50 and stopping with the pill on exactly 5.

THE GROUND, AND WHY EVERY TILE OF IT IS WHERE IT IS
--------------------------------------------------
Arenas A / AK / B / B2 / C / D / F share one open room:

    x:        119..121 122  123  124  125  126  127  128  129  130 131..133
    y=122..124   ###    ###  ###  ###  ###  ###  ###  ###  ###  ###   ###
    y=125        ###     .    .    .    .    p    .    .    .    .    ###
    y=126        ###     .    S    .    .    .    .    .    F    .    ###
    y=127        ###     .    .    B    .    .    .    .    R    .    ###
    y=128..130   ###    ###  ###  ###  ###  ###  ###  ###  ###  ###   ###

    ### BUILDING   . grass   S our spawn pond (123,126)
    F the foe's pond (129,126), filled in under him once he is standing on it
    R the foe's RESPAWN pond (129,127), the one tile that is never filled
    p OUR pill (126,125)   B our base (124,127)
    (arena C moves p to (122,125), the far west end; arenas B and F write p
     at 6 armour and B2 at 10 instead of 15; everything else is identical)

  SEALED, AND WALLED THREE DEEP.  BUILDING is the only terrain a tank cannot
  enter.  Three tiles deep because a shell that misses chews a wall tile down
  a stage at a time (shellsCalcCollision's BUILDING case) and a one-tile wall
  was breached inside a single run of an earlier suite.  Without the seal the
  bot has nothing it can afford, goes exploring, ends up on the water twelve
  tiles away and the arena measures nothing.

  THE ROOM IS THREE ROWS.  Not one: a one-row corridor puts P either IN the
  firing lane (a LIVE pill stops any shell, pillsIsPillHit ignores the owner,
  so it would shield E from us AND block our own approach) or in a one-tile
  alcove -- and a shallow line from the corridor into a one-tile alcove clips
  the wall beside it, so shot_path_clear reads BLOCKED and arena A degenerates
  into arena E.  Three rows give P a row of its own with open ground on both
  sides of the sight line.

  NINE TILES WIDE, AND THAT IS THE OTHER HALF OF THE POINT -- see the comment
  on OUTER below.  A volley counts HITS (the pill's observed armour drop) but
  spends its miss allowance on SHELLS FIRED, and rounds in flight sit between
  the two.  With the room 17 wide and the bot 7 tiles from the pill, a volley
  with perfect aim exited `misses` at hits{3/4} fired{6}.

  S (123,126) OUR SPAWN POND.  A start square must be DEEP SEA
    (starts.c startsIsValidSquare, called at every placement).  The sidecar
    fills it back to grass once our tank is ashore and off it -- filling it
    while the tank still sits there wedges the boat state.

  F (129,126) THE FOE'S POND, WHICH IS FILLED IN UNDER HIM.  The enemy tank E
    is a scripted idle bot (tests/brains/idle.lua), spawned by the sidecar only
    AFTER our own tank is ashore (heat_pill_fire holds the brake for as long as
    a volley runs, and a bot that opens a volley from its spawn pond never
    comes ashore at all -- measured).  E never moves, so the arena's distances
    are constants.

    HE HAS TO END UP ON LAND.  A start square must be DEEP SEA, but a tank left
    afloat dies to the FIRST shell that touches it: tankInGameDamage takes the
    boat away on any hit (tank.c:1258-1265) and a tank on DEEP_SEA with no boat
    drowns on the next update (tank.c:575).  P is 4.1 tiles away and shells him
    unprompted, so a floating E died at hits 3-of-4 of the first measured
    volley and took the target -- and the volley -- with him.  So the pond is
    filled to grass once he is standing on it, and start 3, one tile of water
    that is never filled, catches every later respawn.

  p (126,125) OUR PILL, ALIVE and one row off the y=126 lane.  Off the lane
    because a live pill stops shells: on it, it would shield E from us and
    make the fight the arena needs impossible.

  B (124,127) OUR BASE, full stock, west and off both lanes.
      - EVERY ALIVE FRIENDLY PILL NEEDS A FRIENDLY BASE WITHIN PILL_FIRE_RANGE
        (8) or the reposition pool decides the pill is badly placed and the bot
        SHOOTS ITS OWN PILL DOWN to move it -- which would forge exactly the
        armour drops arenas A and AK are measuring.  2.8 tiles from P here,
        and 2.8 from arena C's P as well, so one base serves every variant.
      - Full stock so there is no refuel bid.
      - A base of ANY owner stops a shell (shot_path_clear), so it is kept off
        the y=126 tank-vs-E lane and off the y=125 tank-to-P sight line.

Arena E replaces the room with a corridor plus a sealed pocket:

    x:        119..123  124  125  126  127  128  129  130..133
    y=119..121   ####   ###  ###  ###  ###  ###  ###   ####
    y=122        ####    .    b    .    p    .    .    ####   <- the POCKET
    y=123..125   ###############################################  <- 3 rows
    y=126        ###   .    S    .    .    .    .    .    F   ###  <- corridor
    y=127        ###   .    .    .    .    .    .    .    R   ###
    y=128..133   ###############################################

    p OUR pill (127,122)   b our base (125,122)
    S (123,126)   F the foe (129,126)   R his respawn pond (129,127)

  THREE ROWS OF WALL, not one.  P does not need line of sight to fire:
  pillsUpdate picks the closest non-allied tank in PILLBOX_RANGE with no LOS
  test at all (pillbox.c:357-405), so P shells E right through the separator
  and chews it -- BUILDING_LIFE is 4, so five rounds a tile, three tiles, and P
  reloads every 100 engine ticks: about 1500 ticks to open one lane.
  heat_pill_test.py runs this arena for 1200.  On top of that, check() walks
  every corridor tile from which the gate would actually REACH the
  line-of-sight test (the ones where `not_closer` and `out_of_range` both pass)
  and refuses any whose sight line shares more than ONE separator tile with P's
  own line of fire -- one hole is not a peephole while the other two rows
  stand.  That is why the corridor stops at the foe's column: from a tile east
  of him the sight line doubles back along P's own diagonal and shares all
  three.

  P IS EAST OF CENTRE IN THE POCKET, not directly above the foe.  Straight
  above, with the separator four rows thick, the pill is exactly as far from
  the foe as the tank is -- measured, `d_pill_enemy{5.0} d_us_enemy{5.0} ->
  SKIP:not_closer`, because the gate's test is `d_pe >= d_us`.  The engage
  phase parks the tank 4 to 5 tiles from its target, not out on the 7-tile
  standoff ring, and every arena here is sized around that measurement rather
  than around TANK_COMBAT_STANDOFF_RANGE.

MAP FACTS THAT BITE (the list every generator in tests/ keeps, same reasons):
  * mapRead recenters the terrain bounding-box midpoint to (126,126), and
    WALLS are terrain, so they count.  check() asserts the midpoint is already
    (126,126) and nothing shifts.
  * a start square must be DEEP SEA.
  * mapRead puts ROAD under every map-file pill.
  * -gametype open starts a tank with TANK_FULL_TREES (40); a map with a
    scenario sidecar is force-switched to gameScripted anyway, which hands out
    the same loadout.
  * EVERY ALIVE friendly pill needs a friendly base within PILL_FIRE_RANGE (8).

Usage:
    python3 tests/generate_heat_pill_map.py [variant] [output_path]
    variant: A | AK | B | B2 | C | D | E | F   (default: all of them)
"""

import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

BUILDING = 0
GRASS = 7
DEEP_SEA = None
MAP_SIZE = 256

# ── engine constants this geometry is built on, mirrored so heat_pill_test.py
#    reads them from one place ───────────────────────────────────────────────
PILLS_MAX_HEALTH = 15
PILLBOX_ATTACK_NORMAL = 100      # pillbox.h: calm reload period
PILLBOX_MAX_FIRERATE = 6         # pillbox.h: angriest a pill gets
PILLBOX_RANGE_TILES = 8.0        # pillbox.h PILLBOX_RANGE 2048 WU
PILL_FIRE_RANGE = 8              # the reposition guard, in tiles
MAP_PILL_SPEED = PILLBOX_ATTACK_NORMAL   # the .map speed byte: start CALM

# ── constants.lua, mirrored ────────────────────────────────────────────────
GUNSIGHT_MAX = 13.875            # half-map-squares; shells fly GUNSIGHT_MAX/2 tiles
GUN_RANGE_TILES = GUNSIGHT_MAX / 2.0                 # 6.9375
HEAT_MIN_HP = 5                  # C.ATTACK_TANK_HEAT_MIN_HP
HEAT_MAX_FRAC = 0.75             # C.ATTACK_TANK_HEAT_MAX_FRAC
# NOT a knob any more.  HEAT_MAX_HITS is a module-local constant in attack.lua
# because it is the LENGTH OF THE ENGINE'S HALVING LADDER, not a preference:
# 100 -> 50 -> 25 -> 12 -> 6 and then `speed > 6` stops it.  Mirrored here so
# the arenas and the test read it from one place.
HEAT_MAX_HITS = 4
PILL_ANGER_BUMP = 0.3333         # C.PILL_ANGER_BUMP
PILL_ANGER_DECAY = 3000          # C.PILL_ANGER_DECAY, in BRAIN ticks
TANK_COMBAT_ENGAGE_RANGE = 7     # C.TANK_COMBAT_ENGAGE_RANGE
TANK_COMBAT_LOS_EXTRA_RANGE = 3  # C.TANK_COMBAT_LOS_EXTRA_RANGE
TANK_COMBAT_MAX_RANGE = 15       # C.TANK_COMBAT_MAX_RANGE
VIEW_RADIUS = 14                 # brain_data.c: the 29x29 brain view

# ── the open room (A / AK / B / C / D) ─────────────────────────────────────
#
# THE ROOM IS DELIBERATELY SMALL -- NINE TILES WIDE.  A volley's progress is
# the pill's OBSERVED armour drop, and an observation is a shell that has
# already LANDED, while the miss allowance (C.ATTACK_TANK_HEAT_MAX_MISSES) is
# counted on shells FIRED.  Shells in flight sit between the two: at
# SHELL_SPEED (32 WU a tick) a round takes 8 engine ticks to cross a tile, so
# from d tiles away the hit counter runs 8d engine ticks behind the fire
# counter, while the tank gets another shell away roughly every 22.  Measured
# on the first draft of this arena, with the room 17 tiles wide and the bot
# standing 7.0 tiles from the pill, a volley with PERFECT AIM (corr=-0 on every
# firing tick) exited `misses` at hits{3/4} fired{6} -- three of its rounds
# were still in the air, none of them missed.  Nine tiles wide puts the pill
# within 4.5 tiles of EVERY tile the tank can occupy, so the fourth hit lands
# before the sixth shell leaves.
OUTER = (119, 133, 122, 130)     # x0, x1, y0, y1 -- solid BUILDING block
ROOM = (122, 130, 125, 127)      # hollowed out to grass
WALL_DEEP = 3                    # tiles of BUILDING on every side of the room
US_SPAWN = (123, 126)            # our pond, filled back to grass by the sidecar
FOE_SPAWN = (129, 126)           # the foe's pond, ALSO filled -- see docstring
FOE_RESPAWN = (129, 127)         # the one tile of water that stays water
BASE = (124, 127)                # full stock; off both lanes; <= 8 from every P
PILL_NEAR = (126, 125)           # A / AK / B / D
PILL_FAR = (122, 125)            # C: further from E than we can ever be
PILL_HP_FULL = PILLS_MAX_HEALTH  # 15
PILL_HP_LOW = 6                  # arena B: the highest hp whose allowance is 0
PILL_HP_CAP = 10                 # arena B2: buys exactly 2 of the 4 halvings
LANE_Y = 126                     # the tank-vs-E firing lane

# ── arena E: corridor + sealed pocket ──────────────────────────────────────
OUTER_E = (119, 133, 119, 133)   # midpoint (126,126) with the taller block
POCKET_E = (124, 129, 122, 122)  # one row of grass, three rows of wall below
CORR_E = (122, 129, 126, 127)    # the tank's corridor.  It STOPS at the foe's
                                 # column: from a tile east of him our sight
                                 # line to P doubles back over P's own line of
                                 # fire, sharing two of the three separator
                                 # tiles, and a breach there really would be a
                                 # peephole (check() walks every corridor tile
                                 # and refuses more than one shared tile).
SEPARATOR_E = (123, 125)         # y rows that must stay solid BUILDING
US_SPAWN_E = (123, 126)
FOE_SPAWN_E = (129, 126)
FOE_RESPAWN_E = (129, 127)
# P sits EAST of centre in the pocket, not above it.  Straight above the enemy
# the pill is sqrt(dx^2 + 16) from him with the separator four rows thick, and
# the tank -- which settles 4 to 5 tiles from the enemy in the engage phase, not
# out at the 7-tile standoff -- ties or beats that: the first draft put P at
# (126,122) and every decision read `d_pill_enemy{5.0} d_us_enemy{5.0} ->
# SKIP:not_closer`, since the gate's test is `d_pe >= d_us` and 5.0 >= 5.0.
# Two tiles east makes it 4.47 against the tank's 5.0 and the gate gets far
# enough down its ladder to reach the line-of-sight test this arena is about.
PILL_E = (127, 122)
BASE_E = (125, 122)

# ── arena D's pre-heat schedule, in ENGINE ticks ───────────────────────────
# Three armour drops make three observed bumps: 3 x 0.3333 = 0.9999 anger,
# comfortably over HEAT_MAX_FRAC.  Each drop is held D_DROP_HOLD engine ticks
# so the brain -- which thinks once per frame, i.e. every second engine tick --
# cannot miss it, then the armour goes straight back to full so the HP gate
# never fires instead of the anger gate.
# The three opening drops have to be DONE before the enemy tank exists, i.e.
# before there is an attack_tank goal for the gate to be asked about at all.
# The sidecar spawns the foe once our pond is filled (engine tick >= 60 + 20),
# so the opening drops run at 20 / 40 / 60 and the proxy is saturated by then.
# Measured with the first draft, which started at tick 200: the very first
# decision came at brain tick 114 (engine 228) with anger{0.33} -- one bump in
# -- and the gate said ENTER.
D_FIRST_DROP = 20                # engine tick of the first drop
D_DROP_GAP = 20                  # engine ticks between the three opening drops
D_DROP_HOLD = 8                  # engine ticks an armour drop is held (the
                                 # brain thinks every SECOND engine tick, so
                                 # this is four chances to see it)
D_REHEAT_PERIOD = 400            # engine ticks between top-ups after that
D_DROP_TO = PILLS_MAX_HEALTH - 1  # 14: one down, still miles over the HP floor

VARIANTS = ("A", "AK", "B", "B2", "C", "D", "E", "F")
OPEN_ROOM_VARIANTS = ("A", "AK", "B", "B2", "C", "D", "F")
FOE_BRAIN = "../tests/brains/idle.lua"


def ladder_speed(halvings):
    """The engine's reload period after `halvings` damaging hits from calm:
    100 -> 50 -> 25 -> 12 -> 6, integer division, floored at
    PILLBOX_MAX_FIRERATE (src/bolo/pillbox.c:504-511)."""
    s = PILLBOX_ATTACK_NORMAL
    for _ in range(halvings):
        if s > PILLBOX_MAX_FIRERATE:
            s = max(PILLBOX_MAX_FIRERATE, s // 2)
    return s


PROPORTIONAL, FLOOR = "proportional", "floor"


def heat_allowed_shots(hp, mode=PROPORTIONAL):
    """attack.lua heat_allowed_shots, mirrored, in both of its modes.

    Every heat shell costs the pill one armour (shells.c:658-661 ->
    pillbox.c:485-487), so a wounded pill buys its rate of fire with armour it
    may not have.  C.ATTACK_TANK_HEAT_CAP_MODE picks how dearly:

      "proportional" (the default, and the new rule) -- the allowance scales
        linearly from "full health, all four halvings" to "at the floor, none":
            allowed = round(HEAT_MAX_HITS x (hp - MIN_HP) / (MAX_HP - MIN_HP))
        ROUNDING, not floor: the rule as written said floor, but its own worked
        table sends hp 7 to 1 and floor sends it to 0.

      "floor" (the rule it replaced, kept so the two can be benched) -- no
        proportional cap at all.  The only limit is that a shot must not take
        the pill under MIN_HP, so the allowance is simply hp - MIN_HP.

    Both clamp to HEAT_MAX_HITS, so `allow{n of N}` always reads "n of the N
    halvings this pill's health can pay for".  The two tables below are the
    rule's own worked values, asserted against this function at import."""
    if mode == FLOOR:
        n = hp - HEAT_MIN_HP
    else:
        span = PILLS_MAX_HEALTH - HEAT_MIN_HP
        if span <= 0:
            return 0
        n = int((HEAT_MAX_HITS * (hp - HEAT_MIN_HP)) / span + 0.5)
    return max(0, min(HEAT_MAX_HITS, n))


ALLOWED_TABLE = {
    PROPORTIONAL: {15: 4, 14: 4, 13: 3, 12: 3, 11: 2, 10: 2, 9: 2,
                   8: 1, 7: 1, 6: 0, 5: 0, 4: 0},
    FLOOR: {15: 4, 14: 4, 13: 4, 12: 4, 11: 4, 10: 4, 9: 4,
            8: 3, 7: 2, 6: 1, 5: 0, 4: 0},
}
for _mode, _table in ALLOWED_TABLE.items():
    for _hp, _want in _table.items():
        assert heat_allowed_shots(_hp, _mode) == _want, (
            f"the mirrored heat_allowed_shots({_hp}, {_mode!r}) gives "
            f"{heat_allowed_shots(_hp, _mode)}, not the rule's {_want} -- this "
            f"generator and attack.lua have drifted apart")
# THE ONE HP THE TWO MODES DISAGREE MOST SHARPLY ABOUT, which is what arenas B
# and F are: at 6 armour the proportional cap allows nothing at all (the gate
# refuses outright, SKIP:hp) while the floor rule allows exactly one shell.
assert (heat_allowed_shots(PILL_HP_LOW, PROPORTIONAL) == 0
        and heat_allowed_shots(PILL_HP_LOW, FLOOR) == 1), (
    f"arenas B and F both stand on a pill at {PILL_HP_LOW} armour because that "
    f"is where the two modes disagree -- proportional 0 against floor 1. They "
    f"now read {heat_allowed_shots(PILL_HP_LOW, PROPORTIONAL)} and "
    f"{heat_allowed_shots(PILL_HP_LOW, FLOOR)}, so the contrast is gone")


def blank():
    return [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]


def fill(t, box, tile=GRASS):
    x0, x1, y0, y1 = box
    for yy in range(y0, y1 + 1):
        for xx in range(x0, x1 + 1):
            t[yy][xx] = tile


def euclid(a, b):
    return ((a[0] - b[0]) ** 2 + (a[1] - b[1]) ** 2) ** 0.5


def pill_of(variant):
    """The tile OUR pill stands on, and the armour it starts with."""
    if variant == "C":
        return PILL_FAR, PILL_HP_FULL
    if variant in ("B", "F"):
        return PILL_NEAR, PILL_HP_LOW
    if variant == "B2":
        return PILL_NEAR, PILL_HP_CAP
    if variant == "E":
        return PILL_E, PILL_HP_FULL
    return PILL_NEAR, PILL_HP_FULL


def cap_mode_of(variant):
    """C.ATTACK_TANK_HEAT_CAP_MODE for this arena.  Only F leaves the default,
    and it does so on the SAME GROUND as arena B -- one token apart, so the
    difference between the two runs is the rule and nothing else."""
    return FLOOR if variant == "F" else PROPORTIONAL


def base_of(variant):
    return BASE_E if variant == "E" else BASE


def spawns_of(variant):
    """(our pond, the foe's pond, the foe's RESPAWN pond)."""
    if variant == "E":
        return US_SPAWN_E, FOE_SPAWN_E, FOE_RESPAWN_E
    return US_SPAWN, FOE_SPAWN, FOE_RESPAWN


def room_tiles(variant):
    """Every tile a TANK can stand on -- the grass, minus the live pill (a live
    pillbox is impassable) and minus the foe's pond."""
    if variant == "E":
        boxes = [CORR_E]
    else:
        boxes = [ROOM]
    out = set()
    for (x0, x1, y0, y1) in boxes:
        for yy in range(y0, y1 + 1):
            for xx in range(x0, x1 + 1):
                out.add((xx, yy))
    pill, _hp = pill_of(variant)
    out.discard(pill)
    out.discard(spawns_of(variant)[2])   # the respawn pond stays water
    return out


def build(variant):
    """Returns terrain, pills, bases, starts for the variant."""
    t = blank()
    us, foe, respawn = spawns_of(variant)
    pill, hp = pill_of(variant)
    base = base_of(variant)
    if variant == "E":
        fill(t, OUTER_E, BUILDING)     # one solid block...
        fill(t, POCKET_E, GRASS)       # ...the sealed pocket for the pill,
        fill(t, CORR_E, GRASS)         # ...and the tank's corridor.
    else:
        fill(t, OUTER, BUILDING)
        fill(t, ROOM, GRASS)
    t[us[1]][us[0]] = DEEP_SEA
    t[foe[1]][foe[0]] = DEEP_SEA
    t[respawn[1]][respawn[0]] = DEEP_SEA
    # (x, y, owner, armour, speed).  Owner 0 in the file; the sidecar re-asserts
    # it after the foe joins, because a fresh connection can shuffle ownership.
    pills = [(pill[0], pill[1], 0, hp, MAP_PILL_SPEED)]
    bases = [(base[0], base[1], 0, 90, 90, 90)]
    # start 1 = ours (facing east, down the lane at E), start 2 = where the foe
    # is created, start 3 = where it comes back after a death (2 is grass by
    # then).  A start square is validated at EVERY placement, so a start the
    # sidecar has filled in cannot be reused.
    starts = [(us[0], us[1], 4), (foe[0], foe[1], 12),
              (respawn[0], respawn[1], 12)]
    return t, pills, bases, starts


def check(variant, terrain, pills, bases, starts):
    """Every number heat_pill_test.py reasons about, asserted here, so a moved
    tile fails at GENERATION instead of as a mystery run."""
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    mid = ((min(xs) + max(xs)) // 2, (min(ys) + max(ys)) // 2)
    assert mid == (126, 126), (
        f"variant {variant}: terrain midpoint is {mid}, not (126,126) -- "
        f"mapRead would shift every coordinate in this file")

    us, foe, respawn = spawns_of(variant)
    for (sx, sy, _d) in starts:
        assert terrain[sy][sx] is DEEP_SEA, (
            f"variant {variant}: start ({sx},{sy}) must be deep sea "
            f"(starts.c startsIsValidSquare, called at EVERY placement)")
    assert (foe[0], foe[1]) == (starts[1][0], starts[1][1]), (
        f"variant {variant}: start 2 must be the foe's pond -- the sidecar's "
        f"on_choose_start sends the idle bot there when it is created")
    assert (respawn[0], respawn[1]) == (starts[2][0], starts[2][1]), (
        f"variant {variant}: start 3 must be the foe's RESPAWN pond -- the "
        f"sidecar fills start 2 in, and a filled-in start is not a valid "
        f"placement any more")
    assert respawn != foe and respawn != us, (
        f"variant {variant}: the respawn pond must be its own tile")

    pill, hp = pill_of(variant)
    base = base_of(variant)
    for (px, py, _o, _a, _s) in pills:
        assert terrain[py][px] is not DEEP_SEA, (
            f"variant {variant}: pill ({px},{py}) is in the sea")
    for (bx, by, *_r) in bases:
        assert terrain[by][bx] is not DEEP_SEA, (
            f"variant {variant}: base ({bx},{by}) is in the sea")

    # ── THE SEAL.  Every neighbour of open ground is wall, and every open tile
    #    is WALL_DEEP tiles from the edge of the block: a shell that misses
    #    chews a wall tile down a stage at a time, and a one-tile wall has been
    #    breached inside a single run before now.
    open_tiles = {(x, y) for y in range(MAP_SIZE) for x in range(MAP_SIZE)
                  if terrain[y][x] not in (BUILDING,) and terrain[y][x] is not None}
    open_tiles |= {us, foe, respawn}
    block = OUTER_E if variant == "E" else OUTER
    x0, x1, y0, y1 = block
    for (xx, yy) in open_tiles:
        for (nx, ny) in ((xx - 1, yy), (xx + 1, yy), (xx, yy - 1), (xx, yy + 1)):
            if (nx, ny) in open_tiles:
                continue
            assert terrain[ny][nx] == BUILDING, (
                f"variant {variant}: the arena leaks at ({nx},{ny}) -- every "
                f"neighbour of open ground must be wall")
        assert (xx - x0 >= WALL_DEEP and x1 - xx >= WALL_DEEP
                and yy - y0 >= WALL_DEEP and y1 - yy >= WALL_DEEP), (
            f"variant {variant}: open tile ({xx},{yy}) is less than "
            f"{WALL_DEEP} tiles from the edge of the block -- shells will chew "
            f"through and the bot will go exploring on a boat")

    # ── THE REPOSITION GUARD.  An alive friendly pill with no friendly base
    #    inside PILL_FIRE_RANGE gets shot down BY US to be moved, which would
    #    forge exactly the armour drops arenas A and AK measure.
    assert euclid(base, pill) <= PILL_FIRE_RANGE, (
        f"variant {variant}: the alive pill at {pill} is "
        f"{euclid(base, pill):.2f} tiles from the only base -- outside "
        f"PILL_FIRE_RANGE ({PILL_FIRE_RANGE}), so the reposition pool would "
        f"shoot our own pill down to move it")

    # ── E MUST BE A REAL SIGHTING, not a ghost: the heat block returns early on
    #    `target.ghost`.  The brain view is 29x29 centred on the tank
    #    (brain_data.c:377-412), and attack_tank only considers tanks inside
    #    TANK_COMBAT_MAX_RANGE.
    tiles = room_tiles(variant)
    worst_foe = max(euclid(t, foe) for t in tiles)
    assert worst_foe <= min(VIEW_RADIUS, TANK_COMBAT_MAX_RANGE), (
        f"variant {variant}: the tank can stand {worst_foe:.2f} tiles from the "
        f"foe, outside the brain's {VIEW_RADIUS}-tile view / "
        f"TANK_COMBAT_MAX_RANGE ({TANK_COMBAT_MAX_RANGE}) -- E would drop to a "
        f"GHOST and the heat block returns before it ever looks at a pill")

    # ── PER-ARENA INVARIANTS.  d_pill_enemy / d_us_enemy / gun range are the
    #    exact quantities heat_pill_select prints, computed here over EVERY
    #    tile the tank could possibly be standing on.
    d_pe = euclid(pill, foe)
    # THE STANDOFF BAND is where the bot actually stands while it fights: the
    # close phase drives to TANK_COMBAT_STANDOFF_RANGE (= ENGAGE_RANGE, 7) and
    # the engage phase holds there.  "closer" and "in gun range" are asserted
    # over that band, not over every tile of the room -- a bot that charges to
    # point blank IS closer to E than P is, and heat_pill_select is right to
    # say `not_closer` on such a tick.  The test only needs the gate to open on
    # the ticks the bot spends where the steering means to put it.
    band = [t for t in tiles
            if abs(euclid(t, foe) - TANK_COMBAT_ENGAGE_RANGE) <= 1.0]
    assert band, f"arena {variant}: no tile sits in the standoff band around E"

    if variant == "C":
        # Arena C has to hold WHEREVER the tank ends up, because `not_closer`
        # is the FIRST gate and one ENTER anywhere would sink the arena.
        worst = max(tiles, key=lambda t: euclid(t, foe))
        assert all(d_pe >= euclid(t, foe) for t in tiles), (
            f"arena C: P at {pill} is {d_pe:.2f} from E, and the tank can "
            f"reach {worst} which is {euclid(worst, foe):.2f} from E -- park "
            f"there and heat_pill_select would say ENTER, not SKIP:not_closer")
    else:
        worst = min(band, key=lambda t: euclid(t, foe))
        assert all(d_pe < euclid(t, foe) for t in band), (
            f"arena {variant}: P at {pill} is {d_pe:.2f} tiles from E and the "
            f"standoff band reaches {worst}, only "
            f"{euclid(worst, foe):.2f} from E -- the FIRST gate in "
            f"heat_pill_select would read `not_closer` on the very ticks the "
            f"bot spends fighting")
        assert d_pe < TANK_COMBAT_ENGAGE_RANGE - 1, (
            f"arena {variant}: P is {d_pe:.2f} tiles from E against an engage "
            f"range of {TANK_COMBAT_ENGAGE_RANGE} -- too little margin; a "
            f"tank one tile inside its standoff would already read "
            f"`not_closer`")
        assert any(euclid(t, pill) <= GUN_RANGE_TILES for t in band), (
            f"arena {variant}: no tile in the {TANK_COMBAT_ENGAGE_RANGE}-tile "
            f"standoff band around E is within gun range "
            f"({GUN_RANGE_TILES:.2f} tiles) of P -- every HEAT_PILL line would "
            f"read SKIP:out_of_range and nothing else would ever be tested")
        assert euclid(us, pill) <= GUN_RANGE_TILES, (
            f"arena {variant}: P is {euclid(us, pill):.2f} tiles from our "
            f"spawn, outside gun range -- the bot would have to drive before "
            f"the gate could ever open")
        # After a death the foe comes back on the RESPAWN pond, so the two
        # distances the gate compares have to survive that move too.
        assert euclid(pill, respawn) < min(euclid(t, respawn) for t in band), (
            f"arena {variant}: once the foe respawns on {respawn}, P is "
            f"{euclid(pill, respawn):.2f} tiles from it while the standoff "
            f"band reaches {min(euclid(t, respawn) for t in band):.2f} -- the "
            f"arena would stop working after the first death")

    # THE HEALTH CAP, which is what every hp-shaped arena here turns on.
    mode = cap_mode_of(variant)
    allowed = heat_allowed_shots(hp, mode)
    if variant in ("A", "AK", "D"):
        assert allowed == HEAT_MAX_HITS, (
            f"arena {variant}: P starts on {hp} armour, which buys {allowed} "
            f"of the engine's {HEAT_MAX_HITS} halvings -- the volley would end "
            f"`hp_cap` instead of `maxed` and the full ladder would never "
            f"appear in the trace")
        assert d_pe < PILLBOX_RANGE_TILES, (
            f"arena {variant}: P is {d_pe:.2f} tiles from E, past "
            f"PILLBOX_RANGE ({PILLBOX_RANGE_TILES}) -- a heated pill that "
            f"cannot reach E makes the whole errand pointless")
    if variant == "B":
        assert allowed == 0, (
            f"arena B: P is on {hp} armour, which buys {allowed} shell(s) -- "
            f"in {mode} mode the gate only says SKIP:hp when the allowance is "
            f"ZERO")
        assert hp > 0, "arena B: P must be ALIVE -- a corpse is not a candidate"
        assert heat_allowed_shots(hp + 1, mode) > 0, (
            f"arena B sits at {hp} armour, but {hp + 1} would also buy nothing "
            f"-- use the HIGHEST hp whose allowance is 0, so the arena is "
            f"testing the edge of the cap and not a corpse")
    if variant == "F":
        # F is arena B's ground and arena B's pill, one cfg token apart.  Where
        # the proportional cap refuses outright, the floor rule fires once and
        # leaves the pill standing at exactly MIN_HP.
        assert mode == FLOOR, "arena F must run in floor mode"
        assert allowed == 1, (
            f"arena F: in floor mode a pill on {hp} armour buys {allowed} "
            f"shell(s), not the 1 this arena is about (hp - MIN_HP = "
            f"{hp} - {HEAT_MIN_HP})")
        assert heat_allowed_shots(hp, PROPORTIONAL) == 0, (
            f"arena F is only worth running because the PROPORTIONAL cap "
            f"refuses this same pill outright; it now allows "
            f"{heat_allowed_shots(hp, PROPORTIONAL)}, so F and B no longer "
            f"contrast")
        assert hp - allowed == HEAT_MIN_HP, (
            f"arena F: after its {allowed} shell(s) the pill would sit on "
            f"{hp - allowed} armour, not exactly the floor ({HEAT_MIN_HP}) -- "
            f"'stop at the floor' is the whole of the rule being benched")
        assert ladder_speed(allowed) > PILLBOX_MAX_FIRERATE, (
            f"arena F: {allowed} halving(s) already reach PILLBOX_MAX_FIRERATE")
    if variant == "B2":
        assert 0 < allowed < HEAT_MAX_HITS, (
            f"arena B2: P is on {hp} armour, which buys {allowed} of "
            f"{HEAT_MAX_HITS} -- this arena needs a PARTIAL allowance, so that "
            f"the volley ends `hp_cap` with the engine's ladder stopped short")
        assert d_pe < PILLBOX_RANGE_TILES, (
            f"arena B2: P is {d_pe:.2f} tiles from E, past PILLBOX_RANGE")
        # The engine's reload period after `allowed` halvings from calm: 100,
        # 50, 25, 12, 6.  The test asserts the trace stops exactly there.
        assert ladder_speed(allowed) > PILLBOX_MAX_FIRERATE, (
            f"arena B2: {allowed} halvings from PILLBOX_ATTACK_NORMAL already "
            f"reach PILLBOX_MAX_FIRERATE, so a capped volley would be "
            f"indistinguishable from arena A's full one")
    if variant == "D":
        # Three observed drops saturate the proxy; the decay between top-ups
        # (in BRAIN ticks, i.e. half the engine ticks) must not carry it back
        # under HEAT_MAX_FRAC or the skip flips to ENTER halfway through.
        peak = min(1.0, 3 * PILL_ANGER_BUMP)
        trough = peak - (D_REHEAT_PERIOD / 2.0) / PILL_ANGER_DECAY
        assert trough > HEAT_MAX_FRAC, (
            f"arena D: the proxy decays to {trough:.3f} between top-ups "
            f"{D_REHEAT_PERIOD} engine ticks apart, under "
            f"C.ATTACK_TANK_HEAT_MAX_FRAC ({HEAT_MAX_FRAC}) -- shorten "
            f"D_REHEAT_PERIOD")
        assert D_DROP_TO >= HEAT_MIN_HP, (
            f"arena D: the pre-heat drops P to {D_DROP_TO}, at or under the HP "
            f"floor ({HEAT_MIN_HP}) -- the skip reason would be `hp`, not "
            f"`already_hot`")

    # ── THE LANES.  A LIVE pill and a base of any owner both stop a shell
    #    (shot_path_clear / pillsIsPillHit), so neither may sit on the row the
    #    tank shoots E along.
    if variant != "E":
        assert pill[1] != LANE_Y, (
            f"variant {variant}: P at {pill} is ON the y={LANE_Y} tank-vs-E "
            f"lane -- a live pill stops any shell and would shield E from us")
        assert base[1] != LANE_Y, (
            f"variant {variant}: the base at {base} is on the y={LANE_Y} lane "
            f"-- a base of ANY owner stops a shell")
        assert base[1] != pill[1], (
            f"variant {variant}: the base at {base} shares P's row, so it can "
            f"sit on the tank-to-P sight line and forge a `no_los` skip")

    # ── ARENA E: the wall really is between us and P, and P's OWN bombardment
    #    cannot open a hole in the part of it we look through.
    if variant == "E":
        for yy in range(SEPARATOR_E[0], SEPARATOR_E[1] + 1):
            for xx in range(OUTER_E[0], OUTER_E[1] + 1):
                assert terrain[yy][xx] == BUILDING, (
                    f"arena E: ({xx},{yy}) is not BUILDING -- the separator "
                    f"between the corridor and the pocket has a hole in it and "
                    f"shot_path_clear would read CLEAR")
        assert pill[1] < SEPARATOR_E[0] and CORR_E[2] > SEPARATOR_E[1], (
            "arena E: the pill and the corridor must be on OPPOSITE sides of "
            "the separator")
        # P SHELLS E THROUGH THE WALL, so the separator is not permanent.
        # pillsUpdate picks the closest non-allied tank in PILLBOX_RANGE with no
        # line-of-sight test at all (pillbox.c:357-405), and a BUILDING takes
        # BUILDING_LIFE + 1 = 5 shells to go to RUBBLE (building.h:43,
        # buildingAddItem).  P reloads every PILLBOX_ATTACK_NORMAL (100) engine
        # ticks and nothing in this arena ever angers it, so clearing one
        # SEPARATOR_ROWS-deep lane costs it about
        #     5 x 3 x 100 = 1500 engine ticks
        # and heat_pill_test.py caps arena E's run under that (see TICKS["E"]).
        #
        # The belt to that brace: even a fully chewed lane is P's OWN diagonal,
        # and our sight line to P is a different one.  They may share at most a
        # single separator tile -- one hole is not a peephole, because the other
        # two rows of our line are still standing.
        #
        # Only the tiles where the gate actually REACHES the line-of-sight test
        # matter.  heat_pill_select tests not_closer and out_of_range first, so
        # from anywhere else a breach could not produce an ENTER however wide
        # the hole -- the verdict would still be one of the earlier gates.
        p_fire = set(bresenham(pill, foe)) | set(bresenham(pill, respawn))
        sep_rows = range(SEPARATOR_E[0], SEPARATOR_E[1] + 1)
        reaches_los = [t for t in tiles
                       if euclid(pill, foe) < euclid(t, foe)
                       and euclid(t, pill) <= GUN_RANGE_TILES]
        assert reaches_los, (
            "arena E: there is no tile the tank can stand on where the gate "
            "gets as far as the line-of-sight test -- every decision would "
            "read not_closer or out_of_range and the arena would prove nothing")
        for t in reaches_los:
            shared = {c for c in bresenham(t, pill)
                      if c in p_fire and c[1] in sep_rows}
            assert len(shared) <= 1, (
                f"arena E: from {t} -- a tile where the gate DOES reach the "
                f"line-of-sight test -- our sight line crosses the separator "
                f"on {sorted(shared)}, all of which P's own line of fire chews. "
                f"Park there once P has breached and SKIP:no_los flips to "
                f"ENTER")
        depth = SEPARATOR_E[1] - SEPARATOR_E[0] + 1
        assert depth >= 3, (
            f"arena E: the separator is only {depth} row(s) deep; P clears a "
            f"lane at 5 shells a tile and would be through it inside the run")


def bresenham(a, b):
    """Integer tile line, good enough to reason about which wall tiles a shell
    passes through (the engine's own shell walk is finer, but a tile this
    misses is a tile the shell only clips a corner of)."""
    x0, y0 = a
    x1, y1 = b
    out = []
    dx, dy = abs(x1 - x0), abs(y1 - y0)
    sx = 1 if x0 < x1 else -1
    sy = 1 if y0 < y1 else -1
    err = dx - dy
    while True:
        out.append((x0, y0))
        if (x0, y0) == (x1, y1):
            break
        e2 = 2 * err
        if e2 > -dy:
            err -= dy
            x0 += sx
        if e2 < dx:
            err += dx
            y0 += sy
    return out


# ── the scenario sidecar ──────────────────────────────────────────────────
SIDECAR_TEMPLATE = '''\
-- Scenario sidecar for tests/heat_pill_{V}.map (auto-loaded as
-- <map>.scenario.lua).  GENERATED by tests/generate_heat_pill_map.py -- edit
-- the generator, not this file.
--
-- Companion to tests/heat_pill_test.py arena {V}.
--
-- FOUR JOBS.
--
-- 1. OWNERSHIP.  The .map file can only say "owner 0", so the pill and the
--    base are handed to slot 0 (the -bots tank) and re-asserted after the foe
--    joins: a fresh connection can shuffle pill ownership.
--
-- 2. THE ENEMY, AND WHY IT IS PUT ASHORE.  Spawn one scripted IDLE bot
--    (tests/brains/idle.lua) on team 1 at start 2, the pond at
--    ({FOE_X},{FOE_Y}), then fill that pond in under it.  It never moves,
--    never shoots and never picks anything up, so d(P,E) and d(us,E) -- the
--    two numbers heat_pill_select compares -- are constants.
--
--    IT HAS TO BE ON LAND.  Left afloat it dies to the FIRST shell that
--    touches it: tankInGameDamage takes the boat away on any hit
--    (src/bolo/tank.c:1258-1265) and a tank standing on DEEP_SEA without a
--    boat drowns on the next update (tank.c:575).  Our own pill is 4.1 tiles
--    from it and shells it unprompted, so a floating enemy died in the middle
--    of the heat volley on the first run of this arena -- the target vanished
--    at hits 3-of-4, attack_tank went invalid and the volley disappeared
--    without an exit.  On grass the same tank has TANK_FULL_ARMOUR and rides
--    out the whole volley.  Start 3 exists because start 2 is grass by then
--    and a filled-in start is not a valid placement any more.
--
-- 3. FILLING OUR SPAWN POND, AND SPAWNING THE ENEMY ONLY AFTERWARDS.  A start
--    square has to be DEEP SEA at placement time, which leaves a one-tile hole
--    in our room.  It is filled back to GRASS once our tank is ashore AND off
--    it -- filling it underneath the tank leaves the boat state wedged.
--
--    THE ENEMY IS NOT SPAWNED UNTIL THAT HAS HAPPENED.  heat_pill_fire holds
--    the brake (KEY_SLOWER) for as long as a volley runs, and on the first run
--    of this arena the enemy existed from tick 2: the bot opened a volley at
--    brain tick 12 while still sitting in its boat on the spawn pond, braked,
--    and never came ashore at all -- 117 ticks of firing from the water, no
--    hits, and the pond never filled because the tank never left it.  With no
--    enemy on the map the bot simply explores, comes ashore, and the fight
--    starts on dry land.
--
-- 4. THE TRACE.  Every change of ARMOUR, OWNER, IN_TANK or **SPEED** on our
--    pill is written to heat_pill_{V}_trace.log in SIM ticks, one row per
--    change.  SPEED IS THE ENGINE'S ANGER (scenario.c:262): 100 =
--    PILLBOX_ATTACK_NORMAL, 6 = PILLBOX_MAX_FIRERATE.  That column is the
--    engine-authoritative evidence the test asserts on -- the brain's
--    `pill.anger` is only a proxy and never appears here.
--
-- Rows are `tick x y armour owner in_tank speed`.  x/y are the pill's ORIGINAL
-- tile -- its identity in this file -- not its live position, because a pill
-- that is picked up rides along with the tank that took it.
{EXTRA_HEADER}
local TRACE      = "heat_pill_{V}_trace.log"
local OURS       = {{ {{ {PILL_X}, {PILL_Y} }} }}
local SPAWN      = {{ {US_X}, {US_Y} }}
local FOE_SPAWN  = {{ {FOE_X}, {FOE_Y} }}
local FOE_BRAIN  = "{FOE_BRAIN}"
local GRASS      = 7
local FILL_TICK  = 60            -- engine ticks: not before this
local SPAWN_WAIT = 20            -- engine ticks after our pond is filled
local LAND_WAIT  = 20            -- engine ticks after the foe is created
local p0         = 0

local last        = {{}}
local filled      = false
local traced      = nil          -- {{ {{pill_index, tile_x, tile_y}}, ... }}
local foe_player  = nil
local foe_tick    = nil
local foe_landed  = false
local spawn_tried = false
{EXTRA_STATE}
local function same(p, t) return p.x == t[1] and p.y == t[2] end

-- Resolve OURS (tile coords) to PILL INDICES, once.  Everything after this
-- reads the pill BY INDEX and never by coordinate: a pill that is picked up
-- moves with the carrier, so a coordinate match would quietly stop finding it
-- at exactly the moment the trace exists to prove nobody took it.
local function resolve(g)
  local out = {{}}
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      for _, t in ipairs(OURS) do
        if same(p, t) then out[#out + 1] = {{ i, t[1], t[2] }} end
      end
    end
  end
  return out
end

local function own_everything(g)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, p0) end
  end
  for i = 1, g.num_bases() do
    if g.base(i) then
      g.set_base_owner(i, p0)
      g.set_base_stock(i, 90, 90, 90)
    end
  end
end

function on_setup(g)
  own_everything(g)
  g.set_team(p0, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y armour owner in_tank speed\\n") f:close() end
end

-- START 1 IS OURS, START 2 IS EVERYBODY ELSE'S -- written that way round, and
-- NOT as `if p == foe_player then return 2 end`, because on_choose_start fires
-- DURING the g.spawn_bot call that creates the foe (scenario.c: the create path
-- runs inside serverSimCreateBot), i.e. before spawn_bot has returned and
-- before foe_player has been assigned.  A `foe_player`-keyed rule therefore
-- returns nil on the one placement that matters and the engine picks for
-- itself: measured on the first run of this arena, the idle bot landed on
-- start 1 -- OUR pond -- and heat_pill_select spent 300 ticks printing
-- d_us_enemy{{0.0}} SKIP:not_closer at an enemy sitting on our own hull.
function on_choose_start(g, p)
  if p == p0 then return 1 end
  -- Start 2 while the foe's pond is still water, start 3 once it has been
  -- filled in.  A start square is re-validated at every placement, so the
  -- filled tile stops being a legal spawn the moment it turns to grass.
  if foe_landed then return 3 end
  return 2
end

function on_tick(g, tick)
  -- 1. Our spawn pond, back to grass -- but only once the tank is ASHORE and
  --    off it.  Filling the tile while the tank still sits on it leaves the
  --    boat state stuck.
  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = tick
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "HEAT_PILL_{V} filled the spawn pond at (%d,%d) with grass at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end

  -- 2. The enemy tank, once, and only after that: a volley started while we
  --    are still afloat on the pond brakes the tank and it never comes ashore.
  if filled and not spawn_tried and tick >= filled + SPAWN_WAIT then
    spawn_tried = true
    local s, err = g.spawn_bot("HeatFoe", FOE_BRAIN, 1, nil)
    if s == nil then
      g.message("HEAT_PILL_{V} spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      foe_tick = tick
      g.set_team(s, 1)
      g.message("HEAT_PILL_{V} foe slot=" .. tostring(s) .. " team=1 at t="
                .. tostring(tick))
      own_everything(g)
    end
  end

  -- 3. Put the enemy ashore, so one shell from our own pillbox cannot sink it
  --    out from under the volley.
  if foe_tick and not foe_landed and tick >= foe_tick + LAND_WAIT then
    local fk = g.tank(foe_player)
    if fk and fk.mx == FOE_SPAWN[1] and fk.my == FOE_SPAWN[2] then
      foe_landed = true
      g.set_tile(FOE_SPAWN[1], FOE_SPAWN[2], GRASS)
      g.message(string.format(
        "HEAT_PILL_{V} put the foe ashore at (%d,%d) at t=%d",
        FOE_SPAWN[1], FOE_SPAWN[2], tick))
    end
  end

  if not traced then traced = resolve(g) end
{EXTRA_TICK}
  for _, e in ipairs(traced) do
    local p = g.pill(e[1])
    if p then
      local in_tank = p.in_tank and 1 or 0
      local sig = p.armour .. "/" .. p.owner .. "/" .. in_tank .. "/" .. p.speed
      if last[e[1]] ~= sig then
        local f = io.open(TRACE, "a")
        if f then
          f:write(string.format("%d %d %d %d %d %d %d\\n",
                                tick, e[2], e[3], p.armour, p.owner, in_tank,
                                p.speed))
          f:close()
        end
        last[e[1]] = sig
      end
    end
  end
end
'''

D_EXTRA_HEADER = '''--
-- ARENA D ALSO PRE-HEATS THE BRAIN'S PROXY, WITHOUT TOUCHING THE ENGINE.
-- world.lua's pill.anger bumps by C.PILL_ANGER_BUMP on every OBSERVED armour
-- DROP and never on a rise, so knocking the armour down by one and putting it
-- straight back is a free bump: three of them saturate the proxy at 0.9999,
-- past C.ATTACK_TANK_HEAT_MAX_FRAC (0.75), while the pill stays at full HP so
-- the skip reason is `already_hot` and not `hp`.  The proxy decays 1/3000 per
-- BRAIN tick, so it is re-topped every D_REHEAT_PERIOD engine ticks for the
-- whole run.
--
-- The ENGINE's own anger -- pill.speed -- never moves: set_pill_armour halves
-- nothing.  The trace showing a flat 100 next to a brain that skips for
-- `already_hot` is the point of this arena, not a defect in it.'''

D_EXTRA_STATE = '''
local FIRST_DROP    = {FIRST_DROP}
local DROP_GAP      = {DROP_GAP}
local DROP_HOLD     = {DROP_HOLD}
local REHEAT_PERIOD = {REHEAT_PERIOD}
local DROP_TO       = {DROP_TO}
local FULL_HP       = {FULL_HP}
local next_drop     = FIRST_DROP
local opening_left  = 3
local restore_at    = nil
'''

D_EXTRA_TICK = '''
  -- Pre-heat: drop one armour, hold it long enough for the brain (which thinks
  -- once every SECOND engine tick) to see it, then put it straight back.
  if traced and traced[1] then
    local idx = traced[1][1]
    local p = g.pill(idx)
    if p and not p.in_tank and (p.armour or 0) > 0 then
      if restore_at and tick >= restore_at then
        g.set_pill_armour(idx, FULL_HP)
        restore_at = nil
      elseif not restore_at and tick >= next_drop then
        g.set_pill_armour(idx, DROP_TO)
        restore_at = tick + DROP_HOLD
        if opening_left > 1 then
          opening_left = opening_left - 1
          next_drop = tick + DROP_GAP
        else
          next_drop = tick + REHEAT_PERIOD
        end
      end
    end
  end
'''


B_EXTRA_HEADER = '''--
-- THIS ARENA ALSO HOLDS THE PILL DOWN AT ITS MAP ARMOUR, because THE BOT
-- REPAIRS IT.  Both hp arenas turn on a pill that is deliberately hurt -- B at
-- {HOLD_HP} armour so the health cap allows nothing, B2 at 10 so it allows
-- exactly two of the engine's four halvings -- and in the first measured run
-- of arena B the builder pool sent the man out and the engine trace read
-- `286 126 125 15` at sim tick 286.  By the time an enemy existed the gate was
-- looking at a 15-hp pill and said ENTER.  A cfg token is the real fix (the
-- test passes BUILDER_POOL_TREES_TOPUP=99, which prices the errand past the
-- tank's whole woodpile); this clamp is the belt to that brace, and it only
-- ever lowers the armour, so it never undoes the volley's own damage.
--
-- Knocking it back down is an armour DROP, so world.lua bumps the anger proxy
-- -- but `hp` is tested BEFORE `already_hot` in heat_pill_select, so the skip
-- reason is unaffected.'''

B_EXTRA_STATE = '''
local HOLD_HP = {HOLD_HP}
'''

B_EXTRA_TICK = '''
  -- Hold the pill at its staged armour: undo any repair the moment it lands.
  if traced and traced[1] then
    local idx = traced[1][1]
    local p = g.pill(idx)
    if p and not p.in_tank and (p.armour or 0) > HOLD_HP then
      g.set_pill_armour(idx, HOLD_HP)
    end
  end
'''


def sidecar_text(variant):
    pill, _hp = pill_of(variant)
    us, foe, _respawn = spawns_of(variant)
    extra_header, extra_state, extra_tick = "", "", ""
    if variant in ("B", "B2", "F"):
        _hold = pill_of(variant)[1]
        extra_header = B_EXTRA_HEADER.format(HOLD_HP=_hold)
        extra_state = B_EXTRA_STATE.format(HOLD_HP=_hold)
        extra_tick = B_EXTRA_TICK
    if variant == "D":
        extra_header = D_EXTRA_HEADER
        extra_state = D_EXTRA_STATE.format(
            FIRST_DROP=D_FIRST_DROP, DROP_GAP=D_DROP_GAP, DROP_HOLD=D_DROP_HOLD,
            REHEAT_PERIOD=D_REHEAT_PERIOD, DROP_TO=D_DROP_TO,
            FULL_HP=PILLS_MAX_HEALTH)
        extra_tick = D_EXTRA_TICK
    return SIDECAR_TEMPLATE.format(
        V=variant, PILL_X=pill[0], PILL_Y=pill[1],
        US_X=us[0], US_Y=us[1], FOE_X=foe[0], FOE_Y=foe[1],
        FOE_BRAIN=FOE_BRAIN, EXTRA_HEADER=extra_header,
        EXTRA_STATE=extra_state, EXTRA_TICK=extra_tick)


def write(variant, output):
    terrain, pills, bases, starts = build(variant)
    check(variant, terrain, pills, bases, starts)

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

    side = Path(output).with_suffix("").with_suffix("")
    side = Path(str(Path(output))[:-4] + ".scenario.lua")
    side.write_text(sidecar_text(variant), encoding="utf-8", newline="\n")

    pill, hp = pill_of(variant)
    _us, foe, _respawn = spawns_of(variant)
    print(f"Wrote {output} ({Path(output).stat().st_size} bytes) and "
          f"{side.name} [variant {variant}: pill {pill} hp={hp}, foe {foe}, "
          f"d(P,E)={euclid(pill, foe):.2f}]")


def main():
    args = sys.argv[1:]
    variants = [args[0]] if args and args[0] in VARIANTS else list(VARIANTS)
    here = Path(__file__).parent
    for v in variants:
        out = (args[1] if len(args) > 1 and len(variants) == 1
               else str(here / f"heat_pill_{v}.map"))
        write(v, out)


if __name__ == '__main__':
    main()
