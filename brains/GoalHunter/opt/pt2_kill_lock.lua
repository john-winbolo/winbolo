-- =========================================================================
-- pt2_kill_lock.lua -- capture lock while our kill shell flies
-- (C.PT2_FINISH_CAPTURE_LOCK, Andrew, Nov 17)
--
-- Pill take v2's finish path (goal._pt2.finish: a 1-health pill, still
-- angry, charged with no cover) ends its swerve when the turn ticks run out.
-- If the pill still lives then but our kill shell is still in the air, the
-- old rule swerved in place until the shell landed (attack.lua, the swerve
-- "extend" branch). Here the tank uses those ticks instead: it drives to a
-- DEAD pill that is free to capture, as long as the armour it is expected to
-- lose before the shell lands leaves it at or above the floor.
--
--   W      = ticks until the kill shell lands (its landing tick - now)
--   shots  = floor(W / reload ticks of that gun)
--            pill:  the engine's halving ladder from the live rules,
--                   pill_attack_ticks halved once per landed hit (hits from
--                   the anger proxy, attack.heat_hits_landed), floored at
--                   pill_attack_min_ticks. Classic: 100 50 25 12 6.
--            tank:  our own reload (live_physics context).
--   dmg    = shots x the armour one shell takes off us (live_physics)
--   guns   = the target pill (it fires until the shell lands), every other
--            hostile/neutral live pill whose range reaches the part of the
--            drive the tank covers in W, every visible enemy tank within
--            TANK_COMBAT_ENGAGE_RANGE of that part.
--   commit when armour - dmg >= floor (PT2_FINISH_LOCK_FLOOR_NONE / _ONE /
--            _MANY by pills carried), else PT2_FINISH_LOCK_SKIP and the old
--            wait.
--
-- While the lock holds, goals.pick_goal returns the capture_pill goal ahead
-- of every pool (take_cover too). It ends the moment the kill shell dies
-- (shot_tracker says it is no longer in flight), or at once when armour goes
-- under the floor or a gun that was not counted at the start comes in range.
-- After that the normal goal choice decides. A target pill still alive when
-- the shell dies is marked wounded (state.wounded_pill), as the swerve's own
-- loiter path does, so the take stays the favourite.
--
-- Its own module so attack.lua, goals.lua and Brain.think gain no new local
-- or upvalue: they call require("pt2_kill_lock").
-- =========================================================================

local C            = require("constants")
local U            = require("util")
local cpf          = require("cpathfinder")
local LP           = require("live_physics")
local ally_state   = require("ally_state")
local shot_tracker = require("shot_tracker")
local print2       = require("print2")

local M = {}

local floor, sqrt = math.floor, math.sqrt

-- Hard backstop only: shot_tracker kills every shell by its own lifetime
-- cap long before this. Not a tunable.
local LOCK_MAX_TICKS = 150

local LPX = {}

-- Pill reload ticks at `anger` (see the header).
local function pill_reload(anger, x)
  local hits = require("attack").heat_hits_landed(anger)
  local r = x.pill_attack_ticks or 100
  local lo = x.pill_attack_min_ticks or 6
  for _ = 1, hits do
    r = floor(r / 2)
  end
  if r < lo then r = lo end
  if r < 1 then r = 1 end
  return r
end
M.pill_reload = pill_reload

local function pill_anger(p, now)
  return require("pt2_attack").anger_now(p, now)
end

local function floor_for(carried)
  if carried <= 0 then return C.PT2_FINISH_LOCK_FLOOR_NONE or 10 end
  if carried == 1 then return C.PT2_FINISH_LOCK_FLOOR_ONE or 15 end
  return C.PT2_FINISH_LOCK_FLOOR_MANY or 20
end

