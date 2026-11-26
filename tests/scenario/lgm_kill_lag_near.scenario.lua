-- lgm_kill_lag with the man close: his row 2 squares north of our tank, a
-- walk of 2 squares each side of the centre column, our tank's column
-- offset clamped to 1. He is 2-3.6 squares from our tank all the job, as in
-- the recorded case (session 20261121_195017_5, bot 2, t=42173-42194: the
-- builder 2.2-2.8 squares away, keys 3 ticks late, sight swept 2 <-> 12,
-- one shell short and one long). Same 3-tick key delay.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua

LKL_ROW = 2
LKL_WALK = 2
LKL_OX_MAX = 1
-- At this range the first shell often leaves before the aim settles, so the
-- bar is lower than lgm_kill_lag's. Measured on seeds 42, 7, 123, 999, 2024
-- (2026-11-22, shells leaving 1.4-3.3 squares from the man): the new aim
-- 2-3 first-shell kills and 7-8 kills of 8 jobs, 36 of 104 shells within
-- 128 wu of the man; the old aim 0-1 first-shell kills, 4-6 kills, 25 of 168.
LKL_NEED_FIRST = 2
LKL_NEED_KILLS = 7
