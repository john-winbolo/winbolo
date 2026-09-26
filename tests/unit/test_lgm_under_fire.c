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
#include <time.h>

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
#include "starts.h"
#include "tank.h"
#include "lgm.h"
#include "shells.h"
#include "threads.h"
#include "log.h"
#include "brain_record.h"
#include "test_harness.h"
#include "loopback_harness.h"

/* Where the .wbv replay of each run is written. CMake points this at
 * <build dir>/replays; the fallback resolves against the working directory,
 * which for CTest and for a hand-run binary is the build directory anyway. */
#ifndef WB_TEST_REPLAY_DIR
#define WB_TEST_REPLAY_DIR "replays"
#endif

/* Where the brain-debug session of each run is written: one dir per run under
 * <build dir>/debug_sessions, which is the folder BrainTest's Load Session
 * browser globs. Same fallback reasoning as the replay dir above. */
#ifndef WB_TEST_DEBUG_SESSION_DIR
#define WB_TEST_DEBUG_SESSION_DIR "debug_sessions"
#endif

/* ── arena ──────────────────────────────────────────────────────────────── */

#define LUF_ARENA_W      30   /* carved grass, in map squares               */
#define LUF_ARENA_H      11
#define LUF_APPROACH      8   /* pill this many squares east of the tank    */

/* The Log Viewer's opening view. It parks a fixed 30x30 box of map squares
 * with its top-left corner at square 127 and never moves it: xOffset/yOffset
 * are set to 127 in lv_screenSetup (src/logviewer/screen.c) and only the
 * Options > Tank Centred item moves them, which is off by default
 * (imgui_main_menu.cpp). lv_playersMakeScreenTanks (src/logviewer/players.c)
 * then adds a tank to the draw list only when its square is inside that box.
 *
 * So a replay of a fight outside the box shows an island and no tank, which is
 * exactly what the first recording of this test did: its arena was in open sea
 * at square (45,29). The arena is therefore placed inside the box, with a
 * square to spare on every side of the tank's drive. */
#define LUF_VIEW_X0     127   /* the box's top-left map square              */
#define LUF_VIEW_SIZE    30   /* and its width and height, in squares       */
#define LUF_VIEW_MID    (LUF_VIEW_X0 + LUF_VIEW_SIZE / 2)

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
    BYTE startNum;            /* the map start it was built around, 1-based */
    bool overBases;           /* the strip had to be cut across a base      */
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

/* No base anywhere in the rectangle or in a one-square border round it. Pills
 * do not matter: the setup keeps exactly one and puts it where it wants it. */
static bool luf_rect_base_free(GameSim *gs, int x0, int y0) {
    int i, j;
    for (j = -1; j <= LUF_ARENA_H; j++) {
        for (i = -1; i <= LUF_ARENA_W; i++) {
            if (basesExistPos(&gs->bs, (BYTE)(x0 + i), (BYTE)(y0 + j))) {
                return false;
            }
        }
    }
    return true;
}

/* The arena's west end IS a start square, so the tank spawns where the move
 * begins and the parking below is a nudge rather than a jump across the sea.
 * The strip runs east from the start and is centred on its row, so the origin
 * is (startX, startY - height/2).
 *
 * An earlier version took the first base-free rectangle anywhere on the map.
 * On Everard Island that is open sea at square (45,29), miles from every
 * start: the tank had to be teleported there, and what the replay showed was
 * an island with a tank that appeared and then went somewhere nobody could
 * find. Building round a start fixes the picture as well as the test. */
static bool luf_rect_fits(int x0, int y0) {
    return x0 > 1 && y0 > 1 &&
           x0 + LUF_ARENA_W + 1 < MAP_ARRAY_SIZE &&
           y0 + LUF_ARENA_H + 1 < MAP_ARRAY_SIZE;
}

/* Is the whole fight — the tank's square, the pill's, and a square of margin
 * round both — inside the Log Viewer's opening box? */
