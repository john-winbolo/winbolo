/*
 * The four map ops a scenario edits terrain and mines with.
 *
 * Each arm validates, mutates through mapSetPos and the mine list — the
 * same pair a builder's work and a laid mine go through — and leaves the
 * publish and the record to them. So each case asks the same three things
 * the other arm files ask: does every refusal in the op's contract come
 * back with its own code, does the mutation land on the sim, and does the
 * map event reach the frame's buffer.
 *
 * Two cases carry what makes the map group its own shape:
 *   - a fill publishes once per square it changes, not once per square in
 *     the rectangle, so painting grass over grass costs nothing
 *     (run_scenario_map_fill_rect);
 *   - a fill past SCN_TILES_PER_TICK applies that many squares, answers
 *     SCN_OP_QUEUED and leaves the rest on the sim, which carries them on
 *     the following tick — so 300 squares land over two ticks with none
 *     lost and none written twice (run_scenario_map_fill_paced).
 *
 * run_scenario_map_set_tile     — one square, deep sea included
 * run_scenario_map_fill_rect    — a rectangle, one publish per change
 * run_scenario_map_fill_paced   — the budget, the queue and the drain
 * run_scenario_map_fill_no_budget_refused
 *                               — a fill that can write nothing is refused
 * run_scenario_map_fill_dropped_at_round_start
 *                               — a remainder does not cross a round start
 * run_scenario_map_fill_dropped_at_map_swap
 *                               — nor a map swapped under it in the lobby
 * run_scenario_map_place_mine   — a mine laid, seen or hidden
 * run_scenario_map_remove_mine  — a mine lifted without going off
 * run_scenario_map_arm_records  — the four changes survive a recording
 *
 * A map event is only recorded while the map-change callback is installed,
 * which in a live server is the frame. The cases that count publishes arm
 * it the way a running half-step does and drop it again afterwards.
 *
 * Drives serverSimApplyScenarioOp and reads the ServerSim struct directly;
 * the unittests profile permits internal access.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* mapEvents[], the fill funnel state, the drain */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"
#include "server/sim/server_sim_shared.h" /* serverSimSetActive, simMapChangeCallback */
#include "game_sim.h"              /* GameSim: mp/pb/bs/mns */
#include "bolo_map.h"              /* mapGetPos / mapSetPos / mapSetChangeCallback */
#include "pillbox.h"               /* pillsExistPos — a square an object owns */
#include "bases.h"                 /* basesExistPos — likewise */
#include "mines.h"                 /* the mine list the arms write */
#include "log.h"                   /* log_MapChange and the stream opcodes */
#include "input_packet.h"          /* EVENT_MAP_CHANGE, EVENT_MINE_VISIBLE */
#include "everard_map.h"
#include "replay_harness.h"
#include "test_harness.h"

#define MA_SLOT 0

/* ── Small helpers ───────────────────────────────────────────────── */

static ServerSim *maMakeLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    return sim;
}

/* Install the map-change callback the way a running half-step does, so the
   mapSetPos inside an arm records an EVENT_MAP_CHANGE this frame. */
static void maArm(ServerSim *sim) {
    serverSimSetActive(sim);
    sim->mapEventCount = 0;
    mapSetChangeCallback(simMapChangeCallback);
}

/* And take it off again: it is process-wide, so a case that left it armed
   would have every later test's terrain writes land in this sim's buffer. */
static void maDisarm(void) {
    mapSetChangeCallback(NULL);
}

static int maCountEvents(ServerSim *sim, BYTE type) {
    const GameEvent *evs = serverSimGetEvents(sim);
    uint8_t n = serverSimGetEventCount(sim);
    int found = 0;
    uint8_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == type) found++;
    }
    return found;
}

/* How many map events name this square this frame, and what the last one
   said the terrain was. */
static int maMapEventsAt(ServerSim *sim, BYTE x, BYTE y, BYTE *terrainOut) {
    const GameEvent *evs = serverSimGetMapEvents(sim);
    uint16_t n = serverSimGetMapEventCount(sim);
    int found = 0;
    uint16_t i;
    for (i = 0; i < n; i++) {
        if (evs[i].type == EVENT_MAP_CHANGE && evs[i].data[0] == x &&
            evs[i].data[1] == y) {
            found++;
            if (terrainOut != NULL) *terrainOut = evs[i].data[2];
        }
    }
    return found;
}

/* A square inside the minable area holding nothing an object owns. A pill or
   a base writes its own tile back, so a test that painted over one would be
   undone behind its back. */
static bool maSquareIsFree(ServerSim *sim, BYTE x, BYTE y) {
    GameSim *gs = &sim->sim;
    return pillsExistPos(&gs->pb, x, y) == FALSE &&
           basesExistPos(&gs->bs, x, y) == FALSE;
}

