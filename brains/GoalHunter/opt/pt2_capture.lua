-- =========================================================================
-- pt2_capture.lua -- record pill take v2 cases for the C parity test
-- (2026-11-18, debug only: PT2_CAPTURE_FILE).
--
-- evaluate(V2, inp, path) runs V2.evaluate_lua on a copy of inp whose
-- functions are wrapped: each wrapper calls the real function and records
-- the answer by its arguments. The case (the inp numbers, the pills, the
-- recorded answers) is then appended to `path`.<brain> as one Lua line:
--   CASES[#CASES + 1] = { ... }
-- replay(case) builds an inp that answers from the record only (an
-- unrecorded question is an error), so the Lua and the C copy can be run
-- on it without the game (test_pt2_old_c_parity.lua).
--
-- The game result does not change: the wrapped Lua copy runs, and every
-- side effect (U.ttype's change list, env.omx / env.omy) still happens.
-- =========================================================================

local M = {}

local fmt = string.format

local function num(x)
  if x ~= x then return "(0/0)" end
  if x == math.huge then return "math.huge" end
  if x == -math.huge then return "-math.huge" end
  return fmt("%.17g", x)
end

-- score_fn (deg, mx, my, cx, cy) and banned (deg): the same answers kept on
-- number keys (mx, my are floor(cx), floor(cy), so deg, cx, cy name a call)
local function memo_score(f)
  local memo = {}
  return function(deg, mx, my, cx, cy)
    local a = memo[deg]
    if not a then a = {}; memo[deg] = a end
    local b = a[cx]
    if not b then b = {}; a[cx] = b end
    local v = b[cy]
    if v == nil then v = f(deg, mx, my, cx, cy); b[cy] = v end
    return v
  end
end
local function memo_banned(f)
  local memo = {}
  return function(deg)
    local v = memo[deg]
    if v == nil then v = f(deg) and true or false; memo[deg] = v end
    return v
  end
end

local function key(...)
  local t = { ... }
  for i = 1, #t do t[i] = num(t[i]) end
  return table.concat(t, ",")
end
M.key = key

-- Serialize a plain value (numbers, strings, booleans, tables of them).
local function ser(v, out)
  local tv = type(v)
  if tv == "number" then out[#out + 1] = num(v)
  elseif tv == "string" then out[#out + 1] = fmt("%q", v)
  elseif tv == "boolean" then out[#out + 1] = v and "true" or "false"
  elseif tv == "table" then
    out[#out + 1] = "{"
    local keys = {}
    for k in pairs(v) do keys[#keys + 1] = k end
    table.sort(keys, function(a, b)
      local ta, tb = type(a), type(b)
      if ta ~= tb then return ta < tb end
      return a < b
    end)
    for _, k in ipairs(keys) do
      out[#out + 1] = "["
      ser(k, out)
      out[#out + 1] = "]="
      ser(v[k], out)
      out[#out + 1] = ","
    end
    out[#out + 1] = "}"
  else
    out[#out + 1] = "nil"
  end
end

local PILL_FIELDS = { "mx", "my", "health", "in_tank", "pos_unknown", "owner", "anger", "id" }
local ENV_FIELDS = { "T_BUILDING", "T_HALFBUILD", "T_BOAT", "T_FOREST", "wall_shots",
                     "half_wall_shots", "boat_shots", "forest_transparent" }

local n_written = 0

function M.evaluate(V2, inp, path)
  local env = inp.env
  local pidx, plist = {}, {}
  local function pi(p)
    if p == nil then return 0 end
    local k = pidx[p]
    if not k then
      k = #plist + 1
      plist[k] = p
      pidx[p] = k
    end
    return k
  end
  for _, p in ipairs(inp.pills) do pi(p) end
  pi(inp.A)
  local rec = {
    ttype = {}, pill_on = {}, is_ally = {}, cover = {}, base_stops = {}, base_at = {},
    in_map = {}, stay_in = {}, standable = {}, buildable = {}, lgm_reach = {},
    score_fn = {}, banned = {}, anger_of = {},
  }
  -- a copy of env with wrapped functions
  local e2 = {}
  for k, v in pairs(env) do e2[k] = v end
  e2.ttype = function(mx, my)
    local v = env.ttype(mx, my)
    rec.ttype[key(mx, my)] = v
    return v
  end
  e2.pill_on = function(mx, my)
    local p = env.pill_on(mx, my)
    rec.pill_on[key(mx, my)] = pi(p)
    return p
  end
  e2.is_ally = function(p)
    local v = env.is_ally(p) and true or false
    rec.is_ally[pi(p)] = v
    return v
  end
  e2.cover_of_pill = function(p)
    local v = env.cover_of_pill(p)
    rec.cover[pi(p)] = v
    return v
  end
  if env.base_stops then
    e2.base_stops = function(mx, my)
      env.omx, env.omy = e2.omx, e2.omy
      local v = env.base_stops(mx, my) and true or false
      rec.base_stops[key(mx, my, e2.omx or mx, e2.omy or my)] = v
      local ba = env.base_at
      if ba and ba[my * 256 + mx] then rec.base_at[my * 256 + mx] = true end
      return v
    end
  end
  local i2 = {}
  for k, v in pairs(inp) do i2[k] = v end
  i2.env = e2
  local function wrap2(name)
    local f = inp[name]
    if not f then return end
    i2[name] = function(a, b)
      local v = f(a, b) and true or false
      rec[name][key(a, b)] = v
      return v
    end
  end
  wrap2("in_map"); wrap2("stay_in"); wrap2("standable"); wrap2("buildable")
  i2.lgm_reach = function(omx, omy, mx, my)
    local v = inp.lgm_reach(omx, omy, mx, my) and true or false
    rec.lgm_reach[key(omx, omy, mx, my)] = v
    return v
  end
  i2.score_fn = function(deg, mx, my, cx, cy)
    local v = inp.score_fn(deg, mx, my, cx, cy)
    rec.score_fn[key(deg, mx, my, cx, cy)] = v
    return v
  end
  if inp.banned then
    i2.banned = function(deg)
      local v = inp.banned(deg) and true or false
      rec.banned[key(deg)] = v
      return v
    end
  end
  if inp.anger_of then
    i2.anger_of = function(p)
      local v = inp.anger_of(p)
      rec.anger_of[pi(p)] = v or false
      return v
    end
  end
  local res = V2.evaluate_lua(i2)
  env.omx, env.omy = e2.omx, e2.omy
  res.inp = inp
  -- the case
  local case = { inp = {}, env = {}, pills = {}, A = pidx[inp.A], rec = rec, chain_skip = {},
                 n_list = #inp.pills }
  for k, v in pairs(inp) do
    local tv = type(v)
    if tv == "number" or tv == "boolean" or tv == "string" then case.inp[k] = v end
  end
  case.inp.radii_offsets = inp.radii_offsets
  -- PT2_FARTHEST_RING: the absolute ring radii
  case.inp.ring_radii = inp.ring_radii
  for _, k in ipairs(ENV_FIELDS) do case.env[k] = env[k] end
  for k, p in ipairs(plist) do
    local q = {}
    for _, f in ipairs(PILL_FIELDS) do q[f] = p[f] end
    q.name = inp.ids and inp.ids[p] or nil
    case.pills[k] = q
  end
  if inp.chain_skip then
    for k, p in ipairs(plist) do
      if inp.chain_skip[p] then case.chain_skip[k] = true end
    end
  end
  local out = { "CASES[#CASES + 1] = " }
  ser(case, out)
  out[#out + 1] = "\n"
  -- one file per brain (two bots of one exe must not write one file).
  -- The brain sandbox has no io (only -brain-debug lifts it): then the
  -- case is not written and the result is still returned.
  local f = rawget(_G, "io") and io.open(path .. "." .. (tostring(M):match("0x(%x+)") or tostring(M):match("(%x+)$") or "x"), "a")
  if f then
    f:write(table.concat(out))
    f:close()
    n_written = n_written + 1
  end
  return res
end

-- An inp that answers from the record. Returns inp and the pill list.
function M.replay(case, V2)
  local rec = case.rec
  local pills = {}
  for k, q in ipairs(case.pills) do
    local p = {}
    for f, v in pairs(q) do if f ~= "name" then p[f] = v end end
    pills[k] = p
  end
  local function look(tab, name, k)
    local v = tab[k]
    if v == nil then error("pt2 replay: no " .. name .. " answer for " .. tostring(k), 2) end
    return v
  end
  local env = {}
  for k, v in pairs(case.env) do env[k] = v end
  env.ttype = function(mx, my) return look(rec.ttype, "ttype", key(mx, my)) end
  env.pill_on = function(mx, my)
    local k = look(rec.pill_on, "pill_on", key(mx, my))
    return k > 0 and pills[k] or nil
  end
  local pidx = {}
  for k, p in ipairs(pills) do pidx[p] = k end
  env.is_ally = function(p) return look(rec.is_ally, "is_ally", pidx[p]) end
  env.cover_of_pill = function(p) return look(rec.cover, "cover", pidx[p]) end
  env.base_at = {}
  for sq in pairs(rec.base_at) do env.base_at[sq] = true end
  env.base_stops = function(mx, my)
    return look(rec.base_stops, "base_stops", key(mx, my, env.omx or mx, env.omy or my))
  end
  local inp = {}
  for k, v in pairs(case.inp) do inp[k] = v end
  inp.env = env
  inp.pills = {}
  -- inp.pills of the game: the first n_list pills of the record (a pill
  -- only pill_on or inp.A found comes after them)
  for k = 1, case.n_list do inp.pills[k] = pills[k] end
  inp.A = pills[case.A]
  inp.ids = {}
  for k, q in ipairs(case.pills) do inp.ids[pills[k]] = q.name end
  for _, name in ipairs({ "in_map", "stay_in", "standable", "buildable" }) do
    local tab = rec[name]
    inp[name] = function(a, b) return look(tab, name, key(a, b)) end
  end
  inp.lgm_reach = function(omx, omy, mx, my) return look(rec.lgm_reach, "lgm_reach", key(omx, omy, mx, my)) end
  inp.score_fn = memo_score(function(deg, mx, my, cx, cy)
    return look(rec.score_fn, "score_fn", key(deg, mx, my, cx, cy))
  end)
  if next(rec.banned) then inp.banned = memo_banned(function(deg) return look(rec.banned, "banned", key(deg)) end) end
  if next(rec.anger_of) then
    inp.anger_of = function(p)
      local v = look(rec.anger_of, "anger_of", pidx[p])
      return v ~= false and v or nil
    end
  end
  inp.chain_skip = {}
  for k in pairs(case.chain_skip) do inp.chain_skip[pills[k]] = true end
  inp.capture_file = nil
  return inp, pills
end

return M
