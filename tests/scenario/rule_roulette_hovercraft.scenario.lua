-- Rule Roulette's Hovercraft mode, then Normal.
--
-- The runner writes data/mods/RuleRoulette.scenario.lua in front of this
-- text (the include on the GATE line), so the mod's locals -- TANK, with
-- its modes, queue, mode, shown and next_at -- are in scope. The tank
-- track's queue is given Hovercraft and then Normal before the mod's
-- on_start takes the first mode from it. The builder track runs beside it
-- on the lobby default and touches none of these rules.
--
-- One tank, with a brain that holds the accelerator and never turns. The
-- arena points it east down one test strip after another with
-- game.teleport, always from grass, so it reaches each strip with no boat.
-- Each strip is a row of one terrain with grass before and after it; the
-- deep sea row has open sea after it, which is the rest of the map.
--
-- What is checked:
--   1. Every frame under Hovercraft: every speed rule but forest and grass
--      holds the grass speed, forest keeps its own, water_loss_shells and
--      water_loss_mines are 0, tank_deep_sea_safe is 1, and the panel line
--      is the mode's. Under Normal every one of them is back.
--   2. The tank's top speed, measured from its position each frame in the
--      middle of each strip, is the same on road, river, deep sea, swamp,
--      crater and rubble as on grass (within 3%).
--   3. Driving down the river strip, where a tank with no boat wades the
--      whole way, takes no shells and no mines.
--   4. The tank lives on deep sea with no boat. A few seconds before the
--      mode ends it is sent out on deep sea again, and it drowns ("deep_sea")
--      within one frame of the rule going back off.
--
-- GATE: ticks=9000 bots=0 ai=yesfull gametype=open include=data/mods/RuleRoulette.scenario.lua

local roulette = scenario
local mod_start, mod_end, mod_spawned = on_start, on_end, on_tank_spawned
on_chat = nil   -- a human's !roulette; the tail must not wrap it

local arena_settings = {}
for i, st in ipairs(roulette.settings) do
  local c = {}
  for k, v in pairs(st) do c[k] = v end
  if c.id == "interval" then c.default = 60 end
  if c.id == "countdown" then c.default = 0 end
  arena_settings[i] = c
end

scenario = {
  name        = GATE_NAME,
  description = "Rule Roulette's Hovercraft mode: grass speed everywhere, dry water, safe deep sea.",
  game        = GATE_GAMETYPE,
  api         = 1,
  needs_bots  = true,
  rules = {
    base_regen_ticks = roulette.rules.base_regen_ticks,
  },
  settings = arena_settings,
}

local ORDER = { "Hovercraft", "Normal" }

local SPEEDS = { "speed_road", "speed_river", "speed_swamp", "speed_crater",
                 "speed_rubble", "speed_boat", "speed_deep_sea",
                 "speed_refuel_base" }
-- The classic table, which this round opens on.
local CLASSIC = {
  speed_road = 16, speed_grass = 12, speed_forest = 6, speed_river = 3,
  speed_swamp = 3, speed_crater = 3, speed_rubble = 3, speed_boat = 16,
  speed_deep_sea = 3, speed_refuel_base = 16,
  water_loss_shells = 1, water_loss_mines = 1, tank_deep_sea_safe = 0,
}
local WATCH = { "speed_road", "speed_grass", "speed_forest", "speed_river",
  "speed_swamp", "speed_crater", "speed_rubble", "speed_boat",
  "speed_deep_sea", "speed_refuel_base", "water_loss_shells",
  "water_loss_mines", "tank_deep_sea_safe" }
local HOVER = { water_loss_shells = 0, water_loss_mines = 0,
                tank_deep_sea_safe = 1 }
for _, r in ipairs(SPEEDS) do HOVER[r] = CLASSIC.speed_grass end

