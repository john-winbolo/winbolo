-- =========================================================================
-- test_shield_overlay_aim9.lua -- standalone unit tests (2026-11-23, E3-15)
-- for attack_shield.draw_overlay with a pill take v2 result whose aim is
-- 6..9 (PT2_AIM_POINTS = 9): the aim line ends at v2's own aim point
-- (pilltake_v2.AIMS9), the aim has its own colour, and the sparse aims
-- table (only the best aim) still draws its blockers and legend row.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_shield_overlay_aim9.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path
if math.atan2 then
  local _atan1, _atan2 = math.atan, math.atan2
  math.atan = function(y, x) if x == nil then return _atan1(y) else return _atan2(y, x) end end
end

local calls = {}
local viz = require("viz")
for _, k in ipairs({ "line", "rect", "circle", "text", "detail_text" }) do
  viz[k] = function(layer, ...) calls[#calls + 1] = { k = k, layer = layer, ... } end
end
viz.detail_live = false

local S  = require("attack_shield")
local V2 = require("pilltake_v2")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then pass = pass + 1; print("  ok   " .. name)
  else fail = fail + 1; print(string.format("  FAIL %s   (got %s)", name, tostring(got))) end
end

_G.BRAIN_DEBUG_MODE = true
print("E3-15: v2 aims 6..9 on the overlay")
for idx = 6, 9 do
  for i = #calls, 1, -1 do calls[i] = nil end
  local best = { cx = 20.5, cy = 30.5, best_aim_idx = idx, aims = {},
                 return_fire = { tiles = nil } }
  best.aims[idx] = { blocked = false, blockers = { { mx = 15, my = 25 } },
                     potential_blockers = { { mx = 16, my = 26 } } }
  local ok, err = pcall(S.draw_overlay, { best = best, pill = { mx = 10, my = 12 } }, 0)
  check(string.format("aim %d: draws with no error", idx), ok, err)
  -- the opt/ copy has no debug drawing (strip.sh): nothing more to test
  if ok and #calls == 0 then
    print("  (no overlay in this build: the drawing checks are skipped)")
    break
  end
  local want = V2.AIMS9[idx]
  local line
  for _, c in ipairs(calls) do
    if c.k == "line" and c.layer == "shield_scan_trajectory" then line = c end
  end
  check(string.format("aim %d: the line ends at v2's aim point", idx),
        line and math.abs(line[3] - (10 + want[1] / 256)) < 1e-9
             and math.abs(line[4] - (12 + want[2] / 256)) < 1e-9,
        line and string.format("%.4f,%.4f", line[3], line[4]))
  local col = S.AIM_COLORS[idx]
  check(string.format("aim %d: its own colour", idx),
        line and col and line[5] == col[1] and line[6] == col[2] and line[7] == col[3])
  local blk, legend = false, false
  for _, c in ipairs(calls) do
    if c.layer == "shield_scan_blockers" then blk = true end
    if c.k == "text" and c.layer == "shield_scan_legend"
       and tostring(c[3]):find("^" .. S.AIM_NAMES9[idx] .. ":1%+1") then legend = true end
  end
  check(string.format("aim %d: its blockers drawn", idx), blk)
  check(string.format("aim %d: its legend row", idx), legend)
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
