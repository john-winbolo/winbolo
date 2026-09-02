/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Server Simulation
 *Filename:      server_sim.c
 *Author:        John Morrison
 *Purpose:
 *  Standalone authoritative game simulation. Runs the
 *  full game from InputPacket inputs, independent of
 *  servercore.c and screen.c.
 *********************************************************/

#include <ctype.h>
#include <string.h>
#include <stdlib.h>
#include <stdio.h>
#include <time.h>
#include <limits.h>  /* INT_MAX for default-team load balance */
/* dirent.h removed — using SDL3 SDL_GlobDirectory for cross-platform directory listing */
#include <SDL3/SDL.h>

#include "bolo_rand.h"
#include "global.h"
#include "bolo_map.h"
#include "netpacks.h"
#include "wire_limits.h"   /* LobbySettingType for serverSimGetSettingLockBit */
#include "lobby_bot_pools.h"   /* lobbyBotPoolCount for serverSimSetTeamMeta */
#include "pillbox.h"
#include "bases.h"
#include "starts.h"
#include "tank.h"
#include "shells.h"
#include "lgm.h"
#include "explosions.h"
#include "mines.h"
#include "minesexp.h"
#include "floodfill.h"
#include "building.h"
#include "rubble.h"
#include "swamp.h"
#include "grass.h"
#include "tankexp.h"
#include "treegrow.h"
#include "gametype.h"
#include "players.h"
#include "log.h"
#include "util.h"
#include "input_packet.h"
#include "messages.h"
#include "sounddist.h"
#include "transport_udp.h"
#include "playersrejoin.h"
#include "bot_manager.h"
#include "bot_worker_pool.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "../winbolonet/http.h"
#include "server_sim_internal.h"
#include "attribution_track.h"
#include "round_stats_derive.h"   /* roundStatsApplyRecord, computeAwards */
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"
#include "control_event.h"
#include "client_sim.h"
#include "client_sim_control.h"
#include "transport_control_codec.h"  /* body encoders for the ring keyframe's control snapshot */
#include "log_internal.h"             /* serverSimSerializeControlSnapshot prototype */
#include <assert.h>
#include "interpolation.h"
#include "position_history.h"
#include "screenbullet.h"
#include "mapgen.h"
#include "../common/wb_log.h"
#include "../common/mp_diag_log.h"
#include "../common/md5.h"
#include "server_sim_join.h"
#include "playername_validate.h"
#include "sim/server_sim_shared.h"   /* serverSimCb* for the callback table, serverSimTrackAppend, the base/pill collectors */

/* Wall-clock length of one sim->tick while a round is running. serverSimTick
 * runs simRunHalfStep twice per 20 ms frame in serverStateRunning and once in
 * every other state, so a running tick is 10 ms and a lobby/countdown tick is
 * 20 ms. Anything converting a round's ticks to time wants this one; change the
 * half-step count and this has to change with it. */
#define SIM_TICK_MS 10u

/* Forward declarations for map-skip publish path — definitions live
 * further down the file. */
static void serverSimFillMapSkipStateEvent(const ServerSim *sim,
                                           ControlEvent *evt);
static void publishMapSkipState(ServerSim *sim);

/* Server-originated English broadcast; defined further down the file. */
static void publishServerMessage(ServerSim *sim, const char *message);

/* Active sim pointer — when non-NULL, servercore.c routing functions
 * access sim state directly instead of using legacy globals. */
static THREAD_LOCAL ServerSim *activeSim = NULL;

/* Live-sim registry.
 *
 * activeSim is per-thread (the gym runs one sim per worker thread), so
 * serverSimDestroy can only ever clear the slot belonging to the thread
 * that runs it.  Every OTHER thread that armed its slot for that sim —
 * SDL's timer thread ticking a hosted server, a WBN worker that published
 * through serverSimPublishControl — is left pointing at freed memory, and
 * the next serverSimGetActive() there hands a dangling pointer to a caller
 * that dereferences it (transportUdpServerGetPlayerName -> serverSimIsBot
 * -> botManagerIsBot).
 *
 * Clearing the other threads' slots from here is not an option: that means
 * writing into the TLS block of a thread that may already have exited.  So
 * the read side validates instead — a pointer no longer registered is not
 * handed out, and the stale slot is dropped on the spot.
 *
 * Registration is create/destroy scoped, so the array holds one entry per
 * concurrently live sim: one or two for the game, one per environment for
 * the gym.  Overflow degrades to the old unchecked behaviour rather than
 * making a live sim invisible to its own routing functions. */
#define SERVER_SIM_LIVE_MAX 256
static ServerSim *s_liveSims[SERVER_SIM_LIVE_MAX];
static SDL_AtomicInt s_liveSimsOverflowed;

static void serverSimRegisterLive(ServerSim *sim) {
    int i;
    for (i = 0; i < SERVER_SIM_LIVE_MAX; i++) {
        if (SDL_CompareAndSwapAtomicPointer((void **)&s_liveSims[i],
                                            NULL, sim)) {
            return;
        }
    }
    SDL_SetAtomicInt(&s_liveSimsOverflowed, 1);
    WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                 "serverSim live registry full (%d slots) — stale activeSim "
                 "detection is now disabled for this process",
                 SERVER_SIM_LIVE_MAX);
}

static void serverSimUnregisterLive(ServerSim *sim) {
    int i;
    for (i = 0; i < SERVER_SIM_LIVE_MAX; i++) {
        if (SDL_GetAtomicPointer((void **)&s_liveSims[i]) == (void *)sim) {
            SDL_SetAtomicPointer((void **)&s_liveSims[i], NULL);
            return;
        }
    }
}

static bool serverSimIsLive(ServerSim *sim) {
    int i;
    if (SDL_GetAtomicInt(&s_liveSimsOverflowed) != 0) {
        return true;
    }
    for (i = 0; i < SERVER_SIM_LIVE_MAX; i++) {
        if (SDL_GetAtomicPointer((void **)&s_liveSims[i]) == (void *)sim) {
            return true;
        }
    }
    return false;
}

/* Map change callback: records terrain changes into the dedicated map event
 * buffer so they never compete with sound/game events for slots. */
static void simMapChangeCallback(BYTE x, BYTE y, BYTE terrain) {
    if (activeSim != NULL && activeSim->mapEventCount < MAX_MAP_EVENTS) {
        GameEvent *ev = &activeSim->mapEvents[activeSim->mapEventCount];
        ev->type = EVENT_MAP_CHANGE;
        memset(ev->data, 0, sizeof(ev->data));
        ev->data[0] = x;
        ev->data[1] = y;
        ev->data[2] = terrain;
        activeSim->mapEventCount++;
    }
}

ServerSim *serverSimGetActive(void) {
    /* A slot armed for a sim that has since been destroyed — necessarily by
     * some other thread, since destroy clears its own — must never be handed
     * out.  Drop it so subsequent calls on this thread are a plain read. */
    if (activeSim != NULL && !serverSimIsLive(activeSim)) {
        activeSim = NULL;
    }
    return activeSim;
}

void serverSimSetActive(ServerSim *sim) {
    activeSim = sim;
}

/*********************************************************
 *NAME:          translateInputToTankButton
 *PURPOSE:
 *  Converts an InputPacket button bitmask to the existing
 *  tankButton enum value.
 *
 *ARGUMENTS:
 *  buttons - The bitmask from InputPacket.buttons
 *********************************************************/
static tankButton translateInputToTankButton(uint8_t buttons) {
    bool accel = (buttons & INPUT_BTN_ACCEL) != 0;
    bool decel = (buttons & INPUT_BTN_DECEL) != 0;
    bool left  = (buttons & INPUT_BTN_LEFT)  != 0;
    bool right = (buttons & INPUT_BTN_RIGHT) != 0;

    /* If both accel and decel, they cancel out */
    if (accel && decel) {
        accel = FALSE;
        decel = FALSE;
    }
    /* If both left and right, they cancel out */
    if (left && right) {
        left = FALSE;
        right = FALSE;
    }

    if (left && accel)  return TLEFTACCEL;
    if (right && accel) return TRIGHTACCEL;
    if (left && decel)  return TLEFTDECEL;
    if (right && decel) return TRIGHTDECEL;
    if (left)           return TLEFT;
    if (right)          return TRIGHT;
    if (accel)          return TACCEL;
    if (decel)          return TDECEL;
    return TNONE;
}

#ifdef WB_NETDEBUG
/* Net-debug rig: true when a tankButton carries a left/right turn
 * component, including the turn+accel/decel combos. Used to count
 * sim-executed turn half-steps. Test-only — never built in production. */
static bool netdebugButtonTurns(tankButton tb) {
    switch (tb) {
        case TLEFT:
        case TRIGHT:
        case TLEFTACCEL:
        case TRIGHTACCEL:
        case TLEFTDECEL:
        case TRIGHTDECEL:
            return true;
        default:
            return false;
    }
}
#endif

static void serverSimInit(ServerSim *sim, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen) {
    BYTE count;

    memset(sim, 0, sizeof(ServerSim));

    /* Sentinel value for "no batch start assigned" — memset gives 0, but 0
     * is a valid start index, so initialise explicitly. */
    for (count = 0; count < MAX_TANKS; count++) {
        sim->sim.pendingStartIdx[count] = MAX_STARTS;
    }

    /* "No tutorial progress yet" — memset would leave 0, which (being below
     * every stop row) would disable all tutorial stops. */
    sim->sim.tutorialMinRow = 0xFF;

    /* Sentinel "use the CLI-configured default brain" for every slot.
     * 0 is a valid brain-catalogue index, so memset doesn't suffice. */
    memset(sim->botBrainIdx, 0xFF, sizeof(sim->botBrainIdx));

    /* No closest base recorded yet for any recipient (BASE_NOT_FOUND is 254,
     * fits one byte). 0 is a valid 1-based base, so memset(0) won't do. */
    memset(sim->lastClosestBase, BASE_NOT_FOUND, sizeof(sim->lastClosestBase));

    /* Populate the available-brains list so the lobby can advertise
     * them via PACKET_LOBBY_BRAIN_LIST. Cheap one-shot scan of the
     * brains/ tree. */
    brainListScan(&sim->brainList, sim->brainPaths);

    sim->startDelay = startDelay;
    sim->gameLength = gameLen;
    sim->originalGameLength = gameLen;
    sim->tickLimit = 0;
    sim->ticksRun = 0;
    sim->gameTickLimit = 0;
    sim->gameTicksRun = 0;
    sim->tick = 0;
    sim->roundLogStartTick = ROUND_LOG_START_UNSET;
    sim->state = serverStateLobby;
    sim->lobbyEnabled = TRUE;
    sim->countdownTicks = 0;
    sim->hadPlayersEver = FALSE;
    sim->quitOnWin = FALSE;
    sim->autoCloseOnEmpty = FALSE;
    sim->mapRotateEnabled = FALSE;
    sim->pendingWinMessage[0] = '\0';
    sim->emptyResetEnabled = TRUE;
    sim->emptyResetMinutes = 5;
    sim->emptyResetTicks = -1;
    sim->hasPassword = FALSE;
    sim->wantLogging = FALSE;
    memset(sim->userLogFileName, 0, sizeof(sim->userLogFileName));
    sim->cachedMapData = NULL;
    sim->cachedMapDataLen = 0;
    sim->previousMapData = NULL;
    sim->previousMapDataLen = 0;
    sim->previousMapName[0] = '\0';
    sim->mapMd5Valid = FALSE;
    sim->mapMd5Hex[0] = '\0';
    memset(sim->mapMd5, 0, sizeof(sim->mapMd5));
    sim->sim.hiddenMines = hiddenMines;
    sim->sim.isServer = TRUE;
    sim->sim.isLocalTransport = TRUE;
    sim->sim.inStartFind = FALSE;
    sim->timeCreated = (uint32_t)time(NULL);
    sim->serverPort = 0;
    memset(sim->mapName, 0, MAP_STR_SIZE);
    memset(sim->lobbyPlayers, 0, sizeof(sim->lobbyPlayers));

    /* ── Layout A lobby state — initial defaults ─────────────────
     * teams[] zeroed by the memset above (in_use=0 → renders with
     * defaults). Same for botConfigs[] (difficulty=easy=0,
     * personality=normal=0). serverLocks defaults to 0 — bolod
     * --lock-* CLI flags set bits at server startup. */

    /* Layout A — guarantee at least two teams always exist so the
     * lobby UI never shows fewer than 2. teams[1] gets the host
     * by default (see player-join path); teams[2] gets the second
     * joiner. Marking them in_use here ensures the UI renders both
     * even before anyone joins. */
    sim->teams[1].in_use     = 1;
    sim->teams[1].color      = 0;  /* red */
    sim->teams[1].namingPool = 0;  /* classic */
    SDL_strlcpy(sim->teams[1].name, "Team 1", LOBBY_TEAM_NAME_LEN);
    sim->teams[2].in_use     = 1;
    sim->teams[2].color      = 1;  /* blue */
    sim->teams[2].namingPool = 0;
    SDL_strlcpy(sim->teams[2].name, "Team 2", LOBBY_TEAM_NAME_LEN);

    sim->openHost            = FALSE;
    sim->allowNewPlayers     = TRUE;   /* lobby starts open */
    sim->autoLockOnGameStart = FALSE;
    sim->savedAllowNewPlayers = TRUE;
    sim->ranked              = FALSE;
    sim->serverLocks         = 0;
    sim->maxPlayers          = MAX_TANKS;
    sim->maxSpectators       = 0;
    sim->specDelayTicks      = 0;
    sim->specRosterEnum      = NULL;
    sim->specRosterEnumCtx   = NULL;
    sim->worldPreLoaded      = TRUE;

    /* Mirror gameType + hiddenMines + time fields so the lobby change
     * path can detect locked-setting attempts and emit clean diffs.
     * aiPolicy is filled in by serverSimSetBotBrainPath / aiType setter
     * after init when the host configures bots. */
    sim->aiPolicy    = 0;
    sim->timeLimit   = (gameLen > 0);
    /* gameLen is in TICKS (50/sec); convert back to whole minutes for the
     * user-facing display. Cap to fit uint16. */
    {
        int32_t mins = sim->timeLimit ? (gameLen / (50 * 60)) : 30;
        if (mins < 1) mins = 1;
        if (mins > 0xFFFF) mins = 0xFFFF;
        sim->timeMinutes = (uint16_t)mins;
    }

    /* Set up server callbacks */
    sim->sim.callbacks.messageAdd = serverSimCbMessageAdd;
    sim->sim.callbacks.soundDist = serverSimCbSoundDist;
    sim->sim.callbacks.soundDistShoot = serverSimCbSoundDistShoot;
    sim->sim.callbacks.soundDistTankHit = serverSimCbSoundDistTankHit;
    sim->sim.callbacks.tankKill = serverSimCbTankKill;
    sim->sim.callbacks.centerTank = serverSimCbCenterTank;
    sim->sim.callbacks.consoleMessage = serverSimCbConsoleMessage;
    sim->sim.callbacks.mineVisible = serverSimCbMineVisible;
    sim->sim.callbacks.explosion = serverSimCbExplosion;
    sim->sim.callbacks.tkExplosion = serverSimCbTkExplosion;
    sim->sim.callbacks.shellDeath = serverSimCbShellDeath;
    sim->sim.callbacks.recordDamage = serverSimCbRecordDamage;
    sim->sim.callbacks.recordPlayerAction = serverSimCbRecordPlayerAction;
    sim->sim.callbacks.recordPillPickup = serverSimCbRecordPillPickup;
    sim->sim.callbacks.ctx = sim;

    for (count = 0; count < MAX_TANKS; count++) {
        sim->sim.tanks[count] = NULL;
        sim->sim.lgmen[count] = NULL;
        sim->inputQueueHead[count] = 0;
        sim->inputQueueTail[count] = 0;
        sim->playerConnected[count] = FALSE;
    }

    sim->sim.posHistoryPtr = sim->posHistory;
    sim->sim.lgmPosHistoryPtr = sim->lgmPosHistory;
    sim->sim.lagCompTicks = 0;
    memset(sim->sim.perPlayerCompTicks, 0, sizeof(sim->sim.perPlayerCompTicks));

    botManagerInitInSim(&sim->botMgr, sim, 0);

    gameTypeSet(&sim->sim.game, game);
    logCreate();

    mapCreate(&sim->sim.mp);
    pillsCreate(&sim->sim.pb);
    startsCreate(&sim->sim.ss);
    basesCreate(&sim->sim.bs);
    playersCreate(&sim->sim.plyrs, TRUE);
    sim->sim.shs = shellsCreate();
    explosionsCreate(&sim->sim.expl);
    rubbleCreate(&sim->sim.rbl);
    buildingCreate(&sim->sim.blds);
    grassCreate(&sim->sim.grs);
    swampCreate(&sim->sim.swp);
    floodCreate(&sim->sim.ff);
    tkExplosionCreate(&sim->sim.tankExplosions);
    minesCreate(&sim->sim.mns, hiddenMines);
    minesExpCreate(&sim->sim.minesExplosions);
    treeGrowCreate(&sim->sim);
    playersRejoinCreate();
    /* Init base timers (was in basesCreate, now lives in GameSim) */
    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            sim->sim.baseTimer[i] = 30000;
        }
        sim->sim.baseTimer[0] = BASE_TICKS_BETWEEN_REFUEL;
    }

    /* Publish the sim as live before any caller can arm an activeSim slot
     * for it.  Paired with serverSimUnregisterLive in serverSimDestroy,
     * which the failure paths of the three creators also route through. */
    serverSimRegisterLive(sim);
}

ServerSim *serverSimCreate(char *mapFileName, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen) {
    ServerSim *sim;

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSim create: map='%s' gameType=%d hiddenMines=%d startDelay=%d gameLen=%d",
        mapFileName ? mapFileName : "(null)",
        (int)game, (int)hiddenMines, (int)startDelay, (int)gameLen);

    sim = (ServerSim *)malloc(sizeof(ServerSim));
    if (sim == NULL) {
        return NULL;
    }
    serverSimInit(sim, game, hiddenMines, startDelay, gameLen);

    if (mapRead(mapFileName, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss) == FALSE) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSim create: mapRead failed for '%s'",
            mapFileName ? mapFileName : "(null)");
        serverSimDestroy(sim);
        return NULL;
    }

    /* Hash the canonical BMAPBOLO file so WBN can match it on register. */
    serverSimCacheMapMd5FromFile(sim, mapFileName);

    /* Store map name (basename without path or .map extension) for info packet responses */
    {
        const char *base = mapFileName;
        const char *p;
        for (p = mapFileName; *p; p++) {
            if (*p == '/' || *p == '\\') {
                base = p + 1;
            }
        }
        strncpy(sim->mapName, base, MAP_STR_SIZE - 1);
        sim->mapName[MAP_STR_SIZE - 1] = '\0';
        /* Strip .map extension if present */
        {
            size_t len = strlen(sim->mapName);
            if (len >= 4 && strcmp(sim->mapName + len - 4, ".map") == 0) {
                sim->mapName[len - 4] = '\0';
            }
        }
    }

    basesClearMines(&sim->sim);

    /* Cache the initial map state for between-round resets */
    {
        BYTE tempBuf[65536];
        int len = serverSimGetCompressedMap(sim, tempBuf);
        sim->cachedMapData = malloc(len);
        if (sim->cachedMapData != NULL) {
            memcpy(sim->cachedMapData, tempBuf, len);
            sim->cachedMapDataLen = len;
        }
    }

    sim->state = sim->lobbyEnabled ? serverStateLobby : serverStateRunning;
    return sim;
}

ServerSim *serverSimCreateCompressed(BYTE *buff, int buffLen, const char *mapName, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen) {
    ServerSim *sim = (ServerSim *)malloc(sizeof(ServerSim));
    if (sim == NULL) {
        return NULL;
    }
    serverSimInit(sim, game, hiddenMines, startDelay, gameLen);

    if (mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss, buff, buffLen) == FALSE) {
        serverSimDestroy(sim);
        return NULL;
    }

    if (mapName != NULL && mapName[0] != '\0') {
        strncpy(sim->mapName, mapName, MAP_STR_SIZE - 1);
        sim->mapName[MAP_STR_SIZE - 1] = '\0';
    }

    basesClearMines(&sim->sim);

    /* Cache the initial map state for between-round resets */
    {
        BYTE tempBuf[65536];
        int len = serverSimGetCompressedMap(sim, tempBuf);
        sim->cachedMapData = malloc(len);
        if (sim->cachedMapData != NULL) {
            memcpy(sim->cachedMapData, tempBuf, len);
            sim->cachedMapDataLen = len;
        }
    }

    sim->state = sim->lobbyEnabled ? serverStateLobby : serverStateRunning;
    return sim;
}

ServerSim *serverSimCreateRandomMap(const MapGenConfig *cfg,
                                    gameType game, bool hiddenMines,
                                    int32_t startDelay, int32_t gameLen) {
    ServerSim *sim;
    BYTE tempBuf[65536];
    int len;
    char seedStr[64];
    int x, y;

    sim = (ServerSim *)malloc(sizeof(ServerSim));
    if (sim == NULL) {
        return NULL;
    }
    serverSimInit(sim, game, hiddenMines, startDelay, gameLen);

    /* Clear map to DEEP_SEA */
    memset((*sim->sim.mp).mapItem, DEEP_SEA, sizeof((*sim->sim.mp).mapItem));

    /* Fill mine border (same as mapRead does) */
    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
            }
        }
    }

    /* Clear objects */
    sim->sim.pb->numPills = 0;
    sim->sim.bs->numBases = 0;
    sim->sim.ss->numStarts = 0;

    /* Generate the map */
    mapGenRun(sim->sim.mp, sim->sim.bs, sim->sim.pb, sim->sim.ss, cfg);

    /* Run generated objects through the same init path as file-loaded maps
     * (pillsSetPill / basesSetBase / startsSetStart) so game-logic fields
     * like coolDown, justStopped, etc. are set correctly. */
    {
        BYTE i;
        for (i = 0; i < sim->sim.pb->numPills; i++) {
            pillbox tmp = sim->sim.pb->item[i];
            pillsSetPill(&sim->sim.pb, &tmp, (BYTE)(i + 1));
        }
        for (i = 0; i < sim->sim.bs->numBases; i++) {
            base tmp = sim->sim.bs->item[i];
            basesSetBase(&sim->sim.bs, &tmp, (BYTE)(i + 1));
        }
        for (i = 0; i < sim->sim.ss->numStarts; i++) {
            start tmp = sim->sim.ss->item[i];
            startsSetStart(&sim->sim.ss, &tmp, (BYTE)(i + 1));
        }
    }

    basesClearMines(&sim->sim);

    /* Set map name to "rand_<seed>" */
    mapGenConfigToSeed(cfg, seedStr, sizeof(seedStr));
    snprintf(sim->mapName, MAP_STR_SIZE, "rand_%.30s", seedStr);

    /* Cache compressed map data for client distribution */
    len = serverSimGetCompressedMap(sim, tempBuf);
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData == NULL) {
        serverSimDestroy(sim);
        return NULL;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    sim->state = sim->lobbyEnabled ? serverStateLobby : serverStateRunning;
    return sim;
}

