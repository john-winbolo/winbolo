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
--   farm     opportunistic wood
--
-- Print2 contract (one line per tick for the verdict, one per event):
--   BUILDER_POOL t=.. owner=.. elig=..      the per-tick verdict + counts
--   BP_DISPATCH  t=.. job=.. target=..      the man just left on a side-quest
--   BP_DENY      t=.. job=.. reason=..      a row that could have won, did not
--   BP_DONE      t=.. job=.. outcome=..     he came home and the job took
--   BP_ABORT     t=.. job=.. why=..         he came home and it did not
--   BP_SEED_DROP t=.. target=.. reason=..   a feeder named a pill that is gone
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
-- lgm_trip: outbound walk ticks (real sim), round trip, and reachability.
--
-- The wall-shield / repair dispatch walk-time math, unchanged: the C
-- tick-by-tick LGM sim with the DESTINATION blessed, so a live pill or base AT
-- the target does not self-block (the man works ON that square). Pills and
-- bases in the PATH still block -- a friendly pill between us and the spot
-- really does stop him.
--
-- Round trip = 2 x outbound + LGM_BUILD_TIME. Returns nil when unreachable.
-- -------------------------------------------------------------------------
function M.lgm_trip(info, mx, my)
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
  if math.abs(mx - tmx) + math.abs(my - tmy) <= 1 then
    local out = C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16
    return out, 2 * out + (C.LGM_BUILD_TIME or 20)
  end
  local out = cpf_lgm_travel_ticks_map(tmx, tmy, mx, my, mx, my,
                                       C.BUILDER_POOL_LGM_MAX_TICKS or 2000,
                                       C.BUILDER_POOL_LGM_STUCK_TICKS or 150)
  if out == nil or out < 0 then return nil end
  if out == 0 then out = C.BUILDER_POOL_GRASS_TICKS_PER_TILE or 16 end
  return out, 2 * out + (C.LGM_BUILD_TIME or 20)
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
        -- "defend-watch" travel class. A defender with goal.repair is NOT --
        -- that one FEEDS the pool (seed_job) rather than competing with it.
        local travel_goal = (C.BUILDER_POOL_TRAVEL_GOALS or {})[goal.kind]
                            and not goal.repair
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
-- Dead and damaged friendly OR ALLIED pills within the leash, plus one
-- opportunistic farm tile. The dead-pill test mirrors filter_repair_pill's
-- REPAIR_DEAD_FILTER discovery -- 0 HP, on the ground, not blocked, not the
-- tile we are capturing / repositioning -- because the two must agree about
-- which corpses are worth wood. It differs on ONE point, deliberately:
-- filter_repair_pill refuses to rebuild a corpse capture_pill could just pick
-- up (rebuilding makes it un-grabbable and wastes the kill). The pool does
-- not, because the pool exists for the case where the tank is NOT going to go
-- and get it -- that is the whole incident. A corpse the tank has actually
-- committed to collecting is still refused, by the our_target guard below.
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
  local leash = C.BUILDER_POOL_LEASH or 8
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
      if d <= leash * 2 then
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
            hard = blk,                       -- discovery-level refusal, shown as REJECT
            out_of_leash = (d > leash) or nil,
          }
        end
      end
    end
  end
  -- One farm row: the nearest forest inside the leash. Opportunistic wood has
  -- no clock, so one candidate is enough -- it exists to give the man
  -- something to do on a genuinely quiet tick, not to be optimised.
  if (info.trees or 0) < (C.TREE_OPPORTUNISTIC_MAX or 20) then
    local best_d, best_x, best_y = math.huge, nil, nil
    for dy = -leash, leash do
      for dx = -leash, leash do
        local fx, fy = tmx + dx, tmy + dy
        if U.in_map(fx, fy) and U.ttype(fx, fy) == C.T_FOREST then
          local d = U.mdist(tmx, tmy, fx, fy)
          -- Deterministic: nearest wins, ties by tile key.
          if d < best_d or (d == best_d and best_y
                            and (fy * 256 + fx) < (best_y * 256 + best_x)) then
            best_d, best_x, best_y = d, fx, fy
          end
        end
      end
    end
    if best_x then
      out[#out + 1] = { type = "farm", id = -(best_y * 256 + best_x),
                        mx = best_x, my = best_y, dist = best_d,
                        trees_need = C.BUILDER_POOL_TREES_FARM or 0 }
    end
  end
  return out
end

