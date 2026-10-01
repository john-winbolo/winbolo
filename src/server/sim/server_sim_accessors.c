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

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "wire_limits.h"   /* LobbySettingType, LOBBY_LOCK_* for serverSimGetSettingLockBit */
#include "server_sim_internal.h"
#include "lobby_bot_pools.h"
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

bool serverSimGetRosterSlot(ServerSim *sim, BYTE i, ServerSimRosterSlot *out) {
    tank *t;
    if (sim == NULL || out == NULL || i >= MAX_TANKS) return false;
    if (!serverSimIsPlayerConnected(sim, i)) return false;

    memset(out, 0, sizeof(*out));
    out->connected = true;
    out->is_bot    = sim->lobbyPlayers[i].isBot;
    out->team      = sim->lobbyPlayers[i].teamNumber;
    /* isServer=TRUE: read the server-side player table directly. */
    playersGetPlayerName(&sim->sim.plyrs, i, out->name, sizeof(out->name),
                         TRUE);
    out->name[sizeof(out->name) - 1] = '\0';
    out->ready = sim->lobbyPlayers[i].ready;
    out->fielded = sim->lobbyPlayers[i].fielded;
    t = &sim->sim.tanks[i];
    out->alive = (*t != NULL && tankGetDeathWait(t) == 0);
    {
        BYTE team = sim->lobbyPlayers[i].teamNumber;
        if (team > 0 && team < MAX_TANKS && sim->teams[team].in_use &&
            (int)sim->teams[team].namingPool < lobbyBotPoolCount()) {
            SDL_strlcpy(out->team_pool,
                        lobbyBotPoolLabel(sim->teams[team].namingPool),
                        sizeof(out->team_pool));
        }
    }
    return true;
}

bool serverSimIsAllied(ServerSim *sim, BYTE a, BYTE b) {
    if (sim == NULL || a >= MAX_TANKS || b >= MAX_TANKS) return false;
    if (!sim->playerConnected[a] || !sim->playerConnected[b]) return false;
    if (a == b) return true;
    return playersIsAllie(&sim->sim.plyrs, a, b) == TRUE;
}

BYTE serverSimGetNumFielded(ServerSim *sim) {
    BYTE count;
    BYTE num = 0;
    if (sim == NULL) return 0;
    /* Seats playing the round, which is fewer than the roster whenever a
       seat is held for a bot that has not been fielded yet. */
    for (count = 0; count < MAX_TANKS; count++) {
        if (sim->playerConnected[count] && sim->lobbyPlayers[count].fielded) {
            num++;
        }
    }
    return num;
}

bool serverSimIsSeatFielded(const ServerSim *sim, BYTE playerNum) {
    if (sim == NULL || playerNum >= MAX_TANKS) return false;
    return sim->playerConnected[playerNum] &&
           sim->lobbyPlayers[playerNum].fielded;
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
    if (!pillsIsActive(&sim->sim.pb, i)) return false;
    pillsGetPill(&sim->sim.pb, &p, i);
    if (x)      *x      = p.x;
    if (y)      *y      = p.y;
    if (owner)  *owner  = p.owner;
    if (armour) *armour = p.armour;
    if (inTank) *inTank = p.inTank;
    return true;
}

/* Reader for binaries that own a ServerSim directly. Only speed: pillsGetPill
   copies six fields and reload and coolDown are not among them, so reading
   them here read whatever was on the stack. */
bool serverSimGetPillSpeed(ServerSim *sim, BYTE i, BYTE *speed) {
    pillbox p;
    BYTE n = pillsGetNumPills(&sim->sim.pb);
    if (i == 0 || i > n) return false;
    if (!pillsIsActive(&sim->sim.pb, i)) return false;
    pillsGetPill(&sim->sim.pb, &p, i);
    if (speed) *speed = p.speed;
    return true;
}

bool serverSimGetPillInfo(ServerSim *sim, BYTE i, ServerSimPillInfo *out) {
    pillbox p;
    BYTE n;
    if (sim == NULL || out == NULL) return false;
    n = pillsGetNumPills(&sim->sim.pb);
    if (i == 0 || i > n) return false;
    memset(&p, 0, sizeof(p));
    pillsGetPill(&sim->sim.pb, &p, i);

    memset(out, 0, sizeof(*out));
    out->x       = p.x;
    out->y       = p.y;
    out->owner   = p.owner;
    out->armour  = p.armour;
    out->speed   = p.speed;
    out->in_tank = p.inTank;
    out->active  = pillsIsActive(&sim->sim.pb, i);
    return true;
}

