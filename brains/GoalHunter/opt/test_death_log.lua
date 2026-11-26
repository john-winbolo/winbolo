-- =========================================================================
-- test_death_log.lua — standalone unit tests (2026-11-23) for the DEBUG ONLY
-- DEATH line record in util.lua (death_log_observe, death_log_should_report,
-- death_log_reported). Recorded case: 2026-11-23 Etaka block 4, slot 0 moved
-- from one Bot Host to another while its tank was dead. The new brain's first
-- think had no tank place and default armour, then the dead flag came, and it
-- logged "DEATH ... tile=(0,0) terrain=deepsea armour=40" for a death the old
-- brain had already logged.
--   * a normal life then death reports, with the last alive place
--   * a brain that starts on a dead tank does not report
--   * a first think with no tank place (0,0) is not a life
--   * a slot move drops the old tank's record
--   * after one death, the next one needs a new alive tick
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_death_log.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

_G.EVENT_TANK_KILLED = _G.EVENT_TANK_KILLED or 6

local U = require("util")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

local function alive(pn, mx, my, arm)
  return { player_number = pn, dead = false,
           tankx = mx * 256 + 128, tanky = my * 256 + 128, armour = arm or 40 }
end
local function dead(pn)
  -- The server has already moved a dead tank to its respawn start.
  return { player_number = pn, dead = true,
           tankx = 124 * 256 + 128, tanky = 134 * 256 + 128, armour = 0 }
end

-- ── a normal life then death ─────────────────────────────────────────────
do
  local rec = { reported = false }
  U.death_log_observe(rec, alive(0, 100, 90, 40), 10)
  U.death_log_observe(rec, alive(0, 101, 90, 25), 11)
  U.death_log_observe(rec, dead(0), 12)
  check("life then death: reports", U.death_log_should_report(rec), rec.alive_tick)
  check("life then death: last alive place kept",
        rec.alive_mx == 101 and rec.alive_my == 90, tostring(rec.alive_mx) .. "," .. tostring(rec.alive_my))
  check("life then death: armour drop tick", rec.drop_tick == 11, rec.drop_tick)
  U.death_log_reported(rec)
  check("after report: no second report for the same death",
        not U.death_log_should_report(rec), rec.alive_tick)
end

-- ── Bot Host handover: brain starts on a dead tank ───────────────────────
do
  local rec = { reported = false }
  -- First think before the first real snapshot: no tank place, default armour.
  U.death_log_observe(rec, { player_number = 0, dead = false,
                             tankx = 0, tanky = 0, armour = 40 }, 31001)
  U.death_log_observe(rec, dead(0), 31002)
  check("handover: no tank place is not a life", rec.alive_tick == nil, rec.alive_tick)
  check("handover: no DEATH report", not U.death_log_should_report(rec), rec.alive_tick)
end

do
  local rec = { reported = false }
  U.death_log_observe(rec, dead(0), 500)
  check("starts dead: no DEATH report", not U.death_log_should_report(rec), rec.alive_tick)
  U.death_log_observe(rec, alive(0, 124, 134), 600)
  U.death_log_observe(rec, dead(0), 700)
  check("starts dead, respawns, dies: reports", U.death_log_should_report(rec), rec.alive_tick)
end

-- ── missing tank place fields ────────────────────────────────────────────
do
  local rec = { reported = false }
  U.death_log_observe(rec, { player_number = 1, dead = false }, 5)
  check("nil tank place: not a life", rec.alive_tick == nil, rec.alive_tick)
end

-- ── slot move with the same brain ────────────────────────────────────────
do
  local rec = { reported = false }
  U.death_log_observe(rec, alive(0, 50, 60), 100)
  U.death_log_observe(rec, dead(2), 101)
  check("slot move: old tank's record dropped", rec.alive_tick == nil, rec.alive_tick)
  check("slot move: no DEATH report", not U.death_log_should_report(rec), rec.alive_tick)
  check("slot move: new player number kept", rec.pn == 2, rec.pn)
end

-- ── kill event latch ─────────────────────────────────────────────────────
do
  local rec = { reported = false }
  U.death_log_observe(rec, alive(3, 70, 70), 200)
  local info = dead(3)
  info.events = {
    { type = EVENT_TANK_KILLED, data = { 5, 4, 2, 0 } },   -- someone else
    { type = EVENT_TANK_KILLED, data = { 255, 3, 2, 1 } }, -- us, by a pill
  }
  U.death_log_observe(rec, info, 201)
  check("event: our kill latched", rec.evt_tick == 201 and rec.evt_killer == 255
        and rec.evt_cause == 2 and rec.evt_carry == 1, rec.evt_killer)
end

-- ── a blank alive tick between two deaths ────────────────────────────────
do
  local rec = { reported = false }
  U.death_log_observe(rec, alive(0, 10, 10), 1)
  U.death_log_observe(rec, dead(0), 2)
  U.death_log_reported(rec)
  U.death_log_observe(rec, { player_number = 0, dead = false,
                             tankx = 0, tanky = 0, armour = 40 }, 3)
  U.death_log_observe(rec, dead(0), 4)
  check("blank tick between deaths: no second report",
        not U.death_log_should_report(rec), rec.alive_tick)
end

print(string.format("test_death_log: %d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
