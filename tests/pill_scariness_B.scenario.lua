-- Scenario sidecar for tests/pill_scariness_B.map.  Variant B: the control.
--
-- A proves a HEALTHY pill catching NEUTRAL-pillbox strays gets no watch bid.
-- B has to prove the rule did not just switch defend off, so it removes every
-- pillbox except ours and puts a real enemy tank six tiles from it.  With no
-- pillbox anywhere for a shell's back-ray to land on, ANY damage our pill
-- takes attributes to TANK fire (and the presence and no-evidence fallbacks
-- both answer "tank" too), so B expects the pre-change behaviour: src=tank and
-- the ARRIVED watch bid firing.
--
-- Both tanks keep the default (open) loadout here, unlike A: an enemy with no
-- shells cannot shell anything.
--
--   slot 0 (the -bots tank) : ours, team 0, start 1 = (126,124)
--   the spawned bot         : team 1, hostile, start 2 = (126,128)
--
-- on_choose_start pins each tank to its own start so the two never swap ends;
-- startsScatterFind would otherwise hand out whichever pond it liked.

local OUR_PILL      = { 126, 122 }
local OUR_BASE      = { 120, 132 }
local TRACE         = "pill_scariness_B_hp.log"

local our_player  = 0
local foe_player  = nil
local spawn_tried = false
local last_hp = nil

local function own_everything(g, owner)
  for i = 1, g.num_pills() do
    if g.pill(i) then g.set_pill_owner(i, owner) end
  end
  for i = 1, g.num_bases() do
    local b = g.base(i)
    if b and b.x == OUR_BASE[1] and b.y == OUR_BASE[2] then
      g.set_base_owner(i, owner)
    end
  end
end

function on_setup(g)
  own_everything(g, our_player)
  g.set_team(our_player, 0)
  local f = io.open(TRACE, "w")
  if f then f:write("# tick hp\n") f:close() end
end

-- Start 1 = ours, start 2 = the foe.  Anything else (a respawn we did not
-- plan for) falls through to the engine's own pick.
function on_choose_start(g, p)
  if p == our_player then return 1 end
  if foe_player and p == foe_player then return 2 end
  return nil
end

function on_tick(g, tick)
  if not spawn_tried and tick >= 2 then
    spawn_tried = true
    local s, err = g.spawn_bot("Aggressor", nil, 1, nil)
    if s == nil then
      g.message("PILL_SCARINESS_B spawn_bot failed: " .. tostring(err))
    else
      foe_player = s
      g.set_team(s, 1)
      g.message("PILL_SCARINESS_B foe slot=" .. tostring(s) .. " team=1")
      -- Re-assert ownership: a fresh join can shuffle pill ownership.
      own_everything(g, our_player)
    end
  end
  for i = 1, g.num_pills() do
    local p = g.pill(i)
    if p and p.x == OUR_PILL[1] and p.y == OUR_PILL[2] then
      if last_hp ~= p.armour then
        local f = io.open(TRACE, "a")
        if f then
          f:write(string.format("%d %d\n", tick, p.armour))
          f:close()
        end
        last_hp = p.armour
      end
      break
    end
  end
end
