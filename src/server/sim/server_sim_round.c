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
 *Name:          Server Simulation Round Lifecycle
 *Filename:      server_sim_round.c
 *Author:        John Morrison
 *Purpose:
 *  The round lifecycle — starting a round, the win and
 *  auto-close checks, game-over resolution and the return
 *  to lobby, plus the WinBolo.net lobby-info refresh that
 *  rides along with it.
 *********************************************************/

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <SDL3/SDL.h>

#include "server_sim_shared.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"   /* lobbyAutoUnreadyOnChange, and the serverLifecycle tick timing (stats, peak, reset) via server_lifecycle.h */
#include "treegrow.h"               /* treeGrowReset — the world reset's tree state */
#include "sim_rules.h"              /* simRulesClassic — the table a round starts from */
#include "start_sides.h"            /* START_SIDE_ANY — the per-team side table handed to startsAssignBatch */
#include "../../winbolonet/winbolonet_core.h"     /* winbolonetAddEvent, WINBOLO_NET_EVENT_WIN */
#include "../../winbolonet/winbolonet_server.h"   /* WbnLobbyInfo, winbolonetSetLobbyInfo, winbolonetSendLobbyUpdate */
#include "../../common/md5.h"       /* the BMAPBOLO map hash WinBolo.net matches against */
#include "../../common/wb_log.h"    /* WB_LOG_INFO — the lobby-reset trace */

/* Return an emptied lobby to the operator's startup configuration. Called
 * from serverSimRemovePlayer when the last human leaves while in the lobby:
 * remove every bot, restore the captured settings snapshot, reset team and
 * bot-slot metadata to creation defaults, and unlock the lobby to joiners. */
void serverSimResetLobbyToDefaults(ServerSim *sim) {
    BYTE i;
    if (sim == NULL) return;

    WB_LOG_INFO(WB_LOG_CAT_SERVER,
        "Lobby empty — resetting to startup defaults");

    /* Drop any bots the previous occupants added, seats held for a bot that
       was never fielded included — those have no bot manager entry, so the
       roster is what has to be asked. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsBot(sim, i)) {
            serverSimRemoveBot(sim, i);
        }
    }

    /* Clear per-slot start reservations back to the none sentinel. */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->lobbyPlayers[i].startIdx = 0xFF;
    }

    /* Restore the operator-configured game settings. Guarded on the snapshot
     * being captured (always true once serverSimApplyInstanceConfig ran); if
     * it somehow wasn't, leave the live settings untouched. */
    if (sim->originalLobbySettings.valid) {
        sim->sim.game            = sim->originalLobbySettings.gameType;
        sim->sim.hiddenMines     = sim->originalLobbySettings.hiddenMines;
        sim->botAiType           = sim->originalLobbySettings.botAiType;
        sim->aiPolicy            = sim->originalLobbySettings.aiPolicy;
        sim->timeLimit           = sim->originalLobbySettings.timeLimit;
        sim->timeMinutes         = sim->originalLobbySettings.timeMinutes;
        sim->gameLength          = sim->originalLobbySettings.gameLength;
        sim->openHost            = sim->originalLobbySettings.openHost;
        sim->autoLockOnGameStart = sim->originalLobbySettings.autoLockOnGameStart;
        sim->ranked              = sim->originalLobbySettings.ranked;
        sim->serverLocks         = sim->originalLobbySettings.serverLocks;
        for (i = 0; i < VIEW_CATEGORY_COUNT; i++) {
            sim->viewPolicy[i]    = sim->originalLobbySettings.viewPolicy[i];
            sim->viewDecaySecs[i] = sim->originalLobbySettings.viewDecaySecs[i];
        }
        sim->classicMode         = sim->originalLobbySettings.classicMode;
        sim->alliesInTrees       = sim->originalLobbySettings.alliesInTrees;
        sim->overviewWindow      = sim->originalLobbySettings.overviewWindow;
        sim->lineOfSight         = sim->originalLobbySettings.lineOfSight;
        sim->smartPingsOff       = sim->originalLobbySettings.smartPingsOff;
    }

    /* A fresh lobby always starts with slot 0 as host, regardless of who
     * hosted the previous round. */
    sim->hostSlot = 0;

    /* Reset team and bot-slot metadata to the creation defaults (two teams
     * always present, default bot configs, per-slot brain back to the
     * CLI-configured default). Mirrors serverSimInit. */
    memset(sim->teams, 0, sizeof(sim->teams));
    sim->teams[1].in_use     = 1;
    sim->teams[1].color      = 0;  /* red */
    sim->teams[1].namingPool = 0;  /* classic */
    SDL_strlcpy(sim->teams[1].name, "Team 1", LOBBY_TEAM_NAME_LEN);
    sim->teams[2].in_use     = 1;
    sim->teams[2].color      = 1;  /* blue */
    sim->teams[2].namingPool = 0;
    SDL_strlcpy(sim->teams[2].name, "Team 2", LOBBY_TEAM_NAME_LEN);
    memset(sim->botConfigs, 0, sizeof(sim->botConfigs));
    /* Difficulty's default is Hard, not the memset's 0 (= Easy) — same
     * reasoning as serverSimInit: every difficulty plays like Hard for now,
     * so Hard is the honest label on a fresh bot's lobby row. */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->botConfigs[i].difficulty = BOT_DIFFICULTY_HARD;
    }
    memset(sim->botBrainIdx, 0xFF, sizeof(sim->botBrainIdx));
    /* Forget the host's manual bot mode/difficulty pick with the configs it
     * came from, and any bot-config event still queued: this is "the lobby
     * is empty, restore the operator's startup state", and the next
     * occupants inherit nothing from the last ones. */
    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';
    sim->botConfigPublishPending = 0;

    /* Unlock the lobby to new players: clear both the host-toggled
     * allow-new-players gate and the transport-level admin lock. */
    sim->allowNewPlayers      = TRUE;
    sim->savedAllowNewPlayers = TRUE;
    transportUdpServerSetLock(sim, FALSE);

    /* Publish the restored settings and refresh the WBN listing. The last
     * human just left, so no control-event subscribers remain to receive the
     * reset; the next joiner picks up the full lobby state (settings, team
     * metadata, bot configs) via the join-time sync replay. */
    /* The reset above emptied the lobby, seats a scenario put there
       included. Seat them again at the end of it, so the next joiner opens
       the map's own lobby rather than a bare one — the scenario is still
       attached, only its lobby was swept. */
    serverSimScenarioSeatLobby(sim);

    /* And the settings the scenario asks for, in the order a map commit does
       the two. The restore above put the operator's game type, ranked flag
       and AI policy back, which are the settings a scripted map displaces:
       without this the lobby would be left on them with the scenario still
       attached. The operator's type is what is remembered as displaced here,
       and it is what a plain map committed later gives back. */
    serverSimScenarioApplyLobbyRules(sim);

    serverSimPublishLobbySettings(sim);
    serverSimWbnLobbyUpdate(sim, FALSE);
}

