-- =========================================================================
-- NewAutopilot/constants.lua — terrain costs, tuning parameters
-- =========================================================================

local M = {}

M.BRAIN_NAME = "NewAutopilot"
M.LOG_STANDOFF_CANDIDATES = false  -- print every standoff candidate (very verbose)

M.TWO_PI = 2 * math.pi
M.MAP_W  = 256
M.WU     = 256   -- world-units per map square; centre = +0x80

-- Incremental A* budget: node expansions per game tick
M.ASTAR_BUDGET   = 1500

-- Recompute the path after this many ticks even if the goal hasn't changed
M.PATH_MAX_AGE   = 120  -- ~2.4 s

-- 8-connected neighbor offsets (N, E, S, W, NE, SE, SW, NW)
-- Cardinal dirs have cost multiplier 1.0, diagonals ~1.41
M.DIRS8 = {
  {0,-1, 1.0}, {1,0, 1.0}, {0,1, 1.0}, {-1,0, 1.0},       -- N, E, S, W
  {1,-1, 1.41}, {1,1, 1.41}, {-1,1, 1.41}, {-1,-1, 1.41},  -- NE, SE, SW, NW
}

-- Terrain type aliases (match engine constants)
M.T_BUILDING  = 0
M.T_RIVER     = 1
M.T_SWAMP     = 2
M.T_CRATER    = 3
M.T_ROAD      = 4
M.T_FOREST    = 5
M.T_RUBBLE    = 6
M.T_GRASS     = 7
M.T_HALFBUILD = 8
M.T_BOAT      = 9
M.T_DEEPSEA   = 10
M.T_REFBASE   = 11
M.T_PILLBOX   = 12

-- Unexplored tile terrain type (get_terrain() returns 13 for unseen tiles)
M.T_UNKNOWN = 13

-- Land-mode terrain costs (lower = faster)
-- Derived from engine mapGetSpeed() in bolo_map.h — cost ≈ 16 / max_speed.
-- Road=16, Grass=12, Forest=6, Swamp/Crater/Rubble=3, Building=0.
-- Swamp/crater/rubble are 5.3× slower than road on paper, but the low
-- max-speed (3) means ~85 ticks per tile vs ~16 on road, and the tank is
-- vulnerable to pill fire the entire time.  We add extra weight (×1.5) to
-- discourage routing through slow terrain near threats.
M.TERRAIN_COST_LAND = {
  [M.T_ROAD]      = 1,     -- speed 16
  [M.T_REFBASE]   = 1,     -- speed 16
  [M.T_GRASS]     = 2,     -- speed 12 → 16/12 ≈ 1.3, rounded up
  [M.T_BOAT]      = 2,     -- speed 16 (embarkation point)
  [M.T_FOREST]    = 3,     -- speed 6  → 16/6 ≈ 2.7
  [M.T_RUBBLE]    = 8,     -- speed 3  → 16/3 ≈ 5.3 × 1.5 vulnerability weight
  [M.T_SWAMP]     = 8,     -- speed 3  → 16/3 ≈ 5.3 × 1.5 vulnerability weight
  [M.T_CRATER]    = 8,     -- speed 3  → 16/3 ≈ 5.3 × 1.5 vulnerability weight
  [M.T_PILLBOX]   = 2,     -- only passable if dead; cost approximates terrain underneath (grass)
  [M.T_RIVER]     = -1,    -- dynamic: cost depends on ammo (see sq_cost)
  [M.T_DEEPSEA]   = 9999,  -- instant death without a boat
  [M.T_BUILDING]  = 9999,  -- speed 0 (wall)
  [M.T_HALFBUILD] = 9999,  -- speed 0 (wall)
  [M.T_UNKNOWN]   = 3,     -- unexplored: assume passable, cost like forest
}

-- Boat-mode terrain costs.
-- Land tiles are expensive because touching land destroys the boat permanently.
-- A* should strongly prefer staying in water and only disembark near the goal.
M.TERRAIN_COST_BOAT = {
  [M.T_RIVER]     = 2,     -- open water, cheap
  [M.T_DEEPSEA]   = 2,     -- open water, cheap
  [M.T_BOAT]      = 2,     -- transition point
  [M.T_ROAD]      = 20,    -- disembark: lose boat
  [M.T_REFBASE]   = 20,    -- disembark: lose boat
  [M.T_GRASS]     = 20,    -- disembark: lose boat
  [M.T_FOREST]    = 25,    -- disembark into slow terrain, lose boat
  [M.T_RUBBLE]    = 30,    -- disembark into slow terrain, lose boat
  [M.T_SWAMP]     = 30,    -- disembark into slow terrain, lose boat
  [M.T_CRATER]    = 30,    -- disembark into slow terrain, lose boat
  [M.T_PILLBOX]   = 20,    -- disembark: lose boat
  [M.T_BUILDING]  = 9999,
  [M.T_HALFBUILD] = 9999,
  [M.T_UNKNOWN]   = 20,    -- unexplored: assume land, lose boat
}

