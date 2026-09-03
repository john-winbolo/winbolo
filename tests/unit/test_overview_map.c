/*
 * The overview map memory (test_overview_map.c).
 *
 * run_overview_regions is the module's pure pieces. overviewMapBuildRegions
 * turns the local player's tank position and the pillboxes they can view
 * through into the set of map squares the overview treats as live, and this
 * pins the shape of that set — a 29x29 block on the tank and a 15x15 block on
 * each viewable pill, trimmed at the map edges, the tank rect always first and
 * pills after it in index order, and never more rects than the caller asked
 * for. overviewMapDeathTankHalf is the schedule the tank's own block shrinks
 * on while it is dead: whole until the tick the classic view cuts to static,
 * closing from there and gone by the end of the close, never widening on the
 * way down, and a drowning on a later clock than a shell.
 * overviewMapDeathStatic is where the overview's own static sits in that same
 * wait — the last stretch of it, ending with the respawn, and starting only
 * after the block has closed, so the map goes dark before the snow comes up.
 *
 * The rest drive a real ClientSim through clientSimDisplayTick and check what
 * the memory ends up holding: a map install seeds every square dimmed on the
 * next tick and only live squares are bright over it, a square outside the
 * live set keeps the tile it was seeded or last stamped with and never picks
 * up a later terrain change the client already knows about, a region that
 * stops being live is stamped once more on the way out so a pill freezes dead
 * or in the captor's colours rather than a tick stale, a tank waiting to
 * respawn holds its block on the square it died on and then closes it over the
 * wreck rather than either dropping it at once or holding it to the respawn,
 * and a round reset clears the lot while a mid-game map resync leaves it
 * alone and a fresh install re-seeds it.
 *
 * One case steps outside the memory to the accessor that feeds it,
 * clientSimGetMyTankMapPos, and pins what it reports for a tank that is dead
 * and waiting to respawn.
 *
 * Another steps forward to what the memory is for: the per-frame entity lists
 * the overview draws from cover the whole map, so an enemy tank the client
 * still knows about has to be dropped by the live-square filter once it
 * leaves the block the player can see, while the player's own tank is drawn
 * wherever it is.
 *
 * One more covers the accessor the overview draws its own crosshair from,
 * clientSimGetGunsightPos: it declines for a hidden sight and for a tank that
 * is dead and waiting to respawn, where clientSimGetGunsightTile still answers
 * with the map origin.
 *
 * The last case runs the reveal checks again over the real UDP transport, where
 * the map arrives as a download and terrain changes arrive out of band as map
 * events, so the mask does not quietly depend on the client and the server
 * sharing a process.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h" /* CLIENT_CONNECT_CONNECTED */
#include "client_sim_internal.h" /* installCompressedMap — the resync path */
#include "input_packet.h"
#include "transport_udp.h" /* the server-side staged map event and its readiness flag */
#include "overview_map.h"
#include "viewport.h" /* viewportCalcSquarePure — the only tile source */
#include "bolo_map.h" /* mapGetPos / mapSetPos — plant a raw terrain byte */
#include "tank.h"     /* tankSetWorld / tankDestroy */
#include "bases.h"
#include "pillbox.h"
#include "players.h"
#include "allience.h"
#include "everard_map.h" /* E_MAP — the blob the resync reinstalls */
#include "overview_camera.h" /* overviewEntityIsVisible — the drawing filter */
#include "test_harness.h"
#include "loopback_harness.h"

/* Fails the calling test unless the rect is exactly these edges. */
#define ASSERT_RECT(r, l, t, rt, b)                                           \
    UT_ASSERT_MSG((r).left == (l) && (r).top == (t) && (r).right == (rt) &&    \
                      (r).bottom == (b),                                      \
                  "rect {%d,%d,%d,%d}, expected {%d,%d,%d,%d}",                \
                  (r).left, (r).top, (r).right, (r).bottom, (l), (t), (rt), (b))

