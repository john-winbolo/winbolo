-- ROOST: "cancel all" is heard, and the bot that held the order lets it go.
--
-- `cancel all` clears every live order on the team. The bot holding one
-- drops its claim and says "released", and that line is the proof: the only
-- place in orders.lua that says it is release_held, which is also the only
-- code that empties the order slot (o.held = nil, state._order = nil). A bot
-- cannot say "released" without having let the order go, so the line and the
-- release are the same event seen from outside.
--
-- The round therefore reads three things: a bot acked the order; the cancel
-- reached that same bot; and that bot answered "released" — within six
-- hundred ticks, which is fifteen times the twenty-odd ticks an order
-- normally takes to come back.
--
-- WHAT THIS ROUND DOES NOT ASSERT, and why. The obvious next question is
-- whether the tank then stops going to the pill, and that question has no
-- answer a round can read. The design says a cancel restores nothing: goal
-- selection simply reruns, and each bot picks its own best goal. On a map
-- whose only pillbox is the one that was ordered, the bot's own best goal IS
-- that pillbox, so a released bot driving straight at it is obeying nothing
-- and looks exactly like one that ignored the cancel. Arenas that put
-- something better beside the bot were tried and moved the problem rather
-- than solving it: attack_pill drives to a planned firing position rather
-- than at the target, so the distance to the pill rises and falls while the
-- order is still held, and with two pillboxes on the map the brain's own
-- message channel starves the order lines and none of them are said at all.
-- So the distances are logged, for the record, and the verdict is read off
-- the line that cannot be said by mistake.

scenario = {
  name        = "ROOST order_cancel",
  description = "`cancel all` is answered by the holder with released.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local POS = { [0] = { 116, 128 }, [1] = { 108, 124 }, [2] = { 108, 132 } }

local SETUP_AT  = 150
local SAY_AT    = 250
local CANCEL_AT = 650     -- 400 ticks after the order
local END_AT    = 1250    -- 600 ticks after the cancel

local pill_n, said_at, cancel_at = nil, nil, nil
local holder, released_at = nil, nil
local d_cancel = nil
local seen = {}
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function away(p, x, y)
  local tk = game.tank(p)
  if not tk then return nil end
  local dx, dy = math.abs(tk.mx - x), math.abs(tk.my - y)
  return (dx > dy) and dx or dy
end

local function fresh(p, text)
  local key = p .. "|" .. text .. "|" .. game.tick()
  if seen[key] then return false end
  seen[key] = true
  return true
end

function on_chat(p, text, scripted)
  if scripted or done or text:sub(1, 1) == "/" then return end
  if not fresh(p, text) or not said_at then return end
  if not holder and text:find("attack_pill #0", 1, true) then
    holder = p
    game.log(string.format("order_cancel: p%d acked at +%d: %s",
                           p, game.tick() - said_at, text))
  elseif cancel_at and not released_at and p == holder
         and text:lower():find("released", 1, true) then
    released_at = game.tick()
    game.log(string.format("order_cancel: p%d released at +%d: %s",
                           p, released_at - cancel_at, text))
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
    pill_n = game.add_pill(PX, PY, game.NEUTRAL, 15)
    if type(pill_n) ~= "number" then
      finish("FAIL order_cancel: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!attack " .. (pill_n - 1), "all") then
      finish("FAIL order_cancel: game.say was refused")
      return
    end
    said_at = t
  end

  if t == CANCEL_AT then
    if not holder then
      finish("FAIL order_cancel: nobody acked the order in 400 ticks")
      return
    end
    if not game.say(SPEAKER, "!cancel all", "all") then
      finish("FAIL order_cancel: the cancel was refused")
      return
    end
    cancel_at = t
    d_cancel = away(holder, PX, PY)
    game.log(string.format("order_cancel: cancelled, p%d was %d away", holder, d_cancel or -1))
  end

  -- The distances either side of the cancel, logged rather than asserted:
  -- see the note at the top of this file.
  if cancel_at and (t - cancel_at) % 300 == 0 then
    game.log(string.format("order_cancel: +%d, p%d is %d away",
                           t - cancel_at, holder, away(holder, PX, PY) or -1))
  end

  if cancel_at and t >= END_AT then
    if released_at then
      finish(string.format("PASS order_cancel: p%d acked, then released at +%d, %d away",
                           holder, released_at - cancel_at, away(holder, PX, PY) or -1))
    else
      finish(string.format("FAIL order_cancel: p%d never said released in %d ticks",
                           holder, t - cancel_at))
    end
  end
end
