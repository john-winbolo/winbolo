-- =========================================================================
-- test_qr_own_fire.lua — standalone unit tests (2026-11-22) for
-- C.QUICK_REPAIR_SKIP_OWN_FIRE: the quick (early blocker) repair row does
-- not read the friendly-fire stamp of our own tank's shells on their way to
-- the take target. Ally shells and our other shells still refuse it.
--   1. quick_repair.own_shell_at_target: owner, angle and life.
--   2. perception.stamp_friendly_fire: the two stamps.
--   3. the builder pool: our shell at the target lets the quick row send;
--      an ally shell, our shell aimed away, or the knob off refuse it
--      (friendly_fire).
--   3b. the stay window (2026-11-24): a DEAD blocker's friendly-fire stamp
--      (from before it died) refuses the row only while its hold still
--      runs when the builder arrives; a live blocker keeps the plain rule.
--   4. the presets: KEEL, MAST and the R221 table keep the old rule.
-- No engine: terrain, the walk sim and the shell death check are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   <luajit> test_qr_own_fire.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_qr_own_fire.lua
-- Against older sources (to show the test fails there):
--   <luajit> test_qr_own_fire.lua src=<dir with the old .lua files>
--   An old perception.lua has no stamp_friendly_fire; the test then stamps
--   with the old rule (every friendly shell within 1 tile of a friendly
--   live pill sets p._friendly_shot_tick), which is that file's own loop.
-- =========================================================================

local SRC
for _, a in ipairs(arg or {}) do
  local v = a:match("^src=(.+)$")
  if v then SRC = v end
end
package.path = (SRC and (SRC .. "/?.lua;") or "") .. "./?.lua;" .. package.path

package.loaded["print2"] = setmetatable({ set_tick = function() end },
  { __call = function() end })

if not math.atan2 then math.atan2 = function(y, x) return math.atan(y, x) end end

_G.OBJECT_TANK, _G.OBJECT_SHOT = 0, 1
_G.BUILDMODE_FARM, _G.BUILDMODE_PBOX = 1, 3

local bit    = require("bitcompat")
local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local BP     = require("builder_pool")
local QR     = require("quick_repair")
local PERC   = require("perception")

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

-- ── Fixture ──────────────────────────────────────────────────────────────
-- Tank (player 1) at (100,100). Take target: the enemy pill at (110,100).
-- Blocker: our pill at (104,100), _in_use, hp 5. Our shells land on the
-- target at NOW+10 and NOW+20, so the quick window is open (send).
local NOW = 1000
local function pill(mx, my, hp, owner, in_use)
  return { mx = mx, my = my, health = hp, owner = owner or "friendly",
           _in_use = in_use or nil }
end
local function mk(objects)
  local pills = {
    [3] = pill(110, 100, 2, "hostile"),
    [8] = pill(104, 100, 5, "friendly", true),
  }
  local world = { pills = pills, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(pills) do
    world.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } }
  end
  local state = {
    player_number = 1, tick = NOW,
    builder = { mode = "suppressed" },
    goal = { kind = "attack_pill", mx = 110, my = 100, target_id = 3,
             substate = "engage", _attack_start_tick = NOW - 50,
             _on_target_land = { NOW + 10, NOW + 20 },
             _on_target_land_tick = NOW },
  }
  local info = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128,
                 trees = 10, man_status = C.LGM_INTANK, inboat = false,
                 armour = 40, carried_pills = 0, player_number = 1,
                 reload = false, reload_ticks = 13, objects = objects or {} }
  return state, world, info
end
-- A friendly shell (info 0) at tile (mx, my) with an 8-bit angle.
-- Angle 64 = east (toward the target), 0 = north.
local function shell(mx, my, angle, owner, life)
  return { type = OBJECT_SHOT, x = mx * 256 + 128, y = my * 256 + 128,
           angle = angle, life = life or 60, owner = owner, info = 0 }
end
local function reset(skip)
  C.QUICK_REPAIR_ON_TAKE = true
  C.QUICK_REPAIR_PREDICT_SHOTS = true
  C.QUICK_REPAIR_MARGIN_TICKS = 2
  C.QUICK_REPAIR_SKIP_OWN_FIRE = skip
