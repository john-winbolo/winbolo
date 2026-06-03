/*
 * Copyright (c) 1998-2008 John Morrison.
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
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"
#include "control_event.h"
#include "client_sim.h"
#include "client_sim_control.h"
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

/* Viewport culling — margin in map squares beyond the visible 15×15 screen */
#define SNAPSHOT_SCREEN_SIZE 15
#define SNAPSHOT_VIEWPORT_MARGIN 20
#define MAX_VIEWPORTS (1 + MAX_SNAPSHOT_PILLS)  /* tank + owned pills */

typedef struct {
    int minMX, maxMX, minMY, maxMY;
} ViewportRect;

static bool inAnyViewport(const ViewportRect *vps, int count, int mx, int my) {
    int i;
    for (i = 0; i < count; i++) {
        if (mx >= vps[i].minMX && mx <= vps[i].maxMX &&
            my >= vps[i].minMY && my <= vps[i].maxMY) {
            return true;
        }
    }
    return false;
}

/* Forward declaration for server console message callback */
extern void serverMessageConsoleMessage(ServerSim *sim, char *msg);

/* Forward declarations for snapshot helpers used before their definitions */
static int serverSimGetBases(ServerSim *sim, BaseSnapshot *out, int maxOut);
static int serverSimGetPills(ServerSim *sim, PillSnapshot *out, int maxOut);

/* Forward declarations for lobby functions used before their definitions */
void serverSimLobbyCheckAllReady(ServerSim *sim);
void serverSimStartGame(ServerSim *sim);

/* Every server-side map mutator calls this at the end of its success
 * path; defined alongside lobbyAutoUnreadyOnChange below. */
static void serverSimApplyMapChange(ServerSim *sim);

/* Forward declarations for map-skip publish path — definitions live
 * further down the file. */
static void serverSimFillMapSkipStateEvent(const ServerSim *sim,
                                           ControlEvent *evt);
static void publishMapSkipState(ServerSim *sim);

/* Active sim pointer — when non-NULL, servercore.c routing functions
 * access sim state directly instead of using legacy globals. */
static THREAD_LOCAL ServerSim *activeSim = NULL;

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
    return activeSim;
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

/* Map an assistant body lang ID to the wire ID carried by
 * EVENT_ASSISTANT_MSG. Wire format unchanged — clients still receive
 * [targetPlayer, msgId] and resolve back to the matching LGM_* /
 * MESSAGE_* string locally. Returns 0 for any non-assistant body. */
static uint8_t assistantBodyIdToWireId(langid bodyId) {
    switch (bodyId) {
        case LGM_MAN_DEAD:               return ASSIST_MSG_MAN_DEAD;
        case LGM_NO_TREE:                return ASSIST_MSG_NO_TREE;
        case LGM_NO_BUILD:               return ASSIST_MSG_NO_BUILD;
        case LGM_NO_BUILD_UNDER_BOAT:    return ASSIST_MSG_NO_BUILD_BOAT;
        case LGM_INSUFFICIENT_TREES:     return ASSIST_MSG_INSUFFICIENT_TREES;
        case LGM_BUILDTANK:              return ASSIST_MSG_BUILDTANK;
        case LGM_PILL_NO_NEED_REPAIR:    return ASSIST_MSG_PILL_NO_REPAIR;
        case LGM_NO_PILLS:               return ASSIST_MSG_NO_PILLS;
        case LGM_INSUFFICIENT_MINES:     return ASSIST_MSG_INSUFFICIENT_MINES;
        case LGM_PILL_NO_BUILD_ON_MINE:  return ASSIST_MSG_PILL_ON_MINE;
        case MESSAGE_TANKSUNK:           return ASSIST_MSG_TANK_SUNK;
        default:                         return 0;
    }
}

/* Server-side messageAdd callback. Only assistant messages turn into
 * EVENT_ASSISTANT_MSG events; other message types (newswire, chat, AI)
 * are client-local — the server has no listener for them, so they are
 * silently dropped. */
static void serverSimCbMessageAdd(void *ctx, messageType msgType,
                                  langid topId, langid bodyId,
                                  const MessageArgs *args) {
    ServerSim *sim = (ServerSim *)ctx;
    (void)topId;
    (void)args;

    if (msgType == assistantMessage) {
        uint8_t msgId = assistantBodyIdToWireId(bodyId);
        if (msgId != 0) {
            GameEvent ev;
            ev.type = EVENT_ASSISTANT_MSG;
            memset(ev.data, 0, sizeof(ev.data));
            ev.data[0] = sim->currentTickPlayer;
            ev.data[1] = msgId;
            serverSimAddEvent(sim, &ev);
        }
    }
}

static void serverSimCbSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_SOUND;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)value;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = sim->currentTickPlayer;
    serverSimAddEvent(sim, &ev);
}

static void serverSimCbSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_SOUND_SHOOT;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)shootNear;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = owner;
    serverSimAddEvent(sim, &ev);
}

static void serverSimCbSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_SOUND_TANK_HIT;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)hitTankNear;
    ev.data[1] = mx;
    ev.data[2] = my;
    ev.data[3] = hitPlayer;
    serverSimAddEvent(sim, &ev);
}

static void serverSimCbMineVisible(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_MINE_VISIBLE;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = mx;
    ev.data[1] = my;
    ev.data[2] = sourcePlayer;
    serverSimAddEvent(sim, &ev);
}

static void serverSimCbExplosion(void *ctx, BYTE mx, BYTE my, BYTE px, BYTE py) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_EXPLOSION;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = mx;
    ev.data[1] = my;
    ev.data[2] = px;
    ev.data[3] = py;
    serverSimAddEvent(sim, &ev);
}

static void serverSimCbTkExplosion(void *ctx, WORLD x, WORLD y,
                                   TURNTYPE angle, BYTE length,
                                   BYTE explodeType, BYTE creator) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_TK_EXPLOSION;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = (uint8_t)((x >> 8) & 0xFF);
    ev.data[1] = (uint8_t)(x & 0xFF);
    ev.data[2] = (uint8_t)((y >> 8) & 0xFF);
    ev.data[3] = (uint8_t)(y & 0xFF);
    ev.data[4] = (uint8_t)angle;
    ev.data[5] = length;
    ev.data[6] = explodeType;
    ev.data[7] = creator;
    serverSimAddEvent(sim, &ev);
}

static void serverSimCbTankKill(void *ctx, BYTE killer, BYTE killed, BYTE deathCause, BYTE carriedPills) {
    ServerSim *sim = (ServerSim *)ctx;
    GameEvent ev;
    ev.type = EVENT_TANK_KILLED;
    memset(ev.data, 0, sizeof(ev.data));
    ev.data[0] = killer;
    ev.data[1] = killed;
    ev.data[2] = deathCause;
    ev.data[3] = carriedPills;
    serverSimAddEvent(sim, &ev);
    winbolonetAddEvent(WINBOLO_NET_EVENT_TANK_KILL, TRUE, killer, killed,
                       botManagerIsBot(sim, killer), botManagerIsBot(sim, killed));
    logAddEvent(log_KillPlayer, killed, killer, 0, 0, 0, NULL);
    logAddEvent(log_PlayerDied, killed, 0, 0, 0, 0, NULL);
}

static void serverSimCbCenterTank(void *ctx) {
    (void)ctx;
    /* No-op on server */
}

static void serverSimCbConsoleMessage(void *ctx, char *msg) {
    ServerSim *sim = (ServerSim *)ctx;
    serverMessageConsoleMessage(sim, msg);
}

static void serverSimInit(ServerSim *sim, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen) {
    BYTE count;

    memset(sim, 0, sizeof(ServerSim));

    /* Sentinel value for "no batch start assigned" — memset gives 0, but 0
     * is a valid start index, so initialise explicitly. */
    for (count = 0; count < MAX_TANKS; count++) {
        sim->sim.pendingStartIdx[count] = MAX_STARTS;
    }

    /* Sentinel "use the CLI-configured default brain" for every slot.
     * 0 is a valid brain-catalogue index, so memset doesn't suffice. */
    memset(sim->botBrainIdx, 0xFF, sizeof(sim->botBrainIdx));

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
    sim->state = serverStateLobby;
    sim->lobbyEnabled = TRUE;
    sim->countdownTicks = 0;
    sim->hadPlayersEver = FALSE;
    sim->quitOnWin = FALSE;
    sim->autoCloseOnEmpty = FALSE;
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

bool serverSimRandomMapRegenerate(ServerSim *sim) {
    BYTE tempBuf[65536];
    int len;
    char seedStr[64];
    char msg[128];
    int x, y;
    MapGenConfig cfg;

    if (!sim->randomMapEnabled) return FALSE;
    if (sim->state != serverStateLobby) return FALSE;

    cfg = sim->randomMapConfig;

    if (!sim->randomMapFixedSeed) {
        /* Generate a new random seed */
        cfg.seed = (uint32_t)time(NULL) ^ ((uint32_t)clock() << 16);
        if (cfg.seed == 0) cfg.seed = 1;
    }
    /* else: keep the same seed for reproducible maps */

    /* Clear map */
    memset((*sim->sim.mp).mapItem, DEEP_SEA, sizeof((*sim->sim.mp).mapItem));

    /* Fill mine border */
    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
            }
        }
    }

    /* Clear and regenerate objects */
    sim->sim.pb->numPills = 0;
    sim->sim.bs->numBases = 0;
    sim->sim.ss->numStarts = 0;
    mapGenRun(sim->sim.mp, sim->sim.bs, sim->sim.pb, sim->sim.ss, &cfg);

    /* Run generated objects through the same init path as file-loaded maps */
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

    /* Update cached map */
    len = serverSimGetCompressedMap(sim, tempBuf);
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData == NULL) {
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    /* Update map name */
    mapGenConfigToSeed(&cfg, seedStr, sizeof(seedStr));
    snprintf(sim->mapName, MAP_STR_SIZE, "rand_%.30s", seedStr);

    /* Store updated config */
    sim->randomMapConfig = cfg;

    snprintf(msg, sizeof(msg), "Random map regenerated, seed: %s", seedStr);
    serverSimConsoleMessage(msg);

    serverSimApplyMapChange(sim);
    return TRUE;
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

    /* Clear the active sim pointer if it points to this sim */
    if (activeSim == sim) {
        activeSim = NULL;
    }

    free(sim);
}

static void serverSimLogTick(ServerSim *sim) {
    BYTE count;

    if (logIsRecording() == FALSE) return;

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

    logWriteTick();
}

/* One half-step of the sim: dequeues one input per player and runs
 * world systems on game ticks (sim->tick % 2 == 0).  Two consecutive
 * half-steps form one 20ms frame and match the client's 100Hz keys/
 * game alternation.  Event buffers (sim->events, sim->mapEvents) are
 * NOT cleared here — that happens once at the top of serverSimTick so
 * events from both half-steps accumulate naturally into one frame's
 * worth of state for downstream consumers (UDP drain, in-process
 * snapshot poll). */
