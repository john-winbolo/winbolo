local bit = require('bitcompat')
-- =========================================================================
-- GoalHunter/builder_pool.lua — LGM side-quests as a parallel track
--
-- A SECOND ARBITER FOR A SECOND RESOURCE. goals.lua's pool decides what the
-- TANK does. This one decides what the MAN does. They are not the same
-- question and, before this module, only one of them was ever asked: builder
-- dispatch was slaved to the tank's goal, so a correct tank goal could sit on
-- an idle LGM for hundreds of ticks while a four-tree errand went undone.
--
-- The incident (20260901_160325_1_par2 bot2, t=21071-21661): bot2's own
-- blocker p15 at (125,114) dies to return fire at 21471. For 190 ticks the LGM
-- sits IN THE TANK with 13 trees, six tiles from the corpse, while the tank
-- (correctly) finishes its take. At 21661 a 1.6 bot drives over p15 and owns
-- it. Four trees would have made it a live friendly pill -- undriveable, race
-- over. The tank goal was right; the architecture lost the pill.
--
-- POOL, NOT QUEUE. Scored candidates compete every tick like the goal pool;
-- the winner (if any) gets the one LGM. Nothing is remembered between ticks
-- except the ACTIVE JOB (the man is already walking) and the claim we advert
-- to allies.
--
-- WHAT IS NOT A POOL ROW: walls, placements, sea legs. Those are goal-owned
-- work with no existence outside their goal. builder.set_mode still turns them
-- into a MODE, and a goal-tied mode pre-empts the pool outright (mode_owned).
-- The pool never negotiates with a pill take's wall building; it loses to it
-- by construction.
--
-- Three job types, and rebuild outranks farm ALWAYS (a corpse has a clock; a
-- forest does not), which falls out of the value constants rather than being
-- special-cased:
--   rebuild  a 0-HP friendly pill inside the leash -> 4 trees make it live
--   topup    a damaged-but-alive friendly pill inside the leash
--   farm     opportunistic wood -- up to FOUR rows since 2026-09-06, the
--            nearest forest in each of four 90-degree wedges (N/E/S/W), so a
--            near forest the man cannot straight-line to loses to a clear one
--
-- Print2 contract (one line per tick for the verdict, one per event):
--   BUILDER_POOL t=.. owner=.. elig=..      the per-tick verdict + counts
--   BP_DISPATCH  t=.. job=.. target=..      the man just left on a side-quest
--   BP_DENY      t=.. job=.. reason=..      a row that could have won, did not
--   BP_DONE      t=.. job=.. outcome=..     he came home and the job took
--   BP_ABORT     t=.. job=.. why=..         he came home and it did not
--   BP_SEED_DROP t=.. target=.. reason=..   a feeder named a pill that is gone
--   BP_ALLY_CAPTURE t=.. pill#.. BLOCKED/RELEASED  an ally is driving over to
--                                           SCOOP this corpse; edge-triggered
--                                           on (tile, ally), never per tick
-- ...plus REPAIR_SPLIT (goals.lua), which is the tank goal standing down
-- because the man can already walk the job from where the tank is.
-- =========================================================================

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local ally_state = require("ally_state")
local print2 = require("print2")
local viz    = require("viz")

local M = {}

-- Deterministic type ordering for tie-breaks (and the claim wire format).
local TYPE_RANK = { rebuild = 1, topup = 2, farm = 3 }
M.TYPE_RANK = TYPE_RANK

-- Farm wedges (BUILDER_POOL_FARM_SECTORS = 4). Fixed indices, because the
-- emission order of the rows is these indices and the pool's tie-breaks have
-- to be reproducible from a log line: N, E, S, W, always.
local WEDGE_N, WEDGE_E, WEDGE_S, WEDGE_W = 1, 2, 3, 4
local WEDGE_NAME = { "N", "E", "S", "W" }

-- -------------------------------------------------------------------------
-- repair_leash: how far a REBUILD/TOPUP target may sit from the tank.
--
-- Two leashes now, and every reader has to ask for the right one: repair rows
-- reach BUILDER_POOL_REPAIR_LEASH (11 tiles, Manhattan) under the linear
-- formula, the farm row keeps BUILDER_POOL_LEASH (8). Gated on
-- BUILDER_POOL_REPAIR_LINEAR so `preset=keel` puts BOTH back to 8 with one
-- entry. Exported because goals.lua's REPAIR_SPLIT prints the number it hands
-- off at, and a panel that names a radius the code does not use is worse than
-- no panel at all.
-- -------------------------------------------------------------------------
function M.repair_leash()
  if C.BUILDER_POOL_REPAIR_LINEAR then
    return C.BUILDER_POOL_REPAIR_LEASH or 11
  end
  return C.BUILDER_POOL_LEASH or 8
end

-- -------------------------------------------------------------------------
-- front_distance: tiles from (mx,my) to the nearest front-line tile.
--
-- "Front distance" is the plan's CLOCK: a dead pill at the contact line is
-- ticking (the enemy is right there and will drive over it); one deep in our
-- rear can wait all game. The influence map already knows where the line is --
-- PP.on_front_line mirrors brainPathfinderFindFrontLine -- so this is a plain
-- chebyshev ring scan outward, stopping at the first hit.
--
-- Returns the distance in tiles, or FRONT_MAX when no front tile is within
-- that radius ("the clock has stopped"). Memoised per (tile, tick): the same
-- pill is asked about by discovery, by scoring and by the panel.
-- -------------------------------------------------------------------------
function M.front_distance(state, mx, my)
  local cap = C.BUILDER_POOL_FRONT_MAX_TILES or 12
  local now = state.tick or 0
  local memo = state._bp_front
  if not memo or memo.tick ~= now then
    memo = { tick = now, v = {} }
    state._bp_front = memo
  end
  local key = my * 256 + mx
  local hit = memo.v[key]
  if hit ~= nil then return hit end
  local found = cap
  for r = 0, cap do
    local any = false
    if r == 0 then
      any = PP.on_front_line(mx, my)
    else
      -- Chebyshev ring: the four edges of the r-box, corners included once.
      for d = -r, r do
        if PP.on_front_line(mx + d, my - r) or PP.on_front_line(mx + d, my + r)
           or PP.on_front_line(mx - r, my + d) or PP.on_front_line(mx + r, my + d) then
          any = true
          break
        end
      end
    end
    if any then found = r; break end
  end
  memo.v[key] = found
  return found
