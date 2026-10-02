-- =========================================================================
-- The compat prelude.
--
-- Our 90 arena scripts were written against the old server-side scenario
-- host, where every hook was handed a `game` table as its first argument and
-- the table carried four rows the host on main does not have. This file is
-- what lets those scripts run unchanged on the host that is on main.
--
-- HOW IT IS USED. The sandbox a scenario runs in has no `require` and no
-- `dofile` (#339), so a script cannot load this itself. The gate runner
-- (tests/scenario/run_gate.py) splits this file on the SCRIPT marker below
-- and writes one temporary file made of
--
--     <the head>  ..  <the script>  ..  <the tail>
--
-- beside a copy of the arena's map, under the name the map's own discovery
-- looks for. The server therefore reads one ordinary scenario file and knows
-- nothing about any of this.
--
-- WHAT THE HEAD DOES. It puts four missing rows on the `game` table and
-- softens three others so the calls our scripts already make are answered
-- the way the old host answered them. Every one of them is written on rows
-- the host on main does have; none of it needs C.
--
-- WHAT THE TAIL DOES. It takes every hook the script declared and puts a
-- wrapper of the host's own shape in its place, so `on_tick(g, tick)` is
-- called by a host that calls `on_tick(tick)`. It also flushes the roster
-- ops the head had to defer, and it carries the verdict deadline.
--
-- tests/scenario/README.md carries the traps these shims leave behind.
-- =========================================================================

-- Rows the head installs itself. The tail must not wrap these with a `game`
-- argument the way it wraps a script's own hooks.
local compat_installed = {}

-- A scenario table is not optional on this host: a file without one is
-- refused at the door and the round plays the map plainly, which for an
-- arena means the arena never happens. Ours needed no such table, so one is
-- declared here for every arena that does not declare its own. An arena that
-- does is read after this and wins, because the later assignment is the one
-- that stands.
--
-- The game type has to be named here. A scenario that names no game is
-- played as strict tournament whatever -gametype was passed, and a strict
-- tournament draws the tank's shells, mines and trees from the neutral share
-- of the bases, which on a map the arena has already taken every base of is
-- nothing at all. The word is the one the runner took its -gametype from, so
-- the table and the flag always name the same game.
scenario = {
  name        = GATE_NAME or "arena",
  description = "A bot-behaviour arena from tests/scenario.",
  game        = GATE_GAMETYPE or "open",
  api         = 1,
  -- The arenas field their own tanks with game.spawn_bot, which a lobby
  -- set to no bots refuses.
  needs_bots  = true,
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
-- a flag and the policy is the answer. A script that declares its own
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
-- scripts name pills by number.
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
      -- than carry on against a pill the script will name by the wrong
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
  local got, code, why = game.add_pill(px, py, game.NEUTRAL, 0, r.speed)
  for _, k in ipairs(placeholders) do
    game.remove_pill(k)              -- they were only holding their numbers
  end
  if got == nil then
    -- Hand back add_pill's own refusal rather than a guess. The refusal
    -- that actually turns up here is not a full map: it is a square that
    -- already holds a pillbox, which this host does not allow and the old
    -- one did. Collapsing that into SCN_OP_FULL sends a reader looking for
    -- the wrong thing.
    return nil, code or "SCN_OP_FULL",
           why or ("no room for pill " .. tostring(n))
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
-- The init table does reach the brain. The host hands it to the bot as the
-- BRAIN_INIT global, and brains/GoalHunter/init.lua flattens it into
-- BRAIN_INIT_ARG -- the "k=v;k=v" string its parse blocks read -- before
-- either of them runs. Two things about that flattening the arenas depend
-- on: the keys are sorted, so the same table always builds the same string;
-- and a value of "1" or true is written as the bare flag word the tick-1
-- parser matches, while "0" or false is dropped. A string already in
-- BRAIN_INIT_ARG, which is the command-line path, keeps its place and the
-- table's tokens follow it.

-- ── a driver's token string, packed into an init table ───────────────────
-- The python drivers pinned a bot's knobs with one string:
--
--   -bot-init "0=<brain>[cfg=TANK_COMBAT_ENABLED=false;cfg=BUILDER_POOL_ENABLED=false]"
--
-- spawn_bot takes a TABLE instead, and the brain flattens that table back
-- into the same string it always parsed: keys sorted, each pair written
-- `key=value`, joined with `;`. Two things stop a driver's string going
-- straight in. A Lua table has one key named `cfg`, so it can carry one
-- `cfg=` pin; and a table value is 64 bytes, where a driver's pin set runs
-- to two or three hundred.
--
-- Both are got round by the same observation: the brain SPLITS the flattened
-- string on `;` and ignores any token it does not recognise. So a value that
-- opens with a `;` puts its own `key=` on one side of a split, where nothing
-- reads it, and leaves whole tokens on the other. Chunking a driver's string
-- across `t01`, `t02` ... — which sort in order — therefore reproduces it
-- exactly:
--
--   { t01 = ";cfg=A=false;cfg=B=false", t02 = ";preset=keel" }
--     flattens to  "t01=;cfg=A=false;cfg=B=false;t02=;preset=keel"
--     splits to    t01= | cfg=A=false | cfg=B=false | t02= | preset=keel
--
-- so an arena can paste its driver's TOKENS line in unchanged:
--
--   game.spawn_bot{ slot = 1, init = game.init_tokens(TOKENS) }
--
-- A pin that will not fit is reported rather than cut, because a pin cut in
-- half is a knob quietly left at its default and a measurement that means
-- nothing.
--
-- Use this rather than writing the pairs out as a table whenever a value
-- might be "0". The flattening reads "0" as a flag switched OFF and drops the
-- token, which is right for `noblitz = "0"` and wrong for a brain that takes
-- a NUMBER of zero — `switch=0` on tests/brains/advert_capture.lua means
-- "never switch" and has to arrive. Packed inside one value by this function
-- it survives, because nothing here looks at what a token means.
local INIT_KEYS  = 16     -- the host takes 16 pairs
local INIT_VALUE = 63     -- 64 bytes, one of them the leading ';'

game.init_tokens = function(text)
  local t = {}
  if type(text) ~= "string" or text == "" then return t end
  local n, cur = 0, ""
  local function flush()
    if cur == "" then return end
    n = n + 1
    if n > INIT_KEYS then
      game.log("compat: init_tokens ran out of keys, pins dropped")
      return
    end
    t[string.format("t%02d", n)] = cur
    cur = ""
  end
  for tok in string.gmatch(text, "[^;,]+") do
    tok = string.gsub(tok, "%s", "")
    if tok ~= "" then
      if #tok + 1 > INIT_VALUE then
        game.log("compat: init token too long: " .. string.sub(tok, 1, 60))
      else
        if #cur + 1 + #tok > INIT_VALUE then flush() end
        cur = cur .. ";" .. tok
      end
    end
  end
  flush()
  return t
end

-- ── a brain path, mapped to a brain name ─────────────────────────
-- An arena names a brain the way the drivers did, by path:
-- "../brains/GoalHunter/init.lua", "../tests/brains/idle.lua". This host
-- takes a NAME -- one directory under a brains parent, holding init.lua --
-- and refuses any value with a separator in it, so an arena naming a path
-- gets SCN_OP_NOT_FOUND, the bot is never made, and the arena measures
-- nothing. The arena bodies are not edited, so the mapping happens here:
--
--   <anything>/tests/brains/<stem>.lua   ->  <stem>
--   <anything>/<name>/init.lua           ->  <name>
--
-- which is the layout the resolver itself reads. The gate stages every
-- tests/brains/*.lua as <build>/Brains/<stem>/init.lua so the first shape
-- resolves, and CMake stages GoalHunter for the second.
--
-- Anything else goes through untouched on purpose: a path that is simply
-- wrong should still be refused by the host rather than quietly turned into
-- some other brain here. Mapping a name again returns the same name, so a
-- table handed to two spawns is safe.
local function brain_name(v)
  if type(v) ~= "string" then return v end
  local s = string.gsub(v, "\\", "/")
  local stem = string.match(s, "tests/brains/([^/]+)%.lua$")
  if stem then return stem end
  local dir = string.match(s, "([^/]+)/init%.lua$")
  if dir then return dir end
  return v
end

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
  if type(a) == "table" then
    if a.brain ~= nil then a.brain = brain_name(a.brain) end
    return raw_spawn_bot(a)
  end
  local t = {}
  if a     ~= nil then t.name    = a     end
  if brain ~= nil then t.brain   = brain_name(brain) end
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
-- before its first tick. It is written down here because it is the one
-- behaviour difference the prelude makes on purpose.

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
-- A script says its verdict with verdict_pass(why) or verdict_fail(why),
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

-- A script that wants an answer at a fixed moment sets this and the tail
-- calls it once, on the first tick at or after the deadline, unless a
-- verdict has already been said. The tick is in game.tick()'s own units,
-- 100 a second.
VERDICT_AT   = VERDICT_AT or nil     -- the tick to decide on
VERDICT_CHECK = VERDICT_CHECK or nil -- function() -> ok, why

--@@SCRIPT@@

-- =========================================================================
-- The tail. Everything below runs after the script's own top level, so it
-- can see the hooks the script declared.
-- =========================================================================

-- Every hook and policy this host calls. A script's version of one takes
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

-- The script's own on_setup, on_start and on_tick are taken out of the
-- wrapping list and handled below: each has compat work to do around it.
local user = {}
for _, name in ipairs(HOOKS) do
  local f = rawget(_G, name)
  if type(f) == "function" and compat_installed[name] ~= f then
    user[name] = f
    _G[name] = function(...) return f(game, ...) end
  end
end

-- A script that wrote no allow_extra_teams gets none: no opinion is the
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
  -- asked for, before the script's own on_start sees the round.
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
