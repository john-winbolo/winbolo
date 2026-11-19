-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open
--
-- pt2_oilrig_single_take -- pill take v2 takes one lone neutral pill
-- (PILLTAKE_V2_EVALUATOR on, the default). A plain single take, to watch
-- next to pt2_oilrig_twin_neutral_double_take.
--
-- Map: data/maps/DH-Oil Rig.map, unchanged. Only map pill 12 at (129,122)
-- stays (neutral, 15 armour); every other map pill is removed, its twin
-- at (130,122) too. The bot has no pills of its own, so the cover must
-- come from the map or from walls it builds. One bot (seat 0), from
-- start 1 (140,110) in the north-east. No other tanks. Every base is
-- given to the bot so it does not go off to take bases and it can refill.
--
-- PASS: the pill is ours (picked up or ours on the map) before the end.
-- 2026-11-17.

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILLTAKE_V2_EVALUATOR=true"
local p0 = 0
local TARGET = { 129, 122 }
local END_TICK = 14800

local target_n = nil
local taken_at = nil
local died = nil
local decided = false

function on_setup(g)
  for n = 1, g.num_pills() do
    local p = g.pill(n)
    if p then
      if p.x == TARGET[1] and p.y == TARGET[2] then
        target_n = n
      else
        g.remove_pill(n)
      end
    end
  end
  for i = 1, g.num_bases() do
    if g.base(i) then g.set_base_owner(i, p0) end
  end
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.log(string.format("PT2_SINGLE target is pill %s", tostring(target_n)))
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

function on_pill_killed(n, by, scripted)
  if n == target_n and not died then
    local who = (by == game.NEUTRAL) and "nobody" or tostring(by)
    died = string.format("t=%d by=%s", game.tick(), who)
    game.log(string.format("PT2_SINGLE pill %d died %s", n, died))
  end
end

local function summary()
  return string.format("pill %s died %s",
    taken_at and ("ours t=" .. taken_at) or "not ours", died or "-")
end

function on_tick(g, tick)
  if decided then return end
  if not taken_at and target_n then
    local p = g.pill(target_n)
    if p and p.owner == p0 then
      taken_at = tick
      g.log(string.format("PT2_SINGLE pill ours t=%d", tick))
    end
  end
  if taken_at then
    decided = true
    verdict(true, summary())
  elseif tick >= END_TICK then
    decided = true
    verdict(false, summary())
  end
end
