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
-- shells lands. The limit is twelve.
--
-- Pillbox massage. Inside pill_massage_range a pillbox leads a tank
-- sliding past it with the original forward prediction, which aims well
-- wide of it, so a tank can circle a pillbox without being hit. A tank
-- driving at or away from the pillbox, more nearly than
-- pill_massage_cosine allows, is still led properly, and beyond the range
-- every tank is. The classic table has the range at zero, which is off.
-- The range is the one the old ENABLE_PILLMASSAGE_BUG build switch used:
-- 384 world units, a square and a half. The cosine is left at its default
-- of 0.5, which is also what that switch used.
--
-- Base defence. When a base is shot, every allied pillbox near it gets
-- angry and fires faster. WinBolo has always counted "near" as a square:
-- up to 9 map squares away on each axis, edge included, which reaches a
-- pillbox 9 across and 9 down, about 12.7 squares off. Mac Bolo counts it
-- as a circle of radius 7 with the edge left out, so a pillbox exactly 7
-- away, or at 5 across and 5 down, stays calm.
--
-- Starts. A spawn start is picked in two passes. Pass one looks for a
-- start with nothing near it at all: no other tank and no pillbox, whoever
-- owns them. If every start has something near it, pass two looks for a
-- start with no enemy tank and no enemy pillbox near it; friendly tanks
-- and pills, and neutral pills, are allowed. If both passes come up empty
-- the engine picks as it always does.
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
-- kind = "mod", so it leaves the win condition alone and can be added to
-- any round, beside a scenario or on its own. bound = false, so it runs
-- over whatever map is loaded.
-- =========================================================================

scenario = {
  name = "Mac Bolo Rules",
  description = "Plays the Mac Bolo shell pushback, pillbox shell cap, " ..
                "pillbox massage, base defence circle and spawn starts.",
  api = 1,
  kind = "mod",
  bound = false,

  rules = {
    -- Shell pushback. Turn the Mac Bolo push on; the classic table plays
    -- with it off.
    tank_slide_mac = 1,
    -- The push at full armour, in world units per 40 ms.
    tank_slide_step = 28,
    -- 32 and 2 are the rules' own defaults, written here so the mod says
    -- what it plays with rather than relying on them.
    tank_slide_armour_bonus = 32,
    tank_bump_decay_shift = 2,

    -- Pillbox shell cap. Turn the cap on; the classic table plays with it
    -- off. Twelve is the rule's own default, written here for the same
    -- reason.
    pill_shell_cap = 1,
    pill_max_shells_at_tank = 12,

    -- Pillbox massage. A square and a half, in world units.
    pill_massage_range = 384,

    -- Base defence.
    pill_base_defend_shape = 1,  -- 1 is a circle, 0 the classic square
    pill_base_defend_range = 7,  -- a radius, with the edge left out
  },

  callbacks = {
    on_choose_start = "Picks a start with no tank or pill near, else one with no enemy tank or pill near.",
  },
}

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

function on_choose_start(p)
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
