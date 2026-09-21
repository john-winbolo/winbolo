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
 * delivers the current phase at registration time: the no-lobby startup
 * path (CTRL_GAME_PHASE_RUNNING) opens the log file there and then, while
 * the lobby path (CTRL_GAME_PHASE_LOBBY) only arms the drain, so that log
 * opens on the first log tick instead. */

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
#include "../winbolonet/winbolonetthread.h"

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

/* Publish target for a finished round, for callers that record to a reused
 * filename. Single player records to one fixed path and would otherwise lose
 * the round to the next lobby entry's logStart, which truncates that same
 * path three seconds after game over. "" disables the move, which is what
 * hosting uses — its rounds already resolve unique timestamped names. */
static char s_completedPath[512];

/* Where the last completed round's log ended up, for callers that want to
 * offer it back. "" until a round has finished. */
static char s_lastRoundFile[512];

/* Whether this server hands the last completed round back to a client that
 * asks for it. Reset to auto by serverDedicatedLogInstall; see
 * ROUND_LOG_SERVE_* in server_dedicated_log.h for what auto resolves to and
 * when. */
static int s_serveMode = ROUND_LOG_SERVE_AUTO;

/* TRUE once the open log has seen a game start, i.e. it holds a round and
 * not just a lobby. Gates the publish: without it, closing the lobby log
 * that a finished round returns to would move that lobby log over the
 * round it just published. */
static bool s_roundRan = FALSE;

/* Control-event work handed to the recording thread. CTRL_GAME_PHASE_RUNNING
 * and CTRL_LOBBY_MAP_CHANGE are published from whichever thread drove the
 * transition — for in-process single player the main thread, not the timer
 * thread logWriteTick pinned as the log's owner — so logAddEvent would drop
 * everything they emit. The deliver path records what to emit and
 * serverDedicatedLogDrain, registered as log.c's pre-tick hook, emits it from
 * the owning thread. The map message keeps the pascal-string shape logAddEvent
 * takes; two map changes before a drain leave the later one.
 *
 * CTRL_GAME_PHASE_LOBBY goes through the same drain for a different reason:
 * ordering against the WinBolo.net session rotation, not thread ownership.
 * serverSimReturnToLobby publishes it from inside serverSimTick, while the
 * finished round's server_key is still installed; only afterwards does
 * serverInstanceTick run winbolonetEndSession -> round-log upload ->
 * winbolonetBeginSession, which mints the next round's key. logStart stamps
 * the header with winboloNetGetServerKey, so opening the log from the deliver
 * path wrote the outgoing round's key into the incoming round's file — one
 * rotation stale for every round after the first, which is the key the
 * standalone viewer then fetches comments against. Opening from the drain puts
 * logStart on the next log tick, after the new key exists. */
static bool s_lobbyEnterPending = FALSE;
static bool s_gameStartPending = FALSE;
static bool s_mapMsgPending = FALSE;
static bool s_settingsPending = FALSE;
static char s_pendingMapMsg[256];

/* The settings last written to the open log. CTRL_LOBBY_SETTINGS is published
 * for map changes, phase transitions and joins as well as real edits, so the
 * drained lobby-edit path compares against this and writes nothing when the
 * settings did not actually move. The three unconditional emit sites write
 * regardless and refresh it, so the settings a round was played under are
 * always in the running segment. Invalid until the open log has one. */
static BYTE s_lastSettings[LOG_SETTINGS_PAYLOAD_LEN];
static bool s_lastSettingsValid = FALSE;

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
    /* Publish the finished round clear of the recording path so the next
     * lobby entry's logStart can truncate that path without destroying the
     * round. Only for a log that actually holds a round: a lobby log closed
     * on the way out of the game must not overwrite the round it followed,
     * nor repoint s_lastRoundFile at itself. */
    if (s_roundRan) {
        /* True unless a move was wanted and did not happen. A caller that
         * records to a path it reuses is asking for the round to be taken off
         * that path, so a move it did not get is the round not surviving. */
        bool published = TRUE;

        if (s_completedPath[0] != '\0' &&
            strcmp(s_completedPath, s_logFileName) != 0) {
            if (SDL_RenamePath(s_logFileName, s_completedPath)) {
                strncpy(s_logFileName, s_completedPath, sizeof(s_logFileName) - 1);
                s_logFileName[sizeof(s_logFileName) - 1] = '\0';
                /* An upload stashed above named the pre-move path. No caller
                 * both publishes and uploads today (hosting sets no completed
                 * path, single player never uploads), but a stale name here
                 * would be a quiet failure for whoever combines them. */
                if (s_pendingUploadFile[0] != '\0') {
                    strncpy(s_pendingUploadFile, s_logFileName,
                            sizeof(s_pendingUploadFile) - 1);
                    s_pendingUploadFile[sizeof(s_pendingUploadFile) - 1] = '\0';
                }
            } else {
                published = FALSE;
            }
        }

        if (published) {
            /* s_logFileName names a file that will still be there when someone
             * comes for it: the published copy after a move, or the original
             * when no move was wanted — a host's rounds already resolve unique
             * timestamped names and nothing goes back over them. */
            strncpy(s_lastRoundFile, s_logFileName, sizeof(s_lastRoundFile) - 1);
            s_lastRoundFile[sizeof(s_lastRoundFile) - 1] = '\0';
        } else {
            /* The move is the whole reason a completed path exists, so a
             * failed one leaves the round sitting on the path it was recorded
             * to — which the next lobby entry's logStart truncates a few
             * seconds later. Naming it would hand the recap a file about to be
             * emptied under an open zip reader, and leaving the previous value
             * would offer the round before this one as if it were this one.
             * No replay for this round is the only honest answer. */
            s_lastRoundFile[0] = '\0';
        }
    }
    s_roundRan = FALSE;
}

