-- lgm_kill_lag_d1 with the old kill-LGM aim (the knobs lgm_kill_lag_old turns
-- off). Expected to fail.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua
-- GATE: expect=fail the old aim misses a builder with keys 1 tick late

LKL_KEY_DELAY = 0
LKL_TOKENS = "cfg=LGM_AIM_KEYS_IN_FLIGHT=false;cfg=LGM_AIM_DELAY_EST=false;"
          .. "cfg=LGM_AIM_LAUNCH_POSE=false;cfg=LGM_AIM_TURN_RULE=false;"
          .. "cfg=LGM_AIM_SEARCH_MOVE=false;cfg=LGM_AIM_SIGHT_TAP=false;"
          .. "cfg=LGM_TELEPORT_REPEAT_VIEW=false;cfg=LGM_AIM_LEAD_CONVERGE=false;"
          .. "cfg=LGM_SHELL_MODEL_RULES=false;cfg=LGM_FIRE_LOS_STRICT=false;"
          .. "cfg=LGM_LEAD_STOP_RETURN=false;cfg=LGM_AIM_DELAY_CONFIRM=false;cfg=LGM_AIM_DELAY_BEST_RATE=false;"
          .. "cfg=DRIVEBY_REQUIRE_REACH=false;cfg=DRIVEBY_YIELD_LGM_KILL=false"
