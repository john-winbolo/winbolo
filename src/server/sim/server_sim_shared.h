/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 */

/*********************************************************
 *Name:          Server Simulation Shared Internals
 *Filename:      server_sim_shared.h
 *Author:        John Morrison
 *Purpose:
 *  The handful of functions that were file-local to
 *  server_sim.c and now cross a translation-unit boundary
 *  between the sources in this directory and their parent.
 *  This list is meant to stay short: every entry is a
 *  function that failed to stay inside one translation
 *  unit.
 *********************************************************/

#ifndef SERVER_SIM_SHARED_H
#define SERVER_SIM_SHARED_H

#include "server_sim_internal.h"   /* ServerSim, plus the sim types the signatures below name */

/* Defined in server_sim_callbacks.c — the callbacks GameSim invokes on the
 * server. serverSimInit takes the address of each one to fill
 * sim->sim.callbacks; nothing else calls them. */
void serverSimCbMessageAdd(void *ctx, messageType msgType,
                           langid topId, langid bodyId,
                           const MessageArgs *args);
void serverSimCbSoundDist(void *ctx, sndEffects value, BYTE mx, BYTE my);
void serverSimCbSoundDistShoot(void *ctx, BYTE mx, BYTE my, BYTE owner);
void serverSimCbSoundDistTankHit(void *ctx, BYTE mx, BYTE my, BYTE hitPlayer);
void serverSimCbMineVisible(void *ctx, BYTE mx, BYTE my, BYTE sourcePlayer);
void serverSimCbExplosion(void *ctx, BYTE mx, BYTE my, BYTE px, BYTE py);
void serverSimCbShellDeath(void *ctx, uint32_t fireTick,
                           uint32_t serverFireTick, BYTE owner,
                           WORLD impactWX, WORLD impactWY,
                           uint8_t outcome);
uint32_t serverSimCbShellFired(void *ctx, BYTE owner);
void serverSimCbTkExplosion(void *ctx, WORLD x, WORLD y,
                            TURNTYPE angle, BYTE length,
                            BYTE explodeType, BYTE creator);
void serverSimCbTankKill(void *ctx, BYTE killer, BYTE killed, BYTE deathCause,
                         BYTE carriedPills);
void serverSimCbRecordDamage(void *ctx, BYTE attacker, BYTE targetKind,
                             BYTE targetIndex, BYTE source,
                             uint16_t dealt, bool destroyed,
                             BYTE mapX, BYTE mapY);
void serverSimCbRecordPlayerAction(void *ctx, BYTE player, BYTE actionKind,
                                   BYTE mapX, BYTE mapY);
void serverSimCbRecordPillPickup(void *ctx, BYTE picker, BYTE pillIndex,
                                 BYTE mapX, BYTE mapY);
/* The three announcements that used to be built inline in bases.c, pillbox.c
 * and lgm.c. Each casts ctx once and raises the game event the shared site
 * used to raise for itself. index is the 0-based item[] slot. */
void serverSimCbBaseOwnerChanged(void *ctx, BYTE index, BYTE oldOwner,
                                 BYTE newOwner, BYTE captureClass,
                                 BYTE mapX, BYTE mapY);
void serverSimCbPillOwnerChanged(void *ctx, BYTE index, BYTE oldOwner,
                                 BYTE newOwner, BYTE captureClass,
                                 BYTE mapX, BYTE mapY);
void serverSimCbLgmDied(void *ctx, BYTE victim, BYTE killer,
                        BYTE mapX, BYTE mapY);
/* The facts that had no event before. Same file, same shape. */
void serverSimCbTankSpawned(void *ctx, BYTE player, BYTE mapX, BYTE mapY,
                            bool respawn);
void serverSimCbLgmLanded(void *ctx, BYTE player, BYTE mapX, BYTE mapY);
void serverSimCbPillPlaced(void *ctx, BYTE player, BYTE index, BYTE mapX,
                           BYTE mapY, BYTE armour);
void serverSimCbPillKilled(void *ctx, BYTE index, BYTE attacker);
void serverSimCbBuilt(void *ctx, BYTE player, BYTE action, BYTE mapX,
                      BYTE mapY);
void serverSimCbMineLaid(void *ctx, BYTE player, BYTE mapX, BYTE mapY);
void serverSimCbMineExploded(void *ctx, BYTE mapX, BYTE mapY, BYTE layer);
void serverSimCbTankHit(void *ctx, BYTE victim, BYTE attacker, BYTE cause,
                        BYTE amount, BYTE pill);
void serverSimCbCenterTank(void *ctx);
void serverSimCbConsoleMessage(void *ctx, char *msg);

