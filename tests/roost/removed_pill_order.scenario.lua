-- ROOST: a pill removed while a bot holds an order on it ends the order.
--
-- game.remove_pill takes a pillbox off the map for good: the slot stays, and
-- game.pill(n) answers nil from then on. A bot that is out of sight of the
-- pill when it goes cannot see it go. It has to learn it from the engine, or
-- it keeps the pill in its own list for the rest of the round, drives to the
-- empty square and shoots at nothing there.
--
-- Seat 2 is parked ten squares from the pill, so the team knows it, and says
-- "!attack 0". Seats 0 and 1 hear it from twenty-two and twenty-six squares
-- out, which is past the fourteen a tank sees, and one of them takes it. The
-- pill is removed on the tick the ack is heard, so the holder is still far
-- away and cannot have seen it go.
--
-- Two things are read:
--   * the holder says "order lapsed", the line orders.lua says when the
--     order's pill is no longer in its list, inside LAPSE_BY ticks;
--   * no bot spends a shell in the WATCH ticks after the removal (after a
--     short grace for a shot already on its way). There is nothing on this
--     island to shoot at once the pill is gone: the bots are allies and the
--     two bases are neutral, which are taken by driving over them.

scenario = {
  name        = "ROOST removed_pill_order",
  description = "An order on a removed pill lapses and nobody shoots at its square.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local POS = { [0] = { 150, 128 },   -- 22 squares from the pill
              [1] = { 154, 120 },   -- 26
              [2] = { 138, 128 } }  -- 10, the speaker

local SETUP_AT = 150
local SAY_AT   = 400      -- time for the speaker's sighting to reach the others
local ACK_BY   = 600      -- ticks after the order for somebody to answer
local LAPSE_BY = 400      -- ticks after the removal for the holder to let go
local GRACE    = 100      -- a shell already fired when the pill went
local WATCH    = 1500     -- ticks after the removal that no shell may be spent

local pill_n, said_at, gone_at = nil, nil, nil
local holder, lapsed_at = nil, nil
local shells = {}
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
  if not fresh(p, text) then return end
  game.log(string.format("removed_pill_order: t=%d p%d says: %s", game.tick(), p, text))
  if said_at and not holder and text:find("attack_pill #0", 1, true) then
    holder = p
    game.remove_pill(pill_n)
    gone_at = game.tick()
    for q = 0, 2 do
      local tk = game.tank(q)
      shells[q] = tk and tk.shells or 0
    end
    game.log(string.format("removed_pill_order: p%d acked; pill removed at t=%d, holder %d away",
                           p, gone_at, away(p, PX, PY) or -1))
  end
  if gone_at and p == holder and not lapsed_at and text:find("order lapsed", 1, true) then
    lapsed_at = game.tick()
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
      finish("FAIL removed_pill_order: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!attack " .. (pill_n - 1), "all") then
      finish("FAIL removed_pill_order: game.say was refused")
      return
    end
    said_at = t
  end

  if said_at and not gone_at and t - said_at >= ACK_BY then
    finish("FAIL removed_pill_order: nobody took the order")
    return
  end
  if not gone_at then return end

  local since = t - gone_at
  if game.pill(pill_n) ~= nil then
    finish("FAIL removed_pill_order: game.pill still answers after the removal")
    return
  end
  if since >= GRACE then
    for q = 0, 2 do
      local tk = game.tank(q)
      if tk and tk.shells < shells[q] then
        finish(string.format("FAIL removed_pill_order: p%d fired at +%d (%d->%d shells)",
                             q, since, shells[q], tk.shells))
        return
      end
      if tk then shells[q] = tk.shells end
    end
  else
    for q = 0, 2 do
      local tk = game.tank(q)
      if tk then shells[q] = tk.shells end
    end
  end
  if since >= LAPSE_BY and not lapsed_at then
    finish(string.format("FAIL removed_pill_order: p%d never said order lapsed; %d away at +%d",
                         holder, away(holder, PX, PY) or -1, since))
    return
  end
  if since >= WATCH then
    finish(string.format("PASS removed_pill_order: p%d lapsed at +%d, no shot in %d ticks",
                         holder, (lapsed_at or t) - gone_at, WATCH))
  end
end
