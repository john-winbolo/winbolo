-- fire_block_A with the fire check turned off, the KEEL value
-- (cfg=FIRE_BLOCK_FRIENDLY_BUILDER=false). Same map, same script. Here the
-- bot shoots its own builder on the pill, so the arena is expected to fail:
-- it shows the bug fire_block_A checks is fixed.
-- Scenario script for maps/fire_block_A.map (a copy of heat_pill_A.map).
--
-- THE BUG. A bot heating its own pill (ATTACK_TANK_HEAT_PILL) shot its own
-- builder while he repaired that pill. A shell that hits a live pill
-- explodes on the pill's square, and a builder on that square dies.
--
-- THE FIX. danger.fire_block, the last check before the keys go out: when
-- the shell the bot would fire now will for sure kill our own builder (or a
-- seen ally's) within FIRE_BLOCK_WINDOW_SECONDS, the fire bit is taken off.
--
-- THE ARENA. heat_pill_A's room: our pill at (126,125), our tank, and an
-- IDLE foe on team 1 that our pill shoots at, so the bot heats the pill.
-- The pill is knocked back to 11 armour 32 ticks after every repair, so the
-- builder keeps going out to repair it while the bot keeps heating it.
--
-- THE VERDICT.
--   * the builder went out at least SORTIES_MIN times;
--   * our own shells hit our pill at least HITS_MIN times in all;
--   * at least one of those hits came after a sortie ended (the bot fires
--     again once the builder is clear);
--   * our own shell never killed our builder.
-- fire_block_A_off is the same arena with the check turned off. It is
-- marked expect=fail: the bot kills its own builder there.

local OUR_BRAIN  = "../brains/GoalHunter/init.lua"
local OUR_SLOT   = 0
local FOE_SLOT   = 1
FB_TOKENS = "cfg=PILL_REPOSITION_ENABLED=false;cfg=HEAT_REQUIRE_ENEMY_RANGE=0;cfg=FIRE_BLOCK_FRIENDLY_BUILDER=false"
local OUR_TOKENS = FB_TOKENS or
  "cfg=PILL_REPOSITION_ENABLED=false;cfg=HEAT_REQUIRE_ENEMY_RANGE=0"
local OURS       = { 126, 125 }
local SPAWN      = { 123, 126 }
local FOE_SPAWN  = { 129, 126 }
local FOE_BRAIN  = "../tests/brains/idle.lua"
local GRASS      = 7
local FILL_TICK  = 120
local SPAWN_WAIT = 40
local LAND_WAIT  = 40
local TOPUP_ARMOUR   = 11
local REDAMAGE_AFTER = 32
local PILL_CALM      = 100     -- PILLBOX_ATTACK_NORMAL: calm, so the bot heats it again
local SORTIES_MIN = 3
local HITS_MIN    = 4
local p0 = 0

local us_tried, spawn_tried = false, false
local filled, foe_tick, foe_landed = false, nil, false
local pill_idx = nil
local redamage_at = nil
local man_out = false
local sorties = 0
local last_back = nil       -- tick the latest sortie ended
local hits = 0              -- our shells that hit our pill
local hits_after_back = 0   -- ... with the builder back in the tank
local hits_man_out = 0      -- ... with the builder out
local own_kills = 0         -- our builder killed by our own shell
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
  if foe_landed then return 3 end
  return 2
end

-- Our own shell on our builder. Asked before the kill; nil keeps the
-- ordinary rule, so the builder still dies.
function can_die(g, kind, n, killer, cause, pill)
  if kind == "builder" and n == p0 and killer == p0 and cause == "shell" then
    own_kills = own_kills + 1
    first_kill = first_kill or g.tick()
    g.log(string.format("FIRE_BLOCK_A our shell killed our builder at t=%d",
                        g.tick()))
  end
  -- The foe is never killed: a dead foe respawns far off and the bot stops
  -- heating the pill.
  if kind == "tank" and n == FOE_SLOT then return false end
  return nil
end

-- can_hit is not wrapped by the compat tail: it takes no `game` argument.
function can_hit(attacker, kind, n, pill)
  if attacker == p0 and kind == "pill" and n == pill_idx then
    hits = hits + 1
    if man_out then
      hits_man_out = hits_man_out + 1
    elseif last_back then
      hits_after_back = hits_after_back + 1
    end
  end
  return nil
end

function on_tick(g, tick)
  if not us_tried and tick >= 2 then
    us_tried = true
    local u, uerr = g.spawn_bot{ slot = OUR_SLOT, name = "FireBlockUs",
                                 brain = OUR_BRAIN, team = 0, start = 1,
                                 init = g.init_tokens(OUR_TOKENS) }
    if u == nil then
      g.message("FIRE_BLOCK_A spawn_bot(us) failed: " .. tostring(uerr))
    end
    own_everything(g)
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
                                brain = FOE_BRAIN, team = 1, start = 2 }
    if s == nil then
      g.message("FIRE_BLOCK_A spawn_bot(foe) failed: " .. tostring(err))
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
      -- First knock as the fight starts: the builder goes to repair the
      -- pill just as the bot starts to heat it.
      if pill_idx then g.set_pill_armour(pill_idx, TOPUP_ARMOUR) end
    end
  end

  if not pill_idx then
    for i = 1, g.num_pills() do
      local p = g.pill(i)
      if p and p.x == OURS[1] and p.y == OURS[2] then pill_idx = i end
    end
  end

  local b = g.builder(p0)
  local out = b ~= nil and (b.state == "going" or b.state == "returning")
  if out and not man_out then sorties = sorties + 1 end
  if man_out and not out then last_back = tick end
  man_out = out

  -- Knock the pill back down once it is full, so the builder goes again.
  if pill_idx and foe_landed then
    local p = g.pill(pill_idx)
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

-- GATE: ticks=12000 bots=0 ai=yesfull gametype=open expect=fail the check is off, so the bot shoots its own builder

VERDICT_CHECK = function(g)
  local tally = string.format(
    "%d sortie(s), %d hit(s) on our pill (%d with the builder out, %d after he was back), %d own kill(s)",
    sorties, hits, hits_man_out, hits_after_back, own_kills)
  if not filled then return false, "our tank never came ashore" end
  if not foe_landed then return false, "the foe was never fielded" end
  if own_kills > 0 then
    return false, string.format("our shell killed our builder at t=%d; %s",
                                first_kill, tally)
  end
  if sorties < SORTIES_MIN then return false, "too few sorties: " .. tally end
  if hits < HITS_MIN then return false, "the bot did not heat the pill: " .. tally end
  if hits_after_back < 1 then
    return false, "the bot never fired again once the builder was back: " .. tally
  end
  return true, tally
end
