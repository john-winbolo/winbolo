/*********************************************************
 * brains/GoalHunter/braintest_panels/pool_grid.cpp
 *
 * Bot-specific BrainTest panel renderer for GoalHunter's
 * goal-pool data model. Compiled into BrainTest at build
 * time via brains/<bot>/braintest_panels/(*.cpp) glob.
 *
 * Visual layout matches the original optimize-branch
 * braintest_poolwindow.cpp pixel-for-pixel: 2x5 grid for
 * pools 1..10 + def_build / wait_for_lgm strips, per-pool
 * color coding, two-line rows (stats line + dim formula
 * line), winner / active-goal / flash backgrounds, click +
 * Ctrl+C copy, double-click detail popup.
 *
 * Input: JSON from goals.get_pool_breakdown_json (see brain
 * for the schema). Renderer is registered for panel type
 * "GoalHunter:pool_grid" via static initializer below.
 *********************************************************/

#include <SDL3/SDL.h>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <unordered_map>
#include "imgui.h"
#include "cJSON.h"
#include "braintest_panel_types.h"

namespace {

/* ── Term documentation (verbatim from optimize-branch poolwindow) ──
 * Looked up by the word immediately before "{value}" in a row's
 * formula — populates the "Meaning" column of the detail popup. */
struct TermDoc { const char *term; const char *desc; };
static const TermDoc kTermDocs[] = {
    {"A*",      "Path cost — Dijkstra/A* weighted grid distance to the target tile"},
    {"A* to standoff", "Path cost to the FIRING STANDOFF tile (R off the enemy), NOT the enemy's tile — so it's cheaper than a test-click on the enemy square."},
    {"far_preempt", "Pill-take guard: while on an attack_pill goal, an exponentially-growing euclidean-distance penalty for a target (enemy tank or LGM) past shoot range, so a far one can't preempt the take (only a close, threatening one can)."},
    {"raw",     "Uncapped A* cost (shown when it was capped for distant/water targets)"},
    {"base",    "Fixed base constant added to every candidate of this goal type"},
    /* Builder-pool (LGM side-quest) rows: repair/top-up/farm jobs scored by
     * builder_pool.lua score_row; chips are emitted by M.score_terms. */
    {"bp_score",  "Builder-pool job score (HIGHER wins). Linear repair rows: hp_w x missing - trip_w x trip. Other rows: bp_base + topup_hp/urg + front - tripcost - dangercost. The job fires only if bp_score >= BUILDER_POOL_MIN_SCORE(20) and no gate rejects the row."},
    {"hp_w",      "BUILDER_POOL_REPAIR_HP_W — points per missing hp on a linear repair row (damage is the main deciding factor)"},
    {"missing",   "PILLS_MAX_HEALTH(15) - pill hp: how much armour the man would put back"},
    {"value",     "The job's value before costs: hp_w x missing on linear repair rows; bp_base + topup_hp/urg + front otherwise"},
    {"trip",      "LGM round trip in brain ticks = out + build + back. With BUILDER_POOL_RETURN_PREDICT off, back == out (the walk home mirrors the walk out) and this is the old 2 x walk_sim + LGM_BUILD_TIME."},
    {"trip_w",    "Points charged per round-trip tick (BUILDER_POOL_REPAIR_TRIP_W on linear repair rows, BUILDER_POOL_TRIP_W otherwise)"},
    {"tripcost",  "trip_w x trip — the walking-time cost subtracted from value"},
    {"bp_base",   "BUILDER_POOL_VALUE_<REBUILD|TOPUP|FARM> — fixed value of the job type (non-linear rows only)"},
    {"topup_hp",  "BUILDER_POOL_TOPUP_PER_HP x missing — extra value per missing hp on a non-linear top-up row (0 on rebuild rows)"},
    {"urg",       "Farm urgency: BUILDER_POOL_FARM_URGENCY x max(0, FARM_LOW_TREES - trees carried) — a low tree stock makes farming worth more"},
    {"front",     "Front clock: BUILDER_POOL_FRONT_URGENCY x (FRONT_MAX - front_dist)/FRONT_MAX — repairs near the front line are worth more (0 on farm rows and on linear repair rows)"},
    {"bp_danger", "threat.at(target tile) — hostile pills/tanks that have the job tile in range. Charged on non-linear rows only; printed but NOT charged on linear repair rows"},
    {"danger_w",  "BUILDER_POOL_DANGER_W — points charged per danger unit (non-linear rows)"},
    {"dangercost","danger_w x bp_danger — the danger cost subtracted from value (non-linear rows)"},
    {"danger",  "Danger at destination × weight — hostile pills/tanks in firing range"},
    {"stale",   "Staleness penalty — target not seen recently; info may be wrong"},
    {"contest", "Contested penalty (ADDITIVE) — MOVING enemy tanks (speed>0) near this refuel base, since sitting still to resupply next to one is how you die. Each qualifying tank within CONTESTED_BASE_RANGE(15 tiles, Manhattan) adds CONTESTED_BASE_PENALTY(120) x (1 - dist/RANGE): the full 120 for a tank parked ON the base, fading linearly to 0 at the range edge — so a tank 14 tiles out is nearly free and one at 7.5 costs 60. Per-tank terms simply SUM (two at 7.5 = 120 total); there is deliberately NO outnumbering multiplier, which priced refuelling out of reach. A tank that an ALLY is already broadcasting an attack_tank goal against is SKIPPED entirely — that threat is someone else's job and shouldn't also scare us off the pumps. The REFUEL_CAND log prints CONTESTED{n=<counted> handled=<skipped> pen=<sum>}."},
    {"hyst",    "Switch penalty (ADDITIVE) — GOAL_SWITCH_PENALTY(30) or GOAL_TARGET_SWITCH_PENALTY(15) + commitment(ticks*0.5, cap 75); negative in Pool 1 means already on this target. Flat cost units, NOT a percent."},
    {"x0.7",    "Active-goal switch bar (MULTIPLICATIVE hysteresis, GOAL_SWITCH_RATIO=0.7) — shown on the '>' current-goal row as its cost x0.7. A challenger only takes over if its (post-'hyst') cost falls BELOW this bar, i.e. it must be >=30% cheaper than the current goal. Separate from and stacked on top of the additive 'hyst' penalty."},
    {"deplete", "Depletion penalty — base observed to have low shells or armour stock"},
    {"ur",      "Refuel urgency MULTIPLIER (0.37..1.0) — min((armour/ARMOUR_LOW)^2, (shells/SHELLS_LOW)^2) clamped to REFUEL_URGENCY_MIN(0.37). Scales the refuel cost DOWN as armour/shells fall (refuel gets cheaper the more hurt you are); 1.0 at/above the LOW thresholds."},
    {"def",     "Refuel deficit bonus (SUBTRACTED, flat) — max((ARMOUR_LOW-armour)/ARMOUR_LOW, (SHELLS_LOW-shells)/SHELLS_LOW) x REFUEL_DEFICIT_BONUS(25). Grows as supplies deplete; 0 at/above the LOW thresholds."},
    {"fill",    "Refuel fill MULTIPLIER (1.0..REFUEL_FULL_COST_MULT=3.0) — applies only when BOTH armour>ARMOUR_LOW and shells>SHELLS_LOW; scales cost UP as the tank tops off toward its targets, until refuel is skipped entirely when full."},
    {"safe",    "Safe-refuel MULTIPLIER (REFUEL_NO_DANGER_DISCOUNT) — applied when the base sits in zero-danger territory (no pill/tank threat) so a safe base wins ties over an exposed one."},
    {"lgm_wait_floor", "LGM-wait cost floor — when a returning LGM is due at this base, refuel cost is capped at this floor so the bot holds position to collect it."},
    {"urgency", "Urgency discount (negative) — more pill damage = higher priority"},
    {"diff",    "Difficulty score — terrain around pill makes the attack harder"},
    {"spot",    "Best attack spot — path cost to the nearest good firing position"},
    {"anger",   "Anger wait cost — pill is riled up; penalty reflects waiting for it to calm"},
    {"xfire",   "Crossfire penalty from other pills that can fire on your engage spot. attack_tank/kill_lgm: escalating per NEW pill whose range covers the engage spot but NOT your current tile (40, 90, 150, ...) — pills already covering you don't count (A* already prices the travel). attack_pill: per-pill proximity at the standoff."},
    {"intcpt",  "Intercept risk — enemy tank may reach this pill before you do"},
    {"spike",   "Spiking-pill discount (MULTIPLIER on the combat block, SPIKE_PILL_DISCOUNT) — this pill sits within PILL_FIRE_RANGE of a friendly base, denying us refuel there until cleared. Scaled by decisiveness (1/cover of its least-contested base) AND breadth (each EXTRA denied base strengthens the pull by SPIKE_BASES_BONUS; floored at SPIKE_DISCOUNT_FLOOR). Small on purpose: the pool tilts toward the spike without beating attack_tank/kill_lgm/refuel cross-pool."},
    {"suicider", "Pillbox-suicider surcharge (MULTIPLIER on this candidate's WHOLE cost, applied at selection alongside the phase weight and the influence scale). This bot holds the pill_suicider role, so it is unwilling to do anything but hit pills and keep itself fuelled: attack_pill x1 (EXEMPT — the one job), refuel_at_base / flee_to_base x1 (EXEMPT — the whole refuel group), capture_pill / place_pill_strategic / def_build / wait_for_lgm x1 (EXEMPT — scooping and fielding what it kills is the job, panic drops are survival, and a tank must never abandon its own LGM to the x3), defend_pill x PILL_SUICIDER_DEFEND_MULT = 6.0, and EVERYTHING else (capture_base, repair_pill, attack_base, attack_tank, kill_lgm, reposition, rescue_lgm, explore) x PILL_SUICIDER_OTHER_MULT = 3.0. The role is assigned on a map listed in C.PILL_SUICIDER_MAPS (matched against info.gameinfo.mapname — the map file's basename with no directory and no '.map', e.g. 'Survival'), where the harasser slate is repurposed: the same bots HARASSER_FRAC would have made harassers become suiciders instead and carry NONE of the harasser biases. The role also never enters a DEFENSIVE swerve (it charges straight in and keeps firing; the pill-dead 'kill' swerve still runs) and never builds shield walls (forced non-PPT, shield.scan skipped)."},
    {"spike_pen", "Spike cross-penalty (ADDITIVE, SPIKE_OTHER_PENALTY) — a spiking pill exists elsewhere, so every NON-spiking pill pays this flat surcharge (applied once, regardless of spike count) to steer the pool toward clearing the spike first."},
    {"hp",      "Health multiplier — lower pill HP = lower cost (easier kill)"},
    {"wound",   "Wounded discount — heavily damaged pill is a very high-value target"},
    {"self_dr", "Self-danger reduction (subtracted) — discount on the spot-path cost equal to the sum of the target pill's own contribution along that path. Applied regardless of pill HP so a fresh-but-targeted pill stops bullying its own approach corridor."},
    {"ammo",    "Shells-budget penalty — INF if shells < pill_hp (can't finish), otherwise 5 per shell that the take would leave us at below SHELLS_LOW (assuming exactly pill.health shots). Replaces the old has_shells gate that cleared the entire pool when shells dipped below SHELLS_LOW mid-take."},
    {"threat",  "Threat coverage × weight — hostile pill fire overlaps this base"},
    {"carry",   "Carry discount (negative) — you are already holding a pill to place"},
    {"mult",    "Constant cost multiplier for this pool (e.g. STRATEGIC_PLACE_COST_MULT) — FIXED per pool, does NOT vary per candidate. Scales the bracketed sum. Per-candidate multiplicative variation comes from other terms (place: bal, lastpill)."},
    // place_pill_strategic (pool 8): score{} terms (placement QUALITY, higher=better spot, a sum) ...
    {"score",   "Total placement quality = SUM of all the terms that follow; HIGHER is a better spot. Drives which spot is chosen — separate from the goal cost{} below."},
    {"prx",     "Base proximity — soft bonus for being near a friendly base: max(0, STRATEGIC_PLACE_MAX_BASE_DIST - dist) x BASE_WEIGHT. Far spots just miss the bonus, they're not rejected."},
    {"bdef",    "Base-defense need — bonus when the spot has <2 friendly pills within DEFENSE_RADIUS: UNDERDEFENDED_BONUS x (2 - count)."},
    {"inf",     "Front-line influence — penalty if beyond the front (influence<0 -> -BEYOND_FRONT_PENALTY); else a bonus for sitting near the front: max(0, FRONT_PROX_CAP - influence) x FRONT_PROX_WEIGHT."},
    {"spc",     "Pill spacing — penalty if too close to a friendly pill (<PILL_SPACING), small bonus when 2-4 tiles apart."},
    {"los",     "Line-of-sight coverage — open sightlines from the spot (los_coverage x LOS_WEIGHT); a pill that can see more is more useful."},
    {"thr",     "Threat at spot — hostile pill/tank fire coverage: -threat x THREAT_WEIGHT (negative). Avoids dropping a pill under fire."},
    {"dst",     "Distance from tank — -mdist(tank,spot) x 0.5; mild preference for closer spots (less travel)."},
    {"spk",     "Offensive spike — bonus when within 2 tiles of an offensive-push (spike) base."},
    {"ep",      "Enemy-pill proximity — penalty if too close to a hostile pill, sweet-spot bonus at mid range, smaller bonus farther out."},
    {"wz",      "Pill war-zone reinforcement — bonus near a contested-pill war zone, fading with distance (<=5 tiles)."},
    {"port",    "Portfolio deficit bias — deficit x PORTFOLIO_WEIGHT; pushes placement toward the under-target category (util/front/aggro/back)."},
    {"cov",     "Protective coverage — friendly pills + bases this spot covers within fire range; the key 'good back protector' signal."},
    {"grd",     "Base-guardian — big bonus per currently-unguarded friendly base this spot would cover (top placement priority)."},
    {"ctr",     "Strategic-center bias — nudge toward the chosen search center (war zone / base-vs-threat / contested pill)."},
    // ... and the winner's cost{} = (path + base + carry_pen - carry) x mult x lastpill x bal x surplus x multi + tankpen (goal COST, lower competes harder)
    {"cost",    "Final goal cost for this candidate = (path + base + carry_pen - carry) x mult x lastpill x bal x surplus x multi + tankpen, floored at 1. LOWER competes harder against other goals."},
    {"path",    "A* travel cost from the tank to the placement spot."},
    {"carry_pen", "Carry-value penalty (added) — raises cost when carrying the pill is currently more useful than placing it: early game, a dead pill to capture nearby, or an active attack opportunity. Zeroed when placement is urgent."},
    {"lastpill", "Last-pill hold multiplier — x1.5 when holding your ONLY pill AND still short on utility pills (don't dump your last blocker); x1.0 otherwise."},
    {"bal",     "Imbalance discount multiplier (<=1) — cheaper to place when a pill category is in deficit; 1.0 when balanced, capped so cost never goes free."},
    {"surplus", "TEAM util-surplus discount multiplier (<=1) — each util pill the team holds over its utility reserve knocks off STRATEGIC_PLACE_UTIL_SURPLUS_DISCOUNT (capped at _MAX). Actively pushes the team-wide hoard out of tanks."},
    {"multi",   "Per-TANK multi-carry discount multiplier (<=1) — each pill THIS tank carries beyond the first knocks off STRATEGIC_PLACE_MULTI_CARRY_DISCOUNT (capped). Carrying >=2 also bypasses the util-reserve hold (the reserve justifies keeping ONE) and raises build urgency, which widens the placement search radius (STRATEGIC_PLACE_URGENCY_RANGE_BONUS)."},
    {"cpill",   "Capture pill multiplier — dead pill pickup scales the path cost down"},
    {"aim",     "Aim bonus (negative) — tank is already in your sights; cheaper to engage"},
    {"wall",    "Wall obstruction penalty — blocks between you and target beyond 1; 2 blocks=+100, 5 blocks=+400"},
    {"dens",    "Density discount multiplier — Phase 4; lower when many friendly candidates of the same kind cluster nearby (n = neighbor count). 1.0 = no discount."},
    {"pickup",  "A-star cost of spot to pill, minus this pill's danger contribution."},
    { "wsim",    "Forward-sim damage cost — additive armour/ammo cost from simulating travel through danger fields; set in the wsim block." },
    { "hist",    "Oscillation history penalty — increases when the bot repeatedly picks/abandons the same goal to break loops." },
    { "ally_claimed", "Ally-contention penalty when an ally bot is broadcasting the same goal (matched by kind + target_id, or kind + tile). refuel_at_base (pool 1): SOFT +ALLY_CLAIMED_REFUEL_PENALTY(100) per ally already targeting the SAME base — additive, NO reject, so a closer/more-urgent bot can still take a crowded base (then brakes beside it via wait_for_ally). place_strategic (pool 8): hard +10000. Pools 2-7 use the ally_claimed REJECT + steal band instead of this term. attack_tank/kill_lgm exempt." },
    { "tankpen",   "Combat-zone penalty on a NON-emergency strategic pill placement: flat +30 when an enemy tank is within 10 tiles (euclidean) of the chosen spot, +60 within 7. Discourages dropping a pill next to enemy tanks. The def_build emergency drop is exempt." },
    { "blitz_discount", "Multiplier (<=1) on any open blitz-call pill we could join, pulling it into our winners. Distance-scaled to the standoff: ~0.25x at 7 tiles, 1.0x (none) by 20. REF base used for reject/sentinel entries." },
    /* ── defend_pill (pool 2) formula terms ── */
    { "base",   "DEFEND_PILL_BASE_COST (250) — flat defend base; (base+dij) is the pre-multiplier travel bid. LIVE TIERS ONLY (siege/setup/sight): a quiet pill prints quiet_dmg{} in place of base{} and m{}." },
    { "dij",    "Dijkstra travel cost from THIS tank to the pill (per-bot pathfinding, so it doubles as the closeness signal for the defend steal band)." },
    { "m",      "Winning threat-tier multiplier on (base+dij) — min of the live siege/setup/sight tiers, times the cover multiplier. Lower = more urgent. Only printed for LIVE tiers; a quiet row has no m{} at all (the quiet_dmg curve replaces base x m outright)." },
    { "quiet_dmg", "QUIET-tier damage curve — REPLACES (base+dij)*m entirely when NO threat tier is live (no fresh damage / siege, no setup tell, no sighting). DEFEND_QUIET_DMG_COST indexed by HITS TAKEN = PILLS_MAX_HEALTH - health (printed as 'N hits'; NOT pill.attack_damage, which world.lua zeroes PILL_ATTACK_COOLDOWN ticks after the last hit and so reads 0 on every quiet pill): 0->1500, 1->1000, 2->800, 3->500, 4->300; past the table each further hit eases HALFWAY toward DEFEND_QUIET_DMG_FLOOR(250) — 5->275, 6->262, 7->256. Rationale: an untouched pill is barely worth leaving your post for, and each hit it has taken raises the urgency of going back. Quiet cost = (quiet_dmg + dij) x rdy, then the WELLDEF clamp and the tb sliver. The two-tier floor (100/200) does NOT apply to quiet bids — this curve supersedes it." },
    { "tb",     "Flat-cost tiebreaker sliver, ADDED after a floor{} or WELLDEF{} clamp flattened the bid: dij x 0.01 + (rdy - 1.00) x 2.0. Flat clamps erase the travel AND readiness ordering, so this folds both back in at ~0.5% scale; the defend steal band (ALLY_CLAIMED_STEAL_FRAC_DEFEND) then hands the job to the closer and better-equipped responder instead of whoever re-scored first." },
    { "siege",  "Siege tier: pill hit within DEFEND_DMG_FRESH_TICKS — someone is shelling it NOW. Savability-scaled: 1-(1-DEFEND_SIEGE_MULT)x(hp/max), so a healthy pill pulls hardest and an almost-dead one decays toward 1." },
    { "setup",  "Setup tier: hostile LGM seen near the pill (a wall/pill-block is going up -> a take is incoming). DEFEND_SETUP_MULT at fresh sighting, decaying linearly to 1 over DEFEND_SIGHT_FRESH_TICKS." },
    { "sight",  "Sight tier: hostile tank seen near the pill. DEFEND_SIGHT_MULT at fresh sighting, decaying to 1 over DEFEND_SIGHT_FRESH_TICKS. Sight-ONLY bids (no siege, no setup) floor at DEFEND_SIGHT_MIN_COST(200) — precaution never outbids real work." },
    { "cover",  "Coverage multiplier: DEFEND_COVERAGE_MULT^n for n other alive team pills whose fire reaches this one — arriving into friendly cover is cheaper. Threat-gated (only applies while a tier is live)." },
    { "late",   "Lateness/feasibility (siege only, but ALWAYS printed on a live-tier row so nothing is implied — 1.00 = on time): ETA(dij x DEFEND_ETA_PER_COST) vs TTL(hp x DEFEND_ASSUMED_TICKS_PER_HP); arriving after the pill would die scales cost up toward DEFEND_FUTILITY_MAX. Quiet rows have no late{} — the curve has no lateness term." },
    { "rdy",    "Readiness multiplier (>=1), ALWAYS printed (1.00 = fully equipped): 1 + shells deficit below DEFEND_READY_SHELLS + armour deficit below DEFEND_READY_ARMOUR, capped at DEFEND_READY_MAX_MULT. An under-equipped responder bids worse, so of equally distant allies the best-equipped wins the steal. Applies to BOTH the live-tier product and the quiet curve; it does NOT apply to an ARRIVED heat{} bid." },
    { "floor",  "The bid product came in under the floor and was clamped UP to this value — LIVE TIERS ONLY. DEFEND_MIN_COST(100) for live siege/setup; DEFEND_SIGHT_MIN_COST(200) for sight-only precaution. Quiet bids are never floored here: the quiet_dmg curve supersedes the floor and carries its own DEFEND_QUIET_DMG_FLOOR(250). Floored (and WELLDEF) bids then add the tb{} sliver so the defend steal band still orders equals by distance/equipment." },
    { "WELLDEF","Well-defended gate: allies already at the pill (visible allied tanks within DEFEND_WELL_DEFENDED_RADIUS, or allies claiming defend/repair there) cover the enemies present at R=ceil(their_team/our_team) — so any bid BELOW DEFEND_WELL_DEFENDED_COST(500) is RAISED to it and nobody else swarms in. Shown as ally count vs foe count, the ratio, and the value clamped to; the tb{} sliver is then added on top. A quiet bid above 500 (e.g. an undamaged 1500 pill) is left untouched — the gate only ever raises." },
    { "heat",   "ARRIVED handoff: within DEFEND_ARRIVE_RADIUS the travel bid is done; this flat DEFEND_HEAT_COST bids only the heat-up action (put HEAT_PILL_SHOTS shells in to anger the pill)." },
    { "sel",    "Selection preview: what THIS row's cost becomes after goal_selection's deterministic scaling — cost x ph (phase weight, distance-attenuated toward 1.0 by PHASE_WEIGHT_DIST_FALLOFF) x inf (territory influence: x0.5 in friendly ground, x2 in hostile). ALWAYS printed with both factors, even when both are neutral, so the row's end score is computable from the row alone. Switch/commit penalties are state-dependent and added on top at selection; see the WINNERS row for the full chain." },
    { "xph",    "Phase-weight factor inside sel{}: PHASE_WEIGHTS[phase][pool], lerped toward 1.0 with distance (a far goal doesn't inherit the local phase bias)." },
    { "xinf",   "Influence factor inside sel{}: 0.5 when the target sits in friendly territory (influence > 50), 2.0 in hostile (< -50), else 1.0." },
    { NULL, NULL }
    };

/* ── cJSON helpers ─────────────────────────────────── */
double getNum(const cJSON *o, const char *k, double d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsNumber(v)) ? v->valuedouble : d;
}
const char *getStr(const cJSON *o, const char *k, const char *d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    return (v && cJSON_IsString(v)) ? v->valuestring : d;
}
bool getBool(const cJSON *o, const char *k, bool d) {
    cJSON *v = cJSON_GetObjectItem(o, k);
    if (!v) return d;
    if (cJSON_IsBool(v))   return cJSON_IsTrue(v);
    if (cJSON_IsNumber(v)) return v->valuedouble != 0.0;
    return d;
}

