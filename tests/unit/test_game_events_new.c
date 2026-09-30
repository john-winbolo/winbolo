/*
 * The eight game events the engine kept to itself until now: a tank spawning,
 * a builder landing, a pill placed, picked up and killed, a building job
 * finished, a mine laid and a mine going up.
 *
 * Each is raised by a GameSim callback implemented in
 * server_sim_callbacks.c, so the sim core announces the fact and the server
 * decides it is an event. The cases below drive the engine function that
 * causes each fact and read the event off a subscriber's game-event channel.
 * Expected bytes are written out by hand at every assertion rather than read
 * back through the emit.
 *
 * EVENT_MINE_PLACED is the one that must never be serialized: it names the
 * square a mine went into, which is what hidden mines exist to withhold. It
 * is raised like any other event and dropped at both drain sites by
 * gameEventIsLocal, so the host's subscriber and the god-view recording build
 * see it and no client's snapshot does — asserted here against a real
 * snapshot build, both ways round.
 *
 * Indices are the 0-based item[] slot. pillsGetPillNum counts from one and
 * lgmDoWork carries the pill number counted from one, so the pill cases
 * assert index 0 and index count - 1 at both ends rather than trusting the
 * middle of the range.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* eventCount — drained between steps */
#include "control_event.h"
#include "client_sim.h"
#include "game_sim.h"
#include "input_packet.h"
#include "bolo_map.h"              /* mapSetPos / mapGetPos */
#include "bases.h"
#include "pillbox.h"
#include "starts.h"
#include "mines.h"
#include "minesexp.h"
#include "tank.h"
#include "lgm.h"
#include "test_harness.h"

#define GE_SLOT  0   /* the player every case drives */
#define GE_OTHER 1   /* a second slot, for an attacker or a late join */

/* ----------------------------------------------------------------
 * A subscriber that records the game-event channel.
 * ---------------------------------------------------------------- */

#define GE_MAX_KEPT 64

typedef struct {
    int       controlCount;
    int       eventCount;
    GameEvent events[GE_MAX_KEPT];
} GeSink;

static void geDeliverControl(void *ctx, const ControlEvent *evt) {
    GeSink *s = (GeSink *)ctx;
    (void)evt;
    s->controlCount++;
}

static void geDeliverEvent(void *ctx, const GameEvent *evt) {
    GeSink *s = (GeSink *)ctx;
    if (s->eventCount < GE_MAX_KEPT) {
        s->events[s->eventCount] = *evt;
    }
    s->eventCount++;
}

static int geKept(const GeSink *s) {
    return s->eventCount < GE_MAX_KEPT ? s->eventCount : GE_MAX_KEPT;
}

static int geCount(const GeSink *s, BYTE type) {
    int found = 0;
    int i;
    for (i = 0; i < geKept(s); i++) {
        if (s->events[i].type == type) found++;
    }
    return found;
}

static const GameEvent *geFind(const GeSink *s, BYTE type) {
    int i;
    for (i = 0; i < geKept(s); i++) {
        if (s->events[i].type == type) return &s->events[i];
    }
    return NULL;
}

static SubscriberHandle geSubscribe(ServerSim *sim, GeSink *s) {
    SubscriberHandle h;
    memset(s, 0, sizeof(*s));
    h = serverSimRegisterSubscriber(sim, geDeliverControl, s);
    if (h != SUBSCRIBER_HANDLE_INVALID) {
        if (!serverSimSetSubscriberEventDeliver(sim, h, geDeliverEvent)) {
            return SUBSCRIBER_HANDLE_INVALID;
        }
    }
    memset(s, 0, sizeof(*s));
    return h;
}

static void geDrain(ServerSim *sim, GeSink *s) {
    sim->eventCount = 0;
    memset(s, 0, sizeof(*s));
}

static int geFirstDiff(const GameEvent *ev, const BYTE *want, int n) {
    int i;
    for (i = 0; i < n; i++) {
        if (ev->data[i] != want[i]) return i;
    }
    return -1;
}

/* Compare the bytes the event is defined to carry. `n` is written out at the
 * call, not read from gameEventDataSize. */
#define GE_ASSERT_BYTES(ev, want, n, what)                                   \
    do {                                                                     \
        int _d = geFirstDiff((ev), (want), (n));                             \
        int _at = _d < 0 ? 0 : _d;                                           \
        UT_ASSERT_MSG(_d < 0, "%s: data[%d] = %u, want %u", (what), _at,     \
                      (unsigned)(ev)->data[_at], (unsigned)(want)[_at]);     \
    } while (0)

