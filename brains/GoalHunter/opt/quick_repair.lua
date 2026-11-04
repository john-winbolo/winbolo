local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/quick_repair.lua — QUICK REPAIR of a take blocker (2026-11-04)
--
-- C.QUICK_REPAIR_ON_TAKE. During an attack_pill take, a friendly pill the team
-- declared a blocker for the take (p._in_use, init.lua) is refused by every
-- repair path while it is alive (builder_pool pill_blocked "take_blocker",
-- goals.lua builder_can_repair `not p._in_use`). This module finds the moment
-- the builder may repair it anyway: the repair must land after the target
-- pill is dead (our shells no longer need the line) and after every hostile
-- shell in the air that will hit the blocker has landed (or the repair is
-- shot off again at once).
--
-- THE RULE, every tick (all ticks are brain ticks, see UNITS):
--   T_kill   = the tick the target pill dies.
--              inflight: our in-flight shells that reach the target tile
--                (attack.lua update_shot_accounting) sorted by landing tick;
--                the hp-th one kills it.
--              predict (C.QUICK_REPAIR_PREDICT_SHOTS, only in the take's
--                firing run): the shells still needed (hp - in flight) are
--                fired one reload apart, the first one now + 1 when the gun
--                is ready or now + reload when it is reloading, and each
--                flies the tank's current distance to the target tile.
--              Otherwise unknown -> wait:nokill.
--   T_safe   = max(T_kill, landing tick of every non-friendly shell in the
--              air whose flight reaches the blocker tile).
--   T_repair = now + 1 + walk. The builder leaves on the tick after the
--              order (+1) and walks `walk` ticks (the C walk sim). The pill's
--              armour comes back ON ARRIVAL, not after the build dwell: in
--              lgm.c lgmMoveAway the arrival test calls lgmDoWork at once
--              (pillsRepairPos for LGM_PILL_REQUEST with no pill carried) and
--              only then sets waitTime = lgm_build_ticks for the stand. So
--              the dwell (LGM_BUILD_TIME) is NOT in T_repair; it is still in
--              the certain-death check, which flies shells against the whole
--              errand (out, dwell, home).
--   send when T_repair >= T_safe + QUICK_REPAIR_MARGIN_TICKS
--             and the builder is in the tank (not afloat, no pool job)
--             and the tank holds the wood the repair needs
--             and danger.lgm_certain_death says nothing for that pill.
--   Otherwise wait, with the first failing reason, in this order:
--   nokill, reach, early, man, wood, death.
--
-- The send itself is the builder pool's: on "send" builder_pool.update marks
-- that pill's row quick (take_blocker, leash, mode, tree reserve and
-- MIN_SCORE waived; wood, reach, the ally claim and certain death kept) and
-- sorts it first. Nothing else changes: with the knob off, or on any tick that
-- is not "send", every repair path refuses the blocker as before.
--
-- UNITS. Shells, the builder and the tank's reload all advance once per game
-- tick (server_sim_tick.c runs tankUpdate's game arm, lgmUpdate and
-- shellsUpdate only on sim->tick % 2 == 0), and the brain thinks once per game
-- tick. So a shell's life, a walk tick, LGM_BUILD_TIME and the reload count
-- (13 classic) are all brain ticks here.
--
-- CHOICE OF BLOCKER: of the damaged or dead (health < max) friendly or allied
-- pills on a blocker tile, the lowest health first (a dead one first), then
-- the shortest straight distance from the tank, then the tile key.
--
-- WHAT IT CANNOT SEE: shots not yet fired by the target pill or an enemy (a
-- pill that fires again at the blocker after the send lands after T_safe); a
-- tank that holds its fire longer than one reload in the firing run. Only
-- visible shells and our own shot record are used.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local LP     = require("live_physics")
local print2 = require("print2")
local viz    = require("viz")

local M = {}

-- Reused across calls.
local WALK = {}
local LPX  = {}

-- Ticks from a shell at (x, y) moving (sx, sy) a tick until it is on tile
-- (mx, my). 0 when it is on it now; nil when it does not get there within
-- `max` ticks.
function M.ticks_to_tile(x, y, sx, sy, mx, my, max)
  for k = 0, max or 72 do
    local px = x + sx * k
    local py = y + sy * k
    if math.floor(px / 256) == mx and math.floor(py / 256) == my then
      return k
    end
  end
  return nil