bool serverSimIsTerminalGameOver(const ServerSim *sim) {
    /* A game-over that shuts the dedicated server down, as opposed to one it
     * recovers from. A lobby server returns to the lobby; a map-rotation server
     * boots everyone and starts the next round. Only a plain no-lobby server
     * (e.g. -nolobby / -quitonwin) treats a win as a process shutdown.
     *
     * The dedicated-server command loop polls this without the tick lock, so it
     * stays a pure read of these flags. On a win or a passed back-to-lobby vote
     * the rotation flips state from gameOver back to running inside a single
     * tick; a loop that quit on that transient gameOver would race the rotation
     * and shut the process down (the "-maprotate exits like -quitonwin" bug). */
    return serverSimGetState(sim) == serverStateGameOver &&
           !serverSimIsLobbyEnabled(sim) &&
           !serverSimIsMapRotateEnabled(sim);
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
            playersGetPlayerName(&sim->sim.plyrs, count, name, sizeof(name), TRUE);

            /* Ping and the input-buffer depth are in-process artifacts for
             * bot slots (0ms ping, a buffer that exists but never gates an
             * in-process input), so they'd only invite a misread. Show them
             * only for real connections. The field formerly printed as
             * "Jitter" is the adaptive input jitter-buffer target depth in
             * ticks, not a measured network statistic — labelled "Buf" so
             * nobody reads network variance into it. */
            BotInfo bi;
            bool isBotSlot = botManagerGetBotInfo(sim, count, &bi);
            /* A seat the roster is holding for a bot that is not on the field
               — a horde seat between waves. It owns nothing and has no tank,
               and the marker is what tells it from a slot whose bot is out
               there playing. Printed in the shape a bot slot gets rather than
               the one below it: the ping, the buffer depth and the address the
               other branch prints are for a real connection, and a held seat
               has none, so they would read as a human sitting at 0ms. The
               marker also says whether the seat has a runner parked behind it,
               because the two cost very different things: a seat with one
               fields its bot by resuming, a seat without one has to build a
               ClientSim and a brain VM at the moment a wave asks for it. */
            bool heldSeat = !isBotSlot && !sim->lobbyPlayers[count].fielded;
            if (isBotSlot || heldSeat) {
                const char *seatMark = "";
                if (heldSeat) {
                    seatMark = botManagerHasRunner(sim, count)
                                   ? " [off the field, runner ready]"
                                   : " [off the field]";
                }
                fprintf(stdout, "%s - (P:%d B:%d)%s\n",
                        name,
                        pillsGetNumberOwnedByPlayer(&sim->sim.pb, count),
                        basesGetNumberOwnedByPlayer(&sim->sim.bs, count),
                        seatMark);
            } else {
                /* Remote players carry their source ip:port; the in-process
                 * host has no UDP client, so the getter reports false and we
                 * print "local" instead. */
                char addr[48];  /* "255.255.255.255:65535" + slack */
                const char *addrStr =
                    transportUdpServerGetClientAddrStr(count, addr, sizeof(addr))
                        ? addr : "local";
                fprintf(stdout, "%s - (P:%d B:%d Ping:%dms Buf:%d Addr:%s)\n",
                        name,
                        pillsGetNumberOwnedByPlayer(&sim->sim.pb, count),
                        basesGetNumberOwnedByPlayer(&sim->sim.bs, count),
                        sim->playerPing[count],
                        sim->jitterTarget[count],
                        addrStr);
            }

            /* For bot slots, append a 4-space-indented [BOT] line with the
             * brain's timing. peak= is the high-water think this game (reset
             * each round start; the magnitude behind overruns, which last=
             * and the EWMA hide); overruns carries its rate against thinkCount
             * so a cumulative count can't masquerade as "spiking right now". */
            if (isBotSlot) {
                if (bi.hasBrain) {
                    if (bi.overrunCount == 0) {
                        fprintf(stdout,
                                "    [BOT] brain=%s last=%.1fms peak=%.1fms target=%.1fms\n",
                                bi.brainName, bi.lastThinkMs, bi.maxThinkMs,
                                bi.targetMs);
                    } else {
                        double rate = bi.thinkCount > 0
                            ? (double)bi.overrunCount * 100.0 / (double)bi.thinkCount
                            : 0.0;
                        fprintf(stdout,
                                "    [BOT] brain=%s last=%.1fms peak=%.1fms target=%.1fms overruns=%u/%u (%.2f%%)\n",
                                bi.brainName, bi.lastThinkMs, bi.maxThinkMs,
                                bi.targetMs, bi.overrunCount, bi.thinkCount, rate);
                    }
                } else {
                    fprintf(stdout, "    [BOT]\n");
                }
            }
        }
    }

    /* Server-loop timing (Tick / Simulation) is recorded every server tick
     * regardless of bots, so show it whenever there's data — operators of
     * a bot-free server still want to see the loop stays inside budget.
     * The bot pool block below stays gated on actual bots. */
    {
        double tickLast = 0.0, tickEwma = 0.0;
        serverLifecycleGetTickStats(&tickLast, &tickEwma);
        double simLast = 0.0, simEwma = 0.0;
        serverLifecycleGetSimStats(&simLast, &simEwma);
        double tickPeak = 0.0;
        unsigned int tickOverBudget = 0;
        serverLifecycleGetTickPeak(&tickPeak, &tickOverBudget);

        if (tickLast > 0.0 || simLast > 0.0) {
            fprintf(stdout, "Server timing:\n");
        }
        /* peak= is the worst tick this round and "over budget" counts the
         * ticks that reached 20ms (both reset at round start). The EWMA
         * decays a spike away within about 22 ticks, so a handful of
         * expensive frames — a wave transition, say — leaves no trace in
         * last= or the average by the time this command is typed. */
        if (tickLast > 0.0) {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms  peak=%.1fms  "
                    "over budget=%u (budget=20ms)\n",
                    "Tick:", tickLast, tickEwma, tickPeak, tickOverBudget);
        }
        if (simLast > 0.0) {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms\n",
                    "Simulation:", simLast, simEwma);
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

        /* peak= is the worst single-bot think across the pool this game
         * (reset each round start) — the EWMA averages spikes away, so
         * without it a pool that mostly runs cheap but takes occasional 20ms
         * thinks looks identical to one that never does. */
        fprintf(stdout,
                "  %-11s last=%.1fms  EWMA=%.1fms  peak=%.1fms\n",
                "Brain:", ps.lastBrainPhaseMs, ps.ewmaBrainPhaseMs,
                ps.maxThinkMs);
        /* "Bot prep" labels the non-brain serial parts of
         * botManagerTick: snapshot/sync + input send. Distinct from
         * "Simulation:" above which times the two serverSimTick calls.
         * Overruns carry their rate against total thinks — a cumulative
         * count alone grows forever and can't tell ongoing from historical. */
        if (ps.totalOverruns == 0) {
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms\n",
                    "Bot prep:", ps.lastSerialMs, ps.ewmaSerialMs);
        } else {
            double poolRate = ps.totalThinks > 0
                ? (double)ps.totalOverruns * 100.0 / (double)ps.totalThinks
                : 0.0;
            fprintf(stdout,
                    "  %-11s last=%.1fms  EWMA=%.1fms  total overruns=%u/%u (%.2f%%)\n",
                    "Bot prep:", ps.lastSerialMs, ps.ewmaSerialMs,
                    ps.totalOverruns, ps.totalThinks, poolRate);
        }
    }

    fprintf(stdout, "\n");
}

