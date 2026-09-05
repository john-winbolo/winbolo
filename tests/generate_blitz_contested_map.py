#!/usr/bin/env python3
"""
Generate the blitz-contested arenas (companion to tests/blitz_contested_test.py).

WHAT THE ARENAS ARE FOR
    C.BLITZ_CONTESTED_ALL_SUICIDERS (constants.lua, 2026-09-05): when the
    commander of a blitz fires GO on a pill that has a live HOSTILE TANK within
    C.BLITZ_CONTESTED_RANGE (9) tiles of it, the take is CONTESTED and every
    member of the party -- all soldiers AND the commander -- is designated a
    temporary pill_suicider, regardless of BLITZ_MIN_SUICIDERS.  The commander
    re-checks on its replans while the take is live, so a defender that arrives
    after GO flips the party too.

    Two arenas, IDENTICAL in every respect but ONE TILE: where the hostile tank
    is parked.  That is the whole experiment -- same terrain, same pill, same
    spawns, same tokens, same seed, and the only thing that differs is the
    pill-to-enemy distance.

      A  CONTESTED.  The enemy sits FOE_A, {DA} tiles from the pill (<= 9).
         Expect: BLITZ_GO, a BLITZ_CONTESTED line, and all three attackers
         carrying blitz_suicider=true reason=blitz_contested.
      B  CONTROL.    The enemy sits FOE_B, {DB} tiles from the pill (> 9), and
         is still plainly VISIBLE from the take (well inside the engine's
         14-tile brain view window, which the test asserts from the attackers'
         own ENGINE_DUMP lines -- otherwise "no BLITZ_CONTESTED" would only
         prove the bots were blind).
         Expect: BLITZ_GO, NO BLITZ_CONTESTED, and nobody a suicider
         (BLITZ_MIN_SUICIDERS is 0, so nothing else designates either).

THE ARENA
    Everything is written between x 106..146 and y 106..146, symmetric about
    (126,126), so mapRead's recenter is a no-op and in-game tiles match this
    file.  Vertical bands, west to east:
      x 106..133   FIELD  (grass) -- the three attackers' world, and the pill
      x 134..135   MOAT   (deep sea, full height of the body)
      x 136..146   ISLAND (grass) -- the enemy, permanently out of reach

    (129,126)  THE PILL.  Owned by the enemy player and held at full armour by
               the sidecar, so it is a HARD TAKE (>= HARD_TAKE_MIN_HP): squad
               elects a commander for it and the commander opens a blitz call,
               which is the only way this test gets a GO at all.
    (114,120)  attacker 0 start pond    (110,116) its base
    (114,126)  attacker 1 start pond    (110,126) its base
    (114,132)  attacker 2 start pond    (110,136) its base
               All three are ~15-16 tiles from the pill, near enough to the
               same distance that no one of them owns the take by geometry.
    (136,126)  A: the enemy's start pond, on the island, 7 tiles from the pill.
    (136,138)  B: the same pond moved south, 13.9 tiles from the pill.

WHY THE ENEMY IS ON AN ISLAND IN A POND
    Two problems to dodge at once.

    * It must not be WORTH ATTACKING.  attack_tank prices an unreachable tank
      at INF and never picks it, so a moat of deep sea with no river anywhere
      (hence no boat, ever) keeps the three attackers on the pill.  This
      matters more here than in the other arenas: in variant A the enemy ends
      up within SQUAD_BLITZ_PREEMPT_TANK_TILES (10) of the charging blitzers,
      which is exactly the range at which a committed blitz is ALLOWED to yield
      to attack_tank -- it only doesn't because the goal cannot be priced.
    * It must not be SHOT BY THE PILL.  A pillbox fires at anything hostile
      within ~8 tiles, and in variant A the enemy is 7 from the pill.  So the
      pill is the ENEMY'S OWN: a pill never shoots its owner, and an
      enemy-owned pill is also what attack_pill wants (owner reads "hostile").

    The start square has to be DEEP SEA (starts.c startsIsValidSquare), so the
    enemy's start is a one-tile pond cut into the island.  It runs
    tests/brains/idle.lua -- it never moves, never fires, never leaves the pond
    -- so it is a stationary "within 9 tiles" fact and nothing else.

WHY `-gametype open -teams 3,1`
    OPEN hands every tank the full 40/40 loadout (gametype.c) so three tanks
    can actually shoot a 15 HP pill down, and it leaves -teams alone
    (TOURNAMENT would override it).  "3,1" is the CONTIGUOUS-BLOCK form
    (servermain.c): bots 0,1,2 are the allied party under test and bot 3 is the
    enemy.  "-teams 3" without the comma is the round-robin form and would make
    the three attackers enemies of each other, which tests nothing.

Usage:
    python tests/generate_blitz_contested_map.py [A|B] [output_path]
    Default: writes both, as tests/blitz_contested_<variant>.map
Writes <output>.map and the matching <output>.scenario.lua sidecar.
"""