int run_overview_regions(void) {
    ServerSim *sim = ut_make_running_sim("Over");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT_MSG(gs != NULL, "serverSimGetGameSim returned NULL");

    /* Slots 1 and 2 join; 0 allies with 1 and leaves 2 hostile. The map's own
     * pills are cleared out so every case below places exactly what it needs. */
    serverSimAddPlayer(sim, 1, "Allie", false);
    serverSimAddPlayer(sim, 2, "Enemy", false);
    players *plrs = &gs->plyrs;
    allienceAdd(&(*plrs)->item[0].allie, 1);
    allienceAdd(&(*plrs)->item[1].allie, 0);
    gs->pb->numPills = 0;

    OverviewRect out[OVERVIEW_MAX_REGIONS + 1];
    int n;

    /* Tank alone, well clear of every edge: one 29x29 rect on it. */
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "tank with no pills gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 86, 86, 114, 114);

    /* Over the left and bottom edges the block is trimmed, not wrapped. */
    n = overviewMapBuildRegions(gs, 0, TRUE, 3, 250, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "corner tank gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 0, 236, 17, 255);

    /* A closing block is the same rect at a smaller half-width, centred where
     * it was; at nothing left it is the single square the wreck is on, and
     * past that the tank contributes no rect at all. */
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, 4, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "a closing block gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 96, 96, 104, 104);

    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, 0, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "a closed block gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 100, 100, 100, 100);

    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, -1, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 0, "a block that has gone gave %d regions, expected 0",
                  n);

    /* One pill of the player's own, alive and on the map. The tank's rect
     * comes first and the pill's follows it — the order the farewell stamp
     * replays. */
    gs->pb->numPills = 1;
    gs->pb->item[0].owner = 0;
    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].inTank = FALSE;
    gs->pb->item[0].x = 200;
    gs->pb->item[0].y = 50;

    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "tank + own pill gave %d regions, expected 2", n);
    ASSERT_RECT(out[0], 86, 86, 114, 114);
    ASSERT_RECT(out[1], 193, 43, 207, 57);

    /* An allied owner views the same as an own one. */
    gs->pb->item[0].owner = 1;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "allied pill gave %d regions, expected 2", n);
    ASSERT_RECT(out[1], 193, 43, 207, 57);

    /* A pill the player cannot view through contributes nothing: owned by
     * someone hostile, or dead, or carried in a tank. */
    gs->pb->item[0].owner = 2;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "enemy pill gave %d regions, expected 1", n);

    gs->pb->item[0].owner = 0;
    gs->pb->item[0].armour = 0;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "dead pill gave %d regions, expected 1", n);

    gs->pb->item[0].armour = PILLBOX_15;
    gs->pb->item[0].inTank = TRUE;
    n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "carried pill gave %d regions, expected 1", n);

    /* No tank: a viewable pill still gives the player its block, and it is
     * the only rect. */
    gs->pb->item[0].inTank = FALSE;
    n = overviewMapBuildRegions(gs, 0, FALSE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 1, "pill without a tank gave %d regions, expected 1", n);
    ASSERT_RECT(out[0], 193, 43, 207, 57);

    /* No tank and nothing to view through: no live squares at all. */
    gs->pb->item[0].armour = 0;
    n = overviewMapBuildRegions(gs, 0, FALSE, 100, 100, OVERVIEW_TANK_HALF, out,
                                OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 0, "no tank and no viewable pill gave %d regions", n);

    /* A full map of viewable pills plus the tank wants more rects than a
     * short buffer holds; the count stops at maxOut and the square past the
     * end is left alone. */
    {
        const int maxOut = 4;
        BYTE i;

        gs->pb->numPills = MAX_PILLS;
        for (i = 0; i < MAX_PILLS; i++) {
            gs->pb->item[i].owner = 0;
            gs->pb->item[i].armour = PILLBOX_15;
            gs->pb->item[i].inTank = FALSE;
            gs->pb->item[i].x = (BYTE)(20 + i * 10);
            gs->pb->item[i].y = 200;
        }
        out[maxOut].left = -1;
        out[maxOut].top = -1;
        out[maxOut].right = -1;
        out[maxOut].bottom = -1;

        n = overviewMapBuildRegions(gs, 0, TRUE, 100, 100, OVERVIEW_TANK_HALF,
                                    out, maxOut);
        UT_ASSERT_MSG(n == maxOut, "capped build returned %d, expected %d", n,
                      maxOut);
        ASSERT_RECT(out[maxOut], -1, -1, -1, -1);
    }

    /* The schedule those half-widths come off while the tank is dead. The
     * block is whole until the tick the classic view cuts to static, closes
     * from there, and is gone by the end of the close — a shell death and a
     * drowning on their own clocks, the drowning being given longer to watch
     * exactly as the static is. */
    {
        int half; /* The width under test */
        int prev; /* The one before it, for the walk down */
        int wait; /* Ticks left on the death wait */

        half = overviewMapDeathTankHalf(TANK_DEATH_WAIT, LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(half == OVERVIEW_TANK_HALF,
                      "a tank that has just died has half %d, expected %d",
                      half, OVERVIEW_TANK_HALF);

        half = overviewMapDeathTankHalf(STATIC_ON_TICKS, LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(half == OVERVIEW_TANK_HALF,
                      "the tick the static starts has half %d, expected %d",
                      half, OVERVIEW_TANK_HALF);

        half = overviewMapDeathTankHalf(STATIC_ON_TICKS - 1,
                                        LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(half >= 0 && half < OVERVIEW_TANK_HALF,
                      "the first closing tick has half %d, expected inside "
                      "0..%d", half, OVERVIEW_TANK_HALF - 1);

        half = overviewMapDeathTankHalf(
            STATIC_ON_TICKS - OVERVIEW_DEATH_CLOSE_TICKS + 1,
            LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(half == 0,
                      "the last closing tick has half %d, expected the wreck's "
                      "own square", half);

        half = overviewMapDeathTankHalf(
            STATIC_ON_TICKS - OVERVIEW_DEATH_CLOSE_TICKS, LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(half < 0, "a closed block has half %d, expected none",
                      half);

        /* A wait that has not been written yet is still the watching phase:
         * closing on it would drop the block for a tick and put it back. */
        half = overviewMapDeathTankHalf(0, LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(half == OVERVIEW_TANK_HALF,
                      "an unwritten wait has half %d, expected %d", half,
                      OVERVIEW_TANK_HALF);

        /* Drowning cuts earlier in the wait, so there is a tick where it is
         * closing and a shell death is not. */
        UT_ASSERT_MSG(
            overviewMapDeathTankHalf(STATIC_ON_TICKS_DEEPSEA - 1,
                                     LAST_DEATH_BY_DEEPSEA) <
                    OVERVIEW_TANK_HALF &&
                overviewMapDeathTankHalf(STATIC_ON_TICKS_DEEPSEA - 1,
                                         LAST_DEATH_BY_SHELL) ==
                    OVERVIEW_TANK_HALF,
            "a drowning and a shell death close on the same tick");

        /* Never wider than it was a tick ago, the whole way down. */
        prev = OVERVIEW_TANK_HALF;
        for (wait = TANK_DEATH_WAIT; wait >= 1; wait--) {
            half = overviewMapDeathTankHalf(wait, LAST_DEATH_BY_SHELL);
            UT_ASSERT_MSG(half <= prev,
                          "the block grew from %d to %d at %d ticks left",
                          prev, half, wait);
            prev = half;
        }
        UT_ASSERT_MSG(prev < 0, "the block had not gone by the end of the wait");
    }

    /* And where the static sits in that wait: the last stretch of it, ending
     * with the respawn. */
    {
        int wait; /* Ticks left on the death wait */

        UT_ASSERT_MSG(!overviewMapDeathStatic(TANK_DEATH_WAIT),
                      "a tank that has just died is already in static");
        UT_ASSERT_MSG(!overviewMapDeathStatic(OVERVIEW_DEATH_STATIC_TICKS + 1),
                      "the static started a tick early");
        UT_ASSERT_MSG(overviewMapDeathStatic(OVERVIEW_DEATH_STATIC_TICKS),
                      "the static did not start where it should");
        UT_ASSERT_MSG(overviewMapDeathStatic(1),
                      "the static stopped before the respawn");
        UT_ASSERT_MSG(!overviewMapDeathStatic(0),
                      "a tank that is not dead is in static");

        /* The order the death plays in: the block has closed before the static
         * comes up, so there is a stretch of dark map in between. Both causes,
         * because they close on different clocks. */
        for (wait = OVERVIEW_DEATH_STATIC_TICKS; wait >= 1; wait--) {
            UT_ASSERT_MSG(
                overviewMapDeathTankHalf(wait, LAST_DEATH_BY_SHELL) < 0 &&
                    overviewMapDeathTankHalf(wait, LAST_DEATH_BY_DEEPSEA) < 0,
                "the static is up at %d ticks left while the block is still "
                "open — the map never goes dark", wait);
        }
        UT_ASSERT_MSG(
            overviewMapDeathTankHalf(OVERVIEW_DEATH_STATIC_TICKS + 1,
                                     LAST_DEATH_BY_SHELL) < 0,
            "the block closes only as the static comes up — no dark in "
            "between");
    }

    serverSimDestroy(sim);
    return 0;
}

/* --------------------------------------------------------------------------
 * Live cases. A ClientSim wired to an in-process ServerSim and driven through
 * clientSimDisplayTick, so the assertions below run against the real wiring
 * rather than a hand-called overviewMapUpdate.
 * -------------------------------------------------------------------------- */

/* Rounds of the input-pair warm-up to run before the client's tank is
 * expected, and the ceiling on how long to keep pumping for it. The active
 * local transport runs two server half-steps per clientSimNetTick, so the
 * producer has to supply two tick numbers per tick or the stream starves into
 * substitutes. */
#define OVERVIEW_WARMUP_TICKS     4
#define OVERVIEW_MAX_WARMUP_TICKS 32

/* E_MAP's compressed length — the literal ut_make_running_sim hands to
 * serverSimCreateCompressed. */
#define OVERVIEW_EMAP_LEN 5097

/* Side of the tank block, for the tile snapshot the tank-removal arm takes. */
#define OVERVIEW_TANK_SIDE (2 * OVERVIEW_TANK_HALF + 1)

/* The map corner the overview was seen jumping to when the local player died.
 * The dead-tank case parks the tank there itself, so what it asserts does not
 * rest on how a dead tank's position goes bad. */
#define OVERVIEW_DEAD_TANK_M 0

typedef struct OverviewFixture {
    ServerSim *sim;
    ClientSim *cs;
    GameSim   *gs;      /* the client's sim, not the server's */
    BYTE       me;      /* the slot the local join landed on */
    BYTE       tankMX;  /* where the client's tank sits, as the overview reads it */
    BYTE       tankMY;
} OverviewFixture;

/* Tears the fixture down, in the order test_active_local_input_to_shot uses.
 * Safe on a half-built fixture so the start path can bail anywhere. */
static void overviewFixtureStop(OverviewFixture *f) {
    if (f->cs != NULL) {
        clientSimDestroy(f->cs);
        f->cs = NULL;
    }
    if (f->sim != NULL) {
        serverSimDestroy(f->sim);
        f->sim = NULL;
    }
}

/* Brings up a running ServerSim with an in-process ClientSim joined to it and
 * pumps the local transport until the client's own tank has landed. Returns
 * NULL on success, or a message naming what failed; either way the caller
 * finishes with overviewFixtureStop. */
static const char *overviewFixtureStart(OverviewFixture *f, const char *name) {
    uint32_t inputTick = 1;
    int i;

    memset(f, 0, sizeof(*f));

    f->sim = ut_make_running_sim("Host");
    if (f->sim == NULL) {
        return "ut_make_running_sim returned NULL";
    }

    f->cs = clientSimAlloc();
    if (f->cs == NULL) {
        return "clientSimAlloc returned NULL";
    }
    clientSimCreate(f->cs);
    if (clientSimConnectLocal(f->cs, f->sim, name, "", 0, 0) != TRUE) {
        return "clientSimConnectLocal failed";
    }

    f->me = clientSimGetMyPlayerNum(f->cs);
    for (i = 0; i < OVERVIEW_MAX_WARMUP_TICKS; i++) {
        InputPacket a, b;

        memset(&a, 0, sizeof(a));
        a.tick      = inputTick;
        a.playerNum = f->me;
        memset(&b, 0, sizeof(b));
        b.tick      = inputTick + 1;
        b.playerNum = f->me;
        clientSimNetSendInput(f->cs, &a);
        clientSimNetSendInput(f->cs, &b);
        clientSimNetTick(f->cs);
        inputTick += 2;

        if (i + 1 >= OVERVIEW_WARMUP_TICKS &&
            clientSimGetMyTankMapPos(f->cs, &f->tankMX, &f->tankMY) == TRUE) {
            break;
        }
    }

    f->gs = clientSimGetGameSim(f->cs);
    if (f->gs == NULL) {
        return "clientSimGetGameSim returned NULL";
    }
    if (clientSimGetMyTankMapPos(f->cs, &f->tankMX, &f->tankMY) != TRUE) {
        return "the client still has no tank after the warm-up";
    }
    return NULL;
}

/* The direction with room in it: everything below steps away from the nearer
 * map edge, so a block 60 or 100 squares off the tank still lands on the map
 * wherever the join dropped the tank. */
static int overviewAwayFromEdge(BYTE mapCoord) {
    return (mapCoord < MAP_ARRAY_SIZE / 2) ? 1 : -1;
}

/* The inclusive block overviewMapBuildRegions builds round a centre, restated
 * so the assertions have something to check against that is not the code under
 * test. run_overview_regions is what pins the two together. */
static OverviewRect overviewTestBlock(int cx, int cy, int half) {
    OverviewRect r; /* Rect to return */

    r.left = cx - half;
    r.top = cy - half;
    r.right = cx + half;
    r.bottom = cy + half;
    if (r.left < 0) {
        r.left = 0;
    }
    if (r.top < 0) {
        r.top = 0;
    }
    if (r.right > MAP_ARRAY_SIZE - 1) {
        r.right = MAP_ARRAY_SIZE - 1;
    }
    if (r.bottom > MAP_ARRAY_SIZE - 1) {
        r.bottom = MAP_ARRAY_SIZE - 1;
    }
    return r;
}

/* TRUE when the square falls inside any of the rects. */
static bool overviewInAnyRect(const OverviewRect *r, int count, int x, int y) {
    int i; /* Looping variable */

    for (i = 0; i < count; i++) {
        if (x >= r[i].left && x <= r[i].right && y >= r[i].top &&
            y <= r[i].bottom) {
            return TRUE;
        }
    }
    return FALSE;
}

/* Reports the first square of the rect whose OVERVIEW_F_LIVE bit is not what
 * was wanted, so a failure can name it. FALSE when every square agrees. */
static bool overviewRectLiveMismatch(const OverviewMap *om,
                                     const OverviewRect *r, bool want,
                                     int *outX, int *outY) {
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (x = r->left; x <= r->right; x++) {
        for (y = r->top; y <= r->bottom; y++) {
            bool live = (om->flags[x][y] & OVERVIEW_F_LIVE) != 0;
            if (live != want) {
                *outX = x;
                *outY = y;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Fails the calling test unless every square of the rect carries (want ==
 * TRUE) or has dropped (want == FALSE) OVERVIEW_F_LIVE. */
#define ASSERT_RECT_LIVE(om, r, want, what)                                   \
    do {                                                                      \
        int bx_ = 0;                                                          \
        int by_ = 0;                                                          \
        UT_ASSERT_MSG(                                                        \
            overviewRectLiveMismatch((om), &(r), (want), &bx_, &by_) != TRUE, \
            "%s: square %d,%d is %s live", (what), bx_, by_,                  \
            ((want) == TRUE) ? "not" : "still");                              \
    } while (0)

/* A terrain byte that has to read differently from what is there now. Sea and
 * grass are far enough apart that no adjacency calculation can collapse the
 * two onto one sprite. */
static BYTE overviewFlipTerrain(BYTE terrain) {
    return (terrain == DEEP_SEA) ? (BYTE)GRASS : (BYTE)DEEP_SEA;
}

/* First square of the rect whose tile actually moves when the terrain under it
 * is swapped. Pill and base squares render from their owner whatever the
 * ground says, so a terrain check aimed at one would pass however the memory
 * behaved. Probes by writing and putting back, leaving the map exactly as it
 * was found; call it before the first display tick so the memory never sees
 * the probe. */
static bool overviewFindTerrainSquare(GameSim *gs, BYTE me,
                                      const OverviewRect *r, BYTE *outX,
                                      BYTE *outY) {
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (x = r->left; x <= r->right; x++) {
        for (y = r->top; y <= r->bottom; y++) {
            bool isMine = FALSE;
            BYTE was = mapGetPos(&gs->mp, (BYTE)x, (BYTE)y);
            BYTE before = viewportCalcSquarePure(gs, me, (BYTE)x, (BYTE)y, &isMine);
            BYTE after;

            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, overviewFlipTerrain(was),
                      TRUE, TRUE);
            after = viewportCalcSquarePure(gs, me, (BYTE)x, (BYTE)y, &isMine);
            mapSetPos(gs, &gs->mp, (BYTE)x, (BYTE)y, was, TRUE, TRUE);
            if (after != before) {
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                return TRUE;
            }
        }
    }
    return FALSE;
}

int run_overview_reveal(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Reveal");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* One pill of the client's own, a hundred squares off the tank, so the
     * live set is a union of two disjoint blocks rather than the tank's block
     * on its own. */
    BYTE pillX = (BYTE)((int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 100);
    BYTE pillY = f.tankMY;
    f.gs->pb->numPills = 1;
    f.gs->pb->item[0].owner = f.me;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].x = pillX;
    f.gs->pb->item[0].y = pillY;

    /* The first display tick of the client's life: everything the memory holds
     * afterwards was put there by this one update. */
    clientSimDisplayTick(f.cs, false);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    OverviewRect expect[OVERVIEW_MAX_REGIONS];
    int n = overviewMapBuildRegions(f.gs, f.me, TRUE, f.tankMX, f.tankMY,
                                    OVERVIEW_TANK_HALF, expect,
                                    OVERVIEW_MAX_REGIONS);
    UT_ASSERT_MSG(n == 2, "expected the tank block and one pill block, got %d",
                  n);
    UT_ASSERT_MSG(om->liveCount == n, "liveCount %d, expected %d",
                  om->liveCount, n);

    /* Every square of the map, either side of the union: everything holds
     * what the per-square calculator says — the install seeded the whole map
     * on this tick and nothing has moved since — with the live flag exactly
     * on the union. */
    int x, y;
    for (x = 0; x < MAP_ARRAY_SIZE; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            BYTE flags = om->flags[x][y];
            bool inUnion = overviewInAnyRect(expect, n, x, y) == TRUE;
            bool isMine = FALSE;
            BYTE want = viewportCalcSquarePure(f.gs, f.me, (BYTE)x, (BYTE)y,
                                               &isMine);

            UT_ASSERT_MSG(((flags & OVERVIEW_F_LIVE) != 0) == inUnion,
                          "square %d,%d is %s the union but %s live",
                          x, y, inUnion ? "inside" : "outside",
                          inUnion ? "not" : "flagged");
            UT_ASSERT_MSG(om->tile[x][y] == want,
                          "square %d,%d holds tile %u, calculator says %u",
                          x, y, (unsigned)om->tile[x][y], (unsigned)want);
            UT_ASSERT_MSG(((flags & OVERVIEW_F_MINE) != 0) == (isMine == TRUE),
                          "square %d,%d mine flag %d, calculator says %d",
                          x, y, (flags & OVERVIEW_F_MINE) != 0,
                          isMine == TRUE);
        }
    }
    UT_ASSERT_MSG(om->seenCount ==
                      (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE),
                  "seenCount %u after the seed, expected every square (%u)",
                  om->seenCount,
                  (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));

    /* A second tick with nothing touched in between. generation is what a
     * renderer reads to decide it can skip a redraw, so a tick where nothing
     * moved must not advance it. */
    unsigned genBefore = om->generation;
    unsigned seenBefore = om->seenCount;
    int liveBefore = om->liveCount;
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->generation == genBefore,
                  "generation went from %u to %u over a tick where nothing "
                  "moved", genBefore, om->generation);
    UT_ASSERT_MSG(om->seenCount == seenBefore,
                  "seenCount went from %u to %u over a tick where nothing "
                  "moved", seenBefore, om->seenCount);
    UT_ASSERT_MSG(om->liveCount == liveBefore,
                  "liveCount went from %d to %d over a tick where nothing "
                  "moved", liveBefore, om->liveCount);

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_freeze_no_leak(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Freeze");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* The map's own pills stay out of this one: with the tank block the only
     * live region, a square that leaves it is genuinely frozen. */
    f.gs->pb->numPills = 0;

    /* Sixty squares is more than the two 29x29 blocks can span between them,
     * so the tank's old block and its new one share no square. */
    int newMX = (int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 60;
    OverviewRect oldRect = overviewTestBlock(f.tankMX, f.tankMY, OVERVIEW_TANK_HALF);
    OverviewRect newRect = overviewTestBlock(newMX, f.tankMY, OVERVIEW_TANK_HALF);

    /* Both squares are picked before the first tick, so the picker's probing
     * never reaches the memory. */
    BYTE frozenX = 0, frozenY = 0, liveX = 0, liveY = 0;
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &oldRect, &frozenX,
                                            &frozenY) == TRUE,
                  "no terrain-driven square in the block at %u,%u",
                  (unsigned)f.tankMX, (unsigned)f.tankMY);
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &newRect, &liveX,
                                            &liveY) == TRUE,
                  "no terrain-driven square in the block at %d,%u", newMX,
                  (unsigned)f.tankMY);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG((om->flags[frozenX][frozenY] & OVERVIEW_F_LIVE) != 0,
                  "square %u,%u is in the tank's block but not live",
                  (unsigned)frozenX, (unsigned)frozenY);
    BYTE frozenTile = om->tile[frozenX][frozenY];
    UT_ASSERT_MSG(frozenTile != OVERVIEW_UNSEEN,
                  "square %u,%u was live but reads unseen", (unsigned)frozenX,
                  (unsigned)frozenY);

    /* Drive off, and the square the tank left behind stops being live while
     * keeping the tile it carried. */
    tankSetWorld(f.gs, &f.gs->tanks[f.me], (WORLD)(newMX << TANK_SHIFT_MAPSIZE),
                 (WORLD)((int)f.tankMY << TANK_SHIFT_MAPSIZE), 0, false);
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, newRect, TRUE, "the block the tank drove into");
    UT_ASSERT_MSG((om->flags[frozenX][frozenY] & OVERVIEW_F_LIVE) == 0,
                  "square %u,%u is out of every region but still live",
                  (unsigned)frozenX, (unsigned)frozenY);
    UT_ASSERT_MSG(om->tile[frozenX][frozenY] == frozenTile,
                  "square %u,%u changed from tile %u to %u on the way out",
                  (unsigned)frozenX, (unsigned)frozenY, (unsigned)frozenTile,
                  (unsigned)om->tile[frozenX][frozenY]);

    /* The leak. The client takes a terrain change on the frozen square — it
     * knows about every change on the map, wherever it is — and the memory has
     * to go on showing what was there when the square was last seen. */
    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, frozenX, frozenY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, frozenX, frozenY, now, TRUE, TRUE);
        UT_ASSERT_MSG(mapGetPos(&f.gs->mp, frozenX, frozenY) == now,
                      "the client did not take the terrain change at %u,%u",
                      (unsigned)frozenX, (unsigned)frozenY);
        UT_ASSERT_MSG(viewportCalcSquarePure(f.gs, f.me, frozenX, frozenY,
                                             &isMine) != frozenTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the leak check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)frozenX,
                      (unsigned)frozenY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[frozenX][frozenY] == frozenTile,
                  "the memory picked up a terrain change on frozen square "
                  "%u,%u: tile %u, expected %u",
                  (unsigned)frozenX, (unsigned)frozenY,
                  (unsigned)om->tile[frozenX][frozenY], (unsigned)frozenTile);

    /* The same change inside the live block does reach the memory, so the
     * freeze above is the region mask at work and not a dead update. */
    UT_ASSERT_MSG((om->flags[liveX][liveY] & OVERVIEW_F_LIVE) != 0,
                  "square %u,%u is in the tank's new block but not live",
                  (unsigned)liveX, (unsigned)liveY);
    BYTE liveTile = om->tile[liveX][liveY];
    unsigned genBefore = om->generation;
    BYTE wantTile;
    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, liveX, liveY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, liveX, liveY, now, TRUE, TRUE);
        wantTile = viewportCalcSquarePure(f.gs, f.me, liveX, liveY, &isMine);
        UT_ASSERT_MSG(wantTile != liveTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the live check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)liveX,
                      (unsigned)liveY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[liveX][liveY] == wantTile,
                  "live square %u,%u holds tile %u, expected %u",
                  (unsigned)liveX, (unsigned)liveY,
                  (unsigned)om->tile[liveX][liveY], (unsigned)wantTile);
    UT_ASSERT_MSG(om->generation > genBefore,
                  "generation stuck at %u over a changed live square",
                  genBefore);

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_pill_capture(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Pills");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* One pill, a hundred squares off the tank so its 15x15 block never meets
     * the tank's 29x29 one, and off any base square — a base renders from its
     * owner whatever sits on it, which would make the carried-pill arm below
     * read the wrong thing. */
    BYTE pillX = (BYTE)((int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 100);
    BYTE pillY = f.tankMY;
    int step;
    for (step = 0; step < 16 && basesExistPos(&f.gs->bs, pillX, pillY) == TRUE;
         step++) {
        pillY = (BYTE)(pillY + 1);
    }
    UT_ASSERT_MSG(basesExistPos(&f.gs->bs, pillX, pillY) != TRUE,
                  "no base-free square near %u,%u for the pill",
                  (unsigned)pillX, (unsigned)f.tankMY);

    f.gs->pb->numPills = 1;
    f.gs->pb->item[0].x = pillX;
    f.gs->pb->item[0].y = pillY;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].owner = NEUTRAL;

    OverviewRect pillRect = overviewTestBlock(pillX, pillY, OVERVIEW_PILL_HALF);
    OverviewRect tankRect = overviewTestBlock(f.tankMX, f.tankMY, OVERVIEW_TANK_HALF);

    /* Any slot but the client's own is hostile: alliances start empty. */
    BYTE hostile = (BYTE)((f.me == 0) ? 1 : 0);
    UT_ASSERT_MSG(playersIsAllie(&f.gs->plyrs, f.me, hostile) != TRUE,
                  "slots %u and %u are allied — breaks the enemy-pill arm",
                  (unsigned)f.me, (unsigned)hostile);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    bool isMine = FALSE;

    /* Neutral: nothing to view through, so the block never lights up. */
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "a neutral pill's block");

    /* Captured: the block goes live and the pill draws in the captor's
     * colours. */
    f.gs->pb->item[0].owner = f.me;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, TRUE, "a captured pill's block");
    BYTE aliveTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(om->tile[pillX][pillY] == aliveTile,
                  "the live pill holds tile %u, calculator says %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)aliveTile);

    /* Killed: the block freezes, and the farewell stamp leaves the pill's own
     * square dead rather than a tick stale on full armour. */
    f.gs->pb->item[0].armour = 0;
    clientSimDisplayTick(f.cs, false);
    BYTE deadTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(deadTile != aliveTile,
                  "a dead pill renders as tile %u, the same as a live one — "
                  "nothing here for the farewell stamp to show",
                  (unsigned)deadTile);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "a dead pill's block");
    UT_ASSERT_MSG(om->tile[pillX][pillY] == deadTile,
                  "the dead pill froze on tile %u, expected %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)deadTile);

    /* Alive again so there is a live block to leave, then handed to someone
     * hostile: the same freeze, with the enemy's colours stamped in. */
    f.gs->pb->item[0].armour = PILLBOX_15;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, TRUE, "a repaired pill's block");
    f.gs->pb->item[0].owner = hostile;
    clientSimDisplayTick(f.cs, false);
    BYTE enemyTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(enemyTile != aliveTile,
                  "an enemy pill renders as tile %u, the same as an allied one",
                  (unsigned)enemyTile);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "an enemy pill's block");
    UT_ASSERT_MSG(om->tile[pillX][pillY] == enemyTile,
                  "the captured pill froze on tile %u, expected %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)enemyTile);

    /* Alive and owned again, then picked up: the square goes back to the
     * ground that was under it. */
    f.gs->pb->item[0].owner = f.me;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, pillRect, TRUE, "a recaptured pill's block");
    f.gs->pb->item[0].inTank = TRUE;
    clientSimDisplayTick(f.cs, false);
    BYTE groundTile = viewportCalcSquarePure(f.gs, f.me, pillX, pillY, &isMine);
    UT_ASSERT_MSG(groundTile != aliveTile && groundTile != deadTile &&
                      groundTile != enemyTile,
                  "the ground under the pill renders as tile %u, which is one "
                  "of the pill tiles", (unsigned)groundTile);
    ASSERT_RECT_LIVE(om, pillRect, FALSE, "a carried pill's block");
    UT_ASSERT_MSG(om->tile[pillX][pillY] == groundTile,
                  "the carried pill's square froze on tile %u, expected %u",
                  (unsigned)om->tile[pillX][pillY], (unsigned)groundTile);

    /* The tank goes the same way: its block stops being live and keeps every
     * tile it already had. */
    BYTE saved[OVERVIEW_TANK_SIDE * OVERVIEW_TANK_SIDE];
    int x, y, i;
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "the tank's block before it is removed");
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            saved[i] = om->tile[x][y];
            i++;
        }
    }

    tankDestroy(f.gs, &f.gs->tanks[f.me]);
    f.gs->tanks[f.me] = NULL;
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, tankRect, FALSE, "the removed tank's block");
    UT_ASSERT_MSG(om->liveCount == 0,
                  "liveCount %d with no tank and the only pill carried",
                  om->liveCount);
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            UT_ASSERT_MSG(om->tile[x][y] == saved[i],
                          "square %d,%d changed from tile %u to %u when the "
                          "tank went", x, y, (unsigned)saved[i],
                          (unsigned)om->tile[x][y]);
            i++;
        }
    }

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_dead_tank(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Dead");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* No pills: the tank's block is the only live region, so whatever is live
     * after the death is the dead tank's doing and nothing else's. */
    f.gs->pb->numPills = 0;

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    OverviewRect tankRect =
        overviewTestBlock(f.tankMX, f.tankMY, OVERVIEW_TANK_HALF);
    OverviewRect deadRect = overviewTestBlock(OVERVIEW_DEAD_TANK_M,
                                              OVERVIEW_DEAD_TANK_M,
                                              OVERVIEW_TANK_HALF);
    /* Sixty squares is more than the two 29x29 blocks can span between them,
     * so the block the tank respawns into shares no square with the one it
     * died holding. */
    int respawnMX = (int)f.tankMX + overviewAwayFromEdge(f.tankMX) * 60;
    OverviewRect respawnRect =
        overviewTestBlock(respawnMX, f.tankMY, OVERVIEW_TANK_HALF);
    int x, y, i;

    /* The square the terrain arm moves later on, picked before the first tick
     * so the picker's probing never reaches the memory. */
    BYTE liveX = 0, liveY = 0;
    UT_ASSERT_MSG(overviewFindTerrainSquare(f.gs, f.me, &tankRect, &liveX,
                                            &liveY) == TRUE,
                  "no terrain-driven square in the block at %u,%u",
                  (unsigned)f.tankMX, (unsigned)f.tankMY);

    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "a living tank's block");
    UT_ASSERT_MSG(om->liveCount == 1,
                  "liveCount %d with a living tank and no pills",
                  om->liveCount);

    /* Every tile of the tank's block, to check dying leaves what is there
     * alone rather than rewriting it from wherever the tank now reads. */
    BYTE saved[OVERVIEW_TANK_SIDE * OVERVIEW_TANK_SIDE];
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            saved[i] = om->tile[x][y];
            i++;
        }
    }

    /* The block the tank is about to be sent to. Checked not-live rather than
     * assumed, so a join that dropped the tank within reach of the corner says
     * so instead of leaving the case proving nothing; its seeded tiles are
     * captured so the death tick can be shown to leave them alone. */
    BYTE corner[OVERVIEW_TANK_SIDE * OVERVIEW_TANK_SIDE];
    i = 0;
    for (x = deadRect.left; x <= deadRect.right; x++) {
        for (y = deadRect.top; y <= deadRect.bottom; y++) {
            UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_LIVE) == 0,
                          "square %d,%d of the corner the dead tank is sent to "
                          "is already live", x, y);
            corner[i] = om->tile[x][y];
            i++;
        }
    }

    /* Dead but still in its slot, which is what the server leaves behind for
     * the whole of deathWait: over full armour, the way a drowning writes it,
     * with a full wait left to run and a shell as the cause. The move to the
     * corner stands in for the map origin a dead tank's position reads as, so
     * what the corner arm below asserts does not rest on how that position
     * goes bad. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)(TANK_FULL_ARMOUR + 1));
    tankSetLastTankDeath(&f.gs->tanks[f.me], LAST_DEATH_BY_SHELL);
    tankSetDeathWait(&f.gs->tanks[f.me], TANK_DEATH_WAIT);
    tankSetWorld(f.gs, &f.gs->tanks[f.me],
                 (WORLD)(OVERVIEW_DEAD_TANK_M << TANK_SHIFT_MAPSIZE),
                 (WORLD)(OVERVIEW_DEAD_TANK_M << TANK_SHIFT_MAPSIZE), 0, false);
    clientSimDisplayTick(f.cs, false);

    /* A dead tank reveals nothing where it reads: the block is held on the
     * square it died on, never on the corner underneath it, so the corner
     * stays out of the live set with its seeded tiles untouched. */
    i = 0;
    for (x = deadRect.left; x <= deadRect.right; x++) {
        for (y = deadRect.top; y <= deadRect.bottom; y++) {
            UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_LIVE) == 0,
                          "a dead tank lit square %d,%d", x, y);
            UT_ASSERT_MSG(om->tile[x][y] == corner[i],
                          "a dead tank restamped square %d,%d: tile %u, "
                          "expected %u", x, y, (unsigned)om->tile[x][y],
                          (unsigned)corner[i]);
            i++;
        }
    }
    UT_ASSERT_MSG(om->liveCount == 1,
                  "liveCount %d with a dead tank and no pills", om->liveCount);

    /* The block it died holding stays live, tiles intact, so the player
     * watches the explosion and the ground round it in colour. */
    ASSERT_RECT_LIVE(om, tankRect, TRUE, "a dead tank's block");
    i = 0;
    for (x = tankRect.left; x <= tankRect.right; x++) {
        for (y = tankRect.top; y <= tankRect.bottom; y++) {
            UT_ASSERT_MSG(om->tile[x][y] == saved[i],
                          "square %d,%d changed from tile %u to %u when the "
                          "tank died", x, y, (unsigned)saved[i],
                          (unsigned)om->tile[x][y]);
            i++;
        }
    }

    /* And the block is genuinely still live rather than merely still flagged:
     * a terrain change inside it reaches the memory while the tank is dead. */
    BYTE liveTile = om->tile[liveX][liveY];
    BYTE wantTile;
    {
        bool isMine = FALSE;
        BYTE was = mapGetPos(&f.gs->mp, liveX, liveY);
        BYTE now = overviewFlipTerrain(was);

        mapSetPos(f.gs, &f.gs->mp, liveX, liveY, now, TRUE, TRUE);
        UT_ASSERT_MSG(mapGetPos(&f.gs->mp, liveX, liveY) == now,
                      "the client did not take the terrain change at %u,%u",
                      (unsigned)liveX, (unsigned)liveY);
        wantTile = viewportCalcSquarePure(f.gs, f.me, liveX, liveY, &isMine);
        UT_ASSERT_MSG(wantTile != liveTile,
                      "terrain %u and %u render as the same tile at %u,%u — "
                      "the live check would pass on its own",
                      (unsigned)was, (unsigned)now, (unsigned)liveX,
                      (unsigned)liveY);
    }
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->tile[liveX][liveY] == wantTile,
                  "square %u,%u sat in a dead tank's block and held tile %u "
                  "over a terrain change, expected %u", (unsigned)liveX,
                  (unsigned)liveY, (unsigned)om->tile[liveX][liveY],
                  (unsigned)wantTile);

    /* The wait run down to the tick after the classic view cuts to static: the
     * block is closing, so the squares it has given up are out of the live set
     * and the ones it still holds are in it. */
    {
        int closingHalf = overviewMapDeathTankHalf(STATIC_ON_TICKS - 1,
                                                   LAST_DEATH_BY_SHELL);
        UT_ASSERT_MSG(closingHalf >= 0 && closingHalf < OVERVIEW_TANK_HALF,
                      "the first closing tick left half %d, expected inside "
                      "0..%d", closingHalf, OVERVIEW_TANK_HALF - 1);
        OverviewRect closingRect =
            overviewTestBlock(f.tankMX, f.tankMY, closingHalf);

        tankSetDeathWait(&f.gs->tanks[f.me], STATIC_ON_TICKS - 1);
        clientSimDisplayTick(f.cs, false);

        ASSERT_RECT_LIVE(om, closingRect, TRUE, "a closing block");
        UT_ASSERT_MSG(om->liveCount == 1,
                      "liveCount %d with a closing block", om->liveCount);
        for (x = tankRect.left; x <= tankRect.right; x++) {
            for (y = tankRect.top; y <= tankRect.bottom; y++) {
                if (x >= closingRect.left && x <= closingRect.right &&
                    y >= closingRect.top && y <= closingRect.bottom) {
                    continue;
                }
                UT_ASSERT_MSG((om->flags[x][y] & OVERVIEW_F_LIVE) == 0,
                              "square %d,%d is still live after the block "
                              "closed past it", x, y);
            }
        }
    }

    /* And closed: the tank has no block left at all, so the ground it died on
     * is remembered like any other square the player has walked away from. A
     * terrain change inside it no longer reaches the memory, which is what
     * stops a dead player watching the square that killed them. */
    tankSetDeathWait(&f.gs->tanks[f.me],
                     STATIC_ON_TICKS - OVERVIEW_DEATH_CLOSE_TICKS);
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, tankRect, FALSE, "a block that has closed");
    UT_ASSERT_MSG(om->liveCount == 0,
                  "liveCount %d with a closed block and no pills",
                  om->liveCount);
    {
        BYTE frozen = om->tile[liveX][liveY];
        BYTE back = mapGetPos(&f.gs->mp, liveX, liveY);

        mapSetPos(f.gs, &f.gs->mp, liveX, liveY, overviewFlipTerrain(back), TRUE,
                  TRUE);
        clientSimDisplayTick(f.cs, false);
        UT_ASSERT_MSG(om->tile[liveX][liveY] == frozen,
                      "square %u,%u took a terrain change through a closed "
                      "block: tile %u, expected %u", (unsigned)liveX,
                      (unsigned)liveY, (unsigned)om->tile[liveX][liveY],
                      (unsigned)frozen);
    }

    /* Respawned sixty squares off: the block follows the tank there and the
     * square it died on lets go, so the remembered position lasts exactly as
     * long as the tank has none of its own. */
    tankSetWorld(f.gs, &f.gs->tanks[f.me],
                 (WORLD)(respawnMX << TANK_SHIFT_MAPSIZE),
                 (WORLD)((int)f.tankMY << TANK_SHIFT_MAPSIZE), 0, false);
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    tankSetDeathWait(&f.gs->tanks[f.me], 0);
    clientSimDisplayTick(f.cs, false);
    ASSERT_RECT_LIVE(om, respawnRect, TRUE, "a respawned tank's block");
    ASSERT_RECT_LIVE(om, tankRect, FALSE,
                     "the block the tank respawned away from");
    UT_ASSERT_MSG(om->liveCount == 1, "liveCount %d after the tank came back",
                  om->liveCount);

    overviewFixtureStop(&f);
    return 0;
}