-- The strips: each a row from X0 to X0 + LEN - 1, with grass from X0 - 3
-- and grass after it (open deep sea after the deep sea row). The tank is
-- measured on squares X0 + 6 to X0 + 15, past its run-up.
local ME    = 0
local X0    = 90
local LEN   = 18
local STRIPS = {
  { name = "grass",    y = 100 },
  { name = "road",     y = 103 },
  { name = "river",    y = 106 },
  { name = "deep_sea", y = 109 },
  { name = "swamp",    y = 112 },
  { name = "crater",   y = 115 },
  { name = "rubble",   y = 118 },
}
local SEA_X, SEA_Y = 80, 125   -- the only start, out on the sea

local bad      = nil     -- the first wrong rule value seen, as text
local strip_i  = 0       -- the strip being driven, 0 before the first
local sent     = false   -- the tank has been sent down strip_i
local rate     = {}      -- strip name -> { d = world units, t = ticks }
local last     = nil     -- { wx, tick } the frame before
local start_n  = nil     -- our start
local river_ok = nil     -- true, or why not
local deep_n   = 0       -- frames alive on deep sea with no boat
local final    = false   -- sent out on deep sea for the end of the mode
local safe_was = nil     -- tank_deep_sea_safe last frame
local flip_at  = nil     -- tick it was first seen back at 0
local died_at  = nil     -- tick of the deep sea death
local died_how = nil
local normal_ok = false

local function T(name) return game.TERRAIN[name] end

function on_setup(g)
  for n = 1, g.num_pills() do
    if g.pill(n) then g.remove_pill(n) end
  end
  for n = 1, g.num_bases() do
    if g.base(n) then g.remove_base(n) end
  end
  for _, s in ipairs(STRIPS) do
    for x = X0 - 3, X0 + LEN + 2 do
      local t = "grass"
      if x >= X0 and x < X0 + LEN then
        t = s.name
      elseif x >= X0 + LEN and s.name == "deep_sea" then
        t = "deep_sea"
      end
      g.set_tile(x, s.y, T(t))
    end
  end
  start_n = g.add_start(SEA_X, SEA_Y, 4)
  for n = g.num_starts(), 1, -1 do
    if n ~= start_n and g.start(n) then g.remove_start(n) end
  end
  g.spawn_bot{ slot = ME, name = "Hover", brain = "../tests/brains/hold_faster.lua",
               start = start_n }
end

function on_choose_start(g, p)
  return start_n
end

