-- Scenario sidecar for tests/farm_sectors_A.map (auto-loaded as
-- <map>.scenario.lua). Companion to tests/farm_sectors_test.py arena A.
--
-- ARENA A: a tank with nothing to do, a forest behind a moat, and a clear
-- forest further away on the other side. The whole geometry is planted HERE,
-- relative to the tile the tank comes to rest on, because "the walled forest
-- is the nearest one" is a claim about the tank's position and baking it into
-- the map file would make it a claim about where the tank happened to stop.
--
-- FILLING THE SPAWN POND. A start square has to be DEEP SEA at map load
-- (starts.c startsIsValidSquare), and the LGM walk sim walks a STRAIGHT LINE
-- with a crude slide, so a hole anywhere on the line makes the target read
-- `unreachable`. The pond is filled back to grass once the tank is ashore and
-- OFF it -- filling it under the tank leaves the boat state stuck and the LGM
-- never becomes available at all.
--
-- PORTED (2026-09-15) from the old scenario host. Three things changed. The
-- trace file is gone -- there is no io in the sandbox -- so the TANK trail is
-- kept as the one number the driver built out of it, the window during which
-- the walled forest really was the nearer of the two. Every DURATION is
-- doubled, because this host's hook clock counts 100 a second where the old
-- one counted 50. And the arena fields its own tank, because the driver pinned
-- it with -bot-init and the runner's -bots N seats come in with no init.

-- farm_sectors_test.py TOKENS, verbatim. Discovery offers no farm row at or
-- above TREE_OPPORTUNISTIC_MAX, and a scripted map hands out 40 trees whatever
-- -gametype says; at 40 trees the urgency term is 0, so the row is worth a flat
-- VALUE_FARM (15) and can never clear MIN_SCORE. 99 is what a 5-tree woodpile
-- would produce.
local TOKENS = "cfg=TREE_OPPORTUNISTIC_MAX=41;cfg=BUILDER_POOL_VALUE_FARM=99"
local OUR_BRAIN = "../brains/GoalHunter_1.7/init.lua"

local SPAWN = { 126, 126 }
local FIELD = { 120, 132, 124, 128 }   -- x0, x1, y0, y1
local WALL_DX, BLOCKED_DX, OPEN_DX = -1, -2, 3
local FILL_TICK, PLANT_TICK = 120, 800   -- both doubled
local MOAT, FOREST, GRASS = 0xFF, 5, 7   -- 0xFF = DEEP SEA (scenario.c l_set_tile)
local LEASH = 8                          -- BUILDER_POOL_LEASH, the farm row's reach
local MIN_WINDOW = 120                   -- the driver's 60 brain ticks, doubled
local p0 = 0

local spawn_tried = false
local filled = false
local planted = false
local blocked, opened = nil, nil
-- The window the driver built out of the TANK trail: the first run of ticks
-- after the plant during which the WALLED forest was strictly the nearer of the
-- two AND both were inside the leash. Every claim either arena makes is made
-- inside it, because outside it the walled forest is not the nearest one and
-- there is nothing to say.
local win_first, win_last, win_ticks = nil, nil, 0
local win_open = false
local win_shut = false

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