bool serverSimSaveMap(ServerSim *sim, char *fileName) {
    return mapWrite(fileName, &sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss);
}

BYTE serverSimWinningOwner(ServerSim *sim) {
    BYTE count;
    BYTE max;
    BYTE live = 0;
    BYTE first = NEUTRAL;
    BYTE current;

    max = basesGetNumBases(&sim->sim.bs);
    if (max == 0) {
        return NEUTRAL;
    }

    /* A removed base is not on the map, so it neither blocks the win nor
       counts toward it; the first live base sets the owner to match. */
    for (count = 1; count <= max; count++) {
        BYTE shellsAmt, minesAmt, armourAmt;
        if (basesIsActive(&sim->sim.bs, count) == FALSE) {
            continue;
        }
        current = basesGetBaseOwner(&sim->sim.bs, count);
        basesGetStats(&sim->sim.bs, count, &shellsAmt, &minesAmt, &armourAmt);
        if (current == NEUTRAL ||
            armourAmt <= sim->sim.rules.base_capture_armour) {
            return NEUTRAL;
        }
        if (live == 0) {
            first = current;
        } else if (!playersIsAllie(&sim->sim.plyrs, current, first)) {
            return NEUTRAL;
        }
        live++;
    }

    if (live == 0) {
        return NEUTRAL;
    }
    return first;
}

bool serverSimCheckGameWin(ServerSim *sim, bool printWinners) {
    BYTE count;
    BYTE first;
    char name[256];

    /* A scenario may take the base sweep out of the round's endings, and
       only the sweep: every other way a round ends is untouched. Asked
       before the board is read, so a scenario that has turned the sweep off
       pays nothing for it. */
    if (sim->scenarioPolicy != NULL && sim->scenarioPolicy->allowBaseWin != NULL) {
        bool allow;
        serverSimScenarioPolicyEnter(sim);
        allow = sim->scenarioPolicy->allowBaseWin(sim->scenarioPolicy->ctx);
        serverSimScenarioPolicyLeave(sim);
        if (allow == FALSE) {
            return FALSE;
        }
    }

    first = serverSimWinningOwner(sim);
    if (first == NEUTRAL) {
        return FALSE;
    }

    if (printWinners) {
        fprintf(stdout, "Game Won!\nWinners:\n");
        for (count = 0; count < MAX_TANKS; count++) {
            if (!sim->playerConnected[count]) continue;
            if (playersIsAllie(&sim->sim.plyrs, count, first) || count == first) {
                playersGetPlayerName(&sim->sim.plyrs, count, name, sizeof(name), TRUE);
                fprintf(stdout, "  %s\n", name);
            }
        }
    }

    return TRUE;
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
        if (sim->playerConnected[count] && !serverSimIsBot(sim, count)) {
            num++;
        }
    }
    return num;
}

/* Compute and cache the MD5 of a BMAPBOLO .map file so WBN can match
 * the map against its library. Streams the file (handles any size) and
 * only accepts it when the BMAPBOLO magic is present; clears the valid
 * flag on any failure or non-.map input. */