void serverSimDestroy(ServerSim *sim) {
    BYTE count;

    if (sim == NULL) {
        return;
    }

    WB_LOG_INFO(WB_LOG_CAT_SERVER, "serverSim destroy: state=%d", (int)sim->state);

    /* Signal the balance thread to stop and wait for it to finish */
    SDL_SetAtomicInt(&sim->balanceProposal.shutdownFlag, 1);
    while (sim->balanceProposal.requestInFlight) {
        SDL_Delay(10);
    }

    /* Tear down the per-sim BotManager before anything else.
     * botManagerRemoveBot reaches back into sim->plyrs and the
     * subscriber list, both of which the destroy steps below
     * dismantle. Idempotent: callers that still pair an explicit
     * botManagerDestroy(sim) / serverSimDestroyBots(sim) before
     * this destroy walk an already-empty bots[] on this call. */
    botManagerDestroy(sim);

    for (count = 0; count < MAX_TANKS; count++) {
        if (sim->sim.tanks[count] != NULL) {
            tankDestroy(&sim->sim, &sim->sim.tanks[count]);
        }
        if (sim->sim.lgmen[count] != NULL) {
            lgmDestroy(&sim->sim.lgmen[count]);
        }
    }

    shellsDestroy(&sim->sim.shs);
    mapDestroy(&sim->sim.mp);
    pillsDestroy(&sim->sim.pb);
    basesDestroy(&sim->sim.bs);
    startsDestroy(&sim->sim.ss);
    playersDestroy(&sim->sim.plyrs);
    explosionsDestroy(&sim->sim.expl);
    minesDestroy(&sim->sim.mns);
    floodDestroy(&sim->sim.ff);
    buildingDestroy(&sim->sim.blds);
    tkExplosionDestroy(&sim->sim.tankExplosions);
    minesExpDestroy(&sim->sim.minesExplosions);
    rubbleDestroy(&sim->sim.rbl);
    swampDestroy(&sim->sim.swp);
    grassDestroy(&sim->sim.grs);
    playersRejoinDestroy();
    logDestroy();

    sim->state = serverStateGameOver;

    /* Free the per-round attribution track buffer */
    if (sim->trackBuf != NULL) {
        free(sim->trackBuf);
        sim->trackBuf = NULL;
        sim->trackCap = 0;
        sim->trackLen = 0;
    }

    /* Free cached map data */
    if (sim->cachedMapData != NULL) {
        free(sim->cachedMapData);
        sim->cachedMapData = NULL;
        sim->cachedMapDataLen = 0;
    }
    if (sim->previousMapData != NULL) {
        free(sim->previousMapData);
        sim->previousMapData = NULL;
        sim->previousMapDataLen = 0;
    }

    /* Retire the sim before the free.  Clearing our own thread's slot is
     * only half of it — every other thread's slot still names this sim, and
     * dropping the registration is what makes serverSimGetActive() refuse
     * to hand those out. */
    serverSimUnregisterLive(sim);
    if (activeSim == sim) {
        activeSim = NULL;
    }

    free(sim);
}

static void serverSimLogTick(ServerSim *sim) {
    BYTE count;

    /* Run whenever a .wbv log is recording OR a spectator ring is registered:
       both consume the per-tick location/shell events and the logWriteTick tap.
       A normal (non-recording) server still has a ring, so gating on .wbv alone
       would leave the ring empty and a connecting spectator with no seed. */
    if (logIsRecording() == FALSE && logHasSpectatorRing() == FALSE) return;

    /* Tank + LGM positions */
    for (count = 0; count < MAX_TANKS; count++) {
        if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
            BYTE mx = tankGetMX(&sim->sim.tanks[count]);
            BYTE my = tankGetMY(&sim->sim.tanks[count]);
            BYTE px = tankGetPX(&sim->sim.tanks[count]);
            BYTE py = tankGetPY(&sim->sim.tanks[count]);
            BYTE dir = tankGetDir(&sim->sim.tanks[count]);
            BYTE onBoat = (BYTE)tankIsOnBoat(&sim->sim.tanks[count]);
            logAddEvent(log_PlayerLocation, count, mx, my,
                        utilPutNibble(px, py),
                        utilPutNibble(dir, onBoat), NULL);

            if (lgmIsOut(&sim->sim.lgmen[count])) {
                logAddEvent(log_LgmLocation,
                            utilPutNibble(count, lgmGetFrame(&sim->sim.lgmen[count])),
                            lgmGetMX(&sim->sim.lgmen[count]),
                            lgmGetMY(&sim->sim.lgmen[count]),
                            utilPutNibble(lgmGetPX(&sim->sim.lgmen[count]),
                                          lgmGetPY(&sim->sim.lgmen[count])),
                            0, NULL);
            }
        }
    }

    /* Shells / explosions / tank explosions */
    {
        screenBullets sb = screenBulletsCreate();
        int entries, i;
        BYTE mx, my, px, py, frame;

        shellsCalcScreenBullets(&sim->sim.shs, &sb, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);
        explosionsCalcScreenBullets(&sim->sim.expl, &sb, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);
        tkExplosionCalcScreenBullets(&sim->sim.tankExplosions, &sb, 0, MAP_ARRAY_LAST, 0, MAP_ARRAY_LAST);

        entries = screenBulletsGetNumEntries(&sb);
        for (i = 1; i <= entries; i++) {
            screenBulletsGetItem(&sb, i, &mx, &my, &px, &py, &frame);
            logAddEvent(log_Shell, mx, my, utilPutNibble(px, py), frame, 0, NULL);
        }
        screenBulletsDestroy(&sb);
    }

    /* Periodic snapshot */
    if ((sim->tick % FULL_SYNC_INTERVAL) == 0) {
        logWriteSnapshot(sim, TRUE);
    }

    /* First entry of the running round is where the round's log segment — and
     * so every clip time measured against it — begins. Latched here rather than
     * derived from startDelay: the hold at the top of simRunHalfStep advances
     * the sim without writing anything, and its counter is in half-steps while
     * the value it was given is in the legacy 20 ms units, so no arithmetic on
     * it gives the right answer. Whichever tick actually wrote first is the
     * answer by definition. */
    if (sim->roundLogStartTick == ROUND_LOG_START_UNSET &&
        sim->state == serverStateRunning) {
        sim->roundLogStartTick = sim->tick;
    }

    logWriteTick();
}

uint8_t serverSimComputeLagCompTicks(uint32_t simTick, uint32_t viewTick,
                                     uint16_t pingMs) {
    uint32_t ticks;
    if (viewTick != 0 && viewTick <= simTick) {
        /* Rewind the real view age. posHistory records once per game tick
         * (20ms), so two server ticks map to one history entry. viewTick is
         * client-supplied, but so is pingMs below, so the trust model is
         * unchanged; the clamp bounds any abuse. */
        ticks = (simTick - viewTick) / 2;
    } else {
        /* viewTick unknown (0) or ahead of the server (stale/garbage): keep
         * the ping-based estimate — snapshot trip out (ping/2) + interp buffer. */
        ticks = (((uint32_t)pingMs / 2) + INTERP_BUFFER_MS) / 20;
    }
    if (ticks > LAG_COMP_MAX_TICKS) ticks = LAG_COMP_MAX_TICKS;
    return (uint8_t)ticks;
}

/* Apply a single input to player `count`'s tank: gap-fill for any ticks
 * lost to packet loss, per-input parity selection (keys vs game arm),
 * lag compensation, fire/mine/build, and the lastProcessedInput advance.
 *
 * Called from two sites: the normal dequeue (a real input,
 * isSubstitute == FALSE) and the stall branch (a substitute synthesised
 * from the last held buttons, isSubstitute == TRUE). A substitute carries
 * no actions and must never invent a one-shot, so for it the pending-
 * harvest merge, the action marker, the fire path, the input-flag handling
 * (autoslow / gunsight) and the lag-comp computation are all skipped — it
 * leaves the last real input's flag state in place and a substituted game
 * tick ends with lagCompTicks == 0 because it cannot shoot. */
static void serverSimApplyOneInput(ServerSim *sim, BYTE count,
                                   const InputPacket *in, bool isSubstitute) {
    /* `local` is a macro (#define local static, brain.h), so name the
     * working copy `applied`. */
    InputPacket applied = *in;
    bool inputIsKeys = (applied.tick % 2) == 1;
    tankButton tb = translateInputToTankButton(applied.buttons);

    /* Fill in gap ticks lost to packet loss.  When a UDP packet
     * is dropped, the dequeued tick jumps ahead (e.g. 98 → 101).
     * The missing ticks must still run so the turn ramp (firstLeft/
     * firstRight) stays in sync with the client's prediction. */
    {
        uint32_t expected = sim->lastProcessedInput[count] + 1;
        uint32_t gap = applied.tick - expected;
        if (gap > 0 && gap < 8) {
            tankButton gapTb = translateInputToTankButton(sim->lastInputButtons[count]);
            uint32_t gt;
            for (gt = expected; gt < applied.tick; gt++) {
                bool gapIsKeys = (gt % 2) == 1;
                sim->statGapFillTicks[count]++;
#ifdef WB_NETDEBUG
                if (netdebugButtonTurns(gapTb)) {
                    sim->dbgExecTurnTicks[count]++;
                }
#endif
                if (gapIsKeys) {
                    BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
                    BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
                    tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, gapTb);
                } else {
                    sim->sim.lagCompTicks = 0;
                    tankUpdate(&sim->sim, &sim->sim.tanks[count], gapTb, FALSE, FALSE);
                }
            }
        }
    }

    /* Save buttons for stall continuity */
    sim->lastInputButtons[count] = applied.buttons;

    /* Flag handling is real-input only. A substitute carries flags = 0; the
     * old stall branch never ran these calls, so applying flags=0 would
     * force-disable autoslow each substituted tick and diverge from the
     * client's prediction. Skipping the block leaves the tank-side flag
     * state from the last real input in place — the old behavior. */
    if (!isSubstitute) {
        /* Apply autoslowdown state from client flags */
        tankSetAutoSlowdown(&sim->sim.tanks[count],
                            (applied.flags & INPUT_FLAG_AUTOSLOW) != 0);

        /* Apply gunsight adjustment (bits 2-3 of flags) */
        uint8_t gsAdj = (applied.flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
        if (gsAdj == 1) {
            tankGunsightIncrease(NULL, &sim->sim, &sim->sim.tanks[count]);
        } else if (gsAdj == 2) {
            tankGunsightDecrease(NULL, &sim->sim, &sim->sim.tanks[count]);
        }
    }

    sim->lastProcessedInput[count] = applied.tick;

#ifdef WB_NETDEBUG
    /* One increment per executed half-step whose button turns,
     * covering both the keys arm and the game arm below. */
    if (netdebugButtonTurns(tb)) {
        sim->dbgExecTurnTicks[count]++;
    }
#endif

    if (inputIsKeys) {
        /* Keys tick: turning only */
        BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
        BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
        tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, tb);
    } else {
        /* Game tick: full update (turning + accel + movement) */

        /* Fold in one-shot actions harvested off stall-dropped stale
         * entries (see the dequeue skip loop). Done here, in the game arm,
         * because mine/build are only acted on for game ticks — folding on
         * a keys tick would consume and silently drop a harvested mine.
         * Per-field with collision deferral so two separately commanded
         * one-shots never merge into one: if this input already carries the
         * same one-shot, leave the pending copy for a later game tick. A
         * substitute carries no actions and never receives pending state. */
        if (!isSubstitute) {
            if ((sim->pendingHarvestActions[count] & INPUT_ACTION_LAY_MINE) &&
                !(applied.actions & INPUT_ACTION_LAY_MINE)) {
                applied.actions |= INPUT_ACTION_LAY_MINE;
                sim->pendingHarvestActions[count] &= ~INPUT_ACTION_LAY_MINE;
            }
            if (sim->pendingHarvestBuildAction[count] != 0 &&
                applied.buildAction == 0) {
                applied.buildAction = sim->pendingHarvestBuildAction[count];
                applied.buildX      = sim->pendingHarvestBuildX[count];
                applied.buildY      = sim->pendingHarvestBuildY[count];
                sim->pendingHarvestBuildAction[count] = 0;
                sim->pendingHarvestBuildX[count] = 0;
                sim->pendingHarvestBuildY[count] = 0;
            }
        }

        bool shoot = (applied.actions & INPUT_ACTION_FIRE) != 0;
        if (isSubstitute) {
            /* A substitute cannot shoot, so it must leave 0 comp behind
             * (matching the old stall branch) rather than rewinding. */
            sim->sim.lagCompTicks = 0;
        } else {
            uint8_t compTicks = serverSimComputeLagCompTicks(
                sim->tick, applied.viewTick, sim->playerPing[count]);
            sim->sim.lagCompTicks = compTicks;
            sim->statLastRewindTicks[count] = compTicks;
        }
        /* Stamp the originating input tick so a shell created inside this
         * tankUpdate carries it (shellsAddItem reads sim->fireInputTick).
         * Reset to 0 immediately after so pill shells / later world systems
         * in this tick don't inherit a stale player tick. */
        sim->sim.fireInputTick = applied.tick;
        tankUpdate(&sim->sim, &sim->sim.tanks[count], tb, shoot, FALSE);
        sim->sim.fireInputTick = 0;

        /* Handle mine laying */
        if (applied.actions & INPUT_ACTION_LAY_MINE) {
            tankLayMine(&sim->sim, &sim->sim.tanks[count]);
#ifdef WB_NETDEBUG
            sim->dbgMineLays[count]++;
#endif
        }

        /* Handle LGM build requests.  buildAction is 1-based in
         * InputPacket (0=none, 1=BsTrees, 2=BsRoad, ...) but
         * lgmAddRequest expects 0-based enum values. */
        if (applied.buildAction != 0) {
            lgmAddRequest(&sim->sim, &sim->sim.lgmen[count],
                          &sim->sim.tanks[count],
                          applied.buildX,
                          applied.buildY,
                          applied.buildAction - 1);
        }
    }

    /* Action marker: a real apply that executed a one-shot (fire, mine,
     * or build, all game-arm only) advances lastActionAppliedTick so a
     * redundant duplicate of this same tick is later recognised as
     * already-executed and not harvested. Substitutes never execute a
     * one-shot, so they never touch the marker. */
    if (!isSubstitute && !inputIsKeys) {
        bool executedAction = (applied.actions & INPUT_ACTION_FIRE) ||
                              (applied.actions & INPUT_ACTION_LAY_MINE) ||
                              (applied.buildAction != 0);
        if (executedAction) {
            sim->lastActionAppliedTick[count] = applied.tick;
        }
    }
}

/* One half-step of the sim: dequeues one input per player and runs
 * world systems on game ticks (sim->tick % 2 == 0).  Two consecutive
 * half-steps form one 20ms frame and match the client's 100Hz keys/
 * game alternation.  Event buffers (sim->events, sim->mapEvents) are
 * NOT cleared here — that happens once at the top of serverSimTick so
 * events from both half-steps accumulate naturally into one frame's
 * worth of state for downstream consumers (UDP drain, in-process
 * snapshot poll). */
/* Backlog of fresh (not-yet-processed) input for `count`, measured as
 * newestQueuedTick - lastProcessedInput: how many ticks behind the newest
 * queued input the server is. Immune to the redundancy duplicates that
 * inflate raw head-tail depth — a tick resent in N packets sits in the
 * queue N times but contributes once here. Returns 0 when nothing fresh
 * is queued. */
static uint32_t serverSimFreshBacklog(ServerSim *sim, BYTE count) {
    uint32_t lpi = sim->lastProcessedInput[count];
    uint32_t newest = lpi;
    uint8_t i = sim->inputQueueTail[count];
    while (i != sim->inputQueueHead[count]) {
        uint32_t t = sim->inputQueue[count][i & (SERVER_INPUT_QUEUE_SIZE - 1)].tick;
        if (t > newest) newest = t;
        i++;
    }
    return newest - lpi;
}

/* Pop entries for `count` until a fresh input (tick > lastProcessedInput)
 * or the queue empties. Stale entries are dropped with one-shot harvest.
 * Returns TRUE with *out filled on fresh; FALSE on empty. */
static bool serverSimDequeueFresh(ServerSim *sim, BYTE count, InputPacket *out) {
    while (sim->inputQueueHead[count] != sim->inputQueueTail[count]) {
        uint8_t tail = sim->inputQueueTail[count] & (SERVER_INPUT_QUEUE_SIZE - 1);
        *out = sim->inputQueue[count][tail];
        sim->inputQueueTail[count]++;
        if (out->tick > sim->lastProcessedInput[count]) {
            return TRUE;
        }
        /* Stale entry (tick <= lastProcessedInput): its movement was
         * already covered, either by a real apply or by a stall
         * substitute, so dropping it here is correct. But a one-shot
         * action commanded on a stall-substituted tick was NEVER
         * executed (the substitute carries no actions), so it must be
         * harvested and carried onto the next real input — exactly
         * once. The discriminator is lastActionAppliedTick: an entry
         * with tick > lastActionAppliedTick was never executed
         * (harvest it); an entry with tick <= lastActionAppliedTick is
         * an ordinary redundant duplicate of an already-applied action
         * (ignore it). Fire is deliberately excluded — it is
         * level-triggered and reload-gated, so re-issuing it carries no
         * benefit and only adds state. Mine + build only. */
        if (out->tick > sim->lastActionAppliedTick[count]) {
            if (out->actions & INPUT_ACTION_LAY_MINE) {
                sim->pendingHarvestActions[count] |= INPUT_ACTION_LAY_MINE;
            }
            if (out->buildAction != 0) {
                /* A later harvested build overwrites an earlier pending
                 * one — newest commanded build intent wins. */
                sim->pendingHarvestBuildAction[count] = out->buildAction;
                sim->pendingHarvestBuildX[count] = out->buildX;
                sim->pendingHarvestBuildY[count] = out->buildY;
            }
            sim->lastActionAppliedTick[count] = out->tick;
        }
        sim->statDroppedStaleInputs[count]++;  /* stale/duplicate entry discarded */
        if (out->buttons != sim->lastInputButtons[count] &&
            out->tick + 8 > sim->lastProcessedInput[count]) {
            sim->statDroppedEdge[count]++;
        }
    }
    return FALSE;
}

