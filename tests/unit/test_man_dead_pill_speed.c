/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The builder's speed on the square of a dead pillbox
 * (man_dead_pill_terrain_speed, mapGetManSpeed).
 *
 * In Mac Bolo the square of a pillbox reads as a pillbox, live or dead, and
 * the man's step on a pillbox square is the road step (moveman's table:
 * road, grass and pillbox all take the same step). So a builder sent to
 * repair a dead pillbox that stands on swamp, crater, rubble or forest is
 * not slowed by that terrain, and a dead pillbox does not stop him.
 *
 *   man_dead_pill_speed_road — with the rule at 0, the classic table, the
 *     square of a dead pillbox walks at man_speed_road on every terrain it
 *     can stand on, water included; a live pillbox still reads 0; the
 *     tank's speed on the same square is not changed.
 *
 *   man_dead_pill_speed_terrain_rule — with the rule at 1, the WinBolo 2.1
 *     way, the square walks at the speed of the terrain under the pillbox.
 *
 *   man_dead_pill_speed_follows_road_rule — the speed is the road rule's,
 *     not a constant: moving man_speed_road moves it.
 *
 *   man_dead_pill_walk_ticks — a builder sent to repair a dead pillbox on
 *     swamp reaches it in the same number of ticks as one on road with the
 *     rule at 0, and in more ticks with the rule at 1.
 *
 *   man_dead_pill_walk_across_water — a builder sent to a square beyond
 *     a dead pillbox on river crosses it with the rule at 0, and is turned
 *     back by it with the rule at 1.
 *
 *   man_dead_pill_brain_walk — the bots' builder walk sim walks a square
 *     the brain marks as a dead pillbox at road speed, on swamp and on
 *     river, and a wall mark is not replaced by a road mark.
 */

#include <stddef.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->sim */
#include "game_sim.h"              /* GameSim: mp/bs/pb, tanks[], lgmen[] */
#include "bolo_map.h"              /* mapGetManSpeed, mapGetSpeed, mapSetPos */
#include "pillbox.h"               /* pillsGetPill, pillsSetPill */
#include "bases.h"                 /* basesExistPos */
#include "lgm.h"                   /* lgmAddRequest, lgmUpdate, struct lgmObj */
#include "tank.h"                  /* tankSetWorld, tankSetTrees */
#include "sim_rules.h"             /* SimRules */
#include "brain.h"                 /* BROAD, BSWAMP, BRIVER */
#include "brain_pathfinder.h"      /* the bots' builder walk sim */
#include "test_harness.h"

#define MDP_SLOT 0
#define MDP_PILL 1

/* The square the tests use, and the square the tank waits on for the walk.
 * Both inside the minable area, well away from the edge. */
#define MDP_X      40
#define MDP_Y      40
#define MDP_TANK_X 36

/* Paint every square from (x0,y) to (x1,y), and the rows either side, with
 * one terrain, so the builder's whole walk is on ground this file chose. */
static void mdpPaintRow(ServerSim *sim, BYTE terrain, int x0, int x1,
                        int y) {
    int x, dy;
    for (dy = -1; dy <= 1; dy++) {
        for (x = x0; x <= x1; x++) {
            mapSetPos(&sim->sim, &sim->sim.mp, (BYTE) x, (BYTE) (y + dy),
                      terrain, FALSE, FALSE);
        }
    }
}

/* Move pillbox MDP_PILL onto (x,y) with the armour given, owned by the
 * test's player. Returns 0 when the pill is there with that armour. */
static int mdpPlacePill(ServerSim *sim, BYTE x, BYTE y, BYTE armour) {
    GameSim *gs = &sim->sim;
    pillbox p;

    UT_ASSERT_MSG(pillsGetNumPills(&gs->pb) >= MDP_PILL,
                  "the map carries no pillbox to move");
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, MDP_PILL);
    p.x = x;
    p.y = y;
    p.armour = armour;
    p.inTank = FALSE;
    p.owner = MDP_SLOT;
    pillsSetPill(gs, &gs->pb, &p, MDP_PILL);

    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, MDP_PILL);
    UT_ASSERT_MSG(p.x == x && p.y == y && p.armour == armour,
                  "pill %d is at (%u,%u) with armour %u, not at (%u,%u) "
                  "with armour %u",
                  MDP_PILL, (unsigned) p.x, (unsigned) p.y,
                  (unsigned) p.armour, (unsigned) x, (unsigned) y,
                  (unsigned) armour);
    UT_ASSERT_MSG(pillsExistPos(&gs->pb, x, y) == TRUE,
                  "pill %d is not found at its own square", MDP_PILL);
    UT_ASSERT_MSG(pillsDeadPos(&gs->pb, x, y) == (armour == 0),
                  "pill %d with armour %u reads %s", MDP_PILL,
                  (unsigned) armour, armour == 0 ? "alive" : "dead");
    return 0;
}