/* The two forms below differ only in whether the multipart POST happens on
 * the caller's thread. Same guards either way: nothing stashed, WinBolo.net
 * off, or no server key and the round goes nowhere, and the stash is cleared
 * regardless so a round is never offered twice. */
static void serverDedicatedLogFlushPending(bool queued) {
    if (s_pendingUploadFile[0] == '\0') {
        return;
    }
    if (winbolonetIsRunning()) {
        char key[WINBOLONET_KEY_LEN];
        winboloNetGetServerKey(key);
        if (key[0] != '\0') {
            /* A refused enqueue means the worker is not running. There is
             * no later moment for the upload, so it posts from here rather
             * than being dropped with the stash. */
            if (!queued ||
                winbolonetThreadAddUpload(s_pendingUploadFile, key) == 0) {
                if (queued) {
                    fprintf(stderr,
                            "WinBolo.net worker refused the round-log upload "
                            "of %s; sending it on this thread\n",
                            s_pendingUploadFile);
                }
                httpSendLogFile(s_pendingUploadFile, key, FALSE);
            }
        }
    }
    s_pendingUploadFile[0] = '\0';
}

void serverDedicatedLogFlushPendingUpload(void) {
    serverDedicatedLogFlushPending(/*queued*/ FALSE);
}

void serverDedicatedLogQueuePendingUpload(void) {
    serverDedicatedLogFlushPending(/*queued*/ TRUE);
}

