-- =========================================================================
-- Mac Bolo Rules — a mod that plays the Mac Bolo rules WinBolo has as
-- settings, all at once.
--
-- Shell pushback. A tank a shell hits and survives slides along the
-- shell's direction of travel. WinBolo slides every tank the same
-- distance, whatever its armour. Mac Bolo pushes a weakened tank harder:
-- about 7 pixels at full armour, rising to about 14 on the last hit a tank
-- survives. tank_slide_mac turns the Mac Bolo push on. tank_slide_step is
-- then the push at full armour and tank_slide_armour_bonus the extra push
-- at zero armour, scaled by the armour missing before the hit. Both, and
-- tank_bump_decay_shift, are read per 40 ms, so a push travels
-- step * 2^shift in all, in a slide that moves every tick.
--
-- Pillbox shell cap. No more than pill_max_shells_at_tank pillbox shells
-- are in the air at any one tank. A pillbox whose nearest target already
-- has that many coming fires at the next nearest enemy in range instead;
-- with no other target it holds its shot and fires as soon as one of those
-- shells lands. The limit is twelve by default.
--
-- Tank clearance. Use the original direction-dependent tank boxes and
-- pixel nudges against solid tiles, allowing close passes beside pills.
--
-- Pillbox aiming and massage. Use Mac Bolo's integer distance and lead
-- calculation. Below one square (as its pixel distance measures it), the
-- unsigned lead wraps and the pill aims roughly 32 squares along a moving
-- tank's heading. Passing closely beside a pill can therefore evade its
-- shots. There is no heading restriction; stopping or being obstructed
-- removes the lead. Beyond that distance the same formula leads normally.
-- This overrides the older pill_massage_range/cosine approximation.
--
-- Base defence. When a base is shot, every allied pillbox near it gets
-- angry and fires faster. WinBolo has always counted "near" as a square:
-- up to 9 map squares away on each axis, edge included, which reaches a
-- pillbox 9 across and 9 down, about 12.7 squares off. Mac Bolo counts it
-- as a circle of radius 7 with the edge left out, so a pillbox exactly 7
-- away, or at 5 across and 5 down, stays calm.
--
-- Builder walk. The builder slows by the terrain he walks over, by the
-- round's man_speed_* rules, everywhere but the square he is going to build
-- on. On that square WinBolo always walks him up to its centre at the
-- refuelling base speed, so a wall or pillbox put on swamp, crater or rubble
-- is reached at full speed. Mac Bolo slows him there too.
-- man_bless_tile_terrain_speed turns the Mac Bolo walk on. A square with no
-- walking speed, such as a river, a wall to repair or a live pillbox, keeps
-- the refuelling base speed. The walk back to the tank is not changed.
--
-- Starts. By default the round's opening tanks go where the lobby put
-- them: each player's start, picked by hand or by the team's side, stands.
-- Only a spawn after that is Mac Bolo's. A spawn start is picked in two
-- passes. Pass one looks for a start with nothing near it at all: no other
-- tank and no pillbox, whoever owns them. If every start has something near
-- it, pass two looks for a start with no enemy tank and no enemy pillbox
-- near it; friendly tanks and pills, and neutral pills, are allowed. If
-- both passes come up empty the engine picks as it always does.
--
-- "Near" is the round's own start_tank_range and start_pill_range rules,
-- measured the way the engine measures them: the larger of the two axis
-- distances, in map squares. Each pass starts at a random start and goes
-- round the list, so a map with several clear starts does not always hand
-- out the same one.
--
-- "Enemy" is read from lobby teams. Two seats on the same team are friends;
-- a seat on no team (team 0) is friends only with itself. An alliance made
-- during play is not seen here.
--
-- Settings. The host turns each part on or off in the lobby's details
-- dialog, and sets the numbers a part plays with (scenario.settings below).
-- Every part is on by default, at the numbers above, so a host who changes
-- nothing plays the same game the mod always played.
--
--   pushback            Mac Bolo shell pushback (tank_slide_mac).
--   push_step           its push at full armour (tank_slide_step, 28).
--   push_armour_bonus   its extra push at zero armour
--                       (tank_slide_armour_bonus, 32).
--   push_decay_shift    its slide length (tank_bump_decay_shift, 2).
--   shell_cap           pillbox shell cap (pill_shell_cap).
--   shell_cap_count     shells in the air at one tank
--                       (pill_max_shells_at_tank, 12).
--   tank_boxes          tank clearance (tank_collision_mac).
--   pill_aim            pillbox aiming and massage (pill_aim_mac).
--   base_defence        base defence circle (pill_base_defend_shape).
--   base_defence_radius its radius (pill_base_defend_range, 7).
--   builder_walk        builder walk (man_bless_tile_terrain_speed).
--   spawn_starts        the Mac Bolo start pick for spawns.
--   first_start_lobby   "Override first start to what was chosen in lobby".
--                       On (the default): the opening tanks keep their
--                       lobby starts, as above. Off: the Mac Bolo pick
--                       places the opening tanks too. Not read while
--                       spawn_starts is off.
--
-- A part that is off sets none of its rules: the round plays the classic
-- table there, or the value another script on the list sets. Its numbers
-- are then not read. With spawn_starts off, on_choose_start names no start
-- and the engine picks every start as it always does.
--
-- The rules table is built from the settings when the file runs, so the
-- rules are in force before the opening tanks are built and keep the list's
-- order of precedence, as a written-out table does. The lobby's script list
-- reads the file with no settings to hand, so its details dialog shows the
-- rules every part plays with on its defaults.
--
-- kind = "mod", so it leaves the win condition alone and can be added to
-- any round, beside a scenario or on its own. bound = false, so it runs
-- over whatever map is loaded.
-- =========================================================================

