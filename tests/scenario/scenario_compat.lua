-- =========================================================================
-- The compat prelude.
--
-- Our 90 arena sidecars were written against the old server-side scenario
-- host, where every hook was handed a `game` table as its first argument and
-- the table carried four rows the host on main does not have. This file is
-- what lets those sidecars run unchanged on the host that is on main.
--
-- HOW IT IS USED. The sandbox a scenario runs in has no `require` and no
-- `dofile` (#339), so a sidecar cannot load this itself. The gate runner
-- (tests/scenario/run_gate.py) splits this file on the SIDECAR marker below
-- and writes one temporary file made of
--
--     <the head>  ..  <the sidecar>  ..  <the tail>
--
-- beside a copy of the arena's map, under the name the map's own discovery
-- looks for. The server therefore reads one ordinary scenario file and knows
-- nothing about any of this.
--
-- WHAT THE HEAD DOES. It puts four missing rows on the `game` table and
-- softens three others so the calls our sidecars already make are answered
-- the way the old host answered them. Every one of them is written on rows
-- the host on main does have; none of it needs C.
--
-- WHAT THE TAIL DOES. It takes every hook the sidecar declared and puts a
-- wrapper of the host's own shape in its place, so `on_tick(g, tick)` is
-- called by a host that calls `on_tick(tick)`. It also flushes the roster
-- ops the head had to defer, and it carries the verdict deadline.
--
-- PORT_MAP.md explains each shim and why it is the shape it is.
-- =========================================================================

-- Rows the head installs itself. The tail must not wrap these with a `game`
-- argument the way it wraps a sidecar's own hooks.
local compat_installed = {}

-- A scenario table is not optional on this host: a file without one is
-- refused at the door and the round plays the map plainly, which for an
-- arena means the arena never happens. Ours needed no such table, so one is
-- declared here for every arena that does not declare its own. An arena that
-- does is read after this and wins, because the later assignment is the one
-- that stands.
--
-- The game type is left out on purpose: the runner passes -gametype and the
-- arenas were written against what it passes, so a scenario.game here would
-- quietly overrule the flag the arena's own GATE line asked for.
scenario = {
  name        = GATE_NAME or "arena",
  description = "A bot-behaviour arena from tests/scenario.",
  api         = 1,
}

-- ── neutral by nil ───────────────────────────────────────────────────────
-- The old host read a missing or negative owner as neutral. This one raises
-- on a missing argument, because a missing argument is a mistake in the
-- script rather than a value the world will not take.

local function owner_of(p)
  if p == nil or p < 0 then return game.NEUTRAL end
  return p
end

local raw_set_pill_owner = game.set_pill_owner
local raw_set_base_owner = game.set_base_owner

game.set_pill_owner = function(n, p)
  return raw_set_pill_owner(n, owner_of(p))
end

game.set_base_owner = function(n, p, keep_stock)
  return raw_set_base_owner(n, owner_of(p), keep_stock)
end

-- ── enemy_team_size ──────────────────────────────────────────────────────
-- Ours answered the size of lobby team 2. team_size answers any team's.

game.enemy_team_size = function()
  return game.team_size(2)
end

-- ── newswire_mute ────────────────────────────────────────────────────────
-- Ours was one switch that silenced the engine newswire everywhere. This
-- host asks the announce() policy before every line instead, so the mute is
-- a flag and the policy is the answer. A sidecar that declares its own
-- announce wins: the tail sees it and leaves this one alone.

local newswire_muted = false

game.newswire_mute = function(on)
  newswire_muted = on and true or false
  return true
end

announce = function(kind, subject, actor)
  if newswire_muted then return false end
  return nil                        -- no opinion: the ordinary rule shows it
end
compat_installed.announce = announce

-- ── hide_pill / show_pill ────────────────────────────────────────────────
-- Ours took a pillbox off the map with no carrier, remembering where it was,
-- and put it back dead. A hidden pill cannot be seen, shot, driven over,
-- repaired or captured — which is the whole point, because the bots' own
-- goal search wants a dead neutral pill on the ground and half these arenas
-- exist to measure what the bots do when one is or is not there. Parking a
-- pill in a corner is not equivalent: the brain would still route to it.
--
-- remove_pill and add_pill together do the right thing except for the slot.
-- pillsAddItem hands out the LOWEST free slot, so putting one pill back
-- while two are hidden can give it a number it did not have, and our
-- sidecars name pills by number.
--
-- So the slot is forced. To show pill n: re-add every still-hidden pill
-- below n, in ascending order, each at its own remembered square — each
-- lands on its own slot because slots fill lowest-first — then add n, which
-- now lands on n, then hide again the ones that were only holding a place.
-- add_pill and remove_pill both apply immediately rather than queueing, so
-- all of this happens inside one hook call: no tick passes and no client
-- ever sees the placeholders.

local hidden = {}          -- pill number -> the record it had when hidden

game.hide_pill = function(n)
  if hidden[n] then return true end          -- already off the map
  local pi = game.pill(n)
  if pi == nil then return nil, "SCN_OP_NO_SUCH_ITEM", "no pill " .. tostring(n) end
  if pi.in_tank then return nil, "SCN_OP_CARRIED", "pill " .. tostring(n) .. " is carried" end
  local ok, code, why = game.remove_pill(n)
  if not ok then return ok, code, why end
  hidden[n] = { x = pi.x, y = pi.y, owner = pi.owner,
                armour = pi.armour, speed = pi.speed }
  return true
end

-- Put the pills below n back so the slot count lands where it should, and
-- say which of them were only placeholders.
local function fill_below(n)
  local placed = {}
  local waiting = {}
  for k in pairs(hidden) do
    if k < n then waiting[#waiting + 1] = k end
  end
  table.sort(waiting)
  for _, k in ipairs(waiting) do
    local r = hidden[k]
    local got = game.add_pill(r.x, r.y, r.owner, r.armour, r.speed)
    if got ~= k then
      -- The slot did not come back where it was asked for. Say so rather
      -- than carry on against a pill the sidecar will name by the wrong
      -- number: a drifted slot is a test failure, not a silent wrong answer.
      game.log(string.format("compat: hide/show slot drift, wanted %d got %s",
                             k, tostring(got)))
    end
    placed[#placed + 1] = got
  end
  return placed
end

game.show_pill = function(n, x, y)
  local r = hidden[n]
  if r == nil then return nil, "SCN_OP_ALREADY", "pill " .. tostring(n) .. " is on the map" end
  local placeholders = fill_below(n)
  local px = x or r.x
  local py = y or r.y
  -- Dead on the ground and nobody's, which is what the old show_pill did.
  local got = game.add_pill(px, py, game.NEUTRAL, 0, r.speed)
  for _, k in ipairs(placeholders) do
    game.remove_pill(k)              -- they were only holding their numbers
  end
  if got == nil then
    return nil, "SCN_OP_FULL", "no room for pill " .. tostring(n)
  end
  if got ~= n then
    game.log(string.format("compat: show_pill wanted slot %d, got %d", n, got))
  end
  hidden[n] = nil
  return true
end

-- ── spawn_bot, positional to table ───────────────────────────────────────
-- Ours: spawn_bot([name][, brain][, team][, mode][, init][, start]), where
-- init is one string of `key=value;key=value` tokens.
-- This host: one table, with `loadout` where ours said `mode` and an `init`
-- that is a flat table of pairs.
--
-- A table as the first argument is passed straight through, so a ported file
-- may be written in the host's own shape.
--
-- NOTE, and it is the one thing this shim cannot fix: the host hands the
-- init table to the brain as the BRAIN_INIT global, and the GoalHunter
-- brains read BRAIN_INIT_ARG, the string. The tokens therefore do not reach
-- the brain on main. PORT_MAP.md names it as a gap. Every sidecar that
-- depends on a token is marked EXPECTED-FAIL for that reason.

local raw_spawn_bot = game.spawn_bot

local function init_table_from(text)
  local t = {}
  if type(text) == "table" then return text end
  if type(text) ~= "string" or text == "" then return t end
  local n = 0
  for token in string.gmatch(text, "[^;,]+") do
    token = string.match(token, "^%s*(.-)%s*$")
    if token ~= "" and n < 16 then
      local k, v = string.match(token, "^([^=]+)=(.*)$")
      if k == nil then k, v = token, "1" end   -- a bare flag is the value "1"
      t[k] = v
      n = n + 1
    end
  end
  return t
end

game.spawn_bot = function(a, brain, team, mode, init, start)
  if type(a) == "table" then return raw_spawn_bot(a) end
  local t = {}
  if a     ~= nil then t.name    = a     end
  if brain ~= nil then t.brain   = brain end
  if team  ~= nil then t.team    = team  end
  if mode  ~= nil then t.loadout = mode  end
  if start ~= nil then t.start   = start end
  if init  ~= nil then t.init    = init_table_from(init) end
  return raw_spawn_bot(t)
end

-- ── roster ops inside on_setup ───────────────────────────────────────────
-- This host refuses spawn_bot, remove_bot, set_team and the three lobby_*
-- ops inside on_setup with SCN_OP_WRONG_STATE: the round is still being
-- built there and a roster edit would re-enter the machinery building it.
-- Ours allowed them, and 104 of our set_team calls are in on_setup — the
-- line that puts our tank on its own team so the scripted opponents are
-- hostile to it.
--
-- So a roster op issued from on_setup is queued here and flushed at the top
-- of on_start, which the tail installs. The teams settle on the round's
-- first running tick rather than before it; nothing we have reads a team
-- before its first tick, and PORT_MAP.md says so out loud because it is the
-- one behaviour difference the port makes on purpose.

local in_setup = false
local deferred = {}

local unpack = unpack or table.unpack

-- The argument count is kept beside the arguments. `{...}` with a nil in the
-- middle of it — which the positional spawn_bot has, every time a caller
-- names a team but no brain — has a hole, and both `#` and a bare `unpack`
-- stop at the hole rather than at the end.
local function defer_roster(name)
  local raw = game[name]
  game[name] = function(...)
    if in_setup then
      local n, args = select("#", ...), { ... }
      deferred[#deferred + 1] = function() raw(unpack(args, 1, n)) end
      return true, "queued"
    end
    return raw(...)
  end
end

for _, name in ipairs({ "set_team", "remove_bot",
                        "lobby_add_bot", "lobby_remove_bot",
                        "lobby_set_team" }) do
  defer_roster(name)
end
-- spawn_bot is deferred through the shim above rather than the raw row, so
-- a deferred spawn still gets the positional-to-table translation.
do
  local shimmed = game.spawn_bot
  game.spawn_bot = function(...)
    if in_setup then
      local n, args = select("#", ...), { ... }
      deferred[#deferred + 1] = function() shimmed(unpack(args, 1, n)) end
      return true, "queued"
    end
    return shimmed(...)
  end
end

-- ── the verdict ──────────────────────────────────────────────────────────
-- game.end_round's text does not reach the server console, so a runner
-- reading stdout cannot see it. game.log does, so that is where a verdict
-- goes. A line is at most 128 bytes and the host drops a longer one
-- silently, so this cuts rather than loses the verdict.
--
-- A sidecar says its verdict with verdict_pass(why) or verdict_fail(why),
-- or answers a question with verdict(ok, why). The first one said is the
-- one that counts; a later one is ignored, so a check inside on_tick may
-- fire on every tick without the answer changing under it.

local verdict_said = false
local VERDICT_MAX  = 120        -- leaves room for the prefix inside 128

function verdict(ok, why)
  if verdict_said then return end
  verdict_said = true
  local name = TEST_NAME or GATE_NAME or game.map_name() or "test"
  local line = string.format("VERDICT %s %s %s",
                             ok and "PASS" or "FAIL", name, why or "")
  if #line > VERDICT_MAX then line = string.sub(line, 1, VERDICT_MAX) end
  game.log(line)
  game.end_round(ok and "test passed" or "test failed")
end

function verdict_pass(why) verdict(true,  why) end
function verdict_fail(why) verdict(false, why) end

-- A sidecar that wants an answer at a fixed moment sets this and the tail
-- calls it once, on the first tick at or after the deadline, unless a
-- verdict has already been said. The tick is in game.tick()'s own units,
-- 100 a second.
VERDICT_AT   = VERDICT_AT or nil     -- the tick to decide on
VERDICT_CHECK = VERDICT_CHECK or nil -- function() -> ok, why

--@@SIDECAR@@

-- =========================================================================
-- The tail. Everything below runs after the sidecar's own top level, so it
-- can see the hooks the sidecar declared.
-- =========================================================================

-- Every hook and policy this host calls. A sidecar's version of one takes
-- `game` first; the host's does not, so each is replaced by a wrapper that
-- puts the global `game` back in front of whatever the host passes.
local HOOKS = {
  "on_setup", "on_start", "on_tick", "on_end",
  "on_lobby", "on_player_join", "on_player_leave", "on_team_changed",
  "on_chat",
  "on_tank_spawned", "on_tank_killed", "on_lgm_died", "on_lgm_landed",
  "on_base_captured", "on_base_neutralized",
  "on_pill_captured", "on_pill_placed", "on_pill_picked_up", "on_pill_killed",
  "on_built", "on_mine_laid", "on_mine_explosion",
  "on_enter_region", "on_leave_region",
  "allow_extra_teams", "allow_base_win", "can_respawn", "can_build",
  "can_capture", "announce", "can_die", "on_choose_start", "spawn_loadout",
  "damage_scale",
}

-- The sidecar's own on_setup, on_start and on_tick are taken out of the
-- wrapping list and handled below: each has compat work to do around it.
local user = {}
for _, name in ipairs(HOOKS) do
  local f = rawget(_G, name)
  if type(f) == "function" and compat_installed[name] ~= f then
    user[name] = f
    _G[name] = function(...) return f(game, ...) end
  end
end

-- A sidecar that wrote no allow_extra_teams gets none: no opinion is the
-- ordinary rule, which is what the old host did with an undeclared hook.

if user.on_setup then
  _G.on_setup = function()
    in_setup = true
    local ok, err = pcall(user.on_setup, game)
    in_setup = false
    if not ok then error(err, 0) end
  end
else
  _G.on_setup = nil
end

_G.on_start = function()
  -- Flush the roster ops on_setup could not issue, in the order they were
  -- asked for, before the sidecar's own on_start sees the round.
  for i = 1, #deferred do deferred[i]() end
  deferred = {}
  if user.on_start then user.on_start(game) end
end

-- An arena that stated a check but no deadline is asked just before the
-- server's own tick limit cuts the round off, which is where the old python
-- drivers read the final state.
if VERDICT_CHECK ~= nil and VERDICT_AT == nil and GATE_TICKS ~= nil then
  VERDICT_AT = GATE_TICKS - 200
end

do
  local user_tick = user.on_tick
  _G.on_tick = function(tick)
    if not verdict_said and VERDICT_AT ~= nil and tick >= VERDICT_AT then
      VERDICT_AT = nil
      if VERDICT_CHECK ~= nil then
        local ok, why = VERDICT_CHECK(game, tick)
        verdict(ok, why)
      end
    end
    if user_tick then return user_tick(game, tick) end
  end
end