/* Seeded into the out-params before every read below. Non-zero, so the map
 * origin a dead tank's position used to read as shows up as a write. */
#define TANK_POS_SENTINEL 0xEE

int run_tank_pos_dead(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "TankPos");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    BYTE gotX = TANK_POS_SENTINEL;
    BYTE gotY = TANK_POS_SENTINEL;

    UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &gotX, &gotY) == TRUE,
                  "a living tank reported no map position");
    UT_ASSERT_MSG(gotX == f.tankMX && gotY == f.tankMY,
                  "a living tank reads %u,%u, expected %u,%u", (unsigned)gotX,
                  (unsigned)gotY, (unsigned)f.tankMX, (unsigned)f.tankMY);

    /* Dead but still in its slot, which is where the server leaves it for the
     * whole of deathWait. The position underneath reads as the map origin, so
     * the accessor has nothing true to report. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)(TANK_FULL_ARMOUR + 1));
    gotX = TANK_POS_SENTINEL;
    gotY = TANK_POS_SENTINEL;
    UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &gotX, &gotY) != TRUE,
                  "a dead tank reported a map position");
    /* A caller that ignores the return value should at least not be handed a
     * square the tank is not on. */
    UT_ASSERT_MSG(gotX == TANK_POS_SENTINEL && gotY == TANK_POS_SENTINEL,
                  "a failed read overwrote the caller's variables with %u,%u",
                  (unsigned)gotX, (unsigned)gotY);

    /* Respawned: the accessor answers again, with the square it had. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    gotX = TANK_POS_SENTINEL;
    gotY = TANK_POS_SENTINEL;
    UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &gotX, &gotY) == TRUE,
                  "a respawned tank reported no map position");
    UT_ASSERT_MSG(gotX == f.tankMX && gotY == f.tankMY,
                  "a respawned tank reads %u,%u, expected %u,%u",
                  (unsigned)gotX, (unsigned)gotY, (unsigned)f.tankMX,
                  (unsigned)f.tankMY);

    overviewFixtureStop(&f);
    return 0;
}

/* Builds the three per-frame entity lists and keeps the tanks. The other two
 * are created and thrown away here because clientSimPrepareOverviewEntities
 * fills all three and the heap lists have to be freed either way. */