-- -------------------------------------------------------------------------
-- Scoring, and the per-candidate half of the eligibility stack.
--
--   value = BASE[type] (+ TOPUP_PER_HP x missing) (+ front clock)
--   cost  = TRIP_W x round_trip_ticks + DANGER_W x threat_at_target
--   score = value - cost
--
-- The front clock is deliberately absent from FARM: a forest is not going
-- anywhere and nobody can steal it, which is exactly why rebuild outranks farm
-- always -- the 200-vs-15 base gap is wider than any trip term inside the
-- leash can close.
--
-- Leaves every chip the pool-grid row prints on the row itself; M.row_formula
-- assembles them into the short||long pair on the panel's cold path, so every
-- number on the row stays reproducible from the chips on that row.
-- -------------------------------------------------------------------------
function M.score_row(state, world, info, now, row, ctx)
  local FMAX = C.BUILDER_POOL_FRONT_MAX_TILES or 12
  local fd = M.front_distance(state, row.mx, row.my)
  row.front_dist = fd
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

  local out_ticks, trip = M.lgm_trip(info, row.mx, row.my)
  row.out_ticks, row.trip = out_ticks, trip
  local dgr = threat.at(row.mx, row.my) or 0
  row.danger = dgr
  if trip then
    row.c_trip   = (C.BUILDER_POOL_TRIP_W or 0.5) * trip
    row.c_danger = (C.BUILDER_POOL_DANGER_W or 1.5) * dgr
    row.score = row.value - row.c_trip - row.c_danger
  else
    row.c_trip, row.c_danger, row.score = 0, 0, -1e9
  end

  -- ── per-candidate eligibility, in cost order (cheap tests first) ──────
  local reject = row.hard and ("discovery:" .. row.hard) or nil
  if not reject and row.out_of_leash then
    reject = string.format("out_of_leash (%d > %d)", row.dist,
                           C.BUILDER_POOL_LEASH or 8)
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
  if not reject then
    -- Most expensive test last: the danger sample along the walk.
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
  return row
end

-- -------------------------------------------------------------------------
-- The pool-grid detail string for one scored row: the short line before "||"
-- and the long term-by-term breakdown after it.
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
  return string.format(
    "%s val{base %.0f + hp %.0f + front %.0f(d=%d/%d)} - trip{%.2fx%s=%.0f}"
    .. " - danger{%.2fx%.0f=%.0f} = %s%s"
    .. "||%s. value = BUILDER_POOL_VALUE_%s(%.0f)%s + FRONT_URGENCY(%d) x"
    .. " max(0,(FRONT_MAX(%d) - front_dist(%d)))/FRONT_MAX = %.0f."
    .. " cost = TRIP_W(%.2f) x round_trip(%s ticks: 2 x walk_sim(%s) + LGM_BUILD_TIME(%d))"
    .. " + DANGER_W(%.2f) x threat.at(%.0f) = %.0f."
    .. " score = value - cost = %s (min to fire: %d). trees need %d, have %d,"
    .. " reserved %d. leash %d, dist %d.%s",
    label,
    row.v_base, row.v_hp, row.v_front, fd, FMAX,
    C.BUILDER_POOL_TRIP_W or 0.5, tostring(trip or "-"), row.c_trip,
    C.BUILDER_POOL_DANGER_W or 1.5, dgr, row.c_danger,
    score_str, reject and (" REJECT " .. reject) or "",
    label, string.upper(row.type),
    row.v_base,
    (row.type == "topup")
      and string.format(" + TOPUP_PER_HP(%d) x missing(%d) = %.0f",
                        C.BUILDER_POOL_TOPUP_PER_HP or 6, row.missing or 0, row.v_hp)
      or ((row.type == "farm")
          and string.format(
            " + FARM_URGENCY(%d) x max(0, FARM_LOW_TREES(%d) - trees(%d)) = %.0f",
            C.BUILDER_POOL_FARM_URGENCY or 12, C.BUILDER_POOL_FARM_LOW_TREES or 12,
            row.f_trees or 0, row.v_hp)
          or ""),
    C.BUILDER_POOL_FRONT_URGENCY or 120, FMAX, fd, row.value,
    C.BUILDER_POOL_TRIP_W or 0.5, tostring(trip or "-"), tostring(out_ticks or "-"),
    C.LGM_BUILD_TIME or 20,
    C.BUILDER_POOL_DANGER_W or 1.5, dgr, row.c_trip + row.c_danger,
    score_str, C.BUILDER_POOL_MIN_SCORE or 20,
    row.trees_need or 0, row.f_trees or 0, row.f_reserve,
    C.BUILDER_POOL_LEASH or 8, row.dist or -1,
    reject and (" REJECTED: " .. reject) or " ACCEPTED.")
end