static bool maFindTile(ServerSim *sim, BYTE want, BYTE *ox, BYTE *oy) {
    GameSim *gs = &sim->sim;
    int x, y;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == want &&
                maSquareIsFree(sim, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* The top-left of a w by h rectangle whose every square is free of pills and
   bases, so a fill across it is the only thing writing those tiles. */
static bool maFindRect(ServerSim *sim, int w, int h, BYTE *ox, BYTE *oy) {
    int x, y, dx, dy;
    for (y = MAP_MINE_EDGE_TOP + 1; y + h <= MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x + w <= MAP_MINE_EDGE_RIGHT; x++) {
            bool clear = true;
            for (dy = 0; dy < h && clear; dy++) {
                for (dx = 0; dx < w && clear; dx++) {
                    if (!maSquareIsFree(sim, (BYTE)(x + dx), (BYTE)(y + dy))) {
                        clear = false;
                    }
                }
            }
            if (clear) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* Write a square without going through an op, for setting a case up. */
static void maPaint(ServerSim *sim, BYTE x, BYTE y, BYTE terrain) {
    mapSetPos(&sim->sim, &sim->sim.mp, x, y, terrain, FALSE, FALSE);
}

/* ── Set tile ────────────────────────────────────────────────────── */

int run_scenario_map_set_tile(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE gx = 0, gy = 0;
    BYTE seen = 0;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(maFindTile(sim, GRASS, &gx, &gy), "map has no free grass");

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_SET_TILE;
    op.u.mapSetTile.x = gx;
    op.u.mapSetTile.y = gy;

    /* The square takes the terrain, and the change is published once. */
    maArm(sim);
    op.u.mapSetTile.terrain = CRATER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, gx, gy) == CRATER,
                  "the square reads %u", (unsigned)mapGetPos(&sim->sim.mp, gx, gy));
    UT_ASSERT_MSG(maMapEventsAt(sim, gx, gy, &seen) == 1,
                  "the change was published %d times",
                  maMapEventsAt(sim, gx, gy, NULL));
    UT_ASSERT_MSG(seen == CRATER, "the map event names terrain %u",
                  (unsigned)seen);

    /* Deep sea is a terrain a scenario may write: putting open water back
       under a pill it has taken away is what the code is there for. It is
       outside the 0..15 run, so it is the one value the range check has to
       name on its own. */
    maArm(sim);
    op.u.mapSetTile.terrain = DEEP_SEA;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "deep sea was refused");
    UT_ASSERT(mapGetPos(&sim->sim.mp, gx, gy) == DEEP_SEA);
    UT_ASSERT_MSG(maMapEventsAt(sim, gx, gy, &seen) == 1 && seen == DEEP_SEA,
                  "the deep-sea change was not published as deep sea");

    /* Every code in the run, mined variants included. */
    maArm(sim);
    op.u.mapSetTile.terrain = MINE_END;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(mapGetPos(&sim->sim.mp, gx, gy) == MINE_END);

    /* A byte past the run and short of deep sea is not a terrain, and
       nothing is written when one arrives. */
    op.u.mapSetTile.terrain = (BYTE)(MINE_END + 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.mapSetTile.terrain = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, gx, gy) == MINE_END,
                  "a refused op still wrote the square");

    /* A mine under the square goes with it: the terrain being written
       carries none, so a visible-mine record left behind would mark a
       square nothing could clear. */
    maPaint(sim, gx, gy, MINE_GRASS);
    minesAddItem(&sim->sim.mns, gx, gy);
    minesSetOwner(&sim->sim.mns, gx, gy, MA_SLOT);
    op.u.mapSetTile.terrain = ROAD;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(mapGetPos(&sim->sim.mp, gx, gy) == ROAD);
    UT_ASSERT_MSG(sim->sim.mns->pos[gx][gy] == FALSE,
                  "the mine record outlived the terrain under it");
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, gx, gy) == NEUTRAL,
                  "the mine's owner outlived the mine");

    /* Off the map is refused. mapGetPos answers deep sea for the border
       whatever the array holds, so a write there would reach every client
       and none of the sim's own reads. */
    op.u.mapSetTile.terrain = GRASS;
    op.u.mapSetTile.x = 0;
    op.u.mapSetTile.y = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapSetTile.x = MAP_MINE_EDGE_LEFT;
    op.u.mapSetTile.y = 100;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapSetTile.x = MAP_MINE_EDGE_RIGHT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapSetTile.x = 100;
    op.u.mapSetTile.y = MAP_MINE_EDGE_TOP;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapSetTile.y = MAP_MINE_EDGE_BOTTOM;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapSetTile.x = 255;
    op.u.mapSetTile.y = 255;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);

    /* The first square inside each edge is on the map. */
    maArm(sim);
    op.u.mapSetTile.x = MAP_MINE_EDGE_LEFT + 1;
    op.u.mapSetTile.y = MAP_MINE_EDGE_TOP + 1;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the first square inside the border was refused");
    op.u.mapSetTile.x = MAP_MINE_EDGE_RIGHT - 1;
    op.u.mapSetTile.y = MAP_MINE_EDGE_BOTTOM - 1;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "the last square inside the border was refused");

    maDisarm();
    serverSimDestroy(sim);
    return 0;
}

/* ── Fill rect ───────────────────────────────────────────────────── */

int run_scenario_map_fill_rect(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE rx = 0, ry = 0;
    const int W = 8, H = 8;
    int dx, dy;
    int expectedChanges = 0;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(maFindRect(sim, W, H, &rx, &ry),
                  "map has no free %dx%d rectangle", W, H);

    /* Half the rectangle already holds the terrain the fill asks for, so the
       changed count and the square count are different numbers. */
    for (dy = 0; dy < H; dy++) {
        for (dx = 0; dx < W; dx++) {
            if (((dx + dy) & 1) != 0) {
                maPaint(sim, (BYTE)(rx + dx), (BYTE)(ry + dy), CRATER);
            } else {
                maPaint(sim, (BYTE)(rx + dx), (BYTE)(ry + dy), GRASS);
                expectedChanges++;
            }
        }
    }

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_FILL_RECT;
    op.u.mapFillRect.x0 = rx;
    op.u.mapFillRect.y0 = ry;
    op.u.mapFillRect.x1 = (BYTE)(rx + W - 1);
    op.u.mapFillRect.y1 = (BYTE)(ry + H - 1);
    op.u.mapFillRect.terrain = CRATER;

    maArm(sim);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "a rectangle inside the budget should finish in the op");
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "a finished fill left a remainder behind");

    /* Every square holds the terrain, and only the squares that changed were
       published — the count is the changed count, not the area. */
    for (dy = 0; dy < H; dy++) {
        for (dx = 0; dx < W; dx++) {
            BYTE x = (BYTE)(rx + dx);
            BYTE y = (BYTE)(ry + dy);
            int published = maMapEventsAt(sim, x, y, NULL);
            UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, x, y) == CRATER,
                          "square %u,%u reads %u", (unsigned)x, (unsigned)y,
                          (unsigned)mapGetPos(&sim->sim.mp, x, y));
            if (((dx + dy) & 1) != 0) {
                UT_ASSERT_MSG(published == 0,
                              "square %u,%u already held the terrain and was "
                              "published %d time(s)", (unsigned)x, (unsigned)y,
                              published);
            } else {
                UT_ASSERT_MSG(published == 1,
                              "square %u,%u was published %d time(s)",
                              (unsigned)x, (unsigned)y, published);
            }
        }
    }
    UT_ASSERT_MSG(serverSimGetMapEventCount(sim) == (uint16_t)expectedChanges,
                  "the fill published %u events for %d changed squares",
                  (unsigned)serverSimGetMapEventCount(sim), expectedChanges);

    /* The same fill again changes nothing and publishes nothing. */
    maArm(sim);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(serverSimGetMapEventCount(sim) == 0,
                  "a fill over ground that already matches published %u events",
                  (unsigned)serverSimGetMapEventCount(sim));

    /* Corners the other way round name the same rectangle. */
    maArm(sim);
    op.u.mapFillRect.x0 = (BYTE)(rx + W - 1);
    op.u.mapFillRect.y0 = (BYTE)(ry + H - 1);
    op.u.mapFillRect.x1 = rx;
    op.u.mapFillRect.y1 = ry;
    op.u.mapFillRect.terrain = ROAD;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                  "a rectangle given bottom-right first was refused");
    UT_ASSERT_MSG(serverSimGetMapEventCount(sim) == (uint16_t)(W * H),
                  "the reversed rectangle covered %u squares, wanted %d",
                  (unsigned)serverSimGetMapEventCount(sim), W * H);
    UT_ASSERT(mapGetPos(&sim->sim.mp, rx, ry) == ROAD);
    UT_ASSERT(mapGetPos(&sim->sim.mp, (BYTE)(rx + W - 1),
                        (BYTE)(ry + H - 1)) == ROAD);

    /* A single square is a rectangle too. */
    maArm(sim);
    op.u.mapFillRect.x0 = rx;
    op.u.mapFillRect.y0 = ry;
    op.u.mapFillRect.x1 = rx;
    op.u.mapFillRect.y1 = ry;
    op.u.mapFillRect.terrain = SWAMP;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(mapGetPos(&sim->sim.mp, rx, ry) == SWAMP);
    UT_ASSERT_MSG(serverSimGetMapEventCount(sim) == 1,
                  "a one-square fill published %u events",
                  (unsigned)serverSimGetMapEventCount(sim));

    /* A corner off the map is refused, and nothing is written when one is. */
    op.u.mapFillRect.terrain = GRASS;
    op.u.mapFillRect.x0 = 0;
    op.u.mapFillRect.y0 = 0;
    op.u.mapFillRect.x1 = (BYTE)(rx + W - 1);
    op.u.mapFillRect.y1 = (BYTE)(ry + H - 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapFillRect.x0 = rx;
    op.u.mapFillRect.y0 = ry;
    op.u.mapFillRect.x1 = MAP_MINE_EDGE_RIGHT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapFillRect.x1 = (BYTE)(rx + W - 1);
    op.u.mapFillRect.y1 = MAP_MINE_EDGE_BOTTOM;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, rx, ry) == SWAMP,
                  "a refused fill still wrote a square");

    /* And a terrain that is not one. */
    op.u.mapFillRect.y1 = (BYTE)(ry + H - 1);
    op.u.mapFillRect.terrain = (BYTE)(MINE_END + 1);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.mapFillRect.terrain = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, rx, ry) == SWAMP,
                  "a refused fill still wrote a square");
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "a refused fill queued itself anyway");

    maDisarm();
    serverSimDestroy(sim);
    return 0;
}

