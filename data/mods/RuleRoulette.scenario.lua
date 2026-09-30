-- =========================================================================
-- Rule Roulette — a mod that changes every tank's numbers on a clock, one
-- named mode at a time.
--
-- There are eight modes. Each one is a fixed set of the six tank modifiers
-- (speed, accel, turn, reload, dealt and taken, as percentages of the
-- classic tank), the same for every tank:
--
--   Good for everyone: Overdrive, Turbo, Iron Hide.
--   Bad for everyone:  Rust Bucket.
--   A trade-off:       Glass Cannon, Juggernaut, Machine Gun, Ice Rink.
--
-- Every `interval` seconds the next mode starts. The order is a shuffle of
-- the eight from the scenario's own math.random, so a seed and a replay give
-- the same order. When all eight have played they are shuffled again, and a
-- mode never follows itself across that seam. A short countdown names the
-- mode that is coming; the new mode goes up on the centre of the screen and
-- on the newswire, and panel 0 shows it, what it means, the numbers it
-- changes, the time left and the modes that come next.
--
-- It exists to play-test bots that read their own modifiers: the numbers
-- change under them every minute, so a bot that plans with the classic
-- values shows it quickly.
--
-- Reload is the time between shots, so 160 is a gun that fires less often.
-- Taken prices every blow the tank takes, so 160 is armour that gives way
-- sooner. For those two a higher number is worse; for the other four it is
-- better.
--
-- The host sets the interval, how many upcoming modes the panel shows, the
-- countdown and the chat commands in the lobby (scenario.settings below).
-- With chat commands on, a human can change the interval and the preview
-- and skip to the next mode during a round; type "!roulette" for the list.
--
-- kind = "mod": the win condition is left to the map and the lobby.
-- bound = false: it names no square, pill or base, so it plays on any map.
-- =========================================================================

-- ── The modes ───────────────────────────────────────────────────────

-- `kind` is "good" (every number better), "bad" (every number worse) or
-- "trade" (some better, some worse). `mean` is what the mode feels like, in
-- one line the panel can show whole. A modifier left out is 100.
local MODES = {
  { name = "Overdrive",    kind = "good",  mean = "Faster, tougher, hits harder.",
    mods = { speed = 150, accel = 160, turn = 140, reload = 60, dealt = 150, taken = 60 } },
  { name = "Turbo",        kind = "good",  mean = "Pure speed: fast and nimble.",
    mods = { speed = 170, accel = 180, turn = 150 } },
  { name = "Iron Hide",    kind = "good",  mean = "Tough as nails, quick reload.",
    mods = { taken = 40, reload = 80 } },
  { name = "Rust Bucket",  kind = "bad",   mean = "Slow, fragile, weak shots.",
    mods = { speed = 60, accel = 50, turn = 70, reload = 160, dealt = 70, taken = 150 } },
  { name = "Glass Cannon", kind = "trade", mean = "Hit hard, break fast.",
    mods = { dealt = 250, taken = 250 } },
  { name = "Juggernaut",   kind = "trade", mean = "Slow and very tough.",
    mods = { speed = 65, accel = 60, reload = 130, taken = 40 } },
  { name = "Machine Gun",  kind = "trade", mean = "Rapid fire, weak shells.",
    mods = { reload = 35, dealt = 45 } },
  { name = "Ice Rink",     kind = "trade", mean = "Hard to stop, hard to steer.",
    mods = { speed = 140, accel = 35, turn = 55 } },
}

local KIND_TAG    = { good = "GOOD FOR ALL", bad = "BAD FOR ALL", trade = "TRADE-OFF" }
local KIND_COLOUR = { good = "green", bad = "red", trade = "yellow" }

local MOD_KEYS   = { "speed", "accel", "turn", "reload", "dealt", "taken" }
local MOD_LABEL  = { speed = "Speed", accel = "Accel", turn = "Turn",
                     reload = "Reload", dealt = "Dealt (Tanks)", taken = "Taken" }
-- Labels too wide for half the panel; each takes a whole line.
local WIDE_LABEL = { dealt = true }
-- For these a higher percentage hurts the tank that has it.
local WORSE_HIGH = { reload = true, taken = true }

local INTERVAL_MIN = 20
local INTERVAL_MAX = 300
local PREVIEW_MAX  = 3     -- the most upcoming modes the panel has room for

-- ── State ───────────────────────────────────────────────────────────