static void simRunHalfStep(ServerSim *sim) {
    BYTE count;
    bool isKeysTick;
    BYTE numTanks;
    tank tanksArray[MAX_TANKS];
    lgm *lgmPtrs[MAX_TANKS];
    InputPacket currentInputs[MAX_TANKS];
    bool hasInput[MAX_TANKS];

    /* Arm this thread's active-sim slot BEFORE the state gate, not after
     * it.  The non-running branches below are not inert: they run
     * serverSimGameVoteTick and logWriteTick, and logWriteTick's pre-tick
     * hook is where the dedicated-log writer drains work queued from
     * another thread (handleLobbyEnter), which reaches back for the sim
     * through serverSimGetActive().  With the assignment after the switch,
     * a lobby tick ran that drain against whatever sim this thread last
     * touched — on the SDL timer thread, a sim from an earlier server in
     * the same process. */
    activeSim = sim;

    /* In-game vote driver — runs in every state so timeouts, heartbeats,
     * and the post-pass 3/2/1 countdown keep firing in SP, host, and
     * dedicated builds alike (independent of transport tick). */
    serverSimGameVoteTick(sim, (uint64_t)SDL_GetTicks());

    /* State machine gate — only run simulation in running state */
    switch (sim->state) {
    case serverStateLobby:
        /* No simulation but still advance tick for periodic lobby broadcasts. */
        sim->tick++;
        logWriteTick();
        return;
    case serverStateCountdown:
        sim->countdownTicks--;
        if (sim->countdownTicks <= 0) {
            serverSimStartGame(sim);
        }
        logWriteTick();
        return;
    case serverStateGameOver:
        sim->countdownTicks--;
        if (sim->countdownTicks <= 0) {
            serverSimReturnToLobby(sim);
        }
        return;
    case serverStateRunning:
        break; /* Fall through to existing simulation code */
    }

    playersRejoinUpdate();

    /* Install map change callback to emit EVENT_MAP_CHANGE during tick */
    mapSetChangeCallback(simMapChangeCallback);

    if (sim->startDelay > 0) {
        sim->startDelay--;
        sim->tick++;
        mapSetChangeCallback(NULL);
        return;
    }

    if (sim->gameLength > 0) {
        sim->gameLength--;
        if (sim->gameLength == 0) {
            mapSetChangeCallback(NULL);
            serverSimConsoleMessage("Game time limit reached.");
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    if (sim->tickLimit > 0) {
        sim->ticksRun++;
        if (sim->ticksRun >= sim->tickLimit) {
            char ticksMsg[64];
            snprintf(ticksMsg, sizeof(ticksMsg),
                     "Reached tick limit (%d). Exiting.",
                     (int)sim->tickLimit);
            sim->tickLimit = 0;
            mapSetChangeCallback(NULL);
            serverSimConsoleMessage(ticksMsg);
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    if (sim->gameTickLimit > 0) {
        sim->gameTicksRun++;
        if (sim->gameTicksRun >= sim->gameTickLimit) {
            char gameTicksMsg[64];
            snprintf(gameTicksMsg, sizeof(gameTicksMsg),
                     "Game tick limit reached (%d). Ending game.",
                     (int)sim->gameTickLimit);
            sim->gameTickLimit = 0;
            sim->gameTicksRun = 0;
            mapSetChangeCallback(NULL);
            serverSimConsoleMessage(gameTicksMsg);
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    /* End the round once the last human leaves, so the server drops back
     * to the lobby instead of looping a bot-only game forever (which it
     * otherwise does — the empty/auto-close checks count bots via
     * serverSimGetNumPlayers). Gated on roundHadHuman so a game that
     * legitimately started with only bots (all bots ready) isn't ended
     * the instant it starts, which would loop start<->gameover. Only for
     * lobby-enabled servers; a no-lobby game-over means shutdown, which a
     * transient human dropout shouldn't trigger. Suppress the win message
     * — nobody won, everyone left. Routes through the normal GAME_OVER ->
     * countdown -> returnToLobby flow (WBN swap, log flush, republish). */
    if (sim->lobbyEnabled) {
        if (serverSimGetNumHumans(sim) > 0) {
            sim->roundHadHuman = true;
        } else if (sim->roundHadHuman) {
            mapSetChangeCallback(NULL);
            sim->returnToLobbyReason = RETURN_REASON_ABANDONED;
            serverSimConsoleMessage("No human players remaining. Returning to lobby.");
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    /* Server tick parity controls world systems only.
     * Per-player keys vs game is determined by the INPUT's tick parity,
     * so client/server parity is always aligned regardless of when
     * the client joined. */
    isKeysTick = (sim->tick % 2) == 1;

    /* Dequeue one input per player for this tick, skipping duplicates.
     * Redundant UDP packets can queue the same tick number multiple times
     * because serverHandleInput's dedup check uses lastProcessedInput which
     * isn't updated until dequeue time.  Drain any stale/duplicate entries
     * so each tick number is only processed once. */
    for (count = 0; count < MAX_TANKS; count++) {
        hasInput[count] = FALSE;
        if (!sim->playerConnected[count]) {
            continue;
        }

        /* Calculate queue depth (inputs available) */
        uint8_t queueDepth = sim->inputQueueHead[count] - sim->inputQueueTail[count];

        /* Jitter buffer gate: wait until we have enough inputs buffered.
         * Once the buffer has filled initially, keep processing even if
         * depth drops to 1 (drain rather than stall). Only re-enter
         * buffering mode if the queue empties completely. */
        if (!sim->inputBufferFilled[count]) {
            if (queueDepth < sim->jitterTarget[count]) {
                continue;  /* Still filling to adaptive target */
            }
            sim->inputBufferFilled[count] = 1;
        }

        /* Dequeue one input, skipping duplicates/stale */
        hasInput[count] = serverSimDequeueFresh(sim, count, &currentInputs[count]);

        /* Adaptive jitter buffer — track stalls and adjust target depth */
        if (sim->inputBufferFilled[count]) {
            if (!hasInput[count]) {
                /* Queue ran dry — we're consuming faster than inputs arrive */
                sim->jitterStallCount[count]++;
                sim->jitterStableTicks[count] = 0;
                if (sim->jitterStallCount[count] >= JITTER_GROW_THRESHOLD &&
                    sim->jitterTarget[count] < JITTER_BUFFER_MAX) {
                    sim->jitterTarget[count]++;
                    sim->jitterStallCount[count] = 0;
                }
            } else {
                sim->jitterStallCount[count] = 0;
                sim->jitterStableTicks[count]++;
                if (sim->jitterStableTicks[count] >= JITTER_SHRINK_INTERVAL &&
                    sim->jitterTarget[count] > JITTER_BUFFER_MIN) {
                    sim->jitterTarget[count]--;
                    sim->jitterStableTicks[count] = 0;
                    /* A long calm stretch also forgets recent drains, so the
                     * buffer is free to settle back toward MIN. */
                    sim->jitterStarveCount[count] = 0;
                }
            }
        }

        /* If queue drained completely, re-enter buffering mode. The drain
         * itself is the reliable too-shallow signal: under jitter the queue
         * empties faster than the grow path above can react, because once we
         * re-enter filling mode a later dry sub-tick takes the `continue`
         * above and never reaches that grow logic. Count drains separately
         * and deepen the buffer off them so jitter actually grows the target
         * instead of pinning it at the default. */
        if (sim->inputQueueHead[count] == sim->inputQueueTail[count] && !hasInput[count]) {
            if (sim->inputBufferFilled[count]) {
                sim->jitterStarveCount[count]++;
                if (sim->jitterStarveCount[count] >= JITTER_STARVE_GROW_THRESHOLD &&
                    sim->jitterTarget[count] < JITTER_BUFFER_MAX) {
                    sim->jitterTarget[count]++;
                    sim->jitterStarveCount[count] = 0;
                }
            }
            sim->inputBufferFilled[count] = 0;
        }
    }

    /* Per-player tank processing: use each input's tick parity */
    for (count = 0; count < MAX_TANKS; count++) {
        if (!sim->playerConnected[count] || sim->sim.tanks[count] == NULL) {
            continue;
        }
        sim->currentTickPlayer = count;
        if (hasInput[count]) {
            sim->inputDryTicks[count] = 0;
            serverSimApplyOneInput(sim, count, &currentInputs[count], FALSE);

            /* Backlog catch-up: bleed a standing queue at +1 input per sub-tick
             * (hard cap 2 applies total) so a jitter-spike backlog drains in ~1s
             * instead of ratcheting input latency for the session. Gate on the
             * fresh backlog (newest queued tick minus lastProcessedInput), not
             * raw head-tail depth: input redundancy resends each tick in several
             * packets, so the same unprocessed tick sits in the queue multiple
             * times and inflates raw depth — fresh backlog counts it once.
             * jitterTarget + 1 so steady-state never triggers it. */
            if (serverSimFreshBacklog(sim, count) > (uint32_t)(sim->jitterTarget[count] + 1)) {
                InputPacket extra;
                if (serverSimDequeueFresh(sim, count, &extra)) {
                    serverSimApplyOneInput(sim, count, &extra, FALSE);
                    sim->statCatchupTicks[count]++;
                }
            }
        } else {
            /* No fresh input this tick. Count every consecutive dry
             * half-step (including those where loop 1 left hasInput FALSE
             * mid-rebuffer) so the stall-advance gate sees the true dry
             * run, not just the jitter-buffer's stall count. */
            sim->inputDryTicks[count]++;
            tankButton stallTb = translateInputToTankButton(sim->lastInputButtons[count]);

            /* "Established at least once" is read off lastProcessedInput,
             * not the live inputBufferFilled flag: the dequeue loop above
             * clears inputBufferFilled on the very drain that produces this
             * stall (it re-enters buffering mode whenever the queue empties),
             * so by the time we get here it is already 0 on every genuine
             * stall — which is why the old statStallTicks gate on it was
             * dead. lastProcessedInput > 0 is the persistent signal — a real
             * input can only have advanced it after inputBufferFilled was
             * set, so it means the stream filled at least once. It gates both
             * the real-stall counter and the stall-advance: dead/loading
             * players that never streamed have it at 0, so they are not
             * counted as stalls and keep the idle path below. */
            if (sim->lastProcessedInput[count] > 0) {
                sim->statStallTicks[count]++;
            }

            /* Stall-advance only on a genuine multi-tick dry spell. A dry
             * run at/below STALL_ADVANCE_DRY_TICKS is routine send-burst
             * cadence ripple (the client batches 2 inputs/packet but the
             * server consumes 1 per half-step, so the queue drains to empty
             * for a half-step or two between packets); advancing there would
             * consume the tick and drop the in-flight real input as stale,
             * making the client reconcile constantly. Below the threshold we
             * fall through to repeat-and-wait so the late input still applies
             * at its true tick. Only an established stream past the threshold
             * is treated as genuine loss and stall-advances. */
            if (sim->lastProcessedInput[count] > 0 &&
                sim->inputDryTicks[count] > STALL_ADVANCE_DRY_TICKS) {
                /* Established stream, genuine loss: stall-advance. A
                 * substituted tick is a *processed* tick — synthesise an
                 * input from the last held buttons at the next tick number
                 * and run it through the canonical apply, which advances
                 * lastProcessedInput past it. The real (late) input for this
                 * tick then arrives stale and its movement is dropped rather
                 * than executing the held turn a second time (the overshoot
                 * fix). The synth carries no actions, so it can never
                 * fire/lay/build, and isSubstitute suppresses the
                 * pending-harvest merge so a harvested one-shot waits for a
                 * real input. Parity comes from subTick (the apply body keys
                 * on its own tick), and the apply body counts the
                 * WB_NETDEBUG turn tick — so this branch must not count it
                 * again. */
                InputPacket synth;
                memset(&synth, 0, sizeof(synth));
                synth.tick      = sim->lastProcessedInput[count] + 1;
                synth.playerNum = count;
                synth.buttons   = sim->lastInputButtons[count];
                serverSimApplyOneInput(sim, count, &synth, TRUE);
            } else {
                /* Brief cadence trough on an established stream, or a player
                 * not yet established (dead/loading/never-streamed): keep
                 * today's idle simulation without consuming a tick. Repeat
                 * the last held buttons so the turn ramp (firstLeft/
                 * firstRight) doesn't reset and pull the angle back. Because
                 * lastProcessedInput is not advanced here, the in-flight real
                 * input for this tick still applies fresh when it arrives. */
#ifdef WB_NETDEBUG
                if (netdebugButtonTurns(stallTb)) {
                    sim->dbgExecTurnTicks[count]++;
                }
#endif
                sim->sim.lagCompTicks = 0;
                if (isKeysTick) {
                    BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
                    BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
                    tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, stallTb);
                } else {
                    tankUpdate(&sim->sim, &sim->sim.tanks[count], stallTb, FALSE, FALSE);
                }
            }
        }
    }

    /* Reset lagCompTicks after per-player loop so pill-fired shells get 0 */
    sim->sim.lagCompTicks = 0;

    /* World systems: run on even server ticks (game ticks) */
    if (!isKeysTick) {
        /* Update LGMs every game tick — server-authoritative state must
         * not depend on per-player input arrival.  Without this, parachute
         * descent, walking back to tank, and build progress freeze whenever
         * a player isn't sending fresh inputs (notably while dead). */
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.lgmen[count] != NULL
                && sim->sim.tanks[count] != NULL) {
                lgmUpdate(&sim->sim, &sim->sim.lgmen[count], &sim->sim.tanks[count]);
            }
        }

        /* Record tank positions for lag compensation history */
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
                posHistoryRecord(&sim->posHistory[count],
                                 (*sim->sim.tanks[count]).x,
                                 (*sim->sim.tanks[count]).y,
                                 (*sim->sim.tanks[count]).armour <= TANK_FULL_ARMOUR);
            }
        }

        /* Record LGM positions for lag compensation history */
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.lgmen[count] != NULL) {
                lgm lgman = sim->sim.lgmen[count];
                bool lgmAlive = !lgman->isDead && !lgman->inTank;
                posHistoryRecord(&sim->lgmPosHistory[count],
                                 lgman->x, lgman->y, lgmAlive);
            }
        }

        /* Update pillboxes — pass all tanks so pills can target closest enemy */
        pillsUpdate(&sim->sim, sim->sim.tanks, sim->playerConnected, MAX_TANKS);

        /* Base stock restocking */
        basesUpdate(&sim->sim, NULL);

        /* Server-authoritative base refueling */
        {
            BYTE numBases = basesGetNumBases(&sim->sim.bs);
            bool baseOccupied[MAX_BASES];
            BYTE b;

            memset(baseOccupied, FALSE, sizeof(baseOccupied));

            for (count = 0; count < MAX_TANKS; count++) {
                WORLD twx, twy;
                BYTE tx, ty, baseNum;
                if (!sim->playerConnected[count] || sim->sim.tanks[count] == NULL) {
                    continue;
                }
                if (tankGetArmour(&sim->sim.tanks[count]) > TANK_FULL_ARMOUR) {
                    continue;
                }
                tankGetWorld(&sim->sim.tanks[count], &twx, &twy);
                tx = (BYTE)(twx >> TANK_SHIFT_MAPSIZE);
                ty = (BYTE)(twy >> TANK_SHIFT_MAPSIZE);
                baseNum = basesGetBaseNum(&sim->sim.bs, tx, ty);
                if (baseNum != BASE_NOT_FOUND) {
                    baseOccupied[baseNum - 1] = TRUE;
                    if ((*sim->sim.bs).item[baseNum - 1].justStopped == FALSE) {
                        basesRefueling(&sim->sim, &sim->sim.tanks[count], baseNum);
                    } else {
                        (*sim->sim.bs).item[baseNum - 1].justStopped = FALSE;
                        (*sim->sim.bs).item[baseNum - 1].refuelTime = basesHalfTickCalulator(BASES_HALFTICK_TYPE_ARMOUR);
                    }
                }
            }

            for (b = 0; b < numBases; b++) {
                if (!baseOccupied[b]) {
                    (*sim->sim.bs).item[b].justStopped = TRUE;
                }
            }
        }

        /* Precompute per-player compensation ticks for pill shell rewind */
        {
            BYTE c;
            for (c = 0; c < MAX_TANKS; c++) {
                uint16_t pingMs = sim->playerPing[c];
                uint16_t delayMs = (pingMs / 2) + INTERP_BUFFER_MS;
                uint8_t ticks = (uint8_t)(delayMs / 20);
                if (ticks > LAG_COMP_MAX_TICKS) ticks = LAG_COMP_MAX_TICKS;
                sim->sim.perPlayerCompTicks[c] = ticks;
            }
        }

        /* Enforce high-ping limits */
        transportUdpServerEnforcePing(sim);

        /* Build arrays for multi-tank subsystem updates.
         *
         * Snapshotted here — as late as possible, immediately before the
         * world-update stage that consumes them — and NOT earlier in the
         * half-step. tanksArray holds tank pointers by value and lgmPtrs holds
         * addresses of sim->sim.lgmen[] slots, so anything that removes a
         * player between this loop and the last consumer below leaves the
         * arrays pointing at freed objects (dangling tanks) or NULLed slots
         * (lgmen), with numTanks still counting the departed slot. That is
         * exactly what a mid-half-step ping kick used to do. Keep any code
         * that can call serverSimRemovePlayer above this point. */
        numTanks = 0;
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
                tanksArray[numTanks] = sim->sim.tanks[count];
                lgmPtrs[numTanks] = &sim->sim.lgmen[count];
                numTanks++;
            }
        }

        /* Update world systems */
        /* tanksArray, NOT &sim->sim.tanks[0]: lgmPtrs is compacted over the
         * connected players, so the tank array must be compacted the same way
         * or the two index spaces diverge as soon as the occupied slots are
         * non-contiguous (anyone leaving mid-game). tkExplosionUpdate pairs
         * lgms[i] with tanks[i] to tell a killed lgm which tank to walk back
         * to, so a mismatch sent the man to another player's tank — or, when
         * the raw slot was empty, to a NULL tank and thus back to where he
         * died. shellsUpdate and minesExpUpdate below already take the
         * compacted array. */
        tkExplosionUpdate(&sim->sim, lgmPtrs, numTanks, tanksArray, &sim->sim.ss);
        shellsUpdate(&sim->sim, tanksArray, numTanks, lgmPtrs, &sim->sim.ss);
        {
            shells q = sim->sim.shs;
            while (q != NULL) {
                q->packSent = TRUE;
                q = q->next;
            }
        }
        minesExpUpdate(&sim->sim, lgmPtrs, numTanks, tanksArray, &sim->sim.ss);
        explosionsUpdate(&sim->sim.expl);
        floodUpdate(&sim->sim);
        for (count = 0; count < numTanks; count++) {
            treeGrowUpdate(&sim->sim);
        }
    }

    /* Clear map change callback */
    mapSetChangeCallback(NULL);

    /* Diff pills and emit update events for any that changed */
    {
        PillSnapshot currentPills[MAX_SNAPSHOT_PILLS];
        int np = serverSimGetPills(sim, currentPills, MAX_SNAPSHOT_PILLS);
        int p;
        for (p = 0; p < np; p++) {
            if (memcmp(&currentPills[p], &sim->prevPills[p], sizeof(PillSnapshot)) != 0) {
                GameEvent ev;
                ev.type = EVENT_PILL_UPDATE;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)p;
                ev.data[1] = currentPills[p].x;
                ev.data[2] = currentPills[p].y;
                ev.data[3] = currentPills[p].owner;
                ev.data[4] = currentPills[p].armourInTank;
                serverSimAddEvent(sim, &ev);
            }
        }
        memcpy(sim->prevPills, currentPills, np * sizeof(PillSnapshot));
        sim->prevPillCount = (uint8_t)np;
    }

    /* Diff bases and emit update events for any that changed */
    {
        BaseSnapshot currentBases[MAX_SNAPSHOT_BASES];
        int nb = serverSimGetBases(sim, currentBases, MAX_SNAPSHOT_BASES);
        int b;
        for (b = 0; b < nb; b++) {
            /* Owner change: reliable, broadcast — everyone sees base colour. */
            if (currentBases[b].owner != sim->prevBases[b].owner) {
                GameEvent ev;
                ev.type = EVENT_BASE_UPDATE;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)b;
                ev.data[1] = currentBases[b].owner;
                serverSimAddEvent(sim, &ev);
            }
            /* Stock change: best-effort, culled per recipient to their closest base. */
            if (currentBases[b].armour != sim->prevBases[b].armour ||
                currentBases[b].shells != sim->prevBases[b].shells ||
                currentBases[b].mines  != sim->prevBases[b].mines) {
                GameEvent ev;
                ev.type = EVENT_BASE_STOCK;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)b;
                ev.data[1] = currentBases[b].armour;
                ev.data[2] = currentBases[b].shells;
                ev.data[3] = currentBases[b].mines;
                serverSimAddEvent(sim, &ev);
            }
        }
        memcpy(sim->prevBases, currentBases, nb * sizeof(BaseSnapshot));
        sim->prevBaseCount = (uint8_t)nb;
    }

    /* All-bases win. Every base held by one alliance with none of them dead
     * (armour > MIN_ARMOUR_CAPTURE — the same test basesGetStatusNum uses to
     * draw the X) IS the win condition, so it ends the round on the spot.
     *
     * There is deliberately no grace period layered on top. The grace period
     * is already built into the condition: a base shelled to 0 stays dead,
     * and therefore keeps the sweep false, for the whole time it takes to
     * regenerate past MIN_ARMOUR_CAPTURE. That is the losing side's window to
     * retake it. A second countdown on top only bought the right to announce
     * a win and then retract it.
     *
     * A no-lobby round has nowhere to return to — quitOnWin ends it the same
     * way and the process shuts down.
     *
     * A return-to-lobby countdown already running (a passed vote, a
     * surrender) is left alone: those are irrevocable decisions and own the
     * reason the returning lobby is given. The sweep must not relabel a
     * surrender's win credit on its way out. */
    if (sim->lobbyEnabled) {
        if (sim->returnToLobbyTicks == 0 && serverSimCheckGameWin(sim, FALSE)) {
            char buf[256];
            char name[256];
            BYTE winner = serverSimWinningOwner(sim);
            playersGetPlayerName(&sim->sim.plyrs, winner, name, sizeof(name),
                                 TRUE);
            snprintf(buf, sizeof(buf),
                     "*** %s and their allies control every base. ***", name);
            publishServerMessage(sim, buf);
            serverSimConsoleMessage(buf);
            /* Set before entering game over: serverSimResolveGameOver
             * switches on it to name the winner in the returning lobby and
             * credit the WinBolo.net win events. */
            sim->returnToLobbyReason = RETURN_REASON_BASE_WIN;
            mapSetChangeCallback(NULL);
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    } else if (sim->quitOnWin && serverSimCheckGameWin(sim, TRUE)) {
        mapSetChangeCallback(NULL);
        serverSimConsoleMessage("Game won!");
        serverSimEnterGameOver(sim);
        sim->tick++;
        return;
    }

    /* Forced return-to-lobby countdown (e.g. from a vote pass). Game
     * keeps running normally — players can move, shoot, etc. — and
     * each game tick this decrements. At 0 we transition to
     * gameOver, which is what triggers the existing lifecycle path
     * (broadcastGameOver + countdownTicks hold + returnToLobby). */
    if (sim->returnToLobbyTicks > 0) {
        sim->returnToLobbyTicks--;
        if (sim->returnToLobbyTicks == 0) {
            /* returnToLobbyReason, set when the countdown was armed, tells
             * serverSimResolveGameOver what the returning lobby is told. */
            serverSimEnterGameOver(sim);
            sim->tick++;
            return;
        }
    }

    /* The legacy server ticked every 20ms (SERVER_TICK_LENGTH) and wrote
     * one log entry per tick.  Our sim ticks every 10ms alternating
     * keys/game.  Only log on game ticks (every 20ms) to match the
     * legacy rate — the log viewer consumes one entry per 20ms. */
    if (!isKeysTick) {
        serverSimLogTick(sim);
    }

    /* Per-second input-pipeline summary: one [netstat] line per connected
     * player, then reset that player's window counters. 100 sub-ticks =
     * 1 second. q and jt are live gauges read now; rewind is a gauge too
     * (most recent value, not reset). The rest accumulated over the window. */
    if ((sim->tick % 100) == 0) {
        for (count = 0; count < MAX_TANKS; count++) {
            if (!sim->playerConnected[count]) {
                continue;
            }
            {
                uint8_t qd = (sim->inputQueueHead[count] - sim->inputQueueTail[count])
                             & (SERVER_INPUT_QUEUE_SIZE - 1);
                mpDiagLog("[netstat] p%d q=%u jt=%u stall=%u gap=%u stale=%u dropEdge=%u catchup=%u rewind=%u",
                          count, qd, sim->jitterTarget[count],
                          sim->statStallTicks[count], sim->statGapFillTicks[count],
                          sim->statDroppedStaleInputs[count], sim->statDroppedEdge[count],
                          sim->statCatchupTicks[count],
                          sim->statLastRewindTicks[count]);
            }
            sim->statStallTicks[count] = 0;
            sim->statGapFillTicks[count] = 0;
            sim->statDroppedStaleInputs[count] = 0;
            sim->statDroppedEdge[count] = 0;
            sim->statCatchupTicks[count] = 0;
        }
    }

    sim->tick++;
}

/* Advance the sim by one 20ms frame.  In the running state this runs
 * two half-steps (the keys/game alternation that matches the client's
 * 100Hz input rate), with event buffers cleared once at the top so
 * events from both half-steps land in the same frame's worth of state
 * for downstream consumers.  In non-running states (lobby / countdown
 * / gameover) only one half-step runs, preserving the legacy state-
 * machine cadence — countdown durations, lobby refresh intervals, and
 * gameover return-to-lobby timing all stay calibrated against the
 * one-half-step-per-frame rate they were tuned for. */
void serverSimTick(ServerSim *sim) {
    if (sim->state == serverStateRunning) {
        sim->eventCount = 0;
        sim->mapEventCount = 0;
        simRunHalfStep(sim);
        simRunHalfStep(sim);
    } else {
        simRunHalfStep(sim);
    }
}

