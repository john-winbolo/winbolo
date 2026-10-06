-- =========================================================================
-- test_place_line_flip.lua — standalone unit tests (2026-09-25) for:
--   (a) pill placement vs blitz shot lines (knob C.PILL_PLACE_AVOID_BLITZ_LINE):
--       squad.blitz_shot_lines, squad.tile_on_blitz_line and the guard-spot
--       pick builder.guard_build_spot; plus the bot's OWN pending pill in
--       the later blitz spot pick (attack._blitz_pick_from_scan).
--   (b) path flip guard (knob C.PF_FLIP_FRESH_ASTAR): steering.flip_track /
--       flip_is_back / flip_holding and cpf_path_to with cpf.path_to stubbed:
--       A->B->A gives a fresh A* from B. Knob C.PF_NEXTSTEP_CHAIN_VEER is the
--       engine side (brain_pathfinder.c); here only its config pass-through.
-- No engine: terrain is all road and the shell simulator walks a straight
-- line through the tiles (both patched onto the real modules).
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_place_line_flip.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C   = require("constants")
local AS  = require("ally_state")
local U   = require("util")
local cpf = require("cpathfinder")
local PF  = require("pathfinder")
local SQ  = require("squad")
local A   = require("attack")
local B   = require("builder")
local S   = require("steering")

