-- ROOST: a pill removed while bots are shooting at it is let go at once.
--
-- The bots see this one go. The pill drops out of what the engine shows
-- them, so they have to stop shooting at its square and not come back to
-- it later from their own memory of it.
--
-- Both map bases are removed at setup, so the only thing on the island
-- worth a shell is a live neutral pill at (128,128). Seats 0 and 1 are put
-- ten and twelve squares from it and attack it on their own. Fifty ticks
-- after the first shell is spent the pill is removed.
--
-- Read: after a short grace for shells already in the air, no bot spends
-- a shell in the WATCH ticks that follow. With nothing left on the island
-- the bots roam, so a tank crossing the empty square proves nothing and is
-- not read.

scenario = {
  name        = "ROOST removed_pill_in_view",
  description = "A pill removed under fire is dropped and never shot at again.",
  api         = 1,
}

local PX, PY  = 128, 128
local POS = { [0] = { 138, 128 }, [1] = { 136, 136 } }

local SETUP_AT = 150
local FIRE_BY  = 1500     -- ticks after setup for somebody to start shooting
local HOLD     = 50       -- ticks of shooting before the pill is removed
local GRACE    = 100
local WATCH    = 1500

local pill_n, first_shot, gone_at = nil, nil, nil
local shells = {}
local done = false

local function finish(text)
  if done then return end
  done = true
  game.log("ROOST VERDICT " .. text)
  game.end_round(text)
end

local function note_shells()
  for q = 0, 1 do
    local tk = game.tank(q)
    if tk then shells[q] = tk.shells end
  end
end

function on_tick(t)
  if done then return end

  if t == SETUP_AT then
    for n = 1, game.num_bases() do
      if game.base(n) then game.remove_base(n) end
    end
    for p = 0, 1 do
      game.builder_recall(p)
      game.teleport(p, POS[p][1], POS[p][2])
      game.set_stocks(p, { shells = 40, armour = 40, trees = 20 })
    end
    pill_n = game.add_pill(PX, PY, game.NEUTRAL, 15)
    if type(pill_n) ~= "number" then
      finish("FAIL removed_pill_in_view: the pill could not be placed")
      return
    end
    note_shells()
    return
  end
  if not pill_n or t < SETUP_AT then return end

  if not first_shot then
    if (t - SETUP_AT) % 200 == 0 then
      for q = 0, 1 do
        local tk = game.tank(q)
        if tk then
          game.log(string.format("removed_pill_in_view: t=%d p%d at (%d,%d) shells %d",
                                 t, q, tk.mx, tk.my, tk.shells))
        end
      end
    end
    for q = 0, 1 do
      local tk = game.tank(q)
      if tk and tk.shells < shells[q] then first_shot = t end
    end
    note_shells()
    if not first_shot and t - SETUP_AT >= FIRE_BY then
      finish("FAIL removed_pill_in_view: nobody shot at the pill")
    end
    return
  end

  if not gone_at then
    if t - first_shot >= HOLD then
      game.remove_pill(pill_n)
      gone_at = t
      game.log(string.format("removed_pill_in_view: pill removed at t=%d", t))
    end
    note_shells()
    return
  end

  local since = t - gone_at
  if game.pill(pill_n) ~= nil then
    finish("FAIL removed_pill_in_view: game.pill still answers after the removal")
    return
  end
  for q = 0, 1 do
    local tk = game.tank(q)
    if since >= GRACE and tk and tk.shells < shells[q] then
      finish(string.format("FAIL removed_pill_in_view: p%d fired at +%d (%d->%d shells)",
                           q, since, shells[q], tk.shells))
      return
    end
  end
  note_shells()
  if since >= WATCH then
    finish(string.format("PASS removed_pill_in_view: no shot in %d ticks", WATCH))
  end
end
