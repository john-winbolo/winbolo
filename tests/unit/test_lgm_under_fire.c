/*
 * The builder sent to wall the square behind a tank that is under pillbox
 * fire — online, with lag, and single player, without it.
 *
 * The move players describe: a tank drives at a hostile pillbox, shoots it a
 * few times so it heats up and fires fast, and then — while shells from the
 * pill are landing on the tank — sends the man out to build a wall on the
 * square directly BEHIND the tank, the far side from the pill. The tank body
 * stands between the man and every shell, so the man walks out, builds, and
 * walks back. That is what happens in single player.
 *
 * Online, with a normal amount of lag, the same move kills the man. This test
 * writes down the player's expectation — the man lives — and runs the move
 * twice over the real loopback transport:
 *
 *   lgm_wall_behind_tank_under_fire_lagged  — delay=80,jitter=20, which the
 *       server measures as a round trip of about 220ms.
 *   lgm_wall_behind_tank_under_fire_nolag   — the same script on a clean
 *       link, the single-player control.
 *
 * The lagged case is EXPECTED TO FAIL today: it is the bug report, written as
 * a test. The control is expected to pass; if it ever fails, lag is not the
 * cause and the two failures should be read together.
 *
 * What the script drives, and what it does not:
 *   - Every tank key, every shot and the build order itself go through the
 *     client input path (clientSimNetSendInput), never into the server sim.
 *     Odd input tick = keys half-step, even = game half-step, which is the
 *     split server_sim_tick.c keys on; see luf_send_frame for the rate.
 *   - The build square is worked out from what the CLIENT can see of its own
 *     tank (clientSimGetMyTankMapPos), because that is what a player aims
 *     with. Both runs print the client's square beside the server's, and in
 *     both of them the two agree — so the lagged man is not being sent
 *     somewhere else, he is being sent to the same square and killed on it.
 *   - Pumps are paced to one 20ms frame of wall clock each
 *     (loopbackHarnessSetPumpInterval), so the delay the impairment layer
 *     holds a datagram for and the delay the server reads off its own ping
 *     measurement are the same number of frames. A free-running pump loop
 *     makes them differ by about tenfold and the link stops being one any
 *     player has.
 *   - The world the move happens in IS built by the test: a carved grass
 *     arena, one neutral pillbox, the tank placed and stocked. That is
 *     fixture, not gameplay, and all of it happens before or outside the
 *     scripted move. The tank's armour is topped up every pump so the run
 *     ends on the man's fate rather than on the tank's.
 *
 * Everything the run saw is printed: the dispatch tick, both views of the
 * tank, the pill's heat, the man's position every frame, and every shell
 * impact with its distance to the man. MAP_SQUARE_MIDDLE (128 world units) is
 * the radius lgmDeathCheckAtPosition kills inside on open ground. What the
 * two printouts differ in is where the pill's shells stop: on the clean link
 * they explode about 170 world units east of the tank's centre, clear of the
 * man walking away west, and under lag they explode about 80 — inside the
 * tank, on the man's side of it, a rewind's worth deeper. shells.c tests a
 * pill shell against each tank at the position it held perPlayerCompTicks
 * frames ago (server_sim_tick.c fills that in from the player's ping), and a
 * tank driving east was further west then. The same rewind is applied to the
 * man himself in lgmDeathCheckAtPosition.
 */

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* ServerSim.sim / tick / playerPing */
#include "game_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "input_packet.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "tank.h"
#include "lgm.h"
#include "shells.h"
#include "threads.h"
#include "log.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Where the .wbv replay of each run is written. CMake points this at
 * <build dir>/replays; the fallback resolves against the working directory,
 * which for CTest and for a hand-run binary is the build directory anyway. */
#ifndef WB_TEST_REPLAY_DIR
#define WB_TEST_REPLAY_DIR "replays"
#endif

/* ── arena ──────────────────────────────────────────────────────────────── */

#define LUF_ARENA_W      30   /* carved grass, in map squares               */
#define LUF_ARENA_H      11
#define LUF_PILL_INSET    5   /* pill this far in from the arena's east end */
#define LUF_APPROACH      8   /* tank starts this many squares west of it   */