/* ── Per-pool color (matches optimize branch verbatim) ── */
ImVec4 poolColorFor(int idx) {
    if (idx == 0)  return ImVec4(1.0f, 0.4f,  1.0f,  1);   /* override: magenta */
    if (idx == 1)  return ImVec4(0.4f, 1.0f,  0.4f,  1);   /* refuel: green */
    if (idx == 2)  return ImVec4(1.0f, 0.85f, 0.3f,  1);   /* defend_pill: gold */
    if (idx == 3)  return ImVec4(0.55f,0.8f,  1.0f,  1);   /* capture_base: sky blue */
    if (idx == 7)  return ImVec4(0.25f,0.45f, 0.95f, 1);   /* attack_base: deep blue */
    if (idx == 4)  return ImVec4(1.0f, 0.7f,  0.75f, 1);   /* capture_pill: light pink */
    if (idx == 5)  return ImVec4(1.0f, 0.5f,  0.45f, 1);   /* repair_pill: salmon */
    if (idx == 6)  return ImVec4(0.95f,0.25f, 0.25f, 1);   /* attack_pill: deep red */
    if (idx == 8)  return ImVec4(0.4f, 0.9f,  0.9f,  1);   /* place_strategic: cyan */
    if (idx == 9)  return ImVec4(1.0f, 0.65f, 0.2f,  1);   /* attack_tank: orange */
    if (idx == 10) return ImVec4(1.0f, 1.0f,  1.0f,  1);   /* winners: white */
    if (idx == 11) return ImVec4(0.7f, 0.5f,  1.0f,  1);   /* def_build: violet */
    if (idx == 12) return ImVec4(0.55f,0.85f, 0.55f, 1);   /* wait_for_lgm: sage */
    if (idx == 13) return ImVec4(1.0f, 0.85f, 0.2f,  1);   /* kill_lgm: yellow */
    if (idx == 14) return ImVec4(0.6f, 0.75f, 0.85f, 1);   /* take_cover: slate */
    if (idx == 15) return ImVec4(0.5f, 0.85f, 1.0f,  1);   /* builder pool: ice blue */
    return ImVec4(0.8f, 0.8f, 0.8f, 1);
}

