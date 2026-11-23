-- shells_probe -- where does a shell that hits nothing burst, and does
-- game.shells() show that place?
--
-- A tank that never moves faces due north on open grass and fires two
-- shells at each of the sights 14, 8, 4, 2, 11 and 7 (tests/brains/
-- fire_north_sights.lua). Nothing is in their way. The arena tracks each
-- shell with game.shells() from the frame it first shows until the frame it
-- is gone, and measures the last place it was seen from the gun.
--
-- The engine (shells.c shellsUpdate): a shell moves one step a tick while
-- its length is above SHELL_DEATH. On the tick its length is at SHELL_DEATH
-- it does not move: it bursts where it is and is marked dead, and the
-- snapshot leaves dead shells out. So for a shell that runs out, the last
-- place seen IS the burst. (A shell that hits something moves one more step
-- and bursts there, or at the middle of the square it hit, in the same
-- tick: its last place seen is short of its burst. This probe has no such
-- shell.)
--
-- THE VERDICT. PASS when every shell's last place seen is within 2 wu of
-- speed x (start_add + L) north of the gun, L = max(0, 1 + floor(life x
-- sight / 2) - start_add): 128 x sight + 32 wu for the classic rules. The
-- log also gives each shell's distance from 128 x sight, the old kill-LGM
-- model, which is 32 wu short.
--
-- GATE: ticks=6000 bots=0 ai=yesfull gametype=open

local BRAIN = "../tests/brains/fire_north_sights.lua"
local US = 0
local NEED = 12

local frame, A, spawned = 0, nil, false
local tracks, done = {}, {}
local prev_pose = nil

function on_choose_start(g, p)
  if p == US then return 1 end
  return nil
end

function can_die(g, kind, n, killer, cause, pill)
  if kind == "tank" then return false end
  return nil
end

local function rule_or(g, name, dflt)
  local ok, v = pcall(g.rule, name)
  if ok and type(v) == "number" then return v end
  return dflt
end

local function engine_travel(g, sl)
  local speed = rule_or(g, "shell_speed", 32)
  local life = rule_or(g, "shell_life", 8)
  local add = rule_or(g, "shell_start_add", 5)
  local L = math.max(0, 1 + math.floor(life * sl / 2) - add)
  return speed * (add + L), L
end

function on_tick(g, tick)
  frame = frame + 1
  if A == nil then
    local s1 = g.start(1)
    if not s1 then return end
    A = { cx = s1.x, cy = s1.y - 6 }
  end
  if not spawned and frame >= 2 then
    spawned = true
    local u, err = g.spawn_bot{ slot = US, name = "Probe", brain = BRAIN, team = 1, start = 1 }
    if u == nil then g.log("SPR spawn_bot failed: " .. tostring(err)) end
  end
  local t = g.tank(US)
  if not t then return end
  if frame == 40 then
    g.fill_rect(A.cx - 4, A.cy - 12, A.cx + 4, A.cy + 2, g.TERRAIN.grass)
    g.teleport(US, A.cx, A.cy, 0)
    g.set_boat(US, false)
    g.set_speed(US, 0)
  end
  if frame < 50 then prev_pose = nil; return end

  -- Track each shell: a shell this frame is the track whose last place is
  -- within 48 wu of it (one 32 wu step); else it is a new shell, fired from
  -- the pose the tank had at the end of the last frame.
  local live = g.shells()
  local seen = {}
  for _, s in ipairs(live) do
    local best, bd = nil, 48
    for i, tr in ipairs(tracks) do
      if not seen[i] then
        local d = math.sqrt((s.wx - tr.x) ^ 2 + (s.wy - tr.y) ^ 2)
        if d <= bd then best, bd = i, d end
      end
    end
    if best then
      local tr = tracks[best]
      tr.x, tr.y, tr.n = s.wx, s.wy, tr.n + 1
      seen[best] = true
    else
      local p0 = prev_pose or { wx = t.wx, wy = t.wy, sight = t.sight }
      tracks[#tracks + 1] = { x = s.wx, y = s.wy, n = 1, gx = p0.wx, gy = p0.wy,
                              sight = p0.sight, first = frame }
      seen[#tracks] = true
    end
  end
  for i = #tracks, 1, -1 do
    if not seen[i] then
      local tr = table.remove(tracks, i)
      local dist = math.sqrt((tr.x - tr.gx) ^ 2 + (tr.y - tr.gy) ^ 2)
      local want, L = engine_travel(g, tr.sight)
      tr.err, tr.dist, tr.want, tr.L = dist - want, dist, want, L
      done[#done + 1] = tr
      -- (a log line over SCN_TEXT_MAX is refused, so keep it short)
      g.log(string.format("SPR shell %d sight %d: seen %.0f wu out, engine %d (L %d) %+.0f,"
        .. " 128xsight %+.0f, %d frames",
        #done, tr.sight, dist, want, L, dist - want, dist - 128 * tr.sight, tr.n))
    end
  end
  prev_pose = { wx = t.wx, wy = t.wy, sight = t.sight }

  if #done >= NEED then
    local worst = 0
    for _, tr in ipairs(done) do
      if math.abs(tr.err) > worst then worst = math.abs(tr.err) end
    end
    verdict(worst <= 2, string.format("%d shells; worst last-seen place %.0f wu from "
      .. "speed x (start_add + L)", #done, worst))
    return
  end
  if tick >= GATE_TICKS - 300 then
    verdict(false, string.format("out of time: %d shells", #done))
  end
end