static void overviewEntitiesPrepare(ClientSim *cs, screenTanks *tks) {
    screenLgm lgms;     /* Built and dropped — this case is about tanks */
    screenBullets sb;   /* Same */

    screenTanksCreate(tks);
    screenLgmCreate(&lgms);
    sb = screenBulletsCreate();
    clientSimPrepareOverviewEntities(cs, tks, &lgms, &sb);
    screenBulletsDestroy(&sb);
    screenLgmDestroy(&lgms);
}

/* The square a player's tank came back on, or FALSE when the list does not
 * carry that player at all. */
static bool overviewFindTank(const screenTanks *tks, BYTE playerNum, BYTE *outX,
                             BYTE *outY) {
    BYTE total = screenTanksGetNumEntries(tks);
    BYTE count; /* Looping variable */

    for (count = 1; count <= total; count++) {
        BYTE mx;
        BYTE my;
        BYTE px;
        BYTE py;
        BYTE frame;
        BYTE pn;
        char name[PLAYER_NAME_LEN];

        screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &pn, name);
        if (pn == playerNum) {
            *outX = mx;
            *outY = my;
            return TRUE;
        }
    }
    return FALSE;
}

int run_overview_entities(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Entities");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* No pills, so the tank's 29x29 block is the whole live set and a square
     * outside it is a square the player genuinely cannot see. */
    f.gs->pb->numPills = 0;

    /* Any slot but the client's own is hostile: alliances start empty. */
    BYTE hostile = (BYTE)((f.me == 0) ? 1 : 0);
    UT_ASSERT_MSG(playersIsAllie(&f.gs->plyrs, f.me, hostile) != TRUE,
                  "slots %u and %u are allied", (unsigned)f.me,
                  (unsigned)hostile);

    /* Five squares off is inside the 29x29 block; twenty is outside it and
     * still inside the 55x55 box the server culls other tanks at, which is
     * the case the filter exists for — the client is told about the tank and
     * has to decline to draw it. */
    int dir = overviewAwayFromEdge(f.tankMX);
    BYTE nearX = (BYTE)((int)f.tankMX + dir * 5);
    BYTE farX = (BYTE)((int)f.tankMX + dir * 20);
    BYTE atY = f.tankMY;

    /* A tank standing in trees is hidden from the list by sight rules that
     * have nothing to do with the overview, which would leave the far arm
     * below passing for the wrong reason. Grass under both squares takes that
     * out of the picture. */
    mapSetPos(f.gs, &f.gs->mp, nearX, atY, GRASS, TRUE, TRUE);
    mapSetPos(f.gs, &f.gs->mp, farX, atY, GRASS, TRUE, TRUE);

    /* The hostile tank, driven straight into the client's own player table —
     * the same state a snapshot would have left behind. pixelX/pixelY of 8
     * put it in the middle of its square, so the list reports the square it
     * was placed on. */
    player *bogey = &f.gs->plyrs->item[hostile];
    bogey->inUse = TRUE;
    strcpy(bogey->playerName, "Bogey");
    bogey->frame = 0;
    bogey->onBoat = FALSE;
    bogey->pixelX = 8;
    bogey->pixelY = 8;
    bogey->mapY = atY;

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    screenTanks tks;
    BYTE gotX = 0;
    BYTE gotY = 0;

    /* Inside the block: in the list, and the filter lets it through. */
    bogey->mapX = nearX;
    clientSimDisplayTick(f.cs, false);
    overviewEntitiesPrepare(f.cs, &tks);
    UT_ASSERT_MSG(overviewFindTank(&tks, hostile, &gotX, &gotY) == TRUE,
                  "the hostile tank is missing from the prepared list at "
                  "%u,%u", (unsigned)nearX, (unsigned)atY);
    UT_ASSERT_MSG(gotX == nearX && gotY == atY,
                  "the prepared list puts the hostile tank on %u,%u, expected "
                  "%u,%u — the full-map rect is not reporting absolute "
                  "squares", (unsigned)gotX, (unsigned)gotY, (unsigned)nearX,
                  (unsigned)atY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY, false) == true,
                  "square %u,%u is inside the tank's block and the filter "
                  "still hides an enemy on it", (unsigned)gotX,
                  (unsigned)gotY);

    /* Our own tank comes back too, and passes wherever it stands. */
    UT_ASSERT_MSG(overviewFindTank(&tks, f.me, &gotX, &gotY) == TRUE,
                  "the local tank is missing from the prepared list");
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY, true) == true,
                  "the local tank is filtered out of its own square %u,%u",
                  (unsigned)gotX, (unsigned)gotY);
    screenTanksDestroy(&tks);

    /* Twenty squares off: still in the list — the client has lost none of
     * what it knew — and the filter is what takes it off the picture. The
     * same square passes for the local tank, which is the one exception. */
    bogey->mapX = farX;
    clientSimDisplayTick(f.cs, false);
    overviewEntitiesPrepare(f.cs, &tks);
    UT_ASSERT_MSG(overviewFindTank(&tks, hostile, &gotX, &gotY) == TRUE,
                  "the hostile tank dropped out of the prepared list at %u,%u "
                  "— this case would then prove absence, not filtering",
                  (unsigned)farX, (unsigned)atY);
    UT_ASSERT_MSG(gotX == farX && gotY == atY,
                  "the prepared list puts the hostile tank on %u,%u, expected "
                  "%u,%u", (unsigned)gotX, (unsigned)gotY, (unsigned)farX,
                  (unsigned)atY);
    UT_ASSERT_MSG((om->flags[farX][atY] & OVERVIEW_F_LIVE) == 0,
                  "square %u,%u is live — it was meant to be outside the "
                  "tank's block", (unsigned)farX, (unsigned)atY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY, false) == false,
                  "an enemy tank on unseen square %u,%u would be drawn",
                  (unsigned)gotX, (unsigned)gotY);
    UT_ASSERT_MSG(overviewEntityIsVisible(om, gotX, gotY, true) == true,
                  "the local tank would be hidden on square %u,%u",
                  (unsigned)gotX, (unsigned)gotY);
    screenTanksDestroy(&tks);

    overviewFixtureStop(&f);
    return 0;
}

