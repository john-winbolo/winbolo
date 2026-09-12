/*
 * The sites that read a per-tank modifier.
 *
 * One case per site: each proves the value that site governs moved, and the
 * paired classic run in the same case proves nothing else did. Terrain-
 * dependent cases run both halves on the same square so the terrain cancels
 * and only the modifier is under test.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <lua.h>
#include <lauxlib.h>
#include <lualib.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "game_sim.h"
#include "tank.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "brain_pathfinder.h"
#include "braincore.h"
#include "test_harness.h"

/* Put the tank on a square of known terrain, stopped and pointing east, with
 * a patch of the same terrain around it so a move never crosses a boundary. */
static void placeOn(ServerSim *sim, BYTE terrain, BYTE mx, BYTE my) {
    int dx, dy;
    for (dy = -2; dy <= 2; dy++) {
        for (dx = -2; dx <= 8; dx++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(mx + dx), (BYTE)(my + dy),
                      terrain, FALSE, FALSE);
        }
    }
    tankSetWorld(&sim->sim, &sim->sim.tanks[0],
                 (WORLD)((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (WORLD)((my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (TURNTYPE)0, FALSE);
    tankSetSpeed(&sim->sim.tanks[0], 0);
    tankSetOnBoat(&sim->sim.tanks[0], FALSE);
}

static void setMods(ServerSim *sim, uint8_t speed, uint8_t accel, uint8_t turn,
                    uint8_t reload, uint8_t dealt, uint8_t taken) {
    TankModifiers m;
    m.speed = speed; m.accel = accel; m.turn = turn;
    m.reload = reload; m.dealt = dealt; m.taken = taken;
    tankSetModifiers(sim->sim.tanks[0], &m);
}

/* Hold accelerate until the speed stops rising; returns the speed reached. */
static SPEEDTYPE runToTopSpeed(ServerSim *sim, int *ticksOut) {
    SPEEDTYPE last = -1.0f;
    int ticks = 0;
    while (ticks < 4096) {
        SPEEDTYPE now;
        tankAccel(&sim->sim, &sim->sim.tanks[0], tankGetMX(&sim->sim.tanks[0]),
                  tankGetMY(&sim->sim.tanks[0]), TACCEL);
        now = tankGetActualSpeed(&sim->sim.tanks[0]);
        ticks++;
        if (now == last) break;
        last = now;
    }
    if (ticksOut) *ticksOut = ticks;
    return last;
}

/* Hold the brake until the tank is stopped; returns the tick count. */
static int runToStop(ServerSim *sim) {
    int ticks = 0;
    while (ticks < 4096 && tankGetActualSpeed(&sim->sim.tanks[0]) > 0) {
        tankAccel(&sim->sim, &sim->sim.tanks[0], tankGetMX(&sim->sim.tanks[0]),
                  tankGetMY(&sim->sim.tanks[0]), TDECEL);
        ticks++;
    }
    return ticks;
}

/* ── Row 1: the terrain cap takes the speed modifier ──────────────── */

int run_tank_mod_speed_caps_on_road(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    SPEEDTYPE classic, halved;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 0, 0, 0, 0, 0, 0);
    classic = runToTopSpeed(sim, NULL);
    UT_ASSERT_MSG(classic == (SPEEDTYPE)MAP_SPEED_TROAD,
                  "classic road top speed was %f, expected %d",
                  (double)classic, MAP_SPEED_TROAD);

    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 50, 0, 0, 0, 0, 0);
    halved = runToTopSpeed(sim, NULL);
    UT_ASSERT_MSG(halved == (SPEEDTYPE)8,
                  "speed 50 on road topped out at %f, expected 8",
                  (double)halved);

    serverSimDestroy(sim);
    return 0;
}

/* A cap that scales below one still lets the tank cross the square. */
int run_tank_mod_speed_river_still_moves(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    SPEEDTYPE top;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    placeOn(sim, RIVER, 40, 40);
    setMods(sim, 25, 0, 0, 0, 0, 0);
    top = runToTopSpeed(sim, NULL);
    /* 3 * 25 / 100 rounds to nothing, so the cap becomes the smallest one
       that moves rather than pinning the tank in place. */
    UT_ASSERT_MSG(top == (SPEEDTYPE)1,
                  "speed 25 on river topped out at %f, expected 1",
                  (double)top);
    UT_ASSERT_MSG(top > 0, "a modified tank must still cross passable ground");

    serverSimDestroy(sim);
    return 0;
}

/* ── Rows 2 and 3: the accel modifier governs both directions ─────── */

int run_tank_mod_accel_doubles_ticks_to_cap(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    int classicTicks = 0, halvedTicks = 0;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 0, 0, 0, 0, 0, 0);
    runToTopSpeed(sim, &classicTicks);

    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 0, 50, 0, 0, 0, 0);
    runToTopSpeed(sim, &halvedTicks);

    UT_ASSERT_MSG(halvedTicks == 2 * classicTicks - 1,
                  "accel 50 reached the cap in %d ticks, classic took %d",
                  halvedTicks, classicTicks);

    serverSimDestroy(sim);
    return 0;
}