end
-- -------------------------------------------------------------------------
-- route_forecast: WHERE THE TANK WILL BE, walked along its own planned route.
--
-- Not a straight line from heading and speed. A tank on a goal is following a
-- route the navigator has already computed and steering is already driving --
-- state.pf.path_chain, the committed chain from cpf.trace_path /
-- cpf.dijkstra_trace_path (steering.lua ~line 614), a FLAT array
-- {x1,y1,x2,y2,...} from the search source to the destination. steer.steer
-- runs at init.lua:6685 and builder_pool.update at init.lua:7650, SAME tick,
-- so the chain read here is this tick's. Nothing is recomputed: no A*, no
-- Dijkstra, no second trace.
--
-- Walked once per tick and memoised on `state`, as a cumulative TIME profile:
-- t[i] = brain ticks to reach waypoint i from the tank. Every row then reads
-- its own horizon off the same profile instead of re-walking the chain.
--
-- MEMO KEY: the tick AND the chain table itself. lgm_trip is also asked from
-- goals.lua (builder_can_repair) and from the repair feeder, and the GOAL POOL
-- RUNS BEFORE STEERING (init.lua: goals, then steer.steer at 6685, then
-- bpool.update at 7650). Keying on the tick alone would let the first caller
-- of the tick freeze LAST tick's route into the memo and hand it to the pool
-- after steering had already replaced it. steering assigns a fresh table on
-- every completed search (cpf.trace_path returns a new one), so comparing the
-- table identity rebuilds exactly when the route really changed and at no
-- other time -- every caller still sees one answer per route, and it is
-- always the freshest route that caller could have seen.
--
-- SPEED, from the engine and not guessed:
--   * bolo_map.h:71-83 MAP_SPEED_T* is the tank's per-terrain speed CAP
--     (road 16, grass 12, forest 6, swamp/crater/rubble/river 3, refbase and
--     boat 16, building/halfbuilding/pillbox 0). C.MAP_SPEED mirrors it.
--   * tank.c:2028 displace = mapGetSpeed(...) and the tank's `speed` field
--     converges on it (TANK_ACCELERATE_RATE up, TANK_TERRAIN_DECEL_RATE down).
--   * tank.c:1614-1621 each tankUpdate does residualSpeed += speed and moves
--     utilCalcDistance(angle, residualSpeed) WORLD UNITS -- so `speed` is WU
--     per tankUpdate.
--   * server_sim_tick.c:319 tankUpdate runs on the keys half-tick and :766
--     lgmUpdate on the game half-tick, one of each per 20 ms frame, and
--     server_lifecycle.c:583 / luabrainshandler.c:1216 run ONE brain think per
--     frame. So one brain tick = one tankUpdate = one LGM walk-sim tick, and
--     C.MAP_SPEED is already WU per BRAIN tick. No conversion.
-- A tile step is 256 WU orthogonally and 256 x sqrt(2) diagonally, so the
-- cost of entering tile i is (step WU) / MAP_SPEED[terrain of tile i].
--
-- Returns the memo, or nil when there is no usable route:
--   { n, x[], y[], t[] }          -- t[1] = 0, at the tank's own tile
-- "Usable" means the chain has at least two waypoints AND one of them is
-- within BUILDER_POOL_RETURN_PREDICT_ROUTE_SNAP tiles of the tank. A chain
-- left over from a goal the tank has since abandoned is not a plan, and
-- predicting along it would be worse than not predicting at all.
-- -------------------------------------------------------------------------
local function route_forecast(state, info)
  local now = state.tick or 0
  local chain = state.pf and state.pf.path_chain
  local memo = state._bp_route
  if memo and memo.tick == now and memo.chain == chain then
    return memo.ok and memo or nil
  end
  memo = { tick = now, chain = chain, ok = false }
  state._bp_route = memo

  local nwp = chain and math.floor(#chain / 2) or 0
  if nwp < 2 then memo.why = "no_route"; return nil end

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  -- Snap to the chain: its own first point is where the SEARCH started, which
  -- the tank has since driven away from. Nearest waypoint by squared tile
  -- distance, the same walk steering.lua does when A* restarts (~line 626).
  -- Deterministic: strictly-less keeps the FIRST (lowest index) of any tie, so
  -- the forecast never depends on scan order.
  local best_i, best_d = nil, math.huge
  for i = 1, nwp do
    local dx = chain[2 * i - 1] - tmx
    local dy = chain[2 * i] - tmy
    local d = dx * dx + dy * dy
    if d < best_d then best_d, best_i = d, i end
  end
  local snap = C.BUILDER_POOL_RETURN_PREDICT_ROUTE_SNAP or 2
  if best_d > snap * snap then
    memo.why = "route_stale"
    return nil
  end
  if best_i >= nwp then memo.why = "route_ended"; return nil end

  -- Walk forward, accumulating brain ticks, until the horizon cap is reached
  -- or the route runs out. Entry 1 of the profile is the tank's own tile at
  -- t = 0, so a horizon of 0 predicts "here" and every fallback is the same
  -- answer as a zero-length walk.
  local cap = C.BUILDER_POOL_RETURN_PREDICT_MAX_TICKS or 400
  local xs, ys, ts = { tmx }, { tmy }, { 0 }
  local n = 1
  local acc = 0
  local px, py = tmx, tmy
  for i = best_i + 1, nwp do
    local nx, ny = chain[2 * i - 1], chain[2 * i]
    local dx, dy = nx - px, ny - py
    if dx ~= 0 or dy ~= 0 then
      local spd = C.MAP_SPEED[U.ttype(nx, ny)] or 12
      if spd <= 0 then break end                 -- route into a wall: stop
      local wu = 256 * math.sqrt(dx * dx + dy * dy)
      acc = acc + wu / spd
      if acc > cap then break end
      n = n + 1
      xs[n], ys[n], ts[n] = nx, ny, acc
      px, py = nx, ny
    end
  end
  if n < 2 then memo.why = "route_no_progress"; return nil end
  memo.ok, memo.n, memo.x, memo.y, memo.t = true, n, xs, ys, ts
  memo.why = "route"
  return memo
end
M.route_forecast = route_forecast

-- The forecast as a printable "(x,y)@ticks,..." list, for BP_PRED. Called only
-- from inside a print2 argument, so lua_strip removes every call and opt/
-- never runs it.
local function route_str(fc)
  if not (fc and fc.ok) then return "none" end
  local wp = {}
  for i = 1, fc.n do
    wp[i] = string.format("(%d,%d)@%.0f", fc.x[i], fc.y[i], fc.t[i])
  end
  return table.concat(wp, ",")
end

-- -------------------------------------------------------------------------
-- lgm_trip: outbound walk ticks (real sim), round trip, and reachability.
--
-- The wall-shield / repair dispatch walk-time math: the C tick-by-tick LGM sim
-- with the DESTINATION blessed, so a live pill or base AT the target does not
-- self-block (the man works ON that square). Pills and bases in the PATH still
-- block -- a friendly pill between us and the spot really does stop him.
--
-- Round trip = outbound + LGM_BUILD_TIME + return. Returns nil when the
-- OUTBOUND leg is unreachable (a job the man cannot get to is not a job).
--
-- THE RETURN LEG (BUILDER_POOL_RETURN_PREDICT, 2026-09-06).
--
-- The old return leg was `out` again: a mirror image of the walk out, which is
-- the walk home only if the tank waits on the spot. It does not. By the time
-- the man has walked out and spent LGM_BUILD_TIME on the tile, a tank that is
-- driving has moved -- and the mirrored leg hides exactly the fact that
-- matters, that a forest AHEAD of the tank is a shorter errand than an equally
-- distant one BEHIND it.
--
-- So: walk the tank forward along ITS OWN PLANNED ROUTE (route_forecast, above
-- -- state.pf.path_chain, at the per-terrain speeds the engine caps it to) for
-- out + LGM_BUILD_TIME brain ticks, capped at
-- BUILDER_POOL_RETURN_PREDICT_MAX_TICKS, and walk the man BACK to the tile it
-- lands on. A straight line from heading and speed is deliberately NOT used:
-- the tank turns, and the route is the turn it has already committed to.
--
-- The blessed square for the return leg is the TARGET tile -- the man starts
-- standing on it, and for a rebuild/topup row that tile is a pillbox, whose
-- man-speed is 0 (brain_pathfinder.c lgm_man_speed[12]); without the bless the
-- sim would refuse to move him on tick 1 and every repair row in the pool
-- would read `unreachable`. lgmTravelTicksCore clears the bless the moment he
-- steps off it, so nothing else in the walk is softened.
--
-- FALLBACKS, in order, each naming itself on the row:
--   no usable route (an idle or stationary tank, or a chain left over from an
--     abandoned goal)                                   -> the tank's own tile
--   the predicted tile is off the map, or the man cannot stand on it (water,
--     building, live pill -- the walk sim's own speed table)
--                                                       -> the tank's own tile
--   the return walk sim reports stuck / unreachable     -> the tank's own tile
-- Falling back to the tank's tile makes the return leg the outbound leg
-- mirrored, i.e. exactly the old number -- so a fallback is never a refusal,
-- only a loss of information, and the row still competes.
--
-- Returns out_ticks, trip, det -- det carrying every number the chips print:
--   { build, back, pred_mx, pred_my, src = "route"/"same"/"off", horizon,
--     route_i, route_n, route_t, fallback = <why> or nil }
-- With the flag off, det.back == out_ticks and det.pred_mx is nil, so
-- trip is exactly 2 x out + LGM_BUILD_TIME, as it always was.
-- -------------------------------------------------------------------------

-- Man-walkable terrain, mirroring brain_pathfinder.c lgm_man_speed[] > 0:
-- building(0), river(1), halfbuilding(8), deepsea(10) and pillbox(12) are the
-- five the man cannot stand on. Kept as a table rather than a speed lookup
-- because the only question here is walkable / not.
local MAN_WALKABLE = {
  [C.T_SWAMP] = true, [C.T_CRATER] = true, [C.T_ROAD] = true,
  [C.T_FOREST] = true, [C.T_RUBBLE] = true, [C.T_GRASS] = true,
  [C.T_BOAT] = true, [C.T_REFBASE] = true, [C.T_UNKNOWN] = true,
}

function M.lgm_trip(state, info, mx, my)
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local build_t = C.LGM_BUILD_TIME or 20
  local near = C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16
  local out
  if math.abs(mx - tmx) + math.abs(my - tmy) <= 1 then
    out = near
  else
    out = cpf_lgm_travel_ticks_map(tmx, tmy, mx, my, mx, my,
                                   C.BUILDER_POOL_LGM_MAX_TICKS or 2000,
                                   C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
    if out == nil or out < 0 then return nil end
    if out == 0 then out = near end
  end

  if not C.BUILDER_POOL_RETURN_PREDICT then
    return out, 2 * out + build_t,
           { build = build_t, back = out, src = "off" }
  end

  local horizon = out + build_t
  local hmax = C.BUILDER_POOL_RETURN_PREDICT_MAX_TICKS or 400
  if horizon > hmax then horizon = hmax end
  local det = { build = build_t, back = out, src = "same", horizon = horizon }

  local pmx, pmy = tmx, tmy
  local fc = route_forecast(state, info)
  if not fc then
    det.fallback = (state._bp_route and state._bp_route.why) or "no_route"
  else
    -- The furthest waypoint reachable inside the horizon. The profile rises
    -- monotonically in t, so a forward scan is exact; it is at most
    -- MAX_TICKS / (256/16) ~ 25 entries long by construction.
    local k = 1
    for i = 2, fc.n do
      if fc.t[i] <= horizon then k = i else break end
    end
    det.route_i, det.route_n, det.route_t = k, fc.n, fc.t[k]
    local cx, cy = fc.x[k], fc.y[k]
    -- The walkability tests run in the SAME ORDER on every path through here,
    -- k == 1 included. U.ttype is a terrain DETECTOR (it primes terrain_prev
    -- and can push a tile into changes.terrain), so skipping the call on a
    -- branch would give that branch a different brain state -- the 2026-09-06
    -- identity bug in miniature.
    local walkable = U.in_map(cx, cy) and MAN_WALKABLE[U.ttype(cx, cy)]
    if not walkable then
      det.fallback = U.in_map(cx, cy) and "unwalkable" or "off_map"
    elseif k == 1 then
      -- The route exists, but the tank cannot clear its own tile inside the
      -- horizon (slow ground, or a very short errand). The same answer as no
      -- prediction, and worth naming rather than reading back as "no route".
      det.fallback = "horizon_too_short"
    else
      pmx, pmy, det.src = cx, cy, "route"
    end
  end

  if pmx == tmx and pmy == tmy then
    -- The tank is predicted to still be on its own tile (or we fell back to
    -- it): the return leg IS the outbound leg reversed, which is the old
    -- number, and there is no second walk sim to pay for.
    det.src = "same"
    det.back = out
    return out, out + build_t + out, det
  end

  local back
  if math.abs(mx - pmx) + math.abs(my - pmy) <= 1 then
    back = near
  else
    back = cpf_lgm_travel_ticks_map(mx, my, pmx, pmy, mx, my,
                                    C.BUILDER_POOL_LGM_MAX_TICKS or 2000,
                                    C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
    if back == nil or back < 0 then
      -- He cannot get from the job to where the tank is heading. That is not
      -- a reason to refuse the job -- the outbound leg is the one that has to
      -- be real -- so fall back to the mirrored leg and say so.
      det.fallback = "back_unreachable"
      det.src = "same"
      det.back = out
      return out, out + build_t + out, det
    end
    if back == 0 then back = near end
  end
  det.back = back
  det.pred_mx, det.pred_my = pmx, pmy
  return out, out + build_t + back, det
end

-- -------------------------------------------------------------------------
-- Ally claims.
--
-- Wire format on the existing /info extra channel, beside `lgmd`:
--     bpj = "TTXXYYCCCCEEEE"   (hex)
--       TT   job type    (BUILDER_POOL_CLAIM_TYPES)
--       XX   target mx
--       YY   target my
--       CCCC claim tick, low 16 bits
--       EEEE remaining ETA ticks at send time
--     bpj = "-"                no claim (explicit, because /info extra MERGES
--                              into receiver slots and a dropped key lingers)
--
-- Arbitration is deterministic and uses NO wall clock: an earlier claim tick
-- wins; a same-tick race breaks to the LOWER player number, the same rule the
-- blitz commander and the standoff-spot picker already use. Claim tick is sent
-- modulo 65536, so comparisons are done on the wrapped value with a half-range
-- guard -- 65536 ticks is 22 minutes of game, far longer than
-- BUILDER_POOL_CLAIM_MAX_AGE, so a wrap can never be mistaken for "older".
-- -------------------------------------------------------------------------
local function tick16(t) return bit.band(t or 0, 0xFFFF) end

-- Ordered "is their claim older than ours" on wrapped ticks.
local function claim_earlier(theirs, ours)
  local d = bit.band(ours - theirs, 0xFFFF)
  return d > 0 and d < 32768
end

function M.claim_advert(state, now)
  local job = state._bp_job
  if not job then return "-" end
  local left = (job.claim_eta_tick or now) - now
  if left < 0 then left = 0 elseif left > 65535 then left = 65535 end
  return string.format("%02X%02X%02X%04X%04X",
    bit.band(TYPE_RANK[job.type] or 0, 0xFF),
    bit.band(job.mx or 0, 0xFF), bit.band(job.my or 0, 0xFF),
    tick16(job.claim_tick), left)
end

-- Parse one ally's bpj advert. Returns mx, my, type_rank, claim_tick16, eta.
local function parse_claim(s)
  if not s or s == "-" or #s < 14 then return nil end
  local tt = tonumber(string.sub(s, 1, 2), 16)
  local mx = tonumber(string.sub(s, 3, 4), 16)
  local my = tonumber(string.sub(s, 5, 6), 16)
  local ct = tonumber(string.sub(s, 7, 10), 16)
  local et = tonumber(string.sub(s, 11, 14), 16)
  if not (tt and mx and my and ct and et) then return nil end
  return mx, my, tt, ct, et
end

-- Is an ALLY claiming this tile, and does their claim beat ours?
-- `our_tick` is the tick WE would claim at (now, for a fresh dispatch; the
-- job's own claim_tick when defending a claim we already hold).
-- Returns nil when the tile is free to us, else { pn, eta, ctick }.
function M.ally_claim_on(state, info, mx, my, now, our_tick)
  local best = nil
  for pn, slot in ally_state.iter_active(now, C.BUILDER_POOL_CLAIM_MAX_AGE or 1750) do
    if pn ~= info.player_number then
      local h = slot.info
      local cmx, cmy, _ctt, cct, cet = parse_claim(h and h.bpj)
      if cmx and cmx == mx and cmy == my then
        -- Earlier claim wins; same tick breaks to the LOWER player number.
        local theirs_wins
        if cct == tick16(our_tick) then
          theirs_wins = pn < (info.player_number or 0)
        else
          theirs_wins = claim_earlier(cct, our_tick)
        end
        if theirs_wins then
          local age = now - (slot.last_tick or now)
          local eta = cet - age
          if eta < 0 then eta = 0 end
          if not best or pn < best.pn then
            best = { pn = pn, eta = eta, ctick = cct }
          end
        end
      end
    end
  end
  return best
end

-- -------------------------------------------------------------------------
-- Ally CAPTURE guard.
--
-- The bpj claim above arbitrates who REPAIRS a pill. It has nothing to say
-- about the other way a corpse can already be spoken for: an ally driving over
-- to SCOOP it. Rebuilding that corpse turns it into a live friendly pill --
-- undriveable -- so the ally's trip and the kill that made the corpse are both
-- thrown away, and our four trees bought the team nothing.
--
-- The advert is the ordinary /info state slate, which allies send on every goal
-- change (plus a 30 s heartbeat):
--     goal=capture_pill|pill_place   target=<pill id>
-- and, when the goal carries no object id, `mx`/`my` instead -- init.lua only
-- spends the bytes on the tile when nothing else identifies the target (the
-- _need_mxmy gate). So: match on the id when the advert has one, on the tile
-- when it does not. A repositioning capture carries BOTH, and either match is
-- the same pill.
--
-- ENDING THE BLOCK. set_info replaces an ally's slate wholesale, so the slot
-- always holds its LATEST advert and nothing else. A move-on is therefore
-- simply "the slot no longer names this pill", which ends the block on the tick
-- the new advert lands, at any age. The TTL is only for the ally that stops
-- talking at all (dead, kicked, removed): see the note on
-- BUILDER_POOL_ALLY_CAPTURE_TTL in constants.lua for why it is measured on
-- last_tick rather than state_tick.
--
-- Returns nil when nobody is coming for the tile, else { pn, age } for the
-- LOWEST-numbered ally that is (deterministic, like every other tie-break here).
-- -------------------------------------------------------------------------
function M.ally_capture_on(state, info, mx, my, pill_id, now)
  if not C.BUILDER_POOL_ALLY_CAPTURE_GUARD then return nil end
  local kinds = C.BUILDER_POOL_ALLY_CAPTURE_GOALS
                or { capture_pill = true, pill_place = true }
  local ttl = C.BUILDER_POOL_ALLY_CAPTURE_TTL or 175
  local self_pn = info and info.player_number
  local best = nil
  -- No max_age on the iterator: the TTL below is the age test, and it is a
  -- tighter one than SQUAD_ALLY_MAX_AGE. iter_active still short-circuits when
  -- no slot has ever been heard from.
  for pn, slot in ally_state.iter_active(now, nil) do
    if pn ~= self_pn and (not best or pn < best.pn) then
      local h = slot.info
      if h and kinds[h.goal] then
        local matched = false
        local tid = tonumber(h.target)
        if tid and pill_id and tid == pill_id then
          matched = true
        else
          local amx, amy = tonumber(h.mx), tonumber(h.my)
          if amx and amy and amx == mx and amy == my then matched = true end
        end
        if matched then
          local age = now - (slot.last_tick or now)
          if age <= ttl then best = { pn = pn, age = age } end
        end
      end
    end
  end
  return best
end

-- One debug line per (pill, ally) TRANSITION, not per tick: the block itself
-- lives on the row's reject string (and therefore on the panel and BP_DENY),
-- and a 350-tick block would otherwise be 350 identical lines. Keyed on the
-- tile and the ally, so a hand-off from one ally to another re-prints.
-- The WHOLE body sits inside `if BRAIN_DEBUG_MODE`, not behind an early
-- `return`, so lua_strip's --strip-block leaves opt/ with an empty stub rather
-- than a hollowed-out if/else full of dead locals (which is what the early
-- return produced -- see the "lua_strip eats else branches" note in the
-- release checklist). Nothing outside this function reads _bp_acap_seen.
local function log_ally_capture(state, now, mx, my, id, ac)
  if BRAIN_DEBUG_MODE then
    state._bp_acap_seen = state._bp_acap_seen or {}
    local k = my * 256 + mx
    local want = ac and ac.pn or nil
    local prev = state._bp_acap_seen[k]
    if prev ~= want then
      state._bp_acap_seen[k] = want
      local ttl = C.BUILDER_POOL_ALLY_CAPTURE_TTL or 175
      if ac then
        print2(string.format(
          "BP_ALLY_CAPTURE t=%d pill#%s@(%d,%d) BLOCKED by p%d (advert %dt old,"
          .. " ttl %dt) -- rebuilding it would make their scoop impossible",
          now, tostring(id), mx, my, ac.pn, ac.age, ttl))
      else
        -- The two ways a block ends look identical on the row and are NOT the
        -- same event, so the release line has to say which one happened:
        -- `silent` is how long the ex-blocker has been quiet (>= ttl means the
        -- expiry fired) and `their_goal` is what its slate says NOW (anything
        -- but capture_pill/pill_place on this pill means it moved on, whatever
        -- its age).
        local slot = prev and ally_state.get(prev)
        print2(string.format(
          "BP_ALLY_CAPTURE t=%d pill#%s@(%d,%d) RELEASED (was p%s) silent=%dt"
          .. " ttl=%dt their_goal=%s their_target=%s",
          now, tostring(id), mx, my, tostring(prev),
          slot and (now - (slot.last_tick or now)) or -1, ttl,
          prev and ally_state.get_key(prev, "goal") or "-",
          prev and ally_state.get_key(prev, "target") or "-"))
      end
    end
  end
end

-- -------------------------------------------------------------------------
-- Tree reserve: never spend below what the active/imminent goal needs.
--
-- Four parts, summed:
--   base   TREE_RESERVE, the standing float every other spender respects;
--   pills  ONE placement's worth (PILL_PLACE_TREE_COST), and only while a pill
--          is actually aboard -- see below;
--   goal   b.need_trees, what set_mode declared for THIS goal (the take's wall
--          shields, the placement's 4, the sea plan's 21);
--   sea    the LIVE sea plan's trees_need, which stays spoken for even while
--          another goal briefly wins the tank pool.
--
-- WHY THE PILL COMPONENT IS FLAT, NOT 4 x CARRIED.
-- It used to be PILL_PLACE_TREE_COST per carried pill, uncapped, on the
-- road_tree_reserve argument that an errand must never eat the wood needed to
-- deploy EVERY pill in the tank. That is the wrong horizon for the tank's own
-- wood: getting ONE pill out is the priority, the next one triggers its own
-- gather, and ambient farming usually covers it. Uncapped, it starved the tank
-- of its own jobs -- 20260903_105448 bot2 stood beside three damaged friendly
-- pills for 20 minutes with 21 trees while the reserve read
-- base 4 + pills 16 + sea 21 = 41 and refused a ONE-tree repair.
--
-- This is NOT builder.road_tree_reserve, deliberately, and the difference is
-- the point: that one adds "trees to repair the worst-damaged nearby friendly
-- pill", because its job is to stop a LUXURY ROAD eating repair wood. Reusing
-- it here would reserve a repair's wood against that very repair -- the pool's
-- top-up row would be refused for lack of the wood it is holding back for
-- itself. (Requiring builder.lua would also be a cycle: goals.lua requires
-- builder, builder requires this module.)
--
-- Returns reserve, base, pills, goal, sea.
-- -------------------------------------------------------------------------
function M.tree_reserve(state, info, b)
  local base  = C.TREE_RESERVE or 4
  local pills = ((info.carried_pills or 0) > 0)
                and (C.PILL_PLACE_TREE_COST or 4) or 0
  local goal_need = (b and b.need_trees) or 0
  local sea = 0
  local sg = state._sea_live and state._sea_live.sea
  if sg and not sg.done then sea = sg.trees_need or 0 end
  local extra = C.BUILDER_POOL_TREE_RESERVE_EXTRA or 0
  return base + pills + goal_need + sea + extra, base, pills, goal_need, sea
end

-- -------------------------------------------------------------------------
-- Eligibility: the POOL-WIDE half.
--
-- Everything here is true or false for the whole tick regardless of which
-- candidate is asking, so it is computed once. The per-candidate half
-- (leash, trees, ally claim, path safety, reserve-vs-trip) lives in score_row
-- because each of those needs the row's own numbers.
--
-- The two halves are reported SEPARATELY on the detail table (d.mode_ok /
-- d.mode_reason and d.fire_ok / d.fire_reason) as well as combined, because a
-- seeded job waives exactly one of them: the tank's own repair goal is allowed
-- to own the man (that is what seeding means), but nothing is allowed to walk
-- him out through a live barrage. Collapsing them into one flag waived both,
-- and a defend->repair handoff would have sent him while shells were landing
-- on the tank he was stepping out of.
--
-- Returns ok(bool), reason(string or nil), and a detail table for the panel.
-- Reasons, in the order they are tested:
--   mode_owned (<mode>)          a goal has spoken for the man
--   fire_exchange:<substate>     shells are flying; his risky moments are the
--                                two ends of the trip, and both are here
--   under_fire(<age>t)           the TANK took a hit / has one inbound within
--                                BUILDER_POOL_UNDER_FIRE_TICKS
-- -------------------------------------------------------------------------
function M.eligibility(state, world, info, now)
  local b = state.builder
  local goal = state.goal or {}
  local mode = (b and b.mode) or "none"
  local sub  = goal.substate
  local d = { mode = mode, substate = sub, goal = goal.kind or "none" }

  -- 1. Mode. Idle-ish modes are free. "suppressed" is the interesting one: it
  --    covers both "we are in a fight" and "we are driving to a fight", and
  --    only the substate can tell them apart. Everything else (gather,
  --    wall_shield, place_pill, sea_*, base_shield) is a goal-tied WORKING
  --    mode -- the man is already spoken for.
  d.mode_ok, d.mode_reason = true, nil
  local idle = (C.BUILDER_POOL_IDLE_MODES or {})[mode]
  if not idle then
    if mode ~= "suppressed" then
      d.mode_ok, d.mode_reason = false, string.format("mode_owned (%s)", mode)
    else
      local cls = sub and (C.BUILDER_POOL_SUBSTATE_CLASS or {})[sub] or nil
      d.class = cls
      if cls == "fire" then
        d.mode_ok, d.mode_reason = false, string.format("fire_exchange:%s", sub)
      elseif cls ~= "travel" then
        -- No substate at all: a defender parked and watching is the plan's
        -- "defend-watch" travel class. A defender with goal.repair was NOT --
        -- that one FEEDS the pool (seed_job) rather than competing with it.
        --
        -- ...which read the pool-WIDE gate off the seed, and that is the
        -- 2026-09-06 bug: the seeded row waives this gate for itself, so the
        -- only rows the exclusion ever stopped were the row's COMPETITORS.
        -- (t=67922: a 1-hp seeded top-up made the pool `mode_owned
        -- (suppressed/defend_pill)`, and a 5-hp repair ten tiles off never got
        -- to bid.) Under BUILDER_POOL_SEEDED_COMPETES the mode gate therefore
        -- reads exactly as it would have if the goal had not seeded at all.
        local travel_goal = (C.BUILDER_POOL_TRAVEL_GOALS or {})[goal.kind]
                            and (C.BUILDER_POOL_SEEDED_COMPETES
                                 or not goal.repair)
        if not travel_goal then
          d.mode_ok = false
          d.mode_reason = string.format("mode_owned (%s%s)", mode,
            sub and ("/" .. sub) or (goal.kind and ("/" .. goal.kind) or ""))
        else
          d.class = "travel"
        end
      end
    end
  end

  -- 2. Under fire. NOT perc.under_fire (a single-tick "the danger field here
  --    is non-zero"), which is both too eager and too blind: it is true all
  --    through a quiet standoff and false in the gap between two shells that
  --    are both aimed at us. danger.tank_fire_age is the sustained clock --
  --    armour actually dropped, or a hostile shell's closest approach lands
  --    inside SWERVE_HIT_RADIUS_WU of us.
  local age, why = danger.tank_fire_age(state, now)
  d.fire_age = age
  d.fire_why = why
  local quiet = C.BUILDER_POOL_UNDER_FIRE_TICKS or 100
  d.fire_ok, d.fire_reason = true, nil
  if age ~= nil and age < quiet then
    d.fire_ok, d.fire_reason = false, string.format("under_fire(%dt)", age)
  end

  if not d.mode_ok then return false, d.mode_reason, d end
  if not d.fire_ok then return false, d.fire_reason, d end
  return true, nil, d
end

-- -------------------------------------------------------------------------
-- Discovery.
--
-- Dead and damaged friendly OR ALLIED pills within the leash, plus up to FOUR
-- opportunistic farm tiles -- the nearest forest in each 90-degree wedge
-- (BUILDER_POOL_FARM_SECTORS; see the block at the bottom of this function).
-- The dead-pill test mirrors filter_repair_pill's
-- REPAIR_DEAD_FILTER discovery -- 0 HP, on the ground, not blocked, not the
-- tile we are capturing / repositioning -- because the two must agree about
-- which corpses are worth wood. It differs on ONE point, deliberately:
-- filter_repair_pill refuses to rebuild a corpse capture_pill could just pick
-- up (rebuilding makes it un-grabbable and wastes the kill). The pool does
-- not, because the pool exists for the case where the tank is NOT going to go
-- and get it -- that is the whole incident. A corpse the tank has actually
-- committed to collecting is still refused, by the our_target guard below --
-- and one an ALLY has committed to collecting by the ally_capture guard in
-- score_row (2026-09-05: our_target only ever looked at OUR OWN goal, so we
-- happily rebuilt a corpse a teammate was two seconds from scooping).
--
-- Pills between LEASH and 2xLEASH are collected too, as out_of_leash REJECT
-- rows: the panel should show that we can SEE the job and say why the man is
-- not going (the tank goal repair_pill is what relocates for those).
-- -------------------------------------------------------------------------
local function pill_blocked(state, p)
  if state.blocked then
    local bk = U.mkey(p.mx, p.my)
    if state.blocked[bk] and (state.tick or 0) < state.blocked[bk] then return "blocked" end
  end
  if state._repos_guard then
    local gu = state._repos_guard[p.my * 256 + p.mx]
    if gu and (state.tick or 0) < gu then return "repos" end
  end
  -- Take-blocker guard, but ALIVE pills only. _in_use means "the team declared
  -- this pill a blocker for a pill take", and topping up a healthy blocker
  -- mid-take is a trip the take did not ask for. A DEAD one is the opposite
  -- case and is the incident this whole module exists for: bot2's own blocker
  -- p15 died to return fire at t=21471, and four trees would have made it a
  -- live friendly pill again -- restoring the shield the take is relying on
  -- AND denying the corpse to the enemy driver who took it 190 ticks later.
  if p._in_use and (p.health or 0) > 0 then return "take_blocker" end
  if p._friendly_shot_tick
     and ((state.tick or 0) - p._friendly_shot_tick)
         < (C.REPAIR_FRIENDLY_FIRE_REJECT_TICKS or 400) then
    return "friendly_fire"
  end
  local g = state.goal
  if g and (g.kind == "capture_pill" or g.kind == "pill_place")
     and g.mx == p.mx and g.my == p.my then
    return "our_target"
  end
  return nil
end

function M.discover(state, world, info)
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local leash = C.BUILDER_POOL_LEASH or 8      -- the FARM row's reach
  local rleash = M.repair_leash()              -- rebuild/topup reach (11)
  local out = {}
  local maxhp = C.PILLS_MAX_HEALTH or 15
  for pid, p in pairs(world.pills or {}) do
    -- ALLIED counts, not just friendly. The engine's pillsRepairPos has no
    -- ownership test at all -- wood plus a man standing on the tile is the
    -- whole rule -- and an ally's chewed-up pill on our side of the line
    -- denies the enemy exactly as much ground as one of ours. It is also the
    -- premise of the plan's arbitration section: "five allies must not all
    -- repair the same pill" only arises because an allied pill is a candidate
    -- for every one of them. The bpj claim is what keeps that from happening.
    if (p.owner == "friendly" or p.owner == "allied")
       and not p.in_tank and not p.carrier then
      local d = U.mdist(tmx, tmy, p.mx, p.my)
      if d <= rleash * 2 then
        local hp = p.health or 0
        local kind, need
        if hp <= 0 then
          kind, need = "rebuild", C.BUILDER_POOL_TREES_REBUILD or 4
        elseif (maxhp - hp) >= (C.BUILDER_POOL_TOPUP_MIN_MISSING or 4) then
          kind, need = "topup", math.max(C.BUILDER_POOL_TREES_TOPUP or 1,
                        math.ceil((maxhp - hp) / (C.PILL_REPAIR_AMOUNT or 4)))
        end
        if kind then
          local blk = pill_blocked(state, p)
          out[#out + 1] = {
            type = kind, id = pid, mx = p.mx, my = p.my,
            dist = d, hp = hp, missing = maxhp - hp, own = p.owner,
            trees_need = need,
            leash = rleash,                   -- the reach THIS row was measured against
            hard = blk,                       -- discovery-level refusal, shown as REJECT
            out_of_leash = (d > rleash) or nil,
          }
        end
      end
    end
  end
  -- FARM ROWS: the nearest forest in each of four 90-degree wedges centred on
  -- N, E, S and W (BUILDER_POOL_FARM_SECTORS = 4), so up to four rows. They
  -- compete on the ordinary farm score -- no new term -- and the point is the
  -- TRIP: the engine's LGM does not pathfind, it walks a straight line and
  -- gets stuck, so the nearest forest is regularly one the man cannot reach
  -- (or scrapes to slowly) while a clear one a tile further out in another
  -- direction was never offered at all. Four directions, and let the trip cost
  -- decide.
  --
  -- WEDGE BOUNDARIES are the 45-degree diagonals: |dx| > |dy| is E or W,
  -- |dy| > |dx| is N or S. The diagonal itself (|dx| == |dy|, the tank's own
  -- tile included) goes to the VERTICAL wedge -- S when dy > 0, N otherwise --
  -- which is arbitrary but fixed, and fixed is the whole requirement: the same
  -- tile must land in the same wedge on every tick of every run.
  --
  -- BUILDER_POOL_FARM_SECTORS = 1 is the pre-2026-09-06 single row, and is
  -- byte-identical to it on purpose: the scan order (dy outer, dx inner, both
  -- ascending), the U.ttype call on every tile of the square (which is a
  -- terrain-change DETECTOR, so the set of tiles it touches is part of the
  -- brain's state), the distance metric and the tile-key tie-break are all
  -- untouched -- with one bucket every tile lands in it and the same forest
  -- wins.
  --
  -- Still gated on TREE_OPPORTUNISTIC_MAX: at 20 trees or more there are no
  -- farm rows at all, four wedges or one.
  if (info.trees or 0) < (C.TREE_OPPORTUNISTIC_MAX or 20) then
    local nw = (C.BUILDER_POOL_FARM_SECTORS == 4) and 4 or 1
    local best_d = { math.huge, math.huge, math.huge, math.huge }
    local best_x = { nil, nil, nil, nil }
    local best_y = { nil, nil, nil, nil }
    for dy = -leash, leash do
      for dx = -leash, leash do
        local fx, fy = tmx + dx, tmy + dy
        if U.in_map(fx, fy) and U.ttype(fx, fy) == C.T_FOREST then
          local d = U.mdist(tmx, tmy, fx, fy)
          local w = 1
          if nw > 1 then
            local ax = dx < 0 and -dx or dx
            local ay = dy < 0 and -dy or dy
            if ax > ay then       w = (dx > 0) and WEDGE_E or WEDGE_W
            else                  w = (dy > 0) and WEDGE_S or WEDGE_N end
          end
          -- Deterministic: nearest wins, ties by tile key.
          if d < best_d[w] or (d == best_d[w] and best_y[w]
                               and (fy * 256 + fx)
                                   < (best_y[w] * 256 + best_x[w])) then
            best_d[w], best_x[w], best_y[w] = d, fx, fy
          end
        end
      end
    end
    for w = 1, nw do
      if best_x[w] then
        out[#out + 1] = { type = "farm", id = -(best_y[w] * 256 + best_x[w]),
                          mx = best_x[w], my = best_y[w], dist = best_d[w],
                          leash = leash,
                          wedge = (nw > 1) and WEDGE_NAME[w] or "all",
                          trees_need = C.BUILDER_POOL_TREES_FARM or 0 }
      end
    end
  end
  return out
end

-- -------------------------------------------------------------------------
-- Scoring, and the per-candidate half of the eligibility stack.
--
-- TWO formulas now, chosen by BUILDER_POOL_REPAIR_LINEAR (see the long note in
-- constants.lua). The FARM row is unaffected by the flag and always uses the
-- old one.
--
--   repair, linear (rebuild/topup, the default since 2026-09-05):
--     score = REPAIR_HP_W x missing_hp - REPAIR_TRIP_W x round_trip_ticks
--     Damage and walking time, nothing else: no base constant, no front clock,
--     no threat term. missing_hp is 15 for a corpse, so the row runs
--     120 (a 4-hp top-up) .. 450 (a rebuild).
--
--   everything else (farm always; repair rows under preset=keel):
--     value = BASE[type] (+ TOPUP_PER_HP x missing | + FARM_URGENCY) (+ front clock)
--     cost  = TRIP_W x round_trip_ticks + DANGER_W x threat_at_target
--     score = value - cost
--
-- The front clock is deliberately absent from FARM: a forest is not going
-- anywhere and nobody can steal it, which is exactly why rebuild outranks farm
-- always -- the 200-vs-15 base gap is wider than any trip term inside the
-- leash can close. (Under the linear formula the same thing is true with more
-- room: 450 against a farm ceiling of 159.)
--
-- Leaves every chip the pool-grid row prints on the row itself; M.row_formula
-- assembles them into the short||long pair on the panel's cold path, so every
-- number on the row stays reproducible from the chips on that row.
-- -------------------------------------------------------------------------
-- The three legs of the trip, onto the row, so out{} / build{} / back{} /
-- pred{} are readable straight back off it by score_terms, row_formula and the
-- BP_* print lines without any of them re-deriving anything. One copy, called
-- from both formula branches, because a leg that appeared on one and not the
-- other would make half the pool's rows un-hand-checkable.
function M.set_trip_legs(row, out_ticks, det)
  if not det then
    row.build_ticks, row.back_ticks = nil, nil
    row.pred_mx, row.pred_my, row.pred_src = nil, nil, nil
    row.pred_why, row.pred_horizon = nil, nil
    row.pred_route_i, row.pred_route_n, row.pred_route_t = nil, nil, nil
    return
  end
  row.build_ticks  = det.build
  row.back_ticks   = det.back
  row.pred_mx      = det.pred_mx
  row.pred_my      = det.pred_my
  row.pred_src     = det.src
  row.pred_why     = det.fallback
  row.pred_horizon = det.horizon
  row.pred_route_i = det.route_i
  row.pred_route_n = det.route_n
  row.pred_route_t = det.route_t
end

-- -------------------------------------------------------------------------
-- goal_weight / apply_goal_weight — BUILDER_POOL_GOAL_PILL_BONUS.
--
-- The row whose pill IS the tank goal's target gets its finished score
-- multiplied by the bonus (1.2 by default; 1.0 under preset=keel, where the
-- same pill was first by SEEDING instead). Matched on the goal's target_id,
-- which is the pill id in world.pills and therefore the same number discovery
-- puts on row.id -- never on the tile, because a hold tile beside the pill is
-- a defend goal's own mx/my and would match nothing.
--
-- Applied AFTER the linear/legacy arithmetic and BEFORE the MIN_SCORE bar and
-- the ordering, so it is simply part of the row's score everywhere downstream.
-- The pre-bonus number is kept on row.raw_score because the printed chips have
-- to let a reader recompute the final one (M.score_terms).
--
-- The multiply is deliberate on negative rows too: 1.2 x "not worth the walk"
-- is further from the bar, not closer to it.
--
-- A HUMAN'S PING (BUILDER_POOL_PING_PILL_BONUS, 2.0): a bot that took a
-- human's defend ping order on one of our pills (state._repair_ping, set and
-- ended in orders.lua) weights that pill the same way, whatever its goal is.  When
-- both apply the LARGER one is used, never the product.  The second return
-- value names which one it was ("goal" / "ping") for the goal_w segment.
function M.goal_weight(state, row)
  if row.type == "farm" then return 1.0, nil end
  local w, src = 1.0, nil
  local gb = C.BUILDER_POOL_GOAL_PILL_BONUS or 1.0
  local g = state.goal
  if gb ~= 1.0 and g and g.target_id
     and (g.kind == "defend_pill" or g.kind == "repair_pill")
     and row.id == g.target_id then
    w, src = gb, "goal"
  end
  local pb = C.BUILDER_POOL_PING_PILL_BONUS or 1.0
  local rp = state._repair_ping
  if pb ~= 1.0 and rp and rp.tid == row.id and pb > w then
    w, src = pb, "ping"
  end
  return w, src
end

local function apply_goal_weight(state, row)
  local w, src = M.goal_weight(state, row)
  row.goal_w = w
  row.goal_src = src
  -- The ping's sender and end tick, for the goal_w segment (row_formula has
  -- no state to read them from).
  local rp = (src == "ping") and state._repair_ping or nil
  row.ping_by    = rp and rp.sender or nil
  row.ping_until = rp and rp.until_tick or nil
  -- Never on the no-route sentinel: -1e9 is an ordering device, not a score,
  -- and scaling it would print arithmetic nobody can check.
  if w ~= 1.0 and row.trip then
    row.raw_score = row.score
    row.score = row.score * w
    -- Which goal claimed it, for the panel's goal_w segment. A plain field,
    -- not a debug-only one: the cold-path formula builder reads it back.
    row.goal_kind = state.goal and state.goal.kind or nil
  else
    row.raw_score, row.goal_kind = nil, nil
  end
end

function M.score_row(state, world, info, now, row, ctx)
  local FMAX = C.BUILDER_POOL_FRONT_MAX_TILES or 12
  -- front_dist is still MEASURED for every row: BP_DISPATCH prints it and the
  -- panel shows it, and "how close to the line was this job" stays a useful
  -- thing to read back off a recording even when it no longer scores.
  local fd = M.front_distance(state, row.mx, row.my)
  row.front_dist = fd
  local linear = (C.BUILDER_POOL_REPAIR_LINEAR and row.type ~= "farm") or false
  row.linear = linear or nil
  row.leash = row.leash or (linear and M.repair_leash() or (C.BUILDER_POOL_LEASH or 8))

  if linear then
    -- Damage x weight, and nothing else on the value side.
    local hp_w = C.BUILDER_POOL_REPAIR_HP_W or 30
    local missing = row.missing or 0
    row.v_hp_w = hp_w
    row.value  = hp_w * missing
    row.v_base, row.v_hp, row.v_front = 0, row.value, 0

    local out_ticks, trip, det = M.lgm_trip(state, info, row.mx, row.my)
    row.out_ticks, row.trip = out_ticks, trip
    M.set_trip_legs(row, out_ticks, det)
    -- threat.at is still SAMPLED (the panel prints it, and a reader asking
    -- "was it dangerous?" should be able to see) but it is not in the score.
    row.danger = threat.at(row.mx, row.my) or 0
    if trip then
      row.c_trip   = (C.BUILDER_POOL_REPAIR_TRIP_W or 0.25) * trip
      row.c_danger = 0
      row.score    = row.value - row.c_trip
    else
      row.c_trip, row.c_danger, row.score = 0, 0, -1e9
    end
    apply_goal_weight(state, row)
    return M.gate_row(state, world, info, now, row, ctx)
  end

  local front_term = 0
  if row.type ~= "farm" then
    front_term = (C.BUILDER_POOL_FRONT_URGENCY or 120)
                 * math.max(0, (FMAX - fd)) / FMAX
  end
  local base
  local hp_term = 0
  if row.type == "rebuild" then
    base = C.BUILDER_POOL_VALUE_REBUILD or 200
  elseif row.type == "topup" then
    base = C.BUILDER_POOL_VALUE_TOPUP or 60
    hp_term = (C.BUILDER_POOL_TOPUP_PER_HP or 6) * (row.missing or 0)
  else
    base = C.BUILDER_POOL_VALUE_FARM or 15
    -- Short of wood is itself the reason to send him. Capped by construction
    -- below VALUE_REBUILD so a corpse always outranks a forest (see the
    -- BUILDER_POOL_FARM_* note in constants.lua). Carried on the hp_term slot
    -- so the printed chips still add up to `value`.
    local low = C.BUILDER_POOL_FARM_LOW_TREES or 12
    hp_term = (C.BUILDER_POOL_FARM_URGENCY or 12)
              * math.max(0, low - (info.trees or 0))
  end
  row.value = base + hp_term + front_term
  row.v_base, row.v_hp, row.v_front = base, hp_term, front_term

  local out_ticks, trip, det = M.lgm_trip(state, info, row.mx, row.my)
  row.out_ticks, row.trip = out_ticks, trip
  M.set_trip_legs(row, out_ticks, det)
  local dgr = threat.at(row.mx, row.my) or 0
  row.danger = dgr
  if trip then
    row.c_trip   = (C.BUILDER_POOL_TRIP_W or 0.5) * trip
    row.c_danger = (C.BUILDER_POOL_DANGER_W or 1.5) * dgr
    row.score = row.value - row.c_trip - row.c_danger
  else
    row.c_trip, row.c_danger, row.score = 0, 0, -1e9
  end

  apply_goal_weight(state, row)
  return M.gate_row(state, world, info, now, row, ctx)
end

-- -------------------------------------------------------------------------
-- The per-candidate half of the eligibility stack, shared by both formulas.
--
-- Split out of score_row when the linear repair formula arrived: the two
-- formulas differ ONLY in how the number is arrived at, and every gate below
-- applies to both. One copy, so a gate can never be added to one path and
-- forgotten on the other.
-- -------------------------------------------------------------------------
function M.gate_row(state, world, info, now, row, ctx)
  local trip = row.trip
  -- ── per-candidate eligibility, in cost order (cheap tests first) ──────
  local reject = row.hard and ("discovery:" .. row.hard) or nil
  if not reject and row.out_of_leash then
    reject = string.format("out_of_leash (%d > %d)", row.dist,
                           row.leash or C.BUILDER_POOL_LEASH or 8)
  end
  if not reject and not trip then reject = "unreachable" end
  if not reject then
    local have = info.trees or 0
    local need = row.trees_need or 0
    if row.type ~= "farm" and (have - ctx.reserve) < need then
      reject = string.format("tree_reserve(%d,%d,%d)", ctx.reserve, have, need)
    end
  end
  if not reject then
    local ac = M.ally_claim_on(state, info, row.mx, row.my, now, now)
    if ac then
      row.ally = ac
      reject = string.format("ally_repairing (p%d eta %dt)", ac.pn, ac.eta)
    end
  end
  -- An ally is coming to SCOOP this corpse (not to repair it). Rebuild only --
  -- a top-up leaves the pill alive either way, so it cannot spoil a pickup, and
  -- a farm row has no pill at all.
  if row.type == "rebuild" then
    local acap = M.ally_capture_on(state, info, row.mx, row.my, row.id, now)
    log_ally_capture(state, now, row.mx, row.my, row.id, acap)
    if acap and not reject then
      row.ally_capture = acap
      reject = string.format("ally_capturing (p%d, %dt)", acap.pn, acap.age)
      -- BP_DENY is edge-triggered on the reject STRING, and this one carries an
      -- age that moves every tick -- which would make a 350-tick block 350 deny
      -- lines. Give the deny key an age-free form of the same reason.
      row.reject_key = string.format("ally_capturing:p%d", acap.pn)
    end
  end
  -- Pool-wide reasons are applied to the row LAST so a row that would also
  -- have failed on its own merits names its own reason first (a row rejected
  -- for "no wood" should not read "mode_owned" -- fixing the mode would not
  -- make it go).
  if not reject and not ctx.ok then reject = ctx.reason end
  if not reject and ctx.reserve_eta then
    local need_t = (trip or 0) + (C.BUILDER_POOL_RESERVE_MARGIN or 40)
    if need_t > ctx.reserve_eta then
      reject = string.format("reserve(%d < trip %d)", ctx.reserve_eta, need_t)
    end
  end
  -- Most expensive test last: the danger sample along the walk.
  --
  -- Andrew 2026-09-05: no path safety for repairs; more safety checks to come
  -- later. Under the linear formula a rebuild/topup is scored on damage and
  -- trip time only, and it is REFUSED only by tree_reserve, ally_repairing,
  -- ally_capturing, mode_owned, fire_exchange, under_fire, the reserve ETA,
  -- out_of_leash, unreachable and MIN_SCORE. The walk's danger sample is not
  -- one of them any more -- send the man. The FARM row keeps the gate (wood is
  -- never worth walking into a shell for), and so does every repair row under
  -- preset=keel.
  local skip_path = row.linear and true or false
  row.path_gate = not skip_path
  if not reject and not skip_path then
    if not danger.lgm_path_safe_enhanced(info, row.mx, row.my,
           C.BUILDER_POOL_PATH_DANGER or C.LGM_DANGER_MED, now, world) then
      reject = "path_unsafe"
    end
  end
  if not reject and row.score < (C.BUILDER_POOL_MIN_SCORE or 20) then
    reject = string.format("below_min_score(%.0f < %d)", row.score,
                           C.BUILDER_POOL_MIN_SCORE or 20)
  end
  row.reject = reject

  -- The two inputs the formula needs that live on `info`/`ctx` rather than on
  -- the row.  Everything else it prints is already a row field, so the string
  -- can be rebuilt later from the row alone.
  row.f_trees   = info.trees or 0
  row.f_reserve = ctx.reserve
  -- Where the tank stood when this row's legs were measured. On the row for
  -- the same reason as the two above: pred{} is only hand-checkable if the
  -- START of the extrapolation is on the row beside its heading and speed.
  row.f_tankx   = info.tankx
  row.f_tanky   = info.tanky
  return row
end

-- -------------------------------------------------------------------------
-- score_terms: the one-line term breakdown, hand-checkable on its own.
--
-- THE RULE this obeys (the author's, standing): every factor in the formula
-- appears in the string, so the final number can be recomputed from the line
-- alone without opening constants.lua. Two shapes, one per formula:
--
--   linear  score 362 = hp_w(30) x missing(15) = 450 - trip_w(0.25) x trip(352t) = 88
--   legacy  val 184 - trip 95 - danger 0
--
-- ...plus, on the ONE row per tick that is the tank goal's own pill and only
-- when BUILDER_POOL_GOAL_PILL_BONUS is not 1, a third link on the end of the
-- chain: `= bp_raw{373} x goal_w{1.20}`, whose product is the bp_score{} at
-- the head. Absent on every other row, and on every row under preset=keel.
--
-- Used by BP_DISPATCH, BP_DENY's neighbours and the panel row, so all three
-- print the SAME arithmetic.
-- -------------------------------------------------------------------------
-- The trip's three legs as chips, appended to BOTH formulas so the printed
-- trip{} is never a number the reader has to take on trust: out{} + build{} +
-- back{} adds up to it, pred{} says where the return leg was walked TO, and on
-- a farm row wedge{} says which quarter of the leash square offered the tile.
--
-- Appended at the END, after tripcost{}, on purpose: tests/repair_priority_test
-- .py anchors LINEAR_RE at the start of this string, so anything inserted
-- ahead of tripcost{} breaks it. No "||" is ever produced here (the chip
-- parser splits the display half off at the first one) and no chip value
-- reaches 32 characters, which is the popup's limit.
local function leg_chips(row)
  local pred
  if row.pred_mx then
    pred = string.format("pred{%d,%d}", row.pred_mx, row.pred_my)
  else
    pred = "pred{same}"
  end
  local wedge = row.wedge and string.format(" wedge{%s}", row.wedge) or ""
  return string.format(
    " [legs out{%s} + build{%s} + back{%s} %s predsrc{%s}%s]",
    tostring(row.out_ticks or "-"),
    tostring(row.build_ticks or (C.LGM_BUILD_TIME or 20)),
    tostring(row.back_ticks or "-"), pred,
    tostring(row.pred_src or "-"), wedge)
end

-- The goal-pill bonus, as the last link of the chain: the arithmetic above it
-- produces bp_raw{}, and bp_raw x goal_w is the bp_score{} at the head of the
-- line. Printed ONLY when the factor is not 1 -- a chip that always says x1 is
-- noise on every row of every game, and its absence means exactly "this row is
-- not the goal's pill" (or BUILDER_POOL_GOAL_PILL_BONUS is 1.0, e.g.
-- preset=keel), which is why the docs say so.
local function goal_chips(row)
  local w = row.goal_w or 1.0
  if w == 1.0 then return "" end
  return string.format(" = bp_raw{%.0f} x goal_w{%.2f}", row.raw_score or 0, w)
end

function M.score_terms(row)
  -- Every chip is word{value}: BrainTest's pool-grid detail popup parses
  -- exactly that shape (pool_grid.cpp) into its term table, so the same
  -- string is the hand-checkable log line AND the popup's term list.
  local score_str = row.trip and string.format("%.0f", row.score or 0)
                    or "n/a: no route"
  if row.linear then
    return string.format(
      "bp_score{%s} = hp_w{%d} x missing{%d} = value{%.0f}"
      .. " - trip_w{%.2f} x trip{%st} = tripcost{%.0f}%s%s",
      score_str, row.v_hp_w or (C.BUILDER_POOL_REPAIR_HP_W or 30),
      row.missing or 0, row.value or 0,
      C.BUILDER_POOL_REPAIR_TRIP_W or 0.25, tostring(row.trip or "-"),
      row.c_trip or 0, goal_chips(row), leg_chips(row))
  end
  local urg_name = (row.type == "farm") and "urg" or "topup_hp"
  return string.format(
    "bp_score{%s} = bp_base{%.0f} + %s{%.0f} + front{%.0f} = value{%.0f}"
    .. " - trip_w{%.2f} x trip{%st} = tripcost{%.0f}"
    .. " - danger_w{%.2f} x bp_danger{%.0f} = dangercost{%.0f}%s%s",
    score_str, row.v_base or 0, urg_name, row.v_hp or 0, row.v_front or 0,
    row.value or 0,
    C.BUILDER_POOL_TRIP_W or 0.5, tostring(row.trip or "-"), row.c_trip or 0,
    C.BUILDER_POOL_DANGER_W or 1.5, row.danger or 0, row.c_danger or 0,
    goal_chips(row), leg_chips(row))
end

-- -------------------------------------------------------------------------
-- The pool-grid detail string for one scored row: the short chip line before
-- "||" and, after it, one "name:computation" segment per chip (joined by
-- "|"), which the popup shows next to the chip's value and meaning.
--
-- COLD PATH.  This used to run inside score_row, so a ~35-argument
-- string.format (plus the label and score_str formats feeding it) executed for
-- every candidate on every tick even in production, where the only consumer --
-- M.panel_section, below, driving BrainTest's pool grid -- never asked for it.
-- It is built on demand instead; every chip is read straight back off the row,
-- so the text is byte-for-byte what score_row used to store.
-- -------------------------------------------------------------------------
function M.row_formula(row)
  local FMAX     = C.BUILDER_POOL_FRONT_MAX_TILES or 12
  local fd       = row.front_dist or 0
  local trip     = row.trip
  local out_ticks = row.out_ticks
  local dgr      = row.danger or 0
  local reject   = row.reject
  local MIN      = C.BUILDER_POOL_MIN_SCORE or 20
  local label = (row.type == "farm")
    and string.format("farm@(%d,%d)", row.mx, row.my)
    or string.format("%s p#%d@(%d,%d) %s hp=%d/%d", row.type, row.id,
                     row.mx, row.my, row.own or "?",
                     row.hp or 0, C.PILLS_MAX_HEALTH or 15)
  -- A row with no walkable route has no score to print: the sentinel that
  -- sorts it last (-1e9) is an ordering device, not an arithmetic result, and
  -- printing it as one ("= -1000000000") invites the reader to check a sum
  -- that does not exist. Say what actually happened instead.
  local score_str = trip and string.format("%.0f", row.score)
                    or "n/a (no walkable route for the man)"
  -- The goal-pill bonus, if this row has one: the tail of the arithmetic
  -- chain, so the bp_score segment ends in the number that is actually on the
  -- row rather than in the pre-bonus one.
  local gw = row.goal_w or 1.0
  local chain = score_str
  if gw ~= 1.0 then
    chain = string.format("bp_raw(%.0f) x goal_w(%.2f) = %s",
                          row.raw_score or 0, gw, score_str)
  end
  local short = string.format("%s %s%s", label, M.score_terms(row),
                              reject and (" REJECT " .. reject) or "")
  local trip_seg = string.format(
    "trip:out(%s) + build(%s) + back(%s) = %s brain ticks for the man"
    .. " to walk out, build and walk back",
    tostring(out_ticks or "-"),
    tostring(row.build_ticks or (C.LGM_BUILD_TIME or 20)),
    tostring(row.back_ticks or "-"), tostring(trip or "-"))
  -- The legs, one segment per chip. Written once and spliced into both
  -- formulas' segment lists, so a repair row and a farm row explain the walk
  -- with the same words.
  local out_seg = string.format(
    "out:cpf_lgm_travel_ticks_map(tank -> (%d,%d), that tile BLESSED so a pill"
    .. "/base on it does not self-block) = %s brain ticks. The straight-line"
    .. " engine walk sim (brain_pathfinder.c lgmTravelTicksCore), not a path"
    .. "finder -- a wall in the way reads STUCK and the row goes unreachable."
    .. " A target 1 tile away or less skips the sim and is charged"
    .. " BUILDER_POOL_GRASS_TICKS_PER_TILE(%d).",
    row.mx, row.my, tostring(out_ticks or "-"),
    C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16)
  local build_seg = string.format(
    "build:LGM_BUILD_TIME(%d) = brain ticks the man stands on the tile"
    .. " building / repairing / chopping (lgm.h:84)",
    C.LGM_BUILD_TIME or 20)
  local back_seg, pred_seg, predsrc_seg
  if not C.BUILDER_POOL_RETURN_PREDICT then
    back_seg = string.format(
      "back:BUILDER_POOL_RETURN_PREDICT is OFF, so the return leg is the"
      .. " outbound leg mirrored -- out(%s) again, i.e. the tank is assumed to"
      .. " wait on the spot.", tostring(out_ticks or "-"))
    pred_seg = "pred:no prediction (BUILDER_POOL_RETURN_PREDICT off) -- the"
      .. " walk home is measured back to the tank's CURRENT tile."
  elseif row.pred_mx then
    back_seg = string.format(
      "back:cpf_lgm_travel_ticks_map((%d,%d) -> the PREDICTED tank tile"
      .. " (%d,%d), the job tile BLESSED because he starts standing on it)"
      .. " = %s brain ticks.",
      row.mx, row.my, row.pred_mx, row.pred_my, tostring(row.back_ticks or "-"))
    pred_seg = string.format(
      "pred:tank(%d,%d) walked %s brain ticks forward along its OWN ROUTE"
      .. " (state.pf.path_chain, waypoint %s of %s, reached at t=%s) at the"
      .. " engine's per-terrain speed caps (C.MAP_SPEED, bolo_map.h"
      .. " MAP_SPEED_T*; 256 WU a tile, 362 diagonally) -> tile (%d,%d)."
      .. " Horizon = min(out + build, BUILDER_POOL_RETURN_PREDICT_MAX_TICKS(%d)).",
      bit.rshift((row.f_tankx or 0), 8), bit.rshift((row.f_tanky or 0), 8),
      tostring(row.pred_horizon or "-"), tostring(row.pred_route_i or "-"),
      tostring(row.pred_route_n or "-"),
      row.pred_route_t and string.format("%.0f", row.pred_route_t) or "-",
      row.pred_mx, row.pred_my,
      C.BUILDER_POOL_RETURN_PREDICT_MAX_TICKS or 400)
  else
    back_seg = string.format(
      "back:the prediction FELL BACK to the tank's current tile (%s), so the"
      .. " return leg is the outbound leg mirrored -- out(%s).",
      tostring(row.pred_why or "-"), tostring(out_ticks or "-"))
    pred_seg = string.format(
      "pred:same tile as the tank -- fallback reason '%s'. no_route / "
      .. "route_stale / route_ended / route_no_progress = the tank has no"
      .. " committed route to walk (idle, or the chain belongs to a goal it has"
      .. " left); horizon_too_short = it has one but cannot clear its own tile"
      .. " inside out+build; off_map / unwalkable = the predicted tile is"
      .. " water, a building or a live pill; back_unreachable = the walk sim"
      .. " could not get the man from the job to it. Horizon was %s ticks.",
      tostring(row.pred_why or "-"), tostring(row.pred_horizon or "-"))
  end
  predsrc_seg = string.format(
    "predsrc:where pred{} came from. route = walked along state.pf.path_chain,"
    .. " the route the navigator computed and steering is already driving"
    .. " (nothing is re-searched here). same = a fallback landed on the tank's"
    .. " own tile, which makes back == out, the pre-2026-09-06 number."
    .. " off = BUILDER_POOL_RETURN_PREDICT is disabled. This row: %s.",
    tostring(row.pred_src or "-"))
  local wedge_seg = row.wedge and string.format(
    "wedge:the 90-degree quarter of the BUILDER_POOL_LEASH(%d) square this"
    .. " forest was the nearest in -- N/E/S/W, boundaries on the 45-degree"
    .. " diagonals (abs(dx) > abs(dy) is E/W, otherwise N/S; the diagonal"
    .. " itself goes to the vertical wedge). One row per wedge, so a nearer forest the"
    .. " man cannot straight-line to loses to a clear one elsewhere."
    .. " 'all' = BUILDER_POOL_FARM_SECTORS is 1 (one row, nearest anywhere).",
    C.BUILDER_POOL_LEASH or 8) or nil
  local tail = string.format(
    " Fires only if score >= BUILDER_POOL_MIN_SCORE(%d) and no gate rejects."
    .. " trees need %d, have %d, reserved %d. leash %d, dist %d, front_dist %d.%s",
    MIN, row.trees_need or 0, row.f_trees or 0, row.f_reserve or 0,
    row.leash or C.BUILDER_POOL_LEASH or 8, row.dist or -1, fd,
    reject and (" REJECTED: " .. reject) or " ACCEPTED.")
  local segs
  -- The linear repair row (BUILDER_POOL_REPAIR_LINEAR). Two terms and no
  -- others, so the string is two terms and no others -- printing a front /
  -- danger chip that scores nothing would invite the reader to check a sum
  -- that is not the sum the code computed.
  if row.linear then
    local hp_w = row.v_hp_w or (C.BUILDER_POOL_REPAIR_HP_W or 30)
    local tw   = C.BUILDER_POOL_REPAIR_TRIP_W or 0.25
    segs = {
      string.format("bp_score:value(%.0f) - tripcost(%.0f) = %s. NO danger term"
        .. " (threat.at(%.0f) here is printed on the panel, not charged) and"
        .. " NO path-safety gate on repair rows.%s",
        row.value or 0, row.c_trip or 0, chain, dgr, tail),
      string.format("hp_w:BUILDER_POOL_REPAIR_HP_W(%d) = points per missing hp", hp_w),
      string.format("missing:PILLS_MAX_HEALTH(%d) - hp(%d) = %d",
        C.PILLS_MAX_HEALTH or 15, row.hp or 0, row.missing or 0),
      string.format("value:hp_w(%d) x missing(%d) = %.0f",
        hp_w, row.missing or 0, row.value or 0),
      trip_seg,
      out_seg, build_seg, back_seg, pred_seg, predsrc_seg,
      string.format("trip_w:BUILDER_POOL_REPAIR_TRIP_W(%.2f) = points per round-trip tick", tw),
      string.format("tripcost:trip_w(%.2f) x trip(%s) = %.0f",
        tw, tostring(trip or "-"), row.c_trip or 0),
    }
  else
    local tw = C.BUILDER_POOL_TRIP_W or 0.5
    local dw = C.BUILDER_POOL_DANGER_W or 1.5
    local urg_seg
    if row.type == "topup" then
      urg_seg = string.format("topup_hp:BUILDER_POOL_TOPUP_PER_HP(%d) x missing(%d) = %.0f",
        C.BUILDER_POOL_TOPUP_PER_HP or 6, row.missing or 0, row.v_hp or 0)
    elseif row.type == "farm" then
      urg_seg = string.format(
        "urg:BUILDER_POOL_FARM_URGENCY(%d) x max(0, FARM_LOW_TREES(%d) - trees(%d)) = %.0f",
        C.BUILDER_POOL_FARM_URGENCY or 12, C.BUILDER_POOL_FARM_LOW_TREES or 12,
        row.f_trees or 0, row.v_hp or 0)
    else
      urg_seg = "topup_hp:no per-hp term on a rebuild row = 0"
    end
    local front_seg = (row.type == "farm")
      and "front:no front clock on farm rows (a forest cannot be stolen) = 0"
      or string.format(
        "front:BUILDER_POOL_FRONT_URGENCY(%d) x max(0, FRONT_MAX(%d) - front_dist(%d)) / FRONT_MAX(%d) = %.0f",
        C.BUILDER_POOL_FRONT_URGENCY or 120, FMAX, fd, FMAX, row.v_front or 0)
    segs = {
      string.format("bp_score:value(%.0f) - tripcost(%.0f) - dangercost(%.0f) = %s.%s",
        row.value or 0, row.c_trip or 0, row.c_danger or 0, chain, tail),
      string.format("bp_base:BUILDER_POOL_VALUE_%s(%.0f) = fixed value of this job type",
        string.upper(row.type), row.v_base or 0),
      urg_seg,
      front_seg,
      string.format("value:base(%.0f) + %s(%.0f) + front(%.0f) = %.0f",
        row.v_base or 0, (row.type == "farm") and "urg" or "topup_hp",
        row.v_hp or 0, row.v_front or 0, row.value or 0),
      trip_seg,
      out_seg, build_seg, back_seg, pred_seg, predsrc_seg,
      string.format("trip_w:BUILDER_POOL_TRIP_W(%.2f) = points per round-trip tick", tw),
      string.format("tripcost:trip_w(%.2f) x trip(%s) = %.0f",
        tw, tostring(trip or "-"), row.c_trip or 0),
      string.format("bp_danger:threat.at(%d,%d) = %.0f (hostile pills/tanks with the tile in range)",
        row.mx, row.my, dgr),
      string.format("danger_w:BUILDER_POOL_DANGER_W(%.2f) = points per danger unit", dw),
      string.format("dangercost:danger_w(%.2f) x danger(%.0f) = %.0f",
        dw, dgr, row.c_danger or 0),
    }
  end
  -- The bonus's own two segments, on both formulas, only when it applied.
  if gw ~= 1.0 then
    segs[#segs + 1] = string.format(
      "bp_raw:the score BEFORE the goal-pill bonus -- value(%.0f) minus the"
      .. " costs above = %.0f. bp_raw x goal_w is the bp_score at the head of"
      .. " the line.", row.value or 0, row.raw_score or 0)
    if row.goal_src == "ping" then
      segs[#segs + 1] = string.format(
        "goal_w:BUILDER_POOL_PING_PILL_BONUS(%.2f) -- a human (p%s) bot-command"
        .. " pinged this row's pill (#%s) and this bot took the order, so its whole score is multiplied by"
        .. " it until t=%s, the pill is dead or fully repaired, or another of"
        .. " our pills is pinged: %.0f x %.2f = %s. It replaces the goal-pill"
        .. " bonus (the larger one is used, never both).",
        gw, tostring(row.ping_by or "?"), tostring(row.id or "?"),
        tostring(row.ping_until or "?"),
        row.raw_score or 0, gw, score_str)
    else
      segs[#segs + 1] = string.format(
        "goal_w:BUILDER_POOL_GOAL_PILL_BONUS(%.2f) -- this row's pill (#%s) IS"
        .. " the tank goal's own target (%s target_id=%s), so its whole score is"
        .. " multiplied by it: %.0f x %.2f = %s. The chip is absent on every"
        .. " other row, and on every row when the bonus is 1.0 (preset=keel).",
        gw, tostring(row.id or "?"),
        tostring(row.goal_kind or "goal"), tostring(row.id or "?"),
        row.raw_score or 0, gw, score_str)
    end
  end
  if wedge_seg then segs[#segs + 1] = wedge_seg end
  return short .. "||" .. table.concat(segs, "|")
end

-- Deterministic ordering: best score, then type rank (rebuild before topup
-- before farm), then tile key. No pairs() order ever reaches this sort.
--
-- Under BUILDER_POOL_SEEDED_COMPETES (the default since 2026-09-06) a SEEDED
-- row has no sort key of its own at all -- it is ordered on its score like
-- everything else, and the tank goal's interest in its pill is expressed by
-- BUILDER_POOL_GOAL_PILL_BONUS instead, which is IN the score and printed with
-- it (bp_raw{} x goal_w{}). That is the difference the 2026-09-06 incident
-- turned on: a seeded 1-hp top-up worth -34 sorting ahead of a 5-hp repair
-- worth ~87, and closing the pool behind it.
--
-- With the knob off (preset=keel) the old key comes back: a seeded row first
-- whatever it scores, because a feeder's job outranks any side-quest by
-- construction. It was a separate sort key rather than a bonus added to the
-- score precisely because the score is PRINTED and has to stay reproducible
-- from the chips beside it: "score=1000089 (val 184 - trip 95 - danger 0)"
-- does not add up and cannot be hand-checked.
local function order_rows(rows)
  local seed_first = not C.BUILDER_POOL_SEEDED_COMPETES
  table.sort(rows, function(a, b)
    if seed_first then
      local sa, sb = a.seeded and 1 or 0, b.seeded and 1 or 0
      if sa ~= sb then return sa > sb end
    end
    if a.score ~= b.score then return a.score > b.score end
    local ra, rb = TYPE_RANK[a.type] or 9, TYPE_RANK[b.type] or 9
    if ra ~= rb then return ra < rb end
    return (a.my * 256 + a.mx) < (b.my * 256 + b.mx)
  end)
  return rows
end

-- -------------------------------------------------------------------------
-- seed_row_of / seed_tail — the seeded row on a BP_DISPATCH / BP_DENY line.
--
-- Since BUILDER_POOL_SEEDED_COMPETES the seeded row is frequently NOT the row
-- the line is about -- that IS the change -- and "the goal asked for a job and
-- something else went instead" is unreadable unless the line carries the
-- seeded row's own score and the reason it lost. When the seeded row IS the
-- subject the tail is the plain `seeded_by=` the dispatch line always had.
-- rows is an array in sort order, so the first seeded row is deterministic
-- (there is at most one seed per tick anyway).
-- -------------------------------------------------------------------------
local function seed_row_of(rows)
  for _, r in ipairs(rows) do
    if r.seeded then return r end
  end
  return nil
end

local function seed_tail(rows, subject, with_subject)
  local sr = seed_row_of(rows)
  if not sr then return "" end
  if sr == subject then
    return with_subject and (" seeded_by=" .. tostring(sr.seeded)) or ""
  end
  return string.format(
    " seed=%s@(%d,%d)/%s seed_score=%.0f seed_reject=%s",
    sr.type, sr.mx, sr.my, tostring(sr.seeded), sr.score or 0,
    tostring(sr.reject or "none"))
end

-- -------------------------------------------------------------------------
-- Active job lifecycle.
--
-- The record exists from dispatch until the man is home. Phases are read from
-- the engine's own LGM state rather than assumed:
--   outbound   he is walking and has not stood on the target tile yet
--   working    he is ON the target tile (building / repairing / chopping)
--   returning  he has been on the target and is walking again
-- The job ends when man_status returns to LGM_INTANK (after ABORT_GRACE, since
-- the engine takes a few ticks to actually move him off the hatch), or when
-- JOB_MAX_TICKS elapses -- a lost LGM must not hold the ally claim for ever.
-- -------------------------------------------------------------------------
function M.update_job(state, world, info, now)
  local job = state._bp_job
  if not job then return end
  local man_mx = bit.rshift(info.man_x or info.tankx, 8)
  local man_my = bit.rshift(info.man_y or info.tanky, 8)
  if info.man_status == C.LGM_MOVING then
    if man_mx == job.mx and man_my == job.my then
      job.reached = true
      job.phase = "working"
    else
      job.phase = job.reached and "returning" or "outbound"
    end
    -- ETA is measured from WHERE HE IS, and to the right place: outbound /
    -- working, that is the remaining walk to the target (what an ally needs to
    -- know -- when the pill gets fixed); returning, it is the walk home (when
    -- the man is available again). Measuring both from the tank, as a single
    -- tank->target call would, overstates the return leg by the whole outbound
    -- distance and would keep the claim alive long after the job was done.
    local left
    if job.phase == "returning" then
      left = cpf_lgm_travel_ticks_map(man_mx, man_my,
                                      bit.rshift(info.tankx, 8),
                                      bit.rshift(info.tanky, 8), 0, 0,
                                      C.BUILDER_POOL_LGM_MAX_TICKS or 2000,
                                      C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
    else
      left = cpf_lgm_travel_ticks_map(man_mx, man_my, job.mx, job.my,
                                      job.mx, job.my,
                                      C.BUILDER_POOL_LGM_MAX_TICKS or 2000,
                                      C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
    end
    if left == nil or left < 0 then left = 0 end
    job.eta = left
    job.claim_eta_tick = now + left
  end

  local age = now - (job.dispatch_tick or now)
  local home = info.man_status == C.LGM_INTANK
               and age > (C.BUILDER_POOL_ABORT_GRACE or 30)
  local dead = info.man_status == C.LGM_DEAD
  local timeout = age > (C.BUILDER_POOL_JOB_MAX_TICKS or 900)
  if not (home or dead or timeout) then return end

  -- Outcome from the WORLD, not from our own account of it: for a repair the
  -- question is whether the pill is friendly and has more armour than when we
  -- left, which is the only thing the errand was for.
  local outcome, detail
  if job.type == "farm" then
    local gained = (info.trees or 0) - (job.trees_at_dispatch or 0)
    outcome = gained > 0 and "ok" or "no_wood"
    detail = string.format("trees %d -> %d", job.trees_at_dispatch or -1, info.trees or -1)
  else
    local lst = world.pill_at and world.pill_at[job.my * 256 + job.mx]
    local p = lst and lst[1] and lst[1].pill
    local hp = p and p.health or 0
    local own = p and p.owner or "gone"
    outcome = (own == "friendly" and hp > (job.hp_at_dispatch or 0)) and "ok" or "no_change"
    detail = string.format("pill %s hp %d -> %d", own, job.hp_at_dispatch or -1, hp)
  end
  -- The man dying does NOT make the errand a failure, and conflating the two
  -- would corrupt the one number this whole design is meant to be judged on.
  -- "LGM deaths on side-quests should be ~0" is a safety metric; "the pill came
  -- back up" is an effectiveness metric. Observed on DH-Oil Rig at t=15183: a
  -- front-line rebuild took the corpse from 0 to 15 armour and the man was
  -- killed on the walk home. That is a WIN with a cost, not an abort, and
  -- filing it as an abort would have made the pool look worse and safer than
  -- it is at the same time. So the world's verdict decides ok/not-ok, and the
  -- man's fate rides along as a chip on whichever line is printed.
  if timeout and not home then outcome = "timeout" end
  local lgm_chip = dead and ", LGM KILLED on this trip" or ""

  if outcome == "ok" then
    print2(string.format(
      "BP_DONE t=%d job=%s target=(%d,%d) outcome=ok took=%dt lgm=%s (%s%s)",
      now, job.type, job.mx, job.my, age, dead and "dead" or "home",
      detail, lgm_chip))
  else
    print2(string.format(
      "BP_ABORT t=%d job=%s target=(%d,%d) why=%s took=%dt lgm=%s (%s%s)",
      now, job.type, job.mx, job.my,
      dead and (outcome .. "+lgm_dead") or outcome,
      age, dead and "dead" or "home", detail, lgm_chip))
  end
  state._bp_last = { type = job.type, mx = job.mx, my = job.my,
                     outcome = outcome, lgm_dead = dead or nil, tick = now }
  state._bp_job = nil
end

-- -------------------------------------------------------------------------
-- seed_job — the FEEDER entry point (plan section 7).
--
-- repair_pill splits by whether the TANK must move:
--   in-leash  -> the pool's job (a side-quest row, discovered above);
--   out-of-leash -> stays a TANK goal meaning "relocate so the repair becomes
--                leash-reachable". On arrival it does NOT dispatch itself; it
--                seeds the pool with a top-priority job and the pool executes.
-- The defend->repair handoff (defend_pill + goal.repair) is the same shape and
-- feeds the same way. ONE EXECUTOR, several feeders -- which is the whole
-- point: before this, a repair could be dispatched from builder.decide's
-- Priority 0.4 with no claim, no job record and no panel row, in parallel with
-- a pool that did not know it had happened.
--
-- A seeded job bypasses the MODE gate (the tank's whole goal IS this repair),
-- the leash (the goal's own danger-blended dispatch range decides how close is
-- close enough) and the tree reserve, but nothing else: under-fire, `have >=
-- need` wood, path safety, the ally claim, MIN_SCORE and the shell gate all
-- still apply, and are all evaluated by the same score_row.
--
-- It does NOT buy a place at the front of the queue (2026-09-06,
-- BUILDER_POOL_SEEDED_COMPETES): the row is scored and ordered like every
-- other, and the goal's stake in its own pill is priced by
-- BUILDER_POOL_GOAL_PILL_BONUS. Nor does the feeder goal shut the other rows
-- out with a `mode_owned` of its own -- see M.eligibility.
-- -------------------------------------------------------------------------
function M.seed_job(state, mx, my, src)
  state._bp_seed = { mx = mx, my = my, src = src, tick = state.tick or 0 }
end

-- -------------------------------------------------------------------------
-- repair_feeder — the goal-driven half of the split, run at the top of
-- update() so a seeded job is scored on the SAME tick the goal asks for it.
--
-- This is builder.decide()'s old Priority 0.4 gate, moved wholesale. It used
-- to `return {action = BUILDMODE_PBOX}` directly, which meant a repair could
-- go out with no job record, no ally claim and no panel row, in parallel with
-- a pool that had no idea it had happened. Now it SEEDS and the pool executes.
-- The gate itself is unchanged, including every print2 the tests read:
--
--   * danger-blended distance cap: insist on REPAIR_DISPATCH_DIST_BASE when
--     calm, widening toward DIST_DANGEROUS as threat_at_tank climbs from
--     DANGER_LOW to DANGER_HIGH. The tank keeps closing either way; we just
--     stop EARLIER when staying close would cost armour.
--   * enemy-near hold (REPAIR_HOLD_ENEMY_NEAR_ENABLED, default OFF).
--   * under-fire hold (REPAIR_HOLD_UNDER_FIRE_ENABLED, default ON): a shell
--     landed on the PILL less than REPAIR_QUIET_TICKS ago. Distinct from the
--     pool's own under-fire clock, which is about the TANK.
--
-- Two feeders, one executor: goal.kind == "repair_pill" (the out-of-leash
-- relocate arriving), and defend_pill + goal.repair (the ARRIVED handoff),
-- whose target comes from goal.pill_mx/my because a defend goal's own mx/my
-- can be a hold tile beside the pill.
-- -------------------------------------------------------------------------
function M.repair_feeder(state, world, info, now)
  local g = state.goal
  if not g then return end
  local px, py, src
  if g.kind == "repair_pill" then
    px, py, src = g.mx, g.my, "repair_pill"
  elseif g.kind == "defend_pill" and g.repair then
    px = g.pill_mx or g.mx
    py = g.pill_my or g.my
    src = "defend_repair"
  end
  if not (px and py) then return end
  if info.man_status ~= C.LGM_INTANK or info.inboat then return end

  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local dist = U.mdist(tmx, tmy, px, py)
  local DANGER_LOW     = C.REPAIR_DISPATCH_DANGER_LOW or 50
  local DANGER_HIGH    = C.REPAIR_DISPATCH_DANGER_HIGH or 150
  local DIST_BASE      = C.REPAIR_DISPATCH_DIST_BASE or 5
  local DIST_DANGEROUS = C.REPAIR_DISPATCH_DIST_DANGEROUS or 12
  local danger_at_tank = (state.perc and state.perc.threat_at_tank) or 0
  local t = (danger_at_tank - DANGER_LOW) / (DANGER_HIGH - DANGER_LOW)
  if t < 0 then t = 0 elseif t > 1 then t = 1 end
  local blend_max = DIST_BASE + (DIST_DANGEROUS - DIST_BASE) * t
  -- ...but never TIGHTER than the leash. The split (goals.lua) takes the
  -- pool-5 row to INF as soon as the pill is inside BUILDER_POOL_LEASH, on the
  -- grounds that the man can walk it from here. If the feeder still insisted
  -- on the calm 5, everything between 5 and 8 was a dead zone: the tank goal
  -- had stood down, the feeder had not yet stood up, and the mode was still
  -- "gather" (repair_pill's mode) so the ordinary side-quest path denied
  -- mode_owned as well. The goal then lost the next replan and the pill was
  -- simply abandoned -- observed at t=293..353 of the variant-B arena, where
  -- the bot went on to shoot its own pill down for a reposition instead.
  -- One number decides "close enough for the man": the leash. The danger
  -- widening still applies on top, so a tank being shelled can still dispatch
  -- from further out than 8.
  --
  -- 2026-09-05: that number is now the REPAIR leash (11 under the linear
  -- formula), not the farm leash, and it has to be -- this is a repair feeder.
  -- Leaving it at 8 would rebuild exactly the dead zone the paragraph above
  -- describes, three tiles further out: builder_can_repair (also on the repair
  -- leash) takes the tank's pool-5 row to INF at 11, so between 9 and 11 the
  -- tank goal would have stood down while the feeder had not yet stood up, and
  -- the ordinary side-quest path would still be denying mode_owned.
  local effective_max = math.max(blend_max, M.repair_leash())
  local in_range = dist <= effective_max
  local has_trees = (info.trees or 0) > 0

  local _tgt_pill
  do
    local lst = world.pill_at and world.pill_at[py * 256 + px]
    _tgt_pill = lst and lst[1] and lst[1].pill
  end
  local enemy_hold = false
  if C.REPAIR_HOLD_ENEMY_NEAR_ENABLED then
    local tp = _tgt_pill
    if tp and tp._enemy_near_tick
       and ((state.tick or 0) - tp._enemy_near_tick) < (C.REPAIR_HOLD_ENEMY_NEAR_TICKS or 400) then
      enemy_hold = true
    end
  end
  local under_fire_hold, _uf_age = false, nil
  if C.REPAIR_HOLD_UNDER_FIRE_ENABLED then
    local lh = _tgt_pill and _tgt_pill.last_hit_tick
    if lh and lh > 0 then
      _uf_age = (state.tick or 0) - lh
      if _uf_age < (C.REPAIR_QUIET_TICKS or 75) then under_fire_hold = true end
    end
  end
  if under_fire_hold and (state._bhuf_log_tick or -1) ~= (state.tick or 0) then
    state._bhuf_log_tick = state.tick or 0
    print2(string.format(
      "BUILDER_HOLD_UNDER_FIRE t=%d pill=(%d,%d) hit_age=%d quiet=%d goal=%s"
      .. " -- LGM stays aboard while shells are still landing",
      state.tick or 0, px, py, _uf_age or -1, C.REPAIR_QUIET_TICKS or 75,
      g.kind or "?"))
  end
  local _hold = enemy_hold or under_fire_hold
  local ticks = (in_range and has_trees and not _hold)
    and M.lgm_trip(state, info, px, py) or nil
  local can_dispatch = in_range and has_trees and not _hold and ticks ~= nil

  if BRAIN_DEBUG_MODE then
    local r, gg, bb
    if can_dispatch then       r, gg, bb =   0, 255,   0
    elseif not in_range then   r, gg, bb = 255, 220,   0
    else                       r, gg, bb = 255, 100, 100 end
    viz.circle("repair_pill_viz", px + 0.5, py + 0.5, 0.55, r, gg, bb, 220)
    viz.circle("repair_pill_viz", px + 0.5, py + 0.5, 0.30, r, gg, bb, 180)
    local tank_fx, tank_fy = info.tankx / 256.0, info.tanky / 256.0
    viz.line("repair_pill_viz", tank_fx, tank_fy, px + 0.5, py + 0.5, r, gg, bb, 120)
    viz.text("repair_pill_viz", tank_fx + 0.6, tank_fy - 1.2,
             string.format("Repair d=%d/%.1f tr=%d dgr=%d lgm=%d%s",
                           dist, effective_max, info.trees or 0,
                           math.floor(danger_at_tank or 0),
                           math.floor(ticks or 0),
                           under_fire_hold
                             and string.format(" HOLD(under fire %dt)", _uf_age or -1)
                             or (enemy_hold and " HOLD(enemy near)" or "")),
             "topleft", r, gg, bb, 240)
    print2(string.format(
      "REPAIR_DISPATCH_CHECK t=%d pill=(%d,%d) tank=(%d,%d) dist=%d eff_max=%.1f trees=%d danger=%d lgm_ticks=%d"
      .. " hold=%s (enemy_near=%s under_fire=%s hit_age=%s) goal=%s can=%s",
      state.tick or 0, px, py, tmx, tmy, dist, effective_max, info.trees or 0,
      math.floor(danger_at_tank or 0), math.floor(ticks or 0), tostring(_hold),
      tostring(enemy_hold), tostring(under_fire_hold),
      _uf_age and tostring(_uf_age) or "-", g.kind or "?", tostring(can_dispatch)))
  end

  if can_dispatch then
    M.seed_job(state, px, py, src)
    state._bp_seed.eta = ticks
    state._bp_seed.uf_age = _uf_age
  end
end

-- -------------------------------------------------------------------------
-- builder_can_repair — the repair_pill split, asked from the TANK pool.
--
-- "Is this pill something the man can already walk to from where the tank
-- stands?" If yes, the tank goal has nothing to add: the pool will do it
-- without moving the tank at all, and the goal row should say so rather than
-- spending a replan driving somewhere it is already close enough to.
-- Returns eta(ticks) when the pool can take it, else nil.
-- -------------------------------------------------------------------------
function M.builder_can_repair(state, world, info, p)
  if not C.BUILDER_POOL_ENABLED then return nil end
  if not p or p.owner ~= "friendly" then return nil end
  if info.man_status ~= C.LGM_INTANK or info.inboat then return nil end
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local leash = M.repair_leash()
  if U.mdist(tmx, tmy, p.mx, p.my) > leash then return nil end
  local maxhp = C.PILLS_MAX_HEALTH or 15
  local hp = p.health or 0
  local need = (hp <= 0) and (C.BUILDER_POOL_TREES_REBUILD or 4)
               or math.ceil((maxhp - hp) / (C.PILL_REPAIR_AMOUNT or 4))
  if (info.trees or 0) < need then return nil end
  local out = M.lgm_trip(state, info, p.mx, p.my)
  if not out then return nil end
  return out
end

-- -------------------------------------------------------------------------
-- update — call once per tick from init.lua, AFTER builder.set_mode and
-- BEFORE builder.decide. Publishes state._builder_pool (the panel + the rung
-- both read it) and, when a winner survives the whole stack, the dispatch the
-- rung will issue.
--
-- The pool is computed even when it cannot possibly fire, because the PANEL
-- has to show why. That is the always-show rule the other strips follow.
-- -------------------------------------------------------------------------
function M.update(state, world, info, now)
  danger.update_fire_clock(state, info, now)
  M.update_job(state, world, info, now)
  if not C.BUILDER_POOL_ENABLED then state._builder_pool = nil; return end

  -- Did LAST tick's winner actually go? The rung sits at Priority 1c, so
  -- decide() can return before ever reaching it -- the drowning road, the
  -- slow-terrain road, and the sea-plan tree reserve all bail above it. That
  -- is correct precedence (survival and spoken-for wood outrank an errand),
  -- but without this the pool would show a winner on the panel and then
  -- silently not dispatch it, which is exactly the class of unexplained
  -- "return nil" that builder.decide's own bail() machinery exists to stop.
  -- Edge-triggered on the row so a long drowning spell is one line.
  local prev = state._builder_pool
  if prev and prev.dispatch and not state._bp_job then
    local pd = prev.dispatch
    local pkey = string.format("%s:%d:%d", pd.type, pd.mx, pd.my)
    if state._bp_preempt_key ~= pkey then
      state._bp_preempt_key = pkey
      print2(string.format(
        "BP_DENY t=%d job=%s target=(%d,%d) reason=preempted_above_rung elig=%s"
        .. " score=%.0f trip=%s trees=%d/%d res=%d"
        .. " -- decide() bailed before Priority 1c (drowning road / slow-terrain"
        .. " road / sea-plan tree reserve all outrank an errand)",
        now, pd.type, pd.mx, pd.my,
        prev.reason and tostring(prev.reason) or "yes",
        pd.score or 0, tostring(pd.trip or "-"),
        info.trees or 0, pd.trees_need or 0, prev.reserve or 0))
    end
  else
    state._bp_preempt_key = nil
  end

  state._bp_seed = nil
  M.repair_feeder(state, world, info, now)

  local b = state.builder
  local goal = state.goal or {}
  local reserve, r_base, r_pills, r_goal, r_sea = M.tree_reserve(state, info, b)
  local ok, reason, d = M.eligibility(state, world, info, now)

  local bp = {
    tick = now,
    owner_kind = goal.kind or "none",
    owner_mode = (b and b.mode) or "none",
    owner_sub  = goal.substate,
    ok = ok, reason = reason, detail = d,
    reserve = reserve, r_base = r_base, r_pills = r_pills,
    r_goal = r_goal, r_sea = r_sea,
    reserve_eta = b and b.reserve_eta or nil,
    reserve_why = b and b.reserve_why or nil,
    trees = info.trees or 0,
    man_status = info.man_status,
    inboat = info.inboat,
    job = state._bp_job,
    rows = {}, dispatch = nil,
  }
  state._builder_pool = bp

  -- Hard preconditions that are not "eligibility" but physics: no man, no
  -- errand. Kept out of the reason ladder so the panel's elig line stays about
  -- decisions rather than about whether the man is standing in the tank.
  local can_send = info.man_status == C.LGM_INTANK and not info.inboat
                   and not state._bp_job
  bp.can_send = can_send

  -- THE FEEDER'S OWN RESERVATION, and the third place it used to speak for the
  -- whole pool. builder.lua sets b.reserve_eta = 0 / "repair_feeder" whenever
  -- the goal is repair_pill or defend_pill+repair -- "my seeded job is pending,
  -- nobody else may take the man". A zero-tick reservation rejects EVERY other
  -- row (`reserve(0 < trip N)`) before it can outscore anything, which is the
  -- mode gate's mistake in another form: under BUILDER_POOL_SEEDED_COMPETES the
  -- seeded job is not "pending", it is a ROW, and it wins or loses on its
  -- score like the rest. Measured on the first run of tests/seeded_repair_test
  -- arena A: with the mode gate open the 5-hp repair still read
  -- `BP_DENY ... reason=reserve(0 < trip 346) ... score=74` while the seeded
  -- 1-hp top-up took the man at 20.
  --
  -- Narrow on purpose: ONLY the feeder's own 0, never the wall-shield / sea /
  -- placement reservations, which are about work the man is genuinely needed
  -- for later. The seeded row already waived it for itself (seed_ctx passes
  -- nil); this waives it for the rows it is competing against, and the verdict
  -- line says so.
  local ctx_reserve_eta = bp.reserve_eta
  if C.BUILDER_POOL_SEEDED_COMPETES and ctx_reserve_eta == 0
     and bp.reserve_why == "repair_feeder" then
    ctx_reserve_eta = nil
    bp.reserve_waived = "seeded_competes"
  end

  local ctx = { ok = ok, reason = reason, reserve = reserve,
                reserve_eta = ctx_reserve_eta }

  -- A seeded job (repair_pill arrival / defend->repair handoff) is a row like
  -- any other, but it enters with three gates already answered by the goal
  -- that seeded it:
  --   * the MODE gate -- the tank's whole goal IS this repair;
  --   * the LEASH -- the goal's own danger-blended dispatch range (5 calm,
  --     widening to 12 under fire) is what decided "close enough", and
  --     narrowing that to the leash would change committed-repair behaviour;
  --   * the TREE RESERVE, all of it. The reserve exists to stop an
  --     OPPORTUNISTIC side-quest eating wood the tank's own plans need. A
  --     seeded job IS the tank's own plan -- the whole goal is this repair --
  --     so holding wood back from it reserves the job's wood against the job.
  --     (The goal component always was waived for that reason; 20260903_105448
  --     showed the other two doing the same thing from one step further out:
  --     base 4 + pills 16 + sea 21 = 41 against 21 trees refused a committed
  --     ONE-tree repair, every tick, for 20 minutes.) A seeded row therefore
  --     asks for exactly its own tree cost, `have >= need`, and nothing more.
  -- Nothing else is waived. In particular the UNDER-FIRE clock still applies
  -- (seed_ctx takes d.fire_ok, not `true`): "my goal is this repair" is a
  -- reason to own the man, not a reason to walk him out of a tank that is
  -- being shelled. Every reject in score_row still runs -- MIN_SCORE, the
  -- `have >= need` wood test, ally_repairing, ally_capturing, the reserve ETA,
  -- unreachable, path safety on a non-linear row and the shell gate.
  --
  -- WHAT SEEDING NO LONGER BUYS (BUILDER_POOL_SEEDED_COMPETES, 2026-09-06).
  -- It used to buy three more things that were never part of the bargain: the
  -- front of the sort whatever the row scored; -- because the feeder goal was
  -- excluded from BUILDER_POOL_TRAVEL_GOALS -- a pool-wide `mode_owned` that
  -- shut every OTHER row out behind it; and the feeder's own
  -- `reserve_eta = 0` (see the block above the ctx table), which rejected every
  -- competitor with `reserve(0 < trip N)` for good measure. All three are gone
  -- when the knob is on, and all three come back with it. The seeded row is
  -- scored by the same formula, must clear the same MIN_SCORE, and is ordered
  -- by score; the mode gate its neighbours see is the one they would have seen
  -- had the goal never seeded (M.eligibility). The three waivers above are the
  -- whole of what "seeded" now means, and the goal's interest in its own pill
  -- is priced instead by BUILDER_POOL_GOAL_PILL_BONUS -- a factor IN the score
  -- and printed beside it. With the knob off, the old first-place ordering and
  -- the old exclusion both come back.
  local seed = state._bp_seed
  local seed_ctx = { ok = d.fire_ok, reason = d.fire_reason,
                     reserve = 0,
                     reserve_eta = nil }

  local rows = M.discover(state, world, info)
  local seed_seen = false
  for _, row in ipairs(rows) do
    local rctx = ctx
    if seed and row.mx == seed.mx and row.my == seed.my then
      row.seeded = seed.src
      row.out_of_leash = nil
      row.hard = nil
      seed_seen = true
      rctx = seed_ctx
    end
    M.score_row(state, world, info, now, row, rctx)
  end
  if seed and not seed_seen then
    -- The feeder named a tile ordinary discovery does not offer. The common
    -- case is a LIGHTLY damaged pill: discovery ignores anything missing less
    -- than BUILDER_POOL_TOPUP_MIN_MISSING (one tree's worth), because walking
    -- the man out for two points of armour is not a side-quest worth having.
    -- But a committed repair_pill / defend->repair goal has already decided it
    -- IS worth it, and before this module that dispatch simply happened. So
    -- synthesise the row rather than dropping the seed: the tank goal is
    -- allowed to spend the man on work the pool would not have chosen itself.
    local lst = world.pill_at and world.pill_at[seed.my * 256 + seed.mx]
    local sp = lst and lst[1] and lst[1].pill
    if sp then
      local maxhp = C.PILLS_MAX_HEALTH or 15
      local hp = sp.health or 0
      local row = {
        type = (hp <= 0) and "rebuild" or "topup",
        id = lst[1].id or -1, mx = seed.mx, my = seed.my,
        dist = U.mdist(bit.rshift(info.tankx, 8), bit.rshift(info.tanky, 8),
                       seed.mx, seed.my),
        hp = hp, missing = maxhp - hp, own = sp.owner,
        trees_need = (hp <= 0) and (C.BUILDER_POOL_TREES_REBUILD or 4)
                     or math.max(1, math.ceil((maxhp - hp)
                                              / (C.PILL_REPAIR_AMOUNT or 4))),
        seeded = seed.src,
      }
      M.score_row(state, world, info, now, row, seed_ctx)
      rows[#rows + 1] = row
      seed_seen = true
    else
      -- No pill there at all any more (it died and was collected, or the tile
      -- moved). Edge-triggered so a stale feeder cannot log every tick.
      local skey = string.format("%d:%d:%s", seed.mx, seed.my, tostring(seed.src))
      if state._bp_seed_drop_key ~= skey then
        state._bp_seed_drop_key = skey
        print2(string.format(
          "BP_SEED_DROP t=%d target=(%d,%d) src=%s reason=no_pill_there"
          .. " -- the feeder's goal outlived its pill",
          now, seed.mx, seed.my, tostring(seed.src)))
      end
      state._bp_seed = nil
    end
  end
  if seed_seen then state._bp_seed_drop_key = nil end
  order_rows(rows)
  bp.rows = rows

  -- THE SHELL GATE. A HARD STOP at the moment of dispatch, not a score term,
  -- and deliberately the LAST thing asked: it is the only test here that flies
  -- a real object forward, so it is paid for exactly once -- on the row that
  -- has already passed every other gate and is about to send the man out.
  --
  -- Refusing does not retire the row. The shell is gone in a handful of ticks
  -- and the same row wins the next tick it is clear, which is the whole point:
  -- "not this tick", never "not this job". A refused row falls through to the
  -- next candidate, because a different target is a different walk and may be
  -- perfectly clear of the same round.
  local winner = nil
  if can_send then
    for _, row in ipairs(rows) do
      if not row.reject then
        local hit = danger.lgm_shell_gate(world, info, row.mx, row.my)
        if hit then
          row.shell_hit = hit
          -- Every factor the refusal turned on is on the line: WHOSE shell,
          -- HOW MANY engine ticks from now it lands, and WHICH of the three
          -- endings it is (ends on our hull / runs out of life over open
          -- ground / detonates on the wall or pill the man is standing on).
          row.reject = string.format("shell_will_hit (shell from %s at +%dt, %s)",
                                     hit.src, hit.t, tostring(hit.how))
          -- Deny lines are edge-triggered on the reason STRING and `+%dt`
          -- counts down every tick, so the key drops the countdown: one line
          -- per shell that blocks this row, not one per tick.
          row.reject_key = "shell_will_hit:" .. hit.src
          if not bp.shell_hit then
            hit.mx, hit.my = row.mx, row.my
            bp.shell_hit = hit
            -- The overlay draws the man's PREDICTED walk, so it has to be the
            -- walk the gate actually simulated. Copied only in a debug build:
            -- it is a ~126-number table and it buys nothing at play time.
            if BRAIN_DEBUG_MODE then hit.walk = danger.lgm_shell_gate_walk(hit) end
          end
        else
          winner = row
          break
        end
      end
    end
  end

  -- One verdict line per tick. Not throttled: it is the single line that
  -- explains a tick's worth of builder-pool behaviour, and it is stripped from
  -- opt/ entirely.
  local n_ok = 0
  for _, r in ipairs(rows) do if not r.reject then n_ok = n_ok + 1 end end
  print2(string.format(
    "BUILDER_POOL t=%d owner=%s/%s%s elig=%s cands=%d ok=%d trees=%d/res=%d"
    .. " (base %d + pills %d + goal %d + sea %d) reserve_eta=%s(%s) under_fire=%s job=%s%s",
    now, bp.owner_kind, bp.owner_mode,
    bp.owner_sub and ("/" .. bp.owner_sub) or "",
    ok and "yes" or ("no: " .. tostring(reason)),
    #rows, n_ok, bp.trees, reserve, r_base, r_pills, r_goal, r_sea,
    tostring(bp.reserve_eta or "-"), tostring(bp.reserve_why or "-"),
    d.fire_age and string.format("%dt", d.fire_age) or "never",
    state._bp_job and string.format("%s@(%d,%d)/%s", state._bp_job.type,
      state._bp_job.mx, state._bp_job.my, state._bp_job.phase or "?") or "-",
    -- On the END of the line: the field order up to job= is fixed by
    -- tests/builder_pool_test.py's POOL_RE.
    bp.reserve_waived and (" reserve_waived=" .. bp.reserve_waived) or ""))

  -- BP_DENY: the row that WOULD have won had it not been rejected. One line,
  -- edge-triggered on (row, reason) so a 200-tick block is one line and not
  -- two hundred.
  if not winner and rows[1] then
    local top = rows[1]
    -- The key carries the eligibility verdict as well as the row's own reason,
    -- so a take that moves from plan_position to shoot_pill re-prints instead
    -- of staying silent behind an unchanged row-level reason.
    local key = string.format("%s:%d:%d:%s:%s", top.type, top.mx, top.my,
                              tostring(top.reject_key or top.reject
                                       or (can_send and "?" or "no_man")),
                              ok and "yes" or tostring(reason))
    if state._bp_deny_key ~= key then
      state._bp_deny_key = key
      -- `elig=` as well as `reason=`: the row names the FIRST thing that
      -- stopped it, which is deliberately its own most specific problem (a
      -- row with no wood should not read "mode_owned" -- fixing the mode
      -- would not send it). But then a pool-wide block is invisible on the
      -- deny line whenever any row also has a local problem, and "why is
      -- nothing happening during this take" is exactly the question the line
      -- exists to answer. Both, so the line is self-contained.
      -- trip is printed with its three legs beside it: a denial that turns on
      -- the walk (below_min_score, reserve, unreachable) is unreadable if the
      -- only number on the line is the total. pred= is where the return leg
      -- was walked to, or the tank's own tile.
      --
      -- The legs go on the END, after res=. The field ORDER up to there is
      -- fixed by tests/builder_pool_test.py's DENY_RE, which reads through to
      -- `res=(\d+)` -- inserting them between trip= and trees= stopped that
      -- regex matching at all, which read back as "the pool never denied
      -- anything" and failed variant C on a claim it had nothing to do with.
      print2(string.format(
        "BP_DENY t=%d job=%s target=(%d,%d) reason=%s elig=%s score=%.0f trip=%s"
        .. " trees=%d/%d res=%d out=%s build=%s back=%s pred=%s%s%s",
        now, top.type, top.mx, top.my,
        tostring(top.reject or (can_send and "none" or "no_man")),
        ok and "yes" or tostring(reason),
        top.score or 0, tostring(top.trip or "-"),
        info.trees or 0, top.trees_need or 0, reserve,
        tostring(top.out_ticks or "-"), tostring(top.build_ticks or "-"),
        tostring(top.back_ticks or "-"),
        top.pred_mx and string.format("(%d,%d)/%s", top.pred_mx, top.pred_my,
                                      tostring(top.pred_src or "-"))
                    or ("same/" .. tostring(top.pred_why or "-")),
        top.wedge and (" wedge=" .. top.wedge) or "",
        seed_tail(rows, top, true)))
    end
  elseif winner then
    state._bp_deny_key = nil
  end

  if winner then bp.dispatch = winner end
  return bp
end

-- -------------------------------------------------------------------------
-- rung — called from builder.decide() between Priority 1b and Priority 2.
-- Returns the build command, or nil. All the deciding already happened in
-- update(); this only turns the winner into an engine action and opens the
-- job + claim records, so there is exactly one place that issues LGM orders.
-- -------------------------------------------------------------------------
function M.rung(state, world, info, now)
  local bp = state._builder_pool
  if not bp or not bp.dispatch then return nil end
  local row = bp.dispatch
  local action = (row.type == "farm") and BUILDMODE_FARM or BUILDMODE_PBOX
  state._bp_job = {
    type = row.type, mx = row.mx, my = row.my, pill_id = row.id,
    phase = "outbound", claim_tick = now, dispatch_tick = now,
    eta = row.out_ticks, trip = row.trip,
    claim_eta_tick = now + (row.out_ticks or 0),
    trees_at_dispatch = info.trees or 0,
    hp_at_dispatch = row.hp or 0,
    seeded = row.seeded,
  }
  bp.job = state._bp_job
  bp.dispatch = nil
  local seed = state._bp_seed
  state._bp_seed = nil
  -- Feeder bookkeeping. A seeded repair is still a repair as far as the rest
  -- of the brain is concerned: init.lua clears the goal on _repair_dispatched
  -- (the LGM finishes autonomously) and uses _repair_dispatch_eta for the
  -- lgmd advert's precise walk-sim ETA. Emitting REPAIR_DISPATCH_FIRED here
  -- keeps ONE dispatch record for a repair however it was fed.
  if row.seeded and seed and seed.src then
    state._repair_dispatched   = true
    state._repair_dispatch_eta = row.out_ticks
    print2(string.format(
      "REPAIR_DISPATCH_FIRED t=%d pill=(%d,%d) action=BUILDMODE_PBOX eta=%d goal=%s hit_age=%s",
      now, row.mx, row.my, row.out_ticks or 0,
      (state.goal and state.goal.kind) or "?",
      seed.uf_age and tostring(seed.uf_age) or "-"))
  end
  -- The chips inside the (...) already carry out{}/build{}/back{}/pred{} (and
  -- wedge{} on a farm row) -- M.score_terms is the one place that string is
  -- built. The legs are repeated in plain key=value form after front= as well,
  -- because that half of the line is what the arena tests and a grep read, and
  -- the field ORDER up to front= is fixed by tests/repair_priority_test.py's
  -- DISP_RE -- so this goes on the end and nothing moves.
  print2(string.format(
    "BP_DISPATCH t=%d job=%s target=(%d,%d)%s score=%.0f (%s)%s"
    .. " eta=%s trip=%s trees=%d-%d front=%d owner=%s/%s claim=%d"
    .. " out=%s build=%s back=%s pred=%s%s%s",
    now, row.type, row.mx, row.my,
    row.seeded and (" seeded_by=" .. tostring(row.seeded)) or "",
    row.score or 0, M.score_terms(row), row.linear and " [linear]" or "",
    tostring(row.out_ticks or "-"), tostring(row.trip or "-"),
    info.trees or 0, row.trees_need or 0, row.front_dist or -1,
    bp.owner_kind, bp.owner_mode, now,
    tostring(row.out_ticks or "-"), tostring(row.build_ticks or "-"),
    tostring(row.back_ticks or "-"),
    row.pred_mx and string.format("(%d,%d)/%s", row.pred_mx, row.pred_my,
                                  tostring(row.pred_src or "-"))
                or ("same/" .. tostring(row.pred_why or "-")),
    row.wedge and (" wedge=" .. row.wedge) or "",
    -- The seeded row, when the man went somewhere ELSE: the goal asked for a
    -- job and a better-scoring one won, which is exactly what
    -- BUILDER_POOL_SEEDED_COMPETES is for. `seeded_by=` is already up beside
    -- target= when the winner IS the seeded row, so it is not repeated here.
    seed_tail(bp.rows or {}, row, false)))
  -- BP_PRED: the ROUTE the return leg was priced against, tile by tile, on the
  -- one tick it actually decided something. The trip on the dispatch line is
  -- only hand-checkable if the reader can see the path pred{} was read off --
  -- "the tank will be at (129,126)" means nothing without "because it is
  -- driving 123 -> 124 -> ... -> 131 and that tile is 71 ticks along it".
  -- One line per dispatch, never per tick, and built INSIDE the print2 call so
  -- lua_strip takes the whole thing (route_str included) out of opt/.
  print2(string.format(
    "BP_PRED t=%d job=%s target=(%d,%d) src=%s horizon=%s pred=%s wp=%s/%s"
    .. " route=%s",
    now, row.type, row.mx, row.my, tostring(row.pred_src or "-"),
    tostring(row.pred_horizon or "-"),
    row.pred_mx and string.format("(%d,%d)", row.pred_mx, row.pred_my)
                or ("same/" .. tostring(row.pred_why or "-")),
    tostring(row.pred_route_i or 0), tostring(row.pred_route_n or 0),
    route_str(state._bp_route)))
  return { x = row.mx, y = row.my, action = action }
end

-- -------------------------------------------------------------------------
-- Panel section (pool_grid JSON, section BUILDER_POOL_PANEL_IDX).
--
-- NOT a new pool: 1..10 keep the 2x5 grid and 11..14 keep their strips, so old
-- recordings load unchanged. Three header lines then one row per candidate:
--
--   BUILDER  owner=attack_pill/wall_shield  (man out, eta 60)
--     eligibility: mode=idle-ish - under_fire=no(38t) - reserve_eta=110 - trees 14(res 8)
--     active: rebuild p15 (outbound, eta 44t)
--
-- Rows use the shared renderer (cost + formula short||long), so the long form
-- is hand-checkable exactly like every other pool row. cost is -score, because
-- the renderer sorts and colours ASCENDING (cheapest = best) and the pool
-- scores DESCENDING.
-- -------------------------------------------------------------------------
function M.panel_section(state)
  local bp = state._builder_pool
  if not bp then return nil end
  local hdr = {}
  local job = bp.job
  -- Name the actual obstacle rather than "unavailable": "the man is walking a
  -- job", "he is dead", "we are afloat" and "he is out on a wall shield" are
  -- four different situations and only one of them is the pool's doing.
  local why_no_man = ""
  if not bp.can_send then
    if job then
      why_no_man = string.format("  (man out on %s @(%d,%d), %s)",
                                 job.type, job.mx, job.my, job.phase or "?")
    elseif bp.man_status == C.LGM_DEAD then     why_no_man = "  (man dead)"
    elseif bp.man_status == C.LGM_MOVING then   why_no_man = "  (man out, not on a pool job)"
    elseif bp.inboat then                       why_no_man = "  (afloat)"
    else                                        why_no_man = "  (man unavailable)" end
  end
  hdr[#hdr + 1] = string.format("owner=%s/%s%s%s",
    bp.owner_kind, bp.owner_mode,
    bp.owner_sub and ("/" .. bp.owner_sub) or "", why_no_man)
  hdr[#hdr + 1] = string.format(
    "eligibility: %s | under_fire=%s | reserve_eta=%s | trees %d (res %d = base %d + pills %d + goal %d + sea %d)",
    bp.ok and "OK" or ("DENY " .. tostring(bp.reason)),
    (bp.detail and bp.detail.fire_age)
      and string.format("%dt ago (%s)", bp.detail.fire_age,
                        tostring(bp.detail.fire_why)) or "never",
    -- WAIVED is printed beside the number, never instead of it: the panel has
    -- to show both what the goal asked for and that the pool did not charge
    -- the other rows for it (BUILDER_POOL_SEEDED_COMPETES).
    string.format("%s (%s)%s", tostring(bp.reserve_eta or "-"),
                  tostring(bp.reserve_why or "none"),
                  bp.reserve_waived
                    and (" WAIVED: " .. bp.reserve_waived) or ""),
    bp.trees, bp.reserve, bp.r_base, bp.r_pills, bp.r_goal, bp.r_sea)
  if job then
    hdr[#hdr + 1] = string.format("active: %s @(%d,%d) %s eta=%st claim=t%d%s",
      job.type, job.mx, job.my, job.phase or "?",
      tostring(job.eta or "-"), job.claim_tick or 0,
      job.seeded and (" seeded_by=" .. tostring(job.seeded)) or "")
  else
    local last = state._bp_last
    hdr[#hdr + 1] = last
      and string.format("active: none (last %s @(%d,%d) %s at t%d%s)",
                        last.type, last.mx, last.my, last.outcome, last.tick,
                        last.lgm_dead and ", LGM killed" or "")
      or "active: none"
  end

  local rows = {}
  local cap = C.BUILDER_POOL_PANEL_ROWS or 8
  for i = 1, math.min(#bp.rows, cap) do
    local r = bp.rows[i]
    rows[#rows + 1] = {
      id = (r.id and r.id >= 0) and r.id or (r.my * 256 + r.mx),
      mx = r.mx, my = r.my,
      -- Renderer convention: lower is better. The pool's score is
      -- higher-is-better, so the row cost is its negation; a rejected row
      -- shows INF like every other rejected row in the grid.
      cost = r.reject and 1e30 or -(r.score or 0),
      -- Built here, on the panel's cold path, rather than stored on the row by
      -- score_row (see M.row_formula).
      formula = M.row_formula(r),
      stale = 0,
      reject = r.reject and (r.reject:match("^[a-z_]+") or r.reject) or nil,
      reject_remaining = 0,
    }
  end
  return {
    idx = C.BUILDER_POOL_PANEL_IDX or 15,
    name = "BUILDER",
    weight = 1.0,
    winner_id = (bp.job and bp.job.pill_id) or -1,
    hdr = hdr,
    rows = rows,
  }
end

-- -------------------------------------------------------------------------
-- Map overlay for the SHELL GATE refusal — the picture of the arithmetic in
-- danger.lgm_shell_gate, and nothing the gate did not compute:
--
--   * the man's PREDICTED walk, as the polyline of the exact per-tick
--     positions the gate walked (cpf_lgm_walk_path), not a straight line to
--     the target and not the route he would eventually take -- only the first
--     LGM_SHELL_PREDICT_TICKS of it, because that is all the gate looked at;
--   * a red ring of LGM_SHELL_KILL_RADIUS_WU (128 WU = half a tile) at the
--     predicted IMPACT POINT, which is the engine's own blast radius, plus a
--     line from it to where the man is predicted to be standing at that tick;
--   * a label naming the shell's source and the tick offset, the same two
--     facts the reject string carries.
--
-- Only drawn while the gate is actually refusing (it lives on bp.shell_hit,
-- which is rebuilt each tick), and the walk polyline only in a debug build,
-- because that is the only build that copies the walk.
-- -------------------------------------------------------------------------
function M.draw_shell_gate(state, info)
  local bp = state._builder_pool
  local hit = bp and bp.shell_hit
  if not hit then return end
  local ix, iy = hit.sx / 256.0, hit.sy / 256.0
  local lx, ly = hit.lx / 256.0, hit.ly / 256.0
  if hit.walk and hit.walk_n and hit.walk_n > 1 then
    local px = hit.walk[1] / 256.0
    local py = hit.walk[2] / 256.0
    for i = 2, hit.walk_n do
      local nx = hit.walk[i * 2 - 1] / 256.0
      local ny = hit.walk[i * 2] / 256.0
      viz.line("builder_pool_shell_gate", px, py, nx, ny, 255, 200, 90, 150)
      px, py = nx, ny
    end
  end
  -- The kill radius, at the impact point, in the engine's own units.
  viz.circle("builder_pool_shell_gate", ix, iy,
             (C.LGM_SHELL_KILL_RADIUS_WU or 128) / 256.0, 255, 70, 70, 230)
  viz.line("builder_pool_shell_gate", ix, iy, lx, ly, 255, 70, 70, 200)
  viz.text("builder_pool_shell_gate", ix, iy - 0.8,
    string.format("shell %s ends %s at +%dt -> kills the man", tostring(hit.src),
                  tostring(hit.how), hit.t or 0),
    "center", 255, 90, 90, 240)
end

-- -------------------------------------------------------------------------
-- Map overlay for the ACTIVE job: a line from the tank to the target, a ring
-- on the target, and the ETA label. Matches the code exactly -- the ring is on
-- the tile the job record names, the line starts at the tank (the man's start
-- AND end point, which is what the trip cost is measured over), and the label
-- carries the same phase/eta the panel's active line shows.
-- -------------------------------------------------------------------------
function M.draw(state, info)
  M.draw_shell_gate(state, info)
  local job = state._bp_job
  if not job then return end
  local r, g, bcol = 120, 220, 255            -- pale blue: the man's own errands
  if job.phase == "working" then r, g, bcol = 120, 255, 120 end
  if job.phase == "returning" then r, g, bcol = 200, 200, 120 end
  local tfx, tfy = info.tankx / 256.0, info.tanky / 256.0
  viz.line("builder_pool_job", tfx, tfy, job.mx + 0.5, job.my + 0.5, r, g, bcol, 160)
  viz.circle("builder_pool_job", job.mx + 0.5, job.my + 0.5, 0.6, r, g, bcol, 220)
  viz.text("builder_pool_job", job.mx + 0.5, job.my - 0.9,
    string.format("%s %s eta=%st", job.type, job.phase or "?",
                  tostring(job.eta or "-")),
    "center", r, g, bcol, 240)
  -- The leashes the discovery actually used, so the overlay cannot claim a
  -- radius the code does not. TWO of them since 2026-09-05: repair rows reach
  -- BUILDER_POOL_REPAIR_LEASH (green, outer) and the farm row keeps
  -- BUILDER_POOL_LEASH (blue). Only one circle is drawn when they are equal
  -- (preset=keel), which is the honest picture of that configuration.
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  local farm_leash = C.BUILDER_POOL_LEASH or 8
  local rep_leash  = M.repair_leash()
  viz.circle("builder_pool_leash", tmx + 0.5, tmy + 0.5,
             farm_leash, 90, 140, 200, 70)
  if rep_leash ~= farm_leash then
    viz.circle("builder_pool_leash", tmx + 0.5, tmy + 0.5,
               rep_leash, 120, 210, 140, 70)
  end
end

return M
