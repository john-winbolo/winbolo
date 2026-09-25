-- =========================================================================
-- test_spot_margin.lua — standalone unit tests for the blitz spot LOS margin
-- (spot_margin.lua, knob C.BLITZ_SPOT_LOS_MARGIN). No engine: the shell
-- simulator and terrain reads are stubbed, the geometry is real.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_spot_margin.lua
-- or with any lua on PATH.
-- =========================================================================

package.path = "./?.lua;" .. package.path

-- Stubs for the engine-backed modules. util: terrain is all grass (the
-- blockers in these tests come from ctx lookups). cpathfinder: every shell
-- reaches the pill tile straight away (a clear line), and we count calls.
local n_sims = 0
package.loaded["util"] = {
  ttype      = function() return 4 end,
  ttype_peek = function() return 4 end,
}
package.loaded["cpathfinder"] = {
  SHOT_TANK = 0,
  simulate_shot = function(ox, oy, awx, awy)
    n_sims = n_sims + 1
    return { { mx = math.floor(awx / 256), my = math.floor(awy / 256) } }
  end,
}
local printed = {}
package.loaded["print2"] = function(s) printed[#printed + 1] = s; print("    [print2] " .. s) end

local ok_shield, shield = pcall(require, "attack_shield")
if not ok_shield then
  -- Fallback: the same table attack_shield builds (AIM_INSET_FIRE = 16 wu).
  local f = 16 / 256.0
  shield = {
    AIM_OFFSETS_TILE_FIRE = { { 0.5, 0.5 }, { f, f }, { 1 - f, f }, { f, 1 - f }, { 1 - f, 1 - f } },
    AIM_NAMES = { "center", "TL", "TR", "BL", "BR" },
  }
  package.loaded["attack_shield"] = shield
end

local SM = require("spot_margin")
print("aim set: " .. (ok_shield and "real attack_shield.lua" or "fallback copy (attack_shield failed to load standalone)"))

local fails, passes = 0, 0
local function check(name, cond, detail)
  if cond then passes = passes + 1; print("PASS  " .. name .. (detail and ("  " .. detail) or ""))
  else fails = fails + 1; print("FAIL  " .. name .. (detail and ("  " .. detail) or "")) end
end
local function near(a, b, tol) return math.abs(a - b) <= (tol or 0.005) end

-- A ctx whose blockers are exactly the listed tiles.
local function ctx_with(pmx, pmy, list)
  local set = {}
  for _, b in ipairs(list) do set[b[2] * 256 + b[1]] = b[3] or "pill" end
  return SM.new_ctx(nil, pmx, pmy, function(x, y) return set[y * 256 + x] or false end)
end

local MARGIN = 0.5

-- ── 1. Brief example 1: tile-centre spot (126.5,144.5), aim pill centre
--      (123.5,137.5), blocker = our pill #15 on tile (126,142). ──────────────
do
  print("\n-- 1. traced spot, tile centre (126.5,144.5) -> aim (123.5,137.5), blocker (126,142)")
  local off, d, L = SM.seg_square_dist(123.5, 137.5, 126.5, 144.5, 126, 142)
  local need = MARGIN * d / L
  print(string.format("    off=%.3f d=%.3f L=%.3f need=%.3f (=0.5*%.3f/%.3f)", off, d, L, need, d, L))
  check("1a L = sqrt(58)", near(L, 7.616))
  check("1b nearest corner (126,143) off ~0.13", near(off, 0.131))
  check("1c d ~6.0", near(d, 6.040, 0.01))
  check("1d need ~0.40", near(need, 0.397, 0.005))
  local ctx = ctx_with(123, 137, { { 126, 142, "pill" } })
  local ok, f = SM.line_margin(ctx, 126.5, 144.5, 123.5, 137.5, MARGIN)
  check("1e line_margin FAILS", ok == false and f and f.bx == 126 and f.by == 142,
        f and string.format("blocker=(%d,%d) off=%.3f need=%.3f", f.bx, f.by, f.off, f.need))
end

-- ── 2. Diagonal-adjacent blocker near the aim point passes. Pill (123,137),
--      blocker (124,138). The brief's example origin (126.5,144.5) would run
--      the line straight THROUGH that tile, so the origin is moved to
--      (131.0,139.0): the line then misses the tile's corner (125,138) by
--      ~0.20 tile at d ~1.57, where the tapered margin is only ~0.10. ─────────
do
  print("\n-- 2. diagonal neighbour (124,138), origin (131.0,139.0) -> aim (123.5,137.5)")
  local off, d, L = SM.seg_square_dist(123.5, 137.5, 131.0, 139.0, 124, 138)
  local need = MARGIN * d / L
  print(string.format("    off=%.3f d=%.3f L=%.3f need=%.3f (=0.5*%.3f/%.3f)", off, d, L, need, d, L))
  check("2a off ~0.20", near(off, 0.196))
  check("2b d ~1.57", near(d, 1.569, 0.01))
  check("2c need ~0.10", near(need, 0.103, 0.005))
  check("2d a flat 0.5 margin would have failed it", off < MARGIN)
  local ctx = ctx_with(123, 137, { { 124, 138, "wall" } })
  local ok, f = SM.line_margin(ctx, 131.0, 139.0, 123.5, 137.5, MARGIN)
  check("2e line_margin PASSES", ok == true, f and string.format("unexpected fail off=%.3f need=%.3f", f.off, f.need))
end

-- ── 3. Same blocker as 1, from the scan's FLOAT point (126.03,144.45) that
--      the recording's scan actually tested (fix A). The line has the room. ──
do
  print("\n-- 3. traced spot, scan float point (126.03,144.45), blocker (126,142)")
  local off, d, L = SM.seg_square_dist(123.5, 137.5, 126.03, 144.45, 126, 142)
  local need = MARGIN * d / L
  print(string.format("    off=%.3f d=%.3f L=%.3f need=%.3f", off, d, L, need))
  local ctx = ctx_with(123, 137, { { 126, 142, "pill" } })
  local ok = SM.line_margin(ctx, 126.03, 144.45, 123.5, 137.5, MARGIN)
  check("3a float point PASSES (off > need)", ok == true and off > need)
  -- And where the tank really stopped, (126.75,144.63): the line enters the tile.
  local off2 = SM.seg_square_dist(123.5, 137.5, 126.75, 144.63, 126, 142)
  check("3b real stop point (126.75,144.63) touches (126,142)", off2 < 0.01, string.format("off=%.3f", off2))
end

-- ── 4. Geometry edge cases. ──────────────────────────────────────────────
do
  print("\n-- 4. edge cases")
  local off, d = SM.seg_square_dist(0.5, 0.5, 10.5, 0.5, 4, 0)
  check("4a segment through the square: off 0, d = entry point", off == 0 and near(d, 3.5), string.format("off=%.3f d=%.3f", off, d))
  local off2, d2 = SM.seg_square_dist(0.5, 0.5, 10.5, 0.5, 4, 2)
  check("4b square beside the segment: off = gap to its edge", near(off2, 1.5) and d2 >= 3.5 and d2 <= 4.5, string.format("off=%.3f d=%.3f", off2, d2))
  local off3, d3 = SM.seg_square_dist(0.5, 0.5, 3.5, 0.5, 6, 0)
  check("4c square past the origin end: off from the end point, d = L", near(off3, 2.5) and near(d3, 3.0))
  -- Target pill tile and origin tile never count.
  local ctx = ctx_with(123, 137, { { 123, 137, "pill" }, { 131, 139, "base" } })
  check("4d target pill tile + origin tile ignored", SM.line_margin(ctx, 131.0, 139.0, 123.5, 137.5, MARGIN) == true)
  local ctx2 = ctx_with(123, 137, { { 126, 142, "pill" } })
  check("4e margin 0 = check off", SM.line_margin(ctx2, 126.5, 144.5, 123.5, 137.5, 0) == true)
  -- Only tiles in the line's bounding box (grown by the margin) are read.
  local reads_far = 0
  local ctx3 = SM.new_ctx(nil, 123, 137, function(x, y)
    if x == 120 and y == 120 then reads_far = reads_far + 1; return "wall" end
    return false
  end)
  check("4f blocker far outside the line's box is never read",
        SM.line_margin(ctx3, 131.0, 139.0, 123.5, 137.5, MARGIN) == true and reads_far == 0)
end

-- ── 5. clear_aim_margin: centre fails, a corner passes both tests. Pill
--      (123,137), origin (131.0,139.0), blocker (126,137): the centre line
--      grazes its corner (126,138), TL and TR run into it, BL clears it. ─────
do
  print("\n-- 5. clear_aim_margin picks the aim point that passes margin + shell")
  local ctx = ctx_with(123, 137, { { 126, 137, "wall" } })
  n_sims = 0
  local idx, awx, awy, trees = SM.clear_aim_margin(131 * 256, 139 * 256, 123, 137, nil, MARGIN,
    { ctx = ctx, site = "test", tick = 1 })
  check("5a returns BL (idx 4)", idx == 4, "idx=" .. tostring(idx))
  check("5b one shell sim only (margin runs first)", n_sims == 1, "sims=" .. n_sims)
  -- Trusted aim: no simulation at all.
  n_sims = 0
  local idx2 = SM.clear_aim_margin(131 * 256, 139 * 256, 123, 137, nil, MARGIN,
    { ctx = ctx, prefer_idx = 4, trusted_idx = 4, trusted_trees = 0, site = "test", tick = 1 })
  check("5c trusted preferred aim: 0 sims", idx2 == 4 and n_sims == 0, "sims=" .. n_sims)
  -- Example 1 spot: every aim fails the margin -> nil + one reject line.
  printed = {}
  local ctx1 = ctx_with(123, 137, { { 126, 142, "pill" } })
  n_sims = 0
  local idx3 = SM.clear_aim_margin(math.floor(126.5 * 256), math.floor(144.5 * 256), 123, 137, nil, MARGIN,
    { ctx = ctx1, site = "test", tick = 6699 })
  check("5d example-1 spot rejected on all 5 aims", idx3 == nil and n_sims == 0)
  check("5e one BLITZ_SPOT_MARGIN_REJECT line with every factor",
        #printed == 1 and printed[1]:find("BLITZ_SPOT_MARGIN_REJECT", 1, true)
        and printed[1]:find("blocker=(126,142)", 1, true) and printed[1]:find("need=", 1, true) ~= nil)
end

print(string.format("\n%d passed, %d failed", passes, fails))
os.exit(fails == 0 and 0 or 1)