-- Points (world units) on the straight drive from the tank to the dead pill,
-- only the part the tank can cover in W ticks at road speed, one every half
-- tile, both ends included.
local function drive_points(info, dmx, dmy, W, x)
  local sx, sy = info.tankx, info.tanky
  local ex, ey = dmx * 256 + 128, dmy * 256 + 128
  local dx, dy = ex - sx, ey - sy
  local d = sqrt(dx * dx + dy * dy)
  local reach = W * (x.speed_road or 16)
  if d > 0 and reach < d then
    ex = sx + dx * reach / d
    ey = sy + dy * reach / d
    d = reach
  end
  local pts = {}
  local n = math.max(1, math.ceil(d / 128))
  for i = 0, n do
    local t = i / n
    pts[#pts + 1] = sx + (ex - sx) * t
    pts[#pts + 1] = sy + (ey - sy) * t
  end
  return pts
end

local function near_path(pts, wx, wy, r)
  local r2 = r * r
  for i = 1, #pts, 2 do
    local dx, dy = pts[i] - wx, pts[i + 1] - wy
    if dx * dx + dy * dy <= r2 then return true end
  end
  return false
end

local function hostile_pill(p)
  return p and (p.health or 0) > 0 and not p.in_tank and not p.carrier
     and not p._synth_carry and p.owner ~= "friendly" and p.owner ~= "allied"
end

-- Every gun that can hit the tank on that drive in W ticks.
-- Returns dmg, parts (list of {key, shots, dmg}), keys (set).
local function guns(state, world, info, tid, W, pts, x, now)
  local hit = x.hit_taken or 5
  local parts, keys, dmg = {}, {}, 0
  local function add(key, reload)
    local shots = floor(W / reload)
    local d = shots * hit
    dmg = dmg + d
    keys[key] = true
    parts[#parts + 1] = { key = key, shots = shots, dmg = d }
  end
  local target = tid and world.pills and world.pills[tid]
  if hostile_pill(target) then
    add("#" .. tid, pill_reload(pill_anger(target, now), x))
  end
  local pr = x.pill_range or 2048
  for id, p in pairs(world.pills or {}) do
    if id ~= tid and hostile_pill(p)
       and near_path(pts, p.mx * 256 + 128, p.my * 256 + 128, pr) then
      add("#" .. id, pill_reload(pill_anger(p, now), x))
    end
  end
  local tr = (C.TANK_COMBAT_ENGAGE_RANGE or 7) * 256
  local ets = state.perc and state.perc.enemy_tanks
  if ets then
    for i = 1, #ets do
      local e = ets[i]
      if e.wx and e.wy and near_path(pts, e.wx, e.wy, tr) then
        add("tank" .. tostring(e.id or i), x.reload or 13)
      end
    end
  end
  return dmg, parts, keys
end

local function parts_str(parts)
  local s = ""
  for i = 1, #parts do
    local q = parts[i]
    s = s .. string.format(" %s shots=%d dmg=%d", q.key, q.shots, q.dmg)
  end
  return s
end

-- A dead pill free to capture: the capture_pill pool's own filter
-- (goals.filter_capture_pill: dead, not carried, not kill-claimed, not
-- blocked, not stale) and no ally already on capture_pill for it (the same
-- ally test as attack.lua handoff_to_capture_pill). Nearest by the
-- completed Dijkstra slate, else by straight distance.
local function pick_dead_pill(state, world, info, now)
  local goals = require("goals")
  local self_pn = info.player_number
  local taken = nil
  for apn, slot in ally_state.iter_active(now, C.SQUAD_ALLY_MAX_AGE or 1750) do
    if apn ~= self_pn then
      local si = slot.info
      if si and si.goal == "capture_pill" then
        local t = tonumber(si.target)
        if t then taken = taken or {}; taken[t] = true end
      end
    end
  end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local boat = info.inboat and 1 or 0
  local best, best_c = nil, math.huge
  for id, p in pairs(world.pills or {}) do
    if (p.health or 0) == 0 and not (taken and taken[id])
       and goals.filter_capture_pill(p, state) == nil then
      local c = cpf.dijkstra_cost_at(1, p.mx, p.my, boat)
      if not c or c >= 1e29 then c = 1e9 + U.edist(tmx, tmy, p.mx, p.my) end
      if c < best_c then best, best_c = id, c end
    end
  end
  return best
end

local function skip(now, why, tail)
  return nil
end

-- Called once per finish swerve, on the tick its turn ticks are out, the
-- pill lives and our on-target shells cover its health. Returns the lock
-- (to hand to M.install after the attack goal is cleared) or nil.
function M.try_start(state, world, info, goal, pill, now)
  goal._pt2_lock_tried = true
  local hp = pill.health or 0
  local shells = goal._on_target_shells
  if not shells or goal._on_target_shells_tick ~= now then
    return skip(now, "no_shell")
  end
  local land = {}
  for i = 1, #shells do
    if shells[i].land then land[#land + 1] = shells[i] end
  end
  if #land < hp or hp <= 0 then
    return skip(now, "no_shell", string.format(" landing=%d hp=%d", #land, hp))
  end
  table.sort(land, function(a, b) return a.land < b.land end)
  local kill = land[hp]
  local W = kill.land - now
  if W <= 0 then return skip(now, "shell_now") end
  local did = pick_dead_pill(state, world, info, now)
  if not did then return skip(now, "no_dead_pill") end
  local dp = world.pills[did]
  local x = LP.context(info, LPX)
  local pts = drive_points(info, dp.mx, dp.my, W, x)
  local dmg, parts, keys = guns(state, world, info, goal.target_id, W, pts, x, now)
  local carried = info.carried_pills or 0
  local fl = floor_for(carried)
  local arm = info.armour or 0
  if arm - dmg < fl then
    return skip(now, string.format("armour %d-%d<%d", arm, dmg, fl),
      string.format(" pill=#%d kill_shell_left=%dt carried=%d%s",
        did, W, carried, BRAIN_DEBUG_MODE and parts_str(parts) or ""))
  end
  return {
    dead_id = did, mx = dp.mx, my = dp.my,
    target_id = goal.target_id, tmx = pill.mx, tmy = pill.my,
    shot = kill.s, land = kill.land, start = now,
    floor = fl, carried = carried, dmg = dmg, keys = keys,
    parts = BRAIN_DEBUG_MODE and parts_str(parts) or "",
    armour = arm,
  }
end

-- The pool panel's capture_pill row for the locked pill gets the
-- "[LOCK kill_shell]" chip (pool_render.lua get_formula).
local function mark_row(state, id, on)
  local ce = state.cost_cache and state.cost_cache["4:" .. id]
  if ce then ce._pt2_lock = on or nil end
end

-- Install capture_pill on the dead pill. state.goal was just cleared by
-- attack.lua's clear_attack_goal; it is changed in place, as
-- handoff_to_capture_pill does (init.lua treats the goal table's identity
-- as the decision).
function M.install(state, world, info, L, now)
  local g = state.goal
  g.kind      = "capture_pill"
  g.mx, g.my  = L.mx, L.my
  g.wx, g.wy  = U.m2w(L.mx), U.m2w(L.my)
  g.target_id = L.dead_id
  g.race_mode = C.CAPTURE_RACE_MODE_CAPTURE
  g._pt2_lock = true
  state.goal_set_tick = now
  state._pt2_lock = L
  mark_row(state, L.dead_id, true)
end

local function end_lock(state, world, now, why)
  local L = state._pt2_lock
  state._pt2_lock = nil
  mark_row(state, L.dead_id, false)
  local g = state.goal
  if g and g._pt2_lock then g._pt2_lock = nil end
  if why ~= "goal_changed" then
    state._force_replan_reason = state._force_replan_reason or "pt2_lock_end"
  end
  local tp = L.target_id and world.pills and world.pills[L.target_id]
  if tp and (tp.health or 0) > 0 then
    -- Same marker the swerve's loiter path sets: the take stays the favourite.
    state.wounded_pill = { id = L.target_id, mx = tp.mx, my = tp.my,
                           hp = tp.health, tick = now, owner = tp.owner }
  end
end

-- Every think (attack.update_attack_substate): end the lock when it must.
function M.update(state, world, info, now)
  local L = state._pt2_lock
  if not L then return end
  local g = state.goal
  local why = nil
  if not g or g.kind ~= "capture_pill" or g.target_id ~= L.dead_id then
    why = "goal_changed"
  elseif not L.shot or L.shot.status ~= "in_flight" then
    local tp = L.target_id and world.pills and world.pills[L.target_id]
    why = (tp and (tp.health or 0) > 0) and "pill_alive" or "shell_dead"
  else
    local dp = world.pills and world.pills[L.dead_id]
    if not dp or dp.in_tank or dp.carrier or (dp.health or 0) > 0 then
      why = "dead_pill_gone"
    elseif (info.armour or 0) < L.floor then
      why = string.format("armour %d<%d", info.armour or 0, L.floor)
    elseif now - L.start > LOCK_MAX_TICKS then
      why = "timeout"
    else
      local W = L.land - now
      if W < 1 then W = 1 end
      local x = LP.context(info, LPX)
      local pts = drive_points(info, L.mx, L.my, W, x)
      local _, _, keys = guns(state, world, info, L.target_id, W, pts, x, now)
      for k in pairs(keys) do
        if not L.keys[k] then why = "new_hostile " .. k break end
      end
    end
  end
  if why then
    end_lock(state, world, now, why)
  else
    mark_row(state, L.dead_id, true)
  end
end

-- goals.pick_goal: true while the lock holds on the live goal. The end
-- tests run first, so a replan on the tick the shell dies is already free.
function M.holds(state, world, info, quiet)
  M.update(state, world, info, state.tick or 0)
  local L = state._pt2_lock
  local g = state.goal
  if not (L and g and g.kind == "capture_pill" and g.target_id == L.dead_id) then
    return false
  end
  return true
end

-- " lock=kill_shell" for the GOAL_CHANGE line, else "".
function M.goal_tag(state, g)
  local L = state._pt2_lock
  if L and g and g.kind == "capture_pill" and g.target_id == L.dead_id then
    return " lock=kill_shell"
  end
  return ""
end

return M