/* ----------------------------------------------------------------
 * Map and builder helpers.
 * ---------------------------------------------------------------- */

/* A square inside the minable area holding `want` terrain, with no pill, base
 * or mine on it and no tank standing there. */
static bool geFindTile(GameSim *gs, BYTE want, BYTE *ox, BYTE *oy) {
    int x, y;
    BYTE ax = gs->tanks[GE_SLOT] != NULL ? tankGetMX(&gs->tanks[GE_SLOT]) : 0xFF;
    BYTE ay = gs->tanks[GE_SLOT] != NULL ? tankGetMY(&gs->tanks[GE_SLOT]) : 0xFF;
    for (y = MAP_MINE_EDGE_TOP + 1; y < MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x < MAP_MINE_EDGE_RIGHT; x++) {
            if ((BYTE)x == ax && (BYTE)y == ay) continue;
            if (mapGetPos(&gs->mp, (BYTE)x, (BYTE)y) == want &&
                !pillsExistPos(&gs->pb, (BYTE)x, (BYTE)y) &&
                !basesExistPos(&gs->bs, (BYTE)x, (BYTE)y) &&
                !minesExistPos(&gs->mns, &gs->mp, (BYTE)x, (BYTE)y)) {
                *ox = (BYTE)x;
                *oy = (BYTE)y;
                return true;
            }
        }
    }
    return false;
}