/* ── Parsed sections / rows ─────────────────────────── */
struct Row {
    int    id;
    int    src_pool;
    int    mx, my;
    float  cost, weighted;
    bool   winner;
    bool   activeGoal;
    bool   isOverride;
    int    staleTicks;
    float  flashAlpha;
    /* Bumped from 512 → 2048 to fit pool-6's multi-segment detail map
     * (hp / anger / stale / finish_other / self_dr / ammo / spot —
     * the last carries the realized path tiles which alone can be
     * 300+ chars). Truncation here cuts off the trailing segments
     * and the parser then prints "?" in the affected rows. */
    char   formula[6144];
    /* Reject info (capture_pill: dead pill that exists on the map but
     * can't be picked this tick — in_tank / blocked / stale). When
     * non-empty, the row renders dimmed with a colored chip. */
    char   reject[24];
    int    rejectRemaining;
    /* Steal indicator: we're keeping this goal away from an ally who's also
     * bidding, by being meaningfully cheaper. allyBy = that ally's player#. */
    bool   stealing;
    int    allyBy;
    /* Blitz indicator (attack_pill only): this pill is a squad blitz — our own
     * lead, the one we've committed to, one we're negotiating to join, or a
     * teammate's open call. Rendered with a distinct color + "BZ" marker. */
    bool   blitz;
};