/* Pumps. One pump is one 20ms server frame, so these are frame budgets. */
#define LUF_CONNECT_MAX 4000  /* join + map download, lagged                */
#define LUF_SETTLE      100   /* tank parked: ping measured, client synced  */
#define LUF_APPROACH_MAX 300  /* drive in and heat the pill                 */
#define LUF_SOAK         60   /* take heated fire before sending the man    */
#define LUF_HEAT_HITS     5   /* hits that take the pill to its fastest rate */
#define LUF_DISPATCH_GAP  3   /* squares tank-to-pill when the man is sent   */
#define LUF_WATCH_MAX   130   /* the man's whole trip, with slack           */

#define LUF_PUMP_MS      20   /* one pump = one 20ms frame of wall clock    */
#define LUF_TRAIL_MAX   600   /* man-position samples kept for the printout */
#define LUF_IMPACT_MAX   64   /* shell impacts kept for the printout        */

typedef struct {
    BYTE pillX, pillY;        /* the pillbox square                        */
    BYTE tankX, tankY;        /* where the tank is parked to start         */
    bool ok;
} LufArena;

typedef struct {
    int   frame;              /* pumps since the build order was sent      */
    int   manX, manY;         /* world position                            */
    int   impactX, impactY;   /* world position of the shell that landed   */
    int   dist;               /* to the man, world units                   */
    BYTE  owner;
} LufImpact;

typedef struct {
    int  frame;
    int  manX, manY;
    BYTE state, waitTime;
    bool inTank, isDead;
} LufTrail;

/* ── arena construction (runs before the client joins) ───────────────────── */

/* A rectangle of the map with no base anywhere in it or in a one-square
 * border round it. Pills do not matter: the setup keeps exactly one and puts
 * it where it wants it. */