static void simRunHalfStep(ServerSim *sim) {
    BYTE count;
    bool isKeysTick;
    BYTE numTanks;
    tank tanksArray[MAX_TANKS];
    lgm *lgmPtrs[MAX_TANKS];
    InputPacket currentInputs[MAX_TANKS];
    bool hasInput[MAX_TANKS];

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

    /* Set active sim so servercore.c routing functions access sim state directly */
    activeSim = sim;

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
            serverSimSetSuppressNextWinMessage(sim, true);
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
        while (sim->inputQueueHead[count] != sim->inputQueueTail[count]) {
            uint8_t tail = sim->inputQueueTail[count] & (SERVER_INPUT_QUEUE_SIZE - 1);
            currentInputs[count] = sim->inputQueue[count][tail];
            sim->inputQueueTail[count]++;
            if (currentInputs[count].tick > sim->lastProcessedInput[count]) {
                hasInput[count] = TRUE;
                break;
            }
        }

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
                }
            }
        }

        /* If queue drained completely, re-enter buffering mode */
        if (sim->inputQueueHead[count] == sim->inputQueueTail[count] && !hasInput[count]) {
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
            bool inputIsKeys = (currentInputs[count].tick % 2) == 1;
            tankButton tb = translateInputToTankButton(currentInputs[count].buttons);

            /* Fill in gap ticks lost to packet loss.  When a UDP packet
             * is dropped, the dequeued tick jumps ahead (e.g. 98 → 101).
             * The missing ticks must still run so the turn ramp (firstLeft/
             * firstRight) stays in sync with the client's prediction. */
            {
                uint32_t expected = sim->lastProcessedInput[count] + 1;
                uint32_t gap = currentInputs[count].tick - expected;
                if (gap > 0 && gap < 8) {
                    tankButton gapTb = translateInputToTankButton(sim->lastInputButtons[count]);
                    uint32_t gt;
                    for (gt = expected; gt < currentInputs[count].tick; gt++) {
                        bool gapIsKeys = (gt % 2) == 1;
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
            sim->lastInputButtons[count] = currentInputs[count].buttons;

            /* Apply autoslowdown state from client flags */
            tankSetAutoSlowdown(&sim->sim.tanks[count],
                                (currentInputs[count].flags & INPUT_FLAG_AUTOSLOW) != 0);

            /* Apply gunsight adjustment (bits 2-3 of flags) */
            {
                uint8_t gsAdj = (currentInputs[count].flags & INPUT_FLAG_GUNSIGHT_MASK) >> INPUT_FLAG_GUNSIGHT_SHIFT;
                if (gsAdj == 1) {
                    tankGunsightIncrease(NULL, &sim->sim, &sim->sim.tanks[count]);
                } else if (gsAdj == 2) {
                    tankGunsightDecrease(NULL, &sim->sim, &sim->sim.tanks[count]);
                }
            }

            sim->lastProcessedInput[count] = currentInputs[count].tick;

            if (inputIsKeys) {
                /* Keys tick: turning only */
                BYTE bmx = tankGetMX(&sim->sim.tanks[count]);
                BYTE bmy = tankGetMY(&sim->sim.tanks[count]);
                tankTurn(&sim->sim, &sim->sim.tanks[count], bmx, bmy, tb);
            } else {
                /* Game tick: full update (turning + accel + movement) */
                bool shoot = (currentInputs[count].actions & INPUT_ACTION_FIRE) != 0;
                {
                    uint16_t pingMs = sim->playerPing[count];
                    uint16_t delayMs = (pingMs / 2) + INTERP_BUFFER_MS;
                    uint8_t compTicks = (uint8_t)(delayMs / 20);
                    if (compTicks > LAG_COMP_MAX_TICKS) compTicks = LAG_COMP_MAX_TICKS;
                    sim->sim.lagCompTicks = compTicks;
                }
                tankUpdate(&sim->sim, &sim->sim.tanks[count], tb, shoot, FALSE);

                /* Handle mine laying */
                if (currentInputs[count].actions & INPUT_ACTION_LAY_MINE) {
                    tankLayMine(&sim->sim, &sim->sim.tanks[count]);
                }

                /* Handle LGM build requests.  buildAction is 1-based in
                 * InputPacket (0=none, 1=BsTrees, 2=BsRoad, ...) but
                 * lgmAddRequest expects 0-based enum values. */
                if (currentInputs[count].buildAction != 0) {
                    lgmAddRequest(&sim->sim, &sim->sim.lgmen[count],
                                  &sim->sim.tanks[count],
                                  currentInputs[count].buildX,
                                  currentInputs[count].buildY,
                                  currentInputs[count].buildAction - 1);
                }
            }
        } else {
            /* No input available — repeat last known buttons so the tank
             * continues turning/moving as the client predicts.  Using TNONE
             * here would reset firstLeft/firstRight (turn ramp), causing
             * the server to turn slower than the client predicted and
             * producing visible angle "pull back" on reconciliation. */
            tankButton stallTb = translateInputToTankButton(sim->lastInputButtons[count]);
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

        /* Build arrays for multi-tank subsystem updates */
        numTanks = 0;
        for (count = 0; count < MAX_TANKS; count++) {
            if (sim->playerConnected[count] && sim->sim.tanks[count] != NULL) {
                tanksArray[numTanks] = sim->sim.tanks[count];
                lgmPtrs[numTanks] = &sim->sim.lgmen[count];
                numTanks++;
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

        /* Update world systems */
        tkExplosionUpdate(&sim->sim, lgmPtrs, numTanks, &sim->sim.tanks[0], &sim->sim.ss);
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
                ev.data[4] = currentPills[p].armour;
                ev.data[5] = currentPills[p].speed;
                ev.data[6] = currentPills[p].inTank;
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
            if (memcmp(&currentBases[b], &sim->prevBases[b], sizeof(BaseSnapshot)) != 0) {
                GameEvent ev;
                ev.type = EVENT_BASE_UPDATE;
                memset(ev.data, 0, sizeof(ev.data));
                ev.data[0] = (uint8_t)b;
                ev.data[1] = currentBases[b].owner;
                ev.data[2] = currentBases[b].armour;
                ev.data[3] = currentBases[b].shells;
                ev.data[4] = currentBases[b].mines;
                serverSimAddEvent(sim, &ev);
            }
        }
        memcpy(sim->prevBases, currentBases, nb * sizeof(BaseSnapshot));
        sim->prevBaseCount = (uint8_t)nb;
    }

    /* Check game-win condition. Fires when one alliance owns every
     * base above the capture-armour threshold. Lobby-enabled rounds
     * always check — winning ends the round and returns to lobby via
     * the existing serverSimEnterGameOver path. quitOnWin is the
     * legacy dedicated-server flag that also shuts the process down
     * when no lobby is configured. */
    if ((sim->quitOnWin || sim->lobbyEnabled) &&
        serverSimCheckGameWin(sim, TRUE)) {
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
            serverSimSetSuppressNextWinMessage(sim, true);
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

void addPlayerInternal(ServerSim *sim, BYTE playerNum, const char *playerName, bool wantRejoin) {
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
    sim->inputBufferFilled[playerNum] = 0;
    sim->jitterTarget[playerNum] = JITTER_BUFFER_DEFAULT;
    sim->jitterStallCount[playerNum] = 0;
    sim->jitterStableTicks[playerNum] = 0;

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
        {
            char pstr[256];
            int nameLen = (int)strlen(playerName);
            BYTE accountFlags = playersGetAccountFlags(&sim->sim.plyrs, playerNum);
            if (nameLen > 255) nameLen = 255;
            pstr[0] = (char)nameLen;
            memcpy(pstr + 1, playerName, nameLen);
            logAddEvent(log_PlayerJoined, playerNum, '?', '?', accountFlags, 0, pstr);
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
    addPlayerInternal(sim, playerNum, playerName, wantRejoin);
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

    addPlayerInternal(sim, (BYTE)slot, validatedName, false);
    setPlayerCountryInternal(sim, (BYTE)slot, country);
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
    if (playerNum >= MAX_TANKS) return;
    {
        char nm[PLAYER_NAME_LEN];
        playersGetPlayerName(&sim->sim.plyrs, playerNum, nm, TRUE);
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
    sim->playerPing[playerNum] = 0;
    sim->inputBufferFilled[playerNum] = 0;
    sim->jitterTarget[playerNum] = JITTER_BUFFER_DEFAULT;
    sim->jitterStallCount[playerNum] = 0;
    sim->jitterStableTicks[playerNum] = 0;

    /* Record ownership for rejoin before migration changes it */
    {
        char pName[PLAYER_NAME_LEN];
        PlayerBitMap pillBits = 0, baseBits = 0;
        BYTE numPills = pillsGetNumPills(&sim->sim.pb);
        BYTE numBases = basesGetNumBases(&sim->sim.bs);
        BYTE i;
        playersGetPlayerName(&sim->sim.plyrs, playerNum, pName, TRUE);
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
    sim->lastFullSyncTick = 0;

    /* Clear lobby state */
    sim->lobbyPlayers[playerNum].teamNumber = 0;
    sim->lobbyPlayers[playerNum].ready = FALSE;
    sim->lobbyPlayers[playerNum].isBot = FALSE;
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
}

bool serverSimAddBot(ServerSim *sim, BYTE playerNum,
                     const ServerSimBotConfig *cfg) {
    if (cfg == NULL || cfg->brainPath == NULL) {
        return false;
    }
    if (playerNum >= MAX_TANKS) {
        return false;
    }
    if (sim->playerConnected[playerNum]) {
        return false;
    }

    /* Stamp PLAYER_FLAG_BOT before addPlayerInternal so the log_PlayerJoined
     * event it emits, and the CTRL_PLAYER_JOIN that fillAndPublishPlayerJoin
     * fans out, both carry the bot identity. A bot slot inherits no human
     * identity bits — set rather than OR. */
    playersSetClientFlags(&sim->sim.plyrs, playerNum, PLAYER_FLAG_BOT);
    addPlayerInternal(sim, playerNum, cfg->brainName, false);
    fillAndPublishPlayerJoin(sim, playerNum);

    sim->lobbyPlayers[playerNum].isBot      = true;
    sim->lobbyPlayers[playerNum].ready      = true;
    sim->lobbyPlayers[playerNum].teamNumber = cfg->teamNumber;
    return true;
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

void serverSimSetBotAiType(ServerSim *sim, aiType ai) {
    sim->botAiType = ai;
}

/* Bot pool wrappers — forward to bot_manager.c. Declarations
 * live in server_sim.h so non-server callers don't include
 * bot_manager.h directly. */

bool serverSimBotPoolInit(int threads) {
    return botManagerInit(threads);
}

void serverSimBotPoolDestroy(void) {
    botWorkerPoolDestroy();
}

void serverSimRequestBotThreads(ServerSim *sim, int total_runners) {
    botManagerRequestThreads(sim, total_runners);
}

int serverSimGetBotThreads(ServerSim *sim) {
    return botManagerGetThreads(sim);
}

int serverSimGetPendingBotThreads(ServerSim *sim) {
    return botManagerGetPendingThreads(sim);
}

void serverSimSetBotDefaultDebugMode(ServerSim *sim, bool enabled) {
    botManagerSetDefaultDebugMode(sim, enabled);
}

void serverSimSetBotPreThinkHook(ServerSim *sim,
                                 void (*hook)(int playerNum)) {
    botManagerSetPreThinkHook(sim, hook);
}

bool serverSimCreateBot(ServerSim *sim, BYTE playerNum,
                        const char *brainPath, const char *brainName,
                        aiType ai, gameType game, bool hiddenMines) {
    return botManagerAddBot(sim, playerNum, brainPath, brainName,
                            ai, game, hiddenMines);
}

void serverSimRemoveBot(ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return;
    botManagerRemoveBot(sim, playerNum);
    serverSimPublishLobbySlot(sim, playerNum);
}

void serverSimDestroyBots(ServerSim *sim) {
    botManagerDestroy(sim);
}

void serverSimSetBotTeams(ServerSim *sim,
                          const BYTE *teamOf, BYTE numPlayers) {
    botManagerSetTeams(sim, teamOf, numPlayers);
}

void serverSimBotTick(ServerSim *sim, aiType ai) {
    botManagerTick(sim, ai);
}

BYTE serverSimGetNumBots(ServerSim *sim) {
    return botManagerGetNumBots(sim);
}

bool serverSimHasAnyBot(ServerSim *sim) {
    return botManagerHasAnyBot(sim);
}

bool serverSimIsBot(ServerSim *sim, BYTE playerNum) {
    return botManagerIsBot(sim, playerNum);
}

double serverSimGetBotLastThinkMs(ServerSim *sim, BYTE playerNum) {
    return botManagerGetLastThinkMs(sim, playerNum);
}

bool serverSimGetBotInfo(ServerSim *sim, BYTE playerNum, BotInfo *out) {
    return botManagerGetBotInfo(sim, playerNum, out);
}

void serverSimGetBotPoolStats(ServerSim *sim, BotPoolStats *out) {
    botManagerGetPoolStats(sim, out);
}

bool serverSimToggleAllBrainDebugMode(ServerSim *sim) {
    return botManagerToggleAllBrainDebugMode(sim);
}

int serverSimGetActiveBotCount(ServerSim *sim) {
    return botManagerGetActiveBotCount(sim);
}

BrainPathfinder *serverSimGetBotBrainPathfinder(ServerSim *sim, BYTE playerNum) {
    return botManagerGetBrainPathfinder(sim, playerNum);
}

OverlayCmdBuffer *serverSimGetBotOverlayCmds(ServerSim *sim, BYTE playerNum) {
    return botManagerGetOverlayCmds(sim, playerNum);
}

bool serverSimGetBotGoalInfo(ServerSim *sim, BYTE playerNum,
                             BrainGoalInfo *out) {
    return botManagerGetGoalInfo(sim, playerNum, out);
}

bool serverSimBotExecLua(ServerSim *sim, BYTE playerNum, const char *src) {
    return botManagerExecLua(sim, playerNum, src);
}

char *serverSimBotEvalLuaString(ServerSim *sim, BYTE playerNum,
                                const char *src) {
    return botManagerEvalLuaString(sim, playerNum, src);
}

void serverSimSetBotBrainPath(ServerSim *sim, const char *path) {
    if (path == NULL || path[0] == '\0') {
        sim->botBrainPath[0] = '\0';
        return;
    }
    strncpy(sim->botBrainPath, path, sizeof(sim->botBrainPath) - 1);
    sim->botBrainPath[sizeof(sim->botBrainPath) - 1] = '\0';
}

void serverSimSetEmptyResetEnabled(ServerSim *sim, bool enabled) {
    sim->emptyResetEnabled = enabled;
}

void serverSimSetHasPassword(ServerSim *sim, bool hasPassword) {
    if (sim == NULL) return;
    sim->hasPassword = hasPassword;
    /* Settings publish only — password changes are allowed mid-game
     * (they only gate new joiners) and must NOT clear humans' ready
     * state or abort an in-flight countdown. */
    serverSimPublishLobbySettings(sim);
}

void serverSimSetPassword(ServerSim *sim, const char *pw, size_t len) {
    if (sim == NULL) return;
    memset(sim->password, 0, sizeof(sim->password));
    if (pw != NULL && len > 0) {
        if (len > sizeof(sim->password) - 1) len = sizeof(sim->password) - 1;
        memcpy(sim->password, pw, len);
        sim->password[len] = '\0';
    }
}

const char *serverSimGetPassword(const ServerSim *sim) {
    if (sim == NULL) return "";
    return sim->password;
}

void serverSimSetLobbyEnabled(ServerSim *sim, bool enabled) {
    sim->lobbyEnabled = enabled;
}

void serverSimEnableRandomMap(ServerSim *sim,
                              const MapGenConfig *cfg,
                              bool fixedSeed) {
    sim->randomMapEnabled = true;
    if (cfg != NULL) sim->randomMapConfig = *cfg;
    sim->randomMapFixedSeed = fixedSeed;
}

void serverSimEnterLobby(ServerSim *sim) {
    sim->state = serverStateLobby;
}

void serverSimApplyInstanceConfig(ServerSim *sim, const ServerInstanceConfig *cfg) {
  sim->sim.viewPlayer = cfg->viewPlayer;

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
  } else if (cfg->lobbyEnabled) {
    serverSimSetLobbyEnabled(sim, true);
    serverSimEnterLobby(sim);
  }
}

void serverSimInstallMapDirList(ServerSim *sim,
                                char **files, int count) {
    sim->mapDirFiles = files;
    sim->mapDirCount = count;
}

void serverSimSetAutoCloseOnEmpty(ServerSim *sim, bool enabled) {
    sim->autoCloseOnEmpty = enabled;
}

void serverSimSetBalanceBroadcastNeeded(ServerSim *sim, bool needed) {
    sim->balanceProposal.broadcastNeeded = needed;
}

void serverSimSetBalanceRequestInFlight(ServerSim *sim, bool inFlight) {
    sim->balanceProposal.requestInFlight = inFlight;
}

void serverSimSetBalanceIncludeBots(ServerSim *sim, bool includeBots) {
    sim->balanceProposal.includeBots = includeBots;
}

void serverSimSetEmptyResetMinutes(ServerSim *sim, int minutes) {
    sim->emptyResetMinutes = minutes;
}

void serverSimSetMapName(ServerSim *sim, const char *name) {
    /* Name only — the map md5 tracks content, not the display name, so
     * it is set/cleared by the content-load paths (serverSimReloadMap,
     * serverSimChangeMap, serverSimReloadCompressedInMemory) and the
     * revert path. Touching it here would wipe a freshly-computed hash
     * when callers fix up the display name after a content load. */
    if (name == NULL || name[0] == '\0') {
        sim->mapName[0] = '\0';
        return;
    }
    strncpy(sim->mapName, name, MAP_STR_SIZE - 1);
    sim->mapName[MAP_STR_SIZE - 1] = '\0';
}

void serverSimSetMessageLogFile(ServerSim *sim, const char *path) {
    if (path == NULL || path[0] == '\0') {
        sim->serverMessageLogFile[0] = '\0';
        sim->serverMessageUseLogFile = FALSE;
        return;
    }
    strncpy(sim->serverMessageLogFile, path,
            sizeof(sim->serverMessageLogFile) - 1);
    sim->serverMessageLogFile[sizeof(sim->serverMessageLogFile) - 1] = '\0';
    sim->serverMessageUseLogFile = TRUE;
}

void serverSimSetQuiet(ServerSim *sim, bool quiet) {
    sim->isServerQuiet = quiet;
}

void serverSimSetQuitOnWin(ServerSim *sim, bool enabled) {
    sim->quitOnWin = enabled;
}

void serverSimSetServerPort(ServerSim *sim, unsigned short port) {
    sim->serverPort = port;
}

void serverSimSetTickLimit(ServerSim *sim, int32_t ticks) {
    sim->tickLimit = ticks;
}

void serverSimSetGameTickLimit(ServerSim *sim, int32_t ticks) {
    sim->gameTickLimit = ticks;
    sim->gameTicksRun = 0;
}

void serverSimSetUserLogFileName(ServerSim *sim, const char *name) {
    if (name == NULL || name[0] == '\0') {
        sim->userLogFileName[0] = '\0';
        return;
    }
    strncpy(sim->userLogFileName, name,
            sizeof(sim->userLogFileName) - 1);
    sim->userLogFileName[sizeof(sim->userLogFileName) - 1] = '\0';
}

void serverSimSetWantLogging(ServerSim *sim, bool enabled) {
    sim->wantLogging = enabled;
}

bool serverSimGetTankState(ServerSim *sim, BYTE playerNum, WORLD *wx, WORLD *wy) {
    if (playerNum >= MAX_TANKS || sim->sim.tanks[playerNum] == NULL) {
        return FALSE;
    }
    tankGetWorld(&sim->sim.tanks[playerNum], wx, wy);
    return TRUE;
}

static int serverSimGetShells(ServerSim *sim, ShellSnapshot *out, int maxOut,
                              const ViewportRect *viewports, int numViewports) {
    shells q;
    int count = 0;

    q = sim->sim.shs;
    while (q != NULL && count < maxOut) {
        if (!inAnyViewport(viewports, numViewports, q->x >> 8, q->y >> 8)) {
            q = q->next;
            continue;
        }
        out[count].worldX = q->x;
        out[count].worldY = q->y;
        out[count].angle = (uint8_t)q->angle;
        out[count].owner = q->owner;
        out[count].length = q->length;
        count++;
        q = q->next;
    }
    return count;
}

static int serverSimGetTkExplosions(ServerSim *sim, TkExplosionSnapshot *out, int maxOut) {
    /* Tank fireballs are now replicated as one-shot EVENT_TK_EXPLOSION
     * reliable events at creation time (see serverSimCbTkExplosion). The
     * client simulates the trail locally, so we no longer send per-tick
     * snapshot data. */
    (void)sim; (void)out; (void)maxOut;
    return 0;
}

static int serverSimGetBases(ServerSim *sim, BaseSnapshot *out, int maxOut) {
    int count = 0;
    BYTE nb;
    BYTE b;
    if (sim->sim.bs == NULL) return 0;
    nb = basesGetNumBases(&sim->sim.bs);
    for (b = 0; b < nb && count < maxOut; b++) {
        out[count].owner = (*sim->sim.bs).item[b].owner;
        out[count].armour = (*sim->sim.bs).item[b].armour;
        out[count].shells = (*sim->sim.bs).item[b].shells;
        out[count].mines = (*sim->sim.bs).item[b].mines;
        count++;
    }
    return count;
}

static int serverSimGetPills(ServerSim *sim, PillSnapshot *out, int maxOut) {
    int count = 0;
    BYTE np;
    BYTE p;
    if (sim->sim.pb == NULL) return 0;
    np = pillsGetNumPills(&sim->sim.pb);
    for (p = 0; p < np && count < maxOut; p++) {
        out[count].x = (*sim->sim.pb).item[p].x;
        out[count].y = (*sim->sim.pb).item[p].y;
        out[count].owner = (*sim->sim.pb).item[p].owner;
        out[count].armour = (*sim->sim.pb).item[p].armour;
        out[count].speed = (*sim->sim.pb).item[p].speed;
        out[count].inTank = (*sim->sim.pb).item[p].inTank ? 1 : 0;
        count++;
    }
    return count;
}

void serverSimAddEvent(ServerSim *sim, const GameEvent *event) {
    if (sim->eventCount < MAX_SNAPSHOT_EVENTS) {
        sim->events[sim->eventCount] = *event;
        sim->eventCount++;
    }
}

int serverSimGetCompressedMap(ServerSim *sim, BYTE *output) {
    return mapSaveCompressedMap(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss, output);
}

void serverSimBuildSnapshot(ServerSim *sim, BYTE clientIdx,
                            SnapshotHeader *hdr,
                            TankSnapshot *tanksOut, int maxTanks,
                            ShellSnapshot *shellsOut, int maxShells,
                            TkExplosionSnapshot *tkExplOut, int maxTkExpl,
                            BaseSnapshot *basesOut, int maxBases,
                            PillSnapshot *pillsOut, int maxPills,
                            GameEvent *eventsOut, int maxEvents,
                            bool noCull) {
    int i;
    int tankCount = 0;
    ViewportRect viewports[MAX_VIEWPORTS];
    int numViewports = 0;

    memset(hdr, 0, sizeof(*hdr));
    hdr->serverTick = sim->tick;
    hdr->lastProcessedInput = sim->lastProcessedInput[clientIdx];
    /* Forced return-to-lobby countdown — clients render their own
     * "Returning to lobby in N" off this. 0 means no pending. */
    hdr->returnToLobbyTicks =
        (sim->returnToLobbyTicks > 0)
            ? (uint16_t)((sim->returnToLobbyTicks > 0xFFFF)
                          ? 0xFFFFu : sim->returnToLobbyTicks)
            : 0;

    /* Primary viewport: client's tank position. Skipped under noCull so
     * the fallback full-map viewport below covers everything. */
    if (!noCull) {
        WORLD clientWX = 0, clientWY = 0;
        if (serverSimGetTankState(sim, clientIdx, &clientWX, &clientWY)) {
            int centerMX = clientWX >> 8;
            int centerMY = clientWY >> 8;
            int halfView = (SNAPSHOT_SCREEN_SIZE / 2) + SNAPSHOT_VIEWPORT_MARGIN;
            viewports[numViewports].minMX = centerMX - halfView;
            viewports[numViewports].maxMX = centerMX + halfView;
            viewports[numViewports].minMY = centerMY - halfView;
            viewports[numViewports].maxMY = centerMY + halfView;
            numViewports++;
        }
    }

    /* Additional viewports: pillboxes owned by this client or its allies
     * (not in tank). Allied pills are visible to the player, so their
     * surroundings must be sent too. */
    if (!noCull && sim->sim.pb != NULL) {
        int halfView = (SNAPSHOT_SCREEN_SIZE / 2) + SNAPSHOT_VIEWPORT_MARGIN;
        BYTE np = pillsGetNumPills(&sim->sim.pb);
        BYTE p;
        for (p = 0; p < np && numViewports < MAX_VIEWPORTS; p++) {
            BYTE owner = (*sim->sim.pb).item[p].owner;
            if (!playersIsAllie(&sim->sim.plyrs, owner, clientIdx)) continue;
            if ((*sim->sim.pb).item[p].inTank) continue;
            viewports[numViewports].minMX = (*sim->sim.pb).item[p].x - halfView;
            viewports[numViewports].maxMX = (*sim->sim.pb).item[p].x + halfView;
            viewports[numViewports].minMY = (*sim->sim.pb).item[p].y - halfView;
            viewports[numViewports].maxMY = (*sim->sim.pb).item[p].y + halfView;
            numViewports++;
        }
    }

    /* No viewports (dead/respawning with no placed pills, or noCull) —
     * send everything. */
    if (numViewports == 0) {
        viewports[0].minMX = 0;  viewports[0].maxMX = 255;
        viewports[0].minMY = 0;  viewports[0].maxMY = 255;
        numViewports = 1;
    }

    /* Build tank snapshots for all connected players */
    for (i = 0; i < MAX_TANKS && tankCount < maxTanks; i++) {
        TankSnapshot *ts;
        WORLD wx, wy;

        if (!sim->playerConnected[i]) continue;
        if (!serverSimGetTankState(sim, (BYTE)i, &wx, &wy)) continue;

        /* Always include the client's own tank; cull others by viewport.
         * Also check LGM position — a parachuting LGM can be far from its
         * tank (starts at a random spawn), so we need to send updates when
         * the LGM is visible even if the tank is not.  Out-of-view tanks
         * are emitted as 1-byte stubs (TANK_SNAPSHOT_HIDDEN_FLAG) rather
         * than skipped, so the client can clear stale ghost positions for
         * tanks that have driven off screen.  noCull bypasses this so
         * recording paths capture every tank in full. */
        if (i != clientIdx && !noCull) {
            bool inView = inAnyViewport(viewports, numViewports, wx >> 8, wy >> 8);
            if (!inView && sim->sim.lgmen[i] != NULL && lgmIsOut(&sim->sim.lgmen[i])) {
                BYTE lgmMX = lgmGetMX(&sim->sim.lgmen[i]);
                BYTE lgmMY = lgmGetMY(&sim->sim.lgmen[i]);
                if (lgmMX != 0 || lgmMY != 0) {
                    inView = inAnyViewport(viewports, numViewports, lgmMX, lgmMY);
                }
            }
            if (!inView) {
                ts = &tanksOut[tankCount];
                memset(ts, 0, sizeof(*ts));
                ts->playerNum = (uint8_t)(i | TANK_SNAPSHOT_HIDDEN_FLAG);
                tankCount++;
                continue;
            }
        }

        ts = &tanksOut[tankCount];
        ts->playerNum = (uint8_t)i;
        ts->worldX = wx;
        ts->worldY = wy;
        ts->angle = (uint16_t)(tankGetAngle(&sim->sim.tanks[i]) * 256.0f);
        ts->speed = (uint16_t)(tankGetActualSpeed(&sim->sim.tanks[i]) * 256.0f);
        {
            BYTE onBoat = tankIsOnBoat(&sim->sim.tanks[i]) ? 1 : 0;
            BYTE isDead = (tankGetDeathWait(&sim->sim.tanks[i]) > 0) ? 1 : 0;
            ts->tankStatus = utilPutNibble(isDead, onBoat);
        }
        ts->lgmFrame = lgmIsOut(&sim->sim.lgmen[i]) ? (lgmGetFrame(&sim->sim.lgmen[i]) + 1) : 0;
        ts->lgmMX = lgmGetMX(&sim->sim.lgmen[i]);
        ts->lgmMY = lgmGetMY(&sim->sim.lgmen[i]);
        ts->lgmPX = lgmGetPX(&sim->sim.lgmen[i]);
        ts->lgmPY = lgmGetPY(&sim->sim.lgmen[i]);
        ts->firstLeft = tankGetFirstLeft(&sim->sim.tanks[i]);
        ts->firstRight = tankGetFirstRight(&sim->sim.tanks[i]);
        ts->pingMs = sim->playerPing[i];
        ts->clientFlags = playersGetClientFlags(&sim->sim.plyrs, (BYTE)i);
        { static bool _snaplg[16] = {0};
          if (!_snaplg[i] && ts->clientFlags != 0) {
            _snaplg[i] = 1;
            WB_LOG_DEBUG(WB_LOG_CAT_SERVER, "[WBN SNAP] player %d clientFlags=0x%02x", i, ts->clientFlags);
          }
        }

        /* Resources: only send to the owning player */
        if (i == clientIdx) {
            ts->armour = tankGetArmour(&sim->sim.tanks[i]);
            ts->shells = tankGetShells(&sim->sim.tanks[i]);
            ts->mines = tankGetMines(&sim->sim.tanks[i]);
            ts->trees = tankGetTrees(&sim->sim.tanks[i]);
            ts->gunsightLen = tankGetGunsightLength(&sim->sim.tanks[i]);
            ts->deathWait = (uint8_t)tankGetDeathWait(&sim->sim.tanks[i]);
            ts->reload = tankGetReloadTime(&sim->sim.tanks[i]);
        } else {
            ts->armour = 0;
            ts->shells = 0;
            ts->mines = 0;
            ts->trees = 0;
            ts->gunsightLen = 0;
            ts->deathWait = 0;
            ts->reload = 0;
        }
        tankCount++;
    }
    hdr->tankCount = (uint8_t)tankCount;

    /* Shell snapshots */
    hdr->shellCount = (uint8_t)serverSimGetShells(sim, shellsOut, maxShells,
                                                   viewports, numViewports);

    /* Tank explosion snapshots (globally important — no viewport filtering) */
    hdr->tkExplosionCount = (uint8_t)serverSimGetTkExplosions(sim, tkExplOut, maxTkExpl);

    /* Periodic full base/pill/map sync to correct any client drift */
    if (sim->lastFullSyncTick == 0 || sim->tick - sim->lastFullSyncTick >= FULL_SYNC_INTERVAL) {
        hdr->baseCount = (uint8_t)serverSimGetBases(sim, basesOut, maxBases);
        hdr->pillCount = (uint8_t)serverSimGetPills(sim, pillsOut, maxPills);
        hdr->mapChecksum = mapCalcChecksum(&sim->sim.mp);
        sim->lastFullSyncTick = sim->tick;
    } else {
        hdr->baseCount = 0;
        hdr->pillCount = 0;
        hdr->mapChecksum = 0;
    }

    /* Game events — filter EVENT_SOUND by distance and deduplicate per type.
     * Non-sound events pass through unchanged. */
    {
        int outCount = 0;
        WORLD cwx = 0, cwy = 0;
        BYTE clientMX = 0, clientMY = 0;
        bool hasClientPos = serverSimGetTankState(sim, clientIdx, &cwx, &cwy);
        if (hasClientPos) {
            clientMX = (BYTE)(cwx >> 8);
            clientMY = (BYTE)(cwy >> 8);
        }

        /* First pass: collect best (closest) sound event per sound type.
         * Track by soundId index — sndEffects has ~24 values. */
        #define MAX_SOUND_TYPES 32
        int bestSoundIdx[MAX_SOUND_TYPES];   /* index into sim->events */
        int bestSoundDist[MAX_SOUND_TYPES];  /* manhattan distance to client */
        int s;
        for (s = 0; s < MAX_SOUND_TYPES; s++) {
            bestSoundIdx[s] = -1;
            bestSoundDist[s] = 255;
        }

        for (i = 0; i < sim->eventCount; i++) {
            uint8_t evType = sim->events[i].type;
            if (evType == EVENT_SOUND || evType == EVENT_SOUND_TANK_HIT || evType == EVENT_SOUND_SHOOT) {
                uint8_t soundId = sim->events[i].data[0];
                uint8_t mx = sim->events[i].data[1];
                uint8_t my = sim->events[i].data[2];

                if (!hasClientPos) continue;

                /* Skip own shoot sound — client plays shootSelf via prediction */
                if (evType == EVENT_SOUND_SHOOT && sim->events[i].data[3] == clientIdx) {
                    continue;
                }

                /* Bubbles only go to the player losing ammo in water */
                if (evType == EVENT_SOUND && soundId == bubbles && sim->events[i].data[3] != clientIdx) {
                    continue;
                }

                /* Calculate manhattan distance to client */
                int dx = (clientMX > mx) ? (clientMX - mx) : (mx - clientMX);
                int dy = (clientMY > my) ? (clientMY - my) : (my - clientMY);

                /* Always send tank hits to the hit player (plays hitTankSelf at full volume) */
                if (evType == EVENT_SOUND_TANK_HIT && sim->events[i].data[3] == clientIdx) {
                    /* Skip distance cull */
                } else if (dx >= SDIST_NONE || dy >= SDIST_NONE) {
                    continue;
                }

                /* Keep closest instance of each sound type */
                int dist = dx + dy;
                if (soundId < MAX_SOUND_TYPES && dist < bestSoundDist[soundId]) {
                    bestSoundIdx[soundId] = i;
                    bestSoundDist[soundId] = dist;
                }
            }
        }

        /* Copy map events first (from dedicated buffer), then non-sound
         * events, then deduplicated sound events */
        for (i = 0; i < sim->mapEventCount && outCount < maxEvents; i++) {
            eventsOut[outCount++] = sim->mapEvents[i];
        }
        for (i = 0; i < sim->eventCount && outCount < maxEvents; i++) {
            uint8_t evType = sim->events[i].type;
            if (evType != EVENT_SOUND && evType != EVENT_SOUND_TANK_HIT && evType != EVENT_SOUND_SHOOT) {
                /* Filter EVENT_MINE_VISIBLE: tank mines (bit 7 set) go to all,
                 * LGM mines go only to the placer and their allies */
                if (evType == EVENT_MINE_VISIBLE) {
                    BYTE sp = sim->events[i].data[2];
                    if (!(sp & 0x80) && clientIdx != (sp & 0x7F) &&
                        !playersIsAllie(&sim->sim.plyrs, clientIdx, sp)) {
                        continue;
                    }
                }
                /* Viewport-cull explosion events */
                if (evType == EVENT_EXPLOSION) {
                    if (!inAnyViewport(viewports, numViewports, sim->events[i].data[0], sim->events[i].data[1])) {
                        continue;
                    }
                }
                eventsOut[outCount++] = sim->events[i];
            }
        }
        for (s = 0; s < MAX_SOUND_TYPES && outCount < maxEvents; s++) {
            if (bestSoundIdx[s] >= 0) {
                eventsOut[outCount++] = sim->events[bestSoundIdx[s]];
            }
        }
        #undef MAX_SOUND_TYPES

        hdr->reliableEventCount = (uint8_t)outCount;
        hdr->reliableBaseSeq = 0; /* Local transport doesn't use sequence tracking */
    }
}

void serverSimInformation(ServerSim *sim, bool locked) {
    BYTE count;
    BYTE numPlayers;
    BYTE maxPlayers;
    char name[256];
    time_t startTime = (time_t)sim->timeCreated;

    numPlayers = serverSimGetNumPlayers(sim);
    maxPlayers = MAX_TANKS;

    fprintf(stdout, "\n");
    fprintf(stdout, "WinBolo Server (new sim)\n");
    fprintf(stdout, "Game start time: %sMap Name: %s - Locked: %s\n",
            asctime(gmtime(&startTime)),
            sim->mapName,
            locked ? "Yes" : "No");
    fprintf(stdout, "Players: (%d/%d) Neutral Pillboxes: (%d/%d), Neutral Bases: (%d/%d)\n\n",
            numPlayers, maxPlayers,
            pillsGetNumNeutral(&sim->sim.pb), pillsGetNumPills(&sim->sim.pb),
            basesGetNumNeutral(&sim->sim.bs), basesGetNumBases(&sim->sim.bs));

    if (numPlayers > 0) {
        fprintf(stdout, "Players:\n");
        for (count = 0; count < MAX_TANKS; count++) {
            if (!sim->playerConnected[count]) continue;
            playersGetPlayerName(&sim->sim.plyrs, count, name, TRUE);
            fprintf(stdout, "%s - (P:%d B:%d Ping:%dms Jitter:%d)\n",
                    name,
                    pillsGetNumberOwnedByPlayer(&sim->sim.pb, count),
                    basesGetNumberOwnedByPlayer(&sim->sim.bs, count),
                    sim->playerPing[count],
                    sim->jitterTarget[count]);

            /* For bot slots, append a 4-space-indented [BOT] line with
             * the brain's most recent timing. Mute fields suppressed
             * when zero — see commit message. */
            BotInfo bi;
            if (botManagerGetBotInfo(sim, count, &bi)) {
                if (bi.hasBrain) {
                    if (bi.overrunCount == 0) {
                        fprintf(stdout,
                                "    [BOT] brain=%s last=%.1fms target=%.1fms\n",
                                bi.brainName, bi.lastThinkMs, bi.targetMs);
                    } else {
                        fprintf(stdout,
                                "    [BOT] brain=%s last=%.1fms target=%.1fms overruns=%u\n",
                                bi.brainName, bi.lastThinkMs, bi.targetMs,
                                bi.overrunCount);
                    }
                } else {
                    fprintf(stdout, "    [BOT]\n");
                }
            }
        }
    }

    /* Bot pool summary block — only when at least one bot slot is
     * active. Shows the per-bot budget against the 20ms server tick
     * plus per-stage last + EWMA wall-clock so operators can spot
     * spikes against averages at a glance. */
    if (botManagerHasAnyBot(sim)) {
        BotPoolStats ps;
        botManagerGetPoolStats(sim, &ps);
        if (ps.workerCount == 0) {
            fprintf(stdout,
                    "Bot pool: single-thread (%d active bots), target=%.1fms/bot\n",
                    ps.activeBots, ps.currentTargetMs);
        } else {
            fprintf(stdout,
                    "Bot pool: %d workers (%d active bots), target=%.1fms/bot\n",
                    ps.workerCount, ps.activeBots, ps.currentTargetMs);
        }

        double tickLast = 0.0, tickEwma = 0.0;
        serverLifecycleGetTickStats(&tickLast, &tickEwma);
        double simLast = 0.0, simEwma = 0.0;
        serverLifecycleGetSimStats(&simLast, &simEwma);

        if (tickLast > 0.0) {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms  (budget=20ms)\n",
                    "Tick:", tickLast, tickEwma);
        }
        fprintf(stdout,
                "  %-11s last=%.1fms  EWMA=%.1fms\n",
                "Brain:", ps.lastBrainPhaseMs, ps.ewmaBrainPhaseMs);
        if (simLast > 0.0) {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms\n",
                    "Simulation:", simLast, simEwma);
        }
        /* "Bot prep" labels the non-brain serial parts of
         * botManagerTick: snapshot/sync + input send. Distinct from
         * "Simulation:" above which times the two serverSimTick calls. */
        if (ps.totalOverruns == 0) {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms\n",
                    "Bot prep:", ps.lastSerialMs, ps.ewmaSerialMs);
        } else {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms  total overruns=%u\n",
                    "Bot prep:", ps.lastSerialMs, ps.ewmaSerialMs,
                    ps.totalOverruns);
        }
    }

    fprintf(stdout, "\n");
}

bool serverSimSaveMap(ServerSim *sim, char *fileName) {
    return mapWrite(fileName, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss);
}

bool serverSimCheckGameWin(ServerSim *sim, bool printWinners) {
    bool allOwned;
    BYTE count;
    BYTE max;
    BYTE first = NEUTRAL;
    BYTE current;
    char name[256];

    allOwned = TRUE;
    max = basesGetNumBases(&sim->sim.bs);

    for (count = 1; count <= max && allOwned; count++) {
        BYTE shellsAmt, minesAmt, armourAmt;
        current = basesGetBaseOwner(&sim->sim.bs, count);
        basesGetStats(&sim->sim.bs, count, &shellsAmt, &minesAmt, &armourAmt);
        if (current == NEUTRAL || armourAmt <= MIN_ARMOUR_CAPTURE) {
            allOwned = FALSE;
        } else if (count == 1) {
            first = current;
        } else {
            allOwned = playersIsAllie(&sim->sim.plyrs, current, first);
        }
    }

    if (allOwned && max > 0 && printWinners) {
        fprintf(stdout, "Game Won!\nWinners:\n");
        for (count = 0; count < MAX_TANKS; count++) {
            if (!sim->playerConnected[count]) continue;
            if (playersIsAllie(&sim->sim.plyrs, count, first) || count == first) {
                playersGetPlayerName(&sim->sim.plyrs, count, name, TRUE);
                fprintf(stdout, "  %s\n", name);
            }
        }
    }

    return allOwned && max > 0;
}

bool serverSimCheckAutoClose(ServerSim *sim) {
    if (!sim->hadPlayersEver) {
        if (serverSimGetNumPlayers(sim) > 0) {
            sim->hadPlayersEver = TRUE;
        }
    } else {
        if (serverSimGetNumPlayers(sim) == 0) {
            return TRUE;
        }
    }
    return FALSE;
}

BYTE serverSimGetNumPlayers(ServerSim *sim) {
    BYTE count;
    BYTE num = 0;
    for (count = 0; count < MAX_TANKS; count++) {
        if (sim->playerConnected[count]) {
            num++;
        }
    }
    return num;
}

BYTE serverSimGetNumHumans(ServerSim *sim) {
    BYTE count;
    BYTE num = 0;
    for (count = 0; count < MAX_TANKS; count++) {
        if (sim->playerConnected[count] && !botManagerIsBot(sim, count)) {
            num++;
        }
    }
    return num;
}

/* Compute and cache the MD5 of a BMAPBOLO .map file so WBN can match
 * the map against its library. Streams the file (handles any size) and
 * only accepts it when the BMAPBOLO magic is present; clears the valid
 * flag on any failure or non-.map input. */
static void serverSimCacheMapMd5FromFile(ServerSim *sim, const char *path) {
    FILE *f;
    BYTE buf[8192];
    size_t n;
    Md5Ctx ctx;
    bool headerChecked = FALSE;
    bool isBmap = FALSE;
    if (sim == NULL) return;
    sim->mapMd5Valid = FALSE;
    if (path == NULL || path[0] == '\0') return;
    f = fopen(path, "rb");
    if (f == NULL) return;
    md5Init(&ctx);
    while ((n = fread(buf, 1, sizeof(buf), f)) > 0) {
        if (!headerChecked) {
            headerChecked = TRUE;
            isBmap = (n >= (sizeof(MAP_HEADER) - 1) &&
                      memcmp(buf, MAP_HEADER, sizeof(MAP_HEADER) - 1) == 0);
            if (!isBmap) break;
        }
        md5Update(&ctx, buf, n);
    }
    fclose(f);
    if (isBmap) {
        md5Final(sim->mapMd5, &ctx);
        sim->mapMd5Valid = TRUE;
    }
}

/* Minimum seconds between server/lobby_update sends — bursty lobby
 * edits (e.g. time-limit slider) coalesce into one send per window. */
#define WBN_LOBBY_UPDATE_INTERVAL 30

void serverSimRefreshWbnLobbyInfo(ServerSim *sim) {
    WbnLobbyInfo info;
    if (sim == NULL) return;
    memset(&info, 0, sizeof(info));
    snprintf(info.map, sizeof(info.map), "%s", sim->mapName);
    /* map_md5 is the hash of the canonical BMAPBOLO bytes; meaningful
     * only for known (non-random) maps WBN can match in its library. */
    if (sim->mapMd5Valid && !sim->randomMapEnabled) {
        static const char hexd[] = "0123456789abcdef";
        int i;
        for (i = 0; i < 16; i++) {
            info.mapMd5[i * 2]     = hexd[(sim->mapMd5[i] >> 4) & 0xF];
            info.mapMd5[i * 2 + 1] = hexd[sim->mapMd5[i] & 0xF];
        }
        info.mapMd5[32] = '\0';
    } else {
        info.mapMd5[0] = '\0';
    }
    info.randomMap       = sim->randomMapEnabled ? true : false;
    info.gameType        = (BYTE)gameTypeGet(&sim->sim.game);
    info.ai              = (BYTE)sim->botAiType;
    info.mines           = sim->sim.hiddenMines ? true : false;
    info.ranked          = sim->ranked ? true : false;
    info.allowNewPlayers = sim->allowNewPlayers ? true : false;
    info.autoLock        = sim->autoLockOnGameStart ? true : false;
    info.timeLimit       = serverSimGetTimeLimit(sim) ? true : false;
    info.timeMinutes     = serverSimGetTimeMinutes(sim);
    info.lobbyLocks      = sim->serverLocks;
    info.numBases        = basesGetNumBases(&sim->sim.bs);
    info.numPills        = pillsGetNumPills(&sim->sim.pb);
    info.freeBases       = serverSimGetNumNeutralBases(sim);
    info.freePills       = serverSimGetNumNeutralPills(sim);
    info.numHumans       = serverSimGetNumHumans(sim);
    info.numBots         = botManagerGetNumBots(sim);
    winbolonetSetLobbyInfo(&info);
}

void serverSimWbnLobbyUpdate(ServerSim *sim, bool force) {
    time_t now;
    if (sim == NULL || !winbolonetIsRunning()) return;
    /* During a session rotation (round end → new register) the
     * still-set winboloNetServerKey belongs to the round that just
     * quit. Sending a lobby_update now would rename that finished
     * game's map on the tracker. Hold the change as dirty; it flushes
     * on the next WBN tick once BeginSession has installed the new
     * key (see serverInstanceTick's rotation sites). force is honored
     * for nothing here — the rotation guard outranks it. */
    if (sim->wbnSessionRotating) {
        sim->wbnLobbyDirty = TRUE;
        return;
    }
    now = time(NULL);
    if (!force && (now - sim->wbnLobbyLastSent) < WBN_LOBBY_UPDATE_INTERVAL) {
        /* Within the rate-limit window: defer to the next tick. */
        sim->wbnLobbyDirty = TRUE;
        return;
    }
    serverSimRefreshWbnLobbyInfo(sim);
    winbolonetSendLobbyUpdate();
    sim->wbnLobbyLastSent = now;
    sim->wbnLobbyDirty = FALSE;
}

void serverSimWbnLobbyTick(ServerSim *sim) {
    if (sim != NULL && sim->wbnLobbyDirty) {
        serverSimWbnLobbyUpdate(sim, FALSE);
    }
}

BYTE serverSimGetNumNeutralBases(ServerSim *sim) {
    return basesGetNumNeutral(&sim->sim.bs);
}

BYTE serverSimGetNumNeutralPills(ServerSim *sim) {
    return pillsGetNumNeutral(&sim->sim.pb);
}

bool serverSimIsRunning(void) {
    ServerSim *sim = serverSimGetActive();
    return sim != NULL && sim->state == serverStateRunning;
}

void serverSimAbortCountdown(ServerSim *sim) {
    sim->state = serverStateLobby;
    sim->countdownTicks = 0;
    /* Tell every subscriber the countdown is over — without this the
     * client's netStat stays at netLobbyCountdown and the lobby UI
     * leaves the "Game starting in N…" overlay drawn even though the
     * server has reverted to lobby. */
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillGamePhaseEvent(sim, &evt);
        serverSimPublishControl(sim, &evt);
    }
}

void serverSimSetCountdownTicks(ServerSim *sim, int32_t ticks) {
    sim->countdownTicks = ticks;
}

void serverSimClearBalanceProposal(ServerSim *sim) {
    memset(&sim->balanceProposal, 0, sizeof(BalanceProposal));
}

void serverSimConsoleMessage(const char *msg) {
    ServerSim *sim = serverSimGetActive();
    if (sim != NULL && sim->sim.callbacks.consoleMessage != NULL) {
        sim->sim.callbacks.consoleMessage(sim->sim.callbacks.ctx, (char *)msg);
    } else {
        /* Fallback: print to stdout if no active sim */
        fprintf(stdout, "%s\n", msg);
    }
}

void serverSimEnterGameOver(ServerSim *sim) {
    if (!sim->lobbyEnabled) {
        /* No lobby — game over means server should shut down */
        sim->state = serverStateGameOver;
        sim->countdownTicks = 0;
        serverSimConsoleMessage("Game over!");
    } else {
        /* Lobby enabled — hold in game-over state then return to lobby */
        sim->state = serverStateGameOver;
        sim->countdownTicks = GAMEOVER_HOLD_TICKS;
        serverSimConsoleMessage("Game over! Returning to lobby...");
        /* CTRL_GAME_PHASE_GAME_OVER and CTRL_GAME_OVER are published
         * by the server lifecycle when it observes the state change;
         * the per-client codec subscriber turns each into the matching
         * wire packet (PACKET_GAME_OVER). */
    }
}

void serverSimReturnToLobby(ServerSim *sim) {
    BYTE i, j;
    bool savedConnected[MAX_TANKS];
    LobbyPlayer savedLobby[MAX_TANKS];

    if (!sim->lobbyEnabled) {
        return;
    }

    /* Open the WBN session-rotation window. The map regenerate / rotation
     * below and the post-tick lifecycle map pick both touch the new
     * round's map; without this guard their serverSimWbnLobbyUpdate would
     * report that map against the just-finished round's server_key. The
     * lifecycle clears the flag after BeginSession installs the new key. */
    sim->wbnSessionRotating = TRUE;

    /* Clear in-game vote state — any in-flight or just-concluded votes
     * are scoped to the round we're leaving. */
    serverSimGameVoteResetAll(sim);

    /* Carry the in-game alliance topology forward into next-round
     * team assignments. For each alliance group we pick the lowest
     * existing teamNumber as the representative and reassign every
     * group member to it; isolated players keep their current team.
     *
     * Two original teams that merged in-game collapse to the lower
     * team id (the higher one becomes unused metadata, fine). A
     * defector ends up on their new alliance's team. Players who
     * dropped alliances mid-game without re-forming one keep their
     * own team. */
    {
        uint8_t newTeam[MAX_TANKS] = {0};
        for (i = 0; i < MAX_TANKS; i++) {
            if (!sim->playerConnected[i]) continue;
            if (newTeam[i] != 0) continue;
            uint8_t repTeam = sim->lobbyPlayers[i].teamNumber;
            bool inGroup[MAX_TANKS] = {0};
            inGroup[i] = true;
            for (j = 0; j < MAX_TANKS; j++) {
                if (j == i || !sim->playerConnected[j]) continue;
                if (playersIsAllie(&sim->sim.plyrs, i, j)) {
                    inGroup[j] = true;
                    uint8_t t = sim->lobbyPlayers[j].teamNumber;
                    if (t > 0 && (repTeam == 0 || t < repTeam)) {
                        repTeam = t;
                    }
                }
            }
            for (j = 0; j < MAX_TANKS; j++) {
                if (inGroup[j]) newTeam[j] = repTeam;
            }
        }
        for (i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i] && newTeam[i] != 0) {
                sim->lobbyPlayers[i].teamNumber = newTeam[i];
            }
        }
    }

    /* Save connection and lobby state (sim-level arrays cleared by the
     * reset). Player identity in sim->sim.plyrs->item[i] (name, country,
     * clientType, clientFlags, brain name) is preserved in-place by
     * serverSimResetGameWorld, so it no longer needs snapshotting here. */
    for (i = 0; i < MAX_TANKS; i++) {
        savedConnected[i] = sim->playerConnected[i];
        savedLobby[i] = sim->lobbyPlayers[i];
    }

    /* Full world reset — reloads map from cached data */
    serverSimResetGameWorld(sim);

    /* Restore connection and lobby state */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->playerConnected[i] = savedConnected[i];
        sim->lobbyPlayers[i] = savedLobby[i];
        sim->lobbyPlayers[i].ready = FALSE;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->lobbyPlayers[i].isBot) {
            sim->lobbyPlayers[i].ready = TRUE;
        }
    }

    /* Reconcile the players table against the restored connection state:
     * clear any slot still marked inUse but no longer connected. The leave
     * path (serverSimRemovePlayer -> playersClearSlot) already does this per
     * departure; this round-boundary backstop heals any slot that diverged
     * through a path that bypassed it, so a stale identity can't survive
     * into the new round and be re-announced to joiners by the inUse-gated
     * CTRL_PLAYER_JOIN sync-replay. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] &&
            playersIsInUse(&sim->sim.plyrs, i) == TRUE) {
            playersClearSlot(&sim->sim.plyrs, i);
        }
    }
    sim->hadPlayersEver = TRUE;
    sim->gameLength = sim->originalGameLength;
    sim->emptyResetTicks = -1;

    /* Identity (name, country, clientType, clientFlags, brain name) survives
     * the reset in-place. The one deliberate change on return-to-lobby is to
     * drop the WBN-session bits: the WBN session is torn down and re-registered
     * with a fresh server_key (see winbolonetReturnToLobby in the caller), so
     * each client must re-auth via PACKET_WBN_REAUTH against the new session.
     * Connection-scoped bits (STEAM_BUILD, SUPPORTER, ADMIN, BOT) stay. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        uint8_t f = playersGetClientFlags(&sim->sim.plyrs, (BYTE)i);
        f &= (uint8_t)~(PLAYER_FLAG_WBN_VERIFIED |
                        PLAYER_FLAG_WBN_STEAM_LINKED);
        playersSetClientFlags(&sim->sim.plyrs, (BYTE)i, f);
    }

    sim->state = serverStateLobby;
    serverSimMapSkipVotesReset(sim);

    /* Regenerate random map between rounds */
    if (sim->randomMapEnabled) {
        serverSimRandomMapRegenerate(sim);
    }

    serverSimConsoleMessage("Returned to lobby.");
    /* Lobby state fan-out happens via the control-event bus — the
     * caller publishes CTRL_LOBBY_SLOT + CTRL_LOBBY_SETTINGS. */

    /* Push the freshly-reset map back to every audience. The server's
     * map was restored to cachedMapData by resetGameWorld above (or
     * to a freshly-generated random / rotation map), but every
     * client — UDP-remote and in-process (SP, bots, host's own
     * loopback) — still carries the in-game mutations from the
     * round that just ended (felled trees, built walls, mine
     * craters). Neither the periodic snapshot's full-sync (carries
     * map checksum only) nor the CTRL_GAME_PHASE_LOBBY event
     * redistributes terrain bytes.
     *
     * Mirror serverSimApplyMapChange: the wire helper refreshes the
     * UDP-side cached compressed map / JOIN_ACCEPT / per-client
     * download tracking, and the CTRL_LOBBY_MAP_CHANGE publish fans
     * out via the bus so in-process subscribers (SP ClientSim, bots)
     * reinstall via boundServerSim — see client_sim_control.c:260.
     * The two together close the asymmetric-runtime gap: SP players
     * would otherwise keep round-end terrain through the next round. */
    transportUdpServerOnLobbyMapChange(sim);
    {
        ControlEvent mapEvt;
        memset(&mapEvt, 0, sizeof(mapEvt));
        mapEvt.type = CTRL_LOBBY_MAP_CHANGE;
        serverSimPublishControl(sim, &mapEvt);
    }

    /* Publish CTRL_GAME_PHASE_LOBBY so subscribers (e.g. the
     * dedicated-server log writer) see the GAME_OVER→LOBBY
     * transition. Existing CTRL_LOBBY_SLOT / CTRL_LOBBY_SETTINGS
     * fan-out happens elsewhere — this is the phase event proper. */
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillGamePhaseEvent(sim, &evt);
        serverSimPublishControl(sim, &evt);
    }
}

