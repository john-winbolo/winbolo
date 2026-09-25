-- =========================================================================
-- test_kill_lgm_dest.lua — standalone unit tests for LGM_DEST_AIM
-- (kill_lgm.find_dest_pill + the dest_pill tiers of kill_lgm.predict_aim).
-- No engine: the terrain is stubbed to open grass, the man's walk is fed
-- through the real kill_lgm.update_velocity one brain tick at a time with
-- 16-wu quantized positions, the way perception sees him.
--
-- Run from this directory (or from opt/) with the LuaJIT built beside the game:
--   ../../build/_deps/luajit_src-src/src/luajit.exe test_kill_lgm_dest.lua
-- =========================================================================

package.path = "./?.lua;" .. package.path

local C = require("constants")

-- Terrain stub: open grass everywhere unless a test paints a tile.
TERRAIN_MASK = 0xFF
local painted = {}
function get_terrain(mx, my)
  return painted[my * 256 + mx] or C.T_GRASS
end

local K = require("kill_lgm")

local pass, fail = 0, 0
local function check(name, cond, got)
  if cond then
    pass = pass + 1
    print(string.format("  ok   %s", name))
  else
    fail = fail + 1
    print(string.format("  FAIL %s   (got %s)", name, tostring(got)))
  end
end

local function q16(v) return math.floor(v / 16) * 16 end

-- Feed a walk into update_velocity. steps = list of {wx, wy} (unquantized),
-- one per brain tick. Returns h, lgm entry, now.
local function feed(steps, idnum)
  local state = {}
  local lgm
  idnum = idnum or 7
  for i, p in ipairs(steps) do
    lgm = { wx = q16(p[1]), wy = q16(p[2]), idnum = idnum }
    K.update_velocity(state, lgm, i)
  end
  return state._enemy_lgm_history[idnum], lgm, #steps, state
end

-- Straight walk: n ticks from (sx, sy) toward (tx, ty) at v wu/tick.
local function straight(sx, sy, tx, ty, v, n)
  local dx, dy = tx - sx, ty - sy
  local d = math.sqrt(dx * dx + dy * dy)
  local ux, uy = dx / d, dy / d
  local out = {}
  for i = 0, n - 1 do out[#out + 1] = { sx + ux * v * i, sy + uy * v * i } end
  return out, ux, uy
end

local function centre(mx, my) return mx * 256 + 128, my * 256 + 128 end

local function pill(mx, my, health, owner)
  return { mx = mx, my = my, health = health or 0, owner = owner or "hostile", in_tank = false }
end

C.LGM_DEST_AIM = true
C.LGM_DEST_LIVE_PILLS = true
-- Tests 1-18 cover the aim without the trigger hold; 19+ turn it on.
C.LGM_DEST_HOLD_FIRE = false

print("kill_lgm.lua — LGM_DEST_AIM find_dest_pill")

-- 1. Straight walker toward a dead hostile pill -> locks it.
do
  local pcx, pcy = centre(110, 104)
  local walk = straight(100 * 256 + 40, 100 * 256 + 200, pcx, pcy, 16, 24)
  local h, lgm, now = feed(walk)
  local pills = { [3] = pill(110, 104) }
  local dp, ray, why = K.find_dest_pill(h, lgm, pills, {}, now)
  check("straight walker locks the pill", dp and dp.id == 3 and why == "lock", why)
  check("lock carries the pill centre", dp and dp.cx == pcx and dp.cy == pcy, dp and dp.cx)
  check("ray speed ~16 wu/tick", ray and math.abs(ray.speed - 16) < 1.5, ray and ray.speed)
end

-- 2. Walker toward nothing -> no_pill (a dead pill well off to the side).
do
  local walk = straight(100 * 256, 100 * 256, 120 * 256, 100 * 256, 16, 24)
  local h, lgm, now = feed(walk)
  local pills = { [1] = pill(108, 104) }   -- 4 tiles off the line
  local dp, ray, why = K.find_dest_pill(h, lgm, pills, {}, now)
  check("walker toward nothing -> fallback no_pill", dp == nil and why == "no_pill", why)
end

-- 3. Stationary man -> stopped.
do
  local walk = {}
  for _ = 1, 24 do walk[#walk + 1] = { 100 * 256 + 128, 100 * 256 + 128 } end
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(101, 100) }, {}, now)
  check("stationary man -> fallback stopped", dp == nil and why == "stopped", why)
end

-- 4. Walked straight then got stuck (last 6 ticks in place) -> stopped.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 110 * 256 + 128, 100 * 256 + 128, 16, 18)
  local last = walk[#walk]
  for _ = 1, 6 do walk[#walk + 1] = { last[1], last[2] } end
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(110, 100) }, {}, now)
  check("stuck man (stopped mid-walk) -> fallback stopped", dp == nil and why == "stopped", why)
