-- =========================================================================
-- GoalHunter/constants.lua — terrain costs, tuning parameters
-- =========================================================================

local M = {}

M.BRAIN_NAME = "GoalHunter"
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
  [M.T_RIVER]     = 2.5,   -- afloat: between grass (2) and forest (3)
  [M.T_DEEPSEA]   = 2.5,   -- afloat: between grass (2) and forest (3)
  [M.T_BOAT]      = 2.5,   -- transition point (matches afloat cost)
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
-- Radial falloff at the edge of PILL_RANGE_MAP. 0.5 = 50% drop, so the
-- rim reads as 50% of point-blank danger. The whole disk is in the
-- pill's actual fire range; the earlier 1-d/(R+1) curve dropped to
-- ~9% at the rim (boundary tiles read as nearly safe), the prior 0.25
-- value of this constant left it too aggressive at 75%.
M.PILL_DANGER_EDGE_FALLOFF = 0.5
M.PILL_RANGE_MAP    = 9    -- danger stamp radius (1 beyond actual fire range of 8)
M.PILL_FIRE_RANGE   = 8    -- actual pillbox firing range: PILLBOX_RANGE(2048) / 256 = 8 tiles
-- Front classification is ASYMMETRIC by purpose:
--   IDENTIFYING existing pills (role_of / counts / reposition eligibility)
--     uses FRONT_NEAR_RADIUS — a pill hugging the line still does front duty.
--   PLACING (what category a candidate TILE would fill: the strategic scan,
--     its heatmap, trail-drop, building-intent viz) uses the strict
--     FRONT_NEAR_RADIUS_PLACE=0 — a new pill only counts as filling the
--     front role if it stands ON a front-band tile (the yellow-circle "3"
--     overlay). The 2-tile halo let a repositioned pill's OLD spot pass as
--     a "front" cell and dodge the back-surplus skip → rebuild-in-place.
M.FRONT_NEAR_RADIUS       = 2  -- identify: existing pill counts front within this of the band
M.FRONT_NEAR_RADIUS_PLACE = 0  -- place: candidate tile must be ON the band to fill "front"
M.MIN_TREEHIDE_DIST_MAP = 3  -- MIN_TREEHIDE_DIST (768) in map tiles
M.CROSSFIRE_MULTIPLIER_ENABLED = false  -- multiply danger by number of pills covering each tile

-- Influence grid tuning (territorial control layer)
M.BASE_INFLUENCE_RADIUS   = 12
M.BASE_INFLUENCE_STRENGTH = 100
M.PILL_INFLUENCE_RADIUS   = 8
M.PILL_INFLUENCE_STRENGTH = 60

-- Pill anger decay: engine takes ~3000 ticks (speed 6->100, +1 every 32 ticks)
M.PILL_ANGER_DECAY = 3000
-- Per-hit anger increment. 1/3 means three hits saturate to fully angry.
M.PILL_ANGER_BUMP  = 0.3333

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
M.ARMOUR_MODERATE  = 25   -- conditionally force PPT when standoff is hot
M.SHELLS_LOW       = 20   -- seek resupply (~15 to kill a pill/base)

-- Ammo-deprivation: if a tank sits below AMMO_DEPRIVED_SHELLS for this long
-- during normal (non-opening) play it's flagged state.ammo_deprived — a lost
-- cause for resupply, so it goes all-in (joins any blitz, suicide-charges the
-- pill instead of holding at standoff). Cleared the moment shells recover to
-- AMMO_DEPRIVED_SHELLS. 50 ticks/sec, so 6000 = 120 s.
M.AMMO_DEPRIVED_SHELLS = M.SHELLS_LOW   -- "min ammo" line for deprivation
M.AMMO_DEPRIVED_TICKS  = 6000           -- 120 s continuously below it

