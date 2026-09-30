-- =========================================================================
-- Rule Roulette — a mod that re-rolls every tank's numbers on a clock.
--
-- Every so often (a minute by default) each tank's six modifiers are rolled
-- again: speed, accel, turn, reload, dealt and taken, as percentages of the
-- classic tank. Humans and bots get the same roll unless the host asks for
-- one roll per tank. A short countdown comes first, and the new numbers go up
-- on the centre of the screen, on the newswire and on a small panel that
-- stays up until the next roll.
--
-- It exists to play-test bots that read the live rules and their own
-- modifiers: the numbers change under them every minute, so a bot that plans
-- with the classic values shows it quickly.
--
-- Reload is the time between shots, so 160 is a gun that fires less often.
-- Taken prices every blow the tank takes, so 160 is armour that gives way
-- sooner. For those two a higher number is worse; for the other four it is
-- better.
--
-- With "also roll rules" on, a few game rules are rolled as well, the same
-- for everybody: shell damage, pillbox range, the gap between a pillbox's
-- shots, the road, grass and forest speed caps, and the builder's build time.
--
-- kind = "mod": the win condition is left to the map and the lobby.
-- bound = false: it names no square, pill or base, so it plays on any map.
--
-- The scenario API has no settings table a lobby can show, so the settings
-- are the table below. A human can also change them during a round with chat
-- commands; type "!roulette" for the list.
-- =========================================================================

local SETTINGS = {
  interval  = 60,       -- seconds between rolls, 20 to 300
  mode      = "same",   -- "same": one roll for every tank; "each": each tank its own
  intensity = "wild",   -- "mild": 70 to 140 %; "wild": 40 to 250 %
  rules     = false,    -- also roll a few game rules (see ROLLED_RULES)
  countdown = 5,        -- seconds of "New roll in N" before each roll; 0 for none
  panel     = true,     -- keep the current numbers on panel 0
  -- Which modifiers roll. One set to false stays at the classic 100.
  roll = { speed = true, accel = true, turn = true,
           reload = true, dealt = true, taken = true },
}

local INTERVAL_MIN = 20
local INTERVAL_MAX = 300

-- Modifier range in percent, and the factor range a rolled rule is scaled by.
local INTENSITY = {
  mild = { lo = 70, hi = 140, rlo = 0.80, rhi = 1.25 },
  wild = { lo = 40, hi = 250, rlo = 0.60, rhi = 1.60 },
}

local MOD_KEYS   = { "speed", "accel", "turn", "reload", "dealt", "taken" }
local MOD_LABEL  = { speed = "Speed", accel = "Accel", turn = "Turn",
                     reload = "Reload", dealt = "Dealt", taken = "Taken" }
-- For these a higher percentage hurts the tank that has it.
local WORSE_HIGH = { reload = true, taken = true }

-- The rules "also roll rules" touches. Each group is scaled by one factor
-- from the classic (or scenario-set) value read at the round start. `low`
-- and `high` are the rule's bounds; `whole` rounds to an integer. A group's
-- `worse_high` says whether a higher factor hurts the tanks.
local ROLLED_RULES = {
  { label = "Shell damage", worse_high = true,
    rules = { { name = "shell_damage", low = 1, high = 255, whole = true } } },
  { label = "Pill range", worse_high = true,
    rules = { { name = "pill_range", low = 256, high = 65535, whole = true },
              { name = "pill_fire_length", low = 0.5, high = 127, whole = false } } },
  { label = "Pill shot gap", worse_high = false,
    -- The floor is pill_attack_min_ticks, filled in at the round start.
    rules = { { name = "pill_attack_ticks", low = 1, high = 255, whole = true } } },
  { label = "Terrain speed", worse_high = false,
    rules = { { name = "speed_road",   low = 1, high = 63, whole = true },
              { name = "speed_grass",  low = 1, high = 63, whole = true },
              { name = "speed_forest", low = 1, high = 63, whole = true } } },
  { label = "Build time", worse_high = true,
    rules = { { name = "lgm_build_ticks", low = 1, high = 255, whole = true } } },
}