end
-- The perception stamp. An old perception.lua has no stamp_friendly_fire:
-- the old rule then runs (see the header).
local OLD_STAMP = PERC.stamp_friendly_fire == nil
local function stamp(state, world, info)
  if not OLD_STAMP then
    return PERC.stamp_friendly_fire(state, world, info, NOW)
  end
  for _, ob in ipairs(info.objects) do
    if ob.type == 1 and ob.info == 0 then
      local smx, smy = bit.rshift(ob.x, 8), bit.rshift(ob.y, 8)
      for _, p in pairs(world.pills) do
        if p.owner == "friendly" and p.health > 0 and not p.in_tank
           and math.abs(p.mx - smx) <= 1 and math.abs(p.my - smy) <= 1 then
          p._friendly_shot_tick = NOW
        end
      end
    end
  end
end
local function run_pool(state, world, info)
  state.tick = NOW
  stamp(state, world, info)
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
local function sent(cmd) return cmd and cmd.x == 104 and cmd.y == 100 end
local function cmd_s(cmd) return cmd and string.format("%d,%d", cmd.x, cmd.y) or "none" end

-- ── 1. own_shell_at_target ───────────────────────────────────────────────
print("1. own_shell_at_target")
do
  local f = QR.own_shell_at_target
  check("helper exists", type(f) == "function", type(f))
  if type(f) == "function" then
    check("our shell going east reaches the target",
          f(shell(105, 100, 64, 1), 1, 110, 100) == true)
    check("an ally shell is not ours", f(shell(105, 100, 64, 2), 1, 110, 100) == false)
    check("a pill shell is not ours", f(shell(105, 100, 64, 255), 1, 110, 100) == false)
    check("our shell going north misses the target",
          f(shell(105, 100, 0, 1), 1, 110, 100) == false)
    check("our shell with too little life falls short",
          f(shell(105, 100, 64, 1, 20), 1, 110, 100) == false)
    check("no target: false", f(shell(105, 100, 64, 1), 1, nil, nil) == false)
  end
end

-- ── 2. The two stamps ────────────────────────────────────────────────────
print("2. perception stamps")
do
  reset(true)
  local st, w, inf = mk({ shell(105, 100, 64, 1) })
  stamp(st, w, inf)
  local b = w.pills[8]
  check("our shell at the target: first stamp set", b._friendly_shot_tick == NOW,
        b._friendly_shot_tick)
  check("our shell at the target: second stamp not set", b._friendly_shot_other_tick == nil,
        b._friendly_shot_other_tick)

  reset(true)
  st, w, inf = mk({ shell(105, 100, 64, 2) })
  stamp(st, w, inf)
  b = w.pills[8]
  check("ally shell: both stamps set",
        b._friendly_shot_tick == NOW and b._friendly_shot_other_tick == NOW,
        tostring(b._friendly_shot_tick) .. "/" .. tostring(b._friendly_shot_other_tick))

  reset(true)
  st, w, inf = mk({ shell(104, 99, 0, 1) })
  stamp(st, w, inf)
  b = w.pills[8]
  check("our shell aimed away: both stamps set",
        b._friendly_shot_tick == NOW and b._friendly_shot_other_tick == NOW,
        tostring(b._friendly_shot_tick) .. "/" .. tostring(b._friendly_shot_other_tick))

  reset(false)
  st, w, inf = mk({ shell(105, 100, 64, 2) })
  stamp(st, w, inf)
  b = w.pills[8]
  check("knob off: first stamp set, second not",
        b._friendly_shot_tick == NOW and b._friendly_shot_other_tick == nil,
        tostring(b._friendly_shot_tick) .. "/" .. tostring(b._friendly_shot_other_tick))
end

-- ── 3. The builder pool ──────────────────────────────────────────────────
print("3. builder pool")
do
  reset(true)
  local st, w, inf = mk({ shell(105, 100, 64, 1) })
  local bp, cmd = run_pool(st, w, inf)
  local r = row_at(bp, 104, 100)
  check("our shell at the target: quick row not refused", r and r.quick and not r.reject,
        r and tostring(r.reject))
  check("our shell at the target: builder sent to the blocker", sent(cmd), cmd_s(cmd))

  reset(true)
  st, w, inf = mk({ shell(105, 100, 64, 2) })
  bp, cmd = run_pool(st, w, inf)
  r = row_at(bp, 104, 100)
  check("ally shell: no send", not sent(cmd), cmd_s(cmd))
  check("ally shell: row refused friendly_fire",
        r == nil or tostring(r.reject):find("friendly_fire", 1, true) ~= nil,
        r and tostring(r.reject))

  reset(true)
  st, w, inf = mk({ shell(104, 99, 0, 1) })
  bp, cmd = run_pool(st, w, inf)
  r = row_at(bp, 104, 100)
  check("our shell aimed away: no send", not sent(cmd), cmd_s(cmd))
  check("our shell aimed away: row refused friendly_fire",
        r == nil or tostring(r.reject):find("friendly_fire", 1, true) ~= nil,
        r and tostring(r.reject))

  reset(false)
  st, w, inf = mk({ shell(105, 100, 64, 1) })
  bp, cmd = run_pool(st, w, inf)
  r = row_at(bp, 104, 100)
  check("knob off, our shell at the target: no send (old rule)", not sent(cmd), cmd_s(cmd))
  check("knob off: row refused friendly_fire",
        r == nil or tostring(r.reject):find("friendly_fire", 1, true) ~= nil,
        r and tostring(r.reject))

  reset(true)
  st, w, inf = mk({})
  bp, cmd = run_pool(st, w, inf)
  check("no shells: builder sent (control)", sent(cmd), cmd_s(cmd))
