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
 *Name:          Server Simulation Accessors
 *Filename:      server_sim_accessors.c
 *Author:        John Morrison
 *Purpose:
 *  The trivial field-access getters and setters for
 *  ServerSim, split out of server_sim.c. Each one reads
 *  or writes struct fields on behalf of a caller that
 *  holds only the opaque handle; none of them owns state
 *  or drives the sim.
 *********************************************************/

#include <string.h>
#include <SDL3/SDL.h>

#include "wire_limits.h"   /* LobbySettingType, LOBBY_LOCK_* for serverSimGetSettingLockBit */
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"   /* lobbyAutoUnreadyOnChange — the setters that clear ready state */

/*********************************************************
 * Server option setters.
 *********************************************************/

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

void serverSimEnterLobby(ServerSim *sim) {
    sim->state = serverStateLobby;
}

void serverSimSetAutoCloseOnEmpty(ServerSim *sim, bool enabled) {
    sim->autoCloseOnEmpty = enabled;
}

void serverSimSetMapRotate(ServerSim *sim, bool enabled) {
    sim->mapRotateEnabled = enabled;
}

bool serverSimIsMapRotateEnabled(const ServerSim *sim) {
    return sim != NULL && sim->mapRotateEnabled;
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

void serverSimSetSnapshotHook(ServerSim *sim, void (*cb)(ServerSim *sim),
                              int32_t intervalTicks) {
    if (sim == NULL) {
        return;
    }
    if (cb == NULL || intervalTicks <= 0) {
        sim->snapshotCb = NULL;
        sim->snapshotInterval = 0;
    } else {
        sim->snapshotCb = cb;
        sim->snapshotInterval = intervalTicks;
    }
    sim->snapshotTicks = 0;
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

const char *serverSimGetMapMd5Hex(const ServerSim *sim) {
    return (sim != NULL) ? sim->mapMd5Hex : "";
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

void serverSimSetTutorialStartIdx(ServerSim *sim, BYTE idx) {
    sim->sim.tutorialStartIdx = idx;
}

bool serverSimTakeTutorialRespawn1(ServerSim *sim) {
    bool v = sim->sim.tutorialRespawn1Pending;
    sim->sim.tutorialRespawn1Pending = FALSE;
    return v;
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
    out->angle   = tankGetAngle(t);
    out->on_boat = tankIsOnBoat(t);
    out->alive   = (tankGetDeathWait(t) == 0);
    return true;
}

bool serverSimGetTankInfo(ServerSim *sim, BYTE i, TankInfo *out) {
    tank *t;
    if (sim == NULL || out == NULL || i >= MAX_TANKS) return false;
    if (!serverSimIsPlayerConnected(sim, i)) return false;

    memset(out, 0, sizeof(*out));
    /* isServer=TRUE: read the server-side player table directly. */
    playersGetPlayerName(&sim->sim.plyrs, i, out->name, sizeof(out->name),
                         TRUE);
    out->name[sizeof(out->name) - 1] = '\0';

    t = &sim->sim.tanks[i];
    if (*t == NULL) {
        /* Connected but no live tank (countdown / death-wait). */
        out->has_tank = false;
        out->alive    = false;
        return true;
    }
    out->has_tank = true;
    tankGetWorld(t, &out->world_x, &out->world_y);
    out->dir     = tankGetDir(t);
    out->on_boat = tankIsOnBoat(t);
    out->alive   = (tankGetDeathWait(t) == 0);
    tankGetKillsDeaths(t, &out->kills, &out->deaths);
    return true;
}

bool serverSimGetDeathCauses(const ServerSim *sim, BYTE slot,
                             uint32_t out[DEATH_CAUSE_NUM]) {
    int c;
    if (out == NULL) {
        return false;
    }
    for (c = 0; c < DEATH_CAUSE_NUM; c++) {
        out[c] = 0;
    }
    if (sim == NULL || slot >= MAX_TANKS) {
        return false;
    }
    for (c = 0; c < DEATH_CAUSE_NUM; c++) {
        out[c] = sim->sim.deathCauseCount[slot][c];
    }
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

/*********************************************************
 * Lobby setting, team and capacity accessors.
 *********************************************************/

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

BYTE serverSimGetHostSlot(const ServerSim *sim) {
    return sim ? sim->hostSlot : 0;
}

void serverSimSetHostSlot(ServerSim *sim, BYTE slot) {
    if (sim == NULL) return;
    sim->hostSlot = slot;
    serverSimPublishLobbySettings(sim);
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

BYTE serverSimGetMaxBots(const ServerSim *sim) {
    if (sim == NULL) return 0;
    return sim->maxBots;
}

BYTE serverSimGetMaxSpectators(const ServerSim *sim) {
    if (sim == NULL) return 0;
    return sim->maxSpectators;   /* 0 = disabled; no MAX_TANKS fallback */
}

void serverSimSetMaxSpectators(ServerSim *sim, BYTE n) {
    if (sim == NULL) return;
    sim->maxSpectators = n;
}

uint32_t serverSimGetSpecDelayTicks(const ServerSim *sim) {
    if (sim == NULL) return 0;
    return sim->specDelayTicks;
}

void serverSimSetSpecDelayTicks(ServerSim *sim, uint32_t ticks) {
    if (sim == NULL) return;
    sim->specDelayTicks = ticks;
}

BYTE serverSimGetLobbyBotCount(const ServerSim *sim) {
    if (sim == NULL) return 0;
    BYTE count = 0;
    for (BYTE i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].isBot) {
            count++;
        }
    }
    return count;
}