void serverSimLobbyCheckAllReady(ServerSim *sim) {
    BYTE numConnected = 0;
    BYTE i;

    if (sim->state != serverStateLobby) return;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        numConnected++;
        if (!sim->lobbyPlayers[i].ready) return; /* Not all ready */
    }
    if (numConnected == 0) return;

    /* In-place is the no-countdown optimisation for fresh-sim SP where
     * everything stays in-process and the UI's lobby→running flip can
     * land in the same packet as the tank-bearing snapshot.  Over UDP
     * (including the host's own loopback client in acceptRemoteClients
     * mode), there's a multi-tick gap between CTRL_GAME_PHASE_RUNNING
     * (which flips inLobby on the client and therefore the UI's render
     * mode) and the first snapshot that actually populates client-side
     * tanks.  windowRunGameTick running in that gap dereferences a
     * still-NULL MY_TANK and crashes.  Countdown closes the gap by
     * giving the wire path a settling window where the UI stays in
     * lobby mode and the snapshot stream warms up before the flip. */
    if (sim->worldPreLoaded && !transportUdpServerHasAnyClient()) {
        /* Pure in-process SP — fresh sim, no wire clients.  Skip the
         * countdown overhead.  Used by SP host first round. */
        serverSimStartGameInPlace(sim);
    } else {
        /* Anything fanning over the wire, or a subsequent round (full
         * reset via countdown + StartGame). */
        /* Force a final WBN lobby snapshot before the countdown so the
         * tracker reflects the exact map/settings/roster the game
         * starts with, regardless of the 30s batch window. */
        serverSimWbnLobbyUpdate(sim, TRUE);
        sim->state = serverStateCountdown;
        sim->countdownTicks = LOBBY_COUNTDOWN_TICKS;
        serverSimConsoleMessage("All players ready! Starting countdown...");
        {
            ControlEvent evt;
            memset(&evt, 0, sizeof(evt));
            serverSimFillGamePhaseEvent(sim, &evt);
            serverSimPublishControl(sim, &evt);
        }
    }
}

