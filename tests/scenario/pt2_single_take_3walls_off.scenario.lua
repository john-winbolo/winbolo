-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open expect=fail our first shells hit our own wall 1 (v2 wall on the real lane)
--
-- pt2_single_take_3walls_off -- the pt2_single_take_3walls arena with the
-- fast setup run off (PT2_FAST_SETUP=false), to compare. Same map and layout.
--
-- Map: tests/scenario/maps/pt2_single_take_3walls_off.map (made by
-- tests/generate_pt2_maps.py): open grass 106..146 x 106..146, one base at
-- (126,140) that the script gives to the bot, and the spawn pond at
-- (126,144), filled with grass once the tank has driven off.
-- The script puts down one neutral pill, 15 armour: A at (126,116). No
-- other pill: each pill in range of the standoff that is not A needs one
-- more wall (a bystander), and PT2_BUILD_MAX is 3, so A alone is the
-- layout where the three walls all go on A's own ray.
-- The bot comes from the south. v2's evaluator on this layout (the bot's
-- own PT2_CMP / PT2_DRIVE lines, Nov 18): mode=SINGLE chain=0, standoff
-- (127.72,123.39) deg=170 aim=3 (the top right corner), build=3, walls at
-- (126,117), (126,118) and (126,119), A's south side.
-- Fast setup does not take this take (PT2_FAST_SETUP_PLAN): at the
-- approach the gun angle (248.4) is 0.4 brad from the edge of its 16-way
-- sector ("near_boundary"), and at the in_range step, with the walls up,
-- no angle from the standoff clears them ("no_lane"). So on and off play
-- the same game.
-- The bot gets 40 trees at the start, so it can build.
--
-- PASS: A is ours (picked up or ours on the map) before the end, and none of
-- our shells hit one of our walls.
-- The script finds each wall as it goes up (a building or a damaged
-- building on the field, which is only grass) and logs every change of its
-- square. It follows each of our shells, from the tick the tank's shell
-- count drops, along the tank's facing at 32 wu a tick for the gun sight's
-- range, and logs what the shell meets first: a wall, a pill or nothing.
-- That model is close to the engine's shell, not the same: the engine's
-- shell is first seen 192 wu along the facing a tick after the shot, and
-- flies on the tank's exact angle, which the script reads to the whole
-- brad. A shell that passes a wall's corner by a few wu can be called
-- either way; the bot's tick log (playerN.jsonl) has the real shells.
--
-- EXPECTED FAIL (Nov 18): the bot's first 4 shells hit wall 1 at (126,117).
-- The tank fires from (32701,31596) at 251.24 brad; the shells pass
-- (126.99,117.08), 2.5 wu inside the wall's square, so wall 1 stops them
-- until it is rubble. v2 tested the lane at aim 3 (251.70) and found it
-- clear; the finetune ends 0.46 brad off that. Fast setup's own test finds
-- no clear lane there. Pay this debt when v2 keeps its walls off the angle
-- the finetune really ends on (awaits Andrew).
--
-- pt2_single_take_3walls_off is the same arena with PT2_FAST_SETUP off.
-- 2026-11-18.

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILLTAKE_V2_EVALUATOR=true;cfg=PT2_FAST_SETUP=false"
local p0 = 0
local PILLS = { { 126, 116 } }   -- [1] = A, the target
local SPAWN = { 126, 144 }
local END_TICK = 14800
local BOX = { 106, 106, 146, 138 }   -- where walls are looked for

local pill_n = {}
local taken_at, died = nil, nil
local walls = {}           -- { x, y, t, state, hits_ours, changes }
local wall_at = {}         -- [y * 256 + x] = index in walls
local shells = {}          -- live modelled shells
local prev_shells = nil
local first_shot = nil
local fired, on_A, on_wall, on_none, on_pill = 0, 0, 0, 0, 0
local armour_lost = 0
local stocked, filled, decided = false, false, false

function on_setup(g)
  for i, xy in ipairs(PILLS) do
    local n, code = g.add_pill(xy[1], xy[2], game.NEUTRAL, 15)
    if not n then g.log("PT2_FIELD add_pill refused: " .. tostring(code)) end
    pill_n[i] = n
  end
  for i = 1, g.num_bases() do
    if g.base(i) then g.set_base_owner(i, p0) end
  end
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  for i, xy in ipairs(PILLS) do
    g.log(string.format("PT2_FIELD pill %d is %s at (%d,%d)%s", i, tostring(pill_n[i]),
      xy[1], xy[2], i == 1 and " (A)" or ""))
  end
end

function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if victim == p0 then armour_lost = armour_lost + (amount or 0) end
end

function on_pill_killed(n, by, scripted)
  if n == pill_n[1] and not died then
    died = string.format("t=%d by=%s", game.tick(), (by == game.NEUTRAL) and "nobody" or tostring(by))
    game.log("PT2_FIELD pill A died " .. died)
  end
end

local function is_wall(tt, g)
  return tt == g.TERRAIN.building or tt == g.TERRAIN.half_building
end

