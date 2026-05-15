#ifndef SERVER_DEDICATED_LOG_H
#define SERVER_DEDICATED_LOG_H

/* Server-side log-recording hooks invoked from server_sim's lifecycle
 * transitions. The real implementation lives in server_dedicated_log.c
 * and is linked only into the WinBoloDS dedicated-server binary, where
 * servermain.c owns the global log state (fileName / isLogging /
 * dontSendLog) plus the makeLogFileName helper. Every other binary
 * links server_dedicated_log_stubs.c, which supplies empty-body
 * versions so server_sim.c can call these unconditionally. */

#include "server_sim.h"

void serverDedicatedLogOnEnterGameOver(ServerSim *sim);
void serverDedicatedLogOnReturnToLobby(ServerSim *sim);
void serverDedicatedLogOnLobbyExit(ServerSim *sim);

#endif /* SERVER_DEDICATED_LOG_H */