void serverSimResetGameWorld(ServerSim *sim) {
    BYTE i;
    bool hiddenMines = sim->sim.hiddenMines;

    /* 1. Destroy all tanks and LGMs */
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->sim.tanks[i] != NULL) {
            tankDestroy(&sim->sim, &sim->sim.tanks[i]);
            sim->sim.tanks[i] = NULL;
        }
        if (sim->sim.lgmen[i] != NULL) {
            lgmDestroy(&sim->sim.lgmen[i]);
            sim->sim.lgmen[i] = NULL;
        }
    }

    /* 2. Destroy and recreate all world systems */
    shellsDestroy(&sim->sim.shs);
    sim->sim.shs = shellsCreate();

    explosionsDestroy(&sim->sim.expl);
    explosionsCreate(&sim->sim.expl);

    minesDestroy(&sim->sim.mns);
    minesCreate(&sim->sim.mns, hiddenMines);

    minesExpDestroy(&sim->sim.minesExplosions);
    minesExpCreate(&sim->sim.minesExplosions);

    rubbleDestroy(&sim->sim.rbl);
    rubbleCreate(&sim->sim.rbl);

    buildingDestroy(&sim->sim.blds);
    buildingCreate(&sim->sim.blds);

    grassDestroy(&sim->sim.grs);
    grassCreate(&sim->sim.grs);

    swampDestroy(&sim->sim.swp);
    swampCreate(&sim->sim.swp);

    floodDestroy(&sim->sim.ff);
    floodCreate(&sim->sim.ff);

    tkExplosionDestroy(&sim->sim.tankExplosions);
    tkExplosionCreate(&sim->sim.tankExplosions);

    /* 3. Reload map/bases/pills/starts from cached data */
    if (sim->cachedMapData != NULL) {
        mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss,
                             sim->cachedMapData, sim->cachedMapDataLen);
    }

    /* 4. Clear mines from under bases */
    basesClearMines(&sim->sim);

    /* 5. Reset lag compensation state */
    for (i = 0; i < MAX_TANKS; i++) {
        posHistoryInit(&sim->posHistory[i]);
        posHistoryInit(&sim->lgmPosHistory[i]);
    }
    sim->sim.lagCompTicks = 0;
    memset(sim->sim.perPlayerCompTicks, 0, sizeof(sim->sim.perPlayerCompTicks));

    /* 6. Clear events */
    sim->eventCount = 0;

    /* 7. Reset tick */
    sim->tick = 0;

    /* 8. Flush all input queues */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->inputQueueHead[i] = 0;
        sim->inputQueueTail[i] = 0;
        sim->lastProcessedInput[i] = 0;
        sim->lastInputButtons[i] = 0;
        sim->inputBufferFilled[i] = 0;
        sim->jitterTarget[i] = JITTER_BUFFER_DEFAULT;
        sim->jitterStallCount[i] = 0;
        sim->jitterStableTicks[i] = 0;
    }

    /* 9. Reset full sync tracking */
    sim->lastFullSyncTick = 0;

    /* 10. Reset change detection */
    sim->prevPillCount = 0;
    sim->prevBaseCount = 0;

    /* 11. Reset player connection state and per-slot round/world state.
     * Connection identity (name, country, clientType, clientFlags, bot/WBN
     * flags, brain name) is preserved in-place: it is connection-scoped, not
     * round-scoped, and is cleared on the leave path (serverSimRemovePlayer),
     * not here. Callers no longer hand-restore identity across the reset —
     * that duplicated, drift-prone dance is what dropped PLAYER_FLAG_BOT and
     * the country code on the networked game-start path. */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->playerConnected[i] = FALSE;
        playersResetRoundState(&sim->sim.plyrs, i);
    }
    sim->hadPlayersEver = FALSE;

    /* The world is no longer the pristine one set up by
     * serverSimCreate* — subsequent lobby→running transitions
     * must go through countdown + serverSimStartGame (full reset),
     * not StartGameInPlace. */
    sim->worldPreLoaded = FALSE;
}

