-- =========================================================================
-- test_qr_tank_clear.lua — standalone unit tests (2026-11-22) for
-- C.QUICK_REPAIR_TANK_CLEAR_TICKS (quick_repair.tank_clear): while a live
-- non-friendly pill can shoot our tank, the quick repair sends only when no
-- non-friendly shell lands on the tank within the window AND no such pill's
-- next shot (last fire + reload, plus the flight to the tank) can land in it.
--   1. far pill, no shells: send (control).
--   2. a pill shell lands on the tank in 4 ticks: wait:tank (shell).
--   3. a pill 2 tiles from the tank, gun ready: its next shot lands in 13
--      ticks: wait:tank (next).
--   4. the same pill fired 1 tick ago at reload 100 (calm): send.
--   5. the same pill fired 3 ticks ago, angry (reload from the ladder):
--      wait:tank (next).
--   6. a dead pill, a friendly pill: they do not count.
--   7. knob 0: the shell of case 2 does not stop the send.
--   8. the builder pool does not send on wait:tank.
--   9. presets: KEEL and MAST 0, default 16.
-- No engine: terrain and the walk sim are stubs.
--
-- Run from this directory with the LuaJIT built beside the game:
--   <luajit> test_qr_tank_clear.lua
-- Against the stripped production copies, run it from opt/:
--   cd opt && <luajit> ../test_qr_tank_clear.lua
-- Against older sources (to show the test fails there):
--   <luajit> test_qr_tank_clear.lua src=<dir with the old .lua files>
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

local C      = require("constants")
local U      = require("util")
local danger = require("danger")
local threat = require("threat")
local PP     = require("pill_portfolio")
local AS     = require("ally_state")
local BP     = require("builder_pool")
local QR     = require("quick_repair")
local KLK    = require("pt2_kill_lock")
local PA     = require("pt2_attack")

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
-- Tank (player 1) at (100,100). Take target: the neutral pill at (106,100),
-- hp 2, 6 tiles east (inside its 8-tile range). Blocker: our pill at
-- (103,100), _in_use, hp 5. Our shells land on the target at NOW+10 and
-- NOW+20, so every other quick repair test passes (send).
local NOW = 1000
local function pill(mx, my, hp, owner, in_use)
  return { mx = mx, my = my, health = hp, owner = owner or "friendly",
           _in_use = in_use or nil, anger = 0, anger_tick = 0 }
end
local function mk(opts)
  opts = opts or {}
  local pills = {
    [3] = pill(106, 100, 2, "neutral"),
    [8] = pill(103, 100, 5, "friendly", true),
  }
  for k, v in pairs(opts.extra or {}) do pills[k] = v end
  local world = { pills = pills, pill_at = {}, bases = {}, base_at = {} }
  for id, p in pairs(pills) do
    world.pill_at[p.my * 256 + p.mx] = { { pill = p, id = id } }
  end
  local state = {
    player_number = 1, tick = NOW,
    builder = { mode = "suppressed" },
    goal = { kind = "attack_pill", mx = 106, my = 100, target_id = 3,
             substate = "engage", _attack_start_tick = NOW - 50,
             _on_target_land = { NOW + 10, NOW + 20 },
             _on_target_land_tick = NOW },
  }
  local info = { tankx = 100 * 256 + 128, tanky = 100 * 256 + 128,
                 trees = 10, man_status = C.LGM_INTANK, inboat = false,
                 armour = 40, carried_pills = 0, player_number = 1,
                 reload = false, reload_ticks = 13, objects = opts.objects or {} }
  return state, world, info
end
-- A pill shell (owner 255, neutral) at world (x, y) with an 8-bit angle.
local function pshell(x, y, angle, life)
  return { type = OBJECT_SHOT, x = x, y = y, angle = angle,
           life = life or 50, owner = 255, info = 2 }
end
local function reset(w)
  C.QUICK_REPAIR_ON_TAKE = true
  C.QUICK_REPAIR_PREDICT_SHOTS = true
  C.QUICK_REPAIR_MARGIN_TICKS = 2
  C.QUICK_REPAIR_SKIP_OWN_FIRE = true
  C.QUICK_REPAIR_TANK_CLEAR_TICKS = w
end
local function eval(opts)
  local st, w, inf = mk(opts)
  local qr = QR.evaluate(st, w, inf, NOW)
  return qr, st, w, inf
end
local function res(qr) return qr and qr.result or "nil" end
-- A pill 2 tiles north of the tank (100,98): its shell flies 12 ticks to the
-- tank tile, so a shot fired at now + 1 lands at + 13 (<= 16).
local function north_pill(anger)
  local p = pill(100, 98, 15, "hostile")
  p.anger, p.anger_tick = anger or 0, NOW
  return p
end

-- ── 1. control ───────────────────────────────────────────────────────────
print("1. control")
do
  reset(16)
  local qr = eval()
  check("far pill, no shells: send", res(qr) == "send", res(qr))
end

-- ── 2. a shell on the tank ───────────────────────────────────────────────
print("2. shell on the tank")
do
  reset(16)
  -- Half a tile east of the tank tile edge, flying west (angle 192): it is
  -- on the edge after 4 moves and inside the tank tile after 5.
  local qr = eval({ objects = { pshell(101 * 256 + 128, 100 * 256 + 128, 192) } })
  check("shell lands on the tank at +5: wait:tank", res(qr) == "wait:tank", res(qr))
  check("why = shell, t = 5", qr and qr.tank and qr.tank.why == "shell" and qr.tank.t == 5,
        qr and qr.tank and (tostring(qr.tank.why) .. "/" .. tostring(qr.tank.t)))
  -- The same shell flying north misses the tank.
  qr = eval({ objects = { pshell(101 * 256 + 128, 100 * 256 + 128, 0) } })
  check("shell flying away: send", res(qr) == "send", res(qr))
