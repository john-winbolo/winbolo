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
-- 36, 72, 108, 180, 216, 252, 288 — 144 and 324 left open), and 10 DEAD
-- neutral pills parked out on the old ring at r=26 — dead on the
-- ground for the attackers' engineering. Those 10 are HIDDEN (off the
-- map entirely) except for the last few seconds before each wave: see
-- PILL_REVEAL_LEAD_TICKS.
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
-- ONE attacker owns the horde's whole estate for a wave: the first one
-- ashore takes all eight shore bases and every free outer pill and keeps
-- them until the wave leaves (stamp_wave_owner). Ownership therefore
-- changes hands exactly once per wave instead of once per arrival. The
-- ocean starts are still per-spoke (on_choose_start pins slot p to start
-- 22-p), so the attackers still come ashore spread evenly around the ring.
--
-- Round flow:
--   * on_setup (the SILENT pre-snapshot tick) deals center bases and
--     their pills round-robin to the defenders actually seated in
--     slots 0..5 — human or bot, so the host can stack their own team
--     with lobby bots for testing — with no newswire spam.
--   * 60 s of grace to dig in, called out at the start and again with
--     30 s and 10 s left, then wave 1. At the START OF EVERY WAVE the 8
--     shore bases flip back to their bots (humans may capture them
--     between waves — engine ownership rules apply mid-wave, including
--     the auto-neutralize when a wave bot dies).
--   * A wave's attackers arrive ONE AT A TIME, one second apart, and
--     leave the same way (see SPAWN_SPACING_TICKS) — spawning ten bots
--     in one tick froze the server long enough to knock remote players
--     off their own tanks.
--   * Wave bots RESPAWN like normal Bolo play (at their own outer
--     start, fully armed) — each wave is 5 minutes of constant
--     pressure, ended only by the clock: when it runs out every
--     attacker vanishes (over a few seconds). Survive all 5 waves and the
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