local function find_walls(g, tick)
  for y = BOX[2], BOX[4] do
    for x = BOX[1], BOX[3] do
      local k = y * 256 + x
      if not wall_at[k] and is_wall(g.map_tile(x, y), g) then
        walls[#walls + 1] = { x = x, y = y, t = tick, state = g.map_tile(x, y), hits_ours = 0 }
        wall_at[k] = #walls
        g.log(string.format("PT2_WALL %d up at (%d,%d) t=%d", #walls, x, y, tick))
      end
    end
  end
end

local function watch_walls(g, tick)
  for i, w in ipairs(walls) do
    local tt = g.map_tile(w.x, w.y)
    if tt ~= w.state then
      g.log(string.format("PT2_WALL %d (%d,%d) changed t=%d %s -> %s", i, w.x, w.y, tick,
        tostring(w.state), tostring(tt)))
      w.state = tt
    end
  end
end

-- Our shells: a straight line from the tank's centre along its facing.
local function step_shells(g, tick)
  local keep = {}
  for _, s in ipairs(shells) do
    s.x = s.x + s.dx
    s.y = s.y + s.dy
    s.left = s.left - 1
    local mx, my = math.floor(s.x / 256), math.floor(s.y / 256)
    local wi = wall_at[my * 256 + mx]
    local tt = g.map_tile(mx, my)
    local pill_here = nil
    for i, xy in ipairs(PILLS) do
      if xy[1] == mx and xy[2] == my then
        local p = g.pill(pill_n[i])
        if p and (p.armour or p.health or 1) > 0 and p.owner ~= p0 then pill_here = i end
      end
    end
    if wi and is_wall(tt, g) then
      on_wall = on_wall + 1
      walls[wi].hits_ours = walls[wi].hits_ours + 1
      g.log(string.format("PT2_SHELL fired t=%d hit OUR WALL %d (%d,%d) t=%d", s.t0, wi, mx, my, tick))
    elseif pill_here then
      if pill_here == 1 then on_A = on_A + 1 else on_pill = on_pill + 1 end
      g.log(string.format("PT2_SHELL fired t=%d hit pill %d t=%d", s.t0, pill_here, tick))
    elseif s.left <= 0 then
      on_none = on_none + 1
      g.log(string.format("PT2_SHELL fired t=%d hit nothing, ended (%d,%d) t=%d", s.t0, mx, my, tick))
    else
      keep[#keep + 1] = s
    end
  end
  shells = keep
end

local function fire_shells(g, tick, tk)
  if prev_shells and tk.shells < prev_shells then
    for _ = 1, prev_shells - tk.shells do
      local a = (tk.dir or 0) * math.pi / 128
      fired = fired + 1
      first_shot = first_shot or tick
      local dx, dy = math.sin(a) * 32, -math.cos(a) * 32
      -- first step lands 192 wu out, as the engine's shell is first seen
      shells[#shells + 1] = { x = tk.wx + dx * 5, y = tk.wy + dy * 5, dx = dx, dy = dy,
                              left = math.max(1, (tk.sight or 14) * 4), t0 = tick }
      g.log(string.format("PT2_SHELL fired t=%d tank=(%d,%d) dir=%s", tick, tk.wx, tk.wy, tostring(tk.dir)))
    end
  end
  prev_shells = tk.shells
end

local function summary()
  local wl = {}
  for i, w in ipairs(walls) do
    wl[#wl + 1] = string.format("%d(%d,%d)t=%d ours=%d", i, w.x, w.y, w.t, w.hits_ours)
  end
  return string.format("A %s died %s, first shot %s, shells %d: A %d wall %d pill %d none %d, armour lost %d, walls %s",
    taken_at and ("ours t=" .. taken_at) or "not ours", died or "-",
    first_shot and ("t=" .. first_shot) or "-", fired, on_A, on_wall, on_pill, on_none,
    armour_lost, #wl > 0 and table.concat(wl, " ") or "none")
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not stocked and tk and not tk.dead then
    stocked = true
    g.set_stocks(p0, { trees = 40 })
  end
  if not filled and tick >= 120 and tk and not tk.boat and not tk.dead
     and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
    filled = true
    g.set_tile(SPAWN[1], SPAWN[2], g.TERRAIN.grass)
  end
  if decided then return end
  watch_walls(g, tick)
  if tick % 5 == 0 then find_walls(g, tick) end
  step_shells(g, tick)
  if tk and not tk.dead then fire_shells(g, tick, tk) else prev_shells = nil end
  if not taken_at and pill_n[1] then
    local p = g.pill(pill_n[1])
    -- on_pill_killed does not name A here, so the armour says when it died
    if p and not died and p.owner ~= p0 and (p.armour or 1) <= 0 then
      died = "t=" .. tick
      g.log("PT2_FIELD pill A died " .. died)
    end
    if p and p.owner == p0 then
      taken_at = tick
      g.log(string.format("PT2_FIELD pill A ours t=%d", tick))
    end
  end
  if taken_at then
    decided = true
    verdict(on_wall == 0, summary())
  elseif tick >= END_TICK then
    decided = true
    verdict(false, summary())
  end
end