/* ── Fill rect: the budget, the queue and the drain ──────────────── */

/* A hook that issues one oversized fill the first time it runs, which is how
 * a scenario would: the op goes out from inside the frame and the sim carries
 * what would not fit into the frames after it. */
typedef struct {
    ServerSim *sim;
    BYTE       x0, y0, x1, y1;
    int        calls;
    ScnOpResult result;
} MaFillHook;

static void maFillHookTick(void *ctx) {
    MaFillHook *hk = (MaFillHook *)ctx;
    ScenarioOp op;
    hk->calls++;
    if (hk->calls != 1) return;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_FILL_RECT;
    op.u.mapFillRect.x0 = hk->x0;
    op.u.mapFillRect.y0 = hk->y0;
    op.u.mapFillRect.x1 = hk->x1;
    op.u.mapFillRect.y1 = hk->y1;
    op.u.mapFillRect.terrain = CRATER;
    hk->result = serverSimApplyScenarioOp(hk->sim, &op, NULL);
}

/* SCN_TILES_PER_TICK squares change per tick and the rest queue. 300 is
 * Appendix G's stated boundary for the constant: past one tick's budget,
 * inside two. Run in the lobby, where a tick runs no simulation, so the only
 * thing writing terrain is the fill.
 *
 * Nothing is written twice: the budget is fixed, so a square painted a second
 * time would spend a slot another square needed and the first tick would end
 * with fewer than SCN_TILES_PER_TICK of the rectangle changed. */
#define MA_FILL_W 20
#define MA_FILL_H 15
#define MA_FILL_N (MA_FILL_W * MA_FILL_H)

int run_scenario_map_fill_paced(void) {
    ServerSim *sim = maMakeLobbySim();
    MaFillHook hk;
    ScenarioOp second;
    BYTE rx = 0, ry = 0;
    int dx, dy, changed;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(MA_FILL_N > SCN_TILES_PER_TICK &&
                  MA_FILL_N <= 2 * SCN_TILES_PER_TICK,
                  "%d squares is not a fill that takes exactly two ticks",
                  MA_FILL_N);
    UT_ASSERT_MSG(maFindRect(sim, MA_FILL_W, MA_FILL_H, &rx, &ry),
                  "map has no free %dx%d rectangle", MA_FILL_W, MA_FILL_H);

    /* Every square differs from what the fill will paint, so the changed
       count is the whole rectangle. */
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            maPaint(sim, (BYTE)(rx + dx), (BYTE)(ry + dy), GRASS);
        }
    }

    memset(&hk, 0, sizeof(hk));
    hk.sim = sim;
    hk.x0 = rx;
    hk.y0 = ry;
    hk.x1 = (BYTE)(rx + MA_FILL_W - 1);
    hk.y1 = (BYTE)(ry + MA_FILL_H - 1);
    hk.result = SCN_OP_UNSUPPORTED;   /* a value no fill can answer with */
    serverSimSetScenarioTick(sim, maFillHookTick, &hk);

    /* First tick: the hook's op spends the budget and queues the rest. */
    serverSimTick(sim);
    UT_ASSERT_MSG(hk.calls == 1, "the hook ran %d times", hk.calls);
    UT_ASSERT_MSG(hk.result == SCN_OP_QUEUED,
                  "a fill past the budget answered %d, wanted SCN_OP_QUEUED",
                  (int)hk.result);
    UT_ASSERT_MSG(sim->scenarioFillPending,
                  "a queued fill left nothing on the sim to carry");

    changed = 0;
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            if (mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                          (BYTE)(ry + dy)) == CRATER) {
                changed++;
            }
        }
    }
    UT_ASSERT_MSG(changed == SCN_TILES_PER_TICK,
                  "the first tick changed %d squares, wanted %d",
                  changed, SCN_TILES_PER_TICK);

    /* A second fill while one is outstanding is refused rather than
       replacing it: the squares the first still owes are not this one's to
       throw away. */
    memset(&second, 0, sizeof(second));
    second.type = SCN_OP_MAP_FILL_RECT;
    second.u.mapFillRect.x0 = rx;
    second.u.mapFillRect.y0 = ry;
    second.u.mapFillRect.x1 = rx;
    second.u.mapFillRect.y1 = ry;
    second.u.mapFillRect.terrain = ROAD;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &second, NULL) == SCN_OP_RATE,
                  "a second fill was accepted while one was outstanding");
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, rx, ry) != ROAD,
                  "the refused fill wrote a square anyway");
    UT_ASSERT_MSG(sim->scenarioFillPending,
                  "the refused fill took the outstanding one's place");

    /* Second tick: the drain carries the rest. */
    serverSimTick(sim);
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "the fill was still outstanding after two ticks");

    changed = 0;
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            if (mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                          (BYTE)(ry + dy)) == CRATER) {
                changed++;
            }
        }
    }
    UT_ASSERT_MSG(changed == MA_FILL_N,
                  "%d of %d squares landed over two ticks", changed, MA_FILL_N);

    /* With nothing outstanding a fill is taken again. */
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &second, NULL) == SCN_OP_OK,
                  "a fill was still refused after the queue emptied");
    UT_ASSERT(mapGetPos(&sim->sim.mp, rx, ry) == ROAD);

    serverSimSetScenarioTick(sim, NULL, NULL);
    serverSimDestroy(sim);
    return 0;
}

