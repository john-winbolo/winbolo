-- Scenario sidecar for tests/defend_alarm_C.map (auto-loaded as <map>.scenario.lua).
-- Companion to tests/defend_alarm_test.py arena C.
--
-- ONE MAP, SEVEN ARENAS on the old host: the arena was chosen by the
-- DEFEND_ALARM_VARIANT environment variable the python driver set on the
-- WinBoloDS process.  There is no os.getenv in this sandbox and the gate
-- runner binds one arena to one file, so the variant is a constant below and
-- each arena is its own copy of this file beside its own copy of the map.  The
-- ground is identical in every one of them -- the generator took no variant
-- argument -- so the copies really are copies; what differs is which pond each
-- tank comes in on, which brain the enemy runs, and one cfg token.
--
--   A   DAMAGE ALARM.   Our tank spawns FAR (start 1, 16 tiles W of the pill).
--                       A scripted shooter parks 6 tiles south of the pill
--                       across the moat and shells it.  Then, DESPAWN_AFTER_HIT
--                       ticks after the FIRST hp drop, the script REMOVES the
--                       shooter outright -- the point of the arena: condition 1
--                       is a LIVE sighting, so the alarm has to go off on the
--                       tick the enemy stops being visible, not five seconds
--                       later when the damage window expires.
--   B   BUILD ALARM.    Our tank spawns FAR.  A scripted enemy parks 6 tiles
--                       NORTH of the pill (our side of the moat) and sends its
--                       LGM to wall (126,122), 4 tiles from the pill -- the
--                       outer ring of the alarm's build stamp.  It never fires,
--                       so trigger 2a can never arm and the only thing that can
--                       raise the alarm is the build.
--   B2  THE CONTROL.    Identical, except the enemy is told to PARK and do
--                       nothing, and THIS SCRIPT flips the same tile to a wall
--                       with game.set_tile.  A build appears inside the stamp
--                       with an enemy tank in the ring and NO hostile LGM ever
--                       seen there.  The alarm must NOT fire.
--   C   TOO CLOSE.      Arena A's shooter, but our tank spawns NEAR (start 2,
--                       6 tiles W of the pill), inside DEFEND_ALARM_MIN_DIST.
--                       Every condition but the third holds.
--   D   KEEL CONTROL.   Arena A's ground and arena A's shooter, run with
--                       cfg=DEFEND_ALARM_MODE=false.  Nothing in the world
--                       changes; the difference is entirely in the token.
--   W   WELL DEFENDED.  Arena A plus four parked tanks -- two ALLIES 6 tiles
--   WC  ITS CONTROL.    from the pill and two filler FOES 18 tiles out, or the
--                       allies moved out to 14 and 18.  NOT BUILT: they need
--                       six seats and four more park_at destinations, and
--                       nothing about the count they exist to produce is
--                       visible to a scenario.
--
-- OWNERSHIP.  The map's pill and our base belong to slot 0; the far-side base
-- belongs to the foe, which is what puts the front line between the two halves
-- so pill_portfolio stops calling our pill a BACK pill and the reposition pool
-- stops bidding to shoot it down and move it.  Every spawn names its own start
-- so the two tanks never swap ends.
--
-- THE ARENA FIELDS ITS OWN TANKS.  The driver pinned our bot with -bot-init
-- and the runner's -bots N seats come in with no init at all, so the GATE line
-- asks for bots=0 and both seats are taken here.
--
-- WHAT THE TRACE WAS FOR.  The driver read defend_alarm_<variant>_trace.log,
-- which this file used to write with io.open.  There is no io in the sandbox,
-- so every question it was asked -- was the pill hit, when was the shooter
-- removed, what happened to the wall tile, where was our tank -- is answered by
-- a local instead.

local VARIANT = "C"

local OUR_PILL = { 126, 126 }
local OUR_BASE = { 122, 122 }
local FOE_BASE = { 126, 133 }
local FOE_WALL = { 126, 122 }

local OUR_BRAIN = "../brains/GoalHunter_1.7/init.lua"
local FOE_BRAIN = "../tests/brains/shell_pill_then_flee.lua"

-- defend_alarm_test.py TOKENS for this arena, verbatim.
-- CAPTURE_PILL_BASE_COST prices the capture out so the bot stays an observer;
-- PILL_REPOSITION_ENABLED off stops the reposition pool bidding to shoot our
-- own pill down and move it, which would put our own shells on the pill the
-- alarm is counting hits on.
local OUR_TOKENS = "cfg=CAPTURE_PILL_BASE_COST=1e30;cfg=PILL_REPOSITION_ENABLED=false"

local OUR_SLOT, FOE_SLOT = 0, 1
local OUR_START, FOE_START = 2, 3

-- DEFEND_ALARM_MIN_DIST, condition 3's threshold.  Arena C exists to sit
-- inside it.
local MIN_DIST = 9
-- Ticks after the first hp drop at which arena A removes the shooter.  DOUBLED
-- from the old host's 200: this host's hook clock counts 100 a second where the
-- old one counted 50, and this is a duration.  400 ticks is about four
-- GOAL_REPLAN_INTERVALs, so the pool has had several chances to pick
-- defend_pill before the enemy vanishes, and the shooter is still alive and
-- still shooting when it happens.
local DESPAWN_AFTER_HIT = 400
-- Tick at which arena B2 flips the wall tile itself, doubled for the same
-- reason.  Late enough that the enemy has parked and the pill has been on the
-- watch list -- and so has its terrain snapshot -- for a while.
local B2_SETTILE_TICK = 1400

