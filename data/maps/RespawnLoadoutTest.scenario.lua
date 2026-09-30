-- =========================================================================
-- RespawnLoadoutTest — an automated regression round on
-- data/maps/RespawnLoadoutTest.map.
--
-- Proves the invariant Survival depends on: a bot spawned with the open
-- loadout is handed the FULL open loadout on EVERY life, not only its
-- first.
--
-- Two different engine paths hand a tank its starting items, and they are
-- separate code:
--   * tankCreate()  — the join, the first spawn
--   * tankDeath()   — the server-authoritative respawn
-- Both must consult the seat's spawn loadout. This round exercises the
-- second one for real, repeatedly.
--
-- WHAT MAKES THE MEASUREMENT VALID
--
-- Two things have to be true or "40/40/40/40 after a respawn" proves
-- nothing.
--
--  1. The tank must have SPENT its stock before dying. A tank that never
--     fires still reads 40 shells the instant it comes back whether or not
--     the engine refuelled it — the number never moved. So the probe bot
--     runs a brain that parks on the start and empties its magazine down a
--     lane the map keeps clear of pillboxes, and this script records the
--     shell count on the last tick before each death and requires it to be
--     below full. Only then does a reading of 40 on the next life prove a
--     refuel actually happened.
--
--  2. Full must not be what this map hands out anyway. The open and the
--     scripted game types give the same full tank, so under scripted rules
--     an open override and no override at all would be identical. The
--     control bot below measures the sim's own rules and fails loudly if
--     they come back full.
--
-- The map is a 13x13 grass island between two columns of neutral
-- full-armour pillboxes, surrounded by deep sea, whose only start is the
-- square between them. The bot cannot survive and cannot leave, and every
-- respawn drops it back in.
--
-- Verdict lines go to the server console, so a runner greps stdout:
--   VERDICT PASS respawn_loadout n=<respawns>
--   VERDICT FAIL respawn_loadout <why>
--
-- PORTED (2026-09-15) onto the scenario host on main. Three changes: the
-- hooks lost their game argument, the two probes are HELD SEATS the lobby
-- declares (which is how this script learns their seat numbers — a spawn
-- that does not name a seat is answered when it lands, not when it is
-- asked), and the verdict goes out on game.log as well as game.message so a
-- runner reading stdout alone still sees it.
-- =========================================================================

scenario = {
  name = "RespawnLoadoutTest",
  description = "Automated test map: one open-mode bot in a pillbox death "
    .. "trap, asserting its loadout on every respawn.",
  api  = 1,
  -- Humans play tournament here; the bot's open spawn loadout has to beat it
  -- on every life. No brain on the team: both probes fall back to the
  -- server's -brain argument, which the runner points at the probe brain.
  game = "tournament",
  lobby = {
    max_players = 1,
    -- Two held seats. Held rather than fielded because this script has to
    -- know which seat each probe took, and naming the seat in the spawn is
    -- the only way a spawn answers with one.
    teams = {
      { id = 2, bots = 2, max_bots = 2, fielded = false },
    },
  },

  -- What each callback below does, in a line a player reads: the lobby's
  -- details dialog lists these under "What this scenario implements:".
  callbacks = {
    allow_base_win = "Holding every base does not end the round; the test ends it.",
    allow_extra_teams = "Keeps the round to the one team the test declares.",
    on_start = "Finds the two held seats the test bots use.",
    on_tick = "Spawns a control bot and an open-loadout probe, then passes only if the probe respawns five times with a full tank.",
  },
}

-- Never let the engine's all-bases sweep end the round. The map has no
-- bases, but the answer costs nothing and says so.
function allow_base_win()  return false end
function allow_extra_teams() return false end

local FULL          = 40    -- the full open loadout
local NEED_RESPAWNS = 5     -- lives 2..6 all checked
local SPAWN_TICK    = 40    -- let the round settle before spawning

local seats        = {}
local slot         = nil
local control_slot = nil
local control_done = false
local was_dead     = false
local respawns     = 0
local verdict      = nil
local first_life_checked = false
local last_alive   = nil

local function stats(t)
  return string.format("sh=%d mn=%d tr=%d ar=%d",
                       t.shells, t.mines, t.trees, t.armour)
end

local function full_loadout(t)
  return t.shells == FULL and t.mines == FULL
     and t.trees == FULL and t.armour == FULL
end

-- A verdict line is at most 128 bytes, so the reason is said short here and
-- at length on the message the players would see.
local function done(pass, short, long)
  verdict = pass and "pass" or "fail"
  game.log(string.format("VERDICT %s respawn_loadout %s",
                         pass and "PASS" or "FAIL", short))
  game.message(string.format("RESPAWN_LOADOUT_TEST %s %s",
                             pass and "PASS" or "FAIL", long or short))
  game.end_round(pass and "respawn loadout test passed"
                      or "respawn loadout test failed")
end

