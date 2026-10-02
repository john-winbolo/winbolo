-- =========================================================================
-- Rule Roulette — a mod that changes every tank's numbers, or the builder's,
-- on a clock, one named mode at a time.
--
-- There are fourteen modes, each the same for every tank. Eight are a fixed
-- set of the six tank modifiers (speed, accel, turn, reload, dealt and
-- taken, as percentages of the classic tank). Four leave the tank alone and
-- change the builder's rules instead, and one, Hovercraft, changes the
-- tank's terrain rules (see "Rule modes" below). One, Normal, changes
-- nothing: classic Bolo between the others.
--
--   Good for everyone: Overdrive, Turbo, Iron Hide, Cleanup Crew, Hustle,
--                      Air Drop.
--   Bad for everyone:  Rust Bucket, Lead Feet.
--   A trade-off:       Glass Cannon, Juggernaut, Machine Gun, Ice Rink,
--                      Hovercraft.
--   No change:         Normal.
--
-- Glass Cannon's shells also take twice the armour off a pillbox (the
-- pill_damage_scale policy below). Dealt on its own prices only the blows
-- a tank takes.
--
-- Every `interval` seconds the next mode starts. The order is a shuffle of
-- all fourteen from the scenario's own math.random, so a seed and a replay
-- give the same order. When all fourteen have played they are shuffled
-- again, and a
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
-- Rule modes. Cleanup Crew makes a road cost the builder no trees. Hustle
-- and Lead Feet make him walk at 125% and 75% of his speed on every
-- terrain. Air Drop makes a dead builder's flight back in 67% faster.
-- Hovercraft drives every square but forest at the grass speed (so roads
-- are slower, and water, swamp and rubble much faster), keeps the water
-- from washing shells and mines out of a tank, and lets a tank with no boat
-- drive over deep sea. A tank still out on deep sea with no boat when the
-- mode ends drowns on the next tick, as it would have driving in. Each
-- reads the rules it changes with game.rule when it starts, so a value a
-- host or another script set is the one it works from, and puts back exactly
-- that value when it ends: at the next mode, and at the end of the round.
-- A rule somebody else changed while the mode was in force is left at their
-- value. The rules table opens every round on the classic numbers (or the
-- scenario's own), so a script switched off mid-round leaves nothing behind
-- past that round.
--
-- For the whole round, bases also rebuild their own stock 50% faster than
-- the classic rate (scenario.rules below). No mode touches that rule.
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

-- `kind` is "good" (every number better), "bad" (every number worse),
-- "trade" (some better, some worse) or "normal" (nothing changed). `mean` is
-- what the mode feels like, in one line the panel can show whole. A
-- modifier left out is 100.
--
-- pill_dealt = pct  prices what a tank's shell takes off a pillbox, as a
--           percent of the classic amount (pill_damage_scale below). Left
--           out, it is 100.
--
-- A mode may also change rules, alone or beside its modifiers:
--   rules = { name = value }  sets each rule to that value. A value may be
--           a function of the rule's old value that answers the new one.
--   scale = { pct, rules, max, max_rule }  sets each rule in the list to pct
--           percent of the value it had, rounded to the nearest whole
--           number. A rule at 0 stays 0, and one above 0 stays at least 1,
--           so a terrain the builder cannot walk stays shut and one he can
--           stays open. `max` is the rule's own top; `max_rule` names a rule
--           whose value is a top as well.
--   show = "..."  the panel's own line, in place of number rows.
--           A "%d" in it is the percentage the first scaled rule really got,
--           after rounding.

-- The builder's walking speed on each terrain (bolo_map.c mapGetManSpeed):
-- the distance he moves each tick, not a cap. 0 to 63 each.
local MAN_SPEED_RULES = {
  "man_speed_road", "man_speed_grass", "man_speed_forest", "man_speed_river",
  "man_speed_swamp", "man_speed_crater", "man_speed_rubble", "man_speed_boat",
  "man_speed_deep_sea", "man_speed_refuel_base",
}

-- The fastest a tank may drive on each terrain (bolo_map.c mapGetSpeed),
-- all but forest and grass itself. Walls, half walls and live pillboxes
-- have no rule: the collision test stops a tank there, not its speed.
local HOVER_SPEED_RULES = {
  "speed_road", "speed_river", "speed_swamp", "speed_crater", "speed_rubble",
  "speed_boat", "speed_deep_sea", "speed_refuel_base",
}

-- Hovercraft's rules: each terrain above at the grass speed in force when
-- the mode starts, no shells or mines lost wading, and deep sea safe
-- without a boat.
local function grass_speed(was)
  return game.rule("speed_grass") or was
end
local HOVER_RULES = { water_loss_shells = 0, water_loss_mines = 0,
                      tank_deep_sea_safe = 1 }
for _, name in ipairs(HOVER_SPEED_RULES) do
  HOVER_RULES[name] = grass_speed
end

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
    mods = { dealt = 250, taken = 250 }, pill_dealt = 200 },
  { name = "Juggernaut",   kind = "trade", mean = "Slow and very tough.",
    mods = { speed = 65, accel = 60, reload = 130, taken = 40 } },
  { name = "Machine Gun",  kind = "trade", mean = "Rapid fire, weak shells.",
    mods = { reload = 35, dealt = 45 } },
  { name = "Ice Rink",     kind = "trade", mean = "Hard to stop, hard to steer.",
    mods = { speed = 140, accel = 35, turn = 55 } },
  -- Fixing up the rubble all the explosions leave: a road costs no trees.
  -- Walls, pillboxes, boats and mines keep their price.
  { name = "Cleanup Crew", kind = "good",  mean = "Free roads: pave over the rubble.",
    rules = { lgm_cost_road = 0 }, show = "Roads: free" },
  { name = "Hustle",       kind = "good",  mean = "Builders sprint to every job.",
    scale = { pct = 125, rules = MAN_SPEED_RULES, max = 63 },
    show = "Builder: %d%% speed" },
  { name = "Lead Feet",    kind = "bad",   mean = "Builders trudge to every job.",
    scale = { pct = 75, rules = MAN_SPEED_RULES, max = 63 },
    show = "Builder: %d%% speed" },
  -- A dead builder flies from a random start straight back to where his
  -- tank was, lgm_helicopter_speed a tick, then walks the rest. He lands
  -- when a step ends inside lgm_arrive_tolerance on both axes, so the speed
  -- is kept at or under that tolerance: a longer step could fly past it.
  -- Classic 3 is 5.01 at 167%, which rounds to 5.
  { name = "Air Drop",     kind = "good",  mean = "Dead builders fly back fast.",
    scale = { pct = 167, rules = { "lgm_helicopter_speed" }, max = 255,
              max_rule = "lgm_arrive_tolerance" },
    show = "Parachute: %d%% speed" },
  -- Every square but forest drives like grass, the water takes nothing,
  -- and deep sea holds a tank up with no boat. Roads get slower, so it is a
  -- trade-off.
  { name = "Hovercraft",   kind = "trade", mean = "Hover over water and roads alike.",
    rules = HOVER_RULES, show = "Grass speed; deep sea safe" },
  -- Classic Bolo: every modifier 100 and no rule changed. The last mode's
  -- rules go back as they do at every change.
  { name = "Normal",       kind = "normal", mean = "Plain Bolo: nothing changed.",
    show = "Everything classic" },
}