local interval  = 60       -- the lobby settings, read in on_start
local preview   = 1
local countdown = 5
local chat      = false

local running  = false
local over     = false
local changes  = 0          -- how many modes this round has had
local left     = 0          -- seconds to the next mode
local next_at  = 0          -- game.tick() of the next mode, for the panel clock
local mode_len = 60         -- seconds the mode in force was given, the bar's full
local mode     = nil        -- the MODES entry in force
local queue    = {}         -- MODES indices still to come, in order
local last_idx = nil        -- the index of the mode in force

-- ── Small helpers ───────────────────────────────────────────────────

-- A seat with a tank on the field. A held seat has none, and
-- set_modifiers needs one.
local function in_round(p)
  return game.tank(p) ~= nil
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- The full six-field table set_modifiers takes: a missing one is 100.
local function full_mods(m)
  local t = {}
  for _, k in ipairs(MOD_KEYS) do
    t[k] = m.mods[k] or 100
  end
  return t
end

-- Kept short: a log line over 128 bytes is refused.
local function mods_text(t)
  return string.format("s%d a%d t%d r%d d%d k%d",
                       t.speed, t.accel, t.turn, t.reload, t.dealt, t.taken)
end

-- "Now: Glass Cannon - hit hard, break fast."
local function now_text(m)
  return "Now: " .. m.name .. " - " .. m.mean:sub(1, 1):lower() .. m.mean:sub(2)
end

-- ── The order ───────────────────────────────────────────────────────