static bool luf_find_clear_rect(GameSim *gs, BYTE *ox, BYTE *oy) {
    int x, y, i, j;
    for (y = 24; y + LUF_ARENA_H + 2 < 224; y++) {
        for (x = 24; x + LUF_ARENA_W + 2 < 224; x++) {
            bool clear = true;
            for (j = -1; j <= LUF_ARENA_H && clear; j++) {
                for (i = -1; i <= LUF_ARENA_W && clear; i++) {
                    if (basesExistPos(&gs->bs, (BYTE)(x + i), (BYTE)(y + j))) {
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

static void luf_build_arena(LoopbackHarness *h, void *user) {
    LufArena *a = (LufArena *)user;
    GameSim  *gs = &h->sim->sim;
    BYTE x0 = 0, y0 = 0;
    int x, y;
    pillbox p;

    a->ok = false;
    if (!luf_find_clear_rect(gs, &x0, &y0)) {
        return;
    }

    /* Open grass, so nothing hides the tank from the pill and the man walks
     * at full speed the whole way. */
    for (y = 0; y < LUF_ARENA_H; y++) {
        for (x = 0; x < LUF_ARENA_W; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)(x0 + x), (BYTE)(y0 + y), GRASS,
                      FALSE, TRUE);
        }
    }

    a->pillY = (BYTE)(y0 + LUF_ARENA_H / 2);
    a->pillX = (BYTE)(x0 + LUF_ARENA_W - 1 - LUF_PILL_INSET);
    a->tankY = a->pillY;
    a->tankX = (BYTE)(a->pillX - LUF_APPROACH);

    /* One pillbox in the world, neutral (so it is hostile to everyone), at
     * full armour and the classic unangered fire rate. Dropping numPills to
     * one takes every other pill on Everard Island out of the round, so
     * nothing else on the map can shoot at this tank. */
    memset(&p, 0, sizeof(p));
    p.x        = a->pillX;
    p.y        = a->pillY;
    p.owner    = NEUTRAL;
    p.armour   = (BYTE)gs->rules.pill_max_armour;
    p.speed    = (BYTE)gs->rules.pill_attack_ticks;
    p.inTank   = FALSE;
    p.reload   = 0;
    p.coolDown = 0;
    p.justSeen = FALSE;
    gs->pb->item[0]   = p;
    gs->pb->active[0] = 1;
    gs->pb->numPills  = 1;

    a->ok = true;
}

/* ── small readers ──────────────────────────────────────────────────────── */

static WORLD luf_world(BYTE mapPos) {
    return (WORLD)(((int)mapPos << TANK_SHIFT_MAPSIZE) + MAP_SQUARE_MIDDLE);
}

static int luf_dist(int ax, int ay, int bx, int by) {
    double dx = (double)(ax - bx);
    double dy = (double)(ay - by);
    return (int)(sqrt(dx * dx + dy * dy) + 0.5);
}

static bool luf_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* One pump's worth of input. A server frame is two half-steps and eats two
 * input ticks, odd = keys and even = game (server_sim_tick.c keys the split on
 * the input's own tick parity), so two per pump is the balanced rate — and a
 * balanced rate is exactly the one whose queue empties the moment a datagram
 * arrives a pump late. An empty queue puts the server back into jitter-
 * buffering, and a half-step spent buffering moves nothing. Sending three
 * keeps a standing tick or two in hand; the server's own backlog catch-up
 * bleeds the surplus back down to its jitter target, so the lead does not
 * grow. One-shots (fire, the build order) ride the last EVEN tick of the
 * batch, because the server only acts on them in the game arm. */
#define LUF_INPUTS_PER_PUMP 3

static void luf_send_frame(LoopbackHarness *h, BYTE slot, uint32_t *tick,
                           uint8_t buttons, uint8_t actions,
                           uint8_t buildAction, BYTE buildX, BYTE buildY) {
    uint32_t base = *tick;
    uint32_t gameTick = 0;
    int n;

    for (n = 1; n <= LUF_INPUTS_PER_PUMP; n++) {
        if (((base + (uint32_t)n) % 2u) == 0u) {
            gameTick = base + (uint32_t)n;
        }
    }
    for (n = 1; n <= LUF_INPUTS_PER_PUMP; n++) {
        InputPacket pkt;
        memset(&pkt, 0, sizeof(pkt));
        pkt.tick      = base + (uint32_t)n;
        pkt.playerNum = slot;
        pkt.buttons   = buttons;
        if (pkt.tick == gameTick) {
            pkt.actions     = actions;
            pkt.buildAction = buildAction;
            pkt.buildX      = buildX;
            pkt.buildY      = buildY;
        }
        clientSimNetSendInput(h->cs, &pkt);
    }
    *tick = base + (uint32_t)LUF_INPUTS_PER_PUMP;
}

/* Keep the tank fightable: full armour so a pill that is meant to be shooting
 * at it cannot end the run by killing it, and stock for shells and one wall. */
static void luf_restock(LoopbackHarness *h, BYTE slot) {
    GameSim *gs = &h->sim->sim;
    threadsWaitForMutex();
    if (gs->tanks[slot] != NULL) {
        tankSetArmour(&gs->tanks[slot], (BYTE)gs->rules.tank_full_armour);
        tankSetShells(gs, &gs->tanks[slot], (BYTE)gs->rules.tank_full_shells);
        tankSetTrees(gs, &gs->tanks[slot], (BYTE)gs->rules.tank_full_trees);
    }
    threadsReleaseMutex();
}

/* ── the run ────────────────────────────────────────────────────────────── */

typedef struct {
    const char *label;
    const char *impair;
    const char *replayLeaf;   /* file name under WB_TEST_REPLAY_DIR */
    uint64_t    seed;
} LufConfig;

/* ── recording the run as a replay ───────────────────────────────────────── */

/* Open a .wbv on the harness's own ServerSim. Called AFTER the client has
 * joined and the tank has been parked, because logStart writes the file's
 * header and its opening snapshot straight away: the map (the carved arena),
 * the pillbox, and one record per player slot. Started any earlier the header
 * would name an empty roster and the untouched island, and the viewer would
 * have nothing to put on screen until the first event caught it up.
 *
 * From here on every running-state server tick writes itself, because
 * serverSimTick already calls logWriteTick; the harness pump is what turns it.
 * Lobby mode is switched off explicitly — it is a process-wide flag and a
 * lobby log drops exactly the world events this replay is for. */
static void luf_record_start(LoopbackHarness *h, const LufConfig *cfg,
                             char *pathOut, size_t pathCap) {
    snprintf(pathOut, pathCap, "%s/%s", WB_TEST_REPLAY_DIR, cfg->replayLeaf);
    SDL_CreateDirectory(WB_TEST_REPLAY_DIR);   /* fine if it already exists */

    logSetLobbyMode(FALSE);
    if (logStart(pathOut, h->sim, 0, MAX_TANKS, FALSE) != TRUE) {
        fprintf(stderr, "  replay: COULD NOT open %s — the run goes on "
                        "unrecorded\n", pathOut);
        pathOut[0] = '\0';
        return;
    }
    fprintf(stderr, "  replay: recording to %s\n", pathOut);
}

/* Close the file: the terminator, the attribution track and the zip central
 * directory all go in here, so a replay that skips this is unreadable. Every
 * exit from luf_run runs it, the failing assertion paths included — that is
 * what LUF_STOP below is for. Safe to call when no log is open. */
static void luf_record_stop(const char *path) {
    logStop();
    if (path != NULL && path[0] != '\0') {
        fprintf(stderr, "  replay written: %s\n", path);
    }
}

/* Close the replay, then tear the harness down. Used on every path out of
 * luf_run, including the ones that fail. */
#define LUF_STOP()                                                          \
    do {                                                                    \
        luf_record_stop(replayPath);                                        \
        loopbackHarnessStop(&h);                                            \
    } while (0)

static int luf_run(const LufConfig *cfg) {
    LoopbackHarness h;
    LufArena  arena;
    LufTrail  trail[LUF_TRAIL_MAX];
    LufImpact impacts[LUF_IMPACT_MAX];
    int       trailCount = 0, impactCount = 0;
    char      replayPath[512];
    uint32_t  inTick = 0;
    BYTE      slot;
    GameSim  *gs;
    lgm      *man;
    int       i;

    /* Recorded at the moment the build order goes out. */
    uint32_t dispatchTick = 0;
    int      dispatchTankX = 0, dispatchTankY = 0;
    double   dispatchAngle = 0.0;
    BYTE     dispatchSrvMX = 0, dispatchSrvMY = 0;
    BYTE     dispatchCliMX = 0, dispatchCliMY = 0;
    BYTE     buildX = 0, buildY = 0;
    BYTE     pillSpeed = 0, pillArmour = 0;
    uint16_t pingMs = 0;
    uint8_t  compTicks = 0;
    bool     manDied = false, manReturned = false, wallBuilt = false;
    int      shotsFired = 0, pillHits = 0;
    BYTE     prevArmour = 0;

    memset(&arena, 0, sizeof(arena));
    memset(trail, 0, sizeof(trail));
    memset(impacts, 0, sizeof(impacts));
    replayPath[0] = '\0';

    fprintf(stderr, "\n  ==== %s (impair=%s seed=%llu) ====\n", cfg->label,
            (cfg->impair != NULL) ? cfg->impair : "clean",
            (unsigned long long)cfg->seed);

    UT_ASSERT_MSG(loopbackHarnessStartPrepared(&h, "Builder", /*lobbyMode*/ false,
                                               cfg->impair, cfg->seed,
                                               luf_build_arena, &arena),
                  "harness start failed (%s)", cfg->label);
    if (!arena.ok) {
        LUF_STOP();
        UT_FAIL("no base-free %dx%d rectangle on the map to build the arena in",
                LUF_ARENA_W, LUF_ARENA_H);
    }
    if (loopbackHarnessPumpUntil(&h, LUF_CONNECT_MAX, luf_connected, NULL) < 0) {
        LUF_STOP();
        UT_FAIL("client never connected (%s)", cfg->label);
    }

    /* From here on one pump costs one frame of wall clock, so the impairment
     * spec's milliseconds and the server's ping arithmetic mean the same
     * number of frames. The join above ran unpaced: it has no frame clock to
     * agree with, and pacing it would cost seconds for nothing. */
    loopbackHarnessSetPumpInterval(&h, LUF_PUMP_MS);

    slot = clientSimGetMyPlayerNum(h.cs);
    gs   = &h.sim->sim;
    if (slot >= MAX_TANKS || gs->tanks[slot] == NULL) {
        LUF_STOP();
        UT_FAIL("no tank for the joined slot %u", (unsigned)slot);
    }
    man = &gs->lgmen[slot];

    /* Park the tank at the west end of the arena facing the pill, and stock
     * it. Fixture: the round's own spawn is wherever Everard Island put it. */
    threadsWaitForMutex();
    tankSetWorld(gs, &gs->tanks[slot], luf_world(arena.tankX),
                 luf_world(arena.tankY), (TURNTYPE)BRADIANS_EAST, false);
    threadsReleaseMutex();
    luf_restock(&h, slot);

    fprintf(stderr, "  arena: pill at (%u,%u) tank starts at (%u,%u) slot %u\n",
            (unsigned)arena.pillX, (unsigned)arena.pillY,
            (unsigned)arena.tankX, (unsigned)arena.tankY, (unsigned)slot);

    /* Record from here: the client is in, the arena is carved and the tank is
     * standing where the script wants it, so the opening snapshot describes
     * the world the move actually happens in. */
    luf_record_start(&h, cfg, replayPath, sizeof(replayPath));

    /* Phase 0 — settle. Hands off the controls while the client syncs to the
     * moved tank and the server's ping measurement catches up with the link. */
    for (i = 0; i < LUF_SETTLE; i++) {
        luf_send_frame(&h, slot, &inTick, 0, 0, 0, 0, 0);
        loopbackHarnessPump(&h);
        luf_restock(&h, slot);
    }

    pingMs    = h.sim->playerPing[slot];
    compTicks = gs->perPlayerCompTicks[slot];
    fprintf(stderr, "  after settle: server ping=%ums  lag-comp rewind=%u "
            "frames (%d world units of man walk)\n",
            (unsigned)pingMs, (unsigned)compTicks,
            (int)compTicks * MAP_MANSPEED_TGRASS);

    /* Phase 1 - drive at the pill, shooting it until it is fully angry. The
     * pill halves its own shot interval on every hit it takes, down to
     * pill_attack_min_ticks, so LUF_HEAT_HITS hits take it from the classic
     * interval to the fastest one. Stop there: a fifth, tenth, fiftieth hit
     * only walks its armour down, and a destroyed pillbox stops being the
     * thing that is supposed to be shooting. */
    prevArmour = gs->pb->item[0].armour;
    for (i = 0; i < LUF_APPROACH_MAX; i++) {
        uint8_t actions = 0;
        int gap = (int)arena.pillX - (int)tankGetMX(&gs->tanks[slot]);

        if (pillHits < LUF_HEAT_HITS && gap <= 7 && gap >= 3) {
            actions = INPUT_ACTION_FIRE;
            shotsFired++;
        }
        luf_send_frame(&h, slot, &inTick, INPUT_BTN_ACCEL, actions, 0, 0, 0);
        loopbackHarnessPump(&h);
        luf_restock(&h, slot);

        if (gs->pb->item[0].armour < prevArmour) {
            pillHits += (int)(prevArmour - gs->pb->item[0].armour);
            prevArmour = gs->pb->item[0].armour;
        }
        if (tankIsDestroyed(&gs->tanks[slot])) {
            LUF_STOP();
            UT_FAIL("%s: the tank died on the approach - the script never got "
                    "to the move it is about", cfg->label);
        }
        gap = (int)arena.pillX - (int)tankGetMX(&gs->tanks[slot]);
        if (pillHits >= LUF_HEAT_HITS && gap <= LUF_DISPATCH_GAP + 2) {
            break;
        }
    }

    pillSpeed  = gs->pb->item[0].speed;
    pillArmour = gs->pb->item[0].armour;
    fprintf(stderr, "  after approach: pill speed=%u (normal %ld, min %ld) "
            "armour=%u after %d hits; tank at map (%u,%u); fire input asserted "
            "on %d frames\n",
            (unsigned)pillSpeed, (long)gs->rules.pill_attack_ticks,
            (long)gs->rules.pill_attack_min_ticks, (unsigned)pillArmour,
            pillHits, (unsigned)tankGetMX(&gs->tanks[slot]),
            (unsigned)tankGetMY(&gs->tanks[slot]), shotsFired);
    if (pillArmour == 0) {
        LUF_STOP();
        UT_FAIL("%s: the pillbox was destroyed on the approach - nothing is "
                "left to shoot at the man", cfg->label);
    }

    /* Phase 2 - soak. Keep the throttle down and take shells from the heated
     * pill, but stop closing before the tank is pressed against it: the move
     * players describe happens with the tank still rolling, and a tank stalled
     * against a pillbox is a tank whose position nothing can be wrong about. */
    for (i = 0; i < LUF_SOAK; i++) {
        int gap;
        luf_send_frame(&h, slot, &inTick, INPUT_BTN_ACCEL, 0, 0, 0, 0);
        loopbackHarnessPump(&h);
        luf_restock(&h, slot);
        if ((i % 20) == 0) {
            WORLD twx = 0, twy = 0;
            BYTE tmx;
            tankGetWorld(&gs->tanks[slot], &twx, &twy);
            tmx = tankGetMX(&gs->tanks[slot]);
            fprintf(stderr, "    soak %3d: tank world %u,%u speed=%u dead=%d "
                    "terrain ahead %u/%u/%u\n",
                    i, (unsigned)twx, (unsigned)twy,
                    (unsigned)tankGetSpeed(&gs->tanks[slot]),
                    (int)tankIsDestroyed(&gs->tanks[slot]),
                    (unsigned)mapGetPos(&gs->mp, (BYTE)(tmx + 1),
                                        tankGetMY(&gs->tanks[slot])),
                    (unsigned)mapGetPos(&gs->mp, (BYTE)(tmx + 2),
                                        tankGetMY(&gs->tanks[slot])),
                    (unsigned)mapGetPos(&gs->mp, (BYTE)(tmx + 3),
                                        tankGetMY(&gs->tanks[slot])));
        }
        gap = (int)arena.pillX - (int)tankGetMX(&gs->tanks[slot]);
        if (gap <= LUF_DISPATCH_GAP) {
            break;
        }
    }


    /* Phase 3 — send the man to wall the square behind the tank. "Behind" is
     * read off the client's own view of its tank, which is what the player
     * aims with: one square back along the line to the pill, i.e. west. */
    if (!clientSimGetMyTankMapPos(h.cs, &dispatchCliMX, &dispatchCliMY)) {
        LUF_STOP();
        UT_FAIL("the client cannot see its own tank at dispatch (%s)",
                cfg->label);
    }
    dispatchSrvMX = tankGetMX(&gs->tanks[slot]);
    dispatchSrvMY = tankGetMY(&gs->tanks[slot]);
    buildX = (BYTE)(dispatchCliMX - 1);   /* pill is due east → behind is west */
    buildY = dispatchCliMY;

    {
        WORLD twx = 0, twy = 0;
        tankGetWorld(&gs->tanks[slot], &twx, &twy);
        dispatchTankX = (int)twx;
        dispatchTankY = (int)twy;
    }
    dispatchAngle = (double)tankGetAngle(&gs->tanks[slot]);
    dispatchTick  = h.sim->tick;
    pillSpeed     = gs->pb->item[0].speed;
    pillArmour    = gs->pb->item[0].armour;
    pingMs        = h.sim->playerPing[slot];
    compTicks     = gs->perPlayerCompTicks[slot];

    shellsDebugHitLogClear();
    luf_send_frame(&h, slot, &inTick, INPUT_BTN_ACCEL, 0,
                   (uint8_t)(LGM_BUILDING_REQUEST + 1), buildX, buildY);
    loopbackHarnessPump(&h);
    luf_restock(&h, slot);

    /* Phase 4 — watch him. Every frame: where he is, and any shell that has
     * just landed, with its distance to him. */
    for (i = 1; i <= LUF_WATCH_MAX; i++) {
        int logCount;
        /* Where the man stood going INTO this frame. The shell that kills him
         * lands inside it, and death moves his record off the square he was
         * standing on, so the distance that decided his fate is the one to the
         * position he held when the shell arrived — not the one he holds after
         * it. A man still in the tank has no position of his own at all. */
        bool preLive = !(*man)->inTank && !(*man)->isDead;
        int  preManX = (int)(*man)->x;
        int  preManY = (int)(*man)->y;

        luf_send_frame(&h, slot, &inTick, INPUT_BTN_ACCEL, 0, 0, 0, 0);
        loopbackHarnessPump(&h);
        luf_restock(&h, slot);

        if (trailCount < LUF_TRAIL_MAX) {
            trail[trailCount].frame    = i;
            trail[trailCount].manX     = (int)(*man)->x;
            trail[trailCount].manY     = (int)(*man)->y;
            trail[trailCount].state    = (*man)->state;
            trail[trailCount].waitTime = (*man)->waitTime;
            trail[trailCount].inTank   = (*man)->inTank;
            trail[trailCount].isDead   = (*man)->isDead;
            trailCount++;
        }

        logCount = shellsDebugHitLogCount();
        while (impactCount < logCount && impactCount < LUF_IMPACT_MAX) {
            int wx = 0, wy = 0;
            uint32_t t = 0;
            uint8_t owner = 0;
            if (shellsDebugHitLogGet(impactCount, &wx, &wy, &t, &owner) <= 0) {
                break;
            }
            impacts[impactCount].frame   = i;
            impacts[impactCount].manX    = preManX;
            impacts[impactCount].manY    = preManY;
            impacts[impactCount].impactX = wx;
            impacts[impactCount].impactY = wy;
            impacts[impactCount].dist    = preLive
                ? luf_dist(wx, wy, preManX, preManY)
                : -1;
            impacts[impactCount].owner   = owner;
            impactCount++;
        }

        if ((*man)->isDead) {
            manDied = true;
            break;
        }
        if (mapGetPos(&gs->mp, buildX, buildY) == BUILDING) {
            wallBuilt = true;
        }
        if (wallBuilt && (*man)->inTank) {
            manReturned = true;
            break;
        }
    }

    /* ── the printout ───────────────────────────────────────────────────── */
    fprintf(stderr,
            "  dispatch at server tick %u\n"
            "    server tank: world (%d,%d) map (%u,%u) angle %.1f bradians\n"
            "    client tank: map (%u,%u)   %s\n"
            "    wall ordered on map (%u,%u)\n"
            "    pill: speed=%u armour=%u   ping=%ums  rewind=%u frames\n",
            (unsigned)dispatchTick, dispatchTankX, dispatchTankY,
            (unsigned)dispatchSrvMX, (unsigned)dispatchSrvMY, dispatchAngle,
            (unsigned)dispatchCliMX, (unsigned)dispatchCliMY,
            (dispatchCliMX == dispatchSrvMX && dispatchCliMY == dispatchSrvMY)
                ? "(agrees with the server)"
                : "(AHEAD OF / BEHIND the server's tank)",
            (unsigned)buildX, (unsigned)buildY,
            (unsigned)pillSpeed, (unsigned)pillArmour, (unsigned)pingMs,
            (unsigned)compTicks);

    fprintf(stderr, "    man, frame by frame (frame: world x,y state wait "
                    "inTank dead):\n");
    for (i = 0; i < trailCount; i++) {
        fprintf(stderr, "      %3d: %5d,%5d  st=%u wait=%2u %s%s\n",
                trail[i].frame, trail[i].manX, trail[i].manY,
                (unsigned)trail[i].state, (unsigned)trail[i].waitTime,
                trail[i].inTank ? "inTank " : "out    ",
                trail[i].isDead ? "DEAD" : "");
    }
    fprintf(stderr, "    shell impacts (frame: impact world x,y owner -> "
                    "distance to man; kill radius %d):\n",
            MAP_SQUARE_MIDDLE);
    for (i = 0; i < impactCount; i++) {
        if (impacts[i].dist < 0) {
            fprintf(stderr, "      %3d: %5d,%5d owner=%u -> man in the tank\n",
                    impacts[i].frame, impacts[i].impactX,
                    impacts[i].impactY, (unsigned)impacts[i].owner);
            continue;
        }
        fprintf(stderr, "      %3d: %5d,%5d owner=%u -> %4d (man at %5d,%5d) "
                        "%s\n",
                impacts[i].frame, impacts[i].impactX, impacts[i].impactY,
                (unsigned)impacts[i].owner, impacts[i].dist,
                impacts[i].manX, impacts[i].manY,
                (impacts[i].dist <= MAP_SQUARE_MIDDLE) ? "<= KILL RADIUS" : "");
    }
    fprintf(stderr, "    outcome: %s (wall built: %s)\n",
            manDied ? "THE MAN DIED" :
            (manReturned ? "the man built and came home" :
                           "still out when the watch ran out"),
            wallBuilt ? "yes" : "no");

    LUF_STOP();

    UT_ASSERT_MSG(!manDied,
                  "%s: the builder was killed walling the square behind his own "
                  "tank — the tank body was between him and the pillbox and the "
                  "player expects him to live", cfg->label);
    UT_ASSERT_MSG(wallBuilt || manReturned,
                  "%s: the builder neither died nor did the work — the script "
                  "did not reproduce the move, so the run proves nothing",
                  cfg->label);
    return 0;
}

int run_lgm_wall_behind_tank_under_fire_lagged(void) {
    LufConfig cfg;
    cfg.label      = "lagged";
    cfg.impair     = "delay=80,jitter=20";
    cfg.replayLeaf = "lgm_under_fire_lagged.wbv";
    cfg.seed       = 4242u;
    return luf_run(&cfg);
}

int run_lgm_wall_behind_tank_under_fire_nolag(void) {
    LufConfig cfg;
    cfg.label      = "no lag (control)";
    cfg.impair     = NULL;
    cfg.replayLeaf = "lgm_under_fire_nolag.wbv";
    cfg.seed       = 4242u;
    return luf_run(&cfg);
}
