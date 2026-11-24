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
--                is ready, else our last shot + reload (now + reload when
--                the last shot is not known), and each
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
--             and the tank is clear of pill fire for
--             QUICK_REPAIR_TANK_CLEAR_TICKS (M.tank_clear).
--             and (C.QUICK_REPAIR_WAIT_ALL_PILLS) no predicted pill shot
--             conflicts with the errand (M.fire_conflict; see "The
--             repair-side rules" below). On a send, C.SWERVE_AVOID_REPAIR_SIDE
--             commits the clean unblocked swerve side (M.side_and_fire).
--   Otherwise wait, with the first failing reason, in this order:
--   nokill, reach, early, man, wood, death, tank, fire_predict / other_pill.
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

-- run_c7 (c6 beside_e t=675): the walk of our real shell from where the
-- engine has it now (shot_tracker real_fx/real_fy, matched this tick) for
-- the moves it has left (real_life), at the engine's 24.8 step for the
-- tank's float fire angle (utilCalcDistanceHP). Returns the squares in
-- order, each { mx, my, k } with k = moves from real_tick to its first
-- entry, or nil when the shell was not matched or has no fire angle.
-- The old test fired a new sim shell from the tank at the byte direction:
-- the shells that killed pill (125,118) graze its edge (y 119.0 at life 1,
-- the last move takes them in), so that sim ended at (125,119).
function M.shell_real_walk(s)
  if not (s.real_fx and s.real_fy and s.real_life and s.angle) then return nil end
  local speed = s.speed or C.SHELL_SPEED
  local a = s.angle - 64
  if a < 0 then a = a + 256 end
  local r = a * (2 * math.pi / 256)
  local function rd(v)
    if v >= 0 then return math.floor(v + 0.5) end
    return -math.floor(-v + 0.5)
  end
  local xs = rd(speed * math.cos(r) * 256) / 256
  local ys = rd(speed * math.sin(r) * 256) / 256
  local out = {}
  local lmx, lmy = math.floor(s.real_fx / 256), math.floor(s.real_fy / 256)
  out[1] = { mx = lmx, my = lmy, k = 0 }
  for k = 1, s.real_life do
    local mx = math.floor((s.real_fx + xs * k) / 256)
    local my = math.floor((s.real_fy + ys * k) / 256)
    if mx ~= lmx or my ~= lmy then
      out[#out + 1] = { mx = mx, my = my, k = k }
      lmx, lmy = mx, my
    end
  end
  return out
end

-- run_c7 (c6 beside_e t=696): the shell sim (cpf.simulate_shot) says this
-- in-flight shot reaches the target tile, but the tracker's straight steps
-- only graze it (the shells ended on the tile's edge, y = 119.0, and still
-- killed pill (125,118)), so shot_land_tick finds no tick. Its landing tick
-- is then the tick of its closest pass to the tile, within the same 72
-- steps.
function M.shot_near_tick(s, mx, my, now)
  local x, y = s.cur_fx, s.cur_fy
  if s.matched and s.closest_fx and s.closest_fy then
    x, y = s.closest_fx, s.closest_fy
  end
  local sx, sy = s.step_x or 0, s.step_y or 0
  local x0, y0, x1, y1 = mx * 256, my * 256, mx * 256 + 255, my * 256 + 255
  local best, bk
  for k = 0, 72 do
    local px, py = x + sx * k, y + sy * k
    local ex = px < x0 and x0 - px or (px > x1 and px - x1 or 0)
    local ey = py < y0 and y0 - py or (py > y1 and py - y1 or 0)
    local d = ex * ex + ey * ey
    if not best or d < best then best, bk = d, k end
  end
  return (now or 0) + bk
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

-- C.QUICK_REPAIR_SKIP_OWN_FIRE: is shell ob our own tank's shell on its way
-- to the take target tile (tmx, tmy)? It is ours when its owner is our
-- player number. It is on its way when its flight (SHELL_SPEED a tick along
-- its exact angle, its remaining life in moves, the same flight as
-- shells_on_blocker) reaches the target tile. Walls are not looked at: a
-- shell we fired at the target is our shot at the target either way.
function M.own_shell_at_target(ob, self_pn, tmx, tmy)
  if not (ob and self_pn ~= nil and tmx and tmy) then return false end
  if ob.owner ~= self_pn then return false end
  local N = C.LGM_SHELL_PREDICT_TICKS or 63
  local life = ob.life
  local moves = (type(life) == "number" and life >= 0 and life <= N) and life or N
  local vx =  U.bsin_f(ob.angle or 0) * C.SHELL_SPEED
  local vy = -U.bcos_f(ob.angle or 0) * C.SHELL_SPEED
  return M.ticks_to_tile(ob.x + 0.0, ob.y + 0.0, vx, vy, tmx, tmy, moves) ~= nil
end

-- A pill's next fire tick (tank_clear (b) below, and
-- QUICK_REPAIR_WAIT_ALL_PILLS): the later of now + 1 and (last fire +
-- reload). last fire = from its youngest visible shell whose line runs back
-- to the pill tile: its distance from the pill centre / SHELL_SPEED moves ago
-- (none seen = no last fire). reload = pt2_kill_lock.pill_reload (the
-- engine's halving ladder at the pill's anger). x = live_physics.context.
-- Returns next, reload, last (last nil when no shell was seen).
function M.pill_next_fire(p, info, now, x)
  local N = C.LGM_SHELL_PREDICT_TICKS or 63
  local objs = info.objects or {}
  local KLK = require("pt2_kill_lock")
  local PA = require("pt2_attack")
  local last
  for _, ob in ipairs(objs) do
    if ob.type == OBJECT_SHOT and (ob.owner or 255) == 255 and (ob.info or 0) ~= 0 then
      local vx =  U.bsin_f(ob.angle or 0) * C.SHELL_SPEED
      local vy = -U.bcos_f(ob.angle or 0) * C.SHELL_SPEED
      if M.ticks_to_tile(ob.x + 0.0, ob.y + 0.0, -vx, -vy, p.mx, p.my, N) then
        -- Its line runs back to this pill: it left the pill centre this
        -- many moves ago.
        local ddx, ddy = ob.x - (p.mx * 256 + 128), ob.y - (p.my * 256 + 128)
        local k = math.floor(math.sqrt(ddx * ddx + ddy * ddy) / C.SHELL_SPEED + 0.5)
        if not last or now - k > last then last = now - k end
      end
    end
  end
  local reload = KLK.pill_reload(PA.anger_now(p, now), x)
  -- run_c7: the engine's ladder from the pill's seen hits, when known.
  if p._eng_hits then
    reload = require("pt2_start").engine_reload(p._eng_hits, now, x)
  end
  local nxt = now + 1
  if last and last + reload > nxt then nxt = last + reload end
  return nxt, reload, last
end

-- C.QUICK_REPAIR_TANK_CLEAR_TICKS (W, Nov 22, Andrew): the tank must be
-- clear of pill fire for W ticks. A pill that shoots at the tank shoots
-- along the line the builder walks out on (the blocker sits between the tank
-- and the pills), and a shell fired after the send is not in the
-- certain-death check. So while a live non-friendly pill can shoot our tank
-- (the pill range reaches the tank), send only when
--   (a) no non-friendly shell in the air lands on the tank tile within W
--       ticks (shells_on_blocker flown at the tank tile), and
--   (b) no such pill's next shot can land on the tank within W ticks:
--       next fire = the later of now + 1 and (last fire + reload);
--       last fire = from its youngest visible shell whose line runs back to
--         the pill tile: its distance from the pill centre / SHELL_SPEED
--         moves ago (none seen = no last fire);
--       reload = pt2_kill_lock.pill_reload (the engine's halving ladder at
--         the pill's anger);
--       land = next fire + the flight from the pill tile to the tank tile.
-- Returns nil when clear (or W <= 0, or no pill can shoot the tank), else
-- { why = "shell" | "next", t = ticks from now, src / pill, ... }.
function M.tank_clear(world, info, now)
  local W = C.QUICK_REPAIR_TANK_CLEAR_TICKS or 0
  if W <= 0 then return nil end
  local x = LP.context(info, LPX) or {}
  local pr = x.pill_range or 2048
  local pr2 = pr * pr
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local guns
  for id, p in pairs(world.pills or {}) do
    if (p.health or 0) > 0 and not p.in_tank and not p.carrier
       and p.owner ~= "friendly" and p.owner ~= "allied" then
      local dx = p.mx * 256 + 128 - info.tankx
      local dy = p.my * 256 + 128 - info.tanky
      if dx * dx + dy * dy <= pr2 then
        guns = guns or {}
        guns[#guns + 1] = id
      end
    end
  end
  if not guns then return nil end
  local hits = M.shells_on_blocker(world, info, tmx, tmy)
  for i = 1, #hits do
    if hits[i].t <= W then
      return { why = "shell", t = hits[i].t, src = hits[i].src }
    end
  end
  local N = C.LGM_SHELL_PREDICT_TICKS or 63
  for gi = 1, #guns do
    local id = guns[gi]
    local p = world.pills[id]
    local nxt, reload, last = M.pill_next_fire(p, info, now, x)
    local px, py = p.mx * 256 + 128, p.my * 256 + 128
    local dx, dy = info.tankx - px, info.tanky - py
    local d = math.sqrt(dx * dx + dy * dy)
    local flight = 0
    if d > 0 then
      flight = M.ticks_to_tile(px, py, dx / d * C.SHELL_SPEED, dy / d * C.SHELL_SPEED,
                               tmx, tmy, N)
    end
    if flight then
      local land = nxt + flight - now
      if land <= W then
        return { why = "next", t = land, pill = id, reload = reload,
                 last = last and (last - now) or nil, flight = flight }
      end
    end
  end
  return nil
end

-- ══ The repair-side rules (2026-11-24, Andrew) ═══════════════════════════
-- C.SWERVE_AVOID_REPAIR_SIDE (Rule 1) and C.QUICK_REPAIR_WAIT_ALL_PILLS
-- (Rule 2). Andrew: "trace the path of the pill to our tank, that's the
-- dividing line. if we swerve all pill shots will flow in one side of that
-- line or the other, a swerve never crosses it." And: "we'll need to pick a
-- side BEFORE the builder goes out".
--
-- THE FAN. A pill shoots at our tank. When the tank swerves to one side,
-- every shot the pill fires at it flies along a ray from the pill centre to
-- a point of the swerve; the rays fill a wedge on that side of the line
-- pill -> tank (the line itself is the open edge). The swerve is the one
-- attack.lua compute_best_swerve_dir samples: SWERVE_SAMPLE_PERP_TILES (2)
-- tiles off to the side of the tank (perpendicular to tank -> target pill),
-- pushed SWERVE_COVER_EXTEND_TILES further out from the target pill, then
-- the retreat ATTACK_APPROACH_OFFSET tiles straight back from the pill. The
-- wedge's angle comes from those points: about 18 degrees at a 6-tile
-- range. A side is DIRTY for a pill when the blocker square, grown by
-- REPAIR_FIRE_LANE_MARGIN_TILES on each side, lies partly inside the
-- wedge, no farther from the pill than its range. The builder's own square
-- is not part of this test: a shell that flies over him does not kill him
-- (Rule 2's explosion test below covers him).
--
-- Rule 1. A side is CLEAN when it is not dirty for any live non-friendly
-- pill whose range reaches our tank. A side is BLOCKED by terrain when
-- attack.lua's own side score has a hazard on it (the deep-water retreat
-- penalty or a -10 out-of-range tile on the sample line; attack.lua hands
-- its scorer over as M.side_scores). At quick-repair send time, a clean
-- side that is not blocked is COMMITTED (goal._repair_swerve_dir): every
-- swerve that starts while that repair runs takes it. With no such side,
-- the send waits for Rule 2. A swerve that starts while a blocker is
-- damaged but no repair is out picks the one clean unblocked side too.
--
-- Rule 2. Every live non-friendly pill whose range covers the blocker and
-- our tank fires its next shot (pill_next_fire) and then one a reload, up
-- to the tick it dies (T_kill for the take target; never for the others)
-- and the end of the builder's errand (arrival + LGM_BUILD_TIME). A shot
-- aims at the tank where it is, or into the swerve side (the committed
-- side, or the side of a swerve that is running) once the swerve starts
-- (now when it runs, else from T_kill). It CONFLICTS when its ray passes
-- the grown blocker square and reaches the pill-blocker distance at a tick
-- inside the errand, or EXPLODES where it would kill the builder. A shell
-- kills a builder only where it explodes (shells.c shellsUpdate calls
-- shellsBlastTestBuilder only on a hit or when the shell runs out; lgm.c
-- lgmBlastCatches): on the very square for a hit on a pill, wall or other
-- solid square, else inside the lgm_blast_shape area round the explosion
-- (half a tile in the classic table, U.lgm_open_blast_catches). A pill's
-- shell that misses the blocker explodes on the tank it is aimed at, or at
-- the end of its flight when the tank is out of range. The builder conflicts
-- when he is inside that area at that tick: on his walk (a straight walk at
-- the walk-sim pace) or on the blocker square while he builds. A shell that
-- only passes over or near him on its flight is no conflict. A pill whose wedge on
-- the swerve side is clean does not conflict. Send only with no conflict;
-- while the builder is out on a quick repair, the same test runs every
-- tick from where he stands (QUICK_REPAIR_ABORT why=pill_fire_predict).
-- ═════════════════════════════════════════════════════════════════════════

M.SWERVE_SAMPLE_PERP_TILES = 2    -- attack.lua compute_best_swerve_dir's sample offset

local TWO_PI = 2 * math.pi
local function wrap(a)
  while a > math.pi do a = a - TWO_PI end
  while a <= -math.pi do a = a + TWO_PI end
  return a
end

-- The repair blocker: the blocker of the quick repair that is out (the
-- builder outbound or working on it), else the quick-repair candidate
-- (pick_blocker: a damaged or dead take blocker). Returns
-- { mx, my, why = "outbound" | "working" | "dead" | "damaged",
--   man_mx, man_my (his square while outbound) } or nil.
function M.repair_blocker(state, world, info)
  local job = state._bp_job
  if job and job.quick and job.type ~= "farm" and job.phase ~= "returning" then
    local rb = { mx = job.mx, my = job.my,
                 why = (job.phase == "working") and "working" or "outbound" }
    if rb.why == "outbound" and info.man_status == C.LGM_MOVING
       and info.man_x and info.man_y then
      rb.man_mx = bit.rshift(info.man_x, 8)
      rb.man_my = bit.rshift(info.man_y, 8)
    end
    return rb
  end
  local p = M.pick_blocker(world, info)
  if p then
    return { mx = p.mx, my = p.my, why = (p.health or 0) <= 0 and "dead" or "damaged" }
  end
  return nil
end

-- Live non-friendly pills whose range (tiles) reaches the tank, and, when
-- (cmx, cmy) is given, the centre of that tile too.
-- { {id, p, gx, gy}, ... } (pill centres in tiles), in id order.
function M.guns(world, info, range, cmx, cmy)
  return M.guns_at(world, info.tankx / 256.0, info.tanky / 256.0, range, cmx, cmy)
end

-- The same for a tank at (tx, ty) (tiles), for a place the tank will be.
function M.guns_at(world, tx, ty, range, cmx, cmy)
  local out = {}
  local r2 = range * range
  for id, p in pairs(world.pills or {}) do
    if (p.health or 0) > 0 and not p.in_tank and not p.carrier
       and p.owner ~= "friendly" and p.owner ~= "allied" then
      local gx, gy = p.mx + 0.5, p.my + 0.5
      local dx, dy = tx - gx, ty - gy
      local ok = dx * dx + dy * dy <= r2
      if ok and cmx then
        local bx, by = cmx + 0.5 - gx, cmy + 0.5 - gy
        ok = bx * bx + by * by <= r2
      end
      if ok then out[#out + 1] = { id = id, p = p, gx = gx, gy = gy } end
    end
  end
  table.sort(out, function(a, b) return tostring(a.id) < tostring(b.id) end)
  return out
end

-- The swerve points of side `dir` (1 = attack.lua's LEFT sample, the one
-- TURNRIGHT drives to; -1 = RIGHT) for a tank at (tx, ty) swerving off the
-- target pill centre (pcx, pcy), tiles. The tank itself is the wedge's
-- start and is not in the list.
function M.swerve_verts(tx, ty, pcx, pcy, dir)
  local dx, dy = pcx - tx, pcy - ty
  local len = math.sqrt(dx * dx + dy * dy)
  if len <= 0.01 then return {} end
  local ux, uy = dx / len, dy / len
  local nx, ny
  if dir > 0 then nx, ny = -uy, ux else nx, ny = uy, -ux end
  local off = M.SWERVE_SAMPLE_PERP_TILES
  local sx, sy = tx + nx * off, ty + ny * off
  local ex, ey = sx - pcx, sy - pcy
  local el = math.sqrt(ex * ex + ey * ey)
  local ext = C.SWERVE_COVER_EXTEND_TILES or 0.5
  local sx2, sy2 = sx, sy
  if el > 0.01 then sx2, sy2 = sx + ex / el * ext, sy + ey / el * ext end
  local back = C.ATTACK_APPROACH_OFFSET or 2.25
  return { { x = sx2, y = sy2 }, { x = sx - ux * back, y = sy - uy * back } }
end

-- The wedge of a pill at (gx, gy) over a swerve from (tx, ty) through
-- verts: lo, hi (radians from the pill -> tank line; one of them is 0), a0.
function M.fan(gx, gy, tx, ty, verts)
  local a0 = math.atan2(ty - gy, tx - gx)
  local lo, hi = 0, 0
  for i = 1, #verts do
    local a = wrap(math.atan2(verts[i].y - gy, verts[i].x - gx) - a0)
    if a < lo then lo = a end
    if a > hi then hi = a end
  end
  return lo, hi, a0
end

-- Does the wedge (lo, hi) about base angle a0 from (gx, gy) touch tile
-- (mx, my) grown by margin m, within range? Open edges: a square that only
-- touches the line itself is not inside.
function M.fan_hits_square(gx, gy, a0, lo, hi, mx, my, m, range)
  if hi - lo <= 0 then return false end
  local h = 0.5 + m
  local cx, cy = mx + 0.5, my + 0.5
  if math.abs(gx - cx) <= h and math.abs(gy - cy) <= h then return true end
  local nx = math.max(cx - h, math.min(gx, cx + h))
  local ny = math.max(cy - h, math.min(gy, cy + h))
  local ddx, ddy = nx - gx, ny - gy
  if ddx * ddx + ddy * ddy > range * range then return false end
  local slo, shi
  for i = 0, 3 do
    local x = cx + ((i % 2 == 0) and -h or h)
    local y = cy + ((i < 2) and -h or h)
    local a = wrap(math.atan2(y - gy, x - gx) - a0)
    if not slo or a < slo then slo = a end
    if not shi or a > shi then shi = a end
  end
  if shi - slo > math.pi then return false end   -- behind the pill
  return shi > lo and slo < hi
end

-- Is side verts dirty for gun g? Only the blocker square counts. The
-- builder's square does not: the engine kills him only where a shell
-- explodes (lgm.c lgmBlastCatches), never where it only flies over him, and
-- a shell into the swerve side explodes on the tank. (Nov 24: the man's
-- square here made the outbound check abort one tick after every send,
-- because he stands on the tank's square, on the wedge's edge, as he leaves.)
local function side_dirty(g, tx, ty, verts, rb, m, range)
  local lo, hi, a0 = M.fan(g.gx, g.gy, tx, ty, verts)
  return M.fan_hits_square(g.gx, g.gy, a0, lo, hi, rb.mx, rb.my, m, range)
end

-- Rule 1 for one moment: both sides' state for the repair blocker rb, the
-- target pill (pmx, pmy) and the tank where it is now. haz = { left = bool,
-- right = bool } (terrain-blocked) or nil (unknown: not blocked).
-- Returns { dir = the clean unblocked side or nil, why, side = {[1], [-1]}
-- each { dirty, blocked, by = pill id, deg = wedge angle off the target
-- pill's line }, pills = { [id] = { [1] = dirty, [-1] = dirty } }, n_guns }.
function M.side_eval(world, info, rb, pmx, pmy, haz)
  local x = LP.context(info, LPX) or {}
  local range = (x.pill_range or 2048) / 256
  local m = C.REPAIR_FIRE_LANE_MARGIN_TILES or 0
  local tx, ty = info.tankx / 256.0, info.tanky / 256.0
  local pcx, pcy = pmx + 0.5, pmy + 0.5
  local guns = M.guns(world, info, range)
  local rec = { side = {}, pills = {}, n_guns = #guns, mx = rb.mx, my = rb.my }
  for _, d in ipairs({ 1, -1 }) do
    local verts = M.swerve_verts(tx, ty, pcx, pcy, d)
    local s = { dirty = false, verts = verts,
                blocked = haz and ((d > 0) and haz.left or haz.right) or false }
    local lo, hi = M.fan(pcx, pcy, tx, ty, verts)
    s.deg = math.deg(math.max(-lo, hi))
    for gi = 1, #guns do
      local g = guns[gi]
      local dirty = side_dirty(g, tx, ty, verts, rb, m, range)
      rec.pills[g.id] = rec.pills[g.id] or {}
      rec.pills[g.id][d] = dirty
      if dirty and not s.dirty then s.dirty, s.by = true, g.id end
    end
    rec.side[d] = s
  end
  local L, R = rec.side[1], rec.side[-1]
  if #guns == 0 then
    rec.why = "no_guns"
  elseif not L.dirty and not R.dirty then
    -- Both clean: the unblocked one if only one is, else none (cover).
    if L.blocked ~= R.blocked then
      rec.dir = L.blocked and -1 or 1
      rec.why = "clean_unblocked"
    else
      rec.why = L.blocked and "both_blocked" or "both_clean"
    end
  elseif L.dirty and R.dirty then
    rec.why = "both_dirty"
  else
    local c = L.dirty and -1 or 1
    if rec.side[c].blocked then
      rec.why = "clean_side_blocked"
    else
      rec.dir = c
      rec.why = "clean_side"
    end
  end
  return rec
end

-- attack.lua sets this to its side scorer at load:
--   M.side_scores(world, pmx, pmy, tmx, tmy, is_ppt) -> { left_hazard,
--   right_hazard, ... }. nil (attack.lua not loaded) = no terrain known.
M.side_scores = nil

function M.side_hazards(world, info, goal)
  if not M.side_scores then return nil end
  local sc = M.side_scores(world, goal.mx, goal.my, bit.rshift(info.tankx, 8),
                           bit.rshift(info.tanky, 8), goal._is_ppt)
  if not sc then return nil end
  return { left = sc.left_hazard and true or false,
           right = sc.right_hazard and true or false }
end

-- Is the committed side (goal._repair_swerve_dir) still in force? It is
-- from the send tick until the quick repair's job ends.
function M.commit_active(state, goal, now)
  local c = goal and goal._repair_swerve_dir
  if not c then return false end
  if c.tick == now then return true end
  local job = state._bp_job
  return job ~= nil and job.quick == true and job.mx == c.mx and job.my == c.my
end

local function side_txt(s)
  if s.dirty then return "dirty" end
  if s.blocked then return "blocked" end
  return "clean"
end
M.side_txt = side_txt

-- The swerve-start half of Rule 1 (attack.lua enter_swerve). Returns the
-- side to swerve to, or nil to keep old_dir. Logs SWERVE_REPAIR_SIDE.
function M.swerve_repair_side(state, world, info, goal, pmx, pmy, haz, old_dir, now)
  if not C.SWERVE_AVOID_REPAIR_SIDE then return nil end
  local c = goal._repair_swerve_dir
  if c and M.commit_active(state, goal, now) then
    if BRAIN_DEBUG_MODE then
      print2(string.format(
        "SWERVE_REPAIR_SIDE t=%d blocker=(%d,%d) left=%s right=%s chosen=%s why=committed"
        .. " (at send t=%d; cover pick %s)",
        now, c.mx, c.my, c.left or "-", c.right or "-", c.dir > 0 and "L" or "R",
        c.tick, (old_dir or 0) > 0 and "L" or "R"))
    end
    return c.dir
  end
  if c then goal._repair_swerve_dir = nil end
  local rb = M.repair_blocker(state, world, info)
  if not rb then return nil end
  local rec = M.side_eval(world, info, rb, pmx, pmy, haz)
  local chosen = rec.dir or old_dir or 1
  if BRAIN_DEBUG_MODE then
    print2(string.format(
      "SWERVE_REPAIR_SIDE t=%d blocker=(%d,%d) left=%s right=%s chosen=%s why=%s"
      .. " (blocker %s, guns=%d, fan L%.0f/R%.0f deg, dirty by L#%s/R#%s, cover pick %s)",
      now, rb.mx, rb.my, side_txt(rec.side[1]), side_txt(rec.side[-1]),
      chosen > 0 and "L" or "R", rec.dir and rec.why or ("cover:" .. rec.why),
      rb.why, rec.n_guns, rec.side[1].deg, rec.side[-1].deg,
      tostring(rec.side[1].by or "-"), tostring(rec.side[-1].by or "-"),
      (old_dir or 0) > 0 and "L" or "R"))
  end
  return rec.dir
end

-- Where a pill's shell aimed at a tank at (tx, ty) explodes (tiles), and
-- after how many ticks of flight. The engine's three endings (shells.c
-- shellsUpdate / shellsCalcCollision):
--   * the first square on its path that stops it (danger.shell_stops_on: a
--     wall, a half wall, a live pill, a forest or a boat with
--     SHELL_STOPS_ON_FOREST_BOAT), or a square solid_at(mx, my, tick) says
--     will be solid at that flight tick (a wall that will be up by then).
--     The explosion is a hit on that square: only a builder ON it dies.
--   * the tank: the open-ground blast area round the tank.
--   * the end of its range: the open-ground blast area there.
-- The pill's own square never stops it. g = { gx, gy } (pill centre).
-- Returns { t = flight ticks, ex, ey, sq_mx, sq_my (nil unless a square
-- hit), how = "square" | "tank" | "range" }.
function M.shell_end(world, g, tx, ty, range, solid_at)
  local spd = (C.SHELL_SPEED or 32) / 256
  local ax, ay = tx - g.gx, ty - g.gy
  local al = math.sqrt(ax * ax + ay * ay)
  local ux, uy = 0, 0
  if al > 0.01 then ux, uy = ax / al, ay / al end
  local land = math.min(al, range)
  local pmx, pmy = math.floor(g.gx), math.floor(g.gy)
  local k = 1
  while k * spd < land do
    local d = k * spd
    local mx, my = math.floor(g.gx + ux * d), math.floor(g.gy + uy * d)
    if not (mx == pmx and my == pmy)
       and (danger.shell_stops_on(world, mx, my) or (solid_at and solid_at(mx, my, k))) then
      return { t = k, ex = mx + 0.5, ey = my + 0.5, sq_mx = mx, sq_my = my, how = "square" }
    end
    k = k + 1
  end
  return { t = math.max(1, math.floor(land / spd + 0.5)),
           ex = g.gx + ux * land, ey = g.gy + uy * land,
           how = (al <= range) and "tank" or "range" }
end

-- Does an explosion e (M.shell_end) catch a builder at (lx, ly) (tiles)?
-- lgm.c lgmBlastCatches: a hit on a square kills only a builder on it;
-- else the lgm_blast_shape area round the explosion.
function M.blast_catches_man(e, lx, ly)
  if e.sq_mx then
    return math.floor(lx) == e.sq_mx and math.floor(ly) == e.sq_my
  end
  return U.lgm_open_blast_catches((e.ex - lx) * 256, (e.ey - ly) * 256)
end

-- Does the segment (ax, ay)-(bx, by) cross tile (mx, my) grown by m?
local function seg_square(ax, ay, bx, by, mx, my, m)
  local h = 0.5 + m
  local x0, x1 = mx + 0.5 - h, mx + 0.5 + h
  local y0, y1 = my + 0.5 - h, my + 0.5 + h
  local dx, dy = bx - ax, by - ay
  local t0, t1 = 0, 1
  local ps = { -dx, dx, -dy, dy }
  local qs = { ax - x0, x1 - ax, ay - y0, y1 - ay }
  for i = 1, 4 do
    local p, q = ps[i], qs[i]
    if p == 0 then
      if q < 0 then return false end
    else
      local r = q / p
      if p < 0 then
        if r > t1 then return false end
        if r > t0 then t0 = r end
      else
        if r < t0 then return false end
        if r < t1 then t1 = r end
      end
    end
  end
  return true
end

-- Rule 2: the first conflicting predicted shot, or nil.
--   rb        the repair blocker ({mx, my}, man_mx/man_my while out).
--   bx0, by0  where the builder starts (tiles: the tank, or the man now).
--   t0        the tick he leaves (now + 1), or now when he is out.
--   t_arr     the tick he reaches the blocker; the errand ends at
--             t_arr + LGM_BUILD_TIME.
--   k         quick_repair kill_tick record of the take target (T_kill).
--   sd        the swerve side the shots go into once the swerve starts, or nil.
--   swerving  true when that swerve is running now.
--   org       { x, y } (tiles): where that side's swerve starts. The
--             committed side keeps the tank's place at the send, so the
--             send and every outbound check test the same wedge. nil = the
--             tank where it is now.
-- Returns { pill = id, target = bool, fire = tick, at = tick,
-- how = "blocker" | "builder" | "side" } or nil.
function M.fire_conflict(world, info, now, goal, k, rb, bx0, by0, t0, t_arr, sd, swerving, org)
  local x = LP.context(info, LPX) or {}
  local range = (x.pill_range or 2048) / 256
  local m = C.REPAIR_FIRE_LANE_MARGIN_TILES or 0
  local spd = (C.SHELL_SPEED or 32) / 256          -- tiles a tick
  local t_end = t_arr + (C.LGM_BUILD_TIME or 20)
  local tx, ty = info.tankx / 256.0, info.tanky / 256.0
  local guns = M.guns(world, info, range, rb.mx, rb.my)
  local ox, oy = tx, ty
  if org then ox, oy = org.x, org.y end
  local verts = sd and M.swerve_verts(ox, oy, goal.mx + 0.5, goal.my + 0.5, sd) or nil
  local bcx, bcy = rb.mx + 0.5, rb.my + 0.5
  local walk = math.max(1, t_arr - t0)
  local kt = k and k.t
  for gi = 1, #guns do
    local g = guns[gi]
    local is_target = (g.p.mx == goal.mx and g.p.my == goal.my)
    local death = is_target and kt or nil
    local sdirty = verts and side_dirty(g, ox, oy, verts, rb, m, range) or false
    -- A pill whose shots all flow on a clean swerve side does not conflict:
    -- the swerve runs now, or it is a pill that outlives the kill (its
    -- shots after T_kill go into the side).
    local all_side = verts and (swerving or not death)
    if not (all_side and not sdirty) then
      local f, reload = M.pill_next_fire(g.p, info, now, x)
      if reload < 1 then reload = 1 end
      local dbx, dby = bcx - g.gx, bcy - g.gy
      local D = math.sqrt(dbx * dbx + dby * dby)
      local at_d = math.floor(D / spd + 0.5)
      local ax, ay = tx - g.gx, ty - g.gy
      local al = math.sqrt(ax * ax + ay * ay)
      local ux, uy = 0, 0
      if al > 0.01 then ux, uy = ax / al, ay / al end
      local lane = al > 0.01 and seg_square(g.gx, g.gy, g.gx + ux * range,
                                            g.gy + uy * range, rb.mx, rb.my, m)
      while f <= t_end and (not death or f < death) do
        local into_side = verts and (swerving or (kt and f >= kt))
        if into_side then
          if sdirty then
            local at = f + at_d
            if at >= t0 and at <= t_end then
              return { pill = g.id, target = is_target, fire = f, at = at, how = "side" }
            end
          end
        elseif al > 0.01 then
          if lane then
            local at = f + at_d
            if at >= t0 and at <= t_end then
              return { pill = g.id, target = is_target, fire = f, at = at, how = "blocker" }
            end
          end
          -- The builder where the shell explodes. It missed the blocker
          -- (else the check above), so it ends on the tank it is aimed at,
          -- or at the end of its flight when the tank is out of range. The
          -- engine tests the builder only there and then (lgmBlastCatches).
          local land = math.min(al, range)
          local at = f + math.floor(land / spd + 0.5)
          if at >= t0 and at <= t_end then
            local mx_, my_
            if at >= t_arr then
              mx_, my_ = bcx, bcy                -- building on the blocker
            else
              local w = (at - t0) / walk
              mx_ = bx0 + (bcx - bx0) * w
              my_ = by0 + (bcy - by0) * w
            end
            local ex, ey = g.gx + ux * land, g.gy + uy * land
            local dx, dy = (ex - mx_) * 256, (ey - my_) * 256
            -- The margin pulls the explosion that much nearer to him.
            if m > 0 then
              local d = math.sqrt(dx * dx + dy * dy)
              local k2 = d > 0 and math.max(0, d - m * 256) / d or 0
              dx, dy = dx * k2, dy * k2
            end
            if U.lgm_open_blast_catches(dx, dy) then
              return { pill = g.id, target = is_target, fire = f, at = at, how = "builder" }
            end
          end
        end
        f = f + reload
      end
    end
  end
  return nil
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
  -- run_c9: while the gun reloads, the next shot comes R ticks after our
  -- last shot (tank.c: reload counts down once a tick and the gun fires on
  -- the tick it reaches 0), not R ticks from now. A full R from now is only
  -- the answer when the last shot is not known.
  local first = now + 1
  if info.reload then
    local lf = goal._on_target_land_tick == now and goal._last_fire_tick or nil
    if lf then
      first = math.max(now + 1, lf + R)
    else
      first = now + R
    end
  end
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

-- The swerve side Rule 2's shots go into: the committed side, else the
-- side of a swerve that is running. Returns side, swerving.
local function shot_side(state, goal, now)
  local sw = goal.substate == "swerve"
  if M.commit_active(state, goal, now) then
    local c = goal._repair_swerve_dir
    return c.dir, sw, c.ox and { x = c.ox, y = c.oy } or nil
  end
  if sw and goal._swerve_dir then return goal._swerve_dir, true end
  return nil, false
end

-- The last rungs of the send ladder (after tank_clear): Rule 1 picks and
-- commits the clean unblocked swerve side (C.SWERVE_AVOID_REPAIR_SIDE),
-- Rule 2 waits while a predicted pill shot conflicts
-- (C.QUICK_REPAIR_WAIT_ALL_PILLS). Returns "send", "wait:fire_predict" (the
-- take target's shot) or "wait:other_pill".
function M.side_and_fire(state, world, info, now, goal, k, p, qr)
  local rb = { mx = p.mx, my = p.my }
  local rs
  if C.SWERVE_AVOID_REPAIR_SIDE then
    rs = M.side_eval(world, info, rb, goal.mx, goal.my, M.side_hazards(world, info, goal))
    qr.side = rs
  end
  -- The side to commit. A swerve that runs now keeps its own side to its
  -- end (enter_swerve picks a side only when a swerve starts), so its shots
  -- go into that side: commit it when it is clean (its terrain no longer
  -- matters, the tank is on it), else nothing is committed and Rule 2 tests
  -- the shots into the dirty running side. With no swerve running, the one
  -- clean unblocked side.
  local cdir, cwhy = nil, nil
  local swerving = goal.substate == "swerve" and goal._swerve_dir ~= nil
  if rs then
    if swerving then
      local s = rs.side[goal._swerve_dir]
      if s and not s.dirty then cdir, cwhy = goal._swerve_dir, "running_clean" end
    elseif rs.dir then
      cdir, cwhy = rs.dir, rs.why
    end
  end
  qr.side_dir, qr.side_why = cdir, cwhy or (rs and rs.why)
  if C.QUICK_REPAIR_WAIT_ALL_PILLS then
    local sd, sw, org
    if cdir then
      sd, sw = cdir, swerving
      org = { x = info.tankx / 256.0, y = info.tanky / 256.0 }
    else
      sd, sw, org = shot_side(state, goal, now)
    end
    local cf = M.fire_conflict(world, info, now, goal, k, rb,
                               info.tankx / 256.0, info.tanky / 256.0,
                               now + 1, qr.t_repair, sd, sw, org)
    if cf then
      qr.fire = cf
      return cf.target and "wait:fire_predict" or "wait:other_pill"
    end
  end
  if cdir then
    goal._repair_swerve_dir = { dir = cdir, mx = p.mx, my = p.my, tick = now,
                                ox = info.tankx / 256.0, oy = info.tanky / 256.0,
                                left = M.side_txt(rs.side[1]),
                                right = M.side_txt(rs.side[-1]), why = cwhy }
    if BRAIN_DEBUG_MODE and swerving then
      print2(string.format(
        "SWERVE_REPAIR_SIDE t=%d blocker=(%d,%d) left=%s right=%s chosen=%s why=committed"
        .. " (running swerve's side, clean; at send)",
        now, p.mx, p.my, M.side_txt(rs.side[1]), M.side_txt(rs.side[-1]),
        cdir > 0 and "L" or "R"))
    end
  end
  return "send"
end

-- QUICK_REPAIR_WAIT_ALL_PILLS while the builder is out on a quick repair:
-- the fire prediction again every tick, from where he stands, with the
-- committed side. A conflict logs QUICK_REPAIR_ABORT why=pill_fire_predict
-- once per job (job._qr_abort). NOTE: a player brain has no way to call the
-- builder back (the engine queues any new order until he is home; lgmRecall
-- is only a scenario op), so this is a log and a mark on the job.
function M.outbound_check(state, world, info, now, goal, k, qr)
  if not C.QUICK_REPAIR_WAIT_ALL_PILLS then return end
  local job = state._bp_job
  if not (job and job.quick and not job.reached and info.man_status == C.LGM_MOVING
          and info.man_x and info.man_y) then return end
  local rb = { mx = job.mx, my = job.my,
               man_mx = bit.rshift(info.man_x, 8), man_my = bit.rshift(info.man_y, 8) }
  local sd, sw, org = shot_side(state, goal, now)
  local cf = M.fire_conflict(world, info, now, goal, k, rb,
                             info.man_x / 256.0, info.man_y / 256.0,
                             now, math.max(now, job.arrive_tick or now), sd, sw, org)
  qr.outbound = cf
  if cf and not job._qr_abort then
    job._qr_abort = { tick = now, pill = cf.pill, how = cf.how }
    print2(string.format(
      "QUICK_REPAIR_ABORT t=%d pill=(%d,%d) why=pill_fire_predict gun=#%s%s shot at %+dt"
      .. " meets the %s at %+dt (arrive %+dt, side=%s%s) recall=none",
      now, job.mx, job.my, tostring(cf.pill), cf.target and "(target)" or "",
      cf.fire - now, cf.how, cf.at - now, (job.arrive_tick or now) - now,
      sd and (sd > 0 and "L" or "R") or "-", sw and " swerving" or ""))
  end
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

  -- SWERVE_AVOID_REPAIR_SIDE: the committed side lives until its job ends.
  if goal._repair_swerve_dir and not M.commit_active(state, goal, now) then
    goal._repair_swerve_dir = nil
  end
  M.outbound_check(state, world, info, now, goal, k, qr)

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
      local tc = M.tank_clear(world, info, now)
      if tc then
        result = "wait:tank"
        qr.tank = tc
      else
        result = M.side_and_fire(state, world, info, now, goal, k, p, qr)
      end
    end
  end
  qr.result = result
  state._quick_repair = qr

  if BRAIN_DEBUG_MODE then
    -- run_c7: the blocker's hp is in the key, so its death gets a line
    -- (c6 beside_e: it died at 637 and the "wait:early" line of 474 stood).
    local key = string.format("%d:%d:%d:%s:%s", p.mx, p.my, qr.hp, result, tostring(k.src))
    if state._qr_log_key ~= key then
      state._qr_log_key = key
      print2(string.format(
        "QUICK_REPAIR t=%d pill=(%d,%d) hp=%d target=(%d,%d) T_kill=%s src=%s"
        .. " T_safe=%s walk=%s dwell=%d -> %s%s"
        .. " (T_repair=now+1+walk=%s margin=%d hostile_hits=%d inflight=%d/%d%s trees=%d/%d)",
        now, p.mx, p.my, qr.hp, goal.mx, goal.my, tostring(k.t),
        tostring(k.src or k.why or "-"), tostring(t_safe), tostring(qr.walk),
        qr.dwell, result .. M.fire_txt(qr, now),
        qr.death and string.format(" (shell %s at +%dt, %s, rewind=%s)",
          tostring(qr.death.src), qr.death.t or 0, tostring(qr.death.how),
          tostring(qr.death.rewind))
        or qr.tank and (qr.tank.why == "shell"
          and string.format(" (shell %s on the tank at +%dt <= %d)",
                            tostring(qr.tank.src), qr.tank.t,
                            C.QUICK_REPAIR_TANK_CLEAR_TICKS or 0)
          or string.format(" (pill #%s next shot on the tank at +%dt <= %d:"
                           .. " last fire %s, reload %d, flight %d)",
                           tostring(qr.tank.pill), qr.tank.t,
                           C.QUICK_REPAIR_TANK_CLEAR_TICKS or 0,
                           qr.tank.last and string.format("%+dt", qr.tank.last) or "none",
                           qr.tank.reload or 0, qr.tank.flight or 0))
        or "",
        tostring(qr.t_repair), margin, #qr.hits,
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

-- The QUICK_REPAIR line's Rule 1 / Rule 2 part.
function M.fire_txt(qr, now)
  local t = ""
  local f = qr.fire
  if f then
    t = string.format(" (pill #%s%s shot at %+dt meets the %s at %+dt)", tostring(f.pill),
                      f.target and "(target)" or "", f.fire - now, f.how, f.at - now)
  end
  local rs = qr.side
  if rs then
    t = t .. string.format(" side=%s(%s L=%s R=%s)",
                           qr.side_dir and (qr.side_dir > 0 and "L" or "R") or "-",
                           tostring(qr.side_why or rs.why),
                           M.side_txt(rs.side[1]), M.side_txt(rs.side[-1]))
  end
  return t
end

-- ── Overlays (BrainTest category "Quick repair") ──────────────────────────
local RESULT_RGB = {
  send          = { 80, 255, 80 },
  ["wait:early"] = { 255, 220, 60 },
  ["wait:death"] = { 255, 70, 70 },
  ["wait:tank"]  = { 255, 120, 70 },
  ["wait:nokill"] = { 170, 170, 170 },
  ["wait:wood"]  = { 200, 140, 60 },
  ["wait:man"]   = { 120, 180, 255 },
  ["wait:reach"] = { 170, 170, 170 },
  ["wait:fire_predict"] = { 255, 90, 160 },
  ["wait:other_pill"]   = { 255, 90, 160 },
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
    tail = string.format(" (shell %s at +%dt, %s, rewind=%s)", tostring(qr.death.src),
                         qr.death.t or 0, tostring(qr.death.how),
                         tostring(qr.death.rewind))
  elseif qr.result == "wait:tank" and qr.tank then
    tail = string.format(" (%s on the tank +%dt)", qr.tank.why, qr.tank.t)
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
