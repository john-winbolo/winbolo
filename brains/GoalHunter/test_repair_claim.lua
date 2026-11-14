-- =========================================================================
-- test_repair_claim.lua — standalone unit tests (2026-11-04) for REPAIR
-- CLAIMS BY ARRIVAL (builder_pool.lua; knob C.BUILDER_REPAIR_CLAIM_SHARE):
--   * the bpj advert carries the arrival tick (AAAA) on a repair job only,
--     and only with the knob on;
--   * an ally that arrives earlier or at the same tick makes us yield; ours
--     strictly earlier goes; a claim more than BUILDER_POOL_JOB_MAX_TICKS
--     after its arrival is ignored; an advert with no arrival keeps the
--     claim-tick rule;
--   * the quick repair row follows the same rule;
--   * the job's arrival tick follows the walk and freezes on the pill; the
--     claim is cleared when the job ends;
--   * knob off: the old 14-char advert and the claim-tick rule only;
--   * the KEEL preset turns the knob off.
-- No engine: terrain and the walk sim are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_repair_claim.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_repair_claim.lua
-- (the print2 lines are stripped there, so the log checks are skipped).
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

-- brain.h values.
_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.BUILDMODE_FARM, _G.BUILDMODE_PBOX = 1, 3

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local BP     = require("builder_pool")
local SHARE_DEFAULT = C.BUILDER_REPAIR_CLAIM_SHARE

local HAS_LOGS
do
  local f = io.open("builder_pool.lua", "r")
  local src = f and f:read("*a") or ""
  if f then f:close() end
  HAS_LOGS = src:find("REPAIR_CLAIM_YIELD", 1, true) ~= nil
end

-- ── Engine stand-ins ─────────────────────────────────────────────────────
U.ttype = function() return C.T_GRASS end
_G.cpf_lgm_travel_ticks_map = function(fx, fy, tx, ty)
  return (math.abs(fx - tx) + math.abs(fy - ty)) * 16
end
danger.update_fire_clock      = function() end
danger.tank_fire_age          = function() return nil end
danger.lgm_shell_gate         = function() return nil end
danger.lgm_certain_death      = function() return nil end
danger.log_certain_death      = function() end
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
-- We are player 1, tank at (100,100). Our pill 8 at (100,104) is dead: a
-- rebuild row. The walk is Manhattan x 16 ticks, so OUR builder arrives at
-- NOW + 64. Player 2 is the ally whose claim is under test.
local NOW = 1000
local OURS = NOW + 64
local PMX, PMY = 100, 104
local function pill(mx, my, hp, owner, in_use)
  return { mx = mx, my = my, health = hp, owner = owner or "friendly",
           _in_use = in_use or nil }
end
local function world_of(pills)
  local w = { pills = pills, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(pills) do
    w.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } }
  end
  return w
end
local function mk()
  local world = world_of({ [8] = pill(PMX, PMY, 0) })
  local state = {
    player_number = 1, tick = NOW,
    builder = { mode = "suppressed" },
    goal = { kind = "defend_pill", target_id = 8 },
  }
  local info = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128,
                 trees = 10, man_status = C.LGM_INTANK, inboat = false,
                 armour = 40, carried_pills = 0, player_number = 1,
                 objects = {} }
  return state, world, info
end
local function run(state, world, info)
  state.tick = NOW
  local bp = BP.update(state, world, info, NOW)
  local cmd = BP.rung(state, world, info, NOW)
  return bp, cmd
end
local function row_at(bp, mx, my)
  for _, r in ipairs(bp and bp.rows or {}) do
    if r.mx == mx and r.my == my then return r end
  end
  return nil
end
-- Player 2's bpj: type, tile, claim tick, eta, and (when given) arrival.
local function ally_claim(pn, mx, my, ctick, eta, arrive, rank)
  local s = string.format("%02X%02X%02X%04X%04X", rank or 1, mx, my,
                          ctick % 65536, eta)
  if arrive then s = s .. string.format("%04X", arrive % 65536) end
  AS.init()
  AS.set_info(pn, NOW, { bpj = s })
  return s
end
local function reset(on)
  AS.init()
  for i = #LOG, 1, -1 do LOG[i] = nil end
  C.BUILDER_REPAIR_CLAIM_SHARE = on
  _G.BRAIN_DEBUG_MODE = true
end
local function sent_to_pill(cmd)
  return cmd ~= nil and cmd.x == PMX and cmd.y == PMY
end

-- ── 0. No claim: the row goes, and our walk is 64 ticks ──────────────────
print("0. fixture")
do
  reset(true)
  local st, w, inf = mk()
  local bp, cmd = run(st, w, inf)
  local r = row_at(bp, PMX, PMY)
  check("no claim: the rebuild row is offered", r and r.type == "rebuild", r and r.type)
  check("no claim: out_ticks 64", r and r.out_ticks == 64, r and r.out_ticks)
  check("no claim: builder sent", sent_to_pill(cmd), cmd and (cmd.x .. "," .. cmd.y))