-- Script DEBUG chatter. The per-tick terrain reports ("[forest] ring
-- r=... planted=..." and "[terrain] shallow rim: ...") are development
-- traces, not news, and the forest one repeats on the tree-ring clock all
-- round -- turned off so the newswire carries only what a player needs.
-- The [pills] / [bases] / "*** ... ***" lines are unaffected.
local DEBUG_MESSAGES = false

local WAVES        = 5
local WAVE_TEAM    = 2      -- the enemy side IS lobby Team 2
-- Wave size AND names come from Team 2's roster as the round begins:
-- the server seeded enemy_bots() lobby bots onto it, the host may have
-- added/removed/renamed some, and on_setup reads the final list ONCE.
local WAVE_SIZE    = 10     -- fallback when no roster exists (harness)
local WAVE_NAMES   = {}     -- roster names, reused for every wave
local GRACE_TICKS  = 1500   -- 30 s of prep before wave 1 (user: "30 seconds of prep")
local BREATHER     = 1500   -- 30 s preparation between waves
local WAVE_LIMIT   = 15000  -- 5 min: leftover attackers vanish at this mark

-- Countdown warnings before a wave lands, in ticks-left order. The 60 s
-- grace gets both ("in 30 seconds", "in 10 seconds"); a 30 s breather
-- only gets the 10 s one, because a warning that would land on the very
-- tick the countdown starts is dropped — "Wave N survived! 30 second
-- preparation for wave N+1" has just said the same thing. See warn_gap.
local WAVE_WARN_TICKS = { 1500, 500 }   -- 30 s, 10 s

-- The wave's dead ring pills are kept OFF THE MAP (game.hide_pill) and
-- put back this many ticks before the wave lands. Andrew's reason,
-- verbatim: don't spawn those enemy dead pillboxes until a few seconds
-- before the AI swarm is spawned, "otherwise humans will run and pick
-- them up".
--
-- 3 s is long enough that the pills are on every client's map before the
-- first attacker comes ashore and short enough that nobody can drive out
-- from the center and scoop one. They sit NEUTRAL for those 3 seconds —
-- the wave's first attacker stamps them on the wave tick
-- (stamp_wave_owner), not before, because no attacker exists yet.
local PILL_REVEAL_LEAD_TICKS = 150   -- 3 s before the wave tick

-- Wave bots arrive and leave ONE AT A TIME, this many ticks apart, instead
-- of all ten inside a single tick.
--
-- Why: game.spawn_bot loads that bot's brain right there and then, and one
-- brain load takes about 75 ms (measured). Ten of them in one tick stops the
-- whole server for about three quarters of a second. The server then runs its
-- catch-up loop to make up the ticks it missed, which goes out to clients as
-- one burst of packets; while that happens the stall-advance path invents
-- inputs for any player whose real ones haven't arrived, and when his real
-- inputs do turn up they are too old to use and get dropped. A player on a
-- slow link (a quarter of a second round trip) therefore sees his tank freeze
-- and drive itself for several seconds at every wave. Clearing a wave is just
-- as bad or worse: each removal tears down a brain and a client sim, and the
-- round then has to push a full resync.
--
-- Spreading the work fixes it: at most one brain load (or one teardown) lands
-- in any one tick, so no tick costs more than a single bot's worth of work.
-- One second apart means a full wave of ten files onto the field over about
-- nine seconds, and files off it the same way.
local SPAWN_SPACING_TICKS  = 50   -- 1 s between one wave arrival and the next
local VANISH_SPACING_TICKS = 50   -- 1 s between one wave removal and the next

-- Newswire mute window around wave churn. Andrew's reason, verbatim in
-- spirit: a wave arriving or leaving fires ten "has joined" / "has quit"
-- newswire lines in a row and buries everything else, so the newswire is
-- silenced completely on every client and on the server for the whole of
-- it -- but the wave WARNING must still show, which it does because
-- game.message is server text and the mute only covers ENGINE-generated
-- newswire. Off 2 s before the first tank moves, back on 2 s after the
-- last one lands (or leaves).
local NEWSWIRE_MUTE_LEAD_TICKS = 100   -- 2 s of silence before the churn
local NEWSWIRE_MUTE_TAIL_TICKS = 100   -- 2 s of silence after it

-- Waves fielded entirely as PILL SUICIDERS: every bot in a listed wave is
-- spawned with the brain init argument "suicider", which forces GoalHunter's
-- pill_suicider role on for that one bot — it charges pillboxes and refuels
-- and pays a heavy cost surcharge on everything else. Unlisted waves spawn
-- plain and keep the brain's own fractional designation.
--
-- EMPTY on purpose (Andrew, 2026-09-05: "take out the two rounds with full
-- suicides"). Waves 2 and 4 used to be all-suicider and it made those two
-- rounds play as one long pill rush instead of a fight. The mechanism, the
-- "suicider" token and the wave banner's suicider hint all stay in place —
-- put a wave number back in this table and it fires again. Note that every
-- wave still designates WAVE_BLITZ_MIN_SUICIDERS suiciders inside a blitz;
-- what is gone is the WHOLE WAVE being suiciders.
local SUICIDER_WAVES = {}

-- Brain init tokens every wave bot is spawned with. They ride
-- BRAIN_INIT_ARG and are parsed by the brain (brains/GoalHunter_1.7);
-- a brain that doesn't know a token ignores it.
local WAVE_PORTFOLIO = "0/25/75"  -- back/front/aggressive pill share the wave bots aim for
local WAVE_BLITZ_MIN = 2          -- minimum tanks in a blitz (counting the commander); below this the commander gives the take up rather than charging short
local WAVE_BLITZ_MAX = 4          -- most tanks one blitz accepts (counting the commander); a call at 4 refuses a 5th
local WAVE_BLITZ_MIN_BY_WAVE = { [2] = 3, [4] = 3 }  -- per-wave override of WAVE_BLITZ_MIN (user: waves 2 and 4 need at least 3 per blitz)
local WAVE_BLITZ_MIN_SUICIDERS = 1  -- at GO the blitz commander designates random soldiers until at least this many of the party are pill_suiciders
local WAVE_BLITZ_MIN_SUICIDERS_BY_WAVE = { [2] = 4, [4] = 4 }  -- per-wave override (user: on waves 2 and 4 everyone in a blitz is a suicider; 4 = the party max, so every member gets designated)
local WAVE_NOBLITZ = { [3] = true }  -- waves whose bots never blitz at all (solo takes only; user: no blitzing on wave 3) -- brain token `noblitz`
local WAVE_NOCLAIM = { [1] = true, [2] = true }  -- waves whose bots ignore allies' claims on live pills, so several attack the same pill and draw its fire (user) -- brain token `noclaim`
local WAVE_REFUEL_MULT = 1.2      -- all refuel costs x1.2 for wave bots: attackers go back for supplies less readily than they would in a normal game
local WAVE_REFUEL_MULT_BY_WAVE = { [2] = 100, [4] = 100 }  -- per-wave override (user: waves 2 and 4 refuel at 100x, i.e. effectively never refuel)

-- "portfolio=...;blitz=MIN/MAX;blitzsuiciders=N;refuel=X", with "suicider;" in
-- front on a suicider wave. Takes the wave number rather than reading the `wave`
-- upvalue: this sits above `local wave` in the file, so reading it here would
-- find the (nil) global and every wave would look non-suicider.
local function wave_init_arg(w)
  local blitz_min = WAVE_BLITZ_MIN_BY_WAVE[w] or WAVE_BLITZ_MIN
  local refuel_mult = WAVE_REFUEL_MULT_BY_WAVE[w] or WAVE_REFUEL_MULT
  local blitz_su = WAVE_BLITZ_MIN_SUICIDERS_BY_WAVE[w] or WAVE_BLITZ_MIN_SUICIDERS
  local arg = string.format("portfolio=%s;blitz=%d/%d;blitzsuiciders=%d;refuel=%s",
                            WAVE_PORTFOLIO, blitz_min, WAVE_BLITZ_MAX,
                            blitz_su, tostring(refuel_mult))
  if SUICIDER_WAVES[w] then arg = "suicider;" .. arg end
  if WAVE_NOBLITZ[w] then arg = "noblitz;" .. arg end
  if WAVE_NOCLAIM[w] then arg = "noclaim;" .. arg end
  return arg
end

-- Map-file layout contracts (see tests/generate_survival_map.py):
local HORDE_BASES  = 8      -- bases 1..8: the horde's shore ring (r=25)
local CENTER_FIRST = 9      -- bases 9..14 form the human center, owners 0..5
local CENTER_PILLS = 6      -- pill k (1..6) pairs center base CENTER_FIRST-1+k
local WAVE_PILLS   = 10     -- pills 7..16: the wave's dead ground pills

local wave = 0
local wave_bots = {}        -- playerNum -> true for living wave members
local next_wave_at = nil    -- tick the next wave spawns (nil = wave live)
local warn_gap = nil        -- ticks the current countdown started with
local warn_next = 1         -- next WAVE_WARN_TICKS entry still to say
local ended = false
local wave_ends_at = nil    -- tick the live wave's time runs out
local last_min_mark = nil   -- minutes-left value last announced
local half_min_said = false -- the one 30-seconds-left warning

-- The staggered ARRIVAL queue (see SPAWN_SPACING_TICKS).
local spawn_left    = 0     -- attackers still to spawn this wave
local spawn_next_at = nil   -- tick the next one spawns (nil = spawn now)
local spawn_index   = 0     -- next WAVE_NAMES entry to use
local spawned       = {}    -- slots this wave's spawns actually landed in
local spawn_fail_said = false  -- the one "no free slot" report per wave
-- Horde bases the wave's owner restocked when it took them, for the one
-- "[bases] horde restocked N" line at the end of the arrival. Ownership
-- moves ONCE per wave now, so this is a plain count.
local wave_bases_restocked = 0

-- The staggered DEPARTURE queue (see VANISH_SPACING_TICKS).
local vanishing     = false -- a wave is filing off the field right now
local vanish_queue  = {}    -- slots still to be removed, in order
local vanish_next_at = nil  -- tick the next removal fires (nil = remove now)

-- The HIDDEN wave pills (see PILL_REVEAL_LEAD_TICKS): pill number ->
-- {x=,y=} the spot to put it back on. Filled at setup with the map's
-- ring spots and at every breather with wherever the wave LEFT each
-- pill, so a pill the attackers carried inland comes back inland.
local hidden_pills = {}
local reveal_at = nil       -- tick the hidden pills come back (nil = none)

-- Newswire mute state (see NEWSWIRE_MUTE_LEAD_TICKS). newswire_muted is
-- our mirror of the server switch so we only ask for changes;
-- newswire_unmute_at is the tick the mute comes off (nil = none pending).
local newswire_muted     = false
local newswire_unmute_at = nil

local function set_newswire_mute(game, on)
  if newswire_muted == on then return end
  newswire_muted = on
  game.newswire_mute(on)
end

-- Shore-base owners AT SETUP ONLY. The eight bases sit on eight of the
-- map's ten 36-degree spokes; slot 15-i owns the base on spoke i, which is
-- also the slot whose ocean start sits on that spoke (on_choose_start pins
-- slot p to start 22-p). Base 1 is the 0 deg spoke (due east), 2 is 72,
-- 3 is 180 (due west), 4 is 252. Eight of the ten spokes are used
-- (0,36,72,108 / 180,216,252,288 deg; 144 and 324 left open).
--
-- WAVES no longer use this mapping: from the wave tick on, ONE slot (the
-- first attacker ashore) owns every shore base and every wave pill -- see
-- stamp_wave_owner. It survives only for deal_center's pre-wave-1 pass,
-- which needs the map-file owners back after the setup re-deal.
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

local function restock_quiet(game, first, last)
  local n = 0
  for b = first, last do
    if game.set_base_stock(b, BASE_FULL, BASE_FULL, BASE_FULL) then
      n = n + 1
    end
  end
  return n
end

-- Read one base back rather than quoting BASE_FULL: the log then shows the
-- engine's real ceiling (90/90/90) instead of what we asked for.
local function restock_report(game, probe, n, what)
  local bi = game.base(probe)
  game.message(string.format("[bases] %s restocked %d bases to %d/%d/%d",
    what, n, bi and bi.armour or 0, bi and bi.shells or 0,
    bi and bi.mines or 0))
end

local function restock(game, first, last, what)
  restock_report(game, first, restock_quiet(game, first, last), what)
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

-- Line every wave bot up to be removed, one per VANISH_SPACING_TICKS (see
-- the constant for why they can't all go in one tick). Returns how many
-- are queued — the message that quotes this still goes out at once, so a
-- player reads "the wave is over" the moment the clock runs out even
-- though the tanks take a few seconds to actually clear off.
--
-- A wave that is still ARRIVING must not race its own departure, so the
-- arrival queue is dropped here first: nothing new comes ashore after the
-- horde has been told to leave. (On the shipped numbers this can't
-- happen — a wave lasts 15000 ticks and arrives in 450 — but the loss
-- check and any future early clear can end a wave whenever they like.)
local function vanish_wave(game, tick)
  spawn_left = 0
  spawn_next_at = nil
  vanish_queue = {}
  for p in pairs(wave_bots) do
    vanish_queue[#vanish_queue + 1] = p
  end
  -- pairs() order is not defined; sort so the same seed removes the same
  -- bot first on every run.
  table.sort(vanish_queue)
  vanishing = true
  -- The first removal used to ride this same tick. It now waits out
  -- NEWSWIRE_MUTE_LEAD_TICKS so the newswire is already silent before the
  -- first attacker disappears -- the caller mutes on this tick.
  vanish_next_at = tick + NEWSWIRE_MUTE_LEAD_TICKS
  return #vanish_queue
end

-- Pop at most one queued removal. Returns true on the tick the LAST wave
-- bot leaves the field (an empty queue counts as drained straight away),
-- which is what the between-wave clock keys on.
local function pump_vanish_queue(game, tick)
  if not vanishing then return false end
  if #vanish_queue > 0 then
    if vanish_next_at ~= nil and tick < vanish_next_at then return false end
    local p = table.remove(vanish_queue, 1)
    game.remove_bot(p)
    wave_bots[p] = nil
    vanish_next_at = tick + VANISH_SPACING_TICKS
  end
  if #vanish_queue > 0 then return false end
  vanishing = false
  vanish_next_at = nil
  return true
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

-- Take every free wave pill OFF THE MAP until the next wave is nearly
-- here (see PILL_REVEAL_LEAD_TICKS), remembering where each one stood so
-- it can be put back on that exact tile.
--
-- The pills that qualify are the ones this script was always allowed to
-- move: dead, on the ground, not in anybody's tank, and not flying
-- defender colours (wave_pill_free — a pill the humans captured and
-- placed is theirs and stays where it is, and a BUILT pill is a manned
-- gun and stays too). A hidden pill is already in_tank, so a second
-- hide pass simply finds nothing to do.
--
-- Ownership is dropped to neutral on the way out: a hidden pill still
-- carries an owner, and the brain's own "pills I am carrying" count is
-- in_tank pills owned by me (brain_data.c). After a wave leaves these
-- are neutral anyway — the engine hands a leaver's pills to a connected
-- ally or to nobody — this just guarantees it before we set in_tank.
local function hide_wave_pills(game, why)
  local n = 0
  for pn = CENTER_PILLS + 1, CENTER_PILLS + WAVE_PILLS do
    local pi = game.pill(pn)
    if wave_pill_free(pi) then
      game.set_pill_owner(pn, nil)             -- neutral; quiet (no newswire)
      if game.hide_pill(pn) then
        hidden_pills[pn] = { x = pi.x, y = pi.y }
        n = n + 1
      end
    end
  end
  game.message(string.format(
    "[pills] %s: %d wave pill(s) off the map until the next wave", why, n))
  return n
end

-- Put the hidden pills back, DEAD on the ground, on the tiles they were
-- hidden on — the map's ring spots for the first wave, and for every
-- wave after that WHEREVER THE LAST WAVE LEFT THEM (an attacker may
-- have carried one halfway to the center and dropped it there when it
-- died), never back out on the ring.
--
-- Numeric loop, not pairs(): the same seed must reveal in the same order
-- on every run.
local function reveal_wave_pills(game)
  local n = 0
  for pn = CENTER_PILLS + 1, CENTER_PILLS + WAVE_PILLS do
    local spot = hidden_pills[pn]
    if spot ~= nil then
      if game.show_pill(pn, spot.x, spot.y) then n = n + 1 end
      hidden_pills[pn] = nil
    end
  end
  if n > 0 then
    game.message(string.format(
      "*** %d dead pillbox(es) drop into place — the horde is landing! ***",
      n))
  end
  return n
end

-- Start the clock on the next wave, `gap` ticks from `tick`, and with it
-- the two things that hang off that clock: the countdown warnings (which
-- marks apply is decided from the gap — see WAVE_WARN_TICKS) and the
-- tick the hidden wave pills come back on.
--
-- A gap SHORTER than the reveal lead (a harness with a tiny grace) puts
-- the reveal on this very tick rather than in the past, so the pills are
-- always on the map before the wave's first attacker stamps them.
local function arm_next_wave(tick, gap)
  next_wave_at = tick + gap
  warn_gap = gap
  warn_next = 1
  reveal_at = next_wave_at - PILL_REVEAL_LEAD_TICKS
  if reveal_at < tick then reveal_at = tick end
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
-- Timing: this needs every attacker's start position, so it runs ONCE,
-- in the same tick as the LAST of the wave's staggered spawns, on the
-- list of slots those spawns landed in. game.spawn_bot still creates each
-- tank synchronously (the engine fires on_choose_start during the call),
-- so game.tank(p).mx/my is the tank's real start tile for every one of
-- them by the time this runs — the tanks that came ashore earlier have
-- moved a few seconds' worth, which only shifts which nearby pill they
-- claim, never whether they get one.
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

-- Hand slot `s` everything the wave means it to own: the horde bases on
-- its spoke and the outer pills stamped to it. Called the moment that
-- slot's bot actually exists, never before (see spawn_wave).
--
-- Order matters per base: game.set_base_owner DRAINS a base every time it
-- moves between two real owners, so the top-up has to follow that base's
-- own stamp. Doing it quietly here and reporting the total once at the end
-- keeps the newswire to the single "[bases] horde restocked ..." line it
-- always had, while never leaving a base empty for an attacker that has
-- already landed on it.
-- A wave pill this script may still stamp: not carried (it is wherever its
-- tank is), and not flying DEFENDER colours (one the humans captured stays
-- theirs). NEUTRAL counts as stampable — the engine reports it as 255,
-- which is what the "> 5" test is really catching.
local function pill_stampable(pn, game)
  local pi = game.pill(pn)
  if pi == nil or pi.in_tank then return false end
  local o = pi.owner
  return o == nil or o > 5
end

-- THE WAVE'S OWNER. The first attacker ashore takes the horde's whole
-- estate for the wave: all eight shore bases and every stampable outer
-- pill, in one pass, on the tick it lands. Later arrivals stamp nothing.
--
-- This replaced a per-spoke mapping (base k to the slot whose ocean start
-- sits on the same spoke) that re-stamped a share of the estate as each of
-- the ten attackers filed in. It bought nothing a player could see (the
-- bots fight over the same ground either way) and it cost a handover per
-- arrival, each of which drains the base and has to be topped back up. One
-- owner, one handover, and nothing wave-owned is ever neutral -- a neutral
-- pill shoots at everybody, which is what the old interim pass existed to
-- avoid.
--
-- Order matters per base: game.set_base_owner DRAINS a base every time it
-- moves between two real owners, so the top-up follows the stamp.
local function stamp_wave_owner(game, s)
  local n = 0
  for k = 1, HORDE_BASES do
    game.set_base_owner(k, s)
    if restock_quiet(game, k, k) > 0 then n = n + 1 end
  end
  for pn = CENTER_PILLS + 1, CENTER_PILLS + WAVE_PILLS do
    -- Not carried (it is wherever its tank is) and not flying DEFENDER
    -- colours (one the humans captured stays theirs). NEUTRAL counts as
    -- stampable, which is what pill_stampable's "> 5" is really catching.
    if pill_stampable(pn, game) then game.set_pill_owner(pn, s) end
  end
  wave_bases_restocked = n
end

-- The wave has finished arriving: report the restock and deal the pills.
local function finish_wave_spawn(game)
  spawn_next_at = nil

  -- The last attacker of this wave has just landed: the newswire comes
  -- back NEWSWIRE_MUTE_TAIL_TICKS from now (on_tick lifts it).
  newswire_unmute_at = game.tick() + NEWSWIRE_MUTE_TAIL_TICKS

  -- No leftover pass any more: the wave's whole estate went to the first
  -- attacker on the wave tick (stamp_wave_owner), so nothing can be left
  -- pointing at a slot that failed to fill.
  restock_report(game, 1, wave_bases_restocked, "horde")

  -- The 10 outer pills (7..16) are DEAD ON THE GROUND by now: they were
  -- hidden until PILL_REVEAL_LEAD_TICKS before this wave's tick and put
  -- back on the tiles they were hidden on — the map's ring spots (r=26,
  -- where the horde's bases used to sit) for wave 1, and wherever the
  -- previous wave left them for every wave after. The stamping above put
  -- them in the wave's slots, so the attackers' brains treat them as
  -- their own dead pills — scoop, carry, place, repair, at the AI's
  -- discretion. One per tank is deliberately left lying there for exactly
  -- that reason; the SURPLUS (a short roster can't cover 10) is loaded
  -- into tanks instead of being left as defender loot. Ownership is fresh
  -- above, so the free/defender test inside reads this wave's state.
  deal_wave_pills(game, spawned)
end

-- Bring at most ONE attacker ashore, no more often than every
-- SPAWN_SPACING_TICKS. Called from on_tick; the first call for a wave
-- happens on the wave's own tick, so wave 1 still starts on time.
local function pump_spawn_queue(game, tick)
  if spawn_left <= 0 then return end
  if spawn_next_at ~= nil and tick < spawn_next_at then return end

  spawn_index = spawn_index + 1
  spawn_left = spawn_left - 1
  local name = WAVE_NAMES[spawn_index]
    or string.format("Wave %d-%d", wave, spawn_index)
  -- 5th argument = the brain's BRAIN_INIT_ARG, per bot: the wave's
  -- portfolio and blitz settings, plus "suicider" on a suicider wave (which
  -- forces GoalHunter's pill_suicider role on for that bot instead of
  -- letting it designate its own harassers). See wave_init_arg.
  local p = game.spawn_bot(name, nil, WAVE_TEAM, "open", wave_init_arg(wave))
  if p then
    wave_bots[p] = true
    spawned[#spawned + 1] = p
    -- The FIRST attacker ashore takes the wave's whole estate: the eight
    -- shore bases and every stampable outer pill, kept until the wave
    -- leaves. Stamped here, once the bot exists -- stamping ahead of time
    -- would leave the bases owned by an empty slot, hostile to both sides,
    -- for the seconds the arrival takes. Later arrivals own nothing of
    -- their own. See stamp_wave_owner.
    if #spawned == 1 then stamp_wave_owner(game, p) end
  elseif not spawn_fail_said then
    -- Every slot is taken. Skip this attacker and carry on with the rest;
    -- say so once per wave rather than once per failed spawn.
    spawn_fail_said = true
    game.message(string.format(
      "[wave] wave %d: no free player slot — the wave lands short-handed.",
      wave))
  end

  if spawn_left > 0 then
    spawn_next_at = tick + SPAWN_SPACING_TICKS
  else
    finish_wave_spawn(game)
  end
end

-- Open a wave: do the bookkeeping and QUEUE the arrivals. No bot is
-- created here — pump_spawn_queue brings them in one at a time from
-- on_tick (see SPAWN_SPACING_TICKS).
local function spawn_wave(game)
  wave = wave + 1

  -- Every wave opens with the horde's eight back in bot hands (whatever
  -- the humans captured since the last one) and with the outer pills the
  -- defenders didn't claim back in the wave's hands too -- a built one
  -- flips allegiance and mans up against the humans again. Both happen in
  -- one go when the wave's FIRST attacker lands: see stamp_wave_owner,
  -- called from pump_spawn_queue. Nothing is worked out up front any
  -- more, because there is no per-slot share to work out.

  spawned = {}
  spawn_index = 0
  spawn_left = WAVE_SIZE
  spawn_next_at = nil          -- the first attacker rides the wave's own tick
  spawn_fail_said = false
  wave_bases_restocked = 0

  wave_ends_at = game.tick() + WAVE_LIMIT
  last_min_mark = nil
  half_min_said = false

  -- Announced up front, on the wave's own tick, off the roster size: the
  -- warning is what the players act on, and it would be useless arriving
  -- nine seconds after the first tank came ashore. (The old code quoted
  -- the number that had just spawned; a spawn that finds no free slot now
  -- reports itself separately, from pump_spawn_queue.)
  game.message(string.format("*** Wave %d/%d: %d attackers inbound!%s ***",
                             wave, WAVES, WAVE_SIZE,
                             SUICIDER_WAVES[wave] and
                               " Scouts report this lot have no interest in"
                               .. " your bases... guard your pillboxes!" or ""))
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
  if DEBUG_MESSAGES then
    game.message(string.format(
      "[forest] ring r=%d (%d tiles): %d picks -> planted=%d already=%d"
      .. " blocked=%d",
      TREE_RING_R, n, TREE_RING_PICKS, planted, standing, blocked))
  end
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
  if DEBUG_MESSAGES then
    game.message(string.format(
      "[terrain] shallow rim: %d coast tiles deep->river, %d spared"
      .. " within r=%d of center", #conv, spared, RIM_EXCLUDE_R))
  end
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

  -- The wave's 10 ring pills go OFF THE MAP right here, after the center
  -- deal (which only touches pills 1..6) and before any client sees the
  -- world — so they ride the baseline snapshot and the round simply
  -- OPENS without them. They come back 3 s before wave 1 lands. Andrew's
  -- reason: a full minute of grace with ten free dead pillboxes lying on
  -- the shore is a minute the defenders spend driving out to collect
  -- them. See PILL_REVEAL_LEAD_TICKS.
  hide_wave_pills(game, "setup")

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

  -- Lift a pending newswire mute. Deliberately ahead of every early
  -- return below (and of the loss check's end_round) so the mute always
  -- comes off on its own clock, whatever the round is doing.
  if newswire_unmute_at ~= nil and tick >= newswire_unmute_at then
    newswire_unmute_at = nil
    set_newswire_mute(game, false)
  end

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
    newswire_unmute_at = nil
    set_newswire_mute(game, false)
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
    arm_next_wave(tick, GRACE_TICKS)
    return
  end

  if next_wave_at ~= nil then
    -- Newswire off NEWSWIRE_MUTE_LEAD_TICKS before the wave lands, so the
    -- ten "has joined" lines the staggered arrival would otherwise write
    -- never appear. A wave clock set closer than the lead (a harness with
    -- a tiny grace) mutes on the first tick it is inside the window. The
    -- "inbound!" warning below is server text and shows regardless.
    if tick >= next_wave_at - NEWSWIRE_MUTE_LEAD_TICKS then
      newswire_unmute_at = nil
      set_newswire_mute(game, true)
    end
    -- Countdown to the next wave: every WAVE_WARN_TICKS mark that is
    -- actually inside this countdown, in order. A tick that skips past
    -- two marks at once says both, oldest first.
    while warn_next <= #WAVE_WARN_TICKS do
      local w = WAVE_WARN_TICKS[warn_next]
      if warn_gap ~= nil and w >= warn_gap then
        warn_next = warn_next + 1        -- countdown never had that long left
      elseif tick >= next_wave_at - w then
        warn_next = warn_next + 1
        game.message(string.format("*** Wave %d incoming in %d seconds! ***",
                                   wave + 1, w / 50))
      else
        break
      end
    end
    -- The wave's dead pills come back PILL_REVEAL_LEAD_TICKS before the
    -- wave itself. Ahead of the spawn below on purpose: on a harness
    -- whose grace is shorter than the lead both land on the same tick,
    -- and the pills must be on the map before stamp_wave_owner runs.
    if reveal_at ~= nil and tick >= reveal_at then
      reveal_at = nil
      reveal_wave_pills(game)
    end
    if tick >= next_wave_at then
      next_wave_at = nil
      spawn_wave(game)
      pump_spawn_queue(game, tick)   -- first attacker lands on the wave tick
    end
    return
  end

  -- A wave is live: bring in whatever of it is still arriving, narrate the
  -- clock, keep the roster pruned, and vanish every attacker once
  -- WAVE_LIMIT runs out. Deaths don't end a wave any more — the attackers
  -- respawn at their outer starts (fully armed) and press until the clock
  -- says otherwise.
  pump_spawn_queue(game, tick)

  living(game)

  if wave_ends_at ~= nil then
    local remaining = wave_ends_at - tick
    if remaining <= 0 then
      wave_ends_at = nil
      -- Mute FIRST; vanish_wave then holds its first removal for
      -- NEWSWIRE_MUTE_LEAD_TICKS, so the newswire is already silent by the
      -- time the first attacker disappears.
      newswire_unmute_at = nil
      set_newswire_mute(game, true)
      local n = vanish_wave(game, tick)
      game.message(string.format(
        "*** Wave %d is over — %d attacker(s) vanish! ***", wave, n))
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

  -- One attacker leaves per VANISH_SPACING_TICKS; the wave only counts as
  -- OVER on the tick the last one is actually gone. The between-wave clock
  -- is started from there rather than from the "wave is over" message, so
  -- the breather is a full BREATHER of empty field instead of one that
  -- starts while ten tanks are still driving around. (With BREATHER at
  -- 1500 ticks and the drain at 450 the difference is small, but it also
  -- guarantees the next wave can never begin arriving while the last one
  -- is still leaving.)
  local wave_over = pump_vanish_queue(game, tick)

  if wave_over then
    -- The last attacker has left: the newswire comes back
    -- NEWSWIRE_MUTE_TAIL_TICKS later (lifted at the top of on_tick). The
    -- breather still keys off THIS tick, exactly as before -- it just
    -- arrives NEWSWIRE_MUTE_LEAD_TICKS later than it used to, because the
    -- departure now starts that much after the wave clock ran out.
    newswire_unmute_at = tick + NEWSWIRE_MUTE_TAIL_TICKS
    if wave >= WAVES then
      ended = true
      newswire_unmute_at = nil
      set_newswire_mute(game, false)
      game.end_round(string.format(
        "*** All %d waves survived — the defenders win! ***", WAVES))
    else
      -- The field is empty: take the wave's leftover dead pills off the
      -- map for the breather (whatever the defenders didn't capture and
      -- place), then start the clock — arm_next_wave sets the reveal for
      -- 3 s before wave N+1, at the tiles they are standing on right now.
      hide_wave_pills(game, string.format("wave %d over", wave))
      arm_next_wave(tick, BREATHER)
      game.message(string.format(
        "*** Wave %d survived! %d second preparation for wave %d. ***",
        wave, BREATHER / 50, wave + 1))
    end
  end
end