void serverSimCacheMapMd5FromFile(ServerSim *sim, const char *path) {
    FILE *f;
    BYTE buf[8192];
    size_t n;
    Md5Ctx ctx;
    bool headerChecked = FALSE;
    bool isBmap = FALSE;
    if (sim == NULL) return;
    sim->mapMd5Valid = FALSE;
    sim->mapMd5Hex[0] = '\0';
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
        md5ToHex(sim->mapMd5, sim->mapMd5Hex);
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
    if (sim->mapMd5Hex[0] != '\0' && !sim->randomMapEnabled) {
        snprintf(info.mapMd5, sizeof(info.mapMd5), "%s", sim->mapMd5Hex);
    } else {
        info.mapMd5[0] = '\0';
    }
    info.randomMap       = sim->randomMapEnabled ? true : false;
    info.gameType        = (BYTE)gameTypeGet(&sim->sim.game);
    info.ai              = (BYTE)sim->botAiType;
    info.mines           = sim->sim.hiddenMines ? true : false;
    info.ranked          = sim->ranked ? true : false;
    info.allowNewPlayers = sim->allowNewPlayers ? true : false;
    info.allowSpectators = serverSimGetMaxSpectators(sim) > 0 ? true : false;
    info.autoLock        = sim->autoLockOnGameStart ? true : false;
    info.hasLobby        = serverSimIsLobbyEnabled(sim);
    info.timeLimit       = serverSimGetTimeLimit(sim) ? true : false;
    info.timeMinutes     = serverSimGetTimeMinutes(sim);
    info.lobbyLocks      = sim->serverLocks;
    info.numBases        = basesGetNumActive(&sim->sim.bs);
    info.numPills        = pillsGetNumActive(&sim->sim.pb);
    info.freeBases       = serverSimGetNumNeutralBases(sim);
    info.freePills       = serverSimGetNumNeutralPills(sim);
    info.numHumans       = serverSimGetNumHumans(sim);
    info.numBots         = botManagerGetNumBots(sim);
    info.pillView        = (BYTE)sim->viewPolicy[viewCategoryPill];
    info.baseView        = (BYTE)sim->viewPolicy[viewCategoryBase];
    info.allyView        = (BYTE)sim->viewPolicy[viewCategoryAlly];
    info.classicMode     = sim->classicMode;
    info.alliesInTrees   = serverSimGetAlliesInTrees(sim);
    info.overviewWindow  = serverSimGetOverviewWindow(sim);
    info.lineOfSight     = serverSimGetLineOfSight(sim);
    info.smartPingsOff   = serverSimGetSmartPingsOff(sim);
    info.pillViewDecay   = serverSimGetViewDecaySecs(sim, viewCategoryPill);
    info.baseViewDecay   = serverSimGetViewDecaySecs(sim, viewCategoryBase);
    info.allyViewDecay   = serverSimGetViewDecaySecs(sim, viewCategoryAlly);
    info.voiceMode       = serverSimGetVoiceMode(sim);
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

void serverSimAbortCountdown(ServerSim *sim) {
    /* The runners this countdown built belong to the round it was leading to,
     * and that round is not happening. Left parked they would sit in the lobby
     * for as long as it lasts and then be handed to a later round, each brain
     * having read the lobby as it stood at this countdown. The next countdown
     * builds them again against the lobby as it stands then. */
    botManagerReleaseParkedRunners(sim);
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

void serverSimEnterGameOver(ServerSim *sim) {
    /* roundStats and the notable-event timeline are final here: nothing
     * mutates them until the next serverSimResetGameWorld (the funnel only
     * runs in serverStateRunning), so they survive the game-over hold intact
     * for downstream phases to publish from. No snapshot/freeze buffer needed. */

    /* Freeze per-slot identity (bot flag, team, name) for the just-finished
     * round while the roster is still intact, so the attribution track can be
     * read back offline without a live server. */
    for (int slot = 0; slot < MAX_TANKS; slot++) {
        AttrSlotIdentity *id = &sim->trackIdentity[slot];
        /* A slot that is already gone keeps whatever serverSimRemovePlayer
         * stamped on the way out — the roster has no name for it any more.
         * serverSimResetGameWorld zeroes the whole table at round start, so
         * an entry surviving to here belongs to the round just played. */
        if (!sim->playerConnected[slot]) continue;
        /* Read the full-length name first; id->name is the shorter
         * wire-sized field, so the copy into it truncates. */
        char nameBuf[PLAYER_NAME_LEN];
        memset(id, 0, sizeof(*id));
        id->isBot = serverSimIsBot(sim, (BYTE)slot) ? 1 : 0;
        id->team  = sim->lobbyPlayers[slot].teamNumber;
        playersGetPlayerName(&sim->sim.plyrs, (BYTE)slot, nameBuf,
                             sizeof(nameBuf), TRUE);
        snprintf(id->name, sizeof(id->name), "%s", nameBuf);
    }

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

    /* The roster the round ended with is back, so this is where the lobby a
       scenario asks for is brought into line with it: what the host changed
       between rounds stands, a team grown past its ceiling is cut back, and
       the seats the script fielded during the round go back to being held so
       the next round starts where the last one did. */
    serverSimScenarioReconcileLobby(sim);

    /* The round is over, so every parked runner goes. A parked brain keeps
       its state table, which is what makes a resume worth having inside a
       round and wrong across one: the brain would open the next round still
       remembering this one's goal and owners. After the reconcile above and
       not before it — that call takes a seat the script still had on the
       field off it, which parks its runner, so a release before it would
       leave exactly those behind. */
    botManagerReleaseParkedRunners(sim);

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

    /* Forget the host's manual bot mode/difficulty pick on EVERY return to
     * the lobby, whether or not anyone stayed. The pick belongs to one lobby
     * session — from entering the lobby until the game starts — and a round
     * ending starts a new one, so the host who set a bot to Survival last
     * game does not find the next lobby's Add Bot already in Survival. The
     * empty-lobby branch below calls serverSimResetLobbyToDefaults, which
     * clears these again; clearing twice costs nothing and keeps the two
     * paths honest on their own.
     *
     * A map change WITHIN one lobby session is deliberately not a new lobby:
     * the pick survives it. That is an assumption about what the host means
     * by picking a difficulty, not a constraint — clear it in
     * serverSimApplyMapChange too if it turns out hosts expect otherwise.
     *
     * The map's own rule — a scenario that fixes a team's mode — is applied
     * by serverSimResolveNewBotConfig regardless of what is remembered here,
     * so it is unaffected either way. */
    sim->lastBotModeKey[0]  = '\0';
    sim->lastBotLevelKey[0] = '\0';

    /* Auto-lock only closes the server while a round is running, so coming
     * back to the lobby must lift it. When a round that had human players ends
     * with none of them left, wipe the slate the way the last-human-leaves-in-
     * lobby path does — drop the bots, restore the operator's startup
     * settings, and unlock — so the next joiner gets a fresh, open lobby with
     * no orphaned bots. A round that only ever had bots (e.g. an idle
     * dedicated server cycling maps) keeps them and just releases the
     * auto-lock, restoring the pre-round join policy (savedAllowNewPlayers,
     * captured by serverSimApplyAutoLockOnGameStart). */
    if (sim->roundHadHuman && serverSimGetNumHumans(sim) == 0) {
        serverSimResetLobbyToDefaults(sim);
    } else {
        sim->allowNewPlayers = sim->savedAllowNewPlayers;
        if (sim->savedAllowNewPlayers) {
            transportUdpServerSetLock(sim, FALSE);
        }
    }

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
    BYTE numHumans = 0;
    BYTE i;

    /* A start publishes while the state is still lobby and every player
     * is still marked ready, so anything that edits the roster from one
     * of those publishes lands back here and would start a second game
     * on top of the one being set up. The flag is held for the whole of
     * both start functions, which is wider than the state check below:
     * the state reads running well before a start finishes, and a roster
     * edit issued from the round-start callback must not re-enter here
     * on a half-built round. */
    if (sim->startInProgress) return;

    if (sim->state != serverStateLobby) return;

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        numConnected++;
        if (!serverSimIsBot(sim, i)) numHumans++;
        if (!sim->lobbyPlayers[i].ready) return; /* Not all ready */
    }
    if (numConnected == 0) return;
    /* Never auto-start a human-less lobby. Bots default to ready, so a lobby
     * holding only ready bots (e.g. the moment the last human leaves) would
     * otherwise trip this and launch a bot-only game. A start is always
     * driven by a human readying up. */
    if (numHumans == 0) return;

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
        /* The countdown is the window the held seats' runners are built in
           (serverSimWarmOneHeldSeat). A seat it refuses is named once per
           countdown, so the record of what it refused starts empty here. */
        sim->warmSkippedSlots = 0;
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
    sim->sim.tkExpUpdateTime = 0;

    treeGrowReset(&sim->sim);

    /* 3. Reload map/bases/pills/starts from cached data */
    if (sim->cachedMapData != NULL) {
        mapLoadCompressedMap(&sim->sim.mp, &sim->sim.pb, &sim->sim.bs, &sim->sim.ss,
                             sim->cachedMapData, sim->cachedMapDataLen);
        /* The map is this sim's now: cap what it brought against the rules. */
        mapClampToRules(&sim->sim);
    }

    /* 4. Clear mines from under bases */
    basesClearMines(&sim->sim);

    /* Every client is about to be handed the reloaded map (the caller
     * republishes it to UDP and in-process audiences alike), so restart every
     * slot's copy of the terrain from it. Placed after the mine clear so the
     * copies match the map the blob is compressed from. */
    serverSimShadowSeedAll(sim);

    /* 5. Reset lag compensation state */
    for (i = 0; i < MAX_TANKS; i++) {
        posHistoryInit(&sim->posHistory[i]);
        posHistoryInit(&sim->lgmPosHistory[i]);
    }
    sim->sim.lagCompTicks = 0;
    memset(sim->sim.perPlayerCompTicks, 0, sizeof(sim->sim.perPlayerCompTicks));

    /* 6. Clear events. The map event count too: a running frame clears it at
       its top but a round that ended with the buffer full would otherwise
       carry the count into the lobby, where the fill drain reads it as a
       bound and would write nothing for as long as the lobby lasted. */
    sim->eventCount = 0;
    sim->mapEventCount = 0;
    /* And any fill a scenario still had squares owing on. Its rectangle was
       aimed at the map that has just been replaced above, so carrying it on
       would paint the reloaded one. The roster changes it had queued name
       seats in the round that is ending, so they go the same way. */
    serverSimScenarioResetFill(sim);
    serverSimScenarioResetRoster(sim);
    /* Drop any ping accepted but not yet buffered, so it can't leak a stale
     * marker into the next round's first running tick — and the rate limiter
     * with it, because sim->tick is rewound to 0 below and last round's tick
     * stamps would then refuse the new round's first pings. */
    serverSimResetPingState(sim);

    /* Post-game stats are round-scoped: clear the accumulator and the notable
     * timeline so an aborted or finished round never leaks into the next. This
     * runs on both round start and return-to-lobby, so a round that ends without
     * reaching game-over is cleared too. */
    memset(sim->roundStats, 0, sizeof(sim->roundStats));
    sim->notableEventCount = 0;

    /* Reset the attribution track for the new round; keep trackBuf/trackCap
     * allocated for reuse across rounds. */
    sim->trackLen = 0;
    sim->trackRecordCount = 0;
    sim->trackTruncated = false;
    memset(sim->trackIdentity, 0, sizeof(sim->trackIdentity));

    /* 7. Reset tick */
    sim->tick = 0;
    /* Re-arm the latch with it: the next round's log segment starts wherever
     * its first written tick lands, not where the last one did. */
    sim->roundLogStartTick = ROUND_LOG_START_UNSET;

    /* 8. Flush all input queues */
    for (i = 0; i < MAX_TANKS; i++) {
        sim->inputQueueHead[i] = 0;
        sim->inputQueueTail[i] = 0;
        sim->lastProcessedInput[i] = 0;
        sim->lastInputButtons[i] = 0;
        sim->lastActionAppliedTick[i] = 0;
        sim->pendingHarvestActions[i] = 0;
        sim->pendingHarvestBuildAction[i] = 0;
        sim->pendingHarvestBuildX[i] = 0;
        sim->pendingHarvestBuildY[i] = 0;
        sim->inputBufferFilled[i] = 0;
        sim->inputDryTicks[i] = 0;
        sim->jitterTarget[i] = JITTER_BUFFER_DEFAULT;
        sim->jitterStallCount[i] = 0;
        sim->jitterStarveCount[i] = 0;
        sim->jitterStableTicks[i] = 0;
        sim->statStallTicks[i] = 0;
        sim->statGapFillTicks[i] = 0;
        sim->statDroppedStaleInputs[i] = 0;
        sim->statCatchupTicks[i] = 0;
        sim->statLastRewindTicks[i] = 0;
    }

    /* 9. Reset full sync tracking */
    memset(sim->lastFullSyncTick, 0, sizeof(sim->lastFullSyncTick));

    /* Per-recipient view state — the decay proximity clocks, the last known
     * tank positions and the reported views all describe the round that just
     * ended. Zeroing viewKind puts every slot back on the tank view. */
    memset(sim->pillNearTick, 0, sizeof(sim->pillNearTick));
    memset(sim->baseNearTick, 0, sizeof(sim->baseNearTick));
    memset(sim->allyNearTick, 0, sizeof(sim->allyNearTick));
    memset(sim->lastTankMX, 0, sizeof(sim->lastTankMX));
    memset(sim->lastTankMY, 0, sizeof(sim->lastTankMY));
    memset(sim->lastTankValid, 0, sizeof(sim->lastTankValid));
    memset(sim->viewKind, 0, sizeof(sim->viewKind));
    memset(sim->viewTarget, 0, sizeof(sim->viewTarget));

    /* 10. Reset change detection */
    sim->prevPillCount = 0;
    sim->prevBaseCount = 0;
    memset(sim->lastClosestBase, BASE_NOT_FOUND, sizeof(sim->lastClosestBase));

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

    /* In-process bot ClientSims: sync their client-side matrices directly.
     * The renderer colours tanks from the followed bot's CLIENT players
     * object; when the control event above doesn't land there (startup
     * timing), allies render as red enemies (20260704_005544: server matrix
     * perfect, every follow view all-red). Remote clients still rely on the
     * CTRL_ALLIANCE_RESET published above. */
    botManagerSyncClientAlliances(sim);
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
        if (!sim->playerConnected[i]) {
            /* Disarm a slot nobody is in, rather than leaving whatever the
             * last round put there. basesUpdate treats every timer that is
             * not the off sentinel as a live restock cycle, so a slot armed
             * for a player who has since left goes on restocking every base
             * for the rest of the server's life. Skipping these was how a
             * server that once held eight players kept refuelling at eight
             * players' rate for a two-player game. */
            basesRemoveTimer(&sim->sim, (int)i);
            continue;
        }
        sim->sim.baseTimer[i] = (sim->sim.rules.base_regen_ticks * (orderIdx + 1)) / numConnected;
        orderIdx++;
    }
}