end

-- 5. Zig-zag -> zigzag (4 ticks east, 4 south-east, 4 east, 4 north-east ...).
do
  local walk = {}
  local x, y = 100 * 256, 100 * 256 + 128
  local legs = { { 16, 0 }, { 11, 11 }, { 16, 0 }, { 11, -11 } }
  for i = 0, 23 do
    walk[#walk + 1] = { x, y }
    local leg = legs[(math.floor(i / 4) % 4) + 1]
    x, y = x + leg[1], y + leg[2]
  end
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(104, 100) }, {}, now)
  check("zig-zag -> fallback zigzag", dp == nil and why == "zigzag", why)
end

-- 6. Two pills on the ray -> the nearest.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 16, 24)
  local h, lgm, now = feed(walk)
  local pills = { [5] = pill(109, 100), [2] = pill(105, 100) }
  local dp, _, why = K.find_dest_pill(h, lgm, pills, {}, now)
  check("two pills on the ray -> nearest", dp and dp.id == 2, dp and dp.id or why)
end

-- 7. Pill behind him -> ignored.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 16, 24)
  local h, lgm, now = feed(walk)
  local pills = { [1] = pill(99, 100) }
  local dp, _, why = K.find_dest_pill(h, lgm, pills, {}, now)
  check("pill behind him -> ignored (no_pill)", dp == nil and why == "no_pill", why)
end

-- 8. Only HOSTILE deployed pills below full armour match (full live pill,
--    friendly, neutral, carried: ignored).
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 16, 24)
  local h, lgm, now = feed(walk)
  local carried = pill(104, 100); carried.in_tank = true
  local pills = { [1] = pill(103, 100, C.PILLS_MAX_HEALTH), [2] = pill(104, 100, 0, "friendly"),
                  [3] = pill(105, 100, 0, "neutral"), [4] = carried }
  local dp, _, why = K.find_dest_pill(h, lgm, pills, {}, now)
  check("full-health / friendly / neutral / carried pills ignored", dp == nil and why == "no_pill", why)
end

-- 9. Beyond LGM_DEST_RAY_TILES -> ignored.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 130 * 256 + 128, 100 * 256 + 128, 16, 24)
  local h, lgm, now = feed(walk)
  local far = 101 + C.LGM_DEST_RAY_TILES + 2
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(far, 100) }, {}, now)
  check("pill beyond the ray length ignored", dp == nil and why == "no_pill", why)
end

-- 10. Two pills side by side at the same depth -> ambiguous.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 256, 112 * 256 + 128, 100 * 256 + 256, 16, 24)
  local h, lgm, now = feed(walk)
  -- Ray runs along the border between rows 100 and 101 at far range where the
  -- tolerance is wide enough to cover both.
  local pills = { [1] = pill(115, 100), [2] = pill(115, 101) }
  local dp, _, why = K.find_dest_pill(h, lgm, pills, {}, now)
  check("two pills at the same depth -> fallback ambiguous", dp == nil and why == "ambiguous", why)
end

-- 11. Turned back along the same line (20 ticks east, then 5 west) -> turned_back.
do
  local walk = {}
  for i = 0, 19 do walk[#walk + 1] = { 100 * 256 + 128 + i * 16, 100 * 256 + 128 } end
  for i = 1, 5 do walk[#walk + 1] = { 100 * 256 + 128 + (19 - i) * 16, 100 * 256 + 128 } end
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(106, 100) }, {}, now)
  check("turned back -> fallback turned_back", dp == nil and why == "turned_back", why)
end

-- 12. Sliding along a wall: straight but at 11 wu/tick on grass (16) -> sliding.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 11, 24)
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(105, 100) }, {}, now)
  check("slow straight walk on grass -> fallback sliding", dp == nil and why == "sliding", why)
  -- Same speed on forest (8) is a normal walk, not a slide.
  for x = 95, 115 do painted[100 * 256 + x] = C.T_FOREST end
  local walk2 = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 8, 24)
  local h2, lgm2, now2 = feed(walk2)
  local dp2, _, why2 = K.find_dest_pill(h2, lgm2, { [1] = pill(103, 100) }, {}, now2)
  check("8 wu/tick through forest still locks", dp2 and dp2.id == 1, why2)
  for x = 95, 115 do painted[100 * 256 + x] = nil end
end