/* Seeded into the gunsight out-params before every read, so a call that fails
 * and still writes something shows it. */
#define GUNSIGHT_SENTINEL 0xEE

int run_overview_gunsight(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Gunsight");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    BYTE mx = GUNSIGHT_SENTINEL;
    BYTE my = GUNSIGHT_SENTINEL;
    BYTE px = GUNSIGHT_SENTINEL;
    BYTE py = GUNSIGHT_SENTINEL;

    /* A tank starts with the sight hidden — the same flag the Show Gunsight
     * preference and auto-hide drive. Nothing to draw, and nothing written. */
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) != TRUE,
                  "a hidden gunsight reported a position");
    UT_ASSERT_MSG(mx == GUNSIGHT_SENTINEL && my == GUNSIGHT_SENTINEL &&
                      px == GUNSIGHT_SENTINEL && py == GUNSIGHT_SENTINEL,
                  "a failed read overwrote the caller's variables with %u,%u "
                  "%u,%u", (unsigned)mx, (unsigned)my, (unsigned)px,
                  (unsigned)py);

    /* Shown, on a living tank: the case the crosshair is drawn for. */
    clientSimSetGunsight(f.cs, true);
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) == TRUE,
                  "a shown gunsight on a living tank reported nothing");

    /* Dead but still in its slot, which is what the server leaves behind for
     * the whole of deathWait. The sight is still shown, so dying is the only
     * thing the accessor can be answering to. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)(TANK_FULL_ARMOUR + 1));
    mx = my = px = py = GUNSIGHT_SENTINEL;
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) != TRUE,
                  "a dead tank reported a gunsight position");
    UT_ASSERT_MSG(mx == GUNSIGHT_SENTINEL && my == GUNSIGHT_SENTINEL &&
                      px == GUNSIGHT_SENTINEL && py == GUNSIGHT_SENTINEL,
                  "a failed read overwrote the caller's variables with %u,%u "
                  "%u,%u", (unsigned)mx, (unsigned)my, (unsigned)px,
                  (unsigned)py);

    /* The difference this accessor exists for, pinned on the same state: the
     * tile accessor answers for a dead tank, and answers with the map origin
     * — a crosshair drawn from it would sit in the map's top-left corner. */
    {
        BYTE tileX = GUNSIGHT_SENTINEL;
        BYTE tileY = GUNSIGHT_SENTINEL;

        UT_ASSERT_MSG(clientSimGetGunsightTile(f.cs, &tileX, &tileY) == TRUE,
                      "clientSimGetGunsightTile stopped answering for a dead "
                      "tank — this case would then pin no difference");
        UT_ASSERT_MSG(tileX == 0 && tileY == 0,
                      "clientSimGetGunsightTile reads %u,%u for a dead tank, "
                      "expected the map origin", (unsigned)tileX,
                      (unsigned)tileY);
    }

    /* Respawned: the accessor answers again, so discounting a dead tank is not
     * a one-way door. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &mx, &my, &px, &py) == TRUE,
                  "a respawned tank reported no gunsight position");

    overviewFixtureStop(&f);
    return 0;
}

int run_overview_reset(void) {
    OverviewFixture f;
    const char *err = overviewFixtureStart(&f, "Reset");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    const OverviewMap *om = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(om != NULL, "clientSimGetOverviewMap returned NULL");

    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->seenCount > 0, "nothing was seen after a tick");

    /* Round over: the memory of the round that ended goes with it. */
    clientSimResetWorld(f.cs);
    UT_ASSERT_MSG(om->seenCount == 0, "seenCount %u after a world reset",
                  om->seenCount);
    UT_ASSERT_MSG(om->liveCount == 0, "liveCount %d after a world reset",
                  om->liveCount);
    {
        int x, y;
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            for (y = 0; y < MAP_ARRAY_SIZE; y++) {
                UT_ASSERT_MSG(om->tile[x][y] == OVERVIEW_UNSEEN,
                              "square %d,%d holds tile %u after a world reset",
                              x, y, (unsigned)om->tile[x][y]);
                UT_ASSERT_MSG(om->flags[x][y] == 0,
                              "square %d,%d carries flags %u after a world reset",
                              x, y, (unsigned)om->flags[x][y]);
            }
        }
    }

    clientSimDisplayTick(f.cs, false);
    unsigned seenAgain = om->seenCount;
    UT_ASSERT_MSG(seenAgain > 0, "nothing was seen after the reset and a tick");

    /* A mid-game resync reinstalls the same map with the viewport left alone,
     * and the memory has to survive it — the player has not stopped having
     * been where they have been. */
    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(installCompressedMap(f.cs, emap, OVERVIEW_EMAP_LEN,
                                           "Everard Island", false) == TRUE,
                      "installCompressedMap rejected the Everard Island blob");
    }
    UT_ASSERT_MSG(om->seenCount == seenAgain,
                  "a resync moved seenCount from %u to %u", seenAgain,
                  om->seenCount);

    /* A fresh install — the lobby map-change path — resets the memory and
     * arms the seed: black until the next tick, then the whole map reads
     * dimmed again. */
    {
        BYTE emap[6000] = E_MAP;
        UT_ASSERT_MSG(installCompressedMap(f.cs, emap, OVERVIEW_EMAP_LEN,
                                           "Everard Island", true) == TRUE,
                      "installCompressedMap rejected the fresh install");
    }
    UT_ASSERT_MSG(om->seenCount == 0,
                  "a fresh install left seenCount at %u before the seeding "
                  "tick", om->seenCount);
    clientSimDisplayTick(f.cs, false);
    UT_ASSERT_MSG(om->seenCount ==
                      (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE),
                  "seenCount %u one tick after a fresh install, expected "
                  "every square (%u)", om->seenCount,
                  (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));

    overviewFixtureStop(&f);
    return 0;
}

