-- lgm_kill_lag with LKL_ARRIVE: our tank has no shells while the man walks out,
-- and is put in place with full shells on the frame he stops. Every shot is in
-- the stop or the walk back (C.LGM_LEAD_STOP_RETURN). Play test 20261121_231141:
-- bot 1, ticks 8386-8426, missed by 400-700 wu in these phases.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua


LKL_ARRIVE = true
-- Our tank is put in place with its nose off on the frame he stops, so its
-- first shell is often fired before the aim settles: the bar here is kills.
-- Measured on seeds 42, 7, 123, 999, 2024 (2026-11-22): the new aim 7-8
-- kills of 8 jobs, first-shell kills 1-3; the old aim 4-5 kills,
-- first-shell kills 0-1.
LKL_NEED_FIRST = 1
LKL_NEED_KILLS = 7
