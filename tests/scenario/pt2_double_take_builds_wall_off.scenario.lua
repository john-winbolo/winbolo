-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open
--
-- pt2_double_take_builds_wall_off -- the pt2_double_take_builds_wall arena with
-- the master knob off (pill take v1), to compare. Same map and layout.
--
-- Map: tests/scenario/maps/pt2_double_take_builds_wall.map (made by
-- tests/generate_pt2_maps.py): open grass 106..146 x 106..146, one base at
-- (126,140) that the script gives to the bot, and the spawn pond at
-- (126,144), filled with grass once the tank has driven off.
-- The script puts down two neutral pills, 15 armour, in a north-south line:
--   A at (126,116) and B at (126,119).
-- The bot comes from the south. From a spot south of B, A's shells at the
-- bot hit B, and B's shells at the bot are stopped by nothing: there is no
-- wall. v2's evaluator, run on this layout by hand (Nov 16), makes a chain
-- A > B and plans one wall at (126,120), B's south side, as the chain's
-- last blocker (5 shots, PT2_CHAIN_BLOCKER_MIN_SHOTS).
-- The bot gets 40 trees at the start, so it can build.
--
-- PASS: both pills are ours (picked up or ours on the map) before the end.
-- The log also says when a wall first stands at (126,120), and how each
-- pill died ("by=0" is our shell, "by=nobody" is a pill's shell).
--
-- pt2_double_take_builds_wall_off is the same arena with the master knob
-- off (pill take v1). 2026-11-16.

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILLTAKE_V2_EVALUATOR=false"
local p0 = 0
local PILLS = { { 126, 116 }, { 126, 119 } }
local WALL = { 126, 120 }
local SPAWN = { 126, 144 }
local END_TICK = 14800

local pill_n = {}          -- [1] = A, [2] = B
local taken_at = {}
local died = {}
local wall_at = nil
local stocked, filled, decided = false, false, false

function on_setup(g)
  for i, xy in ipairs(PILLS) do
    local n, code = g.add_pill(xy[1], xy[2], game.NEUTRAL, 15)
    if not n then g.log("PT2_FIELD add_pill refused: " .. tostring(code)) end
    pill_n[i] = n
  end
  for i = 1, g.num_bases() do
    if g.base(i) then g.set_base_owner(i, p0) end
  end
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.log(string.format("PT2_FIELD A is pill %s at (%d,%d), B is pill %s at (%d,%d)",
    tostring(pill_n[1]), PILLS[1][1], PILLS[1][2],
    tostring(pill_n[2]), PILLS[2][1], PILLS[2][2]))
end

function on_pill_killed(n, by, scripted)
  for i = 1, 2 do
    if n == pill_n[i] and not died[i] then
      local who = (by == game.NEUTRAL) and "nobody" or tostring(by)
      died[i] = string.format("t=%d by=%s", game.tick(), who)
      game.log(string.format("PT2_FIELD pill %s died %s", i == 1 and "A" or "B", died[i]))
    end
  end
end

local function summary()
  return string.format("A %s died %s, B %s died %s, wall %s",
    taken_at[1] and ("ours t=" .. taken_at[1]) or "not ours", died[1] or "-",
    taken_at[2] and ("ours t=" .. taken_at[2]) or "not ours", died[2] or "-",
    wall_at and ("t=" .. wall_at) or "never")
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not stocked and tk and not tk.dead then
    stocked = true
    g.set_stocks(p0, { trees = 40 })
  end
  if not filled and tick >= 120 and tk and not tk.boat and not tk.dead
     and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
    filled = true
    g.set_tile(SPAWN[1], SPAWN[2], g.TERRAIN.grass)
  end
  if decided then return end
  if not wall_at and tick % 10 == 0 then
    local tt = g.map_tile(WALL[1], WALL[2])
    if tt == g.TERRAIN.building or tt == g.TERRAIN.half_building then
      wall_at = tick
      g.log(string.format("PT2_FIELD wall at (%d,%d) t=%d", WALL[1], WALL[2], tick))
    end
  end
  for i = 1, 2 do
    if not taken_at[i] and pill_n[i] then
      local p = g.pill(pill_n[i])
      if p and p.owner == p0 then
        taken_at[i] = tick
        g.log(string.format("PT2_FIELD pill %s ours t=%d", i == 1 and "A" or "B", tick))
      end
    end
  end
  if taken_at[1] and taken_at[2] then
    decided = true
    verdict(true, summary())
  elseif tick >= END_TICK then
    decided = true
    verdict(false, summary())
  end
end
