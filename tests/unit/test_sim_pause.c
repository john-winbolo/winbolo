/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/* The bot pause and the game pause (server_sim_pause.c):
 *
 *  - the idle pause: 0 seconds, N seconds, -1 (never), a human join that
 *    ends it, and the game mode with its wait in a lobby round that had a
 *    human;
 *  - the manual pause, which a human join does not end;
 *  - CMD_PAUSE_SET: who may send it, its wire packet and the status line;
 *  - a paused bot: no overrun counted, no budget kill, a brake-only input;
 *  - the game pause: the world is the same before and after, the queued
 *    inputs are taken off, and the clock stands still;
 *  - a recording with a game pause in it plays back to the same world and
 *    the same length as one without;
 *  - a game with the pause code idle plays out the same as one without. */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "global.h"
#include "bolo_rand.h"
#include "client_command.h"
#include "everard_map.h"
#include "game_sim.h"
#include "bolo_map.h"
#include "pillbox.h"
#include "bases.h"
#include "tank.h"
#include "lgm.h"
#include "players.h"
#include "bot_manager.h"
#include "input_packet.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"
#include "transport_command_codec.h"
#include "threads.h"
#include "replay_harness.h"
#include "test_harness.h"

/* Frames a second, as server_sim_pause.c counts them. */
#define FPS 50u

/* A running game with no player at all: no human, so the idle pause has
 * something to count. */
static ServerSim *makeEmptyRunningSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) {
        return NULL;
    }
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    return sim;
}

static void frames(ServerSim *sim, uint32_t n) {
    for (uint32_t i = 0; i < n; i++) {
        serverSimPauseFrame(sim);
    }
}

/* -idlepause 0: the first frame with no human pauses the bots; a human
 * joining ends it on the next frame. */
int run_sim_pause_idle_zero(void) {
    ServerSim *sim = makeEmptyRunningSim();
    UT_ASSERT(sim != NULL);
    UT_ASSERT_MSG(serverSimGetIdlePauseSeconds(sim) == -1,
                  "the idle pause must be off by default (got %d)",
                  serverSimGetIdlePauseSeconds(sim));
    serverSimSetIdlePause(sim, 0, SIM_PAUSE_BOTS);
    UT_ASSERT(!serverSimAreBotsPaused(sim));
    frames(sim, 1);
    UT_ASSERT_MSG(serverSimAreBotsPaused(sim) && serverSimIsIdlePauseActive(sim),
                  "-idlepause 0 must pause on the first empty frame");
    UT_ASSERT_MSG(!serverSimIsGamePaused(sim), "bots mode must not pause the game");
    UT_ASSERT_MSG(sim->botMgr.paused, "the bot manager must see the pause");
    UT_ASSERT(!serverSimIsManualPause(sim, SIM_PAUSE_BOTS));

    serverSimAddPlayer(sim, 1, "Joiner", false);
    frames(sim, 1);
    UT_ASSERT_MSG(!serverSimAreBotsPaused(sim) && !sim->botMgr.paused,
                  "a human join must end the idle pause at once");
    /* The human stays: no pause comes back. */
    frames(sim, 10 * FPS);
    UT_ASSERT(!serverSimAreBotsPaused(sim));
    serverSimDestroy(sim);
    return 0;
}

/* -idlepause 5: no pause before 250 empty frames, a pause at 250. A human
 * who joins and leaves starts the count again. */