bool serverSimGetBase(ServerSim *sim, BYTE i,
                      BYTE *x, BYTE *y, BYTE *owner) {
    base b;
    BYTE n = basesGetNumBases(&sim->sim.bs);
    if (i == 0 || i > n) return false;
    if (!basesIsActive(&sim->sim.bs, i)) return false;
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
    if (!basesIsActive(&sim->sim.bs, i)) return false;
    basesGetStats(&sim->sim.bs, i, shells, mines, armour);
    return true;
}

bool serverSimGetBaseInfo(ServerSim *sim, BYTE i, ServerSimBaseInfo *out) {
    base b;
    BYTE n;
    if (sim == NULL || out == NULL) return false;
    n = basesGetNumBases(&sim->sim.bs);
    if (i == 0 || i > n) return false;
    /* basesGetBase fills the six fields read below and leaves the rest of
       the struct alone, so start from a cleared one. */
    memset(&b, 0, sizeof(b));
    basesGetBase(&sim->sim.bs, &b, i);

    memset(out, 0, sizeof(*out));
    out->x      = b.x;
    out->y      = b.y;
    out->owner  = b.owner;
    out->armour = b.armour;
    out->shells = b.shells;
    out->mines  = b.mines;
    out->active = basesIsActive(&sim->sim.bs, i);
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

bool serverSimGetStartInfo(ServerSim *sim, BYTE i, ServerSimStartInfo *out) {
    start s;
    BYTE n;
    if (sim == NULL || out == NULL) return false;
    n = startsGetNumStarts(&sim->sim.ss);
    if (i == 0 || i > n) return false;
    /* startsGetStartStruct fills the fields read below and leaves the rest of
       the struct alone, so start from a cleared one. */
    memset(&s, 0, sizeof(s));
    startsGetStartStruct(&sim->sim.ss, &s, i);

    memset(out, 0, sizeof(*out));
    out->x      = s.x;
    out->y      = s.y;
    out->dir    = startsConvertDir((BYTE)((s.dir < 16) ? s.dir : 0));
    out->active = startsIsActive(&sim->sim.ss, i);
    return true;
}

bool serverSimGetMapTerrainBuffer(const ServerSim *sim, BYTE *out, size_t cap) {
    map *mp;
    int x;
    int y;
    if (sim == NULL || out == NULL) return false;
    if (cap < (size_t)SERVER_SIM_TERRAIN_BYTES) return false;

    /* One byte per square through mapGetPos, so the buffer and
       serverSimGetMapTerrain agree on the border squares mapGetPos reports
       as deep sea. Row-major: the row for y is 256 contiguous bytes. */
    mp = &((ServerSim *)sim)->sim.mp;
    for (y = 0; y < MAP_ARRAY_SIZE; y++) {
        for (x = 0; x < MAP_ARRAY_SIZE; x++) {
            out[(y * MAP_ARRAY_SIZE) + x] = mapGetPos(mp, (BYTE)x, (BYTE)y);
        }
    }
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
    /* Identity, not tank state: filled whether or not a tank exists. */
    out->is_bot = sim->lobbyPlayers[i].isBot;

    t = &sim->sim.tanks[i];
    if (*t == NULL) {
        /* Connected but no live tank (countdown / death-wait). */
        out->has_tank = false;
        out->alive    = false;
        return true;
    }
    out->has_tank = true;
    tankGetWorld(t, &out->world_x, &out->world_y);
    out->map_x   = tankGetMX(t);
    out->map_y   = tankGetMY(t);
    out->dir     = tankGetDir(t);
    out->dir256  = tankGet256Dir(t);
    out->on_boat = tankIsOnBoat(t);
    out->alive   = (tankGetDeathWait(t) == 0);
    tankGetStats(t, &out->shells, &out->mines, &out->armour, &out->trees);
    out->pills   = tankGetNumCarriedPills(t);
    tankGetModifiers(*t, &out->mods);
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

/* BuilderJob is handed to the engine's request path as-is, so each member
   has to keep the value of the request code it stands for. */
BOLO_STATIC_ASSERT((int)builderJobTrees    == LGM_TREE_REQUEST,
                   builder_job_trees_matches_request);
BOLO_STATIC_ASSERT((int)builderJobRoad     == LGM_ROAD_REQUEST,
                   builder_job_road_matches_request);
BOLO_STATIC_ASSERT((int)builderJobBuilding == LGM_BUILDING_REQUEST,
                   builder_job_building_matches_request);
BOLO_STATIC_ASSERT((int)builderJobPill     == LGM_PILL_REQUEST,
                   builder_job_pill_matches_request);
BOLO_STATIC_ASSERT((int)builderJobMine     == LGM_MINE_REQUEST,
                   builder_job_mine_matches_request);
BOLO_STATIC_ASSERT((int)builderJobBoat     == LGM_BOAT_REQUEST,
                   builder_job_boat_matches_request);
BOLO_STATIC_ASSERT((int)builderJobNone     == LGM_IDLE,
                   builder_job_none_matches_idle);

bool serverSimGetBuilderInfo(ServerSim *sim, BYTE i, ServerSimBuilderInfo *out) {
    lgm *l;
    tank *t;
    if (sim == NULL || out == NULL || i >= MAX_TANKS) return false;
    if (!serverSimIsPlayerConnected(sim, i)) return false;
    l = &sim->sim.lgmen[i];
    if (*l == NULL) return false;

    memset(out, 0, sizeof(*out));
    t = &sim->sim.tanks[i];
    if ((*l)->inTank) {
        out->state = builderStateInTank;
    } else if ((*l)->isDead) {
        /* isDead covers the whole span from being killed to landing again:
           the man is on his way back down while his tank is still standing,
           and has nowhere to land once it is not. Same split obs_builder.c
           reports to a brain. */
        out->state = (*t != NULL && !tankIsDestroyed(t))
                         ? builderStateParachuting
                         : builderStateDead;
    } else if ((*l)->state == LGM_STATE_GOING) {
        out->state = builderStateGoing;
    } else {
        /* Out of the tank and not walking to a job: walking back to it. */
        out->state = builderStateReturning;
    }
    out->world_x = lgmGetWX(l);
    out->world_y = lgmGetWY(l);
    out->map_x   = lgmGetMX(l);
    out->map_y   = lgmGetMY(l);
    /* The asserts above pin every member to the request code it stands
       for, so the action carries straight across; anything outside that
       range is no job at all. */
    out->job     = ((*l)->action <= LGM_IDLE)
                       ? (BuilderJob)(*l)->action
                       : builderJobNone;
    out->trees   = (*l)->numTrees;
    out->mines   = (*l)->numMines;
    return true;
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

uint32_t serverSimGetServerLocks(const ServerSim *sim) {
    return sim ? sim->serverLocks : 0;
}

uint32_t serverSimGetSettingLockBit(uint8_t lstSettingType) {
    switch (lstSettingType) {
        case LST_GAME_TYPE:         return LOBBY_LOCK_GAME_TYPE;
        case LST_HIDDEN_MINES:      return LOBBY_LOCK_MINES;
        case LST_AI_POLICY:         return LOBBY_LOCK_AI_POLICY;
        case LST_TIME_LIMIT:        return LOBBY_LOCK_TIME_LIMIT;
        case LST_TIME_MINUTES:      return LOBBY_LOCK_TIME_LIMIT;
        case LST_AUTO_LOCK_ON_GAME: return LOBBY_LOCK_AUTO_LOCK_ON_GAME;
        case LST_RANKED:            return LOBBY_LOCK_RANKED;
        case LST_PILL_VIEW:         return LOBBY_LOCK_PILL_VIEW;
        case LST_BASE_VIEW:         return LOBBY_LOCK_BASE_VIEW;
        case LST_ALLY_VIEW:         return LOBBY_LOCK_ALLY_VIEW;
        case LST_CLASSIC_MODE:      return LOBBY_LOCK_CLASSIC_MODE;
        case LST_ALLIES_IN_TREES:   return LOBBY_LOCK_ALLIES_IN_TREES;
        case LST_OVERVIEW_WINDOW:   return LOBBY_LOCK_OVERVIEW_WINDOW;
        case LST_LINE_OF_SIGHT:     return LOBBY_LOCK_LINE_OF_SIGHT;
        case LST_SMART_PINGS_OFF:   return LOBBY_LOCK_SMART_PINGS;
        case LST_MODS_OFF:          return LOBBY_LOCK_MODS;
        case LST_POSITIONAL_SOUND:  return LOBBY_LOCK_POSITIONAL_SOUND;
        default:                    return 0xFFFFFFFFu;  /* unknown setting */
    }
}

bool serverSimIsSettingLocked(const ServerSim *sim, uint8_t lstSettingType) {
    if (sim == NULL) return false;
    uint32_t bit = serverSimGetSettingLockBit(lstSettingType);
    if (bit == 0u || bit == 0xFFFFFFFFu) return false;
    return (sim->serverLocks & bit) != 0u;
}

uint32_t serverSimAddImpliedLocks(uint32_t locks) {
    /* Turning classic mode on writes the three view policies, allies in
     * trees, the overview window, line of sight and positional sound
     * (serverSimSetClassicMode), so leaving the checkbox editable while
     * any of those seven is locked would let a host change a locked value
     * with one tick — and the value does not come back, because turning
     * classic mode off leaves all seven where classic mode put them.
     * Locking any of the seven locks classic mode too.
     *
     * Deliberately decided from the mask alone rather than from the
     * current values: the mask is fixed at startup, so the host sees a
     * checkbox that is either always available or always locked, rather
     * than one that appears and disappears as other settings move. */
    if (locks & (LOBBY_LOCK_PILL_VIEW | LOBBY_LOCK_BASE_VIEW |
                 LOBBY_LOCK_ALLY_VIEW | LOBBY_LOCK_ALLIES_IN_TREES |
                 LOBBY_LOCK_OVERVIEW_WINDOW | LOBBY_LOCK_LINE_OF_SIGHT |
                 LOBBY_LOCK_POSITIONAL_SOUND)) {
        locks |= LOBBY_LOCK_CLASSIC_MODE;
    }
    return locks;
}

void serverSimSetAiPolicy(ServerSim *sim, uint8_t v) {
    if (sim) sim->aiPolicy = v;
}

void serverSimSetViewPolicy(ServerSim *sim, ViewCategory cat,
                            ViewPolicy policy, uint16_t decaySecs) {
    if (sim == NULL) return;
    if ((int)cat < 0 || (int)cat >= VIEW_CATEGORY_COUNT) return;
    if ((int)policy < viewPolicyAlways || (int)policy > viewPolicyOff) return;
    if (decaySecs < VIEW_DECAY_MIN_SECS) decaySecs = VIEW_DECAY_MIN_SECS;
    if (decaySecs > VIEW_DECAY_MAX_SECS) decaySecs = VIEW_DECAY_MAX_SECS;
    sim->viewPolicy[cat]    = policy;
    sim->viewDecaySecs[cat] = decaySecs;
}

/* The NULL-sim and out-of-range answers here, and in the two window /
 * sight getters below, are meaning B in view_policy.h: what a reader
 * assumes of a sender that named no policy, not what a sim starts on.
 * They must not become the VIEW_POLICY_STOCK_* set. */
ViewPolicy serverSimGetViewPolicy(const ServerSim *sim, ViewCategory cat) {
    if (sim == NULL) return viewPolicyAlways;
    if ((int)cat < 0 || (int)cat >= VIEW_CATEGORY_COUNT) return viewPolicyAlways;
    return sim->viewPolicy[cat];
}

void serverSimSetClassicMode(ServerSim *sim, bool on) {
    if (sim == NULL) return;
    sim->classicMode = on;
    if (on) {
        /* Write the three classic values straight through the view-policy
         * setter, so the command-line switch and the lobby setting both
         * get the same result. Each category keeps its own decay seconds
         * so the host's value survives a trip through classic mode.
         *
         * These are classic mode's own set, spelled out on purpose. They
         * match the VIEW_POLICY_STOCK_* set today, but they are a
         * different statement — moving what a stock server runs must not
         * silently redefine what classic mode means. */
        serverSimSetViewPolicy(sim, viewCategoryPill, viewPolicyKey,
                               sim->viewDecaySecs[viewCategoryPill]);
        serverSimSetViewPolicy(sim, viewCategoryBase, viewPolicyOff,
                               sim->viewDecaySecs[viewCategoryBase]);
        serverSimSetViewPolicy(sim, viewCategoryAlly, viewPolicyOff,
                               sim->viewDecaySecs[viewCategoryAlly]);
        /* Classic mode hides allies in trees, so it owns this value too. */
        serverSimSetAlliesInTrees(sim, false);
        /* Classic mode plays every sound centred, so it owns this too. */
        serverSimSetPositionalSound(sim, false);
        /* Classic mode offers no overview at all - no pop-out map and no
         * full screen map - with nothing blocking sight in the framed view
         * it leaves the player. */
        serverSimSetOverviewWindow(sim, (uint8_t)overviewWindowNone);
        serverSimSetLineOfSight(sim, (uint8_t)lineOfSightOff);
    }
}

bool serverSimGetClassicMode(const ServerSim *sim) {
    return sim ? sim->classicMode : false;
}

void serverSimSetAlliesInTrees(ServerSim *sim, bool on) {
    if (sim == NULL) return;
    sim->alliesInTrees = on;
}

bool serverSimGetAlliesInTrees(const ServerSim *sim) {
    return sim ? sim->alliesInTrees : false;
}

void serverSimSetPositionalSound(ServerSim *sim, bool on) {
    if (sim == NULL) return;
    sim->positionalSound = on;
}

bool serverSimGetPositionalSound(const ServerSim *sim) {
    return sim ? sim->positionalSound : false;
}

void serverSimSetOverviewWindow(ServerSim *sim, uint8_t window) {
    if (sim == NULL) return;
    if (window >= (uint8_t)OVERVIEW_WINDOW_COUNT) return;
    sim->overviewWindow = window;
}

uint8_t serverSimGetOverviewWindow(const ServerSim *sim) {
    return sim ? sim->overviewWindow : (uint8_t)overviewWindowExpanded;
}

void serverSimSetLineOfSight(ServerSim *sim, uint8_t mode) {
    if (sim == NULL) return;
    if (mode >= (uint8_t)LINE_OF_SIGHT_COUNT) return;
    sim->lineOfSight = mode;
}

uint8_t serverSimGetLineOfSight(const ServerSim *sim) {
    return sim ? sim->lineOfSight : (uint8_t)lineOfSightOff;
}

void serverSimSetVoiceMode(ServerSim *sim, ServerVoiceMode mode) {
    if (sim == NULL) return;
    if ((int)mode < serverVoiceOn || (int)mode > serverVoiceProximity) return;
    sim->voiceMode = mode;
}

ServerVoiceMode serverSimGetVoiceMode(const ServerSim *sim) {
    return sim ? sim->voiceMode : serverVoiceOn;
}

void serverSimSetScriptUploadPolicy(ServerSim *sim, ScriptUploadPolicy p) {
    if (sim == NULL) return;
    sim->scriptUploadPolicy = p;
}

ScriptUploadPolicy serverSimGetScriptUploadPolicy(const ServerSim *sim) {
    return sim ? sim->scriptUploadPolicy : SCRIPT_UPLOAD_ALLOW;
}

void serverSimSetScriptSharing(ServerSim *sim, bool on) {
    if (sim == NULL) return;
    sim->scriptSharingOff = !on;
}

bool serverSimGetScriptSharing(const ServerSim *sim) {
    return sim ? !sim->scriptSharingOff : true;
}

void serverSimSetScriptUploadDir(ServerSim *sim, const char *dir) {
    if (sim == NULL) return;
    SDL_strlcpy(sim->scriptUploadDir, dir != NULL ? dir : "",
                sizeof(sim->scriptUploadDir));
}

const char *serverSimGetScriptUploadDir(const ServerSim *sim) {
    return sim ? sim->scriptUploadDir : "";
}

void serverSimSetScriptSessionDir(ServerSim *sim, const char *dir) {
    if (sim == NULL) return;
    SDL_strlcpy(sim->scriptSessionDir, dir != NULL ? dir : "",
                sizeof(sim->scriptSessionDir));
}

const char *serverSimGetScriptSessionDir(const ServerSim *sim) {
    return sim ? sim->scriptSessionDir : "";
}

/* Process-wide rather than on a sim, because the listing cache it keeps
   honest is process-wide: one cache serves every sim in the process. Atomic
   because the tick thread bumps it and a client hosting in process lists from
   the UI thread through serverSimEnumerateScenarioDir. */
static SDL_AtomicInt scriptDirsGen;

void serverSimNoteScriptDirsChanged(void) {
    (void)SDL_AddAtomicInt(&scriptDirsGen, 1);
}

uint32_t serverSimScriptDirsGen(void) {
    return (uint32_t)SDL_GetAtomicInt(&scriptDirsGen);
}

int serverSimEmptyScriptSessionDir(ServerSim *sim) {
    char **names;
    int    count   = 0;
    int    removed = 0;
    int    i;

    if (sim == NULL || sim->scriptSessionDir[0] == '\0') return 0;
    /* "*" rather than NULL: a NULL pattern walks into subdirectories, and
       only the files the uploads put here are this call's to take. */
    names = SDL_GlobDirectory(sim->scriptSessionDir, "*", 0, &count);
    if (names == NULL) return 0;
    for (i = 0; i < count; i++) {
        const char  *name = names[i];
        char         path[FILENAME_MAX];
        SDL_PathInfo info;

        if (name == NULL || name[0] == '\0' ||
            SDL_strchr(name, '/') != NULL || SDL_strchr(name, '\\') != NULL) {
            continue;
        }
        SDL_snprintf(path, sizeof(path), "%s/%s", sim->scriptSessionDir, name);
        if (!SDL_GetPathInfo(path, &info) ||
            info.type != SDL_PATHTYPE_FILE) {
            continue;
        }
        if (SDL_RemovePath(path)) {
            removed++;
        } else {
            fprintf(stderr, "Warning: could not remove session script '%s': "
                            "%s\n", path, SDL_GetError());
        }
    }
    SDL_free(names);
    if (removed > 0) {
        serverSimNoteScriptDirsChanged();
    }
    return removed;
}

ScriptUploadPolicy scriptUploadPolicyResolve(const char *word) {
    if (word == NULL || word[0] == '\0') {
        return SCRIPT_UPLOAD_ALLOW;
    }
    if (SDL_strcasecmp(word, "off") == 0)     return SCRIPT_UPLOAD_OFF;
    if (SDL_strcasecmp(word, "allow") == 0)   return SCRIPT_UPLOAD_ALLOW;
    if (SDL_strcasecmp(word, "persist") == 0) return SCRIPT_UPLOAD_PERSIST;
    fprintf(stderr, "Warning: unknown script upload policy '%s', using allow\n",
            word);
    return SCRIPT_UPLOAD_ALLOW;
}

const char *scriptUploadPolicyWord(ScriptUploadPolicy p) {
    switch (p) {
        case SCRIPT_UPLOAD_OFF:     return "Off";
        case SCRIPT_UPLOAD_PERSIST: return "Persist";
        default:                    return "Allow";
    }
}

uint16_t serverSimGetViewDecaySecs(const ServerSim *sim, ViewCategory cat) {
    if (sim == NULL) return VIEW_DECAY_DEFAULT_SECS;
    if ((int)cat < 0 || (int)cat >= VIEW_CATEGORY_COUNT) {
        return VIEW_DECAY_DEFAULT_SECS;
    }
    return sim->viewDecaySecs[cat];
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

/* Negative sense throughout — see the field in server_sim_internal.h.
 * false is "pings allowed", which is what a sim nobody has configured
 * holds and what a NULL sim answers. */
void serverSimSetSmartPingsOff(ServerSim *sim, bool off) {
    if (sim) sim->smartPingsOff = off;
}

bool serverSimGetSmartPingsOff(const ServerSim *sim) {
    return sim ? sim->smartPingsOff : false;
}

/* Negative sense throughout as well — see the field in
 * server_sim_internal.h. false is "the mods run", which is what a sim
 * nobody has configured holds and what a NULL sim answers.
 *
 * The setter does not recompose. Which scripts play is decided in
 * scnDecideScenario and the lobby asks for that decision again through
 * lobbyScenarioReselect (src/server/server_command_dispatch.c), which is
 * where the pick paths already ask for it — a setter that recomposed would
 * do it twice for a host click and once for every test that only wanted the
 * value set. */
void serverSimSetModsOff(ServerSim *sim, bool off) {
    if (sim) sim->modsOff = off;
}

bool serverSimGetModsOff(const ServerSim *sim) {
    return sim ? sim->modsOff : false;
}

void serverSimSetState(ServerSim *sim, ServerState s) {
    if (sim) sim->state = s;
}

void serverSimSetServerLocks(ServerSim *sim, uint32_t locks) {
    if (sim) sim->serverLocks = serverSimAddImpliedLocks(locks);
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