/* Put the builder on a square with a job in hand, ready for lgmDoWork. */
static void geBuilderAt(GameSim *gs, BYTE slot, BYTE mx, BYTE my, BYTE action) {
    lgm *l = &gs->lgmen[slot];
    (*l)->destX = (WORLD)((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->destY = (WORLD)((my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->x = (*l)->destX;
    (*l)->y = (*l)->destY;
    (*l)->action = action;
    (*l)->isDead = FALSE;
    (*l)->inTank = FALSE;
}

/* ================================================================
 * 1. A tank spawning, and one coming back.
 * ================================================================ */
int run_game_events_tank_spawned(void) {
    ServerSim *sim = ut_make_running_sim("Spawner");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* A join makes a tank, which is a spawn and not a respawn. */
    geDrain(sim, &sink);
    serverSimAddPlayer(sim, GE_OTHER, "Joiner", false);
    UT_ASSERT_MSG(geCount(&sink, EVENT_TANK_SPAWNED) == 1,
                  "a join published %d spawn events",
                  geCount(&sink, EVENT_TANK_SPAWNED));
    ev = geFind(&sink, EVENT_TANK_SPAWNED);
    UT_ASSERT(ev != NULL);
    UT_ASSERT(gs->tanks[GE_OTHER] != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GE_OTHER;
    want[1] = tankGetMX(&gs->tanks[GE_OTHER]);
    want[2] = tankGetMY(&gs->tanks[GE_OTHER]);
    want[3] = 0;                       /* a first spawn, not a respawn */
    GE_ASSERT_BYTES(ev, want, 4, "tank spawned on join");

    /* A death that comes back is the same event with the last byte set. */
    geDrain(sim, &sink);
    tankDeath(gs, &gs->tanks[GE_SLOT]);

    UT_ASSERT_MSG(geCount(&sink, EVENT_TANK_SPAWNED) == 1,
                  "a respawn published %d spawn events",
                  geCount(&sink, EVENT_TANK_SPAWNED));
    ev = geFind(&sink, EVENT_TANK_SPAWNED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = tankGetMX(&gs->tanks[GE_SLOT]);
    want[2] = tankGetMY(&gs->tanks[GE_SLOT]);
    want[3] = 1;                       /* a respawn */
    GE_ASSERT_BYTES(ev, want, 4, "tank respawned");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. A builder finishing his flight back.
 * ================================================================ */
int run_game_events_lgm_landed(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE mx = 0, my = 0;
    lgm *l;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(gs->lgmen[GE_SLOT] != NULL);
    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Standing on his destination, so the arrival test fires this call. */
    l = &gs->lgmen[GE_SLOT];
    (*l)->destX = (WORLD)((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->destY = (WORLD)((my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    (*l)->x = (*l)->destX;
    (*l)->y = (*l)->destY;
    (*l)->isDead = TRUE;
    (*l)->frame = LGM_HELICOPTER_FRAME;

    geDrain(sim, &sink);
    lgmParchutingIn(gs, l);

    UT_ASSERT_MSG(geCount(&sink, EVENT_LGM_LANDED) == 1,
                  "the landing published %d events",
                  geCount(&sink, EVENT_LGM_LANDED));
    ev = geFind(&sink, EVENT_LGM_LANDED);
    UT_ASSERT(ev != NULL);

    /* The square he reached — not, as the death event does, the one he was
       sent to fly in from. */
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = mx;
    want[2] = my;
    GE_ASSERT_BYTES(ev, want, 3, "builder landed");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A pill put down, at both ends of the list.
 * ================================================================ */
static int gePlaceOne(ServerSim *sim, GeSink *sink, BYTE pillNumber,
                      BYTE *outMx, BYTE *outMy) {
    GameSim *gs = serverSimGetGameSim(sim);
    lgm *l = &gs->lgmen[GE_SLOT];
    BYTE mx = 0, my = 0;

    if (!geFindTile(gs, GRASS, &mx, &my)) return -1;
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_PILL_REQUEST);
    (*l)->numPills = pillNumber;     /* counted from one, as the engine has it */
    (*l)->numTrees = 0;
    geDrain(sim, sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);
    *outMx = mx;
    *outMy = my;
    return 0;
}

int run_game_events_pill_placed(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE mx = 0, my = 0;
    BYTE np;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    np = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(np >= 2, "map has %u pills, this case needs two",
                  (unsigned)np);

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Pill number 1 is item[0]. */
    UT_ASSERT(gePlaceOne(sim, &sink, 1, &mx, &my) == 0);
    UT_ASSERT_MSG(geCount(&sink, EVENT_PILL_PLACED) == 1,
                  "placing pill 1 published %d events",
                  geCount(&sink, EVENT_PILL_PLACED));
    ev = geFind(&sink, EVENT_PILL_PLACED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = 0;                     /* index, 0-based */
    want[2] = mx;
    want[3] = my;
    GE_ASSERT_BYTES(ev, want, 4, "pill 1 placed");

    /* The last pill is item[np - 1], the end an off-by-one shows at. */
    UT_ASSERT(gePlaceOne(sim, &sink, np, &mx, &my) == 0);
    ev = geFind(&sink, EVENT_PILL_PLACED);
    UT_ASSERT_MSG(ev != NULL, "placing pill %u published nothing",
                  (unsigned)np);

    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = (BYTE)(np - 1);
    want[2] = mx;
    want[3] = my;
    GE_ASSERT_BYTES(ev, want, 4, "last pill placed");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. A pill scooped into a tank, at both ends of the list.
 * ================================================================ */
int run_game_events_pill_picked_up(void) {
    ServerSim *sim = ut_make_running_sim("Picker");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE np;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    np = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(np >= 2, "map has %u pills, this case needs two",
                  (unsigned)np);

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* tankTakePill's argument is the pillbox number, counted from one; the
       event carries the array slot. Pill 1 is index 0. */
    geDrain(sim, &sink);
    tankTakePill(gs, &gs->tanks[GE_SLOT], 1);
    UT_ASSERT_MSG(geCount(&sink, EVENT_PILL_PICKED_UP) == 1,
                  "picking pill 1 published %d events",
                  geCount(&sink, EVENT_PILL_PICKED_UP));
    ev = geFind(&sink, EVENT_PILL_PICKED_UP);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = 0;
    GE_ASSERT_BYTES(ev, want, 2, "pill 1 picked up");

    geDrain(sim, &sink);
    tankTakePill(gs, &gs->tanks[GE_SLOT], np);
    ev = geFind(&sink, EVENT_PILL_PICKED_UP);
    UT_ASSERT_MSG(ev != NULL, "picking pill %u published nothing",
                  (unsigned)np);

    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = (BYTE)(np - 1);
    GE_ASSERT_BYTES(ev, want, 2, "last pill picked up");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. A pill shelled flat, from both damage paths.
 * ================================================================ */
int run_game_events_pill_killed(void) {
    ServerSim *sim = ut_make_running_sim("Shooter");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE np;
    pillbox p;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, GE_OTHER, "Other", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    np = pillsGetNumPills(&gs->pb);
    UT_ASSERT_MSG(np >= 2, "map has %u pills, this case needs two",
                  (unsigned)np);

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Pill 1, on one armour, shelled by a named attacker. */
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, 1);
    (*gs->pb).item[0].armour = 1;
    (*gs->pb).item[0].inTank = FALSE;
    geDrain(sim, &sink);
    UT_ASSERT(pillsDamagePos(gs, p.x, p.y, TRUE, FALSE, GE_OTHER, DMG_NO_PILL) == TRUE);

    UT_ASSERT_MSG(geCount(&sink, EVENT_PILL_KILLED) == 1,
                  "the shell published %d kill events",
                  geCount(&sink, EVENT_PILL_KILLED));
    ev = geFind(&sink, EVENT_PILL_KILLED);
    UT_ASSERT(ev != NULL);

    memset(want, 0, sizeof(want));
    want[0] = 0;                     /* index, 0-based */
    want[1] = GE_OTHER;
    GE_ASSERT_BYTES(ev, want, 2, "pill 1 killed by a shell");

    /* A second blow on the same dead pill kills nothing twice. */
    geDrain(sim, &sink);
    (void)pillsDamagePos(gs, p.x, p.y, TRUE, FALSE, GE_OTHER, DMG_NO_PILL);
    UT_ASSERT_MSG(geCount(&sink, EVENT_PILL_KILLED) == 0,
                  "a pill already dead was killed again");

    /* The last pill, through the splash path, which names nobody. */
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, np);
    (*gs->pb).item[np - 1].armour = 2;
    (*gs->pb).item[np - 1].inTank = FALSE;
    geDrain(sim, &sink);
    pillsGetDamagePos(gs, &gs->pb, p.x, p.y, 5, NEUTRAL);

    ev = geFind(&sink, EVENT_PILL_KILLED);
    UT_ASSERT_MSG(ev != NULL, "the explosion published no kill event");

    memset(want, 0, sizeof(want));
    want[0] = (BYTE)(np - 1);
    want[1] = NEUTRAL;
    GE_ASSERT_BYTES(ev, want, 2, "last pill killed by splash");

    /* And the splash path does not kill an already-dead pill either. */
    geDrain(sim, &sink);
    pillsGetDamagePos(gs, &gs->pb, p.x, p.y, 5, NEUTRAL);
    UT_ASSERT_MSG(geCount(&sink, EVENT_PILL_KILLED) == 0,
                  "splash on a dead pill published a kill");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 6. A finished building job, from each of the five arms.
 * ================================================================ */
int run_game_events_built(void) {
    ServerSim *sim = ut_make_running_sim("Builder");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE mx = 0, my = 0;
    lgm *l;
    pillbox p;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    l = &gs->lgmen[GE_SLOT];

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* Harvesting trees. The action byte is the builder's own request code. */
    UT_ASSERT_MSG(geFindTile(gs, FOREST, &mx, &my), "map has no free forest");
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_TREE_REQUEST);
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);
    ev = geFind(&sink, EVENT_BUILT);
    UT_ASSERT_MSG(ev != NULL, "the harvest published nothing");
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = LGM_TREE_REQUEST;
    want[2] = mx;
    want[3] = my;
    GE_ASSERT_BYTES(ev, want, 4, "trees harvested");

    /* Road. */
    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_ROAD_REQUEST);
    (*l)->numTrees = LGM_GATHER_TREE;
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);
    ev = geFind(&sink, EVENT_BUILT);
    UT_ASSERT_MSG(ev != NULL, "the road published nothing");
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = LGM_ROAD_REQUEST;
    want[2] = mx;
    want[3] = my;
    GE_ASSERT_BYTES(ev, want, 4, "road built");

    /* Wall. */
    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_BUILDING_REQUEST);
    (*l)->numTrees = LGM_GATHER_TREE;
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);
    ev = geFind(&sink, EVENT_BUILT);
    UT_ASSERT_MSG(ev != NULL, "the wall published nothing");
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = LGM_BUILDING_REQUEST;
    want[2] = mx;
    want[3] = my;
    GE_ASSERT_BYTES(ev, want, 4, "wall built");

    /* Boat, which needs river under it. */
    UT_ASSERT_MSG(geFindTile(gs, RIVER, &mx, &my), "map has no free river");
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_BOAT_REQUEST);
    (*l)->numTrees = LGM_GATHER_TREE;
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);
    ev = geFind(&sink, EVENT_BUILT);
    UT_ASSERT_MSG(ev != NULL, "the boat published nothing");
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = LGM_BOAT_REQUEST;
    want[2] = mx;
    want[3] = my;
    GE_ASSERT_BYTES(ev, want, 4, "boat built");

    /* Repairing a pill, which is the pill job with empty hands. Placing one
       is EVENT_PILL_PLACED instead, so this action byte is never ambiguous. */
    memset(&p, 0, sizeof(p));
    pillsGetPill(&gs->pb, &p, 1);
    (*gs->pb).item[0].armour = 1;
    (*gs->pb).item[0].inTank = FALSE;
    geBuilderAt(gs, GE_SLOT, p.x, p.y, LGM_PILL_REQUEST);
    (*l)->numPills = LGM_NO_PILL;
    (*l)->numTrees = LGM_GATHER_TREE;
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);
    ev = geFind(&sink, EVENT_BUILT);
    UT_ASSERT_MSG(ev != NULL, "the repair published nothing");
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = LGM_PILL_REQUEST;
    want[2] = p.x;
    want[3] = p.y;
    GE_ASSERT_BYTES(ev, want, 4, "pill repaired");
    UT_ASSERT_MSG(geCount(&sink, EVENT_PILL_PLACED) == 0,
                  "a repair published a placement");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 7. A mine laid, by a builder and by a tank.
 * ================================================================ */
int run_game_events_mine_laid(void) {
    ServerSim *sim = ut_make_running_sim("Layer");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE mx = 0, my = 0;
    lgm *l;

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    l = &gs->lgmen[GE_SLOT];

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* The builder's mine arm. */
    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_MINE_REQUEST);
    (*l)->numMines = 1;
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);

    UT_ASSERT_MSG(geCount(&sink, EVENT_MINE_PLACED) == 1,
                  "the builder's mine published %d events",
                  geCount(&sink, EVENT_MINE_PLACED));
    ev = geFind(&sink, EVENT_MINE_PLACED);
    UT_ASSERT(ev != NULL);
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = mx;
    want[2] = my;
    GE_ASSERT_BYTES(ev, want, 3, "builder laid a mine");

    /* And the tank's. Put the tank on a square it may mine, off its boat. */
    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");
    gs->tanks[GE_SLOT]->x =
        (WORLD)((mx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    gs->tanks[GE_SLOT]->y =
        (WORLD)((my << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
    gs->tanks[GE_SLOT]->onBoat = FALSE;
    gs->tanks[GE_SLOT]->destroyed = FALSE;
    gs->tanks[GE_SLOT]->mines = 5;
    geDrain(sim, &sink);
    tankLayMine(gs, &gs->tanks[GE_SLOT]);

    UT_ASSERT_MSG(geCount(&sink, EVENT_MINE_PLACED) == 1,
                  "the tank's mine published %d events",
                  geCount(&sink, EVENT_MINE_PLACED));
    ev = geFind(&sink, EVENT_MINE_PLACED);
    UT_ASSERT(ev != NULL);
    memset(want, 0, sizeof(want));
    want[0] = GE_SLOT;
    want[1] = mx;
    want[2] = my;
    GE_ASSERT_BYTES(ev, want, 3, "tank laid a mine");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 8. A mine going up, carrying who laid it.
 * ================================================================ */
int run_game_events_mine_exploded(void) {
    ServerSim *sim = ut_make_running_sim("Layer");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    const GameEvent *ev;
    BYTE want[GAME_EVENT_MAX_DATA];
    BYTE mx = 0, my = 0;
    BYTE terrain;
    lgm *lgmPtrs[1];
    tank tanksArray[1];

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, GE_OTHER, "Other", false);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    /* A mine on the square, laid by the other slot, so the layer byte is not
       the same number as anything else in the payload. */
    terrain = mapGetPos(&gs->mp, mx, my);
    mapSetPos(gs, &gs->mp, mx, my, (BYTE)(terrain + MINE_SUBTRACT), FALSE, FALSE);
    minesAddItem(&gs->mns, mx, my);
    minesSetOwner(&gs->mns, mx, my, GE_OTHER);

    lgmPtrs[0] = &gs->lgmen[GE_SLOT];
    tanksArray[0] = gs->tanks[GE_SLOT];

    geDrain(sim, &sink);
    minesExpCheckFill(gs, lgmPtrs, 1, mx, my, tanksArray, &gs->ss);

    UT_ASSERT_MSG(geCount(&sink, EVENT_MINE_EXPLODED) == 1,
                  "the explosion published %d events",
                  geCount(&sink, EVENT_MINE_EXPLODED));
    ev = geFind(&sink, EVENT_MINE_EXPLODED);
    UT_ASSERT(ev != NULL);

    /* The layer is read before the mine is taken off the field, so it is the
       slot that laid it and not the NEUTRAL the square holds afterwards. */
    memset(want, 0, sizeof(want));
    want[0] = mx;
    want[1] = my;
    want[2] = GE_OTHER;
    GE_ASSERT_BYTES(ev, want, 3, "mine exploded");

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 9. A mine a script laid reaches the host and no client.
 * ================================================================ */
int run_game_events_mine_placed_is_local(void) {
    ServerSim *sim = ut_make_running_sim("Layer");
    GameSim *gs;
    GeSink sink;
    SubscriberHandle h;
    BYTE mx = 0, my = 0;
    lgm *l;
    int i;
    int inClient = 0;
    int inRecording = 0;

    /* The snapshot build's outputs. Only the event array is read. */
    SnapshotHeader      hdr;
    TankSnapshot        tanks[MAX_TANKS];
    ShellSnapshot       shells[MAX_SNAPSHOT_SHELLS];
    TkExplosionSnapshot tkexp[MAX_SNAPSHOT_TK_EXPLOSIONS];
    BaseSnapshot        bases[MAX_SNAPSHOT_BASES];
    PillSnapshot        pills[MAX_SNAPSHOT_PILLS];
    GameEvent           evts[MAX_SNAPSHOT_EVENTS];

    UT_ASSERT(sim != NULL);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    gs->hiddenMines = true;
    l = &gs->lgmen[GE_SLOT];

    h = geSubscribe(sim, &sink);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    UT_ASSERT_MSG(geFindTile(gs, GRASS, &mx, &my), "map has no free grass");
    geBuilderAt(gs, GE_SLOT, mx, my, LGM_MINE_REQUEST);
    (*l)->numMines = 1;
    geDrain(sim, &sink);
    lgmDoWork(gs, l, &gs->tanks[GE_SLOT]);

    /* The host hears it: the subscriber channel is not the wire. */
    UT_ASSERT_MSG(geCount(&sink, EVENT_MINE_PLACED) == 1,
                  "the host heard %d mine events",
                  geCount(&sink, EVENT_MINE_PLACED));

    /* A client's snapshot does not carry it. This is the build both the wire
       transport and the in-process one use, with the same argument. */
    memset(evts, 0, sizeof(evts));
    serverSimBuildSnapshot(sim, GE_SLOT, &hdr,
                           tanks, MAX_TANKS,
                           shells, MAX_SNAPSHOT_SHELLS,
                           tkexp, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bases, MAX_SNAPSHOT_BASES,
                           pills, MAX_SNAPSHOT_PILLS,
                           evts, MAX_SNAPSHOT_EVENTS,
                           false);
    for (i = 0; i < MAX_SNAPSHOT_EVENTS; i++) {
        if (evts[i].type == EVENT_MINE_PLACED) inClient++;
    }
    UT_ASSERT_MSG(inClient == 0,
                  "a hidden mine reached a client snapshot %d times", inClient);

    /* The god-view build, which is what a recording takes, does carry it. */
    memset(evts, 0, sizeof(evts));
    serverSimBuildSnapshot(sim, GE_SLOT, &hdr,
                           tanks, MAX_TANKS,
                           shells, MAX_SNAPSHOT_SHELLS,
                           tkexp, MAX_SNAPSHOT_TK_EXPLOSIONS,
                           bases, MAX_SNAPSHOT_BASES,
                           pills, MAX_SNAPSHOT_PILLS,
                           evts, MAX_SNAPSHOT_EVENTS,
                           true);
    for (i = 0; i < MAX_SNAPSHOT_EVENTS; i++) {
        if (evts[i].type == EVENT_MINE_PLACED) inRecording++;
    }
    UT_ASSERT_MSG(inRecording == 1,
                  "the recording build carried the mine %d times", inRecording);

    /* And the table says so on its own, which is what both drains read. */
    UT_ASSERT(gameEventIsLocal(EVENT_MINE_PLACED) == true);
    UT_ASSERT(gameEventIsLocal(EVENT_MINE_EXPLODED) == false);
    UT_ASSERT(gameEventIsLocal(EVENT_TANK_SPAWNED) == false);

    serverSimUnregisterSubscriber(sim, h);
    serverSimDestroy(sim);
    return 0;
}
