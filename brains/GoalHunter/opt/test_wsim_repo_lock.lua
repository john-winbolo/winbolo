-- =========================================================================
-- test_wsim_repo_lock.lua — standalone unit tests (2026-11-24) for
-- goals.wsim_locked_reposition: the wsim pass must not add its damage cost
-- to the reposition we are committed to (locked or vote-approved), because
-- eval_reposition_pill holds that row at PILL_REPOSITION_LOCK_COST "so nothing
-- steals it mid-swap".
--
-- Real numbers: 20261124 a1_noc_diag_sw bot0, brain t=400. Goal capture_pill
-- #2 (123,116) sub=reposition_shoot, our pill at 10/15 hp. Pool rows:
--   [1] attack_pill@125,117 total=122.0 (base 17 + switch 30 + commit 75)
--   [2] capture_pill@123,116 total=125.0 base=30.0 inf=0.50 wsim=110.0
-- The swap lost by 3 and the bot left its own pill half shot.
-- No engine.
--
-- Run from this directory with the LuaJIT built beside the game:
--   ../../build-xc/_deps/luajit_src-src/src/luajit.exe test_wsim_repo_lock.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")
local G = require("goals")

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

local cur = { kind = "capture_pill", mx = 123, my = 116, target_id = 2,
              reposition = true, substate = "reposition_shoot" }
local row = { kind = "capture_pill", mx = 123, my = 116, target_id = 2,
              reposition = true, awaiting_vote = false }

-- ── the committed swap is recognised ─────────────────────────────────────
check("locked row of the current reposition", G.wsim_locked_reposition(cur, row) == true)

-- ── the numbers from t=400 ───────────────────────────────────────────────
do
  local lock = C.PILL_REPOSITION_LOCK_COST
  local inf  = 0.50
  local attack_total = 122.0
  check("lock cost is 30 (the row's base=30.0)", lock == 30, lock)
  local before = lock * inf + 110.0
  check("old: 15 + wsim 110 = 125 loses to attack 122", before == 125 and before > attack_total, before)
  local wsim_add = G.wsim_locked_reposition(cur, row) and 0 or 110.0
  local after = lock * inf + wsim_add
  check("new: wsim dropped, 15 keeps the swap over attack 122", after == 15 and after < attack_total, after)
end

-- ── rows that are NOT the committed swap keep their wsim cost ────────────
do
  local bid = { kind = "capture_pill", mx = 123, my = 116, target_id = 2,
                reposition = true, awaiting_vote = true }
  check("a BID (awaiting vote) is not locked", G.wsim_locked_reposition(cur, bid) == false)
  local other = { kind = "capture_pill", mx = 121, my = 114, target_id = 5,
                  reposition = true, awaiting_vote = false }
  check("an approved row on another pill is not ours", G.wsim_locked_reposition(cur, other) == false)
  local cap = { kind = "capture_pill", mx = 123, my = 116, target_id = 2 }
  check("a plain capture of the same pill is not locked", G.wsim_locked_reposition(cur, cap) == false)
  local plain_cur = { kind = "capture_pill", mx = 123, my = 116, target_id = 2 }
  check("current goal not a reposition", G.wsim_locked_reposition(plain_cur, row) == false)
  local atk_cur = { kind = "attack_pill", mx = 125, my = 117, target_id = 0 }
  check("current goal attack_pill", G.wsim_locked_reposition(atk_cur, row) == false)
  check("no current goal", G.wsim_locked_reposition(nil, row) == false)
  local no_id = { kind = "capture_pill", reposition = true }
  check("current reposition with no target id", G.wsim_locked_reposition(no_id, row) == false)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