end

-- ── 3-5. the next shot ───────────────────────────────────────────────────
print("3. next shot, gun ready")
do
  reset(16)
  local qr = eval({ extra = { [5] = north_pill(0) } })
  check("pill 2 tiles out, no shell seen: wait:tank", res(qr) == "wait:tank", res(qr))
  check("why = next, t = 13", qr and qr.tank and qr.tank.why == "next" and qr.tank.t == 13,
        qr and qr.tank and (tostring(qr.tank.why) .. "/" .. tostring(qr.tank.t)))
end
print("4. next shot, calm pill fired 1 tick ago")
do
  reset(16)
  -- Its shell left the pill tile 1 tick ago flying north (away from us).
  local sh = pshell(100 * 256 + 128, 98 * 256 + 128 - 32, 0)
  local qr = eval({ extra = { [5] = north_pill(0) }, objects = { sh } })
  check("reload 100 from the last fire: send", res(qr) == "send", res(qr))
end
print("5. next shot, angry pill")
do
  reset(16)
  local r = KLK.pill_reload(PA.anger_now(north_pill(1.0), NOW), {})
  check("angry reload is shorter than calm (100)", r < 100, r)
  -- Fired r - 2 ticks ago: the next shot is at now + 2 and lands at +14.
  local ago = r - 2
  local sh = pshell(100 * 256 + 128, 98 * 256 + 128 - ago * 32, 0)
  local qr = eval({ extra = { [5] = north_pill(1.0) }, objects = { sh } })
  local want = (NOW - ago + r) - NOW + 12
  check("angry pill: wait:tank", res(qr) == "wait:tank", res(qr))
  check(string.format("why = next, t = last %+d + reload %d + flight 12 = %d", -ago, r, want),
        qr and qr.tank and qr.tank.why == "next" and qr.tank.t == want,
        qr and qr.tank and (tostring(qr.tank.why) .. "/" .. tostring(qr.tank.t)))
  -- Fired ago2 ticks ago: next at now + r - ago2, lands at r - ago2 + 12
  -- > 16: send. ago2 = 6 at reload 12; PT2_START_ANGER_RISE puts anger
  -- 1.0 at the minimum reload 6, then ago2 = 1.
  local ago2 = math.max(1, r - 6)
  sh = pshell(100 * 256 + 128, 98 * 256 + 128 - ago2 * 32, 0)
  qr = eval({ extra = { [5] = north_pill(1.0) }, objects = { sh } })
  check(string.format("angry pill fired %d ago: lands at +%d > 16: send", ago2, r - ago2 + 12),
        res(qr) == "send", res(qr))
end

-- ── 6. pills that do not count ───────────────────────────────────────────
print("6. pills that do not count")
do
  reset(16)
  local dead = north_pill(0); dead.health = 0
  local qr = eval({ extra = { [5] = dead } })
  check("dead pill 2 tiles out: send", res(qr) == "send", res(qr))
  local own = north_pill(0); own.owner = "friendly"
  qr = eval({ extra = { [5] = own } })
  check("friendly pill 2 tiles out: send", res(qr) == "send", res(qr))
end

-- ── 7. knob off ──────────────────────────────────────────────────────────
print("7. knob 0")
do
  reset(0)
  local qr = eval({ objects = { pshell(101 * 256 + 128, 100 * 256 + 128, 192) },
                    extra = { [5] = north_pill(0) } })
  check("knob 0: send as before", res(qr) == "send", res(qr))
end

-- ── 8. the builder pool ──────────────────────────────────────────────────
print("8. builder pool")
do
  reset(16)
  local st, w, inf = mk({ objects = { pshell(101 * 256 + 128, 100 * 256 + 128, 192) } })
  local bp = BP.update(st, w, inf, NOW)
  local cmd = BP.rung(st, w, inf, NOW)
  check("wait:tank: no send to the blocker", not (cmd and cmd.x == 103 and cmd.y == 100),
        cmd and string.format("%d,%d", cmd.x, cmd.y))
  reset(16)
  st, w, inf = mk({})
  bp = BP.update(st, w, inf, NOW)
  cmd = BP.rung(st, w, inf, NOW)
  check("clear: builder sent to the blocker (control)", cmd and cmd.x == 103 and cmd.y == 100,
        cmd and string.format("%d,%d", cmd.x, cmd.y))
end

-- ── 9. presets ───────────────────────────────────────────────────────────
print("9. presets")
do
  local D = dofile((SRC and (SRC .. "/") or "") .. "constants.lua")
  check("default: QUICK_REPAIR_TANK_CLEAR_TICKS = 16", D.QUICK_REPAIR_TANK_CLEAR_TICKS == 16,
        D.QUICK_REPAIR_TANK_CLEAR_TICKS)
  check("keel: 0", D.PRESETS.keel.QUICK_REPAIR_TANK_CLEAR_TICKS == 0)
  check("mast: 0", D.PRESETS.mast.QUICK_REPAIR_TANK_CLEAR_TICKS == 0)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
