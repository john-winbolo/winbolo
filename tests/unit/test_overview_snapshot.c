/*
 * The map overview's render snapshot (test_overview_snapshot.c).
 *
 * clientSimFillOverviewSnapshot copies what the overview's render reads out of
 * a ClientSim while the caller holds the client mutex, so the render can run
 * after the mutex is released. These cases pin that the copy is faithful, and
 * that it is a copy.
 *
 * mirror: after a fill the snapshot's map is byte for byte the live memory and
 * every scalar matches the accessor it was read from, across a living tank, a
 * shown gunsight, an item view, a dead tank and a fill from no sim.
 *
 * isolation: writing to the live memory after a fill, straight into it and
 * through a display tick, leaves the snapshot as it was, and the next fill
 * picks the change up.
 *
 * filter: the entity lists come out of the fill exactly as the renderer's own
 * filter used to produce them from the whole-map lists — the same entries in
 * the same order, an enemy outside the live set dropped, an explosion the
 * same, and the player's own dead tank dropped with selfDrawn false.
 *
 * generation: a fill with the generation counter unchanged still yields the
 * live map, and does so without copying — a write to the live memory that
 * moves no counter is not seen until something does move it.
 */

#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "game_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_sim_internal.h" /* cs->overview — the live memory, written to directly */
#include "client_command.h"      /* VIEW_KIND_PILL */
#include "input_packet.h"
#include "viewport.h"            /* viewportCalcSquarePure — which square a terrain flip moves */
#include "bolo_map.h"            /* mapGetPos / mapSetPos */
#include "tank.h"                /* tankSetArmour, tankSetDestroyed */
#include "pillbox.h"
#include "players.h"
#include "allience.h"
#include "explosions.h"          /* explosionsAddItem — a bullet-list entry to place */
#include "overview_camera.h"     /* overviewEntityIsVisible — the renderer's filter */
#include "test_harness.h"

/* Rounds of the input-pair warm-up to run before the client's tank is
 * expected, and the ceiling on how long to keep pumping for it. The active
 * local transport runs two server half-steps per clientSimNetTick, so the
 * producer has to supply two tick numbers per tick or the stream starves into
 * substitutes. */
#define SNAP_WARMUP_TICKS     4
#define SNAP_MAX_WARMUP_TICKS 32

/* Seeded into every out-param before a read that is expected to decline, so
 * a call that fails and still writes something shows it. */
#define SNAP_SENTINEL 0xEE

typedef struct SnapFixture {
    ServerSim        *sim;
    ClientSim        *cs;
    GameSim          *gs;      /* the client's sim, not the server's */
    BYTE              me;      /* the slot the local join landed on */
    BYTE              tankMX;  /* where the client's tank sits, as the overview reads it */
    BYTE              tankMY;
    OverviewSnapshot *snap;
} SnapFixture;

/* Tears the fixture down. Safe on a half-built fixture so the start path can
 * bail anywhere. */