function on_start(g)
  for _, want in ipairs(ORDER) do
    for i, m in ipairs(TANK.modes) do
      if m.name == want then TANK.queue[#TANK.queue + 1] = i end
    end
  end
  mod_start()
end

function on_end(g)
  mod_end()
end

function on_tank_spawned(g, p, ...)
  return mod_spawned(p, ...)
end

function on_tank_killed(g, victim, killer, cause, scripted)
  if victim == ME and died_at == nil then
    died_at, died_how = g.tick(), cause
  end
end

local function check_rules(g)
  local mode, shown = TANK.mode, TANK.shown
  local name = mode and mode.name or "-"
  local want = name == "Hovercraft" and HOVER or {}
  for _, r in ipairs(WATCH) do
    local v = g.rule(r)
    local w = want[r] or CLASSIC[r]
    if v ~= w then
      return string.format("%s: %s is %s, want %s", name, r, tostring(v), tostring(w))
    end
  end
  if name == "Hovercraft" and shown ~= "Grass speed; deep sea safe" then
    return "Hovercraft: panel line '" .. tostring(shown) .. "'"
  end
  if name == "Hovercraft" and mode.kind ~= "trade" then
    return "Hovercraft: kind " .. tostring(mode.kind)
  end
  if name == "Normal" then normal_ok = true end
  return nil
end

-- Points the tank down a strip from the grass before it, facing east.
local function send(g, y)
  local ok, code = g.teleport(ME, X0 - 2, y, 64)
  if ok then
    g.set_stocks(ME, { shells = 40, mines = 40 })
    last = nil
  end
  return ok
end

local function finish()
  if bad ~= nil then return false, bad end
  local g0 = rate.grass
  if g0 == nil or g0.t == 0 then return false, "no grass speed measured" end
  local gr = g0.d / g0.t
  local out = {}
  for _, s in ipairs(STRIPS) do
    local e = rate[s.name]
    if e == nil or e.t == 0 then return false, "no speed measured on " .. s.name end
    local r = (e.d / e.t) / gr
    if r < 0.97 or r > 1.03 then
      return false, string.format("%s at x%.2f of grass (%.2f wu/tick)", s.name, r, e.d / e.t)
    end
    out[#out + 1] = string.format("%.2f", r)
  end
  if river_ok ~= true then return false, tostring(river_ok or "river never driven") end
  if deep_n < 50 then return false, "only " .. deep_n .. " frames alive on deep sea" end
  if flip_at == nil then return false, "tank_deep_sea_safe never went back off" end
  if died_at == nil then return false, "no death after the flip" end
  if died_how ~= "deep_sea" then return false, "died of " .. tostring(died_how) end
  if died_at < flip_at - 2 or died_at > flip_at + 2 then
    return false, string.format("drowned at %d, flip seen at %d", died_at, flip_at)
  end
  if not normal_ok then return false, "Normal never checked" end
  return true, string.format("drowned %+d after flip; river dry; %d frames on sea; " ..
                             "speed/grass %s", died_at - flip_at, deep_n,
                             table.concat(out, "/"))
end

function on_tick(g, tick)
  if bad == nil then
    bad = check_rules(g)
    if bad ~= nil then verdict_fail(bad); return end
  end

  local safe = g.rule("tank_deep_sea_safe")
  if safe_was == 1 and safe == 0 and flip_at == nil then flip_at = tick end
  safe_was = safe

  if died_at ~= nil then
    -- Wait a frame under Normal so its rules are checked once, then answer.
    if normal_ok then
      local ok, why = finish()
      verdict(ok, why)
    end
    return
  end
  if TANK.mode == nil or TANK.mode.name ~= "Hovercraft" then return end

  local tk = g.tank(ME)
  if tk == nil or tk.dead then return end

  -- The end of the mode: out on deep sea when the rule goes back off.
  if not final and strip_i > #STRIPS and TANK.next_at - tick <= 300 then
    if send(g, STRIPS[4].y) then final = true end
    return
  end
  if final then return end

  if strip_i == 0 or (strip_i <= #STRIPS and sent and tk.mx >= X0 + 16) then
    -- On to the next strip; the river's stocks are read as it is left.
    if strip_i > 0 and STRIPS[strip_i].name == "river" then
      if tk.shells == 40 and tk.mines == 40 then
        river_ok = true
      else
        river_ok = string.format("river took stocks: %d shells, %d mines", tk.shells, tk.mines)
      end
    end
    strip_i = strip_i + 1
    sent = false
  end
  if strip_i > #STRIPS then return end
  local s = STRIPS[strip_i]
  if not sent then
    sent = send(g, s.y)
    return
  end

  if tk.my ~= s.y then
    verdict_fail(string.format("left the %s strip at %d,%d", s.name, tk.mx, tk.my))
    return
  end
  if s.name == "deep_sea" and tk.mx >= X0 and not tk.boat then
    deep_n = deep_n + 1
  end
  if g.map_tile(tk.mx, tk.my) == T(s.name) and tk.mx >= X0 + 6 and tk.mx <= X0 + 15
     and last ~= nil and tick > last.tick then
    local e = rate[s.name] or { d = 0, t = 0 }
    e.d = e.d + (tk.wx - last.wx)
    e.t = e.t + (tick - last.tick)
    rate[s.name] = e
  end
  last = { wx = tk.wx, tick = tick }
end

VERDICT_CHECK = function(g)
  return finish()
end