-- Tank max speed per terrain (from bolo_map.h MAP_SPEED_T* defines).
-- Used to scale pill danger by exposure time: slow terrain = more damage taken.
M.TERRAIN_SPEED = {
  [M.T_ROAD]      = 16,
  [M.T_REFBASE]   = 16,
  [M.T_BOAT]      = 16,
  [M.T_GRASS]     = 12,
  [M.T_FOREST]    = 6,
  [M.T_RUBBLE]    = 3,
  [M.T_SWAMP]     = 3,
  [M.T_CRATER]    = 3,
  [M.T_RIVER]     = 3,
  [M.T_DEEPSEA]   = 3,
  [M.T_PILLBOX]   = 16,  -- dead pill = grass-like
  [M.T_BUILDING]  = 0,
  [M.T_HALFBUILD] = 0,
}

-- Pill danger zone tuning
M.PILL_DANGER_BASE  = 8    -- cost penalty from a calm hostile pill at point-blank
M.PILL_DANGER_ANGER = 200  -- additional penalty when fully angry (anger=1.0, quadratic)
M.PILL_RANGE_MAP    = 9    -- pillbox firing range in MAP squares (1 more than tank shell range)
M.MIN_TREEHIDE_DIST_MAP = 3  -- MIN_TREEHIDE_DIST (768) in map tiles

-- Influence grid tuning (territorial control layer)
M.BASE_INFLUENCE_RADIUS   = 12
M.BASE_INFLUENCE_STRENGTH = 100
M.PILL_INFLUENCE_RADIUS   = 8
M.PILL_INFLUENCE_STRENGTH = 60

-- Pill anger decay: engine takes ~3000 ticks (speed 6->100, +1 every 32 ticks)
M.PILL_ANGER_DECAY = 3000

-- Wall shielding: how much protection walls give against pill fire
M.WALL_HP_FULL     = 5    -- shots to destroy a full wall (T_BUILDING)
M.WALL_HP_HALF     = 2    -- shots to destroy a half wall (T_HALFBUILD)
M.WALL_SHIELD_TIME = 200  -- ticks of wall cover = full shielding (~4 seconds)

-- River cost tuning — high base cost to discourage routing through water.
-- Tank moves very slowly in river, loses ammo, and can get stuck.
-- Only the water-escape builder (under self) makes river viable.
M.RIVER_BASE_COST       = 4    -- single river tile: momentum carries you through (slightly worse than forest)
M.RIVER_CONSECUTIVE_ADD = 12   -- extra cost per tile when parent is also river (speed drains to 3)
M.RIVER_AMMO_FACTOR     = 0.1  -- slight extra cost when carrying ammo (lose some crossing river)

-- LGM brain states (from lgm.h)
M.LGM_INTANK = 0
M.LGM_DEAD   = 1   -- dead / parachuted back in
M.LGM_MOVING = 2   -- out on a mission
M.LGM_BUILD_TIME = 20 -- ticks the LGM spends building at destination

-- Tank capacity constants (from gametype.h)
M.TANK_FULL_ARMOUR = 40
M.TANK_FULL_SHELLS = 40

-- Goal selection thresholds
M.ARMOUR_CRITICAL  = 5    -- flee immediately
M.ARMOUR_LOW       = 15   -- seek resupply
M.SHELLS_LOW       = 20   -- seek resupply (~15 to kill a pill/base)
M.REFUEL_MIN_STOCK = 5    -- skip bases with less than this in observed stock (not worth the trip)
M.REFUEL_OBS_STALE = 500  -- ignore observed stock older than this many ticks (base regenerates)
M.PILLS_MAX_HEALTH = 15   -- fully repaired pill
M.BASE_MIN_ARMOUR_CAPTURE = 0  -- engine reports 1 for all hostile bases (fog of war); 0 means truly dead/capturable

