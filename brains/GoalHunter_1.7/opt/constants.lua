-- =========================================================================
-- GoalHunter/constants.lua ??? terrain costs, tuning parameters
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
-- Derived from engine mapGetSpeed() in bolo_map.h ??? cost ??? 16 / max_speed.
-- Road=16, Grass=12, Forest=6, Swamp/Crater/Rubble=3, Building=0.
-- Swamp/crater/rubble are 5.3?? slower than road on paper, but the low
-- max-speed (3) means ~85 ticks per tile vs ~16 on road, and the tank is
-- vulnerable to pill fire the entire time.  We add extra weight (??1.5) to
-- discourage routing through slow terrain near threats.
M.TERRAIN_COST_LAND = {
  [M.T_ROAD]      = 1,     -- speed 16
  [M.T_REFBASE]   = 1,     -- speed 16
  [M.T_GRASS]     = 2,     -- speed 12 ??? 16/12 ??? 1.3, rounded up
  [M.T_BOAT]      = 2,     -- speed 16 (embarkation point)
  [M.T_FOREST]    = 3,     -- speed 6  ??? 16/6 ??? 2.7
  [M.T_RUBBLE]    = 8,     -- speed 3  ??? 16/3 ??? 5.3 ?? 1.5 vulnerability weight
  [M.T_SWAMP]     = 8,     -- speed 3  ??? 16/3 ??? 5.3 ?? 1.5 vulnerability weight
  [M.T_CRATER]    = 8,     -- speed 3  ??? 16/3 ??? 5.3 ?? 1.5 vulnerability weight
  [M.T_PILLBOX]   = 2,     -- only passable if dead; cost approximates terrain underneath (grass)
  [M.T_RIVER]     = -1,    -- dynamic: cost depends on ammo (see sq_cost)
  [M.T_DEEPSEA]   = 9999,  -- instant death without a boat
  [M.T_BUILDING]  = 9999,  -- speed 0 (wall)
  [M.T_HALFBUILD] = 9999,  -- speed 0 (wall)
  [M.T_UNKNOWN]   = 3,     -- unexplored: assume passable, cost like forest
}