int run_sim_pause_idle_seconds(void) {
    ServerSim *sim = makeEmptyRunningSim();
    UT_ASSERT(sim != NULL);
    serverSimSetIdlePause(sim, 5, SIM_PAUSE_BOTS);

    frames(sim, 3 * FPS);
    serverSimAddPlayer(sim, 1, "Visitor", false);
    frames(sim, 1);
    serverSimRemovePlayer(sim, 1);
    frames(sim, 5 * FPS - 1);
    UT_ASSERT_MSG(!serverSimAreBotsPaused(sim),
                  "a visit must start the 5 s count again");
    frames(sim, 1);
    UT_ASSERT_MSG(serverSimAreBotsPaused(sim),
                  "5 s with no human must pause the bots");

    char status[256];
    serverSimFormatPauseStatus(sim, status, sizeof(status));
    UT_ASSERT_MSG(strstr(status, "bots: paused (idle, 0 s)") != NULL &&
                  strstr(status, "game: running") != NULL &&
                  strstr(status, "idle pause: 5 s, mode bots, on") != NULL,
                  "status: '%s'", status);
    frames(sim, 3 * FPS);
    UT_ASSERT_MSG(serverSimGetPauseSeconds(sim, SIM_PAUSE_BOTS) == 3,
                  "the pause must count its seconds (got %u)",
                  (unsigned)serverSimGetPauseSeconds(sim, SIM_PAUSE_BOTS));

    /* -1 at run time ends the idle pause and keeps it off. */
    serverSimSetIdlePause(sim, -1, SIM_PAUSE_BOTS);
    UT_ASSERT(!serverSimAreBotsPaused(sim));
    serverSimDestroy(sim);
    return 0;
}

/* -idlepause -1 (the default): an empty server never pauses. */
int run_sim_pause_idle_never(void) {
    ServerSim *sim = makeEmptyRunningSim();
    UT_ASSERT(sim != NULL);
    frames(sim, 60 * FPS);
    UT_ASSERT(serverSimGetPauseFlags(sim) == 0);
    serverSimSetIdlePause(sim, -1, SIM_PAUSE_GAME);
    frames(sim, 60 * FPS);
    UT_ASSERT(serverSimGetPauseFlags(sim) == 0);
    char status[256];
    serverSimFormatPauseStatus(sim, status, sizeof(status));
    UT_ASSERT_MSG(strcmp(status, "bots: running; game: running; idle pause: off") == 0,
                  "status: '%s'", status);
    serverSimDestroy(sim);
    return 0;
}

/* A manual pause stays when a human joins; only a manual resume ends it.
 * With the idle pause on as well, the join ends the idle part and the
 * manual part stays. */
int run_sim_pause_manual_not_resumed_by_join(void) {
    ServerSim *sim = makeEmptyRunningSim();
    UT_ASSERT(sim != NULL);
    serverSimSetIdlePause(sim, 0, SIM_PAUSE_BOTS);
    frames(sim, 1);
    UT_ASSERT(serverSimIsIdlePauseActive(sim));
    UT_ASSERT(serverSimSetManualPause(sim, SIM_PAUSE_BOTS, true));
    UT_ASSERT_MSG(!serverSimSetManualPause(sim, SIM_PAUSE_BOTS, true),
                  "a second pause must report no change");
    char status[256];
    serverSimFormatPauseStatus(sim, status, sizeof(status));
    UT_ASSERT_MSG(strstr(status, "bots: paused (manual+idle") != NULL,
                  "status: '%s'", status);

    serverSimAddPlayer(sim, 1, "Joiner", false);
    frames(sim, 10 * FPS);
    UT_ASSERT_MSG(!serverSimIsIdlePauseActive(sim),
                  "the join must end the idle part");
    UT_ASSERT_MSG(serverSimAreBotsPaused(sim) && sim->botMgr.paused,
                  "a human join must not end a manual pause");
    serverSimFormatPauseStatus(sim, status, sizeof(status));
    UT_ASSERT_MSG(strstr(status, "bots: paused (manual, 10 s)") != NULL,
                  "status: '%s'", status);

    UT_ASSERT(serverSimSetManualPause(sim, SIM_PAUSE_BOTS, false));
    UT_ASSERT(!serverSimAreBotsPaused(sim) && !sim->botMgr.paused);

    /* The same for the game. */
    UT_ASSERT(serverSimSetManualPause(sim, SIM_PAUSE_GAME, true));
    serverSimAddPlayer(sim, 2, "Second", false);
    frames(sim, FPS);
    UT_ASSERT(serverSimIsGamePaused(sim) && !serverSimAreBotsPaused(sim));
    UT_ASSERT(serverSimSetManualPause(sim, SIM_PAUSE_GAME, false));
    UT_ASSERT(serverSimGetPauseFlags(sim) == 0);

    /* A bad target changes nothing. */
    UT_ASSERT(!serverSimSetManualPause(sim, SIM_PAUSE_TARGET_COUNT, true));
    UT_ASSERT(serverSimGetPauseFlags(sim) == 0);
    serverSimDestroy(sim);
    return 0;
}

