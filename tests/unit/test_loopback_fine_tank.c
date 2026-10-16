/*
 * Other tanks' full positions reach the Smooth draw.
 *
 * The snapshot carries every tank's world position in full (16-bit world
 * units, 16 to a game pixel). The client's render interpolation used to cut
 * that down to a game pixel, after the diagonal snap, when it stored it in
 * the players list, so in the Smooth animation mode the own tank glided and
 * every other tank stepped a whole game pixel at a time.
 *
 * The players list now keeps the full position beside the pixel one, and the
 * screen tank list uses it when the front end turns on
 * clientSimSetFineTankPositions (it does for Smooth only).
 *
 * This drives a real ClientSim over a real localhost socket against a real
 * server, with a bot tank held still on a NE facing at a spot off the pixel
 * grid and off the diagonal lattice, and reads the client's own draw list:
 *
 *   - the full position arrives on the client unchanged;
 *   - with the flag off the entry is exactly what it was before (the snapped
 *     game pixel, world offsets a whole pixel), so Classic and Match
 *     pixelation draw what they drew;
 *   - with the flag on the entry's square and world offset are the full
 *     position, and its pixel is that offset's pixel.
 *
 * A second case uses the same held bot for the client's shell cull: a shell
 * is taken off the screen where it meets the tank as drawn, not the newest
 * sample, and never against the tank that fired it.
 *
 * run_loopback_fine_tank_position
 * run_loopback_shell_drawn_cull
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"   /* cs->sim.plyrs — the players list */
#include "client_net.h"
#include "client_connect_state.h"
#include "client_enums.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType */
#include "server_sim_scenario.h"   /* AddUnfieldedSeat */
#include "game_sim.h"
#include "bolo_map.h"
#include "input_packet.h"
#include "players.h"
#include "screentank.h"
#include "tank.h"
#include "tank_diagonal_snap.h"
#include "util.h"
#include "test_harness.h"
#include "loopback_harness.h"

#define LF_CONNECT_MAX 2000
#define LF_SETTLE      400
#define LF_SEAT        3
#define LF_TEAM        2
/* Off the square middle by amounts that are neither whole pixels nor on
   the NE lattice, so the snap and the pixel cut both move the point. */
#define LF_OFF_X       37
#define LF_OFF_Y       (-21)

static bool lfConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           !clientSimIsInLobby(h->cs);
}

