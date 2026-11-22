-- lgm_kill_lag_near_d1 with the old kill-LGM aim (the knobs
-- lgm_kill_lag_old turns off): the in-server bot's baseline.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua
-- GATE: expect=fail the old aim, for the in-server baseline

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
LKL_TOKENS = "cfg=LGM_AIM_KEYS_IN_FLIGHT=false;cfg=LGM_AIM_DELAY_EST=false;"
          .. "cfg=LGM_AIM_LAUNCH_POSE=false;cfg=LGM_AIM_TURN_RULE=false;"
          .. "cfg=LGM_AIM_SEARCH_MOVE=false;cfg=LGM_AIM_SIGHT_TAP=false;"
          .. "cfg=LGM_TELEPORT_REPEAT_VIEW=false;cfg=LGM_AIM_LEAD_CONVERGE=false;"
          .. "cfg=LGM_SHELL_MODEL_RULES=false;cfg=LGM_FIRE_LOS_STRICT=false;"
          .. "cfg=LGM_LEAD_STOP_RETURN=false"