-- 13. Walking at his own tank -> returning.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 16, 24)
  local h, lgm, now = feed(walk)
  lgm.near_tank_idnum = 4
  local tanks = { { id = 4, wx = 108 * 256, wy = 100 * 256 + 128 } }
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(105, 100) }, tanks, now)
  check("walking toward his tank -> fallback returning", dp == nil and why == "returning", why)
  local tanks_behind = { { id = 4, wx = 98 * 256, wy = 100 * 256 + 128 } }
  local dp2 = K.find_dest_pill(h, lgm, { [1] = pill(105, 100) }, tanks_behind, now)
  check("his tank behind him -> still locks", dp2 and dp2.id == 1, dp2)
end

-- 14. Just out of the tank (few samples) -> warming.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 16, 3)
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(105, 100) }, {}, now)
  check("just out of the tank -> fallback warming", dp == nil and why == "warming", why)
end

print("kill_lgm.lua — LGM_DEST_AIM predict_aim")

-- 15. Close to the pill: impact lands on the pill tile -> aim at its centre.
do
  local pcx, pcy = centre(110, 104)
  -- Start so that after 34 ticks he is ~1.3 tiles short of the pill centre.
  local dx, dy = 1, 0.6
  local d = math.sqrt(dx * dx + dy * dy); dx, dy = dx / d, dy / d
  local sx, sy = pcx - dx * (33 * 16 + 330), pcy - dy * (33 * 16 + 330)
  local walk = straight(sx, sy, pcx, pcy, 16, 34)
  local h, lgm, now = feed(walk)
  h.dest_pill = K.find_dest_pill(h, lgm, { [9] = pill(110, 104) }, {}, now)
  check("close walker locked", h.dest_pill and h.dest_pill.id == 9, h.dest_pill)
  local tx, ty = pcx - 5 * 256, pcy + 3 * 256
  local ax, ay, sl, ft, dwu, tier = K.predict_aim(tx, ty, lgm, h)
  check("impact on the pill tile -> tier dest_pill", tier == "dest_pill", tier)
  check("aim = pill tile centre", ax == pcx and ay == pcy, string.format("(%.0f,%.0f)", ax, ay))
  check("sightLen from tank to centre", sl == K.sightlen_for(math.sqrt((tx - pcx) ^ 2 + (ty - pcy) ^ 2)), sl)
  -- The 3-window dest_lock (map-edge sim) would lead him PAST the pill.
  local h_old = {}
  for k, v in pairs(h) do h_old[k] = v end
  h_old.dest_pill = nil
  local ox, oy, _, _, _, otier = K.predict_aim(tx, ty, lgm, h_old)
  local over = (ox - pcx) * dx + (oy - pcy) * dy
  check("old predictor would overshoot past the pill centre", otier == "dest_lock" and over > 64,
        string.format("%s over=%.0f", tostring(otier), over))
end

-- 16. Far from the pill: impact before he gets there -> lead point on the line,
--     never past the pill.
do
  local pcx, pcy = centre(114, 100)
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, pcx, pcy, 16, 24)
  local h, lgm, now = feed(walk)
  h.dest_pill = K.find_dest_pill(h, lgm, { [1] = pill(114, 100) }, {}, now)
  local tx, ty = lgm.wx + 2 * 256, lgm.wy + 4 * 256
  local ax, ay, _, _, _, tier = K.predict_aim(tx, ty, lgm, h)
  check("far walker -> tier dest_pill_lead", tier == "dest_pill_lead", tier)
  check("lead point ahead of him, short of the pill", ax > lgm.wx and ax < pcx - 128 and math.abs(ay - lgm.wy) < 32,
        string.format("(%.0f,%.0f)", ax, ay))
end

-- 17. Knob off -> predict_aim ignores h.dest_pill: identical to no lock at all.
do
  local pcx, pcy = centre(110, 104)
  local dx, dy = 1, 0.6
  local d = math.sqrt(dx * dx + dy * dy); dx, dy = dx / d, dy / d
  local sx, sy = pcx - dx * (23 * 16 + 330), pcy - dy * (23 * 16 + 330)
  local h, lgm, now = feed(straight(sx, sy, pcx, pcy, 16, 24))
  h.dest_pill = K.find_dest_pill(h, lgm, { [9] = pill(110, 104) }, {}, now)
  local tx, ty = pcx - 5 * 256, pcy + 3 * 256
  C.LGM_DEST_AIM = false
  local a = { K.predict_aim(tx, ty, lgm, h) }
  local saved = h.dest_pill
  h.dest_pill = nil
  local b = { K.predict_aim(tx, ty, lgm, h) }
  h.dest_pill = saved
  C.LGM_DEST_AIM = true
  local same = true
  for i = 1, 6 do if a[i] ~= b[i] then same = false end end
  check("knob off -> aim identical to no destination lock", same and a[6] ~= "dest_pill",
        string.format("%s vs %s", tostring(a[6]), tostring(b[6])))
  check("PRESETS.keel turns the feature off", C.PRESETS.keel.LGM_DEST_AIM == false, C.PRESETS.keel.LGM_DEST_AIM)