/* Maximum section index the renderer knows how to place. 1..10 fill the first
 * five columns of the 2x6 grid; 15 (BUILDER) is the BOTTOM half of the sixth
 * column, beside the winners pool (10), and its top half is deliberately left
 * empty (every column is split in two, used or not); 11..14 are full-width
 * strips below the grid. Bump this
 * (and MAX_SECT_IDX's users) when the brain adds a section -- everything below
 * is sized from it. */
enum { MAX_SECT_IDX = 15 };

struct Section {
    int   idx;
    char  name[32];
    int   count;
    int   winner_id;
    float phase_weight;
    Row  *rows;
    int   nrows;
    /* Optional header lines printed between the section title and the rows.
     * The BUILDER strip (15) uses them for its owner / eligibility / active-job
     * lines -- state that belongs to the whole pool rather than to any one
     * candidate, and so has no row to live on. Sections that send no "hdr"
     * array render exactly as before. */
    char  hdr[4][256];
    int   nhdr;
};

static const float FLASH_DURATION_MS = 1000.0f;

/* All persistent per-window state lives here. Keyed by the panel
 * registry idx the host passes to render() so that opening pool
 * windows for two different bots doesn't bleed selection / detail-
 * popup / rank-flash state between them. */
struct DetailRow {
    bool  open, justOpened;
    int   sectionIdx;
    int   srcPool;
    char  poolName[32];
    int   rowId, mx, my;
    float cost, weighted;
    bool  winner;
    /* Match Row::formula's 2048 — without this, the double-click
     * handler truncates the formula again on its way into the popup
     * and the trailing detail segments (e.g. spot path) get lost. */
    char  formula[2048];
};
struct PanelState {
    /* Per-section rank tracking for flash animation, indexed by
     * section idx (1..MAX_SECT_IDX). map<row_id → rank/flash-start>.
     * Sized MAX_SECT_IDX+1 so the idx itself is a valid subscript. */
    std::unordered_map<int,int>    prevRank[MAX_SECT_IDX + 1];
    std::unordered_map<int,Uint64> flashStart[MAX_SECT_IDX + 1];

    /* Click selection + Ctrl+C copy buffer. */
    int  selectedSection = -1;
    int  selectedRowId   = -1;
    char copyBuf[512]    = {0};
    bool anyRowClickedThisFrame = false;

    /* Double-click detail popup. */
    DetailRow detail = {};

    /* Winners legend window. */
    bool showLegend = false;
};

/* Lookup-by-idx so each (registry_idx → PanelState) pair survives
 * across frames. unordered_map for sparse keying — most idxs never
 * have a pool_grid renderer attached. */
static std::unordered_map<int, PanelState> g_states;

static PanelState &stateFor(int registry_idx) {
    /* operator[] default-constructs on first access; subsequent calls
     * return the same instance. -1 (no idx) gets its own slot. */
    return g_states[registry_idx];
}

/* ── Section list parsing from cJSON ─────────────────── */
static void parseRow(cJSON *jrow, Row *r, int section_idx, bool is_winners) {
    memset(r, 0, sizeof(*r));
    r->id        = (int)getNum(jrow, "id", 0);
    r->src_pool  = is_winners ? (int)getNum(jrow, "src_pool", section_idx)
                              : section_idx;
    r->mx        = (int)getNum(jrow, "mx", 0);
    r->my        = (int)getNum(jrow, "my", 0);
    r->cost      = (float)getNum(jrow, "cost", -1);
    r->weighted  = (float)getNum(jrow, "weighted", -1);
    r->winner    = getBool(jrow, "is_winner", false);
    r->activeGoal= getBool(jrow, "active_goal", false);
    r->isOverride= getBool(jrow, "is_override", false);
    r->staleTicks= (int)getNum(jrow, "stale", -1);
    const char *f = getStr(jrow, "formula", "");
    SDL_strlcpy(r->formula, f, sizeof(r->formula));
    const char *rej = getStr(jrow, "reject", "");
    SDL_strlcpy(r->reject, rej, sizeof(r->reject));
    r->rejectRemaining = (int)getNum(jrow, "reject_remaining", 0);
    r->stealing = getBool(jrow, "stealing", false);
    r->allyBy   = (int)getNum(jrow, "ally_by", -1);
    r->blitz    = getBool(jrow, "blitz", false);
}

static int parseSections(cJSON *root, Section *out, int outMax) {
    cJSON *jsecs = cJSON_GetObjectItem(root, "sections");
    if (!jsecs || !cJSON_IsArray(jsecs)) return 0;
    int n = cJSON_GetArraySize(jsecs);
    if (n > outMax) n = outMax;
    for (int i = 0; i < n; i++) {
        cJSON *js = cJSON_GetArrayItem(jsecs, i);
        Section *s = &out[i];
        memset(s, 0, sizeof(*s));
        s->idx          = (int)getNum(js, "idx", i + 1);
        SDL_strlcpy(s->name, getStr(js, "name", "?"), sizeof(s->name));
        s->phase_weight = (float)getNum(js, "weight", 1.0);
        s->winner_id    = (int)getNum(js, "winner_id", -1);
        cJSON *jhdr = cJSON_GetObjectItem(js, "hdr");
        if (jhdr && cJSON_IsArray(jhdr)) {
            int hn = cJSON_GetArraySize(jhdr);
            if (hn > 4) hn = 4;
            for (int hi = 0; hi < hn; hi++) {
                cJSON *jh = cJSON_GetArrayItem(jhdr, hi);
                if (cJSON_IsString(jh) && jh->valuestring)
                    SDL_strlcpy(s->hdr[hi], jh->valuestring, sizeof(s->hdr[hi]));
            }
            s->nhdr = hn;
        }
        cJSON *jrows = cJSON_GetObjectItem(js, "rows");
        int rn = (jrows && cJSON_IsArray(jrows)) ? cJSON_GetArraySize(jrows) : 0;
        s->nrows = rn;
        s->count = rn;
        if (rn > 0) {
            s->rows = (Row *)calloc(rn, sizeof(Row));
            bool is_winners = (strcmp(s->name, "WINNERS") == 0);
            for (int ri = 0; ri < rn; ri++) {
                parseRow(cJSON_GetArrayItem(jrows, ri),
                         &s->rows[ri], s->idx, is_winners);
            }
        }
    }
    return n;
}

static void freeSections(Section *s, int n) {
    for (int i = 0; i < n; i++) free(s[i].rows);
}

/* ── Row rendering — verbatim from optimize branch ──
 * `st` is the per-window state; selection / detail / copy buffer
 * all live there so two windows for different bots don't share. */
