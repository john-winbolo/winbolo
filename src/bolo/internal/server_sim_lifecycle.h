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

/* The lobby template, applied by the engine. Defined in
 * server_sim_scenario.c; called from the three points a lobby is built or
 * rebuilt. The sim owns all of this — none of it calls back into whoever
 * read the scenario off disk.
 *
 * ReconcileLobby is the softer one a returning round gets: it keeps what the
 * host did between rounds, cuts a team back to maxBots, and returns the
 * seats a script fielded to being held. ClearSeats is the first half of
 * serverSimScenarioSeatLobby on its own. OnMapChanged tells whoever owns the
 * scenario about the new map and then seats what they leave behind.
 *
 * serverSimScenarioSeatLobby and serverSimScenarioApplyLobbyRules are on
 * server_sim.h instead: a process booting onto a scripted map makes both
 * calls itself, and the dedicated server, the headless runner and the
 * desktop host see only the public header. */
void serverSimScenarioClearSeats(ServerSim *sim);
void serverSimScenarioReconcileLobby(ServerSim *sim);
void serverSimScenarioOnMapChanged(ServerSim *sim, const char *mapPath);

/* Whether the map at mapPath has a script beside it, asked through whatever
 * the process registered with serverSimSetScenarioMapScripted. False with
 * nothing registered, which is what a build carrying no scenario library
 * answers for every map. */
bool serverSimScenarioMapIsScripted(const ServerSim *sim, const char *mapPath);

/* The pair a preview and its cancel use. SeatCounts reads the template seats
 * each team holds into out[], indexed by team id and MAX_TANKS long, and
 * answers false without writing when no template is attached. TrimSeatsTo
 * takes each of the template's teams back down to the matching count, the
 * way ReconcileLobby cuts one back to maxBots; a team below its count is
 * left alone, because this never seats upward. */
bool serverSimScenarioSeatCounts(const ServerSim *sim, BYTE *out);
void serverSimScenarioTrimSeatsTo(ServerSim *sim, const BYTE *counts);

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
void serverSimSetServerLocks(ServerSim *sim, uint32_t locks);
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
 * off, or the map's scenario fixes the value).
 *
 * The Result form is that same call saying why it refused, so the
 * sender can be told: CMD_REJECT_SCENARIO for one of the three the
 * attached scenario fixes — the game type, ranked, and the AI policy
 * that runs no bots — and CMD_REJECT_INVALID for everything else. The
 * command dispatcher calls that one; the bool is the same answer with
 * the reason dropped. */
CmdResult serverSimApplyLobbySettingResult(ServerSim *sim,
                                           uint8_t lst,
                                           const uint8_t *value, size_t len);
bool serverSimApplyLobbySetting(ServerSim *sim,
                                uint8_t lst,
                                const uint8_t *value, size_t len);

/* serverSimSetBotConfig lives on public/server_sim.h — the SP-host GUI
 * (gamefront.c) writes a bot's difficulty through it before creating the
 * bot, and the GUI only sees public/. */

/* Apply a team-metadata change atomically — write color, naming pool
 * (with the in-use-pool rewrite when another team owns the requested
 * pool), start side (a value outside the START_SIDE_* range is stored
 * as START_SIDE_ANY), and name, publish CTRL_LOBBY_TEAM_META, and clear
 * humans' ready state. Callers (UDP PACKET_LOBBY_TEAM_META handler,
 * SP-host clientSimNetSendLobbyTeamMeta) just hand over the validated
 * payload. nameLen 0 leaves the team's name empty. */
void serverSimSetTeamMeta(ServerSim *sim, BYTE teamId,
                           uint8_t color, uint8_t namingPool,
                           uint8_t startSide,
                           const uint8_t *name, uint8_t nameLen);

/* Clear a team's metadata back to zero (in_use=0, color=0, pool=0,
 * empty name), publish CTRL_LOBBY_TEAM_META, and clear humans' ready
 * state. Callers (UDP PACKET_LOBBY_TEAM_CLEAR handler, SP-host
 * clientSimNetSendLobbyTeamClear). */
void serverSimClearTeamMeta(ServerSim *sim, BYTE teamId);

#endif
