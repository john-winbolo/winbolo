-- GATE: ticks=8000 bots=1 script=data/maps/tutorials/Tutorial.scenario.lua
--
-- Tutorial: the text is in the player's language (game.lang), an unknown
-- language is English, and a change of language mid-game shows on the next
-- line sent.
--
-- game.lang answers "en" on a dedicated server, so the arena replaces it
-- with one that answers what the arena sets (A.lang). The text compared
-- against is read straight from the script's TEXT tables.
--
-- Player 0 plays the tutorial's player, held still.
--   1. German from the start: the welcome popup and Station 1's panel
--      title are TEXT.de.
--   2. The language becomes "xx", which has no table. The arena drives the
--      tank into Station 2: the s2a popup, the panel title and the status
--      line are the English.
--   3. The language becomes "de" again, with no other change: the status
--      line and the panel title are the German at the next send.
--   4. The arena ticks off Station 2's goals: the s2c popup (a pattern with
--      the shot count filled in), the s2d popup and the "Station 2 done"
--      line are the German.
--
-- Before that, every language's text is checked against the limits the
-- client takes (popup 511 bytes a part, status line 128, announcement 125,
-- panel text 48 with no "\n"), with the widest numbers filled in.
--
-- PASS: every text above is the one expected, and no text is over a limit.

TUTORIAL_PLAYER = 0
ARENA = { phase = 0, lang = "de", pops = {}, status = {}, says = {},
          panels = {}, seen = {} }

game.lang = function(p) return ARENA.lang end

