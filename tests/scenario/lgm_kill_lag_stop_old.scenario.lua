-- lgm_kill_lag_stop with the old kill-LGM aim (the knobs lgm_kill_lag_old
-- turns off). Expected to fail.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua
-- GATE: expect=fail the old lead misses a builder in the stop and the walk back

LKL_ARRIVE = true
-- Our tank is put in place with its nose off on the frame he stops, so its
-- first shell is often fired before the aim settles: the bar here is kills.
-- Measured on seeds 42, 7, 123, 999, 2024 (2026-11-22): the new aim 7-8
-- kills of 8 jobs, first-shell kills 1-3; the old aim 4-5 kills,
-- first-shell kills 0-1.
LKL_NEED_FIRST = 1
LKL_NEED_KILLS = 7
LKL_TOKENS = "cfg=LGM_AIM_KEYS_IN_FLIGHT=false;cfg=LGM_AIM_DELAY_EST=false;"
          .. "cfg=LGM_AIM_LAUNCH_POSE=false;cfg=LGM_AIM_TURN_RULE=false;"
          .. "cfg=LGM_AIM_SEARCH_MOVE=false;cfg=LGM_AIM_SIGHT_TAP=false;"
          .. "cfg=LGM_TELEPORT_REPEAT_VIEW=false;cfg=LGM_AIM_LEAD_CONVERGE=false;"
          .. "cfg=LGM_SHELL_MODEL_RULES=false;cfg=LGM_FIRE_LOS_STRICT=false;"
          .. "cfg=LGM_LEAD_STOP_RETURN=false"
