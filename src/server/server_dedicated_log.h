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

#endif /* SERVER_DEDICATED_LOG_H */
