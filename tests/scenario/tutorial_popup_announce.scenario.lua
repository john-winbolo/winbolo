-- GATE: ticks=2000 bots=1 script=data/maps/Tutorial.scenario.lua
--
-- Tutorial: a goal that finishes a station while a popup is waiting sends
-- the popup and the "Station N done" announcement in the same tick. That
-- is the case the client's announcement hold is for (item 26): the client
-- does not draw the line while the popup is open and starts its time when
-- the popup closes. The gate runs the dedicated server only, with no client
-- to draw anything, so the hold itself is proven in the unit suite
-- (scenario_announce_waits_for_popup, tests/unit/test_scenario_popup.c).
-- This arena proves the script really sends the pair in one tick, so the
-- hold is needed and the unit test covers a case the tutorial makes.
--
-- Seat 0 plays the tutorial's player. At tick 300 the arena ticks off every
-- Station 2 goal but the last, queues the s2b popup, and ticks the last.
--
-- PASS: the s2b popup and "Station 2 done. ..." both went to the player,
-- in the same tick.

TUTORIAL_PLAYER = 0
ARENA = { popups = {}, announces = {} }

local real_popup = game.popup
game.popup = function(text, ...)
  ARENA.popups[#ARENA.popups + 1] = { tick = game.tick(), text = text }
  return real_popup(text, ...)
end

local real_announce = game.announce
game.announce = function(text, ticks, who, ...)
  ARENA.announces[#ARENA.announces + 1] =
    { tick = game.tick(), text = text, who = who }
  return real_announce(text, ticks, who, ...)
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  if A.t == nil and tick >= 300 then
    local items = LIST[2]
    for i = 1, #items - 1 do mark(2, items[i][1]) end
    A.popups, A.announces = {}, {}
    popup_later("s2b")
    mark(2, items[#items][1])
    A.t = tick
    return
  end
  if A.t ~= nil and tick >= A.t + 10 then
    A.done = true
    local pop, ann
    for _, p in ipairs(A.popups) do
      if p.text == POP.s2b or (type(POP.s2b) == "function") then pop = p end
    end
    for _, a in ipairs(A.announces) do
      if type(a.text) == "string" and
         string.find(a.text, "Station 2 done", 1, true) then ann = a end
    end
    verdict(pop ~= nil and ann ~= nil and pop.tick == ann.tick and
            ann.who == S.player, string.format(
      "popup at %s, 'Station 2 done' at %s (to %s, player %s)",
      tostring(pop and pop.tick), tostring(ann and ann.tick),
      tostring(ann and ann.who), tostring(S.player)))
  end
end