static void renderRow(PanelState &st, const Section *s, int i, Row *r) {
    ImVec4 rowCol = poolColorFor(r->src_pool);
    const float lineH = ImGui::GetTextLineHeightWithSpacing();
    const int lines = (r->formula[0] ? 2 : 1);
    /* Rejected rows (e.g., capture_pill row that exists but cannot be
     * picked this tick — in_tank / blocked / stale) render dimmed with
     * a colored chip explaining why. The hit-button stays full-alpha so
     * the row remains clickable / inspectable in the detail popup. */
    const bool isRej = (r->reject[0] != '\0');

    ImVec2 hitCursorStart = ImGui::GetCursorPos();
    char hitId[32];
    SDL_snprintf(hitId, sizeof(hitId), "##hit%d_%d", s->idx, i);
    if (ImGui::InvisibleButton(hitId,
            ImVec2(ImGui::GetContentRegionAvail().x, lineH * lines))) {
        st.selectedSection        = s->idx;
        st.selectedRowId          = r->id;
        st.anyRowClickedThisFrame = true;

        /* For the copy buffer, use the original ID and the final weighted cost. */
        int displayId = (s->idx == 10) ? (r->id & 0xFFFF) : r->id;

        if (r->weighted >= 1e9f)
            SDL_snprintf(st.copyBuf, sizeof(st.copyBuf),
                "#%d (%d,%d) cost=INF  %s",
                displayId, r->mx, r->my, r->formula);
        else
            SDL_snprintf(st.copyBuf, sizeof(st.copyBuf),
                "#%d (%d,%d) cost=%.0f  %s",
                displayId, r->mx, r->my, r->weighted, r->formula);
    }
    if (ImGui::IsItemHovered() &&
        ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)) {
        st.detail.open       = true;
        st.detail.justOpened = true;
        st.detail.sectionIdx = s->idx;
        st.detail.srcPool    = r->src_pool;
        SDL_strlcpy(st.detail.poolName, s->name, sizeof(st.detail.poolName));
        st.detail.rowId    = r->id;
        st.detail.mx       = r->mx;
        st.detail.my       = r->my;
        st.detail.cost     = r->cost;
        st.detail.weighted = r->weighted;
        st.detail.winner   = r->winner;
        SDL_strlcpy(st.detail.formula, r->formula, sizeof(st.detail.formula));
    }
    bool isSelected = (st.selectedSection == s->idx
                       && st.selectedRowId == r->id);
    ImGui::SetCursorPos(hitCursorStart);

    ImVec2 rowStart = ImGui::GetCursorScreenPos();
    float rectW = rowStart.x + ImGui::GetContentRegionAvail().x;
    ImVec2 a = ImVec2(rowStart.x - 2, rowStart.y - 1);
    ImVec2 b = ImVec2(rectW, rowStart.y + lineH * lines + 1);
    if (isSelected) {
        ImGui::GetWindowDrawList()->AddRectFilled(a, b,
            IM_COL32(80, 120, 200, 90), 3.0f);
    }
    if (r->winner) {
        ImU32 bg = IM_COL32(
            (int)(rowCol.x * 90),
            (int)(rowCol.y * 90),
            (int)(rowCol.z * 90),
            160);
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, bg, 3.0f);
    }
    if (r->activeGoal) {
        ImGui::GetWindowDrawList()->AddRectFilled(a, b,
            IM_COL32(80, 220, 80, 55), 3.0f);
        ImGui::GetWindowDrawList()->AddRect(a, b,
            IM_COL32(120, 255, 120, 255), 3.0f, 0, 2.0f);
    }
    if (r->flashAlpha > 0.0f) {
        ImU32 flash = IM_COL32(255, 255, 200,
                               (int)(r->flashAlpha * 120));
        ImGui::GetWindowDrawList()->AddRectFilled(a, b, flash, 3.0f);
    }
    if (r->blitz) {
        /* Blitz tint: magenta wash + border so squad-blitz attack_pill rows
         * stand out from solo takes. Matches the squad_blitz overlay theme. */
        ImGui::GetWindowDrawList()->AddRectFilled(a, b,
            IM_COL32(200, 60, 200, 45), 3.0f);
        ImGui::GetWindowDrawList()->AddRect(a, b,
            IM_COL32(230, 90, 230, 200), 3.0f, 0, 1.5f);
    }

    /* Dim the entire row when rejected. Pop'd at the end of the row.
     * Hit area was already drawn above with full alpha. */
    if (isRej) {
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }

    /* Line 1: marker / id / pos / cost / weighted / staleness */
    /* ASCII markers (not ▶/⚡) so they render in the default ImGui font.
     * Without a font that includes U+25B6 / U+26A1 they show as "?". */
    if (r->activeGoal) {
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1), ">");
    } else if (r->winner) {
        ImGui::TextColored(ImVec4(1, 1, 0.4f, 1), "*");
    } else {
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), " ");
    }
    ImGui::SameLine();
    if (r->isOverride) {
        ImGui::TextColored(ImVec4(1.0f, 0.9f, 0.1f, 1.0f), "!");
        ImGui::SameLine();
    }

    int displayId = (s->idx == 10) ? (r->id & 0xFFFF) : r->id;
    ImGui::TextColored(rowCol, "#%-3d", displayId);
    ImGui::SameLine();
    if (r->blitz) {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 1.0f, 1), "BZ");
        ImGui::SameLine();
    }
    ImGui::TextColored(ImVec4(0.85f, 0.85f, 0.85f, 1),
        "(%3d,%3d)", r->mx, r->my);
    ImGui::SameLine();
    if (r->weighted <= -1e9f) {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "cost=   ?   ");
    } else if (r->weighted >= 1e9f) {
        ImGui::TextColored(ImVec4(1, 0.3f, 0.3f, 1), "cost=  INF  ");
    } else if (r->weighted < 0) {
        /* Negative: active-goal discount drove total below zero. */
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1),
            "cost= %-6.0f", r->weighted);
    } else {
        ImGui::TextColored(ImVec4(1, 1, 0.4f, 1),
            "cost= %-6.0f", r->weighted);
    }
    /* Active goal ('>'): show the multiplicative switch bar (GOAL_SWITCH_RATIO,
     * 0.7) — a challenger only takes over if its cost is below this. Makes the
     * "why did the cheaper goal not win?" stickiness visible. (Mirror of
     * constants.lua GOAL_SWITCH_RATIO — keep in sync.) */
    if (r->activeGoal && r->weighted > -1e9f && r->weighted < 1e9f) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1),
            "x0.7=%-6.0f", r->weighted * 0.7f);
    }
    ImGui::SameLine();
    /* Thresholds match step_eval_queue's tiered re-eval TTLs: close pills
     * refresh every 50 ticks, mid-range every 150, far pills every 500
     * (~10 s). Color green up to the close-tier TTL, fade yellow→orange
     * across the far-tier window, red beyond. */
    if (r->staleTicks < 0) {
        ImGui::TextColored(ImVec4(0.3f, 0.3f, 0.3f, 1), "~");
    } else if (r->staleTicks < 50) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1), "%dt", r->staleTicks);
    } else if (r->staleTicks < 500) {
        float t = (float)(r->staleTicks - 50) / 450.0f;
        ImGui::TextColored(ImVec4(1.0f, 1.0f - t * 0.5f, 0.3f, 1),
                           "%dt", r->staleTicks);
    } else {
        ImGui::TextColored(ImVec4(1.0f, 0.3f, 0.3f, 1), "%dt", r->staleTicks);
    }

    /* Per-row phase multiplier (same xN.N as the section header). Shows how
     * this pool's raw cost is scaled in the competition — the "weighted
     * (xphase)" value in the detail popup is cost x this. Drawn to the right
     * of the staleness timestamp. */
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.55f, 0.78f, 1.0f, 1), "x%.2f", s->phase_weight);

    /* Reject chip — appears on the same line as cost/wt/stale. Pop'd
     * the dim alpha briefly so the chip itself reads at full opacity. */
    if (isRej) {
        ImGui::SameLine();
        ImGui::PopStyleVar();
        ImVec4 chipCol;
        if      (!strcmp(r->reject, "in_tank")) chipCol = ImVec4(0.95f,0.85f,0.20f,1);
        else if (!strcmp(r->reject, "blocked")) chipCol = ImVec4(1.00f,0.40f,0.40f,1);
        else if (!strcmp(r->reject, "stale"))   chipCol = ImVec4(0.70f,0.70f,0.70f,1);
        else if (!strcmp(r->reject, "blitz"))   chipCol = ImVec4(0.90f,0.40f,0.90f,1); /* magenta: joinable blitz, not a solo claim */
        else                                     chipCol = ImVec4(0.85f,0.85f,0.85f,1);
        if ((!strcmp(r->reject, "blocked") || !strcmp(r->reject, "stale"))
            && r->rejectRemaining > 0) {
            ImGui::TextColored(chipCol, "[%s %dt]", r->reject, r->rejectRemaining);
        } else {
            ImGui::TextColored(chipCol, "[%s]", r->reject);
        }
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha,
                            ImGui::GetStyle().Alpha * 0.45f);
    }

    /* Steal chip — this row is kept away from an ally who's also bidding
     * because we're meaningfully cheaper. Not a reject (row stays bright),
     * so render it as a magenta chip with the ally's player number. */
    if (r->stealing) {
        ImGui::SameLine();
        if (r->allyBy >= 0)
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 1.0f, 1), "[STEAL<-p%d]", r->allyBy);
        else
            ImGui::TextColored(ImVec4(1.0f, 0.35f, 1.0f, 1), "[STEAL]");
    }

    /* Blitz chip — this attack_pill is an OPEN squad blitz (ours, joinable, or a
     * teammate's call). NOT a reject: it stays a live, continually re-evaluated
     * candidate (we join via the squad layer if we pick it). Magenta to match the
     * BZ row style. Suppressed when the row is rejected (truly-closed take shows
     * its reject chip instead). */
    if (r->blitz && !isRej) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.95f, 0.40f, 0.95f, 1), "[blitz]");
    }

    /* Line 2: dim formula.
     * Use "!!" as a row-display override separator: "Simplified !! Full || Detail"
     * If "!!" is present, show only what's before it.
     * Otherwise show what's before "||". */
    if (r->formula[0]) {
        ImVec4 fcol = ImVec4(rowCol.x * 0.7f, rowCol.y * 0.7f,
                             rowCol.z * 0.7f, 1);
        const char *rowSep = strstr(r->formula, "!!");
        const char *detailSep = strstr(r->formula, "||");
        const char *end = rowSep ? rowSep : (detailSep ? detailSep : (r->formula + strlen(r->formula)));

        int dispLen = (int)(end - r->formula);
        char dispBuf[512];
        if (dispLen >= (int)sizeof(dispBuf)) dispLen = (int)sizeof(dispBuf) - 1;
        memcpy(dispBuf, r->formula, dispLen);
        dispBuf[dispLen] = '\0';
        ImGui::TextColored(fcol, "    %s", dispBuf);
    }
    if (isRej) ImGui::PopStyleVar();
    ImGui::Spacing();
}