local KIND_TAG    = { good = "GOOD FOR ALL", bad = "BAD FOR ALL", trade = "TRADE-OFF",
                      normal = "NO CHANGES" }
local KIND_COLOUR = { good = "green", bad = "red", trade = "yellow", normal = "white" }

local MOD_KEYS   = { "speed", "accel", "turn", "reload", "dealt", "taken" }
-- The panel's number rows: the six modifiers, with pill_dealt beside dealt.
local ROW_KEYS   = { "speed", "accel", "turn", "reload", "dealt", "pill_dealt", "taken" }
local MOD_LABEL  = { speed = "Speed", accel = "Accel", turn = "Turn",
                     reload = "Reload", dealt = "Dealt (Tanks)",
                     pill_dealt = "Dealt (Pills)", taken = "Taken" }
-- Labels too wide for half the panel; each takes a whole line.
local WIDE_LABEL = { dealt = true, pill_dealt = true }
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
local saved    = nil        -- the rules the mode in force changed, in the
                            -- order it set them: { name, was, set } each,
                            -- `was` the value before and `set` the mode's.
                            -- nil when it changed none.
local shown    = nil        -- the panel's line for the mode's rules, or nil

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

-- The full six-field table set_modifiers takes: a missing one is 100, and
-- a mode with no modifiers at all is the classic tank.
local function full_mods(m)
  local t = {}
  for _, k in ipairs(MOD_KEYS) do
    t[k] = (m.mods and m.mods[k]) or 100
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