scenario = {
  name = "Mac Bolo Rules",
  description = "Plays the Mac Bolo shell pushback, pillbox shell cap, " ..
                "tank clearance, pillbox massage, base defence circle, " ..
                "builder walk and spawn starts. Each part can be turned " ..
                "off in the lobby.",
  api = 1,
  kind = "mod",
  bound = false,

  -- What the host sets in the lobby's details dialog, in the order of the
  -- parts above. Every default is what the mod played before it had
  -- settings. The rules table below is built from these.
  settings = {
    -- Shell pushback: the Mac Bolo push, harder on a weakened tank.
    { id = "pushback", label = "Mac Bolo shell pushback", type = "bool",
      default = true },
    -- The push at full armour, in world units per 40 ms.
    { id = "push_step", label = "Pushback at full armour", type = "int",
      min = 0, max = 63, step = 1, default = 28 },
    -- The extra push at zero armour, scaled by the armour missing.
    { id = "push_armour_bonus", label = "Pushback extra at zero armour",
      type = "int", min = 0, max = 96, step = 1, default = 32 },
    -- The slide's length: a push travels step * 2^shift in all.
    { id = "push_decay_shift", label = "Pushback slide length (shift)",
      type = "int", min = 0, max = 31, step = 1, default = 2 },

    -- Pillbox shell cap: a pill holds or retargets when its target already
    -- has this many pillbox shells coming.
    { id = "shell_cap", label = "Pillbox shell cap", type = "bool",
      default = true },
    { id = "shell_cap_count", label = "Pillbox shells in the air at one tank",
      type = "int", min = 1, max = 100, step = 1, default = 12 },

    -- Tank clearance: the Mac Bolo tank boxes against solid squares.
    { id = "tank_boxes", label = "Mac Bolo tank collision boxes",
      type = "bool", default = true },

    -- Pillbox aiming: Mac Bolo's lead, with the massage bug up close.
    { id = "pill_aim", label = "Mac Bolo pillbox aiming and massage",
      type = "bool", default = true },

    -- Base defence: a circle round a shot base instead of a square.
    { id = "base_defence", label = "Mac Bolo base defence circle",
      type = "bool", default = true },
    -- Its radius in map squares, with the edge left out.
    { id = "base_defence_radius", label = "Base defence circle radius",
      type = "int", min = 1, max = 30, step = 1, default = 7 },

    -- Builder walk: the square he builds on slows him by its terrain.
    { id = "builder_walk", label = "Mac Bolo builder walk", type = "bool",
      default = true },

    -- Starts: the two-pass Mac Bolo pick for spawns.
    { id = "spawn_starts", label = "Mac Bolo spawn starts", type = "bool",
      default = true },
    -- On: the opening tanks keep their lobby starts. Off: the Mac Bolo pick
    -- places them too. Not read while spawn_starts is off.
    { id = "first_start_lobby",
      label = "Override first start to what was chosen in lobby",
      type = "bool", default = true },
  },

  callbacks = {
    on_start = "Notes that the opening tanks are placed, so with the lobby's starts kept the Mac Bolo pick waits for the spawns after them.",
    on_choose_start = "Picks a start with no tank or pill near, else one with no enemy tank or pill near; with spawn starts off, the engine picks.",
  },
}

-- ---- Settings ------------------------------------------------------------

-- The host's choice for one of the settings above. A read with no host
-- behind it (the lobby's script list and -validate run the file with a game
-- table that answers nothing) gets the setting's own default, so the rules
-- the details dialog shows are the ones the defaults play.
local function setting(id)
  local v = game.setting(id)
  if v ~= nil then
    return v
  end
  for _, row in ipairs(scenario.settings) do
    if row.id == id then
      return row.default
    end
  end
  error("no setting is named '" .. id .. "'")
end

-- Read once, when the file runs: the values are fixed for the round, and
-- the rules below have to be known before the round boots.
local PUSHBACK          = setting("pushback")
local SHELL_CAP         = setting("shell_cap")
local TANK_BOXES        = setting("tank_boxes")
local PILL_AIM          = setting("pill_aim")
local BASE_DEFENCE      = setting("base_defence")
local BUILDER_WALK      = setting("builder_walk")
local SPAWN_STARTS      = setting("spawn_starts")
local FIRST_START_LOBBY = setting("first_start_lobby")

