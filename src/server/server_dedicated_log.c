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

/* Bus subscriber that maintains the dedicated-server replay log in
 * response to CTRL_GAME_PHASE_* events. Registered from servermain.c
 * when the --log argument is present. The subscriber's sync-replay
 * delivers the current phase at registration time, so the log file is
 * opened immediately for both lobby (CTRL_GAME_PHASE_LOBBY) and
 * no-lobby (CTRL_GAME_PHASE_RUNNING) startup paths. */

#include <string.h>
#include <stdio.h>
#include <time.h>   /* serverDedicatedLogMakeAutoName timestamp */

#include <SDL3/SDL.h>   /* SDL_RenamePath for the end-of-round map rename */

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

#include "server_lifecycle.h"
#include "server_dedicated_log.h"

/* Module-private log state. Formerly extern globals owned by
 * servermain.c (fileName/isLogging/dontSendLog); made private in Phase 2a
 * so the module is self-contained and linkable outside WinBoloDS. The GUI
 * client defines its own unrelated fileName[], so these must NOT be
 * externs. servermain reads s_isLogging/s_logFileName back through
 * serverDedicatedLogIsActive()/serverDedicatedLogCurrentFile() for its
 * teardown upload; s_dontSendLog arrives via serverDedicatedLogInstall(). */
static char s_logFileName[512];
static bool s_isLogging = FALSE;
static bool s_dontSendLog = FALSE;

/* Single dedicated-server log subscriber per process. The bus forbids
 * subscribers whose ctx == sim, so the deliver callback reaches the sim
 * through this file-static pointer instead. */
static ServerSim *s_logSim = NULL;

/* Round-end stash. handleGameOver finalizes the log and copies the
 * filename here; the lobby/empty-reset cleanup site uploads it after
 * WBN server/quit and before server/register. Empty string == nothing
 * pending. */
static char s_pendingUploadFile[512];

/* Timestamp prefix ("YYYYMMDDtHHMMSS") of the current log's auto-generated
 * name, captured when the file is first named. The map suffix can go stale
 * if the host changes map in the lobby after the file is opened, so the
 * end-of-round rename rebuilds the name from this prefix plus the played
 * map — preserving the recording-start time rather than the game-end time.
 * Empty when the log was named from an explicit -log <file> path. */
static char s_logStamp[16];

static void serverDedicatedLogRenameForMap(ServerSim *sim);

/* Generate a log file name from the current time and map name into
 * outFileName ("YYYYMMDDtHHMMSS_<map>", spaces → underscores). Moved from
 * servermain.c in Phase 2a so the module owns its own auto-naming. */
static void serverDedicatedLogMakeAutoName(char *outFileName, const char *mapName) {
    time_t t;
    struct tm *tmt;
    int count = 0;
    int len;

    time(&t);
    tmt = localtime(&t);

    sprintf(outFileName, "%04d%02d%02dt%02d%02d%02d_%s", (1900 + tmt->tm_year), (1 + tmt->tm_mon), tmt->tm_mday, tmt->tm_hour, tmt->tm_min, tmt->tm_sec, mapName);
    len = (int) strlen(outFileName);
    /* Replace spaces with underscores */
    while (count < len){
        if (outFileName[count] == ' ') {
            outFileName[count] = '_';
        }
        count++;
    }
}

void serverDedicatedLogStashCurrentRound(void) {
    if (!s_isLogging) {
        return;
    }
    logStop();
    s_isLogging = FALSE;
    /* File handle is now closed — safe to rename it to the map that was
     * actually played (the lobby-entry name can be stale if the host
     * switched maps before the countdown). */
    serverDedicatedLogRenameForMap(s_logSim);
    if (!s_dontSendLog) {
        strncpy(s_pendingUploadFile, s_logFileName, sizeof(s_pendingUploadFile) - 1);
        s_pendingUploadFile[sizeof(s_pendingUploadFile) - 1] = '\0';
    } else {
        s_pendingUploadFile[0] = '\0';
    }
}

void serverDedicatedLogFlushPendingUpload(void) {
    if (s_pendingUploadFile[0] == '\0') {
        return;
    }
    if (winbolonetIsRunning()) {
        char key[WINBOLONET_KEY_LEN];
        winboloNetGetServerKey(key);
        if (key[0] != '\0') {
            httpSendLogFile(s_pendingUploadFile, key, FALSE);
        }
    }
    s_pendingUploadFile[0] = '\0';
}

static void handleGameOver(ServerSim *sim) {
    /* No-lobby (-quitonwin): server is about to shut down via
     * servermain.c, which sends server/quit (winbolonetDestroy →
     * winbolonetGoodbye) before its own logStop + httpSendLogFile.
     * That path already has the correct ordering, so leave s_isLogging
     * and s_logFileName intact for it — servermain reads them back via
     * serverDedicatedLogIsActive()/serverDedicatedLogCurrentFile(). */
    if (!sim->lobbyEnabled) {
        return;
    }
    serverDedicatedLogStashCurrentRound();
}

/* Resolve the on-disk replay path into the module's `s_logFileName`,
 * generating the timestamped auto-name from the current map and delegating
 * the directory / file / extension handling to serverDedicatedLogComposePath
 * (defined in server_dedicated_log_path.c). */