void serverSimReapplyTeamAlliances(ServerSim *sim) {
    /* Apply alliances locally and build the full matrix; publish exactly
     * one CTRL_ALLIANCE_RESET carrying it. The earlier shape — one
     * CTRL_ALLIANCE_ACCEPT per allied pair — fanned out up to
     * N×(N-1)/2 events on a single tick (120 for a 16-player team),
     * filling the 128-deep per-client reliable queue before the host's
     * main thread could send a CONTROL_ACK back over loopback. The host
     * got kicked from their own server and the disconnect path recursed
     * through "Elvis has left." broadcasts until the timer thread's
     * stack overflowed. One batched event sidesteps both problems and
     * compresses the wire (~33 bytes vs up to ~480). */
    ControlEvent evt;
    BYTE i, j;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_RESET;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        /* Self-bit set for every connected slot — the matrix is then a
         * full description of which slots exist as well as which pairs
         * are allied. Slots that exist but are unaffiliated still get
         * their self-bit (allies[i] = 1 << i), so the client knows the
         * slot is live and any prior alliances should be cleared. */
        evt.u.allianceReset.allies[i] |= (uint16_t)(1u << i);
        if (sim->lobbyPlayers[i].teamNumber == 0) continue;
        for (j = i + 1; j < MAX_TANKS; j++) {
            if (!sim->playerConnected[j]) continue;
            if (sim->lobbyPlayers[j].teamNumber != sim->lobbyPlayers[i].teamNumber) continue;
            playersAcceptAlliance(&sim->sim, &sim->sim.plyrs, NEUTRAL, i, j, TRUE);
            evt.u.allianceReset.allies[i] |= (uint16_t)(1u << j);
            evt.u.allianceReset.allies[j] |= (uint16_t)(1u << i);
        }
    }

    serverSimPublishControl(sim, &evt);
}

/* At game start, all connected players' restock timers would otherwise
 * be armed on the same tick, collapsing N players' cadence into one
 * shared 800-tick cycle. Spread initial values across the window so the
 * +1 events arrive smoothly. Mid-game joins (one at a time, naturally
 * on different ticks) don't need this. */
static void serverSimStaggerBaseTimers(ServerSim *sim) {
    int numConnected = 0;
    int orderIdx = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) numConnected++;
    }
    if (numConnected == 0) return;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        sim->sim.baseTimer[i] = (BASE_TICKS_BETWEEN_REFUEL * (orderIdx + 1)) / numConnected;
        orderIdx++;
    }
}

void serverSimStartGameInPlace(ServerSim *sim) {
    BYTE i;

    /* Fresh round — the last-human-left return-to-lobby check arms only
     * once a human is seen this round. */
    sim->roundHadHuman = false;

    /* Flush any game-events queued during the lobby before the first
     * running snapshot goes out. The sim doesn't tick in the lobby, so the
     * per-tick drain never runs and discrete events accumulate — most
     * visibly EVENT_PLAYER_LEAVE from removing a bot. The full-reset start
     * path (serverSimStartGame -> serverSimResetGameWorld) already clears
     * these; this in-place SP path bypasses the reset, so without this the
     * stale deltas flush at game start and a slot-only EVENT_PLAYER_LEAVE
     * lands on whatever slot has since been reused. World state rides the
     * snapshot's own arrays, not these queues, so flushing loses nothing. */
    sim->eventCount = 0;
    sim->mapEventCount = 0;

    /* Wire any bots in the roster into the running game (idempotent on
     * a fresh sim with zero bots). */
    botManagerOnGameStart(sim);

    /* Apply team alliances: players with same non-zero teamNumber become allies */
    serverSimReapplyTeamAlliances(sim);

    /* Pre-compute start indices for the whole batch so teammates land
     * near each other (see serverSimStartGame for the rationale). */
    {
        BYTE batchTeam[MAX_TANKS];
        for (i = 0; i < MAX_TANKS; i++) {
            batchTeam[i] = sim->lobbyPlayers[i].teamNumber;
        }
        startsAssignBatch(&sim->sim, &sim->sim.ss,
                          sim->playerConnected, batchTeam,
                          sim->sim.pendingStartIdx);
    }

    /* Create tanks for all connected players */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        if (sim->sim.tanks[i] != NULL) {
            tankDestroy(&sim->sim, &sim->sim.tanks[i]);
            sim->sim.tanks[i] = NULL;
        }
        if (sim->sim.lgmen[i] != NULL) {
            lgmDestroy(&sim->sim.lgmen[i]);
            sim->sim.lgmen[i] = NULL;
        }
        tankCreate(&sim->sim, &sim->sim.tanks[i]);
        sim->sim.lgmen[i] = lgmCreate(i);
    }
    serverSimStaggerBaseTimers(sim);

    sim->state = serverStateRunning;

    /* Layout A: autoLockOnGameStart. Save current allowNewPlayers so
     * we can restore it when the game ends. */
    sim->savedAllowNewPlayers = sim->allowNewPlayers;
    if (sim->autoLockOnGameStart && sim->allowNewPlayers) {
        sim->allowNewPlayers = FALSE;
        transportUdpServerSetLock(sim, FALSE);
    }

    /* Reset per-client reliable-event queues BEFORE publishing the
     * RUNNING phase, so CTRL_GAME_PHASE_RUNNING enters every queue at
     * seq=1 as the first event of the new game.  Skipping this pair
     * leaves stale seqs on any remote-client queue and the per-client
     * controlEventAck reset on the client side then desyncs against
     * the server's still-advancing nextSeq.  The countdown→running
     * path in server_lifecycle.c does the same pair; this is its
     * in-place (worldPreLoaded) counterpart. */
    transportUdpServerOnGameStart(sim);

    /* Announce the RUNNING phase to subscribers. The countdown→running
     * transition in server_lifecycle.c publishes the same event for
     * the full-reset path; this is the analogue for the in-place
     * (worldPreLoaded) path which bypasses countdown. */
    {
        ControlEvent phaseEvt;
        memset(&phaseEvt, 0, sizeof(phaseEvt));
        phaseEvt.type = CTRL_GAME_PHASE_RUNNING;
        serverSimPublishControl(sim, &phaseEvt);
    }
}

void serverSimStartGame(ServerSim *sim) {
    BYTE i;
    /* Save connected-player state before resetting – resetGameWorld clears
       playerConnected[], but we need it to create tanks below. Player
       identity (name, country, clientType, clientFlags) is preserved across
       the reset by serverSimResetGameWorld, so it no longer needs saving. */
    bool savedConnected[MAX_TANKS];
    for (i = 0; i < MAX_TANKS; i++) {
        savedConnected[i] = sim->playerConnected[i];
    }

    activeSim = sim;

    /* Fresh round — drop any vote state from the previous game. */
    serverSimGameVoteResetAll(sim);

    /* Arms the last-human-left return-to-lobby check from a clean slate. */
    sim->roundHadHuman = false;

    /* Reset the game world (map, world systems, queues, tick) */
    serverSimResetGameWorld(sim);

    /* Restore connected-player state so tank creation works */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->playerConnected[i] = savedConnected[i];
    }
    sim->hadPlayersEver = TRUE;

    /* Seed jitter target from lobby ping — localhost players start at minimum
     * buffer depth instead of waiting for the adaptive algorithm to shrink.
     * ping == 0 means no pong received yet, so leave at default. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!savedConnected[i]) continue;
        uint16_t ping = transportUdpServerGetClientPing(i);
        if (ping > 0 && ping < 5) {
            sim->jitterTarget[i] = JITTER_BUFFER_MIN;
        }
    }

    /* Player identity (name, country, clientType, clientFlags including
     * PLAYER_FLAG_BOT) survives the reset in-place — no re-registration
     * needed. */

    sim->gameLength = sim->originalGameLength;

    /* Clear all alliances from previous round */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        playersLeaveAlliance(&sim->sim, &sim->sim.plyrs, NEUTRAL, i, TRUE);
        {
            ControlEvent leaveEvt;
            memset(&leaveEvt, 0, sizeof(leaveEvt));
            leaveEvt.type = CTRL_ALLIANCE_LEAVE;
            leaveEvt.u.allianceLeave.playerNum = i;
            serverSimPublishControl(sim, &leaveEvt);
        }
    }

    /* Apply team alliances: players with same non-zero teamNumber become allies */
    serverSimReapplyTeamAlliances(sim);

    /* Pre-compute start indices for the whole batch so teammates land
     * near each other and rivals don't grab adjacent squares (the tankCreate
     * loop below runs synchronously, so without a batch pass each player's
     * per-position checks would be blind to siblings being created in the
     * same loop). startsGetStart consumes the slot lazily, doing scatter
     * and direction conversion at consumption time so the per-square nudge
     * sees siblings already placed earlier in this loop. */
    {
        BYTE batchTeam[MAX_TANKS];
        for (i = 0; i < MAX_TANKS; i++) {
            batchTeam[i] = sim->lobbyPlayers[i].teamNumber;
        }
        startsAssignBatch(&sim->sim, &sim->sim.ss,
                          sim->playerConnected, batchTeam,
                          sim->sim.pendingStartIdx);
    }

    /* Create tanks for all connected players */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        /* Clean up any existing tank/lgm (shouldn't exist, but be safe) */
        if (sim->sim.tanks[i] != NULL) {
            tankDestroy(&sim->sim, &sim->sim.tanks[i]);
            sim->sim.tanks[i] = NULL;
        }
        if (sim->sim.lgmen[i] != NULL) {
            lgmDestroy(&sim->sim.lgmen[i]);
            sim->sim.lgmen[i] = NULL;
        }
        tankCreate(&sim->sim, &sim->sim.tanks[i]);
        sim->sim.lgmen[i] = lgmCreate(i);
    }
    serverSimStaggerBaseTimers(sim);

    sim->state = serverStateRunning;
    serverSimConsoleMessage("Game started!");

    /* A snapshot will be written on the first running tick
     * (tick 0 % FULL_SYNC_INTERVAL == 0). */
}

bool serverSimChangeMap(ServerSim *sim, char *mapFileName) {
    BYTE tempBuf[65536];
    int len;

    if (sim->state != serverStateLobby) {
        return FALSE;
    }

    /* Clear the existing map to DEEP_SEA before loading.  mapRead() only
     * writes tiles within its "runs" — positions outside runs are expected
     * to already be DEEP_SEA.  Without this reset, old-game terrain leaks
     * into areas that should be deep sea in the new map. */
    memset((*sim->sim.mp).mapItem, DEEP_SEA,
           sizeof((*sim->sim.mp).mapItem));

    /* Load the new map */
    if (mapRead(mapFileName, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss) == FALSE) {
        return FALSE;
    }

    /* Hash the canonical BMAPBOLO file so WBN can match it. */
    serverSimCacheMapMd5FromFile(sim, mapFileName);

    basesClearMines(&sim->sim);

    /* Update cached map data */
    len = serverSimGetCompressedMap(sim, tempBuf);
    if (sim->cachedMapData != NULL) {
        free(sim->cachedMapData);
    }
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData != NULL) {
        memcpy(sim->cachedMapData, tempBuf, len);
        sim->cachedMapDataLen = len;
    }

    /* Update map name (basename without path or .map extension) */
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

    /* Map changed — unready humans, keep bots ready, abort any
     * in-flight countdown, and republish affected slots. The earlier
     * bare loop here cleared bots' ready flags too, which silently
     * blocked the next all-ready check (bot.ready=FALSE → countdown
     * never started). Match the serverSimReloadMap pattern, which
     * delegates to serverSimApplyMapChange → lobbyAutoUnreadyOnChange. */
    lobbyAutoUnreadyOnChange(sim);

    return TRUE;
}

void serverSimSendWbnWinEvents(ServerSim *sim) {
    BYTE count;
    BYTE max;
    BYTE first = NEUTRAL;
    BYTE current;
    bool allOwned = TRUE;

    max = basesGetNumBases(&sim->sim.bs);

    /* Find the winning alliance — same logic as serverSimBuildWinMessage */
    for (count = 1; count <= max && allOwned; count++) {
        BYTE shellsAmt, minesAmt, armourAmt;
        current = basesGetBaseOwner(&sim->sim.bs, count);
        basesGetStats(&sim->sim.bs, count, &shellsAmt, &minesAmt, &armourAmt);
        if (current == NEUTRAL || armourAmt <= MIN_ARMOUR_CAPTURE) {
            allOwned = FALSE;
        } else if (count == 1) {
            first = current;
        } else {
            allOwned = playersIsAllie(&sim->sim.plyrs, current, first);
        }
    }

    if (!allOwned || max == 0) {
        return;
    }

    /* Send a win event for each player in the winning alliance */
    for (count = 0; count < MAX_TANKS; count++) {
        if (!sim->playerConnected[count]) continue;
        if (playersIsAllie(&sim->sim.plyrs, count, first) || count == first) {
            winbolonetAddEvent(WINBOLO_NET_EVENT_WIN, TRUE, count, WINBOLO_NET_NO_PLAYER,
                               botManagerIsBot(sim, count), FALSE);
        }
    }
}

bool serverSimBuildWinMessage(ServerSim *sim, char *buf, size_t bufSize) {
    BYTE count;
    BYTE max;
    BYTE first = NEUTRAL;
    BYTE current;
    bool allOwned = TRUE;
    char name[256];
    size_t pos;

    max = basesGetNumBases(&sim->sim.bs);

    /* Check if all bases are owned by the same alliance */
    for (count = 1; count <= max && allOwned; count++) {
        BYTE shellsAmt, minesAmt, armourAmt;
        current = basesGetBaseOwner(&sim->sim.bs, count);
        basesGetStats(&sim->sim.bs, count, &shellsAmt, &minesAmt, &armourAmt);
        if (current == NEUTRAL || armourAmt <= MIN_ARMOUR_CAPTURE) {
            allOwned = FALSE;
        } else if (count == 1) {
            first = current;
        } else {
            allOwned = playersIsAllie(&sim->sim.plyrs, current, first);
        }
    }

    if (!allOwned || max == 0) {
        snprintf(buf, bufSize, "Game over!");
        return FALSE;
    }

    /* Build winner message */
    pos = 0;
    pos += snprintf(buf + pos, bufSize - pos, "Game Won! Winners:");
    for (count = 0; count < MAX_TANKS && pos < bufSize - 1; count++) {
        if (!sim->playerConnected[count]) continue;
        if (playersIsAllie(&sim->sim.plyrs, count, first) || count == first) {
            playersGetPlayerName(&sim->sim.plyrs, count, name, TRUE);
            pos += snprintf(buf + pos, bufSize - pos, " %s", name);
        }
    }

    return TRUE;
}

void serverSimSendWbnSurrenderWinEvents(ServerSim *sim, uint8_t surrenderTeam) {
    BYTE count;
    if (surrenderTeam == 0) return;

    /* A surrender doesn't sweep the bases, so the base-ownership winner
     * logic never fires. The winners are the opposing side: every
     * connected player on a real team other than the one that gave up. */
    for (count = 0; count < MAX_TANKS; count++) {
        uint8_t t;
        if (!sim->playerConnected[count]) continue;
        t = sim->lobbyPlayers[count].teamNumber;
        if (t == 0 || t == surrenderTeam) continue;
        winbolonetAddEvent(WINBOLO_NET_EVENT_WIN, TRUE, count, WINBOLO_NET_NO_PLAYER,
                           botManagerIsBot(sim, count), FALSE);
    }
}

