local bit = require('bitcompat')
-- =========================================================================
-- cworldsim.lua -- C-accelerated forward world state simulator
--
-- Wraps the C world simulator engine for use by Lua brains. Each brain
-- instance gets its own isolated simulator with its own state.
--
-- QUICK START:
--   local wsim = require("cworldsim")
--   wsim.configure()                     -- load terrain speeds (once)
--   wsim.snapshot(world, info, path)     -- populate sim from current state
--   local r = wsim.run(300)              -- simulate up to 300 ticks
--   print(r.damage, r.killed)            -- check predicted outcome
--
-- The simulator predicts:
--   - Armor damage from hostile/neutral pills along a path
--   - Pill anger escalation and crossfire between adjacent pills
--   - Shell knockback pushing tank into slow terrain
--   - LGM survival when dispatched mid-path
-- =========================================================================

local C = require("constants")

local M = {}

-- Default terrain speeds (matching cpathfinder.lua / constants.lua)
local DEFAULT_TERRAIN_SPEED = {
  [C.T_BUILDING]  = 0,
  [C.T_RIVER]     = 3,
  [C.T_SWAMP]     = 3,
  [C.T_CRATER]    = 3,
  [C.T_ROAD]      = 16,
  [C.T_FOREST]    = 6,
  [C.T_RUBBLE]    = 3,
  [C.T_GRASS]     = 12,
  [C.T_HALFBUILD] = 0,
  [C.T_BOAT]      = 16,
  [C.T_DEEPSEA]   = 3,
  [C.T_REFBASE]   = 16,
  [C.T_PILLBOX]   = 16,
}

-- The table configure() sends; live_physics.lua scales it for live rules.
M.DEFAULTS = { terrain_speed = DEFAULT_TERRAIN_SPEED }

--- Configure terrain speeds (call once at brain open).
--- @param opts table|nil  Optional overrides: opts.terrain_speed = { [type] = speed }
function M.configure(opts)
  opts = opts or {}
  local ts = opts.terrain_speed or {}
  for type, speed in pairs(DEFAULT_TERRAIN_SPEED) do
    wsim_set_terrain_speed(type, ts[type] or speed)
  end
  for type, speed in pairs(ts) do
    if DEFAULT_TERRAIN_SPEED[type] == nil then
      wsim_set_terrain_speed(type, speed)
    end
  end
end

--- Populate the simulator from the current world state.
--- Call this before wsim.run().
---
--- @param world     table    World state (world.pills)
--- @param info      table    BrainInfo from think()
--- @param path      table    Array of {x=mx, y=my} waypoints
--- @param attack_pill_idx number|nil  Pill index we're shooting at, or nil
function M.snapshot(world, info, path, attack_pill_idx)
  wsim_clear()

  -- Add hostile/neutral pills with current anger
  local pill_sim_idx = 0
  for id, pm in pairs(world.pills) do
    if (pm.owner == "hostile" or pm.owner == "neutral") and pm.health > 0 then
      local owner = (pm.owner == "neutral") and 0xFF or 0xFE
      local anger = pm.anger or 0
      wsim_add_pill(pm.mx, pm.my, pm.health, anger, owner, id)
      -- Track mapping: if this is the attack target, record the sim index
      if attack_pill_idx and id == attack_pill_idx then
        wsim_set_attack_target(pill_sim_idx)
      end
      pill_sim_idx = pill_sim_idx + 1
    end
  end

  -- Add our tank
  wsim_add_tank(info.tankx, info.tanky, info.direction, info.speed,
                true, info.player_number, info.armour)

  -- Add visible enemy tanks (hostile only)
  if info.objects then
    for _, obj in ipairs(info.objects) do
      if obj.type == OBJECT_TANK and (bit.band(obj.info, OBJECT_HOSTILE)) ~= 0 then
        -- Use default 40 armor for unknown enemy tanks
        wsim_add_tank(obj.x, obj.y, obj.direction or 0, obj.speed or 0,
                      false, 0xFE, 40)
      end
    end
  end

  -- Set path
  if path and #path > 0 then
    wsim_set_path(path)
  end
end

--- Run the simulation.
--- @param max_ticks number|nil  Maximum ticks to simulate (default 300)
--- @return table  Result with fields: armour, damage, dwell_damage, ticks,
---                arrival, killed, truncated, lgm_survived, lgm_arrival,
---                lgm_death, pills
---                `truncated` = the sim ran out of ticks instead of reaching
---                a natural end. That means UNKNOWN, not safe: a low
---                `damage` on a truncated run proves nothing.
function M.run(max_ticks)
  return wsim_run(max_ticks or 300)
end

-- Thin pass-through wrappers

function M.clear()
  wsim_clear()
end

function M.add_pill(mx, my, health, anger, owner, pill_id)
  wsim_add_pill(mx, my, health, anger, owner, pill_id)
end

function M.add_tank(wx, wy, dir, speed, is_ours, owner, armour)
  wsim_add_tank(wx, wy, dir, speed, is_ours, owner, armour)
end

function M.set_path(path)
  wsim_set_path(path)
end

function M.set_attack_target(pill_index)
  wsim_set_attack_target(pill_index)
end

function M.set_lgm(dispatch_tick, dest_mx, dest_my, speed)
  wsim_set_lgm(dispatch_tick, dest_mx, dest_my, speed or 4)
end

--- Keep simulating for `ticks` after the tank reaches the end of its path,
--- with the tank standing still on the destination tile and taking FULL
--- shell damage (no moving-target discount). Answers "can I survive the
--- drive AND the job I drove there to do?".
---
--- Must be called AFTER M.snapshot() -- snapshot clears the sim, which
--- resets the dwell back to 0. 0 = stop on arrival (the default).
--- @param ticks number|nil  Ticks to stand at the destination
function M.set_dwell(ticks)
  wsim_set_dwell(ticks or 0)
end

function M.set_terrain_speed(type, speed)
  wsim_set_terrain_speed(type, speed)
end

return M
