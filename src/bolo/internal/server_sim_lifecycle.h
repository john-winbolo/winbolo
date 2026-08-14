/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

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

/* Per-slot reserved-start write. Stores idx (1-based map start, or 0xFF
 * for none) in lobbyPlayers[slot].startIdx with no clustering or
 * publish — the CMD_LOBBY_CLAIM_START dispatcher arm validates the
 * index and republishes the affected slots itself. */
void serverSimSetLobbyStartIdx(ServerSim *sim, BYTE slot, BYTE idx);

/* Per-slot team assignment. Writes lobbyPlayers[playerNum].teamNumber,
 * rebakes the alliance graph so any same-team pairs become allies, then
 * re-clusters the slot's reserved start to the new team (the slot's own
 * current start stays a candidate; no-op outside lobby state). All three
 * complete before the call returns. Drivers: UDP PACKET_LOBBY_TEAM_SET /
 * PACKET_LOBBY_ADD_BOT handlers, the SP-host local-transport branch
 * of clientSimNetSendTeamSet, the headless cmd-stdin CMD_OP_SET_TEAM
 * handler. Loop callers use serverSimSetTeamBatch instead — see that
 * declaration. */
void serverSimSetTeam(ServerSim *sim, BYTE playerNum, BYTE teamNumber);

/* Same write as serverSimSetTeam but skips the alliance rebake; the
 * caller must follow up with serverSimReapplyTeamAlliances when its
 * batch of set calls is done. Suitable for loop callers where the
 * O(N²) rebake would otherwise run on every iteration: balance apply,
 * the bg_game lobby-background animation, the servermain CLI bot
 * setup, and the braintest setup loop. */
void serverSimSetTeamBatch(ServerSim *sim, BYTE playerNum, BYTE teamNumber);

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
void serverSimSetPassword(ServerSim *sim, const char *pw, size_t len);
const char *serverSimGetPassword(const ServerSim *sim);
void serverSimSetBotBrainPath(ServerSim *sim, const char *path);
void serverSimSetOpenHost(ServerSim *sim, bool v);
/* serverSimSetHostSlot — set the lobby host slot and publish the lobby
 * settings. Publish-only: unlike serverSimSetOpenHost it does NOT
 * auto-unready players. */
void serverSimSetHostSlot(ServerSim *sim, BYTE slot);
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

/* Mutable lobby accessors. Callers: UDP PACKET_LOBBY_* handlers and
 * client_net.c's local-transport branches. */
TeamMetadata    *serverSimGetTeamMetaMut(ServerSim *sim, BYTE teamId);
LobbyBotConfig  *serverSimGetBotConfigMut(ServerSim *sim, BYTE slot);
LobbyPlayer     *serverSimGetLobbyPlayerMut(ServerSim *sim, BYTE n);

/* Server-side map I/O root. Returns the operator-configured -mapdir
 * path when set, otherwise the built-in "data/maps". Callers: the
 * server's directory enumerate/search functions and the
 * PACKET_LOBBY_SET_MAP / PACKET_LOBBY_MAP_USE_LOCAL path resolvers in
 * transport_udp_server.c. The returned pointer has no trailing slash;
 * concatenate with "/<rel>". */
const char *serverSimGetMapDirRoot(const ServerSim *sim);

/* Absolute directory backing the virtual "Uploads/" folder for
 * PERSIST-policy uploads. Pass NULL or "" to leave it unset (uploads then
 * resolve under "<mapDirRoot>/Uploads"). The enumerate/search/read resolvers
 * redirect the "Uploads"/"Uploads/<name>" prefix here when set. */
void serverSimSetUploadPersistDir(ServerSim *sim, const char *dir);

/* Auto-unready on meaningful lobby change. Called after each apply
 * from PACKET_LOBBY_* handlers and client_net.c local-transport
 * branches. */
void lobbyAutoUnreadyOnChange(ServerSim *sim);

/* Apply the LST_* setting cluster shared by PACKET_LOBBY_SET_SETTING
 * (UDP) and the SP-host local-transport branch of
 * clientSimNetSendLobbySetting. Caller validates lock-bit / authority
 * gates; on success this publishes CTRL_LOBBY_SETTINGS and clears
 * humans' ready state before returning true. Returns false on
 * malformed payload, out-of-range value, or cross-setting invariant
 * rejection (e.g. ranked forbids gameOpen / non-aiNone / autoLock
 * off). */
bool serverSimApplyLobbySetting(ServerSim *sim,
                                uint8_t lst,
                                const uint8_t *value, size_t len);

/* Apply a bot-config change atomically — write difficulty / personality
 * to the slot, optionally rename the bot (when validatedName is
 * non-NULL and non-empty), publish CTRL_LOBBY_BOT_CONFIG +
 * CTRL_LOBBY_SLOT, and clear humans' ready state. Callers (UDP
 * PACKET_LOBBY_BOT_CONFIG handler, SP-host clientSimNetSendLobbyBotConfig)
 * must validate the name beforehand — see lobbyBotNameAcceptable.
 * Pass NULL or an empty string to leave the name unchanged. */
void serverSimSetBotConfig(ServerSim *sim, BYTE slot,
                            uint8_t difficulty, uint8_t personality,
                            const char *validatedName);

/* Apply a team-metadata change atomically — write color, naming pool
 * (with the in-use-pool rewrite when another team owns the requested
 * pool), and name, publish CTRL_LOBBY_TEAM_META, and clear humans'
 * ready state. Callers (UDP PACKET_LOBBY_TEAM_META handler, SP-host
 * clientSimNetSendLobbyTeamMeta) just hand over the validated payload.
 * nameLen 0 leaves the team's name empty. */
void serverSimSetTeamMeta(ServerSim *sim, BYTE teamId,
                           uint8_t color, uint8_t namingPool,
                           const uint8_t *name, uint8_t nameLen);

/* Clear a team's metadata back to zero (in_use=0, color=0, pool=0,
 * empty name), publish CTRL_LOBBY_TEAM_META, and clear humans' ready
 * state. Callers (UDP PACKET_LOBBY_TEAM_CLEAR handler, SP-host
 * clientSimNetSendLobbyTeamClear). */
void serverSimClearTeamMeta(ServerSim *sim, BYTE teamId);

#endif