/* On game start, honour autoLockOnGameStart: if the lobby is still open to
 * new players, close it and raise the transport admin lock so the locked
 * state reaches connected clients and WinBolo.net (winboloNetSendLock).
 * Mirrors the console "lock" command, driven by the lobby setting at the
 * moment the round begins. */
static void serverSimApplyAutoLockOnGameStart(ServerSim *sim) {
    sim->savedAllowNewPlayers = sim->allowNewPlayers;
    if (sim->autoLockOnGameStart && sim->allowNewPlayers) {
        sim->allowNewPlayers = FALSE;
        transportUdpServerSetLock(sim, TRUE);
    }
}

/* Pre-compute start indices for the whole roster in one pass so the players
 * are spread across the map's start regions and rivals don't grab adjacent
 * squares (the tankCreate loops that follow run synchronously, so without a
 * batch pass each player's per-position checks would be blind to siblings
 * being created in the same loop). startsGetStart consumes the slot lazily,
 * doing scatter and direction conversion at consumption time so the
 * per-square nudge sees siblings already placed earlier in that loop. */
static void serverSimRunStartBatch(ServerSim *sim) {
    BYTE batchTeam[MAX_TANKS];
    BYTE reserved0[MAX_TANKS];
    bool fieldedNow[MAX_TANKS];
    BYTE teamSide[MAX_TANKS + 1];   /* indexed by team number; entry 0 unused */
    BYTE numStarts = startsGetNumStarts(&sim->sim.ss);
    BYTE i;
    memset(teamSide, START_SIDE_ANY, sizeof(teamSide));
    for (i = 1; i < MAX_TANKS; i++) {
        teamSide[i] = sim->teams[i].startSide;
    }
    for (i = 0; i < MAX_TANKS; i++) {
        BYTE r = sim->lobbyPlayers[i].startIdx;  /* 1-based, 0xFF = none */
        batchTeam[i] = sim->lobbyPlayers[i].teamNumber;
        reserved0[i] = (r == 0xFF || r < 1 || r > numStarts)
                     ? MAX_STARTS                  /* none / stale-after-map-change */
                     : (BYTE)(r - 1);              /* 1-based public -> 0-based engine */
        /* The batch places the round's field, not its roster: a seat
           held for a bot that is not being fielded takes no square here,
           and takes one from the incremental pick when it is fielded. */
        fieldedNow[i] = sim->playerConnected[i] &&
                        sim->lobbyPlayers[i].fielded;
    }
    startsAssignBatch(&sim->sim, &sim->sim.ss,
                      fieldedNow, batchTeam,
                      sim->sim.pendingStartIdx, reserved0, teamSide);
}

