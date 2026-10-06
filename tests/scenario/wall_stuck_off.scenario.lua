-- wall_stuck_off -- (knob off) does GoalHunter keep its shells when it is stuck at a
-- wall that will not fall?
--
-- Joust's walls take 255 hits (rule building_life = 255). A bot that shoots
-- one to get through, or shoots it because it is stuck against it, throws
-- its shells away. C.WALL_SHOOT_LIFE_MAX (constants.lua; Joust's preset sets
-- 8) says the bot never shoots a wall that takes more hits than that, and the
-- stuck fallback in steering.lua backs off and turns instead.
--
-- The arena: the acquire field (tests/generate_lead_aim_maps.py), grass in
-- deep sea, with a neutral base near its north edge. The script puts a ring
-- of walls round the base, two squares out, so the base -- the shooter's
-- only errand -- cannot be reached without breaking the ring, and sets
-- building_life to 255. Every ROUND_FRAMES the shooter is put on the grass
-- just south of the ring, facing north into it, with full shells. There is
-- nothing else on the map to shoot.
--
-- Measure: shells fired over all rounds (the script tops the shells up, and
-- counts each drop). Pass: at most MAX_FIRED in total, and the shooter was
-- at the ring (within 1 square of a wall square) for at least one frame in
-- every round, so the arena did put it against the wall.
--
-- This is wall_stuck with the knob at its default (0): the bot shoots the
-- ring, so this arena is expected to fail. It shows the fault the knob fixes.
--
-- GATE: ticks=9000 bots=1 gametype=open ai=yesfull expect=fail GoalHunter with WALL_SHOOT_LIFE_MAX=0 (the default) shoots a 255-hit wall it is stuck at

-- KNOB VARIANT. SHOOTER_CFG goes to the shooter through game.bot_init at
-- frame 3, before the first round.
local SHOOTER_CFG = nil
local cfg_sent = false

local SHOOTER      = 0
local LIFE         = 255         -- building_life for the arena
local RING         = 2           -- ring of walls this many squares round the base
local ROUNDS       = 6
local ROUND_FRAMES = 600
local FIRST_ROUND  = 20          -- frame of the first placement
local MAX_FIRED    = 4

local frame = 0
local A = nil
local R = {}
local round = 0
local round_start = nil
local fired = {}                 -- round -> shells fired
local near = {}                  -- round -> frames at the ring
local prev_shells = nil
local done = false

local function on_ring(x, y)
  return math.max(math.abs(x - A.bx), math.abs(y - A.by)) == RING
end

local function place_shooter()
  game.teleport(SHOOTER, A.bx, A.by + RING + 1, 0)
  game.set_boat(SHOOTER, false)
  game.set_stocks(SHOOTER, { shells = R.full_shells, armour = R.full_armour,
                             trees = 0, mines = 0 })
  prev_shells = nil
end

local function finish()
  done = true
  local total, parts, ok, why = 0, {}, true, nil
  for i = 1, ROUNDS do
    local f, n = fired[i] or 0, near[i] or 0
    total = total + f
    parts[#parts + 1] = string.format("r%d:%d/%d", i, f, n)
    if n == 0 and ok then
      ok, why = false, string.format("round %d: the shooter never reached the ring", i)
    end
  end
  game.log(string.format("WALL fired=%d (max %d) per round fired/frames-at-ring %s",
                         total, MAX_FIRED, table.concat(parts, " ")))
  if ok and total > MAX_FIRED then
    ok, why = false, string.format("fired %d shells at a %d-hit wall (max %d)", total, LIFE, MAX_FIRED)
  end
  verdict(ok, (ok and "" or (why .. " ")) .. string.format("fired %d; %s", total, table.concat(parts, " ")))
end

function on_tick(g, tick)
  if done then return end
  frame = frame + 1
  if A == nil then
    local b = nil
    for n = 0, 3 do
      b = g.base(n)
      if b then break end
    end
    if not b then return end
    A = { bx = b.x, by = b.y }
    R.full_shells = g.rule("tank_full_shells")
    R.full_armour = g.rule("tank_full_armour")
    g.set_rule("building_life", LIFE)
    for dy = -RING, RING do
      for dx = -RING, RING do
        if math.max(math.abs(dx), math.abs(dy)) == RING then
          g.set_tile(A.bx + dx, A.by + dy, g.TERRAIN.building)
        end
      end
    end
    g.log(string.format("WALL setup base=(%d,%d) ring=%d building_life=%d",
                        A.bx, A.by, RING, g.rule("building_life")))
  end
  if SHOOTER_CFG and not cfg_sent and frame >= 3 then
    cfg_sent = true
    local ok, code = g.bot_init(SHOOTER, { cfg = SHOOTER_CFG })
    g.log("KNOB cfg=" .. SHOOTER_CFG .. " bot_init " .. tostring(ok) .. " " .. tostring(code))
  end
  local t = g.tank(SHOOTER)
  if not t then return end

  if frame == FIRST_ROUND or (round_start and frame - round_start >= ROUND_FRAMES) then
    if round >= ROUNDS then finish(); return end
    round = round + 1
    round_start = frame
    place_shooter()
    return
  end
  if round == 0 or t.dead then prev_shells = nil; return end

  if prev_shells and t.shells < prev_shells then
    fired[round] = (fired[round] or 0) + (prev_shells - t.shells)
  end
  if t.shells < R.full_shells - 10 then
    g.set_stocks(SHOOTER, { shells = R.full_shells })
    prev_shells = R.full_shells
  else
    prev_shells = t.shells
  end
  local at_ring = false
  for dy = -1, 1 do
    for dx = -1, 1 do
      if on_ring(t.mx + dx, t.my + dy) then at_ring = true end
    end
  end
  if at_ring then near[round] = (near[round] or 0) + 1 end
end

VERDICT_CHECK = function(g)
  if done then return nil end
  return false, string.format("the run ended in round %d of %d", round, ROUNDS)
end