import math
import struct
import sys
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from generate_take_cover_map import encode_map_runs   # noqa: E402

GRASS = 7
DEEP_SEA = None                # background sentinel (unwritten = deep sea)
MAP_SIZE = 256

VARIANTS = ("A", "B")

# -- Geometry (the test runner imports these for its assertions) -------------
BODY_Y = (106, 146)
FIELD_X = (106, 133)           # the attackers' world
MOAT_X = (134, 135)            # never written -> deep sea, full body height
ISLAND_X = (136, 146)          # the enemy's island

PILL = (129, 126)              # brain pill #0, owned by the enemy
BOT_SPAWNS = [(114, 120), (114, 126), (114, 132)]
BASES = [(110, 116), (110, 126), (110, 136)]
FOE_SPAWN = {"A": (136, 126), "B": (136, 138)}

FOE_PLAYER = 3                 # -bots 4, so the idler is slot 3
ALLIES = (0, 1, 2)

# -- Object state ------------------------------------------------------------
NEUTRAL = 255
PILL_HP = 15                   # PILLS_MAX_HEALTH: a HARD take, so a blitz forms
HARD_TAKE_MIN_HP = 12          # constants.lua HARD_TAKE_MIN_HP
SLOW_RELOAD = 200              # pillbox.c reload counter; NORMAL is 100, so
                               # this pill shoots half as often -- a real take
                               # that does not delete a blitzer mid-rally
FULL_STOCK = 90

# -- Brain/engine constants these arenas are designed against ----------------
BLITZ_CONTESTED_RANGE = 9      # constants.lua -- the thing under test
BRAIN_VIEW_HALF = 14           # brain_data.c: the tank view rect is +/-14 tiles
PILLBOX_RANGE = 8              # tiles a pill can shoot (pillbox.c)
SQUAD_BLITZ_PREEMPT_TANK_TILES = 10   # constants.lua
BLITZ_MIN_SUICIDERS = 0        # constants.lua default: nothing else designates
BLITZ_PARTY = (3, 4)           # the "blitz=3/4" token the test passes: GO needs
                               # all three tanks, and the call accepts up to 4


def euclid(a, b):
    return math.hypot(a[0] - b[0], a[1] - b[1])


def cheb(a, b):
    return max(abs(a[0] - b[0]), abs(a[1] - b[1]))


def ponds(variant):
    return list(BOT_SPAWNS) + [FOE_SPAWN[variant]]


def make_map(variant):
    t = [[DEEP_SEA] * MAP_SIZE for _ in range(MAP_SIZE)]
    y0, y1 = BODY_Y
    for yy in range(y0, y1 + 1):
        for xx in range(FIELD_X[0], FIELD_X[1] + 1):
            t[yy][xx] = GRASS
        for xx in range(ISLAND_X[0], ISLAND_X[1] + 1):
            t[yy][xx] = GRASS
        # MOAT_X is left as background deep sea.
    for (px, py) in ponds(variant):
        t[py][px] = DEEP_SEA
    return t


