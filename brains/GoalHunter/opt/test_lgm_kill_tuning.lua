-- =========================================================================
-- test_lgm_kill_tuning.lua — standalone unit tests for the 2026-09-13
-- LGM-kill package (master knob C.LGM_KILL_IMPROVED), as resolved by
-- kill_lgm.tuning():
--   * hard (no level bundle): the package values
--   * medium / easy (MODE_LEVELS) and turtle: master off, today's values
--   * PRESETS.keel: master off and every knob at its KEEL value
--   * CAPTURE_LGM_HUNT_FIRE_WU >= 0 overrides the capture shot only
-- No engine: knobs are written straight into constants and kill_lgm is
-- reloaded, because tuning() caches its table (reset_tuning() drops it).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_lgm_kill_tuning.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")

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

local KNOBS = {
  "LGM_KILL_IMPROVED", "LGM_KILL_FIRE_WU", "KILL_LGM_FIRE_WU",
  "CAPTURE_LGM_HUNT_FIRE_WU", "CAPTURE_LGM_HUNT_TRACK_TILES",
  "CAPTURE_LGM_HUNT_PICK_NEAREST_PILL", "CAPTURE_SIGHT_HOLD_MIN_STEPS",
  "LGM_KILL_AIM_WU", "LGM_KILL_AIM_TILES", "LGM_KILL_AIM_THROTTLE",
  "LGM_ENGINE_KILL_WU",
}
local DEFAULTS = {}
for _, k in ipairs(KNOBS) do DEFAULTS[k] = C[k] end

local function restore()
  for _, k in ipairs(KNOBS) do C[k] = DEFAULTS[k] end
end

-- Apply a knob table (a level bundle or a preset) over the defaults, then
-- reload kill_lgm so tuning() builds a fresh table.
local function tuning_with(tbl)
  restore()
  for k, v in pairs(tbl or {}) do C[k] = v end
  package.loaded["kill_lgm"] = nil
  return require("kill_lgm").tuning()
end

-- ── 1. defaults in constants.lua ──────────────────────────────────────────
print("defaults")
check("LGM_KILL_IMPROVED = true", C.LGM_KILL_IMPROVED == true, C.LGM_KILL_IMPROVED)
check("LGM_KILL_FIRE_WU = 144", C.LGM_KILL_FIRE_WU == 144, C.LGM_KILL_FIRE_WU)
check("CAPTURE_LGM_HUNT_FIRE_WU = -1", C.CAPTURE_LGM_HUNT_FIRE_WU == -1, C.CAPTURE_LGM_HUNT_FIRE_WU)
check("CAPTURE_LGM_HUNT_TRACK_TILES = 10", C.CAPTURE_LGM_HUNT_TRACK_TILES == 10, C.CAPTURE_LGM_HUNT_TRACK_TILES)
check("CAPTURE_LGM_HUNT_PICK_NEAREST_PILL = true", C.CAPTURE_LGM_HUNT_PICK_NEAREST_PILL == true)
check("CAPTURE_SIGHT_HOLD_MIN_STEPS = 2", C.CAPTURE_SIGHT_HOLD_MIN_STEPS == 2, C.CAPTURE_SIGHT_HOLD_MIN_STEPS)
check("LGM_KILL_AIM_WU = 64", C.LGM_KILL_AIM_WU == 64, C.LGM_KILL_AIM_WU)
check("LGM_KILL_AIM_TILES = 5", C.LGM_KILL_AIM_TILES == 5, C.LGM_KILL_AIM_TILES)
check("LGM_KILL_AIM_THROTTLE = false", C.LGM_KILL_AIM_THROTTLE == false)

-- ── 2. hard: the package is on ────────────────────────────────────────────
print("hard (package on)")
local hard = (C.MODE_LEVELS.default or {}).hard
check("default/hard sets no LGM_KILL_IMPROVED", hard and hard.LGM_KILL_IMPROVED == nil)
local t = tuning_with(hard)
check("on", t.on == true, t.on)
check("fire_wu = 144", t.fire_wu == 144, t.fire_wu)
check("capture_fire_wu follows the shared check (144)", t.capture_fire_wu == 144, t.capture_fire_wu)
check("armour_fire_wu = 144", t.armour_fire_wu == 144, t.armour_fire_wu)
check("perp_wu equals the fire check (144)", t.perp_wu == 144, t.perp_wu)
check("track_wu = 10 tiles", t.track_wu == 10 * 256, t.track_wu)
check("pick_near_pill", t.pick_near_pill == true)
check("hold_min_steps = 2", t.hold_min_steps == 2, t.hold_min_steps)
check("aim_wu = 64", t.aim_wu == 64, t.aim_wu)
check("aim_tiles = 5", t.aim_tiles == 5, t.aim_tiles)
check("aim_throttle off", t.aim_throttle == false)
check("engine_kill_wu = 128", t.engine_kill_wu == 128, t.engine_kill_wu)

-- ── 3. medium, easy and turtle: master off, today's values ────────────────
for _, lv in ipairs({ { "default", "medium" }, { "default", "easy" },
                      { "turtle", "medium" }, { "turtle", "easy" } }) do
  local name = lv[1] .. "/" .. lv[2]
  print(name .. " (package off)")
  local b = C.MODE_LEVELS[lv[1]][lv[2]]
  check(name .. " sets LGM_KILL_IMPROVED = false", b.LGM_KILL_IMPROVED == false)
  local o = tuning_with(b)
  check(name .. " off", o.on == false, o.on)
  check(name .. " fire_wu = KILL_LGM_FIRE_WU (128)", o.fire_wu == 128, o.fire_wu)
  check(name .. " capture_fire_wu = 100 (pre-package)", o.capture_fire_wu == 100, o.capture_fire_wu)
  check(name .. " armour_fire_wu = 64 (pre-package)", o.armour_fire_wu == 64, o.armour_fire_wu)
  check(name .. " perp_wu = 128", o.perp_wu == 128, o.perp_wu)
  check(name .. " track_wu nil (old along-the-ray admission)", o.track_wu == nil, o.track_wu)
  check(name .. " old pick", o.pick_near_pill == false)
  check(name .. " hold_min_steps = 2", o.hold_min_steps == 2, o.hold_min_steps)
  check(name .. " no aim refinement", o.aim_wu == nil and o.aim_tiles == nil)
