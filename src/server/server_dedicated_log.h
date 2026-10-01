/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
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

/* Drop the sim and forget the round it recorded. Install is the other half of
 * this and resets the same state, but only the callers that install reach it:
 * a host with logging turned off never calls install at all, so without this
 * the previous server in the process — a single-player game, say — leaves its
 * completed round standing as the last round, and the AUTO serve gate standing
 * with it. That round is then handed to a client that asks for it and named in
 * the host's own recap: the wrong round, and one the host never chose to share.
 *
 * It also drops the registrations that outlive the sim: the transport's round
 * log source and log.c's pre-tick hook. That hook is the one with teeth — the
 * sims that reach logWriteTick are not only the one that installed us, the
 * welcome screen's background game among them, and the drain it calls held a
 * pointer to a sim that had already been freed.
 *
 * Call it wherever a server is torn down, after any final stash and upload,
 * whether or not that server installed. Idempotent, and safe with nothing
 * installed. */
void serverDedicatedLogUninstall(void);

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

/* Absolute path of the most recently completed round's log, or "" when there
 * is no round to offer. For hosting that is the round's own timestamped file;
 * with a completed path set it is that path.
 *
 * "" covers three cases, and a caller need not tell them apart: no round has
 * finished since the writer was installed, the writer has since been
 * uninstalled, or the round finished but could not be moved to its completed
 * path — which leaves it on a recording path the next lobby entry truncates,
 * so there is nothing there worth naming. */
const char *serverDedicatedLogLastRoundFile(void);

/* Finalize the current round's log (logStop, isLogging=false) and
 * stash its filename for a later upload. Called from handleGameOver
 * for lobby-enabled rounds and from the empty-reset path. WBN log
 * uploads require that the session has already been ended via
 * server/quit, so the upload itself is deferred to one of the two
 * calls below — the caller is expected to sandwich it between the end
 * of the old session and the start of the next one, so the upload runs
 * against the just-quit session's still-valid server_key. */
void serverDedicatedLogStashCurrentRound(void);

/* Upload the stashed round log (if any) to WinBolo.net via
 * httpSendLogFile, using the current winboloNetServerKey. Caller
 * must have already POSTed server/quit (WBN rejects uploads to an
 * active session) and must not yet have POSTed server/register
 * (registration overwrites winboloNetServerKey, invalidating the
 * URL key the upload needs). Clears the stash either way.
 *
 * Posts on the calling thread, which is what a teardown wants: it is
 * about to stop the worker, and httpSetLogUploadTimeout applies to a
 * call made here. A round transition wants the queued form below. */
void serverDedicatedLogFlushPendingUpload(void);

/* The same upload, handed to the WinBolo.net worker instead of posted
 * here. Same guards and the same stash clear; it keeps its place in the
 * queue, so a caller that queued server/quit before it and
 * server/register after it gets the order WinBolo.net requires without
 * waiting for any of the three. This is the form the round-log flush
 * hook installs, so every round transition queues. */
void serverDedicatedLogQueuePendingUpload(void);

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

/* Bytes of settings the log_GameSettings blob carries, after its length
 * byte. The layout is written out in docs/replay-format.md; it is
 * append-only, so a later field lands after the last one (offset 16 in
 * the doc's numbering) and this grows with it. Here rather than beside the writer because the emit path compares
 * blobs of this size against the last one it wrote. */
#define LOG_SETTINGS_PAYLOAD_LEN 17

/* Bits of the log_GameSettings settings-flags byte — the last byte of the
 * blob, added once the flags byte at offset 9 filled up. An old recording
 * does not carry the byte at all and a reader treats it as zero, so every
 * bit here has to mean "off / classic behaviour" when clear. */
#define LOG_SETTINGS_FLAG_SMART_PINGS_OFF 0x01u
#define LOG_SETTINGS_FLAG_POSITIONAL_SOUND 0x02u

/* Build the log_GameSettings blob in the pascal form logAddEvent takes:
 * out[0] is the byte count and out[1..] the fields, layout in
 * docs/replay-format.md. out needs room for the count plus the payload.
 * Exposed for tests; the log writer calls it on its own emit paths. */
void serverDedicatedLogBuildSettings(struct ServerSim *sim, char *out);

#endif /* SERVER_DEDICATED_LOG_H */