SIDECAR = '''\
-- Scenario sidecar for tests/blitz_contested_{VAR}.map -- GENERATED by
-- tests/generate_blitz_contested_map.py, do not edit by hand.
--
-- Variant {VAR}: the enemy tank is parked at ({FX},{FY}), {DIST} tiles from the
-- pill at ({PX},{PY}).  C.BLITZ_CONTESTED_RANGE is {RANGE}, so this take is
-- {VERDICT}.
--
-- Four jobs.
--
-- 1. THE PILL IS THE ENEMY'S.  attack_pill only bids on a pill whose owner
--    reads "hostile", and a pill never shoots its own owner -- which is what
--    lets variant A park the enemy {DA} tiles away without the pill deleting
--    it.  Held at full armour so the take stays HARD (>= HARD_TAKE_MIN_HP) and
--    a blitz commander is elected for it; that is the only route to a GO.
--
-- 2. THE BASES ARE THE ATTACKERS'.  One each, full stock, well west of the
--    pill: the bots start full so refuel never wins, but a base gives each of
--    them a real fallback goal instead of wandering.
--
-- 3. PINNED STARTS.  on_choose_start hands every tank its own pond so the
--    three attackers cannot swap ends with each other or with the enemy, and a
--    respawn comes back to the same tile.  Start 4 is the enemy's.
--
-- 4. FILL THE ATTACKERS' PONDS once all three are ashore.  A start square has
--    to be DEEP SEA (starts.c startsIsValidSquare), and a tank that later
--    drives over one without a boat drowns -- noise this test does not want.
--    The ENEMY's pond is deliberately left alone: the idler never moves, so it
--    sits in its boat all round, and filling the water under it is not
--    something this test needs to find out about.
local PILL_XY   = {{ {PX}, {PY} }}
local BASES     = {{ {{ {B0X}, {B0Y}, 0 }}, {{ {B1X}, {B1Y}, 1 }}, {{ {B2X}, {B2Y}, 2 }} }}
local ALLY_PONDS = {{ {{ {S0X}, {S0Y} }}, {{ {S1X}, {S1Y} }}, {{ {S2X}, {S2Y} }} }}
local FOE_PLAYER = {FOE}
local FULL      = {FULL}
local PILL_HP   = {HP}
local GRASS     = 7

local pill_n       = nil
local ponds_filled = false
local said         = false

local function index_pill(g)
  if pill_n then return end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == PILL_XY[1] and p.y == PILL_XY[2] then pill_n = i end
  end
end

-- Re-asserted for the first few ticks as well as in on_setup: the -bots slots
-- are not guaranteed to be filled in by the time on_setup runs, and an owner
-- set to a slot that does not exist yet would silently do nothing.
local function own_everything(g)
  index_pill(g)
  if pill_n then
    g.set_pill_owner(pill_n, FOE_PLAYER)
    local p = g.pill(pill_n)
    if p and not p.in_tank and p.armour < PILL_HP then
      g.set_pill_armour(pill_n, PILL_HP)
    end
  end
  for _, b in ipairs(BASES) do
    for i = 1, g.num_bases() do
      local bb = g.base(i)
      if bb and bb.x == b[1] and bb.y == b[2] then
        g.set_base_owner(i, b[3])
        g.set_base_stock(i, FULL, FULL, 90)
      end
    end
  end
  for _, a in ipairs({{0, 1, 2}}) do g.set_team(a, 0) end
  g.set_team(FOE_PLAYER, 1)
end

function on_setup(g)
  own_everything(g)
end

-- Start 1/2/3 are the three attackers' ponds, in slot order; start 4 is the
-- enemy's.  Anything else the engine may place goes wherever it likes.
function on_choose_start(g, p)
  if p == 0 then return 1 end
  if p == 1 then return 2 end
  if p == 2 then return 3 end
  if p == FOE_PLAYER then return 4 end
  return nil
end

function on_tick(g, tick)
  if tick <= 20 then own_everything(g) end
  if not said and pill_n then
    local p = g.pill(pill_n)
    if p then
      said = true
      g.message(string.format(
        "BLITZ_CONTESTED_ARENA {VAR}: pill#%d (%d,%d) owner=%d armour=%d; enemy at ({FX},{FY}), %s tiles away ({VERDICT})",
        pill_n - 1, p.x, p.y, p.owner, p.armour, "{DIST}"))
    end
  end
  -- Fill the ATTACKERS' ponds once none of them is afloat any more (job 4).
  if not ponds_filled and tick > 200 then
    local afloat = false
    for _, a in ipairs({{0, 1, 2}}) do
      local t = g.tank(a)
      if t and t.boat then afloat = true end
    end
    if not afloat then
      ponds_filled = true
      for _, q in ipairs(ALLY_PONDS) do g.set_tile(q[1], q[2], GRASS) end
      g.message("BLITZ_CONTESTED_ARENA ally spawn ponds filled at tick " .. tostring(tick))
    end
  end
end
'''


