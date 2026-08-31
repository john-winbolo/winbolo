-- =========================================================================
-- Survival — scripted map scenario (runs on the SERVER).
--
-- Up to 6 human defenders hold the center of a circular island against
-- 5 waves of 10 AI tanks. Humans play under TOURNAMENT rules
-- (scenario.game forces it whatever the lobby says); wave bots always
-- spawn as full OPEN-mode tanks.
--
-- The map itself carries the geometry: 6 center bases owned by slots
-- 0..5 hugging the spawn puddle, 6 DEAD pills just beyond them (the
-- defenders' starting pills — scoop, place, repair), 8 horde bases
-- ringing the shore at r=25 on eight of the ten 36-degree spokes (0,
-- 36, 72, 108, 180, 216, 252, 288 — 144 and 324 left open), owned by
-- slots 15..12 and 10..7, and 10 DEAD neutral pills parked out on the
-- old ring at r=26, stamped to the wave at round start — dead on the
-- ground for the attackers' engineering.
--
-- History: the horde first held a full ring of 10 bases at r=26 that
-- never got fought over, then 4 forward bases at r=13 inside the
-- collision zone. The ring came back (2026-08-31) once the brains grew
-- an INFLUENCE TAIL: each base's claim now reaches ~20 tiles inland,
-- so eight shore bases project a continuous hostile claim that meets
-- the defenders' core and draws a front line around the island — the
-- bots fight for the ground between, instead of for bases that never
-- mattered.
--
-- Each base is owned by the slot whose ocean start sits on the SAME
-- spoke (on_choose_start pins slot p to start 22-p), so its bot comes
-- ashore pointing straight at its own base. The other two attackers
-- hold no base.
--
-- Round flow:
--   * on_setup (the SILENT pre-snapshot tick) deals center bases and
--     their pills round-robin to the defenders actually seated in
--     slots 0..5 — human or bot, so the host can stack their own team
--     with lobby bots for testing — with no newswire spam.
--   * ~10 s grace, then wave 1. At the START OF EVERY WAVE the 8
--     shore bases flip back to their bots (humans may capture them
--     between waves — engine ownership rules apply mid-wave, including
--     the auto-neutralize when a wave bot dies).
--   * Wave bots RESPAWN like normal Bolo play (at their own outer
--     start, fully armed) — each wave is 5 minutes of constant
--     pressure, ended only by the clock: when it runs out every
--     attacker vanishes on the spot. Survive all 5 waves and the
--     defenders win — that is the ONLY win (allow_base_win below turns
--     the engine's all-bases sweep off, so clearing the horde's eight
--     mid-wave doesn't cut the game short). The only loss is the
--     instant all-6-inner-bases check in on_tick.
--
-- Script surface reference: src/server/scenario.h
-- =========================================================================

scenario = {
  name = "Survival",
  description = "Co-op survival: hold the island's center through 5 waves "
    .. "of AI attackers storming in from the outer ring. Hold your bases, "
    .. "grab the dead pillboxes, fort up in the forest — win by keeping "
    .. "the enemy from taking over all 6 inner bases.",
  max_players = 6,     -- humans; 10 slots stay free for the wave (6+10=16)
  default_brain = "brains/GoalHunter_1.7/init.lua",  -- wave bot AI
  game = "tournament", -- humans farm; bots override per-spawn below
}

-- Lobby-behavior QUERY HOOKS — the engine calls these (methods, not
-- constants), so the answers can depend on live lobby state via
-- game.lobby_slot(p).

-- How many enemy bots the server seeds onto lobby Team 2 when this map
-- is picked. They arrive as REAL lobby bots (pool-named, editable with
-- the normal team controls); on_setup below reads the final roster.
function enemy_bots(game)
  return 10
end

-- Survival is strictly two-sided: defenders (Team 1) vs the horde
-- (Team 2). No extra teams.
function show_add_team_button(game)
  return false
end

-- The engine's classic all-bases sweep is OFF: taking the horde's
-- shore bases mid-wave must not end the round early (the win is
-- outlasting all 5 waves, nothing else), and the loss is the script's
-- own instant check on the 6 inner bases — far stricter than a
-- full-map sweep anyway.
--
-- This matters MORE now that the horde is down to 4 bases: the map
-- carries 10 bases in total, so defenders holding their 6 and taking
-- the horde's 4 during a breather would be a full sweep. The hook is
-- what keeps that a good position instead of an accidental win.
function allow_base_win(game)
  return false
end

-- Roster-change hook: the horde is HARD-CAPPED at 10 bots. If the host
-- stuffs Team 2 past that, the extras are removed on the spot (highest
-- slots first). Humans are untouchable by construction — the removal
-- API refuses them — and the engine audits this hook's work afterwards.
local HORDE_CAP = 10
function on_lobby(game)
  local horde = {}
  for p = 0, game.max_tanks() - 1 do
    local ls = game.lobby_slot(p)
    if ls ~= nil and ls.bot and ls.team == 2 then
      horde[#horde + 1] = p
    end
  end
  for i = #horde, HORDE_CAP + 1, -1 do
    game.lobby_remove_bot(horde[i])
  end
end

local WAVES        = 5
local WAVE_TEAM    = 2      -- the enemy side IS lobby Team 2
-- Wave size AND names come from Team 2's roster as the round begins:
-- the server seeded enemy_bots() lobby bots onto it, the host may have
-- added/removed/renamed some, and on_setup reads the final list ONCE.
local WAVE_SIZE    = 10     -- fallback when no roster exists (harness)
local WAVE_NAMES   = {}     -- roster names, reused for every wave
local GRACE_TICKS  = 500    -- 10 s before wave 1
local BREATHER     = 1500   -- 30 s preparation between waves
local ANNOUNCE_GAP = 250    -- final "incoming" warning this many ticks early
local WAVE_LIMIT   = 15000  -- 5 min: leftover attackers vanish at this mark

-- Map-file layout contracts (see tests/generate_survival_map.py):
local HORDE_BASES  = 8      -- bases 1..8: the horde's shore ring (r=25)
local CENTER_FIRST = 9      -- bases 9..14 form the human center, owners 0..5
local CENTER_PILLS = 6      -- pill k (1..6) pairs center base CENTER_FIRST-1+k
local WAVE_PILLS   = 10     -- pills 7..16: the wave's dead ground pills

local wave = 0
local wave_bots = {}        -- playerNum -> true for living wave members
local next_wave_at = nil    -- tick the next wave spawns (nil = wave live)
local announced = false
local ended = false
local wave_ends_at = nil    -- tick the live wave's time runs out
local last_min_mark = nil   -- minutes-left value last announced
local half_min_said = false -- the one 30-seconds-left warning

-- Which wave slot owns each shore base. Not arithmetic any more: the
-- eight bases sit on eight of the map's ten 36-degree spokes, and each one
-- belongs to the slot whose ocean start sits on the SAME spoke, so
-- on_choose_start (slot p -> start 22-p) lands that bot pointing at its
-- own base. Base 1 is the 0 deg spoke (due east), 2 is 72, 3 is 180
-- (due west), 4 is 252.
-- 8 of the 10 spokes (0,36,72,108 / 180,216,252,288 deg; 144 and 324 left
-- open): slot 15-i owns the base on spoke i.
local HORDE_BASE_SLOT = { 15, 14, 13, 12, 10, 9, 8, 7 }
local function horde_base_slot(k)
  return HORDE_BASE_SLOT[k]
end

-- Every re-deal below has to be followed by one of these.
--
-- game.set_base_owner DRAINS a base whenever it moves it from one
-- non-neutral owner to another — armour, shells and mines all to zero,
-- that is the engine's capture rule and it does not care that the
-- "capture" came from a script. This map re-deals ownership constantly
-- (the center at setup, the horde's eight at setup AND at the top of
-- every wave), so without this the round opened on empty bases and the
-- horde's bases were wiped clean again every five minutes.
--
-- BASE_FULL is deliberately past the engine's 90: game.set_base_stock
-- clamps each value to the real maximum, so "a big number" means "full"
-- without this file having to track the engine's constant.
local BASE_FULL = 255

local function restock(game, first, last, what)
  local n = 0
  for b = first, last do
    if game.set_base_stock(b, BASE_FULL, BASE_FULL, BASE_FULL) then
      n = n + 1
    end
  end
  -- Read one back rather than quoting BASE_FULL: the log then shows the
  -- engine's real ceiling (90/90/90) instead of what we asked for.
  local bi = game.base(first)
  game.message(string.format("[bases] %s restocked %d bases to %d/%d/%d",
    what, n, bi and bi.armour or 0, bi and bi.shells or 0,
    bi and bi.mines or 0))
end

-- Wave bots respawn like normal Bolo play — a dead one is merely
-- between lives, so nobody is removed here. This just prunes slots
-- that vanished outside our control (kicks) and reports how many of
-- the wave's tanks are alive right now (the between-lives dip is why
-- status messages don't quote this number).
local function living(game)
  local n = 0
  for p in pairs(wave_bots) do
    local t = game.tank(p)
    if t == nil then
      wave_bots[p] = nil
    elseif not t.dead then
      n = n + 1
    end
  end
  return n
end

-- Remove every wave bot on the spot (single tick — no drive-away).
local function vanish_wave(game)
  local n = 0
  for p in pairs(wave_bots) do
    game.remove_bot(p)
    wave_bots[p] = nil
    n = n + 1
  end
  return n
end

-- A wave pill this script is still allowed to move around: one of
-- pills 7..16, DEAD on the ground (armour 0 — the scoopable state), not
-- being carried, and not flying defender colours. A built pill stays
-- where it stands (it is a manned gun now, whoever's it is) and a pill
-- the defenders captured during the break stays theirs.
local function wave_pill_free(pi)
  if pi == nil or pi.in_tank then return false end
  if pi.armour ~= 0 then return false end
  local o = pi.owner
  if o ~= nil and o <= 5 then return false end
  return true
end

-- ONE claimable pill per attacker, and not a crumb more.
--
-- The map parks 10 dead pills out on the ring, but the wave is only as
-- big as Team 2's roster: at WAVE_SIZE 6 the other four just lie there
-- in no-man's land, and defenders were driving out mid-wave to scoop
-- free pillboxes that were never meant for them. So the wave's dead
-- pills are dealt like a hand of cards the moment the tanks exist:
--   * CLAIM — each attacker, in spawn order, takes the nearest free
--     pill still on the table. It stays ON THE GROUND: the bot's own
--     brain sees a dead pill under its nose and goes and gets it, which
--     is the emergent field engineering this map wants.
--   * DISTRIBUTE — everything left over after every attacker has one is
--     loaded straight INTO the tanks (round-robin, so the surplus
--     spreads evenly), off the ground and out of defender reach.
-- With more attackers than pills the claim pass simply runs dry and
-- there is nothing to distribute.
--
-- Timing: this runs in the SAME tick as the spawn, right after the
-- spawn loop. game.spawn_bot creates the tank synchronously (the engine
-- fires on_choose_start during the call), so game.tank(p).mx/my is
-- already the tank's real start tile here — no deferred pass needed.
local function deal_wave_pills(game, spawned)
  if #spawned == 0 then return end

  local pool = {}
  for n = CENTER_PILLS + 1, CENTER_PILLS + WAVE_PILLS do
    local pi = game.pill(n)
    if wave_pill_free(pi) then
      pool[#pool + 1] = { n = n, x = pi.x, y = pi.y }
    end
  end

  local claimed = 0
  for _, p in ipairs(spawned) do
    if #pool == 0 then break end
    local t = game.tank(p)
    if t ~= nil then
      local best, best_d = nil, nil
      for i, e in ipairs(pool) do
        local dx, dy = e.x - t.mx, e.y - t.my
        local d = dx * dx + dy * dy        -- squared: ordering is all we need
        if best_d == nil or d < best_d then best, best_d = i, d end
      end
      table.remove(pool, best)
      claimed = claimed + 1
    end
  end

  local loaded, turn = 0, 0
  for _, e in ipairs(pool) do
    local p = spawned[(turn % #spawned) + 1]
    turn = turn + 1                        -- advance even if the load is
    if game.give_pill(p, e.n) then         -- refused, so the spread stays even
      loaded = loaded + 1
    end
  end

  game.message(string.format(
    "[pills] wave %d: %d claimed on ground, %d loaded into tanks",
    wave, claimed, loaded))
end

local function spawn_wave(game)
  wave = wave + 1

  -- Every wave opens with the horde's eight back in bot hands — whatever
  -- the humans captured since the last one. Clamp into the slots this
  -- wave actually fields (same rule as the pill pass below): with a
  -- shrunken roster the natural owner slot may be EMPTY, and a base
  -- owned by a nonexistent player reads hostile to BOTH sides — all
  -- eight must stay horde no matter how few attackers spawn. Several
  -- can land on the same slot once the roster is short enough; a slot
  -- owning two bases is fine, a base owned by nobody is not.
  local lowest_slot = 16 - WAVE_SIZE
  for k = 1, HORDE_BASES do
    local s = horde_base_slot(k)
    if s < lowest_slot then s = lowest_slot end
    game.set_base_owner(k, s)
  end
  -- ...and undo the drain that re-deal just caused. A wave arriving to
  -- empty bases had nothing to rearm from, which is not the fight this
  -- map is supposed to be.
  restock(game, 1, HORDE_BASES, "horde")

  -- Same for the wave's pills: any of pills 7..16 the DEFENDERS didn't
  -- claim (still built somewhere from a previous wave, or dropped
  -- neutral by a vanished attacker) goes back to this wave's
  -- ownership — a built one flips allegiance and mans up against the
  -- humans again. Defender-owned pills (captured during the break)
  -- stay theirs; carried pills are wherever their tank is.
  for n = CENTER_PILLS + 1, CENTER_PILLS + WAVE_PILLS do
    local pi = game.pill(n)
    if pi and not pi.in_tank then
      local o = pi.owner
      if o == nil or o > 5 then
        -- pill 7 -> slot 15 ... 16 -> 6, clamped into the slots this
        -- wave actually fields when the host shrank the enemy roster.
        local s = 22 - n
        local lowest = 16 - WAVE_SIZE
        if s < lowest then s = lowest end
        game.set_pill_owner(n, s)
      end
    end
  end

  local spawned = {}
  for i = 1, WAVE_SIZE do
    local name = WAVE_NAMES[i] or string.format("Wave %d-%d", wave, i)
    local p = game.spawn_bot(name, nil, WAVE_TEAM, "open")
    if p then
      wave_bots[p] = true
      spawned[#spawned + 1] = p
    end
  end

  -- The 10 outer pills (7..16) start DEAD ON THE GROUND, parked at
  -- their map spots out on the old ring (r=26) the horde's bases used
  -- to sit on — the attackers' first stop ashore. The wave-start
  -- ownership pass above stamps them to the wave's slots, so the
  -- attackers' brains treat them as their own dead pills — scoop, carry,
  -- place, repair, at the AI's discretion. One per tank is deliberately
  -- left lying there for exactly that reason; the SURPLUS (a short roster
  -- can't cover 10) is loaded into tanks instead of being left as
  -- defender loot. Ownership is fresh above, so the free/defender test
  -- inside reads this wave's state.
  deal_wave_pills(game, spawned)

  wave_ends_at = game.tick() + WAVE_LIMIT
  last_min_mark = nil
  half_min_said = false

  game.message(string.format("*** Wave %d/%d: %d attackers inbound! ***",
                             wave, WAVES, #spawned))
end

-- Deterministic spawn pinning (fires for EVERY placement — initial
-- spawns and respawns, lobby-seeded and script-spawned alike). Map-file
-- start order: 1..6 center-puddle defender starts, 7..16 outer-ocean
-- enemy starts.
--
-- Placement is keyed on the slot's TEAM, not its slot number: an enemy
-- bot can end up in a LOW slot (the host adds an 11th to Team 2, the
-- horde cap removes a different one), and a slot-numbered rule would
-- have dropped it into the middle of the human keep.
--
-- INVARIANT: the puddle (starts 1..6) is DEFENDER-ONLY. Every enemy
-- branch below is pure arithmetic that always lands in 7..16, so no
-- script state, error, or odd slot number can ever put an attacker in
-- the middle of the keep. (And with no bases in tiering range of the
-- puddle, even the engine's own fallback never favours it.)
function on_choose_start(game, p)
  local ls = game.lobby_slot(p)
  local enemy
  if ls ~= nil and ls.team ~= 0 then
    enemy = (ls.team == WAVE_TEAM)
  else
    -- no roster info (or team not stamped yet): slot heuristic
    enemy = (p >= 6 and p <= 15)
  end
  if enemy then
    if p >= 6 and p <= 15 then
      -- Each of the 10 wave slots owns one ocean start, on its own
      -- 36-degree spoke. Four of those spokes carry a horde base, so
      -- slots 15/13/10/8 come ashore aimed at their own; the other six
      -- land between them and fight in.
      return 22 - p
    end
    return 7 + (p % 10)         -- low-slot enemy: still the outer ring
  end
  return 1 + (p % 6)            -- defenders: the puddle
end

-- Deal the center bases (and their paired pills) round-robin to the
-- DEFENDERS actually seated in slots 0..5 — human or lobby bot alike,
-- so the host can stack their own team with bots for testing. Returns
-- false while nobody is seated yet.
local dealt = false
local function deal_center(game)
  local defenders = {}
  for p = 0, 5 do
    if game.tank(p) ~= nil then defenders[#defenders + 1] = p end
  end
  if #defenders == 0 then return false end

  local d = 1
  for b = CENTER_FIRST, CENTER_FIRST + 5 do
    local slot = b - CENTER_FIRST           -- map-file owner of this pair
    local pill = b - CENTER_FIRST + 1       -- pill k pairs base 4+k
    local owner = slot
    local present = false
    for _, p in ipairs(defenders) do
      if p == slot then present = true end
    end
    if not present then
      owner = defenders[d]
      d = (d % #defenders) + 1
    end
    game.set_base_owner(b, owner)
    game.set_pill_owner(pill, owner)
  end
  -- The deal above drains every base it moved between two seated
  -- players, so the defenders would open the round on empty bases —
  -- no armour to repair with, no shells, no mines, in a map whose
  -- entire premise is digging in. Put it all back.
  restock(game, CENTER_FIRST, CENTER_FIRST + 5, "center")
  -- Deal the horde's eight to the horde immediately too — spawn_wave
  -- re-deals them every wave, but until wave 1 lands the map-file owners
  -- rule, and with a shrunken roster (WAVE_SIZE < 10) the natural
  -- owners of the low-slot bases are EMPTY slots: red to both sides from
  -- tick 1. Same clamp as spawn_wave's pass.
  do
    local lowest_slot = 16 - WAVE_SIZE
    for k = 1, HORDE_BASES do
      local s = horde_base_slot(k)
      if s < lowest_slot then s = lowest_slot end
      game.set_base_owner(k, s)
    end
    restock(game, 1, HORDE_BASES, "horde")
  end
  return true
end

-- Terrain codes (engine values; see src/bolo/public/global.h). map_tile
-- hands back 0..15, or DEEP_SEA for anything outside the mine border.
local T_BUILDING, T_RIVER, T_ROAD, T_FOREST = 0, 1, 4, 5
local T_HALFBUILDING, T_BOAT = 8, 9
local T_MINE_START, T_DEEP_SEA = 10, 0xFF

-- ---------------------------------------------------------------------
-- The TREE RING: the map's only forest replenishment. A one-tile-thick
-- circle at the radius of the 6 inner bases, re-seeded a few tiles at a
-- time forever. Trees therefore come back exactly where the defenders
-- are dug in — close enough to harvest under fire, far enough out that
-- the fight for the center decides whether they ever reach them. There
-- is no coverage target and no round-boundary replant: the cadence
-- below is the whole system, and it just keeps ticking.
local TREE_RING_R      = 6     -- same circle the 6 win/loss bases sit on
local TREE_RING_PERIOD = 1500  -- 30 s at the engine's 50 ticks/s
local TREE_RING_PICKS  = 6     -- ring spots drawn per replenish

local tree_ring    = {}        -- {x=,y=} ring tiles, permanent blockers cut
local next_ring_at = nil       -- tick the next replenish fires

-- Rounded radius gives a closed, single-tile-thick circle (a plain
-- dx*dx+dy*dy == R*R test leaves gaps on the diagonals). |dx| can never
-- exceed R while the rounded distance is R, so the box is exact.
-- The 6 inner bases sit ON this circle and can never take a tree, so
-- they are cut once here instead of being re-tested forever; everything
-- else that blocks planting can move or be rebuilt, so it is checked
-- live at plant time.
local function build_tree_ring(game)
  local blocked = {}
  for b = CENTER_FIRST, CENTER_FIRST + 5 do
    local bi = game.base(b)
    if bi then blocked[bi.x * 256 + bi.y] = true end
  end
  tree_ring = {}
  for x = 128 - TREE_RING_R, 128 + TREE_RING_R do
    for y = 128 - TREE_RING_R, 128 + TREE_RING_R do
      local dx, dy = x - 128, y - 128
      if math.floor(math.sqrt(dx * dx + dy * dy) + 0.5) == TREE_RING_R
         and not blocked[x * 256 + y] then
        tree_ring[#tree_ring + 1] = { x = x, y = y }
      end
    end
  end
end

-- Ground a tree will take. Walls and water obviously refuse one; a
-- MINED tile (10..15) is skipped because planting over it would eat the
-- mine someone laid, and ROAD is skipped for the same reason the old
-- planter did it — paving the defenders laid themselves must never be
-- overgrown by the script.
local function ring_plantable(t)
  if t == nil or t == T_DEEP_SEA then return false end
  if t == T_BUILDING or t == T_HALFBUILDING then return false end
  if t == T_RIVER or t == T_BOAT then return false end
  if t == T_ROAD then return false end
  if t >= T_MINE_START then return false end
  return true
end

-- Draw TREE_RING_PICKS spots WITH replacement: a duplicate draw simply
-- lands on the tile the previous one just planted and is counted as
-- already-forest, which is cheaper than tracking picks and makes the
-- per-tick yield honestly random rather than guaranteed.
local function replenish_tree_ring(game)
  local n = #tree_ring
  if n == 0 then return end

  -- Structures move: pills get scooped, carried and re-placed, and a
  -- base or pill standing on a ring tile has to be re-checked every
  -- time rather than baked into the ring at setup.
  local occupied = {}
  for b = 1, game.num_bases() do
    local bi = game.base(b)
    if bi then occupied[bi.x * 256 + bi.y] = true end
  end
  for p = 1, game.num_pills() do
    local pi = game.pill(p)
    if pi and not pi.in_tank then occupied[pi.x * 256 + pi.y] = true end
  end

  local planted, standing, blocked = 0, 0, 0
  for _ = 1, TREE_RING_PICKS do
    local s = tree_ring[math.random(n)]
    local t = game.map_tile(s.x, s.y)
    if t == T_FOREST then
      standing = standing + 1
    elseif occupied[s.x * 256 + s.y] or not ring_plantable(t) then
      blocked = blocked + 1
    else
      game.set_tile(s.x, s.y, T_FOREST)
      planted = planted + 1
    end
  end
  game.message(string.format(
    "[forest] ring r=%d (%d tiles): %d picks -> planted=%d already=%d"
    .. " blocked=%d",
    TREE_RING_R, n, TREE_RING_PICKS, planted, standing, blocked))
end

-- ---------------------------------------------------------------------
-- The SHALLOW RIM: one tile of river all the way around the island's
-- coast. Driving off the edge of a circular island is far too easy —
-- and DEEP_SEA drowns a tank outright, which is a stupid way to lose a
-- defender in the middle of a wave. A one-tile shallow lip turns that
-- mistake into a swim back ashore.
--
-- The rim is derived from TILE CONTENTS, never from the island's
-- radius: every deep tile that touches a non-deep one (8-adjacency)
-- becomes river. The map file is hand-edited, so a hardcoded circle
-- would drift off the real coastline the first time someone carves a
-- bay; the adjacency test follows whatever shape the map actually has.
--
-- EXCLUDED: the little deep puddle at the middle of the map. The six
-- human starts sit in it on boats by design, and lining it with
-- shallows would open a swimmable lane straight into the sanctuary. It
-- is the only deep water inside RIM_EXCLUDE_R of the center, so a plain
-- radius cut is enough to spare it.
--
-- The bot starts sit at r=33, three tiles clear of the r~29 coast and
-- touching no land at all, so they stay deep — the wave still arrives
-- by boat.
local RIM_EXCLUDE_R = 6        -- keep the center puddle deep (it is r~2.5)

-- Two passes on purpose, and RIVER never counts as coast. A converted
-- tile is no longer deep, so writing during the scan would let the rim
-- seed itself one tile further out on every scan step; collecting first
-- keeps every test against the map as it stands. And because the rim
-- this lays down is river, skipping river as a seed is what makes a
-- second setup a genuine no-op instead of another tile of shallows.
-- (On the shipped map the only pre-existing river is the hand-cut
-- shallow lip inside the center puddle, which the radius cut below
-- spares anyway — both rules agree on all 240 coast tiles.)
local function build_shallow_rim(game)
  local conv, seen, spared = {}, {}, 0
  local excl2 = RIM_EXCLUDE_R * RIM_EXCLUDE_R
  for x = 0, 255 do
    for y = 0, 255 do
      local t = game.map_tile(x, y)
      -- Walk out from the LAND (~2.6k tiles) instead of testing every
      -- ocean tile's neighbours (~63k of them): same rim, a fraction of
      -- the lookups on a one-shot setup pass.
      if t ~= nil and t ~= T_DEEP_SEA and t ~= T_RIVER then
        for ox = -1, 1 do
          for oy = -1, 1 do
            local nx, ny = x + ox, y + oy
            if nx >= 0 and nx <= 255 and ny >= 0 and ny <= 255
               and not seen[nx * 256 + ny] then
              seen[nx * 256 + ny] = true
              if game.map_tile(nx, ny) == T_DEEP_SEA then
                local dx, dy = nx - 128, ny - 128
                if dx * dx + dy * dy <= excl2 then
                  spared = spared + 1
                else
                  conv[#conv + 1] = { x = nx, y = ny }
                end
              end
            end
          end
        end
      end
    end
  end
  for _, c in ipairs(conv) do
    game.set_tile(c.x, c.y, T_RIVER)
  end
  game.message(string.format(
    "[terrain] shallow rim: %d coast tiles deep->river, %d spared"
    .. " within r=%d of center", #conv, spared, RIM_EXCLUDE_R))
end

-- The SILENT pre-snapshot tick: on a lobby server the round's tanks
-- already exist here, so the deal lands before any client sees the
-- world — nothing "changes alliance" on the newswire at tick 0.
-- (Lobby-less harnesses join players after ticking starts; on_tick
-- below retries the deal until someone is seated.)
function on_setup(game)
  -- Team 2's roster IS the wave: capture its size and NAMES as the
  -- round begins, then pull those bots off the field — the waves
  -- re-field them (same names, full open tanks) on the wave clock.
  -- All silent: this is the pre-snapshot tick.
  WAVE_NAMES = {}
  for p = 0, 15 do
    local ls = game.lobby_slot(p)
    if ls ~= nil and ls.bot and ls.team == WAVE_TEAM then
      WAVE_NAMES[#WAVE_NAMES + 1] = ls.name
      game.remove_bot(p)
    end
  end
  if #WAVE_NAMES > 0 then
    WAVE_SIZE = #WAVE_NAMES
  else
    local n = game.enemy_team_size()
    if n and n > 0 then WAVE_SIZE = n end
  end

  dealt = deal_center(game)

  -- The ring is fixed geometry, so it is measured once, here, off the
  -- base positions the map file actually shipped.
  math.randomseed(os.time())
  build_tree_ring(game)

  -- Shallows around the coast, laid before anyone sees the map: the
  -- terrain edit rides the baseline snapshot instead of arriving as a
  -- map-change event, so the island simply HAS a beach from tick 0.
  build_shallow_rim(game)
end

-- Nothing forest-related happens at a round boundary: whatever trees
-- the map file ships with are the trees the round opens with, and the
-- ring cadence in on_tick takes it from there.
function on_start(game)
  game.message(string.format(
    "*** SURVIVAL: dig in! First of %d waves in %d seconds. ***",
    WAVES, GRACE_TICKS / 50))
end

function on_tick(game, tick)
  if ended then return end

  -- Late deal for lobby-less harnesses (see on_setup).
  if not dealt then dealt = deal_center(game) end

  -- INSTANT LOSS: the round is over the moment no inner base is in
  -- defender hands (slots 0..5) — stolen or neutralized, the center
  -- has fallen. This is the flip side of the blurb's win condition;
  -- no need to wait for the engine's all-10-bases sweep (which is off
  -- anyway — see allow_base_win).
  local held = 0
  for b = CENTER_FIRST, CENTER_FIRST + 5 do
    local bi = game.base(b)
    if bi and bi.owner ~= nil and bi.owner >= 0 and bi.owner <= 5 then
      held = held + 1
    end
  end
  if held == 0 then
    ended = true
    game.end_round(
      "*** The center has fallen — the attackers take the island! ***")
    return
  end

  -- Tree ring, on its own steady clock. Deliberately ahead of every
  -- early return below, so it keeps its cadence through the grace
  -- period, the live waves and the breathers alike — the trees come
  -- back at the same rate no matter what the round is doing.
  if next_ring_at == nil then
    next_ring_at = tick + TREE_RING_PERIOD
  elseif tick >= next_ring_at then
    next_ring_at = tick + TREE_RING_PERIOD
    replenish_tree_ring(game)
  end

  -- Arm wave 1 off the round's first running tick.
  if wave == 0 and next_wave_at == nil then
    next_wave_at = tick + GRACE_TICKS
    return
  end

  if next_wave_at ~= nil then
    -- Countdown to the next wave.
    if not announced and tick >= next_wave_at - ANNOUNCE_GAP then
      announced = true
      game.message(string.format("*** Wave %d incoming in %d seconds! ***",
                                 wave + 1, ANNOUNCE_GAP / 50))
    end
    if tick >= next_wave_at then
      next_wave_at = nil
      announced = false
      spawn_wave(game)
    end
    return
  end

  -- A wave is live: narrate the clock, keep the roster pruned, and
  -- vanish every attacker the instant WAVE_LIMIT runs out. Deaths
  -- don't end a wave any more — the attackers respawn at their outer
  -- starts (fully armed) and press until the clock says otherwise.
  living(game)
  local wave_over = false

  if wave_ends_at ~= nil then
    local remaining = wave_ends_at - tick
    if remaining <= 0 then
      local n = vanish_wave(game)
      game.message(string.format(
        "*** Wave %d is over — %d attacker(s) vanish! ***", wave, n))
      wave_over = true
    elseif remaining <= 1500 and not half_min_said then
      half_min_said = true
      game.message(string.format(
        "*** 30 seconds left in wave %d! ***", wave))
    else
      -- Minute marks: "3 minutes left until wave 3 finishes" etc.
      local mins = math.floor((remaining + 2999) / 3000)   -- ceil, minutes
      -- first mark comes a minute in (not right on top of "inbound!")
      if remaining > 1500 and (last_min_mark == nil or mins < last_min_mark)
         and mins * 3000 < WAVE_LIMIT then
        last_min_mark = mins
        game.message(string.format(
          "*** %d minute(s) left until wave %d finishes. ***", mins, wave))
      end
    end
  end

  if wave_over then
    wave_ends_at = nil
    if wave >= WAVES then
      ended = true
      game.end_round(string.format(
        "*** All %d waves survived — the defenders win! ***", WAVES))
    else
      next_wave_at = tick + BREATHER
      game.message(string.format(
        "*** Wave %d survived! %d second preparation for wave %d. ***",
        wave, BREATHER / 50, wave + 1))
    end
  end
end