/* ── Fill rect: a tick with no budget left ───────────────────────── */

/* A fill that cannot write a single square says so. SCN_OP_QUEUED means the
 * work started and the rest is coming; answering it for a fill that wrote
 * nothing tells a script something it cannot act on, and — worse — leaves a
 * rectangle on the sim that makes no progress, which every later fill is then
 * refused behind for as long as the sim lives. SCN_OP_RATE instead, and
 * nothing is left outstanding.
 *
 * The budget is spent here by a fill that finishes inside it, so the second
 * fill in the same tick meets an empty budget and a clear pending flag — the
 * one case that is neither "already outstanding" nor "room to start". */
int run_scenario_map_fill_no_budget_refused(void) {
    ServerSim *sim = maMakeLobbySim();
    ScenarioOp first;
    ScenarioOp second;
    BYTE rx = 0, ry = 0;
    const int W = 16, TALL = 18;   /* 16 x 16 spends the budget exactly, 2 rows spare */
    int dx, dy;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(W * W == SCN_TILES_PER_TICK,
                  "%d squares is not one tick's budget of %d", W * W,
                  SCN_TILES_PER_TICK);
    UT_ASSERT_MSG(maFindRect(sim, W, TALL, &rx, &ry),
                  "map has no free %dx%d rectangle", W, TALL);

    for (dy = 0; dy < TALL; dy++) {
        for (dx = 0; dx < W; dx++) {
            maPaint(sim, (BYTE)(rx + dx), (BYTE)(ry + dy), GRASS);
        }
    }

    /* The first fill changes exactly one tick's worth and finishes. */
    memset(&first, 0, sizeof(first));
    first.type = SCN_OP_MAP_FILL_RECT;
    first.u.mapFillRect.x0 = rx;
    first.u.mapFillRect.y0 = ry;
    first.u.mapFillRect.x1 = (BYTE)(rx + W - 1);
    first.u.mapFillRect.y1 = (BYTE)(ry + W - 1);
    first.u.mapFillRect.terrain = CRATER;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &first, NULL) == SCN_OP_OK,
                  "a fill of exactly one budget should finish in the op");
    UT_ASSERT_MSG(!sim->scenarioFillPending, "a finished fill stayed queued");
    UT_ASSERT_MSG(sim->scenarioFillSpent == SCN_TILES_PER_TICK,
                  "the fill spent %u of the budget, wanted all %d",
                  (unsigned)sim->scenarioFillSpent, SCN_TILES_PER_TICK);

    /* The second, in the same tick, has nothing to spend. */
    memset(&second, 0, sizeof(second));
    second.type = SCN_OP_MAP_FILL_RECT;
    second.u.mapFillRect.x0 = rx;
    second.u.mapFillRect.y0 = (BYTE)(ry + W);
    second.u.mapFillRect.x1 = (BYTE)(rx + W - 1);
    second.u.mapFillRect.y1 = (BYTE)(ry + TALL - 1);
    second.u.mapFillRect.terrain = CRATER;
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &second, NULL) == SCN_OP_RATE,
                  "a fill with no budget left must be refused, not queued");
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "a refused fill was left outstanding — every later fill is "
                  "now refused behind a rectangle that never moves");
    for (dy = W; dy < TALL; dy++) {
        for (dx = 0; dx < W; dx++) {
            UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                                    (BYTE)(ry + dy)) == GRASS,
                          "the refused fill wrote square %u,%u",
                          (unsigned)(rx + dx), (unsigned)(ry + dy));
        }
    }

    /* The next tick hands the budget back and the same op is taken. */
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->scenarioFillSpent == 0,
                  "the tick did not hand the budget back: %u",
                  (unsigned)sim->scenarioFillSpent);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &second, NULL) == SCN_OP_OK,
                  "the fill was still refused on the tick after");
    for (dy = W; dy < TALL; dy++) {
        for (dx = 0; dx < W; dx++) {
            UT_ASSERT(mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                                (BYTE)(ry + dy)) == CRATER);
        }
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── Fill rect: what a round start does to one still outstanding ─── */

/* A round start reloads the map from the cached blob, so a rectangle the fill
 * was still working through names squares on ground that no longer exists. The
 * remainder is dropped at the round boundary rather than carried across it —
 * otherwise the squares it still owes land partway into the next round, on a
 * map nobody asked it to paint. */