local running  = false
local over     = false
local rolls    = 0          -- how many rolls this round
local left     = 0          -- seconds to the next roll
local next_at  = 0          -- game.tick() of the next roll, for the panel clock
local shared   = nil        -- the "same" roll, as a modifier table
local current  = {}         -- seat -> the modifier table it was last given
local base     = {}         -- rule name -> its value at the round start
local factor   = {}         -- group index -> the factor last rolled, or nil

-- ── Small helpers ───────────────────────────────────────────────────

local function in_round(p)
  local slot = game.lobby_slot(p)
  return slot ~= nil and slot.connected and slot.fielded
end

local function round_to(v, step)
  return math.floor(v / step + 0.5) * step
end

local function clamp(v, lo, hi)
  if v < lo then return lo end
  if v > hi then return hi end
  return v
end

-- Log-uniform between lo and hi, so going up and going down are equally
-- likely: with 40 to 250 the middle of the draw is 100, not 145.
local function log_uniform(lo, hi)
  return lo * (hi / lo) ^ math.random()
end

local function band()
  return INTENSITY[SETTINGS.intensity] or INTENSITY.wild
end

-- ── Modifiers ───────────────────────────────────────────────────────

local function roll_mods()
  local b = band()
  local m = {}
  for _, k in ipairs(MOD_KEYS) do
    if SETTINGS.roll[k] then
      m[k] = clamp(round_to(log_uniform(b.lo, b.hi), 5), b.lo, b.hi)
    else
      m[k] = 100
    end
  end
  return m
end

local function mods_text(m)
  return string.format("speed %d%% accel %d%% turn %d%% reload %d%% dealt %d%% taken %d%%",
                       m.speed, m.accel, m.turn, m.reload, m.dealt, m.taken)
end

local function mods_short(m)
  return string.format("Speed %d  Accel %d  Turn %d  Reload %d  Dealt %d  Taken %d",
                       m.speed, m.accel, m.turn, m.reload, m.dealt, m.taken)
end

local function apply_mods(p, m)
  local ok, code, why = game.set_modifiers(p, m)
  if not ok then
    game.log(string.format("RuleRoulette: set_modifiers(%d) refused: %s %s",
                           p, tostring(code), tostring(why)))
  end
  current[p] = m
end

-- Whether a tank already carries these numbers. A respawn keeps them, and
-- this saves the write when it has.
local function has_mods(p, m)
  local t = game.tank(p)
  if t == nil or t.mods == nil then
    return false
  end
  for _, k in ipairs(MOD_KEYS) do
    if t.mods[k] ~= m[k] then
      return false
    end
  end
  return true
end

-- ── Rules ───────────────────────────────────────────────────────────

local function read_base_rules()
  for _, g in ipairs(ROLLED_RULES) do
    for _, r in ipairs(g.rules) do
      base[r.name] = game.rule(r.name)
    end
  end
  -- A pillbox's shot gap may not fall under its angry floor.
  local floor = game.rule("pill_attack_min_ticks")
  for _, g in ipairs(ROLLED_RULES) do
    for _, r in ipairs(g.rules) do
      if r.name == "pill_attack_ticks" then
        r.low = math.max(r.low, floor)
      end
    end
  end
end

local function write_rule(name, value)
  local ok, code, why = game.set_rule(name, value)
  if not ok then
    game.log(string.format("RuleRoulette: set_rule(%s, %s) refused: %s %s",
                           name, tostring(value), tostring(code), tostring(why)))
  end
end

local function set_group(g, f)
  for _, r in ipairs(g.rules) do
    local v = base[r.name] * f
    if r.whole then
      v = math.floor(v + 0.5)
    end
    write_rule(r.name, clamp(v, r.low, r.high))
  end
end

local function roll_rules()
  local b = band()
  for i, g in ipairs(ROLLED_RULES) do
    local f = clamp(round_to(log_uniform(b.rlo, b.rhi), 0.05), b.rlo, b.rhi)
    factor[i] = f
    set_group(g, f)
  end
end

