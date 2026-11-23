-- =========================================================================
-- test_base_shot.lua — standalone unit tests (2026-11-02) for the shared
-- "does a base stop this shell?" rule (util.lua base_stops_tank_shell and
-- friends) and the shell-line checks that call it.
--
-- The engine rule (src/bolo/shells.c, src/bolo/bases.c basesCanHit): a tank
-- shell from a boat stops at any base; from land it stops only at a base
-- that is not ours or an ally's, not neutral, and has armour above
-- rules.base_hit_armour. A pill shell (owner NEUTRAL) never stops at a base.
--
-- Cases:
--   * own base on the line does not block
--   * allied base does not block
--   * neutral base does not block
--   * hostile base above base_hit_armour blocks
--   * hostile base at/below base_hit_armour does not block (only when the
--     brain can prove it: health 0 and base_hit_armour >= capture armour)
--   * any base blocks when the tank is on a boat
--   * the call sites agree: spot_margin.aim_line_trees, spot_margin
--     line_margin, steering.shot_path_clear, goals.sea_shot_reaches
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_base_shot.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local bit = require('bitcompat')
local C   = require("constants")

-- Terrain: a table, grass by default.
local TMAP = {}
_G.TERRAIN_MASK = 0x0F
_G.TERRAIN_MINE_FLAG = 0x40
_G.get_terrain = function(mx, my) return TMAP[my * 256 + mx] or C.T_GRASS end

local U   = require("util")
local cpf = require("cpathfinder")

