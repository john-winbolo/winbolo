-- boat_dodge_off -- boat_dodge with DODGER_CFG = nil (the knob at its default).
-- Expected to fail. Everything below is boat_dodge's own text.
--
-- boat_dodge -- does a GoalHunter 1.7 boat get out of the way of shells?
--
-- In Joust every tank is on a boat and one hit sinks it. A shell flies at
-- 32 wu a frame and a boat sails at 16, so a boat that turns or changes
-- speed while a shell is on its way leaves the spot the shooter led.
-- C.BOAT_SHELL_DODGE (constants.lua; Joust's preset turns it on) flies every
-- hostile shell forward each brain tick and, when this tick's keys would
-- leave the boat in its path, takes the turn and throttle that keep the
-- shell farthest away (steering.lua boat_shell_dodge).
--
-- The arena: Joust.map, a round deep-sea arena with no land. Two GoalHunter
-- 1.7 bots fight. Seat 0 (the dodger) is handed DODGER_CFG through
-- game.bot_init; seat 1 plays with the defaults. Each round the script puts
-- both on boats in the middle of the sea, 6 squares apart, facing each
-- other, with full shells, and the round ends at the first hit (a hit sinks
-- the boat) or after ROUND_FRAMES. The next round starts when both tanks are
-- back on the map.
--
-- Measure: the hits each seat takes over ROUNDS rounds. Pass: the other seat
-- took at least MIN_FIGHT hits (the bots did fight) and the dodger took at
-- most half as many hits as the other seat.
--
-- boat_dodge_off is the same arena with DODGER_CFG = nil: both seats play
-- with the defaults, they take about the same number of hits, and that arena
-- is expected to fail.
--
-- GATE: ticks=80000 bots=2 gametype=open ai=yesfull
-- GATE: expect=fail GoalHunter 1.7 with BOAT_SHELL_DODGE=false (the default) takes about as many hits as the other seat

-- KNOB VARIANT. DODGER_CFG goes to seat 0 through game.bot_init at frame 3.
local DODGER_CFG = nil
local cfg_sent = false

local DODGER       = 0
local OTHER        = 1
local ROUNDS       = 24
local ROUND_FRAMES = 1200
local HALF_GAP     = 3           -- each tank this many squares from the middle
local FIRST_ROUND  = 50          -- frame of the first placement
local MIN_FIGHT    = 6

local frame = 0
local C = nil                    -- the middle of the sea, from the starts
local full_shells = nil
local round = 0
local round_start = nil
local live = false               -- a round is running
local hits = { [DODGER] = 0, [OTHER] = 0 }
local done = false

local function finish()
  done = true
  local d, o = hits[DODGER], hits[OTHER]
  game.log(string.format("DODGE rounds=%d hits taken: dodger=%d other=%d", round, d, o))
  local ok, why = true, ""
  if o < MIN_FIGHT then
    ok, why = false, string.format("the other seat took only %d hits (want %d): no fight ", o, MIN_FIGHT)
  elseif d * 2 > o then
    ok, why = false, string.format("the dodger took %d hits, more than half of %d ", d, o)
  end
  verdict(ok, why .. string.format("hits taken: dodger %d, other %d, in %d rounds", d, o, round))
end

local function place(p, dx, dir)
  game.teleport(p, C.x + dx, C.y, dir)
  game.set_boat(p, true)
  game.set_stocks(p, { shells = full_shells })
end

function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if live and not done and (victim == DODGER or victim == OTHER)
     and attacker ~= nil and attacker ~= victim then
    hits[victim] = hits[victim] + 1
    game.log(string.format("DODGE round %d: seat %d hit by %d (dodger %d, other %d)", round, victim, attacker, hits[DODGER], hits[OTHER]))
    live = false
  end
end

function on_tick(g, tick)
  if done then return end
  frame = frame + 1
  if C == nil then
    local sx, sy, n = 0, 0, 0
    for i = 1, g.num_starts() do
      local s = g.start(i)
      if s then sx, sy, n = sx + s.x, sy + s.y, n + 1 end
    end
    if n == 0 then return end
    C = { x = math.floor(sx / n + 0.5), y = math.floor(sy / n + 0.5) }
    full_shells = g.rule("tank_full_shells")
    g.log(string.format("DODGE middle=(%d,%d) full_shells=%d", C.x, C.y, full_shells))
  end
  if DODGER_CFG and not cfg_sent and frame >= 3 then
    cfg_sent = true
    local ok, code = g.bot_init(DODGER, { cfg = DODGER_CFG })
    g.log("KNOB cfg=" .. DODGER_CFG .. " bot_init " .. tostring(ok) .. " " .. tostring(code))
  end
  if frame < FIRST_ROUND then return end
  if live and frame - round_start >= ROUND_FRAMES then live = false end
  if live then return end
  local a, b = g.tank(DODGER), g.tank(OTHER)
  if not a or not b or a.dead or b.dead then return end
  if round_start and frame - round_start < 100 then return end
  if round >= ROUNDS then finish(); return end
  round = round + 1
  round_start = frame
  place(DODGER, -HALF_GAP, 64)
  place(OTHER, HALF_GAP, 192)
  live = true
end

VERDICT_CHECK = function(g)
  if done then return nil end
  return false, string.format("the run ended in round %d of %d", round, ROUNDS)
end