static BYTE mdpManSpeed(ServerSim *sim, BYTE x, BYTE y) {
    return mapGetManSpeed(&sim->sim, &sim->sim.mp, &sim->sim.pb,
                          &sim->sim.bs, x, y, MDP_SLOT);
}

static BYTE mdpTankSpeed(ServerSim *sim, BYTE x, BYTE y) {
    return mapGetSpeed(&sim->sim, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs,
                       x, y, FALSE, MDP_SLOT);
}

/* The terrain a pillbox can be left standing on, with the rule that gives
 * a builder his speed there and the rule that gives a tank its speed. */
typedef struct {
    BYTE        terrain;
    const char *name;
    size_t      manRule;   /* offsetof the man_speed_* field */
    size_t      tankRule;  /* offsetof the speed_* field */
} MdpTerrain;

static const MdpTerrain mdpTerrains[] = {
    { GRASS,    "grass",    offsetof(SimRules, man_speed_grass),
                            offsetof(SimRules, speed_grass) },
    { ROAD,     "road",     offsetof(SimRules, man_speed_road),
                            offsetof(SimRules, speed_road) },
    { FOREST,   "forest",   offsetof(SimRules, man_speed_forest),
                            offsetof(SimRules, speed_forest) },
    { SWAMP,    "swamp",    offsetof(SimRules, man_speed_swamp),
                            offsetof(SimRules, speed_swamp) },
    { CRATER,   "crater",   offsetof(SimRules, man_speed_crater),
                            offsetof(SimRules, speed_crater) },
    { RUBBLE,   "rubble",   offsetof(SimRules, man_speed_rubble),
                            offsetof(SimRules, speed_rubble) },
    { RIVER,    "river",    offsetof(SimRules, man_speed_river),
                            offsetof(SimRules, speed_river) },
    { DEEP_SEA, "deep sea", offsetof(SimRules, man_speed_deep_sea),
                            offsetof(SimRules, speed_deep_sea) },
};
#define MDP_TERRAIN_COUNT \
    ((int) (sizeof(mdpTerrains) / sizeof(mdpTerrains[0])))

static int32_t mdpRule(const SimRules *r, size_t off) {
    return *(const int32_t *) ((const char *) r + off);
}

/* A fresh sim with the test square clear of bases, and pill MDP_PILL off
 * every square near it but its own. */
static ServerSim *mdpMakeSim(void) {
    ServerSim *sim = ut_make_running_sim("DeadPill");
    int dx, dy;
    if (sim == NULL) {
        return NULL;
    }
    for (dy = -2; dy <= 2; dy++) {
        for (dx = MDP_TANK_X - MDP_X - 2; dx <= 2; dx++) {
            if (basesExistPos(&sim->sim.bs, (BYTE) (MDP_X + dx),
                              (BYTE) (MDP_Y + dy)) == TRUE) {
                fprintf(stderr, "  a base stands at (%d,%d), inside the "
                        "test's patch\n", MDP_X + dx, MDP_Y + dy);
                serverSimDestroy(sim);
                return NULL;
            }
        }
    }
    return sim;
}

/* Read every terrain three ways: bare, with a dead pillbox, with a live
 * one. rule is the man_dead_pill_terrain_speed value under test. */
