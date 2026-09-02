-- Scenario sidecar for tests/attack_base_cover.map (auto-loaded as
-- <map>.scenario.lua).  Companion to tests/attack_base_cover_test.py.
--
-- Two jobs.
--
-- 1. OWNERSHIP.  The map file can only say "owner 0", so everything starts
--    ours.  Here the two CANDIDATE bases are handed to a second bot on team 1
--    -- pool 7 only ever scores HOSTILE bases -- while our own base and both
--    of our pillboxes stay with slot 0 (the -bots tank).  Re-asserted after
--    the join, because a fresh connection can shuffle pill ownership.
--
-- 2. THE OWNER.  Spawn that second bot on the idle scripted brain
--    (tests/brains/idle.lua) at the far corner of the map.  It exists to make
--    the bases hostile and to be far enough away that it changes nothing else:
--    it never moves, never shoots, and never comes near either base.
--
-- on_choose_start pins each tank to its own start so the owner cannot end up
-- spawning on top of the arena.

local OUR_BASE      = { 126, 112 }
local FOE_BASES     = { { 118, 126 }, { 134, 126 } }
local FOE_BRAIN     = "../tests/brains/idle.lua"

local our_player  = 0
local foe_player  = nil
local spawn_tried = false

local function is_foe_base(b)
  for _, fb in ipairs(FOE_BASES) do
    if b.x == fb[1] and b.y == fb[2] then return true end
  end
  return false
end

local function own_everything(g)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, our_player) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b then
      if is_foe_base(b) then
        if foe_player then
          g.set_base_owner(i, foe_player)
          -- REFILL after the hand-over. set_base_owner drains the stock on a
          -- non-neutral -> non-neutral change, and a hostile base at 0 armour
          -- is CAPTURABLE: it drops out of pool 7 (attack_base) entirely and
          -- the bot just drives onto it via capture_base. The whole test is
          -- about how an UNTOUCHED hostile base is priced.
          g.set_base_stock(i, 90, 90, 90)
        end
      elseif b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
        g.set_base_owner(i, our_player)
        g.set_base_stock(i, 90, 90, 90)
      end
    end
  end
end

function on_setup(g)
  own_everything(g)
  g.set_team(our_player, 0)
end

function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("BaseOwner", FOE_BRAIN, 1, nil)
    if s == nil then
      g.message("ATTACK_BASE_COVER spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("ATTACK_BASE_COVER owner slot=" .. tostring(s) .. " team=1")
      own_everything(g)
    end
  end
end
