-- ROOST: a defend order ends the moment the pillbox stops being ours.
--
-- Andrew, Sep 15: a bot told to defend one of our pillboxes was still in
-- defend_pill after that pillbox had been shot flat and taken by the enemy,
-- and it then did nothing at all. The order was still live, so the reject
-- pass went on killing every other strategic row for a pillbox that was no
-- longer ours to guard, and the bot sat there.
--
-- An order ends the moment its job is gone — that rule was already written
-- for sweeps, bases and named tanks, and defend was left out of it. Being
-- left out was about the TIMER (standing on the spot IS the job, so a defend
-- runs its clock out) and never about the target going away.
--
-- The round asks the two things a script outside the brain can see:
--
--   1. IT SAYS SO. Within sixty ticks of the pillbox changing hands the
--      holder says a line with "lost pill" in it.
--   2. IT MOVES. Within six hundred ticks it is at least three squares from
--      the tile it was holding when the pillbox flipped. "Did nothing" is
--      exactly a bot that stays put, so standing still is the failure.
--
-- Three seats, `-teams 2,1`: seats 0 and 1 on team 1, seat 2 on team 2.
-- Seat 0 gives the order and seat 1 takes it (a sender never hears its own
-- line, so seat 1 is the only bot that can). Seat 2 is the enemy the pillbox
-- is handed to; it is parked in the far corner and does nothing else.

scenario = {
  name        = "ROOST order_defend_lost",
  description = "A defend order ends when the pillbox is lost, and the bot goes back to work.",
  api         = 1,
}

local SPEAKER  = 0        -- team 1, gives the order
local DEFENDER = 1        -- team 1, takes it
local ENEMY    = 2        -- team 2, ends up owning the pillbox

local PX, PY = 128, 128   -- the pillbox
local POS = { [0] = { 118, 128 },   -- the speaker, ten squares off
              [1] = { 122, 128 },   -- the defender, six squares off
              [2] = { 152, 152 } }  -- the enemy, out of the way

local SETUP_AT = 150
local SAY_AT   = 250
local FLIP_GAP = 400      -- ticks after the ack that the pillbox changes hands
local SAY_BY   = 60       -- ticks after the flip to say "lost pill" in
local MOVE_BY  = 600      -- ticks after the flip to have moved in
local MOVED_AT = 3        -- squares that counts as having moved
local END_AT   = 3000     -- a round that got nowhere

local pill_n   = nil
local ack_txt  = nil
local ack_at   = nil
local flip_at  = nil
local hold_mx, hold_my = nil, nil
local lost_at  = nil
local lost_txt = nil
local seen     = {}
local done     = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

-- Squares apart the way a screen is: the larger of the two axes.
local function away_from(p, mx, my)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = tk.mx - mx, tk.my - my
  if dx < 0 then dx = -dx end
  if dy < 0 then dy = -dy end
  return (dx > dy) and dx or dy
end

function on_start()
  for _, p in ipairs({ SPEAKER, DEFENDER, ENEMY }) do
    if not game.tank(p) then
      finish("FAIL order_defend_lost: the round needs three seated bots")
      return
    end
  end
end

function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) then return end

  -- The ack names the goal, so it is how the round knows the order landed.
  if not ack_at and ack_txt and p == DEFENDER
     and text:find(ack_txt, 1, true) then
    ack_at = game.tick()
    game.log(string.format("order_defend_lost: p%d acked at %d: %s",
                           p, ack_at, text))
    return
  end

  if flip_at and not lost_at and p == DEFENDER
     and text:find("lost pill", 1, true) then
    lost_at, lost_txt = game.tick(), text
    game.log(string.format("order_defend_lost: %q at +%d",
                           text, lost_at - flip_at))
  end
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for p = 0, 2 do
      game.builder_recall(p)
      game.teleport(p, POS[p][1], POS[p][2])
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
    -- OUR pillbox, with armour on it: a live pillbox of our own is the one
    -- thing a defend order is for.
    pill_n = game.add_pill(PX, PY, DEFENDER, 15)
    if type(pill_n) ~= "number" then
      finish("FAIL order_defend_lost: the pillbox could not be placed")
      return
    end
    -- brain pill numbers are the engine's, counted from zero; the script's
    -- are counted from one.
    ack_txt = "defend_pill #" .. (pill_n - 1)
    return
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!defend " .. (pill_n - 1), "all") then
      finish("FAIL order_defend_lost: game.say was refused")
    end
    return
  end

  if not ack_at then
    if t >= END_AT then
      finish("FAIL order_defend_lost: nobody took the defend order")
    end
    return
  end

  -- ── THE PILLBOX CHANGES HANDS ─────────────────────────────────────────
  if not flip_at then
    if (t - ack_at) < FLIP_GAP then return end
    local tk = game.tank(DEFENDER)
    if not tk then
      finish("FAIL order_defend_lost: the defender lost its tank")
      return
    end
    hold_mx, hold_my = tk.mx, tk.my
    local ok, code, detail = game.set_pill_owner(pill_n, ENEMY)
    if not ok then
      finish(string.format("FAIL order_defend_lost: the flip was refused %s (%s)",
                           tostring(code), tostring(detail)))
      return
    end
    flip_at = t
    game.log(string.format(
      "order_defend_lost: pill %d is the enemy's at %d; the bot holds (%d,%d)",
      pill_n, t, hold_mx, hold_my))
    return
  end

  local since = t - flip_at

  -- ── 1. IT SAYS SO ─────────────────────────────────────────────────────
  if not lost_at then
    if since >= SAY_BY then
      finish(string.format(
        "FAIL order_defend_lost: %d ticks after the flip, no 'lost pill' line",
        SAY_BY))
    end
    return
  end

  -- ── 2. IT MOVES ───────────────────────────────────────────────────────
  local d = away_from(DEFENDER, hold_mx, hold_my)
  if not d then
    finish("FAIL order_defend_lost: the defender lost its tank")
    return
  end
  if d >= MOVED_AT then
    finish(string.format(
      "PASS order_defend_lost: %q at +%d, %d squares off by +%d",
      lost_txt, lost_at - flip_at, d, since))
    return
  end
  if since >= MOVE_BY then
    finish(string.format(
      "FAIL order_defend_lost: %d ticks on it is still %d from (%d,%d)",
      MOVE_BY, d, hold_mx, hold_my))
  end
end
