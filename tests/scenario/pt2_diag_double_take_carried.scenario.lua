-- GATE: ticks=15000 bots=0 ai=yesfull gametype=open
--
-- pt2_diag_double_take_carried -- a pill take v2 double take of two pills
-- that touch corner to corner, with one pill of ours carried in the tank as
-- the blocker, and the fast setup run on (PT2_FAST_SETUP).
--
-- Map: tests/scenario/maps/pt2_diag_double_take_carried.map (made by
-- tests/generate_pt2_maps.py): open grass 106..146 x 106..146, one base at
-- (126,140) that the script gives to the bot, and the spawn pond at
-- (126,144), filled with grass once the tank has driven off.
-- LAYOUT: A (126,116) and B (127,117) touch corner to corner (A north-west
-- of B). The tank starts in the pond south of them. v2 plans chain 1-0 (B
-- first, then A) from the west, standoff (120.92,115.11) at 290 deg, aim 4,
-- one blocker: our carried pill at (124,115). Fast setup says FAST here.
-- Note: our shells land on the corner the two pills share, so they take
-- armour off A as well as B. The model names only the first pill met, so
-- A's drops show "by=pill shell" although they keep step with our shells.
-- One pill of ours (15 armour) is added and given to the tank (give_pill,
-- retried from tick 12 until the tank is placed), as in
-- pt2_oilrig_twin_carried_pills. No trees.
--
-- PASS: A and B are ours before the end, and none of our shells hit our
-- own pill. The log has:
--   PT2_CARRY  our pill put down (square, tick) and every change of its
--              armour while it stands;
--   PT2_PILL   every change of A's and B's armour, with "ours" when one of
--              our modelled shells reached that pill on that tick (else a
--              pill's shell did it);
--   PT2_SHELL  each of our shells: fire tick, tank place and facing, and
--              what it met first (A, B, our pill or nothing).
-- Our shells are modelled as in pt2_single_take_3walls: first seen 192 wu
-- along the tank's facing a tick after the shot, then 32 wu a tick, for the
-- gun sight's range. The tank's facing is read to the whole brad, so a shell
-- that passes a square's corner by a few wu can be called either way; the
-- bot's tick log (playerN.jsonl) has the real shells.
--
-- pt2_diag_double_take_carried_off is the same arena with PT2_FAST_SETUP
-- off. 2026-11-18.

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILLTAKE_V2_EVALUATOR=true;cfg=PT2_CHAINS=true;cfg=PT2_FAST_SETUP=true"
local p0 = 0
local PILLS = { { 126, 116 }, { 127, 117 } }   -- [1] = A, [2] = B
local START = nil                            -- { x, y, dir } or nil: the pond
local OUR_PILL = { 140, 140 }                -- added here, then carried
local SPAWN = { 126, 144 }
local END_TICK = 14800
local NAMES = { "A", "B" }

local pill_n = {}
local arm = {}             -- last armour of A, B
local died = {}
local taken_at = {}
local our_n = nil
local our_arm, our_down = nil, nil
local gave, moved, filled, decided = false, false, false, false
local shells = {}
local prev_shells = nil
local first_shot = nil
local hit_tick = {}        -- [i] = last tick a modelled shell of ours reached pill i
local cnt = { fired = 0, A = 0, B = 0, ours = 0, none = 0 }
local armour_lost = 0

function on_setup(g)
  for i, xy in ipairs(PILLS) do
    local n, code = g.add_pill(xy[1], xy[2], game.NEUTRAL, 15)
    if not n then g.log("PT2_FIELD add_pill refused: " .. tostring(code)) end
    pill_n[i] = n
    arm[i] = 15
  end
  local n, code = g.add_pill(OUR_PILL[1], OUR_PILL[2], p0, 15)
  if not n then g.log("PT2_CARRY add_pill refused: " .. tostring(code)) end
  our_n = n
  for i = 1, g.num_bases() do
    if g.base(i) then g.set_base_owner(i, p0) end
  end
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
  g.log(string.format("PT2_FIELD A is pill %s at (%d,%d), B is pill %s at (%d,%d), ours is pill %s",
    tostring(pill_n[1]), PILLS[1][1], PILLS[1][2], tostring(pill_n[2]), PILLS[2][1], PILLS[2][2],
    tostring(our_n)))
end

function on_tank_hit(victim, attacker, cause, amount, pill, scripted)
  if victim ~= p0 then return end
  armour_lost = armour_lost + (amount or 0)
  local tk = game.tank(p0)
  game.log(string.format("PT2_TANK hit t=%d by=%s cause=%s pill=%s amount=%s tank=(%s,%s)",
    game.tick(), tostring(attacker), tostring(cause), tostring(pill), tostring(amount),
    tk and tostring(tk.wx) or "-", tk and tostring(tk.wy) or "-"))
end

local function pill_at(g, mx, my)
  for i, xy in ipairs(PILLS) do
    if xy[1] == mx and xy[2] == my then
      local p = g.pill(pill_n[i])
      if p and (p.armour or 0) > 0 and p.owner ~= p0 then return i end
    end
  end
  if our_n then
    local p = g.pill(our_n)
    if p and not p.in_tank and p.x == mx and p.y == my and (p.armour or 0) > 0 then return "ours" end
  end
  return nil
end

local function step_shells(g, tick)
  local keep = {}
  for _, s in ipairs(shells) do
    s.x = s.x + s.dx
    s.y = s.y + s.dy
    s.left = s.left - 1
    local mx, my = math.floor(s.x / 256), math.floor(s.y / 256)
    local what = pill_at(g, mx, my)
    if what == "ours" then
      cnt.ours = cnt.ours + 1
      g.log(string.format("PT2_SHELL fired t=%d hit OUR PILL (%d,%d) t=%d", s.t0, mx, my, tick))
    elseif what then
      cnt[NAMES[what]] = cnt[NAMES[what]] + 1
      hit_tick[what] = tick
      g.log(string.format("PT2_SHELL fired t=%d hit %s t=%d", s.t0, NAMES[what], tick))
    elseif s.left <= 0 then
      cnt.none = cnt.none + 1
      g.log(string.format("PT2_SHELL fired t=%d hit nothing, ended (%d,%d) t=%d", s.t0, mx, my, tick))
    else
      keep[#keep + 1] = s
    end
    if what or s.left <= 0 then s.done = true end
  end
  shells = keep
end

local function fire_shells(g, tick, tk)
  if prev_shells and tk.shells < prev_shells then
    for _ = 1, prev_shells - tk.shells do
      local a = (tk.dir or 0) * math.pi / 128
      local dx, dy = math.sin(a) * 32, -math.cos(a) * 32
      cnt.fired = cnt.fired + 1
      first_shot = first_shot or tick
      shells[#shells + 1] = { x = tk.wx + dx * 5, y = tk.wy + dy * 5, dx = dx, dy = dy,
                              left = math.max(1, (tk.sight or 14) * 4), t0 = tick }
      g.log(string.format("PT2_SHELL fired t=%d tank=(%d,%d) dir=%s", tick, tk.wx, tk.wy, tostring(tk.dir)))
    end
  end
  prev_shells = tk.shells
end

local pending = {}        -- armour changes waiting for the shell model

-- A change of A's or B's armour is named 4 ticks later: "ours" when one of
-- our modelled shells reached that pill within 4 ticks of it, else a pill's
-- shell (the model can be a tick or two off the engine).
local function name_changes(g, tick, flush)
  local keep = {}
  for _, c in ipairs(pending) do
    if flush or tick - c.t >= 4 then
      local h = hit_tick[c.i]
      local by = (h and math.abs(h - c.t) <= 4) and "ours" or "pill shell"
      g.log(string.format("PT2_PILL %s armour %d -> %d t=%d by=%s", NAMES[c.i], c.from, c.to, c.t, by))
      if c.to <= 0 then
        died[c.i] = string.format("t=%d by=%s", c.t, by)
        g.log(string.format("PT2_PILL %s died %s", NAMES[c.i], died[c.i]))
      end
    else
      keep[#keep + 1] = c
    end
  end
  pending = keep
end

local function watch_pills(g, tick)
  name_changes(g, tick, false)
  for i = 1, 2 do
    local p = pill_n[i] and g.pill(pill_n[i])
    if p and not died[i] and p.owner ~= p0 then
      local a = p.armour or 0
      if a ~= arm[i] and not (arm[i] <= 0) then
        pending[#pending + 1] = { i = i, from = arm[i], to = a, t = tick }
        arm[i] = a
      end
    end
    if p and not taken_at[i] and p.owner == p0 then
      taken_at[i] = tick
      g.log(string.format("PT2_PILL %s ours t=%d", NAMES[i], tick))
    end
  end
  local q = gave and our_n and g.pill(our_n)
  if q and not q.in_tank and q.owner == p0 then
    if not our_down then
      our_down = string.format("t=%d at (%d,%d)", tick, q.x, q.y)
      our_arm = q.armour
      g.log(string.format("PT2_CARRY our pill put down %s armour %d", our_down, q.armour or -1))
    elseif q.armour ~= our_arm then
      g.log(string.format("PT2_CARRY our pill armour %s -> %s t=%d", tostring(our_arm), tostring(q.armour), tick))
      our_arm = q.armour
    end
  elseif q and q.in_tank and our_down then
    g.log(string.format("PT2_CARRY our pill picked up again t=%d", tick))
    our_down = nil
  end
end

local function summary()
  return string.format("A %s died %s, B %s died %s, our pill %s armour %s, first shot %s, shells %d: A %d B %d ours %d none %d, armour lost %d",
    taken_at[1] and ("ours t=" .. taken_at[1]) or "not ours", died[1] or "-",
    taken_at[2] and ("ours t=" .. taken_at[2]) or "not ours", died[2] or "-",
    our_down or "not down", tostring(our_arm), first_shot and ("t=" .. first_shot) or "-",
    cnt.fired, cnt.A, cnt.B, cnt.ours, cnt.none, armour_lost)
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if not gave and tick >= 12 and tk and not tk.dead and our_n then
    if START and not moved then
      moved = true
      g.teleport(p0, START[1], START[2], START[3])
    end
    local p = g.pill(our_n)
    if (p and p.in_tank) or g.give_pill(p0, our_n) then
      gave = true
      g.log(string.format("PT2_CARRY t=%d gave our pill to the tank", tick))
    end
  end
  if not filled and tick >= 120 and tk and not tk.boat and not tk.dead
     and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
    filled = true
    g.set_tile(SPAWN[1], SPAWN[2], g.TERRAIN.grass)
  end
  if decided then return end
  step_shells(g, tick)
  if tk and not tk.dead then fire_shells(g, tick, tk) else prev_shells = nil end
  watch_pills(g, tick)
  if taken_at[1] and taken_at[2] then
    decided = true
    name_changes(g, tick, true)
    g.log("PT2_SUMMARY " .. summary())
    verdict(cnt.ours == 0, summary())
  elseif tick >= END_TICK then
    decided = true
    name_changes(g, tick, true)
    g.log("PT2_SUMMARY " .. summary())
    verdict(false, summary())
  end
end
