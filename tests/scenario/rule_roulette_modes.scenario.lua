-- Rule Roulette's rule modes, Normal and Glass Cannon's pillbox shells,
-- played through in a fixed order.
--
-- The runner writes data/mods/RuleRoulette.scenario.lua in front of this
-- text (the include on the GATE line), so the mod's own locals -- MODES,
-- queue, mode, shown -- are in scope here. That is how the order is forced
-- without a test hook in the mod: the queue is filled before the mod's
-- on_start takes the first mode from it.
--
--   Overdrive, Cleanup Crew, Turbo, Hustle, Lead Feet, Air Drop, Normal,
--   Glass Cannon, Iron Hide
--
-- 20 seconds each. Hustle straight into Lead Feet and Lead Feet straight
-- into Air Drop check that one mode's rules are back before the next one
-- reads them; Air Drop straight into Normal checks that Normal puts them
-- back too.
--
-- The round starts on host values, not the classic ones, so a mode that
-- worked from the classic numbers, or put the classic numbers back, shows:
-- lgm_cost_road 3, man_speed_forest 10 and lgm_helicopter_speed 4.
--
-- What is checked:
--   1. Every frame, each rule a mode can touch holds the value the mode in
--      force should give it, and base_regen_ticks holds the mod's 667. So
--      each mode's values are checked while it runs, and the values after
--      it are checked under the mode that follows.
--   2. The panel line of each rule mode.
--   3. Under Cleanup Crew a builder lays a road from a tank with no trees,
--      and the tank still has none.
--   4. The builder walks the road at 125% under Hustle and 75% under Lead
--      Feet, measured from his position each frame.
--   5. A dead builder flies back at 175% under Air Drop (the host's 4 at
--      167% is 6.68, which rounds to 7), measured the same
--      way.
--   6. Under Normal every tank has all six modifiers at 100 (and, by 1,
--      every rule at its value before the roulette).
--   7. A second tank shells its own pillbox all round. Under Glass Cannon
--      each shell takes 2 armour off it, under every other mode 1. Under
--      Glass Cannon the policy also answers 100 for a pillbox's own shell
--      and for a blast.
--
-- GATE: ticks=22000 bots=0 ai=yesfull gametype=open include=data/mods/RuleRoulette.scenario.lua

local roulette = scenario
local mod_start, mod_end, mod_spawned = on_start, on_end, on_tank_spawned
on_chat = nil   -- a human's !roulette; the tail must not wrap it

local HOST = { lgm_cost_road = 3, man_speed_forest = 10, lgm_helicopter_speed = 4 }

local arena_settings = {}
for i, st in ipairs(roulette.settings) do
  local c = {}
  for k, v in pairs(st) do c[k] = v end
  if c.id == "interval" then c.default = 20 end
  if c.id == "countdown" then c.default = 0 end
  arena_settings[i] = c
end

scenario = {
  name        = GATE_NAME,
  description = "Rule Roulette's rule modes, played in a fixed order and checked.",
  game        = GATE_GAMETYPE,
  api         = 1,
  needs_bots  = true,
  rules = {
    base_regen_ticks     = roulette.rules.base_regen_ticks,
    lgm_cost_road        = HOST.lgm_cost_road,
    man_speed_forest     = HOST.man_speed_forest,
    lgm_helicopter_speed = HOST.lgm_helicopter_speed,
  },
  settings = arena_settings,
}

local ORDER = { "Overdrive", "Cleanup Crew", "Turbo", "Hustle", "Lead Feet",
                "Air Drop", "Normal", "Glass Cannon", "Iron Hide" }

-- What each rule holds with no rule mode in force.
local BASE = {
  lgm_cost_road = 3, lgm_helicopter_speed = 4,
  man_speed_road = 16, man_speed_grass = 16, man_speed_forest = 10,
  man_speed_river = 0, man_speed_swamp = 4, man_speed_crater = 4,
  man_speed_rubble = 4, man_speed_boat = 16, man_speed_deep_sea = 0,
  man_speed_refuel_base = 16,
}
local WATCH = { "lgm_cost_road", "lgm_helicopter_speed", "man_speed_road",
  "man_speed_grass", "man_speed_forest", "man_speed_river", "man_speed_swamp",
  "man_speed_crater", "man_speed_rubble", "man_speed_boat",
  "man_speed_deep_sea", "man_speed_refuel_base" }