/* --------------------------------------------------------------------------
 * The same reveal over the real UDP path, where the map arrives as a download
 * and terrain changes arrive out of band as map events rather than being
 * written into the client's own sim. The mask has to come out the same.
 * -------------------------------------------------------------------------- */

/* Convergence ceilings for a clean path. Every pump costs a millisecond of
 * SDL_Delay, so these are bounds to report a failure against, not expected
 * counts - the harness contract is "converges within N pumps", never a packet
 * trace. */
#define OVERVIEW_LOOP_CONNECT_MAX 2000
#define OVERVIEW_LOOP_READY_MAX   2000
#define OVERVIEW_LOOP_MAP_MAX     2000

/* Seeds the harness's bolo_rand stream. Nothing on a clean path draws from it;
 * it keeps the run reproducible regardless. */
#define OVERVIEW_LOOP_SEED 0x5EED0Fu

/* Half-width of the block the hidden square is picked from, far enough off the
 * tank that the tank's own 29x29 block cannot reach it. */
#define OVERVIEW_LOOP_FAR_HALF 4

static bool overviewLoopConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* The server only flips its own download-complete flag once the map's bytes
 * are acked back on CHANNEL_BULK, a round-trip after the client reports
 * CONNECTED. Waiting for it means snapshots have been flowing for a while, so
 * the tank position read below is one the wire delivered rather than whatever
 * the slot-assignment path left on a freshly created tank. */
