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

/* Bus subscriber that maintains the dedicated-server replay log in
 * response to CTRL_GAME_PHASE_* events. Registered from servermain.c
 * when the --log argument is present. The subscriber's sync-replay
 * delivers the current phase at registration time, so the log file is
 * opened immediately for both lobby (CTRL_GAME_PHASE_LOBBY) and
 * no-lobby (CTRL_GAME_PHASE_RUNNING) startup paths. */

#include <string.h>
#include <stdio.h>

#include "global.h"
#include "log.h"
#include "messages.h"
#include "players.h"
#include "transport_udp.h"
#include "server_sim_internal.h"
#include "control_event.h"
#include "../winbolonet/winbolonet_core.h"
#include "../winbolonet/winbolonet_server.h"
#include "../winbolonet/http.h"

#include "server_dedicated_log.h"

extern bool isLogging;
extern bool dontSendLog;
extern char fileName[];

/* Single dedicated-server log subscriber per process. The bus forbids
 * subscribers whose ctx == sim, so the deliver callback reaches the sim
 * through this file-static pointer instead. */
static ServerSim *s_logSim = NULL;

void makeLogFileName(char *outFileName, const char *mapName);

static void handleGameOver(ServerSim *sim) {
    (void)sim;
    if (!isLogging) {
        return;
    }
    logStop();
    isLogging = FALSE;

    if (!dontSendLog && winbolonetIsRunning()) {
        char key[WINBOLONET_KEY_LEN];
        winboloNetGetServerKey(key);
        if (key[0] != '\0') {
            httpCreate();
            httpSendLogFile(fileName, key, FALSE);
            httpDestroy();
        }
    }
}

static void handleLobbyEnter(ServerSim *sim) {
    BYTE i;

    if (!sim->wantLogging) {
        return;
    }

    if (sim->userLogFileName[0] != '\0') {
        strncpy(fileName, sim->userLogFileName, 512 - 1);
    } else {
        makeLogFileName(fileName, sim->mapName);
    }
    {
        size_t flen = strlen(fileName);
        if (flen <= 4 || strcmp(fileName + flen - 4, ".wbv") != 0) {
            strncat(fileName, ".wbv", 512 - flen - 1);
        }
    }
    isLogging = logStart(fileName, sim,
                         0, MAX_TANKS, sim->hasPassword);
    if (isLogging) {
        logAddEvent(log_LobbyEnter, 0, 0, 0, 0, 0, NULL);
        for (i = 0; i < MAX_TANKS; i++) {
            if (sim->playerConnected[i]) {
                const char *name = transportUdpServerGetPlayerName(i);
                if (name != NULL) {
                    char pstr[256];
                    int nameLen = (int)strlen(name);
                    BYTE accountFlags = playersGetAccountFlags(&sim->sim.plyrs, (BYTE)i);
                    if (nameLen > 255) nameLen = 255;
                    pstr[0] = (char)nameLen;
                    memcpy(pstr + 1, name, nameLen);
                    logAddEvent(log_PlayerJoined, i, '?', '?', accountFlags, 0, pstr);
                }
            }
        }
        fprintf(stderr, "Logging to %s (lobby)\n", fileName);
    }
}

static void handleLobbyMapChange(ServerSim *sim) {
    char pstr[256];
    int nameLen;

    if (!sim->wantLogging || !logIsRecording()) {
        return;
    }

    /* Record the chosen map name as a server message so the viewer's
     * chat timeline shows what the host previewed in the lobby. The
     * authoritative snapshot is rewritten when the countdown ends
     * (handleGameStart); these messages are just a cheap audit trail
     * that doesn't churn the heavy pills/bases/starts/RLE state. */
    nameLen = (int)snprintf(pstr + 1, sizeof(pstr) - 1,
                            "Map changed to %s", sim->mapName);
    if (nameLen < 0) return;
    if (nameLen > 255) nameLen = 255;
    pstr[0] = (char)nameLen;
    logAddEvent(log_MessageServer, 0, 0, 0, 0, 0, pstr);
}

static void handleGameStart(ServerSim *sim) {
    if (!sim->wantLogging) {
        return;
    }

    if (logIsRecording()) {
        logAddEvent(log_LobbyExit, 0, 0, 0, 0, 0, NULL);
        /* Rewrite the world snapshot with whatever map the lobby
         * settled on. The original snapshot from handleLobbyEnter
         * froze the map at lobby-entry time, so any in-lobby map
         * swap would otherwise leave the viewer playing the round
         * against the wrong terrain/pills/bases. */
        logWriteSnapshot(sim, TRUE);
        return;
    }

    /* No-lobby case — start the log on the running transition. */
    if (sim->userLogFileName[0] != '\0') {
        strncpy(fileName, sim->userLogFileName, 512 - 1);
    } else {
        makeLogFileName(fileName, sim->mapName);
    }
    {
        size_t flen = strlen(fileName);
        if (flen <= 4 || strcmp(fileName + flen - 4, ".wbv") != 0) {
            strncat(fileName, ".wbv", 512 - flen - 1);
        }
    }
    isLogging = logStart(fileName, sim,
                         0, MAX_TANKS, sim->hasPassword);
    if (isLogging) {
        fprintf(stderr, "Logging to %s\n", fileName);
    }
}

static void serverDedicatedLogDeliver(void *ctx, const ControlEvent *evt) {
    ServerSim *sim = s_logSim;
    (void)ctx;
    if (sim == NULL) {
        return;
    }
    switch (evt->type) {
        case CTRL_GAME_PHASE_LOBBY:
            handleLobbyEnter(sim);
            break;
        case CTRL_GAME_PHASE_RUNNING:
            handleGameStart(sim);
            break;
        case CTRL_GAME_PHASE_GAME_OVER:
            handleGameOver(sim);
            break;
        case CTRL_LOBBY_MAP_CHANGE:
            handleLobbyMapChange(sim);
            break;
        default:
            break;
    }
}

void serverDedicatedLogInstall(ServerSim *sim) {
    if (sim == NULL) {
        return;
    }
    s_logSim = sim;
    serverSimRegisterSubscriber(sim, serverDedicatedLogDeliver, NULL);
}
