-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open
--
-- pt2_oilrig_twin_neutral_double_take -- pill take v2 takes two pills in
-- one go (PILLTAKE_V2_EVALUATOR on, PT2_CHAINS on, the default).
--
-- Map: data/maps/DH-Oil Rig.map, unchanged (tests/generate_pt2_maps.py
-- copies it). It has 16 neutral pills in side-by-side pairs. This arena
-- keeps one pair:
--   map pill 12 at (129,122) and map pill 13 at (130,122) (pill numbers 13
--   and 14 here, game.pill(n) counts from 1), both neutral, 15 armour.
-- Every other map pill is removed. The bot gets two pills of its own, put
-- down on the road beside the pair, one on each side:
--   (128,122) west of the pair and (131,122) east of it.
-- Whichever twin the bot goes for (A), the other twin (B) is between A and
-- the bot, and the bot's own pill on B's far side stops B's shells: A's
-- shells hit B, and B's shells hit our pill. That is a chain of two with
-- our own pill as the last blocker, and nothing to build (v2's evaluator
-- run on this map by hand, Nov 16: from the east A=(129,122) parks near
-- (136.2,120.6); from the west A=(130,122) parks near (123.8,120.6)).
-- One bot (seat 0), from start 1 (140,110) in the north-east. No other
-- tanks. Every base is given to the bot, so it does not go off to take
-- bases first and it can refill.
--
-- PASS: both twins are ours (picked up or ours on the map) before the end.
-- The log says how each twin died: "by=0" is our shell, "by=nobody" is a
-- pill's shell (the other twin's), which is the chain at work.
--
-- pt2_oilrig_twin_neutral_double_take_off is the same arena with the
-- master knob off (pill take v1). 2026-11-16.

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILLTAKE_V2_EVALUATOR=true;cfg=PT2_CHAINS=true"
local p0 = 0
local TWINS = { { 129, 122 }, { 130, 122 } }
local OUR_PILLS = { { 128, 122 }, { 131, 122 } }
local END_TICK = 14800

local twin_n = {}          -- [1], [2] = pill numbers of the twins
local taken_at = {}        -- [i] = tick twin i became ours
local died = {}            -- [i] = "t=... by=..."
local decided = false

local function twin_index(n)
  if n == twin_n[1] then return 1 end
  if n == twin_n[2] then return 2 end
  return nil
end

function on_setup(g)
  for n = 1, g.num_pills() do
    local p = g.pill(n)
    if p then
      local keep = nil
      for i, xy in ipairs(TWINS) do
        if p.x == xy[1] and p.y == xy[2] then keep = i end
      end
      if keep then
        twin_n[keep] = n
      else
        g.remove_pill(n)
      end
    end
  end
  for _, xy in ipairs(OUR_PILLS) do
    local n, code = g.add_pill(xy[1], xy[2], p0, 15)
    if not n then g.log("PT2_OILRIG add_pill refused: " .. tostring(code)) end
  end
  for i = 1, g.num_bases() do
    if g.base(i) then g.set_base_owner(i, p0) end
  end
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.log(string.format("PT2_OILRIG twins are pills %s and %s",
                      tostring(twin_n[1]), tostring(twin_n[2])))
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_pill_killed(n, by, scripted)
  local i = twin_index(n)
  if i and not died[i] then
    local who = (by == game.NEUTRAL) and "nobody" or tostring(by)
    died[i] = string.format("t=%d by=%s", game.tick(), who)
    game.log(string.format("PT2_OILRIG twin %d (pill %d) died %s", i, n, died[i]))
  end
end

local function summary()
  return string.format("twin1 %s died %s, twin2 %s died %s",
    taken_at[1] and ("ours t=" .. taken_at[1]) or "not ours", died[1] or "-",
    taken_at[2] and ("ours t=" .. taken_at[2]) or "not ours", died[2] or "-")
end

function on_tick(g, tick)
  if decided then return end
  for i = 1, 2 do
    if not taken_at[i] and twin_n[i] then
      local p = g.pill(twin_n[i])
      if p and p.owner == p0 then
        taken_at[i] = tick
        g.log(string.format("PT2_OILRIG twin %d ours t=%d", i, tick))
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
