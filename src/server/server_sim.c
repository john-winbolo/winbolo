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
void simMapChangeCallback(BYTE x, BYTE y, BYTE terrain) {
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

static void serverSimInit(ServerSim *sim, gameType game, bool hiddenMines, int32_t startDelay, int32_t gameLen) {
    BYTE count;

    memset(sim, 0, sizeof(ServerSim));

    /* The classic gameplay numbers. A zeroed table would make every rule 0, so
     * this runs before anything can read one. */
    simRulesClassic(&sim->sim.rules);

    /* Sentinel value for "no batch start assigned" — memset gives 0, but 0
     * is a valid start index, so initialise explicitly. */
    for (count = 0; count < MAX_TANKS; count++) {
        sim->sim.pendingStartIdx[count] = MAX_STARTS;
        sim->sim.scenarioStartIdx[count] = MAX_STARTS;
        /* No spawn has named a loadout, so every seat asks the policy. */
        sim->sim.scenarioSpawnLoadout[count] = 0;
    }

    /* No scenario has declared a base game type yet, so a round that turns
       out to be scripted plays strict tournament until a lobby template says
       otherwise. */
    sim->sim.scenarioBaseGame = (gameType)0;

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
    /* ... and the announce.txt / commands.txt that go with them, read here
     * ONCE. The send path used to open both files per brain every time it
     * ran, and it runs inside the sync replay the spectator ring rebuilds on
     * every lobby keyframe. */
    serverSimRefreshBrainDocs(sim);
    /* The bot-name catalogue this server hands out, taken from the pools
     * loaded now. WinBoloDS loads -botnames or the shipped file after the
     * sim is made and calls serverSimRefreshBotPools again once it has. */
    serverSimRefreshBotPools(sim);

    sim->startDelay = startDelay;
    sim->gameLength = gameLen;
    sim->originalGameLength = gameLen;
    sim->tickLimit = 0;
    sim->ticksRun = 0;
    sim->gameTickLimit = 0;
    sim->gameTicksRun = 0;
    sim->snapshotCb = NULL;
    sim->snapshotInterval = 0;
    sim->snapshotTicks = 0;
    sim->tick = 0;
    sim->roundLogStartTick = ROUND_LOG_START_UNSET;
    /* No shells in flight toward a three-shot order yet. */
    memset(sim->shotOrder, 0, sizeof(sim->shotOrder));
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
     * defaults). botConfigs[] is zeroed too (personality=normal=0) but
     * difficulty is set explicitly below: zero is Easy, and a bot that
     * says Easy on its lobby row while playing exactly like Hard —
     * which every difficulty does today — is a lie. serverLocks
     * defaults to 0 — bolod --lock-* CLI flags set bits at server
     * startup. */
    for (count = 0; count < MAX_TANKS; count++) {
        sim->botConfigs[count].difficulty = BOT_DIFFICULTY_HARD;
    }

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
    /* Visibility rules. A server nobody has configured runs the classic
     * set: a pillbox shows only while the player is watching it, and
     * bases and allied tanks show not at all. memset would give every
     * category viewPolicyAlways and a zero decay, so set all three.
     * This is the authority for meaning A in view_policy.h. */
    sim->viewPolicy[viewCategoryPill] = VIEW_POLICY_STOCK_PILL;
    sim->viewPolicy[viewCategoryBase] = VIEW_POLICY_STOCK_BASE;
    sim->viewPolicy[viewCategoryAlly] = VIEW_POLICY_STOCK_ALLY;
    for (count = 0; count < VIEW_CATEGORY_COUNT; count++) {
        sim->viewDecaySecs[count] = VIEW_DECAY_DEFAULT_SECS;
    }
    /* The map overview keeps the narrow window live, with nothing
     * blocking sight inside it. Both are written out rather than left to
     * the memset: LINE_OF_SIGHT_STOCK happens to be zero today, and a
     * later change to it must not quietly stop applying here. */
    sim->overviewWindow = (uint8_t)OVERVIEW_WINDOW_STOCK;
    sim->lineOfSight    = (uint8_t)LINE_OF_SIGHT_STOCK;
    /* Smart pings are allowed until a host says otherwise. Written out
     * rather than left to the memset for the same reason the two above
     * are: the field is stored in the negative sense, so the line has to
     * say which way round "false" reads. */
    sim->smartPingsOff  = FALSE;
    /* And the mods on the pick list compose until a host says otherwise,
     * written out for the same reason: the field is stored in the negative
     * sense, so the line has to say which way round "false" reads. */
    sim->modsOff        = FALSE;
    sim->maxPlayers          = MAX_TANKS;
    sim->maxSpectators       = 0;
    sim->specDelayTicks      = 0;
    sim->specRosterEnum      = NULL;
    sim->specRosterEnumCtx   = NULL;
    sim->startInProgress     = FALSE;
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
    sim->sim.callbacks.shellFired = serverSimCbShellFired;
    sim->sim.callbacks.recordDamage = serverSimCbRecordDamage;
    sim->sim.callbacks.recordPlayerAction = serverSimCbRecordPlayerAction;
    sim->sim.callbacks.recordPillPickup = serverSimCbRecordPillPickup;
    /* The captures and the builder loss. Registered on the server alone, for
     * the same reason the records above are: the event queue is here. */
    sim->sim.callbacks.baseOwnerChanged = serverSimCbBaseOwnerChanged;
    sim->sim.callbacks.pillOwnerChanged = serverSimCbPillOwnerChanged;
    sim->sim.callbacks.lgmDied = serverSimCbLgmDied;
    sim->sim.callbacks.tankSpawned = serverSimCbTankSpawned;
    sim->sim.callbacks.lgmLanded = serverSimCbLgmLanded;
    sim->sim.callbacks.pillPlaced = serverSimCbPillPlaced;
    sim->sim.callbacks.pillKilled = serverSimCbPillKilled;
    sim->sim.callbacks.built = serverSimCbBuilt;
    sim->sim.callbacks.mineLaid = serverSimCbMineLaid;
    sim->sim.callbacks.mineExploded = serverSimCbMineExploded;
    sim->sim.callbacks.tankHit = serverSimCbTankHit;
    /* The policy queries. Registered on the server alone: a ClientSim leaves
     * them NULL, which is what keeps shared code on the classic branch
     * there. */
    sim->sim.callbacks.chooseStart = serverSimCbChooseStart;
    sim->sim.callbacks.spawnLoadout = serverSimCbSpawnLoadout;
    sim->sim.callbacks.canRespawn = serverSimCbCanRespawn;
    sim->sim.callbacks.damageScale = serverSimCbDamageScale;
    sim->sim.callbacks.canBuild = serverSimCbCanBuild;
    sim->sim.callbacks.canCapture = serverSimCbCanCapture;
    sim->sim.callbacks.canDie = serverSimCbCanDie;
    sim->sim.callbacks.canHit = serverSimCbCanHit;
    sim->sim.callbacks.pillDamageScale = serverSimCbPillDamageScale;
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
            sim->sim.baseTimer[i] = BASE_TIMER_OFF;
        }
        sim->sim.baseTimer[0] = sim->sim.rules.base_regen_ticks;
    }

    /* Bind every slot's copy of the terrain to the map just created. The
     * three creators re-seed once their map is loaded; doing it here as well
     * means no slot's handle is left NULL by the memset above. */
    serverSimShadowSeedAll(sim);

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
    /* The file the live map came from, kept so a scenario can be looked for
       beside it. Every other loader either sets this or clears it. */
    SDL_strlcpy(sim->mapFilePath, mapFileName ? mapFileName : "",
                sizeof(sim->mapFilePath));

    if (mapRead(mapFileName, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss) == FALSE) {
        WB_LOG_ERROR(WB_LOG_CAT_SERVER,
            "serverSim create: mapRead failed for '%s'",
            mapFileName ? mapFileName : "(null)");
        serverSimDestroy(sim);
        return NULL;
    }
    /* The map is this sim's now: cap what it brought against the rules. */
    mapClampToRules(&sim->sim);

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
        BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
        int len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
        sim->cachedMapData = malloc(len);
        if (sim->cachedMapData != NULL) {
            memcpy(sim->cachedMapData, tempBuf, len);
            sim->cachedMapDataLen = len;
        }
    }

    /* The map is loaded — restart every slot's copy of the terrain from it. */
    serverSimShadowSeedAll(sim);

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
    /* The map is this sim's now: cap what it brought against the rules. */
    mapClampToRules(&sim->sim);

    if (mapName != NULL && mapName[0] != '\0') {
        strncpy(sim->mapName, mapName, MAP_STR_SIZE - 1);
        sim->mapName[MAP_STR_SIZE - 1] = '\0';
    }

    basesClearMines(&sim->sim);

    /* Cache the initial map state for between-round resets */
    {
        BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
        int len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
        sim->cachedMapData = malloc(len);
        if (sim->cachedMapData != NULL) {
            memcpy(sim->cachedMapData, tempBuf, len);
            sim->cachedMapDataLen = len;
        }
    }

    /* The map is loaded — restart every slot's copy of the terrain from it. */
    serverSimShadowSeedAll(sim);

    sim->state = sim->lobbyEnabled ? serverStateLobby : serverStateRunning;
    return sim;
}

