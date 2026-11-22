-- lgm_kill_lag with the old kill-LGM aim: every LGM_AIM_* knob (with
-- LGM_AIM_LEAD_CONVERGE) and LGM_TELEPORT_REPEAT_VIEW off, their KEEL /
-- MAST values. Same map, same arena, same 3-tick key delay. Expected to fail: it shows the bug that
-- lgm_kill_lag checks is fixed.
--
-- GATE: ticks=40000 bots=0 ai=yesfull gametype=open include=tests/scenario/lgm_kill_lag.scenario.lua
-- GATE: expect=fail the old aim misses a builder with keys 3 ticks late

LKL_TOKENS = "cfg=LGM_AIM_KEYS_IN_FLIGHT=false;cfg=LGM_AIM_DELAY_EST=false;"
          .. "cfg=LGM_AIM_LAUNCH_POSE=false;cfg=LGM_AIM_TURN_RULE=false;"
          .. "cfg=LGM_AIM_SEARCH_MOVE=false;cfg=LGM_AIM_SIGHT_TAP=false;"
          .. "cfg=LGM_TELEPORT_REPEAT_VIEW=false;cfg=LGM_AIM_LEAD_CONVERGE=false"