bool serverSimBuildSurrenderWinMessage(ServerSim *sim, uint8_t surrenderTeam,
                                       char *buf, size_t bufSize) {
    BYTE count;
    char name[256];
    size_t pos;
    bool any = FALSE;
    const char *tname;

    if (surrenderTeam == 0) {
        snprintf(buf, bufSize, "Game over!");
        return FALSE;
    }

    /* Lead with the same surrender announcement shown in-game, so the
     * returning lobby explains why the round ended above the winners. */
    tname = sim->teams[surrenderTeam].name[0]
            ? sim->teams[surrenderTeam].name : "?";
    pos = 0;
    pos += snprintf(buf + pos, bufSize - pos,
                    "*** Team %s has surrendered. ***", tname);

    /* The opposing side wins; list them on a second line. If nobody from
     * the winning side is still connected, the surrender line stands alone. */
    for (count = 0; count < MAX_TANKS && pos < bufSize - 1; count++) {
        uint8_t t;
        if (!sim->playerConnected[count]) continue;
        t = sim->lobbyPlayers[count].teamNumber;
        if (t == 0 || t == surrenderTeam) continue;
        if (!any) {
            pos += snprintf(buf + pos, bufSize - pos, "\nGame Won! Winners:");
            any = TRUE;
        }
        playersGetPlayerName(&sim->sim.plyrs, count, name, TRUE);
        pos += snprintf(buf + pos, bufSize - pos, " %s", name);
    }

    return TRUE;
}

bool serverSimCheckEmptyReset(ServerSim *sim) {
    BYTE numPlayers = serverSimGetNumPlayers(sim);

    if (numPlayers > 0) {
        /* Players present — reset the timer */
        sim->emptyResetTicks = -1;
        return FALSE;
    }

    /* No players — start or continue countdown */
    if (sim->emptyResetTicks < 0) {
        /* Start the countdown: minutes * 60 seconds * 50 ticks/sec */
        sim->emptyResetTicks = sim->emptyResetMinutes * 60 * 50;
        serverSimConsoleMessage("No players connected. Empty reset timer started.");
    }

    sim->emptyResetTicks--;
    if (sim->emptyResetTicks <= 0) {
        return TRUE;
    }

    return FALSE;
}

bool serverSimScanMapDir(const char *dirPath,
                         char ***outFiles, int *outCount) {
    char fullPath[2048];
    char **tempList = NULL;
    int tempCount = 0;
    int tempCapacity = 0;
    int globCount = 0;
    int i;
    map mp;
    pillboxes pb;
    bases bs;
    starts ss;

    /* Use SDL3's cross-platform directory globbing */
    char **files = SDL_GlobDirectory(dirPath, "*.map", 0, &globCount);
    if (files == NULL || globCount == 0) {
        fprintf(stderr, "Error: no .map files found in '%s'\n", dirPath);
        if (files) SDL_free(files);
        return FALSE;
    }

    for (i = 0; i < globCount; i++) {
        snprintf(fullPath, sizeof(fullPath), "%s/%s", dirPath, files[i]);

        /* Validate map by attempting to load it */
        mapCreate(&mp);
        pillsCreate(&pb);
        basesCreate(&bs);
        startsCreate(&ss);
        if (mapRead(fullPath, &mp, &pb, &bs, &ss) == FALSE) {
            mapDestroy(&mp);
            pillsDestroy(&pb);
            basesDestroy(&bs);
            startsDestroy(&ss);
            fprintf(stderr, "Warning: skipping invalid map '%s'\n", fullPath);
            continue;
        }
        mapDestroy(&mp);
        pillsDestroy(&pb);
        basesDestroy(&bs);
        startsDestroy(&ss);

        /* Grow array if needed */
        if (tempCount >= tempCapacity) {
            int newCap = tempCapacity == 0 ? 16 : tempCapacity * 2;
            char **newList = realloc(tempList, newCap * sizeof(char *));
            if (newList == NULL) {
                fprintf(stderr, "Error: out of memory building map list\n");
                break;
            }
            tempList = newList;
            tempCapacity = newCap;
        }

        tempList[tempCount] = SDL_strdup(fullPath);
        if (tempList[tempCount] == NULL) {
            fprintf(stderr, "Error: out of memory duplicating path\n");
            break;
        }
        tempCount++;
        fprintf(stderr, "Map directory: validated '%s'\n", files[i]);
    }

    SDL_free(files);

    if (tempCount == 0) {
        fprintf(stderr, "Error: no valid .map files found in '%s'\n", dirPath);
        free(tempList);
        return FALSE;
    }

    *outFiles = tempList;
    *outCount = tempCount;
    return TRUE;
}

bool serverSimMapDirBuild(ServerSim *sim, const char *dirPath) {
    char **files = NULL;
    int count = 0;
    if (!serverSimScanMapDir(dirPath, &files, &count)) {
        return FALSE;
    }
    sim->mapDirFiles = files;
    sim->mapDirCount = count;

    /* Capture the dirPath as the canonical server-side map root so
     * serverSimEnumerateMapDir / serverSimSearchMapDir / SET_MAP path
     * resolution all read from the same place the rotation list was
     * built from. Strip a trailing slash to keep concatenations
     * ("<root>/<rel>") clean. */
    if (sim->mapDirPath) {
        free(sim->mapDirPath);
        sim->mapDirPath = NULL;
    }
    sim->mapDirPath = SDL_strdup(dirPath);
    if (sim->mapDirPath != NULL) {
        size_t plen = SDL_strlen(sim->mapDirPath);
        while (plen > 1 && (sim->mapDirPath[plen - 1] == '/' ||
                            sim->mapDirPath[plen - 1] == '\\')) {
            sim->mapDirPath[--plen] = '\0';
        }
    }

    fprintf(stderr, "Map directory: %d valid map(s) loaded from '%s'\n",
            count, dirPath);
    return TRUE;
}

const char *serverSimGetMapDirRoot(const ServerSim *sim) {
    if (sim != NULL && sim->mapDirPath != NULL && sim->mapDirPath[0] != '\0') {
        return sim->mapDirPath;
    }
    return "data/maps";
}

bool serverSimMapDirPickRandom(ServerSim *sim) {
    int idx;
    char msg[512];

    if (sim->mapDirFiles == NULL || sim->mapDirCount <= 0) {
        return FALSE;
    }

    idx = (int)bolo_rand_below((uint32_t)sim->mapDirCount);

    /* Try to avoid picking the same map we're already on */
    if (sim->mapDirCount > 1) {
        int attempts;
        for (attempts = 0; attempts < 10; attempts++) {
            const char *base = sim->mapDirFiles[idx];
            const char *p;
            for (p = sim->mapDirFiles[idx]; *p; p++) {
                if (*p == '/' || *p == '\\') base = p + 1;
            }
            if (strcmp(base, sim->mapName) != 0) break;
            idx = (int)bolo_rand_below((uint32_t)sim->mapDirCount);
        }
    }

    if (serverSimChangeMap(sim, sim->mapDirFiles[idx]) == FALSE) {
        /* Map may have been deleted or corrupted since startup — try others */
        int tries;
        for (tries = 0; tries < sim->mapDirCount; tries++) {
            idx = (idx + 1) % sim->mapDirCount;
            if (serverSimChangeMap(sim, sim->mapDirFiles[idx]) == TRUE) {
                break;
            }
        }
        if (tries >= sim->mapDirCount) {
            fprintf(stderr, "Error: all maps in directory failed to load\n");
            return FALSE;
        }
    }

    snprintf(msg, sizeof(msg), "Map rotation: loaded '%s'", sim->mapName);
    serverSimConsoleMessage(msg);
    serverSimApplyMapChange(sim);
    return TRUE;
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
    serverSimPublishControl(sim, &evt);
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

bool serverSimConsumeSuppressNextWinMessage(ServerSim *sim) {
    if (!sim) return false;
    bool v = sim->suppressNextWinMessage;
    sim->suppressNextWinMessage = false;
    return v;
}

void serverSimSetSuppressNextWinMessage(ServerSim *sim, bool v) {
    if (sim) sim->suppressNextWinMessage = v;
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
    sim->baseMonopolyTriggeredThisRound = false;
    sim->returnToLobbyTicks = 0;
    sim->surrenderTeamId = 0;
    sim->returnToLobbyByVote = false;
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
        /* A player-initiated vote leaves a line in the returning lobby
         * explaining why the round ended (no in-game newswire line —
         * clients already render the 3/2/1 countdown). Base-monopoly
         * auto-votes keep their existing winner reporting and don't add
         * this line. */
        if (gv->triggerSrc == GAME_VOTE_TRIGGER_MANUAL) {
            sim->returnToLobbyByVote = true;
        }
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
        sim->surrenderTeamId = gv->teamId;
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

/* Detect "one team owns every base" and auto-start a back-to-lobby vote. */
static void gameVoteCheckBaseMonopoly(ServerSim *sim, uint64_t nowMs) {
    if (sim->state != serverStateRunning) return;
    if (sim->baseMonopolyTriggeredThisRound) return;

    /* Count bases per team-via-owner-player. */
    int nBases = basesGetNumBases(&sim->sim.bs);
    if (nBases <= 0) return;

    uint8_t teamCount[MAX_TANKS] = {0};
    uint8_t neutral = 0;
    int i;
    for (i = 0; i < nBases; i++) {
        BYTE owner = basesGetBaseOwner(&sim->sim.bs, (BYTE)(i + 1));
        if (owner >= MAX_TANKS) { neutral++; continue; }
        if (!sim->playerConnected[owner]) { neutral++; continue; }
        uint8_t t = sim->lobbyPlayers[owner].teamNumber;
        if (t == 0 || t >= MAX_TANKS) { neutral++; continue; }
        teamCount[t]++;
    }
    if (neutral > 0) return;

    int teamsWithBases = 0;
    uint8_t monoTeam = 0;
    int t;
    for (t = 1; t < MAX_TANKS; t++) {
        if (teamCount[t] > 0) { teamsWithBases++; monoTeam = (uint8_t)t; }
    }
    if (teamsWithBases != 1) return;

    /* Require >1 active team in play to make "monopoly" meaningful. */
    if (serverSimCountActiveTeams(sim) < 2) return;

    struct ServerGameVote *gv = gameVoteSlot(sim, GAME_VOTE_KIND_BACK_TO_LOBBY);
    if (!gv) return;
    if (gv->active == GAME_VOTE_ACTIVE_RUNNING) return;

    char buf[128];
    const char *tname = sim->teams[monoTeam].name[0]
                        ? sim->teams[monoTeam].name : "?";
    snprintf(buf, sizeof(buf),
             "Team %s controls every base. Returning to lobby on unanimous vote.",
             tname);
    publishServerMessage(sim, buf);

    gameVoteStart(sim, GAME_VOTE_KIND_BACK_TO_LOBBY,
                  GAME_VOTE_TRIGGER_BASE_MONOPOLY, monoTeam, nowMs, NEUTRAL);
    /* Pre-cast YES for every eligible voter. */
    gv->votesMask    = gameVoteEligibleMask(sim, GAME_VOTE_KIND_BACK_TO_LOBBY, 0);
    gv->answeredMask = gv->votesMask;
    publishGameVoteState(sim, GAME_VOTE_KIND_BACK_TO_LOBBY);

    sim->baseMonopolyTriggeredThisRound = true;
}

void serverSimGameVoteTick(ServerSim *sim, uint64_t nowMs) {
    sim->gameVoteWallMs = nowMs;

    /* Auto-trigger checks before per-slot servicing. */
    gameVoteCheckBaseMonopoly(sim, nowMs);

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
                publishServerMessage(sim, "Surrender vote cancelled — team count changed.");
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_CANCELLED, nowMs);
                continue;
            }

            /* If the surrendering team or the voter pool has emptied
             * entirely (everyone disconnected), the vote is moot. */
            if (popcount16(gameVoteEligibleMask(sim, gv->kind, gv->teamId)) == 0) {
                if (gv->kind == GAME_VOTE_KIND_SURRENDER) {
                    publishServerMessage(sim,
                        "Surrender vote cancelled — surrendering team is empty.");
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

void serverSimMapDirDestroy(ServerSim *sim) {
    if (sim->mapDirFiles != NULL) {
        int i;
        for (i = 0; i < sim->mapDirCount; i++) {
            free(sim->mapDirFiles[i]);
        }
        free(sim->mapDirFiles);
        sim->mapDirFiles = NULL;
        sim->mapDirCount = 0;
    }
    if (sim->mapDirPath != NULL) {
        SDL_free(sim->mapDirPath);
        sim->mapDirPath = NULL;
    }
}

/* ---------------------------------------------------------------------- */
/* Subscriber registry                                                    */
/* ---------------------------------------------------------------------- */

#define SUBSCRIBER_SLOT_COUNT (MAX_TANKS + 1)
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
    evt->u.lobbySettings.inLobby          = sim->lobbyEnabled ? true : false;
    evt->u.lobbySettings.lobbyOpenHost            = sim->openHost;
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

    if (evt->type != CTRL_PLAYER_JOIN) {
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
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i]) {
            memset(&evt, 0, sizeof(evt));
            serverSimFillLobbySlotEvent(sim, i, &evt);
            deliver(ctx, &evt);
        }
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
    mpDiagLog("[bus] SYNC-REPLAY end");
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

/*********************************************************
 * Read accessors.
 *********************************************************/

bool serverSimBalanceShutdownRequested(const ServerSim *sim) {
    /* SDL_GetAtomicInt takes a non-const pointer for ABI reasons,
     * but the operation is a read. Const-cast is the standard
     * pattern for this SDL API. */
    return SDL_GetAtomicInt(
        (SDL_AtomicInt *)&sim->balanceProposal.shutdownFlag) != 0;
}

bool serverSimIsLobbyEnabled(const ServerSim *sim) {
    return sim->lobbyEnabled;
}

bool serverSimHasPassword(const ServerSim *sim) {
    return sim->hasPassword;
}

bool serverSimIsRandomMapEnabled(const ServerSim *sim) {
    return sim->randomMapEnabled;
}

bool serverSimIsQuiet(const ServerSim *sim) {
    return sim->isServerQuiet;
}

bool serverSimGetServerMessageUseLogFile(const ServerSim *sim) {
    return sim->serverMessageUseLogFile;
}

ServerState serverSimGetState(const ServerSim *sim) {
    return sim->state;
}

aiType serverSimGetBotAiType(const ServerSim *sim) {
    return sim->botAiType;
}

uint32_t serverSimGetTick(const ServerSim *sim) {
    return sim->tick;
}

uint32_t serverSimGetTimeCreated(const ServerSim *sim) {
    return sim->timeCreated;
}

int32_t serverSimGetStartDelay(const ServerSim *sim) {
    return sim->startDelay;
}

int32_t serverSimGetGameLength(const ServerSim *sim) {
    return sim->gameLength;
}

int32_t serverSimGetCountdownTicks(const ServerSim *sim) {
    return sim->countdownTicks;
}

unsigned short serverSimGetServerPort(const ServerSim *sim) {
    return sim->serverPort;
}

int serverSimGetMapDirCount(const ServerSim *sim) {
    return sim->mapDirCount;
}

uint8_t serverSimGetEventCount(const ServerSim *sim) {
    return sim->eventCount;
}

uint16_t serverSimGetMapEventCount(const ServerSim *sim) {
    return sim->mapEventCount;
}

const char *serverSimGetMapName(const ServerSim *sim) {
    return sim->mapName;
}

const char *serverSimGetBotBrainPath(const ServerSim *sim) {
    return sim->botBrainPath;
}

const char *serverSimGetServerMessageLogFile(const ServerSim *sim) {
    return sim->serverMessageLogFile;
}

const LobbyPlayer *serverSimGetLobbyPlayer(const ServerSim *sim, BYTE n) {
    if (n >= MAX_TANKS) return NULL;
    return &sim->lobbyPlayers[n];
}

LobbyPlayer *serverSimGetLobbyPlayerMut(ServerSim *sim, BYTE n) {
    if (n >= MAX_TANKS) return NULL;
    return &sim->lobbyPlayers[n];
}

bool serverSimIsPlayerConnected(const ServerSim *sim, BYTE n) {
    if (n >= MAX_TANKS) return false;
    return sim->playerConnected[n];
}

uint32_t serverSimGetLastProcessedInput(const ServerSim *sim, BYTE n) {
    if (n >= MAX_TANKS) return 0;
    return sim->lastProcessedInput[n];
}

bool serverSimIsMapSkipVote(const ServerSim *sim, BYTE n) {
    if (n >= MAX_TANKS) return false;
    return sim->mapSkipVotes[n];
}

char *const *serverSimGetMapDirFiles(const ServerSim *sim) {
    return sim->mapDirFiles;
}

const GameEvent *serverSimGetEvents(const ServerSim *sim) {
    return sim->events;
}

const GameEvent *serverSimGetMapEvents(const ServerSim *sim) {
    return sim->mapEvents;
}

const BalanceProposal *serverSimGetBalanceProposal(const ServerSim *sim) {
    return &sim->balanceProposal;
}

GameSim *serverSimGetGameSim(ServerSim *sim) {
    return &sim->sim;
}

BYTE serverSimGetViewPlayer(const ServerSim *sim) {
    return sim->sim.viewPlayer;
}

void serverSimSetViewPlayer(ServerSim *sim, BYTE playerNum) {
    sim->sim.viewPlayer = playerNum;
}

bool serverSimIsTutorial(const ServerSim *sim) {
    return sim->sim.isTutorial;
}

void serverSimSetTutorial(ServerSim *sim, bool v) {
    sim->sim.isTutorial = v;
}

void serverSimSetPaused(ServerSim *sim, bool paused) {
    sim->sim.paused = paused;
}

/* --- Live-sim map / pill / base / start readers --- */

BYTE serverSimGetMapTerrain(const ServerSim *sim, BYTE x, BYTE y) {
    return mapGetPos(&((ServerSim *)sim)->sim.mp, x, y);
}

bool serverSimMapIsMine(const ServerSim *sim, BYTE x, BYTE y) {
    return mapIsMine(&((ServerSim *)sim)->sim.mp, x, y);
}

bool serverSimPillExistsAt(const ServerSim *sim, BYTE x, BYTE y) {
    return pillsExistPos(&((ServerSim *)sim)->sim.pb, x, y);
}

BYTE serverSimPillGetScreenHealthAt(ServerSim *sim, BYTE x, BYTE y, BYTE viewPlayer) {
    return pillsGetScreenHealth(&sim->sim, &sim->sim.pb, x, y, viewPlayer);
}

bool serverSimBaseExistsAt(const ServerSim *sim, BYTE x, BYTE y) {
    return basesExistPos(&((ServerSim *)sim)->sim.bs, x, y);
}

baseAlliance serverSimBaseGetAllianceAt(ServerSim *sim, BYTE x, BYTE y, BYTE viewPlayer) {
    return basesGetAlliancePos(&sim->sim, x, y, viewPlayer);
}

bool serverSimBaseAmOwnerAt(ServerSim *sim, BYTE player, BYTE x, BYTE y) {
    return basesAmOwner(&sim->sim, player, x, y);
}

BYTE serverSimGetPillCount(const ServerSim *sim) {
    return pillsGetNumPills(&((ServerSim *)sim)->sim.pb);
}

BYTE serverSimGetBaseCount(const ServerSim *sim) {
    return basesGetNumBases(&((ServerSim *)sim)->sim.bs);
}

BYTE serverSimGetStartCount(const ServerSim *sim) {
    return startsGetNumStarts(&((ServerSim *)sim)->sim.ss);
}

bool serverSimGetPill(ServerSim *sim, BYTE i,
                      BYTE *x, BYTE *y, BYTE *owner, BYTE *armour,
                      bool *inTank) {
    pillbox p;
    BYTE n = pillsGetNumPills(&sim->sim.pb);
    if (i == 0 || i > n) return false;
    pillsGetPill(&sim->sim.pb, &p, i);
    if (x)      *x      = p.x;
    if (y)      *y      = p.y;
    if (owner)  *owner  = p.owner;
    if (armour) *armour = p.armour;
    if (inTank) *inTank = p.inTank;
    return true;
}

bool serverSimGetBase(ServerSim *sim, BYTE i,
                      BYTE *x, BYTE *y, BYTE *owner) {
    base b;
    BYTE n = basesGetNumBases(&sim->sim.bs);
    if (i == 0 || i > n) return false;
    basesGetBase(&sim->sim.bs, &b, i);
    if (x)     *x     = b.x;
    if (y)     *y     = b.y;
    if (owner) *owner = b.owner;
    return true;
}

bool serverSimGetBaseStats(ServerSim *sim, BYTE i,
                           BYTE *shells, BYTE *mines, BYTE *armour) {
    BYTE n = basesGetNumBases(&sim->sim.bs);
    if (i == 0 || i > n) return false;
    basesGetStats(&sim->sim.bs, i, shells, mines, armour);
    return true;
}

bool serverSimGetStart(ServerSim *sim, BYTE i,
                       BYTE *x, BYTE *y, BYTE *dir) {
    start s;
    BYTE n = startsGetNumStarts(&sim->sim.ss);
    if (i == 0 || i > n) return false;
    startsGetStartStruct(&sim->sim.ss, &s, i);
    if (x)   *x   = s.x;
    if (y)   *y   = s.y;
    if (dir) *dir = startsConvertDir((BYTE)((s.dir < 16) ? s.dir : 0));
    return true;
}

/* --- Live-sim render-state readers --- */

bool serverSimGetTankRender(ServerSim *sim, BYTE i, TankRenderInfo *out) {
    if (i >= MAX_TANKS) return false;
    tank *t = &sim->sim.tanks[i];
    if (*t == NULL) return false;
    tankGetWorld(t, &out->world_x, &out->world_y);
    out->dir     = tankGetDir(t);
    out->on_boat = tankIsOnBoat(t);
    out->alive   = (tankGetDeathWait(t) == 0);
    return true;
}

tankAlliance serverSimGetTankAllianceFor(ServerSim *sim,
                                         BYTE selfPlayer,
                                         BYTE tankNum) {
    return playersScreenAllience(&sim->sim.plyrs, selfPlayer, tankNum);
}

int serverSimGetShellSnapshot(ServerSim *sim, ShellRender out[], int cap) {
    int n = 0;
    for (shells q = sim->sim.shs; q != NULL && n < cap; q = q->next) {
        if (q->shellDead) continue;
        out[n].x     = q->x;
        out[n].y     = q->y;
        out[n].angle = q->angle;
        n++;
    }
    return n;
}

int serverSimGetExplosionSnapshot(ServerSim *sim, ExplosionRender out[], int cap) {
    int n = 0;
    for (explosions q = sim->sim.expl; q != NULL && n < cap; q = q->next) {
        out[n].mx     = q->mx;
        out[n].my     = q->my;
        out[n].px     = q->px;
        out[n].py     = q->py;
        out[n].length = q->length;
        n++;
    }
    return n;
}

bool serverSimGetLgmRender(ServerSim *sim, BYTE i, LgmRender *out) {
    if (i >= MAX_TANKS) return false;
    lgm *l = &sim->sim.lgmen[i];
    if (*l == NULL) return false;
    if ((*l)->inTank || (*l)->isDead) return false;
    out->x     = (*l)->x;
    out->y     = (*l)->y;
    out->frame = (*l)->frame;
    return true;
}

int serverSimGetTankExplosionSnapshot(ServerSim *sim,
                                      TankExplosionRender out[], int cap) {
    int n = 0;
    for (tkExplosion q = sim->sim.tankExplosions; q != NULL && n < cap; q = q->next) {
        out[n].x = q->x;
        out[n].y = q->y;
        n++;
    }
    return n;
}

bool serverSimMineExistsAt(ServerSim *sim, BYTE x, BYTE y) {
    return minesExistPos(&sim->sim.mns, &sim->sim.mp, x, y);
}

/* ────────────────────────────────────────────────────────────────
 * Map preview / map upload — Lobby Layout A
 * ──────────────────────────────────────────────────────────────── */

static bool serverSimApplyRandomMapConfig(ServerSim *sim,
                                          const MapGenConfig *cfg) {
    BYTE tempBuf[131072];
    int len;
    int x, y;

    memset((*sim->sim.mp).mapItem, DEEP_SEA, sizeof((*sim->sim.mp).mapItem));
    for (x = 0; x < 256; x++) {
        for (y = 0; y < 256; y++) {
            if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
            }
        }
    }

    sim->sim.pb->numPills  = 0;
    sim->sim.bs->numBases  = 0;
    sim->sim.ss->numStarts = 0;
    mapGenRun(sim->sim.mp, sim->sim.bs, sim->sim.pb, sim->sim.ss, cfg);

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

    len = serverSimGetCompressedMap(sim, tempBuf);
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = (BYTE *)malloc(len);
    if (sim->cachedMapData == NULL) {
        sim->cachedMapDataLen = 0;
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    mapGenBuildDisplayName(cfg, sim->mapName, MAP_STR_SIZE);

    {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            if (!sim->lobbyPlayers[i].isBot) {
                sim->lobbyPlayers[i].ready = FALSE;
            }
        }
    }
    return TRUE;
}

bool serverSimReloadMap(ServerSim *sim, const char *mapFileName) {
    BYTE tempBuf[131072];
    int len;
    char msg[256];

    if (sim == NULL || mapFileName == NULL || mapFileName[0] == '\0') {
        mpDiagLog("[srv] reloadMap REJECTED (null arg)");
        return FALSE;
    }
    mpDiagLog("[srv] reloadMap ENTER file='%.64s'", mapFileName);
    if (sim->state != serverStateLobby) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadMap rejected: state=%d (not lobby)",
            (int)sim->state);
        mpDiagLog("[srv] reloadMap REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }

    /* Stash the currently-committed map as the "previous" snapshot
     * before we touch the sim. Keep the ORIGINAL committed map
     * across a chain of previews so one Cancel rolls all the way
     * back to where the user started. */
    if (sim->previousMapData == NULL && sim->cachedMapData != NULL) {
        sim->previousMapData = (BYTE *)malloc(sim->cachedMapDataLen);
        if (sim->previousMapData) {
            memcpy(sim->previousMapData, sim->cachedMapData,
                   sim->cachedMapDataLen);
            sim->previousMapDataLen = sim->cachedMapDataLen;
            memcpy(sim->previousMapName, sim->mapName,
                   sizeof(sim->previousMapName));
        }
    }

    /* Wipe the existing map/pill/base/start contents before mapRead
     * touches them. mapRead's RLE-decoder only writes cells encoded
     * in the new file — any tile NOT included in the new map's runs
     * would otherwise keep the previous map's value. */
    {
        int x, y;
        memset((*sim->sim.mp).mapItem, DEEP_SEA,
               sizeof((*sim->sim.mp).mapItem));
        for (x = 0; x < 256; x++) {
            for (y = 0; y < 256; y++) {
                if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                    y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                    (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
                }
            }
        }
        sim->sim.pb->numPills = 0;
        sim->sim.bs->numBases = 0;
        sim->sim.ss->numStarts = 0;
    }

    if (mapRead((char *)mapFileName,
                &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss) == FALSE) {
        /* Roll the wipe back. cachedMapData still holds the previous
         * map's compressed bytes (the refresh below only runs on the
         * success path), so restoring from it returns the live
         * structures to whatever the lobby was showing before this
         * call — leaving the caller with an unchanged sim is much
         * less surprising than a half-wiped one, especially for the
         * upload path where a malformed map shouldn't blank the
         * host's lobby. */
        if (sim->cachedMapData != NULL && sim->cachedMapDataLen > 0) {
            (void)mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb,
                                        &sim->sim.bs, &sim->sim.ss,
                                        sim->cachedMapData,
                                        sim->cachedMapDataLen);
        }
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSimReloadMap: mapRead failed for '%s'", mapFileName);
        return FALSE;
    }

    /* Hash the canonical BMAPBOLO file so WBN can match it. This is the
     * path the lobby map chooser uses (CMD_LOBBY_SET_MAP). */
    serverSimCacheMapMd5FromFile(sim, mapFileName);

    basesClearMines(&sim->sim);

    /* Update map name from basename, strip .map suffix. */
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
        {
            size_t nameLen = strlen(sim->mapName);
            if (nameLen >= 4 &&
                strcmp(sim->mapName + nameLen - 4, ".map") == 0) {
                sim->mapName[nameLen - 4] = '\0';
            }
        }
    }

    /* Refresh cached compressed map. */
    len = serverSimGetCompressedMap(sim, tempBuf);
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData == NULL) {
        sim->cachedMapDataLen = 0;
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    /* Random-map provenance no longer applies. */
    sim->randomMapEnabled = false;

    snprintf(msg, sizeof(msg), "Map changed to %s", sim->mapName);
    serverSimConsoleMessage(msg);

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimReloadMap: now '%s' (%d compressed bytes)",
        sim->mapName, sim->cachedMapDataLen);

    serverSimApplyMapChange(sim);
    return TRUE;
}