static void serverDedicatedLogResolveFileName(ServerSim *sim) {
    char autoBase[512];
    const char *sep;
    serverDedicatedLogMakeAutoName(autoBase, sim->mapName);
    /* Remember the timestamp prefix so serverDedicatedLogRenameForMap can
     * rebuild the name later without shifting the time. The auto-name emits
     * "<stamp>_<map>"; the stamp is pure digits + 't', so the first '_' is the
     * separator. */
    sep = strchr(autoBase, '_');
    if (sep != NULL && (size_t)(sep - autoBase) < sizeof(s_logStamp)) {
        size_t stampLen = (size_t)(sep - autoBase);
        memcpy(s_logStamp, autoBase, stampLen);
        s_logStamp[stampLen] = '\0';
    } else {
        s_logStamp[0] = '\0';
    }
    serverDedicatedLogComposePath(sim->userLogFileName, autoBase, s_logFileName, 512);
}

/* Rebuild the closed log's on-disk name from the captured timestamp and the
 * map that was actually played, then rename the file so an in-lobby map change
 * is reflected before upload. A no-op when the timestamp wasn't captured, when
 * the name is unchanged (including any explicit -log <file>, whose recomposed
 * path matches verbatim), or when the rename fails. Call only after logStop(),
 * so no write handle is open when the rename runs. */
static void serverDedicatedLogRenameForMap(ServerSim *sim) {
    char autoBase[512];
    char newFileName[512];
    char *p;

    if (sim == NULL || s_logStamp[0] == '\0') {
        return;
    }
    snprintf(autoBase, sizeof(autoBase), "%s_%s", s_logStamp, sim->mapName);
    for (p = autoBase; *p != '\0'; p++) {
        if (*p == ' ') {
            *p = '_';
        }
    }
    serverDedicatedLogComposePath(sim->userLogFileName, autoBase,
                                  newFileName, sizeof(newFileName));
    if (strcmp(newFileName, s_logFileName) == 0) {
        return;
    }
    if (SDL_RenamePath(s_logFileName, newFileName)) {
        strncpy(s_logFileName, newFileName, 512 - 1);
        s_logFileName[512 - 1] = '\0';
        fprintf(stderr, "Renamed log to %s (played map)\n", s_logFileName);
    }
}

static void handleLobbyEnter(ServerSim *sim) {
    BYTE i;

    if (!sim->wantLogging) {
        return;
    }

    serverDedicatedLogResolveFileName(sim);
    /* Flip lobby mode on BEFORE logStart so its opening snapshot is
     * the empty-world variant (no pills/bases/starts, deep-sea map,
     * no tanks). handleGameStart clears the flag and rewrites a
     * snapshot of the real world when the countdown ends. */
    logSetLobbyMode(TRUE);
    s_isLogging = logStart(s_logFileName, sim,
                           0, MAX_TANKS, sim->hasPassword);
    if (s_isLogging) {
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
        fprintf(stderr, "Logging to %s (lobby)\n", s_logFileName);
    } else {
        /* logStart failed — drop the flag so a later no-lobby
         * handleGameStart logStart isn't poisoned. */
        logSetLobbyMode(FALSE);
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
        BYTE i, j;
        /* Leave lobby mode BEFORE log_LobbyExit so the marker, the
         * alliance audit events, and the rewriting snapshot all land
         * in the running segment under normal writer semantics. */
        logSetLobbyMode(FALSE);
        logAddEvent(log_LobbyExit, 0, 0, 0, 0, 0, NULL);
        /* Team-derived alliances from serverSimReapplyTeamAlliances are
         * applied silently — playersAcceptAlliance writes the bitmap but
         * doesn't emit log events the way the in-game /accept path does
         * (server_sim.c:4744). Walk the connected-player pairs here and
         * emit log_AllyAccept for each ally so the viewer's newswire
         * matches what the snapshot is about to encode. */
        for (i = 0; i < MAX_TANKS; i++) {
            if (!sim->playerConnected[i]) continue;
            for (j = (BYTE)(i + 1); j < MAX_TANKS; j++) {
                if (!sim->playerConnected[j]) continue;
                if (playersIsAllie(&sim->sim.plyrs, i, j)) {
                    logAddEvent(log_AllyAccept, i, j, 0, 0, 0, NULL);
                }
            }
        }
        /* Rewrite the world snapshot with whatever map the lobby
         * settled on. The original snapshot from handleLobbyEnter
         * was the empty-world lobby variant; this is the first real
         * pills/bases/starts/RLE state the viewer sees. */
        logWriteSnapshot(sim, TRUE);
        return;
    }

    /* No-lobby case — start the log on the running transition. */
    serverDedicatedLogResolveFileName(sim);
    s_isLogging = logStart(s_logFileName, sim,
                           0, MAX_TANKS, sim->hasPassword);
    if (s_isLogging) {
        fprintf(stderr, "Logging to %s\n", s_logFileName);
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

void serverDedicatedLogInstall(ServerSim *sim, bool dontSendLog) {
    if (sim == NULL) {
        return;
    }
    s_dontSendLog = dontSendLog;
    s_logSim = sim;
    serverSimRegisterSubscriber(sim, serverDedicatedLogDeliver, NULL);
    /* Hand the lifecycle our stash/flush so its lobby/empty-reset
     * cleanup can drive the per-round upload. */
    serverLifecycleSetRoundLogHooks(serverDedicatedLogStashCurrentRound,
                                    serverDedicatedLogFlushPendingUpload);
}

bool serverDedicatedLogIsActive(void) {
    return s_isLogging;
}

const char *serverDedicatedLogCurrentFile(void) {
    return s_logFileName;
}
