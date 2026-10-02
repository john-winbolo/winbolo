-- Scenario script for tests/farm_sectors_B.map (auto-loaded as
-- <map>.scenario.lua). Companion to tests/farm_sectors_test.py arena B.
--
-- ARENA B: a tank DRIVING east along a road to take a neutral base twenty
-- tiles away -- which is what gives it the committed route the return-leg
-- prediction walks. The moment it enters column PLANT_X the script plants two
-- forests, one PLANT_DX tiles behind and one PLANT_DX ahead, both PLANT_DY rows
-- south of the drive row. Equal outbound legs by construction; only the walk
-- HOME can tell them apart.
--
-- They are planted rather than baked in because "the same distance away" is
-- true for exactly one tick of a moving tank, and game.set_tile is the only
-- way to make that tick the arena's to choose.
--
-- PORTED (2026-09-15) from the old scenario host. Three things changed. The
-- trace file is gone -- there is no io in the sandbox -- so the plant and the
-- LGM's walk are kept in locals instead. Every DURATION is doubled, because
-- this host's hook clock counts 100 a second where the old one counted 50. And
-- the arena fields its own tank, because the driver pinned it with -bot-init
-- and the runner's -bots N seats come in with no init.

-- farm_sectors_test.py TOKENS, verbatim. Discovery offers no farm row at or
-- above TREE_OPPORTUNISTIC_MAX, and a scripted map hands out 40 trees whatever
-- -gametype says; at 40 trees the urgency term is 0, so the row is worth a flat
-- VALUE_FARM (15) and can never clear MIN_SCORE. 99 is what a 5-tree woodpile
-- would produce.
local TOKENS = "cfg=TREE_OPPORTUNISTIC_MAX=41;cfg=BUILDER_POOL_VALUE_FARM=99"
local OUR_BRAIN = "../brains/GoalHunter/init.lua"

local SPAWN = { 116, 126 }
local TARGET = { 139, 126 }
local PLANT_X, PLANT_DX, PLANT_DY = 126, 3, 1
local FILL_TICK = 80                     -- doubled
local FOREST, GRASS = 5, 7
local p0 = 0

local spawn_tried = false
local filled = false
local planted = false
local behind, ahead = nil, nil

-- ── WHERE THE MAN WENT ──────────────────────────────────────────────────
-- The driver read build/player0_<stamp>.jsonl and asked it one thing per
-- arena: after the FIRST farm BP_DISPATCH, did the LGM leave the tank and
-- close on the forest that dispatch named. game.builder answers the same
-- question from the engine, so the walk is watched here instead -- and only
-- the FIRST walk, because the driver only ever asserted on the first dispatch
-- and a bot left running will farm both forests in turn.
local ep_start = nil          -- tick the first walk began, or nil for none
local ep_ticks = 0
local ep_a, ep_b = nil, nil   -- closest Manhattan tiles that walk got, per forest
local man_out  = false
local ep_done  = false

-- `may_start` is the stretch during which a walk counts as the answer to the
-- dispatch this arena is about; a walk that begins outside it belongs to a
-- different arrangement of the tank and the two forests.
local function watch_man(g, ta, tb, tick, may_start)
  if ep_done then return end
  local b = g.builder(p0)
  local out = b ~= nil and b.state ~= "in_tank" and b.state ~= "dead"
  if out and not man_out then
    if not may_start then return end
    man_out = true
    ep_start = tick
  elseif man_out and not out then
    man_out = false
    ep_done = true
    return
  end
  if man_out and b then
    ep_ticks = ep_ticks + 1
    local da = math.abs(b.mx - ta[1]) + math.abs(b.my - ta[2])
    local db = math.abs(b.mx - tb[1]) + math.abs(b.my - tb[2])
    if ep_a == nil or da < ep_a then ep_a = da end
    if ep_b == nil or db < ep_b then ep_b = db end
  end
end

local function same(p, t) return p.x == t[1] and p.y == t[2] end

function on_setup(g)
  -- The drive target is a NEUTRAL base: capture_base is what makes the tank
  -- cross the map, and (unlike capture_pill, which sets builder mode
  -- "gather") it leaves the builder pool eligible the whole way.
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and same(b, TARGET) then g.set_base_owner(i, g.NEUTRAL) end
  end
  g.set_team(p0, 0)
end

-- The spawn names its start, so this is only the fallback a respawn takes.
function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_tick(g, tick)
  -- THE ARENA FIELDS ITS OWN TANK. The GATE line asks for bots=0.
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local us, err = g.spawn_bot{ slot = p0, name = "Driver",
                                 brain = OUR_BRAIN, team = 0, start = 1,
                                 init = g.init_tokens(TOKENS) }
    if us == nil then
      g.message("FARM_SECTORS_B spawn_bot failed: " .. tostring(err))
    end
  end

  local tk = g.tank(p0)
  if not tk then return end

  if not filled and tick >= FILL_TICK then
    if not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
    end
  end

  if filled and not planted and not tk.dead and not tk.boat
     and tk.mx >= PLANT_X then
    planted = true
    local my = tk.my + PLANT_DY
    behind = { PLANT_X - PLANT_DX, my }
    ahead  = { PLANT_X + PLANT_DX, my }
    g.set_tile(behind[1], behind[2], FOREST)
    g.set_tile(ahead[1], ahead[2], FOREST)
    g.message(string.format(
      "FARM_SECTORS_B planted: behind (%d,%d), ahead (%d,%d),"
      .. " tank (%d,%d) at t=%d", behind[1], behind[2], ahead[1], ahead[2],
      tk.mx, tk.my, tick))
  end

  if planted then watch_man(g, ahead, behind, tick, true) end
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/farm_sectors_test.py arena B, assertions 0, 2
-- and 5 -- the ones the engine answers. 0 is the arena's own precondition, the
-- two forests planted the same distance out. 2 and 5 are the outcome: the man
-- left the tank and walked at the AHEAD forest rather than the one BEHIND. The
-- driver read the walk out of the per-tick jsonl and game.builder answers it
-- directly. With equal outbound legs by construction, only the walk HOME can
-- separate the two rows, so which forest he was sent to IS the return-leg
-- prediction's answer.
--
-- No arrival is required, for the driver's own reason: the tank keeps driving
-- east and the man is chasing a moving home, so "he set off at it and closed"
-- is the honest claim.
--
-- LEFT BEHIND, because it is print2 and nothing else: assertion 2b, the row
-- carrying wedge{E}; 3, predsrc{route} rather than the `same` fallback, which
-- is what says the prediction walked the tank's OWN committed route; 4, back <
-- out; 5b to 5d, the predicted tile lying on the route BP_PRED printed, inside
-- the out + build horizon and at the last waypoint before it; and 1c to 1e,
-- the score and the legs closing from their own chips. So a PASS here says the
-- man went to the nearer-on-the-way-home forest and says nothing about how the
-- number that sent him there was arrived at.
--
-- GATE: ticks=3200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not planted then
    return false, string.format("the forests were never planted -- the tank "
                                .. "never reached column %d ashore", PLANT_X)
  end
  if ep_start == nil then
    return false, "the man never left the tank after the plant"
  end
  if ep_a == nil or (ep_b ~= nil and ep_b <= ep_a) then
    return false, string.format("first walk t=%d: ahead %s, behind %s -- he "
                                .. "was not sent ahead", ep_start,
                                tostring(ep_a), tostring(ep_b))
  end
  return true, string.format("first walk t=%d closed to %d of ahead (behind "
                             .. "%s)", ep_start, ep_a, tostring(ep_b))
end