end

-- attack.lua update_shot_accounting: the landing tick of one in-flight shot
-- that reaches the target tile (mx, my). Starts from the engine shell the
-- tracker matched this tick when there is one (closest_fx/fy is the real
-- position), else from the tracker's own position.
function M.shot_land_tick(s, mx, my, now)
  local x, y = s.cur_fx, s.cur_fy
  if s.matched and s.closest_fx and s.closest_fy then
    x, y = s.closest_fx, s.closest_fy
  end
  local k = M.ticks_to_tile(x, y, s.step_x or 0, s.step_y or 0, mx, my, 72)
  if not k then return nil end
  return (now or 0) + k
end

-- Is a pill sitting live on (mx, my) other than the one at (xmx, xmy)?
local function live_pill_on(world, mx, my)
  local lst = world and world.pill_at and world.pill_at[my * 256 + mx]
  if lst then
    for i = 1, #lst do
      local p = lst[i].pill
      if p and (p.health or 0) > 0 and not p.in_tank then return true end
    end
  end
  return false
end

-- Every non-friendly shell in the air that reaches the blocker tile (bmx,
-- bmy): { {t = ticks from now, x, y, angle, src}, ... }. The flight is
-- danger.lua's (SHELL_SPEED a tick along the exact angle, `life` moves). It
-- stops only where the stop is sure: a wall (building, half building) or
-- another live pill. A tank or a base it might hit first is ignored, so the
-- shell is taken to fly on to the blocker: that can only make T_safe later.
-- The blocker tile itself counts whether the pill is alive or dead now: once
-- repaired it is alive and the shell hits it.
function M.shells_on_blocker(world, info, bmx, bmy)
  local out = {}
  local objs = info.objects
  if not objs then return out end
  local N = C.LGM_SHELL_PREDICT_TICKS or 63
  for _, ob in ipairs(objs) do
    if ob.type == OBJECT_SHOT and (ob.info or 0) ~= 0 then
      local life = ob.life
      local moves = (type(life) == "number" and life >= 0 and life <= N) and life or N
      local vx =  U.bsin_f(ob.angle or 0) * C.SHELL_SPEED
      local vy = -U.bcos_f(ob.angle or 0) * C.SHELL_SPEED
      local sx, sy = ob.x + 0.0, ob.y + 0.0
      for t = 1, moves do
        sx = sx + vx
        sy = sy + vy
        if sx < 0x100 or sx > 0xFEFF or sy < 0x100 or sy > 0xFEFF then break end
        local smx = bit.rshift(math.floor(sx), 8)
        local smy = bit.rshift(math.floor(sy), 8)
        if smx == bmx and smy == bmy then
          out[#out + 1] = { t = t, x = ob.x, y = ob.y,
                            src = ((ob.owner or 255) == 255) and "pill"
                                  or ("p" .. tostring(ob.owner)) }
          break
        end
        if live_pill_on(world, smx, smy) then break end
        local tt = U.ttype(smx, smy)
        if tt == C.T_BUILDING or tt == C.T_HALFBUILD then break end
      end
    end
  end
  return out
end

-- The reload interval: live_physics' figure (the engine's own reload ticks
-- when the host sends them, else rules x reload mod).
function M.reload_ticks(info)
  local x = LP.context(info, LPX)
  local r = x and x.reload
  if type(r) ~= "number" or r < 1 then r = 13 end
  return r
end

-- Flight ticks of a shell fired now from the tank to the target tile: the
-- shell starts SHELL_START_ADD (5) steps out and must reach the tile edge
-- (half a tile before the centre).
function M.flight_ticks(info, tmx, tmy)
  local dx = (tmx * 256 + 128) - info.tankx
  local dy = (tmy * 256 + 128) - info.tanky
  local d = math.sqrt(dx * dx + dy * dy)
  local f = math.ceil((d - 128 - 5 * C.SHELL_SPEED) / C.SHELL_SPEED)
  if f < 1 then f = 1 end
  return f
end

-- Is the take shooting this pill right now?
local function firing_run(goal)
  if not goal._attack_start_tick then return false end
  local fs = C.QUICK_REPAIR_FIRE_SUBSTATES or {}
  return fs[goal.substate or ""] == true