-- Adds one shuffled round of all fourteen to the queue. The first of the new
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

-- ── Rules ───────────────────────────────────────────────────────────

-- pct percent of v, to the nearest whole number: 0 stays 0, anything above
-- 0 stays at least 1, and a raise goes no higher than `top`. A top already
-- under v holds the rule at v: a mode that raises a rule never lowers it.
local function scaled(v, pct, top)
  if v <= 0 then
    return v
  end
  local n = math.floor(v * pct / 100 + 0.5)
  if n < 1 then n = 1 end
  if top ~= nil and n > top then n = math.max(top, v) end
  return n
end

-- Sets one rule from what it holds now. `to` is the value, or a function of
-- the old value that answers it. A rule already at that value is left out of
-- `saved`, so there is nothing to put back.
local function set_one(name, to)
  local was = game.rule(name)
  if was == nil then
    return
  end
  local v = type(to) == "function" and to(was) or to
  if v == was then
    return
  end
  local ok, code, why = game.set_rule(name, v)
  if ok then
    saved[#saved + 1] = { name = name, was = was, set = v }
  else
    game.log(string.format("RuleRoulette: set_rule %s %s refused: %s %s",
                           name, tostring(v), tostring(code), tostring(why)))
  end
end

-- Puts the mode's rules in force and works out its panel line. A rule
-- that is refused keeps its value and is logged; the rest still apply.
local function apply_rules(m)
  saved, shown = {}, nil
  if m.rules ~= nil then
    -- In name order, never pairs(): the same order every run.
    local names = {}
    for name in pairs(m.rules) do
      names[#names + 1] = name
    end
    table.sort(names)
    for _, name in ipairs(names) do
      set_one(name, m.rules[name])
    end
  end
  local sc = m.scale
  local first_was, first_set
  if sc ~= nil then
    local top = sc.max
    if sc.max_rule ~= nil then
      local cap = game.rule(sc.max_rule)
      if cap ~= nil and (top == nil or cap < top) then
        top = cap
      end
    end
    for i, name in ipairs(sc.rules) do
      if i == 1 then
        first_was = game.rule(name)
      end
      set_one(name, function(v) return scaled(v, sc.pct, top) end)
      if i == 1 then
        first_set = game.rule(name)
      end
    end
  end
  if m.show ~= nil then
    if sc ~= nil then
      -- The percentage the first rule really got: rounding and the top can
      -- move it off sc.pct, and the panel says what is in force.
      local pct = sc.pct
      if first_was ~= nil and first_was > 0 and first_set ~= nil then
        pct = math.floor(first_set * 100 / first_was + 0.5)
      end
      shown = string.format(m.show, pct)
    else
      shown = m.show
    end
  end
  if #saved == 0 then
    saved = nil
  end
end

-- Puts back what apply_rules changed, last first. A rule that no longer
-- holds the mode's value was changed by somebody else meanwhile, and keeps
-- their value.
local function restore_rules()
  local list = saved
  saved, shown = nil, nil
  if list == nil then
    return
  end
  for i = #list, 1, -1 do
    local r = list[i]
    local now = game.rule(r.name)
    if now ~= r.set then
      game.log(string.format("RuleRoulette: %s is %s, not this mode's %s; left as it is",
                             r.name, tostring(now), tostring(r.set)))
    else
      local ok, code, why = game.set_rule(r.name, r.was)
      if not ok then
        game.log(string.format("RuleRoulette: putting %s back to %s refused: %s %s",
                               r.name, tostring(r.was), tostring(code), tostring(why)))
      end
    end
  end
end

-- ── Panel ───────────────────────────────────────────────────────────
--
-- 128 x 128 units. Text heights are the frontend's: "small" is 8 units and
-- "normal" 11, in Inter. The widest strings below, measured from that font:
-- a mode name in normal, "Cleanup Crew", 61 units; a tag in small, "GOOD FOR
-- ALL", 49; a meaning in small, "Free roads: pave over the rubble.", 105; a
-- number cell, "Reload 160%", 41; a rule line, "Parachute: 167% speed", 74.
-- The two dealt cells, "Dealt (Tanks) 250%" and "Dealt (Pills) 200%", are
-- wider than half the panel, so each takes a whole line.
-- So the header, a meaning on one line (indented to 9 in the preview, it
-- ends at 114) and two number cells to a row all fit.
--
--   y   0 .. 15  band: mode name (normal, left), its tag (small, right)
--   y  17        meaning (small)
--   y  27 ..     numbers that are not 100, two to a row, a dealt on a
--                whole row, up to 4 rows; a rule mode, and Normal, has
--                none of those and one line of its own
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
  for _, k in ipairs(ROW_KEYS) do
    local v
    if k == "pill_dealt" then
      v = mode.pill_dealt
    else
      v = mode.mods and mode.mods[k]
    end
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
  -- A rule mode's line, on a whole line of its own, in the mode's colour.
  if shown ~= nil then
    list[#list + 1] = { "text", 3, y, KIND_COLOUR[mode.kind], "small", "left", shown }
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
  -- Only the inside of a 122x4 bar: a bar outlines itself one unit thick
  -- and fills inside that, so the fill alone is a 120x2 rect one unit in.
  -- Nothing is drawn while it is empty.
  local w = math.floor(120 * (mode_len - 1 - secs) / (mode_len - 1))
  if w > 0 then
    list[#list + 1] = { "rect", 4, y + 1, w, 2, "yellow", true }
  end
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
  -- The last mode's rules go back before this one reads any.
  restore_rules()
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
  game.log(string.format("RuleRoulette mode %d tick %d: %s (%s%s) on %d of %d tanks",
                         changes, game.tick(), mode.name, mods_text(t),
                         mode.pill_dealt and (" p" .. mode.pill_dealt) or "",
                         landed, tanks))
  if mode.rules ~= nil or mode.scale ~= nil or mode.show ~= nil then
    apply_rules(mode)
    game.log(string.format("RuleRoulette rules: %d changed, panel \"%s\"",
                           saved and #saved or 0, shown or "-"))
  end
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
  restore_rules()
  game.log(string.format("RuleRoulette ended after %d modes", changes))
end

-- What a blow takes off a pillbox, as a percent: the mode's pill_dealt for
-- a shell a tank fired, 100 for anything else. A pillbox's own shell
-- (attacker game.NEUTRAL) and a dying tank's blast keep the classic amount.
function pill_damage_scale(attacker, n, cause, pill)
  if not running or over or mode == nil or mode.pill_dealt == nil then
    return 100
  end
  if cause ~= "shell" or pill ~= nil or attacker == game.NEUTRAL then
    return 100
  end
  return mode.pill_dealt
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
  description = "Fourteen modes take turns, the same for every tank: good " ..
                "(Overdrive, Turbo, Iron Hide, Cleanup Crew, Hustle, Air Drop), " ..
                "bad (Rust Bucket, Lead Feet), trade-offs (Glass Cannon, " ..
                "Juggernaut, Machine Gun, Ice Rink, Hovercraft) and Normal. " ..
                "Bases rebuild 50% faster.",
  api         = 1,
  kind        = "mod",
  bound       = false,

  -- Bases rebuild their stock of armour, shells and mines 50% faster for the
  -- whole round: one lot every 667 ticks, not the classic 1000.
  rules = {
    base_regen_ticks = 667,
  },

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
    on_end = "Puts back the rules the last mode changed and logs how many " ..
             "modes the round had.",
    on_tank_spawned = "A new or respawned tank gets the mode in force.",
    pill_damage_scale = "In Glass Cannon a tank's shell takes twice the " ..
                        "armour off a pillbox.",
    on_chat = "With chat commands on, !roulette changes the interval and the " ..
              "preview, or skips to the next mode.",
  },
}
