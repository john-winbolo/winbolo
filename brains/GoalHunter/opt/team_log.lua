-- =========================================================================
-- GoalHunter/team_log.lua -- PT2_TEAM_LOG (log only, default off): the
-- "team take summary" study (Nov 28). It writes what an offline script
-- needs to measure how often teammates evaluate the same goal pill on the
-- same view of the map, and how much a shared summary could save.
-- =========================================================================
-- Nothing here changes a decision. Terrain reads use util.ttype_peek
-- (no priming, no counters). The lines go to stderr:
--   TSIG  appended to each PT2EVAL line: the view hashes of the goal pill's
--         area at that evaluate.
--   TSG   one line per pool-6 candidate cost: the hashes and the cost parts.
-- The area is a disk of R_SIG squares round the goal pill: the farthest
-- standoff point (7.4 squares) + pill range (8) + margin, rounded up.
-- hT: terrain types in the disk. hP: every pill in the disk the bot knows
-- (square, health, owner, carried, position unknown). hB: the bases in the
-- disk (square, owner). hA: the anger of each pill in the disk, to 0.001.
-- hQ: the same anger to 0.05 (a coarse form a summary could carry).
-- =========================================================================

local C = require("constants")

local M = {}

local floor = math.floor
local R_SIG = 18

local function h32(h, v)
  return (h * 31 + v) % 4294967296
end

local disk = nil
local function disk_offsets()
  if disk then return disk end
  disk = {}
  for dy = -R_SIG, R_SIG do
    for dx = -R_SIG, R_SIG do
      if dx * dx + dy * dy <= R_SIG * R_SIG then disk[#disk + 1] = { dx, dy } end
    end
  end
  return disk
end

local function owner_code(o)
  return o == "hostile" and 1 or o == "neutral" and 2 or o == "friendly" and 3 or 4
end

-- The view hashes of the area round pill square (ax, ay). pills: the pill
-- list the bot knows (inp.pills or world.pills); anger: function(p) or nil;
-- base_at: world.base_at or nil.
function M.sig(ax, ay, pills, anger, base_at)
  local U = require("util")
  local peek = U.ttype_peek
  local ht = 17
  for _, o in ipairs(disk_offsets()) do
    local x, y = ax + o[1], ay + o[2]
    local t = -1
    if x >= 0 and x <= 255 and y >= 0 and y <= 255 then t = peek(x, y) end
    ht = h32(ht, t + 2)
  end
  local R2 = R_SIG * R_SIG
  -- the pills in the disk, sorted by square (the list order is no input)
  local list = {}
  for _, p in pairs(pills or {}) do
    if type(p) == "table" and p.mx and p.my then
      local dx, dy = p.mx - ax, p.my - ay
      if dx * dx + dy * dy <= R2 then list[#list + 1] = p end
    end
  end
  table.sort(list, function(a, b)
    local ka, kb = a.my * 256 + a.mx, b.my * 256 + b.mx
    if ka ~= kb then return ka < kb end
    return (a.health or 0) < (b.health or 0)
  end)
  local hp, ha, hq = 17, 17, 17
  local na = 0
  for _, p in ipairs(list) do
    local sq = p.my * 256 + p.mx
    hp = h32(hp, sq * 1009 + (p.health or 0) * 101 + owner_code(p.owner) * 7
      + (p.in_tank and 1 or 0) * 2 + (p.pos_unknown and 1 or 0))
    if anger then
      local a = anger(p) or 0
      ha = h32(ha, sq * 1009 + floor(a * 1000 + 0.5))
      hq = h32(hq, sq * 1009 + floor(a * 20 + 0.5))
      if a > 0 then na = na + 1 end
    end
  end
  local hb, nb = 17, 0
  if type(base_at) == "table" then
    local keys = {}
    for k in pairs(base_at) do
      if type(k) == "number" then
        local x, y = k % 256, floor(k / 256)
        local dx, dy = x - ax, y - ay
        if dx * dx + dy * dy <= R2 then keys[#keys + 1] = k end
      end
    end
    table.sort(keys)
    for _, k in ipairs(keys) do
      local b = base_at[k]
      local ow = type(b) == "table" and owner_code(b.owner) or 5
      hb = h32(hb, k * 13 + ow)
      nb = nb + 1
    end
  end
  return string.format("hT=%08x hP=%08x hB=%08x hA=%08x hQ=%08x np=%d nb=%d nang=%d",
    ht, hp, hb, ha, hq, #list, nb, na)
end

-- The TSIG text for one v2 evaluate (pilltake_v2 PT2EVAL line).
function M.eval_tag(inp)
  if not C.PT2_TEAM_LOG then return "" end
  local A = inp and inp.A
  if not (A and A.mx and A.my) then return "" end
  local env = inp.env
  local s = M.sig(A.mx, A.my, inp.pills, type(inp.anger_of) == "function" and inp.anger_of or nil,
    env and env.base_at)
  local tx = inp.tank_wx and floor(inp.tank_wx / 256) or -1
  local ty = inp.tank_wy and floor(inp.tank_wy / 256) or -1
  -- the other tanks the bot sees (env.ts_tanks: x, y pairs in world units)
  local tt = {}
  local ts = type(env) == "table" and env.ts_tanks
  if type(ts) == "table" then
    for i = 1, #ts - 1, 2 do tt[#tt + 1] = string.format("%d,%d", floor(ts[i] / 256), floor(ts[i + 1] / 256)) end
  end
  return string.format(" TSIG %s tank=%d,%d arm=%s carried=%s tt=%s", s, tx, ty,
    tostring(inp.armour), tostring(inp.carried_pills), #tt > 0 and table.concat(tt, ";") or "-")
end

-- PT2_TEAM_DUMP_EVERY: the C record dump (inp.team_dump) on every take
-- evaluate, and on every n-th double-take run of a bot.
function M.mark_dump(inp, state, src)
  if not C.PT2_TEAM_LOG then return end
  if src == "dbl" then
    local n = (state._team_dbl_n or 0) + 1
    state._team_dbl_n = n
    local every = C.PT2_TEAM_DUMP_EVERY or 8
    if every > 0 and n % every == 0 then inp.team_dump = true end
  else
    inp.team_dump = true
  end
end

-- One TSG line: a pool-6 candidate cost.
function M.pool6(state, info, world, now, obj, id, f)
  if not C.PT2_TEAM_LOG then return end
  local PA = package.loaded["pt2_attack"]
  local anger = PA and PA.anger_now and function(p) return PA.anger_now(p, now) end or nil
  -- one hash per pill per tick
  local cache = state._team_sig
  if not cache or cache.t ~= now then cache = { t = now }; state._team_sig = cache end
  local key = obj.my * 256 + obj.mx
  local s = cache[key]
  if not s then
    s = M.sig(obj.mx, obj.my, world.pills, anger, world.base_at)
    cache[key] = s
  end
  io.stderr:write(string.format(
    "TSG p=%s t=%d pill#%s A=(%d,%d) %s c=%.2f spot=%.2f diff=%.2f mult=%.4f post=%.4f travel=%.2f dbl=%d fresh=%d diff_us=%d tank=%d,%d\n",
    tostring(info.player_number), now, tostring(id), obj.mx, obj.my, s,
    f.c or -1, f.spot or -1, f.diff or -1, f.mult or -1, f.post or -1, f.travel or -1,
    f.dbl and 1 or 0, f.fresh and 1 or 0, floor(f.diff_us or 0),
    floor((info.tankx or 0) / 256), floor((info.tanky or 0) / 256)))
end

return M