bool serverSimReloadCompressedInMemory(ServerSim *sim,
                                       const uint8_t *bytes, int len,
                                       const char *mapName) {
    BYTE tempBuf[131072];
    int compressedLen;
    char msg[256];

    if (sim == NULL || bytes == NULL || len <= 0 ||
        mapName == NULL || mapName[0] == '\0') {
        mpDiagLog("[srv] reloadCompressedInMemory REJECTED (null/empty arg)");
        return FALSE;
    }
    mpDiagLog("[srv] reloadCompressedInMemory ENTER name='%.32s' len=%d",
              mapName, len);
    if (sim->state != serverStateLobby) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadCompressedInMemory rejected: state=%d (not lobby)",
            (int)sim->state);
        mpDiagLog("[srv] reloadCompressedInMemory REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }

    /* Stash the currently-committed map as the "previous" snapshot
     * before we touch the sim. Keep the ORIGINAL committed map
     * across a chain of previews so one Cancel rolls all the way
     * back to where the user started. */
    if (sim->previousMapData == NULL && sim->cachedMapData != NULL) {
        sim->previousMapData = (BYTE *)malloc(sim->cachedMapDataLen);
        if (sim->previousMapData) {
            memcpy(sim->previousMapData, sim->cachedMapData,
                   sim->cachedMapDataLen);
            sim->previousMapDataLen = sim->cachedMapDataLen;
            memcpy(sim->previousMapName, sim->mapName,
                   sizeof(sim->previousMapName));
        }
    }

    /* Wipe the existing map/pill/base/start contents before the
     * decoder touches them. mapLoadCompressedMap's RLE-decoder only
     * writes cells encoded in the new blob — any tile NOT included
     * in the new map's runs would otherwise keep the previous map's
     * value. */
    {
        int x, y;
        memset((*sim->sim.mp).mapItem, DEEP_SEA,
               sizeof((*sim->sim.mp).mapItem));
        for (x = 0; x < 256; x++) {
            for (y = 0; y < 256; y++) {
                if (x <= MAP_MINE_EDGE_LEFT || x >= MAP_MINE_EDGE_RIGHT ||
                    y <= MAP_MINE_EDGE_TOP  || y >= MAP_MINE_EDGE_BOTTOM) {
                    (*sim->sim.mp).mapItem[x][y] = DEEP_SEA;
                }
            }
        }
        sim->sim.pb->numPills = 0;
        sim->sim.bs->numBases = 0;
        sim->sim.ss->numStarts = 0;
    }

    /* The wire / upload / WBN paths all hand us a full .map file
     * (starting with the BMAPBOLO magic + version + counts header).
     * mapLoadCompressedMap expects a different on-the-wire layout
     * (raw bases/pills/starts struct dump + LZW map), so feeding it
     * the .map file bytes misaligns every field. Detect the magic
     * and route through mapRead via a temp file when it matches.
     * Fall back to the legacy mapLoadCompressedMap path for any
     * future caller passing compressed-map-format bytes directly. */
    bool loadedOk = FALSE;
    if (len >= (int)(sizeof(MAP_HEADER) - 1) &&
        memcmp(bytes, MAP_HEADER, sizeof(MAP_HEADER) - 1) == 0) {
        char tmpPath[FILENAME_MAX];
        SDL_snprintf(tmpPath, sizeof(tmpPath),
                     "%s/.tmp_inmem_reload.map",
                     serverSimGetMapDirRoot(sim));
        FILE *tf = fopen(tmpPath, "wb");
        if (tf == NULL) {
            WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                "serverSimReloadCompressedInMemory: temp open failed '%s'",
                tmpPath);
            return FALSE;
        }
        size_t wrote = fwrite(bytes, 1, (size_t)len, tf);
        fclose(tf);
        if (wrote != (size_t)len) {
            remove(tmpPath);
            WB_LOG_ERROR(WB_LOG_CAT_SERVER,
                "serverSimReloadCompressedInMemory: temp write short (%zu/%d)",
                wrote, len);
            return FALSE;
        }
        loadedOk = (mapRead(tmpPath, &sim->sim.mp, &sim->sim.pb,
                            &sim->sim.bs, &sim->sim.ss) == TRUE);
        remove(tmpPath);
        /* These bytes are the canonical BMAPBOLO .map file — hash them
         * so WBN can match the map against its library. */
        if (loadedOk) {
            md5Compute(bytes, (size_t)len, sim->mapMd5);
            sim->mapMd5Valid = TRUE;
        }
    } else {
        loadedOk = (mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb,
                                         &sim->sim.bs, &sim->sim.ss,
                                         (BYTE *)bytes, len) == TRUE);
        sim->mapMd5Valid = FALSE;
    }
    if (!loadedOk) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSimReloadCompressedInMemory: load failed (%d bytes)",
            len);
        return FALSE;
    }

    basesClearMines(&sim->sim);

    /* Use caller-supplied display name verbatim — no path or suffix
     * to strip in the in-memory case. */
    strncpy(sim->mapName, mapName, MAP_STR_SIZE - 1);
    sim->mapName[MAP_STR_SIZE - 1] = '\0';

    /* Refresh cached compressed map. */
    compressedLen = serverSimGetCompressedMap(sim, tempBuf);
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = malloc(compressedLen);
    if (sim->cachedMapData == NULL) {
        sim->cachedMapDataLen = 0;
        return FALSE;
    }
    memcpy(sim->cachedMapData, tempBuf, compressedLen);
    sim->cachedMapDataLen = compressedLen;

    /* Random-map provenance no longer applies. */
    sim->randomMapEnabled = false;

    snprintf(msg, sizeof(msg), "Map changed to %s", sim->mapName);
    serverSimConsoleMessage(msg);

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimReloadCompressedInMemory: now '%s' (%d compressed bytes)",
        sim->mapName, sim->cachedMapDataLen);

    serverSimApplyMapChange(sim);
    return TRUE;
}

bool serverSimReloadClientMap(ServerSim *sim, ClientSim *cs) {
    BYTE *buf;
    int len;
    bool ok;
    if (sim == NULL || cs == NULL) return FALSE;
    buf = (BYTE *)malloc(65536);
    if (buf == NULL) return FALSE;
    len = serverSimGetCompressedMap(sim, buf);
    if (len <= 0) {
        free(buf);
        return FALSE;
    }
    {
        GameSim *gs = clientSimGetGameSim(cs);
        ok = mapLoadCompressedMap(&gs->mp, &gs->pb, &gs->bs, &gs->ss, buf, len);
    }
    free(buf);
    return ok;
}

bool serverSimReloadRandomMap(ServerSim *sim, const MapGenConfig *cfg) {
    if (!sim || !cfg) {
        mpDiagLog("[srv] reloadRandomMap REJECTED (null arg)");
        return FALSE;
    }
    mpDiagLog("[srv] reloadRandomMap ENTER");
    if (sim->state != serverStateLobby) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadRandomMap rejected: state=%d",
            (int)sim->state);
        mpDiagLog("[srv] reloadRandomMap REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }

    if (sim->previousMapData == NULL && sim->cachedMapData != NULL) {
        sim->previousMapData = (BYTE *)malloc(sim->cachedMapDataLen);
        if (sim->previousMapData) {
            memcpy(sim->previousMapData, sim->cachedMapData,
                   sim->cachedMapDataLen);
            sim->previousMapDataLen = sim->cachedMapDataLen;
            memcpy(sim->previousMapName, sim->mapName,
                   sizeof(sim->previousMapName));
        }
    }

    if (!serverSimApplyRandomMapConfig(sim, cfg)) return FALSE;

    {
        char seedStr[64];
        char msg[128];
        mapGenConfigToSeed(cfg, seedStr, sizeof(seedStr));
        snprintf(msg, sizeof(msg),
                 "Random preview generated, seed: %s", seedStr);
        serverSimConsoleMessage(msg);
        WB_LOG_INFO(WB_LOG_CAT_SERVER,
            "serverSimReloadRandomMap: now '%s' (%d compressed bytes)",
            sim->mapName, sim->cachedMapDataLen);
    }
    serverSimApplyMapChange(sim);
    return TRUE;
}