def write_sidecar(path, variant):
    foe = FOE_SPAWN[variant]
    d = euclid(PILL, foe)
    text = SIDECAR.format(
        VAR=variant,
        PX=PILL[0], PY=PILL[1],
        FX=foe[0], FY=foe[1],
        DIST="%.1f" % d,
        DA="%.1f" % euclid(PILL, FOE_SPAWN["A"]),
        RANGE=BLITZ_CONTESTED_RANGE,
        VERDICT="CONTESTED" if d <= BLITZ_CONTESTED_RANGE else "NOT contested",
        B0X=BASES[0][0], B0Y=BASES[0][1],
        B1X=BASES[1][0], B1Y=BASES[1][1],
        B2X=BASES[2][0], B2Y=BASES[2][1],
        S0X=BOT_SPAWNS[0][0], S0Y=BOT_SPAWNS[0][1],
        S1X=BOT_SPAWNS[1][0], S1Y=BOT_SPAWNS[1][1],
        S2X=BOT_SPAWNS[2][0], S2Y=BOT_SPAWNS[2][1],
        FOE=FOE_PLAYER, FULL=FULL_STOCK, HP=PILL_HP)
    Path(path).write_text(text, encoding="utf-8", newline="\n")


def check_geometry(terrain, variant):
    foe = FOE_SPAWN[variant]

    # -- Recenter sanity: only WRITTEN tiles define the bounding box, and its
    #    integer midpoint must already be (126,126) or every coordinate shifts.
    xs = [x for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    ys = [y for y in range(MAP_SIZE) for x in range(MAP_SIZE)
          if terrain[y][x] is not DEEP_SEA]
    assert (min(xs) + max(xs)) // 2 == 126, (min(xs), max(xs))
    assert (min(ys) + max(ys)) // 2 == 126, (min(ys), max(ys))

    # -- Every start is deep sea (starts.c startsIsValidSquare), and no two
    #    tanks share one.
    for p in ponds(variant):
        assert terrain[p[1]][p[0]] is DEEP_SEA, p
    assert len(set(ponds(variant))) == 4, ponds(variant)

    # -- The moat runs the FULL height of the body: no land route to the
    #    island, and with no river anywhere no boat can ever be built, so
    #    attack_tank prices the enemy at INF for the whole round.
    for y in range(BODY_Y[0], BODY_Y[1] + 1):
        for x in range(MOAT_X[0], MOAT_X[1] + 1):
            assert terrain[y][x] is DEEP_SEA, (x, y)
    assert GRASS == 7, "the field is plain grass; no river => no boat, ever"
    assert FIELD_X[1] + 1 == MOAT_X[0] and MOAT_X[1] + 1 == ISLAND_X[0]

    # -- THE experiment: A is inside BLITZ_CONTESTED_RANGE and B is outside,
    #    and nothing else about the two arenas differs.
    d = euclid(PILL, foe)
    if variant == "A":
        assert d <= BLITZ_CONTESTED_RANGE, d
    else:
        assert d > BLITZ_CONTESTED_RANGE, d
        # ...and B is NOT a test of blindness: the enemy has to be inside the
        # engine's brain view window from the pill, or "no BLITZ_CONTESTED"
        # would prove nothing at all.
        assert cheb(PILL, foe) <= BRAIN_VIEW_HALF, cheb(PILL, foe)

    # -- The pill is the ENEMY'S (the sidecar sets that) precisely so it does
    #    not shoot the enemy tank down in variant A, where the two are inside
    #    the pill's firing range of each other.
    if variant == "A":
        assert d <= PILLBOX_RANGE + 1, (
            "variant A only needs an enemy-owned pill because the enemy sits "
            "inside pillbox range of it")

    # -- A HARD take, or squad never elects a commander, no call is opened and
    #    there is no GO to observe.
    assert PILL_HP >= HARD_TAKE_MIN_HP, (PILL_HP, HARD_TAKE_MIN_HP)

    # -- Nothing else designates a suicider, so every designation the test sees
    #    is the contested rule.
    assert BLITZ_MIN_SUICIDERS == 0, BLITZ_MIN_SUICIDERS

    # -- The three attackers are near enough to the same distance from the pill
    #    that none of them owns the take by geometry, and none starts inside
    #    the pill's range (so all three really do plan and approach, which is
    #    the window a blitz forms in).
    ds = [euclid(s, PILL) for s in BOT_SPAWNS]
    assert max(ds) - min(ds) < 2.0, ds
    for s, dd in zip(BOT_SPAWNS, ds):
        assert dd > PILLBOX_RANGE, (s, dd)

    # -- Bases: a real fallback goal that never beats the take, and far enough
    #    from the pill that the pill-reposition pool has no opinion about them.
    for s, b in zip(BOT_SPAWNS, BASES):
        assert euclid(s, b) < euclid(s, PILL), (s, b)
    for b in BASES:
        assert euclid(b, PILL) > PILLBOX_RANGE + 8, (b, euclid(b, PILL))

    # -- Everything in the band it belongs to.
    for p in [PILL] + BOT_SPAWNS + BASES:
        assert FIELD_X[0] <= p[0] <= FIELD_X[1], p
        assert BODY_Y[0] <= p[1] <= BODY_Y[1], p
    assert ISLAND_X[0] <= foe[0] <= ISLAND_X[1], foe
    assert BODY_Y[0] <= foe[1] <= BODY_Y[1], foe
    for p in BOT_SPAWNS + [foe]:
        assert p != PILL and p not in BASES, p


def write_variant(variant, output):
    terrain = make_map(variant)
    check_geometry(terrain, variant)
    foe = FOE_SPAWN[variant]

    # Pill records: x, y, owner, armour, speed(reload).  Written NEUTRAL and
    # handed to the enemy by the sidecar, which is the only place that knows a
    # player slot exists.
    pills = [(PILL[0], PILL[1], NEUTRAL, PILL_HP, SLOW_RELOAD)]
    # Base records: x, y, owner, armour, shells, mines.
    bases = [(b[0], b[1], i, FULL_STOCK, FULL_STOCK, 90)
             for i, b in enumerate(BASES)]
    # dir 4 = east in the 16-point start encoding: point every attacker at the
    # pill so none of them wastes its first second turning round.  The enemy's
    # heading is irrelevant (it never moves or fires).
    starts = [(s[0], s[1], 4) for s in BOT_SPAWNS] + [(foe[0], foe[1], 12)]

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

    sidecar = str(Path(output).with_suffix('')) + ".scenario.lua"
    write_sidecar(sidecar, variant)
    d = euclid(PILL, foe)
    print(f"wrote {output}")
    print(f"  {variant}: pill#0 {PILL} owner=p{FOE_PLAYER} hp={PILL_HP}; "
          f"enemy {foe} is {d:.1f} tiles away "
          f"({'CONTESTED' if d <= BLITZ_CONTESTED_RANGE else 'NOT contested'}, "
          f"range {BLITZ_CONTESTED_RANGE}); view cheb from pill {cheb(PILL, foe)} "
          f"(<= {BRAIN_VIEW_HALF} = visible)")
    print(f"  attackers {BOT_SPAWNS} bases {BASES}")
    print(f"wrote {sidecar}")


def main():
    args = list(sys.argv[1:])
    variants = list(VARIANTS)
    if args and args[0].upper() in VARIANTS:
        variants = [args.pop(0).upper()]
    explicit = args[0] if args else None
    if explicit and len(variants) != 1:
        print("an explicit output path needs an explicit variant", file=sys.stderr)
        return 1
    here = Path(__file__).parent
    for v in variants:
        out = explicit or str(here / f"blitz_contested_{v}.map")
        write_variant(v, out)
    return 0


if __name__ == "__main__":
    sys.exit(main())
