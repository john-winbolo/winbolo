-- =========================================================================
-- Survival — a scripted round on Survival.map.
--
-- Up to 6 human defenders hold the centre of a circular island against
-- waves of 10 AI tanks: 5 waves of 4 minutes unless the host sets other
-- numbers in the lobby (scenario.settings below). Humans play under tournament rules (scenario.game
-- forces it whatever the lobby says); wave bots always come in on the full
-- open loadout.
--
-- The map carries the geometry: 6 centre bases hugging the spawn puddle, 6
-- dead pills just beyond them (the defenders' starting pills — scoop, place,
-- repair), and 8 horde bases ringing the shore at r=25 on eight of the ten
-- 36-degree spokes. The owners the file names for those are slot numbers and
-- mean nothing here: the round re-deals every one of them by TEAM at the
-- setup, because the lobby decides which slots a side ends up in.
--
-- The horde's own 10 pillboxes are not in the map file at all. Each is made
-- (game.add_pill) as its attacker comes ashore and loaded straight into that
-- tank, so a wave pill is never lying loose for the defenders to drive out
-- and collect. See drop_pill_for.
--
-- One attacker owns the horde's whole estate for a wave: the first one
-- ashore takes all eight shore bases and every free outer pill and keeps
-- them until the wave leaves (stamp_wave_owner). Ownership changes hands
-- once a wave instead of once an arrival. The ocean starts are still
-- per-spoke, so the attackers come ashore spread evenly around the ring.
--
-- Round flow:
--   * on_setup deals the centre bases and their pills round-robin to the
--     defenders actually on the field, whatever slots the lobby gave them,
--     lays the island's shallow rim and its tree ring, and digs in every
--     defender bot.
--   * 10 s of grace to dig in, called out at the start and again with 10 s
--     left, then wave 1. The breather between waves is still 30 s. Every
--     wave opens with the 8 shore bases back in the horde's hands.
--   * A wave's attackers arrive as fast as the roster queue drains — one op
--     a tick, two ops an attacker, so the whole wave is ashore inside half a
--     second. They leave one a second, because a departure still tears a
--     brain down where an arrival only resumes one.
--   * Wave bots respawn like ordinary play. A wave is WAVE_LIMIT_S of
--     constant pressure (4 minutes unless the lobby says otherwise), ended
--     only by the clock. Survive the last of the WAVES waves and the
--     defenders win — that is the only win, because allow_base_win turns
--     the engine's all-bases sweep off. The only loss is the instant
--     all-6-inner-bases check in on_tick.
--
-- PORTED (2026-09-15) from the old server-side host onto the scenario host
-- on main. The four differences that show in this file:
--
--   * The horde is TEN HELD SEATS, not ten lobby bots that have to be
--     pulled off the field at the round's start. scenario.lobby asks for
--     them with fielded = false, spawn_bot fields one and remove_bot hands
--     it back. The whole staggered-removal machinery the old file needed is
--     gone with it.
--   * game.tick() counts 100 a second here where the old host counted 50,
--     so every interval in this file is written in SECONDS and turned into
--     ticks by secs().
--   * There is no hide_pill/show_pill. Relocating a pill is game.move_pill,
--     which is what that pair was standing in for.
--   * There is no newswire_mute. The announce() policy answers false while
--     the mute is up, which is the same silence a line at a time.
--
-- ROUND STATE. Everything below is a chunk local — dug_in, seen_fielded,
-- arrange_deadline, dealt, the wave counters — and none of it is reset
-- anywhere, because none of it has to be: the host builds a FRESH Lua state
-- for every round and runs this file's bytes again in it (scnRoundBootLocked
-- in src/scenario/scenario_host.c, called from both round-start paths). A
-- second round on a rotation or a play-again therefore starts with every one
-- of these at its written value, and nothing the last round left behind is
-- reachable from this one. Do not add a reset in on_setup for them: it would
-- read as though the state persisted and be dead code.
--
-- The API this is written against: docs/SCENARIO_API.md.
-- =========================================================================

scenario = {
  name = "Survival",
  description = "Co-op survival: hold the island's centre through waves "
    .. "of AI attackers storming in from the outer ring, as many waves and "
    .. "as long as the host sets in the lobby. Hold your bases, grab the "
    .. "dead pillboxes, fort up in the forest.",
  api  = 1,
  -- This file ends its own round, from on_tick, when the last wave is beaten
  -- or the defenders are wiped out, so it is a scenario rather than a mod.
  -- Written out rather than left to the default, because the default is what
  -- a file that says nothing gets and this one has something to say.
  kind = "scenario",
  game = "tournament",   -- humans farm; wave bots override per spawn below
  -- The horde is held bot seats this file fields with game.spawn_bot, and
  -- a lobby set to no bots refuses every one, so the lobby must allow them.
  needs_bots = true,

  -- The lobby, declared rather than built. Six human seats on the
  -- defenders' team, ten seats HELD for the horde: they sit in the roster
  -- where a host can see and trim them, they take no tank, and no brain
  -- loads for one until a wave fields it.
  --
  -- max_bots = 10 on team 2 is the horde cap the old file enforced from a
  -- roster hook. The lobby enforces it now, so there is nothing to undo.
  lobby = {
    max_players = 6,
    teams = {
      { id = 1, bots = 0,  max_bots = 6 },
      { id = 2, bots = 10, max_bots = 10, fielded = false,
        brain = "GoalHunter_1.7",
        -- The horde's mode and difficulty. This is the half the LOBBY reads:
        -- without it a held seat carries whatever the lobby happened to give
        -- it, and every client's row says Easy because nothing ever told it
        -- otherwise. The mode is the brain's ordinary one, named so that a
        -- mode the host picked for some other bot is never carried onto the
        -- horde; everything that makes a horde bot fight the way it does is
        -- in its tokens (the init table below and wave_init). Team 1 is left
        -- alone on purpose — the defenders keep what the host picks.
        mode = "default", difficulty = "hard" },
    },
  },

  -- What the host sets in the lobby's details dialog; read below with
  -- game.setting. The defaults are the round as it always played: 5 waves
  -- of 4 minutes.
  settings = {
    { id = "round_minutes", label = "Round length (minutes)", type = "int",
      min = 1, max = 10, step = 1, default = 4 },
    { id = "rounds", label = "Rounds", type = "int",
      min = 1, max = 5, step = 1, default = 5 },
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    spawn_loadout = "Attackers always spawn fully stocked; defenders get the normal tournament loadout.",
    allow_base_win = "Holding every base does not win; the only win is surviving every wave set in the lobby.",
    allow_extra_teams = "Keeps the round to two teams: the defenders and the horde.",
    can_ally = "Players cannot ally.",
    announce = "Silences the newswire for a few seconds while each wave arrives and leaves.",
    on_choose_start = "Defenders start in the centre puddle; each attacker starts out at sea on its own spoke.",
    on_setup = "Gives the defenders the centre bases and pillboxes, builds the island's shallow rim and tree ring, and digs in defender bots.",
    on_start = "Posts the opening \"dig in\" message and notes which seats the horde will use.",
    on_tick = "Sends N waves of 10 attackers, M minutes each with 30 s breaks, N and M set in the lobby; lose all 6 centre bases and you lose, outlast the last wave to win.",
  },
}

-- ---------------------------------------------------------------------
-- Clocks.
--
-- game.tick() counts 100 a second and the tick handed to on_tick goes up by
-- 2 each frame. Every interval below is therefore written in seconds and run
-- through secs(); nothing in this file holds a raw tick count.
local TICKS_PER_SECOND = 100
local function secs(s) return math.floor(s * TICKS_PER_SECOND) end

-- The number of waves, and each wave's length, are the host's lobby
-- choices (scenario.settings). "Rounds" there is waves here.
local WAVES     = game.setting("rounds")
local WAVE_TEAM = 2        -- the horde is team 2
local DEF_TEAM  = 1        -- the defenders are team 1

local GRACE_S      = 10    -- prep before wave 1
local BREATHER_S   = 30    -- prep between waves
-- The lobby's round length: leftover attackers vanish at this mark. The
-- default 4 minutes is 240 s.
local WAVE_LIMIT_S = game.setting("round_minutes") * 60

-- How often the status panel is redrawn. Once a second is enough: the only
-- thing on it that moves faster is the countdown, and the client counts that
-- down itself off one `timer` primitive. The surface refuses a second update
-- to the same panel in the same tick, so this is a floor as well as a rate.
-- It is also the rate the threat line throbs at, so shortening it makes that
-- a flicker.
local PANEL_PERIOD_S = 1

-- How long a flavour line stays up after the state it belongs to began. The
-- line says the same thing for as long as the state lasts, and a wave runs
-- minutes: past the first few seconds it is a throbbing red line the
-- player has already read, sitting over the map. It says its piece and goes,
-- leaving the headline and the countdown, which do change.
local PANEL_LINE_S = 5

-- Wave bots arrive one at a time and leave one at a time, this far apart.
--
-- ARRIVALS ARE FREE NOW, so the spacing on them is zero and a whole wave
-- lands inside half a second. What the second used to be buying: spawn_bot
-- built that bot's Lua VM there and then, about 75 ms of it, and ten in one
-- tick stopped the server long enough that its catch-up burst knocked
-- players on a slow link off their own tanks. It does not build one any
-- more — every held seat's runner is warmed before the round and the spawn
-- RESUMES it (see pump_spawn_queue), so an arrival is a tank and a map
-- reload. The roster queue still paces the ops one to a tick, which is the
-- real floor: ten attackers and their ten init tables land in twenty ticks.
--
-- DEPARTURES are the same story since the host parks a held seat's runner
-- across the unfield instead of tearing it down: nothing is destroyed, so
-- they go out at the queue's own pace too. Either number is seconds and may
-- be a fraction; at 0 the roster queue still hands out one op a tick.
local SPAWN_SPACING_S  = 0
local VANISH_SPACING_S = 0

-- Newswire mute window around wave churn. A wave arriving or leaving fires
-- ten join or quit lines in a row and buries everything else, so the
-- newswire is silenced for the whole of it. The status panel keeps drawing
-- through the mute: it is its own channel, not a newswire line, so a player
-- still reads the wave number and the countdown while the churn is hidden.
local MUTE_LEAD_S = 2      -- silence before the churn
local MUTE_TAIL_S = 2      -- silence after it