end

-- T_kill, its source, and the parts it was made of (for the log and the
-- overlay). nil when unknown.
function M.kill_tick(goal, target, info, now)
  local hp = target.health or 0
  local k = { hp = hp, n_inflight = 0 }
  if hp <= 0 then
    k.t, k.src = now, "dead"
    return k
  end
  local land = {}
  if goal._on_target_land and goal._on_target_land_tick == now then
    for i = 1, #goal._on_target_land do land[i] = goal._on_target_land[i] end
  end
  table.sort(land)
  k.land = land
  k.n_inflight = #land
  if #land >= hp then
    k.t, k.src = land[hp], "inflight"
    return k
  end
  if not C.QUICK_REPAIR_PREDICT_SHOTS then
    k.why = "inflight_short"
    return k
  end
  if not firing_run(goal) then
    k.why = "not_firing"
    return k
  end
  local R = M.reload_ticks(info)
  local first = info.reload and (now + R) or (now + 1)
  local remaining = hp - #land
  local flight = M.flight_ticks(info, goal.mx, goal.my)
  local last_fire = first + (remaining - 1) * R
  local t = last_fire + flight
  if #land > 0 and land[#land] > t then t = land[#land] end
  k.t, k.src = t, "predict"
  k.reload, k.first, k.remaining, k.flight, k.last_fire = R, first, remaining, flight, last_fire
  return k
end

-- Trees a repair of pill `p` takes: the builder pool's numbers.
function M.trees_need(p)
  local maxhp = C.PILLS_MAX_HEALTH or 15
  local hp = p.health or 0
  if hp <= 0 then return C.BUILDER_POOL_TREES_REBUILD or 4 end
  return math.max(C.BUILDER_POOL_TREES_TOPUP or 1,
                  math.ceil((maxhp - hp) / (C.PILL_REPAIR_AMOUNT or 4)))
end

