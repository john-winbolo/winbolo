-- lgm_kill_lag_near for a bot inside the server: no added key delay
-- (delay 1). The close man, 2-3.6 squares from our tank.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua

LKL_ROW = 2
LKL_WALK = 2
LKL_OX_MAX = 1
LKL_KEY_DELAY = 0
-- Measured on seeds 42, 7, 123, 999, 2024 (2026-11-22, shells leaving
-- 1.3-3.2 squares from the man): the new aim 4 first-shell kills and 8 kills
-- of 8 jobs on every seed, 31 of 68 shells within 128 wu of the man; the old
-- aim 2-3 first-shell kills, 7-8 kills, 33 of 75. The in-server bot is no
-- worse with the new aim.
LKL_NEED_FIRST = 4
LKL_NEED_KILLS = 7