int run_scenario_map_fill_dropped_at_round_start(void) {
    ServerSim *sim = maMakeLobbySim();
    MaFillHook hk;
    BYTE rx = 0, ry = 0;
    BYTE afterStart[MA_FILL_H][MA_FILL_W];
    int dx, dy, changed;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, MA_SLOT, "Tester", false);
    UT_ASSERT_MSG(maFindRect(sim, MA_FILL_W, MA_FILL_H, &rx, &ry),
                  "map has no free %dx%d rectangle", MA_FILL_W, MA_FILL_H);

    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            maPaint(sim, (BYTE)(rx + dx), (BYTE)(ry + dy), GRASS);
        }
    }

    memset(&hk, 0, sizeof(hk));
    hk.sim = sim;
    hk.x0 = rx;
    hk.y0 = ry;
    hk.x1 = (BYTE)(rx + MA_FILL_W - 1);
    hk.y1 = (BYTE)(ry + MA_FILL_H - 1);
    hk.result = SCN_OP_UNSUPPORTED;
    serverSimSetScenarioTick(sim, maFillHookTick, &hk);

    /* One lobby tick leaves a fill part done and the rest owing. */
    serverSimTick(sim);
    UT_ASSERT_MSG(hk.result == SCN_OP_QUEUED,
                  "the fill answered %d, wanted SCN_OP_QUEUED", (int)hk.result);
    UT_ASSERT_MSG(sim->scenarioFillPending, "the fill queued nothing");

    changed = 0;
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            if (mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                          (BYTE)(ry + dy)) == CRATER) {
                changed++;
            }
        }
    }
    UT_ASSERT_MSG(changed == SCN_TILES_PER_TICK,
                  "%d squares changed before the round start, wanted %d — the "
                  "fill was not left partway through", changed,
                  SCN_TILES_PER_TICK);

    /* The start reloads the map, so the rectangle is on ground that is gone. */
    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "the fill survived the round start");
    UT_ASSERT_MSG(sim->scenarioFillSpent == 0,
                  "the fill's spend survived the round start: %u",
                  (unsigned)sim->scenarioFillSpent);

    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            afterStart[dy][dx] = mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                                           (BYTE)(ry + dy));
        }
    }

    /* And a tick of the new round writes none of those squares. */
    serverSimTick(sim);
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            BYTE now = mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                                 (BYTE)(ry + dy));
            UT_ASSERT_MSG(now == afterStart[dy][dx],
                          "square %u,%u read %u after the start and %u after "
                          "the tick — the dropped fill kept painting",
                          (unsigned)(rx + dx), (unsigned)(ry + dy),
                          (unsigned)afterStart[dy][dx], (unsigned)now);
        }
    }
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "the fill came back after a tick of the new round");

    serverSimSetScenarioTick(sim, NULL, NULL);
    serverSimDestroy(sim);
    return 0;
}

/* ── Fill rect: what a lobby map swap does to one still outstanding ─ */

/* The same defect in its purest form: no round starts, nobody resets the
 * world, the map is simply swapped underneath a fill that is partway through
 * its rectangle. A map install in the lobby does not reach
 * serverSimResetGameWorld, so the fill has to be dropped where the new map
 * lands — otherwise the squares it still owes are painted onto whatever map
 * the host has just chosen.
 *
 * Driven through serverSimReloadCompressedInMemory, which is the uploaded-map
 * route; the reload, the map-list pick and the random regenerate reach the
 * same place. */
int run_scenario_map_fill_dropped_at_map_swap(void) {
    ServerSim *sim = maMakeLobbySim();
    BYTE emap[6000] = E_MAP;
    MaFillHook hk;
    BYTE rx = 0, ry = 0;
    BYTE afterSwap[MA_FILL_H][MA_FILL_W];
    int dx, dy, changed;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(maFindRect(sim, MA_FILL_W, MA_FILL_H, &rx, &ry),
                  "map has no free %dx%d rectangle", MA_FILL_W, MA_FILL_H);

    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            maPaint(sim, (BYTE)(rx + dx), (BYTE)(ry + dy), GRASS);
        }
    }

    memset(&hk, 0, sizeof(hk));
    hk.sim = sim;
    hk.x0 = rx;
    hk.y0 = ry;
    hk.x1 = (BYTE)(rx + MA_FILL_W - 1);
    hk.y1 = (BYTE)(ry + MA_FILL_H - 1);
    hk.result = SCN_OP_UNSUPPORTED;
    serverSimSetScenarioTick(sim, maFillHookTick, &hk);

    serverSimTick(sim);
    UT_ASSERT_MSG(hk.result == SCN_OP_QUEUED,
                  "the fill answered %d, wanted SCN_OP_QUEUED", (int)hk.result);
    UT_ASSERT_MSG(sim->scenarioFillPending, "the fill queued nothing");

    changed = 0;
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            if (mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                          (BYTE)(ry + dy)) == CRATER) {
                changed++;
            }
        }
    }
    UT_ASSERT_MSG(changed == SCN_TILES_PER_TICK,
                  "%d squares changed before the swap, wanted %d — the fill "
                  "was not left partway through", changed, SCN_TILES_PER_TICK);

    /* A different map is installed. The state stays lobby throughout: this is
       a map swap, not a round boundary. */
    UT_ASSERT_MSG(serverSimReloadCompressedInMemory(sim, emap, 5097,
                                                    "Everard Island"),
                  "the map swap was rejected");
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateLobby,
                  "the swap left the lobby, so this is not the route it "
                  "claims to cover");
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "the fill survived the map swap — its remainder would paint "
                  "the map the host has just chosen");
    UT_ASSERT_MSG(sim->scenarioFillSpent == 0,
                  "the fill's spend survived the map swap: %u",
                  (unsigned)sim->scenarioFillSpent);

    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            afterSwap[dy][dx] = mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                                          (BYTE)(ry + dy));
        }
    }

    /* And a tick on the new map writes none of those squares. */
    serverSimTick(sim);
    for (dy = 0; dy < MA_FILL_H; dy++) {
        for (dx = 0; dx < MA_FILL_W; dx++) {
            BYTE now = mapGetPos(&sim->sim.mp, (BYTE)(rx + dx),
                                 (BYTE)(ry + dy));
            UT_ASSERT_MSG(now == afterSwap[dy][dx],
                          "square %u,%u read %u after the swap and %u after "
                          "the tick — the dropped fill kept painting",
                          (unsigned)(rx + dx), (unsigned)(ry + dy),
                          (unsigned)afterSwap[dy][dx], (unsigned)now);
        }
    }
    UT_ASSERT_MSG(!sim->scenarioFillPending,
                  "the fill came back after a tick on the new map");

    serverSimSetScenarioTick(sim, NULL, NULL);
    serverSimDestroy(sim);
    return 0;
}

