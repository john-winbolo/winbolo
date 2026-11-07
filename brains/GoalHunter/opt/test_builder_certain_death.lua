-- =========================================================================
-- test_builder_certain_death.lua — standalone unit tests (2026-11-04) for
-- three knobs (constants.lua, "Builder sends: refuse only certain death"):
--   * C.BUILDER_SEND_CERTAIN_DEATH_ONLY: danger.lgm_certain_death against a
--     real shell list (a shell that ends on the builder; one that ends on
--     another tank, on a base, far away, or flies past him), and every
--     builder.decide send site refuses on certain death, sends under fire
--     without it, and keeps the old fire checks with the knob off;
--   * C.BUILDER_NO_IDLE_FARM_UNDER_FIRE: the on-path farm and the pool's
--     farm rows wait while under fire with trees at or above the minimum;
--     a needed harvest and a building job still go;
--   * C.BUILDER_FREE_COMBAT_GOALS: attack_tank, attack_base and a sea
--     capture_pill between its builder jobs free the builder.
-- No engine: terrain, the walk sim, the fire clock, the danger sample and
-- the builder pool's rung are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_builder_certain_death.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local LOG = {}
package.loaded["print2"] = setmetatable({ set_tick = function() end }, {
  __call = function(_, s) LOG[#LOG + 1] = tostring(s) end })

_G.gh_threat = _G.gh_threat or setmetatable({}, { __index = function()
  return function() return 0 end
end })
if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

-- brain.h values.
_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.OBJECT_HOSTILE, _G.OBJECT_NEUTRAL = 1, 2
_G.BUILDMODE_FARM, _G.BUILDMODE_ROAD, _G.BUILDMODE_BUILD = 1, 2, 3
_G.BUILDMODE_PBOX, _G.BUILDMODE_MINE = 4, 5
_G.TERRAIN_MASK, _G.TERRAIN_MINE_FLAG = 0x0F, 0x40
_G.KIND_NORMAL = 0
_G.cpf_estimate_tank_travel_ticks = function() return 50 end

-- Terrain: grass everywhere, plus what a test sets.
local GRASS = 7
local TER = {}
_G.get_terrain = function(mx, my) return TER[my * 256 + mx] or GRASS end

-- Walk sim: 16 world units a tick from the tank square's centre, along x
-- then y, written as the flat (x1, y1, x2, y2, ...) list the C sim gives.
_G.cpf_lgm_walk_path = function(sx, sy, dx, dy, bx, by, maxt, stuck, out)
  local x, y = sx * 256 + 128, sy * 256 + 128
  local tx, ty = dx * 256 + 128, dy * 256 + 128
  local n = 0
  while (x ~= tx or y ~= ty) and n < maxt do
    if x ~= tx then x = x + (tx > x and 16 or -16)
    else y = y + (ty > y and 16 or -16) end
    n = n + 1
    out[n * 2 - 1], out[n * 2] = x, y
  end
  return n
end
_G.cpf_lgm_travel_ticks_map = function(sx, sy, dx, dy)
  return (math.abs(sx - dx) + math.abs(sy - dy)) * 16
end

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local B      = require("builder")
local BP     = require("builder_pool")
local DM     = require("demine")

-- No blitz window, no blitz shot lines, no spike vote, no pill on a square.
local SQ = require("squad")
SQ.blitz_window = function() return nil end
SQ.blitz_shot_lines = function() return nil end
local FV = require("focus_vote")
FV.may_place = function() return true end
FV.note_reserve = function() end
require("pill_cover").pill_on_square = function() return nil end
require("cpathfinder").influence_at = function() return 1 end
-- The pool picks nothing: the decide() sites under test are below its rung.
BP.rung = function() return nil end

-- Fire clock and danger sample, set per test.
local FIRE_AGE, SAFE = nil, true
danger.tank_fire_age = function() return FIRE_AGE, FIRE_AGE and "hit" or nil end
danger.lgm_path_safe_enhanced = function() return SAFE end

-- The certain-death answer for the decide() tests: DEATH(mx, my) true means
-- a shell will kill the builder on that errand. The real model is tested in
-- section 2 through REAL.
local REAL = danger.lgm_certain_death
local DEATH = function() return false end
danger.lgm_certain_death = function(world, info, mx, my)
  if DEATH(mx, my) then
    return { t = 7, sx = 0, sy = 0, lx = 0, ly = 0, src = "p3", how = "tank",
             solid = false, walk_n = 3 }
  end
  return nil
end

local function has_logs()
  local f = io.open("danger.lua", "r")
  if not f then return false end
  local src = f:read("*a"); f:close()
  return src:find("BUILDER_CERTAIN_DEATH", 1, true) ~= nil
end
local HAS_LOGS = has_logs()
local function log_has(pat)
  for i = 1, #LOG do if LOG[i]:find(pat) then return LOG[i] end end
  return nil
end

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

local function set_knobs(cd, farm, free)
  C.BUILDER_SEND_CERTAIN_DEATH_ONLY = cd
  C.BUILDER_NO_IDLE_FARM_UNDER_FIRE = farm
  C.BUILDER_FREE_COMBAT_GOALS = free
end

-- Tank at (100,100). A fresh state, world and info per test.
local TX, TY = 100, 100
local function mk(mode, trees)
  local info = { tankx = TX * 256 + 128, tanky = TY * 256 + 128,
                 man_status = C.LGM_INTANK, inboat = false,
                 trees = trees or 15, carried_pills = 0, speed = 0,
                 direction = 0, mines = 0, shells = 40, armour = 40,
                 player_number = 0, objects = {} }
  local state = { tick = 1000, goal = { kind = "none", mx = 0, my = 0 },
                  builder = { mode = mode or "infrastructure", need_trees = 0 },
                  pf = { next_mx = -1, next_my = -1, status = "idle" },
                  perc = { threat_at_tank = 0, pill_threats = {},
                           enemy_tanks = {} } }
  local world = { pills = {}, bases = {}, pill_at = {}, base_at = {} }
  return state, world, info
end
local function reset()
  for k in pairs(TER) do TER[k] = nil end
  for i = #LOG, 1, -1 do LOG[i] = nil end
  FIRE_AGE, SAFE = nil, true
  DEATH = function() return false end
end
local function death_at(x, y) return function(mx, my) return mx == x and my == y end end
local function always() return true end

-- A send site, run three ways: knob on with certain death (refused), knob on
-- under fire with no certain death (sent), knob off with the old fire check
-- failing (refused by the old check) and with certain death but a safe
-- sample (sent: the knob-off path does not ask the prediction).
local function site(name, setup, want_x, want_y, want_action, old_check)
  print(name)
  -- (a) knob on, certain death.
  reset(); set_knobs(true, true, true)
  local st, w, inf = mk()
  setup(st, w, inf)
  DEATH = always
  local cmd = B.decide(st, w, inf, st.tick)
  local nd = st._builder_no_dispatch
  check(name .. ": certain death -> no send", cmd == nil,
        cmd and string.format("%s@(%d,%d)", cmd.action, cmd.x, cmd.y))
  check(name .. ": bail shell_will_hit, site " .. name,
        nd and nd.why == "shell_will_hit" and nd.site == name,
        nd and (tostring(nd.why) .. "/" .. tostring(nd.site)))
  if HAS_LOGS then
    check(name .. ": one BUILDER_CERTAIN_DEATH line",
          log_has("^BUILDER_CERTAIN_DEATH t=1000 site=" .. name:gsub("_", "%%_") .. " dest=") ~= nil,
          LOG[1])
  end
  -- (b) knob on, heavy fire, no certain death: he goes.
  reset(); set_knobs(true, true, true)
  st, w, inf = mk()
  setup(st, w, inf)
  FIRE_AGE, SAFE = 5, false
  st.perc.threat_at_tank = 99
  cmd = B.decide(st, w, inf, st.tick)
  check(name .. ": under fire, no certain death -> sends",
        cmd and cmd.x == want_x and cmd.y == want_y and cmd.action == want_action,
        cmd and string.format("%s@(%d,%d)", cmd.action, cmd.x, cmd.y))
  -- (c) knob off: the old fire check refuses, the prediction is not asked.
  if old_check then
    reset(); set_knobs(false, false, false)
    st, w, inf = mk()
    setup(st, w, inf)
    SAFE = false
    cmd = B.decide(st, w, inf, st.tick)
    check(name .. ": knob off, unsafe sample -> no send (old check)", cmd == nil,
          cmd and string.format("%s@(%d,%d)", cmd.action, cmd.x, cmd.y))
  end
  reset(); set_knobs(false, false, false)
  st, w, inf = mk()
  setup(st, w, inf)
  DEATH = always
  cmd = B.decide(st, w, inf, st.tick)
  check(name .. ": knob off, certain death not asked -> sends",
        cmd and cmd.x == want_x and cmd.y == want_y and cmd.action == want_action,
        cmd and string.format("%s@(%d,%d)", cmd.action, cmd.x, cmd.y))
  set_knobs(true, true, true)
end

-- ── 1. knob values ───────────────────────────────────────────────────────
print("1. knobs")
check("BUILDER_SEND_CERTAIN_DEATH_ONLY default true", C.BUILDER_SEND_CERTAIN_DEATH_ONLY == true)
check("BUILDER_NO_IDLE_FARM_UNDER_FIRE default true", C.BUILDER_NO_IDLE_FARM_UNDER_FIRE == true)
check("BUILDER_FREE_COMBAT_GOALS default true", C.BUILDER_FREE_COMBAT_GOALS == true)
check("BUILDER_NO_IDLE_FARM_MIN_TREES = FARM_ENEMY_MIN_TREES",
      C.BUILDER_NO_IDLE_FARM_MIN_TREES == C.FARM_ENEMY_MIN_TREES, C.BUILDER_NO_IDLE_FARM_MIN_TREES)
local K = C.PRESETS.keel
check("KEEL: certain-death knob false", K.BUILDER_SEND_CERTAIN_DEATH_ONLY == false)
check("KEEL: idle-farm knob false", K.BUILDER_NO_IDLE_FARM_UNDER_FIRE == false)
check("KEEL: free-combat knob false", K.BUILDER_FREE_COMBAT_GOALS == false)

-- ── 2. the prediction (danger.lgm_certain_death, real) ───────────────────
-- Tank at (100,100) = world (25728, 25728); destination (103,100), so the
-- walk is 48 steps east at 16 a tick. A shell from square (104,100)'s centre
-- (26752) flying west (angle 192) at 32 a tick.
print("2. prediction")
local WX = TX * 256 + 128
local function shot(x, y, angle, owner, life, hostile)
  return { type = OBJECT_SHOT, x = x, y = y, angle = angle, owner = owner,
           life = life, info = hostile and OBJECT_HOSTILE or 0 }
end
do
  reset()
  local st, w, inf = mk()
  check("no shells -> nil", REAL(w, inf, 103, 100) == nil)

  -- A pill's shell, life 20: it expires on tick 21 at x 26112. The builder
  -- is at 25728 + 16*21 = 26064 then (48 away; a pill shell has no rewind).
  inf.objects = { shot(26752, WX, 192, 255, 20) }
  local h = REAL(w, inf, 103, 100)
  check("pill shell expires on him -> certain death", h ~= nil)
  check("  ending expiry", h and h.how == "expiry", h and h.how)
  check("  at tick life + 1 (21)", h and h.t == 21, h and h.t)
  check("  from the pill", h and h.src == "pill", h and h.src)

  -- Life 5: it expires on tick 6 at 26592, he is at 25824 (768 away).
  inf.objects = { shot(26752, WX, 192, 255, 5) }
  check("shell expires far away -> nil", REAL(w, inf, 103, 100) == nil)

  -- Life unknown: no expiry ending; the shell flies on and ends on our tank
  -- (tick 29, x 25824) while he is at 26192.
  inf.objects = { shot(26752, WX, 192, 255, nil) }
  check("life unknown, ends on our tank far from him -> nil", REAL(w, inf, 103, 100) == nil)

  -- Flies through him: south along x 26368, on him at tick 40 (both at
  -- (26368, 25728)); no blast there. Expires on tick 61 at y 26368 while he
  -- stands on the target (26496, 25728).
  inf.objects = { shot(26368, WX - 1280, 128, 255, 60) }
  check("shell passes through him -> nil", REAL(w, inf, 103, 100) == nil)

  -- An enemy's shell (player 3), life 20: a tank's shell tests him one tick
  -- earlier (26048 at tick 20 vs the blast at 26112).
  inf.objects = { shot(26752, WX, 192, 3, 20, true) }
  h = REAL(w, inf, 103, 100)
  check("enemy shell expires on him -> certain death", h ~= nil and h.how == "expiry",
        h and h.how)
  check("  from p3", h and h.src == "p3", h and h.src)

  -- The same shell, with player 2's tank at (26560, 25728): it ends on that
  -- tank on tick 5 (x 26592), 800 from where he was on tick 4.
  inf.objects = { shot(26752, WX, 192, 3, 20, true),
                  { type = OBJECT_TANK, x = 26560, y = WX, idnum = 2, info = OBJECT_HOSTILE } }
  check("shell ends on another tank -> nil", REAL(w, inf, 103, 100) == nil)
  -- The shooter's own tank does not stop its shell.
  inf.objects = { shot(26752, WX, 192, 3, 20, true),
                  { type = OBJECT_TANK, x = 26560, y = WX, idnum = 3, info = OBJECT_HOSTILE } }
  h = REAL(w, inf, 103, 100)
  check("shooter's own tank does not stop it -> certain death", h ~= nil and h.how == "expiry",
        h and h.how)
  -- The other tank stops it beside him: player 2 at x 26000. The shell is
  -- inside its radius on tick 21 (x 26080); he was at 26048 on tick 20.
  inf.objects = { shot(26752, WX, 192, 3, 30, true),
                  { type = OBJECT_TANK, x = 26000, y = WX, idnum = 2, info = OBJECT_HOSTILE } }
  h = REAL(w, inf, 103, 100)
  check("shell ends on another tank beside him -> certain death",
        h ~= nil and h.how == "tank_p2", h and h.how)

  -- A base of ours on square (103,100): the enemy shell enters it on tick 5
  -- (26592). Armour seen 2 (x5 = 10 > base_hit_armour 4): it stops there,
  -- and a base square kills only a builder on it (he is on square 100).
  local function base(owner, health, ally_only)
    w.base_at = { [100 * 256 + 103] = { base = { owner = owner, health = health,
                                                  _ally_only = ally_only } } }
  end
  inf.objects = { shot(26752, WX, 192, 3, 20, true) }
  base("friendly", 2)
  check("our base stops the enemy shell -> nil", REAL(w, inf, 103, 100) == nil)
  base("friendly", 0)
  h = REAL(w, inf, 103, 100)
  check("our base at armour 0 lets it on -> certain death", h ~= nil and h.how == "expiry",
        h and h.how)
  base("friendly", 2, true)
  h = REAL(w, inf, 103, 100)
  check("a base known only from an ally's report is not used -> certain death",
        h ~= nil, h and h.how)
  -- A pill's shell never stops at a base.
  inf.objects = { shot(26752, WX, 192, 255, 20) }
  base("friendly", 2)
  h = REAL(w, inf, 103, 100)
  check("a pill's shell flies over a base -> certain death", h ~= nil, h and h.how)
  w.base_at = {}

  -- A wall on (103,100) where he works: a blast on a wall kills only a
  -- builder on that square. A pill's shell from (106,100) flying west hits
  -- the wall on tick 16 (x 26592); he is still walking (square 101).
  TER[100 * 256 + 103] = C.T_BUILDING
  inf.objects = { shot(106 * 256 + 128, WX, 192, 255, 40) }
  check("shell ends on a wall he is not on -> nil", REAL(w, inf, 103, 100) == nil)
  TER[100 * 256 + 103] = nil

  -- A forest or a boat stops every shell, and with
  -- C.BLAST_TERRAIN_SQUARE_HIT that hit is the square rule. On (102,100), a
  -- pill's shell with life 19 enters it on tick 13 (x 26336) while he is at
  -- 25936 on square 101: he lives. Without the knob it flies on and expires
  -- on tick 20 at 26112, 64 from him: certain death.
  local old_tsh = C.BLAST_TERRAIN_SQUARE_HIT
  for _, tt in ipairs({ { C.T_FOREST, "forest" }, { C.T_BOAT, "boat" } }) do
    TER[100 * 256 + 102] = tt[1]
    inf.objects = { shot(26752, WX, 192, 255, 19) }
    C.BLAST_TERRAIN_SQUARE_HIT = true
    check("shell ends on a " .. tt[2] .. " he is not on -> nil",
          REAL(w, inf, 103, 100) == nil)
    C.BLAST_TERRAIN_SQUARE_HIT = nil
    h = REAL(w, inf, 103, 100)
    check("knob unset: it flies over the " .. tt[2] .. " -> certain death",
          h ~= nil and h.how == "expiry", h and h.how)
    TER[100 * 256 + 102] = nil
  end
  -- A forest on (101,100), his square: a pill's shell with life 30 enters it
  -- on tick 21 (x 26080) while he is at 26064 on it: the square rule kills
  -- him. Without the knob it flies on and ends on our tank on tick 29, 368
  -- from him.
  TER[100 * 256 + 101] = C.T_FOREST
  inf.objects = { shot(26752, WX, 192, 255, 30) }
  C.BLAST_TERRAIN_SQUARE_HIT = true
  h = REAL(w, inf, 103, 100)
  check("shell ends on the forest he is on -> certain death",
        h ~= nil and h.how == "terrain" and h.solid, h and h.how)
  C.BLAST_TERRAIN_SQUARE_HIT = nil
  check("knob unset: it flies over his forest -> nil", REAL(w, inf, 103, 100) == nil)
  TER[100 * 256 + 101] = nil
  C.BLAST_TERRAIN_SQUARE_HIT = old_tsh

  -- A dead pill on (102,100): a pill's shell with life 19 flies over it and
  -- expires on tick 20 at x 26112, the west edge of that square. He is at
  -- 25728 + 16*20 = 26048 then, on square 101, 64 away. With
  -- C.BLAST_DEAD_PILL_OPEN the dead pill's square is open ground and the
  -- blast kills him; without it the square is solid and he lives. A live
  -- pill there stops the shell at the square's centre: he lives either way.
  local function pill_on(health)
    w.pill_at = { [100 * 256 + 102] = { { id = 1, pill = { health = health,
                                                           in_tank = false } } } }
  end
  inf.objects = { shot(26752, WX, 192, 255, 19) }
  pill_on(0)
  C.BLAST_DEAD_PILL_OPEN = true
  h = REAL(w, inf, 103, 100)
  check("blast on a dead pill's edge, he is across it -> certain death",
        h ~= nil and h.how == "expiry", h and h.how)
  C.BLAST_DEAD_PILL_OPEN = false
  check("  knob off: the dead pill's square is solid -> nil",
        REAL(w, inf, 103, 100) == nil)
  C.BLAST_DEAD_PILL_OPEN = true
  pill_on(5)
  check("  a live pill there stops the shell -> nil", REAL(w, inf, 103, 100) == nil)

  -- The shot that kills a pill: health 1 on (102,100). A pill's shell from
  -- x 27620 going west enters that square on tick 40 (27620 - 32*40 =
  -- 26340) and hits the pill; the blast is at the square's centre, 26240.
  -- He is at 25728 + 16*40 = 26368 then: the first world unit of square
  -- 103, 128 from the centre, inside the open-ground circle. A hit on a pill
  -- is a hit on its square, the killing shot too, so he lives. With the
  -- shell 10 ticks closer the hit is on tick 30 and he is at 26208, on the
  -- pill's square: certain death, on the square rule.
  pill_on(1)
  inf.objects = { shot(27620, WX, 192, 255, 50) }
  check("  the shot that kills a pill: he is just off its square -> nil",
        REAL(w, inf, 103, 100) == nil)
  inf.objects = { shot(27300, WX, 192, 255, 50) }
  h = REAL(w, inf, 103, 100)
  check("  the shot that kills a pill: he is on its square -> certain death",
        h ~= nil and h.how == "pill" and h.solid == true and h.t == 30,
        h and (h.how .. " t=" .. tostring(h.t)))
  C.BLAST_DEAD_PILL_OPEN = nil
  w.pill_at = {}

  -- Our own builder back in the tank: a shell that lands on the tank square
  -- late (life 63 from far east, he is home by tick 2*48+20 = 116 > 63, so
  -- use a short errand: (101,100), 16 steps, home after tick 52).
  inf.objects = { shot(26752, WX, 192, 255, 60) }
  check("blast after he is home -> nil", REAL(w, inf, 101, 100) == nil)
end

-- ── 3. every decide() send site ──────────────────────────────────────────
print("3. send sites")
site("water_build", function(st) st.water_build = { x = 100, y = 100 } end,
     100, 100, BUILDMODE_ROAD, true)
site("slow_build", function(st) st.slow_build = { x = 100, y = 100 } end,
     100, 100, BUILDMODE_ROAD, true)
site("sea_mine", function(st, w, inf)
  st.builder.mode = "sea_mine"
  st.builder.sea_target = { mx = 103, my = 100 }
  st.goal = { kind = "capture_pill", mx = 105, my = 100, sea = { S = { 103, 100 } } }
  inf.mines, inf.trees = 1, 21
end, 103, 100, BUILDMODE_MINE, false)
site("sea_boat", function(st, w, inf)
  st.builder.mode = "sea_boat"
  st.builder.sea_target = { mx = 101, my = 100 }
  st.goal = { kind = "capture_pill", mx = 105, my = 100, sea = { S = { 101, 100 } } }
  TER[100 * 256 + 101] = C.T_RIVER
  inf.trees = 20
end, 101, 100, BUILDMODE_BUILD, false)
do
  -- Trail drop: knob is off by default; heading north, so the spot is south.
  local was = C.TRAIL_DROP_ENABLED
  C.TRAIL_DROP_ENABLED = true
  local by = 100 + (C.TRAIL_DROP_BEHIND_DIST or 2)
  site("trail_drop", function(st, w, inf)
    inf.carried_pills, inf.speed, inf.direction = C.TRAIL_DROP_MIN_PILLS, C.TRAIL_DROP_MIN_SPEED, 0
    inf.trees = 15
  end, 100, by, BUILDMODE_PBOX, false)
  C.TRAIL_DROP_ENABLED = was
end
site("place_pill", function(st, w, inf)
  st.builder.mode = "place_pill"
  st.builder.pill_target = { mx = 102, my = 100 }
  inf.carried_pills, inf.trees = 1, 8
end, 102, 100, BUILDMODE_PBOX, false)
site("wall_shield", function(st, w, inf)
  st.builder.mode = "wall_shield"
  st.builder.wall_target = { mx = 102, my = 100 }
  inf.trees = 8
end, 102, 100, BUILDMODE_BUILD, false)
site("base_shield", function(st, w, inf)
  st.builder.mode = "base_shield"
  st.builder.wall_target = { mx = 102, my = 100 }
  inf.trees = 8
end, 102, 100, BUILDMODE_BUILD, true)
site("gather", function(st, w, inf)
  st.builder.mode = "gather"
  st.builder.need_trees = 10
  inf.trees = 2
  TER[100 * 256 + 101] = C.T_FOREST
end, 101, 100, BUILDMODE_FARM, true)
site("trepair", function(st, w, inf)
  st._trepair_job = { mx = 101, my = 100 }
  TER[100 * 256 + 101] = C.T_CRATER
  inf.trees = 15
end, 101, 100, BUILDMODE_ROAD, true)
site("onpath_farm", function(st, w, inf)
  st.builder.mode = "opportunistic"
  inf.trees = 2
  TER[100 * 256 + 101] = C.T_FOREST
end, 101, 100, BUILDMODE_FARM, true)
site("road_ahead", function(st, w, inf)
  st.pf.next_mx, st.pf.next_my = 101, 100
  TER[100 * 256 + 101] = C.T_SWAMP
  inf.trees = 15
end, 101, 100, BUILDMODE_ROAD, true)

print("3b. gather tries the next forest")
do
  reset(); set_knobs(true, true, true)
  local st, w, inf = mk("gather", 2)
  st.builder.need_trees = 10
  TER[100 * 256 + 101] = C.T_FOREST
  TER[103 * 256 + 100] = C.T_FOREST
  DEATH = death_at(101, 100)
  local cmd = B.decide(st, w, inf, st.tick)
  check("certain death on the nearest forest -> the next one",
        cmd and cmd.x == 100 and cmd.y == 103 and cmd.action == BUILDMODE_FARM,
        cmd and string.format("%s@(%d,%d)", cmd.action, cmd.x, cmd.y))
end

print("3c. pool winner and the other pool checks")
do
  local function src(p)
    local f = io.open(p, "r"); local s = f:read("*a"); f:close(); return s
  end
  local s = src("builder_pool.lua")
  check("pool winner asks lgm_certain_death with the knob on",
        s:find("hit = danger.lgm_certain_death(world, info, row.mx, row.my)", 1, true) ~= nil)
  check("pool winner keeps lgm_shell_gate with the knob off",
        s:find("hit = danger.lgm_shell_gate(world, info, row.mx, row.my)", 1, true) ~= nil)

  -- eligibility: a fire-class substate and the under-fire clock.
  reset(); set_knobs(true, true, true)
  local st, w, inf = mk("suppressed")
  st.goal = { kind = "attack_pill", mx = 105, my = 100, substate = "engage" }
  local ok, why, d = BP.eligibility(st, w, inf, st.tick)
  check("fire_exchange substate is not a refusal (knob on)",
        not (why and why:find("fire_exchange", 1, true)), why)
  check("  recorded as fire_exchange_waived", d and d.fire_exchange_waived == "engage")
  set_knobs(false, true, true)
  ok, why = BP.eligibility(st, w, inf, st.tick)
  check("fire_exchange substate refuses (knob off)", ok == false and why == "fire_exchange:engage", why)

  st, w, inf = mk("opportunistic")
  FIRE_AGE = 10
  set_knobs(true, true, true)
  ok, why, d = BP.eligibility(st, w, inf, st.tick)
  check("under fire is not a refusal (knob on)", ok == true, why)
  check("  recorded as under_fire_waived", d and d.under_fire_waived == 10)
  set_knobs(false, true, true)
  ok, why = BP.eligibility(st, w, inf, st.tick)
  check("under fire refuses (knob off)", ok == false and why == "under_fire(10t)", why)
  set_knobs(true, true, true)

  -- gate_row: the danger sample is off with the knob on.
  local function row(t)
    return { type = t or "topup", trip = 40, mx = 102, my = 100, dist = 2,
             score = 100, trees_need = 1, linear = false }
  end
  local ctx = { ok = true, reserve = 0 }
  st, w, inf = mk("opportunistic", 15)
  SAFE = false
  local r = BP.gate_row(st, w, inf, st.tick, row(), ctx)
  check("gate_row: unsafe sample is not a refusal (knob on)", r.reject == nil, r.reject)
  set_knobs(false, false, false)
  r = BP.gate_row(st, w, inf, st.tick, row(), ctx)
  check("gate_row: unsafe sample refuses (knob off)", r.reject == "path_unsafe", r.reject)
  set_knobs(true, true, true)
end

-- ── 4. no idle farm under fire ───────────────────────────────────────────
print("4. idle farm under fire")
do
  local MIN = C.BUILDER_NO_IDLE_FARM_MIN_TREES
  local function onpath(trees, age, knob)
    reset(); set_knobs(true, knob, true)
    local st, w, inf = mk("opportunistic", trees)
    TER[100 * 256 + 101] = C.T_FOREST
    FIRE_AGE = age
    return B.decide(st, w, inf, st.tick)
  end
  local cmd = onpath(MIN, 10, true)
  check("on-path farm: under fire, trees = min -> no harvest", cmd == nil,
        cmd and cmd.action)
  if HAS_LOGS then
    check("  IDLE_FARM_UNDER_FIRE line", log_has("^IDLE_FARM_UNDER_FIRE t=1000 site=onpath_farm fire_age=10") ~= nil, LOG[1])
  end
  cmd = onpath(MIN - 1, 10, true)
  check("on-path farm: under fire, trees below min -> harvests",
        cmd and cmd.action == BUILDMODE_FARM, cmd and cmd.action)
  cmd = onpath(MIN, C.BUILDER_POOL_UNDER_FIRE_TICKS, true)
  check("on-path farm: fire age at the quiet limit -> harvests",
        cmd and cmd.action == BUILDMODE_FARM, cmd and cmd.action)
  cmd = onpath(MIN, nil, true)
  check("on-path farm: never hit -> harvests", cmd and cmd.action == BUILDMODE_FARM,
        cmd and cmd.action)
  cmd = onpath(MIN, 10, false)
  check("on-path farm: knob off -> harvests under fire", cmd and cmd.action == BUILDMODE_FARM,
        cmd and cmd.action)

  -- The road ahead still goes while the farm waits.
  reset(); set_knobs(true, true, true)
  local st, w, inf = mk("opportunistic", 15)
  TER[100 * 256 + 101] = C.T_FOREST
  TER[100 * 256 + 99] = C.T_SWAMP
  st.pf.next_mx, st.pf.next_my = 99, 100
  FIRE_AGE = 10
  cmd = B.decide(st, w, inf, st.tick)
  check("on-path farm waits, the road ahead goes",
        cmd and cmd.action == BUILDMODE_ROAD and cmd.x == 99, cmd and cmd.action)

  -- A needed harvest still goes: gather under need_trees.
  reset(); set_knobs(true, true, true)
  st, w, inf = mk("gather", MIN + 1)
  st.builder.need_trees = 20
  TER[100 * 256 + 101] = C.T_FOREST
  FIRE_AGE = 10
  cmd = B.decide(st, w, inf, st.tick)
  check("gather for a build under fire -> harvests", cmd and cmd.action == BUILDMODE_FARM,
        cmd and cmd.action)
  -- A building job still goes.
  reset(); set_knobs(true, true, true)
  st, w, inf = mk("wall_shield", 8)
  st.builder.wall_target = { mx = 102, my = 100 }
  FIRE_AGE = 10
  cmd = B.decide(st, w, inf, st.tick)
  check("wall build under fire -> builds", cmd and cmd.action == BUILDMODE_BUILD,
        cmd and cmd.action)
  reset(); set_knobs(true, true, true)
  st, w, inf = mk("place_pill", 8)
  st.builder.pill_target = { mx = 102, my = 100 }
  inf.carried_pills = 1
  FIRE_AGE = 10
  cmd = B.decide(st, w, inf, st.tick)
  check("pill placement under fire -> places", cmd and cmd.action == BUILDMODE_PBOX,
        cmd and cmd.action)

  -- Pool farm rows.
  local function farm_row(trees, age, knob, ctx)
    reset(); set_knobs(true, knob, true)
    local s2, w2, i2 = mk("opportunistic", trees)
    FIRE_AGE = age
    local r = { type = "farm", trip = 40, mx = 101, my = 100, dist = 1,
                score = 100, trees_need = 0, linear = false }
    return BP.gate_row(s2, w2, i2, s2.tick, r, ctx or { ok = true, reserve = 0 })
  end
  local r = farm_row(MIN, 10, true)
  check("pool farm row: under fire, trees = min -> idle_farm_under_fire",
        r.reject and r.reject:find("^idle_farm_under_fire%(10t, trees 4 >= 4%)") ~= nil
        and r.reject_key == "idle_farm_under_fire", r.reject)
  r = farm_row(MIN - 1, 10, true)
  check("pool farm row: trees below min -> no block", r.reject == nil, r.reject)
  r = farm_row(MIN, 10, false)
  check("pool farm row: knob off -> no block", r.reject == nil, r.reject)
  r = farm_row(MIN, nil, true)
  check("pool farm row: never hit -> no block", r.reject == nil, r.reject)
  r = farm_row(MIN, 10, true, { ok = true, reserve = 0, not_idle = true, waive_min = true })
  check("pool farm row: ping-forced harvest -> no block", r.reject == nil, r.reject)
  -- A repair row is not a farm row.
  reset(); set_knobs(true, true, true)
  local s3, w3, i3 = mk("opportunistic", 15)
  FIRE_AGE = 10
  r = BP.gate_row(s3, w3, i3, s3.tick, { type = "topup", trip = 40, mx = 102, my = 100,
        dist = 2, score = 100, trees_need = 1, linear = false }, { ok = true, reserve = 0 })
  check("pool repair row under fire -> no block", r.reject == nil, r.reject)
end

-- ── 5. free combat goals ─────────────────────────────────────────────────
print("5. free combat goals")
do
  local function mode_for(goal, knob)
    reset(); set_knobs(true, true, knob)
    local st, w, inf = mk()
    st.goal = goal
    B.set_mode(st, w, inf, goal)
    local ok, why = BP.eligibility(st, w, inf, st.tick)
    return st.builder.mode, ok, why
  end
  local m, ok, why = mode_for({ kind = "attack_tank", mx = 105, my = 100 }, true)
  check("attack_tank -> opportunistic (knob on)", m == "opportunistic", m)
  check("  pool eligible", ok == true, why)
  m, ok, why = mode_for({ kind = "attack_tank", mx = 105, my = 100 }, false)
  check("attack_tank -> suppressed (knob off)", m == "suppressed", m)
  check("  pool refuses", ok == false, why)
  m = mode_for({ kind = "attack_base", mx = 105, my = 100 }, true)
  check("attack_base -> opportunistic (knob on)", m == "opportunistic", m)
  m = mode_for({ kind = "attack_base", mx = 105, my = 100 }, false)
  check("attack_base -> suppressed (knob off)", m == "suppressed", m)
  local function sea(sub)
    return { kind = "capture_pill", mx = 105, my = 100, substate = sub,
             sea = { trees_need = 21, S = { 103, 100 }, F = { 102, 100 } } }
  end
  m = mode_for(sea("board"), true)
  check("sea capture_pill between jobs -> opportunistic (knob on)", m == "opportunistic", m)
  m = mode_for(sea("board"), false)
  check("sea capture_pill between jobs -> suppressed (knob off)", m == "suppressed", m)
  m = mode_for(sea("lay_mine"), true)
  check("sea lay_mine keeps sea_mine (knob on)", m == "sea_mine", m)
  m = mode_for(sea("build_boat"), true)
  check("sea build_boat keeps sea_boat (knob on)", m == "sea_boat", m)
  m = mode_for(sea("seek_trees"), true)
  check("sea seek_trees keeps gather (knob on)", m == "gather", m)

  -- decide() sends during attack_tank with the knob on (on-path farm).
  reset(); set_knobs(true, true, true)
  local st, w, inf = mk(nil, 2)
  st.goal = { kind = "attack_tank", mx = 105, my = 100 }
  B.set_mode(st, w, inf, st.goal)
  TER[100 * 256 + 101] = C.T_FOREST
  local cmd = B.decide(st, w, inf, st.tick)
  check("attack_tank: decide sends (knob on)", cmd and cmd.action == BUILDMODE_FARM,
        cmd and cmd.action)
  set_knobs(true, true, false)
  st, w, inf = mk(nil, 2)
  st.goal = { kind = "attack_tank", mx = 105, my = 100 }
  B.set_mode(st, w, inf, st.goal)
  cmd = B.decide(st, w, inf, st.tick)
  check("attack_tank: decide stays in (knob off)", cmd == nil, cmd and cmd.action)

  -- A terrain-repair job may start during attack_tank.
  local function trepair(knob)
    reset(); set_knobs(true, true, knob)
    local s2, w2, i2 = mk("opportunistic", 15)
    s2.goal = { kind = "attack_tank", mx = 105, my = 100 }
    TER[100 * 256 + 102] = C.T_CRATER
    DM.update(s2, w2, i2)
    return s2._trepair_job
  end
  local j = trepair(true)
  check("attack_tank: terrain-repair job starts (knob on)", j and j.mx == 102 and j.my == 100)
  check("attack_tank: no terrain-repair job (knob off)", trepair(false) == nil)
end

set_knobs(true, true, true)
print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