-- Turn penalty: cost per 45° of direction change at each A* node.
-- Accounts for real game-time lost to deceleration, turning, re-acceleration.
-- 90° turn = 2 steps × 2 = 4 cost.  Example: 7 turns × 4 = 28 extra cost
-- makes a winding maze path less attractive than a straight wall-shoot path.
M.TURN_COST_PER_45  = 2

-- Wall-shooting pathfinding
M.WALL_SHOOT_COST   = 30   -- A* cost to path through a wall (5 shots + rubble traverse)
M.SHELL_RESERVE     = 10   -- never plan to shoot walls if it would drop shells below this

-- Road-building pathfinding
-- Terrain types worth building roads on (maps terrain type → tree cost)
M.ROAD_BUILD_TERRAIN = {
  [M.T_RUBBLE]  = 2,   -- LGM_COST_ROAD
  [M.T_SWAMP]   = 2,   -- LGM_COST_ROAD
  [M.T_CRATER]  = 2,   -- LGM_COST_ROAD
  [M.T_RIVER]   = 2,   -- LGM can build 1 tile into river from land edge
}
M.ROAD_BUILD_COST    = 2    -- A* cost for a tile we plan to pave (= road cost, since it will become road)
M.TREE_RESERVE       = 4    -- don't plan road builds if it would drop trees below this
M.ROAD_RIVER_COST    = 2    -- tree cost to build road on river

-- LGM man speeds by terrain type (from bolo_map.h)
-- Used to cap tank speed while LGM is out building
M.MAN_SPEED = {
  [M.T_DEEPSEA]   = 0,
  [M.T_BUILDING]  = 0,
  [M.T_RIVER]     = 0,
  [M.T_SWAMP]     = 4,
  [M.T_CRATER]    = 4,
  [M.T_RUBBLE]    = 4,
  [M.T_FOREST]    = 8,
  [M.T_ROAD]      = 16,
  [M.T_GRASS]     = 16,
  [M.T_BOAT]      = 16,
  [M.T_REFBASE]   = 16,
  [M.T_HALFBUILD] = 0,
  [M.T_PILLBOX]   = 0,
}
M.MAN_SPEED_BLESSED = 16  -- LGM speed on blessed square

-- Refuel base selection: weight for pill danger at the base itself.
-- Higher = more willing to travel further to a safe base.
-- Two calm neutral pills at 3 tiles produce danger ≈ 20.
--   REFUEL: 20 × 20 = 400  → prefer safe base up to 400 path-cost units further
--   FLEE:   20 × 80 = 1600 → at critical armour, cross most maps to reach safety
M.REFUEL_DANGER_WEIGHT  = 40
M.FLEE_DANGER_WEIGHT    = 80
-- Minimum score improvement required to switch from the current refuel/flee
-- base to a different one.  Prevents flip-flopping between two bases that
-- score within noise distance of each other as the tank moves.
M.REFUEL_SWITCH_THRESHOLD = 100
-- When fleeing at critical armour, reject any base with pill danger above this.
-- A calm pill at 2 tiles produces danger ~12; two pills ~24.  Sitting at such a
-- base with near-zero armour means death before the refuel completes.
M.FLEE_DANGER_REJECT = 10
-- Normal refuel: reject bases with danger above this.  Less strict than flee
-- (we have more armour to burn) but still avoids sitting next to hostile pills.
-- A calm pill at 2 tiles = danger ~12, at 3 tiles = ~10.  Two calm pills at
-- 4 tiles = ~15 combined.  Threshold 25 rejects only heavily exposed bases.
M.REFUEL_DANGER_REJECT  = 25

-- Goal lookahead: when within this distance (world units) of a capture goal,
-- pre-compute the next goal.  If the next goal isn't refueling here, steer
-- through the current target toward the next one instead of braking.
-- 3 tiles × 256 WU = 768.
M.LOOKAHEAD_DIST = 768

-- Staleness: penalise goals targeting objects we haven't seen recently.
-- During the opening rush, neutral bases get captured fast.  If we haven't
-- seen a base in STALE_PENALTY_START ticks, add STALE_PENALTY_PER_TICK
-- per tick of staleness to its path cost estimate.  After STALE_SKIP_TICKS
-- without seeing it, skip it entirely (probably already taken).
M.STALE_PENALTY_START    = 100   -- ticks before staleness penalty kicks in (~2 s)
M.STALE_PENALTY_PER_TICK = 0.5   -- cost per tick of staleness beyond the start
M.STALE_SKIP_TICKS       = 500   -- skip object entirely if unseen this long (~10 s)