/* The policy queries, also in server_sim_callbacks.c. These are where the
 * scenario policy is asked for the decisions shared code takes, so the sim
 * core can put the question without knowing a scenario exists. */
bool serverSimCbChooseStart(void *ctx, BYTE player, BYTE *startIdx);
bool serverSimCbSpawnLoadout(void *ctx, BYTE player, BYTE *shells,
                             BYTE *mines, BYTE *armour, BYTE *trees);
bool serverSimCbCanRespawn(void *ctx, BYTE player);
int  serverSimCbDamageScale(void *ctx, BYTE attacker, BYTE victim, BYTE cause,
                            BYTE pill);
bool serverSimCbCanBuild(void *ctx, BYTE player, BYTE action, BYTE mapX,
                         BYTE mapY, BYTE pillIdx);
bool serverSimCbCanCapture(void *ctx, BYTE kind, BYTE index, BYTE player);
bool serverSimCbCanDie(void *ctx, BYTE kind, BYTE index, BYTE killer,
                       BYTE cause, BYTE pill);
bool serverSimCbCanHit(void *ctx, BYTE attacker, BYTE kind, BYTE index,
                       BYTE pill);
int  serverSimCbPillDamageScale(void *ctx, BYTE attacker, BYTE index,
                                BYTE cause, BYTE pill);

/* Whether a newswire-worthy fact may be shown to players, also in
 * server_sim_callbacks.c. Unlike the queries above this one is not a GameSim
 * callback: nothing in shared code asks it. The server asks it where it builds
 * the fact — the two capture callbacks and the builder death here, the control
 * events in server_sim_control.c, server_sim_players.c and server_sim_round.c,
 * and the vote's own text in server_sim_vote.c — and stamps the answer as the
 * quiet byte the fact carries. TRUE with no policy registered, which is why a
 * plain map's newswire reads exactly as it always has.
 *
 * kind is an ANNOUNCE_KIND_* (scenario_defs.h) and subject and actor are what
 * that table says they are. This is the single bracketed call: the policy
 * enter and leave are here rather than at each of the sites. */
bool serverSimAnnounce(ServerSim *sim, BYTE kind, BYTE subject, BYTE actor);

/* Defined in server_sim_callbacks.c. Appends a packed attribution record to
 * the per-round buffer; the record callbacks above and serverSimAddEvent in
 * server_sim_control.c both feed it. */
void serverSimTrackAppend(ServerSim *sim, const void *rec, size_t n);

/* Defined in server_sim_snapshot.c — the base and pill collectors. They fill
 * the snapshot's periodic full sync, and the tick core in server_sim_tick.c reads
 * them each half-step to diff this frame's bases and pills against the last. */
int serverSimGetBases(ServerSim *sim, BaseSnapshot *out, int maxOut);
int serverSimGetPills(ServerSim *sim, PillSnapshot *out, int maxOut);

/* Defined in server_sim_round.c — round-lifecycle functions that were file-local
 * to server_sim.c until their definitions moved out. Each still has a caller
 * left behind there. */

/* Full lobby reset when the last human leaves — removes bots, restores the
 * startup settings snapshot, and unlocks the lobby. serverSimRemovePlayer in
 * server_sim_players.c calls this when the departing player was the last
 * human. */
void serverSimResetLobbyToDefaults(ServerSim *sim);

/* Defined in server_sim_lobby.c — the start sides a freshly opened lobby
 * gets: team 1 north and team 2 south, or team 1 east and team 2 west when
 * the map's squares that are not deep sea span more columns than rows;
 * both marked filled-in. Run when a lobby server starts up
 * (serverSimApplyInstanceConfig) and when the last human leaves
 * (serverSimResetLobbyToDefaults), so a side the host chose is never
 * written over while anyone is in the lobby. */
void serverSimApplyDefaultTeamSides(ServerSim *sim);

/* Defined in server_sim_lobby.c — on a map change, picks the default pair
 * again for the new map when teams 1 and 2 still hold it untouched (both
 * filled-in, in the default order), and publishes both teams. Leaves any
 * side the host chose. Returns true when the sides changed. */
bool serverSimRefreshDefaultTeamSides(ServerSim *sim);

/* The player slot every base owner is allied to when one side has swept
 * the map, or NEUTRAL when no side has. Same predicate as
 * serverSimCheckGameWin: a base at or below base_capture_armour is dead
 * and recapturable, so it does not count toward a sweep. The tick core in
 * server_sim_tick.c reads it each half-step to spot a win. */
