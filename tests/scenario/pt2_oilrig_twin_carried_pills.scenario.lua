-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open
--
-- pt2_oilrig_twin_carried_pills -- pt2_oilrig_twin_neutral_double_take, but
-- the bot's two pills are NOT on the map: they start in its tank. Question:
-- does pill take v2 put at least one of them down as a blocker?
--
-- Map: data/maps/DH-Oil Rig.map, unchanged. Only the neutral twins at
-- (129,122) and (130,122) stay. The bot's two pills are added and then
-- given to its tank (give_pill, retried from tick 12 until the tank is
-- placed). One bot (seat 0), from start 1 (140,110). Every base is ours.
--
-- PASS: both twins are ours before the end. The log has a
-- PT2_CARRY line for every pill of ours put down on the map while a twin
-- is still alive (that is a blocker drop), with its square and tick.
-- 2026-11-17.

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
local carried = {}       -- pill numbers handed to the tank
local gave = false
local planted = {}       -- [n] = true once logged

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
    if not n then g.log("PT2_OILRIG add_pill refused: " .. tostring(code))
    else carried[#carried + 1] = n end
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
  if not gave and tick >= 12 then
    local t = g.tank(p0)
    if t and not t.dead then
      local ok = true
      for _, n in ipairs(carried) do
        local p = g.pill(n)
        if not (p and p.in_tank) then
          if not g.give_pill(p0, n) then ok = false end
        end
      end
      if ok then
        gave = true
        g.log(string.format("PT2_CARRY t=%d gave %d pills to the tank", tick, #carried))
      end
    end
  end
  if gave then
    for _, n in ipairs(carried) do
      local p = g.pill(n)
      if p and not p.in_tank and p.owner == p0 and not planted[n] then
        planted[n] = true
        local alive = 0
        for i = 1, 2 do
          local q = twin_n[i] and g.pill(twin_n[i])
          if q and q.owner ~= p0 and (q.armour or 0) > 0 then alive = alive + 1 end
        end
        g.log(string.format("PT2_CARRY t=%d pill %d put down at (%d,%d) twins_alive=%d",
                            tick, n, p.x, p.y, alive))
      elseif p and p.in_tank then
        planted[n] = nil
      end
    end
  end
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