void serverSimReassignStarts(ServerSim *sim) {
    BYTE i;

    if (sim == NULL || sim->state != serverStateRunning) {
        return;
    }
    serverSimRunStartBatch(sim);

    /* Re-place every fielded tank from the fresh batch. Destroy-and-create
     * is what the game-start paths do; this is the same loop, and it is safe
     * here because the caller runs before the first tick. A seat held for a
     * bot that is not on the field has no tank and takes no square. */
    serverSimSetActive(sim);
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || !sim->lobbyPlayers[i].fielded) continue;
        if (sim->sim.tanks[i] != NULL) {
            tankDestroy(&sim->sim, &sim->sim.tanks[i]);
            sim->sim.tanks[i] = NULL;
        }
        tankCreate(&sim->sim, &sim->sim.tanks[i]);
        basesUpdateTimer(&sim->sim, i);
    }
}

/* One of the two calls a start makes into the scenario, with the setup window
 * open across it and the frame's game events put back where they stood.
 *
 * The window is what lets the funnel take the ops the call issues, and it
 * holds the set-rule handler's own publish: the start states the whole table
 * once at its end, so a script setting a dozen rules costs one control event
 * rather than a dozen.
 *
 * The event buffer is what the opening snapshot carries to every client, and
 * a client draws its newswire line and counts its scoreboard from what it
 * finds there. What a scenario arranges is the world the round begins in
 * rather than something that happened in it, so the buffer is put back where
 * it stood and the arrangement rides the snapshot's own tank, base and pill
 * lists: sixteen bases dealt at setup arrive as sixteen owners with no run of
 * captures in front of them. Only this call's own events go — the count is
 * taken first, so anything the start itself raised is left where it is. The
 * map events are left alone too: a terrain edit writes no newswire line and
 * has no other way of reaching a client. And the host keeps what it heard,
 * because a subscriber is handed an event as it is raised rather than out of
 * this buffer, so the round's own hooks still see what its setup did. */
static void serverSimScenarioStartCall(ServerSim *sim, void (*call)(void *ctx),
                                       void *ctx) {
    uint8_t eventsBefore;

    if (call == NULL) {
        return;
    }
    eventsBefore             = sim->eventCount;
    sim->scenarioSetupWindow = true;
    /* Terrain written from here is recorded the way a running frame records
       it. Only a running tick installs the map-change callback, and a start
       is not one — it is reached from the countdown branch, which returns
       before the install, and from a command handler, which runs outside a
       tick altogether — so without this every square the call writes reaches
       the server's own map and no client's. simMapChangeCallback records into
       the active sim, so that is pointed at this one first, as the tick does
       at its top. Put back to none rather than to what was there: both
       installs are inside a running tick and neither reaches a start, so
       there is never anything here to restore. */
    serverSimSetActive(sim);
    mapSetChangeCallback(simMapChangeCallback);
    call(ctx);
    mapSetChangeCallback(NULL);
    sim->scenarioSetupWindow = false;
    sim->eventCount          = eventsBefore;
}

/* The two per-seat holders a scenario writes into, back to the values a fresh
 * sim has. Each is written by the roster drain and read once, by the seat it
 * names, on that seat's next placement or spawn — so a value written for a
 * seat that never took it would otherwise be spent on the opening placement of
 * the round after. MAX_STARTS is the start holder's "none": 0 is a start. */
static void serverSimResetScenarioSeats(ServerSim *sim) {
    BYTE i;

    for (i = 0; i < MAX_TANKS; i++) {
        sim->sim.scenarioStartIdx[i]     = MAX_STARTS;
        sim->sim.scenarioSpawnLoadout[i] = 0;
    }
}

void serverSimStartGameInPlace(ServerSim *sim) {
    BYTE i;

    /* Every round opens on the classic table. A scenario writes its own over
     * this at the boot call below, so a scripted round plays by its script
     * either way; what this settles is the round that has no script to write
     * one — the round after a scripted one, and a round whose scenario failed
     * to boot — which would otherwise play by the last script's numbers. */
    simRulesClassic(&sim->sim.rules);
    serverSimResetScenarioSeats(sim);

    /* Held for the whole start so the all-ready detector refuses to run
     * while the roster and the state are being rebuilt. */
    sim->startInProgress = true;

    /* Fresh round — the last-human-left return-to-lobby check arms only
     * once a human is seen this round. */
    sim->roundHadHuman = false;

    /* The tick loop's peak and its over-budget count reset here for the same
     * reason the per-bot ones do in botManagerOnGameStart: they describe the
     * current game, not an accumulation across map rotations. Both of the
     * authoritative starts do it — serverSimStartGame has the twin. */
    serverLifecycleResetTickPeak();

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
    /* A fill queued by a lobby hook is lobby work too, and this path does not
       reload the map, so nothing else would drop it: without this the squares
       it still owes land partway into the round that is starting. The
       full-reset path clears this in serverSimResetGameWorld. */
    serverSimScenarioResetFill(sim);
    serverSimScenarioResetRoster(sim);
    serverSimResetPingState(sim);

    /* Drop the lobby-chat catch-up buffer: this is an authoritative game start
     * just like serverSimStartGame's countdown->running path, and the just-ended
     * lobby's chat must not leak into the next session a returning spectator
     * catches up on. The full-reset path clears this via serverSimStartGame; this
     * in-place path bypasses that reset, so it must clear the buffer itself —
     * otherwise pre-game chat survives in lobbyChatBuffer and the drain-flip
     * replay (serverSendSpectatorBacklog) re-delivers it on return. A spectator
     * connects over the wire while the only player sits directly in the sim, so
     * transportUdpServerHasAnyClient() is false and the ready check routes here. */
    sim->lobbyChatCount = 0;

    /* Wire any bots in the roster into the running game (idempotent on
     * a fresh sim with zero bots). */
    botManagerOnGameStart(sim);

    /* Apply team alliances: players with same non-zero teamNumber become allies */
    serverSimReapplyTeamAlliances(sim);

    /* The round's own Lua state, its chunk and its rules, before a start is
     * picked or a tank is built: the seats fielded at the start are placed
     * and armed by the policies of the round being started rather than by
     * whatever state the host was still holding from the round before. */
    serverSimScenarioStartCall(sim, sim->scenarioRoundBoot,
                               sim->scenarioRoundBootCtx);

    /* Pre-compute start indices for the whole batch so teammates land
     * near each other and a team with a side keeps its side (see
     * serverSimRunStartBatch, and serverSimStartGame for the rationale). */
    serverSimRunStartBatch(sim);

    /* Destroy every connected slot's tank and man before creating any, so
     * a new tank's spawn search never sees the previous round's tanks
     * still sitting at their old positions. */
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
    }

    /* Create tanks for every seat that is being fielded — a seat held for a
       bot the round is not starting with is skipped, as in serverSimStartGame. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || !sim->lobbyPlayers[i].fielded) continue;
        tankCreate(&sim->sim, &sim->sim.tanks[i]);
        sim->sim.lgmen[i] = lgmCreate(i);
    }
    serverSimStaggerBaseTimers(sim);

    sim->state = serverStateRunning;

    serverSimApplyAutoLockOnGameStart(sim);

    /* Reset per-client reliable-event queues BEFORE publishing the
     * RUNNING phase, so CTRL_GAME_PHASE_RUNNING enters every queue at
     * seq=1 as the first event of the new game.  Skipping this pair
     * leaves stale seqs on any remote-client queue and the per-client
     * control event ack reset on the client side then desyncs against
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

    /* The round-start callback, at the point where the world, the tanks
     * and the roster are built and the state already reads running. The
     * setup window is open across it, so the funnel takes the ops it
     * issues although the start has not finished; the publish below then
     * carries whatever rules those ops set. */
    serverSimScenarioStartCall(sim, sim->scenarioRoundStart,
                               sim->scenarioRoundStartCtx);

    /* Each slot's copy of the terrain takes what the boot and the setup
     * wrote. This path is not reached from a tick, so the end-of-frame pass
     * that does this for a running frame never sees those squares, and a copy
     * left behind is what the map handed to anyone downloading it from here
     * on is compressed from. */
    serverSimShadowTick(sim);

    /* And the table the round is starting on, beside the phase. The twin of
     * the publish at the end of serverSimStartGame: this path does not run
     * that function, so it states the table itself. Between the two of them
     * every start path states it exactly once. */
    serverSimPublishSimRules(sim);

    sim->startInProgress = false;
}