BYTE serverSimWinningOwner(ServerSim *sim);

/* Caches the active map's BMAPBOLO MD5 so WinBolo.net can match the map against
 * its library. Called by serverSimCreate in server_sim.c and the map-reload
 * path in server_sim_maps.c, and by serverSimChangeMap alongside it. */
void serverSimCacheMapMd5FromFile(ServerSim *sim, const char *path);

/* Defined in server_sim_maps.c — turns a client-facing map relPath into the
 * file it names, redirecting the virtual "Uploads" folder to the configured
 * persist directory and the virtual "Workshop" folder to the directory
 * serverSimSetWorkshopMapDir named. The listing, the search and serverSimReadMapFile go
 * through it; so do the lobby's set-map command in server_command_dispatch.c
 * and the upload preview's use-local path in udp_server_dispatch.c, which
 * would otherwise open a different file from the one the client picked.
 * relPath must already have passed the caller's path-safety check, and out
 * holds at least FILENAME_MAX bytes. */
void serverSimResolveMapPath(const ServerSim *sim, const char *relPath,
                             char *out, size_t outSize);

/* Defined in server_sim_vote.c — the two publish helpers that were file-local
 * to server_sim.c until their definitions moved out. Each still has a caller
 * left behind there. */

/* Broadcasts the current per-slot map-skip votes as a CTRL_MAP_SKIP_STATE
 * event. serverSimRemovePlayer in server_sim_players.c calls it so a departing
 * player's vote drops off every client's tally. */
void publishMapSkipState(ServerSim *sim);

/* Server-originated English broadcast, published as CTRL_SERVER_TEXT to every
 * subscriber. The base-sweep branch of the tick core in server_sim_tick.c calls
 * it. */
void publishServerMessage(ServerSim *sim, const char *message);

/* The same broadcast for pre-rendered English text that can run past the
 * wire's chat cap (a long winners list): a message over the cap is cut on a
 * UTF-8 character boundary and ends in "...". The return-to-lobby win
 * message in server_sim_lifecycle_local.c sends through it, and so does
 * the UDP server's serverSendServerEnglishBroadcast. */
void publishServerEnglishBroadcast(ServerSim *sim, const char *message);

/* The same line held to one team (1..MAX_TANKS-1), through destTeam on the
 * event. The surrender vote's private notices use it, and so does the scenario
 * funnel's team-text arm in server_sim_scenario.c. */
void publishServerMessageToTeam(ServerSim *sim, const char *message,
                                BYTE teamId);

/* Defined in server_sim_control.c — the one control-event filler with a caller
 * outside that translation unit. */

/* Fills a CTRL_MAP_SKIP_STATE event with the current per-slot skip votes.
 * publishMapSkipState in server_sim_vote.c calls it; the encoding itself sits
 * with the other control-event fillers in server_sim_control.c. */
void serverSimFillMapSkipStateEvent(const ServerSim *sim, ControlEvent *evt);

/* Defined in server_sim_callbacks.c — the three-shot order detector.
 *
 * One of `owner`'s shells left the gun on SERVER tick `fireTick`. EVERY
 * shell counts, whatever it went on to hit, because the two quiet seconds
 * around the three shots ask what the player fired and not what it struck.
 * It also cancels an armed order, which is the "no fourth shot" half of the
 * rule.
 *
 * A SERVER tick. Never the client input tick a shell also carries: that one
 * is the client's own counter, it starts near zero on a mid-round joiner
 * and a modified client can send any value at all.
 *
 * serverSimCbShellFired calls it the moment shellsAddItem creates the
 * shell, with sim->tick, which is what keeps a shell that is still in the
 * air out of nobody's way. serverSimCbShellDeath calls it again on the
 * death with the shell's stamped serverFireTick, and the scenario funnel's
 * shell_expired arm calls it too; both are harmless repeats, because a tick
 * already in the log is ignored. */
void serverSimShotOrderShotFired(ServerSim *sim, BYTE owner, uint32_t fireTick);

/* One of `owner`'s shells, fired on SERVER tick `fireTick`, ran its full
 * range and died at (wx, wy) with nothing hit. Three of them on the SAME
 * open square, fired inside SHOT_ORDER_WINDOW_TICKS of each other and with a
 * quiet second in front of the first, ARM an order; serverSimShotOrderTick
 * sends it a quiet second after the third shot. serverSimCbShellDeath calls
 * this on the expiry outcome with the shell's serverFireTick, and the
 * scenario funnel's shell_expired arm calls it so a scripted round can post
 * the three shells without aiming a gun. */