/* ── Place mine ──────────────────────────────────────────────────── */

int run_scenario_map_place_mine(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp op;
    BYTE gx = 0, gy = 0, hx = 0, hy = 0;
    BYTE seen = 0;
    base firstBase;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(basesGetNumBases(&sim->sim.bs) >= 1);
    UT_ASSERT_MSG(maFindRect(sim, 2, 1, &gx, &gy),
                  "map has no free pair of squares");
    hx = (BYTE)(gx + 1);
    hy = gy;
    maPaint(sim, gx, gy, GRASS);
    maPaint(sim, hx, hy, GRASS);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_PLACE_MINE;
    op.u.mapPlaceMine.x = gx;
    op.u.mapPlaceMine.y = gy;
    op.u.mapPlaceMine.owner = MA_SLOT;
    op.u.mapPlaceMine.visible = true;

    /* The terrain gains the mine, the list learns who laid it, the change is
       published once, and a visible mine is announced to everybody. */
    maArm(sim);
    sim->eventCount = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, gx, gy) == MINE_GRASS,
                  "the square reads %u",
                  (unsigned)mapGetPos(&sim->sim.mp, gx, gy));
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, gx, gy) == MA_SLOT,
                  "the mine names owner %u",
                  (unsigned)minesGetOwner(&sim->sim.mns, gx, gy));
    UT_ASSERT_MSG(sim->sim.mns->pos[gx][gy] == TRUE,
                  "a visible mine is not in the mine list");
    UT_ASSERT_MSG(maMapEventsAt(sim, gx, gy, &seen) == 1 && seen == MINE_GRASS,
                  "the mined square was not published as mined");
    UT_ASSERT_MSG(maCountEvents(sim, EVENT_MINE_VISIBLE) == 1,
                  "a visible mine was announced %d times",
                  maCountEvents(sim, EVENT_MINE_VISIBLE));
    {
        const GameEvent *evs = serverSimGetEvents(sim);
        uint8_t n = serverSimGetEventCount(sim);
        uint8_t i;
        bool broadcast = false;
        for (i = 0; i < n; i++) {
            if (evs[i].type == EVENT_MINE_VISIBLE &&
                (evs[i].data[2] & 0x80) != 0) {
                broadcast = true;
            }
        }
        UT_ASSERT_MSG(broadcast,
                      "a visible mine went to the owner's side only");
    }

    /* A square that already holds one. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_ALREADY);

    /* A hidden mine is laid the same way and announced to nobody, which is
       what a laid mine is to everyone but whoever put it there. */
    maArm(sim);
    sim->eventCount = 0;
    op.u.mapPlaceMine.x = hx;
    op.u.mapPlaceMine.y = hy;
    op.u.mapPlaceMine.visible = false;
    op.u.mapPlaceMine.owner = NEUTRAL;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT(mapGetPos(&sim->sim.mp, hx, hy) == MINE_GRASS);
    UT_ASSERT_MSG(sim->sim.mns->pos[hx][hy] == FALSE,
                  "a hidden mine was put in the mine list");
    UT_ASSERT_MSG(maCountEvents(sim, EVENT_MINE_VISIBLE) == 0,
                  "a hidden mine was announced");
    UT_ASSERT_MSG(maMapEventsAt(sim, hx, hy, NULL) == 1,
                  "a hidden mine's terrain change was not published");

    /* Ground that will not take one, the same list the builder's mine order
       turns down. */
    maPaint(sim, hx, hy, BUILDING);
    op.u.mapPlaceMine.visible = true;
    op.u.mapPlaceMine.owner = MA_SLOT;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    maPaint(sim, hx, hy, HALFBUILDING);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    maPaint(sim, hx, hy, RIVER);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    maPaint(sim, hx, hy, BOAT);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    maPaint(sim, hx, hy, DEEP_SEA);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, hx, hy) == DEEP_SEA,
                  "a refused mine still wrote the square");

    /* And the six the builder will lay on. */
    {
        static const BYTE kMinable[] = { SWAMP, CRATER, ROAD, FOREST, RUBBLE,
                                         GRASS };
        int i;
        for (i = 0; i < (int)(sizeof(kMinable) / sizeof(kMinable[0])); i++) {
            maPaint(sim, hx, hy, kMinable[i]);
            UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                          "terrain %u would not take a mine",
                          (unsigned)kMinable[i]);
            UT_ASSERT(mapGetPos(&sim->sim.mp, hx, hy) ==
                      (BYTE)(kMinable[i] + MINE_SUBTRACT));
        }
    }

    /* A base owns its square and writes the terrain back, so it is no place
       for a mine. */
    memset(&firstBase, 0, sizeof(firstBase));
    basesGetBase(&sim->sim.bs, &firstBase, 1);
    op.u.mapPlaceMine.x = firstBase.x;
    op.u.mapPlaceMine.y = firstBase.y;
    maPaint(sim, firstBase.x, firstBase.y, GRASS);
    UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_TERRAIN,
                  "a base square took a mine");

    /* An owner that is neither a seat nor nobody. */
    op.u.mapPlaceMine.x = hx;
    op.u.mapPlaceMine.y = hy;
    maPaint(sim, hx, hy, GRASS);
    op.u.mapPlaceMine.owner = MAX_TANKS;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    op.u.mapPlaceMine.owner = 200;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_RANGE);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, hx, hy) == GRASS,
                  "a refused mine still wrote the square");

    /* Off the map. */
    op.u.mapPlaceMine.owner = MA_SLOT;
    op.u.mapPlaceMine.x = 0;
    op.u.mapPlaceMine.y = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapPlaceMine.x = MAP_MINE_EDGE_RIGHT;
    op.u.mapPlaceMine.y = 100;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);

    maDisarm();
    serverSimDestroy(sim);
    return 0;
}

/* ── Remove mine ─────────────────────────────────────────────────── */

