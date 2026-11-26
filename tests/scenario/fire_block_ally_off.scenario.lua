-- fire_block_ally with the fire check turned off, the KEEL value
-- (cfg=FIRE_BLOCK_FRIENDLY_BUILDER=false). Same map, same script. Here the
-- bot shoots the ally's builder on the pill, so the arena is expected to
-- fail: it shows the bug fire_block_ally checks is fixed.
-- Scenario script for maps/fire_block_ally.map (a copy of heat_pill_A.map).
--
-- fire_block_A for an ally's builder. danger.fire_block also holds fire when
-- the shell would for sure kill a builder we know is an ally's: one we can
-- see that is not hostile. This ally runs the idle brain, so it sends no
-- lgmd advert. The bot has only what it sees of his builder: his motion over
-- the last few ticks, or where he stands.
--
-- THE ARENA. heat_pill_A's room. Our pill at (126,125), our tank on team 2,
-- an IDLE ally on team 2 parked ashore at (122,127), and an IDLE foe on
-- team 1 that our pill shoots at, so the bot heats the pill. The foe and the
-- ally cannot die (can_die), so the fight never moves. The arena sends the
-- ALLY's builder to repair our pill when the pill is short and he has
-- rested ALLY_REST ticks at home. It knocks the pill back to 11 armour
-- and calm 32 ticks after it is full.
--
-- THE VERDICT.
--   * the ally's builder went out at least SORTIES_MIN times;
--   * our own shells hit our pill at least HITS_MIN times;
--   * our own shell never killed the ally's builder, nor ours.

local OUR_BRAIN  = "../brains/GoalHunter/init.lua"
local OUR_SLOT   = 0
local FOE_SLOT   = 1
local ALLY_SLOT  = 2
local TEAM       = 2
FB_TOKENS = "cfg=PILL_REPOSITION_ENABLED=false;cfg=HEAT_REQUIRE_ENEMY_RANGE=0;cfg=FIRE_BLOCK_FRIENDLY_BUILDER=false"
local OUR_TOKENS = FB_TOKENS or
  "cfg=PILL_REPOSITION_ENABLED=false;cfg=HEAT_REQUIRE_ENEMY_RANGE=0"
local OURS       = { 126, 125 }
local SPAWN      = { 123, 126 }
local FOE_SPAWN  = { 129, 126 }
local ALLY_PARK  = { 122, 127 }
local IDLE_BRAIN = "../tests/brains/idle.lua"
local GRASS      = 7
local FILL_TICK  = 120
local SPAWN_WAIT = 40
local LAND_WAIT  = 40
local TOPUP_ARMOUR   = 11
local REDAMAGE_AFTER = 32
local PILL_CALM      = 100     -- PILLBOX_ATTACK_NORMAL: calm, so the bot heats it again
local ALLY_REST   = 400     -- ticks the ally waits at home between repairs, so the bot has time to heat
local SORTIES_MIN = 3
local HITS_MIN    = 2       -- the bot still heats the pill: the check does not hold fire for good
local p0 = 0

local us_tried, spawn_tried, ally_tried = false, false, false
local ally_parked = false
local filled, foe_tick, foe_landed = false, nil, false
local pill_idx = nil
local redamage_at = nil
local ally_out = false
local ally_home_at = 0
local ally_sorties = 0
local hits = 0
local ally_kills = 0
local own_kills = 0
local first_kill = nil

local function own_everything(g)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, p0) end
  end
  for i = 1, g.num_bases() do
    if g.base(i) then
      g.set_base_owner(i, p0)
      g.set_base_stock(i, 90, 90, 90)
    end
  end
end

function on_setup(g)
  own_everything(g)
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  if p == ALLY_SLOT then return 3 end
  return 2
end

function can_die(g, kind, n, killer, cause, pill)
  if kind == "builder" and killer == p0 and cause == "shell" then
    if n == ALLY_SLOT then ally_kills = ally_kills + 1 end
    if n == p0 then own_kills = own_kills + 1 end
    first_kill = first_kill or g.tick()
    g.log(string.format("FIRE_BLOCK_ALLY our shell killed player %d's builder at t=%d",
                        n, g.tick()))
  end
  -- The foe and the ally are never killed: the fight must not move.
  if kind == "tank" and (n == FOE_SLOT or n == ALLY_SLOT) then return false end
  return nil
end

-- can_hit is not wrapped by the compat tail: it takes no `game` argument.
function can_hit(attacker, kind, n, pill)
  if attacker == p0 and kind == "pill" and n == pill_idx then
    hits = hits + 1
  end
  return nil
end

