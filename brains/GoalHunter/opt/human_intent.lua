-- =========================================================================
-- GoalHunter/human_intent.lua -- HUMAN PILL YIELD (C.HUMAN_PILL_YIELD).
--
-- Bots split dead pills with each other through broadcast claims (ally_state
-- goal/target, sync_ally_claimed_rejects). A human team-mate broadcasts
-- nothing, so a bot took the dead pill the human was driving to. This module
-- reads the human's intent from what the bot can see, and gives the pill a
-- synthetic claim. goals.sync_ally_claimed_rejects turns the claim into the
-- reject "human_claim" on the pool 4 (capture_pill) and pool 5 (repair_pill)
-- row of that pill, and goal selection picks the next goal. The bot yields
-- even when it is much closer (Andrew, Nov 19), and it does not escort.
--
-- Inputs, all visible to this bot's player (no-cheat rule):
--   * allied human tanks in info.objects. The engine puts a tank there only
--     when it is inside this bot's own view rect or the 15x15 view of one of
--     the team's pills (brain_data.c, playersGetBrainTanksInRect). A human
--     out of every view is not seen, so it cannot claim.
--   * shells in info.objects; each carries its firing player (ob.owner).
--   * world.pills (the brain's own pill knowledge) for the dead pills.
--   * the EVENT_TANK_KILLED message (state.tank_dead_at) for a death.
--   * hostile tanks and hostile LGMs in info.objects for the enemy rule.
--
-- Per human (one claim at most, the nearest dead pill that qualifies):
--   claim at once   the human is within HUMAN_YIELD_CLOSE_TILES of the pill,
--                   or one of the human's shells was near the pill (within
--                   ORDER_HUMAN_SHOT_NEAR_TILES) in the last
--                   ORDER_HUMAN_SHOOTING_WINDOW_TICKS;
--   claim on hold   the pill is within HUMAN_YIELD_RADIUS_TILES, the human
--                   moves, closes on it over the track window, and its
--                   heading is within HUMAN_YIELD_CONE_DEG of the pill, for
--                   HUMAN_YIELD_HOLD_TICKS without a break.
--   release         turned away (cos < HUMAN_YIELD_AWAY_COS) for
--                   HUMAN_YIELD_AWAY_TICKS; not seen for HUMAN_YIELD_AWAY_TICKS;
--                   stopped farther than HUMAN_YIELD_STOP_TILES for
--                   HUMAN_YIELD_STOP_TICKS; died; the pill is not dead; held
--                   HUMAN_YIELD_LIMIT_TICKS unless within
--                   HUMAN_YIELD_LIMIT_NEAR_TILES. A released pill stays
--                   claimed HUMAN_YIELD_POST_TICKS more.
-- Per pill: the closest claiming human owns it.
-- No yield (M.yield_claim returns nil):
--   * a hostile tank or hostile LGM this bot sees is closer to the pill than
--     HUMAN_YIELD_ENEMY_RATIO x the owner's distance;
--   * the bot holds a person's order (a bot command or a ping) on that pill.
-- =========================================================================
local C  = require("constants")
local U  = require("util")
local print2 = require("print2")

local M = {}

local function hi_state(state)
  local h = state._hi
  if not h then
    h = { hum = {}, claims = {}, post = {}, logged = {} }
    state._hi = h
  end
  return h
end

-- A dead pill the bot knows of: on the map, health 0.
local function dead_pill(world, pid)
  local p = world and world.pills and world.pills[pid]
  if not p or p.in_tank or not p.mx then return nil end
  if (p.health or 0) ~= 0 then return nil end
  return p
end

local function tiles(ax, ay, bx, by)   -- world units in, tiles out (euclid)
  local dx, dy = (ax - bx) / 256.0, (ay - by) / 256.0
  return math.sqrt(dx * dx + dy * dy)
end

local function release(H, hs, pn, now, why)
  local cl = hs.claim
  if not cl then return end
  H.post[cl.pid] = { pn = pn, until_t = now + (C.HUMAN_YIELD_POST_TICKS or 50) }
  hs.claim = nil
  hs.cand = nil
end

