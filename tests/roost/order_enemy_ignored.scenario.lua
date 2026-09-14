-- ROOST: an order from the other side is not an order.
--
-- Every way of giving an order runs through one check first: the SENDER's
-- team against the bot's. An ally's line is read whichever channel it came
-- down, and an enemy's line is dropped in silence — no goal, and no reply
-- either, because a reply would tell the enemy their line had landed.
--
-- `-teams 2,1` puts seats 0 and 1 on team 1 and seat 2 on team 2, so the
-- speaker is an enemy of the two bots that hear it. The line goes to
-- everyone, so no team filter on the CHANNEL can be what stopped it: the
-- only thing between the line and the goal is the check on who said it.
--
-- Silence is a weak thing to test on its own — a bot says nothing for all
-- sorts of reasons, and a pillbox nobody has seen is one of them. So the
-- round has a second half. Once the enemy's line has been ignored for six
-- hundred and fifty ticks, seat 0 — an ALLY of seat 1, on the same map,
-- naming the same pillbox in the same words — says it again, and seat 1 has
-- to take it. The order line works; the enemy is what did not.
--
-- Seat 0 sits twelve squares from the pillbox and seat 1 twenty, which is
-- the same pair of ranges order_attack_pill uses: the near bot sees the
-- pillbox and shares it with its ally, so both of them know the target the
-- two lines name and neither can be silent for want of knowing it.
--
-- Where the tanks end up is logged and not asserted. Both seats would drive
-- at the only pillbox on the map of their own accord, so their positions say
-- nothing about whose line they took; the control is what says it.

scenario = {
  name        = "ROOST order_enemy_ignored",
  description = "A chat order from the other team is dropped; the same line from an ally is taken.",
  api         = 1,
}

local ENEMY = 2           -- team 2
local ALLY  = 0           -- team 1, and the control speaker
local PX, PY  = 128, 128
local POS = { [0] = { 116, 128 },   -- team 1, 12 squares from the pill
              [1] = { 108, 124 },   -- team 1, 20
              [2] = { 140, 140 } }  -- team 2, well out of the way

local SETUP_AT   = 150
local ENEMY_AT   = 250
local CONTROL_AT = 900    -- 650 ticks after the enemy's line
local END_AT     = 1500

local pill_n = nil
local enemy_answers, first_answer = 0, nil
local control_ack, control_by = nil, nil
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
  local t = game.tick()
  if t < ENEMY_AT then return end
  if t < CONTROL_AT then
    -- Any answer at all counts against the round: an ack, a "didn't
    -- understand", a "Busy", a "not known yet". Silence is the whole point.
    enemy_answers = enemy_answers + 1
    first_answer = first_answer or text
    game.log(string.format("order_enemy_ignored: p%d answered the enemy at +%d: %s",
                           p, t - ENEMY_AT, text))
  elseif not control_ack and text:find("_pill #0", 1, true) then
    control_ack, control_by = t, p
    game.log(string.format("order_enemy_ignored: p%d took the ally's line at +%d: %s",
                           p, t - CONTROL_AT, text))
  end
end

function on_start()
  for p = 0, 2 do
    local s = game.lobby_slot(p)
    if s then game.log(string.format("order_enemy_ignored: p%d %s on team %s",
                                     p, tostring(s.name), tostring(s.team))) end
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
      finish("FAIL order_enemy_ignored: the pill could not be placed")
    end
  end

  if t == ENEMY_AT then
    if not game.say(ENEMY, "!attack " .. (pill_n - 1), "all") then
      finish("FAIL order_enemy_ignored: game.say was refused")
      return
    end
    game.log(string.format("order_enemy_ignored: the enemy ordered, p0 %d and p1 %d away",
                           away(0, PX, PY) or -1, away(1, PX, PY) or -1))
  end

  if t == CONTROL_AT then
    local d0, d1 = away(0, PX, PY), away(1, PX, PY)
    if enemy_answers > 0 then
      finish(string.format("FAIL order_enemy_ignored: the enemy got %d answers: %s",
                           enemy_answers, first_answer or "?"))
      return
    end
    game.log(string.format("order_enemy_ignored: silent, p0 %d and p1 %d away; "
                           .. "now the same line from an ally", d0 or -1, d1 or -1))
    if not game.say(ALLY, "!attack " .. (pill_n - 1), "all") then
      finish("FAIL order_enemy_ignored: the ally's line was refused")
    end
  end

  if t >= END_AT then
    if control_ack then
      finish(string.format("PASS order_enemy_ignored: the enemy got nothing, "
                           .. "the ally got p%d in %d ticks",
                           control_by, control_ack - CONTROL_AT))
    else
      finish("FAIL order_enemy_ignored: the ally's line was ignored too, "
             .. "so the round proves nothing")
    end
  end
end
