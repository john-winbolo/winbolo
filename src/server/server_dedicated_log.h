#ifndef SERVER_DEDICATED_LOG_H
#define SERVER_DEDICATED_LOG_H

/* Registers the dedicated-server log writer as a bus subscriber
 * against the supplied ServerSim. The subscriber listens for
 * CTRL_GAME_PHASE_LOBBY / RUNNING / GAME_OVER and manages the
 * replay-log file lifecycle accordingly. Called by servermain.c
 * once at startup when --log is set. Ships only in WinBoloDS — no
 * other binary references this symbol, so no companion stub file
 * is needed. */

struct ServerSim;

void serverDedicatedLogInstall(struct ServerSim *sim);

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

#endif /* SERVER_DEDICATED_LOG_H */