-- The trap has to stay a trap. If the probe ever shot a pillbox down, later
-- lives would last longer or stop ending at all, and the run would degrade
-- into a timeout rather than an answer.
local function trap_intact()
  for i = 1, game.num_pills() do
    local p = game.pill(i)
    if p == nil or p.armour == 0 then
      return false, i
    end
  end
  return true, 0
end

function on_start()
  for p = 0, game.max_tanks() - 1 do
    local ls = game.lobby_slot(p)
    if ls and ls.bot and not ls.fielded then
      seats[#seats + 1] = p
    end
  end
end

function on_tick(tick)
  if verdict ~= nil then return end

  -- Field the probe once, on the open loadout.
  if slot == nil then
    if tick < SPAWN_TICK then return end
    if seats[1] == nil then
      done(false, "no held seat for the probe")
      return
    end
    local s, code = game.spawn_bot{ slot = seats[1], name = "LoadoutProbe",
                                    loadout = "open" }
    if not s then
      done(false, "spawn_bot: " .. tostring(code))
      return
    end
    slot = seats[1]
    game.message("RESPAWN_LOADOUT_TEST spawned slot=" .. tostring(slot))
    return
  end

  local t = game.tank(slot)
  if t == nil then
    -- A queued spawn lands a tick or two after it is asked for.
    if tick < SPAWN_TICK + 20 then return end
    done(false, "bot vanished at t=" .. tick)
    return
  end

  -- ANTI-DEGENERACY CONTROL. Field a second bot with NO loadout named — it
  -- is armed with whatever game type the sim is actually running — read its
  -- stocks, and require them to be something other than a full tank. Under
  -- the tournament rules this map is meant to run, with zero bases, that
  -- reads 0 shells, 0 mines, 0 trees.
  --
  -- This guard earns its keep: the scripted game type gives the same full
  -- tank as open, so on a server that ends up scripted an open override and
  -- no override at all are indistinguishable and the test would pass
  -- whatever the respawn path did. The control is removed immediately so it
  -- neither soaks up pillbox fire nor perturbs the probe's death cadence.
  if not control_done then
    if control_slot == nil then
      if seats[2] == nil then
        done(false, "no held seat for the control")
        return
      end
      local c, code = game.spawn_bot{ slot = seats[2], name = "ControlProbe" }
      if not c then
        done(false, "control spawn_bot: " .. tostring(code))
        return
      end
      control_slot = seats[2]
      return
    end
    local ct = game.tank(control_slot)
    if ct == nil then return end       -- the spawn is still landing
    game.message(string.format(
      "RESPAWN_LOADOUT_TEST control(sim rules) slot=%d %s",
      control_slot, stats(ct)))
    local degenerate = full_loadout(ct)
    local degenerate_stats = stats(ct)
    game.remove_bot(control_slot)
    control_slot = nil
    control_done = true
    if degenerate then
      done(false, "degenerate fixture: sim rules also give a full tank",
           "DEGENERATE FIXTURE: a bot spawned under the SIM's own rules also "
           .. "got " .. degenerate_stats .. ", so a full tank proves nothing "
           .. "about the open override")
    end
    return
  end

  -- Life 1: the create path. Checked once, on the first tick the bot exists,
  -- before anything can spend the stock.
  if not first_life_checked then
    first_life_checked = true
    game.message(string.format(
      "RESPAWN_LOADOUT_TEST life=1 t=%d %s", tick, stats(t)))
    if not full_loadout(t) then
      done(false, "first spawn not full: " .. stats(t))
    end
    return
  end

  if t.dead then
    was_dead = true
    return
  end

  -- dead to alive: the engine has just refuelled and repositioned the tank.
  if was_dead then
    was_dead = false
    respawns = respawns + 1

    local spent = last_alive and last_alive.shells or FULL
    game.message(string.format(
      "RESPAWN_LOADOUT_TEST life=%d t=%d %s (shells at death: %d)",
      respawns + 1, tick, stats(t), spent))

    local ok, bad_pill = trap_intact()
    if not ok then
      done(false, string.format("pill %d dead: the trap broke", bad_pill),
           string.format("pillbox %d is dead -- the probe broke its own death "
                         .. "trap, so later lives are not comparable",
                         bad_pill))
      return
    end

    -- Precondition: the tank must actually have burned ammunition, or a
    -- reading of 40 is just the number never having moved.
    if spent >= FULL then
      done(false, string.format("probe never fired (%d shells at death)",
                                spent),
           string.format("probe still had %d shells when it died on life %d "
                         .. "-- it never fired, so a full tank on respawn "
                         .. "proves nothing", spent, respawns))
      return
    end

    if not full_loadout(t) then
      done(false, string.format("respawn %d not full: %s", respawns, stats(t)),
           string.format("respawn n=%d t=%d %s want 40/40/40/40 (had %d "
                         .. "shells at death, so the engine did refuel -- "
                         .. "with the WRONG table)",
                         respawns, tick, stats(t), spent))
      return
    end

    if respawns >= NEED_RESPAWNS then
      done(true, "n=" .. respawns)
      return
    end
  end

  last_alive = t
end
