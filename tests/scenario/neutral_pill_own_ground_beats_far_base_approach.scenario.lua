-- Scenario script for
-- tests/scenario/maps/neutral_pill_own_ground_beats_far_base_approach.map
-- (made by tests/generate_neutral_pill_own_ground_maps.py).
--
-- WHAT IT TESTS: in a recorded game (20261116_002603, bot 2, t=30830-31444)
-- the bot drove on to a far enemy base and never took a NEUTRAL live pill
-- in its own ground. Two causes, three knobs (constants.lua):
--   * POOL6_PICK_WITH_FACTORS: pool 6 (attack_pill) sent the hostile pill
--     with the lowest RAW cost (deep in enemy ground) to the competition,
--     not the neutral pill the full score prefers.
--   * NEUTRAL_PILL_OWN_GROUND_FACTOR (0.75): a neutral live pill in our
--     ground costs x0.75 more.
--   * ATTACK_BASE_COMMIT_AFTER_FIRST_SHOT: ATTACK_BASE_COMMITMENT_BONUS
--     (250) held the attack_base goal from its first tick, on the drive
--     there, before any shell went at the base.
-- With the knobs on the bot leaves the drive to the far base and takes the
-- neutral pill. With all three off it keeps on to the base.
--
-- THE GROUND. A long grass field. The tank spawns at the west end (a pond
-- the script fills once the tank is off it). Our base at (118,133). The far
-- base at (150,126) belongs to seat ENEMY, which nobody plays, so the bot
-- reads it as hostile and it is the only goal: the bot sets off to attack
-- it. Every base is owned, so the game is past the opening.
--
-- THE PILLS. Once the tank is TRIGGER_MX or more on its way east (on the
-- attack_base drive, far from the base), two pills go down:
--   * NEUTRAL, full armour, at (118,130): 3 squares from our base, so its
--     square has influence about 77 (our ground).
--   * HOSTILE (seat ENEMY), DECOY_ARMOUR armour, at (146,135): near the far
--     base, influence about -84 (their ground). With little armour its raw
--     cost is below the neutral pill's, so a raw-cost pick sends it.
--
-- PASS: the neutral pill is ours (on the ground or in our tank) before the
-- far base has lost any armour. FAIL: the far base is shot first, or the
-- round ends with the neutral pill not ours.
--
-- GATE: ticks=12000 bots=0 ai=yesfull gametype=open

local BOT_BRAIN = "../brains/GoalHunter/init.lua"
local TOKENS = "cfg=PILL_REPOSITION_ENABLED=false"
local SPAWN = { 104, 126 }
local OUR_BASE = { 118, 133 }
local FAR_BASE = { 150, 126 }
local NEUTRAL_PILL = { 118, 130 }
local DECOY_PILL = { 146, 135 }
local DECOY_ARMOUR = 2
local TRIGGER_MX = 108
local p0 = 0
local ENEMY = 1
local PERIOD = 300
local TRACE_EVERY = 500

local filled = false
local placed_at = nil
local neutral_n, decoy_n = nil, nil
local far_i = nil
local far_armour0 = nil
local far_hit_at = nil
local ours_at = nil
local decided = nil   -- { ok, why } once decided
local flip = 0
local next_stock = PERIOD
local next_trace = 0

local function at(b, xy) return b.x == xy[1] and b.y == xy[2] end

function on_setup(g)
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      if at(b, FAR_BASE) then
        far_i = i
        g.set_base_owner(i, ENEMY)
        g.set_base_stock(i, 90, 0, 90)
      elseif at(b, OUR_BASE) then
        g.set_base_owner(i, p0)
        g.set_base_stock(i, 90, 90, 90)
      end
    end
  end
  g.spawn_bot{ slot = p0, name = "Bot", team = 0, brain = BOT_BRAIN,
               init = g.init_tokens(TOKENS) }
end

function on_choose_start(g, p)
  if p == p0 then return 1 end
  return nil
end