void serverSimShotOrderNote(ServerSim *sim, BYTE owner, uint32_t fireTick,
                            WORLD wx, WORLD wy);

/* Sends any armed order whose quiet second has run out, and drops any whose
 * quiet second was broken. Called once per tick from serverSimTick. */
void serverSimShotOrderTick(ServerSim *sim);

/* Forgets every shot a player has in flight toward an order, and any order
 * already armed. Called when a player joins or leaves and when a round
 * starts. */
void serverSimShotOrderClear(ServerSim *sim, BYTE playerNum);

/* Defined in server_sim_lobby.c — the body of serverSimResolveBotConfigKeys,
 * taking the brain's modes already loaded instead of a path to read them
 * from. The public path form is the wrapper around this one and answers
 * exactly the same; NULL modes is what a brain with no modes.txt looks like
 * here, and gives BOT_CFG_KEYS_NO_MANIFEST as the load failing does.
 *
 * It exists so a caller applying several seats on one brain reads modes.txt
 * once for the lot: the scenario reseat in server_sim_scenario.c walks every
 * seat a template holds, and the lobby's own Add Bot already has the modes in
 * hand by the time it asks. */
BotConfigKeyResult serverSimResolveBotConfigKeysFromModes(
        const BrainModes *modes,
        const char *modeKey,
        const char *levelKey,
        uint8_t *ioMode,
        uint8_t *ioLevel);

/* Defined in server_sim_lobby.c — the body of serverSimResolveNewBotConfig,
 * taking the brain's modes already loaded. NULL modes is a brain with no
 * modes.txt and answers false, as the path form does when the read fails.
 * The scenario's seat loops call it with their one read per brain. */
bool serverSimResolveNewBotConfigFromModes(const ServerSim *sim, int team,
                                           const BrainModes *modes,
                                           bool honourManualPick,
                                           uint8_t *ioMode,
                                           uint8_t *ioLevel);

/* ── One read of modes.txt per brain, for the length of one walk ─────────
 *
 * Seating a lobby, reconciling one and following a game type change all walk
 * every seat, and the seats nearly always name the same brain. Read once per
 * brain here and the walk costs one parse rather than one per seat.
 *
 * It is a local of the loop that builds it and dies with it: nothing about a
 * brain's modes.txt is remembered from one walk to the next, so editing a
 * manifest and reselecting the scenario still picks the edit up.
 *
 * A handful of entries rather than one per seat. A lobby's teams nearly
 * always name one brain between them, and the entries here cover a server
 * default plus a few teams that name their own; a lobby that names more
 * distinct brains than this holds answers exactly the same and pays one
 * extra read for the ones past the end. Size is the reason it is not wider:
 * BrainModes is close to 4 KB on its own, and every walk is reached from the
 * lobby command dispatcher, so this frame sits on top of that whole chain.
 *
 * The path is as long as a bot's brainPath (bot_manager.h) and SCN_PATH_MAX,
 * which are the two places a path comes from. */
#define BRAIN_MODES_CACHED 4

typedef struct {
    char       path[256];
    BrainModes modes;
    bool       haveModes;      /* the brain ships a modes.txt */
} BrainModesCacheEntry;

typedef struct {
    BrainModesCacheEntry entry[BRAIN_MODES_CACHED];
    int                  count;   /* set to 0 before the first ask */
} BrainModesCache;

/* Defined in server_sim_lobby.c. The modes for brainPath, read the first
 * time this cache is asked for it. NULL for a brain with no modes.txt, which
 * is what the resolver reads as "no manifest" — the same answer the seat
 * loops get when a load fails there. */
const BrainModes *serverSimBrainModesCached(BrainModesCache *cache,
                                            const char *brainPath);

/* Defined in server_sim.c — the entries owned by the parent rather than by a
 * source in this directory. */

/* Arms the per-thread active-sim slot that serverSimGetActive reads back; the
 * activeSim pointer itself stays file-local to server_sim.c, so a sim/ source
 * that needs to arm it goes through this. serverSimStartGame in
 * server_sim_round.c and serverSimPublishControl in server_sim_control.c are
 * the callers. */
void serverSimSetActive(ServerSim *sim);

/* Records a terrain change into the active sim's dedicated map-event buffer,
 * where map events never compete with sound and game events for slots.
 * simRunHalfStep in server_sim_tick.c installs it through mapSetChangeCallback
 * for the duration of a running half-step. It reads the activeSim slot
 * directly, which is why it stays beside that slot in server_sim.c rather than
 * travelling with its caller. */
void simMapChangeCallback(BYTE x, BYTE y, BYTE terrain);

#endif
