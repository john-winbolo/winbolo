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

-- The knob fixes last sent with wsim_set_fixes. A clear keeps them in the
-- C sim, so they are sent again only when a knob value changes.
local pushed_fixes = nil

--- Send the world sim knob fixes (WSIM_DRIVE_WHILE_SLIDING,
--- WSIM_HIT_FULL_DIST, WSIM_HIT_FAR_FRAC) when they change. An older exe
--- without wsim_set_fixes keeps the old model.
function M.push_fixes()
  local set = rawget(_G, "wsim_set_fixes")
  if type(set) ~= "function" then return end
  local d = C.WSIM_DRIVE_WHILE_SLIDING and true or false
  local f = C.WSIM_HIT_FULL_DIST or 0
  local r = C.WSIM_HIT_FAR_FRAC or 0
  local p = pushed_fixes
  if p and p[1] == d and p[2] == f and p[3] == r then return end
  set({ drive_while_sliding = d, hit_full_dist = f, hit_far_frac = r })
  pushed_fixes = { d, f, r }
end

--- Forget what was sent (tests; a new sim).
function M.reset_fixes()
  pushed_fixes = nil
end

--- WSIM_CAPTURE_EXIT_MAX: the capture sim path. The incoming path ends ON
--- the pill square (the pickup is instant): a path that ends next to it
--- gets the pill square added. Then the exit leg: the incoming squares
--- again in reverse, at most max_exit, back to the path start (the tank
--- square too when it is next to the first waypoint). No square is made up.
--- The C sim stops the exit leg when the tank is out of range of every pill
--- that fired at it (wsim_set_exit_start).
---
--- @param path     table   flat {x1,y1,x2,y2,...}, after the tank square
--- @param smx      number  tank square
--- @param smy      number
--- @param pmx      number  pill square
--- @param pmy      number
--- @param max_exit number  most exit squares
--- @param cap      number|nil most waypoints in the result (default 250)
--- @return table the new flat path, number incoming waypoints (the exit
---   start for set_exit_start), number exit squares (0 = no exit leg)
function M.capture_exit_path(path, smx, smy, pmx, pmy, max_exit, cap)
  cap = cap or 250
  local out = {}
  local n = 0
  local np = math.floor(#(path or {}) / 2)
  for i = 1, np do
    if n >= cap then break end
    out[2 * n + 1] = path[2 * i - 1]
    out[2 * n + 2] = path[2 * i]
    n = n + 1
  end
  local function next_to(ax, ay, bx, by)
    return math.abs(ax - bx) <= 1 and math.abs(ay - by) <= 1
  end
  if n == 0 then return out, 0, 0 end
  if out[2 * n - 1] ~= pmx or out[2 * n] ~= pmy then
    -- Onto the pill only from the square next to it, and only with room.
    if n >= cap or not next_to(out[2 * n - 1], out[2 * n], pmx, pmy) then
      return out, n, 0
    end
    out[2 * n + 1] = pmx
    out[2 * n + 2] = pmy
    n = n + 1
  end
  local incoming = n
  local added = 0
  for i = incoming - 1, 1, -1 do
    if added >= max_exit or n >= cap then break end
    out[2 * n + 1] = out[2 * i - 1]
    out[2 * n + 2] = out[2 * i]
    n = n + 1
    added = added + 1
  end
  if added < max_exit and n < cap and smx and smy
     and not (out[1] == smx and out[2] == smy)
     and not (smx == pmx and smy == pmy)
     and next_to(out[1], out[2], smx, smy) then
    out[2 * n + 1] = smx
    out[2 * n + 2] = smy
    n = n + 1
    added = added + 1
  end
  return out, incoming, added
end

--- The goal pool damage cost with an exit leg:
--- (damage - exit_damage) x weight + exit_damage x weight x exit_weight.
--- exit_damage 0 gives damage x weight, the old cost.
function M.damage_cost(damage, exit_damage, weight, exit_weight)
  exit_damage = exit_damage or 0
  if exit_damage == 0 then return damage * weight end
  return (damage - exit_damage) * weight + exit_damage * weight * (exit_weight or 1)
end

--- Mark where the exit leg starts: waypoints 1..n are the incoming path.
--- Call it after snapshot (a snapshot clears it). An older exe without
--- wsim_set_exit_start runs the whole path as one drive.
function M.set_exit_start(n)
  local set = rawget(_G, "wsim_set_exit_start")
  if type(set) == "function" then set(n or 0) end
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
  M.push_fixes()

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