-- Adds one shuffled round of all eight to the queue. The first of the new
-- round is never the mode just before it, so no mode plays twice in a row.
local function add_round()
  local bag = {}
  for i = 1, #MODES do
    bag[i] = i
  end
  for i = #bag, 2, -1 do
    local j = math.random(i)
    bag[i], bag[j] = bag[j], bag[i]
  end
  local before = queue[#queue] or last_idx
  if bag[1] == before then
    local j = math.random(2, #bag)
    bag[1], bag[j] = bag[j], bag[1]
  end
  for _, i in ipairs(bag) do
    queue[#queue + 1] = i
  end
end

-- Keeps enough modes queued for the panel to show `preview` of them.
local function fill_queue()
  while #queue < PREVIEW_MAX + 1 do
    add_round()
  end
end

-- ── Modifiers ───────────────────────────────────────────────────────

local function apply_mods(p, t)
  local ok, code, why = game.set_modifiers(p, t)
  if not ok then
    game.log(string.format("RuleRoulette: set_modifiers(%d) refused: %s %s",
                           p, tostring(code), tostring(why)))
  end
end

-- Whether a tank already carries these numbers. A respawn keeps them, and
-- this saves the write when it has.
local function has_mods(p, t)
  local tk = game.tank(p)
  if tk == nil or tk.mods == nil then
    return false
  end
  for _, k in ipairs(MOD_KEYS) do
    if tk.mods[k] ~= t[k] then
      return false
    end
  end
  return true
end

-- ── Panel ───────────────────────────────────────────────────────────
--
-- 128 x 128 units. Text heights are the frontend's: "small" is 8 units and
-- "normal" 11, in Inter. The widest strings below, measured from that font:
-- a mode name in normal, "Glass Cannon", 60 units; a tag in small, "GOOD FOR
-- ALL", 49; a meaning in small, "Tough as nails, quick reload.", 90; a number
-- cell, "Reload 160%", 41. So the header, a meaning on one line and two
-- number cells to a row all fit with room to spare.
--
--   y   0 .. 15  band: mode name (normal, left), its tag (small, right)
--   y  17        meaning (small)
--   y  27 ..     numbers that are not 100, two to a row, up to 3 rows
--   then         "Next mode in" and the clock
--   then         a bar, 4 units tall, filling as this mode runs
--   then         a rule, and per upcoming mode its name and its meaning
--
-- With all six numbers changed and three upcoming modes, the last line
-- starts at y 119 and ends at 127.
--
-- The clock counts itself down on the client, but a bar is drawn at the
-- value it is sent, so each_second sends the panel again once a second.

local function tint(pct, worse_high)
  local better = (pct > 100) ~= (worse_high == true)
  return better and "green" or "red"
end

local function build_panel()
  local list = {
    { "rect", 0, 0, 128, 15, "grey_dark", true },
    { "text", 3, 2, KIND_COLOUR[mode.kind], "normal", "left", mode.name },
    { "text", 125, 4, KIND_COLOUR[mode.kind], "small", "right", KIND_TAG[mode.kind] },
    { "text", 3, 17, "white", "small", "left", mode.mean },
  }
  local y, col = 27, 0
  for _, k in ipairs(MOD_KEYS) do
    local v = mode.mods[k]
    if v ~= nil and v ~= 100 then
      if WIDE_LABEL[k] and col ~= 0 then
        col, y = 0, y + 9
      end
      list[#list + 1] = { "text", col == 0 and 3 or 65, y, tint(v, WORSE_HIGH[k]),
                          "small", "left", MOD_LABEL[k] .. " " .. v .. "%" }
      col = WIDE_LABEL[k] and 2 or col + 1
      if col == 2 then
        col, y = 0, y + 9
      end
    end
  end
  if col ~= 0 then
    y = y + 9
  end
  y = y + 2
  list[#list + 1] = { "text", 3, y, "grey", "small", "left", "Next mode in" }
  list[#list + 1] = { "timer", 125, y, "yellow", "small", "right", "down", next_at }
  y = y + 9
  -- Fills as the mode runs, in step with the clock above it. The clock
  -- floors its ticks, so from one tick after this draw it reads one second
  -- less than the ticks left now. Over a mode it shows mode_len - 1 down to
  -- 0, so the bar fills over those mode_len - 1 steps: empty while the
  -- clock shows the mode's first second, full in its last.
  local secs = clamp(math.floor((next_at - game.tick() - 1) / 100), 0, mode_len - 1)
  list[#list + 1] = { "bar", 3, y, 122, 4, "yellow", mode_len - 1 - secs, mode_len - 1 }
  -- A bar outlines itself in its own colour. An outlined rect at the same
  -- operands strokes the same pixels, so drawn after the bar it gives the
  -- bar a white border that the yellow fill stands out against.
  list[#list + 1] = { "rect", 3, y, 122, 4, "white", false }
  y = y + 7
  if preview > 0 then
    list[#list + 1] = { "line", 3, y, 124, y, "grey_dark" }
    y = y + 2
    for i = 1, preview do
      -- A mode with every number and a whole-line label leaves room for
      -- fewer upcoming modes; drop the ones that would run off the panel.
      if y + 17 > 128 then break end
      local m = MODES[queue[i]]
      list[#list + 1] = { "text", 3, y, KIND_COLOUR[m.kind], "small", "left",
                          (i == 1 and "Next: " or "Then: ") .. m.name }
      list[#list + 1] = { "text", 9, y + 9, "grey", "small", "left", m.mean }
      y = y + 18
    end
  end
  return list
end

-- The lowest unit a list draws on, for the log: text is 8 or 11 units tall.
-- A rect or a bar is { name, x, y, w, h, ... }, a line { name, x0, y0, x1, y1 }.
local function panel_bottom(list)
  local bottom = 0
  for _, it in ipairs(list) do
    local b
    if it[1] == "rect" or it[1] == "bar" then
      b = it[3] + it[5]
    elseif it[1] == "line" then
      b = math.max(it[3], it[5]) + 1
    else
      b = it[3] + (it[5] == "normal" and 11 or 8)
    end
    bottom = math.max(bottom, b)
  end
  return bottom
end

-- Sends the panel. A refusal (two updates in one tick, say) is logged, and
-- the next second's redraw in each_second sends it again.
local function draw_panel(log_it)
  if mode == nil then
    return
  end
  local list = build_panel()
  local ok, code, why = game.panel(0, list)
  if not ok then
    game.log(string.format("RuleRoulette: panel refused: %s %s",
                           tostring(code), tostring(why)))
  end
  if log_it then
    game.log(string.format("RuleRoulette panel: %d items, bottom %d of 128, preview %d",
                           #list, panel_bottom(list), preview))
  end
end

-- ── Changing mode ───────────────────────────────────────────────────

local function start_mode(idx)
  changes  = changes + 1
  mode     = MODES[idx]
  last_idx = idx
  mode_len = interval
  next_at  = game.tick() + interval * 100

  local t = full_mods(mode)
  local tanks, landed = 0, 0
  for p = 0, game.max_tanks() - 1 do
    if in_round(p) then
      apply_mods(p, t)
      tanks = tanks + 1
      -- Read back, so the log says the numbers reached the tanks.
      if has_mods(p, t) then
        landed = landed + 1
      end
    end
  end
  game.announce(now_text(mode), 5)
  game.message("Rule Roulette: " .. now_text(mode))
  game.log(string.format("RuleRoulette mode %d tick %d: %s (%s) on %d of %d tanks",
                         changes, game.tick(), mode.name, mods_text(t), landed, tanks))
  fill_queue()
  draw_panel(true)
end

local function next_mode()
  fill_queue()
  local idx = table.remove(queue, 1)
  start_mode(idx)
end

local function each_second()
  if over then
    return
  end
  left = left - 1
  if left <= 0 then
    next_mode()   -- draws the panel itself
    left = interval
  else
    if left <= countdown then
      game.announce(MODES[queue[1]].name .. " in " .. left, 1)
    end
    draw_panel(false)   -- the bar's new value, and a retry of a refused one
  end
  game.timer(1, each_second)
end

-- ── Chat commands ───────────────────────────────────────────────────

local function status_text()
  return string.format("Rule Roulette: %s, next mode in %d s, interval %d s, preview %d",
                       mode and mode.name or "-", left, interval, preview)
end

local function command(p, args)
  local verb, value = args:match("^(%S*)%s*(%S*)")
  verb = (verb or ""):lower()
  -- Digits only: tonumber would also take "nan" and "inf", and a NaN
  -- interval stops the clock.
  local n = value:match("^%d+$") and tonumber(value) or nil

  if verb == "interval" and n then
    interval = clamp(math.floor(n), INTERVAL_MIN, INTERVAL_MAX)
    game.message(status_text() .. " (the interval counts from the next mode)")
  elseif verb == "preview" and n then
    preview = clamp(math.floor(n), 0, PREVIEW_MAX)
    draw_panel(true)
    game.message(status_text())
  elseif verb == "next" then
    left    = 1   -- the next second changes mode
    next_at = game.tick() + 100
    draw_panel(false)
  else
    game.message(status_text(), p)
    game.message("Type !roulette interval <20-300> | preview <0-3> | next", p)
  end
end

-- ── Hooks ───────────────────────────────────────────────────────────

function on_start()
  interval  = clamp(game.setting("interval"), INTERVAL_MIN, INTERVAL_MAX)
  preview   = clamp(game.setting("preview"), 0, PREVIEW_MAX)
  countdown = game.setting("countdown")
  chat      = game.setting("chat")
  running   = true
  left      = interval
  game.message("Rule Roulette: a new mode every " .. interval .. " s." ..
               (chat and " Type !roulette for the commands." or ""))
  next_mode()
  game.timer(1, each_second)
end

function on_end()
  over = true
  game.log(string.format("RuleRoulette ended after %d modes", changes))
end

-- A tank that takes the field gets the mode in force.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if not running or over or mode == nil then
    return
  end
  local t = full_mods(mode)
  if not has_mods(p, t) then
    apply_mods(p, t)
  end
end

function on_chat(p, text, scripted)
  if not chat or scripted or not running then
    return
  end
  local args = text:match("^!roulette%s*(.-)%s*$")
  if args == nil then
    return
  end
  local slot = game.lobby_slot(p)
  if slot == nil or slot.bot then
    return
  end
  command(p, args)
end

scenario = {
  name        = "Rule Roulette",
  description = "Eight named modes take turns, the same for every tank: good ones " ..
                "(Overdrive, Turbo, Iron Hide), a bad one (Rust Bucket) and " ..
                "trade-offs (Glass Cannon, Juggernaut, Machine Gun, Ice Rink).",
  api         = 1,
  kind        = "mod",
  bound       = false,

  settings = {
    { id = "interval", label = "Seconds per mode", type = "int",
      min = 20, max = 300, step = 10, default = 60 },
    { id = "preview", label = "Upcoming modes shown", type = "int",
      min = 0, max = 3, step = 1, default = 1 },
    { id = "countdown", label = "Countdown before a change (seconds)", type = "int",
      min = 0, max = 10, step = 1, default = 5 },
    { id = "chat", label = "!roulette chat commands", type = "bool",
      default = false },
  },

  callbacks = {
    on_start = "Starts the first mode and the clock.",
    on_end = "Logs how many modes the round had.",
    on_tank_spawned = "A new or respawned tank gets the mode in force.",
    on_chat = "With chat commands on, !roulette changes the interval and the " ..
              "preview, or skips to the next mode.",
  },
}