static void snapFixtureStop(SnapFixture *f) {
    if (f->snap != NULL) {
        overviewSnapshotDestroy(f->snap);
        f->snap = NULL;
    }
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
 * pumps the local transport until the client's own tank has landed, then makes
 * the snapshot the cases fill. Returns NULL on success, or a message naming
 * what failed; either way the caller finishes with snapFixtureStop. */
static const char *snapFixtureStart(SnapFixture *f, const char *name) {
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
    for (i = 0; i < SNAP_MAX_WARMUP_TICKS; i++) {
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

        if (i + 1 >= SNAP_WARMUP_TICKS &&
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

    f->snap = overviewSnapshotCreate();
    if (f->snap == NULL) {
        return "overviewSnapshotCreate returned NULL";
    }
    return NULL;
}

/* The direction with room in it: everything below steps away from the nearer
 * map edge, so a square 20 or 40 off the tank still lands on the map wherever
 * the join dropped the tank. */
static int snapAwayFromEdge(BYTE mapCoord) {
    return (mapCoord < MAP_ARRAY_SIZE / 2) ? 1 : -1;
}

/* A map corner on the far side of the map from the tank: outside every live
 * region, so nothing a display tick does rewrites it. */
static void snapFarCorner(const SnapFixture *f, BYTE *outX, BYTE *outY) {
    *outX = (BYTE)((f->tankMX < MAP_ARRAY_SIZE / 2) ? MAP_ARRAY_SIZE - 2 : 1);
    *outY = (BYTE)((f->tankMY < MAP_ARRAY_SIZE / 2) ? MAP_ARRAY_SIZE - 2 : 1);
}

/* A terrain byte that has to read differently from what is there now. Sea and
 * grass are far enough apart that no adjacency calculation can collapse the
 * two onto one sprite. */
static BYTE snapFlipTerrain(BYTE terrain) {
    return (terrain == DEEP_SEA) ? (BYTE)GRASS : (BYTE)DEEP_SEA;
}

/* A square beside the tank whose tile actually moves when the terrain under
 * it is swapped — a pill or base square renders from its owner whatever the
 * ground says. Found by writing and putting back, so the map is left as it
 * was; call it before the first display tick so the memory never sees the
 * search. */
static bool snapFindTerrainSquare(const SnapFixture *f, BYTE *outX,
                                  BYTE *outY) {
    int x; /* Looping variable */
    int y; /* Looping variable */

    for (x = (int)f->tankMX - 3; x <= (int)f->tankMX + 3; x++) {
        for (y = (int)f->tankMY - 3; y <= (int)f->tankMY + 3; y++) {
            bool isMine = FALSE;
            BYTE was;
            BYTE before;
            BYTE after;

            if (x < 0 || y < 0 || x >= MAP_ARRAY_SIZE || y >= MAP_ARRAY_SIZE) {
                continue;
            }
            was = mapGetPos(&f->gs->mp, (BYTE)x, (BYTE)y);
            before = viewportCalcSquarePure(f->gs, f->me, (BYTE)x, (BYTE)y,
                                            &isMine);
            mapSetPos(f->gs, &f->gs->mp, (BYTE)x, (BYTE)y,
                      snapFlipTerrain(was), TRUE, TRUE);
            after = viewportCalcSquarePure(f->gs, f->me, (BYTE)x, (BYTE)y,
                                           &isMine);
            mapSetPos(f->gs, &f->gs->mp, (BYTE)x, (BYTE)y, was, TRUE, TRUE);
            if (after != before) {
                *outX = (BYTE)x;
                *outY = (BYTE)y;
                return TRUE;
            }
        }
    }
    return FALSE;
}

/* Swaps the terrain under a square the client's memory is live on and runs
 * the tick that stamps it, so the live tile and the generation both move. */
static void snapMoveLiveSquare(SnapFixture *f, BYTE x, BYTE y) {
    BYTE was = mapGetPos(&f->gs->mp, x, y);

    mapSetPos(f->gs, &f->gs->mp, x, y, snapFlipTerrain(was), TRUE, TRUE);
    clientSimDisplayTick(f->cs, false);
}

/* A hostile tank driven straight into the client's own player table — the
 * same state a snapshot would have left behind. pixelX/pixelY of 8 put it in
 * the middle of its square, so the list reports the square it was placed on.
 * Its LGM is put out on the same square. */
static player *snapPlaceHostile(SnapFixture *f, BYTE hostile, BYTE mx,
                                BYTE my) {
    player *bogey = &f->gs->plyrs->item[hostile];

    bogey->inUse = TRUE;
    strcpy(bogey->playerName, "Bogey");
    bogey->frame  = 0;
    bogey->onBoat = FALSE;
    bogey->pixelX = 8;
    bogey->pixelY = 8;
    bogey->mapX   = mx;
    bogey->mapY   = my;
    bogey->lgmMapX   = mx;
    bogey->lgmMapY   = my;
    bogey->lgmPixelX = 8;
    bogey->lgmPixelY = 8;
    bogey->lgmFrame  = 0;
    return bogey;
}

/* The square a player's tank is listed on, or FALSE when the list does not
 * carry that player at all. */
static bool snapFindTank(const screenTanks *tks, BYTE playerNum, BYTE *outX,
                         BYTE *outY) {
    BYTE total = screenTanksGetNumEntries(tks);
    BYTE count; /* Looping variable */

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame, pn;
        char name[PLAYER_NAME_LEN];

        screenTanksGetItem(tks, count, &mx, &my, &px, &py, &frame, &pn, name);
        if (pn == playerNum) {
            if (outX) *outX = mx;
            if (outY) *outY = my;
            return TRUE;
        }
    }
    return FALSE;
}

/* Whether some LGM in the list stands on the square. */
static bool snapLgmOn(const screenLgm *lgms, BYTE x, BYTE y) {
    BYTE total = screenLgmGetNumEntries(lgms);
    BYTE count; /* Looping variable */

    for (count = 1; count <= total; count++) {
        BYTE mx, my, px, py, frame;

        screenLgmGetItem(lgms, count, &mx, &my, &px, &py, &frame);
        if (mx == x && my == y) return TRUE;
    }
    return FALSE;
}

/* The filter as the renderer ran it before the fill took it over, kept here
 * as it was and against the renderer's own overviewEntityIsVisible, so the
 * lists the fill hands out can be checked against it entry for entry. The one
 * change since: a tank is asked about the square under its centre rather than
 * the square its sprite starts in, which the fill's filter does too. */
static void snapOldFilter(const OverviewMap *om, BYTE me, bool selfAlive,
                          const screenTanks *allTks, const screenLgm *allLgms,
                          const screenBullets *allSb, screenTanks *outTks,
                          screenLgm *outLgms, screenBullets *outSb,
                          bool *outSelfDrawn) {
    BYTE mx, my, px, py, frame, playerNum;
    BYTE wx, wy, angle;
    char name[PLAYER_NAME_LEN];
    BYTE count;
    BYTE total;
    int bulletTotal;
    int bullet;

    *outSelfDrawn = false;

    total = screenTanksGetNumEntries(allTks);
    for (count = 1; count <= total; count++) {
        screenTanksGetItem(allTks, count, &mx, &my, &px, &py, &frame,
                           &playerNum, name);
        bool isSelf = (playerNum == me);
        BYTE cx = mx, cy = my;
        if (isSelf && !selfAlive) continue;
        screenTanksGetCentreSquare(allTks, count, &cx, &cy);
        if (!overviewEntityIsVisible(om, cx, cy)) continue;
        screenTanksGetSubPixel(allTks, count, &wx, &wy, &angle);
        screenTanksAddItem(outTks, mx, my, px, py, frame, playerNum, name,
                           wx, wy, angle);
        if (isSelf) *outSelfDrawn = true;
    }

    total = screenLgmGetNumEntries(allLgms);
    for (count = 1; count <= total; count++) {
        screenLgmGetItem(allLgms, count, &mx, &my, &px, &py, &frame);
        if (!overviewEntityIsVisible(om, mx, my)) continue;
        screenLgmGetSubPixel(allLgms, count, &wx, &wy);
        screenLgmAddItem(outLgms, mx, my, px, py, frame, wx, wy);
    }

    bulletTotal = screenBulletsGetNumEntries(allSb);
    for (bullet = 1; bullet <= bulletTotal; bullet++) {
        screenBulletsGetItem(allSb, bullet, &mx, &my, &px, &py, &frame);
        if (!overviewEntityIsVisible(om, mx, my)) continue;
        screenBulletsGetSubPixel(allSb, bullet, &wx, &wy);
        screenBulletsAddItem(outSb, mx, my, px, py, frame, wx, wy);
    }
}

static bool snapTanksEqual(const screenTanks *a, const screenTanks *b) {
    BYTE total = screenTanksGetNumEntries(a);
    BYTE count; /* Looping variable */

    if (total != screenTanksGetNumEntries(b)) return FALSE;
    for (count = 1; count <= total; count++) {
        BYTE amx, amy, apx, apy, aframe, apn, awx, awy, aangle;
        BYTE bmx, bmy, bpx, bpy, bframe, bpn, bwx, bwy, bangle;
        char aname[PLAYER_NAME_LEN];
        char bname[PLAYER_NAME_LEN];

        screenTanksGetItem(a, count, &amx, &amy, &apx, &apy, &aframe, &apn,
                           aname);
        screenTanksGetItem(b, count, &bmx, &bmy, &bpx, &bpy, &bframe, &bpn,
                           bname);
        screenTanksGetSubPixel(a, count, &awx, &awy, &aangle);
        screenTanksGetSubPixel(b, count, &bwx, &bwy, &bangle);
        if (amx != bmx || amy != bmy || apx != bpx || apy != bpy ||
            aframe != bframe || apn != bpn || awx != bwx || awy != bwy ||
            aangle != bangle || strcmp(aname, bname) != 0) {
            return FALSE;
        }
    }
    return TRUE;
}

static bool snapLgmsEqual(const screenLgm *a, const screenLgm *b) {
    BYTE total = screenLgmGetNumEntries(a);
    BYTE count; /* Looping variable */

    if (total != screenLgmGetNumEntries(b)) return FALSE;
    for (count = 1; count <= total; count++) {
        BYTE amx, amy, apx, apy, aframe, awx, awy;
        BYTE bmx, bmy, bpx, bpy, bframe, bwx, bwy;

        screenLgmGetItem(a, count, &amx, &amy, &apx, &apy, &aframe);
        screenLgmGetItem(b, count, &bmx, &bmy, &bpx, &bpy, &bframe);
        screenLgmGetSubPixel(a, count, &awx, &awy);
        screenLgmGetSubPixel(b, count, &bwx, &bwy);
        if (amx != bmx || amy != bmy || apx != bpx || apy != bpy ||
            aframe != bframe || awx != bwx || awy != bwy) {
            return FALSE;
        }
    }
    return TRUE;
}

static bool snapBulletsEqual(const screenBullets *a, const screenBullets *b) {
    int total = screenBulletsGetNumEntries(a);
    int count; /* Looping variable */

    if (total != screenBulletsGetNumEntries(b)) return FALSE;
    for (count = 1; count <= total; count++) {
        BYTE amx, amy, apx, apy, aframe, awx, awy;
        BYTE bmx, bmy, bpx, bpy, bframe, bwx, bwy;

        screenBulletsGetItem(a, count, &amx, &amy, &apx, &apy, &aframe);
        screenBulletsGetItem(b, count, &bmx, &bmy, &bpx, &bpy, &bframe);
        screenBulletsGetSubPixel(a, count, &awx, &awy);
        screenBulletsGetSubPixel(b, count, &bwx, &bwy);
        if (amx != bmx || amy != bmy || apx != bpx || apy != bpy ||
            aframe != bframe || awx != bwx || awy != bwy) {
            return FALSE;
        }
    }
    return TRUE;
}

/* Builds the lists the renderer drew from before the move — the whole-map
 * lists through the old filter — from the sim as it stands, and compares them
 * with what the last fill left in the snapshot. Call with no tick between the
 * fill and this, so both read the same state. NULL when they agree, else a
 * message naming the first list that does not. */
static const char *snapListsMatch(const SnapFixture *f) {
    screenTanks   allTks, wantTks;
    screenLgm     allLgms, wantLgms;
    screenBullets allSb, wantSb;
    bool          wantSelfDrawn = false;
    const char   *err = NULL;

    screenTanksCreate(&allTks);
    screenTanksCreate(&wantTks);
    screenLgmCreate(&allLgms);
    screenLgmCreate(&wantLgms);
    allSb  = screenBulletsCreate();
    wantSb = screenBulletsCreate();

    clientSimPrepareOverviewEntities(f->cs, &allTks, &allLgms, &allSb);
    snapOldFilter(clientSimGetOverviewMap(f->cs), clientSimGetMyPlayerNum(f->cs),
                  clientSimIsMyTankAlive(f->cs), &allTks, &allLgms, &allSb,
                  &wantTks, &wantLgms, &wantSb, &wantSelfDrawn);

    if (!snapTanksEqual(overviewSnapshotTanks(f->snap), &wantTks)) {
        err = "the snapshot's tank list differs from the old filter's";
    } else if (!snapLgmsEqual(overviewSnapshotLgms(f->snap), &wantLgms)) {
        err = "the snapshot's LGM list differs from the old filter's";
    } else if (!snapBulletsEqual(overviewSnapshotBullets(f->snap), &wantSb)) {
        err = "the snapshot's bullet list differs from the old filter's";
    } else if (overviewSnapshotSelfDrawn(f->snap) != wantSelfDrawn) {
        err = "the snapshot's selfDrawn differs from the old filter's";
    }

    screenBulletsDestroy(&wantSb);
    screenBulletsDestroy(&allSb);
    screenLgmDestroy(&wantLgms);
    screenLgmDestroy(&allLgms);
    screenTanksDestroy(&wantTks);
    screenTanksDestroy(&allTks);
    return err;
}

/* Puts the client's item view machinery in the state that holds a view: the
 * per-tick UI pass that keeps or drops it is skipped while the game is
 * starting, over or off the network. */
static void snapSayRunning(SnapFixture *f) {
    clientSimSetRunning(f->cs, TRUE);
    clientSimSetNetStatus(f->cs, netRunning);
    clientSimSetGmeStartDelay(f->cs, 0);
    clientSimSetGmeLength(f->cs, -1);
}

int run_overview_snapshot_mirror(void) {
    SnapFixture f;
    const char *err = snapFixtureStart(&f, "Mirror");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* No pills, so the tank's 29x29 block is the whole live set. */
    f.gs->pb->numPills = 0;
    clientSimDisplayTick(f.cs, false);

    const OverviewMap *live = clientSimGetOverviewMap(f.cs);
    UT_ASSERT_MSG(live != NULL, "clientSimGetOverviewMap returned NULL");

    /* A living tank, sight hidden, tank view. */
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    const OverviewMap *got = overviewSnapshotMap(f.snap);
    UT_ASSERT_MSG(got != NULL, "the snapshot has no map after a fill from a live sim");
    UT_ASSERT_MSG(got != live,
                  "the snapshot hands back the live memory itself rather than "
                  "a copy of it");
    UT_ASSERT_MSG(memcmp(got, live, sizeof(OverviewMap)) == 0,
                  "the snapshot's map differs from the live memory after a fill");
    UT_ASSERT_MSG(overviewSnapshotMyPlayerNum(f.snap) == f.me,
                  "player number %u in the snapshot, expected %u",
                  (unsigned)overviewSnapshotMyPlayerNum(f.snap), (unsigned)f.me);
    UT_ASSERT_MSG(clientSimIsMyTankAlive(f.cs) == TRUE,
                  "the fixture's tank is not alive — nothing below would pin "
                  "the living case");
    UT_ASSERT_MSG(overviewSnapshotTankAlive(f.snap) == TRUE,
                  "a living tank reads as dead through the snapshot");
    {
        float sx = -1.0f, sy = -1.0f, lx = -2.0f, ly = -2.0f;
        UT_ASSERT_MSG(overviewSnapshotTankPos(f.snap, &sx, &sy) == TRUE,
                      "a living tank has no position in the snapshot");
        UT_ASSERT_MSG(clientSimGetMyTankMapPosF(f.cs, &lx, &ly) == TRUE,
                      "clientSimGetMyTankMapPosF declined for a living tank");
        UT_ASSERT_MSG(sx == lx && sy == ly,
                      "the snapshot puts the tank at %f,%f, the sim at %f,%f",
                      sx, sy, lx, ly);
    }
    UT_ASSERT_MSG(overviewSnapshotBlackout(f.snap) ==
                      clientSimIsMyTankDeathBlackout(f.cs),
                  "the snapshot's blackout flag disagrees with the sim's");
    UT_ASSERT_MSG(overviewSnapshotBlackout(f.snap) == FALSE,
                  "a living tank reads as in the death blackout");
    {
        BYTE mx = SNAP_SENTINEL, my = SNAP_SENTINEL;
        BYTE px = SNAP_SENTINEL, py = SNAP_SENTINEL;
        UT_ASSERT_MSG(overviewSnapshotGunsight(f.snap, &mx, &my, &px, &py) == FALSE,
                      "a hidden gunsight reported a position through the snapshot");
        UT_ASSERT_MSG(mx == SNAP_SENTINEL && my == SNAP_SENTINEL &&
                          px == SNAP_SENTINEL && py == SNAP_SENTINEL,
                      "a declined gunsight read wrote %u,%u %u,%u to the "
                      "caller's variables", (unsigned)mx, (unsigned)my,
                      (unsigned)px, (unsigned)py);
    }
    UT_ASSERT_MSG(overviewSnapshotInItemView(f.snap) == FALSE,
                  "the tank view reads as an item view");
    UT_ASSERT_MSG(overviewSnapshotViewKind(f.snap) == clientSimGetViewKind(f.cs),
                  "view kind %u in the snapshot, %u in the sim",
                  (unsigned)overviewSnapshotViewKind(f.snap),
                  (unsigned)clientSimGetViewKind(f.cs));
    UT_ASSERT_MSG(overviewSnapshotViewTarget(f.snap) ==
                      clientSimGetViewTarget(f.cs),
                  "view target %u in the snapshot, %u in the sim",
                  (unsigned)overviewSnapshotViewTarget(f.snap),
                  (unsigned)clientSimGetViewTarget(f.cs));
    {
        int ix = -1, iy = -1;
        overviewSnapshotItemViewSquare(f.snap, &ix, &iy);
        UT_ASSERT_MSG(ix == (int)clientSimGetPillViewX(f.cs) &&
                          iy == (int)clientSimGetPillViewY(f.cs),
                      "item view square %d,%d in the snapshot, %u,%u in the sim",
                      ix, iy, (unsigned)clientSimGetPillViewX(f.cs),
                      (unsigned)clientSimGetPillViewY(f.cs));
    }
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == TRUE,
                  "the living tank on its own live square did not survive the "
                  "filter");
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* Sight shown: the gunsight comes through with the accessor's answer. */
    clientSimSetGunsight(f.cs, true);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    {
        BYTE smx = 0, smy = 0, spx = 0, spy = 0;
        BYTE lmx = 1, lmy = 1, lpx = 1, lpy = 1;
        UT_ASSERT_MSG(overviewSnapshotGunsight(f.snap, &smx, &smy, &spx, &spy) == TRUE,
                      "a shown gunsight on a living tank reported nothing "
                      "through the snapshot");
        UT_ASSERT_MSG(clientSimGetGunsightPos(f.cs, &lmx, &lmy, &lpx, &lpy) == TRUE,
                      "clientSimGetGunsightPos declined for a shown sight");
        UT_ASSERT_MSG(smx == lmx && smy == lmy && spx == lpx && spy == lpy,
                      "the snapshot's gunsight %u,%u %u,%u is not the sim's "
                      "%u,%u %u,%u", (unsigned)smx, (unsigned)smy,
                      (unsigned)spx, (unsigned)spy, (unsigned)lmx,
                      (unsigned)lmy, (unsigned)lpx, (unsigned)lpy);
    }

    /* An item view: one pill of the client's own, forty squares off, watched
     * under viewPolicyKey so the block round the tank closes and the tank
     * goes off the picture with it. */
    int dir = snapAwayFromEdge(f.tankMX);
    BYTE pillX = (BYTE)((int)f.tankMX + dir * 40);
    pillsSetNumPills(&f.gs->pb, 1);
    f.gs->pb->item[0].owner  = f.me;
    f.gs->pb->item[0].armour = PILLBOX_15;
    f.gs->pb->item[0].inTank = FALSE;
    f.gs->pb->item[0].x      = pillX;
    f.gs->pb->item[0].y      = f.tankMY;
    snapSayRunning(&f);
    f.cs->viewPolicy[viewCategoryPill] = viewPolicyKey;
    clientSimPillView(f.cs, 0, 0);
    UT_ASSERT_MSG(clientSimGetViewKind(f.cs) == VIEW_KIND_PILL,
                  "the pill view did not take (kind %u)",
                  (unsigned)clientSimGetViewKind(f.cs));
    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), live, sizeof(OverviewMap)) == 0,
                  "the snapshot's map differs from the live memory inside an "
                  "item view");
    UT_ASSERT_MSG(clientSimIsInItemView(f.cs) == TRUE,
                  "the sim does not report an item view");
    UT_ASSERT_MSG(overviewSnapshotInItemView(f.snap) == TRUE,
                  "the item view does not show in the snapshot");
    UT_ASSERT_MSG(overviewSnapshotViewKind(f.snap) == VIEW_KIND_PILL,
                  "view kind %u in the snapshot inside a pill view",
                  (unsigned)overviewSnapshotViewKind(f.snap));
    UT_ASSERT_MSG(overviewSnapshotViewTarget(f.snap) ==
                      clientSimGetViewTarget(f.cs),
                  "view target %u in the snapshot, %u in the sim",
                  (unsigned)overviewSnapshotViewTarget(f.snap),
                  (unsigned)clientSimGetViewTarget(f.cs));
    {
        int ix = -1, iy = -1;
        overviewSnapshotItemViewSquare(f.snap, &ix, &iy);
        UT_ASSERT_MSG(ix == (int)clientSimGetPillViewX(f.cs) &&
                          iy == (int)clientSimGetPillViewY(f.cs),
                      "item view square %d,%d in the snapshot, %u,%u in the sim",
                      ix, iy, (unsigned)clientSimGetPillViewX(f.cs),
                      (unsigned)clientSimGetPillViewY(f.cs));
        UT_ASSERT_MSG(ix == (int)pillX && iy == (int)f.tankMY,
                      "the watched square is %d,%d, expected the pill at %u,%u",
                      ix, iy, (unsigned)pillX, (unsigned)f.tankMY);
    }
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == FALSE,
                  "the tank survived the filter with its block closed by a "
                  "key pill view");
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* Out of the view, and the tank is back. */
    clientSimTankView(f.cs);
    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(overviewSnapshotInItemView(f.snap) == FALSE,
                  "the snapshot still shows an item view after leaving it");
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == TRUE,
                  "the tank is still off the picture after leaving the view");
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* Dead and waiting to respawn, which is what the server leaves behind for
     * the whole of deathWait: nothing alive, no position, no sight, no tank on
     * the picture. */
    tankSetDestroyed(&f.gs->tanks[f.me], TRUE);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(overviewSnapshotTankAlive(f.snap) == FALSE,
                  "a dead tank reads as alive through the snapshot");
    {
        float sx = -1.0f, sy = -1.0f;
        UT_ASSERT_MSG(overviewSnapshotTankPos(f.snap, &sx, &sy) == FALSE,
                      "a dead tank has a position in the snapshot");
        UT_ASSERT_MSG(sx == -1.0f && sy == -1.0f,
                      "a declined position read wrote %f,%f to the caller's "
                      "variables", sx, sy);
    }
    {
        BYTE mx = SNAP_SENTINEL, my = SNAP_SENTINEL;
        BYTE px = SNAP_SENTINEL, py = SNAP_SENTINEL;
        UT_ASSERT_MSG(overviewSnapshotGunsight(f.snap, &mx, &my, &px, &py) == FALSE,
                      "a dead tank reported a gunsight through the snapshot");
    }
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == FALSE,
                  "a dead tank survived the filter");
    UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), f.me, NULL, NULL) == FALSE,
                  "a dead tank is on the snapshot's tank list");
    UT_ASSERT_MSG(overviewSnapshotBlackout(f.snap) ==
                      clientSimIsMyTankDeathBlackout(f.cs),
                  "the snapshot's blackout flag disagrees with the sim's for a "
                  "dead tank");
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* Respawned: the same fill answers again, so a death is not a one-way
     * door. */
    tankSetArmour(&f.gs->tanks[f.me], (BYTE)TANK_FULL_ARMOUR);
    tankSetDestroyed(&f.gs->tanks[f.me], FALSE);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(overviewSnapshotTankAlive(f.snap) == TRUE,
                  "a respawned tank still reads as dead");
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == TRUE,
                  "a respawned tank is still off the picture");

    /* No sim at all: the render is handed nothing to draw. */
    clientSimFillOverviewSnapshot(NULL, f.snap);
    UT_ASSERT_MSG(overviewSnapshotMap(f.snap) == NULL,
                  "a fill from no sim left a map to draw");
    UT_ASSERT_MSG(screenTanksGetNumEntries(overviewSnapshotTanks(f.snap)) == 0 &&
                      screenLgmGetNumEntries(overviewSnapshotLgms(f.snap)) == 0 &&
                      screenBulletsGetNumEntries(overviewSnapshotBullets(f.snap)) == 0,
                  "a fill from no sim left entries on the lists");
    UT_ASSERT_MSG(overviewSnapshotTankAlive(f.snap) == FALSE &&
                      overviewSnapshotSelfDrawn(f.snap) == FALSE &&
                      overviewSnapshotInItemView(f.snap) == FALSE &&
                      overviewSnapshotBlackout(f.snap) == FALSE,
                  "a fill from no sim left something alive, drawn, watched or "
                  "blacked out");
    /* And a NULL snapshot reads the same way, so a host with none yet draws
     * nothing rather than crashing. */
    UT_ASSERT_MSG(overviewSnapshotMap(NULL) == NULL &&
                      overviewSnapshotTankAlive(NULL) == FALSE &&
                      overviewSnapshotInItemView(NULL) == FALSE &&
                      overviewSnapshotTankPos(NULL, NULL, NULL) == FALSE,
                  "a NULL snapshot reads as something to draw");

    snapFixtureStop(&f);
    return 0;
}