/* -idlepausemode game: the whole game pauses, and a join ends it. In a
 * lobby round that had a human it waits: that round goes back to the
 * lobby when its last human leaves, and a game pause would stop the tick
 * that does it. */
int run_sim_pause_idle_game_mode(void) {
    ServerSim *sim = makeEmptyRunningSim();
    UT_ASSERT(sim != NULL);
    serverSimSetIdlePause(sim, 2, SIM_PAUSE_GAME);
    frames(sim, 2 * FPS);
    UT_ASSERT_MSG(serverSimIsGamePaused(sim) && !serverSimAreBotsPaused(sim),
                  "game mode must pause the game and only the game (flags %u)",
                  (unsigned)serverSimGetPauseFlags(sim));
    serverSimAddPlayer(sim, 1, "Joiner", false);
    frames(sim, 1);
    UT_ASSERT(serverSimGetPauseFlags(sim) == 0);
    serverSimRemovePlayer(sim, 1);

    sim->lobbyEnabled  = true;
    sim->roundHadHuman = true;
    frames(sim, 30 * FPS);
    UT_ASSERT_MSG(!serverSimIsGamePaused(sim),
                  "a lobby round that had a human must wait for the lobby");
    sim->roundHadHuman = false;
    frames(sim, 1);
    UT_ASSERT_MSG(serverSimIsGamePaused(sim),
                  "with the wait gone the pause must start");
    sim->lobbyEnabled = false;

    /* Moving the idle pause to the other target lifts the game pause. */
    serverSimSetIdlePause(sim, 2, SIM_PAUSE_BOTS);
    UT_ASSERT(!serverSimIsGamePaused(sim));
    frames(sim, 1);
    UT_ASSERT(serverSimAreBotsPaused(sim) && !serverSimIsGamePaused(sim));
    serverSimDestroy(sim);
    return 0;
}

static CmdResult applyPauseSet(ServerSim *sim, int sender, uint8_t target,
                               bool paused) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PAUSE_SET;
    cmd.cmdSeq = 1;
    cmd.u.pauseSet.target = target;
    cmd.u.pauseSet.paused = paused;
    threadsWaitForMutex();
    CmdResult r = serverSimApplyCommand(sim, sender, &cmd);
    threadsReleaseMutex();
    return r;
}

/* CMD_PAUSE_SET from the host does the pause; from a player who is not the
 * host, or from a bot, it is refused; a bad target is refused. The wire
 * packet carries it both ways. */