-- The rules each rule mode changes, worked out by hand. 10 x 1.25 = 12.5
-- and 10 x 0.75 = 7.5 both round up; 0 stays 0.
local EXPECT = {
  ["Cleanup Crew"] = { lgm_cost_road = 0 },
  ["Hustle"] = { man_speed_road = 20, man_speed_grass = 20, man_speed_forest = 13,
                 man_speed_swamp = 5, man_speed_crater = 5, man_speed_rubble = 5,
                 man_speed_boat = 20, man_speed_refuel_base = 20 },
  ["Lead Feet"] = { man_speed_road = 12, man_speed_grass = 12, man_speed_forest = 8,
                    man_speed_swamp = 3, man_speed_crater = 3, man_speed_rubble = 3,
                    man_speed_boat = 12, man_speed_refuel_base = 12 },
  ["Air Drop"] = { lgm_helicopter_speed = 7 },
}
local PANEL = {
  ["Cleanup Crew"] = "Roads: free",
  ["Hustle"]       = "Builder: 125% speed",
  ["Lead Feet"]    = "Builder: 75% speed",
  ["Air Drop"]     = "Parachute: 175% speed",
  ["Normal"]       = "Everything classic",
}

-- The test ground: a road east from the tank to a grass square G, with
-- grass around it, and deep sea to the west holding the only start, so a
-- dead builder always flies in from the same place.
local ME      = 0
local Y       = 110
local TANK_X  = 106
local GX      = 117
local SEA_X   = 101

-- The pillbox range, well out of the first's way: the gunner's own pillbox
-- two squares north of him, 14 squares from anything above. A pillbox
-- never fires on its owner, so nothing shoots back.
local GUN     = 1
local GUN_X   = 109
local GUN_Y   = 126
local PILL_Y  = 124

local bad       = nil      -- the first wrong value seen, as text
local seen      = {}       -- mode name -> true once it has been in force
local walk      = {}       -- man_speed_road -> { d = world units, n = frames }
local fly       = {}       -- lgm_helicopter_speed -> { d, n }
local free_road = false    -- a road laid from a tank with no trees
local trip      = nil      -- { free = bool, kill = bool } while one is out
local last      = nil      -- the builder's last sample: { state, wx, wy }
local start_n   = nil      -- our start
local pill_n    = nil      -- the gunner's pillbox
local armour    = nil      -- its armour last frame, nil after a reset
local last_name = nil      -- the mode in force last frame
local pill_hits = { glass = {}, other = {} }  -- armour per shell -> count
local normal_ok = false    -- Normal seen with every modifier at 100
local policy_ok = nil      -- the direct pill_damage_scale answers, or why not

local function T(name) return game.TERRAIN[name] end

function on_setup(g)
  for n = 1, g.num_pills() do
    if g.pill(n) then g.remove_pill(n) end
  end
  for n = 1, g.num_bases() do
    if g.base(n) then g.remove_base(n) end
  end
  g.fill_rect(SEA_X + 2, Y - 2, GX + 4, Y + 2, T("grass"))
  g.fill_rect(SEA_X - 1, Y - 2, SEA_X + 1, Y + 2, T("deep_sea"))
  for x = TANK_X, GX - 1 do
    g.set_tile(x, Y, T("road"))
  end
  start_n = g.add_start(SEA_X, Y, 4)
  for n = g.num_starts(), 1, -1 do
    if n ~= start_n and g.start(n) then g.remove_start(n) end
  end
  g.spawn_bot{ slot = ME, name = "Builder", brain = "idle", start = start_n }
  g.fill_rect(GUN_X - 3, PILL_Y - 2, GUN_X + 3, GUN_Y + 2, T("grass"))
  g.spawn_bot{ slot = GUN, name = "Gunner",
               brain = "../tests/brains/fire_north_in_place.lua", start = start_n }
  pill_n = g.add_pill(GUN_X, PILL_Y, GUN, 15)
end

