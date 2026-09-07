-- Scenario sidecar for tests/defend_alarm.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/defend_alarm_test.py.
--
-- ONE map, five arenas.  The arena is chosen by the DEFEND_ALARM_VARIANT
-- environment variable the test sets on the WinBoloDS process, because a
-- scenario sidecar is bound to its map by filename and there is no other seam
-- to pass a parameter through.  Unset reads as "A".
--
--   A   DAMAGE ALARM.   Our tank spawns FAR (start 1, 16 tiles W of the pill).
--                       A scripted shooter parks 6 tiles south of the pill
--                       across the moat and shells it.  Then, DESPAWN_AFTER_HIT
--                       sim ticks after the FIRST hp drop, this script REMOVES
--                       the shooter outright -- which is the point of the
--                       arena: condition 1 is a LIVE sighting, so the alarm has
--                       to go off on the tick the enemy stops being visible,
--                       not five seconds later when the damage window expires.
--                       Timed off the first hit rather than a fixed tick so the
--                       removal always lands while the shooter is alive and
--                       still shelling.
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
--   D   KEEL CONTROL.   Arena A exactly, run with cfg=DEFEND_ALARM_MODE=false.
--                       Nothing here changes; the difference is entirely in the
--                       brain's token string.
--
-- OWNERSHIP.  The map's pill and our base belong to slot 0 (the -bots tank);
-- the far-side base belongs to the foe, which is what puts the front line
-- between the two halves so pill_portfolio stops calling our pill a BACK pill
-- and the reposition pool stops bidding to shoot it down and move it.
-- on_choose_start pins each tank to its own pond so the two never swap ends.
--
-- TRACE.  Everything the test checks against the ENGINE rather than against the
-- brain's opinion of itself goes to defend_alarm_<variant>_trace.log, in SIM
-- ticks, one line per change:
--     hp <tick> <armour>            our pill's armour
--     foe <tick> <slot> <mx> <my>   the foe tank's tile
--     despawn <tick> <slot>         the tick this script removed the foe
--     wall <tick> <x> <y> <tile>    the terrain code on the wall tile
--     bot <tick> <mx> <my>          OUR tank's tile (arena C reads this to
--                                   know when we were inside MIN_DIST)

local VARIANT = os.getenv("DEFEND_ALARM_VARIANT") or "A"

local OUR_PILL = { 126, 126 }
local OUR_BASE = { 122, 122 }
local FOE_BASE = { 126, 133 }
local FOE_WALL = { 126, 122 }

local SHOOTER_BRAIN = "../tests/brains/shell_pill_then_flee.lua"
local WALLER_BRAIN  = "../tests/brains/park_and_wall.lua"

-- Sim ticks after the first hp drop at which arena A removes the shooter.
-- 200 sim ticks = 100 brain ticks = about four GOAL_REPLAN_INTERVALs, so the
-- pool has had several chances to pick defend_pill before the enemy vanishes,
-- and the shooter (6 shells, 30 think-frames apart = ~360 sim ticks of firing)
-- is still alive and still shooting when it happens.
local DESPAWN_AFTER_HIT = 200
-- Sim tick at which arena B2 flips the wall tile itself.  Late enough that the
-- enemy has parked and the pill has been on the watch list -- and so has its
-- terrain snapshot -- for a while.
local B2_SETTILE_TICK = 700
local TERRAIN_BUILDING = 0        -- constants.lua T_BUILDING

local TRACE = "defend_alarm_" .. VARIANT .. "_trace.log"

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp     = nil
local last_foe    = nil
local last_bot    = nil
local last_wall   = nil
local first_hit_t = nil
local despawned   = false
local settiled    = false

local function trace(fmt, ...)
  local f = io.open(TRACE, "a")
  if f then f:write(string.format(fmt, ...) .. "\n") f:close() end
end

-- Which pond each side spawns on: 1 = ours far, 2 = ours near, 3 = the south
-- shooter, 4 = the north builder.
local function our_start()
  if VARIANT == "C" then return 2 end
  return 1
end
local function foe_start()
  if VARIANT == "B" or VARIANT == "B2" then return 4 end
  return 3
end
local function foe_brain()
  if VARIANT == "B" or VARIANT == "B2" then return WALLER_BRAIN end
  return SHOOTER_BRAIN
end
local function foe_arg()
  if VARIANT == "B"  then return "wall" end
  if VARIANT == "B2" then return "park" end
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
  local f = io.open(TRACE, "w")
  if f then
    f:write("# defend_alarm arena " .. VARIANT .. "\n")
    f:write("# hp/foe/despawn/wall/bot rows, SIM ticks\n")
    f:close()
  end
end

-- NOT `p == foe_player`: on_choose_start is called DURING spawn_bot, before
-- spawn_bot has returned the slot for foe_player to be set from, so the foe's
-- own start pick would fall through to nil and startsScatterFind would hand it
-- whichever pond it liked -- which in arena B is the south one, on the wrong
-- side of the moat and 12 tiles from the pill.  Anybody who is not us is the
-- foe; there are only ever two tanks here.
function on_choose_start(g, p)
  if p == our_player then return our_start() end
  return foe_start()
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("Foe", foe_brain(), 1, nil, foe_arg())
    if s == nil then
      g.message("DEFEND_ALARM spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("DEFEND_ALARM " .. VARIANT .. " foe slot=" .. tostring(s))
      trace("spawn %d %d", tick, s)
      -- A fresh join can shuffle pill ownership; re-assert it.
      own_everything(g, our_player)
    end
  end

  -- Our pill's armour, on every change.
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        trace("hp %d %d", tick, p.armour)
        if first_hit_t == nil and last_hp ~= nil and p.armour < last_hp then
          first_hit_t = tick
        end
        last_hp = p.armour
      end
      break
    end
  end

  -- Both tanks' tiles, on every change.
  local ot = g.tank(our_player)
  if ot then
    local k = ot.mx * 256 + ot.my
    if last_bot ~= k then
      trace("bot %d %d %d", tick, ot.mx, ot.my)
      last_bot = k
    end
  end
  if foe_player and not despawned then
    local ft = g.tank(foe_player)
    if ft then
      local k = ft.mx * 256 + ft.my
      if last_foe ~= k then
        trace("foe %d %d %d %d", tick, foe_player, ft.mx, ft.my)
        last_foe = k
      end
    end
  end

  -- The wall tile's terrain, on every change.
  local wt = g.map_tile(FOE_WALL[1], FOE_WALL[2])
  if wt ~= last_wall then
    trace("wall %d %d %d %s", tick, FOE_WALL[1], FOE_WALL[2], tostring(wt))
    last_wall = wt
  end

  -- Arena A/C/D: pull the shooter off the board once the pill has actually
  -- been hit, so the test can watch condition 1 die on a known tick.
  if VARIANT == "A" and not despawned and foe_player
     and first_hit_t and tick >= first_hit_t + DESPAWN_AFTER_HIT then
    despawned = true
    g.remove_bot(foe_player)
    trace("despawn %d %d", tick, foe_player)
    g.message("DEFEND_ALARM A: shooter removed at tick " .. tostring(tick))
  end

  -- Arena B2: the build with nobody to blame it on.
  if VARIANT == "B2" and not settiled and tick >= B2_SETTILE_TICK then
    settiled = true
    g.set_tile(FOE_WALL[1], FOE_WALL[2], TERRAIN_BUILDING)
    trace("settile %d %d %d", tick, FOE_WALL[1], FOE_WALL[2])
    g.message("DEFEND_ALARM B2: wall placed by the script at tick "
              .. tostring(tick))
  end
end