function on_tick(g, tick)
  if not us_tried and tick >= 2 then
    us_tried = true
    local u, uerr = g.spawn_bot{ slot = OUR_SLOT, name = "FireBlockUs",
                                 brain = OUR_BRAIN, team = TEAM, start = 1,
                                 init = g.init_tokens(OUR_TOKENS) }
    if u == nil then
      g.message("FIRE_BLOCK_ALLY spawn_bot(us) failed: " .. tostring(uerr))
    end
    own_everything(g)
  end

  if not ally_tried and tick >= 4 then
    ally_tried = true
    local a, aerr = g.spawn_bot{ slot = ALLY_SLOT, name = "FireBlockAlly",
                                 brain = IDLE_BRAIN, team = TEAM, start = 3 }
    if a == nil then
      g.message("FIRE_BLOCK_ALLY spawn_bot(ally) failed: " .. tostring(aerr))
    end
    own_everything(g)
  end
  if ally_tried and not ally_parked and tick >= 60 then
    local ak = g.tank(ALLY_SLOT)
    if ak and not ak.dead then
      g.teleport(ALLY_SLOT, ALLY_PARK[1], ALLY_PARK[2])
      ally_parked = true
    end
  end

  if not filled and tick >= FILL_TICK then
    local tk = g.tank(p0)
    if tk and not tk.boat and not tk.dead
       and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
      filled = tick
      g.set_tile(SPAWN[1], SPAWN[2], GRASS)
    end
  end

  if filled and not spawn_tried and tick >= filled + SPAWN_WAIT then
    spawn_tried = true
    local s, err = g.spawn_bot{ slot = FOE_SLOT, name = "FireBlockFoe",
                                brain = IDLE_BRAIN, team = 1, start = 2 }
    if s == nil then
      g.message("FIRE_BLOCK_ALLY spawn_bot(foe) failed: " .. tostring(err))
    else
      foe_tick = tick
      g.set_team(FOE_SLOT, 1)
      own_everything(g)
    end
  end

  if foe_tick and not foe_landed and tick >= foe_tick + LAND_WAIT then
    local fk = g.tank(FOE_SLOT)
    if fk and fk.mx == FOE_SPAWN[1] and fk.my == FOE_SPAWN[2] then
      foe_landed = true
      g.set_tile(FOE_SPAWN[1], FOE_SPAWN[2], GRASS)
      if pill_idx then g.set_pill_armour(pill_idx, TOPUP_ARMOUR) end
    end
  end

  if not pill_idx then
    for i = 1, g.num_pills() do
      local p = g.pill(i)
      if p and p.x == OURS[1] and p.y == OURS[2] then pill_idx = i end
    end
  end

  local b = g.builder(ALLY_SLOT)
  local out = b ~= nil and (b.state == "going" or b.state == "returning")
  if out and not ally_out then ally_sorties = ally_sorties + 1 end
  if ally_out and not out then ally_home_at = tick end
  ally_out = out

  if pill_idx and foe_landed then
    local p = g.pill(pill_idx)
    -- The ally's builder goes to repair our pill once he has rested at home.
    if p and p.armour < 15 and not p.in_tank and b and b.state == "in_tank"
       and tick >= ally_home_at + ALLY_REST then
      g.set_stocks(ALLY_SLOT, { trees = 40 })
      g.builder_order(ALLY_SLOT, "pill", OURS[1], OURS[2])
    end
    if p and p.armour >= 15 and not p.in_tank then
      redamage_at = redamage_at or (tick + REDAMAGE_AFTER)
      if tick >= redamage_at then
        g.set_pill_armour(pill_idx, TOPUP_ARMOUR)
        g.set_pill_speed(pill_idx, PILL_CALM)
        redamage_at = nil
      end
    else
      redamage_at = nil
    end
  end
end

-- GATE: ticks=12000 bots=0 ai=yesfull gametype=open expect=fail the check is off, so the bot shoots the ally's builder

VERDICT_CHECK = function(g)
  local tally = string.format(
    "ally kills %d, own kills %d, hits %d, ally sorties %d",
    ally_kills, own_kills, hits, ally_sorties)
  if not filled then return false, "our tank never came ashore" end
  if not foe_landed then return false, "the foe was never fielded" end
  if not ally_parked then return false, "the ally was never parked" end
  if ally_kills > 0 or own_kills > 0 then
    return false, string.format("our shell killed a friendly builder at t=%d; %s",
                                first_kill, tally)
  end
  if ally_sorties < SORTIES_MIN then return false, "too few ally sorties: " .. tally end
  if hits < HITS_MIN then return false, "the bot did not heat the pill: " .. tally end
  return true, tally
end
