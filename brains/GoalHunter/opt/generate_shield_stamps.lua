-- generate_shield_stamps.lua
-- Precomputes shot-path tile offsets for shield.scan and writes
-- shield_stamp_cache.lua to the brain directory.
--
-- Usage:
--   BrainTest.exe --opt --run-script brains/GoalHunter/generate_shield_stamps.lua
--
-- Output is written to BRAIN_DIR/shield_stamp_cache.lua.
-- Re-run whenever ATTACK_PILL_STANDOFF, AIM_INSET, or STEP_DEG changes.

local C      = require("constants")
local shield = require("attack_shield")
local outpath = (BRAIN_DIR or ".") .. "/shield_stamp_cache.bin"

print("[gen] Computing shield stamp cache …")
print("[gen] standoff=" .. tostring(C.ATTACK_PILL_STANDOFF) ..
      "  aim_inset=" .. tostring(shield.AIM_INSET) ..
      "  step_deg=" .. tostring(shield.STEP_DEG))
print("[gen] Output: " .. outpath)

local stamps = shield.compute_shield_stamps()
local ok = shield.save_shield_stamps_bin(stamps, outpath)
if ok then
  print("[gen] Done. shield_stamp_cache.bin is loaded by C at Brain.open via gh_shield.load().")
else
  print("[gen] FAILED to write output.")
  os.exit(1)
end