end
check("turtle/hard sets no LGM_KILL_IMPROVED",
      C.MODE_LEVELS.turtle.hard.LGM_KILL_IMPROVED == nil)

-- ── 4. keel ───────────────────────────────────────────────────────────────
print("keel")
local kp = C.PRESETS and C.PRESETS.keel or {}
check("PRESETS.keel LGM_KILL_IMPROVED = false", kp.LGM_KILL_IMPROVED == false)
check("PRESETS.keel LGM_KILL_FIRE_WU = 64", kp.LGM_KILL_FIRE_WU == 64, kp.LGM_KILL_FIRE_WU)
check("PRESETS.keel CAPTURE_LGM_HUNT_FIRE_WU = 64", kp.CAPTURE_LGM_HUNT_FIRE_WU == 64, kp.CAPTURE_LGM_HUNT_FIRE_WU)
check("PRESETS.keel CAPTURE_LGM_HUNT_TRACK_TILES = 7", kp.CAPTURE_LGM_HUNT_TRACK_TILES == 7, kp.CAPTURE_LGM_HUNT_TRACK_TILES)
check("PRESETS.keel CAPTURE_LGM_HUNT_PICK_NEAREST_PILL = false", kp.CAPTURE_LGM_HUNT_PICK_NEAREST_PILL == false)
check("PRESETS.keel CAPTURE_SIGHT_HOLD_MIN_STEPS = 2", kp.CAPTURE_SIGHT_HOLD_MIN_STEPS == 2, kp.CAPTURE_SIGHT_HOLD_MIN_STEPS)
check("PRESETS.keel LGM_KILL_AIM_WU = 64", kp.LGM_KILL_AIM_WU == 64, kp.LGM_KILL_AIM_WU)
check("PRESETS.keel LGM_KILL_AIM_TILES = 5", kp.LGM_KILL_AIM_TILES == 5, kp.LGM_KILL_AIM_TILES)
check("PRESETS.keel LGM_KILL_AIM_THROTTLE = false", kp.LGM_KILL_AIM_THROTTLE == false)
check("PRESETS.keel LGM_ENGINE_KILL_WU = 128", kp.LGM_ENGINE_KILL_WU == 128, kp.LGM_ENGINE_KILL_WU)
-- Every keel key must name a real knob, or the preset path refuses it.
for k in pairs(kp) do
  if k:match("^LGM_KILL_") or k:match("^CAPTURE_LGM_HUNT_") or k == "CAPTURE_SIGHT_HOLD_MIN_STEPS"
     or k == "LGM_ENGINE_KILL_WU" then
    check("keel key " .. k .. " exists in constants", DEFAULTS[k] ~= nil or C[k] ~= nil)
  end
end
local k = tuning_with(kp)
check("keel off", k.on == false, k.on)
check("keel fire_wu = 64", k.fire_wu == 64, k.fire_wu)
check("keel capture_fire_wu = 64", k.capture_fire_wu == 64, k.capture_fire_wu)
check("keel armour_fire_wu = 64", k.armour_fire_wu == 64, k.armour_fire_wu)
check("keel perp_wu = 128", k.perp_wu == 128, k.perp_wu)
check("keel track_wu nil", k.track_wu == nil, k.track_wu)
check("keel old pick", k.pick_near_pill == false)
check("keel hold_min_steps = 2", k.hold_min_steps == 2, k.hold_min_steps)
check("keel no aim refinement", k.aim_wu == nil and k.aim_tiles == nil)

-- ── 5. CAPTURE_LGM_HUNT_FIRE_WU override ──────────────────────────────────
print("capture override")
local ov = tuning_with({ CAPTURE_LGM_HUNT_FIRE_WU = 150 })
check("on: override 150 moves the capture shot only", ov.capture_fire_wu == 150 and ov.fire_wu == 144,
      tostring(ov.capture_fire_wu) .. "/" .. tostring(ov.fire_wu))
local ovo = tuning_with({ LGM_KILL_IMPROVED = false, CAPTURE_LGM_HUNT_FIRE_WU = 80 })
check("off: override 80 is still read", ovo.capture_fire_wu == 80, ovo.capture_fire_wu)
local nl = tuning_with({ LGM_KILL_AIM_TILES = 0 })
check("aim_tiles 0 = no limit", nl.aim_tiles == 0, nl.aim_tiles)

-- ── 6. tuning() caches its table ──────────────────────────────────────────
print("cache")
restore()
package.loaded["kill_lgm"] = nil
local KL = require("kill_lgm")
local first = KL.tuning()
C.LGM_KILL_FIRE_WU = 999
check("second call returns the cached table", KL.tuning() == first and KL.tuning().fire_wu == 144)
KL.reset_tuning()     -- what Brain.on_init does after a runtime preset= / cfg=
check("reset_tuning: the next call reads the constants again",
      KL.tuning() ~= first and KL.tuning().fire_wu == 999, KL.tuning().fire_wu)
restore()
KL.reset_tuning()

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