-- Straight-line shell: every tile the centre line crosses, in order.
local function line_tiles(ox, oy, tx, ty)
  local dx, dy = tx - ox, ty - oy
  local d = math.sqrt(dx * dx + dy * dy)
  local out, seen = {}, {}
  if d == 0 then return out end
  for st = 0, math.floor(d) + 16, 16 do
    local mx = math.floor((ox + dx / d * st) / 256)
    local my = math.floor((oy + dy / d * st) / 256)
    local k = my * 256 + mx
    if not seen[k] then seen[k] = true; out[#out + 1] = { mx = mx, my = my } end
  end
  return out
end
cpf.simulate_shot = function(ox, oy, tx, ty) return line_tiles(ox, oy, tx, ty) end
cpf.simulate_shot_with_tanks = function(ox, oy, tx, ty) return line_tiles(ox, oy, tx, ty) end

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

-- A world with one base at (bx, by).
local function world_with_base(bx, by, owner, health)
  local b = { mx = bx, my = by, owner = owner, health = health }
  local w = { pill_at = {}, base_at = {}, pills = {}, bases = { [1] = b } }
  w.base_at[by * 256 + bx] = { id = 1, base = b }
  return w, b
end

local function info_with(rules, inboat)
  return { rules = rules, inboat = inboat }
end

-- ── 1. The rule itself ───────────────────────────────────────────────────
print("\n-- 1. U.base_stops_tank_shell (classic rules: hit 4, capture 9)")
do
  local w, b = world_with_base(10, 10, "friendly", 18)
  U.note_shell_rules(w, info_with(nil, false))
  check("1a own/allied base (friendly), land: no block", U.base_stops_tank_shell(w, b, false) == false)
  b.owner = "allied"
  check("1b allied base, land: no block", U.base_stops_tank_shell(w, b, false) == false)
  b.owner = "neutral"; b.health = 0
  check("1c neutral base, land: no block", U.base_stops_tank_shell(w, b, false) == false)
  b.owner = "hostile"; b.health = 1
  check("1d hostile base (armour > capture > hit), land: blocks", U.base_stops_tank_shell(w, b, false) == true)
  b.health = 0
  check("1e hostile base health 0, classic (hit 4 < capture 9): armour unknown, blocks",
        U.base_stops_tank_shell(w, b, false) == true)
  b.owner = nil
  check("1f unknown owner: blocks (old answer)", U.base_stops_tank_shell(w, b, false) == true)
  for _, o in ipairs({ "friendly", "allied", "neutral", "hostile" }) do
    b.owner = o
    check("1g " .. o .. " base, BOAT shot: blocks", U.base_stops_tank_shell(w, b, true) == true)
  end
  check("1h no base: no block", U.base_stops_tank_shell(w, nil, true) == false)
  check("1i pill shell: never blocks", U.base_stops_pill_shell() == false)
end

print("\n-- 2. hostile base at/below base_hit_armour (live rules)")
do
  local w, b = world_with_base(10, 10, "hostile", 0)
  -- base_hit_armour == base_capture_armour: health 0 proves armour <= hit.
  U.note_shell_rules(w, info_with({ base_hit_armour = 9, base_capture_armour = 9 }, false))
  check("2a hostile, health 0, hit 9 >= capture 9: no block", U.base_stops_tank_shell(w, b, false) == false)
  b.health = 1
  check("2b hostile, health 1 (armour > capture >= hit): blocks", U.base_stops_tank_shell(w, b, false) == true)
  b.health = 0
  check("2c hostile at/below hit but on a boat: blocks", U.base_stops_tank_shell(w, b, true) == true)
  U.note_shell_rules(w, info_with({ base_hit_armour = 4, base_capture_armour = 9 }, false))
  check("2d hostile, health 0, hit 4 < capture 9: blocks", U.base_stops_tank_shell(w, b, false) == true)
  U.note_shell_rules(w, info_with({ base_hit_armour = 0, base_capture_armour = 9 }, false))
  check("2e base_hit_armour 0 is read as 0, not classic", w._shell_rules.hit == 0)
  U.note_shell_rules(w, info_with({}, false))
  check("2f rules without the fields: classic 4 / 9",
        w._shell_rules.hit == 4 and w._shell_rules.cap == 9)
end

print("\n-- 3. U.tank_shell_on_boat (origin tile)")
do
  local w = world_with_base(10, 10, "friendly", 18)
  TMAP[5 * 256 + 5] = C.T_RIVER
  TMAP[5 * 256 + 6] = C.T_DEEPSEA
  TMAP[5 * 256 + 7] = C.T_BOAT
  U.note_shell_rules(w, info_with(nil, false))
  check("3a grass: land", U.tank_shell_on_boat(w, 4, 5) == false)
  check("3b river, not in a boat: land", U.tank_shell_on_boat(w, 5, 5) == false)
  check("3c deep sea: boat", U.tank_shell_on_boat(w, 6, 5) == true)
  check("3d boat tile: boat", U.tank_shell_on_boat(w, 7, 5) == true)
  U.note_shell_rules(w, info_with(nil, true))
  check("3e river, in a boat: boat", U.tank_shell_on_boat(w, 5, 5) == true)
  check("3f grass while info.inboat: land (the boat stays on the shore)", U.tank_shell_on_boat(w, 4, 5) == false)
  TMAP[5 * 256 + 5] = nil; TMAP[5 * 256 + 6] = nil; TMAP[5 * 256 + 7] = nil
end

print("\n-- 4. U.mark_base_shell_pass (the flag gh_attack.c reads)")
do
  local w, b = world_with_base(10, 10, "friendly", 18)
  U.note_shell_rules(w, info_with(nil, true))   -- inboat does not matter: the flag is the land answer
  U.mark_base_shell_pass(w)
  check("4a own base: passes from land", b._shell_pass_land == true)
  b.owner = "hostile"; b.health = 1
  U.mark_base_shell_pass(w)
  check("4b hostile base: does not pass", b._shell_pass_land == false)
end

-- ── 5. Call sites: a shell line from (20,20) to a pill at (30,20), with the
--      base at (25,20) right on the line. ────────────────────────────────────
local SM    = require("spot_margin")
local steer = require("steering")
local G     = require("goals")

local OX, OY = bit.bor(bit.lshift(20, 8), 128), bit.bor(bit.lshift(20, 8), 128)
local PMX, PMY = 30, 20

local function site_results(owner, health, inboat, origin_tt)
  local w = world_with_base(25, 20, owner, health)
  TMAP[20 * 256 + 20] = origin_tt
  local info = { tankx = OX, tanky = OY, objects = {}, player_number = 0, inboat = inboat }
  U.note_shell_rules(w, info)
  local trees = SM.aim_line_trees(OX, OY, 20, 20, PMX, PMY, w, 1)
  local spm_ok = SM.line_margin(SM.new_ctx(w, PMX, PMY), 20.5, 20.5, 30.5, 20.5, 0.5)
  local spc = steer.shot_path_clear(info, w, U.m2w(PMX), U.m2w(PMY), PMX, PMY)
  local tank_reach = G.sea_shot_reaches(w, OX, OY, PMX, PMY, cpf.SHOT_TANK, {})
  local pill_reach = G.sea_shot_reaches(w, OX, OY, PMX, PMY, cpf.SHOT_PILL, {})
  TMAP[20 * 256 + 20] = nil
  return (trees ~= nil), spm_ok, spc, tank_reach, pill_reach
end

print("\n-- 5. call sites (aim_line_trees / line_margin / shot_path_clear / sea_shot_reaches)")
local CASES = {
  { "own base, land",           "friendly", 18, false, nil,         true  },
  { "allied base, land",        "allied",   18, false, nil,         true  },
  { "neutral base, land",       "neutral",   0, false, nil,         true  },
  { "hostile base, land",       "hostile",   1, false, nil,         false },
  { "own base, boat (river)",   "friendly", 18, true,  C.T_RIVER,   false },
  { "neutral base, deep sea",   "neutral",   0, true,  C.T_DEEPSEA, false },
}
for _, cs in ipairs(CASES) do
  local name, owner, health, inboat, ott, want_clear = cs[1], cs[2], cs[3], cs[4], cs[5], cs[6]
  local a, m, s, tr, pr = site_results(owner, health, inboat, ott)
  local tag = want_clear and "clear" or "blocked"
  check("5 " .. name .. ": aim_line_trees " .. tag, a == want_clear, a)
  check("5 " .. name .. ": line_margin " .. tag, m == want_clear, m)
  check("5 " .. name .. ": shot_path_clear " .. tag, s == want_clear, s)
  check("5 " .. name .. ": sea_shot_reaches(tank) " .. tag, tr == want_clear, tr)
  check("5 " .. name .. ": sea_shot_reaches(pill) clear (pill shells fly over bases)", pr == true, pr)
end

-- A hostile base at/below base_hit_armour under rules where the brain can
-- prove it (hit == capture): every site lets the shot through.
do
  local w = world_with_base(25, 20, "hostile", 0)
  local info = { tankx = OX, tanky = OY, objects = {}, player_number = 0, inboat = false,
                 rules = { base_hit_armour = 9, base_capture_armour = 9 } }
  U.note_shell_rules(w, info)
  check("5 hostile at/below hit: aim_line_trees clear", SM.aim_line_trees(OX, OY, 20, 20, PMX, PMY, w, 1) ~= nil)
  check("5 hostile at/below hit: shot_path_clear clear",
        steer.shot_path_clear(info, w, U.m2w(PMX), U.m2w(PMY), PMX, PMY) == true)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