void serverSimApplyInput(ServerSim *sim, const InputPacket *input) {
    BYTE p = input->playerNum;
    uint8_t head, next;
    InputPacket sanitized;
    if (p >= MAX_TANKS || !sim->playerConnected[p]) {
        return;
    }

    /* Sanitize input fields before queuing */
    sanitized = *input;
    sanitized.buttons &= (INPUT_BTN_ACCEL | INPUT_BTN_DECEL | INPUT_BTN_LEFT | INPUT_BTN_RIGHT);
    sanitized.actions &= (INPUT_ACTION_FIRE | INPUT_ACTION_LAY_MINE);
    /* buildAction is 1-based (0=none, 1=BsTrees..5=BsMine); zero out if invalid */
    if (sanitized.buildAction > 5) {
        sanitized.buildAction = 0;
    }
    /* flags: bit 0 = autoslow, bits 2-3 = gunsight adj (0=none, 1=increase, 2=decrease) */
    {
        uint8_t gsAdj = (sanitized.flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
        if (gsAdj > 2) {
            sanitized.flags &= ~INPUT_FLAG_GUNSIGHT_MASK;
        }
        /* Mask off any undefined bits (keep only autoslow + gunsight) */
        sanitized.flags &= (INPUT_FLAG_AUTOSLOW | INPUT_FLAG_GUNSIGHT_MASK);
    }

    head = sim->inputQueueHead[p];
    next = (head + 1) & (SERVER_INPUT_QUEUE_SIZE - 1);
    /* Drop input if queue is full (shouldn't happen in practice) */
    if (next == (sim->inputQueueTail[p] & (SERVER_INPUT_QUEUE_SIZE - 1))) {
        return;
    }
    sim->inputQueue[p][head & (SERVER_INPUT_QUEUE_SIZE - 1)] = sanitized;
    sim->inputQueueHead[p] = head + 1;
    sim->playerPing[p] = transportUdpServerGetClientPing(p);
}

void addPlayerInternal(ServerSim *sim, BYTE playerNum, const char *playerName,
                       const char *country, bool wantRejoin) {
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "addPlayer slot=%u name='%s' wantRejoin=%d state=%d",
        (unsigned)playerNum,
        playerName ? playerName : "(null)",
        (int)wantRejoin,
        (int)sim->state);
    if (playerNum >= MAX_TANKS) {
        WB_LOG_WARN(WB_LOG_CAT_SERVER,
            "addPlayer rejected: slot=%u >= MAX_TANKS=%d",
            (unsigned)playerNum, (int)MAX_TANKS);
        return;
    }

    /* Clean up any leftover state from a previous player in this slot */
    if (sim->sim.tanks[playerNum] != NULL) {
        tankDestroy(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.tanks[playerNum] = NULL;
    }
    if (sim->sim.lgmen[playerNum] != NULL) {
        lgmDestroy(&sim->sim.lgmen[playerNum]);
        sim->sim.lgmen[playerNum] = NULL;
    }
    sim->inputQueueHead[playerNum] = 0;
    sim->inputQueueTail[playerNum] = 0;
    sim->lastProcessedInput[playerNum] = 0;
    sim->lastInputButtons[playerNum] = 0;
    sim->lastActionAppliedTick[playerNum] = 0;
    sim->pendingHarvestActions[playerNum] = 0;
    sim->pendingHarvestBuildAction[playerNum] = 0;
    sim->pendingHarvestBuildX[playerNum] = 0;
    sim->pendingHarvestBuildY[playerNum] = 0;
    sim->inputBufferFilled[playerNum] = 0;
    sim->inputDryTicks[playerNum] = 0;
    sim->jitterTarget[playerNum] = JITTER_BUFFER_DEFAULT;
    sim->jitterStallCount[playerNum] = 0;
    sim->jitterStarveCount[playerNum] = 0;
    sim->jitterStableTicks[playerNum] = 0;
    sim->statStallTicks[playerNum] = 0;
    sim->statGapFillTicks[playerNum] = 0;
    sim->statDroppedStaleInputs[playerNum] = 0;
    sim->statCatchupTicks[playerNum] = 0;
    sim->statLastRewindTicks[playerNum] = 0;

    sim->playerConnected[playerNum] = TRUE;
    sim->hadPlayersEver = TRUE;

    /* Initialize lobby player state. Default-team assignment (Layout A):
     *   slot 0 (host)             → team 1
     *   slot 1 (second joiner)    → team 2
     *   slot 2+ (subsequent)      → smallest existing team (load balance)
     * Bots take whichever team the bot-add path picks (existing logic).
     * Players can self-reassign via the team picker after joining. */
    sim->lobbyPlayers[playerNum].ready = FALSE;
    sim->lobbyPlayers[playerNum].isBot = FALSE;
    sim->lobbyPlayers[playerNum].startIdx = 0xFF;
    {
        uint8_t defaultTeam = 1;
        if (playerNum == 0) {
            defaultTeam = 1;
        } else if (playerNum == 1) {
            defaultTeam = 2;
        } else {
            /* Smallest in-use team wins. Counts include bots. */
            int counts[16] = {0};
            for (int i = 0; i < MAX_TANKS; i++) {
                if (i == playerNum) continue;
                if (!sim->playerConnected[i]) continue;
                uint8_t t = sim->lobbyPlayers[i].teamNumber;
                if (t > 0 && t < 16) counts[t]++;
            }
            int bestTeam = 1, bestCount = INT_MAX;
            for (int t = 1; t < 16; t++) {
                if (!sim->teams[t].in_use) continue;
                if (counts[t] < bestCount) {
                    bestCount = counts[t];
                    bestTeam  = t;
                }
            }
            defaultTeam = (uint8_t)bestTeam;
        }
        sim->lobbyPlayers[playerNum].teamNumber = defaultTeam;
    }

    /* Reserve a clustered start now the team is known (humans and bots). */
    serverSimAssignLobbyStartOnJoin(sim, playerNum);

    /* Set active sim so routing functions access sim state during tankCreate */
    activeSim = sim;

    /* Only create tank immediately in running state (no-lobby mode or mid-game join).
     * In lobby state, tanks are created at game start. */
    if (sim->state == serverStateRunning) {
        tankCreate(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.lgmen[playerNum] = lgmCreate(playerNum);
        basesUpdateTimer(&sim->sim, playerNum);
    }

    /* Register player in sim's players struct so message formatting
     * (e.g. "Player captured a base") uses the correct name. */
    if (playerName != NULL) {
        playersSetPlayer(NULL, &sim->sim.plyrs, NEUTRAL, playerNum, (char *)playerName, "XX",
                         0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
        /* Apply the country before the log event below reads it back out —
         * the recorded join otherwise carries the "XX" placeholder for the
         * whole round. No-ops on NULL or a malformed code, leaving "XX". */
        setPlayerCountryInternal(sim, playerNum, country);
        {
            char pstr[256];
            int nameLen = (int)strlen(playerName);
            BYTE accountFlags = playersGetAccountFlags(&sim->sim.plyrs, playerNum);
            if (nameLen > 255) nameLen = 255;
            pstr[0] = (char)nameLen;
            memcpy(pstr + 1, playerName, nameLen);
            logAddEvent(log_PlayerJoined, playerNum,
                        sim->sim.plyrs->item[playerNum].location[0],
                        sim->sim.plyrs->item[playerNum].location[1],
                        accountFlags, 0, pstr);
        }
    }

    /* Attempt to restore ownership of pills/bases from a previous session */
    if (wantRejoin && sim->state == serverStateRunning && playerName != NULL) {
        playersRejoinRequest(&sim->sim, (char *)playerName, playerNum, &sim->sim.pb);
    }

    /* Broadcast current skip vote state to the new player — existing votes
     * are preserved since the threshold naturally adjusts with more players. */
    if (sim->lobbyEnabled && sim->state == serverStateLobby && (sim->mapDirCount > 1 || sim->randomMapEnabled)) {
        ControlEvent skipEvt;
        BYTE k;
        memset(&skipEvt, 0, sizeof(skipEvt));
        skipEvt.type = CTRL_MAP_SKIP_STATE;
        for (k = 0; k < MAX_TANKS; k++) {
            skipEvt.u.mapSkipState.votes[k] = sim->mapSkipVotes[k] ? 1 : 0;
        }
        serverSimPublishControl(sim, &skipEvt);
    }

}

void fillAndPublishPlayerJoin(ServerSim *sim, BYTE playerNum) {
    ControlEvent joinEvt;
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    memset(&joinEvt, 0, sizeof(joinEvt));
    serverSimFillPlayerJoinEvent(sim, playerNum, &joinEvt);
    serverSimPublishControl(sim, &joinEvt);
}

void setPlayerCountryInternal(ServerSim *sim, BYTE playerNum, const char *cc) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    if (cc == NULL) return;
    if (cc[0] == '\0' || cc[1] == '\0' || cc[2] != '\0') return;
    if (!isalpha((unsigned char)cc[0]) || !isalpha((unsigned char)cc[1])) return;
    if (sim->sim.plyrs == NULL) return;
    sim->sim.plyrs->item[playerNum].location[0] = (char)toupper((unsigned char)cc[0]);
    sim->sim.plyrs->item[playerNum].location[1] = (char)toupper((unsigned char)cc[1]);
    sim->sim.plyrs->item[playerNum].location[2] = '\0';
}

void setClientTypeFlagsInternal(ServerSim *sim, BYTE playerNum,
                                uint8_t clientType, uint8_t clientFlags) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    playersSetClientType(&sim->sim.plyrs, playerNum, clientType);
    playersSetClientFlags(&sim->sim.plyrs, playerNum, clientFlags);
}

void serverSimAddPlayer(ServerSim *sim, BYTE playerNum, const char *playerName, bool wantRejoin) {
    addPlayerInternal(sim, playerNum, playerName, NULL, wantRejoin);
    fillAndPublishPlayerJoin(sim, playerNum);
}

void serverSimSetPlayerCountry(ServerSim *sim, BYTE playerNum, const char *cc) {
    setPlayerCountryInternal(sim, playerNum, cc);
    fillAndPublishPlayerJoin(sim, playerNum);
}

int serverSimFindFreeSlot(const ServerSim *sim) {
    int  i;
    BYTE limit;
    if (sim == NULL) return -1;
    limit = (sim->maxPlayers > 0) ? sim->maxPlayers : (BYTE)MAX_TANKS;
    for (i = 0; i < limit; i++) {
        if (!sim->playerConnected[i] && !botManagerIsBot(sim, (BYTE)i)) {
            return i;
        }
    }
    return -1;
}

LocalJoinResult serverSimLocalJoin(ServerSim *sim,
                                   const char *playerName,
                                   const char *fallbackCountry,
                                   uint8_t clientType,
                                   uint8_t clientFlags,
                                   BYTE *outSlot) {
    char validatedName[PLAYER_NAME_LEN];
    int  slot;
    const char *country;

    if (sim == NULL || playerName == NULL || outSlot == NULL) {
        return LOCAL_JOIN_INVALID_INPUT;
    }

    if (!playerNameValidate(playerName, validatedName, sizeof(validatedName), NULL)) {
        return LOCAL_JOIN_INVALID_NAME;
    }

    /* Mirror the UDP-side game-lock predicate (transport_udp_server.c). The
     * lobby gate is the only one we can replicate locally; the UDP-only
     * gameLocked flag has no local-join analogue. */
    if (!serverSimIsAcceptingJoins(sim)) {
        return LOCAL_JOIN_GAME_LOCKED;
    }

    slot = serverSimFindFreeSlot(sim);
    if (slot < 0) {
        return LOCAL_JOIN_SLOT_FULL;
    }

    country = (fallbackCountry != NULL) ? fallbackCountry : "";

    addPlayerInternal(sim, (BYTE)slot, validatedName, country, false);
    setClientTypeFlagsInternal(sim, (BYTE)slot, clientType, clientFlags);
    fillAndPublishPlayerJoin(sim, (BYTE)slot);

    {
        char serverKey[WINBOLONET_KEY_LEN];
        serverKey[0] = '\0';
        if (winbolonetIsRunning()) {
            winboloNetGetServerKey(serverKey);
            if (serverKey[0] != '\0') {
                winbolonetAddEvent(WINBOLO_NET_EVENT_PLAYER_JOIN, TRUE,
                                   (BYTE)slot, WINBOLO_NET_NO_PLAYER,
                                   botManagerIsBot(sim, (BYTE)slot), FALSE);
            }
        }
    }

    *outSlot = (BYTE)slot;
    return LOCAL_JOIN_OK;
}

void serverSimRemovePlayer(ServerSim *sim, BYTE playerNum) {
    bool wasBot;
    if (playerNum >= MAX_TANKS) return;
    /* Captured before any teardown so the last-human-left reset below can
     * tell a human departure from a bot one. Bot removals run through this
     * same path (botManagerRemoveBot), and the reset itself removes bots —
     * gating on a human leaver keeps that from re-entering. */
    wasBot = botManagerIsBot(sim, playerNum);
    {
        char nm[PLAYER_NAME_LEN];
        playersGetPlayerName(&sim->sim.plyrs, playerNum, nm, sizeof(nm), TRUE);
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "removePlayer slot=%u name='%s' state=%d",
            (unsigned)playerNum, nm, (int)sim->state);
    }
    logAddEvent(log_PlayerLeaving, playerNum, 0, 0, 0, 0, NULL);
    logAddEvent(log_PlayerQuit, playerNum, 0, 0, 0, 0, NULL);

    /* Publish before clearing the slot — the filler reads the player's
     * name and country out of sim->sim.plyrs->item[playerNum], which is
     * still valid here and gets zeroed later in this function. */
    {
        ControlEvent leaveEvt;
        memset(&leaveEvt, 0, sizeof(leaveEvt));
        serverSimFillPlayerLeaveEvent(sim, playerNum, &leaveEvt);
        serverSimPublishControl(sim, &leaveEvt);
    }

    /* Freeze this slot's identity before the roster entry is torn down: the
     * attribution track's identity table is otherwise only filled at game over,
     * which would leave a mid-round leaver nameless in the finished log. */
    {
        AttrSlotIdentity *id = &sim->trackIdentity[playerNum];
        /* Read the full-length name first; id->name is the shorter
         * wire-sized field, so the copy into it truncates. */
        char nameBuf[PLAYER_NAME_LEN];
        memset(id, 0, sizeof(*id));
        id->isBot = wasBot ? 1 : 0;
        id->team  = sim->lobbyPlayers[playerNum].teamNumber;
        playersGetPlayerName(&sim->sim.plyrs, playerNum, nameBuf,
                             sizeof(nameBuf), TRUE);
        snprintf(id->name, sizeof(id->name), "%s", nameBuf);
    }

    sim->playerConnected[playerNum] = FALSE;
    if (sim->sim.tanks[playerNum] != NULL) {
        tankDestroy(&sim->sim, &sim->sim.tanks[playerNum]);
        sim->sim.tanks[playerNum] = NULL;
    }
    if (sim->sim.lgmen[playerNum] != NULL) {
        lgmDestroy(&sim->sim.lgmen[playerNum]);
        sim->sim.lgmen[playerNum] = NULL;
    }
    sim->inputQueueHead[playerNum] = 0;
    sim->inputQueueTail[playerNum] = 0;
    sim->lastProcessedInput[playerNum] = 0;
    sim->lastInputButtons[playerNum] = 0;
    sim->lastActionAppliedTick[playerNum] = 0;
    sim->pendingHarvestActions[playerNum] = 0;
    sim->pendingHarvestBuildAction[playerNum] = 0;
    sim->pendingHarvestBuildX[playerNum] = 0;
    sim->pendingHarvestBuildY[playerNum] = 0;
    sim->playerPing[playerNum] = 0;
    sim->inputBufferFilled[playerNum] = 0;
    sim->inputDryTicks[playerNum] = 0;
    sim->jitterTarget[playerNum] = JITTER_BUFFER_DEFAULT;
    sim->jitterStallCount[playerNum] = 0;
    sim->jitterStarveCount[playerNum] = 0;
    sim->jitterStableTicks[playerNum] = 0;
    sim->statStallTicks[playerNum] = 0;
    sim->statGapFillTicks[playerNum] = 0;
    sim->statDroppedStaleInputs[playerNum] = 0;
    sim->statCatchupTicks[playerNum] = 0;
    sim->statLastRewindTicks[playerNum] = 0;

    /* Post-game stats: a mid-round leaver is dropped from the round summary as
     * if never present. Zero this slot's accumulator row, clear every other
     * slot's matrix cells that reference this slot (the column), and prune the
     * notable-event timeline of entries involving this slot. Other players'
     * aggregate counters are intentionally left as-is — only the leaver's own
     * stats and direct references to them are removed. Mine cells laid by this
     * slot are released so a later detonation isn't credited to a gone player. */
    {
        BYTE s;
        memset(&sim->roundStats[playerNum], 0, sizeof(sim->roundStats[playerNum]));
        for (s = 0; s < MAX_TANKS; s++) {
            sim->roundStats[s].killsOf[playerNum]  = 0;
            sim->roundStats[s].killedBy[playerNum] = 0;
        }
        minesClearOwner(&sim->sim.mns, playerNum);
        {
            uint16_t r, w = 0;
            for (r = 0; r < sim->notableEventCount; r++) {
                const NotableEvent *ne = &sim->notableEvents[r];
                if (ne->actorA == playerNum || ne->actorB == playerNum) continue;
                if (w != r) sim->notableEvents[w] = *ne;
                w++;
            }
            sim->notableEventCount = w;
        }
    }

    /* Record ownership for rejoin before migration changes it */
    {
        char pName[PLAYER_NAME_LEN];
        PlayerBitMap pillBits = 0, baseBits = 0;
        BYTE numPills = pillsGetNumPills(&sim->sim.pb);
        BYTE numBases = basesGetNumBases(&sim->sim.bs);
        BYTE i;
        playersGetPlayerName(&sim->sim.plyrs, playerNum, pName, sizeof(pName),
                             TRUE);
        for (i = 1; i <= numPills; i++) {
            if (pillsGetPillOwner(&sim->sim.pb, i) == playerNum) {
                pillBits |= (1u << (i - 1));
            }
        }
        for (i = 1; i <= numBases; i++) {
            if (basesGetBaseOwner(&sim->sim.bs, i) == playerNum) {
                baseBits |= (1u << (i - 1));
            }
        }
        if (pName[0] != '\0' && (pillBits != 0 || baseBits != 0)) {
            playersRejoinAddPlayer(pName, pillBits, baseBits);
        }
    }

    /* Migrate or neutralize pillboxes owned by the leaving player */
    {
        BYTE numPills = pillsGetNumPills(&sim->sim.pb);
        BYTE i;
        for (i = 1; i <= numPills; i++) {
            if (pillsGetPillOwner(&sim->sim.pb, i) == playerNum) {
                /* Look for a connected allied player to inherit */
                BYTE newOwner = NEUTRAL;
                BYTE k;
                for (k = 0; k < MAX_TANKS; k++) {
                    if (k != playerNum && sim->playerConnected[k] && playersIsAllie(&sim->sim.plyrs, playerNum, k)) {
                        newOwner = k;
                        break;
                    }
                }
                pillsSetPillOwner(&sim->sim, &sim->sim.pb, i, newOwner, TRUE);
            }
        }
    }

    /* Migrate or neutralize bases owned by the leaving player */
    {
        BYTE numBases = basesGetNumBases(&sim->sim.bs);
        BYTE i;
        for (i = 1; i <= numBases; i++) {
            if (basesGetBaseOwner(&sim->sim.bs, i) == playerNum) {
                BYTE newOwner = NEUTRAL;
                BYTE k;
                for (k = 0; k < MAX_TANKS; k++) {
                    if (k != playerNum && sim->playerConnected[k] && playersIsAllie(&sim->sim.plyrs, playerNum, k)) {
                        newOwner = k;
                        break;
                    }
                }
                basesSetBaseOwner(&sim->sim, i, newOwner, TRUE);
            }
        }
    }

    /* Drop the leaving slot from every alliance bitmap — its own, and
     * every other slot's reference to it. Without this, a new player
     * taking the vacated slot is silently inherited as an ally by the
     * old team (because other slots still have the bit set), and the
     * round-end team-carry-forward at serverSimResetGameAndReturnToLobby
     * walks playersIsAllie and propagates the stale grouping into next-
     * round teamNumber. Done after pill/base migration above, which
     * needs the still-intact alliance info to pick an heir. */
    playersLeaveAlliance(&sim->sim, &sim->sim.plyrs, NEUTRAL, playerNum, TRUE);
    {
        ControlEvent allyLeaveEvt;
        memset(&allyLeaveEvt, 0, sizeof(allyLeaveEvt));
        allyLeaveEvt.type = CTRL_ALLIANCE_LEAVE;
        allyLeaveEvt.u.allianceLeave.playerNum = playerNum;
        serverSimPublishControl(sim, &allyLeaveEvt);
    }

    /* Force immediate full sync so clients see ownership changes right away */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));

    /* Clear lobby state */
    sim->lobbyPlayers[playerNum].teamNumber = 0;
    sim->lobbyPlayers[playerNum].ready = FALSE;
    sim->lobbyPlayers[playerNum].isBot = FALSE;
    sim->lobbyPlayers[playerNum].startIdx = 0xFF;
    sim->mapSkipVotes[playerNum] = false;

    /* Check if disconnect pushes skip votes over threshold */
    if (sim->lobbyEnabled && sim->state == serverStateLobby && (sim->mapDirCount > 1 || sim->randomMapEnabled)) {
        int voteCount = 0;
        int connectedHumans = 0;
        BYTE k;
        for (k = 0; k < MAX_TANKS; k++) {
            if (!sim->playerConnected[k] || sim->lobbyPlayers[k].isBot) continue;
            connectedHumans++;
            if (sim->mapSkipVotes[k]) voteCount++;
        }
        if (connectedHumans > 0 && voteCount * 2 > connectedHumans) {
            WB_LOG_INFO(WB_LOG_CAT_SERVER, "Map skip: disconnect pushed votes over threshold (%d/%d), skipping map", voteCount, connectedHumans);
            if (sim->randomMapEnabled) {
                serverSimRandomMapRegenerate(sim);
            } else {
                serverSimMapDirPickRandom(sim);
            }
            serverSimMapSkipVotesReset(sim);
            publishMapSkipState(sim);
            serverSimWbnLobbyUpdate(sim, FALSE);
        }
    }

    /* If in countdown and someone disconnects, revert to lobby. Route
     * through serverSimAbortCountdown rather than mutating state inline
     * so the CTRL_GAME_PHASE_LOBBY publish fires — without it, remote
     * clients' netStat stays at netLobbyCountdown and their overlay
     * doesn't clear. The disconnect path through serverDisconnectClient
     * already aborts via lobbyAutoUnreadyOnChange, so this site is a
     * no-op there (state is already Lobby); it carries the abort for
     * the non-UDP callers — bot removal and local-transport
     * disconnect via client_net.c — that don't share that path. */
    if (sim->lobbyEnabled && sim->state == serverStateCountdown) {
        serverSimAbortCountdown(sim);
        logAddEvent(log_CountdownCancel, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Countdown cancelled — player disconnected.");
    }

    /* Re-check all-ready after disconnect (may need to re-trigger or cancel) */
    if (sim->lobbyEnabled && sim->state == serverStateLobby) {
        serverSimLobbyCheckAllReady(sim);
    }

    /* Notify clients that this player left */
    {
        GameEvent ev;
        ev.type = EVENT_PLAYER_LEAVE;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = playerNum;
        serverSimAddEvent(sim, &ev);
    }

    /* Player composition changed — any pending balance proposal is now
     * sized against a stale roster. Dismiss and broadcast the cleared
     * state so balanceProposalActive flips back to false on every
     * subscriber. */
    if (serverSimGetBalanceProposal(sim)->pending) {
        ControlEvent evt;
        serverSimClearBalanceProposal(sim);
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_BALANCE_PROPOSAL;
        serverSimPublishControl(sim, &evt);
    }

    /* Clear the players-struct identity for the vacated slot. Done last,
     * after every read above that needs the departing player's name /
     * alliances (CTRL_PLAYER_LEAVE fill, rejoin-ownership record, ally
     * migration). Without this the slot stays inUse with the old name and
     * the join sync-replay's inUse-gated CTRL_PLAYER_JOIN loop re-announces
     * the departed player or bot to every new client as a frozen phantom —
     * it never receives snapshot updates, which gate on playerConnected.
     * This is the identity teardown serverSimResetGameWorld's comment
     * already delegates to the leave path. */
    playersClearSlot(&sim->sim.plyrs, playerNum);

    /* If the departing slot was the host, hand the role to the lowest-
     * numbered connected human. Bots can never host; if no humans remain,
     * fall back to slot 0. The setter publishes the lobby settings. */
    if (playerNum == sim->hostSlot) {
        BYTE next = 0;
        for (BYTE i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i] && !serverSimIsBot(sim, i)) {
                next = i;
                break;
            }
        }
        serverSimSetHostSlot(sim, next);
    }

    /* Last human out of the lobby — wipe the slate so the next joiner gets
     * a fresh lobby: drop any bots, restore the operator's startup settings,
     * and unlock. Gated on a human leaver (bots removed here don't recurse)
     * and on the lobby state (running-game departures are handled by the
     * return-to-lobby / empty-reset paths). */
    if (!wasBot && sim->lobbyEnabled && sim->state == serverStateLobby &&
        serverSimGetNumHumans(sim) == 0) {
        serverSimResetLobbyToDefaults(sim);
    }
}