-- Deterministic ordering: a SEEDED row first (a feeder's job outranks any
-- side-quest by construction -- the tank's own goal is that repair), then best
-- score, then type rank (rebuild before topup before farm), then tile key.
--
-- Seeding is a separate sort key rather than a bonus added to the score,
-- because the score is PRINTED and has to stay reproducible from the chips
-- beside it: "score=1000089 (val 184 - trip 95 - danger 0)" does not add up
-- and cannot be hand-checked, which is the whole contract for these rows.
-- No pairs() order ever reaches this sort.
local function order_rows(rows)
  table.sort(rows, function(a, b)
    local sa, sb = a.seeded and 1 or 0, b.seeded and 1 or 0
    if sa ~= sb then return sa > sb end
    if a.score ~= b.score then return a.score > b.score end
    local ra, rb = TYPE_RANK[a.type] or 9, TYPE_RANK[b.type] or 9
    if ra ~= rb then return ra < rb end
    return (a.my * 256 + a.mx) < (b.my * 256 + b.mx)
  end)
  return rows
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
  else
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
-- A seeded job bypasses the MODE gate (the tank's whole goal IS this repair)
-- and the leash (the goal's own danger-blended dispatch range decides how
-- close is close enough), but nothing else: under-fire, trees, path safety and
-- the ally claim all still apply, and are all evaluated by the same score_row.
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
  local effective_max = math.max(blend_max, C.BUILDER_POOL_LEASH or 8)
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
  end
  local _hold = enemy_hold or under_fire_hold
  local ticks = (in_range and has_trees and not _hold)
    and M.lgm_trip(info, px, py) or nil
  local can_dispatch = in_range and has_trees and not _hold and ticks ~= nil


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
  local leash = C.BUILDER_POOL_LEASH or 8
  if U.mdist(tmx, tmy, p.mx, p.my) > leash then return nil end
  local maxhp = C.PILLS_MAX_HEALTH or 15
  local hp = p.health or 0
  local need = (hp <= 0) and (C.BUILDER_POOL_TREES_REBUILD or 4)
               or math.ceil((maxhp - hp) / (C.PILL_REPAIR_AMOUNT or 4))
  if (info.trees or 0) < need then return nil end
  local out = M.lgm_trip(info, p.mx, p.my)
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

  local ctx = { ok = ok, reason = reason, reserve = reserve,
                reserve_eta = bp.reserve_eta }

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
  -- being shelled. Seeding only reorders the row to the front of the sort;
  -- every reject in score_row still runs.
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
      end
      state._bp_seed = nil
    end
  end
  if seed_seen then state._bp_seed_drop_key = nil end
  order_rows(rows)
  bp.rows = rows

  local winner = nil
  if can_send then
    for _, row in ipairs(rows) do
      if not row.reject then winner = row; break end
    end
  end

  -- One verdict line per tick. Not throttled: it is the single line that
  -- explains a tick's worth of builder-pool behaviour, and it is stripped from
  -- opt/ entirely.
  local n_ok = 0
  for _, r in ipairs(rows) do if not r.reject then n_ok = n_ok + 1 end end

  -- BP_DENY: the row that WOULD have won had it not been rejected. One line,
  -- edge-triggered on (row, reason) so a 200-tick block is one line and not
  -- two hundred.
  if not winner and rows[1] then
    local top = rows[1]
    -- The key carries the eligibility verdict as well as the row's own reason,
    -- so a take that moves from plan_position to shoot_pill re-prints instead
    -- of staying silent behind an unchanged row-level reason.
    local key = string.format("%s:%d:%d:%s:%s", top.type, top.mx, top.my,
                              tostring(top.reject or (can_send and "?" or "no_man")),
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
  end
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
    string.format("%s (%s)", tostring(bp.reserve_eta or "-"),
                  tostring(bp.reserve_why or "none")),
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
-- Map overlay for the ACTIVE job: a line from the tank to the target, a ring
-- on the target, and the ETA label. Matches the code exactly -- the ring is on
-- the tile the job record names, the line starts at the tank (the man's start
-- AND end point, which is what the trip cost is measured over), and the label
-- carries the same phase/eta the panel's active line shows.
-- -------------------------------------------------------------------------
function M.draw(state, info)
  local job = state._bp_job
  if not job then return end
  local r, g, bcol = 120, 220, 255            -- pale blue: the man's own errands
  if job.phase == "working" then r, g, bcol = 120, 255, 120 end
  if job.phase == "returning" then r, g, bcol = 200, 200, 120 end
  local tfx, tfy = info.tankx / 256.0, info.tanky / 256.0
  -- The leash the discovery actually used, so the overlay cannot claim a
  -- radius the code does not.
  local tmx = bit.rshift(info.tankx, 8)
  local tmy = bit.rshift(info.tanky, 8)
end

return M