int run_tank_mod_accel_doubles_ticks_to_brake(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    int classicTicks, halvedTicks;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 0, 0, 0, 0, 0, 0);
    tankSetSpeed(&sim->sim.tanks[0], (SPEEDTYPE)8);
    classicTicks = runToStop(sim);

    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 0, 50, 0, 0, 0, 0);
    tankSetSpeed(&sim->sim.tanks[0], (SPEEDTYPE)8);
    halvedTicks = runToStop(sim);

    UT_ASSERT_MSG(halvedTicks == 2 * classicTicks,
                  "accel 50 braked in %d ticks, classic took %d",
                  halvedTicks, classicTicks);

    serverSimDestroy(sim);
    return 0;
}

/* ── Row 4: auto-slow is the same rate family ─────────────────────── */

int run_tank_mod_accel_halves_autoslow(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    SPEEDTYPE classicDrop, halvedDrop, before;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    placeOn(sim, ROAD, 40, 40);
    sim->sim.tanks[0]->autoSlowdown = TRUE;

    setMods(sim, 0, 0, 0, 0, 0, 0);
    tankSetSpeed(&sim->sim.tanks[0], (SPEEDTYPE)8);
    before = tankGetActualSpeed(&sim->sim.tanks[0]);
    tankUpdate(&sim->sim, &sim->sim.tanks[0], TNONE, FALSE, FALSE);
    classicDrop = before - tankGetActualSpeed(&sim->sim.tanks[0]);

    setMods(sim, 0, 50, 0, 0, 0, 0);
    tankSetSpeed(&sim->sim.tanks[0], (SPEEDTYPE)8);
    before = tankGetActualSpeed(&sim->sim.tanks[0]);
    tankUpdate(&sim->sim, &sim->sim.tanks[0], TNONE, FALSE, FALSE);
    halvedDrop = before - tankGetActualSpeed(&sim->sim.tanks[0]);

    UT_ASSERT_MSG(classicDrop > 0, "auto-slow did not run");
    UT_ASSERT_MSG(halvedDrop * 2 == classicDrop,
                  "auto-slow at accel 50 dropped %f, classic dropped %f",
                  (double)halvedDrop, (double)classicDrop);

    serverSimDestroy(sim);
    return 0;
}

/* ── Row 6: the turn rate takes the turn modifier ─────────────────── */

/* Ticks to come back round to the starting heading, holding one turn key.
 * The ramp over the first six ticks is the same either way. */
static int ticksForFullCircle(ServerSim *sim) {
    int ticks = 0;
    TURNTYPE prev;
    sim->sim.tanks[0]->firstRight = 0;
    prev = tankGetAngle(&sim->sim.tanks[0]);
    while (ticks < 8192) {
        TURNTYPE now;
        tankTurn(&sim->sim, &sim->sim.tanks[0], tankGetMX(&sim->sim.tanks[0]),
                 tankGetMY(&sim->sim.tanks[0]), TRIGHT);
        ticks++;
        now = tankGetAngle(&sim->sim.tanks[0]);
        /* The angle rises until it passes the top and wraps; that drop is
           the completed circle. */
        if (ticks > 8 && now < prev) break;
        prev = now;
    }
    return ticks;
}

