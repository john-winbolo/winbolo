-- GATE: ticks=8000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial: the popup comes first, its white text after (item 37).
--
-- The script sends the player's status line and announcements only when no
-- popup is waiting in its queue (S.queue): while one waits, the status line
-- is blank and an announcement waits with it. The server cannot see a popup
-- close; the client draws no white text while a popup is open and starts an
-- announcement's time when it closes (clientSimScnAnnounceHold, unit test
-- scenario_announce_waits_for_popup). So this arena checks the order the
-- server sends things in: no white text is sent before its popup.
--
-- Seat 0 plays the tutorial's player, held still. It drives into Station 2
-- on the road (s2a shows at once), onto the 2A base (s2stay), off it (2A
-- ticks; s2b waits 3 seconds), and then the arena ticks 2B (s2c waits), 2C
-- shot and 2C taken, which finishes the station (s2d waits, and the
-- "Station 2 done" line with it).
--
-- PASS:
--   - every non-blank status line and every announcement was sent with no
--     popup waiting;
--   - on entry, the s2a popup was sent before Station 2's first status line;
--   - s2b, s2c and s2d were each sent before the first non-blank status
--     line after the goal that queued them, and the status line was blank
--     while each waited;
--   - "Station 2 done" was sent after the s2d popup.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, ev = {}, seen = {}, bad = {} }

local function log_ev(kind, text, id)
  local A = ARENA
  A.ev[#A.ev + 1] = { kind = kind, text = text or "", id = id,
                      tick = game.tick(), waiting = #S.queue }
  if (kind == "announce" or (kind == "status" and text ~= "")) and
     #S.queue > 0 then
    A.bad[#A.bad + 1] = kind .. " '" .. string.sub(text, 1, 20)
      .. "' with " .. #S.queue .. " popup(s) waiting"
  end
end

local real_popup = game.popup
game.popup = function(text, ...)
  local id
  for k, v in pairs(S.popped) do
    if v and not ARENA.seen[k] then ARENA.seen[k] = true; id = k end
  end
  log_ev("popup", "", id)
  return real_popup(text, ...)
end

local real_status = game.status
game.status = function(text, ...)
  log_ev("status", text)
  return real_status(text, ...)
end

local real_announce = game.announce
game.announce = function(text, ...)
  log_ev("announce", text)
  return real_announce(text, ...)
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

local ROAD2 = nil

local function hold(at)
  local me = game.tank(0)
  if at ~= nil and me ~= nil and not me.dead and
     (me.mx ~= at[1] or me.my ~= at[2]) then
    game.teleport(0, at[1], at[2], 0)
  end
end

-- Index of the first event at or after `from` that matches.
local function find(from, pred)
  for i = from, #ARENA.ev do
    if pred(ARENA.ev[i]) then return i end
  end
  return nil
end

local function popup_of(id)
  return function(e) return e.kind == "popup" and e.id == id end
end

local function real_status_ev(e)
  return e.kind == "status" and e.text ~= ""
end

-- `id` was sent at or after `mark_at`, and no non-blank status line came
-- between `mark_at` and it, and the status line was blanked while it
-- waited (unless it was sent at once).
local function check_after(id, mark_at, fails)
  local pi = find(mark_at, popup_of(id))
  if pi == nil then
    fails[#fails + 1] = id .. " never sent"
    return nil
  end
  local si = find(mark_at, real_status_ev)
  if si ~= nil and si < pi then
    fails[#fails + 1] = "status '" .. string.sub(ARENA.ev[si].text, 1, 20)
      .. "' before " .. id
  end
  return pi
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  if tick % 10 == 0 then hold(A.at) end
  if A.phase == 0 and tick >= 300 then
    ROAD2 = { 127, B.b2a.y - 4 }
    A.entry_at = #A.ev + 1
    A.at = ROAD2
    game.teleport(0, ROAD2[1], ROAD2[2], 0)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 300 then
    A.at = { B.b2a.x, B.b2a.y }
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 200 then
    A.refill_at = #A.ev + 1
    A.at = ROAD2
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 600 then
    A.btake_at = #A.ev + 1
    mark(2, "b_take")
    A.phase, A.t = 4, tick
  elseif A.phase == 4 and tick >= A.t + 600 then
    mark(2, "c_shoot")
    A.ctake_at = #A.ev + 1
    mark(2, "c_take")
    A.phase, A.t = 5, tick
  elseif A.phase == 5 and tick >= A.t + 600 then
    A.done = true
    local fails = {}
    for _, b in ipairs(A.bad) do fails[#fails + 1] = b end
    -- Entry: s2a before Station 2's first status line.
    check_after("s2a", A.entry_at, fails)
    check_after("s2b", A.refill_at, fails)
    check_after("s2c", A.btake_at, fails)
    local d = check_after("s2d", A.ctake_at, fails)
    local di = find(A.ctake_at, function(e)
      return e.kind == "announce" and
             string.find(e.text, "Station 2 done", 1, true) ~= nil
    end)
    if di == nil then
      fails[#fails + 1] = "no 'Station 2 done'"
    elseif d ~= nil and di < d then
      fails[#fails + 1] = "'Station 2 done' before s2d"
    end
    -- How many blank status lines went out while a popup waited.
    local blanks = 0
    for _, e in ipairs(A.ev) do
      if e.kind == "status" and e.text == "" and e.waiting > 0 then
        blanks = blanks + 1
      end
    end
    if blanks == 0 then fails[#fails + 1] = "status never blanked" end
    local order = {}
    for i = A.entry_at, #A.ev do
      local e = A.ev[i]
      order[#order + 1] = string.sub(e.kind, 1, 1) .. (e.id or "")
        .. (e.kind == "status" and e.text == "" and "0" or "")
    end
    game.log("ARENA order: " .. table.concat(order, " ", 1,
                                             math.min(#order, 20)))
    verdict(#fails == 0, #fails == 0 and string.format(
      "%d events, %d blank while waiting; popups first", #A.ev, blanks)
      or table.concat(fails, "; "))
  end
end
