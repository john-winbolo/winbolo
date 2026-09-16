-- =========================================================================
-- GoalHunter/commands.lua — command parsing, execution, settings API
-- =========================================================================

local U   = require("util")
local C   = require("constants")
local log = require("logger")

local TAG = "[" .. C.BRAIN_NAME .. "]"

local M = {}

-- Parse a message and return a command table, or nil if not a command
-- EVERY sender must lead with "!", humans included. These commands are hard
-- locks: pick_goal hands back the command goal before goal selection runs, so
-- there is no flee, no refuel and no survival exception behind them. A chat
-- word like "stop" said in passing must not freeze a whole team of bots, so
-- the "!" is what separates an order from ordinary talk.
--
-- AND ONLY AN ALLY MAY SAY ONE.  `from_ally` is the sender's team as the
-- caller worked it out: false refuses the line outright.  init.lua used to
-- call this from OUTSIDE the ally test that guards every other reader on that
-- path -- orders.lua, comms.lua and the state slate all check it -- which
-- left the hardest thing anybody can say to a bot as the one line the other
-- team could say too (Andrew's peer review, Sep 16).  The check lives here,
-- beside the parse, so there is one place to read it and one to test it.
--
-- nil means the caller has already checked (orders.lua asks this parser
-- whether a line is a command at all, and it is only ever reached from
-- on_chat, which turned every non-ally away before it).
function M.parse(text, from_ally)
  if from_ally == false then return nil end
  if not text then return nil end
  local lower = text:lower():match("^%s*(.-)%s*$")  -- trim + lowercase
  if lower:sub(1, 1) ~= "!" then return nil end
  lower = lower:sub(2):match("^%s*(.-)%s*$")

  local base_id = lower:match("^base:(%d+)$")
  if base_id then
    return { cmd = "base", id = tonumber(base_id) }
  end

  local pill_id = lower:match("^pill:(%d+)$")
  if pill_id then
    return { cmd = "pill", id = tonumber(pill_id) }
  end

  local attack_id = lower:match("^attack:(%d+)$")
  if attack_id then
    return { cmd = "attack", id = tonumber(attack_id) }
  end

  local bpc_id = lower:match("^bpc:(%d+)$")
  if bpc_id then
    return { cmd = "bpc", id = tonumber(bpc_id) }
  end

  local cp_id = lower:match("^cp:(%d+)$")
  if cp_id then
    return { cmd = "cp", id = tonumber(cp_id) }
  end

  local pp_id = lower:match("^pp:(%d+)$")
  if pp_id then
    return { cmd = "pp", id = tonumber(pp_id) }
  end

  if lower == "cb:all" then
    return { cmd = "cb_all" }
  end
  local cb_id = lower:match("^cb:(%d+)$")
  if cb_id then
    return { cmd = "cb", id = tonumber(cb_id) }
  end

  local watch_id = lower:match("^watch:(%d+)$")
  if watch_id then
    return { cmd = "watch", id = tonumber(watch_id) }
  end

  if lower == "stop"   then return { cmd = "stop" }   end
  if lower == "start"  then return { cmd = "start" }  end
  if lower == "status" then return { cmd = "status" } end

  return nil
end

-- Execute a parsed command. Sets state.command_goal and returns a
-- newswire response string (or nil for no response).
function M.execute(cmd, state, world)
  if cmd.cmd == "stop" then
    state.command_goal      = nil
    state.capture_objective = nil
    state.base_capture_objective = nil
    state.auto_explore = false
    state.paused = true
    state.goal = { kind = "none", mx = 0, my = 0, wx = 0, wy = 0 }
    state.pf.status = "idle"
    print(TAG .. " CMD: stop -- paused, exploration disabled")
    return C.BRAIN_NAME .. ": paused"

  elseif cmd.cmd == "start" then
    state.paused = false
    state.auto_explore = true
    print(TAG .. " CMD: start -- unpaused, exploration enabled")
    return C.BRAIN_NAME .. ": exploring"

  elseif cmd.cmd == "status" then
    local g = state.goal
    if state.command_goal then
      local cg = state.command_goal
      local dist_str = ""
      if state.last_mx >= 0 then
        dist_str = string.format(" dist=%d",
          U.mdist(state.last_mx, state.last_my, cg.mx, cg.my))
      end
      local msg = string.format(C.BRAIN_NAME .. ": CMD %s #%d at (%d,%d)%s pf=%s",
        cg.kind, cg.id, cg.mx, cg.my, dist_str, state.pf.status)
      print(TAG .. " CMD: status -- " .. msg)
      return msg
    else
      local msg = string.format(C.BRAIN_NAME .. ": goal=%s dest=(%d,%d) pf=%s",
        g.kind, g.mx, g.my, state.pf.status)
      print(TAG .. " CMD: status -- " .. msg)
      return msg
    end

  elseif cmd.cmd == "base" then
    local b = world.bases[cmd.id]
    if not b then
      local msg = string.format(C.BRAIN_NAME .. ": base #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    state.paused = false
    state.command_goal = {
      kind = "goto_base", id = cmd.id,
      mx = b.mx, my = b.my, wx = U.m2w(b.mx), wy = U.m2w(b.my),
    }
    state.goal = {
      kind = "goto_base", mx = b.mx, my = b.my,
      wx = U.m2w(b.mx), wy = U.m2w(b.my),
    }
    state.pf.status = "idle"
    state.pf.debug_trace = true
    state.pf_fail_logged = false
    state.stuck_for = 0
    local msg = string.format(C.BRAIN_NAME .. ": navigating to base #%d at (%d,%d) [%s]",
      cmd.id, b.mx, b.my, b.owner)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "pill" then
    local p = world.pills[cmd.id]
    if not p then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    state.paused = false
    state.command_goal = {
      kind = "goto_pill", id = cmd.id,
      mx = p.mx, my = p.my, wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.goal = {
      kind = "goto_pill", mx = p.mx, my = p.my,
      wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.pf.status = "idle"
    state.pf.debug_trace = true
    state.pf_fail_logged = false
    state.stuck_for = 0
    local msg = string.format(C.BRAIN_NAME .. ": navigating to pill #%d at (%d,%d) [%s hp=%d]",
      cmd.id, p.mx, p.my, p.owner, p.health)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "attack" then
    local p = world.pills[cmd.id]
    if not p then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    if p.health == 0 then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d is already dead", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    state.paused = false
    state.command_goal = {
      kind = "attack_pill", id = cmd.id,
      mx = p.mx, my = p.my, wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.goal = {
      kind = "attack_pill", mx = p.mx, my = p.my,
      wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.pf.status = "idle"
    state.pf.debug_trace = true
    state.pf_fail_logged = false
    state.stuck_for = 0
    local msg = string.format(C.BRAIN_NAME .. ": attacking pill #%d at (%d,%d) [%s hp=%d]",
      cmd.id, p.mx, p.my, p.owner, p.health)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "bpc" then
    local p = world.pills[cmd.id]
    if not p then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    if p.health == 0 then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d is already dead", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    -- Start a dedicated BPC log so it doesn't get overwritten by the menu game
    log.close()
    if log.open("bpc_log.jsonl") then
      log.dump_map()
    end

    state.paused = false
    state.command_goal = {
      kind = "attack_pill", id = cmd.id,
      mx = p.mx, my = p.my, wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.goal = {
      kind = "attack_pill", mx = p.mx, my = p.my,
      wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.pf.status = "idle"
    state.pf.debug_trace = true
    state.pf_fail_logged = false
    state.stuck_for = 0
    local msg = string.format(C.BRAIN_NAME .. ": BPC pill #%d at (%d,%d) [%s hp=%d] — stand-and-shoot attack",
      cmd.id, p.mx, p.my, p.owner, p.health)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "cp" then
    local p = world.pills[cmd.id]
    if not p then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    if p.owner == "friendly" then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d is already yours", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    -- Start a dedicated CP log so it doesn't get overwritten by the menu game
    log.close()
    if log.open("cp_log.jsonl") then
      log.dump_map()
    end

    state.paused           = false
    state.command_goal     = nil  -- cp does not use command_goal (would bypass flee)
    state.capture_objective = {
      id = cmd.id,
      mx = p.mx, my = p.my, wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.pf.status        = "idle"
    state.pf.debug_trace   = true
    state.pf_fail_logged   = false
    state.stuck_for        = 0
    local msg = string.format(C.BRAIN_NAME .. ": capturing pill #%d at (%d,%d) [%s hp=%d] (will flee/refuel as needed)",
      cmd.id, p.mx, p.my, p.owner, p.health)
    print(TAG .. " CMD: " .. msg)
    return msg
  elseif cmd.cmd == "pp" then
    local p = world.pills[cmd.id]
    if not p then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    if p.health == 0 then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d is already dead", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    if p.owner == "friendly" then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d is already yours", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    state.paused = false
    state.command_goal = {
      kind = "pill_place", id = cmd.id,
      mx = p.mx, my = p.my, wx = U.m2w(p.mx), wy = U.m2w(p.my),
    }
    state.goal = {
      kind = "pill_place", mx = p.mx, my = p.my,
      wx = U.m2w(p.mx), wy = U.m2w(p.my),
      substate = "select_pill",
    }
    state.pf.status = "idle"
    state.pf.debug_trace = true
    state.pf_fail_logged = false
    state.stuck_for = 0
    local msg = string.format(C.BRAIN_NAME .. ": pill-place attack pill #%d at (%d,%d) [%s hp=%d]",
      cmd.id, p.mx, p.my, p.owner, p.health)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "cb" then
    local b = world.bases[cmd.id]
    if not b then
      local msg = string.format(C.BRAIN_NAME .. ": base #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    if b.owner == "friendly" then
      local msg = string.format(C.BRAIN_NAME .. ": base #%d is already yours", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    state.paused              = false
    state.command_goal        = nil
    state.capture_objective   = nil
    state.base_capture_objective = {
      id = cmd.id, mx = b.mx, my = b.my,
      wx = U.m2w(b.mx), wy = U.m2w(b.my),
      all = false,
    }
    state.pf.status           = "idle"
    state.pf.debug_trace      = true
    state.pf_fail_logged      = false
    state.stuck_for           = 0
    local msg = string.format(C.BRAIN_NAME .. ": capturing base #%d at (%d,%d) [%s]",
      cmd.id, b.mx, b.my, b.owner)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "cb_all" then
    -- Count how many non-friendly bases are known
    local count = 0
    for _, b in pairs(world.bases) do
      if b.owner ~= "friendly" then count = count + 1 end
    end
    if count == 0 then
      local msg = C.BRAIN_NAME .. ": no non-friendly bases known"
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    state.paused              = false
    state.command_goal        = nil
    state.capture_objective   = nil
    state.base_capture_objective = {
      id = nil, mx = 0, my = 0, wx = 0, wy = 0,
      all = true,
    }
    state.pf.status           = "idle"
    state.pf.debug_trace      = true
    state.pf_fail_logged      = false
    state.stuck_for           = 0
    local msg = string.format(C.BRAIN_NAME .. ": capturing all bases (%d remaining)", count)
    print(TAG .. " CMD: " .. msg)
    return msg

  elseif cmd.cmd == "watch" then
    local p = world.pills[cmd.id]
    if not p then
      local msg = string.format(C.BRAIN_NAME .. ": pill #%d not known yet", cmd.id)
      print(TAG .. " CMD: " .. msg)
      return msg
    end
    -- Pick an observation point ~10 tiles from the pill (within visual
    -- range but outside pill fire range of 8).  Use the direction from
    -- the tank's current position.
    local tmx = state.last_mx or 128
    local tmy = state.last_my or 128
    local dx  = tmx - p.mx
    local dy  = tmy - p.my
    local len = math.sqrt(dx * dx + dy * dy)
    if len < 1 then dx, dy, len = 0, -1, 1 end  -- default: north
    local obs_mx = U.mclamp(math.floor(p.mx + dx / len * 10 + 0.5))
    local obs_my = U.mclamp(math.floor(p.my + dy / len * 10 + 0.5))
    state.paused = false
    state.command_goal = {
      kind = "goto_pill", id = cmd.id,
      mx = obs_mx, my = obs_my,
      wx = U.m2w(obs_mx), wy = U.m2w(obs_my),
    }
    state.goal = {
      kind = "goto_pill", mx = obs_mx, my = obs_my,
      wx = U.m2w(obs_mx), wy = U.m2w(obs_my),
    }
    state.pf.status = "idle"
    state.pf.debug_trace = true
    state.pf_fail_logged = false
    state.stuck_for = 0
    local msg = string.format(
      C.BRAIN_NAME .. ": watching pill #%d at (%d,%d) — moving to observation point (%d,%d)",
      cmd.id, p.mx, p.my, obs_mx, obs_my)
    print(TAG .. " CMD: " .. msg)
    return msg
  end

  return nil
end

return M
