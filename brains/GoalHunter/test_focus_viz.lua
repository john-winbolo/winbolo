-- =========================================================================
-- test_focus_viz.lua -- standalone unit tests (2026-11-01) for the
-- "Focused spike" overlays (focus_viz.lua): the four layers draw what the
-- bot's focus_vote state holds, and nothing draws with BRAIN_DEBUG_MODE off.
-- No engine: the overlay_* functions are stubs that record each call.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_focus_viz.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s (got %s)", name, tostring(got)))
  end
end

-- Overlay stubs: record each call.
local calls = {}
local function rec(kind) return function(...) calls[#calls + 1] = { kind = kind, ... } end end
overlay_text, overlay_rect, overlay_line = rec("text"), rec("rect"), rec("line")
overlay_circle, overlay_hud_text = rec("circle"), rec("hud_text")
_G._BT_VIZ_COLLECT = "all"
BRAIN_DEBUG_MODE = true

local C  = require("constants")
local FV = require("focus_vote")
local FZ = require("focus_viz")

-- opt/ (lua_strip removes every viz.* call): the module still loads and
-- draws nothing, so only check that.
do
  local fh = io.open("focus_viz.lua", "r")
  local src = fh and fh:read("*a") or ""
  if fh then fh:close() end
  if not src:find("viz.text(", 1, true) then
    print("stripped build: the draws are removed")
    local F0 = { cool_until = 0, clusters = {}, plan = { rows = {}, tiles = {} } }
    FZ.note(F0, "open", { id = 1, from = 2, me = 2, t = 0, need = 1, tiles = {}, bots = { 2 } })
    FZ.draw({ _fs = F0 }, { bases = {}, pills = {} }, { player_number = 2 }, 10)
    check("no overlay calls in the stripped build", #calls == 0, #calls)
    print(string.format("\n%d passed, %d failed", pass, fail))
    if fail > 0 then os.exit(1) end
    return
  end
end

local function texts(kind)
  local out = {}
  for _, c in ipairs(calls) do
    if c.kind == kind then out[#out + 1] = c[3] end
  end
  return out
end
local function has(list, pat)
  for _, s in ipairs(list) do if type(s) == "string" and s:find(pat, 1, true) then return true end end
  return false
end
local function count(kind)
  local n = 0
  for _, c in ipairs(calls) do if c.kind == kind then n = n + 1 end end
  return n
end

-- Bases 0 (target) and 1 (ours) 5 apart: one cluster at gap 9. Base 2 alone.
local world = {
  bases = {
    [0] = { mx = 10, my = 10, owner = "hostile" },
    [1] = { mx = 15, my = 10, owner = "friendly" },
    [2] = { mx = 40, my = 40, owner = "neutral" },
  },
  pills = {
    [0] = { mx = 12, my = 12, owner = "friendly", health = 15 },
    [1] = { mx = 38, my = 40, owner = "neutral", health = 10 },
  },
}
local list = FV.bases(world)
local F = { cool_until = 0 }
F.clusters = FV.clusters(list, C.FOCUS_SPIKE_CLUSTER_GAP)
check("two clusters at gap 9", #F.clusters == 2, #F.clusters)

print("covered marks")
local old_n = C.FOCUS_SPIKE_NEUTRAL_PILL_COVER
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = false
FZ.snapshot(F, world, FV.pills_of(world, false))
check("base 0 covered by our pill", F.viz_cov[0] == "ours", F.viz_cov[0])
check("base 1 covered by our pill", F.viz_cov[1] == "ours", F.viz_cov[1])
check("base 2 not covered with the knob off", F.viz_cov[2] == nil, F.viz_cov[2])
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = true
FZ.snapshot(F, world, FV.pills_of(world, false))
check("base 2 covered by a neutral pill with the knob on", F.viz_cov[2] == "neutral", F.viz_cov[2])
C.FOCUS_SPIKE_NEUTRAL_PILL_COVER = old_n

print("clusters and best plan")
F.plan = {
  t = 100,
  rows = { { key = "0", nb = 1, need = 1, rank = 0.5, taken = true,
             def = { total = 1, spot = 0, route = 2, detour = 0, tanks = 0, credit = 1 } } },
  tiles = { { mx = 11, my = 9 } },
  risk = { stands = { { mx = 11, my = 14, risk = 0.25 } } },
}
local state = { _fs = F }
FZ.draw(state, world, { player_number = 2 }, 100)
local tx = texts("text")
check("cluster label: bases ours and target", has(tx, "cluster 0: 2 bases (ours 1, target 1)"))
check("cluster label: every risk part",
  has(tx, "need 1  risk 1.00 = spot 0.00 + route 2.00 + detour 0.00 + tanks 0.00 - credit 1.00"))
check("cluster label: rank formula", has(tx, "rank 0.500 = 1 / (1 + 1.0 x 1.00)  BEST"))
check("cluster with no target base", has(tx, "no target base (all ours)"))
check("base marks", has(tx, "b0 target") and has(tx, "b1 ours"))
check("spike spot label", has(tx, "spike spot 1 (11,9)"))
check("stand place label", has(tx, "stand (11,14) r=0.25"))
local lines = 0
for _, c in ipairs(calls) do if c.kind == "line" then lines = lines + 1 end end
check("one cluster link line + one stand line", lines == 2, lines)
local hud = texts("hud_text")
check("no vote yet", has(hud, "FOCUSED SPIKE VOTE: none"))

print("votes")
calls = {}
FZ.note(F, "open", { id = 7, from = 2, me = 2, t = 1000, need = 1, tiles = { { mx = 11, my = 9 } },
                     cluster = "0", bases = { 0 }, bots = { 2, 3, 4 } })
FZ.note(F, "ballot", { id = 7, pn = 2, yes = true, pledge = 1, armour = 40 })
FZ.note(F, "ballot", { id = 7, pn = 3, yes = false, reason = "home_attacked" })
FZ.note(F, "ballot", { id = 99, pn = 4, yes = false, reason = "better" })   -- other id: ignored
FZ.draw(state, world, { player_number = 2 }, 1002)
hud = texts("hud_text")
check("vote header", has(hud, "FOCUSED SPIKE VOTE id=7 by player 2 at 0:20  cluster 0  bases 0"))
check("spike spots and need", has(hud, "spike spots (11,9)  need 1"))
check("yes ballot with pledge", has(hud, "player 2: YES pledge 1"))
check("no ballot in game terms", has(hud, "player 3: NO - a home base is under attack"))
check("silent bot = yes", has(hud, "player 4: silent (counts as yes, pledge 0)"))
check("still voting", has(hud, "voting..."))
FZ.note(F, "result", { id = 7, pass = true, why = "ok", t = 1004, yes = 2, pledged = 1, nbots = 3, reasons = {} })
FZ.note(F, "go", { id = 7, tiles = { { mx = 11, my = 9, pn = 2 } }, t = 1004 })
calls = {}
FZ.draw(state, world, { player_number = 2 }, 1005)
hud = texts("hud_text")
check("result line", has(hud, "RESULT PASSED (ok): yes 2 of 3 bots (need more than 1), pledges 1 of need 1"))
check("go list", has(hud, "GO at 0:20: (11,9)->player 2"))
FZ.note(F, "end", { id = 7, why = "abort_home_attacked", t = 2000 })
F.cool_until = 2000 + 1500
calls = {}
FZ.draw(state, world, { player_number = 2 }, 2010)
hud = texts("hud_text")
check("abort with reason", has(hud, "ABORT at 0:40: home_attacked"))
check("cooldown left", has(hud, "cooldown 30s left"))
calls = {}
FZ.draw(state, world, { player_number = 2 }, 2000 + FZ.HOLD_TICKS + 1)
hud = texts("hud_text")
check("vote leaves the panel after the hold", has(hud, "FOCUSED SPIKE VOTE: none"))
check("cooldown on the empty panel", has(hud, "cooldown"))

print("pledge armour floor, voter view")
FZ.note(F, "open", { id = 8, from = 3, me = 2, t = 3000, need = 1, tiles = {}, cluster = "0",
                     bases = { 0 }, bots = { 2, 3 } })
FZ.note(F, "ballot", { id = 8, pn = 3, yes = true, proposer = true })
FZ.note(F, "ballot", { id = 8, pn = 2, yes = true, pledge = 0, armour = 10 })
FZ.note(F, "result", { id = 8, pass = false, why = "pledges", t = 3004 })
calls = {}
FZ.draw(state, world, { player_number = 2 }, 3005)
hud = texts("hud_text")
check("armour under the floor: no pledge",
  has(hud, string.format("player 2: YES pledge 0 (armour 10 < %d: no pledge)", C.FOCUS_SPIKE_PLEDGE_MIN_ARMOR)))
check("proposer pledge not in the messages", has(hud, "player 3: YES (proposer; its pledge is not in the messages)"))
check("failed result", has(hud, "RESULT FAILED (pledges)"))
check("voter-view note", has(hud, "counts from the ballots this bot heard"))

print("active plan")
F.active = { id = 9, bases = { 0 }, tiles = {
  { mx = 11, my = 9, pn = 2 }, { mx = 20, my = 9, pn = 3, placed = true },
  { mx = 30, my = 9, pn = 4, dropped = true } } }
calls = {}
FZ.draw(state, world, { player_number = 2 }, 3100)
tx = texts("text")
check("going spot", has(tx, "player 2: going"))
check("placed spot", has(tx, "player 3: placed"))
check("dropped spot", has(tx, "player 4: dropped"))

print("adaptive spot")
local cs = { mx = 11, my = 9, total = 0.4, stand = { mx = 11, my = 12 },
             parts = { payoff = 1, stand = 0, route = 0, travel = 0.5, run = 0.1 } }
F.active = { id = 10, adaptive = true, bases = { 0 }, claims = { [0] = 2 }, rel = {},
  tiles = {}, mine = { mx = 11, my = 9, pn = 2, stand = { mx = 11, my = 12, risk = 0, val = 0.3 } },
  switches = { { t = 3000, why = "spot_blocked", old = { mx = 10, my = 8 }, new = { mx = 11, my = 9 },
                 new_total = 0.4 } },
  score = { t = 3050, top = { cs }, chosen = cs, mine = { 0 } } }
F.active.tiles[1] = F.active.mine
calls = {}
FZ.draw(state, world, { player_number = 2 }, 3100)
hud, tx = texts("hud_text"), texts("text")
check("adaptive header", has(hud, "ADAPTIVE SPIKE SPOT id=10 at 1:01  my claimed bases left: 0"))
check("chosen spot and stand", has(hud, "chosen (11,9) stand (11,12) stand risk 0.00 + run 0.30"))
check("top spot with every part",
  has(hud, "1. (11,9) total 0.40 = payoff 1.00 - stand 0.00 - route 0.00 - travel 0.50 - run 0.10  CHOSEN"))
check("claim label per player N", has(tx, "b0 claim player 2"))
check("spot change mark with its reason", has(tx, "1:00 spot_blocked 0.40") and has(tx, "1:00 left: spot_blocked"))

print("debug mode off draws nothing")
BRAIN_DEBUG_MODE = false
calls = {}
FZ.draw(state, world, { player_number = 2 }, 3100)
check("no overlay calls", #calls == 0, #calls)
BRAIN_DEBUG_MODE = true

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