bool serverDedicatedLogHasPendingUpload(void) {
    return s_pendingUploadFile[0] != '\0';
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

/* Write the current settings to the log. `force` is TRUE where the record has
 * to be there whatever came before — the log opening, and the round starting —
 * and FALSE on the drained lobby-edit path, which skips a repeat of settings
 * already written. */
static void serverDedicatedLogEmitSettings(ServerSim *sim, bool force) {
    char blob[LOG_SETTINGS_PAYLOAD_LEN + 1];

    serverDedicatedLogBuildSettings(sim, blob);
    if (!force && s_lastSettingsValid &&
        memcmp(s_lastSettings, blob + 1, LOG_SETTINGS_PAYLOAD_LEN) == 0) {
        return;
    }
    logAddEvent(log_GameSettings, 0, 0, 0, 0, 0, blob);
    memcpy(s_lastSettings, blob + 1, LOG_SETTINGS_PAYLOAD_LEN);
    s_lastSettingsValid = TRUE;
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
    /* A freshly opened lobby log holds no round yet. Redundant with the
     * reset at the end of the stash, deliberately: the invariant then holds
     * whichever path opened this log. */
    s_roundRan = FALSE;
    /* The settings written below belong to this file and nothing before it. */
    s_lastSettingsValid = FALSE;
    if (s_isLogging) {
        logAddEvent(log_LobbyEnter, 0, 0, 0, 0, 0, NULL);
        /* What the lobby opened with. The host can edit any of it before the
         * countdown, which is why the round start writes it again. */
        serverDedicatedLogEmitSettings(sim, TRUE);
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
                    /* The country comes from the client table, not the sim:
                     * this loop re-announces players who joined in an earlier
                     * round, whose original join event is in a previous log
                     * file. XX stands in when the table has nothing (the
                     * non-UDP build, or a slot the sim thinks is connected
                     * and the transport does not). */
                    const char *cc = transportUdpServerGetClientCountryCode(i);
                    bool haveCC = (cc != NULL && cc[0] != '\0' && cc[1] != '\0');
                    logAddEvent(log_PlayerJoined, i,
                                haveCC ? (BYTE)cc[0] : (BYTE)'X',
                                haveCC ? (BYTE)cc[1] : (BYTE)'X',
                                accountFlags, 0, pstr);
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
    memcpy(s_pendingMapMsg, pstr, (size_t)nameLen + 1);
    s_mapMsgPending = TRUE;
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
        /* The open log stops being a lobby log here and becomes a round,
         * which is what makes it worth publishing when it closes. */
        s_roundRan = TRUE;
        logAddEvent(log_LobbyExit, 0, 0, 0, 0, 0, NULL);
        /* The settings the round is actually played under, whatever the lobby
         * opened with and whatever the host changed since. */
        serverDedicatedLogEmitSettings(sim, TRUE);
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
    s_lastSettingsValid = FALSE;
    s_isLogging = logStart(s_logFileName, sim,
                           0, MAX_TANKS, sim->hasPassword);
    if (s_isLogging) {
        /* A no-lobby log is a round from the moment it opens — recording
         * starts at the running transition, with no lobby segment in front
         * of it — so the publish gate has to be armed here too. */
        s_roundRan = TRUE;
        /* No lobby means no edits and no second chance: this is the only
         * settings record a -nolobby or -maprotate round gets. */
        serverDedicatedLogEmitSettings(sim, TRUE);
        fprintf(stderr, "Logging to %s\n", s_logFileName);
    }
}

/* log.c's pre-tick hook: open the round's log and emit what the control-event
 * handlers queued. Runs on the thread logWriteTick pinned, so these writes pass
 * the writer's owner check, and runs before the tick's accounting, so they are
 * framed as this tick's events. Inert when nothing is pending. */
static void serverDedicatedLogDrain(void) {
    ServerSim *sim = s_logSim;

    if (sim == NULL) {
        return;
    }
    if (s_lobbyEnterPending == FALSE && s_gameStartPending == FALSE &&
        s_mapMsgPending == FALSE && s_settingsPending == FALSE) {
        return;
    }
    /* The session rotation is queued, so the next round's key is not in yet:
     * the register is still on the worker and winboloNetGetServerKey still
     * answers with the round that just quit. logStart stamps the header with
     * it, so opening the log now would write the finished round's key into
     * the new round's file — the staleness the block at the top of this file
     * describes. Hold everything, not just the lobby arm: the three below
     * write into a log the arm is what opens. The window is closed by the
     * register result or, when WinBolo.net is off or nothing was queued, on
     * the tick itself, so this cannot hold forever. */
    if (s_lobbyEnterPending == TRUE && winbolonetIsRunning() &&
        sim->wbnSessionRotating) {
        return;
    }
    /* Lobby enter first: it is the arm that opens the log, and the two below
     * only write into an open one — handleGameStart's recording branch and the
     * map message both need logIsRecording() to already be true. */
    if (s_lobbyEnterPending == TRUE) {
        handleLobbyEnter(sim);
    }
    /* Game start before the map message: logWriteSnapshot flushes the queued
     * log_LobbyExit and log_AllyAccept events before it writes the snapshot
     * marker, which is what puts the LOG_EVENT frame ahead of the LOG_SNAPSHOT
     * in the byte stream. */
    if (s_gameStartPending == TRUE) {
        handleGameStart(sim);
    }
    /* Settings after the two above: when a lobby edit and one of them land in
     * the same tick, the record they wrote is the current one and this adds
     * nothing. */
    if (s_settingsPending == TRUE) {
        serverDedicatedLogEmitSettings(sim, FALSE);
    }
    if (s_mapMsgPending == TRUE) {
        logAddEvent(log_MessageServer, 0, 0, 0, 0, 0, s_pendingMapMsg);
    }
    s_lobbyEnterPending = FALSE;
    s_gameStartPending = FALSE;
    s_mapMsgPending = FALSE;
    s_settingsPending = FALSE;
}

static void serverDedicatedLogDeliver(void *ctx, const ControlEvent *evt) {
    ServerSim *sim = s_logSim;
    (void)ctx;
    if (sim == NULL) {
        return;
    }
    switch (evt->type) {
        case CTRL_GAME_PHASE_LOBBY:
            /* Queued, not opened here — the header's WinBolo.net key has to be
             * read after this tick's session rotation, not before it. The
             * lobby state calls logWriteTick() directly (server_sim.c's
             * serverStateLobby arm) rather than going through
             * serverSimLogTick's logIsRecording() early return, so the drain
             * still fires on the very next tick with no log open. */
            s_lobbyEnterPending = TRUE;
            break;
        case CTRL_GAME_PHASE_RUNNING:
            /* The recording branch emits the round's marker, the alliance
             * audit events and the world rewrite, so it has to run on the
             * recording thread. The no-lobby branch only opens the file —
             * logStart re-pins the owner itself — and has to stay here: with
             * no lobby and no spectator ring, serverSimLogTick returns before
             * logWriteTick, so there would be no drain until a log exists. */
            if (sim->wantLogging && logIsRecording()) {
                s_gameStartPending = TRUE;
            } else {
                handleGameStart(sim);
            }
            break;
        case CTRL_GAME_PHASE_GAME_OVER:
            handleGameOver(sim);
            break;
        case CTRL_LOBBY_MAP_CHANGE:
            handleLobbyMapChange(sim);
            break;
        case CTRL_LOBBY_SETTINGS:
            /* Settings apply on whichever thread drove the edit, so the write
             * has to wait for the drain like the map message does. An edit
             * before the log opens needs nothing queued: handleLobbyEnter
             * records the settings the file opens with. */
            if (sim->wantLogging && logIsRecording()) {
                s_settingsPending = TRUE;
            }
            break;
        default:
            break;
    }
}

int serverDedicatedLogServeMode(void) {
    return s_serveMode;
}

void serverDedicatedLogSetServeMode(int mode) {
    if (mode != ROUND_LOG_SERVE_OFF && mode != ROUND_LOG_SERVE_ON &&
        mode != ROUND_LOG_SERVE_AUTO) {
        return;
    }
    s_serveMode = mode;
}

/* RoundLogSource::serveEnabled. Auto resolves here rather than at install
 * because winbolonetIsRunning() can flip after the recorder is installed. */
static bool serverDedicatedLogServeAllowed(void) {
    switch (s_serveMode) {
        case ROUND_LOG_SERVE_OFF: return FALSE;
        case ROUND_LOG_SERVE_ON:  return TRUE;
        default:                  return !winbolonetIsRunning();
    }
}

/* RoundLogSource::read. Owns the size cap: the file is measured first and one
 * over ROUND_LOG_MAX_BYTES is refused unopened, because a .wbv cut down to
 * fit is unopenable rather than merely shorter — minizip writes the zip's
 * central directory only at zipClose(). The buffer is plain malloc so the
 * transport can free it with free() after the bulk sender takes its own copy.
 * The name handed back is the basename: the server's directory layout is not
 * the client's business. */
static RoundLogReadResult serverDedicatedLogReadLastRound(uint8_t **outBuf,
                                                          uint32_t *outLen,
                                                          char *outName,
                                                          size_t outNameSize) {
    SDL_PathInfo info;
    const char *base;
    const char *p;
    FILE *fp;
    uint8_t *buf;
    size_t size;
    size_t got;

    if (outBuf == NULL || outLen == NULL || outName == NULL ||
        outNameSize == 0) {
        return ROUND_LOG_READ_ERROR;
    }
    if (s_lastRoundFile[0] == '\0') {
        return ROUND_LOG_READ_NONE;
    }
    /* A named-but-missing file reads as "no round" rather than an error: the
     * round is gone (moved, deleted between rounds), which is the same thing
     * to the asking client. */
    if (!SDL_GetPathInfo(s_lastRoundFile, &info) ||
        info.type != SDL_PATHTYPE_FILE || info.size == 0) {
        return ROUND_LOG_READ_NONE;
    }
    if (info.size > (Uint64)ROUND_LOG_MAX_BYTES) {
        return ROUND_LOG_READ_TOO_LARGE;
    }
    size = (size_t)info.size;

    fp = fopen(s_lastRoundFile, "rb");
    if (fp == NULL) {
        return ROUND_LOG_READ_ERROR;
    }
    buf = (uint8_t *)malloc(size);
    if (buf == NULL) {
        fclose(fp);
        return ROUND_LOG_READ_ERROR;
    }
    got = fread(buf, 1, size, fp);
    fclose(fp);
    if (got != size) {
        free(buf);
        return ROUND_LOG_READ_ERROR;
    }

    base = s_lastRoundFile;
    for (p = s_lastRoundFile; *p != '\0'; p++) {
        if (*p == '/' || *p == '\\') {
            base = p + 1;
        }
    }
    snprintf(outName, outNameSize, "%s", base);

    *outBuf = buf;
    *outLen = (uint32_t)size;
    return ROUND_LOG_READ_OK;
}

static const RoundLogSource s_roundLogSource = {
    serverDedicatedLogServeAllowed,
    serverDedicatedLogReadLastRound
};

void serverDedicatedLogInstall(ServerSim *sim, bool dontSendLog) {
    if (sim == NULL) {
        return;
    }
    /* Install is where per-sim publish policy resets. The completed path and
     * the serve mode are module state that outlives the sim that asked for
     * them, so without this a single-player game would leave its path set and
     * the next server in the same process — a hosted game, whose rounds must
     * stay where the host configured them — would move its round log there,
     * and would inherit whatever serve mode that game chose. */
    s_completedPath[0] = '\0';
    s_lastRoundFile[0] = '\0';
    s_roundRan = FALSE;
    s_lobbyEnterPending = FALSE;
    s_gameStartPending = FALSE;
    s_mapMsgPending = FALSE;
    s_settingsPending = FALSE;
    s_lastSettingsValid = FALSE;
    s_serveMode = ROUND_LOG_SERVE_AUTO;
    s_dontSendLog = dontSendLog;
    s_logSim = sim;
    serverSimRegisterSubscriber(sim, serverDedicatedLogDeliver, NULL);
    /* Emit point for the handlers that run off the recording thread. Like the
     * two registrations around it, install-only — nothing unregisters it. */
    logSetPreTickHook(serverDedicatedLogDrain);
    /* Hand the lifecycle our stash/flush so its lobby/empty-reset
     * cleanup can drive the per-round upload. */
    serverLifecycleSetRoundLogHooks(serverDedicatedLogStashCurrentRound,
                                    serverDedicatedLogQueuePendingUpload);
    /* Tell the transport where a PACKET_ROUND_LOG_REQ gets its bytes. Pushed
     * outward like the two registrations above so the transport never names a
     * symbol in this file — it must stay linkable without the WinBolo.net
     * upload path this module depends on. */
    transportUdpServerSetRoundLogSource(&s_roundLogSource);
}

void serverDedicatedLogUninstall(void) {
    /* The same state install resets, cleared at the other end of the sim's
     * life. Install alone is not enough: it runs only for a server that logs,
     * so a host that turned logging off never resets anything and inherits
     * whatever the last server in this process left behind — its completed
     * round as the last round, and its serve mode with it.
     *
     * The transport's source goes first. Passing NULL is what makes it answer
     * PACKET_ROUND_LOG_REQ with "nothing here" rather than reading through
     * this module's now-cleared path, and it is the half that closes the leak
     * on its own: the recap can only name a file, but this hands the bytes to
     * anyone who joins. Nothing here is undone by the sim being freed
     * afterwards — it is all module state that outlives it. */
    transportUdpServerSetRoundLogSource(NULL);
    /* log.c calls the drain at the top of every logWriteTick, and the sims that
     * tick are not only the one that installed us — the welcome screen's
     * background game is a real ServerSim and ticks whenever the menu is up.
     * The drain bails on a NULL sim, and clearing s_logSim below is what makes
     * that guard mean anything, since until now it was reading a pointer to a
     * sim serverSimDestroy had already freed. Dropping the hook as well leaves
     * nothing at all pointing into this module between one server and the next.
     * logCreate deliberately does not clear the hook — a background sim created
     * after an install would disarm a live writer — so here is the only place
     * it comes off. */
    logSetPreTickHook(NULL);
    s_logSim = NULL;
    s_completedPath[0] = '\0';
    s_lastRoundFile[0] = '\0';
    s_roundRan = FALSE;
    /* Work the deliver path queued for a drain that will now never come. These
     * gate the drain's early-out alongside the sim pointer, so a session that
     * ended with one still set is the case that reached the dereference. */
    s_lobbyEnterPending = FALSE;
    s_gameStartPending = FALSE;
    s_mapMsgPending = FALSE;
    s_settingsPending = FALSE;
    s_lastSettingsValid = FALSE;
    s_serveMode = ROUND_LOG_SERVE_AUTO;
    /* s_pendingUploadFile is deliberately left alone: a round stashed for
     * WinBolo.net that could not go out yet (the session was down) is still
     * owed, and the teardown paths flush it on their own schedule. */
}

bool serverDedicatedLogIsActive(void) {
    return s_isLogging;
}

const char *serverDedicatedLogCurrentFile(void) {
    return s_logFileName;
}

void serverDedicatedLogSetCompletedPath(const char *path) {
    if (path == NULL || path[0] == '\0') {
        s_completedPath[0] = '\0';
        return;
    }
    strncpy(s_completedPath, path, sizeof(s_completedPath) - 1);
    s_completedPath[sizeof(s_completedPath) - 1] = '\0';
}

const char *serverDedicatedLogLastRoundFile(void) {
    return s_lastRoundFile;
}