end

-- 18. sim_forward_to_dest without bless args is unchanged; with them the man
--     steps onto the pill tile (T_PILLBOX in the brain map) and stops at its centre.
do
  local pcx, pcy = centre(105, 100)
  painted[100 * 256 + 105] = C.T_PILLBOX
  local sx, sy = 100 * 256 + 128, 100 * 256 + 128
  local ex, ey = K.sim_forward_to_dest(sx, sy, pcx, pcy, 200)
  check("plain sim stalls at the pill tile edge", ex < 105 * 256 and ex > 104 * 256, ex)
  local bx, by = K.sim_forward_to_dest(sx, sy, pcx, pcy, 200, 105, 100)
  check("bless sim arrives at the pill centre and holds", bx == pcx and by == pcy, bx)
  painted[100 * 256 + 105] = nil
end

-- =========================================================================
-- LGM_DEST_LIVE_PILLS + LGM_DEST_HOLD_FIRE (Andrew's review, 2026-09-25)
-- =========================================================================
print("\nkill_lgm.lua — live damaged pills + hold fire")

-- 19. A live hostile pill below full armour is a destination (repair walk);
--     a full one is not; LGM_DEST_LIVE_PILLS off = dead pills only.
do
  local walk = straight(100 * 256 + 128, 100 * 256 + 128, 112 * 256 + 128, 100 * 256 + 128, 16, 24)
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [4] = pill(106, 100, 7) }, {}, now)
  check("live damaged pill (health 7) locks", dp and dp.id == 4 and dp.live == true and dp.health == 7, why)
  dp, _, why = K.find_dest_pill(h, lgm, { [4] = pill(106, 100, C.PILLS_MAX_HEALTH) }, {}, now)
  check("full-health live pill does not lock", dp == nil and why == "no_pill", why)
  C.LGM_DEST_LIVE_PILLS = false
  dp, _, why = K.find_dest_pill(h, lgm, { [4] = pill(106, 100, 7) }, {}, now)
  C.LGM_DEST_LIVE_PILLS = true
  check("LGM_DEST_LIVE_PILLS off -> damaged live pill ignored", dp == nil and why == "no_pill", why)
  dp = K.find_dest_pill(h, lgm, { [4] = pill(106, 100, 0) }, {}, now)
  check("dead pill lock says live=false", dp and dp.live == false, dp and dp.live)
end

C.LGM_DEST_HOLD_FIRE = true

-- Walk n ticks east along row 100 toward pill tile (pmx, 100), lock, and
-- predict from a tank at (tx, ty).  Returns tier, h, aim x/y, sl, ft, lgm.
local function walk_and_predict(n, pmx, health, tx, ty, v)
  local pcx, pcy = centre(pmx, 100)
  local walk = straight(96 * 256 + 128, 100 * 256 + 128, pcx, pcy, v or 16, n)
  local h, lgm, now = feed(walk)
  h.dest_pill = K.find_dest_pill(h, lgm, { [1] = pill(pmx, 100, health) }, {}, now)
  local ax, ay, sl, ft, _, tier = K.predict_aim(tx, ty, lgm, h)
  return tier, h, ax, ay, sl, ft, lgm
end

-- 20. Live pill: the aim is the tile centre, and a shell fired at it is in
--     the pill tile at T_entry (it collides there, blast at the centre) and
--     not one tick earlier; T_entry < T_full; the gunsight reaches the tile.
do
  local pmx = 108
  local pcx, pcy = centre(pmx, 100)
  local tx, ty = pcx - 3 * 256 + 40, pcy + 4 * 256
  local tier, h, ax, ay, sl, ft = walk_and_predict(40, pmx, 6, tx, ty)
  local dt = h.dest_timing
  check("live pill: aim = tile centre", ax == pcx and ay == pcy, string.format("(%.0f,%.0f)", ax, ay))
  check("live pill: timing recorded as live", dt and dt.live == true, dt and dt.live)
  local D = math.sqrt((pcx - tx) ^ 2 + (pcy - ty) ^ 2)
  local ux, uy = (pcx - tx) / D, (pcy - ty) / D
  local function tile_at(t)
    local d = 32 * (t + 5)
    return bit.rshift(math.floor(tx + ux * d), 8), bit.rshift(math.floor(ty + uy * d), 8)
  end
  local mx, my = tile_at(dt.T_entry)
  local px, py = tile_at(dt.T_entry - 1)
  check("live pill: shell is in the pill tile at T_entry", mx == pmx and my == 100,
        string.format("(%d,%d) T=%d", mx, my, dt.T_entry))
  check("live pill: ... and not one tick earlier", not (px == pmx and py == 100), string.format("(%d,%d)", px, py))
  check("live pill: goes off at T_entry, before T_full", dt.T_x == dt.T_entry and dt.T_entry < dt.T_full and ft == dt.T_x,
        string.format("T_x=%d T_entry=%d T_full=%d", dt.T_x, dt.T_entry, dt.T_full))
  check("live pill: gunsight range reaches the tile", 128 * sl >= 32 * (dt.T_entry + 5), sl)
