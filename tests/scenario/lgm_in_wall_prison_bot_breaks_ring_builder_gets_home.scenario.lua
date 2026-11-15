-- Scenario script for
-- tests/scenario/maps/lgm_in_wall_prison_bot_breaks_ring_builder_gets_home.map
-- (made by tests/generate_lgm_walled_maps.py).
--
-- WHAT IT TESTS: the bot's own builder is shut in a prison -- a full ring
-- of eight walls round the open square he stands on, so no walk home
-- exists. The bot must get him back: it drives at him, shoots a ring wall to
-- rubble and the builder walks out through the hole (a shot on a ring wall
-- does not kill a builder beside it, lgm.c lgmBlastCatches since PR #262).
-- If a shot kills him, a new builder parachutes back; that is a pass too.
-- Knob: LGM_WALLED_KILL (constants.lua) finds the ring from the bot's own
-- map and makes the rescue win at once. The old stranded rescue already
-- breaks this ring, so the arena passes with the knob off too: it guards
-- that the knob does not make the prison case worse.
--
-- THE GROUND. One open grass field, a forest strip to the west for tree
-- errands, our one pill at (121,123) four armour down (a top-up errand), our
-- one base in the far corner. Nobody else is on the map.
--
-- THE RING. The first tick the builder is out of the tank (going or coming
-- back) at least MIN_D squares from the tank, and the pill is not one of his
-- eight neighbours, all eight neighbours of his square become BUILDING. Once.
-- A wall square stops a builder dead (bolo_map.h MAP_MANSPEED_TBUILDING 0).
--
-- PASS: after the ring, the builder is back in the tank inside the round.
-- The verdict says if he walked out or was killed.
--
-- GATE: ticks=16000 bots=0 ai=yesfull gametype=open

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILL_REPOSITION_ENABLED=false"
local PILL  = { 121, 123 }
local SPAWN = { 126, 130 }
local FILL_TICK = 120
local p0 = 0
local PILL_CLEAR = 1
local TRACE_EVERY = 1000
local MIN_D = 3

local function build_walls(g, mx, my)
  for dy = -1, 1 do
    for dx = -1, 1 do
      if dx ~= 0 or dy ~= 0 then
        g.set_tile(mx + dx, my + dy, g.TERRAIN.building)
      end
    end
  end
end

local filled = false
local walled_at = nil
local wall_mx, wall_my = nil, nil
local died_at = nil
local home_at = nil
local next_trace = 0

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
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

local function try_wall(g, tk, bd)
  if bd.state ~= "going" and bd.state ~= "returning" then return end
  if not bd.wx then return end
  local bmx, bmy = bd.wx >> 8, bd.wy >> 8
  local d = math.max(math.abs(bmx - tk.mx), math.abs(bmy - tk.my))
  if d < MIN_D then return end
  if math.abs(bmx - PILL[1]) <= PILL_CLEAR
     and math.abs(bmy - PILL[2]) <= PILL_CLEAR then
    return
  end
  build_walls(g, bmx, bmy)
  walled_at = g.tick()
  wall_mx, wall_my = bmx, bmy
  g.log(string.format("LGM_WALLED walls built at (%d,%d) t=%d, tank (%d,%d)",
                      bmx, bmy, walled_at, tk.mx, tk.my))
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not filled and tick >= FILL_TICK then
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], g.TERRAIN.grass)
    end
  end
  local bd = g.builder(p0)
  if not tk or tk.dead or not bd then return end
  if filled and not walled_at then
    try_wall(g, tk, bd)
    return
  end
  if not walled_at then return end
  if (bd.state == "dead" or bd.state == "parachuting") and not died_at then
    died_at = tick
    g.log(string.format("LGM_WALLED builder died at t=%d", tick))
  end
  if bd.state == "in_tank" and not home_at then
    home_at = tick
    g.log(string.format("LGM_WALLED builder home at t=%d (died=%s)",
                        tick, tostring(died_at)))
  end
  if tick >= next_trace then
    next_trace = tick + TRACE_EVERY
    g.log(string.format(
      "LGM_WALLED trace t=%d builder=%s (%s,%s) tank (%d,%d) shells=%s",
      tick, bd.state, tostring(bd.mx), tostring(bd.my), tk.mx, tk.my,
      tostring(tk.shells)))
  end
end

VERDICT_CHECK = function(g)
  if not walled_at then
    return false, "the builder was never walled in"
  end
  if not home_at then
    return false, string.format(
      "builder walled in at (%d,%d) t=%d never got back to the tank (died=%s)",
      wall_mx, wall_my, walled_at, tostring(died_at))
  end
  return true, string.format(
    "walled t=%d, %s, builder back in the tank t=%d (%d ticks)",
    walled_at, died_at and ("killed t=" .. died_at) or "walked out",
    home_at, home_at - walled_at)
end