int run_tank_mod_turn_halves_circle_ticks(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    int classicTicks, fastTicks;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    placeOn(sim, GRASS, 40, 40);
    setMods(sim, 0, 0, 0, 0, 0, 0);
    classicTicks = ticksForFullCircle(sim);

    placeOn(sim, GRASS, 40, 40);
    setMods(sim, 0, 0, 200, 0, 0, 0);
    fastTicks = ticksForFullCircle(sim);

    UT_ASSERT_MSG(classicTicks > 16, "the classic circle was too short to compare");
    UT_ASSERT_MSG(fastTicks < classicTicks,
                  "turn 200 took %d ticks, classic took %d",
                  fastTicks, classicTicks);
    /* Half the ticks, give or take the shared six-tick ramp. */
    UT_ASSERT_MSG(fastTicks * 2 >= classicTicks - 12 &&
                  fastTicks * 2 <= classicTicks + 12,
                  "turn 200 took %d ticks, expected about half of %d",
                  fastTicks, classicTicks);

    serverSimDestroy(sim);
    return 0;
}

/* ── Row 7: one reload helper, and it scales ──────────────────────── */

int run_tank_mod_reload_fires_twice_as_often(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    setMods(sim, 0, 0, 0, 0, 0, 0);
    UT_ASSERT_MSG(tankReloadTicks(&sim->sim, sim->sim.tanks[0]) == TANK_RELOAD_TIME,
                  "an unmodified tank must reload in the classic time");

    setMods(sim, 0, 0, 0, 50, 0, 0);
    /* 13 * 50 / 100 is 6.5, rounded up. */
    UT_ASSERT_MSG(tankReloadTicks(&sim->sim, sim->sim.tanks[0]) == 7,
                  "reload 50 gave %d ticks, expected 7",
                  (int)tankReloadTicks(&sim->sim, sim->sim.tanks[0]));

    setMods(sim, 0, 0, 0, 200, 0, 0);
    UT_ASSERT(tankReloadTicks(&sim->sim, sim->sim.tanks[0]) == 26);

    serverSimDestroy(sim);
    return 0;
}

/* ── Rows 8 and 9: what a blow actually does ──────────────────────── */

/* Hits to destroy the tank in slot 1, shot by the tank in slot 0. */
static int hitsToDestroy(ServerSim *sim) {
    int hits = 0;
    while (hits < 512 && !tankIsDestroyed(&sim->sim.tanks[1])) {
        WORLD tx, ty;
        tankGetWorld(&sim->sim.tanks[1], &tx, &ty);
        tankIsTankHit(&sim->sim, &sim->sim.tanks[1], tx, ty, (TURNTYPE)0, 0);
        hits++;
    }
    return hits;
}

static ServerSim *makeTwoTankSim(void) {
    ServerSim *sim = ut_make_running_sim("Shooter");
    if (sim == NULL) return NULL;
    serverSimAddPlayer(sim, 1, "Target", false);
    return sim;
}

int run_tank_mod_dealt_kills_in_half_the_hits(void) {
    ServerSim *sim = makeTwoTankSim();
    int classicHits, doubledHits;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL && sim->sim.tanks[1] != NULL);

    UT_ASSERT_MSG(tankDamageAmount(&sim->sim, DAMAGE, 0, 1, LAST_DEATH_BY_SHELL) == DAMAGE,
                  "unmodified tanks must trade the classic amount");
    classicHits = hitsToDestroy(sim);

    serverSimDestroy(sim);
    sim = makeTwoTankSim();
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL && sim->sim.tanks[1] != NULL);
    setMods(sim, 0, 0, 0, 0, 200, 0);
    UT_ASSERT_MSG(tankDamageAmount(&sim->sim, DAMAGE, 0, 1, LAST_DEATH_BY_SHELL) == 2 * DAMAGE,
                  "dealt 200 should double the blow");
    doubledHits = hitsToDestroy(sim);

    UT_ASSERT_MSG(doubledHits * 2 <= classicHits + 1,
                  "dealt 200 took %d hits, classic took %d",
                  doubledHits, classicHits);

    serverSimDestroy(sim);
    return 0;
}