void serverSimStartGame(ServerSim *sim) {
    BYTE i;
    /* Save connected-player state before resetting – resetGameWorld clears
       playerConnected[], but we need it to create tanks below. Player
       identity (name, country, clientType, clientFlags) is preserved across
       the reset by serverSimResetGameWorld, so it no longer needs saving. */
    bool savedConnected[MAX_TANKS];

    /* The classic table, as in serverSimStartGameInPlace and for the same
     * reason. Before serverSimResetGameWorld below as well as before the
     * boot call: the reload in there caps the map's pills and bases against
     * whatever table the sim is on, and the map a round is starting on is
     * capped by that round's rules and not by the previous round's. */
    simRulesClassic(&sim->sim.rules);
    serverSimResetScenarioSeats(sim);

    /* Held for the whole start, as in serverSimStartGameInPlace. */
    sim->startInProgress = true;

    for (i = 0; i < MAX_TANKS; i++) {
        savedConnected[i] = sim->playerConnected[i];
    }

    serverSimSetActive(sim);

    /* Fresh round — drop any vote state from the previous game. */
    serverSimGameVoteResetAll(sim);

    /* Drop the lobby-chat catch-up buffer: it holds only the just-ended
     * lobby's chat, which must not leak into the next session a returning
     * spectator catches up on. This countdown->running transition is one of the
     * two authoritative game starts (serverSimStartGameInPlace, the no-countdown
     * SP/no-wire-client path, clears it too); the game-over->lobby reset path
     * (serverSimResetGameWorld's other caller) deliberately keeps the buffer
     * so post-game lobby chat survives for the drain-flip replay. */
    sim->lobbyChatCount = 0;

    /* Arms the last-human-left return-to-lobby check from a clean slate. */
    sim->roundHadHuman = false;

    /* The twin of the reset in serverSimStartGameInPlace: the round's worst
     * tick and its over-budget count start empty here too. */
    serverLifecycleResetTickPeak();

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
            /* The round reset clears every alliance; no player asked for it,
               so the policy is asked with no actor. */
            leaveEvt.u.allianceLeave.quiet =
                serverSimAnnounce(sim, ANNOUNCE_KIND_ALLIANCE, i, NEUTRAL)
                    ? 0 : 1;
            serverSimPublishControl(sim, &leaveEvt);
        }
    }

    /* Apply team alliances: players with same non-zero teamNumber become allies */
    serverSimReapplyTeamAlliances(sim);

    /* The round's own Lua state, its chunk and its rules, before a start is
     * picked or a tank is built — as in serverSimStartGameInPlace. */
    serverSimScenarioStartCall(sim, sim->scenarioRoundBoot,
                               sim->scenarioRoundBootCtx);

    /* Pre-compute start indices for the whole batch so teammates land
     * near each other, rivals don't grab adjacent squares, and a team with
     * a side keeps its side (the tankCreate loop below runs synchronously,
     * so without a batch pass each player's per-position checks would be
     * blind to siblings being created in the same loop). startsGetStart
     * consumes the slot lazily, doing scatter and direction conversion at
     * consumption time so the per-square nudge sees siblings already placed
     * earlier in this loop. See serverSimRunStartBatch. */
    serverSimRunStartBatch(sim);

    /* Create tanks for every seat that is being fielded. A seat held for a
       bot the round is not starting with is skipped: it stays in the roster
       with no tank until something fields it. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || !sim->lobbyPlayers[i].fielded) continue;
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
    serverSimApplyAutoLockOnGameStart(sim);
    serverSimConsoleMessage("Game started!");

    /* The round-start callback, as in serverSimStartGameInPlace: the world,
     * the tanks and the roster are built and the state already reads
     * running, and the setup window is open across the call so the funnel
     * takes the ops it issues. The publish below carries whatever rules
     * those ops set. */
    serverSimScenarioStartCall(sim, sim->scenarioRoundStart,
                               sim->scenarioRoundStartCtx);

    /* The table the round is starting on, stated here rather than by each
     * caller. This and serverSimStartGameInPlace are the two authoritative
     * starts — every start path in the server runs one of them and neither
     * runs the other — so a publish in each is a publish exactly once per
     * round however the round was started: the countdown expiring, a lobby
     * skipped at boot, an empty server resetting, a map rotation, the
     * console's start command or the headless fast path. The callers used to
     * do it, and only two of them did, which left the rest of those paths
     * starting a round nobody was told the numbers for. */
    serverSimPublishSimRules(sim);

    sim->startInProgress = false;

    /* A snapshot will be written on the first running tick
     * (tick 0 % FULL_SYNC_INTERVAL == 0). */
}

void serverSimMapRotateRound(ServerSim *sim) {
    /* No-lobby map-rotation round restart. The lifecycle has already booted
     * every client, so unlike serverSimReturnToLobby there is no player
     * save/restore or alliance carry-forward — the next round starts empty.
     * The reusable pieces are the same: pick the next map and reset the
     * world, then start a fresh running round straight away (no lobby wait).
     *
     * Pairs with serverLifecycleRotateRound, which does the surrounding
     * disconnect-all, WBN session rotation and round-log upload. The split
     * mirrors serverSimReturnToLobby / its lifecycle caller: the WBN dance
     * and transport teardown are lifecycle concerns; this is the sim core. */

    /* Open the WBN session-rotation window before touching the map. The
     * map pick below renames the map under the still-live old server_key;
     * serverSimWbnLobbyUpdate holds any such change dirty while this is set.
     * The lifecycle clears it after winbolonetBeginSession installs the new
     * round's key — exactly as serverSimReturnToLobby relies on. */
    sim->wbnSessionRotating = TRUE;

    /* Drop vote / map-skip state scoped to the round we're leaving. */
    serverSimGameVoteResetAll(sim);
    serverSimMapSkipVotesReset(sim);

    /* And every parked runner, for the same reason and with the same rule as
       the gameOver->lobby end: a parked brain carries its state table, and
       the round it remembers is the one being left. This is the end of that
       round — the start below is the next one's. */
    botManagerReleaseParkedRunners(sim);

    /* serverSimChangeMap (reached via serverSimMapDirPickRandom) only runs
     * in lobby state, so drop into it for the pick. serverSimStartGame
     * below moves us back to running. Both existing rotation sites do the
     * same transient lobby hop (the empty-reset path explicitly, the
     * gameOver path via serverSimReturnToLobby). */
    sim->state = serverStateLobby;
    if (sim->mapDirFiles != NULL) {
        serverSimMapDirPickRandom(sim);
    }

    /* Full world reset + tank (re)creation + state -> running. With no
     * players connected this just reloads the freshly-picked map's cached
     * data and starts an empty round waiting for joiners. */
    serverSimStartGame(sim);

    sim->gameLength = sim->originalGameLength;
    sim->emptyResetTicks = -1;

    /* serverSimStartGame latches hadPlayersEver = TRUE, which would make the
     * lifecycle's empty-server check fire on the very next tick (the round
     * starts with zero players) and rotate again immediately. Re-arm it so
     * the empty trigger can only fire once someone joins and then leaves. */
    sim->hadPlayersEver = FALSE;

    serverSimConsoleMessage("Map rotation: started new round.");
}

