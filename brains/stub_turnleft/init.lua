-- =========================================================================
-- stub_turnleft / init.lua — Minimal brain for isolating engine bugs.
-- =========================================================================
-- Does NOTHING except hold the turn-left key. No C bindings, no state,
-- no metatables, no globals beyond the required `return Brain` and the
-- engine-provided KEY_* constants.
--
-- Use this brain when investigating crashes that look engine-side to
-- confirm whether the bug reproduces with a trivial brain. If it
-- DOES reproduce here, the bug is in the engine (or in the C-side
-- info-table construction). If it DOESN'T, the bug is in whichever
-- full brain (e.g. GoalHunter) was being used before.

print("[stub_turnleft] loaded")

local Brain = {}

function Brain.open(info)
  print("[stub_turnleft] open called")
end

function Brain.think(info)
  return {
    holdkeys    = KEY_TURNLEFT,
    tapkeys     = 0,
    build       = -1,
    wantallies  = (info and info.allies) or 0,
    messagedest = 0,
    sendmessage = "",
  }
end

function Brain.close(info)
  print("[stub_turnleft] close called")
end

return Brain