int run_sim_pause_command(void) {
    ServerSim *sim = ut_make_running_sim("Host");
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 1, "Guest", false);
    serverSimAddPlayer(sim, 2, "Bot1", false);
    sim->botMgr.bots[2].active = true;

    UT_ASSERT(applyPauseSet(sim, 0, SIM_PAUSE_BOTS, true) == CMD_OK);
    UT_ASSERT(serverSimIsManualPause(sim, SIM_PAUSE_BOTS));
    UT_ASSERT(applyPauseSet(sim, 0, SIM_PAUSE_GAME, true) == CMD_OK);
    UT_ASSERT(serverSimIsGamePaused(sim));
    UT_ASSERT(applyPauseSet(sim, 0, SIM_PAUSE_GAME, false) == CMD_OK);
    UT_ASSERT(!serverSimIsGamePaused(sim));

    CmdResult r = applyPauseSet(sim, 1, SIM_PAUSE_BOTS, false);
    UT_ASSERT_MSG(r == CMD_REJECT_NOT_HOST, "a guest must be refused (got %d)", (int)r);
    r = applyPauseSet(sim, 2, SIM_PAUSE_BOTS, false);
    UT_ASSERT_MSG(r == CMD_REJECT_NOT_HOST, "a bot must be refused (got %d)", (int)r);
    UT_ASSERT(serverSimIsManualPause(sim, SIM_PAUSE_BOTS));
    r = applyPauseSet(sim, 0, 7, true);
    UT_ASSERT_MSG(r == CMD_REJECT_INVALID, "a bad target must be refused (got %d)", (int)r);
    UT_ASSERT(applyPauseSet(sim, 0, SIM_PAUSE_BOTS, false) == CMD_OK);
    UT_ASSERT(serverSimGetPauseFlags(sim) == 0);
    sim->botMgr.bots[2].active = false;
    serverSimDestroy(sim);

    /* The wire packet. */
    ClientCommand in;
    memset(&in, 0, sizeof(in));
    in.type = CMD_PAUSE_SET;
    in.cmdSeq = 42;
    in.u.pauseSet.target = SIM_PAUSE_GAME;
    in.u.pauseSet.paused = true;
    uint8_t buf[64];
    size_t len = 0;
    UT_ASSERT_MSG(commandCodecEncode(&in, buf, sizeof(buf), &len),
                  "CMD_PAUSE_SET must encode");
    UT_ASSERT(buf[2] == PACKET_PAUSE_SET);
    ClientCommand out;
    memset(&out, 0, sizeof(out));
    UT_ASSERT(commandCodecDecode(buf, len, &out));
    UT_ASSERT(out.type == CMD_PAUSE_SET && out.cmdSeq == 42 &&
              out.u.pauseSet.target == SIM_PAUSE_GAME && out.u.pauseSet.paused);
    UT_ASSERT_MSG(!commandCodecDecode(buf, len - 1, &out),
                  "a short packet must not decode");
    return 0;
}

/* While the game is paused the notice goes out once at the edge and then
 * once a second, as EVENT_SERVER_MSG with the flags in the id. */
int run_sim_pause_notice(void) {
    ServerSim *sim = ut_make_running_sim("Host");
    UT_ASSERT(sim != NULL);
    serverSimSetManualPause(sim, SIM_PAUSE_GAME, true);
    int notices = 0;
    uint8_t lastId = 0;
    for (uint32_t f = 0; f < 3 * FPS; f++) {
        serverSimLocalTick(sim, f, NULL, NULL);
        for (int e = 0; e < sim->eventCount; e++) {
            if (sim->events[e].type == EVENT_SERVER_MSG) {
                notices++;
                lastId = sim->events[e].data[0];
            }
        }
    }
    UT_ASSERT_MSG(notices >= 3 && notices <= 4,
                  "about one notice a second, got %d in 3 s", notices);
    UT_ASSERT_MSG(lastId == SERVER_MSG_PAUSE_STATE_BASE + SIM_PAUSE_FLAG_GAME,
                  "the notice must carry the game flag (id %u)", (unsigned)lastId);
    serverSimSetManualPause(sim, SIM_PAUSE_GAME, false);
    serverSimLocalTick(sim, 1000, NULL, NULL);
    bool sawClear = false;
    for (int e = 0; e < sim->eventCount; e++) {
        if (sim->events[e].type == EVENT_SERVER_MSG &&
            sim->events[e].data[0] == SERVER_MSG_PAUSE_STATE_BASE) {
            sawClear = true;
        }
    }
    UT_ASSERT_MSG(sawClear, "the resume must send a notice with no flags");
    serverSimDestroy(sim);
    return 0;
}

/* Two paused bots: an old think time far over the target is not counted
 * as an overrun, no bot is dropped, and each frame queues a brake-only
 * input for each bot. */
