/*
 * The public reads a script makes of the sim.
 *
 * Each accessor is checked against the internal state it mirrors, and each
 * is checked on the two edges that are easy to get wrong: an empty slot and
 * an index outside the list. Both must return false and leave the caller's
 * out-struct exactly as it was, so every refusal here writes a byte pattern
 * into the struct first and reads it back afterwards.
 *
 * run_scenario_read_roster_slot     — the lobby's view of a seat
 * run_scenario_read_pill_info       — a pill by index
 * run_scenario_read_base_info       — a base by index
 * run_scenario_read_start_info      — a start by index
 * run_scenario_read_tank_info       — the widened per-slot tank snapshot
 * run_scenario_read_builder_info    — the builder's state, job and load
 * run_scenario_read_terrain_buffer  — the whole terrain array in one call
 * run_scenario_read_num_fielded     — how many seats are playing the round
 *
 * Runs against ut_make_running_sim and reads the ServerSim struct directly;
 * the unittests profile permits internal access.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers[], playerConnected[] */
#include "game_sim.h"              /* GameSim: mp/bs/pb/ss, tanks[], lgmen[] */
#include "bolo_map.h"              /* mapGetPos */
#include "starts.h"                /* startsConvertDir */
#include "lgm.h"                   /* LGM_* states and request codes */
#include "tank.h"                  /* tank stock and facing accessors */
#include "test_harness.h"

#define SR_SLOT 0
#define SR_FILL 0xAB

/* The byte every refusal check leaves behind, so an accessor that writes
   into a struct it should not touch is caught. */
static void sr_poison(void *p, size_t n) {
    memset(p, SR_FILL, n);
}

static bool sr_untouched(const void *p, size_t n) {
    const unsigned char *b = (const unsigned char *)p;
    size_t i;
    for (i = 0; i < n; i++) {
        if (b[i] != SR_FILL) return false;
    }
    return true;
}

/* ── Roster slot ─────────────────────────────────────────────────── */

int run_scenario_read_roster_slot(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    ServerSimRosterSlot slot;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    sim->lobbyPlayers[SR_SLOT].teamNumber = 3;
    sim->lobbyPlayers[SR_SLOT].ready      = true;
    sim->lobbyPlayers[SR_SLOT].isBot      = false;

    sr_poison(&slot, sizeof(slot));
    if (!serverSimGetRosterSlot(sim, SR_SLOT, &slot)) {
        UT_FAIL("roster read refused the connected seat");
    }
    UT_ASSERT(slot.connected);
    UT_ASSERT(!slot.is_bot);
    UT_ASSERT_MSG(slot.team == 3, "team %u", (unsigned)slot.team);
    UT_ASSERT_MSG(strcmp(slot.name, "Seat0") == 0, "name '%s'", slot.name);
    UT_ASSERT(slot.ready);
    UT_ASSERT(slot.fielded);
    /* ut_make_running_sim leaves slot 0 with a live tank. */
    UT_ASSERT(slot.alive);

    /* A bot seat reports as one. */
    serverSimAddPlayer(sim, 1, "Botty", false);
    sim->lobbyPlayers[1].isBot = true;
    if (!serverSimGetRosterSlot(sim, 1, &slot)) {
        UT_FAIL("roster read refused the bot seat");
    }
    UT_ASSERT(slot.is_bot);
    UT_ASSERT_MSG(strcmp(slot.name, "Botty") == 0, "name '%s'", slot.name);

    /* An empty seat and an index past the roster are both refused, and
       neither writes anything. */
    sr_poison(&slot, sizeof(slot));
    UT_ASSERT(!serverSimGetRosterSlot(sim, 2, &slot));
    UT_ASSERT(sr_untouched(&slot, sizeof(slot)));
    UT_ASSERT(!serverSimGetRosterSlot(sim, MAX_TANKS, &slot));
    UT_ASSERT(sr_untouched(&slot, sizeof(slot)));
    UT_ASSERT(!serverSimGetRosterSlot(sim, 255, &slot));
    UT_ASSERT(sr_untouched(&slot, sizeof(slot)));

    serverSimDestroy(sim);
    return 0;
}