static int mdpCheckTable(ServerSim *sim, int rule) {
    const SimRules *r = &sim->sim.rules;
    int i;

    sim->sim.rules.man_dead_pill_terrain_speed = rule;
    for (i = 0; i < MDP_TERRAIN_COUNT; i++) {
        const MdpTerrain *t = &mdpTerrains[i];
        BYTE bare, dead, live, tankBare, tankDead;
        BYTE wantDead;

        /* Bare: the terrain's own speed, which is what the pillbox stands
           on. The pill is moved well away first. */
        UT_ASSERT(mdpPlacePill(sim, MDP_X + 8, MDP_Y + 8,
                               (BYTE) r->pill_max_armour) == 0);
        mdpPaintRow(sim, t->terrain, MDP_X - 1, MDP_X + 1, MDP_Y);
        bare = mdpManSpeed(sim, MDP_X, MDP_Y);
        tankBare = mdpTankSpeed(sim, MDP_X, MDP_Y);
        UT_ASSERT_MSG(bare == (BYTE) mdpRule(r, t->manRule),
                      "bare %s walks at %u, not its rule's %ld", t->name,
                      (unsigned) bare, (long) mdpRule(r, t->manRule));

        /* Dead. */
        UT_ASSERT(mdpPlacePill(sim, MDP_X, MDP_Y, 0) == 0);
        dead = mdpManSpeed(sim, MDP_X, MDP_Y);
        tankDead = mdpTankSpeed(sim, MDP_X, MDP_Y);
        wantDead = (rule == 0) ? (BYTE) r->man_speed_road : bare;
        UT_ASSERT_MSG(dead == wantDead,
                      "with man_dead_pill_terrain_speed %d a dead pillbox on "
                      "%s walks at %u, not %u", rule, t->name,
                      (unsigned) dead, (unsigned) wantDead);
        /* The tank is not part of this rule. */
        UT_ASSERT_MSG(tankDead == tankBare,
                      "with man_dead_pill_terrain_speed %d a tank on a dead "
                      "pillbox on %s drives at %u, not the bare %u", rule,
                      t->name, (unsigned) tankDead, (unsigned) tankBare);
        UT_ASSERT_MSG(tankDead == (BYTE) mdpRule(r, t->tankRule),
                      "a tank on a dead pillbox on %s drives at %u, not its "
                      "rule's %ld", t->name, (unsigned) tankDead,
                      (long) mdpRule(r, t->tankRule));

        /* Live: still a square he cannot walk onto, under either value. */
        UT_ASSERT(mdpPlacePill(sim, MDP_X, MDP_Y,
                               (BYTE) r->pill_max_armour) == 0);
        live = mdpManSpeed(sim, MDP_X, MDP_Y);
        UT_ASSERT_MSG(live == MAP_MANSPEED_TPILLBOX,
                      "with man_dead_pill_terrain_speed %d a live pillbox on "
                      "%s walks at %u, not %d", rule, t->name,
                      (unsigned) live, MAP_MANSPEED_TPILLBOX);
    }
    return 0;
}

int run_man_dead_pill_speed_road(void) {
    ServerSim *sim = mdpMakeSim();
    int rc;
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(sim->sim.rules.man_dead_pill_terrain_speed == 0,
                  "a new round has man_dead_pill_terrain_speed %d, not 0",
                  (int) sim->sim.rules.man_dead_pill_terrain_speed);
    rc = mdpCheckTable(sim, 0);
    serverSimDestroy(sim);
    return rc;
}

int run_man_dead_pill_speed_terrain_rule(void) {
    ServerSim *sim = mdpMakeSim();
    int rc;
    UT_ASSERT(sim != NULL);
    rc = mdpCheckTable(sim, 1);
    serverSimDestroy(sim);
    return rc;
}

int run_man_dead_pill_speed_follows_road_rule(void) {
    ServerSim *sim = mdpMakeSim();
    BYTE got;
    int rc = 0;
    UT_ASSERT(sim != NULL);

    /* 11 is no terrain's classic speed, so a match can only be the rule. */
    sim->sim.rules.man_speed_road = 11;
    mdpPaintRow(sim, SWAMP, MDP_X - 1, MDP_X + 1, MDP_Y);
    if (mdpPlacePill(sim, MDP_X, MDP_Y, 0) != 0) {
        rc = 1;
    } else {
        got = mdpManSpeed(sim, MDP_X, MDP_Y);
        if (got != 11) {
            fprintf(stderr, "  FAIL: a dead pillbox on swamp walks at %u with "
                    "man_speed_road 11\n", (unsigned) got);
            rc = 1;
        }
    }
    serverSimDestroy(sim);
    return rc;
}

/* The most ticks a walk is given, well over the longest walk here. */
#define MDP_WALK_LIMIT 3000

/* Send the builder from a tank on road at (MDP_TANK_X, MDP_Y) to the square
 * (destX, MDP_Y), across a dead pillbox at (MDP_X, MDP_Y) standing on
 * `under`, and count the ticks until he arrives. With destX at MDP_X the
 * order is to repair the pillbox; beyond it, the order is a road on grass.
 * The other squares are road. *ticks is -1 when he never arrives. */