-- ---------------------------------------------------------------------
-- Map-file layout contracts (tests/generate_survival_map.py):
local HORDE_BASES  = 8     -- bases 1..8: the horde's shore ring (r=25)
local CENTER_FIRST = 9     -- bases 9..14 form the human centre
local CENTER_PILLS = 6     -- pill k (1..6) pairs centre base CENTER_FIRST-1+k
local WAVE_PILLS   = 10    -- the ten the wave makes for itself

-- A write may only name a square 21..235 on both axes: the outer twenty are
-- the sea frame the map is drawn in and the host refuses a write there.
local WRITE_MIN, WRITE_MAX = 21, 235
local function writable(x, y)
  return x >= WRITE_MIN and x <= WRITE_MAX and y >= WRITE_MIN and y <= WRITE_MAX
end

-- Terrain codes. game.TERRAIN names them, which is what a script should
-- compare against; these locals are the same values under the names the rest
-- of this file already used.
local T_BUILDING     = game.TERRAIN.building
local T_RIVER        = game.TERRAIN.river
local T_ROAD         = game.TERRAIN.road
local T_FOREST       = game.TERRAIN.forest
local T_HALFBUILDING = game.TERRAIN.half_building
local T_BOAT         = game.TERRAIN.boat
local T_DEEP_SEA     = game.TERRAIN.deep_sea
local T_MINE_START   = game.TERRAIN.mine_swamp   -- 10: the first mined code