/* ── Pill ────────────────────────────────────────────────────────── */

int run_scenario_read_pill_info(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    ServerSimPillInfo info;
    BYTE n;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    n = serverSimGetPillCount(sim);
    UT_ASSERT_MSG(n > 0, "map has no pills to read");

    /* Index 0 is below the 1-based list. */
    sr_poison(&info, sizeof(info));
    UT_ASSERT(!serverSimGetPillInfo(sim, 0, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));

    /* First and last live indices mirror the pill list. */
    {
        const pillbox *p = &sim->sim.pb->item[0];
        if (!serverSimGetPillInfo(sim, 1, &info)) UT_FAIL("pill 1 refused");
        UT_ASSERT(info.x == p->x && info.y == p->y);
        UT_ASSERT(info.owner == p->owner);
        UT_ASSERT(info.armour == p->armour);
        UT_ASSERT(info.speed == p->speed);
        UT_ASSERT(info.in_tank == p->inTank);
        UT_ASSERT(info.active);
    }
    {
        const pillbox *p = &sim->sim.pb->item[n - 1];
        if (!serverSimGetPillInfo(sim, n, &info)) UT_FAIL("pill %u refused",
                                                          (unsigned)n);
        UT_ASSERT(info.x == p->x && info.y == p->y);
        UT_ASSERT(info.owner == p->owner);
        UT_ASSERT(info.armour == p->armour);
        UT_ASSERT(info.speed == p->speed);
        UT_ASSERT(info.active);
    }

    /* A value the list has moved past is refused. */
    sr_poison(&info, sizeof(info));
    UT_ASSERT(!serverSimGetPillInfo(sim, (BYTE)(n + 1), &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));
    UT_ASSERT(!serverSimGetPillInfo(sim, 255, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));

    /* A carried pill reports in_tank, and the speed field tracks the list. */
    sim->sim.pb->item[0].inTank = TRUE;
    sim->sim.pb->item[0].speed  = 33;
    if (!serverSimGetPillInfo(sim, 1, &info)) UT_FAIL("pill 1 refused");
    UT_ASSERT(info.in_tank);
    UT_ASSERT_MSG(info.speed == 33, "speed %u", (unsigned)info.speed);

    serverSimDestroy(sim);
    return 0;
}

/* ── Base ────────────────────────────────────────────────────────── */