int run_sim_pause_bots_no_budget_kill(void) {
    static const BYTE kBots[2] = { 1, 2 };
    ServerSim *sim = ut_make_running_sim("Human");
    UT_ASSERT(sim != NULL);
    ut_brain_stub_arm(true);
    ut_brain_stub_lua(true);
    ut_brain_stub_tick_ok(true);
    bool added = true;
    for (int i = 0; i < 2; i++) {
        added = added && serverSimCreateBot(sim, kBots[i], "brains/GoalHunter/init.lua",
                                            i == 0 ? "BotA" : "BotB", aiNone, gameOpen,
                                            false, 0, NULL);
    }
    UT_ASSERT_MSG(added, "serverSimCreateBot failed");
    for (int f = 0; f < 5; f++) {
        serverSimBotTick(sim, aiNone);
        serverSimTick(sim);
    }

    serverSimSetManualPause(sim, SIM_PAUSE_BOTS, true);
    bool brakeOnly = true;
    for (int f = 0; f < 200; f++) {
        for (int i = 0; i < 2; i++) {
            sim->botMgr.bots[kBots[i]].lastThinkMs = 1000.0;
            sim->botMgr.bots[kBots[i]].overrunCount = 0;
        }
        serverSimBotTick(sim, aiNone);
        for (int i = 0; i < 2; i++) {
            BYTE p = kBots[i];
            uint8_t head = (uint8_t)(sim->inputQueueHead[p] - 1) &
                           (SERVER_INPUT_QUEUE_SIZE - 1);
            const InputPacket *in = &sim->inputQueue[p][head];
            if (in->buttons != INPUT_BTN_DECEL || in->actions != 0 ||
                in->buildAction != 0) {
                brakeOnly = false;
            }
        }
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(brakeOnly, "a paused bot must send a brake and nothing else");
    for (int i = 0; i < 2; i++) {
        UT_ASSERT_MSG(sim->botMgr.bots[kBots[i]].overrunCount == 0,
                      "a paused bot must not count an overrun (bot %d: %u)",
                      (int)kBots[i], (unsigned)sim->botMgr.bots[kBots[i]].overrunCount);
        UT_ASSERT_MSG(botManagerIsBot(sim, kBots[i]),
                      "a paused bot must stay in the game (bot %d)", (int)kBots[i]);
    }
    serverSimSetManualPause(sim, SIM_PAUSE_BOTS, false);
    UT_ASSERT(!sim->botMgr.paused);
    serverSimDestroyBots(sim);
    ut_brain_stub_tick_ok(false);
    ut_brain_stub_lua(false);
    ut_brain_stub_arm(false);
    serverSimDestroy(sim);
    return 0;
}

/* The parts of the world a pause must not change. */
typedef struct {
    uint32_t tick;
    struct {
        bool     inUse;
        WORLD    x, y;
        TURNTYPE angle;
        BYTE     speed, shells, mines, armour, trees;
        WORLD    lgmX, lgmY;
    } tanks[MAX_TANKS];
    int         numShells;
    ShellRender shells[64];
    pillbox     pills[MAX_PILLS];
    base        bases[MAX_BASES];
    uint32_t    terrainHash;
} PauseWorld;

static void captureWorld(ServerSim *sim, PauseWorld *w) {
    GameSim *gs = serverSimGetGameSim(sim);
    memset(w, 0, sizeof(*w));
    w->tick = serverSimGetTick(sim);
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (!playersIsInUse(&gs->plyrs, i) || gs->tanks[i] == NULL) {
            continue;
        }
        w->tanks[i].inUse = true;
        tankGetWorld(&gs->tanks[i], &w->tanks[i].x, &w->tanks[i].y);
        w->tanks[i].angle = tankGetAngle(&gs->tanks[i]);
        w->tanks[i].speed = tankGetSpeed(&gs->tanks[i]);
        tankGetStats(&gs->tanks[i], &w->tanks[i].shells, &w->tanks[i].mines,
                     &w->tanks[i].armour, &w->tanks[i].trees);
        if (gs->lgmen[i] != NULL) {
            w->tanks[i].lgmX = lgmGetWX(&gs->lgmen[i]);
            w->tanks[i].lgmY = lgmGetWY(&gs->lgmen[i]);
        }
    }
    w->numShells = serverSimGetShellSnapshot(sim, w->shells, 64);
    BYTE n = pillsGetNumPills(&gs->pb);
    for (BYTE p = 1; p <= n && p <= MAX_PILLS; p++) {
        pillsGetPill(&gs->pb, &w->pills[p - 1], p);
    }
    n = basesGetNumBases(&gs->bs);
    for (BYTE b = 1; b <= n && b <= MAX_BASES; b++) {
        basesGetBase(&gs->bs, &w->bases[b - 1], b);
    }
    uint32_t h = 2166136261u;
    for (int x = 0; x < 256; x++) {
        for (int y = 0; y < 256; y++) {
            h = (h ^ mapGetPos(&gs->mp, (BYTE)x, (BYTE)y)) * 16777619u;
        }
    }
    w->terrainHash = h;
}