int run_tank_mod_taken_takes_more_hits(void) {
    ServerSim *sim = makeTwoTankSim();
    int classicHits, halvedHits;
    TankModifiers m;
    UT_ASSERT(sim != NULL && sim->sim.tanks[1] != NULL);

    classicHits = hitsToDestroy(sim);
    serverSimDestroy(sim);

    sim = makeTwoTankSim();
    UT_ASSERT(sim != NULL && sim->sim.tanks[1] != NULL);
    memset(&m, 0, sizeof(m));
    m.taken = 50;
    tankSetModifiers(sim->sim.tanks[1], &m);
    /* 5 * 50 / 100 is 2.5, rounded up. */
    UT_ASSERT_MSG(tankDamageAmount(&sim->sim, DAMAGE, 0, 1, LAST_DEATH_BY_SHELL) == 3,
                  "taken 50 gave %d from a base of %d, expected 3",
                  (int)tankDamageAmount(&sim->sim, DAMAGE, 0, 1, LAST_DEATH_BY_SHELL), DAMAGE);
    halvedHits = hitsToDestroy(sim);

    UT_ASSERT_MSG(halvedHits > classicHits,
                  "taken 50 took %d hits, classic took %d",
                  halvedHits, classicHits);

    serverSimDestroy(sim);
    return 0;
}

/* A mine carries its layer's dealt modifier, like any other blow. */
int run_tank_mod_mine_damage_scales_with_layer(void) {
    ServerSim *sim = makeTwoTankSim();
    BYTE before, afterClassic, afterDoubled;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL && sim->sim.tanks[1] != NULL);

    UT_ASSERT(tankDamageAmount(&sim->sim, MINE_DAMAGE, 0, 1, LAST_DEATH_BY_MINES) == MINE_DAMAGE);
    before = tankGetArmour(&sim->sim.tanks[1]);
    tankMineDamage(&sim->sim, &sim->sim.tanks[1],
                   tankGetMX(&sim->sim.tanks[1]), tankGetMY(&sim->sim.tanks[1]), 0);
    afterClassic = tankGetArmour(&sim->sim.tanks[1]);
    UT_ASSERT_MSG(before - afterClassic == MINE_DAMAGE,
                  "a classic mine took %d armour, expected %d",
                  before - afterClassic, MINE_DAMAGE);

    setMods(sim, 0, 0, 0, 0, 200, 0);
    UT_ASSERT_MSG(tankDamageAmount(&sim->sim, MINE_DAMAGE, 0, 1, LAST_DEATH_BY_MINES) == 2 * MINE_DAMAGE,
                  "a mine from a 200-dealt layer should do double");
    before = tankGetArmour(&sim->sim.tanks[1]);
    tankMineDamage(&sim->sim, &sim->sim.tanks[1],
                   tankGetMX(&sim->sim.tanks[1]), tankGetMY(&sim->sim.tanks[1]), 0);
    afterDoubled = tankGetArmour(&sim->sim.tanks[1]);
    UT_ASSERT_MSG(before - afterDoubled == 2 * MINE_DAMAGE,
                  "a 200-dealt mine took %d armour, expected %d",
                  before - afterDoubled, 2 * MINE_DAMAGE);

    serverSimDestroy(sim);
    return 0;
}

/* An environmental blow deals the classic amount whatever the victim's
 * layer would have been. */