static bool overviewLoopServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

static bool overviewLoopHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0; /* Discarded - the position is read again once settled */
    BYTE my = 0;

    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

/* The square and terrain overviewLoopTerrainApplied is waiting on. */
typedef struct OverviewTerrainWait {
    BYTE x;
    BYTE y;
    BYTE terrain;
} OverviewTerrainWait;

static bool overviewLoopTerrainApplied(LoopbackHarness *h, void *user) {
    const OverviewTerrainWait *w = (const OverviewTerrainWait *)user;

    return clientSimGetMapTerrain(h->cs, w->x, w->y) == w->terrain;
}

int run_overview_loopback(void) {
    LoopbackHarness h;
    int at; /* Pump the last wait converged on, or -1 */

    if (loopbackHarnessStart(&h, "Loop", /*lobbyMode*/ false,
                             /*impairSpec*/ NULL, OVERVIEW_LOOP_SEED) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (overview loopback) failed");
    }

    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_CONNECT_MAX,
                                  overviewLoopConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps",
                OVERVIEW_LOOP_CONNECT_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_READY_MAX,
                                  overviewLoopServerReady, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never saw the map download acked within %d pumps",
                OVERVIEW_LOOP_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_READY_MAX,
                                  overviewLoopHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps",
                OVERVIEW_LOOP_READY_MAX);
    }

    GameSim *gs = clientSimGetGameSim(h.cs);
    BYTE me = clientSimGetMyPlayerNum(h.cs);
    int slot = (int)me;
    BYTE tankMX = 0;
    BYTE tankMY = 0;
    if (gs == NULL || clientSimGetMyTankMapPos(h.cs, &tankMX, &tankMY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no client sim or tank position after convergence");
    }

    /* Nothing below touches pills or terrain before the assertions: the world
     * being checked is the one the download and the snapshots delivered. Any
     * pill the map ships that the player can view through simply adds its block
     * to the expected set. */
    clientSimDisplayTick(h.cs, false);

    const OverviewMap *om = clientSimGetOverviewMap(h.cs);
    if (om == NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("clientSimGetOverviewMap returned NULL");
    }

    OverviewRect expect[OVERVIEW_MAX_REGIONS];
    int n = overviewMapBuildRegions(gs, me, TRUE, tankMX, tankMY,
                                    OVERVIEW_TANK_HALF, expect,
                                    OVERVIEW_MAX_REGIONS);
    if (n < 1) {
        loopbackHarnessStop(&h);
        UT_FAIL("a client with a tank produced %d regions", n);
    }
    if (om->liveCount != n) {
        loopbackHarnessStop(&h);
        UT_FAIL("liveCount %d, expected %d", om->liveCount, n);
    }

    /* Every square of the map, either side of the union. The mismatch is
     * carried out of the loop rather than asserted in it, so the harness is
     * always torn down before the failure returns. */
    const char *bad = NULL;
    int badX = 0;
    int badY = 0;
    unsigned badGot = 0;
    unsigned badWant = 0;
    int x, y;
    for (x = 0; x < MAP_ARRAY_SIZE && bad == NULL; x++) {
        for (y = 0; y < MAP_ARRAY_SIZE; y++) {
            BYTE flags = om->flags[x][y];

            if (overviewInAnyRect(expect, n, x, y) == TRUE) {
                bool isMine = FALSE;
                BYTE want = viewportCalcSquarePure(gs, me, (BYTE)x, (BYTE)y,
                                                   &isMine);
                if ((flags & OVERVIEW_F_LIVE) == 0) {
                    bad = "is inside a region but not live";
                } else if (om->tile[x][y] != want) {
                    bad = "holds a tile the calculator disagrees with";
                    badGot = om->tile[x][y];
                    badWant = want;
                } else if (((flags & OVERVIEW_F_MINE) != 0) !=
                           (isMine == TRUE)) {
                    bad = "carries the wrong mine flag";
                    badGot = (flags & OVERVIEW_F_MINE) != 0;
                    badWant = (isMine == TRUE);
                }
            } else {
                if ((flags & OVERVIEW_F_LIVE) != 0) {
                    bad = "is outside every region but live";
                } else if (om->tile[x][y] == OVERVIEW_UNSEEN) {
                    bad = "was left out of the seed";
                    badGot = OVERVIEW_UNSEEN;
                    badWant = 0;
                }
            }
            if (bad != NULL) {
                badX = x;
                badY = y;
                break;
            }
        }
    }
    if (bad != NULL) {
        loopbackHarnessStop(&h);
        UT_FAIL("square %d,%d %s (got %u, expected %u)", badX, badY, bad,
                badGot, badWant);
    }
    if (om->seenCount != (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE)) {
        loopbackHarnessStop(&h);
        UT_FAIL("seenCount %u after the seed, expected every square (%u)",
                om->seenCount, (unsigned)(MAP_ARRAY_SIZE * MAP_ARRAY_SIZE));
    }

    /* One square the tank can see and one nothing has revealed. Both are found
     * by probing for a tile that actually moves when the ground under it is
     * swapped, which steps off pill and base squares - those render from their
     * owner, so a terrain change on one would prove nothing either way. The
     * probe puts back what it wrote and no pump runs during it, so the client's
     * map is untouched by the time the events are staged. */
    OverviewRect liveRect = expect[0]; /* the tank's rect is written first */
    OverviewRect farRect = overviewTestBlock(
        (int)tankMX + overviewAwayFromEdge(tankMX) * 100, (int)tankMY,
        OVERVIEW_LOOP_FAR_HALF);
    BYTE liveX = 0, liveY = 0, farX = 0, farY = 0;

    if (overviewFindTerrainSquare(gs, me, &liveRect, &liveX, &liveY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no terrain-driven square in the tank's block at %u,%u",
                (unsigned)tankMX, (unsigned)tankMY);
    }
    if (overviewFindTerrainSquare(gs, me, &farRect, &farX, &farY) != TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("no terrain-driven square in the block 100 off the tank");
    }
    if ((om->flags[farX][farY] & OVERVIEW_F_LIVE) != 0 ||
        overviewInAnyRect(expect, n, farX, farY) == TRUE) {
        loopbackHarnessStop(&h);
        UT_FAIL("the square meant to stay hidden, %u,%u, is live",
                (unsigned)farX, (unsigned)farY);
    }

    BYTE liveTileBefore = om->tile[liveX][liveY];
    BYTE farTileBefore = om->tile[farX][farY];
    OverviewTerrainWait wait;

    /* Both changes go out the way a sim tick emits one: the server's own map
     * moves and an EVENT_MAP_CHANGE rides CHANNEL_MAP to the client. */
    wait.x = liveX;
    wait.y = liveY;
    wait.terrain = overviewFlipTerrain(clientSimGetMapTerrain(h.cs, liveX, liveY));
    if (transportUdpServerTestAddMapEvent(h.sim, slot, liveX, liveY,
                                          wait.terrain) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("staging a map event on the live square %u,%u was rejected",
                (unsigned)liveX, (unsigned)liveY);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_MAP_MAX,
                                  overviewLoopTerrainApplied, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the change on live square %u,%u never reached the client "
                "within %d pumps", (unsigned)liveX, (unsigned)liveY,
                OVERVIEW_LOOP_MAP_MAX);
    }

    wait.x = farX;
    wait.y = farY;
    wait.terrain = overviewFlipTerrain(clientSimGetMapTerrain(h.cs, farX, farY));
    if (transportUdpServerTestAddMapEvent(h.sim, slot, farX, farY,
                                          wait.terrain) != true) {
        loopbackHarnessStop(&h);
        UT_FAIL("staging a map event on the hidden square %u,%u was rejected",
                (unsigned)farX, (unsigned)farY);
    }
    at = loopbackHarnessPumpUntil(&h, OVERVIEW_LOOP_MAP_MAX,
                                  overviewLoopTerrainApplied, &wait);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the change on hidden square %u,%u never reached the client "
                "within %d pumps", (unsigned)farX, (unsigned)farY,
                OVERVIEW_LOOP_MAP_MAX);
    }

    clientSimDisplayTick(h.cs, false);

    /* The live square follows the change. The tile it should now show is
     * recomputed here rather than before the pumps, so tree growth landing on
     * or beside it while the events were in flight moves the memory and the
     * expectation together instead of breaking the comparison. */
    if ((om->flags[liveX][liveY] & OVERVIEW_F_LIVE) == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("square %u,%u dropped out of the live set while the change was "
                "in flight - did the tank move?", (unsigned)liveX,
                (unsigned)liveY);
    }
    {
        bool isMine = FALSE;
        BYTE wantTile = viewportCalcSquarePure(gs, me, liveX, liveY, &isMine);

        if (wantTile == liveTileBefore) {
            loopbackHarnessStop(&h);
            UT_FAIL("terrain %u renders as tile %u at %u,%u either way - the "
                    "live check would pass on its own",
                    (unsigned)wait.terrain, (unsigned)wantTile,
                    (unsigned)liveX, (unsigned)liveY);
        }
        if (om->tile[liveX][liveY] != wantTile) {
            loopbackHarnessStop(&h);
            UT_FAIL("live square %u,%u holds tile %u, expected %u",
                    (unsigned)liveX, (unsigned)liveY,
                    (unsigned)om->tile[liveX][liveY], (unsigned)wantTile);
        }
    }

    /* The hidden square does not. The client has the new terrain - the pump
     * above waited for it - and the memory still shows what the seed stamped
     * there, so a change on ground the player cannot see stays invisible. */
    if (om->tile[farX][farY] != farTileBefore) {
        loopbackHarnessStop(&h);
        UT_FAIL("a map event restamped hidden square %u,%u: tile %u, "
                "expected %u", (unsigned)farX, (unsigned)farY,
                (unsigned)om->tile[farX][farY], (unsigned)farTileBefore);
    }

    loopbackHarnessStop(&h);
    return 0;
}