int run_overview_snapshot_isolation(void) {
    SnapFixture f;
    const char *err = snapFixtureStart(&f, "Isolation");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    f.gs->pb->numPills = 0;

    /* Found before the memory's first tick, so it never sees the search. */
    BYTE flipX = 0, flipY = 0;
    UT_ASSERT_MSG(snapFindTerrainSquare(&f, &flipX, &flipY) == TRUE,
                  "no square beside the tank moves its tile on a terrain flip");

    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);

    OverviewMap *live = &f.cs->overview;
    UT_ASSERT_MSG(overviewSnapshotMap(f.snap) != NULL,
                  "the snapshot has no map after a fill");
    UT_ASSERT_MSG((live->flags[flipX][flipY] & OVERVIEW_F_LIVE) != 0,
                  "square %u,%u beside the tank is not live",
                  (unsigned)flipX, (unsigned)flipY);

    /* What the fill left, kept aside to compare against. */
    OverviewMap *before = (OverviewMap *)malloc(sizeof(OverviewMap));
    UT_ASSERT_MSG(before != NULL, "malloc failed");
    memcpy(before, overviewSnapshotMap(f.snap), sizeof(OverviewMap));

    /* Straight into the live memory: the tile under the tank, a corner
     * square, and the live set itself. */
    BYTE farX = 0, farY = 0;
    snapFarCorner(&f, &farX, &farY);
    live->tile[farX][farY]         = (BYTE)(live->tile[farX][farY] + 1);
    live->tile[f.tankMX][f.tankMY] = (BYTE)(live->tile[f.tankMX][f.tankMY] + 1);
    live->flags[f.tankMX][f.tankMY] =
        (BYTE)(live->flags[f.tankMX][f.tankMY] & ~OVERVIEW_F_LIVE);
    live->liveCount = 0;
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), live, sizeof(OverviewMap)) != 0,
                  "the live memory did not change — this case would prove "
                  "nothing");
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), before, sizeof(OverviewMap)) == 0,
                  "writing to the live memory changed the snapshot");

    /* Through the sim: a terrain change under a live square, and the tick
     * that stamps it. */
    snapMoveLiveSquare(&f, flipX, flipY);
    UT_ASSERT_MSG(live->tile[flipX][flipY] != before->tile[flipX][flipY],
                  "the terrain flip did not move the live tile at %u,%u",
                  (unsigned)flipX, (unsigned)flipY);
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), before, sizeof(OverviewMap)) == 0,
                  "a display tick after the fill changed the snapshot");

    /* The next fill picks all of it up. */
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), live, sizeof(OverviewMap)) == 0,
                  "the snapshot's map differs from the live memory after the "
                  "fill that followed the changes");

    free(before);
    snapFixtureStop(&f);
    return 0;
}