/* One input pair for slot 0 at the server's next two ticks: drive, turn
 * and fire. */
static void queueDriveInput(ServerSim *sim, uint32_t *nextTick) {
    for (int k = 0; k < 2; k++) {
        InputPacket in;
        memset(&in, 0, sizeof(in));
        in.playerNum = 0;
        in.tick = (*nextTick)++;
        in.buttons = INPUT_BTN_ACCEL | INPUT_BTN_LEFT;
        in.actions = (in.tick % 2 == 0) ? INPUT_ACTION_FIRE : 0;
        serverSimApplyInput(sim, &in);
    }
}

/* The game pause through serverSimLocalTick: a tank that is driving,
 * turning and firing, and its shells in the air, are where they were after
 * 10 s of pause, the clock has not moved, and the inputs sent in the pause
 * did not pile up. After the resume the game goes on. */
int run_sim_pause_game_state_frozen(void) {
    ServerSim *sim = ut_make_running_sim("Driver");
    UT_ASSERT(sim != NULL);
    uint32_t nextTick = serverSimGetTick(sim) + 1;
    uint32_t frame = 0;
    uint32_t tick50 = 0;
    for (int f = 0; f < 60; f++) {
        if (f == 50) {
            tick50 = serverSimGetTick(sim);
        }
        queueDriveInput(sim, &nextTick);
        serverSimLocalTick(sim, frame++, NULL, NULL);
    }
    PauseWorld before;
    captureWorld(sim, &before);
    UT_ASSERT_MSG(before.tanks[0].speed > 0, "the tank must be moving first");

    serverSimSetManualPause(sim, SIM_PAUSE_GAME, true);
    for (uint32_t f = 0; f < 10 * FPS; f++) {
        queueDriveInput(sim, &nextTick);
        serverSimLocalTick(sim, frame++, NULL, NULL);
        UT_ASSERT_MSG(sim->inputQueueHead[0] == sim->inputQueueTail[0],
                      "a paused frame must take the queued inputs off");
    }
    PauseWorld after;
    captureWorld(sim, &after);
    UT_ASSERT_MSG(after.tick == before.tick, "the clock moved in a pause (%u to %u)",
                  (unsigned)before.tick, (unsigned)after.tick);
    UT_ASSERT_MSG(memcmp(&before, &after, sizeof(before)) == 0,
                  "the world changed in a game pause");
    UT_ASSERT_MSG(serverSimGetLastProcessedInput(sim, 0) == nextTick - 1,
                  "the dropped inputs must count as processed (%u, want %u)",
                  (unsigned)serverSimGetLastProcessedInput(sim, 0),
                  (unsigned)(nextTick - 1));
    UT_ASSERT(serverSimGetPauseSeconds(sim, SIM_PAUSE_GAME) == 10);

    serverSimSetManualPause(sim, SIM_PAUSE_GAME, false);
    for (int f = 0; f < 10; f++) {
        queueDriveInput(sim, &nextTick);
        serverSimLocalTick(sim, frame++, NULL, NULL);
    }
    /* Ten frames move the clock as far as the ten before the pause did. */
    UT_ASSERT_MSG(serverSimGetTick(sim) - before.tick == before.tick - tick50,
                  "the clock must go on after the resume (%u, %u, %u)",
                  (unsigned)tick50, (unsigned)before.tick,
                  (unsigned)serverSimGetTick(sim));
    serverSimDestroy(sim);
    return 0;
}

/* Record a round with a 5 s game pause in the middle, and one with none.
 * Each plays back to the world it recorded, and both have the same length:
 * the paused frames wrote nothing to the .wbv. */