-- Attack pill tuning
-- Shell travel distance = GUNSIGHT_MAX / 2 = 7 map tiles.  Engage from exactly
-- that range: close enough for shells to hit, far enough that pill shots are
-- harder to land.  Standoff ring is placed at 7 tiles (shell range) not 6.
M.ATTACK_PILL_STANDOFF = 7   -- desired engagement distance from pill (map tiles) = shell range
M.ATTACK_PILL_RANGE    = 7   -- max distance to start shooting (= shell travel at max gunsight)
M.FLEE_PILL_DIST       = 10  -- tiles to flee away from pill when giving up
M.POST_KILL_WAIT_TICKS = 15  -- ticks to hold at standoff after pill dies (in-flight shots clear in ~5)

-- Attack position planner (pick_attack_standoff in goals.lua)
M.ATTACK_PLAN_DIRS          = 12   -- candidate directions sampled around the pill
M.PILL_ATTACK_REPLAN_TICKS  = 150  -- re-evaluate standoff every ~3 s
M.APPROACH_SLOW_IN_RANGE_PEN = 150 -- per-tile penalty for slow terrain (swamp/rubble/crater) within pill range on approach

-- Attack substates (approach → engage → reposition)
M.ATTACK_ENGAGE_RADIUS      = 1    -- mdist from standoff to enter engage substate (tight: standoff = shell range)
M.ATTACK_REPOSITION_RADIUS  = 3    -- mdist from standoff that triggers reposition
M.ATTACK_MIN_ENGAGE_TICKS   = 10   -- minimum ticks in engage before allowing reposition (anti-oscillation)

-- Flee-from-engage: John endures ~45 ticks of pill return fire before retreating.
-- A calm pill (anger=0) takes ~1 second to start returning fire; an angry pill
-- fires immediately.  The brain should flee earlier when the pill is already angry.
M.ENGAGE_MAX_INCOMING_TICKS = 45   -- max ticks under fire before fleeing (calm pill baseline)
M.ENGAGE_ANGRY_FLEE_FACTOR  = 0.5  -- at anger=1.0, flee after 50% of normal incoming time

-- Wall-shield attack tactic
-- Build a wall 1 tile from the pill to absorb return fire.  Tank stands at
-- max gunsight range (~7 tiles) from the pill, offset 25° from the wall line
-- so shells travel at an angle that clears the wall tile.  The wall is adjacent
-- to the pill on the side facing the tank, catching most pill return fire.
-- Prebuild position (9 tiles) is outside the pill's 8-tile firing range.
M.WALL_SHIELD_ENABLED      = true   -- enable wall-shield tactic
M.WALL_SHIELD_MIN_TREES    = 4      -- minimum trees to attempt wall-shield
M.WALL_SHIELD_STANDOFF     = 7      -- desired tank distance from pill (map tiles) = max gunsight range
M.WALL_SHIELD_WALL_DIST    = 1      -- wall placement distance from pill (map tiles)
M.WALL_SHIELD_BUILD_COST   = 2      -- trees consumed per wall build
M.WALL_SHIELD_STANDOFF_ANGLE_OFFSET = 10 -- degrees offset from wall line so shells clear the wall (10° at 7 tiles = ~1.2 tile offset, enough to miss 1-tile wall)
M.WALL_SHIELD_PREBUILD_STANDOFF = 9  -- prebuild position distance from pill (outside 8-tile range)
M.WALL_SHIELD_RETREAT_ANGLE    = 90  -- degrees perpendicular to pill->wall line for retreat
M.WALL_SHIELD_LGM_SAFE_TICKS  = 10  -- min ticks after LGM returns before shooting
M.WALL_SHIELD_LGM_TRIP_WEIGHT = 0.5 -- weight for LGM round-trip ticks in wall candidate scoring
M.WALL_SHIELD_LGM_MAX_TICKS   = 2000 -- max ticks to simulate LGM travel
M.WALL_SHIELD_LGM_STUCK_TICKS = 150  -- same-tile timeout for LGM simulation (~3 seconds)

-- Base shield: build a wall between the base and a hostile pill while refueling.
-- Only when pill is calm (anger low enough that LGM can build before shots arrive)
-- and far enough that the LGM has time to get out and back.
M.BASE_SHIELD_MAX_ANGER = 0.3   -- pill anger must be below this
M.BASE_SHIELD_MIN_DIST  = 3     -- pill must be 3+ tiles away
M.BASE_SHIELD_BUILD_COST = 2    -- trees consumed to build the wall

