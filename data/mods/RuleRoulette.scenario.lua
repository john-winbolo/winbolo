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
-- turns the builder track off: no builder mode plays, and the panel's
-- builder half says "Builder: off". Each track has its own clock, its own
-- order and its own saved rules, so one track changing never moves the
-- other. The order of each track is a shuffle of its own modes from the
-- scenario's own math.random, so a seed and a replay give the same order.
-- When all of a track's modes have played they are shuffled again, and a
-- mode never follows itself across that seam. A short countdown names the
-- mode that is coming; the new mode goes up at the top of the screen and
-- on the newswire, and panel 0 shows the modes in force, the time each has
-- left and the modes that come next on each track. When both tracks change
-- on the same second, one line at the top of the screen names both, and
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
-- Other mods' modifiers. game.set_modifiers replaces a tank's whole set of
-- six, so with another script on the server that writes modifiers too
-- (Pillbox Tag sets the prize holder's speed every frame), the last write
-- wins and a tank mode can be lost. The lobby setting "Multiply other mods'
-- modifiers" decides what happens:
--
--   No (the default): a tank mode writes its own six numbers when it starts
--   and when a tank spawns, as it always has. Another script's later write
--   replaces them.
--
--   Yes: every frame (on_tick), for each seat with a live tank, the script
--   reads the tank's modifiers. It keeps, per seat, the set it wrote last
--   and the base set it multiplied. When the tank's set is not the one it
--   wrote last (another script wrote, the tank was cleared, a new tank took
--   the seat), the tank's set is the new base; otherwise the base stays. The
--   tank then gets base x the mode's percent, field by field, rounded and
--   held to each field's range (speed 1 to 2000, the rest 1 to 255), and
--   only when that differs from what it has. So the script never multiplies
--   its own result, and it follows the other script's every change. A mode
--   that ends, Normal and the end of the round put the base back, not the
--   classic tank. A modifier of 0 is the classic tank, read as 100. One
--   case it cannot see: another script that writes exactly this script's
--   last product is taken to have written nothing.
--
--   For the product to be what the tank drives on, this script's on_tick
--   has to run after the other script's in the same frame. on_tick is the
--   last per-frame hook, and the scripts' on_tick run up the lobby's list,
--   bottom first, so the top script writes last. So put Rule Roulette ABOVE
--   the other mod (a higher priority, nearer the top of the list). Below
--   it, the other mod's on_tick writes after this one, and its value is the
--   one the tank drives on. A write from the other mod's timers or event
--   hooks comes before every on_tick, so the order does not matter for
--   those.
--
-- The host sets both intervals, how many upcoming modes the panel shows for
-- each track, the countdown, the chat commands and the multiply setting in
-- the lobby
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
local multiply  = false    -- "Multiply other mods' modifiers" is Yes

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
  return ok
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

-- ── Multiplying other mods' modifiers ───────────────────────────────
--
-- Only with the "multiply" setting Yes; see the header. MULT.wrote[p] is
-- the six-field set this script last put on seat p, MULT.base[p] the set it
-- multiplied to get it. Both hold percentages, with 0 read as 100.

local MULT = { wrote = {}, base = {} }
-- Each field's top in set_modifiers (scenario_lua.c): speed alone goes past
-- a byte, to TANK_MOD_SPEED_MAX.
local MOD_MAX = { speed = 2000, accel = 255, turn = 255, reload = 255,
                  dealt = 255, taken = 255 }

-- A modifier as a percentage: 0 (and a missing one) is the classic 100.
local function pct_of(v)
  if v == nil or v == 0 then
    return 100
  end
  return v
end

local function same_set(a, b)
  for _, k in ipairs(MOD_KEYS) do
    if pct_of(a[k]) ~= pct_of(b[k]) then
      return false
    end
  end
  return true
end

-- The set as set_modifiers takes it: a field at 100 is left out (classic),
-- so a script that reads a speed of 0 as "no modifier" still does.
local function mods_table(set)
  local out = {}
  for _, k in ipairs(MOD_KEYS) do
    if set[k] ~= 100 then
      out[k] = set[k]
    end
  end
  return out
end

-- Seat p's tank t gets its base times the factors f (a full_mods table).
-- A refused write leaves MULT.wrote as it was, so the next frame cannot
-- take this script's own old product for a base.
function MULT.one(p, t, f)
  local cur = t.mods
  local wrote, base = MULT.wrote[p], MULT.base[p]
  if wrote == nil or base == nil or not same_set(cur, wrote) then
    base = {}
    for _, k in ipairs(MOD_KEYS) do
      base[k] = pct_of(cur[k])
    end
    MULT.base[p] = base
  end
  local target = {}
  for _, k in ipairs(MOD_KEYS) do
    target[k] = clamp(math.floor(base[k] * f[k] / 100 + 0.5), 1, MOD_MAX[k])
  end
  if same_set(cur, target) or apply_mods(p, mods_table(target)) then
    MULT.wrote[p] = target
  end
end

-- Every seat, once a frame. A seat with no tank forgets its sets, so the
-- next tank in it starts from its own. A dead tank is skipped: the frame
-- it is back, its set (kept, or cleared by somebody) is read again.
function MULT.all()
  local f = full_mods(TANK.mode)
  for p = 0, game.max_tanks() - 1 do
    local t = game.tank(p)
    if t == nil then
      MULT.wrote[p], MULT.base[p] = nil, nil
    elseif not t.dead and t.mods ~= nil then
      MULT.one(p, t, f)
    end
  end
end

-- The end of the round: each tank that still has this script's product
-- gets its base back. A write the state refuses changes nothing, so it is
-- not logged.
function MULT.restore()
  for p, wrote in pairs(MULT.wrote) do
    local t = game.tank(p)
    local base = MULT.base[p]
    if t ~= nil and t.mods ~= nil and base ~= nil and same_set(t.mods, wrote) then
      game.set_modifiers(p, mods_table(base))
    end
  end
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
-- "normal" 11, in Inter. The panel is two halves split by a grey line: the
-- tank track above, the builder track below. Each half is laid out alike:
--
--   y +0 .. 13   band: "Tank: Iron Hide" (normal, left, in its kind's
--                colour) and, when it fits on the band, "Next: Juggernaut"
--                (small, right, in the next mode's colour)
--   y +13 .. 15  bar, 2 units tall, filling as the mode runs
--   y +17        "Next: ..." on its own line (small, right) when it did not
--                fit on the band
--   then         "Then: A, B" (small, right, grey) with preview 2 or 3
--   then         the meaning (small, left) and the clock (small, right)
--   then         the tank's numbers that are not 100, two to a row, a
--                dealt on a whole row, up to 4 rows; then the mode's rule
--                line, if it has one (every builder mode does)
--
-- Then a grey line across the panel at the half's end + 1, and the builder
-- half starts 2 units under it. With the builder track off its half is the
-- band alone, "Builder: off" in grey.
--
-- Widths measured from Inter at these sizes: "Tank: Glass Cannon" in
-- normal is 86 units and "Next: Machine Gun" in small 60, so the long names
-- cannot share the band and their "Next:" takes the line under it;
-- text_w below works out which do. The widest meaning, "Hover over water
-- and roads alike.", is 106 from x 3, and a clock ("5:00", 14) right at
-- 125 leaves 2 units between them. "Then: Glass Cannon, Machine Gun" is
-- 108.
--
-- The worst case is a tank mode with four number rows (Overdrive, Rust
-- Bucket), both "Next:" lines wrapped and preview 3: with both "Then:"
-- lines it would end at 135. So the builder's "Then:" goes first (the
-- panel ends at 126), and if it still runs past 128 the tank's goes too.
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

-- Inter's advance widths for bytes 32 .. 126, in thousandths of the text
-- height, measured from data/fonts/InterVariable.ttf the way the frontend
-- sizes it (ascent plus descent is the height).
local CHAR_W = {
  232,237,385,523,530,810,532,248,301,301,414,547,238,379,238,297,
  522,335,503,510,533,491,513,467,512,513,238,249,547,546,547,422,
  799,570,541,604,596,496,488,617,614,222,472,555,467,746,622,632,
  528,632,532,530,533,614,570,813,563,561,520,301,297,301,389,377,
  267,463,505,472,506,482,306,506,488,201,201,453,200,723,488,495,
  505,505,310,436,270,488,464,675,451,464,457,352,275,353,547,
}
local CHAR_W_MAX = 813   -- for a byte not in the table: never too narrow

-- About how wide s draws at `height` units.
local function text_w(s, height)
  local sum = 0
  for i = 1, #s do
    sum = sum + (CHAR_W[s:byte(i) - 31] or CHAR_W_MAX)
  end
  return sum * height / 1000
end

local NEXT_GAP = 6       -- the least room between the name and its "Next:"

-- One track's half from y, laid out as the table above; answers the y
-- after it. `with_then` draws the "Then:" line when preview is 2 or 3.
local function add_half(list, tr, y, with_then)
  local m = tr.mode
  local head = tr.label .. ": " .. m.name
  list[#list + 1] = { "rect", 0, y, 128, 13, "grey_dark", true }
  list[#list + 1] = { "text", 3, y + 1, KIND_COLOUR[m.kind], "normal", "left", head }
  local nm, nxt, wrap = nil, nil, false
  if preview > 0 then
    nm  = tr.modes[tr.queue[1]]
    nxt = "Next: " .. nm.name
    if 3 + text_w(head, 11) + NEXT_GAP + text_w(nxt, 8) <= 125 then
      list[#list + 1] = { "text", 125, y + 3, KIND_COLOUR[nm.kind], "small", "right", nxt }
    else
      wrap = true
    end
  end
  local w = bar_fill(tr, 128)
  if w > 0 then
    list[#list + 1] = { "rect", 0, y + 13, w, 2, "yellow", true }
  end
  y = y + 17
  if wrap then
    list[#list + 1] = { "text", 125, y, KIND_COLOUR[nm.kind], "small", "right", nxt }
    y = y + 9
  end
  if with_then and preview >= 2 then
    local names = {}
    for i = 2, preview do
      names[#names + 1] = tr.modes[tr.queue[i]].name
    end
    list[#list + 1] = { "text", 125, y, "grey", "small", "right",
                        "Then: " .. table.concat(names, ", ") }
    y = y + 9
  end
  list[#list + 1] = { "text", 3, y, "white", "small", "left", m.mean }
  list[#list + 1] = { "timer", 125, y, "yellow", "small", "right", "down", tr.next_at }
  y = add_numbers(list, m, y + 9)
  if tr.shown ~= nil then
    list[#list + 1] = { "text", 3, y, KIND_COLOUR[m.kind], "small", "left", tr.shown }
    y = y + 9
  end
  return y
end

-- Both halves and the line between them.
local function build_halves(tank_then, builder_then)
  local list = {}
  local y = add_half(list, TANK, 0, tank_then)
  list[#list + 1] = { "line", 0, y + 1, 127, y + 1, "grey" }
  y = y + 3
  if BUILDER.mode ~= nil then
    add_half(list, BUILDER, y, builder_then)
  else
    list[#list + 1] = { "rect", 0, y, 128, 13, "grey_dark", true }
    list[#list + 1] = { "text", 3, y + 1, "grey", "normal", "left", "Builder: off" }
  end
  return list
end

-- The panel, dropping "Then:" lines, the builder's first, until it fits.
local function build_panel()
  local list = build_halves(true, true)
  if panel_bottom(list) > 128 then
    list = build_halves(true, false)
  end
  if panel_bottom(list) > 128 then
    list = build_halves(false, false)
  end
  return list
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

  if tr == TANK and multiply then
    -- The next on_tick multiplies every tank's base by the new mode.
    game.log(string.format("RuleRoulette tank mode %d tick %d: %s (%s%s) multiplied",
                           tr.changes, game.tick(), mode.name, mods_text(full_mods(mode)),
                           mode.pill_dealt and (" p" .. mode.pill_dealt) or ""))
  elseif tr == TANK then
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

-- Puts the modes that just started on the screen and the newswire. The
-- screen line goes at the top of the view ("top"; it drops under a status
-- line if another script has one up), as does the countdown. Two at
-- once share one line on the screen, so neither covers the other.
local function announce(changed)
  if #changed == 1 then
    game.announce(now_text(changed[1].mode), 5, nil, "top")
  else
    game.announce(string.format("Now: %s (tank), %s (builder)",
                                TANK.mode.name, BUILDER.mode.name), 5, nil, "top")
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
      game.announce(text, 1, nil, "top")
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
  multiply  = game.setting("multiply") == "Yes"
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
  if multiply then
    MULT.restore()
  end
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

-- A tank that takes the field gets the tank mode in force. With multiply
-- on, on_tick does it instead, from the tank's own set.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if not running or over or TANK.mode == nil or multiply then
    return
  end
  local t = full_mods(TANK.mode)
  if not has_mods(p, t) then
    apply_mods(p, t)
  end
end

-- With multiply on: every tank gets its base times the tank mode, after
-- the other scripts' frame (see the header for the order this needs).
function on_tick(tick)
  if not multiply or not running or over or TANK.mode == nil then
    return
  end
  MULT.all()
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
    -- Yes: each frame a tank's modifiers are the other mods' set times the
    -- tank mode, not replaced by it. List Rule Roulette above those mods.
    { id = "multiply", label = "Multiply other mods' modifiers",
      type = "choice", choices = { "Yes", "No" }, default = "No" },
  },

  callbacks = {
    on_start = "Starts the first tank mode, the first builder mode and the clock.",
    on_end = "Puts back the rules the last tank and builder modes changed and " ..
             "logs how many modes the round had.",
    on_tank_spawned = "A new or respawned tank gets the tank mode in force " ..
                      "(multiply off).",
    on_tick = "With multiply on, each frame every tank's modifiers become the " ..
              "other mods' set times the tank mode.",
    pill_damage_scale = "In Glass Cannon a tank's shell takes twice the " ..
                        "armour off a pillbox.",
    on_chat = "With chat commands on, !roulette changes either interval and the " ..
              "preview, or skips to the next tank or builder mode.",
  },
}
