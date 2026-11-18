-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open
--
-- pt2_chain_claim_two_allies_off -- PT2_CHAIN_CLAIM off. Two allied bots, one pair of side-by-side
-- neutral pills (PT2_CHAIN_CLAIM off). Nov 18: in a HELM 2v2
-- replay one ally went for pill 14 while the other went for a 13+14 double
-- take; each claim named only its target, so both went in.
--
-- Map and pills: the pt2_oilrig_twin_neutral_double_take arena (DH-Oil
-- Rig, one twin pair at (129,122)/(130,122), the bot's own pills at
-- (128,122) and (131,122), so a double take on the pair is possible).
-- Two bots on team 1 (team 0 is no team): seat 0 from start 1 (140,110), seat 1 from start 2.
--
-- Both bots are "noblitz": a blitz lets two allies join on one pill on
-- purpose, and that would hide the claim this arena is about.
--
-- PASS: both twins are ours (either seat) before the end. The log counts pill shells
-- that hit each bot ("PT2_CLAIM2 hit"), and the bots' print2 logs show the
-- chain claims (PCH_TX / PCH_RX / PCH_SKIP / PCH_CLAIM).
--
-- pt2_chain_claim_two_allies_off is the same arena with PT2_CHAIN_CLAIM
-- off. 2026-11-18.

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILLTAKE_V2_EVALUATOR=true;cfg=PT2_CHAINS=true;noblitz;cfg=PT2_CHAIN_CLAIM=false"
local p0, p1 = 0, 1
local TWINS = { { 129, 122 }, { 130, 122 } }
local OUR_PILLS = { { 128, 122 }, { 131, 122 } }
local END_TICK = 14800
local pill_hits = { [0] = 0, [1] = 0 }

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
    if not n then g.log("PT2_CLAIM2 add_pill refused: " .. tostring(code)) end
  end
  for i = 1, g.num_bases() do
    if g.base(i) then g.set_base_owner(i, p0) end
  end
  g.spawn_bot{ slot = p0, name = "BotA", team = 1, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.spawn_bot{ slot = p1, name = "BotB", team = 1, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.log(string.format("PT2_CLAIM2 twins are pills %s and %s",
                      tostring(twin_n[1]), tostring(twin_n[2])))
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  if p == p1 and g.num_starts() >= 2 then return 2 end
  return nil
end

function on_pill_killed(n, by, scripted)
  local i = twin_index(n)
  if i and not died[i] then
    local who = (by == game.NEUTRAL) and "nobody" or tostring(by)
    died[i] = string.format("t=%d by=%s", game.tick(), who)
    game.log(string.format("PT2_CLAIM2 twin %d (pill %d) died %s", i, n, died[i]))
  end
end

function on_tank_spawned(p, x, y)
  game.log(string.format("PT2_CLAIM2 seat %s spawned at %s,%s", tostring(p), tostring(x), tostring(y)))
end

function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if pill and pill_hits[victim] then
    pill_hits[victim] = pill_hits[victim] + 1
    game.log(string.format("PT2_CLAIM2 hit seat %d by pill %s t=%d", victim, tostring(pill), game.tick()))
  end
end

local function summary()
  return string.format("pill hits seat0=%d seat1=%d, ", pill_hits[0], pill_hits[1]) .. string.format("twin1 %s died %s, twin2 %s died %s",
    taken_at[1] and ("ours t=" .. taken_at[1]) or "not ours", died[1] or "-",
    taken_at[2] and ("ours t=" .. taken_at[2]) or "not ours", died[2] or "-")
end

function on_tick(g, tick)
  if decided then return end
  for i = 1, 2 do
    if not taken_at[i] and twin_n[i] then
      local p = g.pill(twin_n[i])
      if p and (p.owner == p0 or p.owner == p1) then
        taken_at[i] = tick
        g.log(string.format("PT2_CLAIM2 twin %d ours t=%d", i, tick))
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