int run_scenario_map_remove_mine(void) {
    ServerSim *sim = ut_make_running_sim("Tester");
    ScenarioOp place;
    ScenarioOp op;
    BYTE gx = 0, gy = 0;
    BYTE seen = 0;

    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(maFindRect(sim, 1, 1, &gx, &gy), "map has no free square");
    maPaint(sim, gx, gy, ROAD);

    memset(&place, 0, sizeof(place));
    place.type = SCN_OP_MAP_PLACE_MINE;
    place.u.mapPlaceMine.x = gx;
    place.u.mapPlaceMine.y = gy;
    place.u.mapPlaceMine.owner = MA_SLOT;
    place.u.mapPlaceMine.visible = true;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &place, NULL) == SCN_OP_OK);
    UT_ASSERT(mapGetPos(&sim->sim.mp, gx, gy) == (BYTE)(ROAD + MINE_SUBTRACT));

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_REMOVE_MINE;
    op.u.mapRemoveMine.x = gx;
    op.u.mapRemoveMine.y = gy;

    /* The ground comes back as it was under the mine — no crater, so nothing
       went off — the list forgets the square, and the change is published. */
    maArm(sim);
    sim->eventCount = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, gx, gy) == ROAD,
                  "the square reads %u, wanted the terrain under the mine",
                  (unsigned)mapGetPos(&sim->sim.mp, gx, gy));
    UT_ASSERT_MSG(sim->sim.mns->pos[gx][gy] == FALSE,
                  "the mine record outlived the mine");
    UT_ASSERT_MSG(minesGetOwner(&sim->sim.mns, gx, gy) == NEUTRAL,
                  "the mine's owner outlived the mine");
    UT_ASSERT_MSG(maMapEventsAt(sim, gx, gy, &seen) == 1 && seen == ROAD,
                  "the cleared square was not published as cleared");
    UT_ASSERT_MSG(maCountEvents(sim, EVENT_EXPLOSION) == 0,
                  "lifting a mine set it off");

    /* A square with no mine on it. */
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    maPaint(sim, gx, gy, DEEP_SEA);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    maPaint(sim, gx, gy, BUILDING);
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_NO_SUCH_ITEM);
    UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, gx, gy) == BUILDING,
                  "a refused remove still wrote the square");

    /* Every mined code comes back as the terrain under it. */
    {
        BYTE t;
        for (t = MINE_START; t <= MINE_END; t++) {
            maPaint(sim, gx, gy, t);
            UT_ASSERT_MSG(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK,
                          "mined terrain %u was not a mine", (unsigned)t);
            UT_ASSERT_MSG(mapGetPos(&sim->sim.mp, gx, gy) ==
                          (BYTE)(t - MINE_SUBTRACT),
                          "mined terrain %u came back as %u", (unsigned)t,
                          (unsigned)mapGetPos(&sim->sim.mp, gx, gy));
        }
    }

    /* Off the map. */
    op.u.mapRemoveMine.x = 0;
    op.u.mapRemoveMine.y = 0;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);
    op.u.mapRemoveMine.x = 100;
    op.u.mapRemoveMine.y = MAP_MINE_EDGE_BOTTOM;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_BAD_SQUARE);

    maDisarm();
    serverSimDestroy(sim);
    return 0;
}

/* ── The records the arms leave behind ───────────────────────────── */

/* The arms write no record of their own: every change rides the log_MapChange
 * mapSetPos already writes. This drives all four through a real recording and
 * reads the file back two ways — the .wbv's own event stream for the exact
 * logitem and payload, and the production reader for the world it rebuilds —
 * so a record that never reached the file shows up twice.
 *
 * The walk below is the writer-side wire shape (LOG_EVENT / LOG_EVENT_LONG,
 * then per event a type byte, a big-endian u16 length and that many payload
 * bytes), the same shape test_scenario_pill_base_arms.c walks. */

typedef struct {
    bool    found;
    uint8_t payload[8];
    int     payloadLen;
} MaLogHit;

static int maReadByte(const uint8_t *buf, size_t len, size_t pos) {
    if (pos >= len) return -1;
    return buf[pos];
}

/* Skip a snapshot body: startDelay+timeLimit, the count-prefixed pills,
 * bases and starts, the map runs up to the deep-sea terminator, then
 * MAX_TANKS player blocks. Plaintext, not length-framed. */
