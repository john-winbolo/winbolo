-- tank_deep_sea_safe, and the classic water rules it sits beside.
--
-- One tank, with a brain that holds the accelerator and never turns. The
-- arena points it east with game.teleport, always from grass, so it reaches
-- the water with no boat.
--
-- Life 1, classic rules: a four-square river strip, then grass, then deep
-- sea. Wading the river takes shells and mines (water_loss_shells and
-- water_loss_mines are 1), and driving onto the deep sea drowns the tank
-- ("deep_sea").
--
-- Life 2: the arena turns tank_deep_sea_safe on, sends the tank onto open
-- deep sea, and it lives there with no boat. After 200 frames out there the
-- arena turns the rule back off, and the tank drowns within one frame.
--
-- GATE: ticks=6000 bots=0 ai=yesfull gametype=open

scenario = {
  name        = GATE_NAME,
  description = "tank_deep_sea_safe: a tank with no boat floats on deep sea, and drowns when the rule goes off.",
  game        = GATE_GAMETYPE,
  api         = 1,
  needs_bots  = true,
}

local ME     = 0
local X0     = 100
local Y1     = 104      -- life 1: grass, river X0..X0+3, grass, deep sea
local Y2     = 110      -- life 2: grass, then open deep sea from X0
local SEA_X, SEA_Y = 80, 120
local STAY   = 200      -- frames alive on deep sea before the flip

local start_n  = nil
local life     = 0      -- which life the tank is on
local sent     = false  -- teleported for this life
local wade     = nil    -- { shells, mines } as the tank left the river
local death1   = nil    -- cause of the first death
local sea_n    = 0      -- frames alive on deep sea in life 2
local flip_at  = nil    -- tick the rule went back off
local death2   = nil    -- { tick, cause } of the second death
local done     = false

local function T(name) return game.TERRAIN[name] end

function on_setup(g)
  for n = 1, g.num_pills() do
    if g.pill(n) then g.remove_pill(n) end
  end
  for n = 1, g.num_bases() do
    if g.base(n) then g.remove_base(n) end
  end
  for x = X0 - 3, X0 + 5 do
    local t = (x >= X0 and x <= X0 + 3) and "river" or "grass"
    g.set_tile(x, Y1, T(t))
  end
  for x = X0 - 3, X0 - 1 do
    g.set_tile(x, Y2, T("grass"))
  end
  start_n = g.add_start(SEA_X, SEA_Y, 4)
  for n = g.num_starts(), 1, -1 do
    if n ~= start_n and g.start(n) then g.remove_start(n) end
  end
  g.spawn_bot{ slot = ME, name = "Swimmer", brain = "../tests/brains/hold_faster.lua",
               start = start_n }
end

function on_choose_start(g, p)
  return start_n
end

function on_tank_spawned(g, p, mx, my, respawn, scripted)
  if p ~= ME then return end
  life = life + 1
  sent = false
end

function on_tank_killed(g, victim, killer, cause, scripted)
  if victim ~= ME then return end
  if life == 1 and death1 == nil then
    death1 = cause
  elseif life == 2 and death2 == nil then
    death2 = { tick = g.tick(), cause = cause }
  end
end

local function finish()
  if wade == nil then return false, "never waded the river" end
  if wade.shells >= 40 or wade.mines >= 40 then
    return false, string.format("classic river took nothing: %d shells, %d mines",
                                wade.shells, wade.mines)
  end
  if death1 ~= "deep_sea" then return false, "first death: " .. tostring(death1) end
  if sea_n < STAY then return false, "only " .. sea_n .. " frames alive on deep sea" end
  if flip_at == nil then return false, "rule never turned back off" end
  if death2 == nil then return false, "no death after the rule went off" end
  if death2.cause ~= "deep_sea" then return false, "second death: " .. tostring(death2.cause) end
  local d = death2.tick - flip_at
  if d < 0 or d > 2 then
    return false, string.format("drowned %d ticks after the flip", d)
  end
  return true, string.format("drowned %d after flip; %d frames afloat; river left %d/%d; " ..
                             "drowned classic", d, sea_n, wade.shells, wade.mines)
end

function on_tick(g, tick)
  if done then return end
  if death2 ~= nil then
    done = true
    local ok, why = finish()
    verdict(ok, why)
    return
  end
  local tk = g.tank(ME)
  if tk == nil or tk.dead then return end

  if life == 1 then
    if not sent then
      if g.rule("tank_deep_sea_safe") ~= 0 or g.rule("water_loss_shells") ~= 1
         or g.rule("water_loss_mines") ~= 1 then
        verdict_fail("the round did not open on the classic water rules")
        done = true
        return
      end
      if g.teleport(ME, X0 - 2, Y1, 64) then
        g.set_stocks(ME, { shells = 40, mines = 40 })
        sent = true
      end
      return
    end
    if wade == nil and tk.my == Y1 and tk.mx >= X0 + 4 then
      wade = { shells = tk.shells, mines = tk.mines }
    end
  elseif life == 2 then
    if not sent then
      g.set_rule("tank_deep_sea_safe", 1)
      if g.teleport(ME, X0 - 2, Y2, 64) then sent = true end
      return
    end
    if flip_at ~= nil then return end
    if tk.my == Y2 and tk.mx >= X0 and g.map_tile(tk.mx, tk.my) == T("deep_sea") then
      if tk.boat then
        verdict_fail("the tank picked up a boat on deep sea")
        done = true
        return
      end
      sea_n = sea_n + 1
      if sea_n >= STAY then
        g.set_rule("tank_deep_sea_safe", 0)
        flip_at = tick
      end
    end
  end
end

VERDICT_CHECK = function(g)
  return finish()
end