int run_overview_snapshot_filter(void) {
    SnapFixture f;
    const char *err = snapFixtureStart(&f, "Filter");
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
     * still inside the 55x55 box the server culls other tanks at. Grass under
     * both, so no tree rule hides anything for a reason of its own. */
    int dir = snapAwayFromEdge(f.tankMX);
    BYTE nearX = (BYTE)((int)f.tankMX + dir * 5);
    BYTE farX  = (BYTE)((int)f.tankMX + dir * 20);
    BYTE atY   = f.tankMY;
    mapSetPos(f.gs, &f.gs->mp, nearX, atY, GRASS, TRUE, TRUE);
    mapSetPos(f.gs, &f.gs->mp, farX, atY, GRASS, TRUE, TRUE);

    player *bogey = snapPlaceHostile(&f, hostile, nearX, atY);
    clientSimDisplayTick(f.cs, false);

    /* One explosion inside the block and one outside, added after the tick
     * so both are on the list the fill reads. */
    explosionsAddItem(&f.gs->expl, nearX, atY, 8, 8, EXPLOSION_START);
    explosionsAddItem(&f.gs->expl, farX, atY, 8, 8, EXPLOSION_START);

    /* Everything inside the block: all of it drawn, in the builders' order. */
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);
    {
        BYTE gotX = 0, gotY = 0;
        UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), hostile, &gotX, &gotY) == TRUE,
                      "the hostile tank inside the block is not on the "
                      "snapshot's list");
        UT_ASSERT_MSG(gotX == nearX && gotY == atY,
                      "the hostile tank is listed on %u,%u, expected %u,%u",
                      (unsigned)gotX, (unsigned)gotY, (unsigned)nearX,
                      (unsigned)atY);
        UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), f.me, NULL, NULL) == TRUE,
                      "the local tank is not on the snapshot's list");
    }
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == TRUE,
                  "the local tank did not survive the filter on its own square");
    UT_ASSERT_MSG(snapLgmOn(overviewSnapshotLgms(f.snap), nearX, atY) == TRUE,
                  "the hostile LGM inside the block is not on the snapshot's "
                  "list");
    UT_ASSERT_MSG(screenBulletsGetNumEntries(overviewSnapshotBullets(f.snap)) == 1,
                  "%d bullet entries in the snapshot, expected the one "
                  "explosion inside the block",
                  screenBulletsGetNumEntries(overviewSnapshotBullets(f.snap)));
    {
        BYTE mx = 0, my = 0, px = 0, py = 0, frame = 0;
        screenBulletsGetItem(overviewSnapshotBullets(f.snap), 1, &mx, &my, &px,
                             &py, &frame);
        UT_ASSERT_MSG(mx == nearX && my == atY,
                      "the kept explosion is on %u,%u, expected %u,%u",
                      (unsigned)mx, (unsigned)my, (unsigned)nearX,
                      (unsigned)atY);
    }

    /* Twenty squares off: still in the whole-map list — the client has lost
     * none of what it knew — and the filter is what takes it off. */
    bogey->mapX = farX;
    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);
    UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), hostile, NULL, NULL) == FALSE,
                  "the hostile tank outside the block is on the snapshot's "
                  "list");
    UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), f.me, NULL, NULL) == TRUE,
                  "the local tank dropped off the list when the enemy moved");

    /* The player's own tank, dead and waiting to respawn: it reads as sitting
     * on the map origin, so it goes before its square is tested, and the
     * reticle goes with it. */
    tankSetDestroyed(&f.gs->tanks[f.me], TRUE);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);
    UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), f.me, NULL, NULL) == FALSE,
                  "the dead local tank is on the snapshot's list");
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == FALSE,
                  "selfDrawn is set for a dead tank");

    snapFixtureStop(&f);
    return 0;
}