-- The out leg the builder pool prices (builder_pool.lgm_trip): next door is
-- one grass tile, else the C walk sim. nil when he cannot get there.
function M.walk_ticks(info, mx, my)
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local near = C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16
  if math.abs(mx - tmx) + math.abs(my - tmy) <= 1 then return near end
  local out = cpf_lgm_travel_ticks_map(tmx, tmy, mx, my, mx, my,
                                       C.BUILDER_POOL_LGM_MAX_TICKS or 2000,
                                       C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
  if out == nil or out < 0 then return nil end
  if out == 0 then out = near end
  return out
end

-- The blocker to repair (see CHOICE OF BLOCKER), or nil.
function M.pick_blocker(world, info)
  local maxhp = C.PILLS_MAX_HEALTH or 15
  local tx, ty = info.tankx / 256.0, info.tanky / 256.0
  local best, best_d, best_id
  for pid, p in pairs(world.pills or {}) do
    if p._in_use and (p.owner == "friendly" or p.owner == "allied")
       and not p.in_tank and not p.carrier and (p.health or 0) < maxhp then
      local dx, dy = p.mx + 0.5 - tx, p.my + 0.5 - ty
      local d = dx * dx + dy * dy
      local better = false
      if not best then better = true
      elseif (p.health or 0) ~= (best.health or 0) then
        better = (p.health or 0) < (best.health or 0)
      elseif d ~= best_d then better = d < best_d
      else better = (p.my * 256 + p.mx) < (best.my * 256 + best.mx) end
      if better then best, best_d, best_id = p, d, pid end
    end
  end
  return best, best_id
end

-- The target pill of the take, or nil.
local function target_pill(world, goal)
  local lst = world.pill_at and world.pill_at[goal.my * 256 + goal.mx]
  if lst then
    for i = 1, #lst do
      local p = lst[i].pill
      if p and p.owner ~= "friendly" and p.owner ~= "allied" and not p.in_tank then
        return p
      end
    end
  end
  return nil
end

-- One tick's answer. Returns the record (also left in state._quick_repair),
-- or nil when there is no quick repair to think about (knob off, no take, no
-- damaged blocker).
function M.evaluate(state, world, info, now)
  state._quick_repair = nil
  if not C.QUICK_REPAIR_ON_TAKE then return nil end
  local goal = state.goal
  if not goal or goal.kind ~= "attack_pill" or not goal.mx or not goal.my then
    state._qr_log_key = nil
    return nil
  end
  local target = target_pill(world, goal)
  if not target then state._qr_log_key = nil; return nil end
  local p, pid = M.pick_blocker(world, info)
  if not p then state._qr_log_key = nil; return nil end

  local margin = C.QUICK_REPAIR_MARGIN_TICKS or 2
  local qr = { tick = now, mx = p.mx, my = p.my, id = pid,
               hp = p.health or 0, target_mx = goal.mx, target_my = goal.my,
               margin = margin, dwell = 0,
               build_after = C.LGM_BUILD_TIME or 20 }
  local k = M.kill_tick(goal, target, info, now)
  qr.kill = k
  qr.t_kill, qr.kill_src = k.t, k.src
  qr.hits = M.shells_on_blocker(world, info, p.mx, p.my)
  local t_safe = k.t
  if t_safe then
    for i = 1, #qr.hits do
      local lt = now + qr.hits[i].t
      if lt > t_safe then t_safe = lt end
    end
  end
  qr.t_safe = t_safe
  qr.walk = M.walk_ticks(info, p.mx, p.my)
  qr.t_repair = qr.walk and (now + 1 + qr.walk) or nil
  qr.trees_need = M.trees_need(p)

  local result
  if not k.t then
    result = "wait:nokill"
  elseif not qr.walk then
    result = "wait:reach"
  elseif qr.t_repair < t_safe + margin then
    result = "wait:early"
    qr.open_in = t_safe + margin - qr.t_repair
  elseif info.man_status ~= C.LGM_INTANK or info.inboat or state._bp_job then
    result = "wait:man"
  elseif (info.trees or 0) < qr.trees_need then
    result = "wait:wood"
  else
    local hit = danger.lgm_certain_death(world, info, p.mx, p.my)
    if hit then
      result = "wait:death"
      qr.death = hit
    else
      result = "send"
    end
  end
  qr.result = result
  state._quick_repair = qr

  if BRAIN_DEBUG_MODE then
    local key = string.format("%d:%d:%s:%s", p.mx, p.my, result, tostring(k.src))
    if state._qr_log_key ~= key then
      state._qr_log_key = key
      print2(string.format(
        "QUICK_REPAIR t=%d pill=(%d,%d) hp=%d target=(%d,%d) T_kill=%s src=%s"
        .. " T_safe=%s walk=%s dwell=%d -> %s"
        .. " (T_repair=now+1+walk=%s margin=%d hostile_hits=%d inflight=%d/%d%s trees=%d/%d)",
        now, p.mx, p.my, qr.hp, goal.mx, goal.my, tostring(k.t),
        tostring(k.src or k.why or "-"), tostring(t_safe), tostring(qr.walk),
        qr.dwell, result, tostring(qr.t_repair), margin, #qr.hits,
        k.n_inflight or 0, k.hp or 0,
        k.src == "predict" and string.format(
          " reload=%d first_fire=%d remaining=%d flight=%d", k.reload, k.first,
          k.remaining, k.flight) or "",
        info.trees or 0, qr.trees_need))
    end
    M.draw(state, info, qr)
  end
  return qr
end

-- ── Overlays (BrainTest category "Quick repair") ──────────────────────────
local RESULT_RGB = {
  send          = { 80, 255, 80 },
  ["wait:early"] = { 255, 220, 60 },
  ["wait:death"] = { 255, 70, 70 },
  ["wait:nokill"] = { 170, 170, 170 },
  ["wait:wood"]  = { 200, 140, 60 },
  ["wait:man"]   = { 120, 180, 255 },
  ["wait:reach"] = { 170, 170, 170 },
}

function M.draw(state, info, qr)
  qr = qr or state._quick_repair
  if not qr then return end
  local now = qr.tick
  local rgb = RESULT_RGB[qr.result] or { 255, 255, 255 }
  local r, g, b = rgb[1], rgb[2], rgb[3]
  local bx, by = qr.mx + 0.5, qr.my + 0.5

  -- 1. The blocker and the verdict.
  viz.rect("quick_repair_blocker", qr.mx, qr.my, qr.mx + 1, qr.my + 1, r, g, b, 90)
  viz.circle("quick_repair_blocker", bx, by, 0.6, r, g, b, 230)
  local state_txt = qr.result:gsub("^wait:", "")
  local tail
  if qr.result == "wait:early" then
    tail = string.format(" opens in %dt", qr.open_in or 0)
  elseif qr.result == "wait:death" and qr.death then
    tail = string.format(" (shell %s at +%dt, %s)", tostring(qr.death.src),
                         qr.death.t or 0, tostring(qr.death.how))
  elseif qr.result == "wait:wood" then
    tail = string.format(" (trees %d < %d)", info.trees or 0, qr.trees_need)
  else
    tail = ""
  end
  viz.text("quick_repair_blocker", bx, by - 1.0,
           string.format("QR %s hp=%d/%d%s", state_txt, qr.hp,
                         C.PILLS_MAX_HEALTH or 15, tail),
           "center", r, g, b, 240)

  -- 2. The predicted kill and the hostile shells on the blocker.
  local k = qr.kill
  local tx, ty = qr.target_mx + 0.5, qr.target_my + 0.5
  if k and k.t then
    local txt = string.format("kill @+%dt (%s, hp %d)", k.t - now, k.src, k.hp)
    if k.src == "inflight" then
      txt = txt .. string.format(" = shell #%d of %d in flight", k.hp, k.n_inflight)
    elseif k.src == "predict" then
      txt = txt .. string.format(
        " = %d in flight + %d to fire: first @+%d, every %dt, last @+%d, flight %dt",
        k.n_inflight, k.remaining, k.first - now, k.reload, k.last_fire - now, k.flight)
    end
    viz.circle("quick_repair_kill", tx, ty, 0.7, 255, 120, 60, 220)
    viz.text("quick_repair_kill", tx, ty - 1.0, txt, "center", 255, 150, 90, 240)
  else
    viz.text("quick_repair_kill", tx, ty - 1.0,
             string.format("kill unknown (%s, %d/%d in flight)",
                           tostring(k and k.why or "-"), k and k.n_inflight or 0,
                           k and k.hp or 0),
             "center", 170, 170, 170, 240)
  end
  for i = 1, #(qr.hits or {}) do
    local h = qr.hits[i]
    local sx, sy = h.x / 256.0, h.y / 256.0
    viz.line("quick_repair_kill", sx, sy, bx, by, 255, 60, 200, 180)
    viz.text("quick_repair_kill", sx, sy - 0.5,
             string.format("%s hits blocker +%dt", h.src, h.t),
             "center", 255, 90, 220, 240)
  end

  -- 3. The walk and T_repair against T_safe.
  local tfx, tfy = info.tankx / 256.0, info.tanky / 256.0
  if type(cpf_lgm_walk_path) == "function" then
    local n = cpf_lgm_walk_path(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8),
                                qr.mx, qr.my, qr.mx, qr.my, 128,
                                C.BUILDER_POOL_LGM_STUCK_TICKS or 150, WALK)
    local px, py = tfx, tfy
    for i = 1, n or 0 do
      local nx, ny = WALK[i * 2 - 1] / 256.0, WALK[i * 2] / 256.0
      viz.line("quick_repair_walk", px, py, nx, ny, r, g, b, 160)
      px, py = nx, ny
    end
  else
    viz.line("quick_repair_walk", tfx, tfy, bx, by, r, g, b, 160)
  end
  viz.text("quick_repair_walk", tfx + 0.6, tfy + 0.8,
           string.format("T_repair +%s = 1 + walk %s (armour on arrival; dwell %d after)"
                         .. " | T_safe +%s = max(kill +%s, last hostile hit +%s) | need"
                         .. " T_repair >= T_safe + margin %d -> %s",
                         qr.t_repair and tostring(qr.t_repair - now) or "-",
                         tostring(qr.walk), qr.build_after,
                         qr.t_safe and tostring(qr.t_safe - now) or "-",
                         k and k.t and tostring(k.t - now) or "-",
                         M.last_hit(qr) and tostring(M.last_hit(qr)) or "none",
                         qr.margin, qr.result),
           "topleft", r, g, b, 240)
end

-- The latest hostile landing on the blocker, ticks from now, or nil.
function M.last_hit(qr)
  local m
  for i = 1, #(qr.hits or {}) do
    if not m or qr.hits[i].t > m then m = qr.hits[i].t end
  end
  return m
end

return M