end

-- ── 1. The arrival rule ──────────────────────────────────────────────────
print("1. arrival rule")
do
  -- Ally arrives 4 ticks before ours: yield.
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 20, 44, OURS - 4)
  local st, w, inf = mk()
  local bp, cmd = run(st, w, inf)
  local r = row_at(bp, PMX, PMY)
  check("ally earlier: row refused ally_repairing",
        r and tostring(r.reject):find("^ally_repairing %(p2 arrives t1060 <= ours t1064%)") ~= nil,
        r and tostring(r.reject))
  check("ally earlier: no send", not sent_to_pill(cmd))
  check("ally earlier: row.ally carries both arrivals",
        r and r.ally and r.ally.arrive == OURS - 4 and r.ally.ours == OURS and r.ally.by == "arrive")
  check_log("ally earlier: REPAIR_CLAIM_YIELD line",
            "^REPAIR_CLAIM_YIELD t=1000 pill=%(100,104%) ours=1064 ally=1060 allyplayer=2")

  -- Same tick: yield.
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 20, 64, OURS)
  st, w, inf = mk()
  bp, cmd = run(st, w, inf)
  r = row_at(bp, PMX, PMY)
  check("tie: row refused", r and tostring(r.reject):find("^ally_repairing") ~= nil, r and tostring(r.reject))
  check("tie: no send", not sent_to_pill(cmd))

  -- Ours one tick earlier: go, although the ally claimed first.
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 20, 65, OURS + 1)
  st, w, inf = mk()
  bp, cmd = run(st, w, inf)
  r = row_at(bp, PMX, PMY)
  check("ours earlier: row not refused", r and r.reject == nil, r and tostring(r.reject))
  check("ours earlier: builder sent", sent_to_pill(cmd), cmd and (cmd.x .. "," .. cmd.y))
  check_log("ours earlier: REPAIR_CLAIM_GO line",
            "^REPAIR_CLAIM_GO t=1000 pill=%(100,104%) ours=1064 ally=1065 allyplayer=2")

  -- A claim on another pill does nothing to this one.
  reset(true)
  ally_claim(2, PMX + 1, PMY, NOW - 20, 10, NOW + 10)
  st, w, inf = mk()
  _, cmd = run(st, w, inf)
  check("other pill: builder sent", sent_to_pill(cmd))

  -- An arrival in the past (his builder is on the pill or walking home):
  -- still live, yield.
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 300, 0, NOW - 200)
  st, w, inf = mk()
  _, cmd = run(st, w, inf)
  check("ally already there: no send", not sent_to_pill(cmd))
end

-- ── 2. Expiry ────────────────────────────────────────────────────────────
print("2. expiry")
do
  local MAXT = C.BUILDER_POOL_JOB_MAX_TICKS or 900
  reset(true)
  ally_claim(2, PMX, PMY, NOW - MAXT - 50, 0, NOW - MAXT)
  local st, w, inf = mk()
  local _, cmd = run(st, w, inf)
  check("arrival + JOB_MAX_TICKS == now: still live, no send", not sent_to_pill(cmd))

  reset(true)
  ally_claim(2, PMX, PMY, NOW - MAXT - 50, 0, NOW - MAXT - 1)
  st, w, inf = mk()
  local bp
  bp, cmd = run(st, w, inf)
  local r = row_at(bp, PMX, PMY)
  check("one tick past: claim ignored, row not refused", r and r.reject == nil, r and tostring(r.reject))
  check("one tick past: builder sent", sent_to_pill(cmd))
  check_log("one tick past: REPAIR_CLAIM_EXPIRED line", "^REPAIR_CLAIM_EXPIRED t=1000 pill=%(100,104%)")

  -- A stale slot (no message for longer than CLAIM_MAX_AGE) is ignored too.
  reset(true)
  AS.set_info(2, NOW - (C.BUILDER_POOL_CLAIM_MAX_AGE or 1750) - 1,
              { bpj = string.format("%02X%02X%02X%04X%04X%04X", 1, PMX, PMY, NOW - 20, 10, NOW + 10) })
  st, w, inf = mk()
  _, cmd = run(st, w, inf)
  check("stale slot: builder sent", sent_to_pill(cmd))
end

-- ── 3. An advert with no arrival: the claim-tick rule ────────────────────
print("3. no arrival on the advert")
do
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 50, 40, nil)
  local st, w, inf = mk()
  local bp, cmd = run(st, w, inf)
  local r = row_at(bp, PMX, PMY)
  check("old advert, earlier claim: refused (claim-tick rule)",
        r and tostring(r.reject):find("^ally_repairing %(p2 eta") ~= nil, r and tostring(r.reject))
  check("old advert, earlier claim: no send", not sent_to_pill(cmd))
  check("old advert: row.ally.by = claim_tick", r and r.ally and r.ally.by == "claim_tick")