static bool luf_start_on_screen(int startX, int startY) {
    int lo = LUF_VIEW_X0 + 1;
    int hi = LUF_VIEW_X0 + LUF_VIEW_SIZE - 1;
    return startX >= lo && startX + LUF_APPROACH <= hi &&
           startY >= lo && startY <= hi;
}

/* Pick the start to build on. Every start whose strip fits on the map is a
 * candidate; they are then ordered by what matters, worst penalty first:
 *
 *   1. a strip that crosses a base            — the arena would swallow one
 *   2. a fight outside the Log Viewer's box   — the replay would show no tank
 *   3. distance from the middle of that box   — tie-break, keeps it central
 *
 * The test owns this map, so the penalties are preferences rather than
 * refusals: a strip across a base still beats no arena at all, and the caller
 * says so in the printout. Returns the 1-based start number, or 0 when the map
 * has no start with room for the strip. */
static BYTE luf_pick_start(GameSim *gs, BYTE *outX, BYTE *outY, bool *overBases) {
    BYTE n = startsGetNumStarts(&gs->ss);
    BYTE best = 0, bestX = 0, bestY = 0;
    bool bestOverBases = false;
    long bestScore = 0;
    BYTE i;

    for (i = 1; i <= n; i++) {
        start s;
        int  x0, y0, dx, dy;
        bool crossesBase;
        long score;

        memset(&s, 0, sizeof(s));
        startsGetStartStruct(&gs->ss, &s, i);
        x0 = (int)s.x;
        y0 = (int)s.y - LUF_ARENA_H / 2;
        if (!luf_rect_fits(x0, y0)) {
            continue;
        }

        crossesBase = !luf_rect_base_free(gs, x0, y0);
        dx = (int)s.x - LUF_VIEW_MID;
        dy = (int)s.y - LUF_VIEW_MID;
        if (dx < 0) dx = -dx;
        if (dy < 0) dy = -dy;
        score = (crossesBase ? 1000000L : 0L)
              + (luf_start_on_screen((int)s.x, (int)s.y) ? 0L : 1000L)
              + (long)(dx > dy ? dx : dy);

        if (best == 0 || score < bestScore) {
            best          = i;
            bestX         = s.x;
            bestY         = s.y;
            bestOverBases = crossesBase;
            bestScore     = score;
        }
    }
    if (best != 0) {
        *outX = bestX;
        *outY = bestY;
        *overBases = bestOverBases;
    }
    return best;
}

