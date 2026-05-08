-- =========================================================================
-- NewAutopilot/changes.lua — shared change notification between util and pathfinder
-- =========================================================================
-- Avoids circular dependency: util.lua records terrain changes here,
-- pathfinder.lua reads and clears them each tick.

return { terrain = {} }
