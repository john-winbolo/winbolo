-- lgm_kill_lag with LKL_PILL and LKL_ARRIVE: a live pillbox is put under the
-- man on the frame he stops (can_target: it never shoots our tank), as in
-- play test 20261121_231141_1, bot 2, tick 4888, where he stood on the pill
-- he built and a shell aimed one tile past him hit it.
--
-- This arena checks that the stricter line of sight (C.LGM_FIRE_LOS_STRICT)
-- does not stop the bot from killing a man who stands on a pillbox, and that
-- no shell hits the pillbox. It does not make the old miss happen: in these
-- jobs our first shell leaves after he has walked off it, so the old aim hits
-- no pillbox either (2026-11-22). The old miss itself is test 13 in
-- brains/GoalHunter/test_lgm_aim.lua.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua


LKL_PILL = true
LKL_ARRIVE = true