-- ── Engine stand-ins ─────────────────────────────────────────────────────
U.ttype      = function() return 4 end       -- road everywhere (placeable)
U.ttype_peek = function() return 4 end
cpf.SHOT_TANK = cpf.SHOT_TANK or 0
cpf.simulate_shot = function(ox, oy, awx, awy)
  local x0, y0, x1, y1 = ox / 256, oy / 256, awx / 256, awy / 256
  local n = math.ceil(math.max(math.abs(x1 - x0), math.abs(y1 - y0)) * 16) + 1
  local out, last = {}, nil
  for i = 0, n do
    local t = i / n
    local mx, my = math.floor(x0 + (x1 - x0) * t), math.floor(y0 + (y1 - y0) * t)
    local k = my * 256 + mx
    if k ~= last then out[#out + 1] = { mx = mx, my = my }; last = k end
  end
  return out
end
PF.wall_hp_between = function() return 0 end
_G.cpf_lgm_travel_ticks_map = function() return 10 end
_G.print2 = _G.print2 or function() end

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
local function xy(x, y) return string.format("(%s,%s)", tostring(x), tostring(y)) end

-- ── 0. Knobs ─────────────────────────────────────────────────────────────
print("knobs")
local keel = C.PRESETS and C.PRESETS.keel or {}
check("PILL_PLACE_AVOID_BLITZ_LINE default true", C.PILL_PLACE_AVOID_BLITZ_LINE == true)
check("PF_NEXTSTEP_CHAIN_VEER default true", C.PF_NEXTSTEP_CHAIN_VEER == true)
check("PF_FLIP_FRESH_ASTAR default true", C.PF_FLIP_FRESH_ASTAR == true)
check("PRESETS.keel PILL_PLACE_AVOID_BLITZ_LINE = false", keel.PILL_PLACE_AVOID_BLITZ_LINE == false)
check("PRESETS.keel PF_NEXTSTEP_CHAIN_VEER = false", keel.PF_NEXTSTEP_CHAIN_VEER == false)
check("PRESETS.keel PF_FLIP_FRESH_ASTAR = false", keel.PF_FLIP_FRESH_ASTAR == false)
check("PF_FLIP_FRESH_ASTAR_TICKS default 60", C.PF_FLIP_FRESH_ASTAR_TICKS == 60, C.PF_FLIP_FRESH_ASTAR_TICKS)
check("PRESETS.keel PF_FLIP_FRESH_ASTAR_TICKS = 60", keel.PF_FLIP_FRESH_ASTAR_TICKS == 60)
do
  local seen = {}
  local saved = {}
  for _, n in ipairs({ "cpf_set_config", "cpf_set_terrain_cost", "cpf_set_boat_cost", "cpf_set_terrain_speed" }) do
    saved[n] = _G[n]
  end
  _G.cpf_set_config = function(k, v) seen[k] = v end
  _G.cpf_set_terrain_cost = function() end
  _G.cpf_set_boat_cost = function() end
  _G.cpf_set_terrain_speed = function() end
  cpf.configure({})
  check("configure: nextstep_chain_veer = 1 (knob on)", seen.nextstep_chain_veer == 1, seen.nextstep_chain_veer)
  C.PF_NEXTSTEP_CHAIN_VEER = false
  cpf.configure({})
  check("configure: nextstep_chain_veer = 0 (knob off, KEEL)", seen.nextstep_chain_veer == 0, seen.nextstep_chain_veer)
  C.PF_NEXTSTEP_CHAIN_VEER = true
  for n, f in pairs(saved) do _G[n] = f end
end

-- ── 1. Blitz shot lines ──────────────────────────────────────────────────
-- Tank on (130,130), threat north. The first guard candidate is (131,129).
-- Blitz target pill 0 on (127,125); own spot (135.5,133.5): the line from the
-- pill centre to the spot runs straight through (131,129).
local PMX, PMY = 127, 125
local SPFX, SPFY = 135.5, 133.5
local function new_world()
  return { pill_at = {}, base_at = {}, pending_pill_at = nil,
           pills = { [0] = { id = 0, mx = PMX, my = PMY, health = 15, owner = "hostile" } } }
end
local function blitz_state(extra)
  local st = { tick = 500, blocked = {},
               goal = { kind = "attack_pill", target_id = 0, mx = PMX, my = PMY,
                        _blitz = true, substate = "approach",
                        standoff_mx = 135, standoff_my = 133,
                        standoff_fx = SPFX, standoff_fy = SPFY } }
  for k, v in pairs(extra or {}) do st[k] = v end
  return st
end
local ME = 1
print("squad.blitz_shot_lines")
AS.init()
do
  local w = new_world()
  local l = SQ.blitz_shot_lines(blitz_state(), w, 500, ME)
  check("commander: own standoff line", l and #l == 1 and l[1].who == "self"
        and l[1].fx == SPFX and l[1].pmx == PMX, l and #l)
  check("no attack goal -> nil", SQ.blitz_shot_lines({ tick = 500, goal = { kind = "none" } }, w, 500, ME) == nil)
  local solo = blitz_state(); solo.goal._blitz_solo = true
  check("solo fallback -> nil", SQ.blitz_shot_lines(solo, w, 500, ME) == nil)
  local plain = blitz_state(); plain.goal._blitz = nil
  check("plain attack_pill (not a blitz) -> nil", SQ.blitz_shot_lines(plain, w, 500, ME) == nil)
  local neg = blitz_state({ squad_negotiate_cmdr = 0 }); neg.goal._blitz = nil
  check("still negotiating -> lines", SQ.blitz_shot_lines(neg, w, 500, ME) ~= nil)
  -- Committed soldier whose goal is briefly something else.
  local sold = { tick = 500, goal = { kind = "place_pill_offensive" },
                 squad_blitz_accepted = 0, squad_blitz_target = 0, squad_cmdr = 0,
                 squad_blitz_engage_mx = 135, squad_blitz_engage_my = 133,
                 squad_blitz_engage_fx = SPFX, squad_blitz_engage_fy = SPFY }
  local ls = SQ.blitz_shot_lines(sold, w, 500, ME)
  check("committed soldier, other goal -> own engage line", ls and ls[1].fx == SPFX, ls and #ls)
  w.pills[0].health = 0
  check("target pill dead -> nil", SQ.blitz_shot_lines(blitz_state(), w, 500, ME) == nil)
  w.pills[0].health = 15
  C.PILL_PLACE_AVOID_BLITZ_LINE = false
  check("knob off -> nil", SQ.blitz_shot_lines(blitz_state(), w, 500, ME) == nil)
  C.PILL_PLACE_AVOID_BLITZ_LINE = true
  -- Allies of the same blitz: the commander (p0) and a soldier (p2, cmdr=0).
  -- p3 follows another commander and is not counted.
  AS.set_info(0, 499, { bes = "120.5000,118.5000" })
  AS.set_info(2, 499, { bes = "121.2500,130.7500", cmdr = "0" })
  AS.set_info(3, 499, { bes = "110.5000,110.5000", cmdr = "7" })
  local la = SQ.blitz_shot_lines(blitz_state({ squad_cmdr = 0 }), w, 500, ME)
  local who = {}
  for _, ln in ipairs(la or {}) do who[ln.who] = ln end
  check("ally lines: self + p0 (cmdr) + p2 (soldier)", la and #la == 3 and who.self and who.p0 and who.p2,
        la and #la)
  check("  p2 line from its bes", who.p2 and who.p2.fx == 121.25 and who.p2.fy == 130.75)
  check("  p3 (other commander) not counted", who.p3 == nil)
  local dead = blitz_state({ squad_cmdr = 0, tank_dead_at = { [2] = 1000 } })
  local ld = SQ.blitz_shot_lines(dead, w, 500, ME)
  check("dead ally's line dropped", ld and #ld == 2, ld and #ld)
  AS.init()
end

print("squad.tile_on_blitz_line")
do
  local w = new_world()
  local l = SQ.blitz_shot_lines(blitz_state(), w, 500, ME)
  check("tile (131,129) on the line -> hit", SQ.tile_on_blitz_line(w, l, 131, 129, 500) ~= nil)
  check("tile (129,131) off the line -> nil", SQ.tile_on_blitz_line(w, l, 129, 131, 500) == nil)
  check("far tile (100,100) -> nil", SQ.tile_on_blitz_line(w, l, 100, 100, 500) == nil)
  check("pill tile itself -> nil", SQ.tile_on_blitz_line(w, l, PMX, PMY, 500) == nil)
  check("probe leaves pending_pill_at as it was (nil)", w.pending_pill_at == nil)
  w.pending_pill_at = { [1] = "x" }
  SQ.tile_on_blitz_line(w, l, 131, 129, 500)
  check("probe restores an existing pending table", w.pending_pill_at[1] == "x"
        and w.pending_pill_at[129 * 256 + 131] == nil)
  -- A spot already blocked (by another pending pill) is not blamed on the tile.
  w.pending_pill_at = { [127 * 256 + 129] = "ally" }
  check("already-blocked spot -> nil", SQ.tile_on_blitz_line(w, l, 131, 129, 500) == nil)
end

-- ── 2. guard_build_spot ──────────────────────────────────────────────────
print("builder.guard_build_spot")
do
  local info = { player_number = ME, tankx = 130 * 256 + 128, tanky = 130 * 256 + 128 }
  local function pick(state)
    local w = new_world()
    local mx, my, tier, cands = B.guard_build_spot(w, info, 130, 130, 130, 120, state)
    return mx, my, cands, w
  end
  local mx0, my0 = pick({ tick = 500, blocked = {}, goal = { kind = "attack_tank" } })
  check("no blitz -> first candidate (131,129)", mx0 == 131 and my0 == 129, xy(mx0, my0))
  local mx1, my1, cands, w1 = pick(blitz_state())
  local rej
  for _, c in ipairs(cands) do if c.mx == 131 and c.my == 129 then rej = c.rej; break end end
  check("blitz: (131,129) rejected as blitz_line", rej == "blitz_line", rej)
  check("blitz: another tile chosen", mx1 ~= nil and not (mx1 == 131 and my1 == 129), xy(mx1, my1))
  local l = SQ.blitz_shot_lines(blitz_state(), w1, 500, ME)
  check("  chosen tile is off every line", mx1 and SQ.tile_on_blitz_line(w1, l, mx1, my1, 500) == nil, xy(mx1, my1))
  C.PILL_PLACE_AVOID_BLITZ_LINE = false
  local mx2, my2 = pick(blitz_state())
  C.PILL_PLACE_AVOID_BLITZ_LINE = true
  check("knob off -> old choice (131,129)", mx2 == 131 and my2 == 129, xy(mx2, my2))
  -- Ally line only (we have no spot yet): p0's bes line through (131,129).
  AS.init()
  AS.set_info(0, 499, { bes = string.format("%.4f,%.4f", SPFX, SPFY) })
  local st = blitz_state({ squad_negotiate_cmdr = 0 })
  st.goal._blitz = nil; st.goal.standoff_mx = nil; st.goal.standoff_fx = nil; st.goal.standoff_fy = nil
  local mx3, my3 = pick(st)
  check("ally's line only -> another tile chosen", mx3 and not (mx3 == 131 and my3 == 129), xy(mx3, my3))
  AS.init()
end

-- ── 3. Own pending pill in the later blitz spot pick ────────────────────
-- Recorded geometry (20260925_105315): target pill (138,128); spot A
-- (143.7326,123.2674). Our OWN man is out building a pill on (142,123):
-- update_pending_pills marks it, and the pick takes the other spot.
print("own pending pill -> spot pick")
do
  local TPX, TPY = 138, 128
  local SAX, SAY = 143.7326, 123.2674
  local function spots()
    return {
      { mx = 143, my = 123, cx = SAX, cy = SAY, has_los = true, aim_idx = 1, aim_trees = 0,
        aim_wx = TPX * 256 + 128, aim_wy = TPY * 256 + 128, total_score = 10, deg = 45 },
      { mx = 144, my = 131, cx = 144.5, cy = 131.5, has_los = true, aim_idx = 1, aim_trees = 0,
        aim_wx = TPX * 256 + 128, aim_wy = TPY * 256 + 128, total_score = 30, deg = 120 },
    }
  end
  local w = { pill_at = {}, base_at = {}, pills = {} }
  local st = { _lgm_dispatch = { x = 142, y = 123, pbox = true, tick = 10 } }
  local info = { player_number = ME, man_status = 1, carried_pills = 0 }
  AS.init()
  SQ.update_pending_pills(st, info, 20, w)
  check("own man out with a pill -> (142,123) pending",
        w.pending_pill_at and w.pending_pill_at[123 * 256 + 142] == "own_lgm")
  local mx, my = A._blitz_pick_from_scan(spots(), { tick = 3022 }, 147, 123, nil, TPX, TPY, w)
  check("pick takes the other spot (144,131)", mx == 144 and my == 131, xy(mx, my))
  info.man_status = C.LGM_INTANK
  SQ.update_pending_pills(st, info, 21, w)
  local mx2, my2 = A._blitz_pick_from_scan(spots(), { tick = 3022 }, 147, 123, nil, TPX, TPY, w)
  check("man home, no pending -> the first spot (143,123)", mx2 == 143 and my2 == 123, xy(mx2, my2))
end

-- ── 4. Path flip guard ───────────────────────────────────────────────────
print("steering flip helpers")
do
  local st = {}
  S.flip_track(st, 147, 123)
  check("first tile: nothing left yet", st._pf_left_mx == nil)
  S.flip_track(st, 147, 122)
  check("move: left = (147,123)", st._pf_left_mx == 147 and st._pf_left_my == 123)
  check("next = tile just left -> back", S.flip_is_back(st, 148, 128, 147, 123) == true)
  check("next = other tile -> not back", S.flip_is_back(st, 148, 128, 148, 123) == false)
  check("next = tile just left but it is the dest -> not back", S.flip_is_back(st, 147, 123, 147, 123) == false)
  check("next = -1 -> not back", S.flip_is_back(st, 148, 128, -1, -1) == false)
  st._pf_flip_hold = { mx = 147, my = 122, dmx = 148, dmy = 128 }
  check("holding on this tile + dest", S.flip_holding(st, 147, 122, 148, 128) == true)
  check("not holding for another dest", S.flip_holding(st, 147, 122, 150, 128) == false)
  S.flip_track(st, 147, 122)
  check("same tile: hold kept", st._pf_flip_hold ~= nil)
  S.flip_track(st, 148, 123)
  check("tile change: hold cleared", st._pf_flip_hold == nil)
end

print("steering cpf_path_to A->B->A")
do
  local calls, script = {}, {}
  local real_path_to = cpf.path_to
  cpf.path_to = function(tmx, tmy, dmx, dmy, ib, sh, tr, mi, ar, budget, skip)
    calls[#calls + 1] = { tmx = tmx, tmy = tmy, skip = skip and true or false }
    local r = table.remove(script, 1) or { 1, -1, -1 }
    return r[1], r[2], r[3]
  end
  cpf.trace_path = function() return {} end
  cpf.dijkstra_trace_path = function() return {} end
  cpf.set_overlay = function() end
  local DX, DY = 148, 128
  local function info_at(mx, my) return { tankx = mx * 256 + 128, tanky = my * 256 + 128 } end
  local function new_state() return { tick = 2931, pf = { next_mx = -1, next_my = -1 } } end
  local function step(st, mx, my, results)
    calls = {}; script = results
    local nx, ny = S._cpf_path_to(st, info_at(mx, my), DX, DY)
    return nx, ny
  end
  -- Recorded case: on (147,123) the veer sent the tank to (147,122); there
  -- the drift step sent it back to (147,123).
  local st = new_state()
  local nx, ny = step(st, 147, 123, { { 1, 147, 122 } })
  check("on A: Dijkstra step (147,122), one call", nx == 147 and ny == 122 and #calls == 1, xy(nx, ny))
  nx, ny = step(st, 147, 122, { { 1, 147, 123 }, { 1, 148, 123 } })
  check("on B: Dijkstra says back to A -> second call is a fresh A*",
        #calls == 2 and calls[1].skip == false and calls[2].skip == true, #calls)
  check("  A* step taken (148,123)", nx == 148 and ny == 123, xy(nx, ny))
  check("  hold set on B", st._pf_flip_hold and st._pf_flip_hold.mx == 147 and st._pf_flip_hold.my == 122)
  nx, ny = step(st, 147, 122, { { 1, 148, 123 } })
  check("still on B: A* only (no Dijkstra call)", #calls == 1 and calls[1].skip == true, #calls)
  check("  A* step (148,123)", nx == 148 and ny == 123, xy(nx, ny))
  nx, ny = step(st, 148, 123, { { 1, 148, 124 } })
  check("off B: hold cleared, Dijkstra again", #calls == 1 and calls[1].skip == false
        and st._pf_flip_hold == nil and nx == 148 and ny == 124, xy(nx, ny))
  -- A* with no step: the Dijkstra step as before (no freeze).
  st = new_state()
  step(st, 147, 123, { { 1, 147, 122 } })
  nx, ny = step(st, 147, 122, { { 1, 147, 123 }, { -1, -1, -1 } })
  check("A* failed -> Dijkstra step kept (147,123)", nx == 147 and ny == 123, xy(nx, ny))
  nx, ny = step(st, 147, 122, { { 0, -1, -1 }, { 1, 147, 123 } })
  check("holding, A* running with no step -> Dijkstra called, its step used",
        #calls == 2 and calls[2].skip == false and nx == 147 and ny == 123, xy(nx, ny))
  -- Destination is the tile just left: a normal step, no A*.
  st = new_state()
  step(st, 147, 123, { { 1, 147, 122 } })
  calls = {}; script = { { 1, 147, 123 } }
  nx, ny = S._cpf_path_to(st, info_at(147, 122), 147, 123)
  check("next = dest -> no A*", #calls == 1 and nx == 147 and ny == 123, #calls)
  -- Sea no-go set live: guard off.
  st = new_state(); st._sea_nogo = { [1] = true }
  step(st, 147, 123, { { 1, 147, 122 } })
  nx, ny = step(st, 147, 122, { { 1, 147, 123 } })
  check("sea no-go live -> no A*", #calls == 1 and nx == 147 and ny == 123, #calls)
  -- Knob off (KEEL): the old flip.
  C.PF_FLIP_FRESH_ASTAR = false
  st = new_state()
  step(st, 147, 123, { { 1, 147, 122 } })
  nx, ny = step(st, 147, 122, { { 1, 147, 123 } })
  check("knob off -> one Dijkstra call, back to A (147,123)",
        #calls == 1 and calls[1].skip == false and nx == 147 and ny == 123, xy(nx, ny))
  check("  no flip state written", st._pf_cur_mx == nil and st._pf_flip_hold == nil)
  C.PF_FLIP_FRESH_ASTAR = true

  -- 2026-09-26: the fresh A* takes the ally avoid tiles (on the overlay for
  -- that one call, then put back); the tracer call gets them as before.
  local ov = {}
  local real_get, real_set = cpf.get_overlay, cpf.set_overlay
  cpf.get_overlay = function(x, y) return ov[y * 256 + x] or 0 end
  cpf.set_overlay = function(x, y, v) ov[y * 256 + x] = v end
  local seen_ov = nil
  cpf.path_to = function(tmx, tmy, dmx, dmy, ib, sh, tr, mi, ar, budget, skip, obst, pen)
    calls[#calls + 1] = { tmx = tmx, tmy = tmy, skip = skip and true or false, obst = obst, pen = pen }
    if skip then
      seen_ov = { a = ov[123 * 256 + 148] or 0, self = ov[122 * 256 + 147] or 0,
                  dest = ov[DY * 256 + DX] or 0 }
    end
    local r = table.remove(script, 1) or { 1, -1, -1 }
    return r[1], r[2], r[3]
  end
  st = new_state()
  ov[123 * 256 + 148] = 800                     -- an existing ally stamp
  st._nav_avoid_tiles = { 123 * 256 + 148, 122 * 256 + 147, DY * 256 + DX }
  step(st, 147, 123, { { 1, 147, 122 } })
  nx, ny = step(st, 147, 122, { { 1, 147, 123 }, { 1, 147, 121 } })
  check("flip A*: Dijkstra call got the avoid list and penalty",
        calls[1].obst == st._nav_avoid_tiles and calls[1].pen == C.NAV_AVOID_PENALTY)
  check("flip A*: avoid tile on the overlay during the A* (capped 32767)",
        seen_ov and seen_ov.a == 32767, seen_ov and seen_ov.a)
  check("  own tile and dest left alone", seen_ov and seen_ov.self == 0 and seen_ov.dest == 0)
  check("  overlay put back after the call", ov[123 * 256 + 148] == 800, ov[123 * 256 + 148])
  check("  A* step used (147,121)", nx == 147 and ny == 121, xy(nx, ny))
  st._nav_avoid_tiles = nil

  -- The hold is capped at PF_FLIP_FRESH_ASTAR_TICKS path calls.
  local cap = C.PF_FLIP_FRESH_ASTAR_TICKS
  C.PF_FLIP_FRESH_ASTAR_TICKS = 3
  st = new_state()
  step(st, 147, 123, { { 1, 147, 122 } })
  local astars = 0
  step(st, 147, 122, { { 1, 147, 123 }, { 1, 148, 123 } })   -- hold set: call 1
  if calls[#calls].skip then astars = astars + 1 end
  for _ = 1, 2 do                                             -- calls 2 and 3
    step(st, 147, 122, { { 1, 148, 123 } })
    if #calls == 1 and calls[1].skip then astars = astars + 1 end
  end
  check("cap 3: three path calls on the tile run the fresh A*", astars == 3, astars)
  nx, ny = step(st, 147, 122, { { 1, 147, 123 } })
  check("  4th call: hold spent -> Dijkstra only, its step",
        #calls == 1 and calls[1].skip == false and nx == 147 and ny == 123, xy(nx, ny))
  check("  hold marked spent", st._pf_flip_hold and st._pf_flip_hold.spent == true)
  nx, ny = step(st, 147, 122, { { 1, 147, 123 } })
  check("  5th call: still Dijkstra only, no new hold", #calls == 1 and calls[1].skip == false
        and st._pf_flip_hold.spent == true, #calls)
  step(st, 147, 123, { { 1, 147, 122 } })
  check("  tank moved: the spent hold on B is gone",
        st._pf_flip_hold == nil or (not st._pf_flip_hold.spent and st._pf_flip_hold.my == 123))
  nx, ny = step(st, 147, 122, { { 1, 147, 123 }, { 1, 148, 123 } })
  check("  back on B: a new hold runs the A* again", #calls == 2 and calls[2].skip == true, #calls)
  C.PF_FLIP_FRESH_ASTAR_TICKS = cap
  cpf.get_overlay, cpf.set_overlay = real_get, real_set
  cpf.path_to = real_path_to
end

print(string.format("\n%d passed, %d failed", pass, fail))
os.exit(fail == 0 and 0 or 1)
