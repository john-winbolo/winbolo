-- =========================================================================
-- NewAutopilot/strategy.lua — Game phase detection + front line (Phases 2-3)
--
-- Classifies each tick into: opening / middle / endgame_winning / endgame_losing
-- with hysteresis to prevent flapping.  Also computes strength ratios and
-- front line summary for later use by goal weighting and pill placement.
-- =========================================================================

local C   = require("constants")
local cpf = require("cpathfinder")
local log = require("logger")

local M = {}

-- Compute center of gravity of friendly bases + pills
local function friendly_cog(world)
  local sx, sy, count = 0, 0, 0
  for _, b in pairs(world.bases) do
    if b.owner == "friendly" then
      sx = sx + b.mx; sy = sy + b.my; count = count + 1
    end
  end
  for _, p in pairs(world.pills) do
    if p.owner == "friendly" and p.health > 0 then
      sx = sx + p.mx; sy = sy + p.my; count = count + 1
    end
  end
  if count == 0 then return nil, nil end
  return math.floor(sx / count + 0.5), math.floor(sy / count + 0.5)
end

-- Called once per tick from init.lua, after perception update.
-- Sets state.phase, state.strength, state.base_strength, and front line data.
function M.update(state, world, info)
  local perc = state.perc

  -- Count totals (perception already has friendly/hostile/neutral counts)
  local friendly_pills = perc.friendly_pill_count or 0
  local total_pills = friendly_pills + (perc.hostile_pill_count or 0)
                      + (perc.dead_neutral_pill_count or 0)
  local friendly_ratio = total_pills > 0 and (friendly_pills / total_pills) or 0.5

  local friendly_bases = perc.friendly_base_count or 0
  local total_bases = friendly_bases + (perc.hostile_base_count or 0)
                      + (perc.neutral_base_count or 0)
  local base_ratio = total_bases > 0 and (friendly_bases / total_bases) or 0.5

  -- Determine candidate phase
  local new_phase
  if (perc.neutral_base_count or 0) > 0 or state.tick < C.OPENING_MIN_TICKS then
    new_phase = "opening"
  elseif friendly_ratio > C.ENDGAME_PILL_RATIO or base_ratio > C.ENDGAME_BASE_RATIO then
    new_phase = "endgame_winning"
  elseif friendly_ratio < (1.0 - C.ENDGAME_PILL_RATIO)
      or base_ratio < (1.0 - C.ENDGAME_BASE_RATIO) then
    new_phase = "endgame_losing"
  else
    new_phase = "middle"
  end

  -- Hysteresis: require N consecutive ticks before switching
  if new_phase ~= state.phase then
    if not state.phase_pending or state.phase_pending.phase ~= new_phase then
      state.phase_pending = { phase = new_phase, count = 1 }
    else
      state.phase_pending.count = state.phase_pending.count + 1
      if state.phase_pending.count >= C.PHASE_HYSTERESIS_TICKS then
        local old = state.phase
        state.phase = new_phase
        state.phase_pending = nil
        log.event("phase_change", (old or "none") .. " -> " .. new_phase)
      end
    end
  else
    state.phase_pending = nil
  end

  -- Strength ratios (used by goal weighting in Phase 4)
  state.strength = friendly_ratio       -- 0.0 = losing, 1.0 = dominating
  state.base_strength = base_ratio

  -- Front line computation (every N ticks, not every tick)
  if not state.front_line_tick or (state.tick - state.front_line_tick) >= C.FRONT_LINE_INTERVAL then
    state.front_line_tick = state.tick
    local pts = cpf.find_front_line()
    local n = #pts / 2

    if n > 0 then
      -- Compute centroid of front line
      local sx, sy = 0, 0
      for i = 1, #pts, 2 do
        sx = sx + pts[i]
        sy = sy + pts[i + 1]
      end
      state.front_center_mx = math.floor(sx / n + 0.5)
      state.front_center_my = math.floor(sy / n + 0.5)
      state.front_line_count = n

      -- Compute front direction (from friendly COG to front center)
      local fcx, fcy = friendly_cog(world)
      if fcx then
        local dx = state.front_center_mx - fcx
        local dy = state.front_center_my - fcy
        local len = math.sqrt(dx * dx + dy * dy)
        if len > 0.1 then
          state.front_dir_x = dx / len
          state.front_dir_y = dy / len
        end
      end
    else
      -- No front line (one side has zero influence, or no contested ground)
      state.front_center_mx = nil
      state.front_center_my = nil
      state.front_line_count = 0
      state.front_dir_x = nil
      state.front_dir_y = nil
    end
  end
end

-- Call from Brain.open() to set initial phase
function M.init(state)
  state.phase = "opening"
  state.phase_pending = nil
  state.strength = 0.5
  state.base_strength = 0.5
  state.front_center_mx = nil
  state.front_center_my = nil
  state.front_line_count = 0
  state.front_dir_x = nil
  state.front_dir_y = nil
  state.front_line_tick = nil
end

return M