function on_choose_start(g, p)
  return start_n
end

function on_start(g)
  for _, want in ipairs(ORDER) do
    for i, m in ipairs(MODES) do
      if m.name == want then queue[#queue + 1] = i end
    end
  end
  mod_start()
end

function on_end(g)
  mod_end()
end

function on_tank_spawned(g, p, ...)
  if p == ME or p == GUN then
    if p == ME then
      g.teleport(p, TANK_X, Y, 64)
    else
      g.teleport(p, GUN_X, GUN_Y, 0)
    end
    local tk = g.tank(p)
    if tk and tk.boat then g.set_boat(p, false) end
  end
  return mod_spawned(p, ...)
end

local function add(t, key, d)
  local e = t[key]
  if e == nil then e = { d = 0, n = 0 }; t[key] = e end
  e.d = e.d + d
  e.n = e.n + 1
end

local function rate(t, key)
  local e = t[key]
  if e == nil or e.n == 0 then return nil end
  return e.d / e.n
end

local function check_rules(g)
  local name = mode and mode.name or "-"
  local want = EXPECT[name] or {}
  for _, r in ipairs(WATCH) do
    local v = g.rule(r)
    local w = want[r] or BASE[r]
    if v ~= w then
      return string.format("%s: %s is %s, want %s", name, r, tostring(v), tostring(w))
    end
  end
  if g.rule("base_regen_ticks") ~= 667 then
    return string.format("%s: base_regen_ticks is %s", name, tostring(g.rule("base_regen_ticks")))
  end
  if PANEL[name] ~= nil and shown ~= PANEL[name] then
    return string.format("%s: panel line '%s'", name, tostring(shown))
  end
  if name == "Normal" then
    for p = 0, g.max_tanks() - 1 do
      local tk = g.tank(p)
      if tk ~= nil then
        for _, k in ipairs({ "speed", "accel", "turn", "reload", "dealt", "taken" }) do
          if tk.mods[k] ~= 100 then
            return string.format("Normal: seat %d %s is %s", p, k, tostring(tk.mods[k]))
          end
        end
      end
    end
    if mode.kind ~= "normal" then
      return "Normal: kind " .. tostring(mode.kind)
    end
    normal_ok = true
  end
  if PANEL[name] == nil and shown ~= nil then
    return string.format("%s: a panel line '%s' with no rule mode", name, shown)
  end
  return nil
end

-- "2 x5" for the armour each shell took and how often.
local function hits_text(t)
  local keys = {}
  for k in pairs(t) do keys[#keys + 1] = k end
  table.sort(keys)
  local out = {}
  for _, k in ipairs(keys) do out[#out + 1] = k .. " x" .. t[k] end
  return #out > 0 and table.concat(out, ", ") or "none"
end

local function only(t, want, least)
  local n = 0
  for k, c in pairs(t) do
    if k ~= want then return false end
    n = n + c
  end
  return n >= least
end

-- The gunner's pillbox: what each shell took this frame, and the gunner's
-- shells and the pillbox's armour kept topped up.
local function watch_pill(g)
  local name = mode and mode.name or "-"
  local changed = name ~= last_name
  last_name = name
  local gt = g.tank(GUN)
  if gt ~= nil and gt.shells < 10 then
    g.set_stocks(GUN, { shells = 40 })
  end
  local pb = pill_n and g.pill(pill_n)
  if pb == nil then return end
  local a = pb.armour
  if armour ~= nil and a < armour and not changed then
    local t = name == "Glass Cannon" and pill_hits.glass or pill_hits.other
    local d = armour - a
    t[d] = (t[d] or 0) + 1
  end
  if a <= 5 then
    g.set_pill_armour(pill_n, 15)
    armour = nil
  else
    armour = a
  end
  if name == "Glass Cannon" and policy_ok == nil then
    local neutral = pill_damage_scale(game.NEUTRAL, pill_n, "shell", pill_n)
    local blast = pill_damage_scale(GUN, pill_n, "explosion", nil)
    local tank = pill_damage_scale(GUN, pill_n, "shell", nil)
    if neutral == 100 and blast == 100 and tank == 200 then
      policy_ok = true
    else
      policy_ok = string.format("policy answers: pillbox shell %s, blast %s, tank shell %s",
                                tostring(neutral), tostring(blast), tostring(tank))
    end
  end
end

local function finish(g)
  local w16, w20, w12 = rate(walk, 16), rate(walk, 20), rate(walk, 12)
  local f4, f7 = rate(fly, 4), rate(fly, 7)
  for _, n in ipairs({ "Cleanup Crew", "Hustle", "Lead Feet", "Air Drop", "Normal",
                       "Glass Cannon" }) do
    if not seen[n] then return false, n .. " never came up" end
  end
  if not free_road then return false, "no road laid from a tank with no trees" end
  if not (w16 and w20 and w12) then
    return false, string.format("walk samples missing: 16 %s 20 %s 12 %s",
                                tostring(w16), tostring(w20), tostring(w12))
  end
  if not (f4 and f7) then
    return false, string.format("flight samples missing: 4 %s 7 %s", tostring(f4), tostring(f7))
  end
  if not normal_ok then return false, "Normal never checked" end
  if policy_ok ~= true then return false, tostring(policy_ok or "policy never asked") end
  if not only(pill_hits.glass, 2, 5) or not only(pill_hits.other, 1, 5) then
    return false, string.format("pillbox armour per shell: Glass Cannon %s, others %s",
                                hits_text(pill_hits.glass), hits_text(pill_hits.other))
  end
  local hr, lr, ar = w20 / w16, w12 / w16, f7 / f4
  local ok = hr > 1.17 and hr < 1.33 and lr > 0.69 and lr < 0.81 and ar > 1.65 and ar < 1.85
  return ok, string.format("walk x%.2f/x%.2f, flight x%.2f, free road, rules back, " ..
                           "Normal 100s, pill 2 x%d / 1 x%d",
                           hr, lr, ar, pill_hits.glass[2], pill_hits.other[1])
end

function on_tick(g, tick)
  if mode ~= nil then seen[mode.name] = true end
  if bad == nil then
    bad = check_rules(g)
    if bad ~= nil then verdict_fail(bad); return end
  end

  watch_pill(g)

  -- Done once Iron Hide, the mode after Glass Cannon, is in force.
  if mode ~= nil and mode.name == "Iron Hide" and seen["Glass Cannon"] then
    local ok, why = finish(g)
    verdict(ok, why)
    return
  end

  local b = g.builder(ME)
  local tk = g.tank(ME)
  if b == nil or tk == nil then return end

  -- Speeds, from the change in his position over one frame.
  if last ~= nil and last.state == b.state then
    if b.state == "going" and b.my == Y and last.my == Y
       and b.mx > TANK_X and b.mx < GX and last.mx > TANK_X then
      add(walk, g.rule("man_speed_road"), b.wx - last.wx)
    elseif b.state == "parachuting" then
      local dx, dy = b.wx - last.wx, b.wy - last.wy
      add(fly, g.rule("lgm_helicopter_speed"), math.sqrt(dx * dx + dy * dy))
    end
  end
  last = { state = b.state, wx = b.wx, wy = b.wy, mx = b.mx, my = b.my }

  if b.state == "going" and trip and trip.kill and b.mx >= TANK_X + 3 then
    trip.kill = false
    g.kill_lgm(ME)
    return
  end

  if b.state ~= "in_tank" then return end

  -- Back in the tank: what did the last trip leave?
  if trip ~= nil then
    if trip.free and g.map_tile(GX, Y) == T("road") and tk.trees == 0 then
      free_road = true
    end
    trip = nil
    g.set_tile(GX, Y, T("grass"))
    return            -- the next trip on the next frame, from grass
  end

  local free = g.rule("lgm_cost_road") == 0
  local heli = g.rule("lgm_helicopter_speed")
  local e = fly[heli]
  local kill = not free and (e == nil or e.n < 40)
  g.set_stocks(ME, { trees = free and 0 or 40 })
  local ok = g.builder_order(ME, "road", GX, Y)
  if ok then
    trip = { free = free, kill = kill }
  end
end

VERDICT_CHECK = function(g)
  if bad ~= nil then return false, bad end
  return finish(g)
end