static void renderSection(PanelState &st, Section *s) {
    ImVec4 col = poolColorFor(s->idx);
    bool isWinners = (strcmp(s->name, "WINNERS") == 0);
    if (!isWinners) {
        ImGui::SetWindowFontScale(1.5f);
        ImGui::TextColored(col, "%d.", s->idx);
        ImGui::SetWindowFontScale(1.0f);
        ImGui::SameLine();
    }
    ImGui::TextColored(col, "%s", s->name);
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
        "(%d  x%.1f)", s->count, s->phase_weight);
    if (isWinners) {
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::SameLine(ImGui::GetCursorPosX() + avail - 22.0f);
        if (ImGui::SmallButton("?##legend"))
            st.showLegend = !st.showLegend;
    }
    ImGui::Separator();

    /* Pool-wide header lines (BUILDER's owner / eligibility / active job).
     * Printed before the "(empty)" bail on purpose: a builder pool with no
     * candidates at all still has to be able to say WHY -- an eligibility
     * verdict of "DENY fire_exchange:shoot_pill" is the most informative
     * thing the strip can show, and it is exactly the tick on which there
     * are no rows to hang it off. */
    for (int hi = 0; hi < s->nhdr; hi++) {
        if (s->hdr[hi][0] == '\0') continue;
        ImGui::TextColored(ImVec4(0.72f, 0.72f, 0.72f, 1), "%s", s->hdr[hi]);
    }

    if (s->nrows == 0) {
        ImGui::TextColored(ImVec4(0.4f, 0.4f, 0.4f, 1), "(empty)");
        return;
    }
    char childId[32];
    SDL_snprintf(childId, sizeof(childId), "##sec%d", s->idx);
    ImGui::BeginChild(childId, ImVec2(0, 0), false,
                      ImGuiWindowFlags_HorizontalScrollbar);
    for (int i = 0; i < s->nrows; i++) {
        renderRow(st, s, i, &s->rows[i]);
    }
    ImGui::EndChild();
}

/* Detail popup — verbatim port from optimize-branch poolwindow.
 * Splits the formula at "||" into a display half and a computation
 * half; parses the display half into name{value} terms; cross-
 * references each term with kTermDocs (meaning) and the computation
 * half (how-computed); renders everything in a 3- or 4-column table. */
static void renderDetailPopup(PanelState &st, int winW, int winH,
                              int tickIn, int followBot) {
    DetailRow &sDetail = st.detail;
    if (!sDetail.open) return;

    if (sDetail.justOpened) {
        ImGui::SetNextWindowFocus();
        sDetail.justOpened = false;
    }
    ImGui::SetNextWindowSize(ImVec2(560, 460), ImGuiCond_FirstUseEver);
    ImGui::SetNextWindowPos(
        ImVec2((float)winW * 0.5f - 280, (float)winH * 0.5f - 230),
        ImGuiCond_FirstUseEver);

    char title[80];
    ImVec4 poolCol = poolColorFor(sDetail.srcPool > 0
                                  ? sDetail.srcPool
                                  : sDetail.sectionIdx);
    SDL_snprintf(title, sizeof(title),
                 "Pool %d (%s)  —  Row #%d###detail",
                 sDetail.sectionIdx, sDetail.poolName, sDetail.rowId);

    bool open = sDetail.open;
    ImGui::Begin(title, &open,
                 ImGuiWindowFlags_NoCollapse | ImGuiWindowFlags_NoSavedSettings);

    if (ImGui::IsWindowFocused() &&
        ImGui::IsKeyPressed(ImGuiKey_Escape, false))
        open = false;

    ImGui::TextColored(poolCol, "Pool %d: %s",
                       sDetail.sectionIdx, sDetail.poolName);
    if (sDetail.winner) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f),
                           "  \xe2\x98\x85 WINNER");
    }
    ImGui::Separator();

    /* Mask synthetic ID for display in WINNERS pool. */
    int displayId = (sDetail.sectionIdx == 10) ? (sDetail.rowId & 0xFFFF) : sDetail.rowId;

    ImGui::Text("ID: #%d    Location: (%d, %d)",
                displayId, sDetail.mx, sDetail.my);
    /* ASCII dashes (not em-dash) so the glyph always renders in the
     * default ImGui font. The em-dash showed as "?" otherwise. */
    if (sDetail.cost < 0)
        ImGui::Text("Cost: (pending)    Weighted: --");
    else if (sDetail.cost >= 1e9f)
        ImGui::Text("Cost: INF    Weighted: INF");
    else
        ImGui::Text("Cost: %.1f    Weighted (xphase): %.1f",
                    sDetail.cost, sDetail.weighted);
    ImGui::Separator();

    /* Split the formula into [RowOverride!!]DisplayHalf||ComputationHalf. */
    const char *rowSep = strstr(sDetail.formula, "!!");
    const char *start = rowSep ? (rowSep + 2) : sDetail.formula;
    const char *detailSep = strstr(start, "||");

    char dispFormula[512] = {0};
    if (detailSep) {
        int dlen = (int)(detailSep - start);
        if (dlen >= (int)sizeof(dispFormula)) dlen = (int)sizeof(dispFormula) - 1;
        memcpy(dispFormula, start, dlen);
    } else {
        SDL_strlcpy(dispFormula, start, sizeof(dispFormula));
    }

    /* Parse the per-term computation map from the || section.
     * Format: "name:computation|name:computation|..." */
    struct TermCompute { char name[32]; char compute[2048]; };
    TermCompute computes[24];
    int nComputes = 0;
    if (detailSep) {
        const char *dp = detailSep + 2;
        while (*dp && nComputes < 24) {
            const char *segEnd = strchr(dp, '|');
            if (!segEnd) segEnd = dp + strlen(dp);
            const char *colon = (const char *)memchr(dp, ':', segEnd - dp);
            if (colon) {
                int nlen = (int)(colon - dp);
                int clen = (int)(segEnd - colon - 1);
                if (nlen > 0 && nlen < 32 && clen > 0 && clen < 2048) {
                    SDL_strlcpy(computes[nComputes].name,    dp,        nlen + 1);
                    SDL_strlcpy(computes[nComputes].compute, colon + 1, clen + 1);
                    nComputes++;
                }
            }
            dp = (*segEnd == '|') ? segEnd + 1 : segEnd;
        }
    }

    /* Display-portion formula in yellow. */
    ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "Formula:");
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 1.0f, 0.65f, 1.0f));
    ImGui::TextWrapped("  %s", dispFormula[0] ? dispFormula : "(none)");
    ImGui::PopStyleColor();
    ImGui::Separator();

    /* Parse word{value} terms out of the display formula. */
    struct ParsedTerm { char name[32]; char value[32]; };
    ParsedTerm terms[32];
    int nTerms = 0;
    const char *p = dispFormula;
    while (*p && nTerms < 32) {
        const char *ob = strchr(p, '{');
        if (!ob) break;
        const char *cb = strchr(ob + 1, '}');
        if (!cb) break;
        const char *ns = ob - 1;
        while (ns >= dispFormula &&
               (isalnum((unsigned char)*ns) || *ns == '*' || *ns == '_'))
            ns--;
        ns++;
        int nlen = (int)(ob - ns);
        int vlen = (int)(cb - ob - 1);
        if (nlen > 0 && nlen < 32 && vlen < 32) {
            SDL_strlcpy(terms[nTerms].name,  ns,     nlen + 1);
            SDL_strlcpy(terms[nTerms].value, ob + 1, vlen + 1);
            (void)tickIn; (void)followBot;
            nTerms++;
        }
        p = cb + 1;
    }

    bool hasComputed = (nComputes > 0);
    if (nTerms > 0) {
        ImGui::Text("Term Breakdown:");
        ImGui::Spacing();
        int nCols = hasComputed ? 4 : 3;
        if (ImGui::BeginTable("##termtbl", nCols,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_ScrollY)) {
            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Term",    ImGuiTableColumnFlags_WidthFixed,  68.0f);
            ImGui::TableSetupColumn("Value",   ImGuiTableColumnFlags_WidthFixed,  52.0f);
            ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
            if (hasComputed)
                ImGui::TableSetupColumn("How computed",
                                        ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (int ti = 0; ti < nTerms; ti++) {
                /* The term scanner walks back over '*' as part of the name,
                 * so a multiplied chip parses as "*m" / "*rdy" / "*late".
                 * Strip the operator before looking the term up, or every
                 * multiplicative factor renders "(no description)". */
                const char *tname = terms[ti].name;
                while (*tname == '*') tname++;
                const char *docStr = NULL;
                for (int di = 0; kTermDocs[di].term; di++) {
                    if (strcmp(tname, kTermDocs[di].term) == 0) {
                        docStr = kTermDocs[di].desc;
                        break;
                    }
                }
                const char *compStr = NULL;
                for (int ci = 0; ci < nComputes; ci++) {
                    if (strcmp(tname, computes[ci].name) == 0) {
                        compStr = computes[ci].compute;
                        break;
                    }
                }
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.5f, 1.0f),
                                   "%s", terms[ti].name);
                ImGui::TableSetColumnIndex(1);
                ImGui::TextColored(ImVec4(0.7f, 0.9f, 1.0f, 1.0f),
                                   "%s", terms[ti].value);
                ImGui::TableSetColumnIndex(2);
                if (docStr) ImGui::TextWrapped("%s", docStr);
                else        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1),
                                               "(no description)");
                if (hasComputed) {
                    ImGui::TableSetColumnIndex(3);
                    if (compStr)
                        ImGui::TextWrapped("%s", compStr);
                    else
                        ImGui::TextColored(ImVec4(0.5f,0.5f,0.5f,1), "—");
                }
            }
            ImGui::EndTable();
        }
    } else {
        ImGui::TextColored(ImVec4(0.5f, 0.5f, 0.5f, 1.0f),
                           "No formula terms to explain.");
    }

    ImGui::End();
    sDetail.open = open;
}