void serverSimSetTeamBatch(ServerSim *sim, BYTE playerNum, BYTE teamNumber) {
    if (playerNum >= MAX_TANKS) {
        return;
    }
    if (teamNumber >= MAX_TANKS) {
        teamNumber = 1;
    }
    sim->lobbyPlayers[playerNum].teamNumber = teamNumber;
}

void serverSimSetTeam(ServerSim *sim, BYTE playerNum, BYTE teamNumber) {
    serverSimSetTeamBatch(sim, playerNum, teamNumber);
    serverSimReapplyTeamAlliances(sim);
    /* Re-cluster the slot's reserved start to its new team now the team is
     * written. The helper frees the slot's own current reservation back into
     * the candidate pool (so the existing start can be re-chosen) and clusters
     * toward same-team holders, or falls to farthest-first when the new team
     * has no other members. No-ops outside lobby state or for an unconnected
     * slot, so the headless/batch drivers are unaffected. Callers republish
     * the slot themselves. */
    serverSimAssignLobbyStartOnJoin(sim, playerNum);
}

void serverSimSetLobbyStartIdx(ServerSim *sim, BYTE slot, BYTE idx) {
    if (slot >= MAX_TANKS) {
        return;
    }
    sim->lobbyPlayers[slot].startIdx = idx;
}

void serverSimSetReady(ServerSim *sim, BYTE playerNum, bool ready) {
    if (playerNum >= MAX_TANKS) {
        return;
    }
    if (!sim->lobbyEnabled) {
        return;
    }
    sim->lobbyPlayers[playerNum].ready = ready ? TRUE : FALSE;
}

void serverSimApplyInstanceConfig(ServerSim *sim, const ServerInstanceConfig *cfg) {
  sim->sim.viewPlayer = cfg->viewPlayer;
  sim->maxBots        = cfg->maxBots;
  sim->maxSpectators  = cfg->maxSpectators;
  sim->specDelayTicks = (uint32_t)cfg->specDelaySeconds * 50u;   /* 50 ticks/s */

  serverSimSetEmptyResetEnabled(sim, cfg->emptyResetEnabled);
  serverSimSetHasPassword(sim, cfg->hasPassword);
  if (cfg->botBrainPath != NULL) {
    serverSimSetBotBrainPath(sim, cfg->botBrainPath);
  }
  if ((aiType)cfg->botAiType != aiNone) {
    serverSimSetBotAiType(sim, (aiType)cfg->botAiType);
  }
  /* ranked forces autolock-on-game-start (matches the server-side
   * LST_RANKED handler at PACKET_LOBBY_SET_SETTING and the existing
   * servermain.c -ranked CLI behaviour). */
  serverSimSetAutoLockOnGameStart(sim,
      cfg->autoLockOnGameStart || cfg->ranked);
  serverSimSetRanked(sim, cfg->ranked);
  serverSimSetOpenHost(sim, cfg->openHost);
  serverSimSetServerLocks(sim, cfg->serverLocks);

  /* lobbyEnabled and skipLobby drive state transitions. If neither is
   * set, the sim stays in whatever state serverSimCreate* left it
   * (today's dedicated-server-with-no-cfg-fields behaviour). */
  if (cfg->skipLobby) {
    serverSimSetLobbyEnabled(sim, false);
    serverSimStartGame(sim);
    /* serverSimStartGame latches hadPlayersEver = TRUE, but a map-rotation
     * server's first round boots up empty and waits for joiners. Left set, the
     * lifecycle's empty-server check would fire on the very next tick and
     * rotate before anyone joins. Re-arm it so the empty rotation only fires
     * once a player has joined and then left — serverSimMapRotateRound does the
     * same for every later round. */
    if (sim->mapRotateEnabled) {
      sim->hadPlayersEver = FALSE;
    }
  } else if (cfg->lobbyEnabled) {
    serverSimSetLobbyEnabled(sim, true);
    serverSimEnterLobby(sim);
  }

  /* Snapshot the configured lobby settings now that every startup field
   * is in place — serverSimResetLobbyToDefaults restores from this when
   * the last human leaves the lobby. */
  sim->originalLobbySettings.valid               = true;
  sim->originalLobbySettings.gameType            = gameTypeGet(&sim->sim.game);
  sim->originalLobbySettings.hiddenMines         = sim->sim.hiddenMines ? true : false;
  sim->originalLobbySettings.botAiType           = sim->botAiType;
  sim->originalLobbySettings.aiPolicy            = sim->aiPolicy;
  sim->originalLobbySettings.timeLimit           = sim->timeLimit;
  sim->originalLobbySettings.timeMinutes         = sim->timeMinutes;
  sim->originalLobbySettings.gameLength          = sim->gameLength;
  sim->originalLobbySettings.openHost            = sim->openHost;
  sim->originalLobbySettings.autoLockOnGameStart = sim->autoLockOnGameStart;
  sim->originalLobbySettings.ranked              = sim->ranked;
  sim->originalLobbySettings.serverLocks         = sim->serverLocks;
}

void serverSimAddEvent(ServerSim *sim, const GameEvent *event) {
    /* Per-round stats funnel. Runs before the snapshot-event buffering below
     * so a full event buffer never drops a stat. Only during a running game,
     * so any state-load/replay re-emit can't double-count. */
    if (sim->state == serverStateRunning) {
        const uint8_t *d = event->data;
        switch (event->type) {
        case EVENT_TANK_KILLED: {
            AttrKillRecord r;
            r.type = ATTR_REC_KILL; r.tick = sim->tick;
            r.killer = d[0]; r.killed = d[1]; r.deathCause = d[2];
            r.carriedPills = d[3]; r.treesWasted = d[4];
            r.mapX = d[5]; r.mapY = d[6];   /* stashed in serverSimCbTankKill */
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        case EVENT_PILL_CAPTURED:
        case EVENT_BASE_CAPTURED: {
            AttrCaptureRecord r;
            r.type = ATTR_REC_CAPTURE; r.tick = sim->tick;
            r.target = (event->type == EVENT_PILL_CAPTURED)
                           ? ATTR_CAP_TGT_PILL : ATTR_CAP_TGT_BASE;
            r.targetIndex = d[3];   /* pill/base array index (server-internal, past wire size) */
            r.newOwner = d[0]; r.prevOwner = d[1]; r.captureClass = d[2];
            r.mapX = d[4]; r.mapY = d[5];   /* pill/base map cell, stashed at emit */
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        case EVENT_LGM_LOST: {
            AttrLgmRecord r;
            r.type = ATTR_REC_LGM; r.tick = sim->tick;
            r.victim = d[0]; r.killer = d[1];
            r.mapX = d[2]; r.mapY = d[3];   /* LGM map cell, stashed at emit */
            serverSimTrackAppend(sim, &r, sizeof r);
            roundStatsApplyRecord(sim->roundStats, sim->notableEvents,
                                  &sim->notableEventCount, NOTABLE_EVENTS_MAX, &r);
            break;
        }
        default: break;
        }
    }
    if (sim->eventCount < MAX_SNAPSHOT_EVENTS) {
        sim->events[sim->eventCount] = *event;
        sim->eventCount++;
    }
}

const PlayerRoundStats *serverSimGetRoundStats(const ServerSim *sim, BYTE slot) {
    if (slot >= MAX_TANKS) return NULL;
    return &sim->roundStats[slot];
}

const uint8_t *serverSimGetTrackBuffer(const ServerSim *sim, size_t *outLen,
                                       uint32_t *outRecordCount, bool *outTruncated) {
    if (outLen != NULL)         *outLen = sim->trackLen;
    if (outRecordCount != NULL) *outRecordCount = sim->trackRecordCount;
    if (outTruncated != NULL)   *outTruncated = sim->trackTruncated;
    return sim->trackBuf;
}

const AttrSlotIdentity *serverSimGetTrackIdentity(const ServerSim *sim) {
    return sim->trackIdentity;
}

void serverSimBuildRoundStatsSummary(ServerSim *sim, RoundStatsSummary *out) {
    memset(out, 0, sizeof(*out));

    bool isBot[MAX_TANKS];
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        isBot[slot] = botManagerIsBot(sim, (BYTE)slot);
    }

    /* One curated scoreboard row per connected slot. Leavers were zeroed
     * out of the accumulator already, so only present slots contribute. */
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        if (!sim->playerConnected[slot]) continue;
        const PlayerRoundStats *rs = &sim->roundStats[slot];
        RoundPlayerSummary *p = &out->players[out->playerCount++];
        p->slot         = (uint8_t)slot;
        p->isBot        = isBot[slot] ? 1 : 0;
        p->kills        = (uint16_t)rs->kills;
        p->deaths       = (uint16_t)rs->deaths;
        p->baseCaptures = (uint16_t)rs->baseCaptures;
        p->pillCaptures = (uint16_t)rs->pillCaptures;
        p->dmgDealt     = (uint32_t)(rs->dmgToPlayers + rs->dmgToPills + rs->dmgToBases);
        p->builds       = (uint16_t)(rs->pillsBuilt + rs->treesFarmed);
        p->lgmKills     = (uint16_t)rs->lgmKills;
        p->lgmDeaths    = (uint16_t)rs->lgmDeaths;
    }

    /* Awards rank over every slot; absent/zeroed slots score 0 and are
     * omitted, so the result already excludes leavers. */
    int n = 0;
    computeAwards(sim->roundStats, MAX_TANKS, /*includeBots*/ true, isBot,
                  out->awards, &n);
    out->awardCount = (uint8_t)n;

    /* The round's highlight clips. The accumulator and the notable timeline are
     * already live, so only the territory series has to be derived here: one
     * walk of the attribution stream turning pill/base ownership changes into
     * per-team map control. sim->trackBuf is NULL on a round that recorded
     * nothing, which computeTerritoryShifts handles by yielding no shifts. */
    uint8_t team[MAX_TANKS];
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        team[slot] = sim->trackIdentity[slot].team;
    }

    TerritoryShift shifts[TERRITORY_SHIFTS_MAX];
    int shiftCount = 0;
    computeTerritoryShifts(sim->trackBuf, sim->trackLen, sim->trackIdentity,
                           MAX_TANKS, shifts, &shiftCount, TERRITORY_SHIFTS_MAX);

    int hlCount = 0;
    computeHighlights(sim->notableEvents, sim->notableEventCount,
                      sim->roundStats, team, out->awards, (int)out->awardCount,
                      shifts, shiftCount, out->highlights, &hlCount,
                      ROUND_STATS_HIGHLIGHTS_WIRE_MAX);
    out->highlightCount = (uint8_t)hlCount;

    /* computeHighlights works in sim ticks; the clients want a time. Convert
     * once, here, so nothing downstream has to know the sim's cadence.
     * The origin is the tick the round's log segment started at, so a clip's
     * ms is measured from the same instant the viewer's window is. A clip that
     * somehow predates the latch clamps to the start rather than wrapping. */
    for (int i = 0; i < hlCount; i++) {
        HighlightWindow *h = &out->highlights[i];
        h->startMs = (sim->roundLogStartTick != ROUND_LOG_START_UNSET &&
                      h->startTick >= sim->roundLogStartTick)
                         ? (h->startTick - sim->roundLogStartTick) *
                               SIM_TICK_MS
                         : 0u;
        h->durationMs = h->durationTicks * SIM_TICK_MS;
    }

    /* The round's WinBolo.net identity is the server key it was played
     * under, read live here so the lobby recap can name this round's log
     * to WBN later. That live read is correct only because of when this
     * runs: the summary is built at game over, ahead of the
     * EndSession/upload/BeginSession sandwich in server_lifecycle.c, so
     * winboloNetServerKey still holds the finished round's key. Move the
     * round-stats publish after that rotation, or the rotation ahead of
     * it, and this silently publishes the next round's key instead.
     * Stays empty (memset above) when WBN isn't running: a LAN or
     * single-player round's log is never uploaded, so there is no key
     * for the recap to point at. */
    if (winbolonetIsRunning()) {
        char serverKey[WINBOLONET_KEY_LEN];
        serverKey[0] = '\0';
        winboloNetGetServerKey(serverKey);
        serverKey[WINBOLONET_KEY_LEN - 1] = '\0';
        strncpy(out->wbnLogKey, serverKey, sizeof(out->wbnLogKey) - 1);
        out->wbnLogKey[sizeof(out->wbnLogKey) - 1] = '\0';
    }
}

bool serverSimIsRunning(void) {
    ServerSim *sim = serverSimGetActive();
    return sim != NULL && sim->state == serverStateRunning;
}

void serverSimClearBalanceProposal(ServerSim *sim) {
    memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
}

void serverSimConsoleMessage(const char *msg) {
    ServerSim *sim = serverSimGetActive();
    if (sim != NULL && sim->sim.callbacks.consoleMessage != NULL) {
        sim->sim.callbacks.consoleMessage(sim->sim.callbacks.ctx, (char *)msg);
    } else {
#ifndef WB_FUZZ
        /* Fallback: print to stdout if no active sim */
        fprintf(stdout, "%s\n", msg);
#endif
    }
}

static void publishMapSkipState(ServerSim *sim) {
    ControlEvent evt;
    if (sim == NULL) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillMapSkipStateEvent(sim, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimMapSkipVoteToggle(ServerSim *sim, uint8_t playerNum) {
    int voteCount = 0;
    int connectedHumans = 0;
    BYTE i;

    if (sim->state != serverStateLobby || (sim->mapDirCount <= 1 && !sim->randomMapEnabled)) {
        return;
    }
    if (playerNum >= MAX_TANKS || !sim->playerConnected[playerNum]) {
        return;
    }
    if (sim->lobbyPlayers[playerNum].isBot) {
        return;
    }

    sim->mapSkipVotes[playerNum] = !sim->mapSkipVotes[playerNum];
    if (sim->mapSkipVotes[playerNum]) {
        logAddEvent(log_MapSkipVote, playerNum, 0, 0, 0, 0, NULL);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || sim->lobbyPlayers[i].isBot) continue;
        connectedHumans++;
        if (sim->mapSkipVotes[i]) voteCount++;
    }

    WB_LOG_INFO(WB_LOG_CAT_SERVER, "Map skip: player %d voted %s (%d/%d)", playerNum,
            sim->mapSkipVotes[playerNum] ? "yes" : "no", voteCount, connectedHumans);

    if (connectedHumans > 0 && voteCount * 2 > connectedHumans) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER, "Map skip: majority reached (%d/%d), skipping map", voteCount, connectedHumans);
        if (sim->randomMapEnabled) {
            serverSimRandomMapRegenerate(sim);
        } else {
            serverSimMapDirPickRandom(sim);
        }
        {
            char pstr[256];
            int nameLen = (int)strlen(sim->mapName);
            if (nameLen > 255) nameLen = 255;
            pstr[0] = (char)nameLen;
            memcpy(pstr + 1, sim->mapName, nameLen);
            logAddEvent(log_MapSkipApplied, 0, 0, 0, 0, 0, pstr);
        }
        serverSimMapSkipVotesReset(sim);
        serverSimWbnLobbyUpdate(sim, FALSE);
    }
    publishMapSkipState(sim);
}

void serverSimMapSkipVotesReset(ServerSim *sim) {
    memset(sim->mapSkipVotes, 0, sizeof(sim->mapSkipVotes));
}

/* ----------------------------------------------------------------------
 * In-game vote system (back-to-lobby + surrender). See docs/voting_plan.md.
 *
 * The state machine lives entirely on the server; clients are mirror-only.
 * Wire format: PACKET_GAME_VOTE_TOGGLE in, PACKET_GAME_VOTE_STATE out.
 *
 * NOTE: this is the data-model + helper layer. Wire serialisation lives
 * in transport_udp_server.c (broadcastGameVoteState). Tick wiring lives
 * in the transport tick path.
 * ---------------------------------------------------------------------- */

/* Server-originated English broadcast via CTRL_CHAT (fromPlayer=0xFE).
 * Inlined here (instead of calling transportUdpServerSendServerMessage)
 * so BrainTest / MapEditor — which link server_sim_static but not the
 * UDP transport — can still announce server messages to subscribers. */
static void publishServerMessage(ServerSim *sim, const char *message) {
    ControlEvent evt;
    if (!sim || !message) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    /* In-process subscribers display via client_sim_control.c's
     * CTRL_SERVER_TEXT handler (newswire / lobby chat); UDP clients
     * receive the codec-encoded PACKET_CHAT_BROADCAST(fromPlayer=0xFE)
     * via the encoder table. */
    serverSimPublishControl(sim, &evt);
}

/* Like publishServerMessage but delivered ONLY to members of teamId (1-16).
 * destTeam rides the ControlEvent and is filtered per-recipient in
 * udpClientDeliverControl + the in-process CTRL_SERVER_TEXT handler — used to
 * keep surrender-vote notices private to the surrendering team. */
static void publishServerMessageToTeam(ServerSim *sim, const char *message,
                                       BYTE teamId) {
    ControlEvent evt;
    if (!sim || !message) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    evt.u.serverText.destTeam = teamId;
    serverSimPublishControl(sim, &evt);
}

/* serverSimBufferLobbyChat — append a chat event to the current-session
 * lobby-chat catch-up buffer, dropping the oldest entry when full. Only
 * called for events that should be replayed to a returning spectator
 * (broadcast player chat + spectator chat captured during lobby/countdown);
 * gating is the caller's responsibility. The event is stored verbatim so the
 * drain-flip replay (serverSimReplayLobbyChat) re-delivers exactly what was
 * fanned live. */
static void serverSimBufferLobbyChat(ServerSim *sim, const ControlEvent *evt) {
    if (sim->lobbyChatCount == LOBBY_CHAT_BUFFER_MAX) {
        memmove(&sim->lobbyChatBuffer[0], &sim->lobbyChatBuffer[1],
                (LOBBY_CHAT_BUFFER_MAX - 1) * sizeof(sim->lobbyChatBuffer[0]));
        sim->lobbyChatCount = LOBBY_CHAT_BUFFER_MAX - 1;
    }
    sim->lobbyChatBuffer[sim->lobbyChatCount++] = *evt;
}

/* serverSimReceiveChat — authoritative entry for any chat the server
 * accepts, regardless of which transport delivered the input.
 *
 * Per docs/ARCHITECTURE.md "Worked example — adding a chat message":
 * every audience (in-process subscribers + UDP-connected clients) must
 * see the same event. We achieve that by routing both inputs (the UDP
 * server's PACKET_CHAT_MESSAGE handler and the bot pool's chat-send
 * callback) through here, then publishing a single CTRL_CHAT — the
 * per-client subscriber in transport_udp_server.c fans it back out on
 * the wire (via the codec encoder) and the in-process CTRL_CHAT
 * handler in client_sim_control.c materializes it into recipient
 * MessageStates.
 *
 * fromPlayer must be a real slot (0..MAX_TANKS-1); destPlayer is the
 * single recipient or 0xFF for broadcast. body/bodyLen is the raw chat
 * payload (no length prefix). Caller is responsible for keeping
 * bodyLen <= PACKET_MAX_CHAT_MESSAGE. */
void serverSimReceiveChat(ServerSim *sim, BYTE fromPlayer, BYTE destPlayer,
                          const void *body, size_t bodyLen) {
    ControlEvent evt;
    if (sim == NULL || body == NULL || fromPlayer >= MAX_TANKS) return;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = fromPlayer;
    evt.u.chat.destPlayer = destPlayer;
    evt.u.chat.bodyLen    = (uint16_t)bodyLen;
    if (bodyLen > 0) {
        memcpy(evt.u.chat.body, body, bodyLen);
    }

    /* The lobby-chat catch-up capture lives in serverSimPublishControl, the one
     * chokepoint this and the wire CMD_CHAT path both publish through. */
    serverSimPublishControl(sim, &evt);
}

/* serverSimReceiveSpectatorChat — authoritative entry for a lobby chat line
 * typed by a tankless spectator. A spectator has no player slot, so it cannot
 * route through serverSimReceiveChat; instead the message is stamped with the
 * sender's specIdx and published as CTRL_SPECTATOR_CHAT, which the bus fans to
 * players and to spectators (the spectator deliver allowlist passes it). The
 * line is recorded into the .wbv as log_SpectatorChat so the log viewer can
 * attribute it. */
void serverSimReceiveSpectatorChat(ServerSim *sim, uint8_t specIdx,
                                   const void *body, size_t bodyLen) {
    ControlEvent evt;
    if (sim == NULL || body == NULL || specIdx >= MAX_SPECTATORS) return;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SPECTATOR_CHAT;
    evt.u.spectatorChat.specIdx = specIdx;
    evt.u.spectatorChat.bodyLen = (uint16_t)bodyLen;
    if (bodyLen > 0) {
        memcpy(evt.u.spectatorChat.body, body, bodyLen);
    }

    /* Capture for the drain-flip catch-up happens in serverSimPublishControl. */
    serverSimPublishControl(sim, &evt);

    {
        char pstr[256];
        int pLen = (int)bodyLen;
        if (pLen > 255) pLen = 255;
        pstr[0] = (char)pLen;
        if (pLen > 0) memcpy(pstr + 1, body, pLen);
        logAddEvent(log_SpectatorChat, specIdx, 0, 0, 0, 0, pstr);
    }
}

/* Publish current vote state through the control-event dispatcher.
 * In-process subscribers see it directly; remote UDP clients receive
 * the wire-encoded PACKET_GAME_VOTE_STATE via the codec encoder. */