int run_overview_snapshot_generation(void) {
    SnapFixture f;
    const char *err = snapFixtureStart(&f, "Generation");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    f.gs->pb->numPills = 0;

    BYTE flipX = 0, flipY = 0;
    UT_ASSERT_MSG(snapFindTerrainSquare(&f, &flipX, &flipY) == TRUE,
                  "no square beside the tank moves its tile on a terrain flip");

    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);

    OverviewMap *live = &f.cs->overview;
    unsigned gen = live->generation;

    /* Nothing moved: the same map again. */
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(live->generation == gen,
                  "a fill moved the generation counter");
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), live, sizeof(OverviewMap)) == 0,
                  "a second fill on an unchanged generation left a map that "
                  "differs from the live memory");

    /* A write the counter does not see — a corner square, outside every live
     * region, so no tick rewrites it — is not copied: the fill trusts the
     * counter and leaves the copy it has. */
    BYTE farX = 0, farY = 0;
    snapFarCorner(&f, &farX, &farY);
    UT_ASSERT_MSG((live->flags[farX][farY] & OVERVIEW_F_LIVE) == 0,
                  "corner square %u,%u is live", (unsigned)farX,
                  (unsigned)farY);
    BYTE was = live->tile[farX][farY];
    live->tile[farX][farY] = (BYTE)(was + 1);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(live->generation == gen,
                  "writing a tile moved the generation counter");
    UT_ASSERT_MSG(overviewSnapshotMap(f.snap)->tile[farX][farY] == was,
                  "a fill on an unchanged generation copied the memory anyway");

    /* The counter moves — a terrain change under a live square and the tick
     * that stamps it — and the next fill copies, corner and all. */
    snapMoveLiveSquare(&f, flipX, flipY);
    UT_ASSERT_MSG(live->generation != gen,
                  "the stamped terrain change did not move the generation "
                  "counter");
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    UT_ASSERT_MSG(memcmp(overviewSnapshotMap(f.snap), live, sizeof(OverviewMap)) == 0,
                  "the snapshot's map differs from the live memory after the "
                  "generation moved");
    UT_ASSERT_MSG(overviewSnapshotMap(f.snap)->tile[farX][farY] == (BYTE)(was + 1),
                  "the copy that followed the generation change left the "
                  "corner square behind");

    snapFixtureStop(&f);
    return 0;
}