-- ---------------------------------------------------------------------
-- THE HORDE IS FED RATHER THAN PRICED, AND THEN TOLD HOW TO FIGHT.
--
-- Two mechanisms, and it is worth being clear which does what.
--
-- 1. THE LOADOUT. Every horde tank comes in on the full open loadout, on its
--    first life and on every respawn, while the round itself is played under
--    tournament rules so the humans still farm. This REPLACES the old
--    `refuel=1.2` token (100 on waves 2 and 4), which multiplied the cost of
--    the whole refuel goal group so an attacker went back for supplies less
--    readily, or effectively never. An attacker that comes back with forty
--    of everything has nothing to go back for, so the multiplier has nothing
--    left to do — and this is the honest way round, because it changes the
--    world rather than lying to the brain about a price.
--
-- 2. THE TOKENS. Everything else the old host told a wave bot still reaches
--    it. spawn_bot's `init` table arrives in the brain as the BRAIN_INIT
--    global, and brains/GoalHunter_1.7 flattens that into the
--    BRAIN_INIT_ARG string its own parser reads: keys sorted, a value of
--    "1" becoming the bare flag word the parser matches, "0" dropped, and
--    everything else staying `k=v`. So this table is written the way the
--    brain wants to read it.
--
-- The `init` table takes at most 16 pairs. This uses at most eight.
local WAVE_PORTFOLIO = "0/25/75"  -- back/front/aggressive pill share
local WAVE_BLITZ_MIN = 2          -- fewest tanks in a blitz, commander counted
local WAVE_BLITZ_MAX = 4          -- most tanks one blitz accepts
local WAVE_BLITZ_MIN_BY_WAVE = { [2] = 3, [4] = 3 }
local WAVE_BLITZ_MIN_SUICIDERS = 1
local WAVE_BLITZ_MIN_SUICIDERS_BY_WAVE = { [2] = 4, [4] = 4 }
local WAVE_NOBLITZ = { [3] = true }              -- waves that never blitz
local WAVE_NOCLAIM = { [1] = true, [2] = true }  -- ignore allies' dead-pill claims
-- Waves that attack pills only inside a blitz: no solo pill attacks (the
-- brain's "blitzonly" flag). Dead-pill grabs are not affected. Empty = off;
-- { [1] = true } makes wave 1 blitz-only.
local WAVE_BLITZ_ONLY = {}

-- Waves fielded entirely as pill suiciders. Empty on purpose: waves 2 and 4
-- used to be, and it made those two rounds play as one long pill rush
-- instead of a fight. The mechanism stays — put a wave number back and it
-- fires again. Every wave still designates blitzsuiciders inside a blitz.
local SUICIDER_WAVES = {}

-- The horde's brain mode and difficulty ride the SEAT's config, not the init
-- table: the team block above names them, the C side turns them into the
-- brain's mode= / difficulty= tokens, and every seat this script fields gets
-- them at its VM's first breath whether the spawn carries a table or not.
-- They are not in wave_init below, and must not be: a wave's table is handed
-- to a brain that is ALREADY RUNNING, and mode= and difficulty= are refused
-- at runtime (_apply_cfg_tokens says so, once per bot per wave, in a log
-- nobody wants that line in). The defenders are left alone either way —
-- their mode and difficulty stay exactly as the lobby chose.

-- The wave-invariant half of the horde's orders, written onto the team block
-- rather than inline in the scenario table at the top of this file, because
-- that table is read at the end of the chunk and these constants are not
-- defined until here. One source of truth either way.
--
-- Why the team block carries them at all: it is what each HELD seat's runner
-- is WARMED with before the round, and a spawn that carries no table of its
-- own is matched against it. Match, and the wave resumes ten runners that
-- are already built; differ, and every attacker pays for a fresh Lua VM as
-- it lands. See the spawn in pump_spawn_queue.
scenario.lobby.teams[2].init = {
  portfolio      = WAVE_PORTFOLIO,
  blitz          = string.format("%d/%d", WAVE_BLITZ_MIN, WAVE_BLITZ_MAX),
  blitzsuiciders = tostring(WAVE_BLITZ_MIN_SUICIDERS),
}

-- The orders for one wave, handed to an attacker through game.bot_init the
-- tick after it lands. Every wave sends the WHOLE set, the bare flags
-- included by their absence: the brain resets the ones it owns before it
-- applies a runtime table, so a flag left out here is a flag turned off
-- rather than one left standing from the last wave.
local function wave_init(w)
  local t = {
    portfolio   = WAVE_PORTFOLIO,
    blitz       = string.format("%d/%d",
                    WAVE_BLITZ_MIN_BY_WAVE[w] or WAVE_BLITZ_MIN,
                    WAVE_BLITZ_MAX),
    blitzsuiciders = tostring(WAVE_BLITZ_MIN_SUICIDERS_BY_WAVE[w]
                              or WAVE_BLITZ_MIN_SUICIDERS),
  }
  -- "1" is how a bare flag is written: the brain turns it into the word its
  -- parser matches, and a flag left out is off.
  if SUICIDER_WAVES[w] then t.suicider    = "1" end
  if WAVE_NOBLITZ[w]   then t.noblitz     = "1" end
  if WAVE_NOCLAIM[w]   then t.noclaimdead = "1" end
  if WAVE_BLITZ_ONLY[w] then t.blitzonly  = "1" end
  -- No `refuel` token. The loadout policy below is what keeps an attacker
  -- from going home, and it does it by filling the tank rather than by
  -- pricing the errand.
  return t
end

-- A defender is answered nil, which is no opinion: they take the round's own
-- tournament loadout, as they always did.
function spawn_loadout(p)
  local ls = game.lobby_slot(p)
  if ls ~= nil and ls.team == WAVE_TEAM then return "open" end
  return nil
end

-- Pill seizure: at the start of wave N (N >= 2) the horde seizes up to N of
-- the defenders' pillboxes, picked at random and loaded into random
-- attackers, but never leaving the defenders below the floor. A short roster
-- is not topped up to the floor; the floor only caps what is taken.
local SEIZE_MIN_LEFT = { [2] = 7, [3] = 6, [4] = 5, [5] = 4 }

-- How far from the attacker a pill drop may look for ground. Three rings is
-- plenty for a shoreline landing.
local PILL_DROP_SEARCH = 3

-- ---------------------------------------------------------------------
-- Round state. Every one of these is set again when the chunk runs, which
-- is once a round: globals do not survive a round on this host.

local seats       = {}     -- the horde's held seats, in seat order
local wave        = 0
local wave_bots   = {}     -- seat -> true for a fielded wave member
local seen_fielded = {}   -- seat -> true once a prune pass saw it on the
                          -- field; reset when a wave launches (see the
                          -- prune for why a queued spawn must not count)
local ashore_said  = {}   -- seat -> true once this wave's arrival line is out
local spawn_start  = {}   -- seat -> the start number the spawn named, or nil
                          -- when the placement was left to on_choose_start
local next_wave_at = nil   -- tick the next wave lands (nil while one is live)
local ended       = false
local wave_ends_at = nil

-- The staggered arrival queue.
local spawn_left     = 0
local spawn_next_at  = nil
local spawn_pill_next = false
local pill_for_bot   = nil
local spawn_index    = 0
local spawned        = {}  -- seats this wave's spawns actually landed in
local spawn_fail_said = false
local wave_bases_restocked = 0
local wave_pills_made = 0

-- The staggered departure queue.
local vanishing      = false
local vanish_queue   = {}
local vanish_next_at = nil
local horde_estate   = nil

-- The mute, and the rim the setup could not finish.
local newswire_muted     = false
local newswire_unmute_at = nil
local rim_todo   = {}      -- coast squares still waiting to become river
local rim_at     = 1
local RIM_PER_TICK = 120   -- squares a tick, well under the frame's 256

local dealt = false

-- ---------------------------------------------------------------------
-- WHICH SIDE A SEAT IS ON.
--
-- A slot NUMBER says nothing about it, and nothing in this file may read a
-- side off one.
--
-- The lobby seats a scenario's teams from the first free slot upward, so the
-- horde's ten HELD seats take whatever numbers are going. A headless run
-- seats its -bots first and the horde follows, which is where "defenders
-- 0..5, horde 6..15" came from. A LOBBY-hosted round is the other way round:
-- the host holds slot 0, the ten held seats are put down the moment the map
-- is committed, and the defenders the host adds land at 11..15 — above the
-- horde. Read as slot numbers, that round has one defender and five
-- attackers in the keep.
--
-- on_choose_start has keyed on the team since the port and says why. These
-- are the same question, for the rest of the file.
local function seat_team(p)
  if p == nil or p < 0 then return nil end
  local ls = game.lobby_slot(p)
  if ls == nil then return nil end
  return ls.team
end

local function is_defender(p) return seat_team(p) == DEF_TEAM end

-- Every seat on one side, in seat order. A held seat counts: the horde's are
-- held for most of the round and are still the horde's.
local function seats_on(team)
  local out = {}
  for p = 0, game.max_tanks() - 1 do
    if seat_team(p) == team then out[#out + 1] = p end
  end
  return out
end

-- Where a seat comes in its OWN side's list, counting from zero: the first
-- seat on that team is 0, the next 1, and so on up the roster.
--
-- This is the number every placement below is built on. A slot number is a
-- number the lobby handed out, and which numbers a side gets depends on who
-- sat down first — a dedicated server seats the scenario's template into an
-- empty roster and the horde takes 0..9, a host's own game seats him first
-- and the horde starts at 1, a headless run seats its -bots first and the
-- horde starts above them. A rank is the same whichever of those happened,
-- so a rule written on it behaves the same in all three.
--
-- nil for a seat with no team, which is the one case a rank cannot be had.
local function seat_rank(p)
  local team = seat_team(p)
  local rank = 0
  if team == nil or team == 0 then return nil end
  for q = 0, p - 1 do
    if seat_team(q) == team then rank = rank + 1 end
  end
  return rank
end

-- ---------------------------------------------------------------------
-- Policies.

-- The engine's classic all-bases sweep is off. Taking the horde's shore
-- bases mid-wave must not end the round early — the win is outlasting all
-- five waves and nothing else — and the loss is this script's own instant
-- check on the six inner bases, which is far stricter than a full-map sweep
-- anyway. The map carries 14 bases, so defenders holding their 6 and taking
-- the horde's 8 during a breather would otherwise be an accidental win.
function allow_base_win()
  return false
end

-- Survival is strictly two-sided: defenders against the horde. This is the
-- host's own question where the old file answered a show_add_team_button
-- one; the effect a host sees is the same.
function allow_extra_teams()
  return false
end

-- And no alliances players make themselves. A defender allied with a horde
-- bot would be spared by the horde's pillboxes while this script still
-- counted them a defender.
function can_ally(p, q)
  return false
end

-- The newswire mute, a line at a time. The old host had one switch that
-- silenced the whole newswire; this host asks before every line, so the
-- mute is a flag and this is the answer.
function announce(kind, subject, actor)
  if newswire_muted then return false end
  return nil                       -- no opinion: the ordinary rule shows it
end

local function set_newswire_mute(on)
  newswire_muted = on
end

-- Deterministic spawn pinning. Fires for every placement — first spawn and
-- respawn alike. Map-file start order: 1..6 the centre-puddle defender
-- starts, 7..16 the outer ocean.
--
-- Placement is keyed on the seat's TEAM, not its number: a horde seat can
-- end up low, and a slot-numbered rule would drop an attacker into the
-- middle of the human keep. The invariant is that the puddle is
-- defender-only, and every horde branch below is arithmetic that always
-- lands in 7..16.
function on_choose_start(p)
  local ls = game.lobby_slot(p)
  local rank = seat_rank(p)
  local enemy
  if ls ~= nil and ls.team ~= 0 then
    enemy = (ls.team == WAVE_TEAM)
  else
    enemy = (p >= 6 and p <= 15)   -- no roster info: the slot heuristic
  end
  if enemy then
    -- Each wave seat owns one ocean start, on its own 36-degree spoke,
    -- taken by its rank on the horde's side rather than by its slot
    -- number. Four of those spokes carry a horde base, so those seats come
    -- ashore aimed at their own and the rest fight in. A seat with no team
    -- has no rank; its slot still lands in 7..16, which is ocean.
    return 7 + ((rank or p) % 10)
  end
  -- Defenders: the puddle, one position per rank on their own side.
  return 1 + ((rank or p) % 6)
end

-- ---------------------------------------------------------------------
-- Bases and pills.

-- game.set_base_owner drains a base whenever it moves it between two real
-- owners — armour, shells and mines all to zero. That is the engine's
-- capture rule and it does not care that the capture came from a script.
-- This map re-deals ownership constantly, so every re-deal is followed by a
-- restock or the round opens on empty bases.
--
-- BASE_FULL is past the engine's own 90 on purpose: set_base_stock holds
-- each value at the real maximum, so "a big number" means "full" without
-- this file tracking the engine's constant.
local BASE_FULL = 255

local function restock_quiet(first, last)
  local n = 0
  for b = first, last do
    if game.set_base_stock(b, BASE_FULL, BASE_FULL, BASE_FULL) then
      n = n + 1
    end
  end
  return n
end

-- Read one base back rather than quoting BASE_FULL: the line then shows the
-- engine's real ceiling instead of what we asked for.
--
-- The console rather than the newswire. This is an operator's line: a player
-- reads where the round is off the status panel, and a restock count is not
-- something they can act on.
local function restock_report(probe, n, what)
  local bi = game.base(probe)
  game.log(string.format("Survival: [bases] %s restocked %d bases to %d/%d/%d",
    what, n, bi and bi.armour or 0, bi and bi.shells or 0,
    bi and bi.mines or 0))
end

local function restock(first, last, what)
  restock_report(first, restock_quiet(first, last), what)
end

-- A wave pill this script may still stamp: not carried (it is wherever its
-- tank is) and not flying defender colours (one the humans captured stays
-- theirs). NEUTRAL counts as stampable, which is what the nil owner catches.
local function pill_stampable(pn)
  local pi = game.pill(pn)
  if pi == nil or pi.in_tank then return false end
  local o = pi.owner
  return o == nil or not is_defender(o)
end

-- The wave's owner. The first attacker ashore takes the horde's whole estate
-- for the wave: all eight shore bases and every stampable outer pill, in one
-- pass, on the tick it lands. Later arrivals stamp nothing.
--
-- One owner and one handover, rather than a share re-stamped as each of the
-- ten files in. It bought nothing a player could see and it cost a handover
-- an arrival, each of which drains the base. Order matters per base, so the
-- top-up follows the stamp.
local function stamp_wave_owner(s)
  local n = 0
  for k = 1, HORDE_BASES do
    game.set_base_owner(k, s)
    if restock_quiet(k, k) > 0 then n = n + 1 end
  end
  -- Bounded by what exists, not by a fixed 7..16: this runs on the first
  -- attacker's tick, when the rest of the wave's pills are not made yet.
  -- Each of those is stamped as it is made instead (drop_pill_for).
  for pn = CENTER_PILLS + 1, game.num_pills() do
    if pill_stampable(pn) then game.set_pill_owner(pn, s) end
  end
  wave_bases_restocked = n
end

-- The defenders actually on the field: on the defenders' team and holding a
-- tank, human or bot alike, so a host can stack their own team with bots for
-- testing. In seat order, which is the order the six centre positions are
-- handed out in.
local function fielded_defenders()
  local out = {}
  for _, p in ipairs(seats_on(DEF_TEAM)) do
    if game.tank(p) ~= nil then out[#out + 1] = p end
  end
  return out
end

-- Deal the centre bases and their paired pills round-robin to the defenders
-- on the field. False while nobody is on it yet.
local function deal_center()
  local defenders = fielded_defenders()
  if #defenders == 0 then return false end

  -- Round-robin over the seats that are there, in seat order: with six
  -- defenders each takes one position, with one defender that player takes
  -- all six. A base and its paired pill always go to the same seat, so the
  -- keep is six matched stations however many people are holding it.
  local d = 1
  for k = 1, CENTER_PILLS do
    local owner = defenders[d]
    d = (d % #defenders) + 1
    game.set_base_owner(CENTER_FIRST - 1 + k, owner)
    game.set_pill_owner(k, owner)
  end
  -- The deal drains every base it moved between two seated players, so the
  -- defenders would open on empty bases — no armour to repair with, no
  -- shells, no mines, on a map whose entire premise is digging in.
  restock(CENTER_FIRST, CENTER_FIRST + 5, "center")

  -- Deal the horde's eight to the horde now too. The waves re-deal them, but
  -- until wave 1 lands the map-file owners rule, and what the map file names
  -- is slot numbers — which in a lobby-hosted round are the defenders' own
  -- seats. Round-robin over the horde's seats instead, so the shore ring is
  -- red to the defenders from tick 1 and to nobody else.
  local horde = seats_on(WAVE_TEAM)
  if #horde > 0 then
    for k = 1, HORDE_BASES do
      game.set_base_owner(k, horde[((k - 1) % #horde) + 1])
    end
    restock(1, HORDE_BASES, "horde")
  end
  return true
end

-- ---------------------------------------------------------------------
-- The wave's own pillboxes.

-- Ground a dead pillbox may be dropped on. Water and structures refuse one,
-- and a mined square is skipped so the drop cannot eat a mine someone laid.
-- Road is fine, unlike for a tree: a pill standing on paving is normal.
local function pill_droppable(t)
  if t == nil or t == T_DEEP_SEA then return false end
  if t == T_BUILDING or t == T_HALFBUILDING then return false end
  if t == T_RIVER or t == T_BOAT then return false end
  if t >= T_MINE_START then return false end
  return true
end

-- A square with a base or a pillbox standing on it refuses a pill as the
-- engine's own rule, whatever its terrain says. The shore bases stand on
-- plain ground on the ring, so a spoke that meets the ring at a base used
-- to name the base's own square, be refused, and leave its attacker
-- empty-handed.
local function square_taken(x, y)
  for k = 1, game.num_bases() do
    local b = game.base(k)
    if b ~= nil and b.x == x and b.y == y then return true end
  end
  for n = 1, game.num_pills() do
    local pi = game.pill(n)
    if pi ~= nil and not pi.in_tank and pi.x == x and pi.y == y then
      return true
    end
  end
  return false
end

local function pill_square_ok(x, y)
  return writable(x, y) and pill_droppable(game.map_tile(x, y))
     and not square_taken(x, y)
end

-- A square the attacker's pill can sit on: its own first, then the rings
-- around it, outward. nil when an attacker landed with no ground beside it.
local function pill_drop_spot(x, y)
  if pill_square_ok(x, y) then return x, y end
  for r = 1, PILL_DROP_SEARCH do
    for dx = -r, r do
      for dy = -r, r do
        -- The ring only: the inner squares were covered by a smaller r, so
        -- nearer ground always wins.
        if dx == -r or dx == r or dy == -r or dy == r then
          local nx, ny = x + dx, y + dy
          if pill_square_ok(nx, ny) then
            return nx, ny
          end
        end
      end
    end
  end
  return nil
end

-- Make one of the wave's pillboxes and load it into the attacker that has
-- just come ashore. False when nothing was made, so a caller looping over
-- the roster knows to stop.
--
-- It is made where the attacker landed rather than on any remembered ring
-- square: the ten ring pills are not in the map file, so there is no square
-- to go back to, and making it where the attacker actually landed is what
-- "the pill arrives with the bot" means. It stands on the ground only for
-- the instant between add_pill and give_pill — both inside one tick, so no
-- player ever sees it loose.
--
-- If give_pill refuses (the tank died between landing and this step) the
-- pill already exists, so it is left standing and stamped to the wave's
-- owner: stamp_wave_owner ran on the first attacker's tick and could not see
-- a pill that did not exist yet, and a neutral pill shoots at everybody.
local function drop_pill_for(p)
  if p == nil then return false end
  if wave_pills_made >= WAVE_PILLS then return false end
  local t = game.tank(p)
  if t == nil then return false end

  -- The square is a staging post, nothing more: the pill is in the tank
  -- before this tick ends, and the engine refuses a pill on water, so it
  -- is made on the first ground that answers. Beside the tank when there
  -- is any, else beside one of the horde's shore bases, which always stand
  -- on land. An attacker still over open water therefore lands with its
  -- pill like every other, instead of empty-handed with its pill handed
  -- to a team-mate by the top-up.
  local x, y = pill_drop_spot(t.mx, t.my)
  if x == nil then
    for k = 1, HORDE_BASES do
      local b = game.base(k)
      if b ~= nil then
        x, y = pill_drop_spot(b.x, b.y)
        if x ~= nil then break end
      end
    end
  end
  if x == nil then return false end

  local n = game.add_pill(x, y)
  if not n then return false end
  wave_pills_made = wave_pills_made + 1

  if game.give_pill(p, n) then return true end

  local owner = spawned[1]
  if owner ~= nil then game.set_pill_owner(n, owner) end
  return true
end

-- A wave pill this script may still move: dead on the ground (the scoopable
-- state), not carried, and not flying defender colours. A built pill stays
-- where it stands — it is a manned gun now, whoever's it is.
local function wave_pill_free(pi)
  if pi == nil or pi.in_tank then return false end
  if pi.armour ~= 0 then return false end
  local o = pi.owner
  if o ~= nil and is_defender(o) then return false end
  return true
end

-- One claimable pill per attacker and not a crumb more.
--
--   * CLAIM — each attacker, in spawn order, takes the nearest free pill
--     still on the table. It stays on the ground: the bot's own brain sees a
--     dead pill under its nose and goes and gets it, which is the emergent
--     field engineering this map wants.
--   * DISTRIBUTE — everything left over after every attacker has one is
--     loaded straight into the tanks, round-robin, off the ground and out of
--     defender reach.
--
-- With more attackers than pills the claim pass runs dry and there is
-- nothing to distribute.
local function deal_wave_pills()
  if #spawned == 0 then return end

  -- Over the pills that actually exist above the centre six, not a fixed
  -- 7..16: the wave's own are made as its attackers land.
  local pool = {}
  for n = CENTER_PILLS + 1, game.num_pills() do
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
        local d = dx * dx + dy * dy         -- squared: ordering is all we need
        if best_d == nil or d < best_d then best, best_d = i, d end
      end
      table.remove(pool, best)
      claimed = claimed + 1
    end
  end

  local loaded, turn = 0, 0
  for _, e in ipairs(pool) do
    local p = spawned[(turn % #spawned) + 1]
    turn = turn + 1                         -- advance even if the load is
    if game.give_pill(p, e.n) then          -- refused, so the spread stays even
      loaded = loaded + 1
    end
  end

  -- The console rather than the newswire: how the wave's ten pills were
  -- split up is a thing to read back off a log, not a thing a defender does
  -- anything with.
  game.log(string.format(
    "Survival: [pills] wave %d: %d claimed on ground, %d loaded into tanks",
    wave, claimed, loaded))
end

-- The defenders' pills on the map: the pool the seizure draws from. Dead or
-- built alike — a placed gun is exactly what the horde wants back.
local function defender_pills()
  local out = {}
  for n = 1, game.num_pills() do
    local pi = game.pill(n)
    if pi and not pi.in_tank and pi.owner ~= nil and is_defender(pi.owner) then
      out[#out + 1] = n
    end
  end
  return out
end

-- ---------------------------------------------------------------------
-- The status panel.
--
-- Everything this round says otherwise goes out as newswire lines, which
-- scroll away: a player who was looking at their tank when the last wave
-- died has no way to find out how long the quiet lasts. The panel is the
-- standing answer to "where am I" -- which wave, what the horde is doing,
-- and how long until it does it again.
--
-- It is a square of 128 logical units, origin top-left, and the frontend
-- decides where that square goes and how big it is drawn. Nothing here is a
-- pixel. Normal text is 11 units tall and small text is 8, which is what the
-- row positions below are spaced against.
--
-- The countdown is one `timer` primitive rather than a number this script
-- rewrites: the client works it out against its own clock, so a five-minute
-- wave costs one message instead of three hundred.

-- What the panel says in each of its four states. The wave-numbered tables
-- are indexed by wave, so the line a player reads is fixed by which wave it
-- is and not by chance -- nothing here touches math.random, which the round
-- shares with the spawner and the seizure.
--
-- Every line is upper case and short on purpose. Small text is 8 units tall
-- in a proportional font, so about 26 characters fit across the square, and
-- a line past that is clipped by the frontend rather than wrapped.
local PANEL_LINE_GRACE = "SOMETHING IS IN THE WATER"

local PANEL_LINE_WAVE = {
  "THEY CAME ASHORE",
  "MORE OF THEM THIS TIME",
  "THE SHORE KEEPS GIVING",
  "THEY ARE NOT TIRED",
  "THE LAST OF THEM. PROBABLY",
}

-- Said while the wave that just ended walks back into the sea, one attacker
-- at a time.
local PANEL_LINE_LEAVING = "ONE BY ONE"

-- And said through the breather, about the wave that was just survived.
local PANEL_LINE_BREATHER = {
  "THAT WAS THE POLITE ONE",
  "THEY WENT TO GET FRIENDS",
  "COUNT YOUR PILLBOXES",
  "THEY KNOW THE MAP NOW",
}

local panel_next_at = nil
-- Flipped on every redraw. The threat lines sit on it and change colour once
-- a second, which reads as a slow throb rather than a flicker -- the panel is
-- only redrawn at PANEL_PERIOD_S, so this cannot go faster than that.
local panel_pulse   = false

-- Which state the last redraw was in, and the tick it began on. A flavour
-- line is shown for PANEL_LINE_S from that tick and not afterwards, so the
-- key has to name the wave as well as the state: wave 2 is a state of its
-- own and puts its own line up, rather than inheriting a clock that ran out
-- during wave 1.
local panel_state    = nil
local panel_state_at = nil

local function panel_draw(tick)
  panel_pulse = not panel_pulse

  -- No backing bar behind the title. The frontend draws its own bar across
  -- the top of the panel while the pointer is on it, to drag the panel by
  -- and to reach its settings, and a grey band painted there by the round
  -- itself sat under that one for the whole game for no reason.
  local list = {
    { "text", 64, 3, "white", "normal", "centre", "SURVIVAL" },
  }

  -- The four states, and the two flags that tell them apart. next_wave_at is
  -- set exactly while no wave is live; wave_ends_at is set exactly while one
  -- is running. Neither is set in the window between the wave clock running
  -- out and the last attacker actually being gone.
  --
  -- `wave == 0` is asked first rather than `next_wave_at ~= nil and
  -- wave == 0`, because on_tick draws the panel before it arms the grace
  -- clock: on the round's very first tick both flags are nil, and a test on
  -- next_wave_at would drop that tick into the leaving branch and open the
  -- round on "THEY ARE LEAVING" for a whole second. A nil target just leaves
  -- the countdown off until the clock is armed.
  local head, head_colour, line1, line2, line_colour, label, target, state
  if wave == 0 then
    state       = "grace"
    head        = "GET READY!"
    head_colour = "yellow"
    line1       = PANEL_LINE_GRACE
    line_colour = panel_pulse and "red" or "orange"
    label       = "FIRST WAVE IN"
    target      = next_wave_at
  elseif next_wave_at ~= nil then
    state       = "breather"
    head        = string.format("WAVE %d/%d SURVIVED", wave, WAVES)
    head_colour = "green"
    line1       = PANEL_LINE_BREATHER[wave] or "THEY ARE STILL OUT THERE"
    -- The seizure that opens the next wave is the one thing a player can
    -- still do something about while the field is empty, so it is on the
    -- panel and not only in the newswire line that scrolls away.
    line2       = string.format("UP TO %d PILLS WILL TURN", wave + 1)
    line_colour = "grey"
    label       = "THE HORDE RETURNS IN"
    target      = next_wave_at
  elseif wave_ends_at ~= nil then
    state       = "wave"
    head        = string.format("WAVE %d/%d", wave, WAVES)
    head_colour = "red"
    line1       = PANEL_LINE_WAVE[wave] or "THEY KEEP COMING"
    line_colour = panel_pulse and "red" or "orange"
    label       = "WAVE ENDS IN"
    target      = wave_ends_at
  else
    state       = "leaving"
    head        = "THEY ARE LEAVING"
    head_colour = "yellow"
    line1       = PANEL_LINE_LEAVING
    line_colour = "grey"
  end

  list[#list + 1] = { "text", 64, 26, head_colour, "normal", "centre", head }

  -- The flavour, for the first PANEL_LINE_S of the state and no longer. The
  -- key carries the wave, so every wave and every breather puts its own line
  -- up again rather than the clock running once for the whole round.
  local key = string.format("%s%d", state, wave)
  if panel_state ~= key then
    panel_state    = key
    panel_state_at = tick
  end
  if tick - panel_state_at < secs(PANEL_LINE_S) then
    list[#list + 1] = { "text", 64, 48, line_colour, "small", "centre", line1 }
    if line2 ~= nil then
      list[#list + 1] =
        { "text", 64, 59, line_colour, "small", "centre", line2 }
    end
  end
  if target ~= nil then
    list[#list + 1] = { "text", 64, 86, "grey", "small", "centre", label }
    list[#list + 1] =
      { "timer", 64, 98, "white", "normal", "centre", "down", target }
  end

  game.panel(0, list)
end

-- Once the whole wave is ashore: seize up to `wave` defender pills,
-- respecting the floor, and load each into a random attacker.
local function seize_defender_pills()
  if wave < 2 or #spawned == 0 then return end
  local pool  = defender_pills()
  local floor = SEIZE_MIN_LEFT[wave] or 0
  local take  = math.min(wave, math.max(0, #pool - floor))
  local seized = 0
  for _ = 1, take do
    if #pool == 0 then break end
    local n = table.remove(pool, math.random(#pool))
    -- Try attackers in random order until one accepts (a dead tank refuses).
    local order = {}
    for i, p in ipairs(spawned) do order[i] = p end
    for i = #order, 2, -1 do
      local j = math.random(i); order[i], order[j] = order[j], order[i]
    end
    for _, p in ipairs(order) do
      if game.give_pill(p, n) then seized = seized + 1; break end
    end
  end
  local left = #defender_pills()
  if seized > 0 then
    game.message(string.format(
      "*** The horde seized %d of your pillboxes! You hold %d. ***",
      seized, left))
  else
    game.message(string.format(
      "*** The horde seized nothing: you hold %d (floor %d). ***",
      left, floor))
  end
end

-- ---------------------------------------------------------------------
-- The arrival queue.

local function finish_wave_spawn()
  spawn_next_at = nil

  -- The last attacker has landed: the newswire comes back MUTE_TAIL_S from
  -- now, lifted at the top of on_tick.
  newswire_unmute_at = game.tick() + secs(MUTE_TAIL_S)

  restock_report(1, wave_bases_restocked, "horde")

  -- Every attacker carries the pill it brought ashore, so none is lying
  -- loose. A short roster has fewer attackers than WAVE_PILLS, so the
  -- remainder are made here, one more to each attacker in turn. Any that
  -- cannot be placed is simply never made, which is the point: these
  -- pillboxes exist only where a bot puts them.
  --
  -- Round-robin rather than always from the front: there is no carry limit,
  -- so restarting at spawned[1] every time would pile the whole surplus into
  -- one tank, which is exactly the loaded-carrier death case.
  local turn = 0
  while wave_pills_made < WAVE_PILLS and #spawned > 0 do
    local placed = false
    for i = 1, #spawned do
      local sp = spawned[((turn + i - 1) % #spawned) + 1]
      if drop_pill_for(sp) then
        placed = true
        turn = turn + i
        break
      end
    end
    if not placed then break end
  end
  deal_wave_pills()
  seize_defender_pills()
end

-- Bring at most one attacker ashore, no more often than SPAWN_SPACING_S.
-- The first call for a wave happens on the wave's own tick, so wave 1 starts
-- on time.
local function pump_spawn_queue(tick)
  if spawn_left <= 0 and not spawn_pill_next then return end
  if spawn_next_at ~= nil and tick < spawn_next_at then return end

  -- A PILL step: the attacker that landed a second ago drops the dead
  -- pillbox it brought, and the next attacker follows a second after that.
  -- The wave therefore fields one tank, one pill, one tank, instead of ten
  -- pills appearing together with nobody there to guard them.
  if spawn_pill_next then
    spawn_pill_next = false
    drop_pill_for(pill_for_bot)
    pill_for_bot = nil
    if spawn_left > 0 then
      spawn_next_at = tick + secs(SPAWN_SPACING_S)
    else
      finish_wave_spawn()
    end
    return
  end

  spawn_index = spawn_index + 1
  spawn_left  = spawn_left - 1

  -- Field one of the horde's HELD seats. The seat carries the name and the
  -- team it was seated with, so neither is restated; the loadout is forced
  -- open whatever the round's tournament rules say, and the init table is
  -- the wave's own brain tokens.
  local seat = seats[spawn_index]
  local p
  local start
  if seat ~= nil then
    -- Each wave seat owns one ocean start, by its rank on the horde's side.
    -- NAMED here for every seat rather than left to on_choose_start: a start
    -- the spawn names is honoured by the tank's own resolver ahead of any
    -- placement policy, so the wave lands on the ring whether or not the
    -- policy is asked. The rank is the seat's position in this wave's own
    -- list where the roster no longer answers for it.
    start = 7 + ((seat_rank(seat) or (spawn_index - 1)) % 10)
    -- The loadout is named here as well as answered by spawn_loadout. A
    -- named one outranks the policy and is taken as it is read, so the
    -- first life is full whatever else is going on; the policy carries
    -- every life after it.
    --
    -- NO init table. That is what makes this a RESUME: the seat's warmed
    -- runner was built with the team block's table, a spawn carrying none is
    -- matched against that same table, and the seat comes back on the field
    -- without a Lua VM being built for it. Carrying the wave's own table
    -- here instead — which is what this line used to do — missed on every
    -- spawn and threw away all ten warmed runners at about 75 ms each. The
    -- wave's own orders follow on the next line.
    p = game.spawn_bot{ slot = seat, start = start, loadout = "open" }
  end

  if p then
    -- What this seat was told, kept for the arrival line below: a recording
    -- of a round otherwise carries no placement at all, and "where did the
    -- attackers land" is the first question a wrong-looking wave raises.
    spawn_start[p] = start
    -- The wave's orders, queued right behind the spawn. Both are roster ops
    -- and the sim drains one a tick, so this lands the tick after the bot
    -- does — by which time it is on the field, which is what bot_init needs.
    game.bot_init(p, wave_init(wave))
    wave_bots[p] = true
    spawned[#spawned + 1] = p
    -- The first attacker ashore takes the wave's whole estate and keeps it
    -- until the wave leaves. Stamped here, once the bot exists — stamping
    -- ahead of time would leave the bases owned by an empty seat, hostile to
    -- both sides, for the seconds the arrival takes.
    if #spawned == 1 then stamp_wave_owner(p) end
    -- This attacker's pill lands on the next step of the queue, but only
    -- while there are any left to make. Wave 1 makes all ten, so waves 2..5
    -- have nothing to bring and field at the plain rhythm.
    if wave_pills_made < WAVE_PILLS then
      pill_for_bot    = p
      spawn_pill_next = true
    end
  elseif not spawn_fail_said then
    spawn_fail_said = true
    game.log(string.format(
      "Survival: [wave] wave %d: no free seat -- the wave lands short-handed.",
      wave))
  end

  -- A pill step still owed counts as queue work, so the wave is not finished
  -- until the last attacker's pill is down.
  if spawn_pill_next or spawn_left > 0 then
    spawn_next_at = tick + secs(SPAWN_SPACING_S)
  else
    finish_wave_spawn()
  end
end

-- Open a wave: do the bookkeeping and queue the arrivals. No bot is made
-- here — pump_spawn_queue brings them in one at a time from on_tick.
local function spawn_wave()
  wave = wave + 1

  spawned       = {}
  spawn_index   = 0
  spawn_left    = #seats
  spawn_next_at = nil          -- the first attacker rides the wave's own tick
  spawn_fail_said = false
  spawn_pill_next = false
  pill_for_bot  = nil
  wave_bases_restocked = 0

  wave_ends_at  = game.tick() + secs(WAVE_LIMIT_S)
  -- A fresh wave, a fresh memory of who has been on the field: the seats
  -- are the same ten every wave, and a mark left from the last wave would
  -- read this wave's queued spawn as a departure.
  seen_fielded = {}
  ashore_said  = {}

  -- The console and nothing else. The panel puts "WAVE n/5" up with a
  -- countdown to the end of it the moment the wave starts, so a player is
  -- already told which wave it is and how long it runs; the tick numbers
  -- here are for an operator reading a round back afterwards.
  game.log(string.format(
    "Survival: [wave] %d: clock started at tick %d, ends at tick %d (%d s) at %s",
    wave, game.tick(), wave_ends_at, WAVE_LIMIT_S, os.date("%H:%M:%S")))
  game.log(string.format("Survival: wave %d/%d inbound, %d attacker(s)",
                         wave, WAVES, #seats))

  -- A wave with no seats to field never reaches finish_wave_spawn, and that
  -- is the only place the newswire comes back on. Lift it here instead, so a
  -- round whose lobby seated no horde -- a harness, or a host who trimmed the
  -- team to nothing -- does not play out in silence.
  if #seats == 0 then
    newswire_unmute_at = game.tick() + secs(MUTE_TAIL_S)
  end
end

-- ---------------------------------------------------------------------
-- The departure queue.

-- Line every wave bot up to be removed, one per VANISH_SPACING_S. The
-- players are told nothing here: the panel drops to "THEY ARE LEAVING" on
-- its next redraw, which is the same second, and it keeps saying it until
-- the last tank is actually gone.
--
-- A wave that is still arriving must not race its own departure, so the
-- arrival queue is dropped here first.
local function vanish_wave(tick)
  -- Record every base and pill the wave owns BEFORE any attacker leaves. The
  -- engine migrates a leaving player's estate to a surviving teammate and
  -- neutralises it when the last one goes; the horde's allegiances are put
  -- back the moment the field is empty, so the breather is played against
  -- the horde's guns rather than neutral ones. Only horde-owned entries are
  -- recorded; whatever the defenders captured is left exactly as it is.
  horde_estate = { bases = {}, pills = {} }
  for k = 1, game.num_bases() do
    local bi = game.base(k)
    if bi and bi.owner ~= nil and wave_bots[bi.owner] then
      horde_estate.bases[k] = bi.owner
    end
  end
  for n = 1, game.num_pills() do
    local pi = game.pill(n)
    if pi and pi.owner ~= nil and wave_bots[pi.owner] then
      horde_estate.pills[n] = pi.owner
    end
  end
  spawn_left = 0
  spawn_next_at = nil
  spawn_pill_next = false
  pill_for_bot = nil
  vanish_queue = {}
  for p in pairs(wave_bots) do
    vanish_queue[#vanish_queue + 1] = p
  end
  -- pairs() order is not defined; sort so the same seed removes the same bot
  -- first on every run.
  table.sort(vanish_queue)
  game.log(string.format(
    "Survival: [wave] %d over at tick %d: %d of %d spawned queued to leave: %s",
    wave, tick, #vanish_queue, #spawned, table.concat(vanish_queue, " ")))
  vanishing = true
  -- The first removal waits out MUTE_LEAD_S so the newswire is already
  -- silent before the first attacker disappears; the caller mutes on this
  -- tick.
  vanish_next_at = tick + secs(MUTE_LEAD_S)
end

-- The last attacker is gone: put every base and pill the wave owned back in
-- its recorded seat (an absent player can own things), unless the defenders
-- captured it meanwhile or it is riding in a tank.
local function restore_horde_estate()
  if not horde_estate then return 0 end
  local n = 0
  for k, slot in pairs(horde_estate.bases) do
    local bi = game.base(k)
    if bi and (bi.owner == nil or not is_defender(bi.owner))
       and bi.owner ~= slot then
      game.set_base_owner(k, slot); n = n + 1
    end
  end
  for pn, slot in pairs(horde_estate.pills) do
    local pi = game.pill(pn)
    if pi and not pi.in_tank and (pi.owner == nil or not is_defender(pi.owner))
       and pi.owner ~= slot then
      game.set_pill_owner(pn, slot); n = n + 1
    end
  end
  horde_estate = nil
  return n
end

-- Pop at most one queued removal. True on the tick the last wave bot leaves
-- the field (an empty queue counts as drained straight away), which is what
-- the between-wave clock keys on.
--
-- remove_bot on a seat the lobby seated hands the seat back to being held
-- rather than emptying it, so the next wave fields the same ten again.
local function pump_vanish_queue(tick)
  if not vanishing then return false end
  if #vanish_queue > 0 then
    if vanish_next_at ~= nil and tick < vanish_next_at then return false end
    local p = table.remove(vanish_queue, 1)
    local ok, why = game.remove_bot(p)
    -- The console. A refused removal is the thing this line exists to catch,
    -- and that is an operator's problem, not something a defender acts on.
    game.log(string.format("Survival: [wave] remove_bot %d at tick %d -> %s%s",
      p, tick, tostring(ok), why and (" " .. tostring(why)) or ""))
    wave_bots[p] = nil
    seen_fielded[p] = nil
    vanish_next_at = tick + secs(VANISH_SPACING_S)
  end
  if #vanish_queue > 0 then return false end
  vanishing = false
  vanish_next_at = nil
  game.log(string.format("Survival: [wave] departures done at tick %d at %s",
    tick, os.date("%H:%M:%S")))
  return true
end

-- Wave bots respawn like ordinary play — a dead one is merely between lives,
-- so nobody is removed here. This prunes seats that vanished outside our
-- control: a seat that was on the field and no longer is, or is gone from
-- the roster.
--
-- Two things it must not read as "gone". A spawn is queued, not done:
-- spawn_bot answers the seat and the sim fields it a tick or more later,
-- so a seat just asked for is not fielded yet, and a prune that read that
-- as departure forgot nine of ten attackers on the tick they were called.
-- And a dead tank waiting to respawn answers no tank at all, so game.tank
-- is no test either. The seat is asked, and only a seat this pass has
-- already seen on the field can be pruned for leaving it. The wave's end
-- then takes every attacker off, instead of the one that happened to be
-- listed, and the next wave lands with all ten seats free.
local function prune_wave_bots()
  for p in pairs(wave_bots) do
    local ls = game.lobby_slot(p)
    if ls ~= nil and ls.fielded then
      seen_fielded[p] = true
      -- Where this attacker actually came ashore, said once per seat per
      -- wave. The seat reads as fielded a tick before its tank exists, so
      -- the line waits for the tank rather than the roster.
      if not ashore_said[p] then
        local t = game.tank(p)
        if t ~= nil then
          local line = string.format(
            "[wave] attacker %d ashore at (%d,%d) start=%s",
            p, t.mx, t.my,
            spawn_start[p] ~= nil and tostring(spawn_start[p]) or "chooser")
          ashore_said[p] = true
          -- The console, so an operator watching a headless round reads it
          -- as it happens. It used to go on the newswire as well, and does
          -- not any more: a square and a start number is not something a
          -- defender acts on, and the panel is where a player now reads what
          -- the horde is doing.
          game.log("Survival: " .. line)
        end
      end
    elseif not vanishing and (ls == nil or seen_fielded[p]) then
      game.log(string.format(
        "Survival: [wave] seat %d left the wave list at tick %d (%s)",
        p, game.tick(),
        (ls == nil) and "gone from the roster" or "no longer fielded"))
      wave_bots[p] = nil
      seen_fielded[p] = nil
    end
  end
end

-- Start the clock on the next wave, `gap` ticks from `tick`. The panel reads
-- next_wave_at straight off and hands the client a countdown to it, so there
-- is nothing else to set up here: the client runs the clock down itself.
local function arm_next_wave(tick, gap)
  next_wave_at = tick + gap
end

-- ---------------------------------------------------------------------
-- The tree ring: the map's only forest replenishment. A one-square-thick
-- circle at the radius of the six inner bases, re-seeded a few squares at a
-- time forever. Trees come back exactly where the defenders are dug in —
-- close enough to harvest under fire, far enough out that the fight for the
-- centre decides whether they ever reach them.
local TREE_RING_R      = 6     -- the circle the six win/loss bases sit on
local TREE_RING_PERIOD_S = 30
local TREE_RING_PICKS  = 6

local tree_ring    = {}
local next_ring_at = nil

-- A rounded radius gives a closed, single-square-thick circle; a plain
-- dx*dx+dy*dy == R*R test leaves gaps on the diagonals. The six inner bases
-- sit on this circle and can never take a tree, so they are cut once here
-- instead of being re-tested forever; everything else that blocks planting
-- can move or be rebuilt, so it is checked live at plant time.
local function build_tree_ring()
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
         and not blocked[x * 256 + y] and writable(x, y) then
        tree_ring[#tree_ring + 1] = { x = x, y = y }
      end
    end
  end
end

-- Ground a tree will take. Walls and water refuse one; a mined square is
-- skipped because planting over it would eat the mine someone laid, and road
-- is skipped because paving the defenders laid themselves must never be
-- overgrown by the script.
local function ring_plantable(t)
  if t == nil or t == T_DEEP_SEA then return false end
  if t == T_BUILDING or t == T_HALFBUILDING then return false end
  if t == T_RIVER or t == T_BOAT then return false end
  if t == T_ROAD then return false end
  if t >= T_MINE_START then return false end
  return true
end

-- Draw TREE_RING_PICKS squares with replacement: a duplicate draw lands on
-- the square the previous one just planted and counts as already-forest,
-- which is cheaper than tracking picks and makes the per-tick yield honestly
-- random rather than guaranteed.
local function replenish_tree_ring()
  local n = #tree_ring
  if n == 0 then return end

  -- Structures move: pills get scooped, carried and re-placed, so a base or
  -- pill standing on a ring square is re-checked every time rather than
  -- baked into the ring at setup.
  local occupied = {}
  for b = 1, game.num_bases() do
    local bi = game.base(b)
    if bi then occupied[bi.x * 256 + bi.y] = true end
  end
  for p = 1, game.num_pills() do
    local pi = game.pill(p)
    if pi and not pi.in_tank then occupied[pi.x * 256 + pi.y] = true end
  end

  for _ = 1, TREE_RING_PICKS do
    local s = tree_ring[math.random(n)]
    local t = game.map_tile(s.x, s.y)
    if t ~= T_FOREST and not occupied[s.x * 256 + s.y] and ring_plantable(t) then
      game.set_tile(s.x, s.y, T_FOREST)
    end
  end
end

-- ---------------------------------------------------------------------
-- The shallow rim: one square of river all the way around the island's
-- coast. Driving off the edge of a circular island is far too easy, and deep
-- sea drowns a tank outright, which is a stupid way to lose a defender in
-- the middle of a wave. A one-square shallow lip turns that mistake into a
-- swim back ashore.
--
-- The rim is derived from square CONTENTS, never from the island's radius:
-- every deep square that touches a non-deep one becomes river. The map file
-- is hand-edited, so a hardcoded circle would drift off the real coastline
-- the first time someone carves a bay.
--
-- EXCLUDED: the little deep puddle at the middle of the map. The six human
-- starts sit in it on boats by design, and lining it with shallows would
-- open a swimmable lane straight into the sanctuary. It is the only deep
-- water within RIM_EXCLUDE_R of the centre, so a plain radius cut spares it.
--
-- The bot starts sit at r=33, three squares clear of the coast and touching
-- no land at all, so they stay deep and the wave still arrives by boat.
local RIM_EXCLUDE_R = 6

-- Read the whole map once as a string rather than calling map_tile 65,536
-- times: a hook has a million instructions and a square-at-a-time scan of
-- the map spends most of them. Rows that are all deep sea are skipped with
-- one C-level find, so the scan only really walks the island.
--
-- Two passes on purpose, and river never counts as coast. A converted square
-- is no longer deep, so writing during the scan would let the rim seed
-- itself one square further out at every step; collecting first keeps every
-- test against the map as it stands.
local function build_shallow_rim()
  local ter = game.terrain()
  if ter == nil then return end
  local deep = string.char(T_DEEP_SEA)

  local function at(x, y)
    if x < 0 or x > 255 or y < 0 or y > 255 then return nil end
    return string.byte(ter, y * 256 + x + 1)
  end

  local conv, seen = {}, {}
  local excl2 = RIM_EXCLUDE_R * RIM_EXCLUDE_R
  for y = 0, 255 do
    local row = string.sub(ter, y * 256 + 1, y * 256 + 256)
    local first = string.find(row, "[^" .. deep .. "]")
    if first ~= nil then
      local last = 256 - string.find(string.reverse(row),
                                     "[^" .. deep .. "]") + 1
      -- Walk out from the LAND rather than testing every ocean square's
      -- neighbours: the same rim for a fraction of the lookups.
      for x = first - 1, last - 1 do
        local t = at(x, y)
        if t ~= nil and t ~= T_DEEP_SEA and t ~= T_RIVER then
          for ox = -1, 1 do
            for oy = -1, 1 do
              local nx, ny = x + ox, y + oy
              local key = nx * 256 + ny
              if not seen[key] and at(nx, ny) == T_DEEP_SEA then
                seen[key] = true
                local dx, dy = nx - 128, ny - 128
                if dx * dx + dy * dy > excl2 and writable(nx, ny) then
                  conv[#conv + 1] = { x = nx, y = ny }
                end
              end
            end
          end
        end
      end
    end
  end

  -- A frame carries at most 256 map changes, and the setup also lays roads,
  -- so the rim is drained a slice a tick from on_tick instead of written in
  -- one go. Two ticks on the shipped map: forty milliseconds, and the beach
  -- is there before anybody has driven anywhere.
  rim_todo = conv
  rim_at   = 1
  game.log(string.format("Survival: %d coast square(s) to shallow", #conv))
end

local function drain_rim()
  if rim_at > #rim_todo then return end
  local stop = math.min(rim_at + RIM_PER_TICK - 1, #rim_todo)
  for i = rim_at, stop do
    local c = rim_todo[i]
    game.set_tile(c.x, c.y, T_RIVER)
  end
  rim_at = stop + 1
  if rim_at > #rim_todo then rim_todo = {} end
end

-- ---------------------------------------------------------------------
-- Pre-built defender pills.
--
-- A bot cannot sensibly build a pill it is carrying — it just wanders with
-- it in-tank — so a defender BOT would meet the wave with a dead pill on
-- board and nothing built. For every defender seat that is a bot we take the
-- pill nearest that seat's start, slide it out along its base-to-pill line to
-- where that line meets the ring road, build it there at full armour, and
-- road it back to the base, so the defence is spread around the ring and
-- ready. Human defenders are skipped on purpose: a human carries the pill
-- in-tank and builds it where they choose.
--
-- Only the six centre pills are considered, and at this point they are the
-- only pills that exist.
--
-- The seats are the DEFENDERS' TEAM's, not slots 0..5. A lobby-hosted round
-- puts the horde's held seats in the low slots and the host's own bots above
-- them, and this pass used to look only at 0..5: it found the host, who is
-- not a bot, and five held horde seats, which are not fielded, so it built
-- nothing and every bot met wave 1 with a dead pill in its tank.
local BUILT_PILL_ARMOUR = 15

-- Paint road along the straight line between two squares, skipping both ends
-- and never stamping over deep sea or outside the writable frame.
local function draw_road_line(x0, y0, x1, y1)
  local dx, dy = math.abs(x1 - x0), math.abs(y1 - y0)
  local sx = (x0 < x1) and 1 or -1
  local sy = (y0 < y1) and 1 or -1
  local err = dx - dy
  local x, y = x0, y0
  while true do
    if not (x == x0 and y == y0) and not (x == x1 and y == y1)
       and writable(x, y) then
      local t = game.map_tile(x, y)
      if t ~= nil and t ~= T_DEEP_SEA then
        game.set_tile(x, y, T_ROAD)
      end
    end
    if x == x1 and y == y1 then break end
    local e2 = 2 * err
    if e2 > -dy then err = err - dy; x = x + sx end
    if e2 <  dx then err = err + dx; y = y + sy end
  end
end

-- Where the line from the base through the pill first meets the ring road:
-- march outward from the pill until a road square. That is where the pill is
-- built, so the defence spreads onto the ring instead of bunching at the
-- centre. nil if the ray never hits a road.
local function ring_road_spot(bx, by, px, py)
  local dx, dy = px - bx, py - by
  if dx == 0 and dy == 0 then dx, dy = px - 128, py - 128 end
  if dx == 0 and dy == 0 then return nil end
  local len = math.sqrt(dx * dx + dy * dy)
  local ux, uy = dx / len, dy / len
  for step = 1, 120 do
    local x = math.floor(px + ux * step + 0.5)
    local y = math.floor(py + uy * step + 0.5)
    if not writable(x, y) then break end
    if game.map_tile(x, y) == T_ROAD then return x, y end
  end
  return nil
end

-- Dig one seat in: slide its pill out to the ring road, build it there, and
-- pave the line back to the base it pairs with.
local function dig_in(p, n, pi)
  local base = game.base(CENTER_FIRST - 1 + n)
  local rx, ry
  if base ~= nil then
    rx, ry = ring_road_spot(base.x, base.y, pi.x, pi.y)
  end
  if rx == nil then rx, ry = pi.x, pi.y end
  -- One op where the old host needed a hide and a show: move_pill is what
  -- that pair was standing in for.
  game.move_pill(n, rx, ry)
  game.set_pill_owner(n, p)
  game.set_pill_armour(n, BUILT_PILL_ARMOUR)
  if base ~= nil then
    draw_road_line(base.x, base.y, rx, ry)
  end
  return rx, ry
end

-- Which pills a seat may take, best first. The rank is the whole of the
-- rule that keeps a person's pill on the ground:
--
--   1  its own — the deal already gave this seat this station, and with a
--      full keep every bot matches here and nobody is robbed of anything
--   2  nobody's
--   3  a spare — its owner holds more than one of the six
--   4  another BOT's only one, which is the last resort: that bot is still
--      owed a pill and looks again on the same pass or the next tick
--
-- A HUMAN holding exactly one is not on the list at all. That single dead
-- pill is the one they scoop and place themselves, and the pre-build must
-- never take it.
--
-- A human's spare is counted in DEAD pills only: a pill built for an empty
-- start (prebuild_empty_starts) is not one they can scoop and place.
local function take_rank(p, o, holds, dead)
  if o == p then return 1 end
  if o == nil then return 2 end
  local ol = game.lobby_slot(o)
  local bot = ol ~= nil and ol.bot
  if (bot and holds[o] or dead[o] or 0) > 1 then return 3 end
  if bot then return 4 end
  return nil
end

-- Dig in every defender BOT that is not dug in yet, one pill each.
--
-- Once per seat per round, and only until the first wave lands. A seat is
-- owed a pill because it reached the field before the fight, not because
-- its pill is dead: a pill that has been shot down stays down where it
-- fell, for its owner to repair or the horde to take. Without the record
-- this pass read "dead pill, bot with no pill standing" as "owed", and a
-- defender's pill that reached zero jumped back onto the ring road at full
-- armour the same tick. Reading the world is still what decides who is
-- owed among the seats not yet served, so the pass is idempotent: with
-- nobody left to serve it reads six pills and the roster and writes
-- nothing. It runs every tick before the first wave because a seat can
-- reach the field after the setup — a host adding a bot in the lobby's
-- last seconds — and the pass that ran at the setup would otherwise be
-- the only one there ever was.
local dug_in = {}       -- seat -> true once this round has served it
local empty_built = {}  -- pill -> true when prebuild_empty_starts built it

local function prebuild_bot_pills()
  local pills = {}
  local holds = {}      -- seat -> how many of the six it holds
  local dead  = {}      -- seat -> how many of those are dead
  local built = {}      -- seat -> true once one of them stands
  local owed  = {}

  for n = 1, CENTER_PILLS do
    local pi = game.pill(n)
    pills[n] = pi
    if pi ~= nil and pi.owner ~= nil then
      holds[pi.owner] = (holds[pi.owner] or 0) + 1
      if pi.armour > 0 then built[pi.owner] = true
      else dead[pi.owner] = (dead[pi.owner] or 0) + 1 end
    end
  end

  for _, p in ipairs(seats_on(DEF_TEAM)) do
    local ls = game.lobby_slot(p)
    if ls ~= nil and ls.bot and ls.fielded and not built[p]
       and not dug_in[p] and game.tank(p) ~= nil then
      owed[#owed + 1] = p
    end
  end
  if #owed == 0 then return end

  for _, p in ipairs(owed) do
    -- The same rule on_choose_start places this seat by, so the pill it is
    -- dealt is the one nearest the puddle position it actually stands on.
    -- A tank on the field is better still: the rank is what put it there.
    local t  = game.tank(p)
    local si = t ~= nil and { x = t.mx, y = t.my }
               or game.start(1 + ((seat_rank(p) or p) % 6))
    if si ~= nil then
      local best, bestr, bestd
      for n = 1, CENTER_PILLS do
        local pi = pills[n]
        if pi ~= nil and not pi.in_tank and pi.armour == 0 then
          local r = take_rank(p, pi.owner, holds, dead)
          if r ~= nil then
            local ddx, ddy = pi.x - si.x, pi.y - si.y
            local d = ddx * ddx + ddy * ddy
            if bestr == nil or r < bestr or (r == bestr and d < bestd) then
              best, bestr, bestd = n, r, d
            end
          end
        end
      end
      -- No dead pill to take: a pill built for an empty start stands where
      -- a late seat's station is, so the seat takes that one over as it is.
      if best == nil then
        for n = 1, CENTER_PILLS do
          local pi = pills[n]
          if empty_built[n] and pi ~= nil and not pi.in_tank
             and pi.armour > 0 then
            local ddx, ddy = pi.x - si.x, pi.y - si.y
            local d = ddx * ddx + ddy * ddy
            if bestd == nil or d < bestd then best, bestd = n, d end
          end
        end
        if best ~= nil then
          empty_built[best] = nil
          game.set_pill_owner(best, p)
          pills[best].owner = p
          built[p] = true
          dug_in[p] = true
          best = nil
        end
      end
      if best ~= nil then
        local pi   = pills[best]
        local prev = pi.owner
        local rx, ry = dig_in(p, best, pi)
        -- The running counts, so two seats on this same pass cannot take the
        -- same pill and a seat robbed of a spare is not robbed of it twice.
        pills[best] = { x = rx, y = ry, owner = p,
                        armour = BUILT_PILL_ARMOUR, in_tank = false }
        if prev ~= nil then
          holds[prev] = (holds[prev] or 1) - 1
          dead[prev]  = (dead[prev] or 1) - 1
        end
        holds[p] = (holds[p] or 0) + 1
        built[p] = true
        dug_in[p] = true
      end
    end
  end
end

-- A short-handed keep: with fewer than six defenders on the field, the
-- puddle starts nobody stands on get their pill already built. For each
-- empty start, the dead pill nearest it is dug in (moved to the ring road,
-- built, paved back to its base) and flies defender colours.
--
-- Runs once, after the deal and the bot pre-build, so every bot has already
-- taken the pill at its own start. The same rule as take_rank keeps a human's
-- pill on the ground: a pill whose human owner holds only that one is never
-- taken, so everyone still has a dead pill to scoop and place themselves.
-- A defender bot that reaches the field later takes one of these over
-- (prebuild_bot_pills) when no dead pill is left for it.
local function prebuild_empty_starts()
  local defenders = fielded_defenders()
  if #defenders == 0 or #defenders >= CENTER_PILLS then return end

  -- The starts in use: the same rule on_choose_start places a defender by.
  local used = {}
  for _, p in ipairs(defenders) do
    used[1 + ((seat_rank(p) or p) % 6)] = true
  end

  local pills = {}
  local holds = {}
  for n = 1, CENTER_PILLS do
    local pi = game.pill(n)
    pills[n] = pi
    if pi ~= nil and pi.owner ~= nil then
      holds[pi.owner] = (holds[pi.owner] or 0) + 1
    end
  end

  local built = 0
  for s = 1, CENTER_PILLS do
    local si = game.start(s)
    if not used[s] and si ~= nil then
      local best, bestd
      for n = 1, CENTER_PILLS do
        local pi = pills[n]
        if pi ~= nil and not pi.in_tank and pi.armour == 0 then
          local o = pi.owner
          local ok = o == nil or (holds[o] or 0) > 1
          if not ok then
            local ol = game.lobby_slot(o)
            ok = ol ~= nil and ol.bot   -- a bot's spare; its own is built
          end
          if ok then
            local ddx, ddy = pi.x - si.x, pi.y - si.y
            local d = ddx * ddx + ddy * ddy
            if bestd == nil or d < bestd then best, bestd = n, d end
          end
        end
      end
      if best ~= nil then
        local pi    = pills[best]
        local prev  = pi.owner
        local owner = prev
        if owner == nil or not is_defender(owner) then owner = defenders[1] end
        local rx, ry = dig_in(owner, best, pi)
        pills[best] = { x = rx, y = ry, owner = owner,
                        armour = BUILT_PILL_ARMOUR, in_tank = false }
        empty_built[best] = true
        -- Built now, so it no longer counts as a dead pill its owner holds.
        if prev ~= nil then holds[prev] = (holds[prev] or 1) - 1 end
        built = built + 1
      end
    end
  end
  game.log(string.format(
    "Survival: [pills] %d defender(s), %d empty start pill(s) built",
    #defenders, built))
end

-- How long the arrangement waits for the whole defending side to reach the
-- field before it goes ahead with whoever is there. A seat that never fields
-- must not hold the keep undealt for the round.
local ARRANGE_WAIT_S = 2
local arrange_deadline = nil

-- Defender BOT seats that are on the field in the roster but have no tank.
local function defender_bots_pending()
  local n = 0
  for _, p in ipairs(seats_on(DEF_TEAM)) do
    local ls = game.lobby_slot(p)
    if ls ~= nil and ls.bot and ls.fielded and game.tank(p) == nil then
      n = n + 1
    end
  end
  return n
end

-- The deal and the pre-build are one step, and the pre-build is the second
-- half of it: it re-owns and moves pills the deal has just handed out, so a
-- pre-build run against an undealt keep would dig a bot in on somebody
-- else's station. False while nobody is on the field yet — which is what a
-- lobby-less harness looks like until it joins its players — and then tried
-- again from on_tick until it takes.
--
-- The deal is the half that cannot be redone: set_base_owner drains a base
-- it moves, so re-dealing the keep to a latecomer would empty six bases in
-- the middle of a round. So the deal waits for the whole defending side,
-- and ARRANGE_WAIT_S is the bound on that wait.
local function arrange_defence()
  local now = game.tick()
  if arrange_deadline == nil then
    arrange_deadline = now + secs(ARRANGE_WAIT_S)
  end
  if defender_bots_pending() > 0 and now < arrange_deadline then
    return false
  end
  if not deal_center() then return false end
  prebuild_bot_pills()
  prebuild_empty_starts()
  return true
end

-- ---------------------------------------------------------------------
-- The round.

-- The setup arranges the world the round begins in. It rides the opening
-- snapshot's own base and pill lists and writes no newswire line, so the
-- deal lands before any client sees the world and nothing changes alliance
-- on the newswire at tick 0. Roster ops are refused here and none is needed:
-- the horde's seats are HELD by the lobby, so there is nothing to pull off
-- the field.
function on_setup()
  dealt = arrange_defence()
  build_tree_ring()
  build_shallow_rim()        -- collects; on_tick drains it
end

function on_start()
  -- The horde's seats, read once. A seat a wave fields and hands back is the
  -- same seat, so the list keeps for the whole round.
  seats = {}
  for p = 0, game.max_tanks() - 1 do
    local ls = game.lobby_slot(p)
    if ls and ls.bot and ls.team == WAVE_TEAM and not ls.fielded then
      seats[#seats + 1] = p
    end
  end
  -- The one line the round opens with. It carries no wave count and no
  -- seconds: the panel is up from the round's first tick with "GET READY!",
  -- the first wave's countdown and which of the five is coming, and a
  -- newswire line saying the same thing scrolls away within the minute.
  game.message("*** SURVIVAL: dig in! ***")
  -- The operator's own line. game.message reaches players and not the
  -- console, so an operator watching a headless run sees the round open
  -- here and nowhere else.
  game.log(string.format("Survival: %d horde seat(s) held, grace %ds",
                         #seats, GRACE_S))
end

function on_tick(tick)
  if ended then return end

  -- Late deal for a lobby-less harness, which joins players after ticking
  -- starts. The pre-build rides with it: it is the half of the arrangement
  -- that used to run once at the setup and never again, so a round whose
  -- defenders were not on the field yet got no dug-in bots at all.
  --
  -- Once the keep is dealt the pre-build keeps going as a catch-up: a seat
  -- that reaches the field later still gets dug in, and a pass with nothing
  -- owed reads the six pills and the roster and writes nothing.
  if not dealt then
    dealt = arrange_defence()
  elseif wave == 0 then
    prebuild_bot_pills()
  end

  -- Finish the coast the setup started.
  drain_rim()

  -- Lift a pending newswire mute. Ahead of every early return below, and of
  -- the loss check's end_round, so the mute always comes off on its own
  -- clock whatever the round is doing.
  if newswire_unmute_at ~= nil and tick >= newswire_unmute_at then
    newswire_unmute_at = nil
    set_newswire_mute(false)
  end

  -- INSTANT LOSS: the round is over the moment no inner base is in defender
  -- hands — stolen or neutralised, the centre has fallen.
  --
  -- Only once the centre has actually been dealt. Before the deal the six
  -- bases still carry the map file's owners, and those are slot numbers,
  -- which name the horde's held seats in a lobby-hosted round: reading a
  -- side off them would end the round on its first tick.
  if dealt then
    local held = 0
    for b = CENTER_FIRST, CENTER_FIRST + 5 do
      local bi = game.base(b)
      if bi and bi.owner ~= nil and is_defender(bi.owner) then
        held = held + 1
      end
    end
    if held == 0 then
      ended = true
      newswire_unmute_at = nil
      set_newswire_mute(false)
      game.end_round(
        "*** The centre has fallen -- the attackers take the island! ***",
        WAVE_TEAM)
      return
    end
  end

  -- The status panel, on the same footing as the tree ring below: ahead of
  -- every early return, so it keeps its cadence whatever the round is doing,
  -- and after the loss check, so a fallen centre does not draw one last
  -- frame on its way out.
  if panel_next_at == nil or tick >= panel_next_at then
    panel_next_at = tick + secs(PANEL_PERIOD_S)
    panel_draw(tick)
  end

  -- The tree ring, on its own steady clock. Ahead of every early return
  -- below, so it keeps its cadence through the grace period, the live waves
  -- and the breathers alike.
  if next_ring_at == nil then
    next_ring_at = tick + secs(TREE_RING_PERIOD_S)
  elseif tick >= next_ring_at then
    next_ring_at = tick + secs(TREE_RING_PERIOD_S)
    replenish_tree_ring()
  end

  -- Arm wave 1 off the round's first running tick.
  if wave == 0 and next_wave_at == nil then
    arm_next_wave(tick, secs(GRACE_S))
    return
  end

  if next_wave_at ~= nil then
    -- Newswire off MUTE_LEAD_S before the wave lands, so the ten join lines
    -- the staggered arrival would write never appear. The panel keeps
    -- drawing through the mute, so a player still has the countdown.
    if tick >= next_wave_at - secs(MUTE_LEAD_S) then
      newswire_unmute_at = nil
      set_newswire_mute(true)
    end
    -- There is no spoken countdown any more. The panel hands the client one
    -- `timer` primitive aimed at next_wave_at and the client runs it down
    -- itself, which is the same information every second instead of at two
    -- marks, and it costs one message a second rather than one a mark.
    if tick >= next_wave_at then
      next_wave_at = nil
      spawn_wave()
      pump_spawn_queue(tick)    -- the first attacker lands on the wave tick
    end
    return
  end

  -- A wave is live: bring in whatever of it is still arriving, keep the
  -- roster pruned, and vanish every attacker once the wave limit runs out.
  -- Deaths do not end a wave — the attackers respawn at their outer starts,
  -- fully armed, and press until the clock says otherwise.
  pump_spawn_queue(tick)
  prune_wave_bots()

  -- The wave's own clock. Nothing is said on the way down: the panel's
  -- "WAVE ENDS IN" timer counts the same clock off wave_ends_at every
  -- second, which is what the half-minute and per-minute lines used to do
  -- twice and five times a wave.
  if wave_ends_at ~= nil and tick >= wave_ends_at then
    game.log(string.format("Survival: [wave] %d clock ran out at tick %d at %s",
      wave, tick, os.date("%H:%M:%S")))
    wave_ends_at = nil
    -- Mute FIRST; vanish_wave then holds its first removal for MUTE_LEAD_S,
    -- so the newswire is already silent by the time the first attacker
    -- disappears.
    newswire_unmute_at = nil
    set_newswire_mute(true)
    vanish_wave(tick)
  end

  -- One attacker leaves per VANISH_SPACING_S; the wave only counts as over
  -- on the tick the last one is actually gone. The between-wave clock starts
  -- from there rather than from the tick the wave's own clock ran out, so
  -- the breather is a full breather of empty field instead of one that
  -- starts while ten tanks are still driving around.
  if pump_vanish_queue(tick) then
    restore_horde_estate()
    newswire_unmute_at = tick + secs(MUTE_TAIL_S)
    if wave >= WAVES then
      ended = true
      newswire_unmute_at = nil
      set_newswire_mute(false)
      game.end_round(string.format(
        "*** All %d waves survived -- the defenders win! ***", WAVES),
        DEF_TEAM)
    else
      -- The field is empty: start the breather. The wave's leftover dead
      -- pills stay where they lie, and they are the horde's again either way
      -- — restore_horde_estate just put every one the defenders did not
      -- capture back in its recorded seat — so the breather is played
      -- against the horde's guns, standing on the ground the last wave left
      -- them on.
      --
      -- Nothing is said here either. The panel's breather state puts up
      -- "WAVE n/5 SURVIVED", the countdown to the next one, and the
      -- "UP TO n PILLS WILL TURN" warning about the seizure that opens it —
      -- which is the whole of what the two lines that used to sit here said,
      -- and it stays on screen for the length of the breather instead of
      -- scrolling away.
      arm_next_wave(tick, secs(BREATHER_S))
    end
  end
end