local our_player  = OUR_SLOT
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil
local hits        = 0        -- the trace's `hp` rows, counted
local first_hit_t = nil
local despawned   = false
local despawn_t   = nil      -- ...its `despawn` row
local settiled    = false
local settile_t   = nil      -- ...its `settile` row
local walled_t    = nil      -- ...its `wall` rows: the tick a wall appeared
local man_out_t   = nil      -- the tick the ENEMY's man first left his tank
local close_ticks = 0        -- ...its `bot` rows, asked one question in arena C
local min_dist    = nil

-- Arena B2 tells the enemy to PARK and do nothing; arena B tells it to WALL.
-- park_and_wall reads BRAIN_INIT_ARG and a value of "1" flattens to the bare
-- word, which is what the brain looks for -- and it looks for "park" only,
-- "wall" being its default, so arena B's token is a statement of intent rather
-- than a switch.  Both are written out even so: the two arenas differ in this
-- one line and a reader should not have to know a default to see it.
local function foe_init()
  return nil
end

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, owner) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
      g.set_base_stock(i, 90, 90, 90)
    elseif b and b.x == FOE_BASE[1] and b.y == FOE_BASE[2] and foe_player then
      g.set_base_owner(i, foe_player)
    end
  end
end

function on_setup(g)
  own_everything(g, our_player)
  g.set_team(our_player, 0)
end

-- Every spawn names its start, so this is only the fallback a RESPAWN takes --
-- arena A's and C's shooter sits inside PILL_FIRE_RANGE and does get killed,
-- and a wrong pond would put it on the far side of the moat.
function on_choose_start(g, p)
  if p == our_player then return OUR_START end
  if p == foe_player then return FOE_START end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local us, uerr = g.spawn_bot{ slot = OUR_SLOT, name = "Observer",
                                  brain = OUR_BRAIN, team = 0,
                                  start = OUR_START,
                                  init = g.init_tokens(OUR_TOKENS) }
    if us == nil then
      g.message("DEFEND_ALARM spawn_bot(us) failed: " .. tostring(uerr))
    end
    foe_player = FOE_SLOT
    local s, err = g.spawn_bot{ slot = FOE_SLOT, name = "Foe",
                                brain = FOE_BRAIN, team = 1,
                                start = FOE_START, init = foe_init() }
    if s == nil then
      foe_player = nil
      g.message("DEFEND_ALARM spawn_bot(foe) failed: " .. tostring(err))
    else
      g.set_team(FOE_SLOT, 1)
      g.message("DEFEND_ALARM " .. VARIANT .. " foe slot=" .. tostring(FOE_SLOT))
    end
    -- Two joins in one tick are two chances for the pill to change hands.
    own_everything(g, our_player)
  end
  if spawn_tried and tick < 120 then own_everything(g, our_player) end

  -- Our pill's armour, on every change.  The `hp` rows were read for the tick
  -- of the first DROP, which is what times arena A's despawn.
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        if last_hp and p.armour < last_hp then
          hits = hits + 1
          first_hit_t = first_hit_t or tick
        end
        last_hp = p.armour
      end
      break
    end
  end

  -- How close WE were to the pill: the `bot` rows, which arena C read to know
  -- the ticks it was inside MIN_DIST.
  local ot = g.tank(our_player)
  if ot and not ot.dead then
    local dx, dy = ot.mx - OUR_PILL[1], ot.my - OUR_PILL[2]
    local d = math.sqrt(dx * dx + dy * dy)
    if min_dist == nil or d < min_dist then min_dist = d end
    if d <= MIN_DIST then close_ticks = close_ticks + 1 end
  end

  -- The wall tile's terrain.  A build lands as a half-wall and finishes as a
  -- wall, and the driver counted either as "the man really built".
  if walled_t == nil then
    local wt = g.map_tile(FOE_WALL[1], FOE_WALL[2])
    if wt == g.TERRAIN.building or wt == g.TERRAIN.half_building then
      walled_t = tick
    end
  end

  -- The ENEMY's man.  This is the one thing that separates arena B from its
  -- control: in B he walks out and builds, in B2 he never leaves the tank and
  -- the wall is the script's.  game.builder answers it from the engine.
  if man_out_t == nil and foe_player then
    local fb = g.builder(foe_player)
    if fb and fb.state ~= "in_tank" then man_out_t = tick end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/defend_alarm_test.py arena C: the two things
-- it asked of the ENGINE, and which every one of its real assertions was scoped
-- BY.  The pill was hit, so conditions 1 and 2 both held; and our tank really
-- was inside DEFEND_ALARM_MIN_DIST of it, which is the whole of what makes this
-- arena different from A -- it is arena A with our tank on the near pond, and
-- if the near pond does not put us inside 9 tiles there is nothing for
-- condition 3 to be the only thing refusing.
--
-- LEFT BEHIND, because it is print2 and nothing else: that on those close ticks
-- the row was REJECTED alarm_off:too_close rather than for any other condition,
-- and that not one DEFEND_ALARM_ON was printed on any tick the engine had us
-- that close.  Both are the assertion; what is left is its scope.  A PASS here
-- says the arena put the tank where it meant to and says nothing about
-- condition 3.
--
-- GATE: ticks=2200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if hits < 1 then
    return false, "the pill was never hit -- conditions 1 and 2 never held"
  end
  if close_ticks < 100 then
    return false, string.format("we were inside %d tiles for %d tick(s) "
                                .. "(closest %.1f)", MIN_DIST, close_ticks,
                                min_dist or -1)
  end
  return true, string.format("pill hit %d, inside %d tiles for %dt (closest "
                             .. "%.1f)", hits, MIN_DIST, close_ticks, min_dist)
end