int run_scenario_read_base_info(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    ServerSimBaseInfo info;
    BYTE n;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    n = serverSimGetBaseCount(sim);
    UT_ASSERT_MSG(n > 0, "map has no bases to read");

    sr_poison(&info, sizeof(info));
    UT_ASSERT(!serverSimGetBaseInfo(sim, 0, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));

    {
        const base *b = &sim->sim.bs->item[0];
        if (!serverSimGetBaseInfo(sim, 1, &info)) UT_FAIL("base 1 refused");
        UT_ASSERT(info.x == b->x && info.y == b->y);
        UT_ASSERT(info.owner == b->owner);
        UT_ASSERT(info.armour == b->armour);
        UT_ASSERT(info.shells == b->shells);
        UT_ASSERT(info.mines == b->mines);
        UT_ASSERT(info.active);
    }
    {
        const base *b = &sim->sim.bs->item[n - 1];
        if (!serverSimGetBaseInfo(sim, n, &info)) UT_FAIL("base %u refused",
                                                          (unsigned)n);
        UT_ASSERT(info.x == b->x && info.y == b->y);
        UT_ASSERT(info.owner == b->owner);
        UT_ASSERT(info.armour == b->armour);
        UT_ASSERT(info.shells == b->shells);
        UT_ASSERT(info.mines == b->mines);
        UT_ASSERT(info.active);
    }

    sr_poison(&info, sizeof(info));
    UT_ASSERT(!serverSimGetBaseInfo(sim, (BYTE)(n + 1), &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));
    UT_ASSERT(!serverSimGetBaseInfo(sim, 255, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));

    /* One call carries what serverSimGetBase and serverSimGetBaseStats
       return between them; the two paths must agree. */
    {
        BYTE bx, by, bowner, bshells, bmines, barmour;
        UT_ASSERT(serverSimGetBase(sim, 1, &bx, &by, &bowner));
        UT_ASSERT(serverSimGetBaseStats(sim, 1, &bshells, &bmines, &barmour));
        UT_ASSERT(serverSimGetBaseInfo(sim, 1, &info));
        UT_ASSERT(info.x == bx && info.y == by && info.owner == bowner);
        UT_ASSERT(info.shells == bshells && info.mines == bmines);
        UT_ASSERT(info.armour == barmour);
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── Start ───────────────────────────────────────────────────────── */

int run_scenario_read_start_info(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    BYTE x = SR_FILL, y = SR_FILL, dir = SR_FILL;
    BYTE n;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    n = serverSimGetStartCount(sim);
    UT_ASSERT_MSG(n > 0, "map has no starts to read");

    UT_ASSERT(!serverSimGetStart(sim, 0, &x, &y, &dir));
    UT_ASSERT(x == SR_FILL && y == SR_FILL && dir == SR_FILL);

    {
        const start *st = &sim->sim.ss->item[0];
        UT_ASSERT(serverSimGetStart(sim, 1, &x, &y, &dir));
        UT_ASSERT(x == st->x && y == st->y);
        UT_ASSERT(dir == startsConvertDir((BYTE)((st->dir < 16) ? st->dir : 0)));
    }
    {
        const start *st = &sim->sim.ss->item[n - 1];
        UT_ASSERT(serverSimGetStart(sim, n, &x, &y, &dir));
        UT_ASSERT(x == st->x && y == st->y);
        UT_ASSERT(dir == startsConvertDir((BYTE)((st->dir < 16) ? st->dir : 0)));
    }

    x = y = dir = SR_FILL;
    UT_ASSERT(!serverSimGetStart(sim, (BYTE)(n + 1), &x, &y, &dir));
    UT_ASSERT(x == SR_FILL && y == SR_FILL && dir == SR_FILL);
    UT_ASSERT(!serverSimGetStart(sim, 255, &x, &y, &dir));
    UT_ASSERT(x == SR_FILL && y == SR_FILL && dir == SR_FILL);

    serverSimDestroy(sim);
    return 0;
}

/* ── Tank ────────────────────────────────────────────────────────── */

int run_scenario_read_tank_info(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    TankInfo info;
    TankModifiers mods;
    tank *t;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    t = &sim->sim.tanks[SR_SLOT];
    UT_ASSERT(*t != NULL);
    tankSetShells(&sim->sim, t, 11);
    tankSetMines(&sim->sim, t, 7);
    tankSetArmour(t, 23);
    tankSetTrees(&sim->sim, t, 5);
    memset(&mods, 0, sizeof(mods));
    mods.speed  = 150;
    mods.reload = 80;
    tankSetModifiers(*t, &mods);
    /* A facing past the 16 drawn frames, so dir and dir256 cannot both be
       the same number. */
    (*t)->angle = 200.0f;

    if (!serverSimGetTankInfo(sim, SR_SLOT, &info)) {
        UT_FAIL("tank read refused the connected slot");
    }
    /* The fields that were already here keep their meaning. */
    UT_ASSERT(info.has_tank);
    UT_ASSERT_MSG(strcmp(info.name, "Seat0") == 0, "name '%s'", info.name);
    UT_ASSERT_MSG(info.dir < 16, "dir %u is not a 0-15 frame",
                  (unsigned)info.dir);
    UT_ASSERT(info.dir == tankGetDir(t));
    /* And the fields added beside them mirror the tank. */
    UT_ASSERT(info.map_x == tankGetMX(t) && info.map_y == tankGetMY(t));
    UT_ASSERT_MSG(info.dir256 == 200, "dir256 %u for angle 200",
                  (unsigned)info.dir256);
    UT_ASSERT(info.dir256 == tankGet256Dir(t));
    UT_ASSERT_MSG(info.shells == 11, "shells %u", (unsigned)info.shells);
    UT_ASSERT_MSG(info.mines == 7, "mines %u", (unsigned)info.mines);
    UT_ASSERT_MSG(info.armour == 23, "armour %u", (unsigned)info.armour);
    UT_ASSERT_MSG(info.trees == 5, "trees %u", (unsigned)info.trees);
    UT_ASSERT_MSG(info.pills == 0, "pills %u on a fresh tank",
                  (unsigned)info.pills);
    UT_ASSERT(!info.is_bot);
    UT_ASSERT(info.mods.speed == 150 && info.mods.reload == 80);
    UT_ASSERT(info.mods.accel == 0 && info.mods.turn == 0);
    UT_ASSERT(info.mods.dealt == 0 && info.mods.taken == 0);

    /* Pills the tank picks up are counted. tankDestroy frees the list. */
    {
        tankCarryPb q = (tankCarryPb)malloc(sizeof(struct tankCarrypbObj));
        UT_ASSERT(q != NULL);
        q->pillNum = 1;
        q->next    = (*t)->carryPills;
        (*t)->carryPills = q;
    }
    UT_ASSERT(serverSimGetTankInfo(sim, SR_SLOT, &info));
    UT_ASSERT_MSG(info.pills == 1, "pills %u carrying one",
                  (unsigned)info.pills);
    UT_ASSERT(info.pills == tankGetNumCarriedPills(t));

    /* Connected with no tank object: identity is filled, tank state is not. */
    {
        tank saved = *t;
        *t = NULL;
        memset(&info, 0, sizeof(info));
        UT_ASSERT(serverSimGetTankInfo(sim, SR_SLOT, &info));
        UT_ASSERT(!info.has_tank && !info.alive);
        UT_ASSERT(strcmp(info.name, "Seat0") == 0);
        UT_ASSERT(info.shells == 0 && info.mines == 0 && info.armour == 0);
        UT_ASSERT(info.map_x == 0 && info.map_y == 0 && info.dir256 == 0);
        UT_ASSERT(info.pills == 0);
        UT_ASSERT(info.mods.speed == 0 && info.mods.reload == 0);
        *t = saved;
    }

    /* An empty slot and an index past the roster are refused untouched. */
    sr_poison(&info, sizeof(info));
    UT_ASSERT(!serverSimGetTankInfo(sim, 1, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));
    UT_ASSERT(!serverSimGetTankInfo(sim, MAX_TANKS, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));
    UT_ASSERT(!serverSimGetTankInfo(sim, 255, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));

    serverSimDestroy(sim);
    return 0;
}

/* ── Builder ─────────────────────────────────────────────────────── */

int run_scenario_read_builder_info(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    ServerSimBuilderInfo info;
    lgm *l;
    tank *t;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    l = &sim->sim.lgmen[SR_SLOT];
    t = &sim->sim.tanks[SR_SLOT];
    UT_ASSERT(*l != NULL && *t != NULL);

    /* A fresh builder rides in the tank with no job. */
    if (!serverSimGetBuilderInfo(sim, SR_SLOT, &info)) {
        UT_FAIL("builder read refused the connected slot");
    }
    UT_ASSERT_MSG(info.state == builderStateInTank, "state %d",
                  (int)info.state);
    UT_ASSERT_MSG(info.job == builderJobNone, "job %d", (int)info.job);
    UT_ASSERT(info.trees == 0 && info.mines == 0);

    /* Out on a job: state, job, position and load all come across. */
    (*l)->inTank   = FALSE;
    (*l)->isDead   = FALSE;
    (*l)->state    = LGM_STATE_GOING;
    (*l)->action   = LGM_ROAD_REQUEST;
    (*l)->numTrees = 3;
    (*l)->numMines = 2;
    (*l)->x = (WORLD)((40 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->y = (WORLD)((50 << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    UT_ASSERT(serverSimGetBuilderInfo(sim, SR_SLOT, &info));
    UT_ASSERT_MSG(info.state == builderStateGoing, "state %d",
                  (int)info.state);
    UT_ASSERT_MSG(info.job == builderJobRoad, "job %d", (int)info.job);
    UT_ASSERT(info.world_x == (*l)->x && info.world_y == (*l)->y);
    UT_ASSERT_MSG(info.map_x == 40 && info.map_y == 50, "square %u,%u",
                  (unsigned)info.map_x, (unsigned)info.map_y);
    UT_ASSERT(info.trees == 3 && info.mines == 2);

    /* Walking back. */
    (*l)->state  = LGM_STATE_RETURN;
    (*l)->action = LGM_IDLE;
    UT_ASSERT(serverSimGetBuilderInfo(sim, SR_SLOT, &info));
    UT_ASSERT_MSG(info.state == builderStateReturning, "state %d",
                  (int)info.state);
    UT_ASSERT(info.job == builderJobNone);

    /* Every job code the engine takes reads back as its own member. */
    {
        const BYTE actions[] = { LGM_TREE_REQUEST, LGM_ROAD_REQUEST,
                                 LGM_BUILDING_REQUEST, LGM_PILL_REQUEST,
                                 LGM_MINE_REQUEST, LGM_BOAT_REQUEST,
                                 LGM_IDLE };
        const BuilderJob jobs[] = { builderJobTrees, builderJobRoad,
                                    builderJobBuilding, builderJobPill,
                                    builderJobMine, builderJobBoat,
                                    builderJobNone };
        size_t k;
        for (k = 0; k < sizeof(actions) / sizeof(actions[0]); k++) {
            (*l)->action = actions[k];
            UT_ASSERT(serverSimGetBuilderInfo(sim, SR_SLOT, &info));
            UT_ASSERT_MSG(info.job == jobs[k], "action %u read back as %d",
                          (unsigned)actions[k], (int)info.job);
        }
        (*l)->action = LGM_IDLE;
    }

    /* Killed with the tank still standing: on the way back down. */
    (*l)->isDead = TRUE;
    UT_ASSERT(serverSimGetBuilderInfo(sim, SR_SLOT, &info));
    UT_ASSERT_MSG(info.state == builderStateParachuting, "state %d",
                  (int)info.state);

    /* Killed with no tank to land on: dead. */
    tankSetDestroyed(t, true);
    UT_ASSERT(serverSimGetBuilderInfo(sim, SR_SLOT, &info));
    UT_ASSERT_MSG(info.state == builderStateDead, "state %d", (int)info.state);

    /* An empty slot and an index past the roster are refused untouched. */
    sr_poison(&info, sizeof(info));
    UT_ASSERT(!serverSimGetBuilderInfo(sim, 1, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));
    UT_ASSERT(!serverSimGetBuilderInfo(sim, MAX_TANKS, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));
    UT_ASSERT(!serverSimGetBuilderInfo(sim, 255, &info));
    UT_ASSERT(sr_untouched(&info, sizeof(info)));

    serverSimDestroy(sim);
    return 0;
}

/* ── Whole map ───────────────────────────────────────────────────── */

int run_scenario_read_terrain_buffer(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    static BYTE buf[SERVER_SIM_TERRAIN_BYTES + 64];
    int x, y;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    /* A cap short of the whole array copies nothing. */
    memset(buf, SR_FILL, sizeof(buf));
    UT_ASSERT(!serverSimGetMapTerrainBuffer(sim, buf,
                                            SERVER_SIM_TERRAIN_BYTES - 1));
    UT_ASSERT(sr_untouched(buf, sizeof(buf)));
    UT_ASSERT(!serverSimGetMapTerrainBuffer(sim, buf, 0));
    UT_ASSERT(sr_untouched(buf, sizeof(buf)));

    /* The whole array agrees with the square-at-a-time read, everywhere.
       A cap with room to spare is accepted. */
    UT_ASSERT(serverSimGetMapTerrainBuffer(sim, buf, sizeof(buf)));
    for (y = 0; y < 256; y++) {
        for (x = 0; x < 256; x++) {
            BYTE want = serverSimGetMapTerrain(sim, (BYTE)x, (BYTE)y);
            BYTE got  = buf[(y * 256) + x];
            UT_ASSERT_MSG(got == want, "square %d,%d: buffer %u, read %u",
                          x, y, (unsigned)got, (unsigned)want);
        }
    }

    /* The corners in particular, so a row-major/column-major mix-up on a
       square-shaped array cannot pass unnoticed. */
    UT_ASSERT(buf[0] == serverSimGetMapTerrain(sim, 0, 0));
    UT_ASSERT(buf[255] == serverSimGetMapTerrain(sim, 255, 0));
    UT_ASSERT(buf[255 * 256] == serverSimGetMapTerrain(sim, 0, 255));
    UT_ASSERT(buf[(255 * 256) + 255] == serverSimGetMapTerrain(sim, 255, 255));

    /* Two squares that are each other transposed, given different terrain,
       so the buffer cannot be read the other way round and still pass. An
       exact-sized cap is enough to copy them. */
    sim->sim.mp->mapItem[70][31] = ROAD;
    sim->sim.mp->mapItem[31][70] = GRASS;
    UT_ASSERT(serverSimGetMapTerrainBuffer(sim, buf, SERVER_SIM_TERRAIN_BYTES));
    UT_ASSERT_MSG(buf[(31 * 256) + 70] == ROAD,
                  "square 70,31 landed at the wrong index (%u)",
                  (unsigned)buf[(31 * 256) + 70]);
    UT_ASSERT_MSG(buf[(70 * 256) + 31] == GRASS,
                  "square 31,70 landed at the wrong index (%u)",
                  (unsigned)buf[(70 * 256) + 31]);

    UT_ASSERT(!serverSimGetMapTerrainBuffer(sim, NULL, sizeof(buf)));

    serverSimDestroy(sim);
    return 0;
}

/* ── Fielded count ───────────────────────────────────────────────── */

int run_scenario_read_num_fielded(void) {
    ServerSim *sim = ut_make_running_sim("Seat0");
    ServerSimRosterSlot slot;

    if (sim == NULL) UT_FAIL("could not build a running sim");

    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 1, "fielded %u with one seat",
                  (unsigned)serverSimGetNumFielded(sim));

    serverSimAddPlayer(sim, 1, "Seat1", false);
    serverSimAddPlayer(sim, 2, "Seat2", false);
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) == 3, "fielded %u with three",
                  (unsigned)serverSimGetNumFielded(sim));

    /* Every seat it counts reports itself as playing the round. */
    {
        BYTE i;
        BYTE counted = 0;
        for (i = 0; i < MAX_TANKS; i++) {
            if (!serverSimGetRosterSlot(sim, i, &slot)) continue;
            UT_ASSERT(slot.fielded);
            counted++;
        }
        UT_ASSERT_MSG(counted == serverSimGetNumFielded(sim),
                      "%u seats read, %u fielded", (unsigned)counted,
                      (unsigned)serverSimGetNumFielded(sim));
    }

    /* Seats that sit out do not exist yet, so this matches the roster
       length; the two part company when they do. */
    UT_ASSERT(serverSimGetNumFielded(sim) == serverSimGetNumPlayers(sim));

    serverSimDestroy(sim);
    return 0;
}