-- Heading of the human over the track window: unit vector (ux, uy) and the
-- oldest sample, or nil while it has not moved.
local function heading(hs)
  local b = hs.buf
  local n = #b
  if n < 2 then return nil end
  local o, l = b[1], b[n]
  local dx, dy = l.x - o.x, l.y - o.y
  local len = math.sqrt(dx * dx + dy * dy)
  if len <= 0 then return nil end
  return dx / len, dy / len, o
end

-- Per-tick update. Call once per think, after world.update.
function M.update(state, world, info, now)
  if not C.HUMAN_PILL_YIELD then
    if state._hi then state._hi = nil end
    return
  end
  local me     = info.player_number
  local allies = info.allies or 0
  if me then allies = bit.band(allies, bit.bnot(bit.lshift(1, me))) end
  local humans = bit.band(allies, bit.bnot(info.player_bots or 0))
  if humans == 0 then
    if state._hi then state._hi = nil end
    return
  end
  local H = hi_state(state)
  local OT, OS = _G.OBJECT_TANK, _G.OBJECT_SHOT
  local OB, OH = _G.OBJECT_BUILDMAN, _G.OBJECT_HOSTILE or 0
  local track   = C.HUMAN_YIELD_TRACK_TICKS or 50
  local R       = C.HUMAN_YIELD_RADIUS_TILES or 12
  local cone    = math.cos(math.rad(C.HUMAN_YIELD_CONE_DEG or 35))
  local close   = C.HUMAN_YIELD_CLOSE_TILES or 2.5
  local hold    = C.HUMAN_YIELD_HOLD_TICKS or 35
  local awaycos = C.HUMAN_YIELD_AWAY_COS or 0.3
  local awayt   = C.HUMAN_YIELD_AWAY_TICKS or 50
  local stopd   = C.HUMAN_YIELD_STOP_TILES or 4
  local stopt   = C.HUMAN_YIELD_STOP_TICKS or 150
  local limt    = C.HUMAN_YIELD_LIMIT_TICKS or 750
  local limd    = C.HUMAN_YIELD_LIMIT_NEAR_TILES or 2
  local shot_w  = (C.ORDER_HUMAN_SHOT_NEAR_TILES or 1.5) * 256.0
  local shot_win = C.ORDER_HUMAN_SHOOTING_WINDOW_TICKS or 150

  -- 1. What the bot sees this tick: human tanks, their shells, enemies.
  local seen = {}
  local foes = nil
  local shells = nil
  for _, ob in ipairs(info.objects or {}) do
    local t = ob.type
    if t == OT then
      local pn = ob.idnum or -1
      if bit.band(ob.info or 0, OH) ~= 0 then
        foes = foes or {}
        foes[#foes + 1] = ob
      elseif pn >= 0 and pn < 32 and bit.band(humans, bit.lshift(1, pn)) ~= 0 then
        seen[pn] = ob
      end
    elseif t == OB then
      if bit.band(ob.info or 0, OH) ~= 0 then
        foes = foes or {}
        foes[#foes + 1] = ob
      end
    elseif t == OS then
      local pn = ob.owner
      if pn and pn >= 0 and pn < 32 and bit.band(humans, bit.lshift(1, pn)) ~= 0 then
        shells = shells or {}
        shells[#shells + 1] = ob
      end
    end
  end
  H.foes = foes
  H.pills = world.pills

  -- 2. Shells of a human near a pill: the human's shot at that pill.
  if shells then
    for _, s in ipairs(shells) do
      local hs = H.hum[s.owner]
      if not hs then
        hs = { buf = {}, shot = {} }
        H.hum[s.owner] = hs
      end
      for pid, p in pairs(world.pills or {}) do
        if not p.in_tank and p.mx
           and U.wdist(s.x or 0, s.y or 0, U.m2w(p.mx), U.m2w(p.my)) <= shot_w then
          hs.shot[pid] = now
        end
      end
    end
  end

  -- 3. Each human: track, then claim or release.
  local dead_at = state.tank_dead_at
  for pn = 0, 31 do
    if bit.band(humans, bit.lshift(1, pn)) ~= 0 then
      local ob = seen[pn]
      local hs = H.hum[pn]
      if ob and not hs then
        hs = { buf = {}, shot = {} }
        H.hum[pn] = hs
      end
      if hs then
        local cl = hs.claim
        if cl and dead_at and dead_at[pn] and dead_at[pn] >= cl.since then
          release(H, hs, pn, now, "died")
          hs.buf = {}
        end
        if not ob then
          hs.unseen = hs.unseen or now
          hs.buf = {}
          hs.cand = nil
          if hs.claim and now - hs.unseen >= awayt then
            release(H, hs, pn, now, "unseen")
          end
        else
          hs.unseen = nil
          local b = hs.buf
          b[#b + 1] = { t = now, x = ob.x or 0, y = ob.y or 0 }
          while #b > 0 and now - b[1].t > track do table.remove(b, 1) end
          hs.x, hs.y = ob.x or 0, ob.y or 0
          local moving = (ob.speed or 0) > 0
          local ux, uy, old = heading(hs)
          hs.ux, hs.uy, hs.moving = ux, uy, moving

          -- Candidates: the nearest instant pill, and the nearest cone pill.
          local inst, inst_d, inst_why = nil, math.huge, nil
          local conep, cone_d = nil, math.huge
          for pid, p in pairs(world.pills or {}) do
            if not p.in_tank and p.mx and (p.health or 0) == 0 then
              local pwx, pwy = U.m2w(p.mx), U.m2w(p.my)
              local d = tiles(hs.x, hs.y, pwx, pwy)
              if d <= R then
                local why = nil
                if d <= close then why = "close"
                elseif hs.shot[pid] and now - hs.shot[pid] <= shot_win then why = "shot" end
                if why and d < inst_d then inst, inst_d, inst_why = pid, d, why end
                if not why and moving and ux and d < cone_d then
                  local tx, ty = (pwx - hs.x) / 256.0, (pwy - hs.y) / 256.0
                  local c = (tx * ux + ty * uy) / d
                  local d_old = tiles(old.x, old.y, pwx, pwy)
                  if c >= cone and d < d_old then conep, cone_d = pid, d end
                end
              end
            end
          end

          cl = hs.claim
          if cl then
            local p = dead_pill(world, cl.pid)
            if not p then
              release(H, hs, pn, now, "not_dead")
            else
              local pwx, pwy = U.m2w(p.mx), U.m2w(p.my)
              local d = tiles(hs.x, hs.y, pwx, pwy)
              cl.d = d
              -- Turned away: heading off the pill for awayt.
              local c = nil
              if moving and ux and d > 0 then
                c = (((pwx - hs.x) / 256.0) * ux + ((pwy - hs.y) / 256.0) * uy) / d
              end
              if c and c < awaycos then
                cl.away_since = cl.away_since or now
              else
                cl.away_since = nil
              end
              if not moving and d > stopd then
                cl.stop_since = cl.stop_since or now
              else
                cl.stop_since = nil
              end
              if cl.away_since and now - cl.away_since >= awayt then
                release(H, hs, pn, now, "turned_away")
              elseif cl.stop_since and now - cl.stop_since >= stopt then
                release(H, hs, pn, now, "stopped")
              elseif now - cl.since >= limt and d > limd then
                release(H, hs, pn, now, "time_limit")
              elseif inst and inst ~= cl.pid then
                release(H, hs, pn, now, "switched")
              end
            end
          end
          -- No new claim for HUMAN_YIELD_POST_TICKS after a death: the dead
          -- tank can stay in view at its death spot on the kill tick.
          local just_died = dead_at and dead_at[pn]
                            and now - dead_at[pn] < (C.HUMAN_YIELD_POST_TICKS or 50)
          if not hs.claim and not just_died then
            local pid, why = nil, nil
            if inst then
              pid, why = inst, inst_why
            elseif conep then
              if hs.cand and hs.cand.pid == conep then
                if now - hs.cand.since >= hold then pid, why = conep, "cone" end
              else
                hs.cand = { pid = conep, since = now }
              end
            else
              hs.cand = nil
            end
            if pid then
              local p = world.pills[pid]
              hs.claim = { pid = pid, since = now, why = why,
                           d = tiles(hs.x, hs.y, U.m2w(p.mx), U.m2w(p.my)) }
              hs.cand = nil
              H.post[pid] = nil
            end
          end
          -- Old shot stamps.
          for pid, t in pairs(hs.shot) do
            if now - t > shot_win then hs.shot[pid] = nil end
          end
        end
      end
    end
  end
  -- Humans that left the team: drop.
  for pn, hs in pairs(H.hum) do
    if bit.band(humans, bit.lshift(1, pn)) == 0 then
      release(H, hs, pn, now, "left_team")
      H.hum[pn] = nil
    end
  end

  -- 4. Per pill: the closest claiming human owns it.
  local claims = {}
  for pn, hs in pairs(H.hum) do
    local cl = hs.claim
    if cl then
      local cur = claims[cl.pid]
      if not cur or cl.d < cur.d then
        claims[cl.pid] = { pn = pn, d = cl.d, since = cl.since, why = cl.why }
      end
    end
  end
  for pid, pc in pairs(H.post) do
    if now >= pc.until_t or not dead_pill(world, pid) then
      H.post[pid] = nil
    elseif not claims[pid] then
      claims[pid] = { pn = pc.pn, post = true, until_t = pc.until_t }
    end
  end
  H.claims = claims
end

-- The claim that makes this bot yield pill `pid`, or nil. Applies the enemy
-- rule and the order rule; the claim table itself does not.
function M.yield_claim(state, pid, now)
  if not C.HUMAN_PILL_YIELD then return nil end
  local H = state._hi
  local cl = H and H.claims and H.claims[pid]
  if not cl then return nil end
  local ord = state._order
  if ord and ord.tkind == "pill" and ord.tid == pid then return nil end
  local p = H.pills and H.pills[pid]
  if p and p.mx and H.foes and not cl.post then
    local hs = H.hum[cl.pn]
    if hs and hs.x then
      local pwx, pwy = U.m2w(p.mx), U.m2w(p.my)
      local dh = tiles(hs.x, hs.y, pwx, pwy)
      local ratio = C.HUMAN_YIELD_ENEMY_RATIO or 0.7
      for _, f in ipairs(H.foes) do
        if tiles(f.x or 0, f.y or 0, pwx, pwy) < ratio * dh then return nil end
      end
    end
  end
  return cl
end

-- The bot's current goal is a pill a human now claims: drop the goal so this
-- tick's replan picks the next one. Returns true when it dropped.
function M.drop_goal(state, now)
  local g = state.goal
  if not g or not g.target_id then return false end
  if g.kind ~= "capture_pill" and g.kind ~= "repair_pill" then return false end
  local cl = M.yield_claim(state, g.target_id, now)
  if not cl then return false end
  state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
  state._force_replan_reason = "human_claim"
  return true
end

-- Viz (BRAIN_DEBUG_MODE): each tracked human's cone, its candidate pill with
-- the hold timer, and each claimed pill with the owner and the claim time.
function M.draw(state, world, now)
  local H = state._hi
  if not H then return end
  local viz = require("viz")
  local ID = "human_pill_claim"
  if not (viz.is_on and viz.is_on(ID)) then return end
  local R = C.HUMAN_YIELD_RADIUS_TILES or 12
  local half = math.rad(C.HUMAN_YIELD_CONE_DEG or 35)
  for pn, hs in pairs(H.hum) do
    if hs.x and not hs.unseen then
      local hx, hy = hs.x / 256.0, hs.y / 256.0
      if hs.ux then
        local a = math.atan2(hs.uy, hs.ux)
        for _, s in ipairs({ -half, half }) do
        end
      end
      local cd = hs.cand
      local p = cd and world.pills and world.pills[cd.pid]
      if p and p.mx then
      end
    end
  end
  for pid, cl in pairs(H.claims or {}) do
    local p = world.pills and world.pills[pid]
    if p and p.mx then
      local yields = M.yield_claim(state, pid, now) ~= nil
      local r, g, b = 255, 90, 200
      if not yields then r, g, b = 160, 160, 160 end
      local s
      if cl.post then
        s = string.format("human p%d released, %.1fs left", cl.pn, (cl.until_t - now) / 50)
      else
        s = string.format("human p%d claim %s %.1fs", cl.pn, cl.why or "?", (now - cl.since) / 50)
      end
      if not yields then s = s .. " (no yield)" end
    end
  end
end

return M