/* The label list names every pill and base on the map at its square. A pill
 * or base a removal has taken off the map is not on it, whatever its slot
 * still holds: the draw-time tile test would drop it anyway, but the number
 * a square lookup gives for a removed item is nothing a label can draw. */
int run_overview_snapshot_removed_item_has_no_label(void) {
    SnapFixture f;
    const char *err = snapFixtureStart(&f, "Labels");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    UT_ASSERT_MSG(pillsGetNumPills(&f.gs->pb) >= 1 && basesGetNumBases(&f.gs->bs) >= 1,
                  "Everard Island should carry a pillbox and a base");
    BYTE px = f.gs->pb->item[0].x;
    BYTE py = f.gs->pb->item[0].y;
    BYTE bx = f.gs->bs->item[0].x;
    BYTE by = f.gs->bs->item[0].y;

    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    int before = overviewSnapshotItemLabelCount(f.snap);
    UT_ASSERT_MSG(before == pillsGetNumPills(&f.gs->pb) + basesGetNumBases(&f.gs->bs),
                  "%d labels for %u pillboxes and %u bases", before,
                  (unsigned)pillsGetNumPills(&f.gs->pb),
                  (unsigned)basesGetNumBases(&f.gs->bs));

    UT_ASSERT(pillsRemoveItem(&f.gs->pb, 1) == TRUE);
    UT_ASSERT(basesRemoveItem(&f.gs->bs, 1) == TRUE);
    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);
    int after = overviewSnapshotItemLabelCount(f.snap);
    UT_ASSERT_MSG(after == before - 2,
                  "%d labels after removing a pillbox and a base, wanted %d",
                  after, before - 2);
    const OverviewItemLabel *labels = overviewSnapshotItemLabels(f.snap);
    int i;
    for (i = 0; i < after; i++) {
        UT_ASSERT_MSG(!(labels[i].isBase == false && labels[i].mapX == px && labels[i].mapY == py),
                      "the removed pillbox still has a label at %u,%u",
                      (unsigned)px, (unsigned)py);
        UT_ASSERT_MSG(!(labels[i].isBase == true && labels[i].mapX == bx && labels[i].mapY == by),
                      "the removed base still has a label at %u,%u",
                      (unsigned)bx, (unsigned)by);
    }

    snapFixtureStop(&f);
    return 0;
}