-- The round's rules, one block per part that is on. A part that is off
-- writes nothing, so the rule stays at the classic table's value or at the
-- value another script sets.
local rules = {}

if PUSHBACK then
  -- Turn the Mac Bolo push on; the classic table plays with it off.
  rules.tank_slide_mac = 1
  -- The push at full armour, in world units per 40 ms (28 by default).
  rules.tank_slide_step = setting("push_step")
  -- 32 and 2 by default, the rules' own defaults, written here so the mod
  -- says what it plays with rather than relying on them.
  rules.tank_slide_armour_bonus = setting("push_armour_bonus")
  rules.tank_bump_decay_shift = setting("push_decay_shift")
end

if SHELL_CAP then
  -- Turn the cap on; the classic table plays with it off. Twelve by
  -- default, the rule's own default, written here for the same reason.
  rules.pill_shell_cap = 1
  rules.pill_max_shells_at_tank = setting("shell_cap_count")
end

if TANK_BOXES then
  -- Original sprite boxes let a tank hug a pill inside its bad-lead range.
  rules.tank_collision_mac = 1
end

if PILL_AIM then
  -- Original Mac Bolo aiming, including the unsigned pill-massage bug.
  rules.pill_aim_mac = 1
end

if BASE_DEFENCE then
  -- 1 is a circle, 0 the classic square.
  rules.pill_base_defend_shape = 1
  -- A radius, with the edge left out (7 by default).
  rules.pill_base_defend_range = setting("base_defence_radius")
end

if BUILDER_WALK then
  -- Cross the square he builds on at its terrain speed.
  rules.man_bless_tile_terrain_speed = 1
end

scenario.rules = rules

-- ---- Starts --------------------------------------------------------------

-- The seat's lobby team, or 0 when it has none.
local function team_of(p)
  local slot = game.lobby_slot(p)
  return slot and slot.team or 0
end

-- Whether seat q is on seat p's side: p itself, or a seat on p's team.
local function is_friend(p, my_team, q)
  if q == p then
    return true
  end
  return my_team ~= 0 and team_of(q) == my_team
end

local function distance(x1, y1, x2, y2)
  return math.max(math.abs(x1 - x2), math.abs(y1 - y2))
end

-- What is near start s for seat p: whether any tank or pill is, and whether
-- an enemy tank or enemy pill is.
local function survey(p, my_team, s, tank_range, pill_range)
  local any, enemy = false, false

  for q = 0, game.max_tanks() - 1 do
    if q ~= p then
      local t = game.tank(q)
      if t and distance(s.x, s.y, t.mx, t.my) <= tank_range then
        any = true
        if not is_friend(p, my_team, q) then
          enemy = true
        end
      end
    end
  end

  -- A pill being carried or with no armour left is no danger.
  for n = 1, game.num_pills() do
    local pill = game.pill(n)
    if pill and not pill.in_tank and pill.armour > 0 and
        distance(s.x, s.y, pill.x, pill.y) <= pill_range then
      any = true
      if pill.owner ~= game.NEUTRAL and not is_friend(p, my_team, pill.owner) then
        enemy = true
      end
    end
  end

  return any, enemy
end

-- A start a tank can be put on: deep sea with no mine.
local function usable(s)
  return game.map_tile(s.x, s.y) == game.TERRAIN.deep_sea and
         not game.is_mine(s.x, s.y)
end

-- Whether the round's opening tanks have been placed. on_start runs once
-- they have; until then the lobby's starts stand, when first_start_lobby
-- is on.
local started = false

function on_start()
  started = true
end

function on_choose_start(p)
  -- With the part off, every start is the engine's.
  if not SPAWN_STARTS then
    return nil
  end

  -- By default the opening placement is the lobby's: each player's start,
  -- picked by hand or by the team's side, was reserved there and the engine
  -- puts the tank on it. Only a spawn after that is Mac Bolo's. With
  -- first_start_lobby off, the Mac Bolo pick places the opening tanks too.
  if FIRST_START_LOBBY and not started then
    return nil
  end

  local count = game.num_starts()
  if count == 0 then
    return nil
  end

  local tank_range = game.rule("start_tank_range")
  local pill_range = game.rule("start_pill_range")
  local my_team = team_of(p)
  local offset = math.random(count) - 1
  local no_enemy = nil

  for i = 0, count - 1 do
    local n = (offset + i) % count + 1
    local s = game.start(n)
    if s and usable(s) then
      local any, enemy = survey(p, my_team, s, tank_range, pill_range)
      if not any then
        return n                 -- pass one: nothing near at all
      end
      if not enemy and no_enemy == nil then
        no_enemy = n             -- pass two's answer, kept in case pass one finds none
      end
    end
  end

  return no_enemy                -- nil leaves the pick to the engine
end