static bool maSkipSnapshot(const uint8_t *buf, size_t len, size_t *pos) {
    size_t p = *pos;
    int n, i;
    if (p + 8 > len) return false;
    p += 8;
    if ((n = maReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = maReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    if ((n = maReadByte(buf, len, p)) < 0) return false; p += 1 + (size_t)n;
    while (1) {
        int dlen, y, sx, ex;
        if (p + 4 > len) return false;
        dlen = maReadByte(buf, len, p);
        y    = maReadByte(buf, len, p + 1);
        sx   = maReadByte(buf, len, p + 2);
        ex   = maReadByte(buf, len, p + 3);
        p += 4;
        if (dlen == 4 && y == 255 && sx == 255 && ex == 255) break;
        if (dlen < 4) return false;
        p += (size_t)(dlen - 4);
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if ((n = maReadByte(buf, len, p)) < 0) return false;
        p += 1 + (size_t)n;
    }
    *pos = p;
    return true;
}

/* Walk the .wbv's event stream and report the LAST log_MapChange naming the
 * square (x, y). The last rather than the first because a round writes the
 * terrain of a square more than once — the world keeps moving around the op —
 * and what each assertion is asking about is the state the ops left behind.
 * Returns false if the stream did not end on a clean LOG_QUIT. */
static bool maFindMapChange(const char *path, uint8_t x, uint8_t y,
                            MaLogHit *hit) {
    uint8_t *buf = NULL;
    size_t   len = 0;
    size_t   pos;
    bool     ok = false;

    memset(hit, 0, sizeof(*hit));
    if (!extractLogDat(path, &buf, &len)) return false;
    /* Header: WBOLOMOV(8) + version(1) + mapname pstr + game(8) + addr(4) +
       port(2) + time(4) + WBN key(32). */
    if (len < 10 || memcmp(buf, "WBOLOMOV", 8) != 0 || buf[8] != LOG_VERSION) {
        free(buf);
        return false;
    }
    pos = 8 + 1;
    pos += 1 + buf[pos];
    pos += 8 + 4 + 2 + 4 + 32;

    while (pos < len) {
        int code = maReadByte(buf, len, pos);
        pos++;
        if (code < 0) break;
        if (code == LOG_QUIT) {
            ok = true;
            break;
        } else if (code == LOG_NOEVENTS) {
            if (maReadByte(buf, len, pos) < 0) break;
            pos += 1;
        } else if (code == LOG_NOEVENTS_LONG) {
            if (pos + 2 > len) break;
            pos += 2;
        } else if (code == LOG_EVENT_SNAPSHOT) {
            if (!maSkipSnapshot(buf, len, &pos)) break;
        } else if (code == LOG_EVENT || code == LOG_EVENT_LONG) {
            int n, i;
            if (code == LOG_EVENT) {
                n = maReadByte(buf, len, pos);
                pos += 1;
                if (n < 0) break;
            } else {
                if (pos + 2 > len) break;
                /* The writer stores data[1]=low, data[2]=high and the reader
                   rebuilds the count as (lo << 8) | hi. */
                n = (buf[pos + 1] << 8) | buf[pos];
                pos += 2;
            }
            for (i = 0; i < n; i++) {
                int ev, plen;
                size_t payloadStart;
                if (pos + 3 > len) { n = -1; break; }
                ev   = buf[pos];
                plen = (buf[pos + 1] << 8) | buf[pos + 2];
                payloadStart = pos + 3;
                if (payloadStart + (size_t)plen > len) { n = -1; break; }
                if (ev == (int)log_MapChange && plen >= 2 &&
                    buf[payloadStart] == x && buf[payloadStart + 1] == y) {
                    int copy = plen;
                    if (copy > (int)sizeof(hit->payload)) {
                        copy = (int)sizeof(hit->payload);
                    }
                    hit->found = true;
                    hit->payloadLen = plen;
                    memcpy(hit->payload, buf + payloadStart, (size_t)copy);
                }
                pos = payloadStart + (size_t)plen;
            }
            if (n < 0) break;
        } else {
            break;
        }
    }

    free(buf);
    return ok;
}

int run_scenario_map_arm_records(void) {
    ReplayHarness h;
    ServerSim *sim;
    ScenarioOp op;
    MaLogHit hit;
    BYTE rx = 0, ry = 0;
    BYTE tileX, tileY, fillX, fillY, mineX, mineY, liftX, liftY;

    memset(&h, 0, sizeof(h));
    UT_ASSERT_MSG(replayHarnessStartRecording(&h, "scnMapArms", "Tester"),
                  "could not start recording");
    sim = h.sim;

    /* Let the round settle after the opening snapshot. */
    replayHarnessTick(&h, 4);

    /* Five squares in a row, one per arm and one the fill is asked for. */
    UT_ASSERT_MSG(maFindRect(sim, 5, 1, &rx, &ry),
                  "map has no free run of five squares");
    tileX = rx;              tileY = ry;
    fillX = (BYTE)(rx + 1);  fillY = ry;   /* the fill covers this one and the next */
    mineX = (BYTE)(rx + 3);  mineY = ry;
    liftX = (BYTE)(rx + 4);  liftY = ry;
    maPaint(sim, tileX, tileY, GRASS);
    maPaint(sim, fillX, fillY, GRASS);
    maPaint(sim, (BYTE)(rx + 2), ry, GRASS);
    maPaint(sim, mineX, mineY, ROAD);
    maPaint(sim, liftX, liftY, SWAMP);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_SET_TILE;
    op.u.mapSetTile.x = tileX;
    op.u.mapSetTile.y = tileY;
    op.u.mapSetTile.terrain = RUBBLE;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_FILL_RECT;
    op.u.mapFillRect.x0 = fillX;
    op.u.mapFillRect.y0 = fillY;
    op.u.mapFillRect.x1 = (BYTE)(rx + 2);
    op.u.mapFillRect.y1 = ry;
    op.u.mapFillRect.terrain = CRATER;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_PLACE_MINE;
    op.u.mapPlaceMine.x = mineX;
    op.u.mapPlaceMine.y = mineY;
    op.u.mapPlaceMine.owner = MA_SLOT;
    op.u.mapPlaceMine.visible = true;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* The square the remove arm lifts from is mined first, so the record the
       walk finds for it is the remove's rather than the lay's. */
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_PLACE_MINE;
    op.u.mapPlaceMine.x = liftX;
    op.u.mapPlaceMine.y = liftY;
    op.u.mapPlaceMine.owner = MA_SLOT;
    op.u.mapPlaceMine.visible = false;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_REMOVE_MINE;
    op.u.mapRemoveMine.x = liftX;
    op.u.mapRemoveMine.y = liftY;
    UT_ASSERT(serverSimApplyScenarioOp(sim, &op, NULL) == SCN_OP_OK);

    /* Ticks so the last change reaches the file. */
    replayHarnessTick(&h, 4);
    UT_ASSERT_MSG(replayHarnessStopRecording(&h), "could not stop recording");

    /* log_MapChange: x, y, terrain — one per arm. */
    UT_ASSERT_MSG(maFindMapChange(h.path, tileX, tileY, &hit),
                  "the recording did not end on a clean quit: %s", h.path);
    UT_ASSERT_MSG(hit.found, "set-tile left no log_MapChange behind");
    UT_ASSERT_MSG(hit.payloadLen == 3, "log_MapChange payload is %d bytes",
                  hit.payloadLen);
    UT_ASSERT_MSG(hit.payload[2] == RUBBLE,
                  "set-tile recorded terrain %u", (unsigned)hit.payload[2]);

    UT_ASSERT(maFindMapChange(h.path, fillX, fillY, &hit));
    UT_ASSERT_MSG(hit.found, "fill left no log_MapChange behind");
    UT_ASSERT_MSG(hit.payload[2] == CRATER,
                  "fill recorded terrain %u", (unsigned)hit.payload[2]);

    UT_ASSERT(maFindMapChange(h.path, mineX, mineY, &hit));
    UT_ASSERT_MSG(hit.found, "place-mine left no log_MapChange behind");
    UT_ASSERT_MSG(hit.payload[2] == (BYTE)(ROAD + MINE_SUBTRACT),
                  "place-mine recorded terrain %u", (unsigned)hit.payload[2]);

    UT_ASSERT(maFindMapChange(h.path, liftX, liftY, &hit));
    UT_ASSERT_MSG(hit.found, "remove-mine left no log_MapChange behind");
    UT_ASSERT_MSG(hit.payload[2] == SWAMP,
                  "remove-mine recorded terrain %u", (unsigned)hit.payload[2]);

    /* And the world the production reader rebuilds from those records agrees
       with the world the ops left behind. */
    UT_ASSERT_MSG(replayHarnessDecode(&h), "could not decode the recording");
    UT_ASSERT_MSG(replayHarnessCompare(&h),
                  "the replay disagrees with the sim: %s",
                  replayHarnessDiff(&h));

    replayHarnessStop(&h);
    return 0;
}