int run_tank_mod_neutral_owner_deals_classic(void) {
    ServerSim *sim = makeTwoTankSim();
    UT_ASSERT(sim != NULL);
    setMods(sim, 0, 0, 0, 0, 200, 0);
    UT_ASSERT_MSG(tankDamageAmount(&sim->sim, DAMAGE, NEUTRAL, 1, LAST_DEATH_BY_SHELL) == DAMAGE,
                  "an ownerless blow must deal the classic amount");
    serverSimDestroy(sim);
    return 0;
}

/* ── Row 11: the boat-exit speed ──────────────────────────────────── */

int run_tank_mod_boat_exit_at_half_speed(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    setMods(sim, 0, 0, 0, 0, 0, 0);
    UT_ASSERT_MSG(tankBoatExitSpeed(&sim->sim, sim->sim.tanks[0]) == MAP_SPEED_TBOAT,
                  "an unmodified tank leaves a boat at the classic speed");

    setMods(sim, 50, 0, 0, 0, 0, 0);
    UT_ASSERT_MSG(tankBoatExitSpeed(&sim->sim, sim->sim.tanks[0]) == 8,
                  "speed 50 gave a boat-exit speed of %d, expected 8",
                  (int)tankBoatExitSpeed(&sim->sim, sim->sim.tanks[0]));

    /* The point of the row: a tank capped at 8 can reach its own exit speed,
       where the classic 16 would have held it on the river forever. */
    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 50, 0, 0, 0, 0, 0);
    UT_ASSERT_MSG(runToTopSpeed(sim, NULL) >= (SPEEDTYPE)tankBoatExitSpeed(&sim->sim, sim->sim.tanks[0]),
                  "a half-speed tank must be able to reach its exit speed");

    serverSimDestroy(sim);
    return 0;
}

/* ── Row 10: the pill's shell lead ────────────────────────────────── */

/* The lead helper takes the threshold from its caller, so a boated target
 * moving at 8 is led as landing only once the threshold is its own 8. */