/* ── Top-level renderer registered for "GoalHunter:pool_grid" ── */
void renderPoolGrid(int registry_idx, const char *body) {
    PanelState &st = stateFor(registry_idx);
    if (!body || !body[0]) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.4f, 1.0f),
                           "(no data yet — polling brain.get_pool_breakdown_json)");
        return;
    }
    cJSON *root = cJSON_Parse(body);
    if (!root) {
        ImGui::TextColored(ImVec4(1.0f, 0.5f, 0.4f, 1.0f),
                           "JSON parse error");
        ImGui::TextWrapped("Body: %.200s%s", body,
                           strlen(body) > 200 ? "..." : "");
        return;
    }

    const int   kSchemaVersion = 1;
    int  schemaV     = (int)getNum(root, "schema_version", 0);
    const char *phase = getStr(root, "phase", "?");
    int  tickIn      = (int)getNum(root, "tick", -1);
    int  replanIn    = (int)getNum(root, "replan_left", -1);
    const char *debugSession = getStr(root, "debug_session", "");
    bool isReplanTick = (replanIn == 0);
    int  followBot    = (int)getNum(root, "bot", -1);

    /* Schema-version banner — render but don't bail. The renderer
     * may still produce a useful (if degraded) view if the brain
     * upgraded fields without breaking shape. */
    if (schemaV != kSchemaVersion) {
        ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.3f, 1.0f),
            "WARN: pool_grid schema_version=%d, renderer expects %d",
            schemaV, kSchemaVersion);
    }

    /* Index sections by idx for grid placement (1..12). */
    enum { MAX_SECS = 32 };
    Section sections[MAX_SECS];
    int nSections = parseSections(root, sections, MAX_SECS);

    /* Sized MAX_SECT_IDX+1 so the section idx is the subscript. The bound used
     * to be 12, which silently dropped kill_lgm (13) and take_cover (14) -- the
     * brain has been emitting both for some time and neither ever rendered.
     * Fixed here alongside the new BUILDER strip (15). */
    Section *byIdx[MAX_SECT_IDX + 1] = {0};
    for (int i = 0; i < nSections; i++) {
        if (sections[i].idx >= 1 && sections[i].idx <= MAX_SECT_IDX) {
            byIdx[sections[i].idx] = &sections[i];
        }
    }

    /* Flash animation: any row whose rank changed this frame gets a
     * fresh flash timestamp; flashAlpha then decays over
     * FLASH_DURATION_MS. */
    Uint64 now = SDL_GetTicks();
    for (int si = 1; si <= MAX_SECT_IDX; si++) {
        Section *sec = byIdx[si];
        if (!sec) continue;
        auto &prev  = st.prevRank[si];
        auto &flash = st.flashStart[si];
        for (int ri = 0; ri < sec->nrows; ri++) {
            Row *r = &sec->rows[ri];
            /* Use a composite key to distinguish items with the same ID
             * from different source pools (common in the WINNERS pool).
             * Without this, two winners sharing an ID would clash in
             * flashStart and prevRank, causing one to always think its
             * rank changed and getting stuck in the white flash state. */
            int key = (r->src_pool << 16) | r->id;
            auto it = prev.find(key);
            bool rankChanged = (it == prev.end()) || (it->second != ri);
            if (rankChanged) flash[key] = now;
            auto fit = flash.find(key);
            if (fit != flash.end()) {
                float elapsed = (float)(now - fit->second);
                r->flashAlpha = 1.0f - elapsed / FLASH_DURATION_MS;
                if (r->flashAlpha < 0.0f) r->flashAlpha = 0.0f;
            }
        }
        prev.clear();
        for (int ri = 0; ri < sec->nrows; ri++) {
            int key = (sec->rows[ri].src_pool << 16) | sec->rows[ri].id;
            prev[key] = ri;
        }
    }

    /* Top-of-window header. The host has already opened a Begin() —
     * we render the inner content. */
    ImGui::SetWindowFontScale(1.6f);
    ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1),
                       "TANK %d", followBot);
    ImGui::SetWindowFontScale(1.0f);
    ImGui::SameLine();
    if (tickIn >= 0)
        ImGui::Text("  Tick %d  Phase: %s", tickIn, phase);
    else
        ImGui::Text("  Phase: %s", phase);
    if (debugSession[0] != '\0') {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1.0f),
                           "  [%s]", debugSession);
        ImGui::SameLine();
        if (ImGui::SmallButton("Copy")) {
            char clip[256];
            SDL_snprintf(clip, sizeof(clip), "%s bot%d tick %d",
                         debugSession, followBot, tickIn);
            SDL_SetClipboardText(clip);
        }
    }
    ImGui::SameLine();
    if (replanIn == 0) {
        ImGui::TextColored(ImVec4(0.3f, 1.0f, 0.3f, 1),
            "    >>> REPLAN THIS TICK <<<");
    } else if (replanIn > 0 && replanIn <= 5) {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.3f, 1),
            "    Replan: %d", replanIn);
    } else if (replanIn > 0) {
        ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1),
            "    Replan: %d", replanIn);
    }
    ImGui::SameLine();
    ImGui::TextColored(ImVec4(0.6f, 0.6f, 0.6f, 1),
        "    *=winner per pool   wt=cost*phase_weight");
    ImGui::Separator();

    /* 2x6 grid: sections 1..10 in the first five columns (1..5 on top, 6..10
     * below), and the BUILDER section (15) in the BOTTOM half of the sixth
     * column, next to the winners pool (10). The sixth column's top half
     * stays empty on purpose -- the grid's pattern is "every column split in
     * two", and BUILDER is the only section that lives there. (It used to be a full-width strip under the
     * grid; the author wanted a column that follows the pattern instead.) */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    /* Reserve room at the bottom for the def_build (11) / wait_for_lgm (12) /
     * kill_lgm (13) / take_cover (14) strips when those sections exist. */
    const float kStripH        = 52.0f;
    const float kStripStrideH  = 56.0f;
    float reservedH = 0.0f;
    if (byIdx[11]) reservedH += kStripStrideH;
    if (byIdx[12]) reservedH += kStripStrideH;
    if (byIdx[13]) reservedH += kStripStrideH;
    if (byIdx[14]) reservedH += kStripStrideH;
    const float gap = 4.0f;
    const int   kCols = 6;
    float gridH = avail.y - reservedH;
    if (gridH < 100.0f) gridH = 100.0f;
    float rowH = (gridH - gap) * 0.5f;
    float colW = (avail.x - gap * (float)(kCols - 1)) / (float)kCols;

    if (isReplanTick) {
        ImGui::PushStyleColor(ImGuiCol_ChildBg,
                              IM_COL32(48, 48, 52, 255));
    }
    for (int row = 0; row < 2; row++) {
        for (int col = 0; col < kCols; col++) {
            if (col > 0) ImGui::SameLine(0.0f, gap);
            /* Section for this cell: pools 1..10 fill columns 0..4; the sixth
             * column is empty on top and BUILDER (15) underneath, beside the
             * winners pool. */
            int sidx;
            if (col < 5)        sidx = row * 5 + col + 1;
            else if (row == 1)  sidx = 15;
            else                sidx = 0;
            char cellId[24];
            SDL_snprintf(cellId, sizeof(cellId), "##cell%d%d", row, col);
            ImGui::BeginChild(cellId, ImVec2(colW, rowH), true);
            if (sidx > 0 && byIdx[sidx]) {
                renderSection(st, byIdx[sidx]);
            } else if (sidx == 15) {
                ImGui::TextColored(poolColorFor(sidx), "BUILDER (no data)");
            } else if (sidx > 0) {
                ImGui::TextColored(poolColorFor(sidx),
                                   "%d. (no data)", sidx);
            }
            /* sidx == 0: the empty top half of the BUILDER column. */
            ImGui::EndChild();
        }
    }
    if (isReplanTick) ImGui::PopStyleColor();

    /* Full-width strips below the grid, in brain-emission order:
     *   11 def_build      12 wait_for_lgm   13 kill_lgm   14 take_cover
     * 13 and 14 are injected straight into pool_cache by the brain (no
     * eval_queue / goal_competition path) so they come and go with a visible
     * hostile LGM / a live cover scan. BUILDER (15) is not a strip any more:
     * it is the bottom cell of the sixth grid column above, beside the
     * winners pool (three header lines then its candidate rows, in a cell as
     * tall as any pool's). */
    struct { int idx; const char *id; float h; } kStrips[] = {
        { 11, "##cell11", kStripH },
        { 12, "##cell12", kStripH },
        { 13, "##cell13", kStripH },
        { 14, "##cell14", kStripH },
    };
    for (size_t i = 0; i < sizeof(kStrips) / sizeof(kStrips[0]); i++) {
        Section *sec = byIdx[kStrips[i].idx];
        if (!sec) continue;
        ImVec2 av = ImGui::GetContentRegionAvail();
        ImGui::BeginChild(kStrips[i].id, ImVec2(av.x, kStrips[i].h), true);
        renderSection(st, sec);
        ImGui::EndChild();
    }

    /* Click on empty space deselects. */
    if (ImGui::IsMouseClicked(0) && !st.anyRowClickedThisFrame) {
        st.selectedSection = -1;
        st.selectedRowId   = -1;
        st.copyBuf[0]      = '\0';
    }
    st.anyRowClickedThisFrame = false;

    /* Ctrl+C copies the selected row to the system clipboard. */
    if (st.selectedRowId >= 0 && st.copyBuf[0] &&
            ImGui::GetIO().KeyCtrl &&
            ImGui::IsKeyPressed(ImGuiKey_C, false)) {
        ImGui::SetClipboardText(st.copyBuf);
    }

    /* Refresh the detail window from this frame's freshly-parsed
     * sections so playback scrubbing keeps it in sync. Match by
     * (sectionIdx, rowId). If the row isn't present this frame
     * (e.g. the candidate fell out of the queue), leave sDetail
     * as-is so the user keeps the last-known state. */
    if (st.detail.open && !st.detail.justOpened) {
        for (int si = 0; si < nSections; si++) {
            if (sections[si].idx != st.detail.sectionIdx) continue;
            for (int ri = 0; ri < sections[si].nrows; ri++) {
                Row *r = &sections[si].rows[ri];
                if (r->id != st.detail.rowId) continue;
                st.detail.srcPool  = r->src_pool;
                st.detail.mx       = r->mx;
                st.detail.my       = r->my;
                st.detail.cost     = r->cost;
                st.detail.weighted = r->weighted;
                st.detail.winner   = r->winner;
                SDL_strlcpy(st.detail.formula, r->formula, sizeof(st.detail.formula));
                break;
            }
            break;
        }
    }

    int winW = (int)ImGui::GetWindowWidth();
    int winH = (int)ImGui::GetWindowHeight();
    renderDetailPopup(st, winW, winH, tickIn, followBot);

    /* Winners formula legend window. */
    if (st.showLegend) {
        ImGui::SetNextWindowSize(ImVec2(360, 400), ImGuiCond_FirstUseEver);
        ImGui::SetNextWindowPos(
            ImVec2((float)winW * 0.5f - 180, (float)winH * 0.5f - 200),
            ImGuiCond_FirstUseEver);
        if (ImGui::Begin("Winners Formula Legend###winnersLegend",
                         &st.showLegend,
                         ImGuiWindowFlags_NoCollapse)) {
            /* Row markers (the symbol left of each row's #id). */
            ImGui::TextColored(ImVec4(1,1,0.5f,1), "Row markers (left of #id)");
            ImGui::Separator();
            if (ImGui::BeginTable("##legmark", 2,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Mark",    ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(0.4f,1.0f,0.4f,1), ">");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("Active goal: the one the bot is executing now");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1,1,0.4f,1), "*");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextWrapped("Winner: lowest-cost candidate (in WINNERS, a "
                                   "pool's winning row). Shown only when it isn't "
                                   "also the active goal -- '>' takes precedence.");

                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                ImGui::TextColored(ImVec4(1.0f,0.9f,0.1f,1), "!");
                ImGui::TableSetColumnIndex(1);
                ImGui::TextUnformatted("Override: selection forced by an override rule");

                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(1,1,0.5f,1), "Second-line abbreviations");
            ImGui::Separator();
            if (ImGui::BeginTable("##leg", 2,
                    ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
                    ImGuiTableFlags_SizingFixedFit)) {
                ImGui::TableSetupColumn("Short",   ImGuiTableColumnFlags_WidthFixed, 60.0f);
                ImGui::TableSetupColumn("Meaning", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableHeadersRow();
                static const struct { const char *abbr; const char *desc; } kLegend[] = {
                    { "OP@w",  "Base cost in Opening phase  (w = phase weight)" },
                    { "EA@w",  "Base cost in Early phase" },
                    { "MD@w",  "Base cost in Middle phase" },
                    { "LT@w",  "Base cost in Late phase" },
                    { "EG@w",  "Base cost in Endgame phase" },
                    { "SW",    "Switch penalty (goal-type change)" },
                    { "CM",    "Commitment penalty (ticks on current goal)" },
                    { "HT",    "History/oscillation penalty" },
                    { "WS",    "Wsim forward-sim damage cost" },
                    { "IM",    "Imminent capture (cost floored)" },
                    { NULL, NULL },
                };
                for (int i = 0; kLegend[i].abbr; i++) {
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::TextColored(ImVec4(1,1,0.5f,1), "%s", kLegend[i].abbr);
                    ImGui::TableSetColumnIndex(1);
                    ImGui::TextUnformatted(kLegend[i].desc);
                }
                ImGui::EndTable();
            }
            ImGui::Spacing();
            ImGui::TextColored(ImVec4(0.6f,0.6f,0.6f,1),
                "Format:  base PHASE@w +penalties =total");
        }
        ImGui::End();
    }

    freeSections(sections, nSections);
    cJSON_Delete(root);
}

/* Self-register at static-init time. */
struct AutoRegister {
    AutoRegister() {
        panelTypeRegister("GoalHunter:pool_grid", &renderPoolGrid);
        /* The brain registers the panel ENTRY as type "pool_grid" (init.lua), so
         * also register the un-namespaced alias — matches tier_control.cpp's dual
         * registration. Without it, panelTypeFind("pool_grid") misses and the
         * panel falls back to the raw-text dump (shows JSON, not the grid). */
        panelTypeRegister("pool_grid", &renderPoolGrid);
    }
};
static AutoRegister _auto;

} /* namespace */