static void publishGameVoteState(ServerSim *sim, uint8_t kind) {
    ServerGameVoteSnapshot snap;
    ControlEvent evt;
    if (!serverSimGetGameVoteSnapshot(sim, kind, &snap)) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_VOTE_STATE;
    evt.u.gameVoteState.kind             = snap.kind;
    evt.u.gameVoteState.active           = snap.active;
    evt.u.gameVoteState.triggerSrc       = snap.triggerSrc;
    evt.u.gameVoteState.teamId           = snap.teamId;
    evt.u.gameVoteState.threshold        = snap.threshold;
    evt.u.gameVoteState.yesCount         = snap.yesCount;
    evt.u.gameVoteState.noCount          = snap.noCount;
    evt.u.gameVoteState.eligibleCount    = snap.eligibleCount;
    evt.u.gameVoteState.secondsRemaining = snap.secondsRemaining;
    evt.u.gameVoteState.votes            = snap.votes;
    serverSimPublishControl(sim, &evt);
}

/* Forward declarations for the in-TU helpers — gameVoteThreshold is
 * called from the public snapshot accessor which sits above the
 * helper's definition. */
static uint8_t gameVoteThreshold(const ServerSim *sim, const struct ServerGameVote *gv);

static struct ServerGameVote *gameVoteSlot(ServerSim *sim, uint8_t kind) {
    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) return &sim->gameVotes[0];
    if (kind == GAME_VOTE_KIND_SURRENDER)     return &sim->gameVotes[1];
    return NULL;
}

static const struct ServerGameVote *gameVoteSlotConst(const ServerSim *sim, uint8_t kind) {
    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) return &sim->gameVotes[0];
    if (kind == GAME_VOTE_KIND_SURRENDER)     return &sim->gameVotes[1];
    return NULL;
}

/* Returns the bitmask of slots eligible to vote on this kind. For
 * back-to-lobby that's every connected human; for surrender it's the
 * connected humans on `teamId`. */
static uint16_t gameVoteEligibleMask(const ServerSim *sim,
                                     uint8_t kind, uint8_t teamId) {
    uint16_t mask = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        if (sim->lobbyPlayers[i].isBot) continue;
        if (kind == GAME_VOTE_KIND_SURRENDER &&
            sim->lobbyPlayers[i].teamNumber != teamId) continue;
        mask |= (uint16_t)(1u << i);
    }
    return mask;
}

static uint8_t popcount16(uint16_t v) {
    uint8_t n = 0;
    while (v) { n += (uint8_t)(v & 1u); v >>= 1; }
    return n;
}

uint8_t serverSimCountActiveTeams(const ServerSim *sim) {
    /* Counts distinct teamNumbers across teams with at least one
     * connected human. Bots don't count — surrender needs a human
     * on each side. */
    bool seen[MAX_TANKS] = {0};
    uint8_t count = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || sim->lobbyPlayers[i].isBot) continue;
        uint8_t t = sim->lobbyPlayers[i].teamNumber;
        if (t == 0 || t >= MAX_TANKS) continue;
        if (!seen[t]) { seen[t] = true; count++; }
    }
    return count;
}

bool serverSimGameVoteIsRunning(const ServerSim *sim, uint8_t kind) {
    const struct ServerGameVote *gv = gameVoteSlotConst(sim, kind);
    return gv && gv->active == GAME_VOTE_ACTIVE_RUNNING;
}

bool serverSimGetGameVoteSnapshot(const ServerSim *sim, uint8_t kind,
                                  ServerGameVoteSnapshot *out) {
    const struct ServerGameVote *gv = gameVoteSlotConst(sim, kind);
    if (!gv || !out) return false;
    memset(out, 0, sizeof(*out));
    out->kind       = gv->kind;
    out->active     = gv->active;
    out->triggerSrc = gv->triggerSrc;
    out->teamId     = gv->teamId;
    uint16_t eligibleMask = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
    out->eligibleCount = popcount16(eligibleMask);
    out->threshold  = gameVoteThreshold(sim, gv);
    out->yesCount   = popcount16(gv->votesMask);
    out->noCount    = popcount16(gv->answeredMask & ~gv->votesMask);
    out->votes      = gv->votesMask;
    if (gv->active == GAME_VOTE_ACTIVE_RUNNING) {
        uint64_t now = sim->gameVoteWallMs;
        /* During the pre-pass grace window, surface that grace's
         * remaining seconds so the client widget shows the short
         * "Passing in N..." countdown instead of the long 60-s
         * timeout. The client infers the state from
         * (yesCount == threshold) + small secondsRemaining. */
        uint64_t until = (gv->pendingPassUntilMs != 0)
                         ? gv->pendingPassUntilMs
                         : gv->deadlineMs;
        uint64_t rem = (until > now) ? (until - now) : 0;
        uint32_t secs = (uint32_t)((rem + 999) / 1000);
        if (secs > 0xFFu) secs = 0xFFu;
        out->secondsRemaining = (uint8_t)secs;
    }
    return true;
}

void serverSimGameVoteResetAll(ServerSim *sim) {
    memset(sim->gameVotes, 0, sizeof(sim->gameVotes));
    sim->gameVotes[0].kind = GAME_VOTE_KIND_BACK_TO_LOBBY;
    sim->gameVotes[1].kind = GAME_VOTE_KIND_SURRENDER;
    sim->returnToLobbyTicks = 0;
    sim->returnToLobbyReason = RETURN_REASON_NONE;
    sim->returnToLobbyTeamId = 0;
}

static void gameVoteStart(ServerSim *sim, uint8_t kind, uint8_t triggerSrc,
                          uint8_t teamId, uint64_t nowMs, uint8_t initiator) {
    struct ServerGameVote *gv = gameVoteSlot(sim, kind);
    if (!gv) return;
    memset(gv, 0, sizeof(*gv));
    gv->kind        = kind;
    gv->active      = GAME_VOTE_ACTIVE_RUNNING;
    gv->triggerSrc  = triggerSrc;
    gv->teamId      = teamId;
    gv->startMs     = nowMs;
    gv->deadlineMs  = nowMs + (uint64_t)GAME_VOTE_DEADLINE_SECONDS * 1000ULL;
    gv->lastHeartbeatMs = nowMs;
    logAddEvent(log_GameVoteStart, kind, initiator, teamId, 0, 0, NULL);
}

static void gameVoteConclude(ServerSim *sim, struct ServerGameVote *gv,
                             uint8_t finalState, uint64_t nowMs) {
    gv->active = finalState;
    gv->concludedAtMs = nowMs;
    publishGameVoteState(sim, gv->kind);
    logAddEvent(log_GameVoteEnd, gv->kind,
                finalState == GAME_VOTE_ACTIVE_PASSED ? 1 : 0,
                0, 0, 0, NULL);
}

/* Fire the actual pass effects (countdown for back-to-lobby,
 * announcement + chained vote for surrender). Called from the tick
 * once the pending-pass grace expires. */
static void gameVoteFirePass(ServerSim *sim, struct ServerGameVote *gv,
                             uint64_t nowMs) {
    uint8_t kind = gv->kind;
    gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_PASSED, nowMs);

    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) {
        /* Game keeps running — players can still move, shoot,
         * etc. — but each tick decrements sim->returnToLobbyTicks
         * and at 0 the running tick transitions to gameOver. The
         * snapshot header carries the remaining ticks every frame
         * so clients can render their own 3/2/1 countdown.
         *
         * Budget: 7 seconds at 100Hz (each serverSimTick call). */
        /* The vote leaves a line in the returning lobby explaining why the
         * round ended (no in-game newswire line — clients already render
         * the 3/2/1 countdown). Every back-to-lobby vote is player-started
         * since the base-monopoly auto-vote was removed. */
        sim->returnToLobbyReason = RETURN_REASON_MANUAL_VOTE;
        sim->returnToLobbyTicks = 700;
    } else if (kind == GAME_VOTE_KIND_SURRENDER) {
        char buf[160];
        const char *tname = sim->teams[gv->teamId].name[0]
                            ? sim->teams[gv->teamId].name : "?";
        snprintf(buf, sizeof(buf),
                 "*** Team %s has surrendered. ***", tname);
        publishServerMessage(sim, buf);

        /* A surrender ends the round immediately — no chained
         * back-to-lobby vote. Record the team that gave up so the
         * game-over handler credits the opposing team with the win
         * (WBN events + lobby winner line), then return to the lobby on
         * the same countdown a back-to-lobby vote uses. */
        sim->returnToLobbyReason = RETURN_REASON_SURRENDER;
        sim->returnToLobbyTeamId = gv->teamId;
        sim->returnToLobbyTicks = 700;
    }
}

/* YES-vote count needed for the vote to pass, given the current
 * eligible voter pool and the configured pass percentage.
 *
 *   threshold = ceil(eligible * NUM / DENOM)
 *
 * For NUM/DENOM = 100/100 that's exact unanimity (== eligible). */
static uint8_t gameVoteThreshold(const ServerSim *sim, const struct ServerGameVote *gv) {
    uint32_t eligible = popcount16(gameVoteEligibleMask(sim, gv->kind, gv->teamId));
    if (eligible == 0) return 0;
    uint32_t num = (uint32_t)GAME_VOTE_PASS_PCT_NUM;
    uint32_t den = (uint32_t)GAME_VOTE_PASS_PCT_DENOM;
    /* ceil(eligible * num / den) */
    uint32_t thr = (eligible * num + (den - 1)) / den;
    if (thr > 0xFFu) thr = 0xFFu;
    return (uint8_t)thr;
}

/* Drop bits from votes/answered for slots that disappeared. */
static void gameVotePruneVotes(const ServerSim *sim, struct ServerGameVote *gv) {
    uint16_t elig = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
    gv->votesMask    &= elig;
    gv->answeredMask &= elig;
}

void serverSimGameVoteToggle(ServerSim *sim, uint8_t playerNum,
                             uint8_t kind, uint8_t toggleMode) {
    /* No lobby means no place to return to: a passed back-to-lobby /
     * surrender vote would only terminate (or, under map rotation,
     * blindly rotate) the server. Disable voting entirely in that mode. */
    if (!sim->lobbyEnabled) return;
    if (playerNum >= MAX_TANKS) return;
    if (!sim->playerConnected[playerNum]) return;
    if (sim->lobbyPlayers[playerNum].isBot) return;
    struct ServerGameVote *gv = gameVoteSlot(sim, kind);
    if (!gv) return;

    /* Only allow during running game. */
    if (sim->state != serverStateRunning) return;

    /* Reject malformed toggleMode bytes from the wire before any
     * state-mutating branch can react to them. The historical else-fall
     * treated anything that wasn't YES as NO, so 0xFF would be recorded
     * as a NO vote. */
    if (toggleMode != GAME_VOTE_TOGGLE_NO &&
        toggleMode != GAME_VOTE_TOGGLE_YES &&
        toggleMode != GAME_VOTE_TOGGLE_OPEN_ONLY) {
        return;
    }

    /* Surrender precondition: exactly two teams in play, and the
     * caller must be on a real team — an Unassigned (team 0) player
     * surrendering "team 0" would broadcast a fake side and chain a
     * back-to-lobby vote against two unrelated playing teams. */
    if (kind == GAME_VOTE_KIND_SURRENDER) {
        if (serverSimCountActiveTeams(sim) != 2) return;
        if (sim->lobbyPlayers[playerNum].teamNumber == 0) return;
    }

    uint64_t nowMs = sim->gameVoteWallMs;
    uint8_t teamId = (kind == GAME_VOTE_KIND_SURRENDER)
                     ? sim->lobbyPlayers[playerNum].teamNumber
                     : 0;

    /* A standalone NO has no effect when no vote is running. The
     * vote-start branch below would otherwise open a fresh vote and
     * record the caller as NO+answered, which is meaningless. Only
     * YES or OPEN_ONLY may open a vote. */
    if (gv->active != GAME_VOTE_ACTIVE_RUNNING &&
        toggleMode == GAME_VOTE_TOGGLE_NO) {
        return;
    }

    /* Open-only re-press: if a vote is running, just rebroadcast (so the
     * client can pop the widget back up); if no vote is running, start one
     * as if the caller voted yes. */
    if (toggleMode == GAME_VOTE_TOGGLE_OPEN_ONLY) {
        if (gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            publishGameVoteState(sim, kind);
            return;
        }
        gameVoteStart(sim, kind, GAME_VOTE_TRIGGER_MANUAL, teamId, nowMs, playerNum);
        gv->votesMask    |= (uint16_t)(1u << playerNum);
        gv->answeredMask |= (uint16_t)(1u << playerNum);
        logAddEvent(log_GameVoteCast, kind, playerNum, 1, 0, 0, NULL);

        /* Check whether opening + auto-YES already constitutes a pass.
         * Solo (threshold==1) starts the 5-s grace immediately so the
         * very first broadcast carries secondsRemaining=5 instead of
         * the 60-s timeout (otherwise the widget flashes "60s" before
         * the next heartbeat brings it down). Multi-voter unanimity
         * fires the pass right away. */
        uint8_t thr = gameVoteThreshold(sim, gv);
        uint8_t yes = popcount16(gv->votesMask);
        if (thr > 0 && yes >= thr) {
            if (thr == 1) {
                gv->pendingPassUntilMs = nowMs +
                    (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL;
                publishGameVoteState(sim, kind);
            } else {
                publishGameVoteState(sim, kind);
                gameVoteFirePass(sim, gv, nowMs);
            }
            return;
        }

        publishGameVoteState(sim, kind);
        return;
    }

    if (gv->active != GAME_VOTE_ACTIVE_RUNNING) {
        /* First voter starts the vote. */
        gameVoteStart(sim, kind, GAME_VOTE_TRIGGER_MANUAL, teamId, nowMs, playerNum);
    } else if (kind == GAME_VOTE_KIND_SURRENDER &&
               gv->teamId != sim->lobbyPlayers[playerNum].teamNumber) {
        /* Different-team player can't vote on a team's surrender. */
        return;
    }

    gv->answeredMask |= (uint16_t)(1u << playerNum);
    if (toggleMode == GAME_VOTE_TOGGLE_YES) {
        gv->votesMask |= (uint16_t)(1u << playerNum);
    } else {
        gv->votesMask &= (uint16_t)~(1u << playerNum);
    }
    logAddEvent(log_GameVoteCast, kind, playerNum,
                toggleMode == GAME_VOTE_TOGGLE_YES ? 1 : 0, 0, 0, NULL);

    gameVotePruneVotes(sim, gv);

    uint8_t thr = gameVoteThreshold(sim, gv);
    uint8_t yes = popcount16(gv->votesMask);

    if (thr > 0 && yes >= thr) {
        /* Solo voter ("am I sure?") path: one-human votes get a
         * 5-second grace before the effect applies so a misclick
         * is reversible. Multi-human votes fire instantly — by the
         * time everyone has agreed there's nothing to second-guess. */
        if (thr == 1) {
            if (gv->pendingPassUntilMs == 0) {
                gv->pendingPassUntilMs = nowMs +
                    (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL;
            }
            publishGameVoteState(sim, kind);
        } else {
            publishGameVoteState(sim, kind);
            gameVoteFirePass(sim, gv, nowMs);
        }
        return;
    }

    /* Lost unanimity during a solo grace — cancel the pending pass. */
    if (gv->pendingPassUntilMs != 0) {
        gv->pendingPassUntilMs = 0;
    }

    publishGameVoteState(sim, kind);

    /* Everyone answered but yes count didn't reach the pass
     * threshold → fail now instead of waiting for the timeout. */
    {
        uint16_t eligibleMask = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
        uint8_t eligible = popcount16(eligibleMask);
        uint8_t answered = popcount16(gv->answeredMask & eligibleMask);
        if (eligible > 0 && answered >= eligible && yes < thr) {
            gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_FAILED, nowMs);
        }
    }
}

void serverSimGameVoteTick(ServerSim *sim, uint64_t nowMs) {
    /* Voting only exists on lobby-enabled servers (see
     * serverSimGameVoteToggle). Skip the whole vote machinery when there
     * is no lobby. */
    if (!sim->lobbyEnabled) return;

    sim->gameVoteWallMs = nowMs;

    int k;
    for (k = 0; k < 2; k++) {
        struct ServerGameVote *gv = &sim->gameVotes[k];

        if (gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            /* Prune in case a voter disconnected. */
            gameVotePruneVotes(sim, gv);

            /* 60s timeout. */
            if (nowMs >= gv->deadlineMs) {
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_FAILED, nowMs);
                continue;
            }

            /* Surrender invalidation: team count must remain == 2. */
            if (gv->kind == GAME_VOTE_KIND_SURRENDER &&
                serverSimCountActiveTeams(sim) != 2) {
                publishServerMessageToTeam(sim, "Surrender vote cancelled — team count changed.", gv->teamId);
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_CANCELLED, nowMs);
                continue;
            }

            /* If the surrendering team or the voter pool has emptied
             * entirely (everyone disconnected), the vote is moot. */
            if (popcount16(gameVoteEligibleMask(sim, gv->kind, gv->teamId)) == 0) {
                if (gv->kind == GAME_VOTE_KIND_SURRENDER) {
                    publishServerMessageToTeam(sim,
                        "Surrender vote cancelled — surrendering team is empty.", gv->teamId);
                }
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_CANCELLED, nowMs);
                continue;
            }

            /* 1Hz heartbeat broadcast (drives client countdown display). */
            if (nowMs - gv->lastHeartbeatMs >= 1000ULL) {
                gv->lastHeartbeatMs = nowMs;
                publishGameVoteState(sim, gv->kind);
            }

            /* Re-check pass under the live eligible-mask. Eligibility
             * may have shrunk (disconnect) so unanimity can land here
             * without a fresh toggle. Solo (threshold==1) votes use
             * a 5-s grace; everything else fires immediately. */
            uint16_t eligibleMask = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
            uint8_t eligible = popcount16(eligibleMask);
            uint8_t answered = popcount16(gv->answeredMask & eligibleMask);
            uint8_t thr = gameVoteThreshold(sim, gv);
            uint8_t yes = popcount16(gv->votesMask);

            if (thr > 0 && yes >= thr) {
                if (thr == 1) {
                    if (gv->pendingPassUntilMs == 0) {
                        gv->pendingPassUntilMs = nowMs +
                            (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL;
                    }
                } else {
                    gameVoteFirePass(sim, gv, nowMs);
                    continue;
                }
            } else if (gv->pendingPassUntilMs != 0) {
                gv->pendingPassUntilMs = 0;
            }

            /* Solo pre-pass grace expired → fire for real. */
            if (gv->pendingPassUntilMs != 0 &&
                nowMs >= gv->pendingPassUntilMs) {
                gv->pendingPassUntilMs = 0;
                gameVoteFirePass(sim, gv, nowMs);
                continue;
            }

            /* Everyone eligible has answered but the yes side fell
             * short of the pass threshold → vote fails immediately,
             * no point waiting on the 60-s timeout. */
            if (eligible > 0 && answered >= eligible &&
                yes < thr && gv->pendingPassUntilMs == 0) {
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_FAILED, nowMs);
                continue;
            }
        }

    }
}

/* ---------------------------------------------------------------------- */
/* Subscriber registry                                                    */
/* ---------------------------------------------------------------------- */

/* SUBSCRIBER_SLOT_COUNT is defined in server_sim_internal.h (it sizes the
 * subscriber arrays on the ServerSim struct). */
#define SUBSCRIBER_HANDLE_ENCODE(slot, gen) (((int)(slot) << 16) | (uint16_t)(gen))
#define SUBSCRIBER_HANDLE_SLOT(h)           (((h) >> 16) & 0xFFFF)
#define SUBSCRIBER_HANDLE_GEN(h)            ((uint16_t)((h) & 0xFFFF))

static netStatus serverPhaseToNetStat(ServerState s) {
    switch (s) {
    case serverStateLobby:     return netLobby;
    case serverStateCountdown: return netLobbyCountdown;
    case serverStateRunning:   return netRunning;
    case serverStateGameOver:  return netLobby;
    }
    return netLobby;
}

void serverSimFillGamePhaseEvent(const ServerSim *sim, ControlEvent *evt) {
    switch (sim->state) {
    case serverStateLobby:     evt->type = CTRL_GAME_PHASE_LOBBY;     break;
    case serverStateCountdown: evt->type = CTRL_GAME_PHASE_COUNTDOWN; break;
    case serverStateRunning:   evt->type = CTRL_GAME_PHASE_RUNNING;   break;
    case serverStateGameOver:  evt->type = CTRL_GAME_PHASE_GAME_OVER; break;
    default:                   evt->type = CTRL_GAME_PHASE_LOBBY;     break;
    }
    /* countdownTicks is a 50Hz counter; round up so a partial second still
     * surfaces as 1 rather than 0 to a freshly-synced subscriber. */
    if (sim->countdownTicks > 0) {
        evt->u.gamePhase.countdownSeconds = (int)((sim->countdownTicks + 49) / 50);
    } else {
        evt->u.gamePhase.countdownSeconds = 0;
    }
}

void serverSimFillLobbySettingsEvent(ServerSim *sim, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_SETTINGS;
    memset(evt->u.lobbySettings.mapName, 0, MAP_STR_SIZE);
    snprintf(evt->u.lobbySettings.mapName, MAP_STR_SIZE, "%s", sim->mapName);
    evt->u.lobbySettings.lobbyGameType    = gameTypeGet(&sim->sim.game);
    evt->u.lobbySettings.lobbyHiddenMines = sim->sim.hiddenMines ? true : false;
    evt->u.lobbySettings.lobbyAiType      = (uint8_t)sim->botAiType;
    evt->u.lobbySettings.lobbyTimeLimit   = sim->gameLength;
    evt->u.lobbySettings.lobbyStartDelay  = serverSimGetStartDelay(sim);
    evt->u.lobbySettings.lobbyPillCount   = pillsGetNumPills(&sim->sim.pb);
    evt->u.lobbySettings.lobbyBaseCount   = basesGetNumBases(&sim->sim.bs);
    evt->u.lobbySettings.lobbyStartCount  = startsGetNumStarts(&sim->sim.ss);
    evt->u.lobbySettings.mapSkipAvailable =
        (sim->mapDirCount > 1 || sim->randomMapEnabled) ? true : false;
    evt->u.lobbySettings.netStat          = serverPhaseToNetStat(sim->state);
    evt->u.lobbySettings.hasLobby         = sim->lobbyEnabled ? true : false;
    evt->u.lobbySettings.lobbyOpenHost            = sim->openHost;
    evt->u.lobbySettings.hostSlot                 = sim->hostSlot;
    evt->u.lobbySettings.lobbyAutoLockOnGameStart = sim->autoLockOnGameStart;
    evt->u.lobbySettings.lobbyRanked              = sim->ranked;
    evt->u.lobbySettings.lobbyAllowNewPlayers     = sim->allowNewPlayers;
    evt->u.lobbySettings.lobbyWbnAvailable        = winbolonetIsRunning();
    evt->u.lobbySettings.lobbyServerLocks         = sim->serverLocks;
    evt->u.lobbySettings.uploadPolicy             = sim->uploadPolicy;
}

