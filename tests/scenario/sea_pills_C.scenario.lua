-- Scenario script for tests/sea_pills_C.map (auto-loaded as <map>.scenario.lua).
--
-- Three jobs.
--
-- 1. The map loader replaces RIVER/DEEP_SEA/BUILDING/HALFBUILDING under every
--    map-file pillbox with ROAD (bolo_map.c), so the dead pills this arena puts
--    in the sea would really sit on one-tile road pedestals -- a boat driving
--    onto one BEACHES and the tank then drowns stepping off.  Restore the deep
--    sea under all of them in on_setup, which runs before the first snapshot.
--    Same trick as tests/water_pills.scenario.lua and blocked_aim.scenario.lua.
--
-- 2. LOADOUT.  The old arena ran with -bots 0 and spawned its own tank so that
--    game.spawn_bot's mode argument could hand it the STRICT loadout, which was
--    the only route to a -bots tank's stocks.  This host has game.set_stocks, so
--    the -bots tank is emptied on its own first tick instead: the same 0/0/0
--    start, and the round is never empty -- the gate runner passes no
--    -noemptyreset and a bot the scenario spawns arrives a tick late.
--
-- 3. Watch the shore band for the two things a harvest leaves in the ground: a
--    mined tile, and a boat.  The old arena wrote every terrain change out to
--    sea_pills_terrain_C.log for the python driver to read; this sandbox has
--    no io, so the arena reads the band itself and says what it saw.

local T_DEEP_SEA = 0xFF
local OUR_PLAYER = 0
local DEAD_PILLS = { { 138, 124 }, { 139, 126 }, { 138, 128 } }
local HOSTILE_PILLS = { { 147, 126 } }
local OUR_BASE = { 132, 130 }
local STRICT = false          -- start the tank on 0 shells / 0 mines / 0 trees

local watched = {}
local sea = {}                 -- { index, x, y } for each dead sea pill
local stocked = false
local mine_seen = false
local boat_seen = false
local mine_first = false       -- a mine appeared before any boat did
local closest2 = 1e9           -- closest the boat ever came to a hostile pill
local home = nil               -- the tile the tank started on
local moved = false            -- ...and whether it ever left it

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      local hostile = false
      for _, h in ipairs(HOSTILE_PILLS) do
        if p.x == h[1] and p.y == h[2] then hostile = true end
      end
      if hostile then g.set_pill_owner(i, nil)
      else g.set_pill_owner(i, owner) end
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
    end
  end
end

-- The sea pills by SLOT, resolved once: a pill that is picked up moves with
-- its carrier, so a coordinate match would stop finding it at exactly the
-- moment the verdict wants to know whether it was fetched.
local function resolve(g)
  for _, d in ipairs(DEAD_PILLS) do
    for i = 1, g.num_pills() do
      local p = g.pill(i)
      if p and p.x == d[1] and p.y == d[2] then
        sea[#sea + 1] = { n = i, x = d[1], y = d[2] }
      end
    end
  end
end

-- What the old driver called collected: the pill is no longer lying dead on
-- its own square.
local function collected(g, e)
  local p = g.pill(e.n)
  if p == nil or p.in_tank then return true end
  if p.x ~= e.x or p.y ~= e.y then return true end
  return (p.armour or 0) > 0
end

function on_setup(g)
  for _, p in ipairs(DEAD_PILLS) do
    g.set_tile(p[1], p[2], T_DEEP_SEA)
  end
  -- A live hostile pill standing in deep sea can never be reached, repaired or
  -- driven over, so it stays exactly where the test needs it for the whole run.
  for _, p in ipairs(HOSTILE_PILLS) do
    g.set_tile(p[1], p[2], T_DEEP_SEA)
  end
  own_everything(g, OUR_PLAYER)
  resolve(g)
  -- Watch the shore band (the entrance is always in it) plus the first sea
  -- column, so a boat appearing anywhere along it is recorded.
  for y = 118, 134 do
    for x = 134, 137 do
      watched[#watched + 1] = { x, y }
    end
  end
end

function on_tick(g, tick)
  if STRICT and not stocked and tick >= 2 then
    local tk = g.tank(OUR_PLAYER)
    if tk and not tk.dead then
      stocked = true
      g.set_stocks(OUR_PLAYER, { shells = 0, mines = 0, trees = 0 })
    end
  end
  for _, w in ipairs(watched) do
    local t = g.map_tile(w[1], w[2])
    if t >= 10 and t <= 15 then                  -- base terrain + MINE_SUBTRACT
      if not boat_seen then mine_first = true end
      mine_seen = true
    elseif t == 9 then
      boat_seen = true
    end
  end
  local me = g.tank(OUR_PLAYER)
  if me and not me.dead then
    -- A verdict that is the ABSENCE of something has to know the bot played at
    -- all: a brain that died at tick 0 leaves the same empty shore behind.
    if home == nil then home = { me.mx, me.my }
    elseif me.mx ~= home[1] or me.my ~= home[2] then moved = true end
  end
  if #HOSTILE_PILLS > 0 then
    local tk = g.tank(OUR_PLAYER)
    if tk and tk.boat and not tk.dead then
      local dx = tk.mx - HOSTILE_PILLS[1][1]
      local dy = tk.my - HOSTILE_PILLS[1][2]
      local d2 = dx * dx + dy * dy
      if d2 < closest2 then closest2 = d2 end
    end
  end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/sea_pills_test.py variant C.  What moved:
-- the three dead pills all come out of the water, a mine goes down in
-- the shore band and a boat appears there -- C is A's geometry with the
-- hostile pill put behind a breakwater, so it must execute like A.
--
-- What did NOT move:
-- the SEA_PLAN entrance type and its S tile, the substate chain and the
-- per-cluster budget: all of them are read off brain debug lines.
--
-- GATE: ticks=12000 bots=1 gametype=open ai=yesfull limit=20

VERDICT_CHECK = function(g)
  local got = 0
  for _, e in ipairs(sea) do
    if collected(g, e) then got = got + 1 end
  end
  if got < #sea then
    return false, string.format("only %d of %d sea pills collected", got, #sea)
  end
  if not mine_seen then return false, "all pills taken but no mine was ever laid" end
  if not boat_seen then return false, "all pills taken but no boat ever appeared" end
  return true, string.format("%d pills taken, mine laid, boat built", got)
end