-- A bot learns a hostile base's armour only from EVENT_BASE_STOCK, sent when
-- the stock CHANGES. Every PERIOD ticks the far base's mines flip 90/89 so
-- the reading stays fresh. Armour is clamped DOWN only (no regen), never
-- healed.
local function assert_stock(g)
  if not far_i then return end
  local b = g.base(far_i)
  if not b or b.owner ~= ENEMY then return end
  flip = 1 - flip
  local arm = b.armour
  if far_armour0 and arm > far_armour0 then arm = far_armour0 end
  g.set_base_stock(far_i, arm, 0, 90 - flip)
end

local function neutral_ours(g)
  local p = neutral_n and g.pill(neutral_n)
  return p and p.owner == p0
end

function on_tick(g, tick)
  local tk = g.tank(p0)
  if tick >= next_stock then
    next_stock = tick + PERIOD
    assert_stock(g)
  end
  if not filled and tick >= 120 and tk and not tk.boat and not tk.dead
     and (tk.mx ~= SPAWN[1] or tk.my ~= SPAWN[2]) then
    filled = true
    g.set_tile(SPAWN[1], SPAWN[2], g.TERRAIN.grass)
  end
  if decided then return end
  if not placed_at then
    if tk and not tk.dead and filled and tk.mx >= TRIGGER_MX then
      local fb = far_i and g.base(far_i)
      far_armour0 = fb and fb.armour or 90
      local code
      neutral_n, code = g.add_pill(NEUTRAL_PILL[1], NEUTRAL_PILL[2], g.NEUTRAL, 15)
      if not neutral_n then
        g.log("NEUTRAL_OWN neutral add_pill refused: " .. tostring(code))
        decided = { false, "neutral pill add refused" }
        return
      end
      decoy_n, code = g.add_pill(DECOY_PILL[1], DECOY_PILL[2], ENEMY, DECOY_ARMOUR)
      if not decoy_n then
        g.log("NEUTRAL_OWN decoy add_pill refused: " .. tostring(code))
        decided = { false, "decoy pill add refused" }
        return
      end
      placed_at = tick
      g.log(string.format("NEUTRAL_OWN pills down t=%d tank (%d,%d) far base armour %d",
                          tick, tk.mx, tk.my, far_armour0))
    end
    return
  end
  local fb = far_i and g.base(far_i)
  if fb and not far_hit_at and (fb.owner ~= ENEMY or fb.armour < far_armour0) then
    far_hit_at = tick
    g.log(string.format("NEUTRAL_OWN far base shot t=%d armour %d", tick, fb.armour))
  end
  if neutral_ours(g) and not ours_at then
    ours_at = tick
    g.log(string.format("NEUTRAL_OWN neutral pill ours t=%d", tick))
  end
  if ours_at and not far_hit_at then
    decided = { true, string.format("neutral pill ours t=%d (%d ticks after the pills), far base untouched",
                                    ours_at, ours_at - placed_at) }
  elseif far_hit_at and not ours_at then
    decided = { false, string.format("far base shot t=%d before the neutral pill was taken", far_hit_at) }
  end
  if decided then
    g.log("NEUTRAL_OWN " .. (decided[1] and "pass: " or "fail: ") .. decided[2])
    verdict(decided[1], decided[2])
  end
  if tk and tick >= next_trace then
    next_trace = tick + TRACE_EVERY
    local np = neutral_n and g.pill(neutral_n)
    local dp = decoy_n and g.pill(decoy_n)
    g.log(string.format(
      "NEUTRAL_OWN trace t=%d tank (%d,%d) dead=%s sh=%s arm=%s neutral=%s/%s decoy=%s/%s far=%s",
      tick, tk.mx, tk.my, tostring(tk.dead), tostring(tk.shells), tostring(tk.armour),
      tostring(np and np.owner), tostring(np and np.armour),
      tostring(dp and dp.owner), tostring(dp and dp.armour),
      tostring(fb and fb.armour)))
  end
end

VERDICT_CHECK = function(g)
  if decided then return decided[1], decided[2] end
  if not placed_at then
    return false, "the tank never set off east, the pills were never put down"
  end
  return false, string.format("round over: neutral pill not ours, far base %s",
                              far_hit_at and ("shot t=" .. far_hit_at) or "untouched")
end
