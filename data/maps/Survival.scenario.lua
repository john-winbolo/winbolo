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
-- defenders' starting pills — scoop, place, repair), 10 outer-ring
-- bases owned by slots 15..6 (wave bots fill slots top-down, so each
-- bot is pulled to the outer start beside its own base), and 10 DEAD
-- neutral pills parked out there for the wave-1 tanks to carry in.
--
-- Round flow:
--   * on_setup (the SILENT pre-snapshot tick) deals center bases and
--     their pills round-robin to the defenders actually seated in
--     slots 0..5 — human or bot, so the host can stack their own team
--     with lobby bots for testing — with no newswire spam.
--   * ~10 s grace, then wave 1. At the START OF EVERY WAVE the 10 outer
--     bases flip back to their bots (humans may capture them between
--     waves — engine ownership rules apply mid-wave, including the
--     auto-neutralize when a wave bot dies).
--   * Wave bots RESPAWN like normal Bolo play (at their own outer
--     start, fully armed) — each wave is 5 minutes of constant
--     pressure, ended only by the clock: when it runs out every
--     attacker vanishes on the spot. Survive all 5 waves and the
--     defenders win — that is the ONLY win (allow_base_win below turns
--     the engine's all-bases sweep off, so capturing the whole ring
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
  default_brain = "brains/GoalHunter_1.6/init.lua",  -- wave bot AI
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

-- The engine's classic all-bases sweep is OFF: sweeping the outer ring
-- mid-wave must not end the round early (the win is outlasting all 5
-- waves, nothing else), and the loss is the script's own instant check
-- on the 6 inner bases — far stricter than a full-map sweep anyway.
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
local OUTER_BASES  = 10     -- bases 1..10 ring the island, base k -> slot 16-k
local CENTER_FIRST = 11     -- bases 11..16 form the human center, owners 0..5
local CENTER_PILLS = 6      -- pill k (1..6) pairs center base 10+k
local WAVE_PILLS   = 10     -- pills 7..16 ride in with the wave-1 tanks

local wave = 0
local wave_bots = {}        -- playerNum -> true for living wave members
local next_wave_at = nil    -- tick the next wave spawns (nil = wave live)
local announced = false
local ended = false
local wave_ends_at = nil    -- tick the live wave's time runs out
local last_min_mark = nil   -- minutes-left value last announced
local half_min_said = false -- the one 30-seconds-left warning

local function outer_base_slot(k)   -- base 1 -> slot 15 ... base 10 -> slot 6
  return 16 - k
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

local function spawn_wave(game)
  wave = wave + 1

  -- Every wave opens with the outer ring back in bot hands — whatever
  -- the humans captured since the last one.
  for k = 1, OUTER_BASES do
    game.set_base_owner(k, outer_base_slot(k))
  end

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

  -- Wave 1 tanks each carry one of the outer pills (7..16) into
  -- battle. From then on those pills are fair game wherever they fall
  -- — defenders that capture them keep them.
  if wave == 1 then
    for i, p in ipairs(spawned) do
      local pill = CENTER_PILLS + i
      if pill <= CENTER_PILLS + WAVE_PILLS then
        local ok, err = game.give_pill(p, pill)
        if not ok then
          game.message(string.format("Scenario warning: pill %d failed to "
                                     .. "load (%s)", pill, tostring(err)))
        end
      end
    end
  end

  wave_ends_at = game.tick() + WAVE_LIMIT
  last_min_mark = nil
  half_min_said = false

  if wave == 1 then
    game.message(string.format(
      "*** Wave %d/%d: %d attackers inbound — each carrying a pillbox! ***",
      wave, WAVES, #spawned))
  else
    game.message(string.format("*** Wave %d/%d: %d attackers inbound! ***",
                               wave, WAVES, #spawned))
  end
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
      return 22 - p             -- the outer start by "your" base
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
    local pill = b - CENTER_FIRST + 1       -- pill k pairs base 10+k
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
  return true
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
end

-- Terrain codes (engine values; see global.h).
local T_SWAMP, T_CRATER, T_ROAD, T_FOREST, T_GRASS = 2, 3, 4, 5, 7

-- Fresh forest every round: fill ~30% of the area INSIDE the ring road
-- (radius < 10 around the center, puddle excluded automatically — only
-- grass/road/crater/swamp convert) with trees, skipping the tiles under
-- bases and pills. Runs from on_start (a RUNNING tick) so every client
-- receives the changes through the normal map-delta stream; a new
-- random layout each round keeps the defenders in building material.
local function plant_core_forest(game)
  math.randomseed(os.time())
  local structures = {}
  for b = 1, game.num_bases() do
    local bi = game.base(b)
    if bi then structures[bi.x * 256 + bi.y] = true end
  end
  for n = 1, game.num_pills() do
    local pi = game.pill(n)
    if pi and not pi.in_tank then structures[pi.x * 256 + pi.y] = true end
  end
  local planted = 0
  for x = 118, 138 do
    for y = 118, 138 do
      local dx, dy = x - 128, y - 128
      if dx * dx + dy * dy < 93 then          -- strictly inside r=10 ring
        local t = game.map_tile(x, y)
        if (t == T_GRASS or t == T_ROAD or t == T_CRATER or t == T_SWAMP)
            and not structures[x * 256 + y]
            and math.random() < 0.30 then
          game.set_tile(x, y, T_FOREST)
          planted = planted + 1
        end
      end
    end
  end
  return planted
end

function on_start(game)
  plant_core_forest(game)
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
  -- no need to wait for the engine's all-16-bases sweep.
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
