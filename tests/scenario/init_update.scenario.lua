-- Scenario sidecar for tests/init_update.map — NEW ORDERS MID-ROUND.
--
-- This one is not a port. It is the arena for `game.bot_init(p, t)`, the row
-- that hands a bot already on the field a new init table, and for the
-- Brain.on_init that GoalHunter answers it with.
--
-- Two real GoalHunter 1.7 bots on one team, so there is somebody for the
-- announcement to be said TO: a team line with nobody on the team is dropped
-- unsaid, which is right for the brain and useless for a test.
--
-- What must happen, in order:
--   1. the op is accepted while the round runs, and refused for a seat that
--      holds nobody;
--   2. the brain's Brain.on_init runs — which nothing outside the bot can see
--      directly, so it says one team line, "init updated: N tokens";
--   3. that line arrives at on_chat from the seat we wrote to.
--
-- Step 3 is the whole verdict. A scenario cannot see a bot think, so an
-- arena about a brain being TOLD something has to be answered by the brain
-- saying something back, through the only channel that leaves the bot.

local TARGET = 0
local OTHER  = 1
local TEAM   = 1        -- team 0 is NO team on this host: two bots on it are
                        -- not allies and the line would reach nobody
local GOALHUNTER = "../brains/GoalHunter_1.7/init.lua"  -- from the build dir

-- Sent at ORDER_AT. Two tokens: one bare flag the tick-1 parser matches by
-- name, and one constant override that goes through _cfg_set. Both are things
-- the brain applies live, which is what makes "unsupported at runtime" (mode=
-- and difficulty=) a different answer and not this arena's business.
local NEW_INIT = { noblitz = "1", cfg = "PILL_REPOSITION_ENABLED=false" }
local WANT_TOKENS = 2

-- The hook clock counts 100 a second here, so this is six seconds in: long
-- enough that both brains are past their first think and settled, short
-- enough to leave the rest of the round for the answer.
local ORDER_AT = 600

local sent_at    = nil    -- tick game.bot_init was accepted
local refused_ok = nil    -- the empty-seat refusal answered under its own code
local heard_at   = nil    -- tick the announcement arrived
local heard_from = nil
local heard_text = nil

function on_setup(g)
  -- Both bots are fielded by the arena rather than by the runner's -bots: the
  -- arena has to know which seat it is going to write to. The roster ops are
  -- refused inside on_setup and the prelude defers them to on_start, so these
  -- two land on the round's first tick. `start` is named here rather than
  -- left to on_choose_start because that hook fires INSIDE spawn_bot.
  g.spawn_bot{ slot = TARGET, name = "Told", brain = GOALHUNTER,
               team = TEAM, start = 1 }
  g.spawn_bot{ slot = OTHER, name = "Hears", brain = GOALHUNTER,
               team = TEAM, start = 2 }
end

-- Only for a respawn: the first lives come in on the starts named above.
function on_choose_start(g, p)
  if p == TARGET then return 1 end
  if p == OTHER  then return 2 end
  return nil
end

function on_tick(g, tick)
  if sent_at ~= nil or tick < ORDER_AT then return end

  -- A seat nobody is in, first: the refusal has to come back under its own
  -- code rather than as a silent nothing, because a script that cannot tell
  -- a refusal from a success cannot be written against this row.
  local ok, code = g.bot_init(15, NEW_INIT)
  refused_ok = (ok == nil and code == "SCN_OP_NO_SUCH_PLAYER")
  if not refused_ok then
    g.log(string.format("init_update: empty seat answered %s / %s",
                        tostring(ok), tostring(code)))
  end

  -- And then the real one.
  local accepted = g.bot_init(TARGET, NEW_INIT)
  if accepted then
    sent_at = tick
  else
    g.log("init_update: the bot refused its new orders")
    sent_at = -1
  end
end

-- The announcement. Anything else the bots say on the way past is ignored:
-- this is looking for one line from one seat.
function on_chat(g, p, text, scripted)
  if heard_at ~= nil or scripted then return end
  if p ~= TARGET then return end
  if not string.find(text, "init updated", 1, true) then return end
  heard_at, heard_from, heard_text = g.tick(), p, text
end

-- ── the verdict ─────────────────────────────────────────────────────────
--
-- GATE: ticks=4000 bots=0 ai=yesfull gametype=open limit=20

VERDICT_CHECK = function(g)
  if sent_at == nil then
    return false, "the arena never issued its bot_init"
  end
  if sent_at < 0 then
    return false, "bot_init was refused for a seat holding a live bot"
  end
  if not refused_ok then
    return false, "an empty seat was not refused as SCN_OP_NO_SUCH_PLAYER"
  end
  if heard_at == nil then
    return false, string.format("ordered at t=%d and the bot never said its "
                                .. "init had changed", sent_at)
  end
  if not string.find(heard_text, tostring(WANT_TOKENS) .. " tokens", 1, true) then
    return false, string.format("the bot said '%s', expected %d tokens",
                                heard_text, WANT_TOKENS)
  end
  return true, string.format("ordered at t=%d, p%d said '%s' at t=%d",
                             sent_at, heard_from, heard_text, heard_at)
end
