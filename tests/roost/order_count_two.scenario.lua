-- ROOST: a NUMBER in front of the verb says how many bots go.
--
-- "2 attack 0" is the count who-word. It is not a new way of choosing bots:
-- the auction that a plain "attack 0" already runs ranks every free bot by
-- what the trip costs it, and the count simply raises how many of those bids
-- win from one to two. So the count is readable from the outside in exactly
-- the place `all` is — the GROUP LINE, which carries the number: two bots on
-- one order say "2 on pill #0" once, from the lowest player number among
-- them, instead of an ack each.
--
-- This round is order_all_attack's arena and timings, with `all` replaced by
-- `2`, for a reason: a plain "attack 0" on the same squares is a different
-- round already in this directory (order_attack_pill), and it ends with ONE
-- bot acking and ONE bot arriving. The two rounds together are the reading —
-- same map, same seats, same seed, one word different, one bot or two.
--
-- Like order_all_attack it accepts either shape of answer, the group line or
-- an ack from each bot, because both mean two bots took it and which one
-- arrives depends on whether the claims got a free message slot in time. What
-- it will not accept is one bot going.
--
-- Seat 2 speaks, so seats 0 and 1 are the team the order can reach. Arrival
-- is read at eight squares because attack_pill stands off and shoots.

scenario = {
  name        = "ROOST order_count_two",
  description = "A count who-word puts exactly that many bots on the order.",
  api         = 1,
}

local SPEAKER = 2
local PX, PY  = 128, 128
local POS = { [0] = { 116, 128 }, [1] = { 108, 124 }, [2] = { 108, 132 } }

local SETUP_AT = 150
local SAY_AT   = 250
local ACK_BY   = 600
local ARRIVE_BY = 1500
local NEAR     = 8

local pill_n, said_at = nil, nil
local ackers, n_ackers = {}, 0
local group_line = nil
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
  if text:find("attack_pill #0", 1, true) and not ackers[p] then
    ackers[p] = true
    n_ackers = n_ackers + 1
    game.log(string.format("order_count_two: p%d acked at +%d: %s",
                           p, game.tick() - said_at, text))
  elseif text:find("on pill #0", 1, true) then
    group_line = text
    game.log(string.format("order_count_two: group line at +%d: %s",
                           game.tick() - said_at, text))
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
      finish("FAIL order_count_two: the pill could not be placed")
    end
  end

  if t == SAY_AT then
    -- brain pill numbers are the engine's, counted from zero.
    if not game.say(SPEAKER, "!2 attack " .. (pill_n - 1), "all") then
      finish("FAIL order_count_two: game.say was refused")
      return
    end
    said_at = t
    game.log(string.format("order_count_two: ordered, p0 %d away, p1 %d away",
                           away(0, PX, PY) or -1, away(1, PX, PY) or -1))
  end

  if not said_at then return end
  local since = t - said_at

  if since == ACK_BY and not group_line and n_ackers < 2 then
    finish(string.format("FAIL order_count_two: no group line and %d acks in %d ticks",
                         n_ackers, ACK_BY))
    return
  end

  -- THE GROUP LINE CARRIES THE COUNT. If it came at all it has to say two:
  -- a line reading "3 on pill #0" would mean the number was read as `all`.
  if group_line and not group_line:find("2 on pill #0", 1, true) then
    finish(string.format("FAIL order_count_two: group line says %s", group_line))
    return
  end

  if since >= ACK_BY then
    local d0, d1 = away(0, PX, PY), away(1, PX, PY)
    local near = 0
    if d0 and d0 <= NEAR then near = near + 1 end
    if d1 and d1 <= NEAR then near = near + 1 end
    if near >= 2 then
      finish(string.format("PASS order_count_two: %s, p0 %d and p1 %d away at +%d",
                           group_line and "group line" or (n_ackers .. " acks"),
                           d0, d1, since))
    elseif since >= ARRIVE_BY then
      finish(string.format("FAIL order_count_two: %d acks, p0 %d and p1 %d away at +%d",
                           n_ackers, d0 or -1, d1 or -1, since))
    end
  end
end
