-- =========================================================================
-- test_ping_defend_repair.lua — standalone unit tests for THE PING DEFEND
-- REPAIR (knob C.PING_DEFEND_REPAIR, 2026-09-26): a bot that holds a human's
-- bot-command ping defend order on a damaged friendly pill, with its tank
-- within PING_DEFEND_REPAIR_RANGE tiles, forces the man to repair it
-- (harvesting first when it carries no wood).
--
-- Drives builder_pool.update + builder_pool.rung directly.  No engine: the
-- walk sim is Manhattan x 16 ticks, the danger / threat / front-line reads
-- are stubbed, and the world is two or three pills and a few forest tiles.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_ping_defend_repair.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_ping_defend_repair.lua
-- (the print2 lines are stripped there, so the log checks are skipped).
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- print2 stand-in that keeps the lines, installed before any module loads.
local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local BP     = require("builder_pool")

-- Is this the source copy (print2 lines present) or opt/ (stripped)?
local HAS_LOGS
do
  local f = io.open("builder_pool.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  HAS_LOGS = src:find("print2(", 1, true) ~= nil   -- opt/ strips every call
end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
local FOREST = {}                       -- [my*256+mx] = true
U.ttype = function(mx, my)
  return FOREST[my * 256 + mx] and C.T_FOREST or C.T_GRASS
end
_G.cpf_lgm_travel_ticks_map = function(fx, fy, tx, ty)
  return (math.abs(fx - tx) + math.abs(fy - ty)) * 16
end
_G.BUILDMODE_PBOX = 3
_G.BUILDMODE_FARM = 1
local FIRE_AGE = nil                    -- tank_fire_age answer
local SHELL = {}                        -- [my*256+mx] = hit
danger.update_fire_clock      = function() end
danger.tank_fire_age          = function() return FIRE_AGE, FIRE_AGE and "hit" or nil end
danger.lgm_shell_gate         = function(_, _, mx, my) return SHELL[my * 256 + mx] end
danger.lgm_path_safe_enhanced = function() return true end
threat.at          = function() return 0 end
PP.on_front_line   = function() return false end
AS.init()

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end
local function logged(pat)
  for _, s in ipairs(LOG) do if s:find(pat) then return s end end
  return nil
end
local function check_log(name, pat)
  if not HAS_LOGS then return end
  local s = logged(pat)
  check(name, s ~= nil, s or "no line")
end

-- ── Fixture ──────────────────────────────────────────────────────────────
-- Tank at (100,100).  Pill 7 (ours, pinged) at (106,100), hp 10: a topup
-- row worth 30 x 5 - 0.25 x 212 = 97.  Pill 8 (ours, DEAD) at (100,104): a
-- rebuild worth 30 x 15 - 0.25 x 148 = 413 -- the competing builder job that
-- beats the ping's x2 weight (194) today.
local TMX, TMY = 100, 100
local function pill(mx, my, hp, owner)
  return { mx = mx, my = my, health = hp, owner = owner or "friendly" }
end
local function world_of(pills)
  local w = { pills = pills, pill_at = {} }
  for id, p in pairs(pills) do
    w.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } }
  end
  return w
end
local function fresh_state(order)
  return {
    player_number = 1, tick = 1000,
    builder = { mode = "suppressed" },
    goal = { kind = "defend_pill", target_id = 7, alarm = true },
    _order = order,
  }
end
local function ping_order(tid) return { kind = "defend_pill", tid = tid or 7, ping = true, sender = 0 } end
local function info_at(mx, my, trees)
  return { tankx = mx * 256 + 128, tanky = my * 256 + 128, trees = trees or 10,
           man_status = C.LGM_INTANK, inboat = false, armour = 40,
           carried_pills = 0, player_number = 1 }
end
local NOW = 1000
local function run(state, world, info, now)
  now = now or NOW
  state.tick = now
  local bp = BP.update(state, world, info, now)
  local cmd = BP.rung(state, world, info, now)
  return bp, cmd
end
local function forced_row(bp)
  for _, r in ipairs(bp.rows) do if r.forced then return r end end
  return nil