end

-- ── 3b. Dead blocker: the friendly-fire hold against his arrival ────────
-- run_c8 beside_e: the blocker died at 637 with a stamp of 636; the hold
-- ran to 686 (REPAIR_FRIENDLY_FIRE_REJECT_TICKS 50) and held the pool,
-- though the builder (walk 85) would arrive at 723.
print("3b. dead blocker stay window")
do
  local RJ = C.REPAIR_FRIENDLY_FIRE_REJECT_TICKS or 50
  local function dead(bmx, stamp_age, quick_on, hp)
    reset(true)
    C.QUICK_REPAIR_ON_TAKE = quick_on
    local st, w, inf = mk({})
    local b = w.pills[8]
    w.pill_at[b.my * 256 + b.mx] = nil
    b.mx = bmx
    b.health = hp or 0
    if hp then b._in_use = nil end        -- a live take blocker is refused take_blocker first
    w.pill_at[b.my * 256 + b.mx] = { { pill = b, id = 8 } }
    b._friendly_shot_tick = NOW - stamp_age
    b._friendly_shot_other_tick = NOW - stamp_age
    local bp, cmd = run_pool(st, w, inf)
    return row_at(bp, bmx, 100), cmd
  end
  local function ff(r) return r and tostring(r.reject):find("friendly_fire", 1, true) ~= nil end
  -- (104,100): walk 4 tiles x 16 = 64, arrive NOW+65 > hold end NOW+49.
  local r = dead(104, 1, false)
  check("discovery row, dead blocker, hold ends before he arrives: not refused friendly_fire",
        r and not ff(r), r and tostring(r.reject))
  r = dead(104, 1, true)
  check("quick row, dead blocker, hold ends before he arrives: not refused friendly_fire",
        r and not ff(r), r and tostring(r.reject))
  -- (101,100): next to the tank, walk 16, arrive NOW+17 <= hold end NOW+49.
  r = dead(101, 1, false)
  check("discovery row, dead blocker, hold runs when he arrives: refused friendly_fire",
        ff(r), r and tostring(r.reject))
  -- The hold end equal to the arrival still holds: NOW-(RJ-65) + RJ = NOW+65.
  r = dead(104, RJ - 65, false)
  check("hold end == arrival: refused friendly_fire", ff(r), r and tostring(r.reject))
  r = dead(104, RJ - 64, false)
  check("hold end one tick before arrival: not refused", r and not ff(r), r and tostring(r.reject))
  -- A live pill keeps the plain rule (the team shoots it to move it).
  r = dead(104, 1, false, 5)
  check("live pill: refused friendly_fire (plain rule)", ff(r),
        r and tostring(r.reject))
end

-- ── 4. Presets ───────────────────────────────────────────────────────────
print("4. presets")
do
  local D = dofile((SRC and (SRC .. "/") or "") .. "constants.lua")
  check("default: QUICK_REPAIR_SKIP_OWN_FIRE = true", D.QUICK_REPAIR_SKIP_OWN_FIRE == true,
        D.QUICK_REPAIR_SKIP_OWN_FIRE)
  check("keel: QUICK_REPAIR_SKIP_OWN_FIRE = false",
        D.PRESETS.keel.QUICK_REPAIR_SKIP_OWN_FIRE == false)
  check("mast: QUICK_REPAIR_SKIP_OWN_FIRE = false",
        D.PRESETS.mast.QUICK_REPAIR_SKIP_OWN_FIRE == false)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
