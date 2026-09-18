-- Rules Demo — nothing but a rules table, so the lobby's Rules popup is
-- the only thing that can say what it does.
--
-- Drop in the hosting Scenario Dir, pick it in the lobby, then press Rules
-- on the scenario line. bound = false so it plays over whatever map is
-- committed.

scenario = {
  name        = "Rules Demo",
  description = "Nothing but a rules table",
  api         = 1,
  bound       = false,

  rules = {
    tank_reload_ticks = 5,      -- classic 13   -> "2.6x faster"
    tank_full_shells  = 80,     -- classic 40   -> "twice as many"
    tank_full_mines   = 20,     -- classic 40   -> "half as many"
    tank_accel_rate   = 0.5,    -- classic 0.25 -> "2x faster"
    tank_full_armour  = 48,     -- classic 40   -> "+8", under the 1.5x band
  },
}