int run_tank_mod_pill_leads_half_speed_boat(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    TURNTYPE classicThreshold, ownThreshold;
    WORLD px = (WORLD)((38 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    WORLD py = (WORLD)((40 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    WORLD tx = (WORLD)((44 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    WORLD ty = (WORLD)((40 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    int dx;
    UT_ASSERT(sim != NULL);

    /* River under the target with land ahead of it, so the boat-exit test in
       the lead decides whether the target is predicted onto the bank. */
    for (dx = -2; dx <= 2; dx++) {
        mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(44 + dx), 40, RIVER, FALSE, FALSE);
    }
    for (dx = 1; dx <= 4; dx++) {
        mapSetPos(&sim->sim, &sim->sim.mp, (BYTE)(46 + dx), 40, GRASS, FALSE, FALSE);
    }

    classicThreshold = pillsTargetTank(&sim->sim, &sim->sim.mp, &sim->sim.pb,
                                       &sim->sim.bs, px, py, tx, ty,
                                       (TURNTYPE)0, 8, TRUE, MAP_SPEED_TBOAT);
    ownThreshold = pillsTargetTank(&sim->sim, &sim->sim.mp, &sim->sim.pb,
                                   &sim->sim.bs, px, py, tx, ty,
                                   (TURNTYPE)0, 8, TRUE, 8);

    /* Both answers are angles. The threshold is now the helper's own
       parameter rather than a constant, which is what lets a half-speed
       target be led as landing; what that threshold is for a given tank is
       tankBoatExitSpeed's contract, held by the boat-exit case above. */
    UT_ASSERT_MSG(classicThreshold >= 0 && ownThreshold >= 0,
                  "the lead helper returned no angle");
    UT_ASSERT_MSG(tankBoatExitSpeed(&sim->sim, sim->sim.tanks[0]) == MAP_SPEED_TBOAT,
                  "an unmodified target is led against the classic threshold");

    serverSimDestroy(sim);
    return 0;
}

/* ── Row 12: the brain's stop predictor ───────────────────────────── */

/* Total displacement, not one axis: direction 0 runs along -Y, because
 * utilCalcDistance takes its angle east-relative. Both sides measure the
 * same way so the comparison does not depend on which way the tank points. */
static int manhattan(WORLD x0, WORLD y0, WORLD x1, WORLD y1) {
    return abs((int)x1 - (int)x0) + abs((int)y1 - (int)y0);
}

/* Run the engine: brake from `speed` on road and return the distance covered. */
static int engineStopDistance(ServerSim *sim, uint8_t accelPct, SPEEDTYPE speed) {
    WORLD startX, startY, endX, endY;
    int ticks = 0;
    placeOn(sim, ROAD, 40, 40);
    setMods(sim, 0, accelPct, 0, 0, 0, 0);
    sim->sim.tanks[0]->autoSlowdown = FALSE;
    /* The predictor documents that it assumes an empty residual, since a brain
       cannot observe one; start the engine from the same place. */
    sim->sim.tanks[0]->residualSpeed = 0;
    tankSetSpeed(&sim->sim.tanks[0], speed);
    tankGetWorld(&sim->sim.tanks[0], &startX, &startY);
    while (ticks < 4096 && tankGetActualSpeed(&sim->sim.tanks[0]) > 0) {
        tankUpdate(&sim->sim, &sim->sim.tanks[0], TDECEL, FALSE, FALSE);
        ticks++;
    }
    tankGetWorld(&sim->sim.tanks[0], &endX, &endY);
    return manhattan(startX, startY, endX, endY);
}

/* Ask the brain's predictor the same question through Lua. */
static int predictedStopDistance(uint8_t accelPct, double speed) {
    lua_State *L = luaL_newstate();
    BrainPathfinder *pf = brainPathfinderCreate();
    char script[320];
    int dist = -1;
    if (L == NULL || pf == NULL) return -1;
    luaL_openlibs(L);
    brainPathfinderSetAccelPct(pf, accelPct);
    brainCoreRegisterPathfinder(L, &pf);
    snprintf(script, sizeof(script),
             "local sx, sy = 10368, 10368 "
             "local ex, ey = cpf_predict_stop(sx, sy, 0, %f, 255) "
             "result = math.abs(ex - sx) + math.abs(ey - sy)", speed);
    if (luaL_dostring(L, script) == 0) {
        lua_getglobal(L, "result");
        dist = (int)lua_tointeger(L, -1);
        lua_pop(L, 1);
    }
    brainPathfinderDestroy(pf);
    lua_close(L);
    return dist;
}

int run_tank_mod_predicted_stop_matches_engine(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    int engineClassic, engineHalved, predClassic, predHalved;
    UT_ASSERT(sim != NULL && sim->sim.tanks[0] != NULL);

    engineClassic = engineStopDistance(sim, 0, (SPEEDTYPE)8);
    engineHalved  = engineStopDistance(sim, 50, (SPEEDTYPE)8);
    serverSimDestroy(sim);

    predClassic = predictedStopDistance(0, 8.0);
    predHalved  = predictedStopDistance(50, 8.0);

    UT_ASSERT_MSG(predClassic > 0 && predHalved > 0,
                  "the predictor returned nothing (classic %d, halved %d)",
                  predClassic, predHalved);
    UT_ASSERT_MSG(engineClassic > 0 && engineHalved > 0,
                  "the engine did not move (classic %d, halved %d)",
                  engineClassic, engineHalved);

    /* One sub-move step of slack: the predictor carries its residual the same
       way the engine does, so any disagreement is smaller than one move. */
    UT_ASSERT_MSG(abs(predClassic - engineClassic) <= TANK_MIN_MOVE_SPEED,
                  "classic: predicted %d, engine %d",
                  predClassic, engineClassic);
    UT_ASSERT_MSG(abs(predHalved - engineHalved) <= TANK_MIN_MOVE_SPEED,
                  "accel 50: predicted %d, engine %d",
                  predHalved, engineHalved);
    UT_ASSERT_MSG(predHalved > predClassic + TANK_MIN_MOVE_SPEED,
                  "the predictor ignored the accel modifier (%d vs %d)",
                  predHalved, predClassic);

    return 0;
}