-- PPT-force thresholds. PPT (Protected Pill Take) is normally only
-- chosen for high-HP pills (>= PPT_HEALTH_THRESHOLD). These knobs let
-- low-armour situations force PPT even on a soft pill, because the
-- bot can't afford to take return fire while charging:
--   - armour <= ARMOUR_LOW: always force PPT regardless of standoff
--     danger (we're one or two hits from flee territory).
--   - armour <= ARMOUR_MODERATE AND standoff danger >= ARMOUR_MOD_PPT_DANGER:
--     mid-armour and the chosen standoff is hot — too risky to charge
--     unshielded even on a low-HP pill.
M.ARMOUR_MOD_PPT_DANGER = 75
M.ARMOUR_COMBAT    = 30   -- seek resupply if next goal is attack_pill
M.SHELLS_COMBAT    = 30   -- seek resupply if next goal is attack_pill
M.ARMOUR_PER_PILL_HP = 2  -- estimated armour lost per pill HP when attacking
-- Hard "don't take a healthy pill on low armour" gate.  Applied as a
-- REJECT in the attack_pill eval pool and as an immediate abort at
-- the start of approach / build_walls / charge.  Heuristic: a near-
-- full-HP pill (>= UNSAFE_HP) deals more damage than we can absorb
-- with < UNSAFE_ARMOUR_FLOOR plating, so refusing the take is better
-- than dying mid-charge.
M.ATTACK_PILL_UNSAFE_HP_THRESHOLD  = 13
M.ATTACK_PILL_UNSAFE_ARMOUR_FLOOR  = 20
M.ATTACK_PILL_RISKY_ARMOUR         = 30   -- not "unsafe" (that's the 20 floor) but RISKY: below this, a SOLO (non-2+-tank-blitz) attack_pill gets +ATTACK_PILL_RISKY_PENALTY so the bot leans toward safer goals unless an ally shares the fire
M.ATTACK_PILL_RISKY_PENALTY        = 100  -- flat cost added per the above
-- "danger_nearby" penalty: when we abort/swerve out of a pill take
-- because an enemy LGM is within PILL_DANGER_NEARBY_RADIUS of the
-- target, stamp the pill_id for PILL_DANGER_NEARBY_TICKS so its
-- attack_pill (pool 6) cost is multiplied by PILL_DANGER_NEARBY_MULT
-- and we don't bounce right back onto it.  50 ticks/s, so 1500 ≈ 30s.
M.PILL_DANGER_NEARBY_RADIUS = 3       -- 7x7 grid (radius 3) centered on pill
M.PILL_DANGER_NEARBY_TICKS  = 1500    -- ~30 s of cooldown
M.PILL_DANGER_NEARBY_MULT   = 1.5     -- 1.5x cost while stamp is active
-- Ally-avoid overlay cost — stamped on the 5x5 around an allied tank
-- doing a pill take (and the firing lane to the pill).  Set to roughly
-- half of STUCK_PENALTY (1500) so A* visibly routes around it without
-- treating it as a hard wall — and well above the tiny noise the prior
-- value (10) added.
M.ALLY_AVOID_COST           = 800
-- Blitz participant's EXACT tile: a converging blitzer marks the single tile a
-- fellow blitz tank currently occupies as effectively impassable so others route
-- around it (not the soft 800 used for the solo 5x5). Massive but sub-32767 so a
-- single occupied tile reroutes without hard-walling a tight cluster's approach.
M.ALLY_BLITZ_TILE_COST      = 12000
-- Penalty added to an ally tank's tile when the Dijkstra route tracer dodges it
-- at trace time (cpf.path_to obstacles). Large so an occupied tile is a last
-- resort, but finite so a fully-boxed tank still finds a step.
M.NAV_AVOID_PENALTY         = 1000000
-- Ally-pill-take priority window: when an ally is broadcasting
-- attack_pill on a target, REJECT our capture_pill candidate for the
-- same pill for this many ticks (~2 s @ 50Hz). Gives them first dibs
-- at the dead pill they killed instead of us swooping in.  Refreshes
-- every tick the ally is still on attack_pill, so it decays naturally
-- once they switch to capture_pill themselves.
M.ALLY_PILL_TAKE_PRIORITY_TICKS = 100
-- Known-world (/info kw) resync: min ticks between honoring ally /info kwq
-- queries, so a cluster of (re)spawns can't make us re-dump our whole known
-- world every tick. The dump itself drains a few objects per idle message slot.
M.KW_RESYNC_COOLDOWN = 150
M.REFUEL_MIN_STOCK = 5    -- skip bases with less than this in observed stock (not worth the trip)
-- Dynamic refuel targets (state.shell_target / state.armour_target).
-- Must stay above SHELLS_LOW (20) or offense pools (eval_attack_pill /
-- attack_tank) refuse to fire and the bot leaves base unable to fight.
M.REFUEL_BASELINE_SHELLS         = 25   -- minimum shell target, above offense gates
M.REFUEL_PER_ENEMY_TANK          = 6    -- shells budgeted per nearby hostile tank
M.REFUEL_ENEMY_TANK_RANGE        = 40   -- tiles: hostile tank counts toward combat target
M.REFUEL_MAX_ENEMY_TANKS_COUNTED = 2    -- cap on tanks counted in combat target
M.REFUEL_DEPLETION_PENALTY = 80   -- max penalty for a base that can't get us above LOW thresholds
M.REFUEL_OBS_STALE = 500  -- ignore observed stock older than this many ticks (base regenerates)
M.REFUEL_DEPLETED_COOLDOWN = 500  -- when a COMMITTED refuel goal is abandoned because its base went depleted, block that base tile for this many ticks (~10s at 50Hz) so finalize can't re-pick it the moment the depleted observation ages out (prevents the drive-there / observe-depleted / leave flip-flop). Re-arms on each abandon.
-- When true, suppress goal-replan while traveling to or topping up at a
-- refuel base. Prevents thrash where the goal scorer flips back to
-- attack/capture mid-refuel. Disable to test how the bot behaves with
-- normal replanning during refuel.
M.REFUEL_LOCK_IN = false
-- Wait-for-ally on refuel base. If we close to within REFUEL_ALLY_WAIT_DIST
-- (chebyshev tiles) of our target base and an ally tank is currently
-- standing on the base tile, enter the `wait_for_ally` substate and brake
-- until they leave or REFUEL_ALLY_WAIT_TICKS elapses (then we blocklist
-- the base and replan).  Keeps two bots from piling onto one base.
M.REFUEL_ALLY_WAIT_DIST  = 2     -- tiles
M.REFUEL_ALLY_WAIT_TICKS = 500   -- ~10 s @ 50 Hz
M.PILLS_MAX_HEALTH = 15   -- fully repaired pill
M.BASE_MIN_ARMOUR_CAPTURE = 0  -- engine reports 1 for all hostile bases (fog of war); 0 means truly dead/capturable

-- Turn penalty: cost per 45° of direction change at each A* node.
-- Accounts for real game-time lost to deceleration, turning, re-acceleration.
-- 90° turn = 2 steps × 2 = 4 cost.  Example: 7 turns × 4 = 28 extra cost
-- makes a winding maze path less attractive than a straight wall-shoot path.
M.TURN_COST_PER_45  = 2

-- Wall-shooting pathfinding
M.WALL_SHOOT_COST   = 30   -- A* cost to path through a wall (5 shots + rubble traverse)
M.SHELL_RESERVE     = 0    -- let tank use all shells to break walls

-- Road-building pathfinding
-- Terrain types worth building roads on (maps terrain type → tree cost)
M.ROAD_BUILD_TERRAIN = {
  [M.T_RUBBLE]  = 2,   -- LGM_COST_ROAD
  [M.T_SWAMP]   = 2,   -- LGM_COST_ROAD
  [M.T_CRATER]  = 2,   -- LGM_COST_ROAD
  [M.T_RIVER]   = 2,   -- LGM can build 1 tile into river from land edge
}
M.ROAD_BUILD_COST    = 2    -- A* cost for a tile we plan to pave (= road cost, since it will become road)
M.TREE_RESERVE       = 4    -- BASE road-build reserve. Non-emergency roads use builder.road_tree_reserve(): this base + PILL_PLACE_TREE_COST per carried pill (uncapped GUARANTEE — always keep enough wood to deploy every carried pill) + ceil(worst nearby friendly pill deficit / PILL_REPAIR_AMOUNT) repair trees (≤4). Roads only get built from genuine surplus. (The drowning-emergency road bypasses all reserves.)
M.ROAD_RESERVE_REPAIR_RADIUS = 20  -- friendly damaged pill within this many tiles adds its repair cost to the road reserve
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
M.REFUEL_DANGER_WEIGHT  = 20
-- Danger/threat-term multiplier applied across goal cost formulas
-- when state.cautious_mode is true (see init.lua).  Cautious mode
-- triggers on conditions like "LGM dead AND carrying pills" — we
-- can't afford to lose what we're carrying, so the danger terms
-- in cost formulas get pumped to bias hard toward safer routes /
-- targets.
M.CAUTIOUS_MODE_MULT = 5
-- PENALTY applied to refuel_at_base cost when the base's tile danger value is
-- > 0 (an EXPOSED refuel spot). Multiplies the final cost so refueling out in
-- the open is less attractive; a truly safe base (danger 0) keeps its raw cost
-- (no discount). Was a 0.75 safe-DISCOUNT, which made refuel too attractive
-- overall — flipped to a 1/0.75 unsafe-penalty (same safe:unsafe ratio, higher
-- absolute cost). ~1.33.
M.REFUEL_DANGER_PENALTY = 1 / 0.75
M.FLEE_DANGER_WEIGHT    = 80
-- Minimum score improvement required to switch from the current refuel/flee
-- base to a different one.  Prevents flip-flopping between two bases that
-- score within noise distance of each other as the tank moves.
-- Set to 30 to MATCH the generic GOAL_SWITCH_PENALTY: the refuel-specific
-- -REFUEL_SWITCH_THRESHOLD cost was removed (it double-damped the same switch,
-- see goals.lua), so this value now only labels the desc as `hyst{-N}`. Keeping
-- it at the real switch penalty (30) makes that label reflect the hysteresis
-- refuel actually pays via goal_selection, instead of overstating it at 100.
M.REFUEL_SWITCH_THRESHOLD = 30
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
M.ATTACK_PILL_STANDOFF = 7.4  -- desired engagement distance from pill (max shell range)
M.ATTACK_PILL_STANDOFF_CHARGE = 7.0  -- non-PPT charge pulls the engage spot in to here on the first charge tick (PPT keeps its shielded standoff)
M.ATTACK_PILL_RANGE    = 9.5  -- max distance to start shooting
M.ATTACK_PILL_MIN_ARMOUR = 1  -- minimum armour to attempt pill take

-- "Finish what you started" bias. When state.wounded_pill is set AND
-- that pill's current HP is at or below WOUNDED_FINISH_THRESHOLD, the
-- attack-pill cost evaluator scales the cost of OTHER pill takes by
-- up to WOUNDED_FINISH_OTHER_PENALTY. The penalty fades linearly with
-- ticks since wounded_pill was set (matches the 500-tick wounded
-- expiry in init.lua) and scales with how close the wounded pill is
-- to dead (1 HP gets the full penalty, threshold HP gets none). Self-
-- defense (attack_tank, flee) is unaffected — only sibling pill
-- takes get penalized so an enemy tank rush still wins priority.
M.WOUNDED_FINISH_THRESHOLD     = 10
M.WOUNDED_FINISH_DECAY_TICKS   = 15000  -- 5 min @ 50Hz
M.WOUNDED_FINISH_OTHER_PENALTY = 3.0  -- max cost multiplier on other pills

-- Cross-goal commit discount. Stacks with the existing 0.3x in-pool
-- pill discount: when the wounded pill is in the active "finish_other"
-- window, ALSO multiply the wounded-pill take's cost by this factor.
-- The 0.3x already nudges it past sibling pills; this further tilts
-- it past unrelated goals (capture_base, refuel, attack_tank when
-- not urgent, etc.). Decays alongside finish_other via the same
-- time_factor — fully active at age=0 (×0.5), back to ×1.0 once
-- WOUNDED_FINISH_DECAY_TICKS expires.
M.WOUNDED_COMMIT_DISCOUNT      = 0.5

-- Protected pill take (PPT). When the target pill's health is at least
-- PPT_HEALTH_THRESHOLD, the bot enters PPT mode: shorter standoff
-- (PPT_STANDOFF), and a slower / more precise charge so the carefully
-- chosen wall-shielded angle is preserved instead of overshooting it.
M.PPT_HEALTH_THRESHOLD = 8     -- only PPT if pill HP >= this
M.PPT_ANGER_THRESHOLD  = 0.34  -- ...OR force PPT when pill anger > this (~>1 hit): an angry pill reloads fast, don't charge it bare
M.SWERVE_SKIP_ARMOUR_PER_HP = 5  -- skip the kill/curve swerve while pill.health*this <= armour (we can tank finishing it, so buck in)
M.TANK_FINISH_MAX_HP    = 3    -- tank the finish only when remaining pill HP <= this
M.TANK_FINISH_MAX_ANGER = 0.25 -- ...AND the pill is currently CALM (anger <= this). An angrier pill reloads fast, so don't soak its last HP — swerve instead.
M.PPT_STANDOFF         = 7.0   -- pull in slightly closer than ATTACK_PILL_STANDOFF
M.PPT_CHARGE_MAX_SPEED = 4     -- speed cap during PPT charge (creep, not rush)
-- Cautious "boat in a river" cruise when on/stepping onto an ally's pill-take
-- ring (init.lua). info.speed is raw engine×4 (0..64; 16 max → 64). 28 ≈ engine
-- 7 (~44% of max): steady per-tile progress, not the spd=4 dead-creep that PPT
-- uses. Decelerate only when ABOVE this; below it, keep steering's drive.
M.TAKE_CRAWL_MAX_SPEED = 28
-- Hard-brake override for the crawl: if we're within APPROACH_HARDBRAKE_DIST wu
-- of our OWN attack_pill stop point (approach point / in-range standoff) and
-- still moving faster than APPROACH_HARDBRAKE_SPEED, force a full brake even on
-- an ally take ring, so the crawl-cruise can't coast us through our own setup.
-- 128 wu = 1/2 tile; speed 8 = engine 2 (2x the speed<=4 in-position gate).
M.APPROACH_HARDBRAKE_DIST  = 128
M.APPROACH_HARDBRAKE_SPEED = 8
M.PPT_CHARGE_BRAKE_DIST = 32   -- start braking inside this many wu of standoff
-- Legacy (non-PPT) charge brakes on REACH, not standoff distance: it rolls in
-- until braking from the predicted stop (cpf.predict_stop) would still land a
-- shot on the pill, fixing a standoff that rounded just outside shell range.
-- This floor only bounds the creep so we never drive onto the pill.
M.CHARGE_MIN_STANDOFF   = 5.0  -- never creep closer than this many tiles from the pill
M.CHARGE_SUICIDE_STANDOFF = 1.5  -- ammo_deprived suicide charge: drive this close to the pill instead (≈ adjacent; the pill tile is solid so can't go onto it)
-- Max ticks to wait at the approach point for the LGM to gather enough
-- trees for the shield walls before giving up and degrading to a
-- no-shield (legacy aim/charge) attack. 1500 = 30 s @ 50 Hz.
M.PPT_GATHER_TIMEOUT   = 1500
M.ATTACK_APPROACH_OFFSET = 2.25 -- tiles beyond standoff to start approach from
-- Precise final-approach homing: A* only has to get us within APPROACH_PRECISE_DIST
-- of the exact float approach point (approach_fx/fy); inside that the nav switches
-- to the exact-center creep so we land within APPROACH_PRECISE_TOL wu of it (the
-- in-position transition needs 16 wu — the loose 64 wu nav default parked us short
-- of it and stalled). Sized so the tank can brake from speed to the creep cap.
M.APPROACH_PRECISE_DIST = 384  -- wu (~1.5 tiles): engage exact creep within this of approach_fx
M.APPROACH_PRECISE_TOL  = 16   -- wu: homing tolerance (matches attack.lua approach DIST_TOL)
-- predict_stop: speed at/below which the brake sim stops early. MUST be 0 — the
-- sim loops `while speed > min_speed`, so any value >0 makes it return the tank's
-- CURRENT position (0 steps) whenever entry speed <= it, badly under-predicting
-- the stop at low speed (e.g. 4 → predicted 0 wu vs real ~24 wu) and causing a
-- speed limit-cycle in charge. Left as a tunable but keep at 0 (full sim).
M.PREDICT_STOP_MIN_SPEED = 0
-- predict_stop realism scale: the engine model under-predicts the real coast by
-- ~15% (tanks were stopping ~2 game-units short / overshooting the brake point),
-- so the wrapper stretches the predicted stop point outward from the tank by this
-- factor. Brakes fire slightly earlier and land on the spot. 1.0 = raw model.
M.STOP_PREDICT_SCALE = 1.15
-- Fast approach: when true the attack_pill `approach` substate skips BOTH the
-- standoff-relative proportional brake-zone crawl (stage 1, sdist*0.03) AND the
-- speed-4 precise creep (stage 2). Instead the tank cruises at full speed and
-- brakes purely off cpf.predict_stop, hitting KEY_SLOWER the tick the predicted
-- stop point lands on the approach point (within APPROACH_PRECISE_TOL = 16 wu).
-- Faster, tighter landings; relies on the engine-exact stop predictor being
-- accurate (it projects along the tank facing, valid since A* homes straight at
-- approach_fx on the final leg). Flip false to restore the staged creep.
M.FAST_APPROACH = true
-- Distance (wu, euclidean) from the destination at which the FAST_APPROACH
-- predict_stop fast-path hands off to the precise stage-2 creep. 256 wu = 1
-- tile. Shared by every substate that uses the fast-path (approach,
-- in_range_position) so they all switch to the creep at the same range.
M.FAST_APPROACH_HANDOFF_WU = 256
M.FLEE_PILL_DIST       = 10  -- tiles to flee away from pill when giving up
M.POST_KILL_WAIT_TICKS = 15  -- ticks to hold at standoff after pill dies (in-flight shots clear in ~5)

-- Attack position planner (pick_attack_standoff in goals.lua)
M.ATTACK_PLAN_DIRS          = 12   -- candidate directions sampled around the pill
M.PILL_ATTACK_REPLAN_TICKS  = 150  -- re-evaluate standoff every ~3 s
M.ATTACK_PILL_TANK_PRESENT_PENALTY = 30  -- flat cost added to EVERY attack_pill candidate when a non-rejected enemy tank exists this eval (state._attack_tank_present, set by eval_attack_tank). Biases the bot toward dealing with the live tank instead of chipping pills. Shows as the `atk_tank{30}` term in the attack_pill breakdown.
M.PANIC_BUILD_NEAR_TILES = 12  -- reference range for the panic_build viz closeness gauge: the threat-distance bar is full when the enemy tank is adjacent and empty at this many tiles. Display-only (does not gate the build).
M.APPROACH_SLOW_IN_RANGE_PEN = 150 -- per-tile penalty for slow terrain within pill range on approach
-- Standoff influence bias: -inf * weight added to score, so the ring
-- candidate on the friendly side of the pill wins over the hostile side
-- at comparable terrain. A +50/-50 influence swing moves the score by
-- 200 — comparable to water_pen, less than crossfire. Keeps retreat
-- on low health from dumping us in enemy territory.
M.ATTACK_STANDOFF_INFLUENCE_WEIGHT = 2.0

-- plan_position: terrain analysis for best attack spot
M.ATTACK_SCAN_DEGREES       = 5    -- degrees per step around circle (72 spots at 5°)
M.ATTACK_SAFE_RADIUS        = 3    -- tiles around standoff to check for danger/maneuver room
M.ATTACK_DANGER_THRESHOLD   = 30   -- max total score to be considered safe
M.ATTACK_DANGER_HOTSPOT     = 15   -- any tile in maneuver area above this triggers B penalty

-- Soldier self-planning for a standoff spot (blitz/pill-take): prefer a spot
-- whose shot to the pill CENTER crosses fewer trees and is not blocked by an
-- already-built wall (the commander rejects center-blocked spots, so favoring
-- center-clear here converges the negotiation faster).
M.STANDOFF_SHOT_TREE_PENALTY    = 8    -- per forest tile on the center shot path
M.STANDOFF_SHOT_BLOCKED_PENALTY = 200  -- center shot blocked by a built wall (T_BUILDING/HALFBUILD)

-- Pill-take spots that sit deep inside enemy influence are much harder to
-- hold during the take. In mid/late game (phase != "opening"), multiply
-- those spots' total_score so a hostile-territory take ranks well below
-- a friendly/contested-territory alternative.
M.PILL_TAKE_HOSTILE_INF_THRESHOLD = -20  -- influence at or below = deep enemy territory
M.PILL_TAKE_HOSTILE_INF_MULT      = 5.0  -- score multiplier applied to such spots

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
M.PPT_BLOCKERS_ENOUGH         = 1   -- protected-take build phase ends in SUCCESS as soon as this many blockers are NEWLY placed (wall or dropped pillbox) — but ONLY when a blitz is underway (see PPT_BLOCKERS_ENOUGH_MIN_INWAIT / BLITZ_MIN_READY_TO_CHARGE). Solo, the full planned shield is built. 1 = one blocker is enough cover once the squad is overwhelming the pill.
M.PPT_BLOCKERS_ENOUGH_MIN_INWAIT = 1  -- the one-blocker early-success also applies while the commander is still building IF at least this many soldiers are already parked in blitz_wait (sharing the pill's fire). Pairs with BLITZ_MIN_READY_TO_CHARGE (the ready-to-charge quorum) as the other trigger.
M.PPT_COVER_TARGET_SHOTS = 15  -- build phase ends once shield cover reaches this many shots-to-break (friendly pill = PILLS_MAX_HEALTH 15, wall = WALL_HP_FULL 5). 15 = one pill OR three walls. Independent of the blitz gate.
M.PPT_PILL_WALL_EQUIV = 3.0    -- a dropped/standing friendly pillbox blocker is worth this many WALLS of shield cover (PILLS_MAX_HEALTH 15 / WALL_HP_FULL 5 = 3). The C combo scorer (gh_shield_stamp) weights a pill-filled slot accordingly, so one carried pill stands in for a 3-wall shield.
M.PPT_PILL_BLOCKERS_MAX = 2    -- cap on how many carried pillboxes the shield planner assumes it can drop onto buildable slots (matches builder.lua's PILLBOX_BLOCKERS_MAX). num_pill_blockers passed to the scorer = min(carried_pills, this).
M.WALL_SHIELD_LGM_STUCK_TICKS = 150  -- same-tile timeout for LGM simulation (~3 seconds)
-- Last-wall early end: once the FINAL wall blocker is dispatched (LGM out
-- building it) and the estimated LGM round-trip — go to the slot + LGM_BUILD_TIME
-- + walk back to the tank — is <= this many ticks, count the build phase over and
-- let the take proceed while the LGM finishes the last wall in parallel. The
-- already-built blockers protect the tank. ~5 s @ 50 Hz. Only fires for the last
-- blocker of a multi-wall shield (>=1 other blocker already up).
M.PPT_LAST_WALL_EARLY_TICKS = 250

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
-- Unified attack pill: curve-away evasion after taking hits
M.ATTACK_CURVE_AFTER_HITS = 2   -- actual return-fire HITS taken before the defensive swerve (one per tick our armour drops; charge/shoot/engage all count it the same way now — not armour points)
M.ATTACK_CURVE_TICKS      = 100  -- ticks of swerve dodge (2 seconds)
-- Run the same defensive swerve (ATTACK_CURVE_AFTER_HITS hits taken OR kill
-- locked) during the `charge` substate, not just engage/shoot_pill — so a
-- charging tank dodges return fire instead of driving straight in. The
-- deliberate no-dodge straight rush is the dedicated kill_hardline substate.
-- Flagged so it can be reverted to the old always-straight charge.
M.CHARGE_SWERVE_ENABLED   = true
M.ATTACK_RUSH_ARRIVE      = 1   -- mdist to pill to count as "arrived" during rush

-- Kill-rush fast path: a 1-HP, barely-provoked pill is a free kill. Skip the
-- careful standoff/shield planning and just drive point-blank and fire on the
-- first clear shot, eating whatever return fire the pill throws. Gated on
-- armour so we can afford the hits.
M.ATTACK_RUSH_MIN_ARMOUR  = 5     -- only rush when armour >= this (a 1-shot kill is near risk-free)
M.ATTACK_RUSH_MAX_ANGER   = 0.34  -- pill anger must be <= this (~one PILL_ANGER_BUMP)
M.ATTACK_RUSH_STANDOFF    = 1.5   -- tiles from pill center to park the point-blank charge
M.HARDLINE_ENGAGE_RANGE   = 10    -- only switch to kill_hardline within this many tiles of the pill (PILL_FIRE_RANGE 8 + 2). Farther away, use the normal approach (which handles boats / disembark); the hardline land-rush only kicks in once we're near firing range.
M.HARDLINE_FIRE_AIM_TOL   = 8     -- kill_hardline fires only when the tank's heading is within this many degrees of the pill, so the shot we fire matches the pill-aimed line shot_path_clear validated (never lob a shell off-axis into a stray pillbox/base while driving).

-- Swerve durations (confirmed-kill swerve: pill dead or bullets_fired >= needed)
M.SWERVE_TOTAL_TICKS      = 95  -- total swerve duration
M.SWERVE_TURN_TICKS       = 35  -- ticks of turning at start of swerve

-- Defensive swerve (pill still alive, took hits or crosshairs off): longer
M.SWERVE_DEFENSIVE_TOTAL_TICKS = 95
M.SWERVE_DEFENSIVE_TURN_TICKS  = 40

-- Early swerve exit: peel off before the full duration if no hostile shell is
-- actually heading at us and we aren't taking damage. The swerve's whole point
-- is dodging the pill's predictive return fire; if nothing is coming near, the
-- remaining straight portion is wasted time.
--   SWERVE_SHELL_NEAR_WU    — scan/awareness ring (viz): shells whose RELATIVE
--                             closest-approach to us is within this is shown.
--   SWERVE_HIT_RADIUS_WU    — the actual "will it HIT me" test: a shell whose
--                             relative closest-approach lands within this many WU
--                             of us connects, given how we're currently moving.
--                             The swerve stays ARMED while any shell will hit and
--                             ends the instant none do (we've dodged). Tank hit box
--                             is ~128 WU; 160 keeps a small safety margin.
--   SWERVE_ARM_TIMEOUT_TICKS — the swerve starts UNLOADED; if no shell is inside
--                             the circle (or predicted to land inside it) within
--                             this many ticks, nothing's incoming so the swerve
--                             finishes. Once ARMED it runs until the threat clears.
--                             ~1s @ 50Hz. (Replaces the old clear-tick early-exit.)
M.SWERVE_SHELL_NEAR_WU         = 400
M.SWERVE_HIT_RADIUS_WU         = 160
M.SWERVE_ARM_TIMEOUT_TICKS     = 50

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
M.GUNSIGHT_MAX = 13.875 -- max sightLen (in half-map-squares); shells travel sightLen/2 map tiles. Pulled back ~1 pixel — TODO revisit when shell-hitbox-improvements aim fixes are merged to main

-- Dynamic flee threshold (engage substate)
M.FLEE_ESCAPE_COST_DIVISOR  = 50   -- escape path cost units per +1 armour flee buffer
M.FLEE_SLOW_TERRAIN_BONUS   = 3    -- extra armour buffer when standing on slow terrain

-- Cost-based goal selection (unified scoring — all goals compete on same scale)
M.GOAL_SWITCH_PENALTY      = 30    -- base cost added when switching to a different goal group
M.GOAL_TARGET_SWITCH_PENALTY = 15  -- cost added when same group but different target
-- Ally-claimed cost penalty: if any other bot is broadcasting the same
-- goal (matched by kind + target_id, or kind + mx,my for tile-keyed
-- goals) we add this penalty so we prefer a different objective.
-- attack_tank is exempt — tank threats are time-critical and locally
-- observed; a stale ally broadcast shouldn't pull us off a fight.
-- refuel_at_base uses a much smaller SOFT penalty (just steer us to a
-- different base when possible) since refuel is fungible: it is NOT in
-- _REJECT_POOLS, and instead pays ALLY_CLAIMED_REFUEL_PENALTY PER ally
-- already targeting the same base (added at eval in the pool-1 branch).
-- A closer / more-urgent bot can still pick a crowded base and then brake
-- beside it (wait_for_ally) rather than being hard-rejected off it.
M.ALLY_CLAIMED_PENALTY        = 10000
M.ALLY_CLAIMED_REFUEL_PENALTY = 100  -- per ally, soft (see pool-1 eval branch)
-- "Steal margin" used by the ally-claimed REJECT path on hard-pools
-- (2,3,4,5,6,7). We only REJECT our candidate when the ally's
-- broadcast cost is at least this many units below ours; within the
-- band both bots keep the candidate so a 1-unit cost flicker (caused
-- by broadcast tick lag / cache freshness mismatch) can't flip the
-- yield direction every tick.
M.ALLY_CLAIMED_STEAL_THRESHOLD = 100   -- (legacy additive band; superseded by the ratio below)
-- Ratio-based steal band: a challenger must be at least this fraction cheaper
-- than the current holder to take over a claimed goal. Scale-invariant, so it
-- behaves the same for cheap pill captures (~20-40) and expensive base
-- captures (~1000s). 0.10 = "must be >=10% cheaper to steal".
M.ALLY_CLAIMED_STEAL_FRAC      = 0.10
-- capture_pill override: a drive-over grab has no sunk investment (no shells,
-- no setup), so essentially ANY cost edge should win the pickup — the closer
-- bot grabs it, the other re-routes for free. 0.01 = ">=1% cheaper steals".
-- (The killer-priority window and reposition guards still trump this.)
M.ALLY_CLAIMED_STEAL_FRAC_CAPTURE = 0.01
-- DEPRECATED / unused: the refuel ally-claim FCFS reject (and its
-- far-claimer override) was replaced by the SOFT per-ally cost penalty
-- (ALLY_CLAIMED_REFUEL_PENALTY). Refuel is no longer in _REJECT_POOLS, so
-- there is no first-come claim to override. Kept only to avoid a nil
-- lookup if any stale reference survives; safe to remove later.
M.REFUEL_CLAIM_FAR_TILES       = 10
M.GOAL_COMMITMENT_PER_TICK = 0.5   -- extra switch penalty per tick spent on current goal
M.GOAL_COMMITMENT_CAP      = 75    -- max commitment penalty (reached after 150 ticks / 3s)
M.REFUEL_FULL_COST_MULT    = 8.0   -- pool-1 cost multiplier ceiling when tank is between low and full thresholds; applied at goal-selection time so stale cache costs scale with current state. Ramp is QUADRATIC in fill (see refuel_shape): gentle when genuinely low (fill 0.3 → ~×1.6), punishing when nearly full (fill 0.9 → ~×6.7) so a near-full tank prices refuel out of contention against real goals (20260703_221238 t=24898: old linear ×3 gave a full-armour/60%-shells tank refuel at ~×2.2 — cheap enough to read as top priority). At max fullness the entry is skipped entirely.
-- Refuel dynamic cost shaping (applied live every tick at competition time so
-- the bot can peel off to a closer opportunity as armour/shells climb):
--   final = (cached + BASE_COST - BONUS * deficit_ratio) * full_mult
-- deficit_ratio = max((ARMOUR_LOW - armour)/ARMOUR_LOW, (SHELLS_LOW - shells)/SHELLS_LOW)
M.REFUEL_BASE_COST         = 45    -- flat floor so refuel-at-own-base isn't ~0
M.REFUEL_DEFICIT_BONUS     = 25    -- max discount when fully depleted
-- "Gotta share" scarcity shaping. REFUEL_FULL_COST_MULT (above) is the cost of
-- topping off PAST the LOW watermarks (SHELLS_LOW=20 / ARMOUR_LOW=15) toward
-- target. We steepen that ramp when the team is starved for bases, so a bot
-- leaves near the floor (~20 shells / ~15 armour) to free the base for
-- teammates, and keep it gentle (fill toward target) when bases are plentiful.
-- scarcity = 1 + RATIO_K*max(0, team_tanks/friendly_bases - 1) + LOCAL_K*nearby_allies
-- effective top-off mult = 1 + fill²*(REFUEL_FULL_COST_MULT-1)*scarcity
M.REFUEL_SHARE_ENABLED      = true
M.REFUEL_SHARE_RATIO_K      = 1.5  -- how hard base-scarcity (team tanks per friendly base, over 1.0) steepens the top-off ramp. Losing game (few bases, many tanks) => leave at the floor. 0 disables the ratio term.
M.REFUEL_SHARE_LOCAL_K      = 0.5  -- extra ramp per teammate currently within REFUEL_SHARE_LOCAL_TILES (more bots crowding this base now => leave sooner)
M.REFUEL_SHARE_LOCAL_TILES  = 20   -- tiles; an ally tank inside this counts as "locally competing" for the base
M.REFUEL_SHARE_SCARCITY_CAP = 8.0  -- clamp on the scarcity multiplier so the ramp can't explode
M.REFUEL_BASE_HOP_PENALTY   = 500  -- while standing ON a refuel base, every OTHER base costs +this, so an ally claiming our base (ally_claimed) can't bounce us off mid-refuel — we finish here. A depleted current base is rejected upstream, which still frees us to move to another base.
-- Mines: never hold the bot at base (REFUEL_MIN_MINES=0 → mines never count as
-- "need"), and topping mines past REFUEL_MINE_FREE adds an exponential staying-
-- cost so a mine-rich tank leaves sooner. cost = WEIGHT*(BASE^(mines-FREE) - 1).
M.REFUEL_MIN_MINES         = 0     -- mines below this still count as a refuel need (0 = never wait for mines)
M.REFUEL_MINE_FREE         = 5     -- mines up to here add no staying-cost
M.REFUEL_MINE_HOARD_BASE   = 1.3   -- exponential base for the per-extra-mine cost past FREE
M.REFUEL_MINE_HOARD_WEIGHT = 8     -- scale on the exponential mine-hoard cost term
M.ANGRY_PILL_AT_BASE_PENALTY = 200 -- added to pool-1 cost when an angry hostile pill is in fire range of the base
-- Critical-armour flee: when true, injects a cost=40 flee_to_base candidate
-- into pool 1 so the tank retreats to a safe base. When false (default),
-- relies on the normal pool-1 refuel candidate — REFUEL_DEFICIT_BONUS
-- pushes its cost very low at critical armour, so refuel_at_base usually
-- wins naturally without a dedicated flee path. Flip on if you see the bot
-- fighting instead of retreating when almost dead.
M.CRITICAL_FLEE_ENABLED      = false
-- facing_away brake: when true, tank brakes to speed 8 (or 16 under
-- fire/race) if |heading_err| > 64 brad (~90°). Safer for U-turns but
-- sometimes over-brakes when plow + lookahead swing move_dir 132°.
-- Set false to let the tank carry momentum through sharp reorientations.
M.FACING_AWAY_BRAKE_ENABLED  = false
-- Stay-for-LGM: when tank is at a base and LGM is returning soon, make the
-- refuel_at_base goal cheap enough to usually win but interruptible by an
-- immediate combat opportunity (close capture / close tank).
M.LGM_WAIT_COST            = 15    -- pool-1 cost floor when LGM is returning and tank is at the base (dangerous area)
M.LGM_WAIT_COST_SAFE       = 4     -- pool-1 cost floor when LGM is returning AND the base is in a low-threat area
M.LGM_WAIT_SAFE_THRESHOLD  = 30    -- threat value at/below which the safe floor is used; linear blend between this and 0
-- Multiplicative hysteresis: a different-group / different-target winner
-- must beat the current goal by this much to actually switch. The
-- additive penalties above are absolute and can be swamped by large
-- cost gaps; this percentage is proportional and stays meaningful at
-- any goal cost magnitude.
--   0.7 = the winner must cost ≤ 70% of the current goal's cost to switch
--         (i.e. the winner has to be at least 30% cheaper).
M.GOAL_SWITCH_RATIO        = 0.7
-- Goal history / oscillation detection. The brain remembers the last
-- GOAL_HISTORY_SIZE picked goals; on each new selection, candidates
-- whose (kind, mx, my) or kind appears repeatedly get a gently
-- exponentially growing cost penalty. Catches refuel→capture→refuel
-- style loops where individual targets cycle but the *kind* keeps
-- coming back.
M.GOAL_HISTORY_SIZE        = 10
M.GOAL_HISTORY_EXP         = 1.4  -- penalty = BASE * (EXP^count - 1)
M.GOAL_HISTORY_TARGET_BASE = 25   -- per-(kind,mx,my) repeat
M.GOAL_HISTORY_KIND_BASE   = 8    -- per-kind any-target repeat
M.GOAL_MIN_COMMIT_TICKS    = 25    -- suppress non-urgent replan for this many ticks after a switch
M.GOAL_ABANDON_COOLDOWN    = 0     -- ticks before an abandoned goal can be picked again (0=disabled)
M.WALL_SHIELD_COMMITMENT        = 200  -- extra switch penalty when wall-shield attack is in progress
M.ATTACK_TANK_COMMITMENT_BONUS  = 50   -- extra commitment when currently fighting a tank (see it through)
M.ATTACK_PILL_COMMITMENT_BONUS  = 80   -- extra commitment when mid-attack on a pill; also revokes hysteresis exemption for attack_tank/capture_pill so they can't interrupt for free
M.ATTACK_BASE_COMMITMENT_BONUS  = 250  -- extra commitment when mid-attack on a base — applied while we still have >=1 shell. Once you start a base, follow through; the ONLY non-urgent reason to break off is literally running out of shells (0). Critical-armour flee still preempts via the urgent goal-override path.
M.CAPTURE_BASE_COMMITMENT_BONUS = 250  -- extra commitment when mid-CAPTURE of a base (driving onto a neutral/ground-down base). Comparable to ATTACK_BASE: if you did the work to grind a base down, follow through and actually take it — don't let a normal-cost goal (another base/pill, non-critical refuel) steal it. No shell gate (capturing needs no ammo). Critical-armour flee still preempts via the urgent goal-override path.
M.BLITZ_STANDOFF_SCORE_BUCKET   = 50   -- soldier blitz-standoff pick: ellipse spots are bucketed into score bands this wide; all spots in the best spot's band are the "best pool", and the soldier offers the one CLOSEST to its tank (least travel for ~equal shield quality) instead of the globally-top-scored far spot.
M.EARLY_CAPTURE_BASE_HYST_EXEMPT = true -- opening phase: capture_base skips ALL hysteresis (switch + commit + history), same as capture_pill
M.REFUEL_URGENCY_MIN       = 0.37  -- minimum urgency multiplier for refuel cost
M.REFUEL_MIN_COST          = 25    -- routine-refuel cost floor: an on-base top-off otherwise collapses to ~4 and outbids free-pill grabs (20260703_210207 t=5747). Bypassed at ARMOUR_CRITICAL — survival refuel may enter the reserved <20 band.
                                   -- (with squared urgency: floor cost at
                                   -- bscore × 0.37; e.g. bscore=60 → ~22)
M.PILL_HEALTH_WEIGHT       = 5     -- cost per HP of hostile pill (full 15HP pill = +75)
-- ── Repair fix master toggle ──────────────────────────────────────────────
-- false → repair_pill reverts to the 1.90-beta1 behavior: alive-damaged pills
-- only (no dead-pill rebuild-in-place), plain `max(0, path - dmg*BONUS)` cost,
-- and none of the post-1.90 guards (contested ×3, REPAIR_BASE_COST floor,
-- friendly-fire reject, reposition block) or the influence-exemption. true →
-- current (post-1.90) behavior. Read live, so flipping this one constant is the
-- whole switch. Released builds set this false until the improvements are tested.
M.REPAIR_FIX_ENABLED       = true  -- repair fix ON (dead-pill rebuild + guards); tested OK
M.REPAIR_BASE_COST         = 30    -- flat floor so a close/damaged repair doesn't trivially out-rank other goals
M.REPAIR_DAMAGE_BONUS      = 3     -- cost reduction per missing HP on friendly pill
M.REPAIR_CONTESTED_MULT    = 3.0   -- repair cost ×N when an enemy tank is closer to the pill than us (contested → likely futile)
-- ── Dead-pill repair (rebuild a friendly 0-HP pill IN PLACE) ───────────────
-- The LGM walks out with wood and the engine rebuilds it (lgm.c: a 0-armour
-- pill is the 4×LGM_COST_PILLREPAIR tier). Distinct from alive-damaged repair:
-- capture-in-tank is preferred when SAFE (flexible placement), but in danger
-- rebuilding-in-place risks the expendable LGM instead of the tank, so the cost
-- ignores pill-fire entirely and is driven only by terrain (LGM walk time +
-- reachability) and enemy-TANK snipe risk — which melts under a tank-count lead.
M.REPAIR_DEAD_BASE_COST          = 40    -- flat floor; keeps capture (≈5 when safe) preferred in calm
M.REPAIR_DEAD_MIN_TREES          = 4     -- 0-HP rebuild needs LGM_COST_PILLREPAIR×4 wood; don't commit with less (engine would only partial-repair)
-- distance curve on TILE distance (mdist): flat in the sweet zone, gentle to
-- the knee, exponential beyond (cross-map repairs self-reject).
M.REPAIR_DEAD_SWEET_TILES        = 8     -- ~shooting distance: flat & attractive within
M.REPAIR_DEAD_KNEE_TILES         = 14    -- gentle rise sweet..knee; exponential past
M.REPAIR_DEAD_NEAR_W             = 4.8   -- cost per tile inside the sweet zone
M.REPAIR_DEAD_MID_W              = 15.6  -- cost per tile, sweet..knee
M.REPAIR_DEAD_EXP_BASE           = 2.0   -- exp growth base past the knee
M.REPAIR_DEAD_EXP_STEP_TILES     = 2.0   -- tiles per doubling past the knee
M.REPAIR_DEAD_EXP_SCALE          = 30    -- multiplier on (EXP_BASE^… − 1)
-- mild terrain surcharge: LGM walk-ticks beyond an all-grass walk × this weight.
M.REPAIR_DEAD_TERRAIN_W          = 0.1
M.REPAIR_DEAD_GRASS_TICKS_PER_TILE = 16  -- MAP_MANSPEED_TGRASS; ticks the LGM needs per clear tile
-- snipe: enemy tanks in range of the pill that can pick off the stationary builder.
M.REPAIR_DEAD_SNIPE_RANGE        = 8     -- tiles
M.REPAIR_DEAD_SNIPE_PEN_PER_TANK = 60
M.REPAIR_DEAD_ADV_RELIEF_PER_TANK = 0.25 -- each net friendly tank cuts snipe risk this much
M.REPAIR_DEAD_ADV_FLOOR          = 0.1   -- min snipe multiplier (never fully free)
M.REPAIR_DEAD_LGM_MAX_TICKS      = 2000  -- LGM-travel sim budget (matches other callers)
M.REPAIR_DEAD_LGM_STUCK_TICKS    = 150
M.REPAIR_FRIENDLY_FIRE_REJECT_TICKS = 400  -- 8 s @ 50 Hz: after a friendly shot (own or ally) lands on a friendly pill, refuse to repair it — the team is shooting it down to reposition (perception sets pill._friendly_shot_tick)
M.ATTACK_PILL_BASE_COST    = 30    -- flat cost added to every attack_pill (like ATTACK_BASE_EXTRA_COST for bases) so a pill take isn't free vs other goals
M.ATTACK_BASE_EXTRA_COST   = 80    -- flat cost added to hostile base attacks
M.ATTACK_BASE_THREAT_WEIGHT = 3    -- multiplier for threat at base location (penalise bases behind enemy pills/tanks)
M.ATTACK_BASE_MAX_WALLS    = 1    -- walls the base shot may cross and still fire (we grind them down). Pillboxes (any owner), other bases, and allied tanks ALWAYS block — if the shot isn't valid we drive in for a point-blank shot instead.
-- Crossfire-aware base engage point: instead of always driving point-blank to the
-- base (into any pill crossfire around it), trace the approach path and stop at the
-- CLOSEST path tile from which we can still land a shot on the base (LOS + in shell
-- reach) AND that no more than _MAX_CROSSFIRE enemy/neutral pills can fire on. If no
-- such tile exists, rush right up to the base (old behaviour).
M.ATTACK_BASE_ENGAGE_AVOID_CROSSFIRE = true  -- master toggle for the standoff-engage-point picker
M.ATTACK_BASE_ENGAGE_MAX_CROSSFIRE   = 0     -- max # of pills allowed to cover the engage tile (threat.coverage_at). 0 = fully out of pill fire; bump to 1+ to accept light exposure for a closer/faster shot
M.ATTACK_BASE_ENGAGE_REPLAN          = 40    -- ticks between engage-point recomputes (cached on the goal between)
-- Stalled-rush promotion: the point-blank rush can be rebuffed indefinitely by
-- pill knockback (crawl of a few wu/tick, never arrives, heading never sweeps
-- the base so the opportunistic fire never triggers). When in range with a
-- clear shot but the tank closed less than STALL_WU_PER_TICK x STALL_WINDOW
-- world-units over the last STALL_WINDOW ticks, it stops and shells the base
-- from where it is instead (full speed ~16 wu/tick; observed rebuffed crawl
-- ~4.5 wu/tick).
M.ATTACK_BASE_STALL_WU_PER_TICK = 6   -- avg closing speed below this = stalled
M.ATTACK_BASE_STALL_WINDOW      = 50  -- ~1 s @ 50 Hz measurement window
-- Commit-to-finish: once we put a shot INTO a hostile base, lock onto finishing
-- it (init.lua goal-override). Stays committed until ATTACK_BASE_COMMIT_TICKS
-- after the last shot (refreshed each shot), then releases. Only flee or a tank/
-- LGM within ATTACK_BASE_PREEMPT_SHOOT_TILES preempts — don't chase a far tank/
-- LGM off a base we're nearly done shooting down.
M.ATTACK_BASE_COMMIT_TICKS        = 500   -- ~10 s @ 50 Hz since the last shot landed
M.ATTACK_BASE_PREEMPT_SHOOT_TILES = 8     -- tank/LGM must be within this (≈ shooting distance) to break the base commit
M.GOAL_CROSSFIRE_PENALTY   = 100   -- goal cost per nearby hostile pill that can crossfire at standoff
M.GOAL_CROSSFIRE_NEW_PILL_BASE = 40  -- attack_tank/kill_lgm: cost for the 1st pill whose fire-range covers the engage spot but NOT our current tile (NEW exposure only)
M.GOAL_CROSSFIRE_NEW_PILL_STEP = 10  -- ...and +this for each additional new-exposure pill (so 40, 90, 150, 230, ...)
M.BASE_PILL_COVER_PEN      = 3     -- capture_base/attack_base: flat cost per enemy pill whose fire-range covers the base tile (clear LOS) but does NOT already cover our current tile. Small per-base nudge toward safer bases; affects which base wins.
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
-- PILL_REPAIR_AMOUNT: armour restored PER TREE when repairing a pill.
--   Engine (pillbox.c pillsRepairPos): repairAmount = trees × PILL_REPAIR_AMOUNT(4);
--   the LGM takes ceil(deficit/4) trees per trip (lgm.c armour tiers), so a FULL
--   repair from any HP costs at most 4 trees. (Was M.PILL_REPAIR_COST=1/HP —
--   a 4× overestimate that made the gather target farm 15 trees for a dead pill.)
M.FARM_GATHER_RADIUS       = 3
M.FARM_OPPORTUNISTIC_RADIUS = 2
M.FARM_REFUEL_RADIUS       = 4   -- wider farm radius when stationary at refuel base
M.TREE_OPPORTUNISTIC_MAX   = 20
M.LGM_DEPLOY_DIST          = 3
M.LGM_DEPLOY_DIST_REFUEL   = 5   -- max deploy distance when stationary at base
-- Near an enemy tank, don't pull the LGM out to opportunistically farm unless
-- we're critically low on trees — farming exposes the LGM and stalls us while a
-- threat closes. Only farm within AVOID_DIST tiles of an enemy if trees < MIN.
M.FARM_ENEMY_AVOID_DIST    = 10  -- tiles (mdist) to nearest hostile tank
M.FARM_ENEMY_MIN_TREES     = 4   -- below this, farm anyway (need trees to build)
M.PILL_REPAIR_AMOUNT       = 4   -- armour per tree (engine PILL_REPAIR_AMOUNT)
M.LGM_ETA_DEPART_BUFFER    = 10  -- ticks: leave base this many ticks before LGM returns
M.LGM_NEARBY_TILES         = 3   -- tiles: consider LGM "nearby" within this range
M.LGM_NEARBY_ARRIVAL_TICKS = 60  -- ticks: if LGM arrives within this, skip rescue
M.LGM_NEARBY_NOPACE_TICKS  = 30  -- ticks: if LGM arrives within this, don't slow down

-- wait_for_lgm: when the LGM is out (farming, opportunistic build) but
-- not stranded, inject a low-cost wait_for_lgm candidate so the bot
-- prefers to sit and pick him up before chasing a new objective.
M.WAIT_FOR_LGM_ENABLED     = false  -- master toggle; off = candidate never injected
M.WAIT_FOR_LGM_COST        = 50     -- (only meaningful while ENABLED is true)
-- Carrying a pillbox: even with the master toggle off, wait for the LGM to get
-- back so we can build the pill quickly. Injected at a deliberately HIGH priority
-- (low cost) and NOT suppressed during combat/flee/refuel — when holding a pill,
-- getting the LGM home to build takes precedence (per design intent).
M.WAIT_FOR_LGM_COST_CARRYING = 20
-- Danger-aware wait spot (pick_wait_spot in goals.lua): when the tank's own
-- tile has danger (or it's under fire), wait_for_lgm parks on a chosen SAFE
-- tile instead — scored ring of 8 bearings × radii {3,6,9} + the LGM's tile,
-- minimizing danger-weighted travel + BETA × the LGM's extra walk. Slides
-- toward the LGM when that side is safe, away from the pill when it isn't.
M.WAIT_LGM_DANGER_OK          = 0    -- threat.at at/below this counts as a safe tile
M.WAIT_LGM_BETA_COST          = 6    -- cost per tile of extra LGM walk (~1/3 of tank per-tile travel: prefer sliding toward the LGM when safe)
M.WAIT_LGM_DANGER_W           = 25   -- fallback weight per danger point when NO fully safe tile exists (pick least-bad)
M.WAIT_LGM_HOLD_ARRIVAL_TICKS = 150  -- ~3s: LGM arriving sooner → hold despite danger (moving drags the pickup point) — unless low armour AND under fire
M.ENEMY_LGM_RETURN_TICKS   = 3000     -- estimated ticks for enemy LGM to respawn (~60 sec)
M.RESPAWN_CACHE_WIPE_DIST  = 12       -- tiles; if respawn point is farther than this from death point, wipe all distance-dependent caches
M.ENEMY_LGM_DEAD_ATTACK_DISCOUNT = 0.5  -- multiply attack pill cost when enemy LGM is dead

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
M.LGM_GATHER_MAX_DANGER = 20  -- pre-flight tree gather: mild pill danger on the LGM's harvest path is acceptable (don't refuse trees over a little danger). If the nearest tree's path is too hot, try the next-nearest up to LGM_GATHER_RETRIES.
M.LGM_GATHER_RETRIES    = 5   -- how many nearby forest candidates (nearest first) the gather tries before giving up

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
-- The wait cap above shrinks (in ticks) when sitting out the cooldown is cheap
-- or pointless. Subtractions stack additively, floored at 0 — a 1-HP pill at
-- high armour waits ANGER_WAIT_MAX - 150 - 100 = 250 ticks (~5s).
M.ANGER_WAIT_NEAR_KILL_SUB    = 150  -- ticks (~3s) trimmed when the pill is one hit from death (<=NEAR_KILL_HP): a single shot kills it even fully angry, so the cooldown wait is wasted
M.ANGER_WAIT_NEAR_KILL_HP     = 1    -- pill health at/below which "one hit left" applies
M.ANGER_WAIT_HIGH_ARMOUR_SUB  = 100  -- ticks (~2s) trimmed when our armour is high (>=HIGH_ARMOUR_ARM): we can tank the angry pill on approach, so the wait matters less
M.ANGER_WAIT_HIGH_ARMOUR_ARM  = 30   -- armour at/above which the high-armour trim applies
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
M.WARMUP_MIN_REAL_GOALS    = 10    -- the rolling pool-eval warms up over ~14-49 ticks; until this many finite-cost (pickable) candidates exist across all pools the bot rides the explore fallback. Once reached: exit explore immediately (don't wait out GOAL_MIN_COMMIT) and stop showing the warmup reject row in the WINNERS panel.
M.GOAL_POOL_COUNT          = 10    -- number of pool evaluators to spread across ticks
M.GOAL_CANDS_PER_TICK      = 1     -- A* cost_to evaluations per tick (round-robin)
M.STARTUP_HOLD_TICKS       = 16    -- hold still for this many ticks after brain start so the eval queue warms up before we commit to a direction

-- Incremental replan (experimental toggle). A replan tick runs the full
-- synchronous pipeline finalize_pools -> build_eval_queue -> pick_goal,
-- which is the dominant per-tick cost spike (~6ms on busy maps). The goal
-- DECISION (finalize_pools + pick_goal) must happen on the replan tick,
-- but build_eval_queue only seeds the NEXT cycle's eval queue, so it can
-- be deferred one tick to split the spike across two ticks. When true,
-- build_eval_queue runs on the tick AFTER the replan instead of inline.
-- Default false = original (single-tick) behavior. Flip to true to A/B.
M.INCREMENTAL_REPLAN       = false

-- -------------------------------------------------------------------------
-- Incremental Dijkstra (split-across-ticks full-map cost search)
-- -------------------------------------------------------------------------
-- One Dijkstra search produces cost-to-every-tile in ~14ms wall time. We
-- spread that work across DIJKSTRA_SPREAD_TICKS to keep per-tick cost low,
-- and rerun the search every DIJKSTRA_RECOMPUTE_INTERVAL ticks (or sooner
-- if the tank moves more than DIJKSTRA_RESTART_DIST tiles).
M.DIJKSTRA_SPREAD_TICKS         = 50    -- ticks to spread one search over (legacy; see per-range values below)
M.DIJKSTRA_SHORT_SPREAD_TICKS   = 25    -- (unused with hard budget; kept for reference)
M.DIJKSTRA_LONG_SPREAD_TICKS    = 125   -- half of RECOMPUTE_INTERVAL: long-range completes in ~2.5 s
M.DIJKSTRA_RECOMPUTE_INTERVAL   = 250   -- ticks between recompute kickoffs for long-range slate (5 s @ 50 Hz).
M.DIJKSTRA_RESTART_DIST         = 3     -- tank-moved threshold (tiles) to force restart
M.DIJKSTRA_MAX_COST             = 0     -- 0 = unlimited; long-range covers the whole map

-- Short-range "radar ping": fast local Dijkstra that restarts frequently
-- for responsive nearby navigation. Falls through to long-range for distant tiles.
M.DIJKSTRA_SHORT_INTERVAL       = 10    -- ticks between short-range restarts (0.2 s @ 50 Hz)
M.DIJKSTRA_SHORT_BUDGET         = 500   -- hard node-expansion cap per tick for short-range slates
                                        -- (10 ticks × 500 = 5000 nodes ≈ 10-tile radius)
M.DIJKSTRA_SHORT_MAX_COST       = 0     -- 0 = unlimited; expansion budget limits coverage, not cost cap
M.DIJKSTRA_SHORT_RESTART_DIST   = 2     -- tank-moved threshold for short-range restart
M.DIJKSTRA_USE_FOR_GOALS        = true  -- replace cost_to in step_eval_queue with dijkstra
                                        -- lookup_by_kind. Pill pools use kind=1 (low-danger
                                        -- slate), other pools use kind=0 (normal-danger slate).
M.DIJKSTRA_EXACT                = true  -- track per-node shell budget so wall_shoot edges
                                        -- deplete shells exactly like cost_to does. true = matches
                                        -- A* bit-for-bit, slightly slower. false = optimistic
                                        -- (assume infinite shells, faster but can plan paths the
                                        -- executor refuses).
M.DIJKSTRA_PILL_DANGER_SCALE    = 0.1   -- danger weighting for the "pill take" Dijkstra slate.
                                        -- Pill goals intentionally drive into danger, so the
                                        -- routing search should under-weight it (matches the
                                        -- 0.1 that step_eval_queue temporarily sets for pill pools).

-- Strategic pill placement (idle deployment near front line / friendly base)
M.STRATEGIC_PLACE_ENABLED       = true
M.STRATEGIC_PLACE_SEARCH_RADIUS = 12    -- tiles around the tank to search for a placement spot. Widened from 8: the spot-quality floor (STRATEGIC_PLACE_MIN_SCORE) holds placement whenever no GOOD spot is within range, so a too-tight radius made a slightly-forward tank carry pills forever (its 8-tile bubble was all front/enemy ground). 12 reaches the good back/guardian spots near our bases without lowering standards. CPU is (2R+1)^2 but bounded by the capacity clamp (place_r) on constrained ticks; far spots also cost more travel (path_cost), so they still only win when worth it.
M.STRATEGIC_PLACE_AGGRO_SEARCH_RADIUS = 14  -- wider scan when filling the AGGRO role: aggro tiles sit beyond the front (negative influence), deeper than the default radius reaches from the tank's (rear) position. Lets a good forward aggro tile up to 14 tiles out be found + placed.
M.STRATEGIC_PLACE_FRONT_WEIGHT  = 3.0   -- bonus per tile of proximity to front line
M.STRATEGIC_PLACE_BASE_WEIGHT   = 2.0   -- bonus per tile of proximity to nearest friendly base
M.STRATEGIC_PLACE_THREAT_WEIGHT = 1.5   -- penalty per unit of threat.at(pos)
M.STRATEGIC_PLACE_PILL_SPACING  = 4     -- minimum tile distance from existing friendly pills
M.STRATEGIC_PLACE_PILL_PENALTY  = 50    -- penalty for being within PILL_SPACING of existing pill
M.STRATEGIC_PLACE_LOS_WEIGHT    = 0.5   -- bonus per tile of LOS coverage
M.STRATEGIC_PLACE_LOS_DIRS      = 8     -- number of directions to sample for LOS
M.STRATEGIC_PLACE_LOS_MAX_RANGE = 8     -- max tiles to trace per LOS ray
M.STRATEGIC_PLACE_BASE_COST     = 60    -- base cost so it loses to attack/capture but beats explore
-- Near-enemy-tank placement penalty (combat zone): flat add to a NON-emergency
-- place_pill_strategic cost when an enemy tank is within euclidean range of the
-- chosen spot. CLOSE supersedes NEAR. The def_build emergency drop is exempt.
M.STRATEGIC_PLACE_NEAR_TANK_DIST     = 10
M.STRATEGIC_PLACE_NEAR_TANK_PENALTY  = 30
M.STRATEGIC_PLACE_CLOSE_TANK_DIST    = 7
M.STRATEGIC_PLACE_CLOSE_TANK_PENALTY = 60
M.STRATEGIC_PLACE_MAX_BASE_DIST       = 15    -- don't place pills too far from bases
M.STRATEGIC_PLACE_DEFENSE_RADIUS      = 8     -- radius to count defending pills
M.STRATEGIC_PLACE_UNDERDEFENDED_BONUS = 50    -- bonus per missing defender (target: 2)
M.STRATEGIC_PLACE_BEYOND_FRONT_PENALTY = 100  -- penalty for placing in enemy territory
M.STRATEGIC_PLACE_FRONT_PROX_CAP      = 80    -- influence cap for front proximity bonus
M.STRATEGIC_PLACE_FRONT_PROX_WEIGHT   = 0.5   -- weight for front proximity score
M.STRATEGIC_PLACE_SPACING_BONUS       = 15    -- bonus for 2-4 tile spacing
M.STRATEGIC_PLACE_OFFENSIVE_THRESHOLD = 0.6   -- strength ratio to switch to offensive
M.STRATEGIC_PLACE_SPIKE_BONUS         = 80    -- bonus for placing adjacent to hostile base
-- Carry-time urgency: every tick a pill sits in the tank, place_pill cost
-- drops by this much, capped. After ~600 ticks the pill is essentially free.
M.STRATEGIC_PLACE_CARRY_DISCOUNT_PER_TICK = 0.5
M.STRATEGIC_PLACE_CARRY_DISCOUNT_MAX      = 300

-- Carry value penalties — increase placement cost when carrying is useful
M.STRATEGIC_PLACE_CARRY_EARLY_PENALTY     = 40   -- opening/early_expansion phase
M.STRATEGIC_PLACE_CARRY_CAPTURE_PENALTY   = 60   -- dead pill nearby to capture
M.STRATEGIC_PLACE_CARRY_ATTACK_PENALTY    = 30   -- currently attacking a pill
M.STRATEGIC_PLACE_CARRY_CAPTURE_RANGE     = 15   -- tiles: dead pill within this triggers carry

-- Enemy pill proximity scoring
M.STRATEGIC_PLACE_ENEMY_PILL_DANGER_RANGE  = 7    -- within fire range = penalty
M.STRATEGIC_PLACE_ENEMY_PILL_DANGER_PEN    = 80   -- penalty for being in fire range
M.STRATEGIC_PLACE_ENEMY_PILL_SWEET_MIN     = 8    -- sweet spot min distance
M.STRATEGIC_PLACE_ENEMY_PILL_SWEET_MAX     = 11   -- sweet spot max distance
M.STRATEGIC_PLACE_ENEMY_PILL_SWEET_BONUS   = 40   -- bonus for crossfire support position
M.STRATEGIC_PLACE_ENEMY_PILL_FAR_RANGE     = 15   -- outer limit for small bonus
M.STRATEGIC_PLACE_ENEMY_PILL_FAR_BONUS     = 10   -- small bonus for general proximity

-- Pill war reinforcement
M.STRATEGIC_PLACE_WAR_ZONE_BONUS          = 60   -- bonus for tiles near an active pill war
-- Portfolio model (see PILL_REPOSITION_PLAN.md / pill_portfolio.lua). Classify
-- friendly pills into back/front/aggro and bias placement toward the under-target
-- role (35/45/20). Roles (pill_portfolio.classify): front = near the front line;
-- aggro = own tile in enemy influence OR surrounded by it; back = positive
-- influence and outside the front range.
M.AGGRO_NEG_NEIGHBORS                = 5      -- pill role = aggro if >= this many of its 8 adjacent tiles have negative influence (or own tile negative)
-- Per-unit deficit bias. Strong (>= BEYOND_FRONT_PENALTY) so an under-target
-- aggressive role can pull placement past sc3's beyond-front penalty.
M.STRATEGIC_PLACE_PORTFOLIO_WEIGHT   = 120
-- Protective coverage: bonus per friendly pill / base within fire range a spot
-- covers. Pills weighted higher (mutual support is the key protector signal).
M.STRATEGIC_PLACE_COVERAGE_PILL_WEIGHT = 25
M.STRATEGIC_PLACE_COVERAGE_BASE_WEIGHT = 15
-- Base guardian: every friendly base should have >=1 pill in shooting range.
-- Big bonus per currently-unguarded base a candidate spot would cover.
M.STRATEGIC_PLACE_GUARDIAN_BONUS = 150
-- Minimum spot SCORE to deploy a carried pill (eval_place_pill_strategic). The
-- carry / util-surplus cost discounts make placement EAGER (win the goal); this
-- keeps the QUALITY bar on WHERE it goes so an eager bot doesn't dump a pill at a
-- mediocre spot. Reference scores: role-fill alone ≈ 120 (PORTFOLIO_WEIGHT),
-- a base guardian ≈ 150+, a well-positioned protector 250-400. 150 ⇒ a deficit-
-- role spot must ALSO have real positioning (proximity/coverage) or guard a base.
-- Bypassed when placement is urgent (naked base under fire / about to die).
M.STRATEGIC_PLACE_MIN_SCORE = 150
-- ...BUT relax the bar the more util/carried pills the team is hoarding over the
-- reserve: holding a pile of pills is itself bad, so accept a less-perfect spot
-- rather than carry forever. Effective floor = MIN_SCORE − util_surplus×DROP,
-- never below FLOOR_MIN (so an exposed/purposeless spot is still always held).
-- e.g. surplus 0→150, 1→125, 2→100, 3→75, 4+→70.
M.STRATEGIC_PLACE_MIN_SCORE_SURPLUS_DROP = 25
M.STRATEGIC_PLACE_MIN_SCORE_FLOOR        = 70
-- Out-of-ratio urgency: placement cost is discounted when a pill type is in
-- deficit. Per-deficit-unit fraction, capped.
M.STRATEGIC_PLACE_IMBALANCE_DISCOUNT     = 0.25
M.STRATEGIC_PLACE_IMBALANCE_MAX_DISCOUNT = 0.60
-- Util-surplus urgency: once we hold MORE carried/util pills than the utility
-- reserve, deploying gets cheaper per surplus pill (capped) so the team actively
-- empties tanks of the excess instead of hoarding it. Strengthened (was
-- 0.25 / 0.60) now that trail-drop is gone — place_pill_strategic is the ONLY
-- path that sheds the hoard, so it must win the goal competition hard when pills
-- pile up: surplus 1 → 40% off, 2 → 80% off, 3+ → 85% off (capped).
M.STRATEGIC_PLACE_UTIL_SURPLUS_DISCOUNT     = 0.40
M.STRATEGIC_PLACE_UTIL_SURPLUS_MAX_DISCOUNT = 0.85
-- Per-TANK multi-carry push (vs the team-wide surplus above): each pill THIS
-- tank holds beyond the first cuts placement cost further — one tank hogging
-- 3 utility pills is worse than 3 tanks holding 1 each (one death loses the
-- whole reserve, and carried pills can't block takes). Carrying >= 2 also
-- BYPASSES the util-reserve hold: the reserve only justifies keeping ONE in
-- the tank; the extras get placed even while team util is at/below reserve.
M.STRATEGIC_PLACE_MULTI_CARRY_DISCOUNT     = 0.25  -- per carried pill beyond the first
M.STRATEGIC_PLACE_MULTI_CARRY_MAX_DISCOUNT = 0.50  -- cap (3+ extras)
-- Build-urgency range widening: as the urge to place grows (time carried
-- and/or multiple pills in the tank), the placement scan reaches further for
-- a good spot of the needed type instead of waiting for one to appear inside
-- the default radius. urgency = max(carry_time/CARRY_DISCOUNT_MAX cap,
-- (carried-1)/2 cap 1); extra radius = floor(urgency × this). Applied on top
-- of the default/aggro radius, still capped by the capacity clamp (place_r).
M.STRATEGIC_PLACE_URGENCY_RANGE_BONUS = 6
-- Flat cost multiplier for place_pill_strategic. <1 = preferred. Combined
-- with the carry discount this makes "I'm holding a pill" a near-overriding
-- priority compared to attack/capture goals. Trimmed 0.1 → 0.085 to make
-- deploying a carried pill win a bit more readily across the board.
M.STRATEGIC_PLACE_COST_MULT          = 0.085
-- Defensive pill build: when an enemy tank is visible, place at ±45° from
-- the threat direction, 2-5 tiles out, with clear LGM path.
M.DEFENSIVE_BUILD_MIN_DIST     = 1    -- tiles from tank (inner bound, tried FIRST — nearest spiral out)
M.DEFENSIVE_BUILD_MAX_DIST     = 5    -- tiles from tank (outer bound)
M.DEF_BUILD_THREAT_RANGE       = 8    -- tiles (euclidean): nearest enemy tank must be within this to trigger a panic/defensive build. Past it the tank can't shoot us, so no need to panic-drop a guard pill mid-carry. ~tank gun range + 1 slack.
M.DEFENSIVE_BUILD_ANGLE_OFFSET = 32   -- ±45° in WinBolo 256-unit circle
-- Panic-build cover dedup: a healthy friendly/allied pill within this radius
-- of the tank IS the guard a panic build would drop — skip building another
-- beside it. A cover pill at or below MIN_HP is nearly dead and doesn't
-- count (build the replacement while it still soaks a few shots).
M.PANIC_COVER_RADIUS           = 8    -- tiles: pill fire range — it engages anything shooting us
M.PANIC_COVER_MIN_HP           = 4    -- cover pill hp <= this => doesn't count as cover
-- Emergency def_build dispatches the LGM to run to the spot from wherever the
-- tank is (no within-1-tile gate). Cap how far we'll send the LGM: spots are
-- picked at <= DEFENSIVE_BUILD_MAX_DIST, +1 slack for tank drift between
-- candidate selection and dispatch. Beyond this the LGM walk is too slow/risky.
M.PLACE_EMERGENCY_MAX_DIST     = 6    -- tiles: max tank->spot for emergency LGM dispatch
-- capture_pill cost: path^1.5 * DIST_SCALE + threat * DANGER_WEIGHT.
--   Close+safe   → very low cost (always high priority)
--   Close+hot    → danger term pushes cost up, deprioritises vs safer goals
--   Far (any)    → path^1.5 grows fast, nearly ignored beyond ~200 path cost
M.CAPTURE_PILL_BASE_COST     = 20     -- flat floor so capture_pill never beats a trivially cheap goal
M.CAPTURE_PILL_DIST_SCALE    = 0.05   -- coefficient on path_cost^1.5
M.CAPTURE_PILL_DANGER_SCALE  = 0.10   -- coefficient on danger (linear, wsim handles lethality)
-- "Free pill" value bonus: a dead pill that's close and safe is an easy grab —
-- worth a flat VALUE subtracted from its cost (floored at MIN_COST), so a
-- pristine grab lands near MIN_COST and outranks routine goals INCLUDING a
-- near-zero-cost on-base refuel top-off. (The old ×0.5 multiplier could never
-- get under refuel's floor: 20260703_210207 t=5747, top-off cost 4.5 beat
-- capture 31.6×0.5=15.8 while two free corpses sat uncontested — the enemy
-- took them. Free pills are transient; base stock isn't.) NOT binary: the
-- bonus scales from VALUE (best, at zero danger / point-blank) to 0 (none) by
-- a "badness" = max(distance-over, danger). Both ramp to full price quickly
-- once past their thresholds.
--   distance: full bonus within our shooting reach × RANGE_MULT; beyond that,
--     badness climbs to 1.0 over DIST_FALLOFF tiles.
--   danger: threat.at at the pill tile (folds in pillbox crossfire AND enemy-tank
--     radius, already reduced by LOS/blockers/distance). One covering pill
--     (~PILL_DANGER_BASE) still keeps a strong bonus; DANGER_FALLOFF ≈ two
--     calm pills, where the bonus has faded to ~none.
-- Cost-scale contract: routine goals live at ≥ ~20; the band below 20 is
-- reserved for survival-critical work (flee-level refuel bypasses
-- REFUEL_MIN_COST; urgent defends go negative). A pristine free-pill grab
-- floors at MIN_COST = 20 — the cheapest ROUTINE goal, under routine
-- refuel's 25 floor but never into the reserved band.
M.CAPTURE_FREE_PILL_VALUE      = 30    -- flat cost bonus for a pristine grab (danger 0, in range)
M.CAPTURE_FREE_PILL_MIN_COST   = 20    -- floor after the bonus — cheapest routine goal, above the reserved <20 band
M.CAPTURE_FREE_PILL_RANGE_MULT = 1.5   -- × TANK_COMBAT_ENGAGE_RANGE = free-grab distance
M.CAPTURE_FREE_PILL_DIST_FALLOFF   = 5  -- tiles past grab range → bonus gone
M.CAPTURE_FREE_PILL_DANGER_FALLOFF = 16 -- danger (≈ 2 calm pills) → bonus gone

-- Race-mode capture: bias capture pathfinding toward direct routes and relax
-- steering speed caps so we don't lose races to opponents driving straight.
-- Wsim lethality rejection still vetoes actually-suicidal paths.
M.CAPTURE_THREAT_WEIGHT      = 0.3    -- danger_scale override on capture A* fallback (default ~1.0)
M.CAPTURE_RACE_MODE_CAPTURE  = true   -- tag every capture_* goal with race_mode
M.CAPTURE_RACE_MODE_IMMINENT = true   -- also tag race_mode whenever imminent-capture fires

-- Imminent-capture priority. A capturable (health==0) pill/base a few steps
-- away is essentially a free pickup. Collapse its cost to a small positive
-- floor so refuel / attack_tank / place_strategic can't outscore it and
-- leave the body on the ground. Gated on armour so a one-hit-from-dead tank
-- still flees instead of chasing the pickup.
M.IMMINENT_CAPTURE_PATH_COST  = 30   -- path cost below which capture is "imminent"
M.IMMINENT_CAPTURE_FLOOR      = 5    -- cost floor applied to imminent captures
M.IMMINENT_CAPTURE_MIN_ARMOUR = 8    -- suppress override if armour below this (let flee win)

-- Fresh-kill pickup. The moment WE (or our blitz) drop a pill to 0 armour,
-- commit HARD to grabbing the body — overriding refuel / flee / survival
-- ENTIRELY (no armour gate, unlike imminent-capture above). A wasted kill
-- hands the pill straight back to the enemy, so this is worth dying for: if
-- the grabber dies mid-pickup an ally finishes it. When multiple blitz
-- killers claim the same pill, the one with the LOWEST Dijkstra path cost to
-- the pill grabs (tie → lower player number) and the rest stand down and
-- resume normal goals. Goes through goal_selection's Override 3b. Shot-clear
-- hold reuses POST_KILL_WAIT_TICKS.
M.KILL_PICKUP_ENABLED = true
M.KILL_PICKUP_TTL     = 250    -- rolling TTL: ticks the claim survives since the
                               -- last kill/refresh (~5 s). The grabber refreshes
                               -- it every replan while driving in, so it only
                               -- lapses if the grabber stops making the claim.
M.KILL_PICKUP_HANDOFF = true   -- defer the pickup to the blitz member with the
                               -- LOWEST capture_pill score (tie → lower player #)
-- Grab TIMEOUT: absolute cap measured from the FIRST claim of a pill. The
-- grabber refreshes the rolling TTL every tick while pursuing, so this is the
-- real "give up" deadline — a grabber that hasn't completed the pickup by now
-- drops the claim and the pill reopens to anyone. The unreachable WAY OUT
-- (Override 3b drops on an INF capture cost) usually fires first for walled-in
-- bodies; this backstops the reachable-but-slow case. ~10 s. A body you just
-- killed is normally a few tiles away, so a real grab finishes well inside it.
M.KILL_PICKUP_MAX_TICKS = 500
-- Strategic-center bias: the placement scan is tank-centric, but spots near the
-- chosen strategic center (war zone / base-vs-threat / contested pill) score
-- higher. Bonus = max(0, CAP - dist_to_center) * WEIGHT. Optional (0 if no center).
M.STRATEGIC_PLACE_CENTER_BIAS_CAP    = 16   -- tiles; beyond this the center bias is 0
M.STRATEGIC_PLACE_CENTER_BIAS_WEIGHT = 4    -- score per tile closer to the strategic center

-- Tank combat
M.TANK_COMBAT_ENABLED           = true
M.TANK_COMBAT_MIN_SHELLS        = 10    -- don't engage with fewer shells
M.TANK_COMBAT_MIN_ARMOUR        = 10    -- don't engage with less armour
M.TANK_COMBAT_MAX_RANGE         = 15    -- only consider tanks within this many tiles
M.TANK_COMBAT_AIM_BONUS         = 40    -- cost reduction if already aimed near target
M.TANK_COMBAT_AIM_THRESHOLD     = 20    -- bolo angle units (~28°) for aim bonus
M.TANK_COMBAT_ENGAGE_RANGE      = 7     -- tiles: start shooting at this distance (~max shell range)
-- Pill-take guard: while the bot is on an attack_pill goal, attack_tank
-- candidates past shoot range get an exponentially-growing euclidean-distance
-- penalty so a far tank can't preempt the pill take (only a CLOSE, threatening
-- one can). penalty = min(BASE^(edist-RANGE) * K, CAP).
M.ATTACK_FAR_PREEMPT_RANGE = 7      -- tiles: euclidean shoot range; no penalty within this
M.ATTACK_FAR_PREEMPT_BASE  = 1.7    -- exponential base per tile beyond range (~+70%/tile)
M.ATTACK_FAR_PREEMPT_K     = 8      -- multiplier on the exponential term
M.ATTACK_FAR_PREEMPT_CAP   = 1e6    -- penalty ceiling (effectively un-preemptable when far)
M.TANK_COMBAT_STANDOFF_RANGE    = 7     -- tiles: nav target when closing (at max shell range)
M.TANK_COMBAT_OPTIMAL_DIST      = 5     -- tiles: ideal engagement distance
M.TANK_COMBAT_TOO_CLOSE         = 2     -- tiles: back off if closer than this
M.TANK_COMBAT_FLEE_ARMOUR       = 0     -- disengage if armour drops to this (0 = never flee on armour alone)
M.TANK_COMBAT_FLEE_SHELLS       = 0     -- disengage if shells drop to this (0 = never flee on shells alone)
-- Pillbox-crossfire disengage: break off a tank chase if our own tile is
-- covered by this much enemy PILL danger (don't trade armour into a tank
-- that's camping under its own pillboxes). Applies in EVERY phase — this is
-- the disengage that actually matters. ~one angry pill or a couple of calm
-- ones overlapping. Tune up to be more willing to fight near pills.
M.TANK_COMBAT_DEFENDED_DANGER   = 30
M.TANK_COMBAT_NEAR_PILL_PENALTY = 80    -- cost penalty if enemy is near a hostile pill (crossfire)
M.TANK_COMBAT_NEAR_PILL_RANGE   = 5     -- tiles: how close to hostile pill counts
M.TANK_COMBAT_BASE_COST         = 30    -- base cost so pills/captures usually win over tank hunting
M.TANK_COMBAT_SHELL_SPEED       = 32    -- WU per sim step (for lead-target calc)
M.TANK_COMBAT_SHELL_STEPS_PER_TILE = 8  -- steps for shell to cross 1 tile (256/32)
M.TANK_COMBAT_JINK_PERIOD       = 10    -- ticks between jink direction changes
M.TANK_COMBAT_JINK_ANGLE        = 32    -- bolo angle offset for lateral jink (~45°)
M.TANK_COMBAT_OPPORTUNISTIC_RANGE = 4   -- tiles: fire at enemy if already aimed near them
M.TANK_COMBAT_OPPORTUNISTIC_AIM = 8     -- bolo angle units (~11°) aim tolerance for opportunistic shot
-- Stuck-fire: when aimed at enemy but shot_path_clear keeps rejecting
-- (wall in the way), fire anyway after this many ticks. Shells will
-- chip the wall until LOS opens up, so two tanks dug in on opposite
-- sides of a wall don't sit there forever. Reset whenever a normal
-- clear shot fires or we leave engage. ~30 ticks ≈ 1s.
M.TANK_COMBAT_STUCK_FIRE_TICKS  = 30

-- Kill-LGM shoot gates.  LGMs are small (1 tile, hitbox even smaller),
-- move slowly (~3 wu/tick), and die in one hit — so we fire from
-- farther than tank-combat opportunistic but require tighter aim.
M.KILL_LGM_SHOOT_RANGE = 8   -- tiles: open fire when within this distance
M.KILL_LGM_NAV_INSET   = 3   -- tiles: nav target sits this far INSIDE the engage boundary
                             -- so tank crosses into engage range with forward momentum
                             -- (engage trigger still fires at SHOOT_RANGE; only the
                             -- "where to drive to" target gets pulled in)

M.TANK_COMBAT_LOS_EXTRA_RANGE       = 3    -- tiles beyond ENGAGE_RANGE that qualify for LOS fast-engage
M.TANK_COMBAT_LOS_BASE_COST         = 5    -- very cheap base cost when enemy is in-range with clear LOS
M.TANK_COMBAT_LOS_COST_PER_TILE     = 3    -- added cost per tile of separation in LOS engage
M.TANK_COMBAT_WALL_PENALTY_PER_HP   = 20   -- cost penalty per wall HP above 1 block (WALL_HP_FULL=5)
                                            -- 0-1 block: no penalty; 2 blocks: +100; 5 blocks: +400
M.TANK_COMBAT_LOW_SHELLS_THRESHOLD  = 20   -- shells below this trigger a cost penalty

-- Ghost tank tracking: when an enemy tank goes out of sight (e.g. hides in a
-- forest), keep it as a TARGETABLE "ghost" at an extrapolated position for a
-- short window so the bot hunts it down instead of instantly giving up.
M.GHOST_TANK_TTL_TICKS      = 100   -- ~2 s @ 50 Hz: keep ghosting after last sighting (only while the ghost tile or a cardinal neighbour has trees; open-ground ghosts are dropped — a tank there would be visible)
M.GHOST_TANK_HIST           = 10    -- position samples used to average heading/speed
M.GHOST_TANK_COST_PENALTY   = 40    -- added attack_tank cost for a ghost (prefer a tank we can actually see)
M.TANK_COMBAT_LOW_SHELLS_COST_PER   = 1.5  -- cost per shell below threshold (0→30, 10→15, 15→7.5)
M.TANK_COMBAT_BOAT_MULT            = 0.8   -- cost multiplier for enemy on river/boat (exposed)
M.TANK_COMBAT_DEEPSEA_MULT         = 0.25  -- cost multiplier for enemy on deep sea (one-shot kill)

-- Anti-tank opportunistic pill drop
M.ANTITANK_DROP_ENABLED         = true
M.ANTITANK_DROP_RANGE           = 12    -- enemy tank must be within this many tiles
M.ANTITANK_DROP_COOLDOWN        = 200   -- ticks between opportunistic drops

-- Emergency pill drop (aIndy: drop pill when about to die to save it)
M.EMERGENCY_DROP_ENABLED        = true
M.EMERGENCY_DROP_ARMOUR         = 5     -- drop when armour at or below this
M.EMERGENCY_DROP_MIN_ENEMIES    = 1     -- don't drop if no enemies visible
M.EMERGENCY_DROP_SHELL_SAFE_DIST = 3    -- tiles: don't drop if shell within this range
M.EMERGENCY_DROP_SEARCH_DIRS    = 8     -- directions to search for safe drop tile

-- Base Killer Mode (aIndy: auto-activate when team outnumbers opponents)
M.BASE_KILLER_TEAM_ADVANTAGE    = 2     -- activate when team has this many more players
M.BASE_KILLER_ATTACK_DISCOUNT   = 0.3   -- multiply attack_base cost (makes bases top priority)
M.BASE_KILLER_PILL_PENALTY      = 2.0   -- multiply attack_pill cost (deprioritize pills)
-- Spiking pills: a hostile/neutral pill within firing range of a friendly
-- base denies us refuel there ("spiking"). Clearing spikes is prioritized
-- with a SMALL discount on the spike itself plus a SMALL whole-cost
-- multiplier on every other attack_pill candidate while ANY spike exists
-- (applied once, not per spike). BOTH effects scale with the spike's
-- DECISIVENESS
-- (1/cover, where cover = spikes sitting on its least-contested base):
-- a lone spike whose removal fully frees a base gets the full values; one
-- of 4 pills co-spiking an area barely registers (that area is lost —
-- killing one changes nothing). Deliberately mild: the tilt is within
-- pool 6 only, so attack_tank / kill_lgm / refuel keep their normal
-- cross-pool balance. (Replaces BASE_THREAT_PILL_DISCOUNT=0.5, which
-- lived only in the dead eval_attack_pill path and never affected live
-- selection.)
M.SPIKE_PILL_DISCOUNT           = 0.7   -- combat-cost multiplier at FULL decisiveness (lerps toward 1.0 as cover grows: cover 2 → 0.85, 4 → 0.925)
M.SPIKE_OTHER_PENALTY_MULT      = 1.2   -- whole-cost multiplier on every NON-spiking attack_pill candidate at full decisiveness (lerps toward 1.0: cover 2 → 1.10, 4 → 1.05)
-- Breadth scaling: each base a spike denies BEYOND the first strengthens the
-- discount pull by this fraction (decisive 3-base spike: effect x1.7 →
-- 1-0.3*1.7 = 0.49 → floored at SPIKE_DISCOUNT_FLOOR). Applied on top of
-- decisiveness, so a co-spiked wide pill still pulls weakly (dec scales the
-- whole effect first: dec 0.5 + 3 bases → x0.745).
M.SPIKE_BASES_BONUS             = 0.35
M.SPIKE_DISCOUNT_FLOOR          = 0.5   -- combat multiplier never drops below this
-- Net tilt toward a spike ≈ (0.3 + 0.2)/cover = 50%/cover (lone spike 50%,
-- pair 25%, quad 12.5%).

-- Automatic de-mine interrupt (demine.lua): shoot known mines in crosshair
-- range, pushing a kill_mine goal over the current one and popping back
-- when cleared. Cost per mine = dist * (1 + BEHIND_MULT * angoff/128) —
-- mines near the current heading are strongly preferred over ones behind.
M.DEMINE_ENABLE       = true
M.DEMINE_MIN_SHELLS   = 3     -- don't start a clear with fewer shells than this
M.DEMINE_MIN_DIST_WU  = 512   -- >= 2 tiles: never detonate a mine at our own feet
M.DEMINE_BEHIND_MULT  = 2.0   -- directly-behind mine costs 3x its distance
M.DEMINE_SCAN_PERIOD  = 5     -- ticks between eligibility scans
M.DEMINE_MAX_TICKS    = 150   -- give up (pop + tile cooldown) after ~3s
M.DEMINE_LAND_WU      = 100   -- shell end-of-life must land this close to the mine tile center
-- Terrain repair (demine.lua): pave mine craters / rubble / crater-flood
-- water with roads when the LGM can do it safely in OUR territory
-- (influence > 0). A parallel LGM job like farming — the tank carries on
-- with its goal while the LGM walks out and paves; the job retires when the
-- tile is paved (timeout / enemy-near / tank-left-it-behind backstops).
M.TREPAIR_ENABLE       = true
M.TREPAIR_RADIUS       = 3   -- tiles from the tank a repairable tile may be (was 5; halved — full radius fired too often)
M.TREPAIR_MAX_TICKS    = 400 -- give up (drop job + tile cooldown) after ~8s
M.TREPAIR_ENEMY_RANGE  = 10  -- no repairs (and abort) with a hostile tank this close
M.TREPAIR_ABANDON_DIST = 6   -- drop the job if the tank is this many tiles past it and the LGM never left

-- Friendly pill as barrier bonus (aIndy: use friendly pills as shields)
M.FPILL_BARRIER_BONUS           = 80    -- cost reduction when friendly pill is between us and target

-- Pill repositioning. Legacy badness conditions (ORPHAN_DIST/THRESHOLD, AFAIK
-- from aIndy's "pissing") are RETIRED — reposition now scores on the influence
-- portfolio (see pill_portfolio.lua / PILL_REPOSITION_PLAN.md): cost is driven
-- primarily by category balance (35/45/20 back/front/aggressive).
M.PILL_REPOSITION_ENABLED       = true
M.PILL_REPOSITION_BASE_COST     = 500   -- flat floor so reposition isn't trivially cheap (raised 350->500: reposition was firing too often)
M.PILL_REPOSITION_SURPLUS_W     = 100   -- PRIMARY: discount per pill over its category allotment (trimmed 150->100 to keep cost nearer the floor)
M.PILL_REPOSITION_COVERAGE_PILL_W = 25  -- cost added for ONE friendly pill covered in fire range (good mutual support: keep)
M.PILL_REPOSITION_EXCESS_PILL_W   = 35  -- discount per friendly pill BEYOND the first in fire range (over-covered: roll surplus forward)
M.PILL_REPOSITION_COVERAGE_BASE_W = 20  -- cost added per friendly base covered in fire range
M.PILL_REPOSITION_ADJACENCY_W   = 30    -- discount per friendly pill in the 8 neighbors (double-take risk)
M.PILL_REPOSITION_OVEREXTEND_W  = 60    -- discount for an aggressive pill deeper than -50 influence
M.PILL_REPOSITION_LEGACY_CAP    = 40    -- cap on the legacy "bad spot" discount (orphan/crossfire/terrain); secondary to surplus (trimmed 75->40 to fire less)
M.PILL_ROLE_REEVAL_TICKS        = 3000  -- re-evaluate a pill's back/front/aggro role every 60s (influence shifts over time)
M.PILL_UTILITY_TARGET_FRAC      = 0.25  -- desired share of pills available as blockers/utility; below this, hold a spare pill in tank
M.ALLY_BLOCKER_REJECT_TICKS     = 150   -- ticks a pill stays rejected from our pools after an ally declares it a blocker while we were targeting it (yield window so we don't immediately re-pick it)
M.PILL_REPOSITION_MIN_SHELLS    = 15    -- need this many shells to reposition (must shoot the pill down to 0 to pick it up)
M.REPOSITION_DEMOLISH_GRACE_TICKS = 1500 -- ~30s: while demolishing a pill for reposition, suppress repair_pill on it (avoid shoot→repair→shoot oscillation)
M.REPAIR_REPOSITION_BLOCK_TICKS = 400  -- 8s @ 50Hz: block repair_pill on a pill we're repositioning (driven by the live goal each tick) AND for this long AFTER the reposition goal ends — so a freed pool can't immediately heal the pill we just shot down. Covers the gap REPOSITION_DEMOLISH_GRACE missed (it only refreshes while reposition_steer is firing).
-- Reposition risk penalties (raise cost = discourage repositioning):
M.PILL_REPOSITION_FEW_PILLS_THRESHOLD = 3   -- team total pills (deployed + carried) at/below which reposition is penalized
M.PILL_REPOSITION_FEW_PILLS_PENALTY   = 400 -- cost added when the team has few pills (can't afford to take one offline)
M.PILL_REPOSITION_ENEMY_TANK_PAD      = 5   -- tiles beyond PILL_FIRE_RANGE within which an enemy tank counts as "around" the pill
M.PILL_REPOSITION_ENEMY_TANK_W        = 150 -- cost added per enemy tank within (PILL_FIRE_RANGE + pad) of the pill
M.PILL_REPOSITION_UNPROTECTED_PENALTY = 100 -- extra cost when enemy tanks are near AND no friendly pill covers this one (no support)
M.PILL_REPOSITION_COOLDOWN_TICKS      = 1500 -- ~30s @ 50Hz: after THIS bot finishes (or abandons) a reposition, it won't START another for this long. Per-bot rate limit so a single tank doesn't churn reposition after reposition. A committed/locked reposition is never blocked by this (it's allowed to finish). 0 disables.
M.PILL_REPOSITION_LOCK_TICKS          = 750 -- ~15s @ 50Hz: hold a committed reposition until the pill is demolished or this elapses
M.PILL_REPOSITION_LOCK_COST           = 30  -- locked-in reposition cost (beats routine goal churn; sub-30 survival goals still preempt)
-- Legacy (unused; kept for reference / any external readers):
M.PILL_REPOSITION_ORPHAN_DIST   = 15
M.PILL_REPOSITION_THRESHOLD     = 50

-- ── Reposition VOTING (team consensus) ──────────────────────────────────────
-- Reposition no longer fires off the raw pool-10 cost alone. A bot that wants to
-- move a back pill opens a team VOTE; it only carries the move out if the vote
-- passes (silence = abstain = yes; any NO blocks). Allies vote NO when moving the
-- pill would be unsafe or they have a better candidate (see reposition_vote.lua).
M.REPOSITION_VOTE_ENABLED               = true
M.REPOSITION_VOTE_WINDOW_TICKS          = 10   -- ticks the initiator waits for NO votes before resolving
-- Pacing model (simple 30/30): the team may MOVE one pill every ~30s
-- (RECENT_MEMORY, timed from consumption / observed execution — never from
-- a vote merely passing), and a FAILED vote costs the whole team a flat
-- ~30s before anyone proposes again (FAIL_COOLDOWN — retrying a just-vetoed
-- move via a different proposer is spam; the NO reasons haven't changed).
-- (Replaces the imbalance-scaled INITIATE_COOLDOWN model.)
M.REPOSITION_VOTE_FAIL_COOLDOWN         = 1500 -- ~30s @ 50Hz: team-wide proposal hold after ANY failed vote (own or observed)
M.REPOSITION_VOTE_RECENT_MEMORY_TICKS   = 1500 -- ~30s @ 50Hz: a bot votes NO (and won't propose) if it remembers a reposition EXECUTING within this window
M.REPOSITION_URGENT_SCORE               = 0    -- candidates scoring below this are URGENT (actively harmful position): the proposer bypasses the recent-memory blackout and voters skip the recent_repo NO. Safety NOs (blocker / enemy near / better candidate) and the fail cooldown still apply.
M.REPOSITION_VOTE_ENEMY_NEAR_TILES      = 15   -- vote NO if an enemy tank is within this many tiles of the pill AND nothing else covers it
M.REPOSITION_VOTE_TANK_COVER_TILES      = 10   -- vote NO if the pill IS covered by >=1 other pill but an enemy tank is within this many tiles
M.REPOSITION_VOTE_APPROVAL_TTL          = 1500 -- ~30s @ 50Hz of ACTIONABLE time: the TTL burns only on ticks the initiator can actually act (can_carry_now — LGM in tank, hands free, shells); busy stretches pause it, with a hard wall-clock cap at 3× TTL. While an approval is held, luxury LGM dispatches (opportunistic farm / road_ahead / trepair) are suppressed so the window isn't wasted
M.REPOSITION_APPROVED_COST              = 80   -- fixed reposition cost for the initiator on the pill its team vote APPROVED; beats routine goals (capture/base/place) so the move actually wins the pool, while sub-80 survival/refuel goals can still preempt
M.REPOSITION_VOTE_RESULT_LATCH_TICKS    = 120  -- keep the vote-result panel on screen this long after resolve so it's readable
M.REPOS_GUARD_TTL                       = 1500 -- ~30s: reposition-target tiles stay repair/defend/rebuild-proof this long past the last refresh (refreshed every tick while the move runs, so this is the tail AFTER the vote pass / goal broadcast stops — generous so slow pickups and comms gaps can't let a rebuild slip in)
M.REPOS_PLACE_EXCLUDE_RADIUS            = 5    -- tiles: place_pill_strategic refuses candidate tiles within this of a live _repos_guard entry — the pickup CREATED the coverage hole the scorer wants to fill, so without this the vacated tile re-wins and the pill is rebuilt where it stood (20260704_022107 t=5625)
-- Exponential "redundant pill" discount: the MORE friendly pills already cover a
-- pill, the exponentially cheaper it is to move (a redundant back pill is the
-- best thing to relocate). disc = min(CAP, W * (BASE^covering_pills - 1)),
-- subtracted from the reposition position-cost. BASE>1 → grows fast.
M.REPOSITION_COVERAGE_EXP_BASE  = 1.7
M.REPOSITION_COVERAGE_EXP_W     = 60
M.REPOSITION_COVERAGE_EXP_CAP   = 400
M.REPOSITION_VOTE_SCORE_TOPN    = 5    -- how many of a tank's top reposition candidates the score visualizers keep/show
-- Reposition position-scan scheduling: the heavy O(pills^2) coverage scan is
-- decoupled from the (busy) replan tick. Once the cache is this stale we look
-- for a QUIET tick (this tick's OWN elapsed-so-far at/below the rolling average,
-- measured at the scheduler point) to rescan; if none shows up within +MAX_DEFER
-- ticks we force it so it can't starve.
M.REPOSITION_SCORE_INTERVAL  = 50   -- ticks of staleness before a rescan is due (~1s, matches replan cadence)
M.REPOSITION_SCORE_MAX_DEFER = 40   -- extra ticks we'll wait for a quiet tick before forcing the rescan
-- Hard floor: with this many or fewer BUILT (deployed) team pills, NO reposition
-- happens at all — we can't afford to take one offline. (Stricter, absolute gate
-- vs the softer PILL_REPOSITION_FEW_PILLS_* cost penalty.)
M.REPOSITION_MIN_TEAM_PILLS  = 4

-- Defensive trail dropping
-- Trail drop DISABLED: it dispatched the LGM to drop a carried pill behind a
-- moving tank via a direct BUILDMODE_PBOX, circumventing place_pill_strategic
-- (no scored spot search, no portfolio ratio, no PLACE_PILL_GATE). Surplus pills
-- should be deployed through place_pill_strategic, which is scored + ratio-aware
-- and already gets cheaper the more the team hoards (util-surplus discount).
M.TRAIL_DROP_ENABLED            = false
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
M.OPENING_UNREACHABLE_GRACE_TICKS = 1500  -- ~30s: only AFTER this do we drop genuinely-unreachable (INF dijkstra cost) neutral bases from the opening-exit count. Before it, the cold-start dijkstra surface hasn't expanded and bases legitimately aren't reached yet, so every neutral still counts (don't disrupt the early land-grab).
M.OPENING_BASE_RESCORE_TILES = 10  -- opening phase: one tick before every replan, force a strict-A* rescore of every base within this many tiles of the tank (the incremental base eval is dijkstra-only, so nearby bases sit at INF during the cold-start until the surface reaches them). Makes them selectable at the imminent replan.
M.PHASE_HYSTERESIS_TICKS  = 100    -- ~2 seconds of consistent signal before switching
M.ENDGAME_PILL_RATIO      = 0.70   -- >70% of pills = endgame
M.ENDGAME_BASE_RATIO      = 0.80   -- >80% of bases = endgame
M.FRONT_LINE_INTERVAL     = 50     -- recompute front line every ~1 second

-- How far (tiles, Manhattan tank→goal) the phase-weight bias fades to neutral
-- (1.0), PER goal type. The phase preference is a LOCAL strategy — full weight at
-- the tank, lerping to 1.0 by this distance. Big = the bias reaches far (we'll
-- travel for it); small = only nearby goals feel it. Per-pool so e.g. opening
-- capture_base reaches across the map (bases are the priority) while a far
-- capture_pill stops pulling us off course. `default` covers any pool not listed.
M.PHASE_WEIGHT_DIST_FALLOFF = {
  default      = 40,
  capture_base = 90,   -- bases are the opening priority — chase them far
  capture_pill = 18,   -- don't cross the map for a dead pill
  attack_pill  = 30,
}

-- Phase-dependent goal cost multipliers.
-- < 1.0 = cheaper (more attractive), > 1.0 = more expensive (less attractive).
-- Keys must match the pool_name strings used in goals.lua.
M.PHASE_WEIGHTS = {
  opening = {
    capture_base     = 0.3,
    capture_pill     = 0.5,
    repair_pill      = 2.0,
    attack_pill      = 3.0,
    attack_base      = 2.0,
    place_strategic  = 3.0,
    defend_pill      = 2.0,
  },
  middle = {
    capture_base     = 1.0,
    capture_pill     = 1.0,
    repair_pill      = 0.8,
    attack_pill      = 0.6,
    attack_base      = 1.2,
    place_strategic  = 0.7,
    defend_pill      = 0.5,
  },
  -- endgame_winning + endgame_losing: ALL pools held at 1.0 for now
  -- while we tune the rest of the pipeline.  Real endgame weights to
  -- be reintroduced once mid-game cost balance is settled.
  endgame_winning = {
    capture_base     = 1.0,
    capture_pill     = 1.0,
    repair_pill      = 1.0,
    attack_pill      = 1.0,
    attack_base      = 1.0,
    place_strategic  = 1.0,
    defend_pill      = 1.0,
  },
  endgame_losing = {
    capture_base     = 1.0,
    capture_pill     = 1.0,
    repair_pill      = 1.0,
    attack_pill      = 1.0,
    attack_base      = 1.0,
    place_strategic  = 1.0,
    defend_pill      = 1.0,
  },
}

-- Forward world simulation (cworldsim)
M.WSIM_ENABLED             = true   -- enable forward sim for goal evaluation
M.WSIM_MAX_TICKS           = 300    -- max ticks to simulate per path
M.WSIM_DAMAGE_COST_WEIGHT  = 10     -- cost per point of predicted armor damage
M.WSIM_KILL_REJECT         = true   -- hard-reject goals where sim predicts death
M.WSIM_KILL_PERSIST_TICKS  = 50     -- ~1s: keep a KILL verdict on a goal (kind@tile) for this long so it stays rejected on ticks where wsim is capped/disabled — stops the bot flapping back onto a lethal goal before refuel/hysteresis take over
M.WSIM_KILL_REJECT_OPENING = false  -- enforce the death-reject in the opening phase too. Default false: opening is the critical land-grab window — dying to grab a base is an acceptable trade, so we don't let wsim veto an attempt.
M.WSIM_OPENING_ENABLED     = false  -- run wsim AT ALL during the opening phase. Default false: same reasoning — taking bases early is so important that we'd rather be reckless and risk dying than have wsim's damage-cost shaping pull us off an opportunity.
M.WSIM_LGM_DEATH_PENALTY   = 200    -- extra cost if sim predicts LGM will die

-- =========================================================================
-- BRAIN_CAPACITY_LEVELS[1..10]: 1=10%, 10=100%. Levers per tier:
--   dij_short  : SHORT-slate nodes/tick (default 500). Powers steering nav.
--   dij_long   : LONG-slate nodes/tick (default ~560 from 70k/125 ticks).
--   scan_step  : eval_pill_difficulty step in degrees (5/10/20/45).
--   pp_spread  : plan_position scan spread over N ticks. 1=do all 72
--                angles in one tick (best). N=spread across N ticks
--                (~72/N angles per tick). At tier 1 (spread=50) the
--                full 72-angle sweep completes in ~1 s. No resolution
--                loss — always 5° / 72 angles regardless of tier.
--   ttl_mult   : multiplier on pool-6 diff_cache distance-tier TTLs.
--   eval_iv    : ticks between step_eval_queue pops (1=every tick).
--   wsim       : sims top-N of the merged ~9-entry pool[]. nil=all, false=skip.
--   place_r    : caps STRATEGIC_PLACE_SEARCH_RADIUS for the pool-8 placement
--                scan (12 at the top tiers, scaling down to 2 under CPU load).
--   tank_step  : eval_attack_tank standoff scan step in degrees (default 5).
--   sb_positions / sb_step : shield-blocker ring scan density.
--                positions × step ≈ angular coverage. Tier 10 keeps the
--                legacy 28 × 0.5° (~±7°) sweep. Lower tiers drop to
--                fewer positions across a proportionally wider step so
--                coverage stays similar but per-scan cost drops.
-- =========================================================================
M.BRAIN_CAPACITY_LEVELS = {
  [10] = { dij_short=500, dij_long=560, scan_step=5,  pp_spread=1,  ttl_mult=1.0, eval_iv=1, wsim=nil,   place_r=12, tank_step=5,  sb_positions=28, sb_step=0.5 },
  [9]  = { dij_short=500, dij_long=450, scan_step=5,  pp_spread=6,  ttl_mult=1.0, eval_iv=1, wsim=nil,   place_r=12, tank_step=5,  sb_positions=28, sb_step=0.5 },
  [8]  = { dij_short=450, dij_long=350, scan_step=10, pp_spread=12, ttl_mult=1.1, eval_iv=1, wsim=5,     place_r=11, tank_step=10, sb_positions=28, sb_step=0.5 },
  [7]  = { dij_short=400, dij_long=275, scan_step=10, pp_spread=17, ttl_mult=1.3, eval_iv=1, wsim=5,     place_r=10, tank_step=10, sb_positions=28, sb_step=0.5 },
  [6]  = { dij_short=400, dij_long=200, scan_step=20, pp_spread=22, ttl_mult=1.5, eval_iv=2, wsim=3,     place_r=8, tank_step=10, sb_positions=28, sb_step=0.5 },
  [5]  = { dij_short=400, dij_long=150, scan_step=20, pp_spread=28, ttl_mult=1.7, eval_iv=2, wsim=3,     place_r=5, tank_step=20, sb_positions=28, sb_step=0.5 },
  [4]  = { dij_short=400, dij_long=100, scan_step=20, pp_spread=33, ttl_mult=2.0, eval_iv=3, wsim=1,     place_r=5, tank_step=20, sb_positions=28, sb_step=0.5 },
  [3]  = { dij_short=400, dij_long=75,  scan_step=45, pp_spread=39, ttl_mult=2.5, eval_iv=3, wsim=1,     place_r=4, tank_step=45, sb_positions=28, sb_step=0.5 },
  [2]  = { dij_short=400, dij_long=50,  scan_step=45, pp_spread=44, ttl_mult=3.0, eval_iv=4, wsim=false, place_r=3, tank_step=45, sb_positions=28, sb_step=0.5 },
  [1]  = { dij_short=400, dij_long=25,  scan_step=45, pp_spread=50, ttl_mult=4.0, eval_iv=5, wsim=false, place_r=2, tank_step=45, sb_positions=28, sb_step=0.5 },
}

-- Tier control: per-tier ms history corroborates raise decisions; drops
-- are aggressive (multi-tier on bigger overruns + on host-killed ticks).
M.CAPACITY_EWMA_ALPHA       = 0.20  -- EWMA blend on think/target ratio AND per-tier ms.
M.CAPACITY_DROP_RATIO       = 1.20  -- drop 1 tier when smoothed ratio > 1.2.
M.CAPACITY_DROP_BIG_RATIO   = 1.50  -- drop 2 tiers when smoothed ratio > 1.5.
M.CAPACITY_RAISE_RATIO      = 0.70  -- raise 1 tier (with corroboration) when ratio < 0.7.
M.CAPACITY_RAISE_FREE_RATIO = 0.40  -- raise 1 tier (skip corroboration) when ratio < 0.4.
M.CAPACITY_KILLED_CUT       = 3     -- tiers to drop when wasKilled (host force-killed last tick).
M.CAPACITY_KILLED_AVOID     = 200   -- ticks to avoid a tier that just got killed at.
M.CAPACITY_RAISE_HEADROOM   = 0.90  -- raise only if next-tier history < target * this.
M.CAPACITY_DEFAULT_TIER     = 10    -- start at full quality; throttle on observed pressure.
-- Test override: when non-nil, ignore brain.targetMs and pretend this is
-- the per-bot budget. Lets you force the throttle tiers to engage even
-- when the host has plenty of headroom. Set to 0.75 to drive the bot
-- into low tiers and verify the levers actually fire. nil = use host's
-- published targetMs (production behavior).
M.CAPACITY_FORCED_TARGET_MS = nil

-- ── Squad coordination (Phase 1: pill blitz) ──────────────────────────────
M.HARASSER_FRAC       = 0.20   -- fraction of the protocol-bot set that are harassers (floor)
-- Dynamic harasser ramp: once we hold a clear BASE advantage (base_strength past
-- HARASSER_BASE_THRESHOLD) AND aren't bleeding pills (pill strength at/above
-- HARASSER_PILL_FLOOR), raise the harasser fraction from HARASSER_FRAC up toward
-- HARASSER_FRAC_MAX — dominating bases while holding pills means we can spare more
-- bots to harass. Below the base threshold, or while losing a lot of pills, it
-- stays at the HARASSER_FRAC floor.
M.HARASSER_FRAC_MAX       = 0.50  -- harasser fraction ceiling at full base dominance
M.HARASSER_BASE_THRESHOLD = 0.60  -- base_strength (friendly/contested) where the ramp starts
M.HARASSER_PILL_FLOOR     = 0.40  -- min pill strength to allow ramping ("at least not losing a lot")
M.BASELINE_SQUAD_SIZE = 3      -- commanders = ceil(non-harasser count / this)
M.SQUAD_ALLY_MAX_AGE  = 1750   -- ticks; allies staler than this drop out of the protocol set

-- ── 2026-06-01 brainstorm features — ALL DEFAULT OFF (flip to test) ─────────
-- New behaviors land behind flags so the verified post-merge brain is
-- unchanged until each is enabled. See SQUAD_BRAINSTORM_DECISIONS_2026-06-01.md.
M.DYNAMIC_COMMANDERS   = true   -- R0: commander = whoever is mid hard-take (else current deterministic roles)
M.HARD_TAKE_MIN_HP     = 12     -- R0: pill HP at/above which a take is "hard" (spawns a squad/commander)
M.CIRCLE_REINFORCE_ENABLED = true  -- R3-exec: uncommitted bots reinforce nearest losing circle
M.CIRCLE_REINFORCE_TIMEOUT = 1500  -- R3: base ticks (~30s) to reach the safe tile; scaled by travel distance
M.UTIL_ACTIVE_PICKUP   = true   -- R1b: actively pick up a back pill to hit the util reserve target (STUB — no-op)
-- Harasser model: a harasser is just a normal GoalHunter bot whose attack_pill
-- cost is multiplied by this, biasing it AWAY from the (congested) pill economy
-- and toward bases / tank fights / defense via ordinary goal selection. The old
-- R4 base-stealer mission (HARASSER_TAKE_BASES / _REAR_PUSH / _MINE) was scrapped.
M.HARASSER_PILL_COST_MULT = 5.0
-- Harasser distance de-emphasis: every distance-tied cost term in a harasser's
-- combat goals (attack_pill travel, attack_tank / kill_lgm path_cost + LOS
-- per-tile + far-preempt) is multiplied by this, so harassers roam far to fight
-- instead of being pinned near home. 1.0 = no discount; lower = ranges farther.
M.HARASSER_TRAVEL_MULT = 0.2
-- Recruitment (slice 2): a soldier answers a nearby commander's pill take when
-- it's in a follow-the-call state and not too low on resources.
M.SQUAD_MIN_HELP_SHELLS  = 3   -- below this shells a soldier won't answer (hard decline)
-- Armour needed to OPEN/LEAD a blitz (be its commander). Joining has no armour
-- floor — a partner shares the fire — EXCEPT while carrying a pillbox (cautious
-- mode): then a joiner needs commander-level armour too, so it doesn't risk the
-- pill it's holding by diving in weak. Established leaders aren't demoted if
-- their armour later drops (avoids abandoning a take mid-flight).
M.SQUAD_COMMANDER_MIN_ARMOUR = 30
M.SQUAD_REFUEL_OK_ARMOUR = 25  -- a refueling soldier may answer only if already at/above this
M.SQUAD_REFUEL_OK_SHELLS = 10  -- ...and this (i.e. it was topping off, not desperate)
M.SQUAD_HELP_RANGE       = 30  -- tiles; only answer a commander whose pill is within this
M.SQUAD_CMD_RACE_TOL     = 3   -- ticks; two blitz calls on one pill opened within this of each other count as a same-tick race (broken by lower player id); otherwise first-to-the-take keeps command
M.SQUAD_BLITZ_AIM_TOL    = 8   -- brad; a blitz soldier must be facing the pill within this before it reports rdy=1 (so on GO it can fire/charge immediately, not spin to aim)
M.SQUAD_MAX_SIZE         = 1   -- max SOLDIERS per squad; with the commander that's 2 tanks total per blitz. A full squad recruits no more
M.SQUAD_BLITZ_COST       = 30  -- flat attack_pill cost a squad soldier assigns its commander's blitz pill: low enough to win normal goals, high enough that attack_tank/flee/refuel can still preempt
M.SQUAD_BLITZ_BUCKET     = 5   -- a blitz standoff is picked at random from clear-LOS spots scoring within this of the best
M.SQUAD_BLITZ_GO_EARLY_READY = 2   -- commander fires GO as soon as this many TOTAL blitzers are ready (in position + aimed), without waiting for the rest or the READY_TIMEOUT. Counts the commander as 1 (same convention as BLITZ_MIN_READY_TO_CHARGE), so 2 = commander + 1 ready soldier already goes; a still-approaching extra joins on the broadcast GO. Set to 3 to require commander + 2 soldiers.
M.SQUAD_BLITZ_READY_TIMEOUT = 550  -- ticks (~11s @ 50Hz) the commander waits in blitz_wait for ALL soldiers to report rdy before firing GO anyway. Sized to cover a worst-case in-place aim: a 180-deg turn on swamp/crater/river/rubble (turn rate 0.25 brad/tick) is ~512 ticks (~10.4s), so the timeout must exceed that or the commander GOes before a slow-terrain soldier can finish turning to face the pill. (A genuinely stuck/dead soldier still can't stall past this.)
M.SQUAD_BLITZ_WAIT_TIMEOUT  = 1500 -- ticks (~30s) hard backstop: a soldier holding in blitz_wait abandons the take if the commander's GO never arrives (commander silently stuck/disconnected). Faster aborts (commander died / retargeted) fire on their own signals.
M.SQUAD_BLITZ_PROGRESS_CHECK = 250  -- ticks (~5s): commander re-checks soldier approach this often in blitz_wait; if the closest still-coming soldier got closer since last check, the ready-timeout is extended by another PROGRESS_CHECK (keep waiting on a tank that's still closing; stop extending once it stalls). Also the size of the grace a near-deadline "where are you?" query (SQUAD_BLITZ_QUERY_LEAD) buys an unseen-but-closing soldier.
M.SQUAD_BLITZ_BD_REFRESH_TICKS = 500  -- ticks (~10s): how often a COMMITTED, still-approaching soldier recomputes its broadcast walk distance (bd) as a FALLBACK signal. The commander measures visible soldiers' progress itself every tick from their live tank positions, so this slow, >=1-tile-quantized broadcast only matters when a soldier is out of the commander's perception — kept coarse to avoid /info spam. A direct commander query (bwq) force-refreshes it immediately when it's about to matter (see SQUAD_BLITZ_QUERY_LEAD).
M.SQUAD_BLITZ_QUERY_LEAD = 50  -- ticks (~1s): when the blitz_wait GO timeout is this close to firing AND a still-pending soldier is out of the commander's sight (so it's relying on that soldier's coarse broadcast bd), the commander broadcasts a "where are you now?" query (bwq). The soldier answers by force-refreshing bd this tick; if the fresh answer shows it's still closing, the commander grants one more PROGRESS_CHECK window instead of giving up.
M.SQUAD_BLITZ_CLASH_TILES = 2   -- two standoffs within this EUCLIDEAN distance (tiles) conflict; the commander makes the nearer/junior soldier repick so spots stay >= this far apart
M.SQUAD_BLITZ_PREEMPT_TANK_TILES = 10  -- a committed blitz only yields to attack_tank when the hostile tank is within this many tiles of us (close enough to actually threaten); farther tanks don't break the blitz
M.BLITZ_ABORT_BUILD_ON_READY = true  -- if a soldier JOINS while the commander is mid build_walls (laying its guard pills/shield), abandon the remaining blocks and rally NOW (build_walls -> blitz_wait, unshielded charge route). The joiner's simultaneous overwhelm replaces the shield as protection — same routing as if the joiner had answered before the in-position decision. Off = finish the shield first, then rally (original behavior).
M.BLITZ_MIN_READY_TO_CHARGE = 2  -- (used with BLITZ_ABORT_BUILD_ON_READY) minimum READY blitzers — total tanks in position and aimed (rdy=1) — required before the commander abandons the build and charges. The commander itself always counts as 1 (it's at its standoff). 2 = commander + one ready soldier; a still-approaching 3rd is left to keep closing and joins the charge when it arrives. Default 2 = original abort-on-join behavior.
-- Outbound /info batching: several internal-channel messages are packed into the
-- one BrainInfo.sendmessage buffer per tick (joined by comms.MSG_SEP), capped here
-- so the packed string stays under PACKET_MAX_CHAT_MESSAGE (128; bot_manager.c
-- truncates there). 124 leaves a few bytes margin. The first message is always
-- accepted even if oversized (matches pre-batch behavior); later ones only join if
-- they fit. Anything that doesn't fit re-queues next tick.
M.MSG_BATCH_MAX = 124
-- Ally right-of-way: brake to let a higher-priority ally (lower player number)
-- pass when our projected path tiles cross theirs.
M.ALLY_YIELD_MIN_SPEED  = 6   -- below this speed a tank can't reach its projected tiles, so it neither yields nor is yielded to
M.ALLY_YIELD_LOOKAHEAD  = 2   -- how many tiles ahead (along heading) to project for both us and the ally when checking for a path crossing
M.SQUAD_BLITZ_REPICK_GAP  = 1   -- min ticks between standoff repicks. Safe at 1: brj rejects are spot-tagged "pn:[mx,my]", so a soldier only repicks when the commander rejected the spot it is CURRENTLY offering — a stale reject for an already-abandoned spot is ignored, so there's no candidate-list burn (see read_cmdr_brj).
-- Pick-to-join blitz discount: while negotiating to join a commander's blitz,
-- multiply the candidate pill's pool-6 cost by a distance factor (<=1, so it
-- only ever discounts). Distance is the dijkstra slate path cost to the pill.
-- Piecewise: <=NEAR -> NEAR_MULT; NEAR..MID -> NEAR_MULT..MID_MULT;
-- MID..FULL -> MID_MULT..1.0; >=FULL -> 1.0 (no discount). A pill whose entry
-- is reject/sentinel'd (no usable cost, e.g. armour_too_low on a hard pill —
-- the typical blitz target) gets factor * REF_COST instead so it's still joinable.
M.SQUAD_BLITZ_JOIN_MIN_MULT    = 0.25  -- biggest blitz-join discount (factor at d=0); exponential rises to 1.0 (no discount) at FULL_TILES
M.SQUAD_BLITZ_JOIN_FULL_TILES  = 20    -- path tiles at which the join discount fully vanishes; beyond this there's no pull to join (the score, not a range gate, decides)
M.SQUAD_BLITZ_JOIN_REF_COST    = 120
M.SQUAD_BLITZ_INRANGE_TILES    = 9    -- euclidean tiles: if an OPEN, not-full blitz pill (or the blitzing commander's tank) is within this shooting distance of our tank, stack an extra flat discount on its attack_pill (we could already help kill it). ~tank gun range + slack.
M.SQUAD_BLITZ_INRANGE_MULT     = 0.5  -- the extra in-shooting-range multiplier (0.5 = another -50% on top of the distance-curve join discount)
M.AMMO_DEPRIVED_BLITZ_MULT     = 0.6  -- ammo_deprived bots: flat overall multiplier on the blitz-join factor (40% off, near AND far — a far call at factor 1.0 becomes 0.6, joinable) so a useless-for-shooting tank throws itself into blitzes more readily; distance shape unchanged
-- If blitz_negotiate can't find an appropriate standoff for a blitz pill, reject
-- that pill (no re-discount / no re-pick) for this many ticks (~30s @ 50 tps).
M.SQUAD_BLITZ_NOSPOT_REJECT_TICKS = 1500
-- Negotiation watchdog: a soldier offering to join a blitz holds goal=none
-- (blitz-negotiation pause gate in goals.lua) until the commander accepts. A
-- healthy commander rosters a conflict-free soldier within a tick or two; if no
-- accept arrives within TIMEOUT ticks (wedged commander, perpetual spot clash,
-- unreachable), abandon the negotiation and reject that pill for REJECT ticks so
-- the soldier stops re-picking it and does something else instead of idling.
M.SQUAD_BLITZ_NEGOTIATE_TIMEOUT_TICKS = 250   -- ~5s awaiting accept before giving up
M.SQUAD_BLITZ_NEGOTIATE_REJECT_TICKS  = 500   -- ~10s cooldown on the abandoned pill
M.SQUAD_BLITZ_COMMIT_GRACE_TICKS      = 5     -- after committing to a blitz, wait this many ticks before the "pulled to another goal -> leave the squad" check fires (lets pick_goal adopt the blitz attack_pill goal post-accept)
-- Comm-line latch: how long a one-tick "busy/declined/no-spot" response stays
-- drawn in blitz_comm_lines so transient rejects are actually visible (~0.4s).
M.BLITZ_COMM_LATCH_TICKS       = 18
-- Urgent replan when a tank WE CAN SEE dies. Any visible enemy death triggers;
-- an ally death only triggers if within ALLY_RANGE tiles (manhattan) AND there's
-- a DROPPED pill to grab — a dead (not-in-tank) pillbox within PILL_RANGE tiles
-- (euclidean). The whole point of the ally-death interrupt is noticing pills the
-- dead teammate dropped; with nothing to grab it only churned a live goal to none.
M.TANK_DEATH_REPLAN_ALLY_RANGE = 20
M.TANK_DEATH_REPLAN_PILL_RANGE = 12
return M
