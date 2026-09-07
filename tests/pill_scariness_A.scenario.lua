-- Scenario sidecar for tests/pill_scariness_A.map (auto-loaded as
-- <map>.scenario.lua).  Variant A: NEUTRAL-pillbox strays on a healthy pill.
--
-- Three jobs.
--
-- 1. LOADOUT.  A map with a sidecar boots as gameScripted, and
--    gameTypeGetItems treats gameScripted exactly like OPEN (40 shells /
--    40 mines / 40 trees) -- neither -gametype nor scenario.game reaches a
--    -bots tank.  game.spawn_bot's mode argument is the one thing that does
--    (it arms sim->spawnLoadout for the slot BEFORE tankCreate runs), so this
--    variant runs with -bots 0 and spawns its own bot in "strict" mode: 0
--    shells, 0 mines, 0 trees, full armour.
--
--    That is not a detail, it is the whole arena.  With shells the bot would
--    simply shoot the neutral pillbox down and the strays would stop; with
--    trees its LGM would repair our pill back to full between hits.  An empty
--    tank can do neither, so the only question left is the one the test asks:
--    when our pill takes stray fire, does the bot park on watch or go do its
--    job?  It matches the incident too -- par2 bot3 had a live goal queue and
--    a pill it was not going to save by standing next to it.
--
-- 2. OWNERSHIP.  The map's live pill P and the dead pill belong to whatever
--    slot the bot lands in; the neutral pillbox N stays neutral (nil owner).
--    Our base is re-owned and then EMPTIED, so refuel_at_base has nothing to
--    offer and does not become the arena's main event.
--
-- 3. TRACE.  Every tick, write P's armour to pill_scariness_A_hp.log so the
--    test can say exactly when the pill was healthy (>= 2/3 of 15) without
--    trusting the brain's own log for it.

local OUR_PILL      = { 126, 122 }
local NEUTRAL_PILL  = { 126, 120 }
local OUR_BASE      = { 120, 132 }
local SPAWN_MODE    = "strict"
local TRACE         = "pill_scariness_A_hp.log"

local spawn_tried = false
local last_hp = nil

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p then
      if p.x == NEUTRAL_PILL[1] and p.y == NEUTRAL_PILL[2] then
        g.set_pill_owner(i, nil)          -- nil = neutral: shoots everyone
      else
        g.set_pill_owner(i, owner)
      end
    end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
      -- Emptied AFTER the owner change: set_base_owner drains stock on a
      -- non-neutral -> non-neutral change anyway, but say it outright so the
      -- intent survives an engine-rule change.
      g.set_base_stock(i, 0, 0, 0)
    end
  end
end

function on_setup(g)
  own_everything(g, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick hp\n") f:close() end
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    -- 2026-09-06: pinned to the KEEL defend evaluator (DEFEND_ALARM_MODE off):
    -- A2 asserts the arrived NO-BID / WATCH rungs that alarm mode deletes.
    local s, err = g.spawn_bot("StrayWatch", nil, 0, SPAWN_MODE,
                               "cfg=DEFEND_ALARM_MODE=false")
    if s == nil then
      g.message("PILL_SCARINESS spawn_bot failed: " .. tostring(err))
    else
      g.message("PILL_SCARINESS spawned slot=" .. tostring(s)
                .. " mode=" .. SPAWN_MODE)
      own_everything(g, s)
    end
  end
  -- Trace our pill's HP on every CHANGE (a per-tick line would be 6000 rows
  -- of the same number).
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        local f = io.open(TRACE, "a")
        if f then
          f:write(string.format("%d %d\n", tick, p.armour))
          f:close()
        end
        last_hp = p.armour
      end
      break
    end
  end
end