void serverSimFillLobbySlotEvent(ServerSim *sim, BYTE i, ControlEvent *evt) {
    ClientLobbySlot slot;
    memset(&slot, 0, sizeof(slot));
    slot.connected = sim->playerConnected[i] ? true : false;
    if (slot.connected) {
        const char *name = sim->sim.plyrs->item[i].playerName;
        strncpy(slot.playerName, name, PACKET_MAX_PLAYER_NAME - 1);
        slot.playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
        slot.teamNumber = sim->lobbyPlayers[i].teamNumber;
        slot.ready      = sim->lobbyPlayers[i].ready;
        slot.isBot      = sim->lobbyPlayers[i].isBot;
        slot.startIdx   = sim->lobbyPlayers[i].startIdx;
        /* sim->playerPing[i] is only refreshed by queueInput; in lobby
         * no inputs flow, so it sits at 0 the whole time. The PING/PONG
         * handler keeps udpServer.clients[i].pingMs live across every
         * state, so route through that for the lobby fill. */
        slot.pingMs     = transportUdpServerGetClientPing(i);
        slot.countryCode[0] = sim->sim.plyrs->item[i].location[0];
        slot.countryCode[1] = sim->sim.plyrs->item[i].location[1];
        slot.countryCode[2] = '\0';
        slot.clientType  = playersGetClientType(&sim->sim.plyrs, i);
        slot.clientFlags = playersGetClientFlags(&sim->sim.plyrs, i);
    }
    evt->type = CTRL_LOBBY_SLOT;
    evt->u.lobbySlot.playerNum = i;
    evt->u.lobbySlot.slot = slot;
}

void serverSimFillPlayerJoinEvent(ServerSim *sim, BYTE i, ControlEvent *evt) {
    PlayerBitMap allies = playersGetAlliesBitMap(&sim->sim.plyrs, i);
    BYTE numAllies = 0;
    BYTE bit;

    evt->type = CTRL_PLAYER_JOIN;
    evt->u.playerJoin.playerNum = i;
    memset(evt->u.playerJoin.name, 0, PACKET_MAX_PLAYER_NAME);
    strncpy(evt->u.playerJoin.name, sim->sim.plyrs->item[i].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    evt->u.playerJoin.country[0] = sim->sim.plyrs->item[i].location[0];
    evt->u.playerJoin.country[1] = sim->sim.plyrs->item[i].location[1];
    evt->u.playerJoin.country[2] = '\0';
    evt->u.playerJoin.clientType  = playersGetClientType(&sim->sim.plyrs, i);
    evt->u.playerJoin.clientFlags = playersGetClientFlags(&sim->sim.plyrs, i);
    for (bit = 0; bit < MAX_TANKS && numAllies < MAX_TANKS; bit++) {
        if (allies & ((PlayerBitMap)1u << bit)) {
            evt->u.playerJoin.allies[numAllies++] = bit;
        }
    }
    evt->u.playerJoin.numAllies = numAllies;
}

void serverSimFillPlayerLeaveEvent(ServerSim *sim, BYTE i, ControlEvent *evt) {
    evt->type = CTRL_PLAYER_LEAVE;
    evt->u.playerLeave.playerNum = i;
    memset(evt->u.playerLeave.name, 0, PACKET_MAX_PLAYER_NAME);
    strncpy(evt->u.playerLeave.name, sim->sim.plyrs->item[i].playerName,
            PACKET_MAX_PLAYER_NAME - 1);
    evt->u.playerLeave.country[0] = sim->sim.plyrs->item[i].location[0];
    evt->u.playerLeave.country[1] = sim->sim.plyrs->item[i].location[1];
    evt->u.playerLeave.country[2] = '\0';
}

void serverSimFillLobbyTeamMetaEvent(const ServerSim *sim, BYTE teamId, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_TEAM_META;
    evt->u.lobbyTeamMeta.teamId = teamId;
    if (teamId == 0 || teamId >= MAX_TANKS) {
        evt->u.lobbyTeamMeta.in_use     = 0;
        evt->u.lobbyTeamMeta.color      = 0;
        evt->u.lobbyTeamMeta.namingPool = 0;
        evt->u.lobbyTeamMeta.name[0]    = '\0';
        return;
    }
    evt->u.lobbyTeamMeta.in_use     = sim->teams[teamId].in_use;
    evt->u.lobbyTeamMeta.color      = sim->teams[teamId].color;
    evt->u.lobbyTeamMeta.namingPool = sim->teams[teamId].namingPool;
    memset(evt->u.lobbyTeamMeta.name, 0, LOBBY_TEAM_NAME_LEN);
    strncpy(evt->u.lobbyTeamMeta.name, sim->teams[teamId].name,
            LOBBY_TEAM_NAME_LEN - 1);
}

void serverSimFillLobbyBotConfigEvent(ServerSim *sim, BYTE slot, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_BOT_CONFIG;
    evt->u.lobbyBotConfig.slot = slot;
    memset(evt->u.lobbyBotConfig.name, 0, PACKET_MAX_PLAYER_NAME);
    if (slot >= MAX_TANKS) {
        evt->u.lobbyBotConfig.difficulty  = 0;
        evt->u.lobbyBotConfig.personality = 0;
        return;
    }
    evt->u.lobbyBotConfig.difficulty  = sim->botConfigs[slot].difficulty;
    evt->u.lobbyBotConfig.personality = sim->botConfigs[slot].personality;
    if (sim->playerConnected[slot]) {
        strncpy(evt->u.lobbyBotConfig.name,
                sim->sim.plyrs->item[slot].playerName,
                PACKET_MAX_PLAYER_NAME - 1);
    }
}

void serverSimFillLobbyBotBrainEvent(const ServerSim *sim, BYTE slot, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_BOT_BRAIN;
    evt->u.lobbyBotBrain.slot = slot;
    evt->u.lobbyBotBrain.brainIdx = 0xFF;
    if (slot >= MAX_TANKS) return;
    evt->u.lobbyBotBrain.brainIdx = sim->botBrainIdx[slot];
}

void serverSimFillLobbyBrainListEvent(const ServerSim *sim, ControlEvent *evt) {
    evt->type = CTRL_LOBBY_BRAIN_LIST;
    evt->u.lobbyBrainList.list = sim->brainList;
}

/* Fill a CTRL_GAME_VOTE_STATE event for the given vote kind. Returns false
 * if there's no snapshot (caller must not deliver). Mirrors the inline
 * publish at publishGameVoteState. */
static bool serverSimFillGameVoteStateEvent(const ServerSim *sim, uint8_t kind,
                                            ControlEvent *evt) {
    ServerGameVoteSnapshot snap;
    if (!serverSimGetGameVoteSnapshot(sim, kind, &snap)) return false;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_GAME_VOTE_STATE;
    evt->u.gameVoteState.kind             = snap.kind;
    evt->u.gameVoteState.active           = snap.active;
    evt->u.gameVoteState.triggerSrc       = snap.triggerSrc;
    evt->u.gameVoteState.teamId           = snap.teamId;
    evt->u.gameVoteState.threshold        = snap.threshold;
    evt->u.gameVoteState.yesCount         = snap.yesCount;
    evt->u.gameVoteState.noCount          = snap.noCount;
    evt->u.gameVoteState.eligibleCount    = snap.eligibleCount;
    evt->u.gameVoteState.secondsRemaining = snap.secondsRemaining;
    evt->u.gameVoteState.votes            = snap.votes;
    return true;
}

/* Fill a CTRL_BALANCE_PROPOSAL event with the current proposed team
 * assignment. Mirrors the publish at server_lifecycle.c. */
static void serverSimFillBalanceProposalEvent(const ServerSim *sim,
                                              ControlEvent *evt) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_BALANCE_PROPOSAL;
    memcpy(evt->u.balanceProposal.teamForSlot,
           sim->balanceProposal.teamForSlot, MAX_TANKS);
}

/* Fill a CTRL_MAP_SKIP_STATE event with the current per-slot skip votes.
 * Mirrors the inline builders at the join + map-change publish sites. */
static void serverSimFillMapSkipStateEvent(const ServerSim *sim,
                                           ControlEvent *evt) {
    BYTE k;
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_MAP_SKIP_STATE;
    for (k = 0; k < MAX_TANKS; k++) {
        evt->u.mapSkipState.votes[k] = sim->mapSkipVotes[k] ? 1 : 0;
    }
}

/* Wrapper used to enforce the documented sync ordering:
 *   a CTRL_GAME_PHASE_* event first; CTRL_PLAYER_JOIN events last
 *   (a regression that reorders sync would silently mis-initialize a
 *   subscriber, so catch it loudly in debug builds). Asserts compile
 *   out under NDEBUG.
 */
typedef struct {
    void (*inner)(void *, const struct ControlEvent *);
    void *innerCtx;
    bool sawNonPhase;
    bool sawPlayerJoin;
} SyncOrderingCheck;

static void serverSimSyncOrderingDeliver(void *ctx,
                                         const struct ControlEvent *evt) {
    SyncOrderingCheck *check = (SyncOrderingCheck *)ctx;
    bool isPhase = (evt->type == CTRL_GAME_PHASE_LOBBY ||
                    evt->type == CTRL_GAME_PHASE_COUNTDOWN ||
                    evt->type == CTRL_GAME_PHASE_RUNNING ||
                    evt->type == CTRL_GAME_PHASE_GAME_OVER);

    if (isPhase) {
        assert(!check->sawNonPhase &&
               "a CTRL_GAME_PHASE_* event must be the first event in sync");
    } else {
        check->sawNonPhase = true;
    }

    if (evt->type != CTRL_PLAYER_JOIN &&
        evt->type != CTRL_LOBBY_SYNC_COMPLETE) {
        assert(!check->sawPlayerJoin &&
               "no non-CTRL_PLAYER_JOIN event may follow "
               "CTRL_PLAYER_JOIN in sync");
    } else {
        check->sawPlayerJoin = true;
    }

    check->inner(check->innerCtx, evt);
}

static void serverSimSyncSubscriber(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    ControlEvent evt;
    BYTE i;
    SyncOrderingCheck check;
    {
        int connectedCount = 0;
        char connectedSlots[64];
        int csPos = 0;
        connectedSlots[0] = '\0';
        for (i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i]) {
                connectedCount++;
                if (csPos < (int)sizeof(connectedSlots) - 8) {
                    csPos += snprintf(connectedSlots + csPos,
                                      sizeof(connectedSlots) - csPos,
                                      "%s%d", csPos == 0 ? "" : ",", (int)i);
                }
            }
        }
        mpDiagLog("[bus] SYNC-REPLAY begin state=%d connectedCount=%d connectedSlots=[%s]",
                  (int)sim->state, connectedCount, connectedSlots);
    }

    check.inner         = deliver;
    check.innerCtx      = ctx;
    check.sawNonPhase   = false;
    check.sawPlayerJoin = false;
    deliver = serverSimSyncOrderingDeliver;
    ctx     = &check;

    memset(&evt, 0, sizeof(evt));
    serverSimFillGamePhaseEvent(sim, &evt);
    deliver(ctx, &evt);

    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    deliver(ctx, &evt);

    /* BrainList (~900 bytes) is only used by the lobby AiConfig combobox.
     * Mid-game joiners don't need it during sync replay; embedding it in a
     * snapshot would risk exceeding MTU room.  The game-over → lobby
     * transition re-publishes it so the mid-game joiner gets it before
     * the lobby UI needs it. */
    if (sim->state == serverStateLobby || sim->state == serverStateCountdown) {
        memset(&evt, 0, sizeof(evt));
        serverSimFillLobbyBrainListEvent(sim, &evt);
        deliver(ctx, &evt);

        /* Bot-pool catalog: the server's themed naming pools (loaded from
         * -botnames / data/bot_names.json), zlib-compressed and streamed
         * as CTRL_LOBBY_BOT_POOL_CHUNK fragments so the joiner renders and
         * picks from the SERVER's pools rather than its own shipped file.
         * Same lobby-only gate as the brain list. */
        {
            unsigned char *blob =
                (unsigned char *)malloc(LOBBY_BOT_CATALOG_WIRE_MAX);
            if (blob) {
                int blen = lobbyBotPoolsSerialize(blob,
                                                  (int)LOBBY_BOT_CATALOG_WIRE_MAX);
                if (blen > 0) {
                    int frag = LOBBY_BOT_POOL_CHUNK_FRAG_MAX;
                    int nChunks = (blen + frag - 1) / frag;
                    int off = 0, ci;
                    if (nChunks <= 255) {
                        for (ci = 0; ci < nChunks; ci++) {
                            int fl = blen - off;
                            if (fl > frag) fl = frag;
                            memset(&evt, 0, sizeof(evt));
                            evt.type = CTRL_LOBBY_BOT_POOL_CHUNK;
                            evt.u.lobbyBotPoolChunk.seq     = (uint8_t)ci;
                            evt.u.lobbyBotPoolChunk.count   = (uint8_t)nChunks;
                            evt.u.lobbyBotPoolChunk.fragLen = (uint16_t)fl;
                            memcpy(evt.u.lobbyBotPoolChunk.frag, blob + off,
                                   (size_t)fl);
                            deliver(ctx, &evt);
                            off += fl;
                        }
                    }
                }
                free(blob);
            }
        }
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbySlotEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Spectator roster — one CTRL_SPECTATOR_SLOT per connected spectator. The
     * roster lives in the transport layer, so the sim asks the registered
     * enumerator to emit the rows through this same deliver path. Feeds both the
     * live sync replay and serverSimSerializeControlSnapshot (the delayed ring
     * keyframe). */
    if (sim->specRosterEnum != NULL) {
        sim->specRosterEnum(sim->specRosterEnumCtx, deliver, ctx);
    }

    /* Team metadata for every team in use (skip team 0 — unassigned). */
    for (i = 1; i < MAX_TANKS; i++) {
        if (sim->teams[i].in_use) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbyTeamMetaEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Bot config + brain for each connected bot slot. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].isBot) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbyBotConfigEvent(sim, i, &evt);
            deliver(ctx, &evt);

            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbyBotBrainEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Game vote state per kind. Each fill returns false when no snapshot
     * exists, so an inactive vote contributes nothing to the replay. */
    {
        if (serverSimFillGameVoteStateEvent(sim, GAME_VOTE_KIND_BACK_TO_LOBBY, &evt)) {
            deliver(ctx, &evt);
        }
        if (serverSimFillGameVoteStateEvent(sim, GAME_VOTE_KIND_SURRENDER, &evt)) {
            deliver(ctx, &evt);
        }
    }

    /* Balance proposal — only emitted when one is pending (matches the
     * predicate the join handler uses to dismiss the proposal). */
    if (sim->balanceProposal.pending) {
        serverSimFillBalanceProposalEvent(sim, &evt);
        deliver(ctx, &evt);
    }

    /* Map skip state — emitted under the same gate as the inline publish:
     * lobby phase with a map-skip pool available. */
    if (sim->lobbyEnabled && sim->state == serverStateLobby
        && (sim->mapDirCount > 1 || sim->randomMapEnabled)) {
        serverSimFillMapSkipStateEvent(sim, &evt);
        deliver(ctx, &evt);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (playersIsInUse(&sim->sim.plyrs, i) == TRUE) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillPlayerJoinEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
    }

    /* Terminal marker: the roster replay above re-announces every existing
     * player/slot with the subscriber already in the lobby. This final event
     * lets the subscriber tell the replay burst apart from live events, so it
     * can suppress per-event lobby sounds until the burst is done. Must be the
     * last event delivered in the sync. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SYNC_COMPLETE;
    deliver(ctx, &evt);
    mpDiagLog("[bus] SYNC-REPLAY end");
}

/* Buffer-writing sink for serverSimSerializeControlSnapshot: each delivered
 * sync event is body-encoded and appended as a [u16 type][u16 bodyLen][body]
 * record. */
typedef struct {
    BYTE *out;
    int   cap;
    int   len;
    bool  overflow;
} ControlSnapshotSink;

static void serverSimControlSnapshotDeliver(void *ctx,
                                            const struct ControlEvent *evt) {
    ControlSnapshotSink *s = (ControlSnapshotSink *)ctx;
    ControlEncodeBodyFn fn;
    uint8_t body[MAX_CONTROL_PACKET];
    size_t bodyLen = 0;
    EncodeResult r;
    int recLen;

    if (s->overflow) {
        return;  /* already failed; drain the rest of the replay silently */
    }
    fn = transportControlCodecBodyEncoder(evt->type);
    if (fn == NULL) {
        return;  /* no body codec for this kind — nothing to record */
    }
    /* Body encoders ignore the recipient (per-recipient filtering lives in the
     * delivery path), so NULL is safe here. */
    r = fn(evt, NULL, body, sizeof(body), &bodyLen);
    if (r == ENCODE_SKIP) {
        return;  /* event has no form here (e.g. an invalid slot) — skip */
    }
    if (r != ENCODE_OK) {
        s->overflow = true;  /* ENCODE_OVERFLOW: body did not fit MAX_CONTROL_PACKET */
        return;
    }
    recLen = 4 + (int)bodyLen;
    if (s->len > s->cap - recLen) {
        s->overflow = true;
        return;
    }
    s->out[s->len++] = (BYTE)(((uint16_t)evt->type >> 8) & 0xFF);
    s->out[s->len++] = (BYTE)((uint16_t)evt->type & 0xFF);
    s->out[s->len++] = (BYTE)(((uint16_t)bodyLen >> 8) & 0xFF);
    s->out[s->len++] = (BYTE)((uint16_t)bodyLen & 0xFF);
    memcpy(s->out + s->len, body, bodyLen);
    s->len += (int)bodyLen;
}

int serverSimSerializeControlSnapshot(ServerSim *sim, BYTE *out, int cap) {
    ControlSnapshotSink sink;

    if (sim == NULL || out == NULL || cap < 0) {
        return -1;
    }
    sink.out      = out;
    sink.cap      = cap;
    sink.len      = 0;
    sink.overflow = false;
    serverSimSyncSubscriber(sim, serverSimControlSnapshotDeliver, &sink);
    return sink.overflow ? -1 : sink.len;
}

/* serverSimReplayLobbyChat — re-deliver the current-session lobby-chat buffer
 * oldest->newest through the caller's deliver callback. Mirrors the sync /
 * roster-enumerator inversion: the sim owns the buffer, the transport supplies
 * delivery. Invoked only at the spectator drain-flip (after the re-register's
 * sync replay has set the lobby phase, so the events land in lobbyChatHistory);
 * never on a fresh accept or player join, which is what makes it
 * drain-flip-only. */
void serverSimReplayLobbyChat(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    int i;
    if (sim == NULL || deliver == NULL) return;
    for (i = 0; i < sim->lobbyChatCount; i++) {
        deliver(ctx, &sim->lobbyChatBuffer[i]);
    }
}

SubscriberHandle serverSimRegisterSubscriber(
    ServerSim *sim,
    void (*deliver)(void *, const struct ControlEvent *),
    void *ctx) {
    int i;
    int slot = -1;

    if (sim == NULL || deliver == NULL) {
        return SUBSCRIBER_HANDLE_INVALID;
    }

    for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
        if (sim->subscribers[i].deliver == NULL) {
            slot = i;
            break;
        }
    }
    if (slot < 0) {
        return SUBSCRIBER_HANDLE_INVALID;
    }

    assert(sim->subscriberGen[slot] < UINT16_MAX);
    sim->subscriberGen[slot]++;

    /* Fire the sync replay BEFORE inserting the slot into the bus
     * array, so that any side-effect publish triggered by a deliver
     * during replay reaches the existing subscribers only — not this
     * new one mid-way through its own snapshot.
     *
     * (The serverSimPublishControl reentrancy assert (!sim->publishing)
     * is the primary guard against deliver-publishes-during-replay; this
     * ordering is defence-in-depth so a release build that bypasses the
     * assert still gives the new subscriber a coherent replay rather
     * than an interleaved one.)
     *
     * The deliver function must already be ready to receive events at
     * this point — for the wire transport that means
     * udpServer.clients[slot] is fully populated before the caller
     * invokes serverSimRegisterSubscriber. */
    serverSimSyncSubscriber(sim, deliver, ctx);

    sim->subscribers[slot].deliver    = deliver;
    sim->subscribers[slot].ctx        = ctx;
    sim->subscribers[slot].generation = sim->subscriberGen[slot];
    sim->numSubscribers++;

    return SUBSCRIBER_HANDLE_ENCODE(slot, sim->subscriberGen[slot]);
}

static void serverSimDeliverToClientSim(void *ctx, const struct ControlEvent *evt) {
    /* In-process bus delivery — the host's local ClientSim (and bots) land here.
     * Log so we can tell whether the host's view divergence is at the publish
     * stage or the wire stage. */
    char extra[160];
    extra[0] = '\0';
    if (evt->type == CTRL_LOBBY_SLOT) {
        snprintf(extra, sizeof(extra),
                 " lobbySlot[player=%d team=%d ready=%d connected=%d isBot=%d name='%.12s']",
                 (int)evt->u.lobbySlot.playerNum,
                 (int)evt->u.lobbySlot.slot.teamNumber,
                 (int)evt->u.lobbySlot.slot.ready,
                 (int)evt->u.lobbySlot.slot.connected,
                 (int)evt->u.lobbySlot.slot.isBot,
                 evt->u.lobbySlot.slot.playerName);
    } else if (evt->type == CTRL_PLAYER_JOIN) {
        snprintf(extra, sizeof(extra),
                 " playerJoin[player=%d name='%.16s']",
                 (int)evt->u.playerJoin.playerNum,
                 evt->u.playerJoin.name);
    }
    mpDiagLog("[bus] in-process deliver cs=%p type=%d%s",
              ctx, (int)evt->type, extra);
    clientSimApplyControl((ClientSim *)ctx, evt);
}

SubscriberHandle serverSimRegisterClientSubscriber(ServerSim *sim, ClientSim *cs) {
    return serverSimRegisterSubscriber(sim, serverSimDeliverToClientSim, cs);
}

void serverSimSetSpectatorRosterEnumerator(ServerSim *sim,
                                           SpectatorRosterEnumFn fn,
                                           void *enumCtx) {
    if (sim == NULL) return;
    sim->specRosterEnum    = fn;
    sim->specRosterEnumCtx = enumCtx;
}

void serverSimRequestBalanceProposal(ServerSim *sim,
                                     uint8_t totalPlayers,
                                     uint8_t teamSize,
                                     const uint8_t *botSlots,
                                     uint8_t numBotSlots) {
    winbolonetServerRequestBalance(totalPlayers, teamSize,
                                   botSlots, numBotSlots,
                                   &sim->balanceProposal);
}

