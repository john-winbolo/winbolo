-- Scenario script for
-- tests/scenario/maps/lgm_on_pill_bot_shoots_pill_and_kills_builder.map
-- (made by tests/generate_lgm_walled_maps.py).
--
-- WHAT IT TESTS: a live enemy pillbox is put down ON the square of the
-- bot's own builder while he walks (in a recorded game a human player did
-- this). A live pill square has builder speed 0 (bolo_map.c mapGetManSpeed,
-- MAP_MANSPEED_TPILLBOX), so he never moves again, and the tank cannot
-- drive onto a live pill square to pick him up. The only way out is a
-- shot on his square: a shell that hits the pill kills a builder on that
-- square (lgm.c lgmBlastCatches), and a new builder parachutes back to the
-- tank. Knob: LGM_WALLED_KILL (constants.lua), kind "on_pill". With the
-- knob off the bot drives at him and never gets him back.
--
-- THE GROUND. The same field as the other walled-builder arenas: open
-- grass, a forest strip to the west for tree errands, our one pill at
-- (126,122) four armour down (a top-up errand), our one base in the far
-- corner. Nobody else is on the map.
--
-- THE PILL. The first tick the builder is out of the tank (going or coming
-- back) at least MIN_D squares from the tank, and our pill is not one of
-- his eight neighbours, a new pill owned by seat ENEMY (nobody plays it, so
-- it is hostile to the bot) with full armour is put on his square. Once.
--
-- PASS: after the pill, the builder is back in the tank inside the round.
-- The verdict says if he was killed (the only way out in this arena).
--
-- GATE: ticks=16000 bots=0 ai=yesfull gametype=open

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILL_REPOSITION_ENABLED=false"
local PILL  = { 126, 122 }
local SPAWN = { 126, 130 }
local FILL_TICK = 120
local p0 = 0
local ENEMY = 1
local ENEMY_ARMOUR = 15
local PILL_CLEAR = 1
local TRACE_EVERY = 1000
local MIN_D = 2

local filled = false
local placed_at = nil
local pill_mx, pill_my = nil, nil
local pill_n = nil
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

local function try_pill(g, tk, bd)
  if bd.state ~= "going" and bd.state ~= "returning" then return end
  if not bd.wx then return end
  local bmx, bmy = bd.wx >> 8, bd.wy >> 8
  local d = math.max(math.abs(bmx - tk.mx), math.abs(bmy - tk.my))
  if d < MIN_D then return end
  if math.abs(bmx - PILL[1]) <= PILL_CLEAR
     and math.abs(bmy - PILL[2]) <= PILL_CLEAR then
    return
  end
  local n, code = g.add_pill(bmx, bmy, ENEMY, ENEMY_ARMOUR)
  if not n then
    g.log(string.format("LGM_ON_PILL add_pill at (%d,%d) refused: %s",
                        bmx, bmy, tostring(code)))
    return
  end
  pill_n = n
  placed_at = g.tick()
  pill_mx, pill_my = bmx, bmy
  g.log(string.format("LGM_ON_PILL pill %s put on the builder at (%d,%d) t=%d, tank (%d,%d)",
                      tostring(n), bmx, bmy, placed_at, tk.mx, tk.my))
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
  if not tk or not bd then return end
  if filled and not placed_at then
    if tk.dead then return end
    try_pill(g, tk, bd)
    return
  end
  if not placed_at then return end
  if (bd.state == "dead" or bd.state == "parachuting") and not died_at then
    died_at = tick
    g.log(string.format("LGM_ON_PILL builder died at t=%d", tick))
  end
  if bd.state == "in_tank" and not home_at then
    home_at = tick
    g.log(string.format("LGM_ON_PILL builder home at t=%d (died=%s)",
                        tick, tostring(died_at)))
  end
  if tick >= next_trace then
    next_trace = tick + TRACE_EVERY
    local p = pill_n and g.pill(pill_n)
    g.log(string.format(
      "LGM_ON_PILL trace t=%d builder=%s (%s,%s) tank (%d,%d) dead=%s shells=%s armour=%s pill_armour=%s",
      tick, bd.state, tostring(bd.mx), tostring(bd.my), tk.mx, tk.my,
      tostring(tk.dead), tostring(tk.shells), tostring(tk.armour),
      tostring(p and p.armour)))
  end
end

VERDICT_CHECK = function(g)
  if not placed_at then
    return false, "the pill was never put on the builder"
  end
  if not home_at then
    return false, string.format(
      "pill put on the builder at (%d,%d) t=%d, he never got back to the tank (died=%s)",
      pill_mx, pill_my, placed_at, tostring(died_at))
  end
  return true, string.format(
    "pill t=%d, %s, builder back in the tank t=%d (%d ticks)",
    placed_at, died_at and ("killed t=" .. died_at) or "walked out",
    home_at, home_at - placed_at)
end