bool serverSimChangeMap(ServerSim *sim, char *mapFileName) {
    BYTE tempBuf[MAP_COMPRESSED_MAX_SIZE];
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

    /* The map is this sim's now: cap what it brought against the rules. */
    mapClampToRules(&sim->sim);

    /* Hash the canonical BMAPBOLO file so WBN can match it. */
    serverSimCacheMapMd5FromFile(sim, mapFileName);

    /* And keep the file itself: a scenario is discovered beside its .map and
       the display name cannot find it again. The rotation reaches the map
       change through here. */
    SDL_strlcpy(sim->mapFilePath, mapFileName, sizeof(sim->mapFilePath));

    basesClearMines(&sim->sim);

    /* A different map is installed — restart every slot's copy of the terrain
     * from it, so a lobby client's snapshot checksum is taken against the map
     * the lobby now holds. A fill still owing squares was aimed at the map
     * that has just gone, so it goes with it. */
    serverSimShadowSeedAll(sim);
    serverSimScenarioResetFill(sim);

    /* Update cached map data */
    len = serverSimGetCompressedMap(sim, tempBuf, (int)sizeof(tempBuf));
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
    BYTE first = serverSimWinningOwner(sim);

    if (first == NEUTRAL) {
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

/* Advance a write cursor by an snprintf result, clamped so it never runs
 * past the buffer. snprintf returns the length it WOULD have written, which
 * can exceed the space left on truncation; added to pos unclamped that
 * overshoots bufSize and the next (bufSize - pos) underflows. Callers must
 * pass pos < bufSize. */
static size_t winMsgAdvance(size_t pos, size_t bufSize, int written) {
    if (written < 0) return pos;
    if ((size_t)written >= bufSize - pos) return bufSize - 1;
    return pos + (size_t)written;
}

bool serverSimBuildWinMessage(ServerSim *sim, char *buf, size_t bufSize) {
    BYTE count;
    BYTE first = serverSimWinningOwner(sim);
    char name[256];
    size_t pos;

    if (first == NEUTRAL) {
        snprintf(buf, bufSize, "Game over!");
        return FALSE;
    }

    /* Build winner message */
    pos = 0;
    pos = winMsgAdvance(pos, bufSize,
                        snprintf(buf + pos, bufSize - pos, "Game Won! Winners:"));
    for (count = 0; count < MAX_TANKS && pos < bufSize - 1; count++) {
        if (!sim->playerConnected[count]) continue;
        if (playersIsAllie(&sim->sim.plyrs, count, first) || count == first) {
            playersGetPlayerName(&sim->sim.plyrs, count, name, sizeof(name), TRUE);
            pos = winMsgAdvance(pos, bufSize,
                                snprintf(buf + pos, bufSize - pos, " %s", name));
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
    pos = winMsgAdvance(pos, bufSize,
                        snprintf(buf + pos, bufSize - pos,
                                 "*** Team %s has surrendered. ***", tname));

    /* The opposing side wins; list them on a second line. If nobody from
     * the winning side is still connected, the surrender line stands alone. */
    for (count = 0; count < MAX_TANKS && pos < bufSize - 1; count++) {
        uint8_t t;
        if (!sim->playerConnected[count]) continue;
        t = sim->lobbyPlayers[count].teamNumber;
        if (t == 0 || t == surrenderTeam) continue;
        if (!any) {
            pos = winMsgAdvance(pos, bufSize,
                                snprintf(buf + pos, bufSize - pos,
                                         "\nGame Won! Winners:"));
            any = TRUE;
        }
        playersGetPlayerName(&sim->sim.plyrs, count, name, sizeof(name), TRUE);
        pos = winMsgAdvance(pos, bufSize,
                            snprintf(buf + pos, bufSize - pos, " %s", name));
    }

    return TRUE;
}

void serverSimResolveGameOver(ServerSim *sim) {
    /* Game-outcome policy at the running->gameOver transition: decide the
     * win/exit message and WBN crediting. Kept here in the sim core (not in
     * the dedicated-server lifecycle tick) so every host that resolves a
     * game over — dedicated server and in-process SP/host alike — runs the
     * identical branch. Lobby-less sims (e.g. background game) have no lobby
     * to return a message to. */
    if (!sim->lobbyEnabled) return;

    switch (sim->returnToLobbyReason) {
    case RETURN_REASON_SURRENDER:
        /* A surrender vote ended the round — the opposing team wins. The
         * base sweep never fires on a surrender, so credit the win by
         * team instead. */
        serverSimBuildSurrenderWinMessage(sim, sim->returnToLobbyTeamId,
                                          sim->pendingWinMessage,
                                          sizeof(sim->pendingWinMessage));
        serverSimSendWbnSurrenderWinEvents(sim, sim->returnToLobbyTeamId);
        break;

    case RETURN_REASON_MANUAL_VOTE:
        /* A manual back-to-lobby vote ended the round — no winner, but
         * leave a line in the returning lobby explaining why (the
         * in-game countdown only reached the players still in the game). */
        SDL_strlcpy(sim->pendingWinMessage,
                    "*** Players voted to return to the lobby. ***",
                    sizeof(sim->pendingWinMessage));
        break;

    case RETURN_REASON_ABANDONED:
        /* Everyone left — nobody won, so no lobby line and no WBN
         * crediting. */
        sim->pendingWinMessage[0] = '\0';
        break;

    case RETURN_REASON_SCENARIO:
        /* A scenario op ended the round and wrote the returning lobby's line
         * as it did. The message stands exactly as the op left it, and
         * nobody is credited: the round ended because a script said so,
         * whatever the map looked like at the end. */
        break;

    case RETURN_REASON_BASE_WIN:
    case RETURN_REASON_NONE:
    default:
        /* An all-bases sweep, or a game-over with no countdown behind it
         * (game-time / tick limits): both report whatever the sweep says.
         * Capture the win message now while game state is intact; it is
         * sent once the players are back in the lobby. */
        serverSimBuildWinMessage(sim,
                                 sim->pendingWinMessage,
                                 sizeof(sim->pendingWinMessage));
        serverSimSendWbnWinEvents(sim);
        break;
    }
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
