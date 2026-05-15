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

/* Real implementations of the server-side log-recording hooks for the
 * WinBoloDS dedicated-server binary. servermain.c owns the global log
 * state these bodies read and mutate. */

#include <string.h>
#include <stdio.h>

#include "global.h"
#include "log.h"
#include "messages.h"
#include "players.h"
#include "transport_udp.h"
#include "server_sim_internal.h"
#include "../winbolonet/winbolonet.h"
#include "../winbolonet/http.h"

#include "server_dedicated_log.h"

extern bool isLogging;
extern bool dontSendLog;
extern char fileName[];

void makeLogFileName(char *outFileName, const char *mapName);

void serverDedicatedLogOnEnterGameOver(ServerSim *sim) {
    (void)sim;
    if (isLogging) {
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
}

void serverDedicatedLogOnReturnToLobby(ServerSim *sim) {
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

void serverDedicatedLogOnLobbyExit(ServerSim *sim) {
    if (!sim->wantLogging) {
        return;
    }

    if (logIsRecording()) {
        logAddEvent(log_LobbyExit, 0, 0, 0, 0, 0, NULL);
        return;
    }

    /* Logging was requested but not yet started — start now */
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
