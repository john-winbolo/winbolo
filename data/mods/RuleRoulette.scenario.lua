-- =========================================================================
-- Rule Roulette — a mod that changes every tank's numbers, and the
-- builder's rules, on two clocks, one named mode at a time on each.
--
-- There are two tracks of modes, each the same for every tank, and they run
-- side by side:
--
--   The tank track, ten modes. Eight are a fixed set of the six tank
--   modifiers (speed, accel, turn, reload, dealt and taken, as percentages
--   of the classic tank). One, Hovercraft, changes the tank's terrain rules
--   (see "Rule modes" below). One, Normal, changes nothing: the classic
--   tank between the others.
--
--   The builder track, five modes. Four leave the tank alone and change the
--   builder's rules instead. One, Normal, changes no builder rule.
--
--   Good for everyone: Overdrive, Turbo, Iron Hide (tank); Cleanup Crew,
--                      Hustle, Air Drop (builder).
--   Bad for everyone:  Rust Bucket (tank); Lead Feet (builder).
--   A trade-off:       Glass Cannon, Juggernaut, Machine Gun, Ice Rink,
--                      Hovercraft (tank).
--   No change:         Normal (one on each track).
--
-- Glass Cannon's shells also take twice the armour off a pillbox (the
-- pill_damage_scale policy below). Dealt on its own prices only the blows
-- a tank takes.
--
-- Every `interval` seconds the next tank mode starts, and every
-- `builder_interval` seconds the next builder mode. A builder interval of 0
-- turns the builder track off: no builder mode plays, and the panel is the
-- tank track's alone. Each track has its own clock, its own order and its
-- own saved rules, so one track changing never moves the other. The order
-- of each track is a shuffle of its own modes from the scenario's own
-- math.random, so a seed and a replay give the same order. When all of a
-- track's modes have played they are shuffled again, and a mode never
-- follows itself across that seam. A short countdown names the mode that
-- is coming; the new mode goes up on the centre of the screen and on the
-- newswire, and panel 0 shows the modes in force, the time each has left
-- and the modes that come next on each track. When both tracks change on
-- the same second, one line on the centre of the screen names both, and
-- the newswire has a line for each.
--
-- It exists to play-test bots that read their own modifiers and rules: the
-- numbers change under them every minute, so a bot that plans with the
-- classic values shows it quickly.
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
-- that value when it ends: at its track's next mode, and at the end of the
-- round. A rule somebody else changed while the mode was in force is left
-- at their value. No tank mode and builder mode change the same rule (the
-- script checks that when it loads), so the two tracks' saved values never
-- cross. The rules table opens every round on the classic numbers (or the
-- scenario's own), so a script switched off mid-round leaves nothing behind
-- past that round.
--
-- For the whole round, bases also rebuild their own stock 50% faster than
-- the classic rate (scenario.rules below). No mode touches that rule.
--
-- The host sets both intervals, how many upcoming modes the panel shows for
-- each track, the countdown and the chat commands in the lobby
-- (scenario.settings below). With chat commands on, a human can change the
-- intervals and the preview and skip to the next mode of either track
-- during a round; type "!roulette" for the list.
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
--
-- Only a tank mode may have modifiers or pill_dealt: the builder track never
-- writes a tank's modifiers.

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

local TANK_MODES = {
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
  -- Every square but forest drives like grass, the water takes nothing,
  -- and deep sea holds a tank up with no boat. Roads get slower, so it is a
  -- trade-off.
  { name = "Hovercraft",   kind = "trade", mean = "Hover over water and roads alike.",
    rules = HOVER_RULES, show = "Grass speed; deep sea safe" },
  -- The classic tank: every modifier 100 and no rule changed. The last
  -- tank mode's rules go back as they do at every change.
  { name = "Normal",       kind = "normal", mean = "Plain tanks: nothing changed.",
    show = "Tank numbers all classic" },
}

local BUILDER_MODES = {
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
  -- No builder rule changed. The last builder mode's rules go back as they
  -- do at every change.
  { name = "Normal",       kind = "normal", mean = "Plain builders: nothing changed.",
    show = "Builder rules all classic" },
}

-- The rules a mode writes: its `rules` names and its scaled rules.
local function rules_written(m, into)
  if m.rules ~= nil then
    for name in pairs(m.rules) do into[name] = true end
  end
  if m.scale ~= nil then
    for _, name in ipairs(m.scale.rules) do into[name] = true end
  end
  return into
end

-- Each track saves and puts back only the rules its own modes wrote, so
-- the two tracks must never write the same rule: the one that ended second
-- would put back the other's value, not the round's. Checked once, here.
do
  local tank_rules = {}
  for _, m in ipairs(TANK_MODES) do rules_written(m, tank_rules) end
  for _, m in ipairs(BUILDER_MODES) do
    if m.mods ~= nil or m.pill_dealt ~= nil then
      error("RuleRoulette: builder mode " .. m.name .. " has tank modifiers")
    end
    for name in pairs(rules_written(m, {})) do
      if tank_rules[name] then
        error("RuleRoulette: " .. name .. " is written by both tracks")
      end
    end
  end
end

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
local PREVIEW_MAX  = 3     -- the most upcoming modes a track shows on the panel

-- ── State ───────────────────────────────────────────────────────────

local preview   = 1        -- the lobby settings, read in on_start
local countdown = 5
local chat      = false

local running  = false
local over     = false
local quiet    = 0          -- seconds a change line is still up; no
                            -- countdown line goes over it meanwhile
local refused  = 0          -- panel updates refused this round

-- One per track. Every field but id, label and modes is set as it runs.
local function new_track(id, label, modes)
  return {
    id       = id,
    label    = label,
    modes    = modes,     -- the track's own modes
    interval = 0,         -- seconds per mode; 0 is a track that is off
    changes  = 0,         -- how many modes this track has had this round
    left     = 0,         -- seconds to the next mode
    next_at  = 0,         -- game.tick() of the next mode, for the panel clock
    mode_len = 0,         -- seconds the mode in force was given, the bar's full
    mode     = nil,       -- the entry of `modes` in force, nil while off
    queue    = {},        -- indices into `modes` still to come, in order
    last_idx = nil,       -- the index of the mode in force
    saved    = nil,       -- the rules the mode in force changed, in the
                          -- order it set them: { name, was, set } each,
                          -- `was` the value before and `set` the mode's.
                          -- nil when it changed none.
    shown    = nil,       -- the panel's line for the mode's rules, or nil
    wake     = false,     -- turned on by chat: start a mode next second
  }
end

local TANK    = new_track("tank", "Tank", TANK_MODES)
local BUILDER = new_track("builder", "Builder", BUILDER_MODES)
local TRACKS  = { TANK, BUILDER }

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

-- The builder interval: 0 is off, anything else is held to the range.
local function builder_secs(n)
  if n <= 0 then return 0 end
  return clamp(n, INTERVAL_MIN, INTERVAL_MAX)
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

-- Adds one shuffled round of all the track's modes to its queue. The first
-- of the new round is never the mode just before it, so no mode plays twice
-- in a row.
local function add_round(tr)
  local bag = {}
  for i = 1, #tr.modes do
    bag[i] = i
  end
  for i = #bag, 2, -1 do
    local j = math.random(i)
    bag[i], bag[j] = bag[j], bag[i]
  end
  local before = tr.queue[#tr.queue] or tr.last_idx
  if bag[1] == before then
    local j = math.random(2, #bag)
    bag[1], bag[j] = bag[j], bag[1]
  end
  for _, i in ipairs(bag) do
    tr.queue[#tr.queue + 1] = i
  end
end

-- Keeps enough modes queued for the panel to show `preview` of them.
local function fill_queue(tr)
  while #tr.queue < PREVIEW_MAX + 1 do
    add_round(tr)
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
-- the track's `saved`, so there is nothing to put back.
local function set_one(tr, name, to)
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
    tr.saved[#tr.saved + 1] = { name = name, was = was, set = v }
  else
    game.log(string.format("RuleRoulette: set_rule %s %s refused: %s %s",
                           name, tostring(v), tostring(code), tostring(why)))
  end
end

-- Puts the track's mode's rules in force and works out its panel line. A
-- rule that is refused keeps its value and is logged; the rest still apply.
local function apply_rules(tr)
  local m = tr.mode
  tr.saved, tr.shown = {}, nil
  if m.rules ~= nil then
    -- In name order, never pairs(): the same order every run.
    local names = {}
    for name in pairs(m.rules) do
      names[#names + 1] = name
    end
    table.sort(names)
    for _, name in ipairs(names) do
      set_one(tr, name, m.rules[name])
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
      set_one(tr, name, function(v) return scaled(v, sc.pct, top) end)
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
      tr.shown = string.format(m.show, pct)
    else
      tr.shown = m.show
    end
  end
  if #tr.saved == 0 then
    tr.saved = nil
  end
end

-- Puts back what apply_rules changed for this track, last first. A rule
-- that no longer holds the mode's value was changed by somebody else
-- meanwhile, and keeps their value.
local function restore_rules(tr)
  local list = tr.saved
  tr.saved, tr.shown = nil, nil
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
-- number cell, "Reload 160%", 41; a rule line, "Parachute: 167% speed", 74;
-- a mode name in small, "Cleanup Crew", about 45.
-- The two dealt cells, "Dealt (Tanks) 250%" and "Dealt (Pills) 200%", are
-- wider than half the panel, so each takes a whole line.
--
-- With the builder track off, the tank track has the whole panel:
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
-- An upcoming mode is drawn only if both its lines fit. With three number
-- rows (Glass Cannon) and three upcoming modes, the last line starts at
-- y 119 and ends at 127. With four rows (Overdrive, Rust Bucket) the second
-- upcoming mode ends at 118 and the third is left off.
--
-- With both tracks on, each track has a block, and the upcoming modes of
-- both share the last rows, one mode a row in each column:
--
--   y   0 .. 13  tank band: mode name (normal, left), its clock (small,
--                right); the tag is left out, the name's colour says it
--   y  13 .. 15  tank bar, 2 units tall, filling as the mode runs
--   y  17        tank meaning (small)
--   y  26 ..     tank numbers as above, up to 4 rows (to y 61), or its
--                one line
--   then +1      builder band, 13 tall, the same as the tank's
--   then         builder bar, 2 units tall
--   then +2      builder meaning (small)
--   then         builder rule line (small); every builder mode has one
--   then +1      a rule
--   then +2      per upcoming row, the tank's next mode at x 3 and the
--                builder's at x 65, each in its kind's colour
--
-- The builder block is always 35 units. With all six tank numbers changed
-- the builder band starts at y 63, its rule line at y 89, the rule at y 99,
-- and the three upcoming rows at y 101, 110 and 119: the last ends at 127.
-- So `preview` counts for both tracks alike and three fits.
--
-- The clocks count themselves down on the client, but a bar is drawn at the
-- value it is sent, so each_second sends the panel again once a second.

local function tint(pct, worse_high)
  local better = (pct > 100) ~= (worse_high == true)
  return better and "green" or "red"
end

-- How far the track's bar has filled, out of `full` units. Fills as the
-- mode runs, in step with the clock. The clock floors its ticks, so from
-- one tick after this draw it reads one second less than the ticks left
-- now. Over a mode it shows mode_len - 1 down to 0, so the bar fills over
-- those mode_len - 1 steps: empty while the clock shows the mode's first
-- second, full in its last.
local function bar_fill(tr, full)
  local len = math.max(tr.mode_len, 2)
  local secs = clamp(math.floor((tr.next_at - game.tick() - 1) / 100), 0, len - 1)
  return math.floor(full * (len - 1 - secs) / (len - 1))
end

-- The tank mode's numbers that are not 100, from y; answers the y after.
local function add_numbers(list, m, y)
  local col = 0
  for _, k in ipairs(ROW_KEYS) do
    local v
    if k == "pill_dealt" then
      v = m.pill_dealt
    else
      v = m.mods and m.mods[k]
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
  return y
end

-- The tank track alone, laid out as the first table above.
local function build_panel_one()
  local mode = TANK.mode
  local list = {
    { "rect", 0, 0, 128, 15, "grey_dark", true },
    { "text", 3, 2, KIND_COLOUR[mode.kind], "normal", "left", mode.name },
    { "text", 125, 4, KIND_COLOUR[mode.kind], "small", "right", KIND_TAG[mode.kind] },
    { "text", 3, 17, "white", "small", "left", mode.mean },
  }
  local y = add_numbers(list, mode, 27)
  -- A rule mode's line, on a whole line of its own, in the mode's colour.
  if TANK.shown ~= nil then
    list[#list + 1] = { "text", 3, y, KIND_COLOUR[mode.kind], "small", "left", TANK.shown }
    y = y + 9
  end
  y = y + 2
  list[#list + 1] = { "text", 3, y, "grey", "small", "left", "Next mode in" }
  list[#list + 1] = { "timer", 125, y, "yellow", "small", "right", "down", TANK.next_at }
  y = y + 9
  -- Only the inside of a 122x4 bar: a bar outlines itself one unit thick
  -- and fills inside that, so the fill alone is a 120x2 rect one unit in.
  -- Nothing is drawn while it is empty.
  local w = bar_fill(TANK, 120)
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
      local m = TANK.modes[TANK.queue[i]]
      list[#list + 1] = { "text", 3, y, KIND_COLOUR[m.kind], "small", "left",
                          (i == 1 and "Next: " or "Then: ") .. m.name }
      list[#list + 1] = { "text", 9, y + 9, "grey", "small", "left", m.mean }
      y = y + 18
    end
  end
  return list
end

-- One track's band and bar, from y; answers the y after the bar.
local function add_band(list, tr, y)
  local m = tr.mode
  list[#list + 1] = { "rect", 0, y, 128, 13, "grey_dark", true }
  list[#list + 1] = { "text", 3, y + 1, KIND_COLOUR[m.kind], "normal", "left", m.name }
  list[#list + 1] = { "timer", 125, y + 3, "yellow", "small", "right", "down", tr.next_at }
  local w = bar_fill(tr, 128)
  if w > 0 then
    list[#list + 1] = { "rect", 0, y + 13, w, 2, "yellow", true }
  end
  return y + 15
end

-- Both tracks, laid out as the second table above.
local function build_panel_two()
  local list = {}
  local tm, bm = TANK.mode, BUILDER.mode
  local y = add_band(list, TANK, 0) + 2
  list[#list + 1] = { "text", 3, y, "white", "small", "left", tm.mean }
  y = add_numbers(list, tm, y + 9)
  if TANK.shown ~= nil then
    list[#list + 1] = { "text", 3, y, KIND_COLOUR[tm.kind], "small", "left", TANK.shown }
    y = y + 9
  end
  y = add_band(list, BUILDER, y + 1) + 2
  list[#list + 1] = { "text", 3, y, "white", "small", "left", bm.mean }
  y = y + 9
  if BUILDER.shown ~= nil then
    list[#list + 1] = { "text", 3, y, KIND_COLOUR[bm.kind], "small", "left", BUILDER.shown }
  end
  y = y + 9
  if preview > 0 then
    list[#list + 1] = { "line", 3, y + 1, 124, y + 1, "grey_dark" }
    y = y + 3
    for i = 1, preview do
      if y + 8 > 128 then break end
      for _, c in ipairs({ { TANK, 3 }, { BUILDER, 65 } }) do
        local m = c[1].modes[c[1].queue[i]]
        list[#list + 1] = { "text", c[2], y, KIND_COLOUR[m.kind], "small", "left", m.name }
      end
      y = y + 9
    end
  end
  return list
end

local function build_panel()
  if BUILDER.mode ~= nil then
    return build_panel_two()
  end
  return build_panel_one()
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

-- Sends the panel. A refusal (two updates in one tick, say) is logged and
-- counted, and the next second's redraw in each_second sends it again.
local function draw_panel(log_it)
  if TANK.mode == nil then
    return
  end
  local list = build_panel()
  local ok, code, why = game.panel(0, list)
  if not ok then
    refused = refused + 1
    game.log(string.format("RuleRoulette: panel refused: %s %s",
                           tostring(code), tostring(why)))
  end
  if log_it then
    game.log(string.format("RuleRoulette panel: %d items, bottom %d of 128, preview %d",
                           #list, panel_bottom(list), preview))
  end
end

-- ── Changing mode ───────────────────────────────────────────────────

-- Starts the track's next mode. The caller announces it and draws the
-- panel, once for every track that changed this second.
local function start_mode(tr, idx)
  -- The last mode's rules go back before this one reads any.
  restore_rules(tr)
  tr.changes  = tr.changes + 1
  tr.mode     = tr.modes[idx]
  tr.last_idx = idx
  tr.mode_len = tr.interval
  tr.left     = tr.interval
  tr.next_at  = game.tick() + tr.interval * 100
  local mode  = tr.mode

  if tr == TANK then
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
    game.log(string.format("RuleRoulette tank mode %d tick %d: %s (%s%s) on %d of %d tanks",
                           tr.changes, game.tick(), mode.name, mods_text(t),
                           mode.pill_dealt and (" p" .. mode.pill_dealt) or "",
                           landed, tanks))
  else
    game.log(string.format("RuleRoulette builder mode %d tick %d: %s",
                           tr.changes, game.tick(), mode.name))
  end
  if mode.rules ~= nil or mode.scale ~= nil or mode.show ~= nil then
    apply_rules(tr)
    game.log(string.format("RuleRoulette %s rules: %d changed, panel \"%s\"",
                           tr.id, tr.saved and #tr.saved or 0, tr.shown or "-"))
  end
  fill_queue(tr)
end

local function next_mode(tr)
  fill_queue(tr)
  start_mode(tr, table.remove(tr.queue, 1))
end

-- Takes the builder track off mid-round: its rules go back at once.
local function stop_track(tr)
  restore_rules(tr)
  tr.mode, tr.wake = nil, false
  game.log(string.format("RuleRoulette %s track off at tick %d", tr.id, game.tick()))
end

-- Puts the modes that just started on the screen and the newswire. Two at
-- once share one line on the screen, so neither covers the other.
local function announce(changed)
  if #changed == 1 then
    game.announce(now_text(changed[1].mode), 5)
  else
    game.announce(string.format("Now: %s (tank), %s (builder)",
                                TANK.mode.name, BUILDER.mode.name), 5)
  end
  for _, tr in ipairs(changed) do
    game.message("Rule Roulette " .. tr.id .. ": " .. now_text(tr.mode))
  end
  quiet = 5
end

-- "Turbo in 3", or "Turbo in 3, Hustle in 5" when both tracks are close.
local function countdown_text()
  local parts = {}
  for _, tr in ipairs(TRACKS) do
    if tr.mode ~= nil and tr.left <= countdown then
      parts[#parts + 1] = tr.modes[tr.queue[1]].name .. " in " .. tr.left
    end
  end
  return #parts > 0 and table.concat(parts, ", ") or nil
end

local function each_second()
  if over then
    return
  end
  local changed = {}
  for _, tr in ipairs(TRACKS) do
    if tr.mode ~= nil then
      tr.left = tr.left - 1
      if tr.left <= 0 then
        next_mode(tr)
        changed[#changed + 1] = tr
      end
    elseif tr.wake and tr.interval > 0 then
      tr.wake = false
      next_mode(tr)
      changed[#changed + 1] = tr
    end
  end
  if #changed > 0 then
    announce(changed)
  else
    quiet = quiet - 1
    local text = quiet <= 0 and countdown_text() or nil
    if text ~= nil then
      game.announce(text, 1)
    end
  end
  -- The bars' new values, the new modes, and a retry of a refused one.
  draw_panel(#changed > 0)
  game.timer(1, each_second)
end

-- ── Chat commands ───────────────────────────────────────────────────

local function status_text()
  local b = "builder off"
  if BUILDER.mode ~= nil then
    b = string.format("builder %s (%d s left, every %d s)",
                      BUILDER.mode.name, BUILDER.left, BUILDER.interval)
  end
  return string.format("Rule Roulette: tank %s (%d s left, every %d s), %s, preview %d",
                       TANK.mode and TANK.mode.name or "-", TANK.left, TANK.interval,
                       b, preview)
end

local function command(p, args)
  local verb, value = args:match("^(%S*)%s*(%S*)")
  verb = (verb or ""):lower()
  value = (value or ""):lower()
  -- Digits only: tonumber would also take "nan" and "inf", and a NaN
  -- interval stops the clock.
  local n = value:match("^%d+$") and tonumber(value) or nil

  if verb == "interval" and n then
    TANK.interval = clamp(math.floor(n), INTERVAL_MIN, INTERVAL_MAX)
    game.message(status_text() .. " (the interval counts from the next mode)")
  elseif verb == "builder" and n then
    BUILDER.interval = builder_secs(math.floor(n))
    if BUILDER.interval == 0 then
      if BUILDER.mode ~= nil then
        stop_track(BUILDER)
        draw_panel(true)
      end
      BUILDER.wake = false
    elseif BUILDER.mode == nil then
      BUILDER.wake = true   -- the next second starts a builder mode
    end
    game.message(status_text() .. " (the builder interval counts from the next mode)")
  elseif verb == "preview" and n then
    preview = clamp(math.floor(n), 0, PREVIEW_MAX)
    draw_panel(true)
    game.message(status_text())
  elseif verb == "next" and (value == "" or value == "tank" or value == "builder") then
    local tr = value == "builder" and BUILDER or TANK
    if tr.mode ~= nil then
      tr.left    = 1   -- the next second changes this track's mode
      tr.next_at = game.tick() + 100
      draw_panel(false)
    end
  else
    game.message(status_text(), p)
    game.message("Type !roulette interval <20-300> | builder <0=off, 20-300> | " ..
                 "preview <0-3> | next [builder]", p)
  end
end

-- ── Hooks ───────────────────────────────────────────────────────────

function on_start()
  TANK.interval    = clamp(game.setting("interval"), INTERVAL_MIN, INTERVAL_MAX)
  BUILDER.interval = builder_secs(game.setting("builder_interval"))
  preview   = clamp(game.setting("preview"), 0, PREVIEW_MAX)
  countdown = game.setting("countdown")
  chat      = game.setting("chat")
  running   = true
  game.message("Rule Roulette: a new tank mode every " .. TANK.interval .. " s" ..
               (BUILDER.interval > 0
                  and (", a new builder mode every " .. BUILDER.interval .. " s.")
                  or ".") ..
               (chat and " Type !roulette for the commands." or ""))
  local changed = { TANK }
  next_mode(TANK)
  if BUILDER.interval > 0 then
    next_mode(BUILDER)
    changed[2] = BUILDER
  end
  announce(changed)
  draw_panel(true)
  game.timer(1, each_second)
end

function on_end()
  over = true
  -- In the opposite order to the start; the tracks share no rule, so either
  -- order puts back the same values.
  restore_rules(BUILDER)
  restore_rules(TANK)
  game.log(string.format("RuleRoulette ended after %d tank modes and %d builder modes, " ..
                         "%d panel refusals", TANK.changes, BUILDER.changes, refused))
end

-- What a blow takes off a pillbox, as a percent: the tank mode's pill_dealt
-- for a shell a tank fired, 100 for anything else. A pillbox's own shell
-- (attacker game.NEUTRAL) and a dying tank's blast keep the classic amount.
function pill_damage_scale(attacker, n, cause, pill)
  local mode = TANK.mode
  if not running or over or mode == nil or mode.pill_dealt == nil then
    return 100
  end
  if cause ~= "shell" or pill ~= nil or attacker == game.NEUTRAL then
    return 100
  end
  return mode.pill_dealt
end

-- A tank that takes the field gets the tank mode in force.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if not running or over or TANK.mode == nil then
    return
  end
  local t = full_mods(TANK.mode)
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
  description = "Tank and builder modes change on two clocks. Tank: Overdrive, " ..
                "Turbo, Iron Hide, Rust Bucket, Glass Cannon, Juggernaut, Machine " ..
                "Gun, Ice Rink, Hovercraft, Normal. Builder: Cleanup Crew, Hustle, " ..
                "Lead Feet, Air Drop, Normal. Bases rebuild 50% faster.",
  api         = 1,
  kind        = "mod",
  bound       = false,

  -- Bases rebuild their stock of armour, shells and mines 50% faster for the
  -- whole round: one lot every 667 ticks, not the classic 1000.
  rules = {
    base_regen_ticks = 667,
  },

  settings = {
    { id = "interval", label = "Seconds per tank mode", type = "int",
      min = 20, max = 300, step = 10, default = 60 },
    -- 0 turns the builder track off; 10 is taken as 20, the shortest.
    { id = "builder_interval", label = "Seconds per builder mode (0 = off)", type = "int",
      min = 0, max = 300, step = 10, default = 90 },
    { id = "preview", label = "Upcoming modes shown per track", type = "int",
      min = 0, max = 3, step = 1, default = 1 },
    { id = "countdown", label = "Countdown before a change (seconds)", type = "int",
      min = 0, max = 10, step = 1, default = 5 },
    { id = "chat", label = "!roulette chat commands", type = "bool",
      default = false },
  },

  callbacks = {
    on_start = "Starts the first tank mode, the first builder mode and the clock.",
    on_end = "Puts back the rules the last tank and builder modes changed and " ..
             "logs how many modes the round had.",
    on_tank_spawned = "A new or respawned tank gets the tank mode in force.",
    pill_damage_scale = "In Glass Cannon a tank's shell takes twice the " ..
                        "armour off a pillbox.",
    on_chat = "With chat commands on, !roulette changes either interval and the " ..
              "preview, or skips to the next tank or builder mode.",
  },
}