static void luf_build_arena(LoopbackHarness *h, void *user) {
    LufArena *a = (LufArena *)user;
    GameSim  *gs = &h->sim->sim;
    BYTE startX = 0, startY = 0;
    int x0, y0;
    int x, y, i;
    pillbox p;

    a->ok = false;
    a->startNum = luf_pick_start(gs, &startX, &startY, &a->overBases);
    if (a->startNum == 0) {
        return;
    }
    x0 = (int)startX;
    y0 = (int)startY - LUF_ARENA_H / 2;

    /* Send every slot to that start, so the joiner spawns at the arena's west
     * end instead of wherever the open-game placement scattered it. This is
     * the same field a scenario's "start" op writes, and startsGetStart
     * consumes it ahead of the placement policy. */
    for (i = 0; i < MAX_TANKS; i++) {
        gs->scenarioStartIdx[i] = (BYTE)(a->startNum - 1);
    }

    /* Open grass, so nothing hides the tank from the pill and the man walks
     * at full speed the whole way. */
    for (y = 0; y < LUF_ARENA_H; y++) {
        for (x = 0; x < LUF_ARENA_W; x++) {
            mapSetPos(gs, &gs->mp, (BYTE)(x0 + x), (BYTE)(y0 + y), GRASS,
                      FALSE, TRUE);
        }
    }

    a->tankX = startX;              /* the start itself: the strip's west end */
    a->tankY = startY;
    a->pillY = startY;
    a->pillX = (BYTE)(startX + LUF_APPROACH);

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

/* Runs after every pump of the connect loop, so it doubles as that phase's
 * session recorder — loopbackHarnessPumpUntil has no other per-pump seam. */
static bool luf_connected(LoopbackHarness *h, void *user) {
    (void)user;
    brainRecordTick(h->sim);
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
    const char *replayLeaf;   /* file name under WB_TEST_REPLAY_DIR       */
    const char *sessionTag;   /* leading part of the debug-session dir    */
    uint64_t    seed;
} LufConfig;

/* ── recording the run as a brain-debug session ──────────────────────────── */

/* Open a BrainTest session for this run: debug_sessions/<tag>_<timestamp>/.
 *
 * There is no bot in this test and the recorder does not need one. It resolves
 * its output dir from a bot's Lua DEBUG_SESSION_DIR only when no dir was set
 * explicitly, and brainRecordSetSessionDir sets one, so the bot fallback never
 * runs. Everything else it writes per frame is server-side world state — the
 * god-view snapshot of tanks, shells, bases, pills and the terrain — and the
 * per-bot block is simply written with a count of zero.
 *
 * The one thing the recorder does NOT get here is a caller: the only
 * brainRecordTick() call in the tree is in botManagerTick, which the dedicated
 * server runs only when the game has bots. So this test drives the recorder
 * itself, once per pump, from luf_pump below. */
static void luf_session_start(const LufConfig *cfg, char *dirOut, size_t cap) {
    time_t    now = time(NULL);
    struct tm tmv;
    char      stamp[32];

    dirOut[0] = '\0';
#ifdef _WIN32
    if (localtime_s(&tmv, &now) != 0) {
        return;
    }
#else
    if (localtime_r(&now, &tmv) == NULL) {
        return;
    }
#endif
    strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &tmv);
    snprintf(dirOut, cap, "%s/%s_%s", WB_TEST_DEBUG_SESSION_DIR,
             cfg->sessionTag, stamp);

    SDL_CreateDirectory(WB_TEST_DEBUG_SESSION_DIR);  /* fine if it exists */
    if (!SDL_CreateDirectory(dirOut)) {
        fprintf(stderr, "  session: COULD NOT create %s (%s) — the run goes "
                        "on unrecorded\n", dirOut, SDL_GetError());
        dirOut[0] = '\0';
        return;
    }
    brainRecordSetSessionDir(dirOut);
    brainRecordSetEnabled(true);
    fprintf(stderr, "  session: recording to %s\n", dirOut);
}

/* Close the .btr and disarm. Disarming matters: the recorder is process-wide
 * and every other test in this binary runs in the same process. */
static void luf_session_stop(const char *dir) {
    brainRecordEndGame();
    brainRecordSetEnabled(false);
    brainRecordSetSessionDir(NULL);
    if (dir != NULL && dir[0] != '\0') {
        fprintf(stderr, "  session written: %s  (BrainTest -> Load Session)\n",
                dir);
    }
}

/* One pump, and the frame of it that the session records. Every pump in the
 * run goes through here so the recording has no holes. */
static void luf_pump(LoopbackHarness *h) {
    loopbackHarnessPump(h);
    brainRecordTick(h->sim);
}

/* ── recording the run as a replay ───────────────────────────────────────── */

