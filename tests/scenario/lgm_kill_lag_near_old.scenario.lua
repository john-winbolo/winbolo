-- lgm_kill_lag_near with the old kill-LGM aim (the knobs lgm_kill_lag_old
-- turns off). Expected to fail.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua
-- GATE: expect=fail the old aim misses a close builder with keys 3 ticks late

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
LKL_TOKENS = "cfg=LGM_AIM_KEYS_IN_FLIGHT=false;cfg=LGM_AIM_DELAY_EST=false;"
          .. "cfg=LGM_AIM_LAUNCH_POSE=false;cfg=LGM_AIM_TURN_RULE=false;"
          .. "cfg=LGM_AIM_SEARCH_MOVE=false;cfg=LGM_AIM_SIGHT_TAP=false;"
          .. "cfg=LGM_TELEPORT_REPEAT_VIEW=false;cfg=LGM_AIM_LEAD_CONVERGE=false;"
          .. "cfg=LGM_SHELL_MODEL_RULES=false;cfg=LGM_FIRE_LOS_STRICT=false;"
          .. "cfg=LGM_LEAD_STOP_RETURN=false;cfg=LGM_AIM_DELAY_CONFIRM=false;cfg=LGM_AIM_DELAY_BEST_RATE=false;"
          .. "cfg=DRIVEBY_REQUIRE_REACH=false;cfg=DRIVEBY_YIELD_LGM_KILL=false"