function on_setup(g)
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
    local us, err = g.spawn_bot{ slot = p0, name = "Farmer",
                                 brain = OUR_BRAIN, team = 0, start = 1,
                                 init = g.init_tokens(TOKENS) }
    if us == nil then
      g.message("FARM_SECTORS_A spawn_bot failed: " .. tostring(err))
    end
  end

  local tk = g.tank(p0)
  if not tk then return end

  if not filled and tick >= FILL_TICK then
    if not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = true
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
      g.message(string.format(
        "FARM_SECTORS_A filled the spawn pond at (%d,%d) at t=%d",
        SPAWN[1], SPAWN[2], tick))
    end
  end

  -- Plant only once the tank has settled AND the geometry fits inside the
  -- field from where it is standing. If it never fits the verdict says so; it
  -- must never plant a half-arena and let the run look like a real result.
  if filled and not planted and tick >= PLANT_TICK and not tk.dead
     and not tk.boat then
    local mx, my = tk.mx, tk.my
    local ok = (mx + BLOCKED_DX >= FIELD[1]) and (mx + OPEN_DX <= FIELD[2])
    if ok then
      planted = true
      -- A MOAT OF DEEP SEA, not a wall of buildings, and it spans the WHOLE
      -- field height. Two measured failures are behind both halves of that:
      --   * a three-tile BUILDING wall beside the tank was walked around --
      --     the walk sim runs a straight line from wherever the tank IS, and a
      --     two-row drift clears the end of a short wall;
      --   * a FULL-HEIGHT building wall was SHOT DOWN. The bot prices walls
      --     into its own paths (wall_shoot_cost) and cleared the column, then
      --     drove through the gap.
      -- Deep sea is the one barrier the man can never cross
      -- (brain_pathfinder.c lgm_man_speed[10] = 0) and the tank can never
      -- remove.
      for wy = FIELD[3], FIELD[4] do
        g.set_tile(mx + WALL_DX, wy, MOAT)
      end
      blocked = { mx + BLOCKED_DX, my }
      opened  = { mx + OPEN_DX, my }
      g.set_tile(blocked[1], blocked[2], FOREST)
      g.set_tile(opened[1], opened[2], FOREST)
      g.message(string.format(
        "FARM_SECTORS_A planted: moat x=%d, walled forest (%d,%d),"
        .. " open forest (%d,%d), tank (%d,%d) at t=%d",
        mx + WALL_DX, blocked[1], blocked[2], opened[1], opened[2],
        mx, my, tick))
    end
  end

  if not planted or tk.dead then return end

  -- The window, walked forward one tick at a time rather than reconstructed
  -- from a trail: the FIRST contiguous stretch only, because once the tank has
  -- wandered out of position the arena is over and a later stretch is a
  -- different arena.
  if not win_shut then
    local db = math.abs(tk.mx - blocked[1]) + math.abs(tk.my - blocked[2])
    local do_ = math.abs(tk.mx - opened[1]) + math.abs(tk.my - opened[2])
    if db < do_ and db <= LEASH and do_ <= LEASH then
      win_open = true
      win_first = win_first or tick
      win_last = tick
      win_ticks = win_ticks + 1
    elseif win_open then
      win_open = false
      win_shut = true
    end
  end

  -- ONLY a walk that begins INSIDE the window counts, with no grace after it,
  -- and the margin is as thin as the driver's own was.  Measured on A2: the
  -- window ran t=800..960, the pool dispatched to the open forest at 964 and
  -- the man left the tank on that same tick -- four ticks out.  The driver
  -- bounded its dispatch the same way (brain 482 against a window ending at
  -- 480) and called the late one "the single-wedge rule working exactly as
  -- described", because by then the tank had wandered and the open forest was
  -- the nearest.  A grace period here would quietly turn that into a failure.
  watch_man(g, opened, blocked, tick, win_open)
end

-- ── the verdict ─────────────────────────────────────────────────────────
-- PORTED (2026-09-15) from tests/farm_sectors_test.py arena A, assertions 0,
-- 0b and 3 -- the ones the engine answers. 0 and 0b are the arena's own
-- preconditions: the geometry was planted, and the WALLED forest really was
-- the nearer of the two for long enough to be evidence. 3 is check_man_out,
-- which the driver read out of the per-tick jsonl and which game.builder
-- answers directly: the man left the tank inside that window and walked at the
-- OPEN forest rather than the walled one. That walk is the whole point of the
-- four wedges -- the near forest is unreachable, and before the change it was
-- the only row discovery would offer.
--
-- LEFT BEHIND, because it is print2 and nothing else: assertion 1, the first
-- farm BP_DISPATCH inside the window naming the open forest; 1b, the
-- BUILDER_POOL line for that tick showing 2+ candidates, which is what makes
-- "it picked the open one" mean anything at all; 1c to 1f, the dispatch line's
-- score and its out/build/back legs closing from their own chips and the row
-- carrying wedge{E}; and 2, the walled forest turning up on a BP_DENY. So a
-- PASS here says the man went east and says nothing about the pool having
-- scored both rows against each other when it decided.
--
-- GATE: ticks=2200 bots=0 ai=yesfull gametype=open

VERDICT_CHECK = function(g)
  if not planted then
    return false, "the arena was never planted -- the tank never settled"
  end
  if win_first == nil or (win_last - win_first) < MIN_WINDOW then
    return false, string.format("the walled forest was nearest for %s tick(s)"
                                .. " -- not evidence",
                                win_first and (win_last - win_first) or "0")
  end
  if ep_start == nil then
    return false, string.format("the man never left the tank (window %d..%d)",
                                win_first, win_last)
  end
  if ep_a == nil or (ep_b ~= nil and ep_b <= ep_a) then
    return false, string.format("the man left at t=%d and got %s from the open"
                                .. " forest, %s from the walled one", ep_start,
                                tostring(ep_a), tostring(ep_b))
  end
  return true, string.format("man out t=%d for %dt, open %d walled %d",
                             ep_start, ep_ticks, ep_a, ep_b)
end