static bool recordRound(const char *tag, bool withPause, ReplayHarness *h,
                        ReplayFileInfo *info, char *why, size_t whyLen) {
    memset(h, 0, sizeof(*h));
    if (!replayHarnessStartRecording(h, tag, "Tester")) {
        snprintf(why, whyLen, "could not start the recording");
        return false;
    }
    uint32_t frame = 0;
    for (int f = 0; f < 20; f++) {
        serverSimLocalTick(h->sim, frame++, NULL, NULL);
    }
    if (withPause) {
        serverSimSetManualPause(h->sim, SIM_PAUSE_GAME, true);
        for (uint32_t f = 0; f < 5 * FPS; f++) {
            serverSimLocalTick(h->sim, frame++, NULL, NULL);
        }
        serverSimSetManualPause(h->sim, SIM_PAUSE_GAME, false);
    }
    for (int f = 0; f < 20; f++) {
        serverSimLocalTick(h->sim, frame++, NULL, NULL);
    }
    if (!replayHarnessStopRecording(h) || !replayHarnessDecode(h)) {
        snprintf(why, whyLen, "could not stop or decode %s", h->path);
        return false;
    }
    if (!replayHarnessCompare(h)) {
        snprintf(why, whyLen, "%s", replayHarnessDiff(h));
        return false;
    }
    ReplayWorld w;
    if (!replayHarnessDecodeFile(h->path, &w, info)) {
        snprintf(why, whyLen, "could not decode %s for its length", h->path);
        return false;
    }
    return true;
}

int run_sim_pause_recording_round_trip(void) {
    ReplayHarness a, b;
    ReplayFileInfo ia, ib;
    char why[256] = "";
    memset(&ia, 0, sizeof(ia));
    memset(&ib, 0, sizeof(ib));
    bool okA = recordRound("pauseGame", true, &a, &ia, why, sizeof(why));
    replayHarnessStop(&a);
    UT_ASSERT_MSG(okA, "the paused round: %s", why);
    bool okB = recordRound("noPause", false, &b, &ib, why, sizeof(why));
    replayHarnessStop(&b);
    UT_ASSERT_MSG(okB, "the plain round: %s", why);
    UT_ASSERT_MSG(ia.ticks == ib.ticks && ia.totalTimeMs == ib.totalTimeMs,
                  "the pause must add nothing to the recording "
                  "(%d ticks %u ms against %d ticks %u ms)",
                  ia.ticks, (unsigned)ia.totalTimeMs,
                  ib.ticks, (unsigned)ib.totalTimeMs);
    return 0;
}

/* The pause code, when it does not pause, changes nothing: the same seeded
 * game played with the idle pause off, and with it on but a human there
 * the whole time (so it never starts), ends in the same world. */
static void playSeeded(bool idleArmed, PauseWorld *out) {
    static const BoloRandState kSeed = { { 0x12345678u, 0x9abcdef0u,
                                           0x0fedcba9u, 0x87654321u } };
    bolo_rand_restore(&kSeed);
    ServerSim *sim = ut_make_running_sim("Driver");
    if (sim == NULL) {
        memset(out, 0xff, sizeof(*out));
        return;
    }
    if (idleArmed) {
        serverSimSetIdlePause(sim, 0, SIM_PAUSE_GAME);
    }
    uint32_t nextTick = serverSimGetTick(sim) + 1;
    for (uint32_t f = 0; f < 500; f++) {
        queueDriveInput(sim, &nextTick);
        serverSimLocalTick(sim, f, NULL, NULL);
    }
    captureWorld(sim, out);
    serverSimDestroy(sim);
}

int run_sim_pause_determinism(void) {
    PauseWorld *plain = (PauseWorld *)malloc(sizeof(PauseWorld));
    PauseWorld *armed = (PauseWorld *)malloc(sizeof(PauseWorld));
    UT_ASSERT(plain != NULL && armed != NULL);
    playSeeded(false, plain);
    playSeeded(true, armed);
    bool same = memcmp(plain, armed, sizeof(PauseWorld)) == 0;
    uint32_t tick = plain->tick;
    free(plain);
    free(armed);
    UT_ASSERT_MSG(tick >= 500, "the game must have run (tick %u)", (unsigned)tick);
    UT_ASSERT_MSG(same, "an idle pause that never starts changed the game");
    return 0;
}