-- Basic pill capture (bpc) — circle-strafe attack inspired by human play.
-- Basic pill capture — "hardline" technique (from ErYan's replay):
-- Drive to standoff, park, face pill, pump shells.  After a few hits from
-- return fire, hard-turn away — the pill's predictive aim overshoots the
-- curve.  Retreat to base, refuel, return to finish.
M.BPC_STANDOFF         = 7    -- standoff distance (map tiles); ErYan parked ~7 tiles out
M.BPC_RANGE            = 7    -- max distance to start shooting (shell travel at max gunsight)
M.BPC_RUSH_ARRIVE      = 1    -- mdist to pill to count as "arrived" during rush
M.BPC_CURVE_AFTER_HITS = 3    -- hits taken before curving away (ErYan curved after 2-3)
M.BPC_CURVE_TICKS      = 30   -- ticks of curving before disengage (~90° turn)

-- Pill placement attack tactic
-- Place a friendly pill 1-5 tiles from a hostile pill.  The placed pill
-- auto-fires at nearby hostiles, creating a pill-vs-pill attrition war.
-- Tank adds DPS with its own shells to tip the balance.
-- From ErYan's replay: used for 12 of 14 pill captures (86%).
M.PILL_PLACE_DIST       = 2     -- ideal placement distance from target (tiles)
M.PILL_PLACE_DIST_MIN   = 1     -- minimum placement distance
M.PILL_PLACE_DIST_MAX   = 5     -- maximum placement distance
M.PILL_ENGAGE_DIST      = 6     -- tank shooting distance during pill-vs-pill engagement
M.PILL_CHAIN_MIN_HP     = 5     -- don't place a pill with less than this HP (dies too fast)
M.PILL_VS_MULTI_PENALTY = 1.5   -- expected attrition multiplier vs 2+ hostiles
M.PILL_PLACE_TREE_COST  = 4     -- trees consumed to place a pill (LGM_COST_PILLNEW)
M.PILL_PLACE_TIMEOUT    = 400   -- ticks to wait for LGM to place pill before giving up
M.PILL_PLACE_ENGAGE_AIM = 4     -- aim correction threshold for firing during engage

-- Gunsight
M.GUNSIGHT_MAX = 14   -- max sightLen (in half-map-squares); shells travel sightLen/2 map tiles

-- Dynamic flee threshold (engage substate)
M.FLEE_ESCAPE_COST_DIVISOR  = 50   -- escape path cost units per +1 armour flee buffer
M.FLEE_SLOW_TERRAIN_BONUS   = 3    -- extra armour buffer when standing on slow terrain

-- Cost-based goal selection (unified scoring — all goals compete on same scale)
M.GOAL_SWITCH_PENALTY      = 30    -- base cost added when switching to a different goal group
M.GOAL_TARGET_SWITCH_PENALTY = 15  -- cost added when same group but different target
M.GOAL_COMMITMENT_PER_TICK = 0.5   -- extra switch penalty per tick spent on current goal
M.GOAL_COMMITMENT_CAP      = 75    -- max commitment penalty (reached after 150 ticks / 3s)
M.WALL_SHIELD_COMMITMENT   = 200   -- extra switch penalty when wall-shield attack is in progress
M.REFUEL_URGENCY_MIN       = 0.15  -- minimum urgency multiplier for refuel cost
M.PILL_HEALTH_WEIGHT       = 5     -- cost per HP of hostile pill (full 15HP pill = +75)
M.REPAIR_DAMAGE_BONUS      = 3     -- cost reduction per missing HP on friendly pill
M.ATTACK_BASE_EXTRA_COST   = 80    -- flat cost added to hostile base attacks
M.ATTACK_BASE_THREAT_WEIGHT = 3    -- multiplier for threat at base location (penalise bases behind enemy pills/tanks)
M.GOAL_CROSSFIRE_PENALTY   = 100   -- goal cost per nearby hostile pill that can crossfire at standoff
M.EXPLORE_BASE_COST        = 500   -- base cost for exploration fallback

-- Exploration
M.MIN_EXPLORE_DIST = 3  -- don't target frontier squares within this range

-- Builder / tree management
-- FARM_GATHER_RADIUS: how many tiles off-path to search for forest when in
--   "gather" mode and below need_trees.  Larger = more willing to detour.
-- FARM_OPPORTUNISTIC_RADIUS: radius for free on-path grabs (opportunistic/gather-stocked).
--   Keep small — every tile of LGM travel triggers steering pacing slowdown.
--   At radius 2, LGM walk time ≈ 64 ticks; tank paced to ~6 WU/tick (70% of forest speed 8).
-- TREE_OPPORTUNISTIC_MAX: only opportunistically farm below this level.
--   No sense over-farming when the tank has no specific tree-consuming plan.
-- LGM_DEPLOY_DIST: max map-tile distance from tank to dispatch LGM for a farm/build.
--   Beyond this the pacing slowdown outweighs the benefit.
-- PILL_REPAIR_COST: trees consumed per HP when repairing a pill (1:1 in Bolo).
M.FARM_GATHER_RADIUS       = 3
M.FARM_OPPORTUNISTIC_RADIUS = 2
M.TREE_OPPORTUNISTIC_MAX   = 20
M.LGM_DEPLOY_DIST          = 3
M.PILL_REPAIR_COST         = 1

-- -------------------------------------------------------------------------
-- Shell trajectory prediction
-- -------------------------------------------------------------------------
-- bsin/bcos return integers in [-128,128] representing unit-vector components.
-- SHELL_SPEED is the simulation step size in raw world units BEFORE the /128
-- scaling.  Per step, the shell advances bsin(dir)*SHELL_SPEED/128 WU in X
-- and -bcos(dir)*SHELL_SPEED/128 WU in Y.  With SHELL_SPEED=32:
--   step size ≈ 32 WU (1/8 of a map tile), 64 steps → 2048 WU = 8 map tiles.
M.SHELL_SPEED            = 32   -- simulation step size (WU, before /128 bsin scale)
M.SHELL_MAX_STEPS        = 64   -- max trajectory steps  (64 × 32/128 × max-bsin = 8 tiles)
M.DANGER_SHELL_IMPACT    = 100  -- danger value for every cell on a predicted shell path
M.DANGER_DECAY_TICKS_SHELL = 5  -- ticks before a shell-impact zone expires

-- LGM build-safety thresholds (compared against danger.danger_at value)
-- danger_at = DANGER_SHELL_IMPACT(100) if a shell passes through + pill_danger (0..~95)
M.LGM_DANGER_LOW  = 0    -- normal road/farm: any danger (shell or mild pill) aborts dispatch
M.LGM_DANGER_MED  = 20   -- repair pill: mild pill danger is acceptable
M.LGM_DANGER_HIGH = 80   -- emergency wall / refuel: only heavy fire (angry pill or shell) aborts

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: enemy intercept (TTK vs TTI)
-- -------------------------------------------------------------------------
M.INTERCEPT_PENALTY        = 120   -- cost added when enemy tank may arrive before we finish
M.INTERCEPT_SAFETY_MARGIN  = 0.8   -- TTK must be < TTI × this to avoid penalty
M.TTK_TICKS_PER_HIT        = 8     -- conservative: ticks per effective shell hit on pill
M.INTERCEPT_MAX_RANGE      = 20    -- only consider enemy tanks within this range of pill

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: pill anger cooldown
-- -------------------------------------------------------------------------
M.ANGER_ATTACK_THRESHOLD   = 0.5   -- anger level above which we prefer to wait
M.ANGER_WAIT_MAX           = 500   -- max ticks we're willing to wait for cooldown (~10s)
M.ANGER_COST_PER_TICK      = 0.3   -- cost per tick of remaining anger cooldown

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: armour drain projection
-- -------------------------------------------------------------------------
M.DRAIN_PROJECTION_MIN_TICKS = 20  -- min ticks under fire before projecting
M.DRAIN_ARMOUR_MARGIN      = 3     -- extra armour buffer above flee threshold

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: contested base avoidance
-- -------------------------------------------------------------------------
M.CONTESTED_BASE_PENALTY   = 120   -- cost added to bases with approaching enemy
M.CONTESTED_BASE_RANGE     = 15    -- enemy must be within this range of base (tiles)
M.CONTESTED_BASE_HEADING   = 32    -- heading tolerance (bolo angle units, ~45°)

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: angry pill at refuel base
-- -------------------------------------------------------------------------
M.ANGRY_REFUEL_THRESHOLD   = 0.6   -- pill anger above which we flee the base

-- -------------------------------------------------------------------------
-- Goal replan scheduling
-- -------------------------------------------------------------------------
M.GOAL_REPLAN_INTERVAL     = 50    -- ticks between goal decisions
M.GOAL_POOL_COUNT          = 10    -- number of pool evaluators to spread across ticks
M.GOAL_CANDS_PER_TICK      = 2     -- A* cost_to evaluations per tick (round-robin)

-- Strategic pill placement (idle deployment near front line / friendly base)
M.STRATEGIC_PLACE_ENABLED       = true
M.STRATEGIC_PLACE_SEARCH_RADIUS = 8     -- tiles around midpoint to search
M.STRATEGIC_PLACE_FRONT_WEIGHT  = 3.0   -- bonus per tile of proximity to front line
M.STRATEGIC_PLACE_BASE_WEIGHT   = 2.0   -- bonus per tile of proximity to nearest friendly base
M.STRATEGIC_PLACE_THREAT_WEIGHT = 1.5   -- penalty per unit of threat.at(pos)
M.STRATEGIC_PLACE_PILL_SPACING  = 4     -- minimum tile distance from existing friendly pills
M.STRATEGIC_PLACE_PILL_PENALTY  = 50    -- penalty for being within PILL_SPACING of existing pill
M.STRATEGIC_PLACE_LOS_WEIGHT    = 0.5   -- bonus per tile of LOS coverage
M.STRATEGIC_PLACE_LOS_DIRS      = 8     -- number of directions to sample for LOS
M.STRATEGIC_PLACE_LOS_MAX_RANGE = 8     -- max tiles to trace per LOS ray
M.STRATEGIC_PLACE_BASE_COST     = 60    -- base cost so it loses to attack/capture but beats explore
M.STRATEGIC_PLACE_MAX_BASE_DIST       = 15    -- don't place pills too far from bases
M.STRATEGIC_PLACE_DEFENSE_RADIUS      = 8     -- radius to count defending pills
M.STRATEGIC_PLACE_UNDERDEFENDED_BONUS = 50    -- bonus per missing defender (target: 2)
M.STRATEGIC_PLACE_BEYOND_FRONT_PENALTY = 100  -- penalty for placing in enemy territory
M.STRATEGIC_PLACE_FRONT_PROX_CAP      = 80    -- influence cap for front proximity bonus
M.STRATEGIC_PLACE_FRONT_PROX_WEIGHT   = 0.5   -- weight for front proximity score
M.STRATEGIC_PLACE_SPACING_BONUS       = 15    -- bonus for 2-4 tile spacing
M.STRATEGIC_PLACE_OFFENSIVE_THRESHOLD = 0.6   -- strength ratio to switch to offensive
M.STRATEGIC_PLACE_SPIKE_BONUS         = 80    -- bonus for placing adjacent to hostile base

-- Tank combat
M.TANK_COMBAT_ENABLED           = true
M.TANK_COMBAT_MIN_SHELLS        = 10    -- don't engage with fewer shells
M.TANK_COMBAT_MIN_ARMOUR        = 10    -- don't engage with less armour
M.TANK_COMBAT_MAX_RANGE         = 15    -- only consider tanks within this many tiles
M.TANK_COMBAT_AIM_BONUS         = 40    -- cost reduction if already aimed near target
M.TANK_COMBAT_AIM_THRESHOLD     = 20    -- bolo angle units (~28°) for aim bonus
M.TANK_COMBAT_ENGAGE_RANGE      = 7     -- tiles: close enough to shoot (= shell range)
M.TANK_COMBAT_OPTIMAL_DIST      = 5     -- tiles: ideal engagement distance
M.TANK_COMBAT_TOO_CLOSE         = 2     -- tiles: back off if closer than this
M.TANK_COMBAT_FLEE_ARMOUR       = 6     -- disengage if armour drops to this
M.TANK_COMBAT_FLEE_SHELLS       = 5     -- disengage if shells drop to this
M.TANK_COMBAT_NEAR_PILL_PENALTY = 80    -- cost penalty if enemy is near a hostile pill (crossfire)
M.TANK_COMBAT_NEAR_PILL_RANGE   = 5     -- tiles: how close to hostile pill counts
M.TANK_COMBAT_BASE_COST         = 30    -- base cost so pills/captures usually win over tank hunting
M.TANK_COMBAT_SHELL_SPEED       = 32    -- WU per sim step (for lead-target calc)
M.TANK_COMBAT_SHELL_STEPS_PER_TILE = 8  -- steps for shell to cross 1 tile (256/32)
M.TANK_COMBAT_JINK_PERIOD       = 10    -- ticks between jink direction changes
M.TANK_COMBAT_JINK_ANGLE        = 32    -- bolo angle offset for lateral jink (~45°)
M.TANK_COMBAT_OPPORTUNISTIC_RANGE = 4   -- tiles: fire at enemy if already aimed near them
M.TANK_COMBAT_OPPORTUNISTIC_AIM = 8     -- bolo angle units (~11°) aim tolerance for opportunistic shot

-- Anti-tank opportunistic pill drop
M.ANTITANK_DROP_ENABLED         = true
M.ANTITANK_DROP_RANGE           = 12    -- enemy tank must be within this many tiles
M.ANTITANK_DROP_COOLDOWN        = 200   -- ticks between opportunistic drops

-- Defensive trail dropping
M.TRAIL_DROP_ENABLED            = true
M.TRAIL_DROP_MIN_PILLS          = 2     -- must carry at least this many pills
M.TRAIL_DROP_NO_PILL_RADIUS     = 6     -- no friendly pill within this range
M.TRAIL_DROP_BEHIND_DIST        = 2     -- tiles behind current heading
M.TRAIL_DROP_COOLDOWN           = 300   -- ticks between trail drops
M.TRAIL_DROP_MIN_SPEED          = 4     -- tank must be moving at this speed

-- Defend pill response
M.PILL_ATTACK_COOLDOWN       = 200   -- ticks before clearing under_attack flag (~4 sec)
M.DEFEND_PILL_MIN_DAMAGE     = 3     -- minimum HP lost before triggering defense
M.DEFEND_PILL_BASE_COST      = 30    -- base cost for defend goal
M.DEFEND_PILL_URGENCY_WEIGHT = 5     -- cost reduction per damage point above threshold
M.DEFEND_PILL_MAX_TRAVEL     = 150   -- don't defend pills too far away (would arrive too late)

-- Strategy / game phase detection
M.OPENING_MIN_TICKS       = 500    -- ~10 seconds minimum opening phase
M.PHASE_HYSTERESIS_TICKS  = 100    -- ~2 seconds of consistent signal before switching
M.ENDGAME_PILL_RATIO      = 0.70   -- >70% of pills = endgame
M.ENDGAME_BASE_RATIO      = 0.80   -- >80% of bases = endgame
M.FRONT_LINE_INTERVAL     = 50     -- recompute front line every ~1 second

-- Phase-dependent goal cost multipliers.
-- < 1.0 = cheaper (more attractive), > 1.0 = more expensive (less attractive).
-- Keys must match the pool_name strings used in goals.lua.
M.PHASE_WEIGHTS = {
  opening = {
    capture_base     = 0.3,
    capture_pill     = 0.5,
    repair_pill      = 2.0,
    attack_pill      = 1.5,
    attack_base      = 2.0,
    place_strategic  = 3.0,
    defend_pill      = 2.0,
  },
  middle = {
    capture_base     = 1.0,
    capture_pill     = 1.0,
    repair_pill      = 0.8,
    attack_pill      = 0.8,
    attack_base      = 1.2,
    place_strategic  = 0.7,
    defend_pill      = 0.5,
  },
  endgame_winning = {
    capture_base     = 0.5,
    capture_pill     = 0.5,
    repair_pill      = 1.5,
    attack_pill      = 0.4,
    attack_base      = 0.5,
    place_strategic  = 1.0,
    defend_pill      = 1.5,
  },
  endgame_losing = {
    capture_base     = 1.5,
    capture_pill     = 0.8,
    repair_pill      = 0.4,
    attack_pill      = 2.0,
    attack_base      = 3.0,
    place_strategic  = 0.5,
    defend_pill      = 0.3,
  },
}

-- Forward world simulation (cworldsim)
M.WSIM_ENABLED             = true   -- enable forward sim for goal evaluation
M.WSIM_MAX_TICKS           = 300    -- max ticks to simulate per path
M.WSIM_DAMAGE_COST_WEIGHT  = 10     -- cost per point of predicted armor damage
M.WSIM_KILL_REJECT         = true   -- hard-reject goals where sim predicts death
M.WSIM_LGM_DEATH_PENALTY   = 200    -- extra cost if sim predicts LGM will die

return M