end

-- 21. Hold at the lead phase, release once the blast lands while he is on
--     the tile.  Tank 4 tiles south of the pill, man walking east.
do
  local pmx = 108
  local pcx, pcy = centre(pmx, 100)
  local tx, ty = pcx, pcy + 4 * 256
  local first_fire, early_before, bad = nil, 0, nil
  for n = 8, 185 do   -- 185 steps = 2960 wu, still short of the centre (3072)
    local tier, h = walk_and_predict(n, pmx, 0, tx, ty)
    local dt = h.dest_timing
    if tier == "dest_pill" then
      if not first_fire then first_fire = n end
      if not (dt and dt.verdict == "fire" and dt.s_enter <= dt.s_x
              and (dt.s_arrive == nil or dt.s_x <= dt.s_arrive + dt.dwell + dt.s_exit)) then bad = n end
    elseif tier == "dest_pill_hold" then
      if not first_fire then
        early_before = early_before + 1
        if dt.verdict ~= "early" then bad = n end
      end
    elseif h.dest_pill then
      bad = n  -- locked but neither fire nor hold
    end
  end
  check("far from the pill -> held (early) for several ticks", early_before >= 10 and not bad,
        string.format("early=%d bad=%s", early_before, tostring(bad)))
  check("released: tier dest_pill once the blast is inside his on-tile window", first_fire ~= nil and not bad,
        tostring(first_fire))
  local tier, h, ax, ay = walk_and_predict(10, pmx, 0, tx, ty)
  check("while held the aim is still the pill centre (gun ready)", tier == "dest_pill_hold" and ax == pcx and ay == pcy,
        string.format("%s (%.0f,%.0f)", tostring(tier), ax, ay))
end

-- 22. Late: he arrives and finishes his 20-tick wait before the blast.
do
  local pmx = 108
  local pcx, pcy = centre(pmx, 100)
  local tx, ty = pcx, pcy + 6 * 256 + 128   -- long flight
  -- Stop the walk 48 wu short of the centre (3 steps to arrive).
  local steps = math.floor((pcx - 48 - (96 * 256 + 128)) / 16) + 1
  local tier, h = walk_and_predict(steps, pmx, 0, tx, ty)
  local dt = h.dest_timing
  check("late: arrives early, gone before the shell -> hold (late)",
        tier == "dest_pill_hold" and dt and dt.verdict == "late",
        dt and string.format("%s s_x=%d arrive=%s", tier, dt.s_x, tostring(dt.s_arrive)) or tostring(tier))
end

