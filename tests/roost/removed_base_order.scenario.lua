-- ROOST: a base removed while a bot holds an order on it ends the order.
--
-- The base twin of removed_pill_order. game.remove_base takes a base off the
-- map and game.base(n) answers nil from then on. A bot out of sight of the
-- base when it goes has to learn that from the engine, or it keeps the base
-- in its own list and drives onto the empty square to take it.
--
-- The arena's base at (104,112) is the target. Seat 2 is parked eight
-- squares from it, so the team knows it, and says "!capture base N". Seats 0
-- and 1 hear it from twenty-two and twenty-six squares out, past the
-- fourteen a tank sees, and one of them takes it. The base is removed on the
-- tick the ack is heard.
--
-- Two things are read:
--   * the holder says "order lapsed" inside LAPSE_BY ticks, as it does for
--     a pill that has gone;
--   * no tank drives onto the removed base's square in the WATCH ticks
--     after it went. The other base, forty squares south, is still there to
--     be taken, so a bot has somewhere real to go.

scenario = {
  name        = "ROOST removed_base_order",
  description = "An order on a removed base lapses and nobody drives onto its square.",
  api         = 1,
}

local SPEAKER = 2
local BX, BY  = 104, 112
local POS = { [0] = { 126, 112 },   -- 22 squares from the base
              [1] = { 130, 118 },   -- 26
              [2] = { 112, 112 } }  -- 8, the speaker

local SETUP_AT = 150
local SAY_AT   = 400
local ACK_BY   = 600
local LAPSE_BY = 400
local WATCH    = 1500

local base_n, said_at, gone_at = nil, nil, nil
local holder, lapsed_at = nil, nil
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
  game.log(string.format("removed_base_order: t=%d p%d says: %s", game.tick(), p, text))
  if said_at and not holder and base_n
     and text:find("capture_base #" .. (base_n - 1), 1, true) then
    holder = p
    game.remove_base(base_n)
    gone_at = game.tick()
    game.log(string.format("removed_base_order: p%d acked; base removed at t=%d, holder %d away",
                           p, gone_at, away(p, BX, BY) or -1))
  end
  if gone_at and p == holder and not lapsed_at and text:find("order lapsed", 1, true) then
    lapsed_at = game.tick()
  end
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for n = 1, game.num_bases() do
      local b = game.base(n)
      if b and b.x == BX and b.y == BY then base_n = n end
    end
    if not base_n then
      finish("FAIL removed_base_order: no base at (104,112)")
      return
    end
    for p = 0, 2 do
      game.builder_recall(p)
      game.teleport(p, POS[p][1], POS[p][2])
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
  end

  if t == SAY_AT then
    if not game.say(SPEAKER, "!capture base " .. (base_n - 1), "all") then
      finish("FAIL removed_base_order: game.say was refused")
      return
    end
    said_at = t
  end

  if said_at and not gone_at and t - said_at >= ACK_BY then
    finish("FAIL removed_base_order: nobody took the order")
    return
  end
  if not gone_at then return end

  local since = t - gone_at
  if game.base(base_n) ~= nil then
    finish("FAIL removed_base_order: game.base still answers after the removal")
    return
  end
  for q = 0, 2 do
    if away(q, BX, BY) == 0 then
      finish(string.format("FAIL removed_base_order: p%d drove onto the removed base at +%d",
                           q, since))
      return
    end
  end
  if since >= LAPSE_BY and not lapsed_at then
    finish(string.format("FAIL removed_base_order: p%d never said order lapsed; %d away at +%d",
                         holder, away(holder, BX, BY) or -1, since))
    return
  end
  if since >= WATCH then
    finish(string.format("PASS removed_base_order: p%d lapsed at +%d, square left alone %d ticks",
                         holder, (lapsed_at or t) - gone_at, WATCH))
  end
end
