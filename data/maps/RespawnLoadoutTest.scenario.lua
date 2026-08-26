-- =========================================================================
-- RespawnLoadoutTest — automated regression scenario (runs on the SERVER).
--
-- Proves the invariant a survival map depends on: a bot spawned with
--   game.spawn_bot(name, brain, team, "open")
-- is handed the FULL OPEN loadout on EVERY life, not only its first.
--
-- Two different engine paths hand a tank its starting items, and they are
-- separate code:
--   * tankCreate()  — the join / first spawn
--   * tankDeath()   — the server-authoritative death respawn
-- Both must consult GameSim::spawnLoadout[slot]. This scenario exercises the
-- second one for real, repeatedly.
--
-- WHAT MAKES THE MEASUREMENT VALID
--
-- Two things have to be true or "40/40/40/40 after a respawn" proves nothing:
--
--  1. The tank must have SPENT its stock before dying. A tank that never
--     fires still reads 40 shells at the instant it comes back whether or not
--     the engine refuelled it — the number never moved. So the probe bot runs
--     tests/brains/fire_north_in_place.lua, which parks on the start and
--     empties its magazine northwards down a lane the map keeps clear of
--     pillboxes. The scenario records the shell count on the last tick before
--     each death and REQUIRES it to be below full; only then does a reading of
--     40 on the next life prove a refuel actually happened.
--
--  2. Full must not be what this map hands out anyway. gameTypeGetItems gives
--     gameOpen AND gameScripted the same full tank, so if the sim were running
--     scripted rules an open override and no override would be identical. The
--     control bot below measures the sim's own rules and fails loudly if they
--     come back full. (See the runner's -ranked note.)
--
-- The map (data/maps/RespawnLoadoutTest.map, tests/generate_respawn_loadout_map.py)
-- is a 13x13 grass island between two columns of neutral full-armour pillboxes,
-- surrounded by deep sea, whose only start is the tile between them. The bot
-- cannot survive and cannot leave, and every respawn drops it back in.
--
-- Verdict lines go to the server console via game.message (which calls
-- serverSimConsoleMessage), so the runner just greps stdout:
--   RESPAWN_LOADOUT_TEST PASS n=<respawns>
--   RESPAWN_LOADOUT_TEST FAIL ...
-- =========================================================================

scenario = {
  name = "RespawnLoadoutTest",
  description = "Automated test map: one open-mode bot in a pillbox death "
    .. "trap, asserting its loadout on every respawn.",
  max_players = 1,
  -- Humans play TOURNAMENT here; the bot's "open" spawn mode has to beat it
  -- on every life. No default_brain: both bots fall back to the server's
  -- -brain argument, which the runner points at the probe brain.
  game = "tournament",
}

-- No lobby enemy team, no extra teams, and never let the engine's all-bases
-- sweep end the round (the map has no bases anyway).
function enemy_bots(game) return 0 end
function show_add_team_button(game) return false end
function allow_base_win(game) return false end

-- The full open loadout, from gametype.h TANK_FULL_*.
local FULL = 40
local NEED_RESPAWNS = 5      -- lives 2..6 all checked
local SPAWN_TICK = 20        -- let the round settle before spawning

local slot = nil
local control_slot = nil
local control_done = false
local was_dead = false
local respawns = 0
local verdict = nil
local first_life_checked = false
local last_alive = nil       -- stats from the last tick the tank was alive

local function stats(t)
  return string.format("sh=%d mn=%d tr=%d ar=%d",
                       t.shells, t.mines, t.trees, t.armour)
end

local function full_loadout(t)
  return t.shells == FULL and t.mines == FULL
     and t.trees == FULL and t.armour == FULL
end

-- `game` is a per-call argument, not a global, so it is threaded through.
local function fail(game, msg)
  verdict = "fail"
  game.message("RESPAWN_LOADOUT_TEST FAIL " .. msg)
  game.end_round("respawn loadout test failed")
end

-- The trap has to stay a trap. If the probe ever managed to shoot a pillbox
-- down, later lives would last longer or stop ending at all, and the run would
-- degrade into a timeout rather than an answer.
local function trap_intact(game)
  for i = 1, game.num_pills() do
    local p = game.pill(i)
    if p == nil or p.armour == 0 then
      return false, i
    end
  end
  return true, 0
end

function on_tick(game, tick)
  if verdict ~= nil then return end

  -- Spawn the probe bot once, in OPEN mode, on its own team.
  if slot == nil then
    if tick < SPAWN_TICK then return end
    local s, err = game.spawn_bot("LoadoutProbe", nil, 2, "open")
    if s == nil then
      verdict = "fail"
      game.message("RESPAWN_LOADOUT_TEST FAIL spawn_bot: " .. tostring(err))
      game.end_round("respawn loadout test could not spawn its bot")
      return
    end
    slot = s
    game.message("RESPAWN_LOADOUT_TEST spawned slot=" .. tostring(slot))
    return
  end

  local t = game.tank(slot)
  if t == nil then
    verdict = "fail"
    game.message("RESPAWN_LOADOUT_TEST FAIL bot vanished at t=" .. tick)
    game.end_round("respawn loadout test lost its bot")
    return
  end

  -- ANTI-DEGENERACY CONTROL. Spawn a second bot with NO spawn mode — it is
  -- armed with whatever game type the sim is actually running — read its
  -- loadout, and require it to be something other than a full tank. Under the
  -- tournament rules this map is meant to run, with zero bases, that reads
  -- 0 shells / 0 mines / 0 trees.
  --
  -- This guard earns its keep: gameTypeGetItems gives gameScripted the SAME
  -- full tank as gameOpen, and a dedicated server booted with -nolobby ends up
  -- on gameScripted (serverInstanceStartup runs the skip-lobby StartGame —
  -- which applies scenario.game — and only THEN calls
  -- serverSimApplyScenarioCommit, which stamps gameScripted over it). In that
  -- configuration an open override and no override at all are
  -- indistinguishable and the test would pass no matter what the respawn path
  -- did. The runner therefore adds -ranked, the one case
  -- serverSimApplyScenarioCommit leaves the game type alone. The control is
  -- removed immediately so it neither soaks up pillbox fire nor perturbs the
  -- probe's death cadence.
  if not control_done then
    if control_slot == nil then
      local c, cerr = game.spawn_bot("ControlProbe", nil, 2)
      if c == nil then
        verdict = "fail"
        game.message("RESPAWN_LOADOUT_TEST FAIL control spawn_bot: "
                     .. tostring(cerr))
        game.end_round("respawn loadout test could not spawn its control")
        return
      end
      control_slot = c
      return
    end
    local ct = game.tank(control_slot)
    if ct == nil then
      verdict = "fail"
      game.message("RESPAWN_LOADOUT_TEST FAIL control tank missing")
      game.end_round("respawn loadout test lost its control")
      return
    end
    game.message(string.format(
      "RESPAWN_LOADOUT_TEST control(sim rules) slot=%d %s",
      control_slot, stats(ct)))
    local degenerate = full_loadout(ct)
    game.remove_bot(control_slot)
    control_slot = nil
    control_done = true
    if degenerate then
      fail(game, string.format(
        "DEGENERATE FIXTURE: a bot spawned under the SIM's own rules also got "
        .. "%s, so a full tank proves nothing about the open override (the sim "
        .. "is not running tournament — check that the runner passes -ranked)",
        stats(ct)))
    end
    return
  end

  -- Life 1: the tankCreate path. Checked once, on the first tick the bot
  -- exists, before anything can spend the stock.
  if not first_life_checked then
    first_life_checked = true
    game.message(string.format(
      "RESPAWN_LOADOUT_TEST life=1 t=%d %s", tick, stats(t)))
    if not full_loadout(t) then
      fail(game, string.format(
        "first spawn t=%d %s want sh=40 mn=40 tr=40 ar=40", tick, stats(t)))
    end
    return
  end

  if t.dead then
    was_dead = true
    return
  end

  -- dead -> alive: the engine has just refuelled and repositioned the tank.
  if was_dead then
    was_dead = false
    respawns = respawns + 1

    local spent = last_alive and last_alive.shells or FULL
    game.message(string.format(
      "RESPAWN_LOADOUT_TEST life=%d t=%d %s (shells at death: %d)",
      respawns + 1, tick, stats(t), spent))

    local ok, badPill = trap_intact(game)
    if not ok then
      fail(game, string.format(
        "pillbox %d is dead — the probe broke its own death trap, so later "
        .. "lives are no longer comparable", badPill))
      return
    end

    -- Precondition: the tank must actually have burned ammunition, or a
    -- reading of 40 is just the number never having moved.
    if spent >= FULL then
      fail(game, string.format(
        "probe still had %d shells when it died on life %d — it never fired, "
        .. "so a full tank on respawn proves nothing (is the probe brain "
        .. "loaded and aimed?)", spent, respawns))
      return
    end

    if not full_loadout(t) then
      fail(game, string.format(
        "respawn n=%d t=%d %s want sh=40 mn=40 tr=40 ar=40 (had %d shells at "
        .. "death, so the engine did refuel — with the WRONG table)",
        respawns, tick, stats(t), spent))
      return
    end

    if respawns >= NEED_RESPAWNS then
      verdict = "pass"
      game.message("RESPAWN_LOADOUT_TEST PASS n=" .. respawns)
      game.end_round("respawn loadout test passed")
      return
    end
  end

  last_alive = t
end
