-- Scenario sidecar for tests/sea_pills_F.map (auto-loaded as <map>.scenario.lua).
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
-- 2. LOADOUT.  A map with a sidecar boots as gameScripted, and gameTypeGetItems
--    treats gameScripted exactly like OPEN (40 shells / 40 mines / 40 trees) --
--    neither -gametype nor scenario.game reaches a -bots tank.  game.spawn_bot's
--    mode argument is the one thing that does (it arms sim->spawnLoadout for the
--    slot BEFORE tankCreate runs, scenario.c:470), so the variants that need an
--    empty tank run with -bots 0 and spawn their own bot here.  The map's pills
--    and our base are then re-owned to whatever slot it landed in.
--
-- 3. Trace the terrain of the shore band every tick to
--    sea_pills_terrain_F.log so the test can assert the sequence the harvest
--    actually produces: GRASS(7) -> GRASS+MINE(15) -> CRATER(3) -> RIVER(1)
--    -> BOAT(9).  Values are the ENGINE's (global.h): a mined tile is its base
--    terrain + MINE_SUBTRACT(8).

local T_DEEP_SEA = 0xFF
local OUR_PLAYER = 0
local DEAD_PILLS = { { 138, 124 }, { 139, 126 }, { 138, 128 } }
local HOSTILE_PILLS = { }
local OUR_BASE = { 132, 130 }
local SPAWN_MODE = "strict"     -- nil = use the -bots tank as it spawned
local TRACE = "sea_pills_terrain_F.log"

local watched = {}
local last = {}
local spawn_tried = false

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
  -- Watch the shore band (the entrance is always in it) plus the first sea
  -- column, so a boat appearing anywhere along it is recorded.
  for y = 118, 134 do
    for x = 134, 137 do
      watched[#watched + 1] = { x, y }
    end
  end
  local f = io.open(TRACE, "w")
  if f then f:write("# tick x y old new\n") f:close() end
end

function on_tick(g, tick)
  if SPAWN_MODE and not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("SeaHarvest", nil, 0, SPAWN_MODE)
    if s == nil then
      g.message("SEA_PILLS_TEST spawn_bot failed: " .. tostring(err))
    else
      g.message("SEA_PILLS_TEST spawned slot=" .. tostring(s)
                .. " mode=" .. SPAWN_MODE)
      own_everything(g, s)
    end
  end
  local out = nil
  for _, w in ipairs(watched) do
    local k = w[2] * 256 + w[1]
    local t = g.map_tile(w[1], w[2])
    if last[k] ~= t then
      if last[k] ~= nil then
        out = out or {}
        out[#out + 1] = string.format("%d %d %d %d %d", tick, w[1], w[2], last[k], t)
      end
      last[k] = t
    end
  end
  if out then
    local f = io.open(TRACE, "a")
    if f then
      f:write(table.concat(out, "\n") .. "\n")
      f:close()
    end
  end
end