local function restore_rules()
  for i, g in ipairs(ROLLED_RULES) do
    if factor[i] ~= nil then
      set_group(g, 1)
      factor[i] = nil
    end
  end
end

-- What a group is set to now, as a percent of its round-start value. Read
-- back from the first rule, so rounding shows (shell damage 5 * 1.1 is 6,
-- which is 120 %).
local function group_pct(g)
  local r = g.rules[1]
  if base[r.name] == nil or base[r.name] == 0 then
    return 100
  end
  return math.floor(game.rule(r.name) * 100 / base[r.name] + 0.5)
end

local function rules_text()
  local parts = {}
  for i, g in ipairs(ROLLED_RULES) do
    if factor[i] ~= nil then
      parts[#parts + 1] = string.format("%s %d%%", g.label:lower(), group_pct(g))
    end
  end
  return table.concat(parts, ", ")
end

-- ── Panel ───────────────────────────────────────────────────────────

local function tint(pct, worse_high)
  if pct == 100 then return "white" end
  local better = (pct > 100) ~= (worse_high == true)
  return better and "green" or "red"
end

local function draw_panel(m, target)
  if not SETTINGS.panel then
    return
  end
  local list = {
    { "rect", 0, 0, 128, 12, "grey_dark", true },
    { "text", 64, 2, "white", "small", "centre", "Rule Roulette" },
    { "text", 3, 15, "grey", "small", "left", "Next roll" },
    { "timer", 125, 15, "yellow", "small", "right", "down", next_at },
  }
  local y = 27
  for _, k in ipairs(MOD_KEYS) do
    list[#list + 1] = { "text", 3, y, "white", "small", "left", MOD_LABEL[k] }
    list[#list + 1] = { "text", 125, y, tint(m[k], WORSE_HIGH[k]), "small", "right",
                        m[k] .. "%" }
    y = y + 9
  end
  if SETTINGS.rules then
    y = y + 2
    for i, g in ipairs(ROLLED_RULES) do
      if factor[i] ~= nil then
        local pct = group_pct(g)
        list[#list + 1] = { "text", 3, y, "white", "small", "left", g.label }
        list[#list + 1] = { "text", 125, y, tint(pct, g.worse_high), "small", "right",
                            pct .. "%" }
        y = y + 9
      end
    end
  end
  game.panel(0, list, target)
end

local function draw_all_panels()
  if not SETTINGS.panel then
    return
  end
  if SETTINGS.mode == "each" then
    for p = 0, game.max_tanks() - 1 do
      if current[p] ~= nil and in_round(p) then
        draw_panel(current[p], p)
      end
    end
  elseif shared ~= nil then
    draw_panel(shared)
  end
end

-- ── The roll ────────────────────────────────────────────────────────

local function do_roll()
  rolls = rolls + 1
  next_at = game.tick() + SETTINGS.interval * 100

  if SETTINGS.rules then
    roll_rules()
  else
    restore_rules()
  end

  if SETTINGS.mode == "each" then
    shared = nil
    for p = 0, game.max_tanks() - 1 do
      if in_round(p) then
        local m = roll_mods()
        apply_mods(p, m)
        game.announce(mods_short(m), 5, p)
        game.log(string.format("RuleRoulette roll %d tick %d seat %d: %s%s",
                               rolls, game.tick(), p, mods_text(m),
                               has_mods(p, m) and "" or " (NOT read back)"))
      end
    end
    game.message(string.format("Rule Roulette #%d: every tank rolled its own numbers.",
                               rolls))
  else
    shared = roll_mods()
    local tanks, landed = 0, 0
    for p = 0, game.max_tanks() - 1 do
      if in_round(p) then
        apply_mods(p, shared)
        tanks = tanks + 1
        -- Read back, so the log says the numbers reached the tanks.
        if has_mods(p, shared) then
          landed = landed + 1
        end
      end
    end
    game.announce(mods_short(shared), 5)
    game.message(string.format("Rule Roulette #%d: %s", rolls, mods_text(shared)))
    game.log(string.format("RuleRoulette roll %d tick %d all: %s (read back on %d of %d tanks)",
                           rolls, game.tick(), mods_text(shared), landed, tanks))
  end

  if SETTINGS.rules then
    local rt = rules_text()
    game.message("Rules: " .. rt)
    game.log(string.format("RuleRoulette roll %d tick %d rules: %s", rolls, game.tick(), rt))
  end

  draw_all_panels()
end

local function each_second()
  if over then
    return
  end
  left = left - 1
  if left <= 0 then
    do_roll()
    left = SETTINGS.interval
  elseif left <= SETTINGS.countdown then
    game.announce("New roll in " .. left, 1)
  end
  game.timer(1, each_second)
end

-- ── Chat commands ───────────────────────────────────────────────────

local function settings_text()
  return string.format("Rule Roulette: interval %d s, mode %s, intensity %s, rules %s",
                       SETTINGS.interval, SETTINGS.mode, SETTINGS.intensity,
                       SETTINGS.rules and "on" or "off")
end

local function command(p, args)
  local verb, value = args:match("^(%S*)%s*(%S*)")
  verb = (verb or ""):lower()
  value = (value or ""):lower()

  if verb == "interval" and tonumber(value) then
    SETTINGS.interval = clamp(math.floor(tonumber(value)), INTERVAL_MIN, INTERVAL_MAX)
    game.message(settings_text() .. " (from the next roll)")
  elseif verb == "mode" and (value == "same" or value == "each") then
    SETTINGS.mode = value
    game.message(settings_text() .. " (from the next roll)")
  elseif verb == "intensity" and INTENSITY[value] then
    SETTINGS.intensity = value
    game.message(settings_text() .. " (from the next roll)")
  elseif verb == "rules" and (value == "on" or value == "off") then
    SETTINGS.rules = (value == "on")
    if not SETTINGS.rules then
      restore_rules()
      draw_all_panels()
    end
    game.message(settings_text() .. (SETTINGS.rules and " (from the next roll)" or ""))
  elseif verb == "now" then
    left = 1   -- the next second rolls
  else
    game.message(settings_text(), p)
    game.message("Type !roulette interval <20-300> | mode same|each | " ..
                 "intensity mild|wild | rules on|off | now", p)
  end
end

-- ── Hooks ───────────────────────────────────────────────────────────

function on_start()
  running = true
  read_base_rules()
  left = SETTINGS.interval
  game.message("Rule Roulette: tank numbers re-roll every " .. SETTINGS.interval ..
               " s. Type !roulette for the settings.")
  do_roll()
  game.timer(1, each_second)
end

function on_end()
  over = true
  game.log(string.format("RuleRoulette ended after %d rolls", rolls))
end

-- A tank that takes the field gets the numbers in force: the shared roll,
-- or in "each" mode its own, rolled now if it has none yet.
function on_tank_spawned(p, mx, my, respawn, scripted)
  if not running or over then
    return
  end
  local m = shared
  if SETTINGS.mode == "each" then
    m = current[p]
    if m == nil then
      m = roll_mods()
      game.log(string.format("RuleRoulette join tick %d seat %d: %s",
                             game.tick(), p, mods_text(m)))
    end
  end
  if m == nil then
    return
  end
  if not has_mods(p, m) then
    apply_mods(p, m)
  end
  current[p] = m
  if SETTINGS.mode == "each" then
    draw_panel(m, p)
  end
end

function on_player_leave(p, scripted)
  current[p] = nil
end

function on_chat(p, text, scripted)
  if scripted or not running then
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
  description = "Every tank's speed, accel, turn, reload, dealt and taken are " ..
                "re-rolled every 60 s (wild, 40 to 250 %, the same for everyone). " ..
                "Type !roulette in chat to change the interval, mode, intensity " ..
                "or to roll rules too.",
  api         = 1,
  kind        = "mod",
  bound       = false,

  callbacks = {
    on_start = "Rolls the first set of numbers and starts the clock.",
    on_end = "Logs how many rolls the round had.",
    on_tank_spawned = "A new or respawned tank gets the numbers in force.",
    on_player_leave = "Forgets a leaver's own roll.",
    on_chat = "!roulette changes the interval, mode, intensity and rules.",
  },
}
