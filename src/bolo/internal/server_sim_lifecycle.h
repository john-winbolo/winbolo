#ifndef BOLO_INTERNAL_SERVER_SIM_LIFECYCLE_H
#define BOLO_INTERNAL_SERVER_SIM_LIFECYCLE_H

#include "server_sim.h"
#include "../../server/server_lifecycle.h"  /* ServerInstanceConfig */

/* Lobby/round lifecycle transitions. Callers: the UDP server's
 * countdown / all-ready paths, dedicated server admin /start,
 * headless --cmd-stdin scripted ops, gamefront SP host. */
void serverSimStartGameInPlace(ServerSim *sim);
void serverSimStartGame(ServerSim *sim);
void serverSimEnterLobby(ServerSim *sim);
void serverSimReturnToLobby(ServerSim *sim);
void serverSimEnterGameOver(ServerSim *sim);
void serverSimLobbyCheckAllReady(ServerSim *sim);

/* Per-slot lobby state setter. Driven by UDP PACKET_LOBBY_READY handlers
 * and by client_net.c's local-transport branches. */
void serverSimSetReady(ServerSim *sim, BYTE playerNum, bool ready);

/* Per-slot team assignment. Driven by UDP PACKET_LOBBY_TEAM_SET /
 * PACKET_LOBBY_ADD_BOT handlers, the SP-host local-transport branch
 * of clientSimNetSendTeamSet, the gamefront SP-host setup loop, and
 * the bg_game lobby-background animation (per-file T2 grant). */
void serverSimSetTeam(ServerSim *sim, BYTE playerNum, BYTE teamNumber);

/* viewPlayer — which player perspective the sim renders from.
 * Set at startup from cfg->viewPlayer; mutated transiently by the
 * bg_game camera-perspective override (per-file T2 grant). */
void serverSimSetViewPlayer(ServerSim *sim, BYTE playerNum);

/* In-memory analogue of serverSimReloadMap: loads the supplied
 * compressed map blob directly instead of reading from a file. The
 * caller-supplied mapName is used as the display name (no basename
 * or suffix stripping). Same return contract, same side effects as
 * serverSimReloadMap. Driven by the UDP MAP_UPLOAD_DONE handler and
 * the SP-host local-transport branch of
 * clientSimNetSendLobbyMapUploadBytes. */
bool serverSimReloadCompressedInMemory(ServerSim *sim,
                                       const uint8_t *bytes, int len,
                                       const char *mapName);

/* Startup-time config setters. Applied by serverInstanceStartup from
 * ServerInstanceConfig fields. The Ranked / AutoLockOnGameStart /
 * BotAiType variants are also mutated by serverSimApplyLobbySetting
 * (LST_RANKED / LST_AUTO_LOCK_ON_GAME / LST_AI_POLICY). */
void serverSimSetHasPassword(ServerSim *sim, bool hasPassword);
void serverSimSetBotBrainPath(ServerSim *sim, const char *path);
void serverSimSetOpenHost(ServerSim *sim, bool v);
void serverSimSetServerLocks(ServerSim *sim, uint16_t locks);
void serverSimSetLobbyEnabled(ServerSim *sim, bool enabled);
void serverSimSetBotAiType(ServerSim *sim, aiType ai);
void serverSimSetAutoLockOnGameStart(ServerSim *sim, bool v);
void serverSimSetRanked(ServerSim *sim, bool v);

/* Apply ServerInstanceConfig's cfg-driven setter cluster onto sim.
 * Called by serverInstanceStartup (production) and
 * braintest_lifecycle_stub.c (BrainTest's minimal alternative).
 * Covers every cfg field that isn't transport / WBN / tracker / NAT
 * setup — those are gated on cfg->acceptRemoteClients and stay in
 * serverInstanceStartup. */
void serverSimApplyInstanceConfig(ServerSim *sim,
                                  const ServerInstanceConfig *cfg);

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

/* Apply the LST_* setting cluster shared by PACKET_LOBBY_SET_SETTING
 * (UDP) and the SP-host local-transport branch of
 * clientSimNetSendLobbySetting. Caller validates lock-bit / authority
 * gates and runs the post-apply publish + auto-unready pass. Returns
 * false on malformed payload, out-of-range value, or cross-setting
 * invariant rejection (e.g. ranked forbids gameOpen / non-aiNone /
 * autoLock off). */
bool serverSimApplyLobbySetting(ServerSim *sim,
                                uint8_t lst,
                                const uint8_t *value, size_t len);

#endif