local real_popup = game.popup
game.popup = function(text, ...)
  local id
  for k, v in pairs(S.popped) do
    if v and not ARENA.seen[k] then ARENA.seen[k] = true; id = k end
  end
  -- The second part of a popup sent as two has no new id.
  local pops = ARENA.pops
  pops[#pops + 1] = { id = id or (pops[#pops] and pops[#pops].id), text = text }
  return real_popup(text, ...)
end

local real_status = game.status
game.status = function(text, ...)
  if text ~= "" then ARENA.status[#ARENA.status + 1] = text end
  return real_status(text, ...)
end

local real_announce = game.announce
game.announce = function(text, ...)
  ARENA.says[#ARENA.says + 1] = text
  return real_announce(text, ...)
end

local real_panel = game.panel
game.panel = function(id, list, ...)
  -- The title is the panel's second item: { "text", 64, 2, ..., title }.
  local t = list[2]
  if t ~= nil and t[1] == "text" then
    ARENA.panels[#ARENA.panels + 1] = t[7]
  end
  return real_panel(id, list, ...)
end

local arena_real_spawned = on_tank_spawned
function on_tank_spawned(p, mx, my, respawn, scripted)
  arena_real_spawned(p, mx, my, respawn, scripted)
  if p == 0 then game.set_modifiers(0, { speed = 1, turn = 1 }) end
end

-- The first part of a popup text (a table when written as two parts).
local function first(v)
  if type(v) == "table" then return v[1] end
  return v
end

local function check(fails, what, got, want)
  if got ~= want then
    fails[#fails + 1] = string.format("%s: got '%s' want '%s'", what,
      string.sub(tostring(got), 1, 24), string.sub(tostring(want), 1, 24))
  end
end

-- The first popup sent with that id, from index `from` on.
local function pop_text(id, from)
  for i = from or 1, #ARENA.pops do
    if ARENA.pops[i].id == id then return ARENA.pops[i].text end
  end
  return nil
end

local function said(text, from)
  for i = from or 1, #ARENA.says do
    if ARENA.says[i] == text then return true end
  end
  return false
end

-- Every language's text within the limits the client takes, with the
-- widest numbers filled in: popup 511 bytes, status line 128, announcement
-- 125 (with "top"), panel text 48 and no "\n". Answers the faults found.
local function check_limits()
  local LIMIT = { pop = 511, title = 48, list = 48, info = 48, hint = 48,
                  terrain = 48, step = 128, line = 128, say = 125 }
  local NUMS = { 99, 15 }
  local bad = {}
  for code, T in pairs(TEXT) do
    for section, lim in pairs(LIMIT) do
      for key, v in pairs(T[section] or {}) do
        local parts = type(v) == "table" and v or { v }
        if type(v) == "function" then parts = { v(99) } end
        for _, s in ipairs(parts) do
          local ok, out = pcall(string.format, s, NUMS[1], NUMS[2])
          if not ok then out = s end
          if section == "info" and (key == "on" or key == "on_plain") then
            -- The terrain line with this language's longest terrain word.
            local word = ""
            for _, w in pairs(T.terrain or TEXT.en.terrain) do
              if #w > #word then word = w end
            end
            ok, out = pcall(string.format, s, word, 255)
            if not ok then out = s end
          end
          if #out > lim or (lim == 48 and string.find(out, "\n")) then
            bad[#bad + 1] = code .. " " .. section .. "." .. tostring(key)
          end
        end
      end
    end
  end
  return bad
end

local arena_real_tick = on_tick
function on_tick(tick)
  arena_real_tick(tick)
  local A = ARENA
  if A.done then return end
  if tick % 10 == 0 and A.at ~= nil then
    local me = game.tank(0)
    if me ~= nil and not me.dead and
       (me.mx ~= A.at[1] or me.my ~= A.at[2]) then
      game.teleport(0, A.at[1], A.at[2], 0)
    end
  end
  A.fails = A.fails or {}
  local fails = A.fails
  if A.phase == 0 and tick >= 300 then
    if TEXT.de == nil then
      A.done = true
      verdict(false, "the script has no TEXT.de")
      return
    end
    local over = check_limits()
    if #over > 0 then
      fails[#fails + 1] = #over .. " over a limit: " ..
        table.concat(over, ", ", 1, math.min(#over, 3))
    end
    local langs = 0
    for _ in pairs(TEXT) do langs = langs + 1 end
    A.langs = langs
    check(fails, "de welcome", pop_text("welcome"), first(TEXT.de.pop.welcome))
    check(fails, "de title 1", A.panels[#A.panels], TEXT.de.title[1])
    -- 2: an unknown language, then into Station 2.
    A.lang = "xx"
    A.pop_at = #A.pops + 1
    A.at = { 127, B.b2a.y - 4 }
    game.teleport(0, A.at[1], A.at[2], 0)
    A.phase, A.t = 1, tick
  elseif A.phase == 1 and tick >= A.t + 300 then
    A.at = { B.b2a.x, B.b2a.y }
    A.phase, A.t = 2, tick
  elseif A.phase == 2 and tick >= A.t + 600 then
    check(fails, "xx s2a", pop_text("s2a", A.pop_at), TEXT.en.pop.s2a)
    check(fails, "xx title 2", A.panels[#A.panels], TEXT.en.title[2])
    A.en_status = A.status[#A.status]
    -- 3: back to German, nothing else changed.
    A.lang = "de"
    A.status_at = #A.status + 1
    A.phase, A.t = 3, tick
  elseif A.phase == 3 and tick >= A.t + 300 then
    check(fails, "de title 2", A.panels[#A.panels], TEXT.de.title[2])
    local en_line = A.en_status
    local de_line = A.status[#A.status]
    if en_line == nil or de_line == nil or A.status_at > #A.status then
      fails[#fails + 1] = "no status line before or after the change"
    else
      -- The same goal's line, in each language.
      local key
      for k, v in pairs(TEXT.en.step) do
        if v == en_line then key = k end
      end
      if key == nil then
        fails[#fails + 1] = "xx status not English: '" ..
          string.sub(en_line, 1, 24) .. "'"
      else
        check(fails, "de step " .. key, de_line,
              TEXT.de.step[key] or TEXT.en.step[key])
      end
    end
    -- 4: Station 2's goals, in German.
    A.pop_at = #A.pops + 1
    A.say_at = #A.says + 1
    A.at = { 127, B.b2a.y - 4 }
    mark(2, "a_refill")
    mark(2, "b_take")
    A.phase, A.t = 4, tick
  elseif A.phase == 4 and tick >= A.t + 600 then
    mark(2, "c_shoot")
    mark(2, "c_take")
    A.phase, A.t = 5, tick
  elseif A.phase == 5 and tick >= A.t + 900 then
    A.done = true
    local s2c = TEXT.de.pop.s2c
    local shots = POP_ARGS.s2c()
    check(fails, "de s2c", pop_text("s2c", A.pop_at),
          string.format(first(s2c), shots))
    check(fails, "de s2d", pop_text("s2d", A.pop_at), first(TEXT.de.pop.s2d))
    local done = string.format(TEXT.de.say.done, 2)
    if not said(done, A.say_at) then
      fails[#fails + 1] = "no '" .. string.sub(done, 1, 24) .. "'"
    end
    verdict(#fails == 0, #fails == 0 and string.format(
      "%d languages in limits; %d popups, %d lines, %d panels in de, xx, de",
      A.langs, #A.pops, #A.status, #A.panels) or table.concat(fails, "; "))
  end
end
