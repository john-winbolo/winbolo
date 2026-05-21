#ifndef BOLO_INTERNAL_SERVER_SIM_LIFECYCLE_H
#define BOLO_INTERNAL_SERVER_SIM_LIFECYCLE_H

#include "server_sim.h"

/* Lobby/round lifecycle transitions. Callers: the UDP server's
 * countdown / all-ready paths, dedicated server admin /start,
 * headless --cmd-stdin scripted ops, gamefront SP host. */
void serverSimStartGameInPlace(ServerSim *sim);
void serverSimEnterLobby(ServerSim *sim);
void serverSimReturnToLobby(ServerSim *sim);
void serverSimEnterGameOver(ServerSim *sim);
void serverSimLobbyCheckAllReady(ServerSim *sim);

/* Per-slot lobby state setter. Driven by UDP PACKET_LOBBY_READY handlers
 * and by client_net.c's local-transport branches. */
void serverSimSetReady(ServerSim *sim, BYTE playerNum, bool ready);

/* Startup-time config setters. Applied by serverInstanceStartup from
 * ServerInstanceConfig fields. */
void serverSimSetHasPassword(ServerSim *sim, bool hasPassword);
void serverSimSetBotBrainPath(ServerSim *sim, const char *path);
void serverSimSetOpenHost(ServerSim *sim, bool v);
void serverSimSetServerLocks(ServerSim *sim, uint16_t locks);

/* Runtime lobby-state toggles. Mutated only by their in-process
 * packet handlers and lifecycle code. */
void serverSimSetAllowNewPlayers(ServerSim *sim, bool v);
void serverSimSetBalanceBroadcastNeeded(ServerSim *sim, bool needed);
void serverSimSetBalanceRequestInFlight(ServerSim *sim, bool inFlight);
void serverSimSetBalanceIncludeBots(ServerSim *sim, bool includeBots);
void serverSimSetSuppressNextWinMessage(ServerSim *sim, bool v);

/* Mutable lobby accessors. Callers: UDP PACKET_LOBBY_* handlers and
 * client_net.c's local-transport branches. */
TeamMetadata    *serverSimGetTeamMetaMut(ServerSim *sim, BYTE teamId);
LobbyBotConfig  *serverSimGetBotConfigMut(ServerSim *sim, BYTE slot);
LobbyPlayer     *serverSimGetLobbyPlayerMut(ServerSim *sim, BYTE n);

/* Auto-unready on meaningful lobby change. Called after each apply
 * from PACKET_LOBBY_* handlers and client_net.c local-transport
 * branches. */
void lobbyAutoUnreadyOnChange(ServerSim *sim);

#endif