-- Per-terrain tank MAX SPEED (WU/tick), mirrored from the engine's
-- MAP_SPEED_T* table (bolo_map.h). Used by the tank-combat lead calc to
-- forward-simulate a target's terrain-limited path over the shell's flight
-- (a shell can outlast a road->swamp transition, so a constant-velocity
-- extrapolation misses). Unknown/unseen tiles assume grass.
M.MAP_SPEED = {
  [M.T_BUILDING]  = 0,
  [M.T_RIVER]     = 3,
  [M.T_SWAMP]     = 3,
  [M.T_CRATER]    = 3,
  [M.T_ROAD]      = 16,
  [M.T_FOREST]    = 6,
  [M.T_RUBBLE]    = 3,
  [M.T_GRASS]     = 12,
  [M.T_HALFBUILD] = 0,
  [M.T_BOAT]      = 16,
  [M.T_DEEPSEA]   = 3,
  [M.T_REFBASE]   = 16,
  [M.T_PILLBOX]   = 0,
  [M.T_UNKNOWN]   = 12,   -- assume grass for unseen tiles
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
--     uses FRONT_NEAR_RADIUS ??? a pill hugging the line still does front duty.
--   PLACING (what category a candidate TILE would fill: the strategic scan,
--     its heatmap, trail-drop, building-intent viz) uses the strict
--     FRONT_NEAR_RADIUS_PLACE=0 ??? a new pill only counts as filling the
--     front role if it stands ON a front-band tile (the yellow-circle "3"
--     overlay). The 2-tile halo let a repositioned pill's OLD spot pass as
--     a "front" cell and dodge the back-surplus skip ??? rebuild-in-place.
M.FRONT_NEAR_RADIUS       = 3  -- identify: existing pill counts front within this of the band
M.FRONT_NEAR_RADIUS_PLACE = 0  -- place: candidate tile must be ON the band to fill "front"
M.MIN_TREEHIDE_DIST_MAP = 3  -- MIN_TREEHIDE_DIST (768) in map tiles
M.CROSSFIRE_MULTIPLIER_ENABLED = false  -- multiply danger by number of pills covering each tile

-- Influence grid tuning (territorial control layer)
M.BASE_INFLUENCE_RADIUS   = 12
M.BASE_INFLUENCE_STRENGTH = 100
M.PILL_INFLUENCE_RADIUS   = 8
M.PILL_INFLUENCE_STRENGTH = 60
-- Influence tail: grow each side's stamped cores outward over passable ground
-- so unclaimed gaps between our pills/bases read as ours (and theirs as
-- theirs), and the front line forms where the two tails meet instead of at
-- the disc edges. Merged into the same signed grid, so every consumer and the
-- '1'/'3' overlays see it without change. See brain_pathfinder.c.
M.EXPAND_ENABLED       = true
M.EXPAND_SEED_MIN      = 20   -- |stamped| at/above this is a core the tail grows from
M.EXPAND_RADIUS        = 10   -- steps the tail reaches (one pill range plus a bit)
M.EXPAND_START         = 15   -- value at the core edge, 0 at RADIUS. Below every
                              -- hard threshold (+-50 goal mults, -20 deep-enemy take)
M.EXPAND_NEUTRAL_STEP  = 3    -- step cost inside a live NEUTRAL pill's range:
                              -- the tail penetrates ~1/3 as far and fades 3x faster
M.EXPAND_WATER_STEP    = 2    -- shallow water / boat tile. Deep sea, buildings,
                              -- walls and pillboxes block the tail outright
M.EXPAND_DEEP_MARGIN   = 4    -- tiles within this many tiles (king-move / Chebyshev)
                              -- of deep sea or of the map edge (off-map counts as
                              -- deep sea) are never claimed by the tail: no tail
                              -- value, and no growth passes through them, so the
                              -- front line stops short of the shore instead of
                              -- being drawn out over the water. Stamped cores are
                              -- unaffected (they still seed). 0 = old behaviour
M.INFLUENCE_ENEMY_TAIL = false -- does the ENEMY's influence grow too?
                              -- true = the old behaviour: both sides grow, and the
                              -- two tails cancel inside one signed grid, so the
                              -- front line settles midway and the blank ground
                              -- between the sides is split evenly.
                              -- false (default) = only OUR cores grow. Enemy bases
                              -- and pills keep their raw stamped discs (the stamp
                              -- radii above are untouched) but spread no further,
                              -- so nothing cancels our tail and unclaimed ground
                              -- reads as ours right up to the edge of their disc.
                              -- The merge still keeps the larger magnitude, so an
                              -- enemy footprint is never overwritten -- only its
                              -- faint outer ring (|stamp| < EXPAND_START) can flip.
                              -- Pushes the front line outward: more ground counts
                              -- as claimed, so placement, the back/front/aggro
                              -- portfolio and the beyond-front penalty all
                              -- encourage the bots to take and hold more land.
M.EXPAND_REFRESH_TICKS = 250  -- backstop rebuild; normally only on a stamp-set change
M.EXPAND_DEBUG_MAP     = false -- debug only: TAIL_MAP print2 (ASCII 64x64 around the
                              -- tank on every rebuild). Off for normal play
-- classify(): an unclaimed (0) tile away from any front line. true = "front"
-- (today's behaviour: fillable, placement can land there); false = "beyond"
-- (not a fillable role; the placement scan pays the beyond-front penalty).
M.FRONT_ZERO_IS_FRONT  = true

-- Pill anger decay: engine takes ~3000 ticks (speed 6->100, +1 every 32 ticks)
M.PILL_ANGER_DECAY = 3000
-- Per-hit anger increment. 1/3 means three hits saturate to fully angry.
M.PILL_ANGER_BUMP  = 0.3333

-- Wall shielding: how much protection walls give against pill fire
M.WALL_HP_FULL     = 5    -- shots to destroy a full wall (T_BUILDING)
M.WALL_HP_HALF     = 2    -- shots to destroy a half wall (T_HALFBUILD)
M.WALL_SHIELD_TIME = 200  -- ticks of wall cover = full shielding (~4 seconds)

-- River cost tuning ??? high base cost to discourage routing through water.
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
-- during normal (non-opening) play it's flagged state.ammo_deprived ??? a lost
-- cause for resupply, so it goes all-in (joins any blitz, suicide-charges the
-- pill instead of holding at standoff). Cleared the moment shells recover to
-- AMMO_DEPRIVED_SHELLS. 50 ticks/sec, so 6000 = 120 s.
M.AMMO_DEPRIVED_SHELLS = 10             -- "min ammo" line for deprivation: the clock runs only while shells are BELOW this and resets the instant they recover to it. Lowered 20->10 so a tank isn't tracked toward decoy until genuinely low (<10) and sheds decoy status as soon as it's back to 10 (was: clock ran below 20 and only reset once shells climbed all the way back to 20)
M.AMMO_DEPRIVED_TICKS  = 3000           -- 60 s continuously below it (was 120 s ??? bots moped too long before going decoy)
-- A deprived bot is a SUICIDE decoy ??? it doesn't care about shells OR armour, so
-- refuel is heavily deprioritised (x this) instead of special-cased per-resource.
M.AMMO_DEPRIVED_REFUEL_MULT = 3         -- multiply refuel/flee cost while ammo_deprived (so attack_pill/blitz out-bids it)
-- Max time a bot stays deprived before the flag auto-resets: gives it a window to
-- actually refuel again; if still starved it re-earns deprivation ~60 s later.
-- Also reset on death (init.lua).
M.AMMO_DEPRIVED_MAX_TICKS   = 15000     -- ~5 min @ 50Hz

-- TEST AID (set 0 before merging to a release): freeze EVERY bot for this
-- many ticks at game start ??? outputs zeroed, brains still tick ??? so a human
-- can grab most of the map first and then watch ammo-deprivation/decoy
-- behavior kick in. Countdown printed to the console every 10 s by bot 0.
M.BOT_TEST_GLOBAL_HOLD_TICKS = 0        -- off (superseded by TEST_NEVER_REFUEL_CHANCE)

-- TEST AID (set 0 before merging to a release): each bot rolls this chance
-- at first think to become a NEVER-REFUEL bot ??? every refuel candidate is
-- rejected ("test_no_refuel" in the pool grid), so it burns its shells and
-- hits ammo-deprivation naturally, exercising the decoy path on any map.
-- Console prints "[TEST] bot N NEVER-REFUEL" at roll time; the followed
-- bot shows a red "TEST: NEVER-REFUEL" HUD banner (viz id test_no_refuel).
M.TEST_NEVER_REFUEL_CHANCE = 0        -- off (use -bot-init [ammoless] to target a specific bot)

-- PPT-force thresholds. PPT (Protected Pill Take) is normally only
-- chosen for high-HP pills (>= PPT_HEALTH_THRESHOLD). These knobs let
-- low-armour situations force PPT even on a soft pill, because the
-- bot can't afford to take return fire while charging:
--   - armour <= ARMOUR_LOW: always force PPT regardless of standoff
--     danger (we're one or two hits from flee territory).
--   - armour <= ARMOUR_MODERATE AND standoff danger >= ARMOUR_MOD_PPT_DANGER:
--     mid-armour and the chosen standoff is hot ??? too risky to charge
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
-- and we don't bounce right back onto it.  50 ticks/s, so 1500 ??? 30s.
M.PILL_DANGER_NEARBY_RADIUS = 3       -- 7x7 grid (radius 3) centered on pill
M.PILL_DANGER_NEARBY_TICKS  = 1500    -- ~30 s of cooldown
M.PILL_DANGER_NEARBY_MULT   = 1.5     -- 1.5x cost while stamp is active
-- Ally-avoid overlay cost ??? stamped on the 5x5 around an allied tank
-- doing a pill take (and the firing lane to the pill).  Set to roughly
-- half of STUCK_PENALTY (1500) so A* visibly routes around it without
-- treating it as a hard wall ??? and well above the tiny noise the prior
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
-- Low-stock markup (author, 2026-09-03): a base that gets PAST the low_stock
-- reject (it can supply at least one thing we need) but whose observed stock
-- of a resource we DO need is below REFUEL_MIN_STOCK is 10% dearer for each
-- such resource. Multiplicative on the base's whole cost, so it scales with the
-- trip; both short at once compounds to x1.21. Fresh observations only (within
-- REFUEL_OBS_STALE) -- an unseen or stale base pays nothing here, the staleness
-- terms own that. Compared the way the reject compares (see
-- refuel_low_stock_mult in goals.lua for the obs_armour units caveat).
M.REFUEL_LOW_ARMOUR_MULT = 1.10  -- need armour and obs_armour < REFUEL_MIN_STOCK
M.REFUEL_LOW_SHELLS_MULT = 1.10  -- need shells and obs_shells < REFUEL_MIN_STOCK
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

-- Turn penalty: cost per 45?? of direction change at each A* node.
-- Accounts for real game-time lost to deceleration, turning, re-acceleration.
-- 90?? turn = 2 steps ?? 2 = 4 cost.  Example: 7 turns ?? 4 = 28 extra cost
-- makes a winding maze path less attractive than a straight wall-shoot path.
M.TURN_COST_PER_45  = 2

-- Wall-shooting pathfinding
M.WALL_SHOOT_COST   = 30   -- A* cost to path through a wall (5 shots + rubble traverse)
M.SHELL_RESERVE     = 0    -- let tank use all shells to break walls

-- Road-building pathfinding
-- Terrain types worth building roads on (maps terrain type ??? tree cost)
M.ROAD_BUILD_TERRAIN = {
  [M.T_RUBBLE]  = 2,   -- LGM_COST_ROAD
  [M.T_SWAMP]   = 2,   -- LGM_COST_ROAD
  [M.T_CRATER]  = 2,   -- LGM_COST_ROAD
  [M.T_RIVER]   = 2,   -- LGM can build 1 tile into river from land edge
}
M.ROAD_BUILD_COST    = 2    -- A* cost for a tile we plan to pave (= road cost, since it will become road)
M.TREE_RESERVE       = 4    -- BASE road-build reserve. Non-emergency roads use builder.road_tree_reserve(): this base + PILL_PLACE_TREE_COST per carried pill (uncapped GUARANTEE ??? always keep enough wood to deploy every carried pill) + ceil(worst nearby friendly pill deficit / PILL_REPAIR_AMOUNT) repair trees (???4). Roads only get built from genuine surplus. (The drowning-emergency road bypasses all reserves.)
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
-- Two calm neutral pills at 3 tiles produce danger ??? 20.
--   REFUEL: 20 ?? 20 = 400  ??? prefer safe base up to 400 path-cost units further
--   FLEE:   20 ?? 80 = 1600 ??? at critical armour, cross most maps to reach safety
M.REFUEL_DANGER_WEIGHT  = 20
-- Half a tile diagonal (sqrt(2)/2 = 0.7071), in TILES: the farthest a tank
-- parked on a base tile can sit from that tile's centre.  Used ONLY by the
-- refuel-pad reach test (goals.lua refuel_pad_pill_reach), which decides
-- whether a pill can shoot a tank while it DOCKS -- never by the driving
-- stamp.  The engine (pillbox.c -> util.c utilIsItemInRange) fires when the
-- euclidean distance from the PILL's tile centre to the TANK's world position
-- is <= PILLBOX_RANGE (2048 wu = PILL_FIRE_RANGE 8 tiles), so a pill can touch
-- a docked tank only when dist(pill tile, base tile) <= PILL_FIRE_RANGE + this
-- (= 8.7071 tiles).  Incident 20260903_193428 bot2 t=1563: refuel candidate
-- base#0 was priced raw 9 + base 45 + danger 427 (x danger{1.33}) = 640.9 with
-- the tank on 10 armour two tiles away, and lost to a base twelve tiles off at
-- 301.  The 427 was the RIM of the PILL_RANGE_MAP(9) danger stamp -- an angry
-- pill at exactly 9.0 tiles, which cannot hit that base tile from anywhere on
-- it.  The stamp keeps its 1-tile pad (it steers the DRIVE); the refuel pad
-- read now drops the pill layer when no pill reaches the pad.
M.REFUEL_PAD_TANK_OFFSET = 0.7071
-- Danger/threat-term multiplier applied across goal cost formulas
-- when state.cautious_mode is true (see init.lua).  Cautious mode
-- triggers on conditions like "LGM dead AND carrying pills" ??? we
-- can't afford to lose what we're carrying, so the danger terms
-- in cost formulas get pumped to bias hard toward safer routes /
-- targets.
M.CAUTIOUS_MODE_MULT = 5
-- PENALTY applied to refuel_at_base cost when the base's tile danger value is
-- > 0 (an EXPOSED refuel spot). Multiplies the final cost so refueling out in
-- the open is less attractive; a truly safe base (danger 0) keeps its raw cost
-- (no discount). Was a 0.75 safe-DISCOUNT, which made refuel too attractive
-- overall ??? flipped to a 1/0.75 unsafe-penalty (same safe:unsafe ratio, higher
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
-- 3 tiles ?? 256 WU = 768.
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
-- defense (attack_tank, flee) is unaffected ??? only sibling pill
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
-- time_factor ??? fully active at age=0 (??0.5), back to ??1.0 once
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
M.TANK_FINISH_MAX_ANGER = 0.25 -- ...AND the pill is currently CALM (anger <= this). An angrier pill reloads fast, so don't soak its last HP ??? swerve instead.
M.PPT_STANDOFF         = 7.0   -- pull in slightly closer than ATTACK_PILL_STANDOFF
M.PPT_CHARGE_MAX_SPEED = 4     -- speed cap during PPT charge (creep, not rush)
-- Cautious "boat in a river" cruise when on/stepping onto an ally's pill-take
-- ring (init.lua). info.speed is raw engine??4 (0..64; 16 max ??? 64). 28 ??? engine
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
M.CHARGE_SUICIDE_STANDOFF = 1.5  -- ammo_deprived suicide charge: drive this close to the pill instead (??? adjacent; the pill tile is solid so can't go onto it)
-- Max ticks to wait at the approach point for the LGM to gather enough
-- trees for the shield walls before giving up and degrading to a
-- no-shield (legacy aim/charge) attack. 1500 = 30 s @ 50 Hz.
M.PPT_GATHER_TIMEOUT   = 1500
M.ATTACK_APPROACH_OFFSET = 2.25 -- tiles beyond standoff to start approach from
-- Precise final-approach homing: A* only has to get us within APPROACH_PRECISE_DIST
-- of the exact float approach point (approach_fx/fy); inside that the nav switches
-- to the exact-center creep so we land within APPROACH_PRECISE_TOL wu of it (the
-- in-position transition needs 16 wu ??? the loose 64 wu nav default parked us short
-- of it and stalled). Sized so the tank can brake from speed to the creep cap.
M.APPROACH_PRECISE_DIST = 384  -- wu (~1.5 tiles): engage exact creep within this of approach_fx
M.APPROACH_PRECISE_TOL  = 16   -- wu: homing tolerance (matches attack.lua approach DIST_TOL)
-- predict_stop: speed at/below which the brake sim stops early. MUST be 0 ??? the
-- sim loops `while speed > min_speed`, so any value >0 makes it return the tank's
-- CURRENT position (0 steps) whenever entry speed <= it, badly under-predicting
-- the stop at low speed (e.g. 4 ??? predicted 0 wu vs real ~24 wu) and causing a
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
-- 200 ??? comparable to water_pen, less than crossfire. Keeps retreat
-- on low health from dumping us in enemy territory.
M.ATTACK_STANDOFF_INFLUENCE_WEIGHT = 2.0

-- plan_position: terrain analysis for best attack spot
M.ATTACK_SCAN_DEGREES       = 5    -- degrees per step around circle (72 spots at 5??)
M.ATTACK_SAFE_RADIUS        = 3    -- tiles around standoff to check for danger/maneuver room
M.ATTACK_DANGER_THRESHOLD   = 30   -- max total score to be considered safe
M.ATTACK_DANGER_HOTSPOT     = 15   -- any tile in maneuver area above this triggers B penalty

-- Standoff-spot scoring for a pill take. The spot scan now picks, per spot, the
-- point on the pill it can actually hit (centre or one of the 4 corners) and
-- REJECTS the spot outright when no aim point has a clean shell path — see
-- attack.spot_clear_aim. So the only thing left to score is how many trees sit
-- on the chosen line: each one eats a shell before the pill takes any.
M.STANDOFF_SHOT_TREE_PENALTY    = 8    -- per forest tile on the CHOSEN aim path
-- RETIRED (kept so old saves / any stray reference still resolve): a live pill,
-- a base or a wall on the line used to be a 200-point nudge on the CENTRE path.
-- A nudge is not a defence — 20260831_173448 bot2 took a spot with our own pill
-- dead on the centre line anyway because the rest of it scored well. Blocked
-- spots are now hard-rejected instead of penalized.
M.STANDOFF_SHOT_BLOCKED_PENALTY = 200

-- Pill-take spots that sit deep inside enemy influence are much harder to
-- hold during the take. In mid/late game (phase != "opening"), multiply
-- those spots' total_score so a hostile-territory take ranks well below
-- a friendly/contested-territory alternative.
M.PILL_TAKE_HOSTILE_INF_THRESHOLD = -20  -- influence at or below = deep enemy territory
M.PILL_TAKE_HOSTILE_INF_MULT      = 5.0  -- score multiplier applied to such spots

-- Attack substates (approach ??? engage ??? reposition)
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
-- max gunsight range (~7 tiles) from the pill, offset 25?? from the wall line
-- so shells travel at an angle that clears the wall tile.  The wall is adjacent
-- to the pill on the side facing the tank, catching most pill return fire.
-- Prebuild position (9 tiles) is outside the pill's 8-tile firing range.
M.WALL_SHIELD_ENABLED      = true   -- enable wall-shield tactic
M.WALL_SHIELD_MIN_TREES    = 4      -- minimum trees to attempt wall-shield
M.WALL_SHIELD_STANDOFF     = 7      -- desired tank distance from pill (map tiles) = max gunsight range
M.WALL_SHIELD_WALL_DIST    = 1      -- wall placement distance from pill (map tiles)
M.WALL_SHIELD_BUILD_COST   = 2      -- trees consumed per wall build
M.WALL_SHIELD_STANDOFF_ANGLE_OFFSET = 10 -- degrees offset from wall line so shells clear the wall (10?? at 7 tiles = ~1.2 tile offset, enough to miss 1-tile wall)
M.WALL_SHIELD_PREBUILD_STANDOFF = 9  -- prebuild position distance from pill (outside 8-tile range)
M.WALL_SHIELD_RETREAT_ANGLE    = 90  -- degrees perpendicular to pill->wall line for retreat
M.WALL_SHIELD_LGM_SAFE_TICKS  = 10  -- min ticks after LGM returns before shooting
M.WALL_SHIELD_LGM_TRIP_WEIGHT = 0.5 -- weight for LGM round-trip ticks in wall candidate scoring
M.WALL_SHIELD_LGM_MAX_TICKS   = 2000 -- max ticks to simulate LGM travel
M.PPT_BLOCKERS_ENOUGH         = 1   -- protected-take build phase ends in SUCCESS as soon as this many blockers are NEWLY placed (wall or dropped pillbox) ??? but ONLY when a blitz is underway (see PPT_BLOCKERS_ENOUGH_MIN_INWAIT / BLITZ_MIN_READY_TO_CHARGE). Solo, the full planned shield is built. 1 = one blocker is enough cover once the squad is overwhelming the pill.
M.PPT_BLOCKERS_ENOUGH_MIN_INWAIT = 1  -- the one-blocker early-success also applies while the commander is still building IF at least this many soldiers are already parked in blitz_wait (sharing the pill's fire). Pairs with BLITZ_MIN_READY_TO_CHARGE (the ready-to-charge quorum) as the other trigger.
M.SHIELD_ADJ_FRIENDLY_PENALTY = 3  -- shield-slot score: per would-be blocker slot that touches (8-neighbour) an existing friendly pill. A PENALTY, not a ban: a pill on such a slot doubles the double-take exposure of both pills and rarely adds cover the neighbour doesn't already give, but there are takes where it is still the right slot. Under a wall's 5 cover so it only breaks near-ties. (20260831_092854 bot2 t=8413: pill #5 dropped at (127,140) beside pill 6 at (127,139).)
M.SANITY_PILL_REPLANS_MAX = 3      -- attack_pill: consecutive shot-path sanity replans caused by a FRIENDLY pill in the line before the take is abandoned (each one also bans the angle; this is the backstop). SHARED counter across every blocked-line site (standoff sanity, blitz GO gate, charge, shoot_pill) so "spots tried" is one number for the whole take, not one per site.
M.CHARGE_SHOT_CHECK_AT_SPOT_TILES = 1.0  -- charge: how close (tiles) the tank must be to its standoff before the impassable-obstacle shot-path check is allowed to fire at all. While still driving in, the live gun line swings across whatever happens to be beside the target and clears itself on arrival — checking early binned whole takes (loss_b6 bot3 t=9483: a pill at (130,131) crossed the line to pill (129,130) 8 tiles out). Being closer to the pill than the standoff is also counts as "at spot".
M.BLOCKED_AIM_RETRY_TICKS = 10     -- blocked-line ladder: minimum ticks between two 5-aim-point clear-line searches at the SAME site on one goal. The search costs five shell simulations, and after a successful re-aim the gun needs a few ticks to swing onto the new corner, during which the live line is still blocked — so we hold rather than re-search (or abort) every tick.
M.PPT_COVER_TARGET_SHOTS = 15  -- build phase ends once shield cover reaches this many shots-to-break (friendly pill = PILLS_MAX_HEALTH 15, wall = WALL_HP_FULL 5). 15 = one pill OR three walls. Independent of the blitz gate.
M.PPT_PILL_WALL_EQUIV = 3.0    -- a dropped/standing friendly pillbox blocker is worth this many WALLS of shield cover (PILLS_MAX_HEALTH 15 / WALL_HP_FULL 5 = 3). The C combo scorer (gh_shield_stamp) weights a pill-filled slot accordingly, so one carried pill stands in for a 3-wall shield.
M.PPT_PILL_BLOCKERS_MAX = 2    -- cap on how many carried pillboxes the shield planner assumes it can drop onto buildable slots (matches builder.lua's PILLBOX_BLOCKERS_MAX). num_pill_blockers passed to the scorer = min(carried_pills, this).
M.SHIELD_SCAN_BLACKLIST_TRIES = 2    -- a shield.scan that starts but never completes (per-tick budget kill unwinds the whole think) leaves its attempt marker behind; after this many incomplete attempts on the SAME scan key, skip the scan and attack with the no-shield plan instead of livelocking on a scan that can't fit the budget
M.SHIELD_SCAN_BLACKLIST_TICKS = 250  -- blacklist expiry (~5 s): after this long the marker is dropped and the scan may be retried (the key changing ??? pill hp, our armour, standoff ??? also resets it immediately)
M.WALL_SHIELD_LGM_STUCK_TICKS = 150  -- same-tile timeout for LGM simulation (~3 seconds)
-- Last-wall early end: once the FINAL wall blocker is dispatched (LGM out
-- building it) and the estimated LGM round-trip ??? go to the slot + LGM_BUILD_TIME
-- + walk back to the tank ??? is <= this many ticks, count the build phase over and
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

-- Basic pill capture (bpc) ??? circle-strafe attack inspired by human play.
-- Basic pill capture ??? "hardline" technique (from ErYan's replay):
-- Drive to standoff, park, face pill, pump shells.  After a few hits from
-- return fire, hard-turn away ??? the pill's predictive aim overshoots the
-- curve.  Retreat to base, refuel, return to finish.
-- Unified attack pill: curve-away evasion after taking hits
M.ATTACK_CURVE_AFTER_HITS = 2   -- actual return-fire HITS taken before the defensive swerve (one per tick our armour drops; charge/shoot/engage all count it the same way now ??? not armour points)
M.ATTACK_CURVE_TICKS      = 100  -- ticks of swerve dodge (2 seconds)
-- Run the same defensive swerve (ATTACK_CURVE_AFTER_HITS hits taken OR kill
-- locked) during the `charge` substate, not just engage/shoot_pill ??? so a
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
M.SWERVE_COVER_EXTEND_TILES = 0.5 -- swerve side-pick cover lines: extend the OUTER end (the 2-tile perpendicular sample point) this far further from the pill; the pill end stays the origin
M.SWERVE_TOTAL_TICKS      = 95  -- total swerve duration
M.SWERVE_TURN_TICKS       = 35  -- ticks of turning at start of swerve

-- Defensive swerve (pill still alive, took hits or crosshairs off): longer
M.SWERVE_DEFENSIVE_TOTAL_TICKS = 95
M.SWERVE_DEFENSIVE_TURN_TICKS  = 40

-- Early swerve exit: peel off before the full duration if no hostile shell is
-- actually heading at us and we aren't taking damage. The swerve's whole point
-- is dodging the pill's predictive return fire; if nothing is coming near, the
-- remaining straight portion is wasted time.
--   SWERVE_SHELL_NEAR_WU    ??? scan/awareness ring (viz): shells whose RELATIVE
--                             closest-approach to us is within this is shown.
--   SWERVE_HIT_RADIUS_WU    ??? the actual "will it HIT me" test: a shell whose
--                             relative closest-approach lands within this many WU
--                             of us connects, given how we're currently moving.
--                             The swerve stays ARMED while any shell will hit and
--                             ends the instant none do (we've dodged). Tank hit box
--                             is ~128 WU; 160 keeps a small safety margin.
--   SWERVE_ARM_TIMEOUT_TICKS ??? the swerve starts UNLOADED; if no shell is inside
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

-- Seek-trees: when carrying pills we can't afford to place (each needs
-- PILL_PLACE_TREE_COST wood) and we're out of trees, travel to a SAFE forest and
-- harvest instead of deadlocking on a build we can't pay for. The search is
-- map-wide (expanding rings) and prefers the nearest LOW-THREAT forest ???
-- harvesting in enemy territory gets the LGM killed. Late game forest is scarce,
-- so radius is large and distance barely dents the priority: getting wood to
-- deploy carried pills is non-optional.
M.SEEK_TREES_MAX_RADIUS   = 120  -- expanding-ring search cap (near full map) ??? travel far if we must
M.SEEK_TREES_MAX_THREAT   = 8    -- threat.at above this = enemy territory, skip that forest
M.SEEK_TREES_THREAT_WEIGHT = 6   -- ring-score penalty per unit threat (bias to safer trees within a band)
M.SEEK_TREES_INFLUENCE_WEIGHT = 8 -- ring-score BONUS per unit of our influence (favor own-territory forest)
M.SEEK_TREES_MIN_INFLUENCE = 3   -- skip forest whose influence is below -this (clearly enemy territory)
M.SEEK_TREES_RING_SLACK   = 6    -- after finding a forest, scan this many more rings so a safer/friendlier (our-influence) one can still win (kept small ??? the ring scan is O(r^2))
M.SEEK_TREES_CACHE_TICKS  = 150  -- reuse the chosen forest for this many ticks (forest is static) so the map-wide scan runs rarely, not every pool re-eval
M.SEEK_TREES_BASE_COST    = 40   -- base pool cost of the seek-trees goal
M.SEEK_TREES_CARRY_DISCOUNT = 6  -- cost reduction per carried pill (6 pills = strong pressure)
M.SEEK_TREES_DIST_WEIGHT  = 0.4  -- token per-tile distance term (kept small so far forest still wins)
M.SEEK_TREES_MIN_COST     = 4    -- cost floor
M.PILL_PLACE_ENGAGE_AIM = 4     -- aim correction threshold for firing during engage

-- Gunsight
M.GUNSIGHT_MAX = 13.875 -- max sightLen (in half-map-squares); shells travel sightLen/2 map tiles. Pulled back ~1 pixel ??? TODO revisit when shell-hitbox-improvements aim fixes are merged to main

-- Dynamic flee threshold (engage substate)
M.FLEE_ESCAPE_COST_DIVISOR  = 50   -- escape path cost units per +1 armour flee buffer
M.FLEE_SLOW_TERRAIN_BONUS   = 3    -- extra armour buffer when standing on slow terrain

-- Cost-based goal selection (unified scoring ??? all goals compete on same scale)
M.GOAL_SWITCH_PENALTY      = 30    -- base cost added when switching to a different goal group
M.GOAL_TARGET_SWITCH_PENALTY = 15  -- cost added when same group but different target
-- Ally-claimed cost penalty: if any other bot is broadcasting the same
-- goal (matched by kind + target_id, or kind + mx,my for tile-keyed
-- goals) we add this penalty so we prefer a different objective.
-- attack_tank is exempt ??? tank threats are time-critical and locally
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
-- no setup), so essentially ANY cost edge should win the pickup ??? the closer
-- bot grabs it, the other re-routes for free. 0.01 = ">=1% cheaper steals".
-- (The killer-priority window and reposition guards still trump this.)
M.ALLY_CLAIMED_STEAL_FRAC_CAPTURE = 0.01
-- ?????? attack_pill steal NEGOTIATION (stq / sta / str verbs) ??????????????????????????????????????????
-- A cheaper challenger no longer silently takes over an ally's pre-commit
-- attack_pill (the loser stayed committed as a zombie co-attacker ???
-- 20260713_010904_1 t=5022, three bots soloing pill #5). Instead it ASKS:
--   stq <pid> <to> <cost>  "I want to steal this from you, my score is N"
--   sta <pid> <to> <cost>  holder accepts (re-evaluated score Y) + yields
--   str <pid> <to> <cost>  holder rejects (re-evaluated score Y)
-- A holder past "approach" is NEVER stealable ??? it's already moving into
-- position. Only plan_position/approach holders can be asked.
M.STEAL_REQ_COOLDOWN   = 150 -- ticks between requests for the same pill (after send or a reject)
M.STEAL_GRANT_TTL      = 250 -- ticks a received accept stays valid in goal selection
M.STEAL_REPLY_DEADLINE = 20  -- holder answers with its cached score at latest after this many ticks
M.STEAL_REEVAL_MAX_AGE = 40  -- cached pool-6 score older than this defers the reply (waits for a fresh re-eval)
M.STEAL_YIELD_BLOCK    = 300 -- after yielding, don't re-pick that pill for this long (covers the gap until the winner's claim broadcast lands)
-- TARGETED STEAL REQUESTS + EARLY YIELD RELEASE (20260903_193428_1 bot3,
-- t=1266-1270). p4 sent bot3 two steal requests back to back -- `stq 4 3 188`
-- and `stq 0 3 379` -- while p4's own goal was attack_pill #5 and stayed #5 the
-- whole time (it was the commander of the pill-5 blitz). Bot3 yielded both, and
-- its pool 6 then showed pills #0 and #4 ally_claimed by p4 for the full
-- STEAL_YIELD_BLOCK (still up at t=1487) for takes p4 never went near.
-- VARIANT (c) fixes the two mechanical halves of that and nothing else:
--   * a request now goes out only for the row goal selection would actually
--     pick if the claim were lifted, at most one per replan;
--   * a yield block ends the moment the stealer's own advert shows it on
--     something else.
-- The prices on the wire stay the RAW cost_cache costs both sides always
-- traded -- no competed totals, no commitment adjustment, no `cq=` tag --
-- so ALLY_CLAIMED_STEAL_FRAC keeps deciding on exactly the numbers it used to.
M.STEAL_YIELD_RELEASE_GRACE = 60  -- ticks after a yield before a stealer's advert may release it (same incident: the yield block outlived the steal by 200+ ticks). One replan interval plus slack, so the stealer gets a full replan to pick the pill up -- its /info state is event-driven and only re-sends on a goal CHANGE, so its pre-yield goal would otherwise read as "went elsewhere" the very next tick
-- DEPRECATED / unused: the refuel ally-claim FCFS reject (and its
-- far-claimer override) was replaced by the SOFT per-ally cost penalty
-- (ALLY_CLAIMED_REFUEL_PENALTY). Refuel is no longer in _REJECT_POOLS, so
-- there is no first-come claim to override. Kept only to avoid a nil
-- lookup if any stale reference survives; safe to remove later.
M.REFUEL_CLAIM_FAR_TILES       = 10
M.GOAL_COMMITMENT_PER_TICK = 0.5   -- extra switch penalty per tick spent on current goal
M.GOAL_COMMITMENT_CAP      = 75    -- max commitment penalty (reached after 150 ticks / 3s)
M.REFUEL_FULL_COST_MULT    = 8.0   -- pool-1 cost multiplier ceiling when tank is between low and full thresholds; applied at goal-selection time so stale cache costs scale with current state. Ramp is QUADRATIC in fill (see refuel_shape): gentle when genuinely low (fill 0.3 ??? ~??1.6), punishing when nearly full (fill 0.9 ??? ~??6.7) so a near-full tank prices refuel out of contention against real goals (20260703_221238 t=24898: old linear ??3 gave a full-armour/60%-shells tank refuel at ~??2.2 ??? cheap enough to read as top priority). At max fullness the entry is skipped entirely.
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
-- effective top-off mult = 1 + fill??*(REFUEL_FULL_COST_MULT-1)*scarcity
M.REFUEL_SHARE_ENABLED      = true
M.REFUEL_SHARE_RATIO_K      = 1.5  -- how hard base-scarcity (team tanks per friendly base, over 1.0) steepens the top-off ramp. Losing game (few bases, many tanks) => leave at the floor. 0 disables the ratio term.
M.REFUEL_SHARE_LOCAL_K      = 0.5  -- extra ramp per teammate currently within REFUEL_SHARE_LOCAL_TILES (more bots crowding this base now => leave sooner)
M.REFUEL_SHARE_LOCAL_TILES  = 20   -- tiles; an ally tank inside this counts as "locally competing" for the base
M.REFUEL_SHARE_SCARCITY_CAP = 8.0  -- clamp on the scarcity multiplier so the ramp can't explode
M.REFUEL_BASE_HOP_PENALTY   = 500  -- while standing ON a refuel base, every OTHER base costs +this, so an ally claiming our base (ally_claimed) can't bounce us off mid-refuel ??? we finish here. A depleted current base is rejected upstream, which still frees us to move to another base.
-- Mines: never hold the bot at base (REFUEL_MIN_MINES=0 ??? mines never count as
-- "need"), and topping mines past REFUEL_MINE_FREE adds an exponential staying-
-- cost so a mine-rich tank leaves sooner. cost = WEIGHT*(BASE^(mines-FREE) - 1).
M.REFUEL_MIN_MINES         = 0     -- mines below this still count as a refuel need (0 = never wait for mines)
M.REFUEL_MINE_FREE         = 5     -- mines up to here add no staying-cost
M.REFUEL_MINE_HOARD_BASE   = 1.3   -- exponential base for the per-extra-mine cost past FREE
M.REFUEL_MINE_HOARD_WEIGHT = 8     -- scale on the exponential mine-hoard cost term
M.ANGRY_PILL_AT_BASE_PENALTY = 200 -- added to pool-1 cost when an angry hostile pill is in fire range of the base
-- Critical-armour flee: when true, injects a cost=40 flee_to_base candidate
-- into pool 1 so the tank retreats to a safe base. When false (default),
-- relies on the normal pool-1 refuel candidate ??? REFUEL_DEFICIT_BONUS
-- pushes its cost very low at critical armour, so refuel_at_base usually
-- wins naturally without a dedicated flee path. Flip on if you see the bot
-- fighting instead of retreating when almost dead.
--   false  = off (normal refuel handles it)
--   true   = flee whenever armour <= the flee threshold (critical)
--   "no_builder_and_carrying_only" = flee to PROTECT CARRIED PILLS, scaled by a
--     haul-protection LEVEL (0..1) that reflects how badly we need to bail:
--       ??? builder DEAD, or committed OUT on a mission (LGM_MOVING) ??? the pills
--         can't be placed soon and are pure liability ??? FULL protection (1.0) at
--         carry >= 1.
--       ??? builder still in tank ??? we can place them ourselves, so protect only a
--         STACK (carry >= 2), slightly weaker, ramping to full by
--         FLEE_HAUL_FULL_PILLS.
--     Triggers scale with level: a higher level reaches further for a threatening
--     tank (FLEE_HAUL_TANK_RANGE ?? level) and bails at higher armour (ARMOUR_LOW ??
--     level); only full strength also bails on mere hostile-pill coverage.
--     Critical armour always bails.
M.CRITICAL_FLEE_ENABLED      = "no_builder_and_carrying_only"
M.FLEE_HAUL_TANK_RANGE       = 12   -- tiles: an enemy tank within (?? level) counts as "engaging" for the haul flee
M.FLEE_HAUL_FULL_PILLS       = 4    -- have-builder haul protection ramps from carry=2 (weak) to full strength at this count
-- facing_away brake: when true, tank brakes to speed 8 (or 16 under
-- fire/race) if |heading_err| > 64 brad (~90??). Safer for U-turns but
-- sometimes over-brakes when plow + lookahead swing move_dir 132??.
-- Set false to let the tank carry momentum through sharp reorientations.
M.FACING_AWAY_BRAKE_ENABLED  = false
-- U-turn commitment (navigate). When the nav target is nearly straight
-- behind, the shortest-turn side flips with tiny waypoint/heading jitter:
-- the path is re-rooted from the tank's tile every tick, and an equal-cost
-- diagonal alternative makes the next tile alternate NE/SE of the tank,
-- swinging move_dir ~70 brad. Re-deciding L/R each tick then alternates
-- the turn key every ~10 ticks and the tank drives AWAY from its goal at
-- full speed (loss_b5 bot2 t=10203-10370: 6 tiles the wrong way; 23-28
-- such episodes per bot per game). Once |heading_err| exceeds COMMIT the
-- turn side is latched and held while |heading_err| stays >= HOLD; below
-- HOLD the plain shortest-side rule resumes. The latch also drops when
-- the goal changes or another steering mode ran last tick.
M.UTURN_COMMIT_BRAD = 96   -- latch the turn side beyond this |heading_err|
M.UTURN_HOLD_BRAD   = 80   -- keep the latched side down to this |heading_err|
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
--   0.7 = the winner must cost ??? 70% of the current goal's cost to switch
--         (i.e. the winner has to be at least 30% cheaper).
M.GOAL_SWITCH_RATIO        = 0.7
-- Goal history / oscillation detection. The brain remembers the last
-- GOAL_HISTORY_SIZE picked goals; on each new selection, candidates
-- whose (kind, mx, my) or kind appears repeatedly get a gently
-- exponentially growing cost penalty. Catches refuel???capture???refuel
-- style loops where individual targets cycle but the *kind* keeps
-- coming back.
M.GOAL_HISTORY_SIZE        = 10
M.GOAL_HISTORY_EXP         = 1.4  -- penalty = BASE * (EXP^count - 1)
M.GOAL_HISTORY_TARGET_BASE = 25   -- per-(kind,mx,my) repeat
M.GOAL_HISTORY_KIND_BASE   = 8    -- per-kind any-target repeat
-- Ceiling on the COMBINED (target + kind) history penalty. BASE*(EXP^n - 1)
-- is unbounded: with EXP=1.4 a goal that recurred 7?? already carried ~+147 and
-- 15?? carries ~+3.4k, so late in a match a goal the bot legitimately keeps
-- needing (refuel above all) becomes permanently unreachable no matter how
-- cheap it really is. Anti-thrash is meant to break ties, not to remove goals
-- from the game. 150 still dwarfs the switch fee (30) + commitment cap (25).
M.GOAL_HISTORY_PEN_CAP     = 150
M.GOAL_MIN_COMMIT_TICKS    = 25    -- suppress non-urgent replan for this many ticks after a switch
M.GOAL_ABANDON_COOLDOWN    = 0     -- ticks before an abandoned goal can be picked again (0=disabled)
M.WALL_SHIELD_COMMITMENT        = 200  -- extra switch penalty when wall-shield attack is in progress
M.ATTACK_TANK_COMMITMENT_BONUS  = 50   -- extra commitment when currently fighting a tank (see it through)
M.ATTACK_PILL_COMMITMENT_BONUS  = 80   -- extra commitment when mid-attack on a pill; also revokes hysteresis exemption for attack_tank/capture_pill so they can't interrupt for free
-- defend_pill (reactive: a friendly pill is being destroyed NOW) vs an in-progress
-- attack_pill. How hard it is to pull us off the take scales with the take's real
-- investment: FULL hysteresis only while actually shooting/aiming (shells + position
-- at risk), MODERATE while merely building the shield (LGM/trees, recoverable), and
-- FREE otherwise (approach / planning ??? nothing invested yet).
M.DEFEND_ATTACK_BUILD_COMMITMENT = 40  -- moderate commitment defend_pill pays to interrupt a wall-building attack_pill
M.ATTACK_BASE_COMMITMENT_BONUS  = 250  -- extra commitment when mid-attack on a base ??? applied while we still have >=1 shell. Once you start a base, follow through; the ONLY non-urgent reason to break off is literally running out of shells (0). Critical-armour flee still preempts via the urgent goal-override path.
M.CAPTURE_BASE_COMMITMENT_BONUS = 250  -- extra commitment when mid-CAPTURE of a base (driving onto a neutral/ground-down base). Comparable to ATTACK_BASE: if you did the work to grind a base down, follow through and actually take it ??? don't let a normal-cost goal (another base/pill, non-critical refuel) steal it. No shell gate (capturing needs no ammo). Critical-armour flee still preempts via the urgent goal-override path.
M.BLITZ_STANDOFF_SCORE_BUCKET   = 50   -- soldier blitz-standoff pick: ellipse spots are bucketed into score bands this wide; all spots in the best spot's band are the "best pool", and the soldier offers the one CLOSEST to its tank (least travel for ~equal shield quality) instead of the globally-top-scored far spot.
M.EARLY_CAPTURE_BASE_HYST_EXEMPT = true -- opening phase: capture_base skips ALL hysteresis (switch + commit + history), same as capture_pill
M.REFUEL_URGENCY_MIN       = 0.37  -- minimum urgency multiplier for refuel cost
-- Critical-armour need floor, expressed in the "need" convention used across
-- the brain: 0..1, HIGHER = more badly needed. The refuel evaluators carry the
-- INVERSE of it (`urgency`, a cost multiplier where LOWER = more urgent, see
-- eval_refuel / the pool-1 finalize path), so this maps to
-- urgency = min(urgency, 1 - REFUEL_CRITICAL_NEED_MIN) = 0.10.
-- Why: REFUEL_URGENCY_MIN floors urgency at 0.37, so armour=0 with full shells
-- priced refuel identically to armour=14 ??? the shells term is what the min()
-- was clamping against and armour could never speak louder. That left refuel
-- unable to out-bid the anti-thrash stack it had accumulated (switch fee +
-- commitment + the repeat-goal ratchet) and the bot cycled at 0 armour.
M.REFUEL_CRITICAL_NEED_MIN = 0.9   -- at/below ARMOUR_LOW, refuel need is at least this
M.REFUEL_COST_MULT = 1.0  -- whole-cost multiplier on the "refuel" GOAL_GROUP (refuel_at_base + flee_to_base, the same group the pill-suicider surcharge exempts), applied at the ONE selection-layer choke point in goal_selection next to the phase weight / influence / suicider passes. 1.0 = no change. >1 makes a bot resupply less readily (it holds the line longer and dies more), <1 more readily. DEFAULT ONLY: a per-bot "refuel=X" BRAIN_INIT_ARG token replaces it, so read goals.refuel_mult()
M.REFUEL_MIN_COST          = 25    -- routine-refuel cost floor: an on-base top-off otherwise collapses to ~4 and outbids free-pill grabs (20260703_210207 t=5747). Bypassed at ARMOUR_CRITICAL ??? survival refuel may enter the reserved <20 band.
                                   -- (with squared urgency: floor cost at
                                   -- bscore ?? 0.37; e.g. bscore=60 ??? ~22)
M.PILL_HEALTH_WEIGHT       = 5     -- cost per HP of hostile pill (full 15HP pill = +75)
-- ?????? Repair fix master toggle ??????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
-- false ??? repair_pill reverts to the 1.90-beta1 behavior: alive-damaged pills
-- only (no dead-pill rebuild-in-place), plain `max(0, path - dmg*BONUS)` cost,
-- and none of the post-1.90 guards (contested ??3, REPAIR_BASE_COST floor,
-- friendly-fire reject, reposition block) or the influence-exemption. true ???
-- current (post-1.90) behavior. Read live, so flipping this one constant is the
-- whole switch. Released builds set this false until the improvements are tested.
M.REPAIR_FIX_ENABLED       = true  -- repair fix ON (dead-pill rebuild + guards); tested OK
M.REPAIR_BASE_COST         = 30    -- flat floor so a close/damaged repair doesn't trivially out-rank other goals
M.REPAIR_DAMAGE_BONUS      = 3     -- cost reduction per missing HP on friendly pill
M.REPAIR_CONTESTED_MULT    = 3.0   -- repair cost ??N when an enemy tank is closer to the pill than us (contested ??? likely futile)
-- Repair contest terms (2026-09-02 rework). REPAIR_CONTESTED_MULT above is kept
-- as the value and as the name anything older may still read; the live pool-5
-- code calls it REPAIR_UNDER_FIRE_MULT because that is now what it means.
--
-- The old "contested" test was (any enemy tank closer to the pill than we are,
-- at ANY range) OR (a hostile seen within DEFEND_ENEMY_NEAR_RADIUS of the pill
-- in the last DEFEND_SIGHT_FRESH_TICKS = 600 ticks = 12 s). Neither reads
-- whether the pill is actually being SHOT, so a repair stayed priced x3 for
-- twelve seconds after the shooter drove off, and a pill under live fire on the
-- far side of the map read exactly the same as one nobody had touched. Split in
-- two, both keyed on evidence the brain already keeps:
--   UNDER_FIRE      pill.last_hit_tick (world.lua stamps it on REAL damage
--                   only) is fresher than REPAIR_QUIET_TICKS -> shells are
--                   landing on it right now. x3: this repair walks the LGM
--                   into fire and the pill will just be re-damaged.
--   ENEMY_IN_RANGE  a hostile tank we can SEE this tick sits within
--                   PILL_FIRE_RANGE + PILL_REPOSITION_ENEMY_TANK_PAD of the
--                   pill (or is closer to it than we are and inside
--                   DEFEND_ENEMY_NEAR_RADIUS). A soft x1.5: it may start
--                   shooting, it is not shooting yet.
-- The two are exclusive (UNDER_FIRE supersedes) so the desc names exactly one.
M.REPAIR_QUIET_TICKS        = 75    -- ~1.5 s with no fresh hit on the pill = "the
                                    -- shelling has stopped". Also the LGM
                                    -- interlock in builder.lua (see
                                    -- REPAIR_HOLD_UNDER_FIRE_ENABLED) and the
                                    -- defend->repair handoff gate.
M.REPAIR_UNDER_FIRE_MULT    = 3.0   -- = REPAIR_CONTESTED_MULT; repair cost xN while
                                    -- the pill is still taking hits
M.REPAIR_ENEMY_IN_RANGE_MULT = 1.5  -- softer xN for a visible hostile tank near the
                                    -- pill that is not (yet) hitting it
-- ?????? Dead-pill repair (rebuild a friendly 0-HP pill IN PLACE) ?????????????????????????????????????????????
-- The LGM walks out with wood and the engine rebuilds it (lgm.c: a 0-armour
-- pill is the 4??LGM_COST_PILLREPAIR tier). Distinct from alive-damaged repair:
-- capture-in-tank is preferred when SAFE (flexible placement), but in danger
-- rebuilding-in-place risks the expendable LGM instead of the tank, so the cost
-- ignores pill-fire entirely and is driven only by terrain (LGM walk time +
-- reachability) and enemy-TANK snipe risk ??? which melts under a tank-count lead.
M.REPAIR_DEAD_BASE_COST          = 40    -- flat floor; keeps capture (???5 when safe) preferred in calm
M.REPAIR_DEAD_MIN_TREES          = 4     -- 0-HP rebuild needs LGM_COST_PILLREPAIR??4 wood; don't commit with less (engine would only partial-repair)
-- distance curve on TILE distance (mdist): flat in the sweet zone, gentle to
-- the knee, exponential beyond (cross-map repairs self-reject).
M.REPAIR_DEAD_SWEET_TILES        = 8     -- ~shooting distance: flat & attractive within
M.REPAIR_DEAD_KNEE_TILES         = 14    -- gentle rise sweet..knee; exponential past
M.REPAIR_DEAD_NEAR_W             = 4.8   -- cost per tile inside the sweet zone
M.REPAIR_DEAD_MID_W              = 15.6  -- cost per tile, sweet..knee
M.REPAIR_DEAD_EXP_BASE           = 2.0   -- exp growth base past the knee
M.REPAIR_DEAD_EXP_STEP_TILES     = 2.0   -- tiles per doubling past the knee
M.REPAIR_DEAD_EXP_SCALE          = 30    -- multiplier on (EXP_BASE^??? ??? 1)
-- mild terrain surcharge: LGM walk-ticks beyond an all-grass walk ?? this weight.
M.REPAIR_DEAD_TERRAIN_W          = 0.1
M.REPAIR_DEAD_GRASS_TICKS_PER_TILE = 16  -- MAP_MANSPEED_TGRASS; ticks the LGM needs per clear tile
-- snipe: enemy tanks in range of the pill that can pick off the stationary builder.
M.REPAIR_DEAD_SNIPE_RANGE        = 8     -- tiles
M.REPAIR_DEAD_SNIPE_PEN_PER_TANK = 60
M.REPAIR_DEAD_ADV_RELIEF_PER_TANK = 0.25 -- each net friendly tank cuts snipe risk this much
M.REPAIR_DEAD_ADV_FLOOR          = 0.1   -- min snipe multiplier (never fully free)
M.REPAIR_DEAD_LGM_MAX_TICKS      = 2000  -- LGM-travel sim budget (matches other callers)
M.REPAIR_DEAD_LGM_STUCK_TICKS    = 150
M.REPAIR_FRIENDLY_FIRE_REJECT_TICKS = 400  -- 8 s @ 50 Hz: after a friendly shot (own or ally) lands on a friendly pill, refuse to repair it ??? the team is shooting it down to reposition (perception sets pill._friendly_shot_tick)
M.ATTACK_PILL_BASE_COST    = 30    -- flat cost added to every attack_pill (like ATTACK_BASE_EXTRA_COST for bases) so a pill take isn't free vs other goals
M.ATTACK_BASE_EXTRA_COST   = 80    -- flat cost added to hostile base attacks
M.ATTACK_BASE_THREAT_WEIGHT = 3    -- multiplier for threat at base location (penalise bases behind enemy pills/tanks)
-- Close-out: a hostile base whose health (= real armour / 5) is at/below
-- CLOSEOUT_HEALTH is a few shots from neutral. Don't let the normal
-- SHELLS_LOW gate abandon it to refuel ??? finish it with ANY ammo (shells >
-- SHELL_RESERVE), and force the cost low so nothing routine outbids the kill.
-- (Once it neutralizes, attack_base's health>0 filter drops out and
-- capture_base's IMMINENT_CAPTURE_FLOOR=5 takes over to claim it.) A survival
-- refuel at ARMOUR_CRITICAL still prices below this, so we don't suicide.
M.ATTACK_BASE_CLOSEOUT_HEALTH = 3   -- health<=this (armour<=15, ~3 shots) = close it out
M.ATTACK_BASE_CLOSEOUT_COST   = 8   -- forced cost when closing out with ammo (beats refuel's 25 floor, sits above capture's 5)
M.ATTACK_BASE_MAX_WALLS    = 1    -- walls the base shot may cross and still fire (we grind them down). Pillboxes (any owner), other bases, and allied tanks ALWAYS block ??? if the shot isn't valid we drive in for a point-blank shot instead.
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
-- Point-blank orbit escape. At close range the discrete shot-trace can skip the
-- exact base tile even though the engine's base hitbox (wider than one tile,
-- pillbox.c) would register the hit, so the aim-shot latch test never fires and
-- the tank circles hunting a heading that never comes (observed: 600+ ticks of
-- BASE_NOFIRE heading_misses at corr~-95, spd 8, orbiting 2.7 tiles out). When
-- we're this close and the stall window shows no progress, latch to stop+pivot
-- regardless of the trace, and let the fire fallback shoot once we face the base.
M.ATTACK_BASE_POINTBLANK_WU   = 900   -- <= this (wu) = point-blank; ~3.5 tiles
M.ATTACK_BASE_POINTBLANK_CORR = 12    -- brad; fire fallback once body within this of aim

-- Boat shoreline forward-alignment: while threading a shoreline afloat, sprint
-- only when the heading will cross into the same next tile the steering wants;
-- when it wouldn't (a corner-cut), decelerate toward this CRAWL rather than a
-- dead stop, so the tank keeps creeping + turning until it lines up (a full stop
-- stalls ??? it can't re-align a boat without some motion). engine speed ??4.
M.BOAT_ALIGN       = true -- master enable for the boat shoreline forward-alignment throttle
M.BOAT_ALIGN_CRAWL = 8   -- ~2 real wu/tick ??? slow enough that a turn can't corner-cut
M.BOAT_ALIGN_BRAD  = 16  -- heading within this of move_dir (~22??) counts as aligned -> sprint
-- Commit-to-finish: once we put a shot INTO a hostile base, lock onto finishing
-- it (init.lua goal-override). Stays committed until ATTACK_BASE_COMMIT_TICKS
-- after the last shot (refreshed each shot), then releases. Only flee or a tank/
-- LGM within ATTACK_BASE_PREEMPT_SHOOT_TILES preempts ??? don't chase a far tank/
-- LGM off a base we're nearly done shooting down.
M.ATTACK_BASE_COMMIT_TICKS        = 500   -- ~10 s @ 50 Hz since the last shot landed
M.ATTACK_BASE_PREEMPT_SHOOT_TILES = 8     -- tank/LGM must be within this (??? shooting distance) to break the base commit
M.GOAL_CROSSFIRE_PENALTY   = 100   -- goal cost per nearby hostile pill that can crossfire at standoff
M.GOAL_CROSSFIRE_NEW_PILL_BASE = 40  -- attack_tank/kill_lgm: cost for the 1st pill whose fire-range covers the engage spot but NOT our current tile (NEW exposure only)
M.GOAL_CROSSFIRE_NEW_PILL_STEP = 10  -- ...and +this for each additional new-exposure pill (so 40, 90, 150, 230, ...)
M.BASE_PILL_COVER_PEN      = 3     -- capture_base/attack_base: flat cost per enemy pill whose fire-range covers the base tile (clear LOS) but does NOT already cover our current tile. Small per-base nudge toward safer bases; affects which base wins.
M.EXPLORE_BASE_COST        = 2000  -- base cost for exploration fallback (500 let explore outbid real goals far too easily ??? a goal has to be truly unaffordable before wandering wins)

-- Exploration
M.MIN_EXPLORE_DIST = 3  -- don't target frontier squares within this range

-- Builder / tree management
-- FARM_GATHER_RADIUS: how many tiles off-path to search for forest when in
--   "gather" mode and below need_trees.  Larger = more willing to detour.
-- FARM_OPPORTUNISTIC_RADIUS: radius for free on-path grabs (opportunistic/gather-stocked).
--   Keep small ??? every tile of LGM travel triggers steering pacing slowdown.
--   At radius 2, LGM walk time ??? 64 ticks; tank paced to ~6 WU/tick (70% of forest speed 8).
-- TREE_OPPORTUNISTIC_MAX: only opportunistically farm below this level.
--   No sense over-farming when the tank has no specific tree-consuming plan.
-- LGM_DEPLOY_DIST: max map-tile distance from tank to dispatch LGM for a farm/build.
--   Beyond this the pacing slowdown outweighs the benefit.
-- PILL_REPAIR_AMOUNT: armour restored PER TREE when repairing a pill.
--   Engine (pillbox.c pillsRepairPos): repairAmount = trees ?? PILL_REPAIR_AMOUNT(4);
--   the LGM takes ceil(deficit/4) trees per trip (lgm.c armour tiers), so a FULL
--   repair from any HP costs at most 4 trees. (Was M.PILL_REPAIR_COST=1/HP ???
--   a 4?? overestimate that made the gather target farm 15 trees for a dead pill.)
M.FARM_GATHER_RADIUS       = 3
M.FARM_OPPORTUNISTIC_RADIUS = 2
M.FARM_REFUEL_RADIUS       = 4   -- wider farm radius when stationary at refuel base
M.TREE_OPPORTUNISTIC_MAX   = 20
M.LGM_DEPLOY_DIST          = 3
M.LGM_DEPLOY_DIST_REFUEL   = 5   -- max deploy distance when stationary at base
-- Near an enemy tank, don't pull the LGM out to opportunistically farm unless
-- we're critically low on trees ??? farming exposes the LGM and stalls us while a
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
-- (low cost) and NOT suppressed during combat/flee/refuel ??? when holding a pill,
-- getting the LGM home to build takes precedence (per design intent).
M.WAIT_FOR_LGM_COST_CARRYING = 20
-- Danger-aware wait spot (pick_wait_spot in goals.lua): when the tank's own
-- tile has danger (or it's under fire), wait_for_lgm parks on a chosen SAFE
-- tile instead ??? scored ring of 8 bearings ?? radii {3,6,9} + the LGM's tile,
-- minimizing danger-weighted travel + BETA ?? the LGM's extra walk. Slides
-- toward the LGM when that side is safe, away from the pill when it isn't.
M.WAIT_LGM_DANGER_OK          = 0    -- threat.at at/below this counts as a safe tile
M.WAIT_LGM_BETA_COST          = 6    -- cost per tile of extra LGM walk (~1/3 of tank per-tile travel: prefer sliding toward the LGM when safe)
M.WAIT_LGM_DANGER_W           = 25   -- fallback weight per danger point when NO fully safe tile exists (pick least-bad)
M.WAIT_LGM_HOLD_ARRIVAL_TICKS = 150  -- ~3s: LGM arriving sooner ??? hold despite danger (moving drags the pickup point) ??? unless low armour AND under fire
M.ENEMY_LGM_RETURN_TICKS   = 3000     -- estimated ticks for enemy LGM to respawn (~60 sec)
M.RESPAWN_CACHE_WIPE_DIST  = 12       -- tiles; if respawn point is farther than this from death point, wipe all distance-dependent caches
M.ENEMY_LGM_DEAD_ATTACK_DISCOUNT = 0.5  -- multiply attack pill cost when enemy LGM is dead

-- -------------------------------------------------------------------------
-- Steering: deep-sea cliff safety
-- -------------------------------------------------------------------------
-- Step size (world units) for the global cliff brake's heading-ray scan in
-- steering.lua. 256 wu = 1 tile, so 64 wu is a quarter tile: whatever the
-- heading, a quarter-tile hop moves at most 0.25 tiles in x and 0.25 in y,
-- so it cannot jump clean over a tile, and every tile the tank's CENTER is
-- about to drive through gets a terrain lookup.
-- The old scan sampled once per WHOLE tile of look-ahead and could step over
-- a deep corner tile on a diagonal heading: on the DH-Oil Rig NE staircase
-- the single sample landed on road at (141,112) while the tank's centre path
-- clipped deep sea at (142,112) in between, and the tank drowned.
-- A tile the centre only grazes for less than a quarter tile (a dead-on
-- corner cut) can still slip between samples; that case is handled by the
-- lookahead cliff guard's L-step decomposition, not by this brake.
-- With the 768 wu look-ahead clamp this is at most 12 terrain lookups.
M.CLIFF_SCAN_STEP_WU = 64
-- Brake look-ahead (steering.lua) = cpf.predict_stop's stopping distance (the
-- calibrated engine model shared with the charge/approach brakes) + a
-- margin, capped. On 20260831_173448 bot2 the old speed*6 look-ahead let a
-- speed-52 tank slide 317 wu into deep sea after the brake fired.
M.CLIFF_MIN_SPEED       = 1      -- any motion: cliff brake + evade whenever the centre path crosses deep sea within the scan. Was a hard-coded 12, then 6; a swerve creeping at speed 4-8 on the shore (20260831_230309 bot2 t=7816) oscillated between "too slow for the brake" and "brake, no turn" and drifted into the water. With the evasive turn the old stuck-at-water's-edge worry no longer applies: a slow tank facing water turns away instead of freezing.
M.CLIFF_EVADE_BRADS     = 32     -- evasive turn: compare clear runway on rays rotated +-this (32 brads = 45 deg) and turn toward the freer side while braking
M.CLIFF_STOP_MARGIN_WU  = 64     -- a quarter tile: the travel between the sample that sees water and the brake biting
M.CLIFF_LOOK_MAX_WU     = 1280   -- 5 tiles: scan-cost bound (was 768)

-- -------------------------------------------------------------------------
-- Shell trajectory prediction
-- -------------------------------------------------------------------------
-- bsin/bcos return integers in [-128,128] representing unit-vector components.
-- SHELL_SPEED is the simulation step size in raw world units BEFORE the /128
-- scaling.  Per step, the shell advances bsin(dir)*SHELL_SPEED/128 WU in X
-- and -bcos(dir)*SHELL_SPEED/128 WU in Y.  With SHELL_SPEED=32:
--   step size ??? 32 WU (1/8 of a map tile), 64 steps ??? 2048 WU = 8 map tiles.
M.SHELL_SPEED            = 32   -- simulation step size (WU, before /128 bsin scale)
M.SHELL_MAX_STEPS        = 64   -- max trajectory steps  (64 ?? 32/128 ?? max-bsin = 8 tiles)
M.DANGER_SHELL_IMPACT    = 100  -- danger value for every cell on a predicted shell path
M.DANGER_DECAY_TICKS_SHELL = 5  -- ticks before a shell-impact zone expires

-- LGM build-safety thresholds (compared against danger.danger_at value)
-- danger_at = DANGER_SHELL_IMPACT(100) if a shell passes through + pill_danger (0..~95)
M.LGM_DANGER_LOW  = 0    -- normal road/farm: any danger (shell or mild pill) aborts dispatch
M.LGM_DANGER_MED  = 20   -- repair pill: mild pill danger is acceptable
M.LGM_DANGER_HIGH = 80   -- emergency wall / refuel: only heavy fire (angry pill or shell) aborts
M.LGM_GATHER_MAX_DANGER = 20  -- pre-flight tree gather: mild pill danger on the LGM's harvest path is acceptable (don't refuse trees over a little danger). If the nearest tree's path is too hot, try the next-nearest up to LGM_GATHER_RETRIES.
M.LGM_GATHER_RETRIES    = 5   -- how many nearby forest candidates (nearest first) the gather tries before giving up

-- The LGM build-danger gate is retired for placements. It sampled a STRAIGHT
-- LINE the builder does not walk, against a danger field built for TANKS --
-- and pillboxes never target builders, which die to explosion splash. So it
-- refused on a model that did not apply to the unit it protected, and a single
-- predicted shell path (DANGER_SHELL_IMPACT 100) against a flat threshold of 80
-- closed it for good: the tank being shot at could NEVER dispatch its builder,
-- which is exactly when a guard pill in the ground is worth most.
-- (20260827_115304 bot3: PLACE_PILL_GATE reach=true safe=false from t=12523
-- until it died at t=12678, still holding 4 pills; the panic and emergency
-- retries hit the same wall.) Placements now pass LGM_DANGER_YOLO and the two
-- scores in danger.lua make the judgement instead. Other callers -- repair,
-- wall builds, demine -- keep their real thresholds.

-- Gate-failure breaker (builder.lua's place_pill branch). A refused build gate
-- used to be silent AND permanent: the tank sits on the drop spot, the 50-tick
-- replan re-picks the same goal at a LOWER cost each time (carry_discount keeps
-- growing), and the generic stuck detector is reset by every shell we fire back
-- (init.lua's fired_this_tick). Nothing breaks the loop, so the bot bleeds out
-- in place. Count consecutive refusals for one drop spot; past FAIL_TICKS give
-- the spot up ??? block the tile so the placement scan cannot hand it straight
-- back, and clear the goal so pick_goal has to look elsewhere.
-- How old the gate-failure stamp may be and still count as "consecutive".
-- builder.decide() evaluates the gate once per tick, and init.lua's stuck check
-- runs BEFORE decide() so it necessarily reads last tick's value ??? 2 ticks
-- covers that lag with one to spare. Anything older means decide() returned
-- early past the gate branch (drowning/slow-terrain road builds) or the tank
-- left and came back, neither of which is a consecutive run of refusals.
M.PLACE_GATE_STALE_TICKS = 2

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: enemy intercept (TTK vs TTI)
-- -------------------------------------------------------------------------
M.INTERCEPT_PENALTY        = 30    -- cost added when enemy tank may arrive before we finish (was 120: with the ratio cap of 2 it fired as +240 on nearly every take with any enemy inside INTERCEPT_MAX_RANGE ??? far too harsh; 30 caps the effect at +60)
M.INTERCEPT_SAFETY_MARGIN  = 0.8   -- TTK must be < TTI ?? this to avoid penalty
M.TTK_TICKS_PER_HIT        = 8     -- conservative: ticks per effective shell hit on pill
M.INTERCEPT_MAX_RANGE      = 20    -- only consider enemy tanks within this range of pill

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: pill anger cooldown
-- -------------------------------------------------------------------------
M.ANGER_ATTACK_THRESHOLD   = 0.5   -- anger level above which we prefer to wait
M.ANGER_WAIT_MAX           = 500   -- max ticks we're willing to wait for cooldown (~10s)
-- The wait cap above shrinks (in ticks) when sitting out the cooldown is cheap
-- or pointless. Subtractions stack additively, floored at 0 ??? a 1-HP pill at
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
-- Contested refuel base: MOVING enemy tanks near a base make it a bad place to
-- sit and resupply. Cost is a SUM over qualifying tanks of
--   PENALTY x (1 - dist/RANGE)
-- so a tank parked on the base costs the full PENALTY and one at the range edge
-- costs nothing (was a flat step that treated 14 tiles like 0). Distance is
-- Manhattan (U.mdist), matching the range test. A tank an ALLY is already
-- broadcasting an attack_tank goal against is skipped entirely ??? that threat is
-- someone else's job and shouldn't also scare us off the pumps. The terms just
-- add: no outnumbering multiplier (scaling by head count priced refuelling out
-- of reach). See goals.contested_penalty; both refuel paths share it.
M.CONTESTED_BASE_PENALTY   = 120   -- cost of ONE moving enemy tank sitting exactly on the base; scales linearly to 0 at CONTESTED_BASE_RANGE
M.CONTESTED_BASE_RANGE     = 15    -- enemy must be within this range of base (tiles); also the distance over which the penalty fades to 0
M.CONTESTED_BASE_HEADING   = 32    -- heading tolerance (bolo angle units, ~45??)

-- -------------------------------------------------------------------------
-- Anticipatory reasoning: angry pill at refuel base
-- -------------------------------------------------------------------------
M.ANGRY_REFUEL_THRESHOLD   = 0.6   -- pill anger above which we flee the base

-- -------------------------------------------------------------------------
-- Goal replan scheduling
-- -------------------------------------------------------------------------
M.GOAL_REPLAN_INTERVAL     = 50    -- ticks between goal decisions
M.WARMUP_CAPTURE_MAX_COST  = 150   -- warm-up exemption: a capture_pill (dead pill pickup) priced at or below this wins even before the pools are warm. ~13 tiles by sea cost 34; a cross-map pickup is far above this. (20260831_173448 bot3: respawned in a boat, cost-34 water pickup lost to the explore fallback, landed, pills unreachable thereafter.)
M.WARMUP_MIN_REAL_GOALS    = 10   -- the rolling pool-eval warms up over ~14-49 ticks; until this many finite-cost (pickable) candidates exist across all pools the bot rides the explore fallback. Once reached: exit explore immediately (don't wait out GOAL_MIN_COMMIT) and stop showing the warmup reject row in the WINNERS panel.
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
                                        -- (10 ticks ?? 500 = 5000 nodes ??? 10-tile radius)
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
M.STRATEGIC_PLACE_PILL_SPACING  = 5     -- target minimum tile gap from existing friendly pills
-- Clustering penalty grows exponentially as a spot crowds an existing friendly
-- pill: pen = W * (BASE ^ (SPACING - dist) - 1), clamped to CAP. With W=25,
-- BASE=2, SPACING=5: dist 4???25, 3???75, 2???175, 1???375 (capped 400). Past the
-- ~150 MIN_SCORE floor by dist 2, so placement effectively refuses < ~3 tiles
-- and is nudged toward the full 5-tile gap.
M.STRATEGIC_PLACE_PILL_PENALTY_W    = 25    -- exponential penalty scale
M.STRATEGIC_PLACE_PILL_PENALTY_BASE = 2.0   -- exponential growth base (per tile closer)
M.STRATEGIC_PLACE_PILL_PENALTY_CAP  = 400   -- clamp on the clustering penalty
M.STRATEGIC_PLACE_LOS_WEIGHT    = 0.5   -- bonus per tile of LOS coverage
M.STRATEGIC_PLACE_LOS_DIRS      = 8     -- number of directions to sample for LOS
M.STRATEGIC_PLACE_LOS_MAX_RANGE = 8     -- max tiles to trace per LOS ray
M.STRATEGIC_PLACE_BASE_COST     = 60    -- base cost so it loses to attack/capture but beats explore
-- Near-enemy-tank placement penalty (combat zone): flat add to a NON-emergency
-- place_pill_strategic cost when an enemy tank is within euclidean range of the
-- chosen spot. CLOSE supersedes NEAR. The offensive_build emergency drop is exempt.
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
M.STRATEGIC_PLACE_SPACING_BONUS       = 15    -- bonus for a well-spaced spot (>= SPACING, <= BONUS_MAX)
M.STRATEGIC_PLACE_SPACING_BONUS_MAX   = 8     -- upper tile bound for the spacing bonus (still in mutual fire support)
M.STRATEGIC_PLACE_OFFENSIVE_THRESHOLD = 0.6   -- strength ratio to switch to offensive
M.STRATEGIC_PLACE_SPIKE_BONUS         = 80    -- bonus for placing adjacent to hostile base
-- Carry-time urgency: every tick a pill sits in the tank, place_pill cost
-- drops by this much, capped. After ~600 ticks the pill is essentially free.
M.STRATEGIC_PLACE_CARRY_DISCOUNT_PER_TICK = 0.5
M.STRATEGIC_PLACE_CARRY_DISCOUNT_MAX      = 300

-- Carry value penalties ??? increase placement cost when carrying is useful
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
-- role (default shares 20 back / 45 front / 20 aggro / 15 utility, where utility
-- = blockers + carried reserve; a per-bot "portfolio=B/F/A[/U]" BRAIN_INIT_ARG
-- token replaces them, so read pill_portfolio.TARGET_* live, never these
-- numbers). Roles (pill_portfolio.classify): front = near the front line;
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
-- Guardian surplus cap: the guardian bonus is withheld when the spot's
-- category already holds >= CAP x its portfolio target (back 4/1 with
-- CAP=3 -> no more guard-placed back pills; 2/1 still guards).
M.STRATEGIC_PLACE_GUARDIAN_SURPLUS_CAP = 3
-- Hardcore balance: non-panic strategic placement fills ONLY the single
-- most-needed category (see the hard gate in the placement scan);
-- classification drift with a moving front line otherwise bleeds the
-- portfolio out of balance. false reverts to any-under-target.
M.STRATEGIC_PLACE_STRICT_NEED = true
-- Minimum spot SCORE to deploy a carried pill (eval_place_pill_strategic). The
-- carry / util-surplus cost discounts make placement EAGER (win the goal); this
-- keeps the QUALITY bar on WHERE it goes so an eager bot doesn't dump a pill at a
-- mediocre spot. Reference scores: role-fill alone ??? 120 (PORTFOLIO_WEIGHT),
-- a base guardian ??? 150+, a well-positioned protector 250-400. 150 ??? a deficit-
-- role spot must ALSO have real positioning (proximity/coverage) or guard a base.
-- Bypassed when placement is urgent (naked base under fire / about to die).
M.STRATEGIC_PLACE_MIN_SCORE = 150
-- ...BUT relax the bar the more util/carried pills the team is hoarding over the
-- reserve: holding a pile of pills is itself bad, so accept a less-perfect spot
-- rather than carry forever. Effective floor = MIN_SCORE ??? util_surplus??DROP,
-- never below FLOOR_MIN (so an exposed/purposeless spot is still always held).
-- e.g. surplus 0???150, 1???125, 2???100, 3???75, 4+???70.
M.STRATEGIC_PLACE_MIN_SCORE_SURPLUS_DROP = 25
M.STRATEGIC_PLACE_MIN_SCORE_FLOOR        = 70
-- Out-of-ratio urgency: placement cost is discounted when a pill type is in
-- deficit. Per-deficit-unit fraction, capped.
M.STRATEGIC_PLACE_IMBALANCE_DISCOUNT     = 0.25
M.STRATEGIC_PLACE_IMBALANCE_MAX_DISCOUNT = 0.60
-- Util-surplus urgency: once we hold MORE carried/util pills than the utility
-- reserve, deploying gets cheaper per surplus pill (capped) so the team actively
-- empties tanks of the excess instead of hoarding it. Strengthened (was
-- 0.25 / 0.60) now that trail-drop is gone ??? place_pill_strategic is the ONLY
-- path that sheds the hoard, so it must win the goal competition hard when pills
-- pile up: surplus 1 ??? 40% off, 2 ??? 80% off, 3+ ??? 85% off (capped).
M.STRATEGIC_PLACE_UTIL_SURPLUS_DISCOUNT     = 0.40
M.STRATEGIC_PLACE_UTIL_SURPLUS_MAX_DISCOUNT = 0.85
-- Per-TANK multi-carry push (vs the team-wide surplus above): each pill THIS
-- tank holds beyond the first cuts placement cost further ??? one tank hogging
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
-- (carried-1)/2 cap 1); extra radius = floor(urgency ?? this). Applied on top
-- of the default/aggro radius, still capped by the capacity clamp (place_r).
M.STRATEGIC_PLACE_URGENCY_RANGE_BONUS = 6
-- Flat cost multiplier for place_pill_strategic. <1 = preferred. Combined
-- with the carry discount this makes "I'm holding a pill" a near-overriding
-- priority compared to attack/capture goals. Trimmed 0.1 ??? 0.085 to make
-- deploying a carried pill win a bit more readily across the board.
M.STRATEGIC_PLACE_COST_MULT          = 0.085
-- How close a hostile TANK has to be for goal_selection to pin a normal
-- place_pill_strategic under the cheapest attack_tank row ("fighting a tank
-- beats casually dropping a pill"). Manhattan tiles, measured from OUR tank to
-- the nearest VISIBLE hostile tank (perception.nearest_hostile_tank). Default
-- 7 = TANK_COMBAT_ENGAGE_RANGE, i.e. gun range: the pin means "fight first
-- when the fight is imminent", and a tank that cannot shoot us this second is
-- not an imminent fight. Incident 20260903_105448 bot2 t=34774: a cost-1.0
-- placement (four pills aboard, carry discount maxed) was pinned to 2163
-- because an attack_tank row priced 2162 for an enemy 15 tiles away across
-- water this tank could not cross -- so nothing was ever placed and the bot
-- died carrying all four. Distance, not "is the fight winnable": attack_tank's
-- own viability check called that 2162 row viable, so any cost/odds threshold
-- would have to be tuned against attack_tank's pricing and would drift with
-- it. The EMERGENCY build (goal._place_forced) is exempt from the pin at any
-- range, as before.
M.PLACE_PIN_ENEMY_RANGE              = 7
-- Defensive pill build: when an enemy tank is visible, place at ??45?? from
-- the threat direction, 2-5 tiles out, with clear LGM path.
M.DEFENSIVE_BUILD_MIN_DIST     = 1    -- tiles from tank (inner bound, tried FIRST ??? nearest spiral out)
M.DEFENSIVE_BUILD_MAX_DIST     = 5    -- tiles from tank (outer bound)
-- Offensive build: nearest enemy tank must be within this (euclidean) to
-- trigger. 8 is exactly a pillbox's reach -- PILLBOX_RANGE 2048 WU over 256 WU
-- per tile -- so a pill dropped now can actually engage the tank that caused
-- it. The deleted antitank drop used 12, which only made sense beside its
-- midpoint spot (that put the pill 6 tiles nearer the enemy); with a ??45??
-- close-in spot, a trigger at 12 places a pill that cannot reach until the
-- enemy closes 4 more tiles. Also deliberately distinct from the odds rings' 15.
M.OFF_BUILD_THREAT_RANGE       = 8
M.DEFENSIVE_BUILD_ANGLE_OFFSET = 32   -- ??45?? in WinBolo 256-unit circle
-- Support-pill veto: a healthy friendly/allied pill within this radius IS the
-- guard an offensive build would drop ??? skip building another beside it. A pill
-- at or below MIN_HP is nearly dead and doesn't count (build the replacement
-- while it still soaks a few shots).
--
-- The offensive build requires the pill to be within this radius of BOTH us and
-- the enemy: one that covers us but cannot shoot the tank we are fighting is
-- not support for THIS fight. The in-combat guard drop deliberately keeps the
-- tank-only test ??? see nearby_support_pill.
M.SUPPORT_PILL_RADIUS           = 8    -- tiles: pill fire range ??? it engages anything shooting us
M.SUPPORT_PILL_MIN_HP           = 4    -- cover pill hp <= this => doesn't count as cover
-- DEATH_BUILD_ARMOUR / DEATH_BUILD_HIT_WINDOW / PANIC_BUILD_ARMOUR are gone.
-- All three were armour thresholds deciding when to dump a carried pill, and
-- armour alone says nothing about whether anything is actually threatening us
-- -- which is how a bot at armour 10, with the nearest enemy 15 tiles away and
-- not even visible, dumped four pills in 200 ticks. The panic build now reads
-- the two scores in danger.lua instead:
--
--   threshold = min(35, 50 - vulnerability * 0.8)
--   panic     = carrying and builder aboard and not in a boat
--               and vulnerability <= 50 and imdanger <= threshold
--
-- so the more we stand to lose, the less arriving danger it takes to justify
-- banking it. With a DEAD/out builder the pill can't be placed at all, so the
-- haul-protection flee (CRITICAL_FLEE_ENABLED) still covers that case.
-- Emergency offensive_build dispatches the LGM to run to the spot from wherever the
-- tank is (no within-1-tile gate). Cap how far we'll send the LGM: spots are
-- picked at <= DEFENSIVE_BUILD_MAX_DIST, +1 slack for tank drift between
-- candidate selection and dispatch. Beyond this the LGM walk is too slow/risky.
M.PLACE_EMERGENCY_MAX_DIST     = 6    -- tiles: max tank->spot for emergency LGM dispatch
-- capture_pill cost: path^1.5 * DIST_SCALE + threat * DANGER_WEIGHT.
--   Close+safe   ??? very low cost (always high priority)
--   Close+hot    ??? danger term pushes cost up, deprioritises vs safer goals
--   Far (any)    ??? path^1.5 grows fast, nearly ignored beyond ~200 path cost
M.CAPTURE_PILL_BASE_COST     = 20     -- flat floor so capture_pill never beats a trivially cheap goal
M.CAPTURE_PILL_DIST_SCALE    = 0.05   -- coefficient on path_cost^1.5
M.CAPTURE_PILL_DANGER_SCALE  = 0.10   -- coefficient on danger (linear, wsim handles lethality)
-- "Free pill" value bonus: a dead pill that's close and safe is an easy grab ???
-- worth a flat VALUE subtracted from its cost (floored at MIN_COST), so a
-- pristine grab lands near MIN_COST and outranks routine goals INCLUDING a
-- near-zero-cost on-base refuel top-off. (The old ??0.5 multiplier could never
-- get under refuel's floor: 20260703_210207 t=5747, top-off cost 4.5 beat
-- capture 31.6??0.5=15.8 while two free corpses sat uncontested ??? the enemy
-- took them. Free pills are transient; base stock isn't.) NOT binary: the
-- bonus scales from VALUE (best, at zero danger / point-blank) to 0 (none) by
-- a "badness" = max(distance-over, danger). Both ramp to full price quickly
-- once past their thresholds.
--   distance: full bonus within our shooting reach ?? RANGE_MULT; beyond that,
--     badness climbs to 1.0 over DIST_FALLOFF tiles.
--   danger: threat.at at the pill tile (folds in pillbox crossfire AND enemy-tank
--     radius, already reduced by LOS/blockers/distance). One covering pill
--     (~PILL_DANGER_BASE) still keeps a strong bonus; DANGER_FALLOFF ??? two
--     calm pills, where the bonus has faded to ~none.
-- Cost-scale contract: routine goals live at ??? ~20; the band below 20 is
-- reserved for survival-critical work (flee-level refuel bypasses
-- REFUEL_MIN_COST; urgent defends go negative). A pristine free-pill grab
-- floors at MIN_COST = 20 ??? the cheapest ROUTINE goal, under routine
-- refuel's 25 floor but never into the reserved band.
M.CAPTURE_FREE_PILL_VALUE      = 30    -- flat cost bonus for a pristine grab (danger 0, in range)
M.CAPTURE_FREE_PILL_MIN_COST   = 20    -- floor after the bonus ??? cheapest routine goal, above the reserved <20 band
M.CAPTURE_FREE_PILL_RANGE_MULT = 1.5   -- ?? TANK_COMBAT_ENGAGE_RANGE = free-grab distance
M.CAPTURE_FREE_PILL_DIST_FALLOFF   = 5  -- tiles past grab range ??? bonus gone
M.CAPTURE_FREE_PILL_DANGER_FALLOFF = 16 -- danger (??? 2 calm pills) ??? bonus gone

-- Race-mode capture: bias capture pathfinding toward direct routes and relax
-- steering speed caps so we don't lose races to opponents driving straight.
-- Wsim lethality rejection still vetoes actually-suicidal paths.
M.CAPTURE_THREAT_WEIGHT      = 0.3    -- danger_scale override on capture A* fallback (default ~1.0)
M.CAPTURE_RACE_MODE_CAPTURE  = true   -- tag every capture_* goal with race_mode
M.CAPTURE_RACE_MODE_IMMINENT = true   -- also tag race_mode whenever imminent-capture fires

-- ?????? capture_pill DIRECT-ROUTE probe ??????
-- The pill we are going to pick up is DEAD: it radiates no danger of its own,
-- so the danger-weighted Dijkstra slate routes us the long way round OTHER
-- pills' fire fields that we may well be able to drive straight through. For
-- the ONE pill we are actually going for, ask the direct question instead:
-- A* with danger switched OFF, then the wsim "do I die driving that?".
--   survive ??? keep the direct route and its cost
--   die     ??? re-ask A* with danger ON and use that route and cost
-- COST CONTROL: an A* is ~7-10 ms against a ~12 ms per-bot think budget, so
-- the probe only ever runs for the FOCUS pill (the one we hold a capture_pill
-- goal / kill_pickup claim on, else the current pool-4 leader), never per
-- candidate, and never more than one A* in a single think ??? the "die ??? re-ask
-- with danger" half runs on the NEXT tick. Every other capture_pill candidate
-- keeps the cheap Dijkstra lookup for the ranking pass.
M.CAPTURE_ROUTE_DIRECT       = true  -- master switch for the probe
M.CAPTURE_ROUTE_ASTAR_BUDGET = 4000  -- A* node budget; matches compute_pool4_cost's land-only probe
M.CAPTURE_ROUTE_TTL          = 100   -- ticks (~2 s) a verdict stays good before we re-probe
M.CAPTURE_ROUTE_MOVE_TILES   = 6     -- re-probe once the tank is this far (Manhattan) from where we probed
M.CAPTURE_ROUTE_MIN_TIER     = 6     -- skip the probe below this capacity tier ??? the CPU is already hot
-- Only probe once we are reasonably CLOSE (tank???pill Manhattan tiles, the same
-- measure the pool-4 intercept and free-pill terms use). A far body is not worth
-- an A* and a wsim: most of that route is ground we have not seen, the verdict
-- goes stale long before the tank arrives (the tank crosses MOVE_TILES and we
-- re-probe anyway), and trace_last_search only hands back 64 waypoints ??? so on a
-- long path the wsim answers "do I die?" about the first fraction of it and calls
-- that the whole route. Beyond this the cheap Dijkstra slate cost stands, exactly
-- as it did before the probe existed.
M.CAPTURE_ROUTE_MAX_TILES    = 10    -- tank???pill Manhattan tiles; farther pills are never probed

-- Imminent-capture priority. A capturable (health==0) pill/base a few steps
-- away is essentially a free pickup. Collapse its cost to a small positive
-- floor so refuel / attack_tank / place_strategic can't outscore it and
-- leave the body on the ground. Gated on armour so a one-hit-from-dead tank
-- still flees instead of chasing the pickup.
M.IMMINENT_CAPTURE_PATH_COST  = 30   -- path cost below which capture is "imminent"
M.IMMINENT_CAPTURE_FLOOR      = 5    -- cost floor applied to imminent captures
M.IMMINENT_CAPTURE_MIN_ARMOUR = 8    -- suppress override if armour below this (let flee win)

-- Fresh-kill pickup. The moment WE (or our blitz) drop a pill to 0 armour,
-- commit HARD to grabbing the body ??? overriding refuel / flee / survival
-- ENTIRELY (no armour gate, unlike imminent-capture above). A wasted kill
-- hands the pill straight back to the enemy, so this is worth dying for: if
-- the grabber dies mid-pickup an ally finishes it. When multiple blitz
-- killers claim the same pill, the one with the LOWEST Dijkstra path cost to
-- the pill grabs (tie ??? lower player number) and the rest stand down and
-- resume normal goals. Goes through goal_selection's Override 3b. Shot-clear
-- hold reuses POST_KILL_WAIT_TICKS.
M.KILL_PICKUP_ENABLED = true
M.KILL_PICKUP_TTL     = 250    -- rolling TTL: ticks the claim survives since the
                               -- last kill/refresh (~5 s). The grabber refreshes
                               -- it every replan while driving in, so it only
                               -- lapses if the grabber stops making the claim.
M.KILL_PICKUP_HANDOFF = true   -- defer the pickup to the blitz member with the
                               -- LOWEST capture_pill score (tie ??? lower player #)
-- Grab TIMEOUT: absolute cap measured from the FIRST claim of a pill. The
-- grabber refreshes the rolling TTL every tick while pursuing, so this is the
-- real "give up" deadline ??? a grabber that hasn't completed the pickup by now
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
M.TANK_COMBAT_AIM_THRESHOLD     = 20    -- bolo angle units (~28??) for aim bonus
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
-- that's camping under its own pillboxes). Applies in EVERY phase ??? this is
-- the disengage that actually matters. ~one angry pill or a couple of calm
-- ones overlapping. Tune up to be more willing to fight near pills.
M.TANK_COMBAT_DEFENDED_DANGER   = 30
M.TANK_COMBAT_NEAR_PILL_PENALTY = 80    -- cost penalty if enemy is near a hostile pill (crossfire)
M.TANK_COMBAT_NEAR_PILL_RANGE   = 5     -- tiles: how close to hostile pill counts
M.TANK_COMBAT_BASE_COST         = 30    -- base cost so pills/captures usually win over tank hunting
M.TANK_COMBAT_SHELL_SPEED       = 32    -- WU per sim step (for lead-target calc)
M.TANK_COMBAT_SHELL_STEPS_PER_TILE = 8  -- steps for shell to cross 1 tile (256/32)
M.TANK_COMBAT_JINK_PERIOD       = 10    -- ticks between jink direction changes
M.TANK_COMBAT_JINK_ANGLE        = 32    -- bolo angle offset for lateral jink (~45??)
M.TANK_COMBAT_OPPORTUNISTIC_RANGE = 4   -- tiles: fire at enemy if already aimed near them
M.TANK_COMBAT_OPPORTUNISTIC_AIM = 8     -- bolo angle units (~11??) aim tolerance for opportunistic shot
-- Stuck-fire: when aimed at enemy but shot_path_clear keeps rejecting
-- (wall in the way), fire anyway after this many ticks. Shells will
-- chip the wall until LOS opens up, so two tanks dug in on opposite
-- sides of a wall don't sit there forever. Reset whenever a normal
-- clear shot fires or we leave engage. ~30 ticks ??? 1s.
M.TANK_COMBAT_STUCK_FIRE_TICKS  = 30

-- Heading-stability fire gate (see steering.lua tank_combat_steer): only trust
-- the lead once the target's heading has held steady, so we don't fire during a
-- knockback bump (a landed hit shoves the target sideways, decaying over a few
-- ticks) or a reversal. Pure speed changes from terrain don't trip this.
M.TANK_COMBAT_STEADY_MIN_SPEED  = 4     -- WU/tick below which heading is ignored (treat as point shot)
M.TANK_COMBAT_STEADY_TURN_RAD   = 0.20  -- max heading change/tick (~11??) to still count as "steady"
M.TANK_COMBAT_STEADY_TICKS      = 3     -- consecutive steady ticks required before firing

-- Kill-LGM shoot gates.  LGMs are small (1 tile, hitbox even smaller),
-- move slowly (~3 wu/tick), and die in one hit ??? so we fire from
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
M.GHOST_TANK_TTL_TICKS      = 100   -- ~2 s @ 50 Hz: keep ghosting after last sighting (only while the ghost tile or a cardinal neighbour has trees; open-ground ghosts are dropped ??? a tank there would be visible)
M.GHOST_TANK_HIST           = 10    -- position samples used to average heading/speed
M.GHOST_TANK_COST_PENALTY   = 40    -- added attack_tank cost for a ghost (prefer a tank we can actually see)
M.TANK_COMBAT_LOW_SHELLS_COST_PER   = 1.5  -- cost per shell below threshold (0???30, 10???15, 15???7.5)
M.TANK_COMBAT_BOAT_MULT            = 0.8   -- cost multiplier for enemy on river/boat (exposed)
M.TANK_COMBAT_DEEPSEA_MULT         = 0.25  -- cost multiplier for enemy on deep sea (one-shot kill)

-- Emergency pill drop (aIndy: drop pill when about to die to save it)
M.EMERGENCY_DROP_ENABLED        = true
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
-- of 4 pills co-spiking an area barely registers (that area is lost ???
-- killing one changes nothing). Deliberately mild: the tilt is within
-- pool 6 only, so attack_tank / kill_lgm / refuel keep their normal
-- cross-pool balance. (Replaces BASE_THREAT_PILL_DISCOUNT=0.5, which
-- lived only in the dead eval_attack_pill path and never affected live
-- selection.)
M.SPIKE_PILL_DISCOUNT           = 0.7   -- combat-cost multiplier at FULL decisiveness (lerps toward 1.0 as cover grows: cover 2 ??? 0.85, 4 ??? 0.925)
M.SPIKE_OTHER_PENALTY_MULT      = 1.2   -- whole-cost multiplier on every NON-spiking attack_pill candidate at full decisiveness (lerps toward 1.0: cover 2 ??? 1.10, 4 ??? 1.05)
-- Breadth scaling: each base a spike denies BEYOND the first strengthens the
-- discount pull by this fraction (decisive 3-base spike: effect x1.7 ???
-- 1-0.3*1.7 = 0.49 ??? floored at SPIKE_DISCOUNT_FLOOR). Applied on top of
-- decisiveness, so a co-spiked wide pill still pulls weakly (dec scales the
-- whole effect first: dec 0.5 + 3 bases ??? x0.745).
M.SPIKE_BASES_BONUS             = 0.35
M.SPIKE_DISCOUNT_FLOOR          = 0.5   -- combat multiplier never drops below this
-- Net tilt toward a spike ??? (0.3 + 0.2)/cover = 50%/cover (lone spike 50%,
-- pair 25%, quad 12.5%).

-- Automatic de-mine interrupt (demine.lua): shoot known mines in crosshair
-- range, pushing a kill_mine goal over the current one and popping back
-- when cleared. Cost per mine = dist * (1 + BEHIND_MULT * angoff/128) ???
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
-- (influence > 0). A parallel LGM job like farming ??? the tank carries on
-- with its goal while the LGM walks out and paves; the job retires when the
-- tile is paved (timeout / enemy-near / tank-left-it-behind backstops).
M.TREPAIR_ENABLE       = true
M.TREPAIR_RADIUS       = 3   -- tiles from the tank a repairable tile may be (was 5; halved ??? full radius fired too often)
M.TREPAIR_MAX_TICKS    = 400 -- give up (drop job + tile cooldown) after ~8s
M.TREPAIR_ENEMY_RANGE  = 10  -- no repairs (and abort) with a hostile tank this close
M.TREPAIR_ABANDON_DIST = 6   -- drop the job if the tank is this many tiles past it and the LGM never left

-- Friendly pill as barrier bonus (aIndy: use friendly pills as shields)
M.FPILL_BARRIER_BONUS           = 80    -- cost reduction when friendly pill is between us and target

-- Pill repositioning. Legacy badness conditions (ORPHAN_DIST/THRESHOLD, AFAIK
-- from aIndy's "pissing") are RETIRED ??? reposition now scores on the influence
-- portfolio (see pill_portfolio.lua / PILL_REPOSITION_PLAN.md): cost is driven
-- primarily by category balance (default shares 20/45/20/15
-- back/front/aggressive/utility, per-bot overridable via "portfolio=").
M.PILL_REPOSITION_ENABLED       = true
-- BASE_COST: the honest BID floor. Win-then-vote means this cost must actually
-- WIN the goal pool for a vote to open, so it is tuned to sit just above routine
-- combat goals (attack_pill ~88) and above dead-pill capture (~20): a genuinely
-- worthwhile back pill (surplus + close + safe) dips below the field and wins;
-- a base-guard / threatened / far pill stays well above and never does. FIRST-
-- PASS magnitudes ??? tune SURPLUS/BASE_PROTECT/ACTIVITY/DISTANCE against play.
-- (Was 500 in the old flat-80-approval flow, where the raw cost never won.)
M.PILL_REPOSITION_BASE_COST     = 150   -- honest BID floor (must beat the field to trigger a vote)
M.PILL_REPOSITION_SURPLUS_W     = 50    -- PRIMARY discount per pill over its category allotment (imbalance)
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
M.REPOSITION_DEMOLISH_GRACE_TICKS = 1500 -- ~30s: while demolishing a pill for reposition, suppress repair_pill on it (avoid shoot???repair???shoot oscillation)
M.REPAIR_REPOSITION_BLOCK_TICKS = 400  -- 8s @ 50Hz: block repair_pill on a pill we're repositioning (driven by the live goal each tick) AND for this long AFTER the reposition goal ends ??? so a freed pool can't immediately heal the pill we just shot down. Covers the gap REPOSITION_DEMOLISH_GRACE missed (it only refreshes while reposition_steer is firing).
-- Reposition risk penalties (raise cost = discourage repositioning):
M.PILL_REPOSITION_FEW_PILLS_THRESHOLD = 3   -- team total pills (deployed + carried) at/below which reposition is penalized
M.PILL_REPOSITION_FEW_PILLS_PENALTY   = 400 -- cost added when the team has few pills (can't afford to take one offline)
M.PILL_REPOSITION_ENEMY_TANK_PAD      = 5   -- tiles beyond PILL_FIRE_RANGE within which an enemy tank counts as "around" the pill
M.PILL_REPOSITION_ENEMY_TANK_W        = 150 -- cost added per enemy tank within (PILL_FIRE_RANGE + pad) of the pill
M.PILL_REPOSITION_UNPROTECTED_PENALTY = 100 -- extra cost when enemy tanks are near AND no friendly pill covers this one (no support)
-- ?????? Reposition score v2 (win-then-vote) factors ???????????????????????????????????????????????????????????????????????????????????????
-- The reposition score lists EVERY live friendly pill (own + ally); only BACK
-- pills are eligible to win + be moved. Score (a COST, lower = more worth
-- moving) = BASE + travel*DIST_W + base_protect + enemy_activity + enemy_tank
--           - surplus_disc - cardinal_adj_disc. The best eligible pill competes
-- in the pool at this honest cost; only if it WINS does a team vote open.
M.PILL_REPOSITION_DISTANCE_W          = 1.0  -- weight on Dijkstra travel cost tank->pill (closer = cheaper)
M.PILL_REPOSITION_CARDINAL_ADJ_W      = 40   -- discount per FRIENDLY pill 1 tile away in a cardinal dir (N/S/E/W) ??? thin redundant clusters
M.PILL_REPOSITION_BASE_PROTECT_W      = 120  -- penalty per friendly base the pill covers (within PILL_FIRE_RANGE) ??? a base-guard has a real job; more bases = more penalty (strong: one base pushes it out of winning range)
-- Enemy-activity penalty: decays over DECAY_TICKS. Stamped per-tick whenever a
-- hostile pill is within PILL_RANGE or a hostile LGM within LGM_RANGE of the
-- pill (state._repo_enemy_activity[pid] = tick). Repositioning kills the pill
-- temporarily, so it's dangerous while enemies are (or recently were) near.
M.PILL_REPOSITION_ENEMY_ACTIVITY_W          = 250  -- penalty at full strength (just-seen)
M.PILL_REPOSITION_ENEMY_ACTIVITY_PILL_RANGE = 10   -- tiles: hostile PILL within this of the pill = activity
M.PILL_REPOSITION_ENEMY_ACTIVITY_LGM_RANGE  = 6    -- tiles: hostile LGM within this of the pill = activity
M.PILL_REPOSITION_ENEMY_ACTIVITY_DECAY_TICKS = 1500 -- ~30s @ 50Hz: penalty decays linearly to 0 over this
M.PILL_REPOSITION_COOLDOWN_TICKS      = 1500 -- ~30s @ 50Hz: after THIS bot finishes (or abandons) a reposition, it won't START another for this long. Per-bot rate limit so a single tank doesn't churn reposition after reposition. A committed/locked reposition is never blocked by this (it's allowed to finish). 0 disables.
M.PILL_REPOSITION_LOCK_TICKS          = 750 -- ~15s @ 50Hz: hold a committed reposition until the pill is demolished or this elapses
M.PILL_REPOSITION_LOCK_COST           = 30  -- locked-in reposition cost (beats routine goal churn; sub-30 survival goals still preempt)
-- Legacy (unused; kept for reference / any external readers):
M.PILL_REPOSITION_ORPHAN_DIST   = 15
M.PILL_REPOSITION_THRESHOLD     = 50

-- ?????? Reposition VOTING (team consensus) ??????????????????????????????????????????????????????????????????????????????????????????????????????????????????
-- Reposition no longer fires off the raw pool-10 cost alone. A bot that wants to
-- move a back pill opens a team VOTE; it only carries the move out if the vote
-- passes (silence = abstain = yes; any NO blocks). Allies vote NO when moving the
-- pill would be unsafe or they have a better candidate (see reposition_vote.lua).
M.REPOSITION_VOTE_ENABLED               = true
M.REPOSITION_VOTE_WINDOW_TICKS          = 25   -- HARD timeout cap (~0.5s @ 50Hz): the vote resolves EARLY the moment every active ally has cast a ballot (usually a few ticks); this only bounds the wait when an ally stays silent
-- Pacing model (simple 30/30): the team may MOVE one pill every ~30s
-- (RECENT_MEMORY, timed from consumption / observed execution ??? never from
-- a vote merely passing), and a FAILED vote costs the whole team a flat
-- ~30s before anyone proposes again (FAIL_COOLDOWN ??? retrying a just-vetoed
-- move via a different proposer is spam; the NO reasons haven't changed).
-- (Replaces the imbalance-scaled INITIATE_COOLDOWN model.)
M.REPOSITION_VOTE_FAIL_COOLDOWN         = 1500 -- ~30s @ 50Hz: team-wide proposal hold after ANY failed vote (own or observed)
M.REPOSITION_VOTE_BID_TIMEOUT_MARGIN    = 15   -- ticks ON TOP of WINDOW_TICKS: a capture_pill+reposition BID that never gets approved inside this budget (proposal lost, vote never opened, ballots never came back) is abandoned instead of parking in `approach` forever. The drop reuses the FAIL_COOLDOWN hold so the pool doesn't re-select the same reposition next replan
M.REPOSITION_VOTE_RECENT_MEMORY_TICKS   = 1500 -- ~30s @ 50Hz: a bot votes NO (and won't propose) if it remembers a reposition EXECUTING within this window
M.REPOSITION_URGENT_SCORE               = 0    -- candidates scoring below this are URGENT (actively harmful position): the proposer bypasses the recent-memory blackout and voters skip the recent_repo NO. Safety NOs (blocker / enemy near / better candidate) and the fail cooldown still apply.
M.REPOSITION_VOTE_ENEMY_NEAR_TILES      = 15   -- BASE range: vote NO if ANY enemy tank is within this many tiles of the pill AND nothing else covers it
M.REPOSITION_VOTE_TANK_COVER_TILES      = 10   -- BASE range: vote NO if the pill IS covered by >=1 other pill but ANY enemy tank is within this many tiles
-- Gate relaxation. The two proximity gates above are the main reason a vote
-- fails, and at full range they can hold the back line frozen forever. Two
-- inputs relax them.
--
-- (1) LOCAL STRENGTH ladder. Count allied and enemy tanks within the gate's
-- BASE range of the pill, with the VOTER COUNTING ITSELF whenever its own tank
-- is inside that range (info.objects omits our own tank, so the vote adds it
-- explicitly). Equal numbers buys nothing:
--   * allies >  enemies      ??? range x STALE_RANGE_SCALE (15->12, 10->8): the
--                              enemy has to be closer before it matters
--   * allies >= 2 * enemies  ??? the distance gates are SKIPPED entirely: at 2:1
--                              the team can cover the pill's downtime, so enemy
--                              presence is no reason to veto at all. (>=2x
--                              implies >, so this tier also carries the scale.)
-- The skip touches ONLY the two distance gates ??? blocker, just_built,
-- recent_repo and better_candidate all still apply.
--
-- (2) STALE ??? the shared "last reposition" clock (state._repo_last_seen_tick,
-- stamped on approved/executing/executed moves) is older than STALE_TICKS. No
-- move on record at all (clock nil) counts as stale: nothing has happened yet,
-- so be brave. Worth the same STALE_RANGE_SCALE.
--
-- Staleness and strength are ALTERNATIVES, not cumulative: the range floors at
-- one 0.8 application even when both hold (never 0.64).
M.REPOSITION_STALE_TICKS                = 3000 -- ~2 minutes of brain ticks (brains think 25/s) with no team reposition = stale
M.REPOSITION_STALE_RANGE_SCALE          = 0.8  -- range multiplier when stale AND/OR our side outnumbers theirs nearby (never squared; 2:1 skips the gates outright)
-- USER-TUNABLE FLAG: with any HUMAN player on our team, don't reposition at
-- all, ever. Repositioning takes a pill offline for a while and rearranges a
-- back line a human teammate is reading and relying on; a human hasn't opted
-- into the bots' consensus and can't vote in it. Set this to FALSE to re-enable
-- repositioning in mixed human/bot teams.
-- Detection is engine-authoritative, not a broadcast heuristic ??? see
-- util.human_ally_count (info.allies & ~info.player_bots). Enforced in three
-- places so one bot mis-detecting can't carry a move: eval_reposition_pill
-- produces no candidate, the vote never OPENs, and any voter that does see a
-- human vetoes with reason "human_allies".
M.REPOSITION_DISABLE_WITH_HUMAN_ALLIES  = true
M.PILL_JUST_BUILT_TICKS                 = 1500 -- 1 minute ??? freshly placed pills settle before anyone may propose moving them. Voters NO with reason "just_built" and eval_reposition_pill won't even offer such a pill as a candidate. Age comes from world.pills[].placed_tick, stamped in world.lua on a REAL placement (carried->deployed, or alive at a tile it wasn't at before) ??? never on a plain re-sighting
M.REPOSITION_VOTE_APPROVAL_TTL          = 1500 -- ~30s @ 50Hz of ACTIONABLE time: the TTL burns only on ticks the initiator can actually act (can_carry_now ??? LGM in tank, hands free, shells); busy stretches pause it, with a hard wall-clock cap at 3?? TTL. While an approval is held, luxury LGM dispatches (opportunistic farm / road_ahead / trepair) are suppressed so the window isn't wasted
M.REPOSITION_APPROVED_COST              = 80   -- fixed reposition cost for the initiator on the pill its team vote APPROVED; beats routine goals (capture/base/place) so the move actually wins the pool, while sub-80 survival/refuel goals can still preempt
M.REPOSITION_VOTE_RESULT_LATCH_TICKS    = 120  -- keep the vote-result panel on screen this long after resolve so it's readable
M.REPOS_GUARD_TTL                       = 1500 -- ~30s: reposition-target tiles stay repair/defend/rebuild-proof this long past the last refresh (refreshed every tick while the move runs, so this is the tail AFTER the vote pass / goal broadcast stops ??? generous so slow pickups and comms gaps can't let a rebuild slip in)
M.REPOS_PLACE_EXCLUDE_RADIUS            = 5    -- tiles: place_pill_strategic refuses candidate tiles within this of a live _repos_guard entry ??? the pickup CREATED the coverage hole the scorer wants to fill, so without this the vacated tile re-wins and the pill is rebuilt where it stood (20260704_022107 t=5625)
-- Exponential "redundant pill" discount: the MORE friendly pills already cover a
-- pill, the exponentially cheaper it is to move (a redundant back pill is the
-- best thing to relocate). disc = min(CAP, W * (BASE^covering_pills - 1)),
-- subtracted from the reposition position-cost. BASE>1 ??? grows fast.
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
-- happens at all ??? we can't afford to take one offline. (Stricter, absolute gate
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
M.DEFEND_PILL_BASE_COST      = 250   -- intrinsic commitment cost of a defense trip; distance
                                     -- adds on top, threat multipliers scale the sum DOWN

-- Defend formula (eval_defend_pill): cost = (base+travel) * threat_mult
-- * lateness * readiness for the LIVE tiers (siege/setup/sight); quiet
-- pills use the DEFEND_QUIET_DMG_COST curve below instead. Threat evidence only ever
-- LOWERS the cost (multiplier < 1, strongest live tier wins); lateness
-- only ever RAISES it. Multiplicative on a ~250 base so common threatened
-- cases land naturally around 100-300 instead of clipping the MIN floor ???
-- severity and distance stay ordered. Hand-tunable; expect these to move
-- after Heat Lab runs.
M.DEFEND_ENEMY_NEAR_RADIUS   = 10    -- hostile tank within this of a team pill -> sighting stamp
M.DEFEND_LGM_NEAR_RADIUS     = 6     -- setup-tell stamp radius: hostile LGM seen within this of a
                                     -- team pill, a wall/halfwall APPEARING (non-friendly builder;
                                     -- threat.lua terrain diff), or a hostile pill freshly planted
                                     -- (world.lua) ??? all stamp _lgm_near_tick
M.DEFEND_DMG_FRESH_TICKS     = 400   -- last_hit_tick age for "active siege" (~8 s)
M.DEFEND_SIGHT_FRESH_TICKS   = 600   -- sighting/setup stamp age still counted (~12 s), linear decay
M.DEFEND_SETUP_MULT          = 0.25  -- LGM building/planting nearby: the MOST savable moment
                                     -- (nothing lost yet, build interruptible) -> strongest tier
M.DEFEND_SIEGE_MULT          = 0.30  -- fresh damage on a FULL-health pill; scales toward 1 as hp
                                     -- drops (almost-dead = mostly lost = weak pull; recovery is
                                     -- capture/rebuild territory)
M.DEFEND_SIGHT_MULT          = 0.50  -- hostile tank seen near the pill (prevention tier)
M.DEFEND_COVERAGE_MULT       = 0.95  -- per covering friendly pill (heat-up potential); threat-gated
-- WEAR DISCOUNT on the sight/setup-tier defend cost. Linear in standing
-- damage (hits taken = PILLS_MAX_HEALTH - health): a worn pill is fragile and
-- worth guarding, so protecting it should outbid guarding an untouched one at
-- the same tier. Tops out at ~33% off for a nearly-dead pill (13 hits ->
-- ~29% off, 5 hits -> ~11%).
-- NOT applied to the siege tier (its savability logic deliberately runs the
-- other way: an almost-dead pill under fire is mostly lost) and NOT to the
-- quiet tier (the DEFEND_QUIET_DMG_COST curve below already prices wear).
-- Incident: full-hp pill #0 outbid hp-2 pill #14, both sight-tier, at
-- session 20260831_113629 bot3 t=19709.
M.DEFEND_WEAR_WEIGHT         = 0.3333
-- QUIET-PILL DAMAGE CURVE. A pill with NO activity around it ??? no fresh
-- damage (siege), no setup tell (hostile LGM), no sighting ??? is NOT priced
-- off the (base+travel)*mult product at all. It is priced ONLY by how
-- chewed-up it already is. Rationale: an untouched pill is barely worth
-- leaving your post for; each hit it has taken raises the urgency of going
-- back and standing on it.
-- Indexed by HITS TAKEN = PILLS_MAX_HEALTH - health (the persistent wear,
-- NOT pill.attack_damage, which is a recent-burst accumulator that world.lua
-- zeroes PILL_ATTACK_COOLDOWN ticks after the last hit ??? always 0 on a quiet
-- pill). Above the top index the cost eases HALFWAY toward
-- DEFEND_QUIET_DMG_FLOOR per extra hit (5 -> 275, 6 -> 262, 7 -> 256 ...),
-- so 5+ converges on the floor instead of stepping off a cliff.
-- Final quiet cost = (curve + dij_travel) * readiness, then the
-- well-defended clamp (which RAISES bids under DEFEND_WELL_DEFENDED_COST up
-- to it) and the flat-cost tiebreaker sliver. The two-tier floor
-- (DEFEND_MIN_COST / DEFEND_SIGHT_MIN_COST) does NOT apply to quiet bids ???
-- the curve supersedes it; that floor stays for sight-only precaution bids.
M.DEFEND_QUIET_DMG_COST  = { [0] = 1500, [1] = 1000, [2] = 800, [3] = 500, [4] = 300 }
M.DEFEND_QUIET_DMG_FLOOR = 250
M.DEFEND_ACTIVE_WINDOW       = 50    -- ticks (~1 s): >= 2 hits on the pill inside this window = it is
                                     -- being killed RIGHT NOW; the measured ticks-per-hit gives its TTL.
                                     -- Fewer = no late penalty (shooter paused / rate unknown)
M.DEFEND_ETA_PER_COST        = 6     -- rough ticks of travel per dij cost unit (ETA estimate)
M.DEFEND_FUTILITY_MAX        = 3.0   -- cap on the late-arrival cost multiplier during a siege
M.DEFEND_MIN_COST            = 100   -- floor backstop, rarely hit with the multipliers above.
-- Precaution floor: a defend bid with NO fresh damage and NO setup tell
-- (no "a pill block is being built -> take incoming" LGM sighting) is
-- responding to a mere drive-by ??? floor it here instead of
-- DEFEND_MIN_COST so sight-only defends don't outbid real work.
M.DEFEND_SIGHT_MIN_COST      = 200
-- Well-defended gate: when allied tanks ALREADY at the pill cover the
-- enemies there in proportion to the overall team sizes, the bid jumps
-- straight to this cost so the rest of the team doesn't swarm one pill.
-- Required coverage rounds in the defenders' favour:
--   R = ceil(their_team / our_team); well-defended when
--   foes_near <= allies_near * R (allies exclude the bidder itself).
-- Ex: teams 5v10 -> R=2 -> one defender holding vs two attackers is
-- enough; teams 3v4 -> R=2 as well; equal teams -> 1v1 covers it.
M.DEFEND_WELL_DEFENDED_COST   = 500
M.DEFEND_WELL_DEFENDED_RADIUS = 10   -- Euclidean tiles around the pill
-- defend_pill steal band: distance decides (defend costs are base-
-- dominated + flat-clamped, with a travel*0.01 tiebreaker restoring the
-- ordering) ??? the closer responder's sliver-cheaper bid takes the claim
-- from a farther ally that merely re-scored first.
M.ALLY_CLAIMED_STEAL_FRAC_DEFEND = 0.005
-- Readiness scaling: an under-equipped responder is a WORSE defender ???
-- each of shells-below-SHELLS_LOW and armour-below-ARMOUR_LOW adds up
-- to +1.0x to the bid (linear in the deficit, capped). Feeds the total-
-- score steal: of equally distant allies, the best-equipped goes.
M.DEFEND_READY_SHELLS   = 20    -- full weight at/above (= SHELLS_LOW)
M.DEFEND_READY_ARMOUR   = 15    -- full weight at/above (= ARMOUR_LOW)
M.DEFEND_READY_MAX_MULT = 2.5
                                     -- Sits ABOVE attack_tank engage (~11) and mid-take
                                     -- attack_pill locks (10-50), so on arrival the fight
                                     -- takes over from the drive; below explore (500) and
                                     -- most fresh attacks, so defense still wins the pool.

-- Arrival handoff: within this Euclidean tile radius of the pill, the
-- "travel closer" phase is COMPLETE ??? the travel formula stops bidding
-- entirely (it must not beat real close-range goals like attack_tank or
-- repair_pill) and the pill's bid becomes the heat-up action alone.
M.DEFEND_ARRIVE_RADIUS       = 10
M.DEFEND_HEAT_COST           = 200   -- flat bid for "put 3 shells in the pill to anger it".
M.HEAT_REQUIRE_ENEMY_RANGE   = 10    -- heat only with a hostile tank VISIBLE within this
                                     -- euclidean range of the pill (and not actively
                                     -- shelling it ??? taking_damage blocks first).
                                     -- Loses to attack_tank (~11) and close repair (<100);
                                     -- beats explore (500) when nothing else is pressing.
M.HEAT_PILL_SHOTS            = 3     -- shells per heat sequence
M.HEAT_PILL_MIN_HP           = 6     -- don't shave a pill that can't spare the HP
M.HEAT_PILL_MAX_ANGER        = 0.4   -- already hot -> more shells add nothing (3 hits saturate)
M.HEAT_SELF_STAMP_TICKS      = 150   -- world.lua skips the under_attack stamp this long after our
                                     -- own heat shot / an all-friendly shell watch stamp (covers
                                     -- shell flight) ??? tickling must not read as an enemy siege
M.HEAT_ALLY_SHELL_RADIUS     = 2     -- perception's shell watch: shells within this of a team pill
                                     -- count as "about to hit it"; ALL friendly-labeled -> ally
                                     -- heating (suppress alarm), ANY hostile/neutral -> real attack
M.HEAT_SEQUENCE_TICKS        = 150   -- estimated heat volley length: aim + 3 reload cycles + flight
M.HEAT_REPAIR_OVERLAP_MARGIN = 100   -- safety margin on the timed ally-repair overlap check: block
                                     -- heating only when their repair ETA lands within
                                     -- HEAT_SEQUENCE_TICKS + this of now
-- Enemy-near repair hold: OFF by default. When the pool has committed to
-- repair_pill, the LGM goes ??? holding him in the tank while the pill dies
-- (then getting stuck-blocked for waiting) defends nothing. Flip on to
-- restore the old "never walk the LGM toward a seen enemy" caution.
M.REPAIR_HOLD_ENEMY_NEAR_ENABLED = false
M.REPAIR_HOLD_ENEMY_NEAR_TICKS = 400 -- hold the repair LGM dispatch while a hostile tank was seen
                                     -- near the pill this recently (~8 s) ??? don't walk the little
                                     -- guy into a live fight; the pool's contested x3 already
                                     -- de-prioritizes the trip itself
-- Under-fire repair hold: ON. This is the interlock the enemy-near hold above
-- was reaching for and got wrong. "A hostile was seen somewhere near this pill
-- in the last 8 s" is not a reason to keep the little guy in the tank -- it is
-- true for most of a real game and it is why the flag above had to be turned
-- off. "A shell landed on this pill less than REPAIR_QUIET_TICKS ago" is: the
-- LGM walks at 1 tile/~13 ticks and dies to a single hit, so sending him while
-- the volley is still landing simply loses him. The moment the hits stop, he
-- goes. Set false to dispatch regardless (the pre-2026-09-02 behaviour).
M.REPAIR_HOLD_UNDER_FIRE_ENABLED = true

-- LGM repair-dispatch RANGE. These were four bare locals inside
-- builder.decide (DANGER_LOW/HIGH, DIST_BASE/DIST_DANGEROUS); they are
-- constants now because a second caller needs to read the same numbers.
-- The cap is danger-blended: insist the tank be within DIST_BASE tiles of the
-- pill while nothing is shooting at us, widening toward DIST_DANGEROUS as the
-- tank's own local threat climbs from DANGER_LOW to DANGER_HIGH ("get as close
-- as we can while it is safe", re-evaluated every tick).
--
-- The second caller is defend_pill_score's REPAIR handoff: a defend win that
-- means "fix this pill" must not steer the tank AT the pill tile (a live pill
-- is impassable), so it drives to its hold tile instead -- but only when that
-- tile is inside the UNSCALED DIST_BASE cap, or the LGM could never be
-- dispatched from where we parked and the tank would sit there forever.
M.REPAIR_DISPATCH_DIST_BASE      = 5    -- tiles, calm
M.REPAIR_DISPATCH_DIST_DANGEROUS = 12   -- tiles, at DANGER_HIGH
M.REPAIR_DISPATCH_DANGER_LOW     = 50   -- threat_at_tank where the widening starts
M.REPAIR_DISPATCH_DANGER_HIGH    = 150  -- ...and where it is fully widened

-- Strategy / game phase detection
M.OPENING_MIN_TICKS       = 500    -- ~10 seconds minimum opening phase
M.OPENING_UNREACHABLE_GRACE_TICKS = 1500  -- ~30s: only AFTER this do we drop genuinely-unreachable (INF dijkstra cost) neutral bases from the opening-exit count. Before it, the cold-start dijkstra surface hasn't expanded and bases legitimately aren't reached yet, so every neutral still counts (don't disrupt the early land-grab).
M.OPENING_BASE_RESCORE_TILES = 10  -- opening phase: one tick before every replan, force a strict-A* rescore of every base within this many tiles of the tank (the incremental base eval is dijkstra-only, so nearby bases sit at INF during the cold-start until the surface reaches them). Makes them selectable at the imminent replan.
M.PHASE_HYSTERESIS_TICKS  = 100    -- ~2 seconds of consistent signal before switching
M.ENDGAME_PILL_RATIO      = 0.70   -- >70% of pills = endgame
M.ENDGAME_BASE_RATIO      = 0.80   -- >80% of bases = endgame
M.FRONT_LINE_INTERVAL     = 50     -- recompute front line every ~1 second

-- How far (tiles, Manhattan tank???goal) the phase-weight bias fades to neutral
-- (1.0), PER goal type. The phase preference is a LOCAL strategy ??? full weight at
-- the tank, lerping to 1.0 by this distance. Big = the bias reaches far (we'll
-- travel for it); small = only nearby goals feel it. Per-pool so e.g. opening
-- capture_base reaches across the map (bases are the priority) while a far
-- capture_pill stops pulling us off course. `default` covers any pool not listed.
M.PHASE_WEIGHT_DIST_FALLOFF = {
  default      = 40,
  capture_base = 90,   -- bases are the opening priority ??? chase them far
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
M.WSIM_KILL_PERSIST_TICKS  = 50     -- ~1s: keep a KILL verdict on a goal (kind@tile) for this long so it stays rejected on ticks where wsim is capped/disabled ??? stops the bot flapping back onto a lethal goal before refuel/hysteresis take over
M.WSIM_KILL_REJECT_OPENING = false  -- enforce the death-reject in the opening phase too. Default false: opening is the critical land-grab window ??? dying to grab a base is an acceptable trade, so we don't let wsim veto an attempt.

-- Fresh-kill "sweep the pill you killed" incentive. When WE kill a pill
-- (our attack_pill target dies), keep its capture attractive for a window by
-- DAMPING the world-sim pill-fire danger penalty on that capture ??? so a
-- neighbouring pill's predicted shots don't scare us off finishing the sweep
-- and handing the enemy a free rebuild. The wsim KILL (predicted-death) reject
-- is NOT damped, so it still won't suicide into the fire.
M.SWEEP_KILL_WINDOW        = 300    -- ticks (~6s) the sweep incentive lasts after our kill
M.SWEEP_KILL_WSIM_MULT     = 0.3    -- damp the wsim danger penalty to this fraction during the window
M.WSIM_OPENING_ENABLED     = false  -- run wsim AT ALL during the opening phase. Default false: same reasoning ??? taking bases early is so important that we'd rather be reckless and risk dying than have wsim's damage-cost shaping pull us off an opportunity.
M.WSIM_LGM_DEATH_PENALTY   = 200    -- extra cost if sim predicts LGM will die

-- Dwell: how long the tank has to STAND STILL at the destination once it
-- gets there. The sim keeps running for this many ticks after arrival, with
-- the tank parked and taking full (undiscounted) shell damage ??? that window
-- is where a bot that drives up next to a hostile pillbox actually dies, and
-- before this existed the sim stopped dead on arrival and never priced it.
--
-- For a pill placement the dwell is the LGM round trip: walk out to the drop
-- spot, build, walk back. LGM walk speed on grass is MAN_SPEED = 16 WU/tick
-- over a 256 WU tile = 16 ticks/tile (forest is 2x slower, swamp/crater/
-- rubble 4x ??? we deliberately assume grass rather than simulating terrain).
-- LGM_BUILD_TIME is 20 ticks (lgm.h:79). So:
--   dwell = 2 * tiles * WSIM_DWELL_TICKS_PER_TILE + WSIM_DWELL_BUILD_TICKS
-- ~340 ticks for 10 tiles, ~500 for 15.
M.WSIM_DWELL_TICKS_PER_TILE = 16    -- LGM walk ticks per tile on grass
M.WSIM_DWELL_BUILD_TICKS    = 20    -- LGM_BUILD_TIME
-- Floor: even placing under our own tracks, the LGM has to step off the
-- tank, build and step back ??? roughly a 2-tile round trip plus the build.
M.WSIM_DWELL_MIN_TICKS      = 80
-- Ceiling: past this the prediction is guesswork anyway, and every dwell
-- tick is sim time inside the brain's per-tick budget.
M.WSIM_DWELL_MAX_TICKS      = 600
-- Tick budget for placement goals specifically. WSIM_MAX_TICKS (300) at
-- ~21 sim-ticks/tile only reaches ~14 tiles, so a 15-tile drive was being
-- cut off before the dangerous part ??? and now the dwell has to fit after it
-- too. 15 tiles (~315 ticks) + a 500-tick dwell needs ~815; 1000 leaves room.
-- The sim is cooperatively abortable, so overrunning degrades to a
-- `truncated` result rather than blowing the tick budget.
M.WSIM_PLACE_MAX_TICKS      = 1000

-- Flat cost added when a sim run comes back `truncated` -- it ran out of
-- ticks instead of reaching a natural end (arrival, or death). Truncation
-- means UNKNOWN, and without this the pool reads the low damage number off
-- the simulated prefix as evidence the trip is SAFE, which is backwards: the
-- part that was never simulated is the far end of the route, which is
-- exactly where a bot driving into a defended area gets killed.
--
-- Sized deliberately small. A truncated run is unknown, not lethal, and
-- over-pricing it would push the bot off long-but-fine routes. At
-- WSIM_DAMAGE_COST_WEIGHT = 10 this is 2.5 armour points -- about half a
-- shell hit of caution. Enough to lose a tie against an equivalent route
-- that was simulated all the way through; nowhere near WSIM_LGM_DEATH_PENALTY
-- (200) and nothing like the +99999 kill reject.
M.WSIM_TRUNCATED_COST       = 25

-- =========================================================================
-- BRAIN_CAPACITY_LEVELS[1..10]: 1=10%, 10=100%. Levers per tier:
--   dij_short  : SHORT-slate nodes/tick (default 500). Powers steering nav.
--   dij_long   : LONG-slate nodes/tick (default ~560 from 70k/125 ticks).
--   scan_step  : eval_pill_difficulty step in degrees (5/10/20/45).
--   pp_spread  : plan_position scan spread over N ticks. 1=do all 72
--                angles in one tick (best). N=spread across N ticks
--                (~72/N angles per tick). At tier 1 (spread=50) the
--                full 72-angle sweep completes in ~1 s. No resolution
--                loss ??? always 5?? / 72 angles regardless of tier.
--   ttl_mult   : multiplier on pool-6 diff_cache distance-tier TTLs.
--   eval_iv    : ticks between step_eval_queue pops (1=every tick).
--   wsim       : sims top-N of the merged ~9-entry pool[]. nil=all, false=skip.
--   place_r    : caps STRATEGIC_PLACE_SEARCH_RADIUS for the pool-8 placement
--                scan (12 at the top tiers, scaling down to 2 under CPU load).
--   tank_step  : eval_attack_tank standoff scan step in degrees (default 5).
--   sb_positions / sb_step : shield-blocker ring scan density.
--                positions ?? step ??? angular coverage. Tier 10 keeps the
--                legacy 28 ?? 0.5?? (~??7??) sweep. Lower tiers drop to
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

-- Hard ceiling on the angles ONE advance_pill_eval_chunk call may sweep,
-- regardless of what pp_spread asks for. The chunk cursor only advances AFTER
-- evaluate_pill_difficulty returns, so a chunk too big to fit the per-tick
-- budget makes ZERO progress and re-runs identically every tick ??? a livelock.
-- Tier 10 (pp_spread=1) asked for all 72 angles (~9 ms) in one call; capped at
-- 12 it needs 6 ticks per sweep, which fits any budget the host hands out.
M.PP_ANGLES_PER_TICK_CAP    = 12
-- Escape hatch for a chunk that STILL can't fit (mirrors the shield-scan
-- blacklist): the budget kill unwinds the whole think, so kills are counted by
-- a marker written BEFORE the call. After this many kills at the same cursor,
-- the sweep is abandoned and the pill is held off the attack pool for
-- PP_BLACKLIST_TICKS so pick_goal doesn't instantly re-adopt the same take.
M.PP_CHUNK_KILL_TRIES       = 3
M.PP_CHUNK_ATTEMPT_TTL      = 250   -- ticks (~5 s): stale attempt markers are dropped, so isolated kills never accumulate into a blacklist
M.PP_BLACKLIST_TICKS        = 500   -- ticks (~10 s) the pill stays out of attack_pill / plan_position after its sweep is abandoned

-- ?????? Replan rate control ??????
-- A replan tick is the most expensive shape the brain runs (build/finalize
-- pools + pick_goal). Several urgent-replan triggers are LEVEL conditions
-- rather than edges, so they re-fire every tick for as long as the condition
-- holds; when base cost plus drift pushes such a tick past the per-bot budget
-- the bot budget-kills forever (the goal is re-selected identically each tick
-- and the only exit is completing a think). These bound how often the
-- level-ish triggers may pay for a full replan.
M.REPLAN_MIN_INTERVAL       = 5     -- ticks: floor between soft/level-triggered full replans (hard events bypass it)
M.ATK_PREEMPT_MIN_INTERVAL  = 10    -- ticks: floor between attack_pill "tank preempt" replans specifically
M.ATK_PREEMPT_NEAR_TILES    = 8     -- tiles: nearest enemy crossing INTO this radius counts as a change worth replanning for

-- ?????? Tick-budget kill catch-all ??????
-- Last-resort guard above the per-call blacklists (PP_* above, the shield
-- scan): those only count kills that land inside the one call they guard, and
-- a kill landing in the replan path touches neither. This one counts kills at
-- the granularity of the whole think, keyed on the goal shape that was running.
M.THINK_KILL_TRIES          = 5     -- consecutive budget-killed thinks in the same (kind,substate,target) before the goal is abandoned
M.THINK_KILL_TTL            = 50    -- ticks: a stale attempt marker is dropped, so isolated kills never accumulate
M.GOAL_BLACKLIST_TICKS      = 750   -- ticks (~15 s) the (kind,target) pair stays out of goal selection
M.REPLAN_LOG_MIN_TIER       = 3     -- capacity tier below which the per-candidate replan dumps (FINAL_SCORES / pool[] / gc:) are skipped

-- Tier control: per-tier ms history corroborates raise decisions; drops
-- are aggressive (multi-tier on bigger overruns + on host-killed ticks).
M.CAPACITY_EWMA_ALPHA       = 0.20  -- EWMA blend on think/target ratio AND per-tier ms.
M.CAPACITY_DROP_RATIO       = 1.20  -- drop 1 tier when smoothed ratio > 1.2.
M.CAPACITY_DROP_BIG_RATIO   = 1.50  -- drop 2 tiers when smoothed ratio > 1.5.
M.CAPACITY_RAISE_RATIO      = 0.70  -- raise 1 tier (with corroboration) when ratio < 0.7.
M.CAPACITY_RAISE_FREE_RATIO = 0.40  -- raise 1 tier (skip corroboration) when ratio < 0.4.
M.CAPACITY_KILLED_CUT       = 3     -- tiers to drop when wasKilled (host force-killed last tick).
M.CAPACITY_KILLED_COST_MULT = 1.5   -- a killed tick reports lastThinkMs ??? the budget (the kill fires AT the cap), so both EWMAs would learn "we fit, barely" from a tick that did NOT fit ??? and the per-tier history then corroborates raising straight back into the kill. Charge a kill at least target ?? this instead.
M.CAPACITY_KILLED_AVOID     = 200   -- ticks to avoid a tier that just got killed at.
M.CAPACITY_RAISE_HEADROOM   = 0.90  -- raise only if next-tier history < target * this.
M.CAPACITY_DEFAULT_TIER     = 10    -- start at full quality; throttle on observed pressure.
-- Test override: when non-nil, ignore brain.targetMs and pretend this is
-- the per-bot budget. Lets you force the throttle tiers to engage even
-- when the host has plenty of headroom. Set to 0.75 to drive the bot
-- into low tiers and verify the levers actually fire. nil = use host's
-- published targetMs (production behavior).
M.CAPACITY_FORCED_TARGET_MS = nil

-- ?????? Profiling lite (debug brain only) ???????????????????????????????????????????????????????????????????????????????????????????????????????????????
-- Fraction of the per-tick budget above which the debug brain's end-of-tick
-- NEAR_BUDGET line prints its stage breakdown (see init.lua "profiling
-- lite"). Only ever read from inside `if BRAIN_DEBUG_MODE` blocks, so opt/
-- never touches it ??? but the constants twins are kept byte-identical by
-- convention, hence the same line lives in opt/constants.lua.
M.PROFILE_LITE_NEAR_FRAC = 0.85

-- ?????? Squad coordination (Phase 1: pill blitz) ??????????????????????????????????????????????????????????????????????????????????????????
M.HARASSER_FRAC       = 0.20   -- fraction of the protocol-bot set that are harassers (floor)
-- Dynamic harasser ramp: once we hold a clear BASE advantage (base_strength past
-- HARASSER_BASE_THRESHOLD) AND aren't bleeding pills (pill strength at/above
-- HARASSER_PILL_FLOOR), raise the harasser fraction from HARASSER_FRAC up toward
-- HARASSER_FRAC_MAX ??? dominating bases while holding pills means we can spare more
-- bots to harass. Below the base threshold, or while losing a lot of pills, it
-- stays at the HARASSER_FRAC floor.
M.HARASSER_FRAC_MAX       = 0.50  -- harasser fraction ceiling at full base dominance
M.HARASSER_BASE_THRESHOLD = 0.60  -- base_strength (friendly/contested) where the ramp starts
M.HARASSER_PILL_FLOOR     = 0.40  -- min pill strength to allow ramping ("at least not losing a lot")
M.BASELINE_SQUAD_SIZE = 3      -- commanders = ceil(non-harasser count / this)
M.SQUAD_ALLY_MAX_AGE  = 1750   -- ticks; allies staler than this drop out of the protocol set

-- ?????? 2026-06-01 brainstorm features ??? ALL DEFAULT OFF (flip to test) ???????????????????????????
-- New behaviors land behind flags so the verified post-merge brain is
-- unchanged until each is enabled. See SQUAD_BRAINSTORM_DECISIONS_2026-06-01.md.
M.DYNAMIC_COMMANDERS   = true   -- R0: commander = whoever is mid hard-take (else current deterministic roles)
M.HARD_TAKE_MIN_HP     = 12     -- R0: pill HP at/above which a take is "hard" (spawns a squad/commander)
M.CIRCLE_REINFORCE_ENABLED = true  -- R3-exec: uncommitted bots reinforce nearest losing circle
M.CIRCLE_REINFORCE_TIMEOUT = 1500  -- R3: base ticks (~30s) to reach the safe tile; scaled by travel distance
M.UTIL_ACTIVE_PICKUP   = true   -- R1b: actively pick up a back pill to hit the util reserve target (STUB ??? no-op)
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
-- ?????? Pillbox suiciders (map opt-in) ????????????????????????????????????????????????????????????????????????????????????????????????????????????????????????
-- On a map listed here the harasser slate is REPURPOSED: every bot the harasser
-- assignment would have flagged (same HARASSER_FRAC / dynamic-ramp machinery,
-- so this table changes WHICH role they get, never HOW MANY) becomes a
-- "pill_suicider" instead, and nobody on that map is a harasser. A suicider is
-- an ordinary GoalHunter bot with three differences:
--   * a goal-cost surcharge on everything EXCEPT hitting pills and refuelling
--     (PILL_SUICIDER_OTHER_MULT / _DEFEND_MULT below),
--   * it NEVER enters the defensive swerve (no dodging the pill it is taking ???
--     it charges straight in and keeps firing; the pill-DEAD "kill" swerve
--     still runs, that one is the capture/exit handoff, not a dodge),
--   * it never builds shield walls / blockers (forced non-PPT, shield.scan
--     skipped entirely ??? which also saves that scan's tick budget).
-- Everything else ??? squad membership, blitz calls/joins/standoff slots,
-- commanding a blitz ??? is unchanged.
-- KEY FORMAT: exactly what the engine reports as info.gameinfo.mapname, i.e.
-- the map file's BASENAME with no directory and no ".map" extension
-- (server_sim.c strips both), e.g. "data/maps/Survival.map" -> "Survival".
M.PILL_SUICIDER_MAPS = { ["Survival"] = true }
-- Suicider goal-cost surcharge. User's spec, verbatim: "instead of doing
-- attack_pill 0.33, do everything but refuel 3x cost" ??? plus the addendum
-- "defend_pill specifically is x6, not x3". Applied at ONE choke point
-- (goal_selection's pool loop in goals.lua), keyed on goal.kind, so every pool
-- is covered:
--
--   attack_pill                     x1  (EXEMPT ??? the one job)
--   refuel_at_base / flee_to_base   x1  (EXEMPT ??? the whole "refuel" GOAL_GROUP;
--                                        a suicider still keeps itself fuelled)
--   defend_pill                     x6  PILL_SUICIDER_DEFEND_MULT
--   capture_pill / place_pill_strategic / offensive_build / wait_for_lgm x1 (EXEMPT:
--     scooping and fielding the pills it kills IS the job; panic drops are
--     survival; and waiting for its own LGM must never lose to the x3)
--   everything else                 x3  PILL_SUICIDER_OTHER_MULT
--     (capture_base, repair_pill, attack_base, attack_tank, kill_lgm,
--      reposition, rescue_lgm, explore, ...)
--
-- attack_pill is exempted rather than tripled along with the rest because
-- tripling EVERY pool including attack_pill would leave all relative
-- preferences unchanged (a no-op); the intent the earlier 0.333 expressed is
-- attack_pill-favoured, and exempting it is the multiplicative equivalent.
-- Net: a suicider is unwilling to do anything but hit pills and stay fuelled.
M.PILL_SUICIDER_OTHER_MULT  = 3.0  -- x3 on every non-exempt goal kind
M.PILL_SUICIDER_DEFEND_MULT = 6.0  -- x6 on defend_pill specifically (parking on a pill is the LAST thing it should do)
-- Recruitment (slice 2): a soldier answers a nearby commander's pill take when
-- it's in a follow-the-call state and not too low on resources.
M.SQUAD_MIN_HELP_SHELLS  = 3   -- below this shells a soldier won't answer (hard decline)
-- Armour needed to OPEN/LEAD a blitz (be its commander). Joining has no armour
-- floor ??? a partner shares the fire ??? EXCEPT while carrying a pillbox (cautious
-- mode): then a joiner needs commander-level armour too, so it doesn't risk the
-- pill it's holding by diving in weak. Established leaders aren't demoted if
-- their armour later drops (avoids abandoning a take mid-flight).
M.SQUAD_COMMANDER_MIN_ARMOUR = 30
M.SQUAD_REFUEL_OK_ARMOUR = 25  -- a refueling soldier may answer only if already at/above this
M.SQUAD_REFUEL_OK_SHELLS = 10  -- ...and this (i.e. it was topping off, not desperate)
M.SQUAD_HELP_RANGE       = 30  -- tiles; only answer a commander whose pill is within this
M.SQUAD_CMD_RACE_TOL     = 3   -- ticks; two blitz calls on one pill opened within this of each other count as a same-tick race (broken by lower player id); otherwise first-to-the-take keeps command
M.SQUAD_BLITZ_AIM_TOL    = 8   -- brad; a blitz soldier must be facing the pill within this before it reports rdy=1 (so on GO it can fire/charge immediately, not spin to aim)
M.SQUAD_MAX_SIZE         = 3   -- max SOLDIERS per squad; with the commander that's 4 tanks total per blitz. A full squad recruits no more. 2026-09-05: raised 1 -> 3 (default party 2..2 -> 2..4). Bigger calls were only reachable through a per-bot "blitz=2/4" token (the Survival scenario's WAVE_BLITZ_MAX), and they worked much better than 2-tank takes: the party MIN is unchanged at 2, so a call still GOes with two tanks, it just no longer turns away a third and fourth joiner. DEFAULT ONLY: it seeds squad.lua's runtime party MAX (= this + 1, tanks including the commander); a per-bot "blitz=MIN[/MAX]" BRAIN_INIT_ARG token replaces it, so read squad.blitz_max()/squad.blitz_soldier_cap(), never this constant
M.SQUAD_BLITZ_COST       = 30  -- flat attack_pill cost a squad soldier assigns its commander's blitz pill: low enough to win normal goals, high enough that attack_tank/flee/refuel can still preempt
M.SQUAD_BLITZ_BUCKET     = 5   -- a blitz standoff is picked at random from clear-LOS spots scoring within this of the best
M.SQUAD_BLITZ_GO_EARLY_READY = 2   -- commander fires GO as soon as this many TOTAL blitzers are ready (in position + aimed), without waiting for the rest or the READY_TIMEOUT. Counts the commander as 1 (same convention as BLITZ_MIN_READY_TO_CHARGE), so 2 = commander + 1 ready soldier already goes; a still-approaching extra joins on the broadcast GO. DEFAULT ONLY: together with BLITZ_MIN_READY_TO_CHARGE (max of the two, both 2) it seeds squad.lua's ONE runtime party MIN, which a per-bot "blitz=" token replaces; the code reads squad.blitz_min().
M.SQUAD_BLITZ_READY_TIMEOUT = 550  -- ticks (~11s @ 50Hz) the commander waits in blitz_wait for ALL soldiers to report rdy before firing GO anyway -- but only if the party (commander + committed soldiers) has reached squad.blitz_min(); short-handed at the deadline the commander ABANDONS the blitz instead. Sized to cover a worst-case in-place aim: a 180-deg turn on swamp/crater/river/rubble (turn rate 0.25 brad/tick) is ~512 ticks (~10.4s), so the timeout must exceed that or the commander GOes before a slow-terrain soldier can finish turning to face the pill. (A genuinely stuck/dead soldier still can't stall past this.)
M.SQUAD_BLITZ_WAIT_TIMEOUT  = 1500 -- ticks (~30s) hard backstop: a soldier holding in blitz_wait abandons the take if the commander's GO never arrives (commander silently stuck/disconnected). Faster aborts (commander died / retargeted) fire on their own signals.
M.SQUAD_BLITZ_PROGRESS_CHECK = 250  -- ticks (~5s): commander re-checks soldier approach this often in blitz_wait; if the closest still-coming soldier got closer since last check, the ready-timeout is extended by another PROGRESS_CHECK (keep waiting on a tank that's still closing; stop extending once it stalls). Also the size of the grace a near-deadline "where are you?" query (SQUAD_BLITZ_QUERY_LEAD) buys an unseen-but-closing soldier.
M.SQUAD_BLITZ_BD_REFRESH_TICKS = 500  -- ticks (~10s): how often a COMMITTED, still-approaching soldier recomputes its broadcast walk distance (bd) as a FALLBACK signal. The commander measures visible soldiers' progress itself every tick from their live tank positions, so this slow, >=1-tile-quantized broadcast only matters when a soldier is out of the commander's perception ??? kept coarse to avoid /info spam. A direct commander query (bwq) force-refreshes it immediately when it's about to matter (see SQUAD_BLITZ_QUERY_LEAD).
M.SQUAD_BLITZ_QUERY_LEAD = 50  -- ticks (~1s): when the blitz_wait GO timeout is this close to firing AND a still-pending soldier is out of the commander's sight (so it's relying on that soldier's coarse broadcast bd), the commander broadcasts a "where are you now?" query (bwq). The soldier answers by force-refreshing bd this tick; if the fresh answer shows it's still closing, the commander grants one more PROGRESS_CHECK window instead of giving up.
M.SQUAD_BLITZ_CLASH_TILES = 2   -- two standoffs within this EUCLIDEAN distance (tiles) conflict; the commander makes the nearer/junior soldier repick so spots stay >= this far apart
M.SQUAD_BLITZ_PREEMPT_TANK_TILES = 10  -- a committed blitz only yields to attack_tank when the hostile tank is within this many tiles of us (close enough to actually threaten); farther tanks don't break the blitz
M.BLITZ_MIN_SUICIDERS = 0  -- at GO the commander designates random SOLDIERS as temporary pill_suiciders until at least this many members of the blitz are suiciders, counting those already suiciders by token or slate (and counting the commander itself); never a bot that already is one, and the commander designates ITSELF only when the soldiers cannot cover the minimum. 0 = never designate = today's behaviour. DEFAULT ONLY: a per-bot "blitzsuiciders=N" BRAIN_INIT_ARG token replaces it, so read squad.blitz_min_suiciders()
M.BLITZ_SUICIDER_MAX_TICKS = 1600  -- ticks (~32s) hard backstop on a blitz-designated suicider: 2 x SQUAD_BLITZ_READY_TIMEOUT (550) plus a ~500-tick charge window. The designation normally ends when the blitz does (pill dead/taken, take abandoned, commander gone, death); this is the "nothing ever told us" floor so a bot can't stay a suicider for the rest of the game
M.BLITZ_CONTESTED_RANGE = 9  -- EUCLIDEAN tiles: the take of a pill is CONTESTED when a live hostile TANK is seen this close to that pill. Only REAL sightings count (perception's enemy_tanks, which is hostile-only) -- never a ghost, which is a guess at where an unseen tank went and is no basis for rewriting the whole party's role. 9 is a bit over tank gun range, so "an enemy that can already shoot at the take, plus slack"
M.BLITZ_CONTESTED_ALL_SUICIDERS = true  -- 2026-09-05: on a CONTESTED take (see BLITZ_CONTESTED_RANGE) the commander designates EVERY blitzer -- all soldiers AND itself -- a temporary pill_suicider at GO, regardless of BLITZ_MIN_SUICIDERS, and re-checks on its replans while the take is live in case an enemy arrives later. A contested take is the one that usually gets undone: the defender's LGM walks back out and repairs the pill while our survivors reload. Everyone going in as a suicider means at least one normally survives, and a suicider is the role that actually hunts the repairing LGM instead of backing off. Applies to every game, not just the Survival scenario. false = today's behaviour (only the BLITZ_MIN_SUICIDERS quota). Precedence is unchanged: permanent "suicider"/"nosuicider" token > blitz temp designation > harasser slate; a "noblitz" bot is in no blitz so it is never designated
M.BLITZ_ABORT_BUILD_ON_READY = true  -- if a soldier JOINS while the commander is mid build_walls (laying its guard pills/shield), abandon the remaining blocks and rally NOW (build_walls -> blitz_wait, unshielded charge route). The joiner's simultaneous overwhelm replaces the shield as protection ??? same routing as if the joiner had answered before the in-position decision. Off = finish the shield first, then rally (original behavior).
M.BLITZ_MIN_READY_TO_CHARGE = 2  -- (used with BLITZ_ABORT_BUILD_ON_READY) DEFAULT ONLY, see SQUAD_BLITZ_GO_EARLY_READY: minimum READY blitzers ??? total tanks in position and aimed (rdy=1) ??? required before the commander abandons the build and charges. The commander itself always counts as 1 (it's at its standoff). 2 = commander + one ready soldier; a still-approaching 3rd is left to keep closing and joins the charge when it arrives. Default 2 = original abort-on-join behavior.
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
M.SQUAD_BLITZ_REPICK_GAP  = 1   -- min ticks between standoff repicks. Safe at 1: brj rejects are spot-tagged "pn:[mx,my]", so a soldier only repicks when the commander rejected the spot it is CURRENTLY offering ??? a stale reject for an already-abandoned spot is ignored, so there's no candidate-list burn (see read_cmdr_brj).
-- Pick-to-join blitz discount: while negotiating to join a commander's blitz,
-- multiply the candidate pill's pool-6 cost by a distance factor (<=1, so it
-- only ever discounts). Distance is the dijkstra slate path cost to the pill.
-- Piecewise: <=NEAR -> NEAR_MULT; NEAR..MID -> NEAR_MULT..MID_MULT;
-- MID..FULL -> MID_MULT..1.0; >=FULL -> 1.0 (no discount). A pill whose entry
-- is reject/sentinel'd (no usable cost, e.g. armour_too_low on a hard pill ???
-- the typical blitz target) gets factor * REF_COST instead so it's still joinable.
M.SQUAD_BLITZ_JOIN_MIN_MULT    = 0.25  -- biggest blitz-join discount (factor at d=0); exponential rises to 1.0 (no discount) at FULL_TILES
M.SQUAD_BLITZ_JOIN_FULL_TILES  = 20    -- path tiles at which the join discount fully vanishes; beyond this there's no pull to join (the score, not a range gate, decides)
M.SQUAD_BLITZ_JOIN_REF_COST    = 120
M.SQUAD_BLITZ_INRANGE_TILES    = 9    -- euclidean tiles: if an OPEN, not-full blitz pill (or the blitzing commander's tank) is within this shooting distance of our tank, stack an extra flat discount on its attack_pill (we could already help kill it). ~tank gun range + slack.
M.SQUAD_BLITZ_INRANGE_MULT     = 0.5  -- the extra in-shooting-range multiplier (0.5 = another -50% on top of the distance-curve join discount)
M.AMMO_DEPRIVED_BLITZ_MULT     = 0.6  -- ammo_deprived bots: flat overall multiplier on the blitz-join factor (40% off, near AND far ??? a far call at factor 1.0 becomes 0.6, joinable) so a useless-for-shooting tank throws itself into blitzes more readily; distance shape unchanged
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
-- a DROPPED pill to grab ??? a dead (not-in-tank) pillbox within PILL_RANGE tiles
-- (euclidean). The whole point of the ally-death interrupt is noticing pills the
-- dead teammate dropped; with nothing to grab it only churned a live goal to none.
M.TANK_DEATH_REPLAN_ALLY_RANGE = 20
M.TANK_DEATH_REPLAN_PILL_RANGE = 12

-- JSONL logger (logger.lua) per-tick accumulator cap. logger.event /
-- logger.reason append all through a tick and the whole set is serialized
-- into that tick's record. This caps how many of each a single tick may
-- carry: past the cap new entries are dropped and counted, and one summary
-- entry ("logger:dropped N events (cap M)") lands in the record instead.
-- Backstop only ??? a tick that legitimately wants more than this is already
-- producing a log line nobody can read, and an uncapped tick record is how
-- a serialization overrun becomes a permanent per-tick budget kill.
M.LOGGER_TICK_MAX_ENTRIES = 256

-- =========================================================================
-- vulnerability / imdanger ??? the two 0-100 build scores (danger.lua)
-- =========================================================================
-- Both start at 50 and move up (better) or down (worse). 50 is neutral, 75
-- is in our favour, 25 is real concern. Every factor below is a signed
-- contribution to that 50, and they are sized so the full set spans the
-- range -- see VULNERABILITY_AND_BUILDS_PLAN.md for the derivations.
--
-- vulnerability = what I stand to lose (armour + cargo). Intrinsic to the
-- tank; nothing about the world.
M.VULN_ARMOUR_SPAN     = 50    -- full armour = +25, empty = -25 (span/2 each way)
M.VULN_ARMOUR_CAP      = 40    -- min(armour, this): the engine marks DEATH by
                               -- pushing armour ABOVE TANK_FULL_ARMOUR, which
                               -- would otherwise read a corpse as safest.
M.VULN_CARRY_EMPTY     = 25    -- 0 pills: nothing to lose
M.VULN_CARRY_FULL      = -25   -- 4+ pills: everything to lose (linear from 1 pill = 0)
M.VULN_CARRY_SATURATE  = 4     -- pills at which the carry term bottoms out

-- imdanger = what is arriving at me. Environmental; nothing about our state.
M.IMD_COVER_MAX        = 30    -- our pills covering us, max contribution
M.IMD_COVER_PER_UNIT   = 10    -- 3 calm pills, or 1 heated one, reaches the max
M.IMD_COVER_HEATED_MULT= 3     -- a fast-firing pill counts triple
M.HEATED_ANGER         = 0.6   -- anger at/above this = "heated". TWO hits:
                               -- PILL_ANGER_BUMP 0.3333 x2 = 0.6666 clears it,
                               -- and the engine HALVES reload per hit
                               -- (100 -> 50 -> 25 -> 12 -> 6), so two hits is
                               -- 4x normal output. 0.67 would have meant three.
M.IMD_COVER_W_NEAR     = 1.00  -- d <= 4
M.IMD_COVER_W_MID      = 0.75  -- 4 < d <= 8   (8 = a pillbox's true reach)
M.IMD_COVER_W_FAR      = 0.25  -- 8 < d <= 9   (PILL_RANGE_MAP's 1-tile margin)
M.IMD_EXPOSURE_MAX     = -10   -- hostile pill has our tile in range
M.IMD_EXPOSURE_DIV     = 100   -- threat.pill_at value at which exposure saturates
M.IMD_SHELLS_ONE       = -8    -- one hostile shell arriving
M.IMD_SHELLS_MANY      = -15   -- two or more
M.IMD_SHELLS_RADIUS_WU = 768   -- 3 tiles. Wider than the 160-WU hit test: the
                               -- signal wanted is "someone is shooting at us
                               -- and may correct their aim", and at 0.625
                               -- tiles a shell flickers hit/miss as we swerve.
M.IMD_ODDS_MAX         = -35   -- outnumbered up close
M.IMD_ODDS_FAR_MAX     = -10   -- a gathering at range worries only a dying bot
M.IMD_ODDS_SCALE       = 47    -- one unopposed enemy inside 8 saturates ODDS_MAX
M.IMD_ODDS_W_NEAR      = 0.75  -- d <= 8
M.IMD_ODDS_W_FAR       = 0.25  -- 8 < d <= 15
M.IMD_ODDS_NEAR_TILES  = 8
M.IMD_ODDS_FAR_TILES   = 15
M.SCORE_ALLY_MAX_AGE   = 1750  -- ticks: ally slate older than this is ignored
                               -- when excluding enemies an ally is engaging

-- Emergency-build pill spacing. Chebyshev, and a RANKING key rather than a
-- rejection: a spot is classified clear / diagonal / orthogonal by the worst
-- crowding against any pill we already have, and the search prefers the best
-- class available. Nothing is refused for spacing -- dying with pills aboard
-- is worse than a badly spaced pill.
--
-- The comparator is STRICTLY LESS THAN: gap 2 makes exactly the eight tiles
-- touching a pill non-clear (the 3x3 around it). "Within 2" would read as <= 2
-- and take in the 5x5 ring instead -- three times the area, and `clear` would
-- almost never be reachable near a pill group.
M.PANIC_BUILD_MIN_PILL_GAP = 2

-- =========================================================================
-- Placement dispatch, and giving up (builder.lua)
-- =========================================================================
-- Send the builder from where we stand instead of driving onto the spot first,
-- when ANY of these fires. The old rule was pdist <= 1 for a normal placement,
-- so the tank had to close the distance itself -- and arriving is what made the
-- answer "no".
M.PLACE_DISPATCH_VULN     = 30   -- vulnerability at/below this => dispatch
M.PLACE_DISPATCH_IMDANGER = 30   -- imdanger at/below this => dispatch
M.PLACE_DISPATCH_MAX_DIST = 15   -- tiles: furthest we will send the builder on a
                                 -- trigger. Beyond this the walk is too long to
                                 -- be worth committing to (it cannot be recalled).

-- The LGM danger gate, neutralised rather than deleted at its call site: a
-- threshold nothing exceeds, so the path sample is never taken for a placement.
-- lgm_path_safe_enhanced samples a STRAIGHT LINE the builder does not walk,
-- against a danger field built for TANKS -- and pillboxes never target builders.
-- It refused on a model that does not apply to the unit it protects, and being
-- shot at closed it permanently, which is exactly when a guard pill pays.
-- Other callers (repair, wall builds, demine) keep their real thresholds.
M.LGM_DANGER_YOLO = 1e9

-- Giving up on a placement spot. Four clauses, see builder.decide.
M.PLACE_REFUSE_GIVEUP_TICKS = 100  -- ~2s of CONSECUTIVE gate refusals of one
                                   -- target => block and abandon. Sized against
                                   -- the 20260827 incident: bot3 was parked 155
                                   -- ticks between arriving and dying, so a
                                   -- 600-tick trigger would never have fired.
M.PLACE_GIVEUP_BLOCK_TICKS  = 600  -- 12s the abandoned tile stays blocked.
                                   -- lgm_can_reach measures from the tank's
                                   -- CURRENT position, so unreachable is not a
                                   -- property of the tile -- from six tiles east
                                   -- the answer may flip. Hence a timed block.

-- Map-edge band. minesExistPos returns TRUE unconditionally inside it
-- (global.h MAP_MINE_EDGE_*), so the engine refuses ANY build there, mine or
-- not. Mirrored brain-side or an edge placement refuses forever with nothing in
-- the log to say why.
M.MAP_EDGE_BAND = 20

-- Placement trip flag (state._place_trip) -- one record per builder dispatch
-- for a placement, harvest or not. See VULNERABILITY_AND_BUILDS_PLAN.md,
-- "The trip flag" / "Harvest, then place".
M.PLACE_TRIP_ACCEPT_TICKS  = 5    -- LGM still aboard this long after the request:
                                  -- the engine refused it, clear (not_accepted)
M.PLACE_TRIP_MAX_TICKS     = 2000 -- builder never came back (40s): clear (timeout)
                                  -- so wait_for_lgm suppression cannot stick forever
M.HARVEST_RESUME_MARGIN_ABS  = 30   -- resume unless the tile's score fell by more
M.HARVEST_RESUME_MARGIN_FRAC = 0.15 -- than max(ABS, dispatch_score x FRAC)
M.HARVEST_RESUME_MAX_TICKS   = 25   -- resume request not dispatched within this
                                    -- many ticks (decide() early-returned every
                                    -- tick, e.g. water_build): drop it (stalled)

-- ── Follow-through: finish the build you harvested for (2026-09-02) ───────
-- While a HARVEST trip is live the builder is out of the tank, so
-- eval_place_pill_strategic's `actionable` gate (carried >= 1 AND
-- man == LGM_INTANK) returned nil and the placement row simply vanished from
-- the pool for the whole walk. The tank then took whatever won next -- at
-- 20260902_030233 bot2 t=37012 that was refuel_at_base 22 tiles away -- and
-- when the man came back with the wood the re-score had collapsed (311 -> 182)
-- because half of the placement score is measured from where the TANK stands
-- (strategic centre, spike base, nearest hostile pill). The harvest was thrown
-- away (HARVEST_DROP worse_than_margin) and placement restarted elsewhere.
--
-- So while the trip is live the placement keeps bidding, at a FLAT price: this
-- is not "drive somewhere and build", it is "stay where you already are until
-- the man gets back", which costs nothing but the wait. Where 25 sits, cheapest
-- first, in the band it competes against:
--   10    take_cover haul / panic floor (TAKE_COVER_HAUL_FLOOR)  -- BEATS us
--   20-30 a real attack_tank engage (TANK_COMBAT_BASE_COST 30)   -- BEATS us
--         (and goal_selection's "a normal place_pill_strategic must never
--          out-rank an attack_tank" clamp still applies to this row, so a live
--          tank fight takes the tank whatever the arithmetic says)
--   25    >>> PLACE_FOLLOW_THROUGH_COST <<<
--   30    defend_pill WATCH (DEFEND_WATCH_COST)                  -- loses
--   34    seek_trees at carry=1 (SEEK_TREES_BASE_COST 40 - 6)    -- loses
--   40    defend_pill REPAIR (DEFEND_REPAIR_COST)                -- loses
--   45+   refuel (REFUEL_BASE_COST floor; ~60-130 at 35 armour)  -- loses
--   60    attack_base / STRATEGIC_PLACE_BASE_COST                -- loses
--   120   calm take_cover (TAKE_COVER_CALM_MIN)                  -- loses
-- The row is pinned to this number AFTER the phase weight (place_strategic is
-- x3.0 in opening, x0.7 mid) for the same reason the number is flat at all --
-- a hold has no travel and no local phase preference to scale -- and it is
-- exempt from the influence x2/x0.5 like defend_pill: the trip tile's influence
-- is a property of the spot the pool already picked, not of standing near it.
M.PLACE_FOLLOW_THROUGH_COST = 25
-- How far from the trip tile the tank may hold while the man is out. Reuses the
-- builder's CALM dispatch range (REPAIR_DISPATCH_DIST_BASE, 5) rather than a
-- number of its own: being inside it is exactly the condition for the resume
-- dispatch to be a short walk for the returning builder. The tank does NOT
-- have to stand ON the tile -- the man is walking to it.
M.PLACE_FOLLOW_THROUGH_HOLD_DIST = M.REPAIR_DISPATCH_DIST_BASE
-- =========================================================================
-- take_cover (2026-09-01)
-- =========================================================================
-- A goal whose whole job is "stand somewhere less lethal".  Two field
-- incidents drove it (20260901_000042_1_loss_b6 bot3):
--   A t=11075 -- carrying 2 pills with a DEAD builder, two enemy tanks
--     closing.  The only "get out of here" reaction the brain had was a
--     BUILD (danger.should_panic_build), which returns false with "lgm_out"
--     when the builder is gone, so the bot stood still and then died 1v2.
--   B t=16625 -- full tank, one tile from a forest, parked inside an ANGRY
--     hostile pill's range for 130 ticks: defend_pill produced no bid
--     (heat blocked), attack_tank was gated by pill_crossfire, repair
--     needed the LGM, and the haul flee's destination picker rejected every
--     base as "empty" because a full tank needs nothing.
--
-- take_cover is ALWAYS evaluated and ALWAYS emits a pool row (pool 14), so
-- its score is visible even when it loses; when it genuinely shouldn't be
-- considered the row carries a reject reason instead of vanishing.
--
-- Per-tile safety, every term hand-computable from the printed row:
--   safety(t) = W_COVER*cover(t) - W_EXPO*expo(t)
--               - W_ENEMY*enemy(t) + W_ALLY*ally(t)
--   pick      = argmax over candidates of safety(t) - W_TRAVEL*travel(t)
--   margin    = safety(pick) - safety(here)
M.TAKE_COVER_W_COVER   = 8     -- per cover UNIT (danger.cover_weight sum, capped at
                               -- IMD_COVER_MAX/IMD_COVER_PER_UNIT = 3): one pill
                               -- inside 4 tiles is worth 8 safety points.
M.TAKE_COVER_W_EXPO    = 0.15  -- per point of threat.pill_at: an angry pill's ~204
                               -- at its centre costs ~30 (= one covering own pill
                               -- x4), a calm pill's 8 costs ~1.
M.TAKE_COVER_W_ENEMY   = 20    -- per danger.odds_weight unit of visible enemy tank:
                               -- one enemy inside 8 tiles (w 0.75) costs 15.
M.TAKE_COVER_W_ALLY    = 4     -- per odds_weight unit of visible allied tank: a
                               -- friend nearby helps, but a quarter as much as an
                               -- enemy hurts (we are the one being shot at).
M.TAKE_COVER_W_TRAVEL  = 0.6   -- per unit of smart_cost. Grass is 2.0/tile in
                               -- brain_pathfinder.c, so a 6-tile grass drive costs
                               -- ~7 points -- roughly one own-pill's worth of cover.
M.TAKE_COVER_MIN_MARGIN = 8    -- with NO trigger, only bid when the best tile is at
                               -- least this much safer than standing still (about
                               -- one covering pill); otherwise REJECT no_safer_tile.
-- ...but "safer" has to mean SAFER FROM SOMETHING. The margin is the sum of
-- four terms and two of them (cover, ally) are COMFORT: a tile that is merely
-- shaded by one more of our own pills, with the same zero incoming pill fire
-- and the same zero enemy tanks as where we stand, is not an escape. At
-- 20260902_000405 bot2 t=22561 the whole 12.0 margin was cover*8 + ally*4 with
-- expo and enemy identical at both tiles -- nothing was being avoided -- and
-- that calm bid still preempted a live defend on a pill being shelled at 5
-- hits/s. So the CALM branch (the one with no real-danger trigger behind it)
-- gates on the DANGER half only: W_EXPO * dexpo + W_ENEMY * denemy, i.e. the
-- harm the move actually avoids. The full margin still sets the discount.
M.TAKE_COVER_MIN_DANGER_MARGIN = 8  -- calm branch: how much AVOIDED HARM (pill
                               -- fire + enemy tanks) the pick must buy before
                               -- take_cover bids at all.
M.TAKE_COVER_BASE_COST = 60    -- untriggered/bad-ground bid before the margin
                               -- discount: loses to a real attack (20-30) and to
                               -- capture work, beats seek_trees filler at 34+.
M.TAKE_COVER_CALM_MIN  = 120   -- ...but a CALM bid (no haul, no panic, no bad
                               -- ground -- just a safer tile) may only ever beat
                               -- FILLER. 120 sits above the whole live band it
                               -- was preempting -- attack_tank engage (~20-30),
                               -- a defend rescue (100-300), a close repair
                               -- (<100) -- and below explore (500), so calm
                               -- cover is what you do when there is genuinely
                               -- nothing else. (seek_trees, 34 at carry 1, also
                               -- outbids it now; wanting wood while nothing is
                               -- shooting at you is the same kind of idle.)
                               -- calm cost = max(CALM_MIN, BASE_COST - K*margin),
                               -- so with BASE_COST 60 the discount is inert and
                               -- the calm bid is simply the floor -- deliberate:
                               -- the discount was what let the margin talk it
                               -- down into the live band in the first place.
M.TAKE_COVER_K         = 1.0   -- cost = max(1, BASE_COST - K * margin)
M.TAKE_COVER_HAUL_FLOOR = 10   -- the "get the cargo out" / "panic but cannot build"
                               -- floor. Below attack_tank's engage band on purpose:
                               -- when this fires, leaving IS the plan.
M.TAKE_COVER_BAD_GROUND_PILL_AT = M.TANK_COMBAT_DEFENDED_DANGER  -- 30 = "an angry
                               -- pill covers this tile"; same line attack_tank
                               -- disengages on, so the two agree about bad ground.
M.TAKE_COVER_SCAN_TICKS   = 10   -- reuse the last scan for this many ticks
M.TAKE_COVER_STICKY_TICKS = 500  -- keep a chosen tile this long (as pick_wait_spot)
M.TAKE_COVER_STICKY_DIST  = 12   -- ...unless the tank wandered further than this
M.TAKE_COVER_HOLD_RELEASE_MARGIN = 4  -- while holding, release once standing here is
                               -- within this much of how safe the pick looked
M.TAKE_COVER_PILL_NEIGHBOUR_RANGE = 15  -- own/allied pills within this many tiles
                               -- contribute their 8 neighbours as candidates
M.TAKE_COVER_BASE_TILE_RANGE      = 15  -- friendly base tiles within this many
M.TAKE_COVER_REJECT_COST = 1e8 -- sentinel for a rejected pool-14 entry: never wins,
                               -- but < 1e29 so the WINNERS strip still renders it
                               -- (same trick as REPOSITION_REJECT_COST)

-- defend_pill ARRIVED-but-heat-blocked "watch" bid. Previously that case
-- produced NO bid at all, which is how incident B ended up on seek_trees
-- inside an angry pill's range. Must LOSE to a real attack_tank (~20-30)
-- and to take_cover's haul floor (10), and BEAT seek_trees (40 - carry*6,
-- so 34 at carry=1).
M.DEFEND_WATCH_COST = 30

-- defend_pill ARRIVED-and-we-can-actually-FIX-it bid. The watch ladder had no
-- rung for "the shelling stopped, I am parked next to a chewed-up pill with the
-- LGM aboard and 23 trees". taking_damage blocks heat for DEFEND_DMG_FRESH_TICKS
-- (400 = 8 s), which drops the bid to WATCH and the defender then babysits a
-- 9/15 pill it could have healed. Priced BETWEEN watch (30) and heat (200):
-- repairing beats standing around, but a live attack_tank (~20-30) or a haul
-- flee (10) still wins, and the LGM interlock in builder.lua is what actually
-- decides whether the man leaves the tank.
M.DEFEND_REPAIR_COST = 40

-- ── sea-pill harvest (2026-09-01) ─────────────────────────────────────────
-- Dead pills sitting in DEEP SEA off a shore. capture_pill's deep-sea branch
-- prices a mine→crater→river→boat entrance instead of the old flat
-- `deepsea_no_boat` reject. See SEA_PILL_HARVEST_PLAN.md.
M.SEA_PILL_ENABLED            = true  -- master switch for the deep-sea branch of capture_pill
M.SEA_PILL_SCAN_PERIOD        = 50    -- ticks between full re-plans (matches the pool's own 50t CAPTURE_CAND cadence)
M.SEA_PILL_CLUSTER_RADIUS     = 3     -- dead sea pills within this many tiles are ONE harvest cluster (one boat trip takes them all)
M.SEA_PILL_ENTRANCE_MAX_DIST  = 12    -- how far from the cluster we will look for a water entrance S
M.PILLBOX_RANGE_WU            = 2048  -- pillbox.h PILLBOX_RANGE. util.c utilIsItemInRange is INCLUSIVE and euclidean
                                      -- in world units, so this is the exact circle a pill can fire into
M.SEA_COVER_MARGIN_CALM_WU    = 0     -- a CALM pill (anger < HEATED_ANGER) reloads slowly and has to notice us first:
                                      -- sail right up to its 8-tile line
M.SEA_COVER_MARGIN_HOT_WU     = 256   -- a HEATED pill is already firing on a short reload: keep one tile of buffer
                                      -- (covered radius 9.0 tiles instead of 8.0)
M.SEA_PILL_PILL_SAFE_RANGE    = 9     -- PILL_FIRE_RANGE(8) + 1, measured EUCLIDEAN (U.edist): every hostile/neutral pill
                                      -- this close to a cluster tile or a boat-path tile gets the line-of-fire test
M.SEA_PILL_ENEMY_TANK_NEAR    = 10    -- a visible enemy tank this close (EUCLIDEAN) to the cluster or the boat path rejects
                                      -- (or aborts) the harvest — a boat is one shell from dead
M.SEA_PILL_MIN_SHELLS         = 3     -- shells reserved to detonate the mine: one to land on it, +1 slack, +1 margin
M.SEA_PILL_MIN_SHELLS_FOREST  = 5     -- ...and more when a FOREST tile sits in the F->S lane: a shell dies on the first
                                      -- forest it meets and only turns it to grass (shells.c:803), so that one is wasted
M.SEA_PILL_FIRE_DIST          = 2     -- F sits exactly this many tiles from S: 512 wu > the 384 wu mine blast box
M.SEA_PILL_MAX_DETONATE_SHOTS = 4     -- give up (and replan) after this many shells at the mine
M.SEA_BOAT_TREES              = 20    -- LGM_COST_BOAT (lgm.h:54): a wall built on RIVER is a BOAT and costs 20 trees
M.SEA_MINE_TREES              = 1     -- LGM_COST_MINE (lgm.h:57): laying the mine itself costs 1 tree (plus 1 mine)
M.SEA_PILL_TREES_TOTAL        = 21    -- 20 + 1 — what a mine-then-boat entrance needs IN HAND before lay_mine may start
                                      -- (a live mine on our own shore while the LGM is off farming is the failure mode)
M.SEA_TREES_PER_FOREST        = 4     -- LGM_GATHER_TREE (lgm.h:61): one harvested forest tile yields 4 trees
M.SEA_TREES_RADIUS            = 12    -- forest is only counted as obtainable within this many tiles of S or of the tank
M.SEA_COMPONENT_BOX           = 16    -- BFS box (tiles either side of the cluster) for the connected-water scan
M.SEA_COMPONENT_MIN_WATER     = 3     -- the pills' water must hold at least this many tiles MORE than the pills themselves,
                                      -- or there is nothing for a boat to sail (a one-tile puddle is not a sea)
M.SEA_COMPONENT_MAX_TILES     = 1200  -- hard cap on that BFS so an ocean map cannot flood the whole grid each rescan
M.SEA_FLOOD_WAIT_TICKS        = 900   -- how long to wait after the mine goes off for the crater to flood to RIVER
                                      -- (floodfill.c counts FLOOD_FILL_WAIT down before it converts; it is not instant)
M.SEA_TREES_MIN_INFLUENCE     = 0     -- SUPERSEDED BY THE COVERAGE TEST (2026-09-01). sea_count_safe_forest used to
                                      -- require cpf.influence_at above this to call a forest tile harvestable. par2 bot3
                                      -- t=23410 showed why that is the wrong question: two hostile BASES owned the corner
                                      -- by influence, so a shore we were actively raiding with four pills aboard read as
                                      -- "no trees in territory" for the rest of the game. The gate is now "no live
                                      -- hostile/neutral PILLBOX covers the tile" (range + heated margin + line of fire,
                                      -- the same rule the boat uses for water). Nothing reads this value any more; it is
                                      -- left defined only so an old config that sets it does not become a nil-index.
M.SEA_PILL_TREE_LEG_PER_TREE  = 6     -- cost charged per missing tree for the seek_trees leg (≈ one LGM farm round trip / 4 trees)
M.SEA_NOGO_SPEED_CAP          = 16    -- info.speed cap (0..64) while covered water is near: a boat's turn RADIUS grows
                                      -- with speed (the turn RATE is terrain-fixed), so 52 overshoots a turn by ~1 tile
                                      -- and 16 by ~0.3. Slowing early is what keeps it out; braking late does not
M.SEA_NOGO_SLOW_RADIUS        = 1.5   -- tiles: within this of a covered tile, hold the cap
M.SEA_NOGO_HEADING_RADIUS     = 3     -- tiles: within this, never accelerate while turning hard
M.SEA_NOGO_TURN_BRADS         = 32    -- 32/256 = 45 degrees off the next waypoint counts as "turning hard"
M.SEA_NOGO_AVOID_PENALTY      = 30000 -- trace-time cost for covered water in the nav obstacle set: a wall in all but name
M.SEA_ROUTE_LOOKAHEAD         = 3     -- waypoints ahead on the safe water route the boat steers for while collecting
M.SEA_PILL_BOAT_STEP_COST     = 12    -- cost per boat-path tile when scoring candidate S tiles (river/boat terrain cost ≈ grass)
M.SEA_PILL_DANGER_W           = 2.0   -- weight on threat.pill_at along S and the boat path in the S score
M.SEA_PILL_COST_MULT          = 0.3   -- the pills are FREE, so the whole trip is priced at 30% of its travel
M.SEA_PILL_COST_FLOOR         = 5     -- ...but never below this, so a real fight beside us still wins
M.SEA_PILL_COMMITMENT         = 400   -- hysteresis bonus from lay_mine until the boat exists (a live mine on our own shore must be finished)
M.SEA_PILL_LEG_COMMITMENT     = 150   -- smaller surcharge for the NON-committed substates of a live plan (fetching a
                                      -- mine, farming the wood, driving to the firing spot). A plan with resource legs
                                      -- prices around 25 and loses to routine goals; without this the bot walks off
                                      -- mid-leg and starts over
M.SEA_PILL_ABORT_RECHECK      = 25    -- ticks between abort re-checks while holding at F
M.SEA_PILL_LOG_PERIOD         = 50    -- rate limit for SEA_REJECT lines
M.SEA_PILL_BLACKLIST_TICKS    = 1500  -- an S that did not flood is off the table this long
M.SEA_PILL_SUB_TIMEOUT        = 3000  -- no progress in a substate this long → abort and replan. The resource legs
                                      -- reset it on every tree/mine that arrives, so this only fires on a real stall
M.POOL_REJECT_COST            = 1e30  -- the ONE cost a rejected pool row displays. Shaping multipliers (influence x2,
                                      -- suicider, phase weight, hysteresis) skip it and it is pinned back before the
                                      -- sort, so FINAL_SCORES shows 1e30 instead of an unreadable 5e43
M.SEA_BASE_STOCK_STALE        = 3000  -- a base whose stock reading is older than this counts as "never read" when
                                      -- looking for mines: base stock is only reported first-hand up close, so an
                                      -- unvisited base must not read as empty (that cost sea_pills_D 500 ticks of
                                      -- explore while sitting 4 tiles from a base holding 90 mines)
M.SEA_PILL_CLAIM_RADIUS       = 3     -- an ally's deep-sea capture target claims every dead sea pill this close (same as the cluster radius)

-- =========================================================================
-- BEGIN pill-damage attribution + land capture clusters (2026-09-01)
-- Added for the 20260901_160325_1_par2 bot3 incident (t=17930-18600): six
-- free dead pills sat unclaimed for 700 ticks while defend/take_cover cycled
-- on a HEALTHY team pill that was only catching STRAY shells from a neutral
-- pillbox shooting at our tank.  Three rules, in one delimited block so the
-- whole change is greppable.
-- =========================================================================

-- --- Part 1: how scary is the thing that hit our pill? -------------------
-- Pillboxes only ever fire at TANKS (pillbox.c), so a shell that lands on a
-- team pill from an enemy/neutral pillbox is a MISS aimed at somebody else.
-- A tank shelling our pill is deliberate and will finish the job; a stray is
-- noise.  The factor scales the DISCOUNT the siege tier gives (see
-- defend_pill_score): 1.0 leaves the tier exactly as it was, 0.25 keeps only
-- a quarter of its urgency.
M.DEFEND_SRC_TANK_MULT       = 1.0   -- aimed tank fire: unchanged, the old behaviour
M.DEFEND_SRC_EPILL_MULT      = 0.5   -- enemy pillbox stray: half the urgency
M.DEFEND_SRC_NPILL_MULT      = 0.25  -- neutral pillbox stray: a quarter
M.DEFEND_WATCH_MIN_HP_FRAC   = 2/3   -- ARRIVED watch on non-tank damage only below this hp
                                     -- fraction: a 13/15 pill catching strays is fine

-- Shell -> source attribution (perception.lua).  A shell's muzzle is on its
-- own back-ray: walk BACKWARDS from where we see it and see whether a live
-- hostile/neutral pillbox sits on that line.
M.PILL_SRC_SHELL_RADIUS      = 3     -- tiles: a non-friendly shell this close to a team
                                     -- pill is logged as a possible hit on it
M.PILL_SRC_SHELL_WINDOW      = 90    -- ticks a logged shell sighting stays eligible to
                                     -- explain a hit (the hp drop is reported after the
                                     -- shell object is already gone)
M.PILL_SRC_ORIGIN_SLOP_WU    = 384   -- 1.5 tiles: how far off the back-ray a pillbox
                                     -- centre may sit and still count as the muzzle
M.PILL_SRC_PRESENCE_WINDOW   = 300   -- ticks: with NO shells seen at all, a hostile-tank
                                     -- sighting stamp this fresh means "tank fire"
M.PILL_SRC_LOG_MAX           = 8     -- ring size of per-pill shell sightings

-- --- Part 2: land dead-pill clusters ------------------------------------
-- Mirrors SEA_PILL_CLUSTER_RADIUS on land.  Six dead pills in one heap are
-- one errand, not six: price each member as its share of the trip.
M.CAPTURE_CLUSTER_RADIUS      = 3    -- dead pills within this many tiles chain into one cluster
M.CAPTURE_CLUSTER_DIVISOR_MAX = 6    -- cap on the divisor (a 20-pill heap is not 20x cheaper)
M.CAPTURE_CLUSTER_MIN_COST    = 5    -- the discount can never price a capture below this
M.CAPTURE_CLUSTER_SCAN_TICKS  = 50   -- guard re-scan cadence (5 shell sims per guard pill)

-- --- Part 3: hostile territory over the cluster -------------------------
-- The discount must not walk a tank into a fortress.  Count the live
-- hostile/neutral pillboxes that can actually put a shell on a cluster tile
-- (PILLBOX_RANGE + the heated margin AND a clear line of fire, the same test
-- the sea harvest uses) and price the cluster back up.
M.CAPTURE_CLUSTER_GUARD_MULT  = 0.75 -- surcharge PER covering pill (1 -> x1.75, 2 -> x2.5)
M.CAPTURE_CLUSTER_GUARD_MAX   = 4.0  -- cap: three or more guards is already a no-go
-- =========================================================================
-- END pill-damage attribution + land capture clusters
-- =========================================================================

-- =========================================================================
-- BEGIN armour-aware base pricing + IMMINENT BASE STEAL  (20260901)
-- =========================================================================
-- ENGINE FACTS (src/bolo/internal/bases.h, src/bolo/public/global.h):
--   BASE_FULL_ARMOUR   90   a base tops out at 90 armour
--   MIN_ARMOUR_CAPTURE  9   at or below 9 the base is DEAD / drive-on capturable
--   DAMAGE              5   one tank shell takes 5 armour off a base
--   BASE_TICKS_BETWEEN_REFUEL 1000  a base regains +1 armour / +1 shell /
--                           +1 mine once per 1000 ticks (basesUpdateStock)
-- So a FULL base is ceil((90 - 9) / 5) = 17 shells away from capturable, and a
-- base at armour <= 14 is ONE shell away.
--
-- WHAT THE BOT CAN ACTUALLY SEE.  basesGetItems (bases.c ~1649) FOGS hostile
-- base armour in the per-tick object scan: an enemy base reports direction = 1
-- ("alive") or 0 ("capturable"), never its armour.  That is why
-- world.bases[id].health for a HOSTILE base is only ever 0 or 1 -- reading it as
-- an armour number is wrong (allied bases do report armour/5, i.e. 0..18, which
-- is what makes the confusion so easy).
--
-- The REAL armour reaches a bot through EVENT_BASE_STOCK.  The server culls that
-- event to neutral/allied bases for HUMANS but sends every one of them to BOTS
-- (server_sim.c ~3667: `if (evType == EVENT_BASE_STOCK && !recipientIsBot)`), and
-- world.lua stores it as bases[id].obs_armour with bases[id].obs_tick.  So
-- obs_armour/obs_tick is the ONLY armour source for a hostile base.  It is
-- emitted on CHANGE only, which means:
--   * while anyone is shelling the base it refreshes every hit (age ~0),
--   * an idle base below full refreshes once per refuel cycle (age <= 1000),
--   * a base at full armour/shells/mines never refreshes at all -- and that is
--     the safe direction, because "no reading" prices at FULL markup.
-- It also means a stale reading can only UNDERSTATE armour by the regen rate
-- (+1 per 1000 ticks); nothing but our own shells drives it down.
M.BASE_FULL_ARMOUR         = 90   -- bases.h BASE_FULL_ARMOUR
M.BASE_CAPTURE_ARMOUR      = 9    -- bases.h MIN_ARMOUR_CAPTURE
M.BASE_SHELL_DAMAGE        = 5    -- global.h DAMAGE (per shell, on a base)
M.BASE_FULL_SHELLS_TO_KILL = 17   -- ceil((90 - 9) / 5): the markup denominator

-- (1) ARMOUR-AWARE attack_base MARKUP.  ATTACK_BASE_EXTRA_COST is the flat
-- "attacking a base is an errand, not a stroll" surcharge.  It was flat, so a
-- base one shell from falling cost the same 80 as an untouched one.  It now
-- scales with the WORK LEFT: shells-still-needed / 17.  A stale reading decays
-- the discount linearly back to the full flat price over BASE_MARKUP_STALE
-- ticks -- old information must never make a healthy base look free, and the
-- decayed price can never drop below what the fresh reading would have paid.
M.BASE_MARKUP_STALE = 1200  -- = BASE_STEAL_OBS_STALE (was 500 = REFUEL_OBS_STALE).
                            -- 500 was half a base refuel cycle, and an idle
                            -- damaged base only re-reports once per
                            -- BASE_TICKS_BETWEEN_REFUEL (1000): base #8 at
                            -- 20260902_000405 bot2 t=22561 had a 560-tick-old
                            -- reading of 74 armour (13 of 17 shells left) and was
                            -- still priced as a never-seen full base. The reading
                            -- can only understate armour by +1 regen per 1000
                            -- ticks, so the wider window is the same safe
                            -- direction the steal already trusts.

-- FRIENDLY PILL COVER on an attack_base errand -- the mirror of the steal's
-- hostile coverage guard (refresh_base_steal: a base we can only reach down a
-- pillbox's line of fire is not a free base). Our OWN live pills within
-- PILL_FIRE_RANGE of the base tile, or of any tile on the straight approach we
-- would drive down, are doing part of this job already: they shell the base and
-- they shoot the tanks that come to defend it. Nothing priced that, so a base
-- sitting under one of our pillboxes cost exactly as much to attack as one deep
-- in their half. Each covering pill multiplies the ENGAGE half of the candidate
-- cost -- (armour-aware markup + threat), NOT the travel: a friendly pill does
-- not shorten the drive.
--
-- Unlike the hostile guard this is a RANGE test only, with no line-of-fire shot
-- sim. It is a discount, not a veto, so over-counting is the safe direction, and
-- the sim is the expensive half (5 shell sims per pill).
M.ATTACK_BASE_FRIENDLY_COVER_MULT  = 0.8  -- per covering friendly/allied live pill
M.ATTACK_BASE_FRIENDLY_COVER_FLOOR = 0.5  -- ...never below half, however many
M.ATTACK_BASE_LOG_CANDS            = 2    -- how many pool-7 candidates the
                                          -- P7_CANDS diagnostic line prints.
                                          -- finalize competes only the best;
                                          -- the runner-up is what tells you WHY
                                          -- the best won

-- (2) IMMINENT BASE STEAL.  A hostile base a shell or three from capturable,
-- sitting right next to us while we have the ammo to finish it, is not a normal
-- attack_base errand -- it is a free base.  Price it at a snap floor so it wins
-- the pool outright; the existing armour-0 IMMINENT capture then takes the
-- hand-off the moment the base falls.
--
-- NOTE ON THE ARMOUR NUMBER.  The brief for this change said "armour <= 3",
-- written in the allied /5 DISPLAY scale.  obs_armour is in ENGINE units, so the
-- same gate is 24 = MIN_ARMOUR_CAPTURE(9) + 3 x DAMAGE(5): three shells or fewer
-- from capturable.
M.BASE_STEAL_MAX_ARMOUR = 24   -- fresh-observed engine armour at/below which a
                               -- base is "one push" from falling (<= 3 shells)
M.BASE_STEAL_RANGE      = 6    -- tiles (manhattan) from the tank to the base
M.BASE_STEAL_COST       = 10   -- snap cost floor in the attack_base slot. Sits
                               -- above capture's IMMINENT_CAPTURE_FLOOR (5) and
                               -- below refuel's REFUEL_BASE_COST floor (25).
M.BASE_STEAL_OBS_STALE  = 1200 -- max age of the armour reading the steal will
                               -- act on. Wider than BASE_MARKUP_STALE on
                               -- purpose: an idle damaged base only reports once
                               -- per BASE_TICKS_BETWEEN_REFUEL (1000), so a 500
                               -- window would blank a real steal for half of
                               -- every cycle. Safe because the only thing that
                               -- can have happened since the reading is +1 regen
                               -- per 1000 ticks (or someone else shooting it,
                               -- which only helps).
-- Guards. The steal must not be a way to drive into a pillbox's field of fire
-- for a cheap base, and it must not fire from a boat or on a nearly-dead tank.
M.BASE_STEAL_MIN_ARMOUR = 8    -- = IMMINENT_CAPTURE_MIN_ARMOUR: same tank-armour
                               -- floor the imminent capture already respects
M.BASE_STEAL_COVER_RANGE = 9   -- tiles: pill-scan radius for the coverage guard
                               -- (= SEA_PILL_PILL_SAFE_RANGE, PILLBOX_RANGE 8
                               -- plus a tile of heat margin)

-- (3) PANEL.  The pool grid lists every base candidate it has, cheapest first;
-- this caps how many rows the base pools render so a 16-base map stays legible.
-- The ACTIVE goal's row is always kept even when it falls outside the cap.
M.BASE_PANEL_MAX = 6
-- =========================================================================
-- END armour-aware base pricing + IMMINENT BASE STEAL
-- =========================================================================

-- ── stuck-dest blame gate (2026-09-01) ────────────────────────────────────
M.STUCK_DEST_BLAME_TICKS = 100 -- the stuck escalation only BLACKLISTS its destination when that
                               -- dest has been pursued continuously this long; the escape itself
                               -- always runs (par1b bot3 t=8533: 3 ticks of pursuit stamped a
                               -- fresh base "unreachable" for 600t on another base's evidence)

-- =========================================================================
-- BUILDER POOL — LGM side-quests as a parallel track (2026-09-02)
-- =========================================================================
-- The incident: 20260901_160325_1_par2 bot2, t=21071-21661. bot2 places its
-- own blocker p15 at (125,114) for a take on pill #10; p15 dies to return fire
-- at 21471; for the next 190 ticks the LGM sits idle IN THE TANK with 13 trees
-- about 6 tiles from the corpse while the tank (correctly) finishes the take.
-- At 21661 a 1.6 bot drives over p15 and takes it. A 4-tree LGM repair would
-- have converted a dead pill back into a live friendly one -- undriveable,
-- race over -- and it never had a chance to run, because builder dispatch is
-- slaved to the TANK's goal. The tank goal was right; the architecture lost
-- the pill.
--
-- Fix: a second arbiter for a second resource. The goal pool decides what the
-- TANK does; the builder pool decides what the MAN does. Scored candidates
-- compete every tick, the winner (if any) gets the one LGM, and the whole
-- thing is gated by an eligibility stack whose every denial is named.
--
-- What is NOT a pool row: walls, placements and sea legs. Those are goal-owned
-- work with no existence outside their goal (builder.set_mode still owns them
-- and pre-empts the pool by MODE, exactly as before).
-- =========================================================================
M.BUILDER_POOL_ENABLED = true   -- master switch; false = 1.7-at-HARBOR behaviour

-- ── Leash ────────────────────────────────────────────────────────────────
-- How far from the TANK a side-quest target may sit. The man walks at roughly
-- BUILDER_POOL_GRASS_TICKS_PER_TILE per clear tile and dies to a single hit,
-- so the round trip is what is actually being bounded: 8 tiles out is ~128
-- ticks each way on grass, i.e. a ~5 s errand. Anything further stops being a
-- side-quest and becomes a reason to move the tank (that is the out-of-leash
-- repair_pill goal, below).
M.BUILDER_POOL_LEASH = 8
M.BUILDER_POOL_GRASS_TICKS_PER_TILE = 16   -- = REPAIR_DEAD_GRASS_TICKS_PER_TILE
                                           -- (MAP_MANSPEED_TGRASS); only used for
                                           -- the fallback ETA when the walk sim
                                           -- declines to answer
M.BUILDER_POOL_LGM_MAX_TICKS   = 2000      -- walk-sim budget (matches every other caller)
M.BUILDER_POOL_LGM_STUCK_TICKS = 150

-- ── Value: the front-distance clock ──────────────────────────────────────
-- A dead pill AT the contact line is ticking -- the enemy is right there and
-- will drive over it. One deep in our own rear can wait all game. The
-- influence map already knows where the line is (pill_portfolio.on_front_line
-- mirrors brainPathfinderFindFrontLine), so front distance is free: ring-scan
-- outward from the target until a front tile is found.
--
--   value = BASE[type] + FRONT_URGENCY * max(0, (FRONT_MAX - front_dist)/FRONT_MAX)
--
-- front_dist is in tiles, capped at FRONT_MAX (beyond which the clock has
-- stopped and the job is worth its base alone).
M.BUILDER_POOL_FRONT_MAX_TILES = 12   -- ring-scan cap; also the "clock stopped" distance
M.BUILDER_POOL_FRONT_URGENCY   = 120  -- value added to a job sitting ON the front line
M.BUILDER_POOL_VALUE_REBUILD   = 200  -- a 0-HP friendly pill: 4 trees turn a corpse the
                                      -- enemy can DRIVE OVER into a live pill they cannot.
                                      -- The single highest-leverage LGM errand there is.
M.BUILDER_POOL_VALUE_TOPUP     = 60   -- a damaged-but-alive friendly pill: real value, no
                                      -- race against a driver -- nobody can steal it
M.BUILDER_POOL_VALUE_FARM      = 15   -- opportunistic wood, at a full woodpile. No clock
                                      -- at all -- a forest is not going anywhere and
                                      -- nobody can steal it
-- ...but wood being the thing we are SHORT of is itself a reason to spend the
-- man, and at 15 flat the farm row could never clear MIN_SCORE even for a
-- forest one tile away (a 1-tile round trip already costs 26). So the row
-- earns its keep only when the woodpile is genuinely low:
--     value_farm = VALUE_FARM + FARM_URGENCY x max(0, FARM_LOW_TREES - trees)
-- The ceiling is deliberate arithmetic, not a coincidence:
--     15 + 12 x 12 = 159  <  VALUE_REBUILD (200)
-- so DEAD-PILL REBUILD OUTRANKS FARM ALWAYS, at any tree count, exactly as the
-- plan requires -- and it falls out of the numbers rather than a special case.
-- Above FARM_LOW_TREES the row still exists (always-show) and still loses,
-- leaving ordinary top-ups to decide()'s Priority 4 on-path farm, which is
-- strictly better at that job: it picks forest the TANK is going to drive past.
M.BUILDER_POOL_FARM_LOW_TREES  = 12
M.BUILDER_POOL_FARM_URGENCY    = 12
M.BUILDER_POOL_TOPUP_PER_HP    = 6    -- + this per point of missing armour on a top-up, so
                                      -- a 4/15 pill outbids a 14/15 one
M.BUILDER_POOL_TOPUP_MIN_MISSING = 4  -- don't walk out for less than one tree's worth
                                      -- (PILL_REPAIR_AMOUNT) of damage

-- ── Trip and danger: what the job COSTS ──────────────────────────────────
--   cost = TRIP_W * round_trip_ticks + DANGER_W * threat_at_target
--   score = value - cost           (higher is better; ties break deterministically)
-- TRIP_W is set so that the 8-tile leash edge (~256 ticks round trip on grass)
-- costs 128 -- roughly the whole front-urgency bonus. A job at the leash edge
-- therefore needs to be genuinely urgent to beat a nearer one.
M.BUILDER_POOL_TRIP_W   = 0.5
M.BUILDER_POOL_DANGER_W = 1.5   -- threat.at() at the target tile
M.BUILDER_POOL_MIN_SCORE = 20   -- below this the errand is not worth the man's time at all
                                -- (keeps the farm row from firing on every quiet tick)

-- ── Eligibility ──────────────────────────────────────────────────────────
-- Under-fire: NOT a single-tick test. perc.under_fire is "the danger field at
-- our tile is non-zero", which is true for most of a firefight's quiet moments
-- and false in the gap between two shells that are both aimed at us. The man's
-- risky moments are the two ENDS of the trip (departure and return splash), so
-- what matters is whether anything has actually connected or is inbound
-- RECENTLY. danger.tank_fire_age() stamps a clock on either evidence:
--   * armour dropped since last tick (state.took_damage_this_tick), or
--   * a hostile shell's closest approach lands inside SWERVE_HIT_RADIUS_WU
--     (danger.shells_incoming_near's own hit test).
M.BUILDER_POOL_UNDER_FIRE_TICKS = 100  -- 2 s: no hit and no inbound shell for this long
                                       -- before the man may leave. Deliberately longer
                                       -- than REPAIR_QUIET_TICKS (75, which is about a
                                       -- PILL going quiet) -- this one is about the TANK,
                                       -- and the tank is where the man starts and ends
M.BUILDER_POOL_RESERVE_MARGIN   = 40   -- ticks of slack the round trip must leave inside
                                       -- b.reserve_eta. A wall shield that finds the man
                                       -- still walking is the failure this prevents
M.BUILDER_POOL_TREE_RESERVE_EXTRA = 0  -- extra wood held back on top of the goal's own
                                       -- declared need (road_tree_reserve / _trees_for_walls
                                       -- / the sea plan). 0 = trust those numbers
M.BUILDER_POOL_PATH_DANGER = M.LGM_DANGER_MED   -- lgm_path_safe_enhanced threshold for the
                                                -- trip: same tier the repair dispatch uses
M.BUILDER_POOL_TREES_REBUILD = 4  -- = REPAIR_DEAD_MIN_TREES / LGM_COST_PILLREPAIR x 4
M.BUILDER_POOL_TREES_TOPUP   = 1  -- one tree = PILL_REPAIR_AMOUNT(4) armour
M.BUILDER_POOL_TREES_FARM    = 0  -- a farm trip SPENDS nothing, it brings wood home

-- ── Substate classes ─────────────────────────────────────────────────────
-- ONE table, keyed by substate name (they are unique enough across goals that
-- a per-kind nesting would only duplicate rows). Two classes:
--
--   "travel"  the tank is driving, aiming at nothing, and nothing is aimed at
--             it as a consequence of what it is doing. The man may go, gated
--             by reserve_eta and the under-fire clock.
--   "fire"    the tank is in a shooting exchange. Shells detonating on or near
--             it splash the man at departure AND at return, which are exactly
--             the two ends of the errand. DENY, unconditionally.
--
-- Anything not listed DENIES (mode_owned). That default is deliberate: a new
-- substate should have to be classified on purpose, not inherit permission.
-- The LGM-work substates (ws_prebuild / ws_rebuild / build_walls / dispatch)
-- are absent because builder.set_mode has already turned them into a goal-tied
-- MODE by the time the pool is asked, and mode_owned names them better.
M.BUILDER_POOL_SUBSTATE_CLASS = {
  -- attack_pill, travel half
  plan_position   = "travel",
  approach        = "travel",
  gather_trees    = "travel",
  blitz_wait      = "travel",
  ws_prewait      = "travel",
  ws_advance      = "travel",
  ws_retreat      = "travel",
  detree          = "travel",
  navigate        = "travel",
  select_pill     = "travel",
  loiter          = "travel",
  curve_away      = "travel",
  post_engage     = "travel",
  disengage       = "travel",
  reposition      = "travel",
  -- capture_pill / sea, travel half
  collect_target  = "travel",
  collect         = "travel",
  pickup          = "travel",
  seek_trees      = "travel",
  entrance_plan   = "travel",
  wait_place      = "travel",
  -- fire exchange: every substate where a shell is in flight either way
  shoot_pill            = "fire",
  engage                = "fire",
  ws_engage             = "fire",
  charge                = "fire",
  swerve                = "fire",
  kill_hardline         = "fire",
  aim                   = "fire",
  in_range_aim          = "fire",
  in_range_aim_pre      = "fire",
  in_range_aim_finetune = "fire",
  in_range_position     = "fire",
  finish                = "fire",
  rush                  = "fire",
  close                 = "fire",
  heat_pill_aim         = "fire",
  heat_pill_position    = "fire",
  heat_pill_shoot       = "fire",
  heat_done             = "fire",
  reposition_shoot      = "fire",
}
-- Goal kinds that map to builder mode "suppressed" and carry NO substate, but
-- whose suppression is about keeping the man aboard for THIS goal rather than
-- about being in a fight. A defender parked and watching is the plan's
-- "defend-watch" travel class. A defender with goal.repair set is excluded --
-- that one is a pool FEEDER (it seeds the job directly), not a side-quest.
M.BUILDER_POOL_TRAVEL_GOALS = { defend_pill = true }
-- Builder modes that are "idle-ish": no goal has spoken for the man, so the
-- pool may spend him freely. Everything else that is not "suppressed" is a
-- goal-tied working mode and denies with mode_owned.
M.BUILDER_POOL_IDLE_MODES = {
  opportunistic  = true,
  infrastructure = true,
  repair_nearby  = true,
}

-- ── Claims (multi-bot arbitration) ───────────────────────────────────────
-- Five allies must not all repair the same pill. The claim rides the existing
-- /info extra channel as `bpj=TTXXYYCCCCEEEE` (type, tile, claim tick low 16
-- bits, ETA) alongside `lgmd`. Arbitration is deterministic and needs no
-- wall clock: earlier claim tick wins; a same-tick race breaks to the LOWER
-- player number (the same rule the blitz commander and the standoff-spot
-- picker use). A live claim also removes the target from allies' DISCOVERY --
-- the claim covers the outcome, not just the trip.
M.BUILDER_POOL_CLAIM_MAX_AGE = M.SQUAD_ALLY_MAX_AGE or 1750  -- claim expiry (heartbeat gap)
M.BUILDER_POOL_CLAIM_TYPES = { rebuild = 1, topup = 2, farm = 3 }

-- ── Job lifecycle ────────────────────────────────────────────────────────
M.BUILDER_POOL_JOB_MAX_TICKS = 900   -- the man never came back (18 s): drop the job record
                                     -- so a lost LGM cannot hold the claim forever
M.BUILDER_POOL_ABORT_GRACE   = 30    -- ticks after dispatch before an abort may be declared
                                     -- (the engine takes a few ticks to move him off tile)

-- ── Panel ────────────────────────────────────────────────────────────────
-- Section 15 in the pool_grid JSON. NOT a new pool: the 1..10 grid numbering
-- and the 11..14 strips are untouched, so old recordings still load.
M.BUILDER_POOL_PANEL_IDX  = 15
M.BUILDER_POOL_PANEL_ROWS = 8    -- cap on side-quest rows rendered (always-show rule
                                 -- keeps rejects visible; this only bounds a huge map)

-- =========================================================================
-- Lua garbage collector
-- =========================================================================
-- The brain allocates a lot: ~94 KB per think per bot (brain_alloc_stats via
-- -brain-profile-log), which at 50 thinks/s across four bots is ~19 MB/s, so
-- how the collector is configured is a fair question to ask.
--
-- Brain.open used to ask for Lua 5.4's generational collector, but production
-- runs on LuaJIT, where the 5.4->5.1 compat shim in luabrainshandler.c swallows
-- collectgarbage("generational") and the brain silently got LuaJIT's stock
-- incremental settings.  These are the knobs LuaJIT actually has:
--
--   GC_PAUSE   -- % of the post-collection heap the heap may reach before the
--                 next cycle starts. LuaJIT's default is 200 (collect when the
--                 heap doubles).
--   GC_STEPMUL -- how much work each incremental step does relative to
--                 allocation. LuaJIT's default is 200. Higher = the same total
--                 work in fewer, larger steps.
--
-- MEASURED on a 12000-tick 4-bot DH-Oil Rig prod run (per-think ms over all
-- bots; full table in tests/cpuopt_results.md). The sweep is recorded here so
-- nobody has to repeat it -- in the range these knobs cover, they do not move
-- this workload:
--
--   pause/stepmul   mean    p50    p90    p99     max
--   200/200 (dflt)  0.80   0.74   1.26   2.12   10.45
--   150/200         0.82   0.76   1.30   2.20    9.33
--   300/200         0.81   0.75   1.29   2.14   12.38
--   150/400         0.79   0.73   1.27   2.25    7.55
--   200/400         0.77   0.71   1.25   2.11   10.27
--   250/400         0.78   0.72   1.22   2.15    9.60
--   200/600         0.76   0.70   1.22   2.20    8.21
--   200/200 again   0.76   0.70   1.22   2.12    8.40
--   200/400 again   0.75   0.69   1.21   2.12    9.78
--   200/800 again   0.77   0.70   1.22   2.31    7.86
--
-- The two repeats differ from their first sample by ~0.04 ms on p50, which is
-- the size of the entire spread across the sweep: the collector is not what
-- the tail is waiting on, and no setting here buys anything real. 200/400 is
-- kept because it was a hair ahead of the default on p50/p90 in both samples
-- and behind in neither. Setting them at all is still worth it -- the call
-- these replaced did nothing on LuaJIT, so the collector was configured by
-- accident rather than on purpose.
M.GC_PAUSE   = 200
M.GC_STEPMUL = 400

-- ══════════════════════════════════════════════════════════════════════════
-- PRESETS — named bundles of constant overrides, applied per bot
-- ══════════════════════════════════════════════════════════════════════════
-- A bot given the BRAIN_INIT_ARG token "preset=NAME" has every entry of
-- M.PRESETS[NAME] written into ITS OWN copy of this table before any other
-- module requires it (init.lua, right after `require("constants")`), so
-- module-level captures like squad.lua's BLITZ_MAX see the preset value.
-- Explicit "cfg=NAME=VALUE" tokens are applied AFTER the preset whatever
-- order they appear in, so a cfg= always wins.
--
-- The point is A/B play: two sides of the same game, same brain, same build,
-- one running today's constants and one running the older behaviour --
--   -bot-init "0-1=<brain>[preset=keel],2-3=<brain>[]"
-- (see tests/ab_bench.py).
--
-- ── RULE (2026-09-05) ─────────────────────────────────────────────────────
-- EVERY behaviour change from now on adds its PRE-CHANGE value to `keel`, in
-- the same commit that changes the constant, with a dated one-line comment.
-- That is the whole contract: `preset=keel` must always reproduce the KEEL
-- baseline (tag b29c6225 in winbolo2) behaviour, so a bench never has to
-- rebuild an old brain to have something to compare against. A change that
-- forgets its entry here silently makes the baseline drift, and every bench
-- result taken afterwards is measuring less than it says it is.
--
-- Only PLAIN VALUES belong here (numbers, booleans, strings): the override
-- refuses to write a table or a function, and refuses a value whose type does
-- not match the constant it replaces.
M.PRESETS = {
  keel = {
    -- 2026-09-05: raised to 3, i.e. the default blitz party went 2..2 -> 2..4.
    SQUAD_MAX_SIZE                = 1,
    -- 2026-09-05: a take with a hostile tank within BLITZ_CONTESTED_RANGE now
    -- makes every blitzer a temporary suicider. KEEL only ever designated the
    -- BLITZ_MIN_SUICIDERS quota (0 by default, i.e. nobody).
    BLITZ_CONTESTED_ALL_SUICIDERS = false,
  },
}

return M
