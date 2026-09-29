-- ROOST: a pill removed while a tank carries it is not placed and not dropped.
--
-- Seat 0 and seat 1 are each given a pill to carry. The engine refuses to
-- remove a pill that is in a tank, so a script drops it first: on one tick
-- seat 0's pill is dropped on a far square and removed. Seat 0's brain saw
-- the pill in its own tank a moment ago and never sees it on the ground.
-- Seat 1 keeps its pill. Whether seat 1 places it is only logged: a bot
-- with no enemy on the map often has no square it wants a pill on.
--
-- Read:
--   * seat 0's tank reports no pill carried once its pill is removed;
--   * seat 0's builder never goes out on a "pill" job in the WATCH ticks;
--   * when seat 0's tank is killed at the end, the removed pill does not
--     come back onto the map.

scenario = {
  name        = "ROOST removed_carried_pill",
  description = "A carried pill that is removed is never placed and never dropped.",
  api         = 1,
}

local POS = { [0] = { 128, 128 }, [1] = { 132, 136 } }

local SETUP_AT  = 150
local REMOVE_AT = 200
local WATCH     = 3000

local gone_n, kept_n = nil, nil
local control_placed = false
local killed_at = nil
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for p = 0, 1 do
      game.builder_recall(p)
      game.teleport(p, POS[p][1], POS[p][2])
      game.set_stocks(p, { shells = 40, armour = 40, trees = 40 })
    end
    gone_n = game.add_pill(150, 150, game.NEUTRAL, 0)
    kept_n = game.add_pill(150, 106, game.NEUTRAL, 0)
    if type(gone_n) ~= "number" or type(kept_n) ~= "number" then
      finish("FAIL removed_carried_pill: the pills could not be placed")
      return
    end
    game.give_pill(0, gone_n)
    game.give_pill(1, kept_n)
    return
  end
  if not gone_n or t < SETUP_AT then return end

  if t == REMOVE_AT then
    local ok1, why1 = game.drop_pill(0, gone_n, 150, 150)
    local ok2, why2 = game.remove_pill(gone_n)
    if not ok1 or not ok2 then
      finish(string.format("FAIL removed_carried_pill: drop %s / remove %s",
                           tostring(why1 or ok1), tostring(why2 or ok2)))
      return
    end
    game.log(string.format("removed_carried_pill: pill %d removed from p0's tank at t=%d", gone_n, t))
    return
  end
  if t < REMOVE_AT then return end

  local since = t - REMOVE_AT
  if game.pill(gone_n) ~= nil then
    finish(string.format("FAIL removed_carried_pill: game.pill(%d) answers at +%d", gone_n, since))
    return
  end

  if killed_at then
    if t - killed_at >= 200 then
      finish(string.format("PASS removed_carried_pill: never placed, not dropped on death"))
    end
    return
  end

  if since % 1000 == 0 then
    for q = 0, 1 do
      local tq, bq = game.tank(q), game.builder(q)
      game.log(string.format("removed_carried_pill: t=%d p%d at (%d,%d) pills %d builder %s %s",
                             t, q, tq and tq.mx or -1, tq and tq.my or -1, tq and tq.pills or -1,
                             bq and bq.state or "?", bq and tostring(bq.job) or "?"))
    end
  end
  local tk = game.tank(0)
  if since >= 10 and tk and tk.pills ~= 0 then
    finish(string.format("FAIL removed_carried_pill: p0 still carries %d pill(s) at +%d", tk.pills, since))
    return
  end
  local b0 = game.builder(0)
  if b0 and b0.job == "pill" then
    finish(string.format("FAIL removed_carried_pill: p0 builder sent to place a pill at +%d (%d,%d)",
                         since, b0.mx, b0.my))
    return
  end
  local b1 = game.builder(1)
  if b1 and b1.job == "pill" then control_placed = true end

  if since >= WATCH then
    game.log(string.format("removed_carried_pill: p1 placed its own pill: %s", tostring(control_placed)))
    game.kill_tank(0)
    killed_at = t
  end
end