static uint32_t lfFeed(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Hold the bot's tank still at (wx, wy), facing NE, on open grass. */
static void lfHold(ServerSim *sim, WORLD wx, WORLD wy) {
    tank *t = &sim->sim.tanks[LF_SEAT];
    tankSetWorld(&sim->sim, t, wx, wy, (TURNTYPE)BRADIANS_NEAST, FALSE);
    /* Clear the speed, the coast and the slowdown every tick, so the bot
       brain cannot drive the tank off the held spot. */
    tankSetSpeed(t, 0);
    tankSetOnBoat(t, FALSE);
    (*t)->residualSpeed = 0;
    (*t)->autoSlowdown = FALSE;
}

/* The seat's entry in the client's draw list, built over the whole map the
   way a frame builds it. False when the seat is not in the list. */
static bool lfEntry(ClientSim *cs, BYTE *mx, BYTE *my, BYTE *px, BYTE *py,
                    BYTE *wx, BYTE *wy) {
    screenTanks list;
    BYTE        n, i;
    bool        found = false;

    screenTanksCreate(&list);
    screenTanksPrepare(cs, &list,
                       &clientSimGetGameSim(cs)->tanks[
                           clientSimGetMyPlayerNum(cs)],
                       0, MAP_ARRAY_SIZE - 1, 0, MAP_ARRAY_SIZE - 1);
    n = screenTanksGetNumEntries(&list);
    for (i = 0; i < n; i++) {
        BYTE frame, pn, angle = 0;
        char name[FILENAME_MAX];
        screenTanksGetItem(&list, (BYTE)(i + 1), mx, my, px, py, &frame,
                           &pn, name);
        if (pn == LF_SEAT) {
            screenTanksGetSubPixel(&list, (BYTE)(i + 1), wx, wy, &angle);
            found = true;
            break;
        }
    }
    screenTanksDestroy(&list);
    return found;
}

/* Start the harness, field the bot seat two squares right of the viewer on
   open grass, and hold it still until the client's players row has its full
   position. On failure the harness is stopped and *err says why. */
static bool lfFieldHeldBot(LoopbackHarness *h, uint32_t *tick, WORLD *outX,
                           WORLD *outY, char *err, size_t errLen) {
    int             i, dx, dy;
    WORLD           vx = 0, vy = 0;
    BYTE            smx, smy;
    WORLD           holdX, holdY;
    bool            arrived = false;

    memset(h, 0, sizeof(*h));
    if (!loopbackHarnessStart(h, "Viewer", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, /*seed*/ 0xF1E5u)) {
        snprintf(err, errLen, "harness start failed");
        return false;
    }

    if (loopbackHarnessPumpUntil(h, LF_CONNECT_MAX, lfConnected, NULL) < 0) {
        loopbackHarnessStop(h);
        snprintf(err, errLen, "the client never reached a running game");
        return false;
    }

    ut_brain_stub_arm(true);
    serverSimSetBotAiType(h->sim, aiFull);
    if (!serverSimAddUnfieldedSeat(h->sim, LF_SEAT, "Glider", LF_TEAM) ||
        !serverSimAddBot(h->sim, LF_SEAT, &(ServerSimBotConfig){
                             .brainPath   = "brains/GoalHunter/init.lua",
                             .brainName   = "Glider",
                             .ai          = aiFull,
                             .gameType    = gameOpen,
                             .hiddenMines = false,
                             .teamNumber  = LF_TEAM }) ||
        h->sim->sim.tanks[LF_SEAT] == NULL) {
        loopbackHarnessStop(h);
        snprintf(err, errLen, "the bot seat could not be fielded");
        return false;
    }

    /* Two squares right of the viewer, well inside its view, with open
       grass round both tanks so nothing hides the bot. */
    tankGetWorld(&h->sim->sim.tanks[clientSimGetMyPlayerNum(h->cs)], &vx, &vy);
    smx = (BYTE)((vx >> TANK_SHIFT_MAPSIZE) + 2);
    smy = (BYTE)(vy >> TANK_SHIFT_MAPSIZE);
    for (dy = -3; dy <= 3; dy++) {
        for (dx = -5; dx <= 3; dx++) {
            mapSetPos(&h->sim->sim, &h->sim->sim.mp, (BYTE)(smx + dx),
                      (BYTE)(smy + dy), GRASS, FALSE, FALSE);
        }
    }
    holdX = (WORLD)((smx << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE + LF_OFF_X);
    holdY = (WORLD)((smy << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE + LF_OFF_Y);

    for (i = 0; i < LF_SETTLE && !arrived; i++) {
        lfHold(h->sim, holdX, holdY);
        *tick = lfFeed(h, *tick);
        loopbackHarnessPump(h);
        /* The per-frame seam a human client runs before each draw: it is
           what moves other tanks into the players list. */
        clientSimRenderPrepare(h->cs, (uint32_t)SDL_GetTicks());
        {
            player *row = &h->cs->sim.plyrs->item[LF_SEAT];
            arrived = row->fineSet && row->fineX == holdX &&
                      row->fineY == holdY;
        }
    }
    if (!arrived) {
        /* Read the row before the stop frees it. */
        const player *row = &h->cs->sim.plyrs->item[LF_SEAT];
        int fineSet = (int)row->fineSet;
        int fineX = (int)row->fineX, fineY = (int)row->fineY;
        loopbackHarnessStop(h);
        snprintf(err, errLen,
                 "the client never held the bot's full position (%d,%d); "
                 "players row has fineSet %d at (%d,%d)",
                 (int)holdX, (int)holdY, fineSet, fineX, fineY);
        return false;
    }
    *outX = holdX;
    *outY = holdY;
    return true;
}

int run_loopback_fine_tank_position(void) {
    LoopbackHarness h;
    uint32_t        tick = 1;
    WORLD           holdX = 0, holdY = 0;
    int             snapX, snapY;
    BYTE            mx = 0, my = 0, px = 0, py = 0, wx = 0, wy = 0;
    char            err[256];

    if (!lfFieldHeldBot(&h, &tick, &holdX, &holdY, err, sizeof(err))) {
        UT_FAIL("%s", err);
    }

    /* What the pixel path stores: the NE snap, then the game pixel. */
    snapX = (int)holdX;
    snapY = (int)holdY;
    tankDiagonalSnap(utilGetDir((TURNTYPE)BRADIANS_NEAST), &snapX, &snapY);
    if (snapX == (int)holdX && snapY == (int)holdY) {
        loopbackHarnessStop(&h);
        UT_FAIL("the held spot (%d,%d) is already on the NE lattice, so "
                "this case cannot tell the two paths apart",
                (int)holdX, (int)holdY);
    }

    /* Flag off: the snapped game pixel, exactly the old entry. */
    clientSimSetFineTankPositions(h.cs, false);
    UT_ASSERT_MSG(lfEntry(h.cs, &mx, &my, &px, &py, &wx, &wy),
                  "the bot's tank is not in the draw list (flag off)");
    {
        int gotX = mx * 256 + wx + TANK_SUBTRACT;
        int gotY = my * 256 + wy + TANK_SUBTRACT;
        UT_ASSERT_MSG(gotX == snapX && gotY == snapY,
                      "flag off: entry at world (%d,%d), expected the snapped "
                      "pixel (%d,%d)", gotX, gotY, snapX, snapY);
        UT_ASSERT_MSG(wx == (BYTE)(px << 4) && wy == (BYTE)(py << 4),
                      "flag off: world offsets (%d,%d) are not the whole "
                      "pixel (%d,%d)", wx, wy, px << 4, py << 4);
    }

    /* Flag on: the full position, square, pixel and offset agreeing. */
    clientSimSetFineTankPositions(h.cs, true);
    UT_ASSERT_MSG(lfEntry(h.cs, &mx, &my, &px, &py, &wx, &wy),
                  "the bot's tank is not in the draw list (flag on)");
    {
        int gotX = mx * 256 + wx + TANK_SUBTRACT;
        int gotY = my * 256 + wy + TANK_SUBTRACT;
        UT_ASSERT_MSG(gotX == (int)holdX && gotY == (int)holdY,
                      "flag on: entry at world (%d,%d), expected the full "
                      "position (%d,%d)", gotX, gotY, (int)holdX, (int)holdY);
        UT_ASSERT_MSG(px == (wx >> 4) && py == (wy >> 4),
                      "flag on: pixel (%d,%d) does not match world offsets "
                      "(%d,%d)", px, py, wx, wy);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Advance one projected shell from (x - step, y) by step to (x, y) and say
   whether the client's cull kept it. */
static bool lfShellSurvives(ClientSim *cs, WORLD x, WORLD y, uint8_t owner) {
    const float step = 16.0f;
    ProjectedShell *ps = &cs->projectedShells[0];

    memset(ps, 0, sizeof(*ps));
    ps->fx     = (float)x - step;
    ps->fy     = (float)y;
    ps->vx     = step;
    ps->vy     = 0.0f;
    ps->owner  = owner;
    ps->length = 40;
    ps->active = true;
    cs->projectedShellCount = 1;
    clientSimAdvanceProjectedShells(cs);
    return cs->projectedShellCount == 1;
}

/* The client's shell cull meets the tank where it is drawn. The bot is held
   still, so the newest interpolation sample sits on the held spot; the drawn
   position is then put two hit radii to the right of it, the way the display
   delay leaves the drawn hull apart from the newest sample. A shell on the
   newest sample but clear of the drawn hull stays on the screen; one on the
   drawn hull is culled; and a shell the bot fired is never culled against the
   bot. */
int run_loopback_shell_drawn_cull(void) {
    LoopbackHarness h;
    uint32_t        tick = 1;
    WORLD           holdX = 0, holdY = 0;
    WORLD           nx = 0, ny = 0;     /* The newest sample */
    WORLD           drawnX;
    TURNTYPE        angle = 0;
    bool            onBoat = false;
    int             hitR;
    uint8_t         other;              /* A shooter that is neither tank */
    player         *row;
    char            err[256];

    if (!lfFieldHeldBot(&h, &tick, &holdX, &holdY, err, sizeof(err))) {
        UT_FAIL("%s", err);
    }
    if (h.cs->isBot ||
        !interpIsAlive(&h.cs->interpCtx, LF_SEAT) ||
        !interpGetPosition(&h.cs->interpCtx, LF_SEAT, 1.0f, &nx, &ny, &angle,
                           &onBoat)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client holds no live sample for the bot");
    }
    if (nx != holdX || ny != holdY) {
        int gx = (int)nx, gy = (int)ny;
        loopbackHarnessStop(&h);
        UT_FAIL("the newest sample (%d,%d) is not the held spot (%d,%d)", gx,
                gy, (int)holdX, (int)holdY);
    }

    hitR   = (int)h.cs->sim.rules.tank_hit_radius;
    drawnX = (WORLD)((int)nx + 2 * hitR + 16);
    other  = (uint8_t)((LF_SEAT + 1) % MAX_TANKS);
    if (other == clientSimGetMyPlayerNum(h.cs)) {
        other = (uint8_t)((other + 1) % MAX_TANKS);
    }

    /* The drawn position, the way the Smooth draw reads it. Nothing pumps
       between here and the checks, so nothing writes the row over. */
    clientSimSetFineTankPositions(h.cs, true);
    row = &h.cs->sim.plyrs->item[LF_SEAT];
    row->fineX   = drawnX;
    row->fineY   = ny;
    row->fineSet = true;

    if (!lfShellSurvives(h.cs, nx, ny, other)) {
        loopbackHarnessStop(&h);
        UT_FAIL("a shell on the newest sample, %d units short of the drawn "
                "hull, was culled", 2 * hitR + 16);
    }
    if (lfShellSurvives(h.cs, drawnX, ny, other)) {
        loopbackHarnessStop(&h);
        UT_FAIL("a shell on the drawn hull was not culled");
    }
    if (!lfShellSurvives(h.cs, drawnX, ny, (uint8_t)LF_SEAT)) {
        loopbackHarnessStop(&h);
        UT_FAIL("a shell was culled against the tank that fired it");
    }

    h.cs->projectedShellCount = 0;
    loopbackHarnessStop(&h);
    return 0;
}