bool serverSimHasPreviewMap(const ServerSim *sim) {
    return sim != NULL && sim->previousMapData != NULL;
}

const char *serverSimGetPreviousMapName(const ServerSim *sim) {
    if (!sim || !sim->previousMapData) return "";
    return sim->previousMapName;
}

bool serverSimRevertPreview(ServerSim *sim) {
    BYTE tempBuf[131072];
    int len;
    if (!sim || !sim->previousMapData) {
        mpDiagLog("[srv] revertPreview REJECTED (no preview to revert)");
        return FALSE;
    }
    if (sim->state != serverStateLobby) {
        mpDiagLog("[srv] revertPreview REJECTED state=%d (not lobby)",
                  (int)sim->state);
        return FALSE;
    }
    mpDiagLog("[srv] revertPreview ENTER prevMap='%.32s'", sim->previousMapName);

    if (mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb,
                              &sim->sim.bs, &sim->sim.ss,
                              sim->previousMapData,
                              sim->previousMapDataLen) == FALSE) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSimRevertPreview: mapLoadCompressedMap failed");
        return FALSE;
    }
    basesClearMines(&sim->sim);

    /* Reverted to the previous map from its compressed bytes — we no
     * longer have its .map file to hash, so clear the cached md5. */
    sim->mapMd5Valid = FALSE;

    memcpy(sim->mapName, sim->previousMapName, sizeof(sim->mapName));
    len = serverSimGetCompressedMap(sim, tempBuf);
    if (sim->cachedMapData) free(sim->cachedMapData);
    sim->cachedMapData = (BYTE *)malloc(len);
    if (sim->cachedMapData) {
        memcpy(sim->cachedMapData, tempBuf, len);
        sim->cachedMapDataLen = len;
    } else {
        sim->cachedMapDataLen = 0;
    }

    free(sim->previousMapData);
    sim->previousMapData = NULL;
    sim->previousMapDataLen = 0;
    sim->previousMapName[0] = '\0';

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimRevertPreview: rolled back to '%s'", sim->mapName);
    serverSimApplyMapChange(sim);
    return TRUE;
}

void serverSimCommitPreview(ServerSim *sim) {
    if (!sim || !sim->previousMapData) return;
    free(sim->previousMapData);
    sim->previousMapData = NULL;
    sim->previousMapDataLen = 0;
    sim->previousMapName[0] = '\0';
    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "serverSimCommitPreview: committed '%s'", sim->mapName);
}

/* ────────────────────────────────────────────────────────────────
 * Map directory enumeration / search
 * ──────────────────────────────────────────────────────────────── */

static bool relPathIsSafe(const char *p) {
    if (!p) return true;
    if (p[0] == '/' || p[0] == '\\') return false;
    if (p[0] != '\0' && (p[1] == ':' || (p[2] == ':' && p[3] != '\0')))
        return false; /* "C:..." Windows drive */
    for (const char *s = p; *s;) {
        if (s[0] == '.' && s[1] == '.' &&
            (s[2] == '\0' || s[2] == '/' || s[2] == '\\')) {
            return false;
        }
        while (*s && *s != '/' && *s != '\\') s++;
        while (*s == '/' || *s == '\\') s++;
    }
    return true;
}

int serverSimEnumerateMapDir(ServerSim *sim, const char *relPath,
                              ServerMapEntry *entries, int maxEntries) {
    if (!entries || maxEntries <= 0) return -1;
    if (!relPathIsSafe(relPath)) return -1;

    const char *root = serverSimGetMapDirRoot(sim);
    char fullPath[FILENAME_MAX];
    if (!relPath || relPath[0] == '\0') {
        SDL_strlcpy(fullPath, root, sizeof(fullPath));
    } else {
        SDL_snprintf(fullPath, sizeof(fullPath), "%s/%s", root, relPath);
    }

    int count = 0;
    int globCount = 0;
    char **list = SDL_GlobDirectory(fullPath, NULL, 0, &globCount);
    if (!list) return 0;

    for (int i = 0; i < globCount && count < maxEntries; i++) {
        const char *name = list[i];
        if (!name || name[0] == '.') continue;

        char child[FILENAME_MAX];
        SDL_snprintf(child, sizeof(child), "%s/%s", fullPath, name);

        SDL_PathInfo info;
        if (!SDL_GetPathInfo(child, &info)) continue;
        bool isDir = (info.type == SDL_PATHTYPE_DIRECTORY);

        if (!isDir) {
            size_t nlen = SDL_strlen(name);
            if (nlen <= 4 ||
                SDL_strcasecmp(name + nlen - 4, ".map") != 0) {
                continue;
            }
        }

        ServerMapEntry *e = &entries[count++];
        SDL_strlcpy(e->name, name, sizeof(e->name));
        e->isFolder = isDir;
        e->modTime  = (int64_t)info.modify_time;
        e->size     = isDir ? 0 : (int64_t)info.size;
    }
    SDL_free(list);

    /* Folders first; alphabetical within each group. */
    for (int i = 1; i < count; i++) {
        ServerMapEntry cur = entries[i];
        int j = i - 1;
        while (j >= 0) {
            const ServerMapEntry *a = &entries[j];
            bool aFirst;
            if (a->isFolder != cur.isFolder) aFirst = a->isFolder;
            else aFirst = SDL_strcasecmp(a->name, cur.name) <= 0;
            if (aFirst) break;
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = cur;
    }

    return count;
}

static void searchDirRecursive(const char *fullRoot,
                                const char *subRel,
                                const char *queryLower,
                                size_t queryLen,
                                ServerMapEntry *entries,
                                int maxEntries,
                                int *count,
                                int depth) {
    const int kMaxDepth = 8;
    if (*count >= maxEntries) return;
    if (depth > kMaxDepth) return;

    char dirPath[FILENAME_MAX];
    if (subRel[0] == '\0') {
        SDL_strlcpy(dirPath, fullRoot, sizeof(dirPath));
    } else {
        SDL_snprintf(dirPath, sizeof(dirPath), "%s/%s",
                     fullRoot, subRel);
    }

    int globCount = 0;
    char **list = SDL_GlobDirectory(dirPath, NULL, 0, &globCount);
    if (!list) return;

    for (int i = 0; i < globCount && *count < maxEntries; i++) {
        const char *name = list[i];
        if (!name || name[0] == '.') continue;

        char childPath[FILENAME_MAX];
        SDL_snprintf(childPath, sizeof(childPath), "%s/%s",
                     dirPath, name);

        SDL_PathInfo info;
        if (!SDL_GetPathInfo(childPath, &info)) continue;
        bool isDir = (info.type == SDL_PATHTYPE_DIRECTORY);

        char rel[256];
        if (subRel[0] == '\0') {
            SDL_strlcpy(rel, name, sizeof(rel));
        } else {
            SDL_snprintf(rel, sizeof(rel), "%s/%s", subRel, name);
        }

        if (isDir) {
            searchDirRecursive(fullRoot, rel, queryLower, queryLen,
                               entries, maxEntries, count, depth + 1);
            continue;
        }

        size_t nlen = SDL_strlen(name);
        if (nlen <= 4 ||
            SDL_strcasecmp(name + nlen - 4, ".map") != 0) continue;

        bool match = false;
        for (size_t k = 0; k + queryLen <= nlen; k++) {
            size_t m;
            for (m = 0; m < queryLen; m++) {
                char hc = name[k + m];
                if (hc >= 'A' && hc <= 'Z') hc = (char)(hc + 32);
                if (hc != queryLower[m]) break;
            }
            if (m == queryLen) { match = true; break; }
        }
        if (!match) continue;

        ServerMapEntry *e = &entries[(*count)++];
        SDL_strlcpy(e->name, rel, sizeof(e->name));
        e->isFolder = false;
        e->modTime  = (int64_t)info.modify_time;
        e->size     = (int64_t)info.size;
    }
    SDL_free(list);
}

int serverSimSearchMapDir(ServerSim *sim, const char *relPath,
                           const char *query,
                           ServerMapEntry *entries, int maxEntries) {
    if (!entries || maxEntries <= 0) return -1;
    if (!query || query[0] == '\0') return 0;
    if (!relPathIsSafe(relPath)) return -1;

    const char *root = serverSimGetMapDirRoot(sim);
    char fullRoot[FILENAME_MAX];
    if (!relPath || relPath[0] == '\0') {
        SDL_strlcpy(fullRoot, root, sizeof(fullRoot));
    } else {
        SDL_snprintf(fullRoot, sizeof(fullRoot), "%s/%s", root, relPath);
    }

    char queryLower[128];
    size_t qlen = SDL_strlen(query);
    if (qlen >= sizeof(queryLower)) qlen = sizeof(queryLower) - 1;
    for (size_t i = 0; i < qlen; i++) {
        char c = query[i];
        if (c >= 'A' && c <= 'Z') c = (char)(c + 32);
        queryLower[i] = c;
    }
    queryLower[qlen] = '\0';

    int count = 0;
    searchDirRecursive(fullRoot, "", queryLower, qlen,
                       entries, maxEntries, &count, 0);

    for (int i = 1; i < count; i++) {
        ServerMapEntry cur = entries[i];
        int j = i - 1;
        while (j >= 0 &&
               SDL_strcasecmp(entries[j].name, cur.name) > 0) {
            entries[j + 1] = entries[j];
            j--;
        }
        entries[j + 1] = cur;
    }
    return count;
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

bool serverSimGetAutoLockOnGameStart(const ServerSim *sim) {
    return sim ? sim->autoLockOnGameStart : false;
}

void serverSimSetAutoLockOnGameStart(ServerSim *sim, bool v) {
    if (sim) sim->autoLockOnGameStart = v;
}

bool serverSimGetOpenHost(const ServerSim *sim) {
    return sim ? sim->openHost : false;
}

void serverSimSetOpenHost(ServerSim *sim, bool v) {
    if (sim == NULL) return;
    sim->openHost = v;
    serverSimPublishLobbySettings(sim);
    lobbyAutoUnreadyOnChange(sim);
}

bool serverSimGetFirstJoinerBecomesHost(const ServerSim *sim) {
    return sim ? sim->firstJoinerBecomesHost : false;
}

void serverSimSetFirstJoinerBecomesHost(ServerSim *sim, bool v) {
    if (sim) sim->firstJoinerBecomesHost = v;
}

uint16_t serverSimGetServerLocks(const ServerSim *sim) {
    return sim ? sim->serverLocks : 0;
}

uint16_t serverSimGetSettingLockBit(uint8_t lstSettingType) {
    switch (lstSettingType) {
        case LST_GAME_TYPE:         return LOBBY_LOCK_GAME_TYPE;
        case LST_HIDDEN_MINES:      return LOBBY_LOCK_MINES;
        case LST_AI_POLICY:         return LOBBY_LOCK_AI_POLICY;
        case LST_TIME_LIMIT:        return LOBBY_LOCK_TIME_LIMIT;
        case LST_TIME_MINUTES:      return LOBBY_LOCK_TIME_LIMIT;
        case LST_AUTO_LOCK_ON_GAME: return LOBBY_LOCK_AUTO_LOCK_ON_GAME;
        case LST_RANKED:            return LOBBY_LOCK_RANKED;
        default:                    return 0xFFFFu;  /* unknown setting */
    }
}

bool serverSimIsSettingLocked(const ServerSim *sim, uint8_t lstSettingType) {
    if (sim == NULL) return false;
    uint16_t bit = serverSimGetSettingLockBit(lstSettingType);
    if (bit == 0u || bit == 0xFFFFu) return false;
    return (sim->serverLocks & bit) != 0u;
}

void serverSimSetAiPolicy(ServerSim *sim, uint8_t v) {
    if (sim) sim->aiPolicy = v;
}

bool serverSimGetTimeLimit(const ServerSim *sim) {
    return sim ? sim->timeLimit : false;
}

void serverSimSetTimeLimit(ServerSim *sim, bool v) {
    if (sim) sim->timeLimit = v;
}

uint16_t serverSimGetTimeMinutes(const ServerSim *sim) {
    return sim ? sim->timeMinutes : 0;
}

void serverSimSetTimeMinutes(ServerSim *sim, uint16_t v) {
    if (sim) sim->timeMinutes = v;
}

TeamMetadata *serverSimGetTeamMetaMut(ServerSim *sim, BYTE teamId) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return NULL;
    return &sim->teams[teamId];
}

const LobbyBotConfig *serverSimGetBotConfig(const ServerSim *sim, BYTE slot) {
    if (!sim || slot >= MAX_TANKS) return NULL;
    return &sim->botConfigs[slot];
}

LobbyBotConfig *serverSimGetBotConfigMut(ServerSim *sim, BYTE slot) {
    if (!sim || slot >= MAX_TANKS) return NULL;
    return &sim->botConfigs[slot];
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

void serverSimSetGameLength(ServerSim *sim, int32_t ticks) {
    if (sim) sim->gameLength = ticks;
}

gameType serverSimGetGameType(const ServerSim *sim) {
    return sim ? sim->sim.game : gameOpen;
}

void serverSimSetGameType(ServerSim *sim, gameType gt) {
    if (sim) sim->sim.game = gt;
}

void serverSimSetHiddenMines(ServerSim *sim, bool hiddenMines) {
    if (sim) sim->sim.hiddenMines = hiddenMines;
}

void serverSimSetState(ServerSim *sim, ServerState s) {
    if (sim) sim->state = s;
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

/* Side-effects every server-side map change owes its audience:
 * refresh the per-client compressed-map blob on the wire helper,
 * publish the map-change control event so in-process subscribers
 * (SP host's ClientSim, bots, replay log writers) react, re-publish
 * settings so the mapName / pillCount / baseCount / startCount
 * fields in CTRL_LOBBY_SETTINGS reflect the new map, and clear
 * humans' ready state — which aborts any in-flight countdown via
 * lobbyAutoUnreadyOnChange. Called from every map-mutator at the
 * end of its success path. */
static void serverSimApplyMapChange(ServerSim *sim) {
    mpDiagLog("[srv] applyMapChange map='%.32s' cachedLen=%d random=%d",
              sim->mapName, sim->cachedMapDataLen,
              (int)sim->randomMapEnabled);
    transportUdpServerOnLobbyMapChange(sim);
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_LOBBY_MAP_CHANGE;
        serverSimPublishControl(sim, &evt);
    }
    serverSimPublishLobbySettings(sim);
    lobbyAutoUnreadyOnChange(sim);
    serverSimWbnLobbyUpdate(sim, FALSE);
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

bool serverSimReadMapFile(ServerSim *sim, const char *relPath,
                           uint8_t **outBytes, size_t *outLen) {
    if (!outBytes || !outLen) return false;
    *outBytes = NULL;
    *outLen   = 0;
    if (!relPath || !*relPath) return false;
    if (!relPathIsSafe(relPath)) return false;

    char fullPath[FILENAME_MAX];
    SDL_snprintf(fullPath, sizeof(fullPath), "%s/%s",
                 serverSimGetMapDirRoot(sim), relPath);
    SDL_PathInfo info;
    if (!SDL_GetPathInfo(fullPath, &info)) return false;
    if (info.type != SDL_PATHTYPE_FILE) return false;

    FILE *fp = fopen(fullPath, "rb");
    if (!fp) return false;
    if (fseek(fp, 0, SEEK_END) != 0) { fclose(fp); return false; }
    long sz = ftell(fp);
    if (sz <= 0 || (size_t)sz > LOBBY_MAP_UPLOAD_MAX_BYTES) { fclose(fp); return false; }
    if (fseek(fp, 0, SEEK_SET) != 0) { fclose(fp); return false; }
    uint8_t *buf = (uint8_t *)malloc((size_t)sz);
    if (!buf) { fclose(fp); return false; }
    size_t got = fread(buf, 1, (size_t)sz, fp);
    fclose(fp);
    if (got != (size_t)sz) { free(buf); return false; }
    *outBytes = buf;
    *outLen   = (size_t)sz;
    return true;
}

void serverSimSetServerLocks(ServerSim *sim, uint16_t locks) {
    if (sim) sim->serverLocks = locks;
}

bool serverSimGetRanked(const ServerSim *sim) {
    return sim ? sim->ranked : false;
}

void serverSimSetRanked(ServerSim *sim, bool v) {
    if (sim) sim->ranked = v;
}

void serverSimSetAllowNewPlayers(ServerSim *sim, bool v) {
    if (sim == NULL) return;
    sim->allowNewPlayers = v;
    serverSimPublishLobbySettings(sim);
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

uint8_t serverSimGetAiPolicy(const ServerSim *sim) {
    return sim ? sim->aiPolicy : 0;
}

const TeamMetadata *serverSimGetTeamMeta(const ServerSim *sim, BYTE teamId) {
    if (!sim || teamId == 0 || teamId >= MAX_TANKS) return NULL;
    return &sim->teams[teamId];
}

bool serverSimIsAcceptingJoins(const ServerSim *sim) {
    return sim && sim->allowNewPlayers;
}

BYTE serverSimGetMaxPlayers(const ServerSim *sim) {
    if (sim == NULL) return MAX_TANKS;
    return (sim->maxPlayers > 0) ? sim->maxPlayers : (BYTE)MAX_TANKS;
}

