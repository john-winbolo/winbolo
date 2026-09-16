-- ROOST: a chat line from a seat reaches a bot's brain and it obeys.
--
-- What is under test is game.say: the scenario op that makes a SEAT talk,
-- as against game.message, which makes the SERVER talk. A server line goes
-- on a player's newswire and never enters a brain's inbox; a seat's line is
-- chat, and chat is the only thing a brain reads. So this round is the
-- proof that a scripted test can hand a bot the order a human ally would
-- type.
--
-- Two GoalHunter bots on one team, from the same brain and the same seed.
-- One of them (the speaker) says "!stop" to everyone. The other (the
-- listener) is the only one who hears it — a sender never receives its own
-- line — so the two bots differ in exactly one thing.
--
-- THE "!" IS THE POINT. This command is a hard lock: it hands the bot a goal
-- ahead of goal selection, so nothing survives it — no flee, no refuel. A
-- chat word said in passing must therefore not fire it. The round proves
-- both halves in one go: a BARE "stop" earlier on leaves the listener
-- turning as usual, and the "!stop" that follows freezes it.
--
-- The verdict is read off the tank's facing, not its position. GoalHunter
-- answers "stop" by pausing: it holds no key from then on. A Bolo tank that
-- is handed no key keeps the speed it had and stops TURNING, so it coasts
-- in a dead straight line. Facing is therefore the clean signal: after the
-- order the listener's direction must never change again, while the
-- speaker's changes constantly. Position would say much less, because a
-- coasting tank still moves.
--
-- Note on the destination. The line goes to "all" rather than to the team
-- (which is what game.say does with no target). Under -nolobby, which is
-- how a headless round starts without a human to ready up, no lobby-slot
-- event is ever published, so each bot's own client copy of the roster has
-- team 0 and the receiver's team filter drops team chat. The server-side
-- roster does carry the team, so the op itself is accepted; it is the
-- receiver that never sees it. A broadcast has no team filter to fail.

scenario = {
  name        = "ROOST say_stop_halts_bot",
  description = "A seat's chat line reaches a bot's brain and it obeys.",
  api         = 1,
}

local LISTENER = 0    -- hears the order
local SPEAKER  = 1    -- gives it, and never hears its own line

-- Phase 1: the bare word, which must do nothing.
local BARE_AT   = 100    -- game.tick units: 100 a second, so 1 s in
local BARE_FROM = 300    -- the "it kept going" window, 2 s after the word
local BARE_TO   = 900    -- and 6 s wide, closing before the real order

-- Phase 2: the same word with its "!", which must stop the bot dead.
local SAY_AT    = 1000   -- 10 s in
local SETTLE_AT = 1400   -- the window opens here, 4 s after the order
local VERDICT_AT= 3000   -- and closes here, 16 s after it

-- How much turning the speaker has to do for the round to say anything at
-- all. A control bot that never turned either would make a listener that
-- never turned meaningless.
local CONTROL_MIN_TURNS = 20
-- And how much the listener has to still be turning after the bare word,
-- over a window a quarter as long.
local BARE_MIN_TURNS = 5

local last_dir  = {}     -- seat -> the facing we last saw
local turns     = {}     -- seat -> how often it changed inside the window
local bare_dir  = {}     -- the same pair for the bare-word window
local bare_turns= {}
local said_bare = false
local judged_bare = false
local said      = false
local done      = false

local function watch(p)
  local tk = game.tank(p)
  if not tk then return end
  if last_dir[p] and tk.dir ~= last_dir[p] then
    turns[p] = (turns[p] or 0) + 1
  end
  last_dir[p] = tk.dir
end

local function watch_bare(p)
  local tk = game.tank(p)
  if not tk then return end
  if bare_dir[p] and tk.dir ~= bare_dir[p] then
    bare_turns[p] = (bare_turns[p] or 0) + 1
  end
  bare_dir[p] = tk.dir
end

local function finish(text)
  if done then return end
  done = true
  -- The log line is what the runner reads: the round's own end text does
  -- not reach the server's console.
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

function on_start()
  if not game.tank(LISTENER) or not game.tank(SPEAKER) then
    finish("FAIL say_stop_halts_bot: the round needs two seated bots")
  end
end

function on_tick(t)
  if done then return end

  -- ── Phase 1: the bare word ────────────────────────────────────────────
  if not said_bare and t >= BARE_AT then
    said_bare = true
    local ok, code, detail = game.say(SPEAKER, "stop", "all")
    if not ok then
      finish(string.format(
        "FAIL say_stop_halts_bot: game.say was refused %s (%s)",
        tostring(code), tostring(detail)))
      return
    end
  end

  if t >= BARE_FROM and t < BARE_TO then
    watch_bare(LISTENER)
  end

  if not judged_bare and t >= BARE_TO then
    judged_bare = true
    local kept = bare_turns[LISTENER] or 0
    if kept < BARE_MIN_TURNS then
      finish(string.format(
        "FAIL say_stop_halts_bot: a bare stop was obeyed, only %d turns "
        .. "(need %d)", kept, BARE_MIN_TURNS))
      return
    end
  end

  -- ── Phase 2: the same word with its "!" ───────────────────────────────
  if not said and t >= SAY_AT then
    said = true
    local ok, code, detail = game.say(SPEAKER, "!stop", "all")
    if not ok then
      finish(string.format(
        "FAIL say_stop_halts_bot: game.say was refused %s (%s)",
        tostring(code), tostring(detail)))
      return
    end
  end

  if t >= SETTLE_AT then
    watch(LISTENER)
    watch(SPEAKER)
  end

  if t >= VERDICT_AT then
    local heard  = turns[LISTENER] or 0
    local control = turns[SPEAKER] or 0
    if control < CONTROL_MIN_TURNS then
      finish(string.format(
        "FAIL say_stop_halts_bot: the control bot turned %d times, under %d, "
        .. "so the round proves nothing", control, CONTROL_MIN_TURNS))
    elseif heard ~= 0 then
      finish(string.format(
        "FAIL say_stop_halts_bot: the bot told to stop turned %d times "
        .. "(control %d), so it never got the line", heard, control))
    else
      -- Every verdict line has to fit game.log's 128-byte chat budget, or
      -- the call is refused and the round looks like it said nothing.
      finish(string.format(
        "PASS say_stop_halts_bot: bare stop left %d turns, !stop left 0, "
        .. "control %d", bare_turns[LISTENER] or 0, control))
    end
  end
end