-- 23. Lock lost (he stops) -> no lock, no hold, old aim fires as before.
do
  local pmx = 108
  local pcx, pcy = centre(pmx, 100)
  local walk = straight(96 * 256 + 128, 100 * 256 + 128, pcx, pcy, 16, 20)
  local last = walk[#walk]
  for _ = 1, 8 do walk[#walk + 1] = { last[1], last[2] } end
  local h, lgm, now = feed(walk)
  local dp, _, why = K.find_dest_pill(h, lgm, { [1] = pill(pmx, 100, 0) }, {}, now)
  h.dest_pill = dp
  local a = { K.predict_aim(pcx, pcy + 4 * 256, lgm, h) }
  C.LGM_DEST_AIM = false
  local b = { K.predict_aim(pcx, pcy + 4 * 256, lgm, h) }
  C.LGM_DEST_AIM = true
  local same = true
  for i = 1, 6 do if a[i] ~= b[i] then same = false end end
  check("stopped man -> lock dropped (stopped)", dp == nil and why == "stopped", why)
  check("lock lost -> old aim exactly, no hold, no timing", same and a[6] ~= "dest_pill_hold" and h.dest_timing == nil,
        tostring(a[6]))
end

-- 24. The sim cannot walk him to the pill (wall across the line) -> the hold
--     branch gives up: old predictor, no hold.
do
  local pmx = 108
  local pcx, pcy = centre(pmx, 100)
  -- Column 97: inside his next s_full sim steps (the sim only looks that far).
  for y = 97, 103 do painted[y * 256 + 97] = C.T_BUILDING end
  local tier, h = walk_and_predict(24, pmx, 0, pcx, pcy + 4 * 256, 16)
  for y = 97, 103 do painted[y * 256 + 97] = nil end
  check("wall between man and pill -> no hold, old tier", h.dest_pill ~= nil and tier ~= "dest_pill_hold"
        and tier ~= "dest_pill" and h.dest_timing == nil, tier)
end

-- 25. LGM_DEST_HOLD_FIRE off -> exactly the aim of tests 15/16 (no hold
--     tier, no timing); keel pins both new knobs off.
do
  local pmx = 110
  local pcx = centre(pmx, 100)
  local tx, ty = 98 * 256 + 128, 104 * 256 + 128
  C.LGM_DEST_HOLD_FIRE = false
  local tier, h, ax = walk_and_predict(24, pmx, 0, tx, ty)
  C.LGM_DEST_HOLD_FIRE = true
  local tier2 = walk_and_predict(24, pmx, 0, tx, ty)
  check("hold knob off -> lead tier, no timing", tier == "dest_pill_lead" and h.dest_timing == nil
        and ax < pcx - 128, tier)
  check("hold knob on -> same situation is held", tier2 == "dest_pill_hold", tier2)
  check("PRESETS.keel pins LGM_DEST_HOLD_FIRE / LIVE_PILLS off",
        C.PRESETS.keel.LGM_DEST_HOLD_FIRE == false and C.PRESETS.keel.LGM_DEST_LIVE_PILLS == false,
        tostring(C.PRESETS.keel.LGM_DEST_HOLD_FIRE))
end

print("\nconstants.lua — LGM destination aim is Hard only")

-- 26. Difficulty gating.  Resolve the knobs the way init.lua's
--     _apply_cfg_tokens does: a FRESH constants table, then the
--     MODE_LEVELS[mode][difficulty] bundle, then any preset= (level < preset).
--     Like _cfg_set, a bundle key must name an existing constant of the same
--     type (else init.lua refuses it and the level would not switch it off).
do
  local KNOBS = { "LGM_DEST_AIM", "LGM_DEST_LIVE_PILLS", "LGM_DEST_HOLD_FIRE" }
  local function resolve(mode, diff, preset)
    local M = dofile("constants.lua")
    local bad
    local function apply(tbl)
      for k, v in pairs(tbl) do
        if M[k] == nil or type(M[k]) ~= type(v) then bad = k else M[k] = v end
      end
    end
    if diff then
      local lv = M.MODE_LEVELS[mode] and M.MODE_LEVELS[mode][diff]
      if lv then apply(lv) end
    end
    if preset then apply(M.PRESETS[preset]) end
    return M, bad
  end
  local function all(M, want)
    for _, k in ipairs(KNOBS) do if M[k] ~= want then return false, k end end
    return true
  end
  local D = dofile("constants.lua")
  check("no difficulty token = C.DIFFICULTY 'hard' = feature on",
        D.DIFFICULTY == "hard" and all(D, true), D.DIFFICULTY)
  for _, mode in ipairs({ "default", "survival" }) do
    local M, bad = resolve(mode, "hard")
    check(mode .. "/hard -> AIM, LIVE_PILLS, HOLD_FIRE on", all(M, true) and not bad, bad)
    for _, diff in ipairs({ "medium", "easy" }) do
      local Md, badd = resolve(mode, diff)
      local ok, which = all(Md, false)
      check(mode .. "/" .. diff .. " -> all three off", ok and not badd, which or badd)
    end
  end
  for _, diff in ipairs({ "hard", "medium", "easy" }) do
    local M = resolve("default", diff, "keel")
    check("default/" .. diff .. " + preset=keel -> all three off", (all(M, false)), diff)
  end
  -- Behaviour at Medium: the real knobs set from the medium bundle, a man
  -- walking into a dead pill that find_dest_pill locks -> predict_aim gives
  -- exactly the aim of no lock at all, and no hold (tier never dest_pill_hold).
  local saved = {}
  for _, k in ipairs(KNOBS) do saved[k] = C[k]; C[k] = C.MODE_LEVELS.default.medium[k] end
  local pmx = 108
  local pcx, pcy = centre(pmx, 100)
  local tx, ty = pcx, pcy + 4 * 256
  local walk = straight(96 * 256 + 128, 100 * 256 + 128, pcx, pcy, 16, 10)
  local h, lgm, now = feed(walk)
  h.dest_pill = K.find_dest_pill(h, lgm, { [1] = pill(pmx, 100, 0) }, {}, now)
  local a = { K.predict_aim(tx, ty, lgm, h) }
  local lock = h.dest_pill
  h.dest_pill = nil
  local b = { K.predict_aim(tx, ty, lgm, h) }
  for _, k in ipairs(KNOBS) do C[k] = saved[k] end
  local same = true
  for i = 1, 6 do if a[i] ~= b[i] then same = false end end
  check("medium: a lock exists but the aim equals no lock (old predictor)",
        lock ~= nil and same and a[6] ~= "dest_pill" and a[6] ~= "dest_pill_hold"
        and a[6] ~= "dest_pill_lead" and h.dest_timing == nil,
        string.format("%s vs %s", tostring(a[6]), tostring(b[6])))
  -- The same walk on Hard is held (test 21's early phase).
  local ah = { K.predict_aim(tx, ty, lgm, (function() h.dest_pill = lock; return h end)()) }
  check("hard: same walk -> dest_pill_hold", ah[6] == "dest_pill_hold", ah[6])
end

-- =========================================================================
-- The walk off the pill tile after the build (bug fix, 2026-09-25): he is
-- still on the pill tile until he crosses its edge, and the pill is live
-- by then, so a shell in the tile during the walk-off kills him.
-- =========================================================================
print("\nkill_lgm.lua — hold fire: the walk off the tile")

-- 27. Exit steps from the centre toward his tank: east vs north.  Man at
--     16 wu/step (MAN_SPEED_BLESSED on the pill tile).  East: x offset
--     128 + 16k leaves [0,256) at k = 8 -> 7 steps still on the tile.
--     North: y offset 128 - 16k is still 0 (on the tile) at k = 8 and
--     leaves at k = 9 -> 8 steps.  South-east diagonal: 11.3 wu per axis per
--     step, leaves at k = 12 -> 11.
do
  local pmx, pmy = 108, 100
  local pcx, pcy = centre(pmx, pmy)
  local p = { mx = pmx, my = pmy, cx = pcx, cy = pcy }
  local e, es = K.dest_exit_steps(p, pcx + 10 * 256, pcy)
  local n, ns = K.dest_exit_steps(p, pcx, pcy - 10 * 256)
  local d, ds = K.dest_exit_steps(p, pcx + 10 * 256, pcy + 10 * 256)
  check("exit toward a tank to the east: 7 steps still on the tile", e == 7 and not es, e)
  check("exit toward a tank to the north: 8 steps still on the tile", n == 8 and not ns, n)
  check("exit toward a tank to the south-east: 11 steps", d == 11 and not ds, d)
  -- Wall on the way out (east neighbour is a building, tank due east): the
  -- sim walks him to the tile edge (7 steps) and stops him there, still on
  -- the tile -> 7 steps, stalled, loop bounded.
  painted[pmy * 256 + pmx + 1] = C.T_BUILDING
  local w, ws = K.dest_exit_steps(p, pcx + 10 * 256, pcy)
  painted[pmy * 256 + pmx + 1] = nil
  check("exit blocked by a wall -> stalled at the edge, 7 steps", w == 7 and ws == true, tostring(w) .. " " .. tostring(ws))
end

-- 28. The fire window includes the walk-off.  Man at the pill centre now
--     (s_enter = s_arrive = 0), dead pill, his tank 10 tiles east
--     (s_exit = 7), shooter south of the pill at a sweep of distances.
--     Window = [0, 0 + 20 + 7].
do
  local pmx, pmy = 108, 100
  local pcx, pcy = centre(pmx, pmy)
  local p = { id = 1, mx = pmx, my = pmy, cx = pcx, cy = pcy, live = false }
  local walkoff_fire, late, dwell_fire, bad = 0, 0, 0, nil
  for dy = 2 * 256, 12 * 256, 16 do
    local h = { owner_wx = pcx + 10 * 256, owner_wy = pcy, owner_src = "tank" }
    local _, _, _, _, _, tier = K.predict_dest_hold(pcx, pcy + dy, { wx = pcx, wy = pcy }, h, p)
    local dt = h.dest_timing
    if not (dt.s_enter == 0 and dt.s_arrive == 0 and dt.s_exit == 7 and dt.exit_src == "tank") then
      bad = "timeline " .. dy
    elseif dt.s_x <= 20 then
      if tier == "dest_pill" and dt.verdict == "fire" then dwell_fire = dwell_fire + 1 else bad = "dwell " .. dy end
    elseif dt.s_x <= 27 then
      if tier == "dest_pill" and dt.verdict == "fire" then walkoff_fire = walkoff_fire + 1 else bad = "walkoff " .. dy end
    else
      if tier == "dest_pill_hold" and dt.verdict == "late" then late = late + 1 else bad = "late " .. dy end
    end
  end
  check("shell goes off during the build -> fire", dwell_fire > 0 and not bad, dwell_fire)
  check("shell goes off during the walk-off (s_arrive+20 < s_x <= +s_exit) -> fire (was 'late')",
        walkoff_fire > 0 and not bad, string.format("%d bad=%s", walkoff_fire, tostring(bad)))
  check("shell goes off after he leaves the tile -> late", late > 0 and not bad,
        string.format("%d bad=%s", late, tostring(bad)))
end

-- 29. Same shooter, his tank north instead of east: s_exit = 8, so the
--     window end moves one step (the timeline follows the exit direction).
do
  local pmx, pmy = 108, 100
  local pcx, pcy = centre(pmx, pmy)
  local p = { id = 1, mx = pmx, my = pmy, cx = pcx, cy = pcy, live = false }
  local found
  for dy = 2 * 256, 12 * 256, 16 do
    local he = { owner_wx = pcx + 10 * 256, owner_wy = pcy, owner_src = "tank" }
    local hn = { owner_wx = pcx, owner_wy = pcy - 10 * 256, owner_src = "tank" }
    K.predict_dest_hold(pcx, pcy + dy, { wx = pcx, wy = pcy }, he, p)
    K.predict_dest_hold(pcx, pcy + dy, { wx = pcx, wy = pcy }, hn, p)
    if he.dest_timing.s_x == 28 then
      found = { he.dest_timing, hn.dest_timing }
      break
    end
  end
  check("s_x = 28: tank east (s_exit 7) -> late, tank north (s_exit 8) -> fire",
        found and found[1].verdict == "late" and found[1].s_exit == 7
              and found[2].verdict == "fire" and found[2].s_exit == 8,
        found and (found[1].verdict .. "/" .. found[2].verdict) or "no s_x = 28 in sweep")
end

-- 30. No tank position -> mirror: s_exit = s_arrive - s_enter.  Man 200 wu
--     west of the centre walking east at 16: on the tile at step 5 (-120),
--     arrives at step 12 (within 16 wu) -> s_exit = 7, exit_src "mirror".
do
  local pmx, pmy = 108, 100
  local pcx, pcy = centre(pmx, pmy)
  local p = { id = 1, mx = pmx, my = pmy, cx = pcx, cy = pcy, live = false }
  local h = {}
  K.predict_dest_hold(pcx, pcy + 6 * 256, { wx = pcx - 200, wy = pcy }, h, p)
  local dt = h.dest_timing
  check("unknown tank -> mirror fallback, s_exit = s_arrive - s_enter",
        dt and dt.exit_src == "mirror" and dt.s_enter == 5 and dt.s_arrive == 12 and dt.s_exit == 7,
        dt and string.format("%s enter=%s arrive=%s exit=%s", tostring(dt.exit_src),
                             tostring(dt.s_enter), tostring(dt.s_arrive), tostring(dt.s_exit)))
  -- No arrival in the look-ahead: no late end, no exit computed.
  local h2 = {}
  K.predict_dest_hold(pcx, pcy + 2 * 256, { wx = pcx - 20 * 256, wy = pcy }, h2, p)
  check("no arrival in look-ahead -> no s_exit, never late",
        h2.dest_timing and h2.dest_timing.s_arrive == nil and h2.dest_timing.s_exit == nil
        and h2.dest_timing.verdict ~= "late", h2.dest_timing and h2.dest_timing.verdict)
end

-- 31. note_owner_pos: his tank visible -> "tank"; out of sight -> the last
--     position we saw it at ("tank_last"); a different owner id -> cleared.
do
  local h = {}
  local lgm = { near_tank_idnum = 4 }
  K.note_owner_pos(h, lgm, { { id = 2, wx = 1, wy = 1 }, { id = 4, wx = 5000, wy = 6000 } })
  check("owner visible -> tank position, src tank",
        h.owner_wx == 5000 and h.owner_wy == 6000 and h.owner_src == "tank", h.owner_src)
  K.note_owner_pos(h, lgm, { { id = 2, wx = 1, wy = 1 } })
  check("owner out of sight -> last seen position, src tank_last",
        h.owner_wx == 5000 and h.owner_wy == 6000 and h.owner_src == "tank_last", h.owner_src)
  K.note_owner_pos(h, { near_tank_idnum = 9 }, {})
  check("different owner id -> position cleared (mirror fallback)",
        h.owner_wx == nil and h.owner_src == nil, h.owner_src)
  local h0 = {}
  K.note_owner_pos(h0, {}, { { id = 4, wx = 5000, wy = 6000 } })
  check("no owner id -> nothing recorded", h0.owner_wx == nil, h0.owner_wx)
end

print(string.format("\n%d passed, %d failed", pass, fail))
if fail > 0 then os.exit(1) end
