# NewAutopilot Roadmap — Features from aIndy Analysis

Based on reverse-engineering of aIndy 3.1 (Paul Joswig, 1997) and comparison
with current NewAutopilot behavior.

---

## Implementation Status

### Implemented ✓

| # | Feature | Status | Notes |
|---|---------|--------|-------|
| 1 | Attack Position Selection | ✓ Already better than aIndy | score_standoff() in attack.lua has 10+ scoring factors. Added friendly pill barrier bonus + visual overlays |
| 2 | Lead-Time Aiming | ✓ | Real velocity tracking via frame-delta in perception.lua. Replaced animation-frame hack. Debug overlay: red=actual, orange=predicted |
| 3 | Emergency Pill Drop | ✓ | Drop pill behind tank when armour <= 5, with incoming shell safety check and enemy presence requirement |
| 4 | Stuck Detection Improvements | ✓ | BPC/refuel/rescue exempt from stuck counter. Visual: HUD counter + blocked dest rects |
| 5 | LGM Pacing Improvements | ✓ | Water-ahead block, wider refuel farming, LGM ETA, smart pickup, enhanced path safety, enemy LGM tracking |
| 6 | Base Killer Mode | ✓ | Auto-activate when team advantage >= 2. Discounts base attacks, penalizes pill attacks |
| 7 | Pill Placement Goal | ✓ Already existed | place_pill_strategic with front-line targeting, offensive mode |
| 8 | Brain-to-Brain Coordination | ✓ | comms.lua: /nap protocol for pill/base claim broadcasting with 200-tick expiry |
| 9 | Pill Repositioning ("Pissing") | ✓ | Evaluates friendly pill positioning (orphan distance, crossfire, terrain). Creates capture_pill goal for replanting |

### Other Ideas Status

| Feature | Status | Notes |
|---------|--------|-------|
| Pill firing rate monitoring | ✓ Already covered | Anger model in world.lua tracks pill threat level |
| Refuel base corner sitting | Skipped | Base shield wall system is a better solution |
| Two-phase refuel commit | ✓ Already covered | Override 2 + LGM ETA prevents oscillation |
| Detect unreachable bases | ✓ Already covered | Blocked destination system (600-tick blocks) |
| Shoot fleeing to damage pills | ✓ | Opportunistic pill/tank shooting during any navigation |
| Dead enemy men tracking | ✓ | OBJECT_PARACHUTE tracking + attack discount |
| Friendly pill as barrier | ✓ | Standoff scoring bonus (FPILL_BARRIER_BONUS=80) |
| Allied LGM protection | ✓ | Track OBJECT_BUILDMAN positions, visual overlay |
| Deep sea memory | ✓ | Remember seen deepsea tiles, detect bait pills |

---

## Visual Overlays Added

| Overlay | Color | What it shows |
|---------|-------|---------------|
| Yellow square | Yellow | Raw A* next step (pf.next_mx/my) |
| Magenta circle + line | Magenta | Path lookahead target (where steering actually aims) |
| White circle | White | Navigation destination |
| Red circle | Red | Enemy tank actual position (combat) |
| Orange circle + line | Orange | Lead-time predicted intercept point |
| Yellow line | Yellow | Opportunistic shot at enemy pill/tank |
| Green circle (map) | Green | LGM position when out on mission |
| Green circle (standoff) | Green | Chosen attack standoff position |
| Color-coded rects | Green→Red gradient | Attack position candidates (scored) |
| Green line | Green | Pill → chosen standoff line |
| Cyan circle | Cyan | Friendly pill acting as shield |
| Orange outline | Orange | Orphaned friendly pill (reposition candidate) |
| Purple rect + "BAIT?" | Purple | Dead pill on known deep sea |
| Red rect | Red | Blocked destinations (stuck avoidance) |
| Green "ALLY LGM" | Green | Allied builder positions |
| Red "EMERGENCY PILL DROP!" | Red | Emergency pill drop location |

### HUD Text Indicators

| Position | Text | Color | Meaning |
|----------|------|-------|---------|
| Top-right | Goal: ... | Green | Current goal, target, substate |
| Top-right | PF: ... | Yellow | Pathfinder status and age |
| Top-right | -- candidates -- | Gray | Goal cost competition pool |
| Top-left | Enemy LGM dead | Red | Enemy can't repair pills |
| Top-left | BASE KILLER | Yellow | Team advantage mode active |
| Top-left | Stuck: N/150 | Yellow→Red | Stuck counter building |
| Top-left | EMERGENCY PILL DROP! | Red | About to drop pill to save it |
| Bottom-left | Shells/Mines/Armour/Trees | Various | Tank inventory |
| Bottom-left | Boat YES/no | Blue | Boat status |
| Bottom-left | LGM ETA: N ticks | Green | Builder return estimate |
| Bottom-left | LGM nearby (Nt) | Cyan | LGM close and arriving |
| Bottom-left | LGM STRANDED | Red | LGM can't reach tank |
| Bottom-left | LGM blocked: water ahead | Orange | LGM dispatch suppressed |

---

## ML-Tunable Parameters

All parameters are in `constants.lua` unless noted.

| Parameter | Default | Purpose |
|-----------|---------|---------|
| `road_build_danger_max` | 10 | Don't assume road-build above this danger (brain_pathfinder.c) |
| `PILL_DANGER_BASE` | 8 | Base danger from calm pill at point-blank |
| `PILL_DANGER_ANGER` | 200 | Additional danger when fully angry |
| `PILL_RANGE_MAP` | 9 | Pill firing range in map squares |
| `PHASE_WEIGHTS.*` | varies | Goal priority per game phase |
| `ATTACK_PILL_STANDOFF` | 7 | Distance to stand from pill when attacking |
| `ASTAR_BUDGET` | 1500 | A* node expansions per tick |
| `FARM_REFUEL_RADIUS` | 4 | Wider farm radius when stationary at base |
| `LGM_DEPLOY_DIST_REFUEL` | 5 | Max deploy distance at base |
| `LGM_ETA_DEPART_BUFFER` | 10 | Leave base this many ticks before LGM returns |
| `LGM_NEARBY_TILES` | 3 | Consider LGM "nearby" within this range |
| `LGM_NEARBY_ARRIVAL_TICKS` | 60 | Skip rescue if LGM arrives within this |
| `ENEMY_LGM_RETURN_TICKS` | 3000 | Estimated enemy LGM respawn time |
| `ENEMY_LGM_DEAD_ATTACK_DISCOUNT` | 0.5 | Attack pill cost multiplier when enemy LGM dead |
| `EMERGENCY_DROP_ARMOUR` | 5 | Drop pill when armour at or below |
| `EMERGENCY_DROP_SHELL_SAFE_DIST` | 3 | Don't drop if shell within this range |
| `BASE_KILLER_TEAM_ADVANTAGE` | 2 | Activate base killer when advantage >= this |
| `BASE_KILLER_ATTACK_DISCOUNT` | 0.3 | Attack_base cost multiplier in BK mode |
| `BASE_KILLER_PILL_PENALTY` | 2.0 | Attack_pill cost multiplier in BK mode |
| `FPILL_BARRIER_BONUS` | 80 | Standoff cost reduction behind friendly pill |
| `PILL_REPOSITION_ORPHAN_DIST` | 15 | Tiles from base to consider pill "orphaned" |
| `PILL_REPOSITION_THRESHOLD` | 50 | Minimum badness to trigger repositioning |
| `TANK_COMBAT_SHELL_SPEED` | 32 | Shell speed for lead-time prediction |
