/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

#ifndef SERVER_DEDICATED_LOG_H
#define SERVER_DEDICATED_LOG_H

#include <stddef.h>  /* size_t */
#include <stdbool.h>  /* bool */

/* Registers the dedicated-server log writer as a bus subscriber
 * against the supplied ServerSim. The subscriber listens for
 * CTRL_GAME_PHASE_LOBBY / RUNNING / GAME_OVER and manages the
 * replay-log file lifecycle accordingly. Called by servermain.c
 * once at startup when --log is set. Ships only in WinBoloDS — no
 * other binary references this symbol, so no companion stub file
 * is needed. */

struct ServerSim;

/* Registers the subscriber and hands the module its per-process log
 * state. `dontSendLog` (from WinBoloDS's -dontsendlog arg) is stored
 * privately and consulted by the per-round stash to skip the WBN
 * upload — the module owns this flag now that it is no longer a shared
 * servermain global. */
void serverDedicatedLogInstall(struct ServerSim *sim, bool dontSendLog);

/* Teardown query API for servermain's final-round upload. Since the
 * module owns the log state privately, servermain's shutdown reads it
 * back through these instead of the former shared globals. IsActive
 * reports whether a log is still open (true only on the no-lobby
 * teardown path; lobby rounds already stashed + flipped it false);
 * CurrentFile returns the on-disk .wbv path to upload. */
bool serverDedicatedLogIsActive(void);
const char *serverDedicatedLogCurrentFile(void);

/* Where a finished round should be published. Set by a caller that records
 * to a reused filename (single player), so the completed log is moved clear
 * before the next lobby entry truncates the recording path. Pass NULL or ""
 * to disable — the default, used by hosting, whose rounds already resolve
 * unique timestamped names. Cleared by serverDedicatedLogInstall, so set it
 * after installing, not before. */
void serverDedicatedLogSetCompletedPath(const char *path);

/* Absolute path of the most recently completed round's log, or "" when no
 * round has finished since the writer was installed. For hosting that is the
 * round's own timestamped file; with a completed path set it is that path. */
const char *serverDedicatedLogLastRoundFile(void);

/* Finalize the current round's log (logStop, isLogging=false) and
 * stash its filename for a later upload. Called from handleGameOver
 * for lobby-enabled rounds and from the empty-reset path. WBN log
 * uploads require that the session has already been ended via
 * server/quit, so the upload itself is deferred to
 * serverDedicatedLogFlushPendingUpload — the caller is expected to
 * sandwich that flush between winbolonetEndSession() and
 * winbolonetBeginSession() so the upload runs against the just-
 * quit session's still-valid server_key. */
void serverDedicatedLogStashCurrentRound(void);

/* Upload the stashed round log (if any) to WinBolo.net via
 * httpSendLogFile, using the current winboloNetServerKey. Caller
 * must have already POSTed server/quit (WBN rejects uploads to an
 * active session) and must not yet have POSTed server/register
 * (registration overwrites winboloNetServerKey, invalidating the
 * URL key the upload needs). Clears the stash either way. */
void serverDedicatedLogFlushPendingUpload(void);

/* True when a round log has been stashed and is waiting to be uploaded.
 * Lets a teardown caller decide whether it needs to end the WBN session
 * for the upload at all — false for non-logging hosts and for hosts that
 * opted out of uploads (dontSendLog), so those stay untouched. */
bool serverDedicatedLogHasPendingUpload(void);

/* Round-log serve policy — whether this server answers a joined client's
 * PACKET_ROUND_LOG_REQ with the last completed round's .wbv. AUTO resolves
 * at serve time to "on unless WinBolo.net is running", because a WBN
 * server's round log is uploaded there anyway; the resolution is deliberately
 * not latched at install, since WBN can start after the recorder does.
 * serverDedicatedLogInstall resets the mode to AUTO along with the rest of
 * this module's per-sim policy, so set it after installing, not before.
 * A value outside 0..2 is ignored. */
#define ROUND_LOG_SERVE_OFF  0
#define ROUND_LOG_SERVE_ON   1
#define ROUND_LOG_SERVE_AUTO 2

int  serverDedicatedLogServeMode(void);
void serverDedicatedLogSetServeMode(int mode);

/* Compose the final .wbv replay path from the -log argument value.
 * Pure (no globals / time / RNG) and cross-platform (SDL_GetPathInfo) so
 * it is unit-testable. See the definition in server_dedicated_log.c for
 * the directory / file / auto-name rules. Exposed for tests. */
void serverDedicatedLogComposePath(const char *logArg, const char *autoBase,
                                   char *out, size_t outSize);

#endif /* SERVER_DEDICATED_LOG_H */