static int mdpWalkTicks(int rule, BYTE under, BYTE destX, int *ticks) {
    ServerSim *sim = mdpMakeSim();
    GameSim *gs;
    lgm *man;
    int t;

    *ticks = -1;
    UT_ASSERT(sim != NULL);
    gs = &sim->sim;
    UT_ASSERT_MSG(gs->tanks[MDP_SLOT] != NULL && gs->lgmen[MDP_SLOT] != NULL,
                  "slot %d joined without a tank or a builder", MDP_SLOT);
    gs->rules.man_dead_pill_terrain_speed = rule;

    mdpPaintRow(sim, ROAD, MDP_TANK_X - 2, MDP_X + 3, MDP_Y);
    mdpPaintRow(sim, under, MDP_X, MDP_X, MDP_Y);
    if (destX != MDP_X) {
        mdpPaintRow(sim, GRASS, destX, destX, MDP_Y);
    }
    if (mdpPlacePill(sim, MDP_X, MDP_Y, 0) != 0) {
        serverSimDestroy(sim);
        return 1;
    }
    tankSetWorld(gs, &gs->tanks[MDP_SLOT],
                 (WORLD) ((MDP_TANK_X << TANK_SHIFT_MAPSIZE) +
                          MAP_SQUARE_MIDDLE),
                 (WORLD) ((MDP_Y << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE),
                 (TURNTYPE) 0, FALSE);
    tankSetTrees(gs, &gs->tanks[MDP_SLOT], (BYTE) TANK_FULL_TREES);

    man = &gs->lgmen[MDP_SLOT];
    lgmAddRequest(gs, man, &gs->tanks[MDP_SLOT], destX, MDP_Y,
                  destX == MDP_X ? LGM_PILL_REQUEST : LGM_ROAD_REQUEST);
    if ((*man)->inTank == TRUE || (*man)->state != LGM_STATE_GOING) {
        fprintf(stderr, "  FAIL: the order to (%u,%u) across %u never sent "
                "the builder out (state %u)\n", (unsigned) destX,
                (unsigned) MDP_Y, (unsigned) under,
                (unsigned) (*man)->state);
        serverSimDestroy(sim);
        return 1;
    }

    for (t = 1; t <= MDP_WALK_LIMIT; t++) {
        lgmUpdate(gs, man, &gs->tanks[MDP_SLOT]);
        if ((*man)->state != LGM_STATE_GOING) {
            /* He leaves GOING when he reaches the square, or when he is
               stuck; only the first sets him building. */
            if ((*man)->waitTime > 0) {
                *ticks = t;
            }
            break;
        }
    }
    serverSimDestroy(sim);
    return 0;
}

int run_man_dead_pill_walk_ticks(void) {
    int onRoad0 = -1, onSwamp0 = -1, onSwamp1 = -1, onRoad1 = -1;

    UT_ASSERT(mdpWalkTicks(0, ROAD, MDP_X, &onRoad0) == 0);
    UT_ASSERT(mdpWalkTicks(0, SWAMP, MDP_X, &onSwamp0) == 0);
    UT_ASSERT(mdpWalkTicks(1, ROAD, MDP_X, &onRoad1) == 0);
    UT_ASSERT(mdpWalkTicks(1, SWAMP, MDP_X, &onSwamp1) == 0);

    UT_ASSERT_MSG(onRoad0 > 0 && onSwamp0 > 0 && onRoad1 > 0 && onSwamp1 > 0,
                  "a builder never reached the dead pillbox: road %d, "
                  "swamp %d (rule 0); road %d, swamp %d (rule 1)",
                  onRoad0, onSwamp0, onRoad1, onSwamp1);
    /* Rule 0: the swamp under the pillbox does not slow him. */
    UT_ASSERT_MSG(onSwamp0 == onRoad0,
                  "with man_dead_pill_terrain_speed 0 he reached a dead "
                  "pillbox on swamp in %d ticks and one on road in %d",
                  onSwamp0, onRoad0);
    /* A pillbox on road is the same walk under either value. */
    UT_ASSERT_MSG(onRoad1 == onRoad0,
                  "a dead pillbox on road took %d ticks with the rule at 1 "
                  "and %d at 0", onRoad1, onRoad0);
    /* Rule 1: the last part of the walk is at swamp speed. */
    UT_ASSERT_MSG(onSwamp1 > onSwamp0,
                  "with man_dead_pill_terrain_speed 1 he reached a dead "
                  "pillbox on swamp in %d ticks, no slower than the %d with "
                  "the rule at 0", onSwamp1, onSwamp0);
    return 0;
}

int run_man_dead_pill_walk_across_water(void) {
    int onRoad0 = -1, onRiver0 = -1, onRiver1 = -1;

    /* The same walk with road where the river is, for the tick count. */
    UT_ASSERT(mdpWalkTicks(0, ROAD, MDP_X + 2, &onRoad0) == 0);
    UT_ASSERT(mdpWalkTicks(0, RIVER, MDP_X + 2, &onRiver0) == 0);
    UT_ASSERT(mdpWalkTicks(1, RIVER, MDP_X + 2, &onRiver1) == 0);

    UT_ASSERT_MSG(onRoad0 > 0, "a builder never reached (%d,%d) along a "
                  "road with a dead pillbox on it", MDP_X + 2, MDP_Y);
    /* Rule 0: the dead pillbox on river is a road square on his way. */
    UT_ASSERT_MSG(onRiver0 == onRoad0,
                  "with man_dead_pill_terrain_speed 0 he crossed a dead "
                  "pillbox on river in %d ticks, one on road in %d",
                  onRiver0, onRoad0);
    /* Rule 1: the river under it stops him, as it did in WinBolo 2.1. */
    UT_ASSERT_MSG(onRiver1 == -1,
                  "with man_dead_pill_terrain_speed 1 he crossed a dead "
                  "pillbox on river in %d ticks", onRiver1);
    return 0;
}

/* ---------------------------------------------------------------------- */
/* The bots' builder walk sim (brain_pathfinder.c lgmGetBrainManSpeed).    */
/* ---------------------------------------------------------------------- */

/* The brain map is terrain only, so the brain stamps each dead pillbox it
 * sees as a road mark (perception.lua, cpf.set_lgm_blocked). One walk from
 * (36,40) to (44,40) on road, through the square (40,40), which carries
 * `under` and, when `mark` is not 0, that overlay mark. The walk's target
 * square is (44,40), so (40,40) is not the blessed square. Returns the
 * ticks, or -1 when he cannot get through. */
static int mdpBrainWalk(BrainPathfinder *pf, BYTE *map, BYTE under,
                        int mark) {
    memset(map, BROAD, (size_t) MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
    map[MDP_Y * MAP_ARRAY_SIZE + MDP_X] = under;
    brainPathfinderSetMap(pf, map);
    brainPathfinderClearLgmBlock(pf);
    if (mark == BRAIN_PF_LGM_ROAD) {
        brainPathfinderSetLgmRoad(pf, MDP_X, MDP_Y);
    } else if (mark == BRAIN_PF_LGM_BLOCKED) {
        brainPathfinderSetLgmBlock(pf, MDP_X, MDP_Y);
        /* A road mark after a wall leaves the wall. */
        brainPathfinderSetLgmRoad(pf, MDP_X, MDP_Y);
    }
    return brainPathfinderLgmTravelTicksMap(pf, MDP_TANK_X, MDP_Y,
                                            MDP_X + 4, MDP_Y,
                                            MDP_X + 4, MDP_Y, 2000, 200);
}

int run_man_dead_pill_brain_walk(void) {
    BrainPathfinder *pf = brainPathfinderCreate();
    BYTE *map = (BYTE *) malloc((size_t) MAP_ARRAY_SIZE * MAP_ARRAY_SIZE);
    int road, swamp, swampMarked, river, riverMarked, wall;

    UT_ASSERT(pf != NULL && map != NULL);

    road        = mdpBrainWalk(pf, map, BROAD, 0);
    swamp       = mdpBrainWalk(pf, map, BSWAMP, 0);
    swampMarked = mdpBrainWalk(pf, map, BSWAMP, BRAIN_PF_LGM_ROAD);
    river       = mdpBrainWalk(pf, map, BRIVER, 0);
    riverMarked = mdpBrainWalk(pf, map, BRIVER, BRAIN_PF_LGM_ROAD);
    wall        = mdpBrainWalk(pf, map, BROAD, BRAIN_PF_LGM_BLOCKED);

    brainPathfinderDestroy(pf);
    free(map);

    UT_ASSERT_MSG(road > 0, "the all-road walk took %d ticks", road);
    /* Bare swamp, no pillbox: slower than road. */
    UT_ASSERT_MSG(swamp > road,
                  "a square of swamp took %d ticks, road took %d",
                  swamp, road);
    /* A dead pillbox on swamp: as fast as road, as the engine walks it. */
    UT_ASSERT_MSG(swampMarked == road,
                  "a dead pillbox on swamp took %d ticks, road took %d",
                  swampMarked, road);
    /* Bare river stops him; a dead pillbox on river does not. */
    UT_ASSERT_MSG(river == -1, "a square of river took %d ticks", river);
    UT_ASSERT_MSG(riverMarked == road,
                  "a dead pillbox on river took %d ticks, road took %d",
                  riverMarked, road);
    /* A wall mark is not replaced by a road mark. */
    UT_ASSERT_MSG(wall == -1, "a walled square took %d ticks", wall);
    return 0;
}