end
local function snapshot(bp, cmd)
  local t = {}
  for _, r in ipairs(bp.rows) do
    t[#t + 1] = string.format("%s@%d,%d s=%.3f r=%s f=%s", r.type, r.mx, r.my,
                              r.score or 0, tostring(r.reject), tostring(r.forced))
  end
  t[#t + 1] = cmd and string.format("cmd %d,%d a=%d", cmd.x, cmd.y, cmd.action) or "cmd nil"
  return table.concat(t, " | ")
end
local function base_world()
  return world_of({ [7] = pill(106, 100, 10), [8] = pill(100, 104, 0) })
end

-- ── 0. Knobs ─────────────────────────────────────────────────────────────
print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("PING_DEFEND_REPAIR default true", C.PING_DEFEND_REPAIR == true)
check("PING_DEFEND_REPAIR_RANGE default 10", C.PING_DEFEND_REPAIR_RANGE == 10)
check("PING_DEFEND_REPAIR_BONUS default 1000", C.PING_DEFEND_REPAIR_BONUS == 1000)
check("PRESETS.keel PING_DEFEND_REPAIR = false", keel.PING_DEFEND_REPAIR == false)
check("PRESETS.keel lists PING_DEFEND_REPAIR_RANGE", keel.PING_DEFEND_REPAIR_RANGE == 10)
check("PRESETS.keel lists PING_DEFEND_REPAIR_BONUS", keel.PING_DEFEND_REPAIR_BONUS == 1000)

-- ── 1. Master off = no change ────────────────────────────────────────────
print("1. master off = exactly today's pool")
do
  C.PING_DEFEND_REPAIR = false
  local bp1, c1 = run(fresh_state(ping_order()), base_world(), info_at(TMX, TMY))
  local s_off_order = snapshot(bp1, c1)
  local bp2, c2 = run(fresh_state(nil), base_world(), info_at(TMX, TMY))
  local s_off_none = snapshot(bp2, c2)
  C.PING_DEFEND_REPAIR = true
  local bp3, c3 = run(fresh_state(nil), base_world(), info_at(TMX, TMY))
  local s_on_none = snapshot(bp3, c3)
  check("off + order == off + no order", s_off_order == s_off_none, s_off_order)
  check("off + order == on + no order (knob inert without an order)", s_off_order == s_on_none, s_on_none)
  check("off: the rebuild (competing job) wins", c1 and c1.x == 100 and c1.y == 104, c1 and (c1.x .. "," .. c1.y))
  check("off: no forced row", forced_row(bp1) == nil)
  check("off: bp.force nil", bp1.force == nil and bp1.force_why == "knob_off", bp1.force_why)
  -- The existing ping weight alone still loses to the rebuild.
  C.PING_DEFEND_REPAIR = false
  local st = fresh_state(ping_order())
  st._repair_ping = { tid = 7, sender = 0, until_tick = NOW + 500 }
  local bp4, c4 = run(st, base_world(), info_at(TMX, TMY))
  local t7
  for _, r in ipairs(bp4.rows) do if r.id == 7 then t7 = r end end
  check("off: ping x2 weight applied to pill 7 (194)", t7 and t7.goal_src == "ping" and math.floor(t7.score + 0.5) == 194, t7 and t7.score)
  check("off: ...and the rebuild still wins (today's bug)", c4 and c4.x == 100 and c4.y == 104)
  C.PING_DEFEND_REPAIR = true
end

-- ── 2. Damaged pill + tank within 10 = forced repair beats the rebuild ───
print("2. damaged pill, tank 6 tiles away -> forced repair wins")
do
  LOG = {}
  local st = fresh_state(ping_order())
  st._repair_ping = { tid = 7, sender = 0, until_tick = NOW + 500 }
  local bp, cmd = run(st, base_world(), info_at(TMX, TMY))
  local fr = forced_row(bp)
  check("force on", bp.force ~= nil and bp.force.step == "repair", bp.force_why)
  check("forced row is pill 7's topup", fr and fr.id == 7 and fr.type == "topup" and fr.forced == "repair")
  check("forced row sorted first", bp.rows[1] == fr)
  check("dispatch goes to pill 7 (106,100) as a PBOX", cmd and cmd.x == 106 and cmd.y == 100 and cmd.action == BUILDMODE_PBOX,
        cmd and (cmd.x .. "," .. cmd.y))
  check("job carries forced=repair", st._bp_job and st._bp_job.forced == "repair")
  check("NOT seeded: the defend goal is not cleared", st._repair_dispatched == nil)
  -- Hand-computable score: value - tripcost = raw; raw + bonus = score.
  check("raw = 30 x 5 - 0.25 x 212 = 97", fr and fr.raw_score == 150 - 0.25 * 212, fr and fr.raw_score)
  check("score = raw + 1000 (no x2 weight on a forced row)", fr and fr.score == fr.raw_score + 1000 and fr.goal_w == 1.0, fr and fr.score)
  check("needs 1 tree", fr and fr.trees_need == 1, fr and fr.trees_need)
  local terms = fr and BP.score_terms(fr) or ""
  check("score_terms shows bp_raw + ping_force + forced", terms:find("= bp_raw{97} + ping_force{1000} forced{repair}", 1, true) ~= nil, terms)
  local f = fr and BP.row_formula(fr) or ""
  check("row_formula chain", f:find("bp_raw(97) + ping_force(1000) = 1097", 1, true) ~= nil, f:sub(1, 200))
  check("row_formula has ping_force: and forced: segments",
        f:find("|ping_force:PING_DEFEND_REPAIR_BONUS(1000)", 1, true) and f:find("|forced:repair", 1, true))
  local ps = BP.panel_section(st)
  local hdr_ok = false
  for _, h in ipairs(ps.hdr) do if h:find("PING REPAIR FORCED: pill #7", 1, true) then hdr_ok = true end end
  check("panel header line names the force", hdr_ok and #ps.hdr <= 4, #ps.hdr)
  check_log("PING_REPAIR_FORCE start line", "^PING_REPAIR_FORCE t=1000 pill=7@%(106,100%) from p0 hp=10/15 dist=6.0<=10 step=repair")
  check_log("PING_REPAIR_FORCE_DISPATCH line", "^PING_REPAIR_FORCE_DISPATCH t=1000 step=repair job=topup target=%(106,100%)")
  check_log("BP_DISPATCH carries forced=repair", "^BP_DISPATCH .* forced=repair$")
end

-- ── 3. Tank at 11+ tiles = no force (existing bonus only) ────────────────
print("3. tank 11 tiles away -> not forced")
do
  LOG = {}
  local w = world_of({ [7] = pill(111, 100, 10), [8] = pill(100, 104, 0) })
  local st = fresh_state(ping_order())
  st._repair_ping = { tid = 7, sender = 0, until_tick = NOW + 500 }
  local bp, cmd = run(st, w, info_at(TMX, TMY))
  check("force off: out_of_range", bp.force == nil and bp.force_why == "out_of_range", bp.force_why)
  check("no forced row", forced_row(bp) == nil)
  local t7
  for _, r in ipairs(bp.rows) do if r.id == 7 then t7 = r end end
  check("pill 7 row keeps the ping x2 weight", t7 and t7.goal_src == "ping" and t7.goal_w == 2.0)
  check("the rebuild wins as today", cmd and cmd.x == 100 and cmd.y == 104)
  local ps = BP.panel_section(st)
  check("panel says why it is not forced", ps.hdr[4] and ps.hdr[4]:find("not forced (out_of_range, tank 11.0 tiles, range 10)", 1, true), ps.hdr[4])
  -- Exactly 10 tiles is inside.
  local w10 = world_of({ [7] = pill(110, 100, 10), [8] = pill(100, 104, 0) })
  local bp10 = run(fresh_state(ping_order()), w10, info_at(TMX, TMY))
  check("exactly 10.0 tiles -> forced", bp10.force ~= nil)
  -- Straight line, not Manhattan: (107,107) is 9.9 tiles, Manhattan 14 (> leash 11).
  local wd = world_of({ [7] = pill(107, 107, 10), [8] = pill(100, 104, 0) })
  local bpd, cd = run(fresh_state(ping_order()), wd, info_at(TMX, TMY))
  local fd = forced_row(bpd)
  check("diagonal 9.9 tiles: forced, leash waived", fd and fd.out_of_leash == nil and cd and cd.x == 107 and cd.y == 107,
        fd and fd.reject)
end

-- ── 4. Pill full = nothing new ───────────────────────────────────────────
print("4. pill at full health -> nothing new")
do
  local w = world_of({ [7] = pill(106, 100, 15), [8] = pill(100, 104, 0) })
  local bp, cmd = run(fresh_state(ping_order()), w, info_at(TMX, TMY))
  check("force off: full_health", bp.force == nil and bp.force_why == "full_health", bp.force_why)
  check("rebuild wins as today", cmd and cmd.x == 100 and cmd.y == 104)
  C.PING_DEFEND_REPAIR = false
  local bp0, c0 = run(fresh_state(ping_order()), w, info_at(TMX, TMY))
  C.PING_DEFEND_REPAIR = true
  check("same pool as the knob off", snapshot(bp, cmd) == snapshot(bp0, c0))
end

-- ── 5. Light damage (missing < TOPUP_MIN_MISSING) is still forced ────────
print("5. hp 13/15 (discovery offers no row) -> synthesised forced row")
do
  local w = world_of({ [7] = pill(106, 100, 13), [8] = pill(100, 104, 0) })
  C.PING_DEFEND_REPAIR = false
  local bp0 = run(fresh_state(ping_order()), w, info_at(TMX, TMY))
  C.PING_DEFEND_REPAIR = true
  local has7 = false
  for _, r in ipairs(bp0.rows) do if r.id == 7 then has7 = true end end
  check("knob off: no row for pill 7 at all", not has7)
  local st = fresh_state(ping_order())
  local bp, cmd = run(st, w, info_at(TMX, TMY))
  check("knob on: forced row synthesised", forced_row(bp) and forced_row(bp).id == 7)
  check("and it is dispatched", cmd and cmd.x == 106 and cmd.y == 100)
end

-- ── 6. Ends: dead / lost / order cancelled / armour critical ─────────────
print("6. the force ends")
do
  local function ends(name, mutate, why)
    LOG = {}
    local st = fresh_state(ping_order())
    local w = base_world()
    local bp = run(st, w, info_at(TMX, TMY))
    check(name .. ": on first", bp.force ~= nil)
    st._bp_job = nil                      -- man home again (not the point here)
    local info = info_at(TMX, TMY)
    mutate(st, w, info)
    local bp2 = run(st, w, info, NOW + 1)
    check(name .. ": off, why=" .. why, bp2.force == nil and bp2.force_why == why, bp2.force_why)
    check(name .. ": no forced row", forced_row(bp2) == nil)
    check_log(name .. ": END line", "^PING_REPAIR_FORCE_END t=1001 pill=7 why=" .. why)
  end
  ends("pill dead", function(_, w) w.pills[7].health = 0 end, "pill_dead")
  ends("pill lost", function(_, w) w.pills[7].owner = "hostile" end, "not_ours")
  ends("pill picked up", function(_, w) w.pills[7].in_tank = true end, "not_ours")
  ends("pill gone", function(_, w) w.pills[7] = nil end, "pill_gone")
  ends("order cancelled", function(st) st._order = nil end, "no_ping_defend_order")
  ends("order is a typed (chat) defend, not a ping", function(st) st._order = { kind = "defend_pill", tid = 7, sender = 0 } end, "no_ping_defend_order")
  ends("tank drove out of range", function(_, _, info) info.tankx = 90 * 256 end, "out_of_range")
  ends("armour critical (order paused)", function(_, _, info) info.armour = C.ARMOUR_CRITICAL end, "order_paused_armour")
  ends("repaired to full", function(_, w) w.pills[7].health = 15 end, "full_health")
end

-- ── 7. Safety gates still apply ──────────────────────────────────────────
print("7. what still blocks the forced repair")
do
  -- Under fire: the whole pool waits (the man's two risky ends).
  FIRE_AGE = 20
  local st = fresh_state(ping_order())
  local bp, cmd = run(st, base_world(), info_at(TMX, TMY))
  FIRE_AGE = nil
  local fr = forced_row(bp)
  check("under_fire: forced row rejected under_fire", fr and fr.reject and fr.reject:find("^under_fire"), fr and fr.reject)
  check("under_fire: nobody goes", cmd == nil)
  check("under_fire: bp.force_wait names it", bp.force_wait == "under_fire", bp.force_wait)
  -- Shell gate: the forced row HOLDS the man; the rebuild does not take him.
  SHELL[100 * 256 + 106] = { src = "p3", t = 12, how = "open_ground" }
  local st2 = fresh_state(ping_order())
  local bp2, c2 = run(st2, base_world(), info_at(TMX, TMY))
  SHELL = {}
  check("shell gate: no dispatch (rebuild not sent instead)", c2 == nil, c2 and (c2.x .. "," .. c2.y))
  check("shell gate: force_hold", bp2.force_hold == "shell_will_hit", bp2.force_hold)
  -- Man out on another job: wait, do not recall.
  local st3 = fresh_state(ping_order())
  st3._bp_job = { type = "farm", mx = 95, my = 100, dispatch_tick = NOW - 10, phase = "outbound" }
  local info3 = info_at(TMX, TMY); info3.man_status = C.LGM_MOVING; info3.man_x = 97 * 256; info3.man_y = 100 * 256
  local bp3, c3 = run(st3, base_world(), info3)
  check("man out on a farm job: no dispatch, job kept", c3 == nil and st3._bp_job and st3._bp_job.type == "farm")
  check("man out: force_wait = man_out_on_farm", bp3.force_wait == "man_out_on_farm", bp3.force_wait)
  -- Man dead.
  local info4 = info_at(TMX, TMY); info4.man_status = C.LGM_DEAD
  local bp4, c4 = run(fresh_state(ping_order()), base_world(), info4)
  check("man dead: no dispatch, force_wait=man_dead", c4 == nil and bp4.force_wait == "man_dead", bp4.force_wait)
  -- An ally already walking to repair it: the pool carries on without us.
  AS.set_info(2, NOW, { bpj = string.format("%02X%02X%02X%04X%04X", 2, 106, 100, NOW - 50, 40) })
  local bp5, c5 = run(fresh_state(ping_order()), base_world(), info_at(TMX, TMY))
  AS.init()
  check("ally repairing: forced row rejected ally_repairing", forced_row(bp5) and forced_row(bp5).reject:find("^ally_repairing"))
  check("ally repairing: the next job goes (rebuild)", c5 and c5.x == 100 and c5.y == 104)
  -- Waived: the tree reserve (1 tree with a pill aboard: reserve 8).
  local info6 = info_at(TMX, TMY, 1); info6.carried_pills = 1
  local bp6, c6 = run(fresh_state(ping_order()), base_world(), info6)
  check("1 tree, reserve 8: still forced and dispatched", c6 and c6.x == 106 and c6.y == 100, forced_row(bp6) and forced_row(bp6).reject)
  -- Waived: the take-blocker and friendly-fire discovery guards.
  local w7 = base_world(); w7.pills[7]._in_use = true
  local _, c7 = run(fresh_state(ping_order()), w7, info_at(TMX, TMY))
  check("take_blocker guard waived", c7 and c7.x == 106)
  -- Waived: mode_owned (a working mode while the order is held).
  local st8 = fresh_state(ping_order()); st8.builder.mode = "gather"; st8.goal = { kind = "capture_pill" }
  local _, c8 = run(st8, base_world(), info_at(TMX, TMY))
  check("mode_owned waived", c8 and c8.x == 106)
  -- Kept: fire_exchange substate.
  local st9 = fresh_state(ping_order()); st9.goal = { kind = "attack_tank", substate = "engage" }
  local bp9, c9 = run(st9, base_world(), info_at(TMX, TMY))
  check("fire_exchange kept", c9 == nil and forced_row(bp9).reject:find("^fire_exchange"), forced_row(bp9) and forced_row(bp9).reject)
end

-- ── 8. No wood -> harvest first, then repair ─────────────────────────────
print("8. no wood: harvest is step 1")
do
  LOG = {}
  FOREST[97 * 256 + 100] = true          -- N wedge, 3 tiles: out 48
  FOREST[100 * 256 + 104] = true         -- E wedge, 4 tiles: out 64
  local st = fresh_state(ping_order())
  local bp, cmd = run(st, base_world(), info_at(TMX, TMY, 0))
  local fr = forced_row(bp)
  check("force step = harvest", bp.force and bp.force.step == "harvest")
  check("the repair row says no_wood", (function()
    for _, r in ipairs(bp.rows) do if r.forced == "repair" then return r.reject and r.reject:find("^no_wood") end end
  end)())
  check("forced harvest = nearest walk (100,97)", bp.force_harvest and bp.force_harvest.mx == 100 and bp.force_harvest.my == 97)
  check("dispatch is a FARM to (100,97)", cmd and cmd.x == 100 and cmd.y == 97 and cmd.action == BUILDMODE_FARM,
        cmd and (cmd.x .. "," .. cmd.y))
  check("job forced=harvest", st._bp_job and st._bp_job.forced == "harvest")
  local hr = bp.force_harvest
  check("harvest score = raw + 1000", hr and hr.score == hr.raw_score + 1000)
  check("harvest terms show forced{harvest}", hr and BP.score_terms(hr):find("ping_force{1000} forced{harvest}", 1, true))
  check_log("PING_REPAIR_FORCE step=harvest", "^PING_REPAIR_FORCE t=1000 pill=7@%(106,100%) from p0 hp=10/15 dist=6.0<=10 step=harvest trees=0")
  -- Same fixture WITH wood -> repair.
  local st2 = fresh_state(ping_order())
  local _, c2 = run(st2, base_world(), info_at(TMX, TMY, 3))
  check("with wood: repair goes", c2 and c2.x == 106 and c2.y == 100 and c2.action == BUILDMODE_PBOX)
  -- Knob off with no wood: the pool's own farm choice, unforced.
  C.PING_DEFEND_REPAIR = false
  local bp3, c3 = run(fresh_state(ping_order()), base_world(), info_at(TMX, TMY, 0))
  local bp4, c4 = run(fresh_state(nil), base_world(), info_at(TMX, TMY, 0))
  C.PING_DEFEND_REPAIR = true
  check("knob off, no wood: same pool as no order", snapshot(bp3, c3) == snapshot(bp4, c4))
  FOREST = {}
  -- No forest in reach: the force waits and says so.
  local bp5, c5 = run(fresh_state(ping_order()), base_world(), info_at(TMX, TMY, 0))
  check("no forest: no dispatch, force_wait=no_farm_row_ok", c5 == nil and bp5.force_wait == "no_farm_row_ok", bp5.force_wait)
end

-- ── 9. Repeated repairs until full (with a harvest in between) ───────────
print("9. repeat until full")
do
  LOG = {}
  FOREST[97 * 256 + 100] = true
  local w = world_of({ [7] = pill(106, 100, 3) })
  local st = fresh_state(ping_order())
  local info = info_at(TMX, TMY, 1)
  local now = NOW
  local trips, harvests = 0, 0
  for _ = 1, 12 do
    local bp, cmd = run(st, w, info, now)
    if not bp.force then break end
    if cmd then
      if cmd.action == BUILDMODE_PBOX then
        trips = trips + 1
        -- The engine: 4 armour per tree, only what is needed.
        local need = math.ceil((15 - w.pills[7].health) / 4)
        local used = math.min(info.trees, need)
        w.pills[7].health = math.min(15, w.pills[7].health + used * 4)
        info.trees = info.trees - used
      else
        harvests = harvests + 1
        info.trees = info.trees + 1      -- one tree per harvest, the worst case
      end
      -- The man walks out and back: MOVING, then home past ABORT_GRACE.
      info.man_status = C.LGM_MOVING; info.man_x = info.tankx; info.man_y = info.tanky
      now = now + 5
      run(st, w, info, now)
      info.man_status = C.LGM_INTANK
      now = now + (C.BUILDER_POOL_ABORT_GRACE or 30) + 1
      BP.update_job(st, w, info, now)    -- the man is home: the job closes
      check("job closed after the trip", st._bp_job == nil)
    end
    now = now + 1
  end
  check("pill repaired to full", w.pills[7].health == 15, w.pills[7].health)
  check("3 repair trips (3 -> 7 -> 11 -> 15)", trips == 3, trips)
  check("2 harvests between them", harvests == 2, harvests)
  local bp = run(st, w, info, now + 1)
  check("force off once full", bp.force == nil and bp.force_why == "full_health", bp.force_why)
  check_log("END why=full_health", "^PING_REPAIR_FORCE_END t=%d+ pill=7 why=full_health")
  FOREST = {}
end

print(string.format("%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