/* The player's own tank nosed into an inside corner of buildings, with line
 * of sight on. The tank sits in the top left quarter of its square, so the
 * square its sprite starts in is the corner block diagonally before it, which
 * the sight mask rightly calls hidden: a square behind two walls. The square
 * the tank is standing on is live, and that is the square the filter has to
 * ask about. Before the fix the tank, and the reticle with it, vanished from
 * the full screen map here. The corner block being hidden is asserted too, so
 * the case is known to be the real one and not a corner the mask can see. */
int run_overview_snapshot_corner_keeps_self(void) {
    SnapFixture f;
    const char *err = snapFixtureStart(&f, "Corner");
    UT_ASSERT_MSG(err == NULL, "%s", err);

    /* No pills, so nothing but the buildings put down here blocks sight. */
    f.gs->pb->numPills = 0;
    f.cs->lineOfSight = (uint8_t)lineOfSightBuildingsAndTrees;

    UT_ASSERT_MSG(f.tankMX >= 2 && f.tankMY >= 2,
                  "the tank landed at %u,%u, too near the map edge for a "
                  "corner to its north west", (unsigned)f.tankMX,
                  (unsigned)f.tankMY);
    BYTE wallX = (BYTE)(f.tankMX - 1); /* The column to the west */
    BYTE wallY = (BYTE)(f.tankMY - 1); /* The row to the north */
    mapSetPos(f.gs, &f.gs->mp, wallX, f.tankMY, BUILDING, TRUE, TRUE);
    mapSetPos(f.gs, &f.gs->mp, f.tankMX, wallY, BUILDING, TRUE, TRUE);
    mapSetPos(f.gs, &f.gs->mp, wallX, wallY, BUILDING, TRUE, TRUE);
    mapSetPos(f.gs, &f.gs->mp, f.tankMX, f.tankMY, GRASS, TRUE, TRUE);

    /* Into the top left quarter of its square, facing north west, as a tank
     * driven into the corner ends up. 100 world units in is short of the 128
     * that puts the sprite's top left on the tank's own square. */
    tankSetLocationData(&f.gs->tanks[f.me],
                        (WORLD)(((WORLD)f.tankMX << TANK_SHIFT_MAPSIZE) + 100),
                        (WORLD)(((WORLD)f.tankMY << TANK_SHIFT_MAPSIZE) + 100),
                        (TURNTYPE)32, 0, FALSE);
    {
        BYTE mx = 0, my = 0;
        UT_ASSERT_MSG(clientSimGetMyTankMapPos(f.cs, &mx, &my) == TRUE &&
                          mx == f.tankMX && my == f.tankMY,
                      "moving the tank inside its square changed its square to "
                      "%u,%u", (unsigned)mx, (unsigned)my);
    }

    clientSimDisplayTick(f.cs, false);
    clientSimFillOverviewSnapshot(f.cs, f.snap);

    const OverviewMap *om = overviewSnapshotMap(f.snap);
    UT_ASSERT_MSG(om != NULL, "the snapshot has no map");
    UT_ASSERT_MSG(om->hiddenActive == TRUE,
                  "line of sight is on and the tank is live, yet the map is "
                  "not hiding anything");
    UT_ASSERT_MSG((om->flags[wallX][wallY] & OVERVIEW_F_HIDDEN) != 0,
                  "the corner block at %u,%u is not hidden (flags %02x), so "
                  "this is not the case being pinned", (unsigned)wallX,
                  (unsigned)wallY, (unsigned)om->flags[wallX][wallY]);
    UT_ASSERT_MSG((om->flags[f.tankMX][f.tankMY] & OVERVIEW_F_LIVE) != 0,
                  "the tank's own square is not live");

    /* The whole-map list carries the tank on the corner block, which is the
     * square that used to be tested. */
    {
        screenTanks   allTks;
        screenLgm     allLgms;
        screenBullets allSb;
        BYTE listX = 0, listY = 0, cx = 0, cy = 0;

        screenTanksCreate(&allTks);
        screenLgmCreate(&allLgms);
        allSb = screenBulletsCreate();
        clientSimPrepareOverviewEntities(f.cs, &allTks, &allLgms, &allSb);
        UT_ASSERT_MSG(snapFindTank(&allTks, f.me, &listX, &listY) == TRUE,
                      "the whole-map list has no local tank");
        UT_ASSERT_MSG(listX == wallX && listY == wallY,
                      "the local tank is listed on %u,%u, expected the corner "
                      "block %u,%u", (unsigned)listX, (unsigned)listY,
                      (unsigned)wallX, (unsigned)wallY);
        screenTanksGetCentreSquare(&allTks, 1, &cx, &cy);
        UT_ASSERT_MSG(cx == f.tankMX && cy == f.tankMY,
                      "the centre square reads %u,%u, expected %u,%u",
                      (unsigned)cx, (unsigned)cy, (unsigned)f.tankMX,
                      (unsigned)f.tankMY);
        screenBulletsDestroy(&allSb);
        screenLgmDestroy(&allLgms);
        screenTanksDestroy(&allTks);
    }

    UT_ASSERT_MSG(snapFindTank(overviewSnapshotTanks(f.snap), f.me, NULL, NULL) == TRUE,
                  "the local tank dropped off the full screen map in an "
                  "inside corner");
    UT_ASSERT_MSG(overviewSnapshotSelfDrawn(f.snap) == TRUE,
                  "selfDrawn is false with the tank in an inside corner");
    err = snapListsMatch(&f);
    UT_ASSERT_MSG(err == NULL, "%s", err);

    snapFixtureStop(&f);
    return 0;
}