void serverSimUnregisterSubscriber(ServerSim *sim, SubscriberHandle h) {
    int slot;
    uint16_t gen;

    if (sim == NULL || h == SUBSCRIBER_HANDLE_INVALID) {
        return;
    }
    slot = SUBSCRIBER_HANDLE_SLOT(h);
    gen  = SUBSCRIBER_HANDLE_GEN(h);
    if (slot < 0 || slot >= SUBSCRIBER_SLOT_COUNT) {
        return;
    }
    if (sim->subscribers[slot].deliver == NULL ||
        sim->subscribers[slot].generation != gen) {
        return;
    }
    sim->subscribers[slot].deliver    = NULL;
    sim->subscribers[slot].ctx        = NULL;
    sim->subscribers[slot].generation = 0;
    if (sim->numSubscribers > 0) {
        sim->numSubscribers--;
    }
}

void serverSimAcceptAlliance(ServerSim *sim, BYTE accepter, BYTE newMember) {
    GameSim *gs;
    ControlEvent evt;
    if (sim == NULL) {
        return;
    }
    gs = serverSimGetGameSim(sim);
    playersAcceptAlliance(gs, &gs->plyrs, NEUTRAL, accepter, newMember, TRUE);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_ACCEPT;
    evt.u.allianceAccept.acceptedBy = accepter;
    evt.u.allianceAccept.newMember  = newMember;
    serverSimPublishControl(sim, &evt);
    /* WBN tracker + replay-log side effects live here so every input
     * source (UDP wire, local transport, headless cmd-stdin) fires
     * them uniformly. winbolonetAddEvent is gated internally by
     * winbolonetIsRunning(), so SP / non-WBN-aware builds pay nothing.
     * logAddEvent is gated by whether a replay log is open. */
    winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_JOIN, TRUE, accepter, newMember,
                       botManagerIsBot(sim, accepter), botManagerIsBot(sim, newMember));
    logAddEvent(log_AllyAccept, accepter, newMember, 0, 0, 0, NULL);
}

void serverSimLeaveAlliance(ServerSim *sim, BYTE playerNum) {
    GameSim *gs;
    ControlEvent evt;
    if (sim == NULL) {
        return;
    }
    gs = serverSimGetGameSim(sim);
    playersLeaveAlliance(gs, &gs->plyrs, NEUTRAL, playerNum, TRUE);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_LEAVE;
    evt.u.allianceLeave.playerNum = playerNum;
    serverSimPublishControl(sim, &evt);
    /* WBN + replay-log side effects — see serverSimAcceptAlliance. */
    winbolonetAddEvent(WINBOLO_NET_EVENT_ALLY_LEAVE, TRUE,
                       playerNum, WINBOLO_NET_NO_PLAYER,
                       botManagerIsBot(sim, playerNum), FALSE);
    logAddEvent(log_AllyLeave, playerNum, 0, 0, 0, 0, NULL);
}

void serverSimSetPlayerName(ServerSim *sim, BYTE playerNum, const char *name) {
    GameSim *gs;
    ControlEvent evt;
    char nameBuf[PACKET_MAX_PLAYER_NAME];
    if (sim == NULL || name == NULL) {
        return;
    }
    gs = serverSimGetGameSim(sim);
    strncpy(nameBuf, name, sizeof(nameBuf) - 1);
    nameBuf[sizeof(nameBuf) - 1] = '\0';
    playersSetPlayerName(NULL, gs, &gs->plyrs, NEUTRAL, playerNum, nameBuf, TRUE);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_NAME;
    evt.u.playerName.playerNum = playerNum;
    snprintf(evt.u.playerName.name, PACKET_MAX_PLAYER_NAME, "%s", nameBuf);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishControl(ServerSim *sim, const struct ControlEvent *evt) {
    ControlSubscriber snapshot[SUBSCRIBER_SLOT_COUNT];
    int snapCount = 0;
    int i;

    if (sim == NULL || evt == NULL) {
        return;
    }

    /* Spectator lobby-chat catch-up capture. EVERY chat reaches the bus through
     * here — the wire CMD_CHAT dispatcher and the serverSimReceiveChat funnel
     * both publish via this one call — so capturing here (not in either caller)
     * is the single chokepoint that catches both. Broadcast player chat
     * (destPlayer 0xFF) + spectator chat, lobby/countdown only. */
    if ((sim->state == serverStateLobby || sim->state == serverStateCountdown) &&
        ((evt->type == CTRL_CHAT && evt->u.chat.destPlayer == 0xFF) ||
         evt->type == CTRL_SPECTATOR_CHAT)) {
        serverSimBufferLobbyChat(sim, evt);
    }

    /* Reentrancy guard: a deliver callback that triggers another publish
     * is a design error. */
    assert(!sim->publishing);

    /* Server is not a subscriber: double-mutating sim itself would corrupt
     * already-applied state. */
    for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
        assert(sim->subscribers[i].ctx != sim);
    }

    sim->publishing = true;

    /* Iterate a snapshot of the active list so a deliver that
     * registers/unregisters does not corrupt our walk. */
    for (i = 0; i < SUBSCRIBER_SLOT_COUNT; i++) {
        if (sim->subscribers[i].deliver != NULL) {
            snapshot[snapCount++] = sim->subscribers[i];
        }
    }
    {
        char extra[256];
        extra[0] = '\0';
        if (evt->type == CTRL_LOBBY_SLOT) {
            snprintf(extra, sizeof(extra),
                     " lobbySlot[player=%d team=%d ready=%d connected=%d isBot=%d name='%.12s']",
                     (int)evt->u.lobbySlot.playerNum,
                     (int)evt->u.lobbySlot.slot.teamNumber,
                     (int)evt->u.lobbySlot.slot.ready,
                     (int)evt->u.lobbySlot.slot.connected,
                     (int)evt->u.lobbySlot.slot.isBot,
                     evt->u.lobbySlot.slot.playerName);
        } else if (evt->type == CTRL_PLAYER_JOIN) {
            snprintf(extra, sizeof(extra),
                     " playerJoin[player=%d name='%.16s']",
                     (int)evt->u.playerJoin.playerNum,
                     evt->u.playerJoin.name);
        } else if (evt->type == CTRL_LOBBY_SETTINGS) {
            snprintf(extra, sizeof(extra),
                     " settings[map='%.16s' gameType=%d hiddenMines=%d aiType=%d timeLimit=%d startDelay=%d open=%d autoLock=%d ranked=%d allowNew=%d locks=0x%04x]",
                     evt->u.lobbySettings.mapName,
                     (int)evt->u.lobbySettings.lobbyGameType,
                     (int)evt->u.lobbySettings.lobbyHiddenMines,
                     (int)evt->u.lobbySettings.lobbyAiType,
                     (int)evt->u.lobbySettings.lobbyTimeLimit,
                     (int)evt->u.lobbySettings.lobbyStartDelay,
                     (int)evt->u.lobbySettings.lobbyOpenHost,
                     (int)evt->u.lobbySettings.lobbyAutoLockOnGameStart,
                     (int)evt->u.lobbySettings.lobbyRanked,
                     (int)evt->u.lobbySettings.lobbyAllowNewPlayers,
                     (unsigned)evt->u.lobbySettings.lobbyServerLocks);
        }
        mpDiagLog("[bus] PUBLISH type=%d subscribers=%d%s",
                  (int)evt->type, snapCount, extra);
    }
    /* Make sim visible to deliver callbacks that recover it via
     * serverSimGetActive() (e.g. udpClientDeliverControl's enqueue
     * diagnostic + running-phase early-send gate). The main thread
     * already sets this per tick; worker threads (WBN balance) have
     * NULL in their TLS slot until we set it here. */
    activeSim = sim;
    for (i = 0; i < snapCount; i++) {
        snapshot[i].deliver(snapshot[i].ctx, evt);
    }

    sim->publishing = false;
}

/* ────────────────────────────────────────────────────────────────
 * Lobby Layout A accessors / mutators / publish helpers
 * ──────────────────────────────────────────────────────────────── */

void serverSimSetBotBrainIdxFor(ServerSim *sim, BYTE slot, uint8_t brainIdx) {
    if (!sim || slot >= MAX_TANKS) return;
    /* 0xFF is the "use server default" sentinel; any other in-range
     * value indexes into the catalogue. Out-of-range is a no-op,
     * matching the existing "ignore malformed input" pattern. */
    if (brainIdx != 0xFF && brainIdx >= sim->brainList.count) return;
    sim->botBrainIdx[slot] = brainIdx;
    serverSimPublishLobbyBotBrain(sim, slot);
    lobbyAutoUnreadyOnChange(sim);
}

const char *serverSimGetBrainPathForIdx(const ServerSim *sim, uint8_t brainIdx) {
    if (!sim) return NULL;
    if (brainIdx == 0xFF) return sim->botBrainPath;
    if (brainIdx >= sim->brainList.count) return NULL;
    return sim->brainPaths[brainIdx];
}

void serverSimSetBotConfig(ServerSim *sim, BYTE slot,
                            uint8_t difficulty, uint8_t personality,
                            const char *validatedName) {
    if (!sim || slot >= MAX_TANKS) return;
    {
        LobbyBotConfig *bc = serverSimGetBotConfigMut(sim, slot);
        if (bc) {
            bc->difficulty  = difficulty;
            bc->personality = personality;
        }
    }
    if (validatedName != NULL && validatedName[0] != '\0') {
        serverSimRenameBotSlot(sim, slot, validatedName);
    }
    serverSimPublishLobbyBotConfig(sim, slot);
    serverSimPublishLobbySlot(sim, slot);
    lobbyAutoUnreadyOnChange(sim);
}

void serverSimSwitchBotBrain(ServerSim *sim, BYTE slot, uint8_t brainIdx) {
    if (!sim || slot >= MAX_TANKS) return;
    serverSimSetBotBrainIdxFor(sim, slot, brainIdx);
    botManagerSetBrainIdx(sim, slot, sim->botBrainIdx[slot]);
}

void serverSimRenameBotSlot(ServerSim *sim, BYTE slot, const char *name) {
    if (!sim || slot >= MAX_TANKS || !name) return;
    {
        char nameBuf[32];
        char loc[3] = "??";
        SDL_strlcpy(nameBuf, name, sizeof(nameBuf));
        playersSetPlayer(NULL, &sim->sim.plyrs, NEUTRAL, slot,
                         nameBuf, loc,
                         0, 0, 0, 0, 0, FALSE, 0, NULL, TRUE);
    }
    /* Subscriber ClientSims track names in sim.plyrs (what the in-game
     * players panel reads), not in lobbySlots. Publish so the rename
     * propagates past the lobby UI into the game view. */
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_PLAYER_NAME;
        evt.u.playerName.playerNum = slot;
        snprintf(evt.u.playerName.name, PACKET_MAX_PLAYER_NAME, "%s", name);
        serverSimPublishControl(sim, &evt);
    }
}

void serverSimPublishLobbySlot(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySlotEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishLobbyBotBrain(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyBotBrainEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishLobbyBotConfig(ServerSim *sim, BYTE slot) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyBotConfigEvent(sim, slot, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimPublishLobbyTeamMeta(ServerSim *sim, BYTE teamId) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbyTeamMetaEvent(sim, teamId, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimSetTeamMeta(ServerSim *sim, BYTE teamId,
                           uint8_t color, uint8_t namingPool,
                           const uint8_t *name, uint8_t nameLen) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
    TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
    if (t == NULL) return;
    t->in_use = 1;
    t->color = color;
    /* Per-team uniqueness on namingPool: if another in_use team
     * already owns this pool, pick the lowest pool index not
     * used by any other team. Falls back to the requested value
     * if every pool is taken. */
    {
        int poolCount = lobbyBotPoolCount();
        bool poolTaken = false;
        for (BYTE other = 1; other < MAX_TANKS; other++) {
            if (other == teamId) continue;
            const TeamMetadata *ot = serverSimGetTeamMetaMut(sim, other);
            if (ot && ot->in_use && ot->namingPool == namingPool) {
                poolTaken = true;
                break;
            }
        }
        if (poolTaken && poolCount > 0) {
            for (int p = 0; p < poolCount; p++) {
                bool used = false;
                for (BYTE other = 1; other < MAX_TANKS; other++) {
                    if (other == teamId) continue;
                    const TeamMetadata *ot = serverSimGetTeamMetaMut(sim, other);
                    if (ot && ot->in_use && ot->namingPool == p) {
                        used = true;
                        break;
                    }
                }
                if (!used) { namingPool = (uint8_t)p; break; }
            }
        }
    }
    t->namingPool = namingPool;
    memset(t->name, 0, LOBBY_TEAM_NAME_LEN);
    if (nameLen > 0 && name != NULL) {
        memcpy(t->name, name, nameLen);
    }
    serverSimPublishLobbyTeamMeta(sim, teamId);
    lobbyAutoUnreadyOnChange(sim);
}

void serverSimClearTeamMeta(ServerSim *sim, BYTE teamId) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return;
    TeamMetadata *t = serverSimGetTeamMetaMut(sim, teamId);
    if (t != NULL) {
        memset(t, 0, sizeof(TeamMetadata));
    }
    serverSimPublishLobbyTeamMeta(sim, teamId);
    lobbyAutoUnreadyOnChange(sim);
}

void serverSimPublishLobbySettings(ServerSim *sim) {
    ControlEvent evt;
    if (!sim) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillLobbySettingsEvent(sim, &evt);
    serverSimPublishControl(sim, &evt);
}

/* Shared apply path for the LST_* setting cluster carried in
 * PACKET_LOBBY_SET_SETTING and its SP-host local-transport
 * equivalent. The caller is responsible for upstream lock-bit /
 * authority gates; on success this helper publishes
 * CTRL_LOBBY_SETTINGS and clears humans' ready state before
 * returning true.
 *
 * Returns true if the setting was applied, false if the payload
 * was malformed, out of range, or rejected by a cross-setting
 * invariant (e.g. ranked forbids gameOpen / non-aiNone / autoLock
 * off). */
static bool serverSimApplyLobbySettingInner(ServerSim *sim,
                                            uint8_t lst,
                                            const uint8_t *value, size_t len) {
    if (sim == NULL || value == NULL) return false;
    switch (lst) {
        case LST_GAME_TYPE:
            if (len != 1 || value[0] < 1 || value[0] > 3) return false;
            if (serverSimGetRanked(sim) &&
                (gameType)value[0] == gameOpen) return false;
            serverSimSetGameType(sim, (gameType)value[0]);
            return true;
        case LST_HIDDEN_MINES:
            if (len != 1) return false;
            serverSimSetHiddenMines(sim, value[0] != 0);
            return true;
        case LST_AI_POLICY:
            if (len != 1 || value[0] > 3) return false;
            if (serverSimGetRanked(sim) &&
                (aiType)value[0] != aiNone) return false;
            serverSimSetAiPolicy(sim, value[0]);
            serverSimSetBotAiType(sim, (aiType)value[0]);
            if ((aiType)value[0] == aiNone) {
                for (BYTE bi = 0; bi < MAX_TANKS; bi++) {
                    if (botManagerIsBot(sim, bi)) {
                        serverSimRemoveBot(sim, bi);
                    }
                }
            }
            return true;
        case LST_TIME_LIMIT: {
            if (len != 1) return false;
            bool tl = value[0] != 0;
            serverSimSetTimeLimit(sim, tl);
            if (tl) {
                uint16_t mins = serverSimGetTimeMinutes(sim) > 0
                    ? serverSimGetTimeMinutes(sim) : 30;
                serverSimSetGameLength(sim,
                    (int32_t)mins * 60 * GAME_NUMGAMETICKS_SEC);
            } else {
                serverSimSetGameLength(sim, UNLIMITED_GAME_TIME);
            }
            return true;
        }
        case LST_TIME_MINUTES: {
            if (len != 2) return false;
            uint16_t mins = (uint16_t)((value[0] << 8) | value[1]);
            if (!lobbyTimeMinutesIsValid(mins)) return false;
            serverSimSetTimeMinutes(sim, mins);
            if (serverSimGetTimeLimit(sim)) {
                serverSimSetGameLength(sim,
                    (int32_t)mins * 60 * GAME_NUMGAMETICKS_SEC);
            }
            return true;
        }
        case LST_AUTO_LOCK_ON_GAME: {
            if (len != 1) return false;
            bool v = value[0] != 0;
            if (serverSimGetRanked(sim) && !v) return false;
            serverSimSetAutoLockOnGameStart(sim, v);
            return true;
        }
        case LST_RANKED: {
            if (len != 1) return false;
            bool r = value[0] != 0;
            serverSimSetRanked(sim, r);
            if (r) {
                serverSimSetAiPolicy(sim, (uint8_t)aiNone);
                serverSimSetBotAiType(sim, aiNone);
                for (BYTE bi = 0; bi < MAX_TANKS; bi++) {
                    if (botManagerIsBot(sim, bi)) {
                        serverSimRemoveBot(sim, bi);
                    }
                }
                if (serverSimGetGameType(sim) == gameOpen) {
                    serverSimSetGameType(sim, gameTournament);
                }
                if (!serverSimGetAutoLockOnGameStart(sim)) {
                    serverSimSetAutoLockOnGameStart(sim, true);
                }
            }
            return true;
        }
        default:
            return false;
    }
}

bool serverSimApplyLobbySetting(ServerSim *sim,
                                uint8_t lst,
                                const uint8_t *value, size_t len) {
    if (!serverSimApplyLobbySettingInner(sim, lst, value, len)) return false;
    serverSimPublishLobbySettings(sim);
    lobbyAutoUnreadyOnChange(sim);
    serverSimWbnLobbyUpdate(sim, FALSE);
    return true;
}

/* Reserve a free lobby start for one slot, storing it in lobbyStartIdx.
 * No-op (leaves startIdx at 0xFF) outside lobby state, for an unconnected
 * slot, or when the lobby map has no starts. Builds the taken set from
 * every other connected slot's reservation and the teammate set from
 * same-team holders, then picks a clustered (or farthest-first when
 * teamless) start. The slot's own current reservation is ignored, so
 * this is safe to call to re-pick a slot that already holds one. Does
 * not publish — callers republish the slot (the join/team-set/add paths
 * already do, and the map-change reconcile publishes reassigned slots),
 * which keeps the reservation out of the add-time event stream. Runs for
 * humans and bots alike. */
void serverSimAssignLobbyStartOnJoin(ServerSim *sim, BYTE slot) {
    BYTE numStarts;
    bool taken[MAX_STARTS];
    BYTE teammateStarts0[MAX_TANKS];
    int  teammateCount = 0;
    BYTE myTeam;
    BYTE picked;
    BYTE i;
    BYTE k;

    if (sim == NULL) return;
    if (sim->state != serverStateLobby) return;
    if (slot >= MAX_TANKS) return;
    if (!sim->playerConnected[slot]) return;
    numStarts = startsGetNumStarts(&sim->sim.ss);
    if (numStarts == 0) return;

    for (i = 0; i < MAX_STARTS; i++) {
        taken[i] = false;
    }
    myTeam = sim->lobbyPlayers[slot].teamNumber;

    /* taken[] = every connected slot's reservation (1-based -> 0-based);
     * teammateStarts0[] = same-team holders' reservations. Team 0
     * (unassigned) has no teammates, so it falls to farthest-first. */
    for (k = 0; k < MAX_TANKS; k++) {
        BYTE r;
        if (k == slot) continue; /* never count our own current reservation */
        if (!sim->playerConnected[k]) continue;
        r = sim->lobbyPlayers[k].startIdx;
        if (r == 0xFF) continue;
        if (r < 1 || r > numStarts) continue;
        taken[r - 1] = true;
        if (myTeam != 0 && sim->lobbyPlayers[k].teamNumber == myTeam) {
            teammateStarts0[teammateCount++] = (BYTE)(r - 1);
        }
    }

    picked = startsPickIncremental(&sim->sim, &sim->sim.ss, taken,
                                   teammateStarts0, teammateCount);
    if (picked >= numStarts) {
        sim->lobbyPlayers[slot].startIdx = 0xFF;
    } else {
        sim->lobbyPlayers[slot].startIdx = (BYTE)(picked + 1);
    }
}

/* Auto-unready: any meaningful lobby change clears every human's
 * ready flag and aborts an in-flight countdown. The per-slot
 * CTRL_LOBBY_SLOT publishes (plus the CTRL_GAME_PHASE_LOBBY publish
 * if the countdown was aborted) fan out to both in-process subscribers and
 * remote UDP clients via the codec — no wire-only blast needed. Bots
 * stay permanently ready by design (set in botManagerAddBot) so the
 * next all-ready check still triggers a countdown when the human
 * re-confirms. */
void lobbyAutoUnreadyOnChange(ServerSim *sim) {
    BYTE i;
    bool countdownWasRunning = (serverSimGetState(sim) == serverStateCountdown);
    bool toggled[MAX_TANKS];

    for (i = 0; i < MAX_TANKS; i++) {
        const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, i);
        toggled[i] = false;
        if (lp == NULL) continue;
        if (lp->isBot) continue;
        if (lp->ready) {
            serverSimSetReady(sim, i, false);
            toggled[i] = true;
        }
    }

    if (countdownWasRunning) {
        /* serverSimAbortCountdown publishes the CTRL_GAME_PHASE_LOBBY
         * transition itself; no separate publish needed here. */
        serverSimAbortCountdown(sim);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (toggled[i]) {
            serverSimPublishLobbySlot(sim, i);
        }
    }
}

const BrainList *serverSimGetBrainList(const ServerSim *sim) {
    return sim ? &sim->brainList : NULL;
}

bool serverSimRankedShapeReady(const ServerSim *sim) {
    if (sim == NULL) return false;
    int teamSizes[17] = {0};
    int teamsInUse = 0;
    for (int i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsPlayerConnected(sim, i)) continue;
        const LobbyPlayer *lp = serverSimGetLobbyPlayer(sim, i);
        if (lp == NULL || lp->isBot) continue;
        uint8_t t = lp->teamNumber;
        if (t == 0 || t > 16) continue;
        if (teamSizes[t] == 0) teamsInUse++;
        teamSizes[t]++;
    }
    int firstSize = 0, secondSize = 0;
    for (int t = 1; t <= 16; t++) {
        if (teamSizes[t] == 0) continue;
        if (firstSize == 0) firstSize = teamSizes[t];
        else                secondSize = teamSizes[t];
    }
    return (teamsInUse == 2) &&
           (firstSize == secondSize) &&
           (firstSize >= 1 && firstSize <= 3);
}

#ifdef WB_NETDEBUG
void serverSimNetdebugResetCounters(ServerSim *sim) {
    if (sim == NULL) return;
    memset(sim->dbgExecTurnTicks, 0, sizeof(sim->dbgExecTurnTicks));
    memset(sim->dbgMineLays, 0, sizeof(sim->dbgMineLays));
}

uint32_t serverSimNetdebugGetExecTurnTicks(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return 0;
    return sim->dbgExecTurnTicks[playerNum];
}

uint32_t serverSimNetdebugGetMineLays(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return 0;
    return sim->dbgMineLays[playerNum];
}
#endif