end

-- ── 4. Knob off: the claim-tick rule only, old advert ────────────────────
print("4. knob off")
do
  -- The ally claimed first but arrives LATER than ours. Knob on: we go.
  -- Knob off: the arrival is ignored and the earlier claim holds us back.
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 50, 100, OURS + 36)
  local st, w, inf = mk()
  local _, cmd = run(st, w, inf)
  check("knob on, ours earlier: sent", sent_to_pill(cmd))

  reset(false)
  ally_claim(2, PMX, PMY, NOW - 50, 100, OURS + 36)
  st, w, inf = mk()
  local bp
  bp, cmd = run(st, w, inf)
  local r = row_at(bp, PMX, PMY)
  check("knob off: no send (claim-tick rule)", not sent_to_pill(cmd))
  check("knob off: old reason text",
        r and tostring(r.reject):find("^ally_repairing %(p2 eta %d+t%)$") ~= nil, r and tostring(r.reject))
  check("knob off: no reject_key", r and r.reject_key == nil, r and r.reject_key)
  check("knob off: no REPAIR_CLAIM line", logged("^REPAIR_CLAIM") == nil, logged("^REPAIR_CLAIM"))

  -- Ally arrives earlier but claimed LATER (same tick, higher player number):
  -- knob off, we go as before.
  reset(false)
  ally_claim(2, PMX, PMY, NOW, 10, NOW + 10)
  st, w, inf = mk()
  _, cmd = run(st, w, inf)
  check("knob off: later claim from player 2 -> we go", sent_to_pill(cmd))
  check("knob off: advert is the old 14 chars",
        #BP.claim_advert(st, NOW) == 14, BP.claim_advert(st, NOW))
end

-- ── 5. The advert and the job lifecycle ──────────────────────────────────
print("5. advert and job")
do
  reset(true)
  local st, w, inf = mk()
  local _, cmd = run(st, w, inf)
  check("dispatch: sent", sent_to_pill(cmd))
  local job = st._bp_job
  check("dispatch: arrive_tick = now + out_ticks", job and job.arrive_tick == OURS, job and job.arrive_tick)
  local adv = BP.claim_advert(st, NOW)
  check("advert: 18 chars with knob on", #adv == 18, adv)
  local mx, my, tt, ct, et, at = BP.parse_claim(adv)
  check("advert: parses back to tile, type, claim, eta, arrival",
        mx == PMX and my == PMY and tt == 1 and ct == NOW and et == 64 and at == OURS,
        string.format("%s %s %s %s %s %s", tostring(mx), tostring(my), tostring(tt),
                      tostring(ct), tostring(et), tostring(at)))
  check_log("dispatch: REPAIR_CLAIM_TX line",
            "^REPAIR_CLAIM_TX t=1000 pill=%(100,104%) arrive=1064 player=1")

  -- Walking: two tiles out at NOW+40, the arrival is re-measured (+32).
  inf.man_status = C.LGM_MOVING
  inf.man_x, inf.man_y = 100 * 256 + 128, 102 * 256 + 128
  BP.update_job(st, w, inf, NOW + 40)
  check("walking: arrive_tick = now + remaining walk", job.arrive_tick == NOW + 40 + 32, job.arrive_tick)
  -- On the pill at NOW+70: frozen at 1070.
  inf.man_x, inf.man_y = PMX * 256 + 128, PMY * 256 + 128
  BP.update_job(st, w, inf, NOW + 70)
  check("on the pill: arrive_tick = reach tick", job.arrive_tick == NOW + 70, job.arrive_tick)
  -- Walking home: stays at the reach tick.
  inf.man_x, inf.man_y = 100 * 256 + 128, 102 * 256 + 128
  BP.update_job(st, w, inf, NOW + 100)
  check("walking home: arrive_tick kept", job.arrive_tick == NOW + 70, job.arrive_tick)
  check("walking home: advert still carries the reach tick",
        select(6, BP.parse_claim(BP.claim_advert(st, NOW + 100))) == NOW + 70)

  -- Home: the job ends and the claim is cleared.
  inf.man_status = C.LGM_INTANK
  BP.update_job(st, w, inf, NOW + 200)
  check("job end: no job record", st._bp_job == nil)
  check("job end: advert is \"-\"", BP.claim_advert(st, NOW + 200) == "-", BP.claim_advert(st, NOW + 200))
  check_log("job end: REPAIR_CLAIM_CLEAR line",
            "^REPAIR_CLAIM_CLEAR t=1200 pill=%(100,104%) arrive=1070 player=1")

  -- Builder dead: cleared too.
  reset(true)
  st, w, inf = mk()
  run(st, w, inf)
  inf.man_status = C.LGM_DEAD
  BP.update_job(st, w, inf, NOW + 50)
  check("builder dead: no job record, advert \"-\"",
        st._bp_job == nil and BP.claim_advert(st, NOW + 50) == "-")
  check_log("builder dead: REPAIR_CLAIM_CLEAR names it", "^REPAIR_CLAIM_CLEAR .*builder_dead")

  -- A farm job: the old 14-char advert even with the knob on.
  st = { _bp_job = { type = "farm", mx = 90, my = 90, claim_tick = NOW,
                     claim_eta_tick = NOW + 30, arrive_tick = NOW + 30 } }
  check("farm job: 14 chars", #BP.claim_advert(st, NOW) == 14, BP.claim_advert(st, NOW))
end

-- ── 6. RX line ───────────────────────────────────────────────────────────
if HAS_LOGS then
  print("6. RX line")
  reset(true)
  ally_claim(2, PMX, PMY, NOW - 20, 44, OURS - 4)
  local st, w, inf = mk()
  run(st, w, inf)
  check_log("RX: REPAIR_CLAIM_RX line",
            "^REPAIR_CLAIM_RX t=1000 pill=%(100,104%) arrive=1060 player=2")
  local n = 0
  for _, s in ipairs(LOG) do if s:find("^REPAIR_CLAIM_RX") then n = n + 1 end end
  st._bp_job = nil
  run(st, w, inf)
  local n2 = 0
  for _, s in ipairs(LOG) do if s:find("^REPAIR_CLAIM_RX") then n2 = n2 + 1 end end
  check("RX: one line per claim, not per tick", n == 1 and n2 == 1, n .. "/" .. n2)
end

-- ── 7. The quick repair row follows the same rule ────────────────────────
print("7. quick repair row")
do
  local QR = require("quick_repair")
  local qsave = C.QUICK_REPAIR_ON_TAKE
  C.QUICK_REPAIR_ON_TAKE = true
  C.QUICK_REPAIR_PREDICT_SHOTS = true
  C.QUICK_REPAIR_MARGIN_TICKS = 2
  -- The quick_repair test's take: target pill 3 at (110,100), our blocker 8
  -- at (104,100) hp 5, our shells land at +10 and +20: "send". Our walk is
  -- 4 tiles = 64 ticks, arrival NOW + 64.
  local function mkq()
    local w = world_of({ [3] = pill(110, 100, 2, "hostile"),
                         [8] = pill(104, 100, 5, "friendly", true) })
    local st = {
      player_number = 1, tick = NOW,
      builder = { mode = "suppressed" },
      goal = { kind = "attack_pill", mx = 110, my = 100, target_id = 3,
               substate = "engage", _attack_start_tick = NOW - 50,
               _on_target_land = { NOW + 10, NOW + 20 },
               _on_target_land_tick = NOW },
    }
    local inf = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128,
                  trees = 10, man_status = C.LGM_INTANK, inboat = false,
                  armour = 40, carried_pills = 0, player_number = 1,
                  reload = false, reload_ticks = 13, objects = {} }
    return st, w, inf
  end
  reset(true)
  local st, w, inf = mkq()
  local bp, cmd = run(st, w, inf)
  check("quick, no claim: sent", cmd and cmd.x == 104 and cmd.y == 100 and st._bp_job
        and st._bp_job.quick == true)
  check("quick: advert carries the arrival",
        select(6, BP.parse_claim(BP.claim_advert(st, NOW))) == NOW + 64)

  reset(true)
  ally_claim(2, 104, 100, NOW - 20, 64, NOW + 64, 2)
  st, w, inf = mkq()
  bp, cmd = run(st, w, inf)
  local r = row_at(bp, 104, 100)
  check("quick, ally same tick: row refused",
        r and r.quick and tostring(r.reject):find("^ally_repairing") ~= nil, r and tostring(r.reject))
  check("quick, ally same tick: no send", not (cmd and cmd.x == 104 and cmd.y == 100))

  reset(true)
  ally_claim(2, 104, 100, NOW - 20, 70, NOW + 70, 2)
  st, w, inf = mkq()
  bp, cmd = run(st, w, inf)
  check("quick, ours earlier: sent", cmd and cmd.x == 104 and cmd.y == 100)
  C.QUICK_REPAIR_ON_TAKE = qsave
  local _ = QR
end

-- ── 8. KEEL preset ───────────────────────────────────────────────────────
print("8. KEEL preset")
do
  check("default: BUILDER_REPAIR_CLAIM_SHARE = true", SHARE_DEFAULT == true)
  check("keel: BUILDER_REPAIR_CLAIM_SHARE = false",
        C.PRESETS.keel.BUILDER_REPAIR_CLAIM_SHARE == false)
end

_G.BRAIN_DEBUG_MODE = nil
print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