ServerSim *serverSimCreateRandomMap(const MapGenConfig *cfg,
                                    gameType game, bool hiddenMines,
                                    int32_t startDelay, int32_t gameLen) {
    ServerSim *sim;
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
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
            pillsSetPill(&sim->sim, &sim->sim.pb, &tmp, (BYTE)(i + 1));
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
    /* The generated map is this sim's too: cap what it put in the lists
       against the rules, as a loaded one is capped. */
    mapClampToRules(&sim->sim);

    basesClearMines(&sim->sim);

    /* Set map name to "rand_<seed>" */
    mapGenConfigToSeed(cfg, seedStr, sizeof(seedStr));
    snprintf(sim->mapName, MAP_STR_SIZE, "rand_%.30s", seedStr);

    /* Cache compressed map data for client distribution */
    len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
    sim->cachedMapData = malloc(len);
    if (sim->cachedMapData == NULL) {
        serverSimDestroy(sim);
        return NULL;
    }
    memcpy(sim->cachedMapData, tempBuf, len);
    sim->cachedMapDataLen = len;

    /* The generated map is in place — restart every slot's copy from it. */
    serverSimShadowSeedAll(sim);

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

    /* The brains' lobby texts, allocated on the first refresh. */
    serverSimFreeBrainDocs(sim);
    free(sim->botPoolBlob);
    sim->botPoolBlob = NULL;

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

    /* And the scripts.json text beside it. logDestroy above has already
     * closed any recording that could still write it. */
    serverSimSetScenarioRecordText(sim, NULL, 0);

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

uint16_t serverSimGetPlayerKills(const ServerSim *sim, BYTE slot) {
    if (sim == NULL || slot >= MAX_TANKS) return 0;
    return (uint16_t)sim->roundStats[slot].kills;
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
        isBot[slot] = serverSimIsBot(sim, (BYTE)slot);
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

    /* A scenario's own scoreboard, taken from the rows its score op wrote.
     * Both stores are cleared at round start, so a round with no scenario —
     * or one whose scenario never scored — leaves hasScenarioScore false and
     * the recap without a column.
     *
     * Player rows are keyed by a 0-based slot and team rows by the team
     * number (1..MAX_TANKS-1, row 0 naming no team); the summary keeps those
     * two bases, so a row is copied straight across at its own index, and
     * each mask carries the store's own valid bit at that same index. The
     * mask is what tells a reader a row scored zero from a row nobody
     * scored, which the number alone cannot.
     *
     * The op lets every row carry its own label and the recap has one column
     * to head, so the first label found wins: player rows by ascending slot,
     * then team rows by ascending team. A scenario that wants a predictable
     * title gives every row the same one. */
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        const ScnScoreRow *row = &sim->scenarioPlayerScores[slot];
        if (!row->valid) continue;
        out->scenarioScoreMask |= (uint16_t)(1u << slot);
        out->scenarioScore[slot] = row->score;
        if (out->scenarioScoreLabel[0] == '\0' && row->label[0] != '\0') {
            strncpy(out->scenarioScoreLabel, row->label,
                    sizeof(out->scenarioScoreLabel) - 1);
        }
    }
    for (int team = 1; team < MAX_TANKS; team++) {
        const ScnScoreRow *row = &sim->scenarioTeamScores[team];
        if (!row->valid) continue;
        out->scenarioTeamScoreMask |= (uint16_t)(1u << team);
        out->scenarioTeamScore[team] = row->score;
        if (out->scenarioScoreLabel[0] == '\0' && row->label[0] != '\0') {
            strncpy(out->scenarioScoreLabel, row->label,
                    sizeof(out->scenarioScoreLabel) - 1);
        }
    }
    out->scenarioScoreLabel[sizeof(out->scenarioScoreLabel) - 1] = '\0';
    out->hasScenarioScore =
        (out->scenarioScoreMask | out->scenarioTeamScoreMask) != 0;
}

bool serverSimIsRunning(void) {
    ServerSim *sim = serverSimGetActive();
    return sim != NULL && sim->state == serverStateRunning;
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