/* Open a .wbv on the harness's own ServerSim. Called after the arena is
 * carved and BEFORE the client joins: logStart writes the header and the
 * opening snapshot (the carved map and the pillbox) straight away, and the
 * join is then recorded as a player-joined event, which is what the viewer
 * needs to draw a tank at all.
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
        luf_session_stop(sessionDir);                                        \
        loopbackHarnessStop(&h);                                            \
    } while (0)

static int luf_run(const LufConfig *cfg) {
    LoopbackHarness h;
    LufArena  arena;
    LufTrail  trail[LUF_TRAIL_MAX];
    LufImpact impacts[LUF_IMPACT_MAX];
    int       trailCount = 0, impactCount = 0;
    char      replayPath[512];
    char      sessionDir[512];
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
    sessionDir[0] = '\0';

    fprintf(stderr, "\n  ==== %s (impair=%s seed=%llu) ====\n", cfg->label,
            (cfg->impair != NULL) ? cfg->impair : "clean",
            (unsigned long long)cfg->seed);

    UT_ASSERT_MSG(loopbackHarnessStartPrepared(&h, "Builder", /*lobbyMode*/ false,
                                               cfg->impair, cfg->seed,
                                               luf_build_arena, &arena),
                  "harness start failed (%s)", cfg->label);
    if (!arena.ok) {
        LUF_STOP();
        UT_FAIL("no start on the map has room for a %dx%d strip beside it",
                LUF_ARENA_W, LUF_ARENA_H);
    }
    /* Record from here, BEFORE the client joins: the arena is already carved,
     * so the opening snapshot is the right world, and the join that follows
     * is written as a player-joined event. Started after the join, the file
     * held no player record at all and the viewer drew an empty island. The
     * tank's parking move below arrives as ordinary per-tick deltas. */
    luf_record_start(&h, cfg, replayPath, sizeof(replayPath));
    /* And the BrainTest session, from the same moment and for the same reason:
     * the world it records is already the carved one. luf_connected records a
     * frame per pump so the join is in the session too. */
    luf_session_start(cfg, sessionDir, sizeof(sessionDir));
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

    /* Face the tank at the pill, and stock it. The arena's west end is the
     * start the tank spawned on, so this is a turn on the spot and at most a
     * square or two of nudge if the spawn scatter moved it off the centre —
     * not the cross-map teleport the old open-sea arena needed. */
    {
        BYTE spawnX = tankGetMX(&gs->tanks[slot]);
        BYTE spawnY = tankGetMY(&gs->tanks[slot]);
        bool onStart = (spawnX == arena.tankX && spawnY == arena.tankY);

        threadsWaitForMutex();
        tankSetWorld(gs, &gs->tanks[slot], luf_world(arena.tankX),
                     luf_world(arena.tankY), (TURNTYPE)BRADIANS_EAST, false);
        threadsReleaseMutex();
        luf_restock(&h, slot);

        fprintf(stderr,
                "  arena: built on map start %u; tank square (%u,%u), pill "
                "(%u,%u)%s\n"
                "  spawn: the tank came up on (%u,%u) — %s\n",
                (unsigned)arena.startNum,
                (unsigned)arena.tankX, (unsigned)arena.tankY,
                (unsigned)arena.pillX, (unsigned)arena.pillY,
                arena.overBases ? "  (the strip crosses a base: no start on "
                                  "this map had a base-free strip)" : "",
                (unsigned)spawnX, (unsigned)spawnY,
                onStart ? "the start itself, so parking only turns it east"
                        : "a scatter square beside the start, so parking "
                          "moves it back");
        fprintf(stderr, "  slot %u; the replay's view box is squares %d..%d "
                        "both ways, so the fight is %s it\n",
                (unsigned)slot, LUF_VIEW_X0, LUF_VIEW_X0 + LUF_VIEW_SIZE,
                luf_start_on_screen((int)arena.tankX, (int)arena.tankY)
                    ? "inside" : "OUTSIDE");
    }

    /* Phase 0 — settle. Hands off the controls while the client syncs to the
     * moved tank and the server's ping measurement catches up with the link. */
    for (i = 0; i < LUF_SETTLE; i++) {
        luf_send_frame(&h, slot, &inTick, 0, 0, 0, 0, 0);
        luf_pump(&h);
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
        luf_pump(&h);
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
        luf_pump(&h);
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
    luf_pump(&h);
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
        luf_pump(&h);
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
    cfg.sessionTag = "lgm_lagged";
    cfg.seed       = 4242u;
    return luf_run(&cfg);
}

int run_lgm_wall_behind_tank_under_fire_nolag(void) {
    LufConfig cfg;
    cfg.label      = "no lag (control)";
    cfg.impair     = NULL;
    cfg.replayLeaf = "lgm_under_fire_nolag.wbv";
    cfg.sessionTag = "lgm_nolag";
    cfg.seed       = 4242u;
    return luf_run(&cfg);
}
