/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 2 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 */

/*********************************************************
 *Name:          Client Sim Control
 *Filename:      client_sim_control.c
 *Purpose:
 *  Dispatcher implementation. Mutates ClientSim game state
 *  only — UI, transport-internal, and wire-protocol
 *  housekeeping live in their respective callers.
 *
 *  All wire decoders in transport_udp_client.c build a
 *  ControlEvent and route through clientSimApplyControl.
 *  They do not mutate ClientSim state directly and do not
 *  call frontEnd* callbacks directly. SP, bots, and network
 *  converge on this single funnel — see docs/ARCHITECTURE.md
 *  "Adding a new server event" for the recipe.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>
#include "client_sim_control.h"
#include "client_sim_internal.h"
#include "client_sim.h"
#include "frontend.h"    /* frontEndAudioReturningToLobby */
#include "messages.h"
#include "netpacks.h"
#include "players.h"
#include "server_sim.h"  /* serverSimGetCompressedMap / serverSimGetMapName */
#include "../gui/lang.h" /* langGetTextFmt, STR_DLGLOBBY_HOST_CHANGED_FMT */
#include "../steam/steam_wrapper.h"
#include "global.h"
#include "../common/wb_log.h"
#include "../common/mp_diag_log.h"

/* Localized lobby team label: the host-assigned team name, or "Team N"
 * when the team is unnamed (matching the lobby roster header). */
static void clientSimLobbyTeamLabel(const ClientSim *cs, BYTE team,
                                    char *out, size_t outLen) {
    const char *name = clientSimGetLobbyTeamName(cs, team);
    if (name != NULL && name[0] != '\0') {
        SDL_strlcpy(out, name, outLen);
        return;
    }
    MessageArgs args;
    memset(&args, 0, sizeof(args));
    args.number = team;
    SDL_strlcpy(out, langGetTextFmt(STR_DLGLOBBY_TEAM_HEADER, &args), outLen);
}

void clientSimApplyControl(ClientSim *cs, const ControlEvent *evt) {
    if (cs == NULL || evt == NULL) {
        return;
    }

    /* Test-only observer hook (set via clientSimSetControlObserver).
     * Fires before any state mutation so the observed stream matches
     * what the dispatcher actually receives, including the self-skip
     * branch below. The callback gets a const event and returns void —
     * it cannot influence dispatch. */
    if (cs->controlObserverCb != NULL) {
        cs->controlObserverCb(cs->controlObserverCtx, evt);
    }

    /* Transport-internal observer hook. Same contract as the test
     * observer, separate slot so the two don't displace each other.
     * Wired by the UDP transport for joinState / re-join / WBN re-auth
     * side effects that don't belong in the bolo lib subscriber. */
    if (cs->transportObserverCb != NULL) {
        cs->transportObserverCb(cs->transportObserverCtx, evt);
    }

    /* Self-skip on CTRL_PLAYER_LEAVE only: a recipient must not process
     * their own departure. CTRL_PLAYER_JOIN is allowed for self because
     * playersSetPlayer's self-branch (inUse already TRUE, iMyPlayerNum
     * == iPlayerNum) updates only the location field and leaves
     * position, name, and alliances untouched — making it the correct
     * sink for country-code refreshes. CTRL_LOBBY_SLOT and
     * CTRL_PLAYER_NAME similarly update self because the server is the
     * source of truth for those fields. */
    switch (evt->type) {
    case CTRL_PLAYER_LEAVE:
        if (evt->u.playerLeave.playerNum == cs->myPlayerNum) return;
        break;
    default:
        break;
    }

    /* Wire-supplied player numbers index the fixed players array
     * (item[MAX_TANKS]) and the lobbySlots[MAX_TANKS] mirror; an out-of-range
     * value from a hostile or buggy server would index out of bounds in the
     * handlers below (e.g. playersLeaveAlliance / playersLeaveGame write
     * item[playerNum].allie; CTRL_LOBBY_SLOT writes lobbySlots[playerNum]).
     * Reject such events at this trust boundary — a valid server never sends
     * them. The handlers' own indices that derive from loop counters or the
     * server-assigned myPlayerNum are in range by construction. */
    switch (evt->type) {
    case CTRL_ALLIANCE_LEAVE:
        if (evt->u.allianceLeave.playerNum >= MAX_TANKS) return;
        break;
    case CTRL_ALLIANCE_ACCEPT:
        if (evt->u.allianceAccept.acceptedBy >= MAX_TANKS ||
            evt->u.allianceAccept.newMember >= MAX_TANKS) return;
        break;
    case CTRL_ALLIANCE_REQUEST:
        if (evt->u.allianceRequest.toPlayer >= MAX_TANKS ||
            evt->u.allianceRequest.fromPlayer >= MAX_TANKS) return;
        break;
    case CTRL_PLAYER_JOIN:
        if (evt->u.playerJoin.playerNum >= MAX_TANKS) return;
        break;
    case CTRL_PLAYER_NAME:
        if (evt->u.playerName.playerNum >= MAX_TANKS) return;
        break;
    case CTRL_PLAYER_LEAVE:
        if (evt->u.playerLeave.playerNum >= MAX_TANKS) return;
        break;
    case CTRL_LOBBY_SLOT:
        if (evt->u.lobbySlot.playerNum >= MAX_TANKS) return;
        break;
    default:
        break;
    }

    switch (evt->type) {
    case CTRL_ALLIANCE_REQUEST:
        /* Flag a pending request for the addressed slot. The transport
         * observer (transport_udp_client.c) pops the SDL dialog for
         * UDP-connected clients; the host's local-transport ClientSim
         * exposes the field via clientSimGetPendingAllianceRequest so
         * its frontend can poll. In-process subscribers (bots) see
         * every publish; gate on the addressed slot so only the
         * intended recipient flags. */
        if (evt->u.allianceRequest.toPlayer == cs->myPlayerNum) {
            cs->pendingAllianceRequestFrom = evt->u.allianceRequest.fromPlayer;
        }
        break;

    case CTRL_ALLIANCE_ACCEPT:
        playersAcceptAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                              evt->u.allianceAccept.acceptedBy,
                              evt->u.allianceAccept.newMember,
                              FALSE);
        break;

    case CTRL_ALLIANCE_LEAVE:
        playersLeaveAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                             evt->u.allianceLeave.playerNum, FALSE);
        break;

    case CTRL_ALLIANCE_RESET: {
        /* Mirror serverSimReapplyTeamAlliances on the receiver: clear
         * every existing alliance, then re-accept per the matrix the
         * server sent. Matrix is symmetric — iterate the upper triangle
         * only. Self-bit is informational (slot is connected) and does
         * not produce an accept. */
        BYTE i, j;
        for (i = 0; i < MAX_TANKS; i++) {
            playersLeaveAlliance(&cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                                 i, FALSE);
        }
        for (i = 0; i < MAX_TANKS; i++) {
            uint16_t mask = evt->u.allianceReset.allies[i];
            for (j = (BYTE)(i + 1); j < MAX_TANKS; j++) {
                if (mask & (uint16_t)(1u << j)) {
                    playersAcceptAlliance(&cs->sim, &cs->sim.plyrs,
                                          cs->myPlayerNum, i, j, FALSE);
                }
            }
        }
        break;
    }

    case CTRL_PLAYER_JOIN: {
        BYTE pNum = evt->u.playerJoin.playerNum;
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        char ccBuf[3];
        BYTE allies[MAX_TANKS];
        BYTE numAllies = evt->u.playerJoin.numAllies;
        memcpy(nameBuf, evt->u.playerJoin.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        ccBuf[0] = evt->u.playerJoin.country[0];
        ccBuf[1] = evt->u.playerJoin.country[1];
        ccBuf[2] = '\0';
        if (numAllies > MAX_TANKS) numAllies = MAX_TANKS;
        if (numAllies > 0) {
            memcpy(allies, evt->u.playerJoin.allies, numAllies);
        }
        playersSetClientType(&cs->sim.plyrs, pNum, evt->u.playerJoin.clientType);
        playersSetClientFlags(&cs->sim.plyrs, pNum, evt->u.playerJoin.clientFlags);
        /* A spectator has no self: pass a selfPlayer that matches no real
         * slot so playersSetPlayer's self-branch (which would skip the
         * name/alliance refresh) can never swallow a real slot-0 join.
         * 0xFF is compare-only there — playersScreenAllience guards its
         * one index with selfPlayer < MAX_TANKS. */
        playersSetPlayer(cs, &cs->sim.plyrs,
                         cs->isSpectator ? (BYTE)0xFF : cs->myPlayerNum, pNum,
                         nameBuf, ccBuf,
                         0, 0, 0, 0, 0, FALSE,
                         numAllies, numAllies > 0 ? allies : NULL, FALSE);
        if ((cs->isSpectator || pNum != cs->myPlayerNum) && cs->inLobby) {
            char joinMsg[PACKET_MAX_PLAYER_NAME + 16];
            snprintf(joinMsg, sizeof(joinMsg), "%s has joined.", nameBuf);
            clientSimAppendLobbyChat(cs, "***", joinMsg);
            if (cs->lobbySyncSettled) frontEndPlaySound(cs, lobbyPlayerJoin);
        }
        break;
    }

    case CTRL_PLAYER_NAME: {
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        memcpy(nameBuf, evt->u.playerName.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        playersSetPlayerName(cs, &cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                             evt->u.playerName.playerNum, nameBuf, FALSE);
        break;
    }

    case CTRL_LOBBY_SLOT:
        mpDiagLog("[clientSim] APPLY CTRL_LOBBY_SLOT cs=%p myPlayerNum=%d slot=%u team=%u ready=%d isBot=%d name='%.12s' connected=%d",
                  (void *)cs, (int)cs->myPlayerNum,
                  (unsigned)evt->u.lobbySlot.playerNum,
                  (unsigned)evt->u.lobbySlot.slot.teamNumber,
                  (int)evt->u.lobbySlot.slot.ready,
                  (int)evt->u.lobbySlot.slot.isBot,
                  evt->u.lobbySlot.slot.playerName,
                  (int)evt->u.lobbySlot.slot.connected);
        {
            /* Announce team membership changes as Team-chat system lines,
             * generated locally from the slot update so the in-process host
             * sees them too. cs->lobbySlots[pn] is still the pre-update
             * mirror here (the overwrite below is last), so old-vs-new gives
             * the transition. CTRL_LOBBY_SLOT is republished on every lobby
             * change, so the oldTeam != newTeam guard fires only on a real
             * transition. This one site covers join (oldTeam 0), leave
             * (newTeam 0), and switch (both). Lines are shown only to the
             * affected team's members. */
            BYTE pn = evt->u.lobbySlot.playerNum;
            const ClientLobbySlot *oldSlot = &cs->lobbySlots[pn];
            const ClientLobbySlot *newSlot = &evt->u.lobbySlot.slot;
            BYTE oldTeam = oldSlot->connected ? oldSlot->teamNumber : 0;
            BYTE newTeam = newSlot->connected ? newSlot->teamNumber : 0;
            BYTE myPN    = clientSimGetMyPlayerNum(cs);
            const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPN);
            BYTE myTeam  = (mySlot != NULL) ? mySlot->teamNumber : 0;

            /* A tankless spectator belongs to no team, and its myPN/myTeam
             * alias real slot 0, so every oldTeam/newTeam == myTeam test
             * below is meaningless for it (and would emit team-scoped lines
             * a viewer must never see). Skip the team-membership chatter
             * entirely for a spectator. */
            if (cs->inLobby && !cs->isSpectator && pn != myPN &&
                oldTeam != newTeam) {
                if (oldTeam != 0 && oldTeam == myTeam) {
                    MessageArgs a;
                    memset(&a, 0, sizeof(a));
                    SDL_strlcpy(a.playerName, oldSlot->playerName,
                                sizeof(a.playerName));
                    clientSimLobbyTeamLabel(cs, oldTeam, a.string1,
                                            sizeof(a.string1));
                    clientSimAppendLobbyTeamChat(cs, "***",
                        langGetTextFmt(STR_DLGLOBBY_TEAM_LEFT_FMT, &a));
                }
                if (newTeam != 0 && newTeam == myTeam) {
                    MessageArgs a;
                    memset(&a, 0, sizeof(a));
                    SDL_strlcpy(a.playerName, newSlot->playerName,
                                sizeof(a.playerName));
                    clientSimLobbyTeamLabel(cs, newTeam, a.string1,
                                            sizeof(a.string1));
                    clientSimAppendLobbyTeamChat(cs, "***",
                        langGetTextFmt(STR_DLGLOBBY_TEAM_JOINED_FMT, &a));
                }
            }

            /* Ready toggle by another connected player -> audio cue. Gated on
             * lobbySyncSettled so the join-replay slot burst is silent, on
             * pn != myPN so the local player's own toggle doesn't self-sound,
             * and on both slots being connected so join/leave aren't misread
             * as a ready change. Runs before the commit below, while oldSlot
             * still holds the pre-update state. */
            if (cs->inLobby && (cs->isSpectator || pn != myPN) &&
                cs->lobbySyncSettled &&
                oldSlot->connected && newSlot->connected &&
                oldSlot->ready != newSlot->ready) {
                frontEndPlaySound(cs, newSlot->ready ? lobbyReady : lobbyUnready);
            }
        }
        cs->lobbySlots[evt->u.lobbySlot.playerNum] = evt->u.lobbySlot.slot;
        break;

    case CTRL_SPECTATOR_SLOT:
        if (evt->u.spectatorSlot.specIdx < MAX_SPECTATORS) {
            cs->spectatorSlots[evt->u.spectatorSlot.specIdx] = evt->u.spectatorSlot.slot;
        }
        break;

    case CTRL_SPECTATOR_CHAT: {
        /* A spectator's lobby chat line. Resolve the sender's name from the
         * mirrored spectator roster and render it [Spectator]-tagged in the
         * shared lobby chat log. No self-skip: the sending spectator owns no
         * slot to echo locally, so seeing its own round-trip line is intended. */
        const ClientSpectatorSlot *sp =
            clientSimGetSpectatorSlot(cs, evt->u.spectatorChat.specIdx);
        const char *senderName =
            (sp && sp->connected && sp->playerName[0]) ? sp->playerName : "?";
        char msg[PACKET_MAX_CHAT_MESSAGE + 1];
        uint16_t copyLen = evt->u.spectatorChat.bodyLen;
        if (copyLen > PACKET_MAX_CHAT_MESSAGE) copyLen = PACKET_MAX_CHAT_MESSAGE;
        if (copyLen > 0) memcpy(msg, evt->u.spectatorChat.body, copyLen);
        msg[copyLen] = '\0';
        {
            MessageArgs args = {0};
            char tagged[PACKET_MAX_PLAYER_NAME + 32];
            snprintf(args.string1, sizeof(args.string1), "%s", senderName);
            SDL_strlcpy(tagged,
                        langGetTextFmt(STR_DLGLOBBY_SPECTATOR_TAG, &args),
                        sizeof(tagged));
            clientSimAppendLobbyChat(cs, tagged, msg);
        }
        if (clientSimIsInLobby(cs) && cs->lobbySyncSettled) {
            frontEndPlaySound(cs, lobbyChatReceived);
        }
        break;
    }

    case CTRL_LOBBY_SETTINGS:
        strncpy(cs->mapName, evt->u.lobbySettings.mapName, MAP_STR_SIZE - 1);
        cs->mapName[MAP_STR_SIZE - 1] = '\0';
        cs->lobbyGameType    = evt->u.lobbySettings.lobbyGameType;
        cs->lobbyHiddenMines = evt->u.lobbySettings.lobbyHiddenMines;
        cs->lobbyAiType      = evt->u.lobbySettings.lobbyAiType;
        cs->lobbyTimeLimit   = evt->u.lobbySettings.lobbyTimeLimit;
        cs->lobbyPillCount   = evt->u.lobbySettings.lobbyPillCount;
        cs->lobbyBaseCount   = evt->u.lobbySettings.lobbyBaseCount;
        cs->lobbyStartCount  = evt->u.lobbySettings.lobbyStartCount;
        cs->mapSkipAvailable = evt->u.lobbySettings.mapSkipAvailable;
        /* Server lobby capability ("does this server run a lobby"), distinct
         * from cs->inLobby (the current phase). Carried by the hasLobby wire
         * field but stored separately so it survives phase changes and can
         * gate the in-game vote UI — votes are meaningless without a lobby
         * and the server rejects them there. */
        cs->lobbyAvailable = evt->u.lobbySettings.hasLobby;
        /* netStat and inLobby are owned by the CTRL_GAME_PHASE_* events,
         * which fire on every live transition and which the join sync
         * replay always delivers before this settings snapshot. Adopting
         * them here clobbered the phase: the server fills inLobby from
         * lobbyEnabled ("this server has a lobby", true mid-game), so a
         * mid-game joiner's replay applied PHASE_RUNNING (inLobby=false)
         * and then this event flipped it back to true, dropping the
         * client into the lobby screen. */
        cs->lobbyOpenHost            = evt->u.lobbySettings.lobbyOpenHost;
        {
            /* Announce a host handoff as a lobby system line. The host slot
             * arrives here for every cause (explicit transfer, host-leave
             * reassignment, server console command), so this one diff covers
             * them all. Suppressed on the first settings snapshot of a lobby
             * session (lobbyHostSlotKnown) so the initial host assignment is
             * not reported as a change. */
            BYTE oldHost = cs->lobbyHostSlot;
            BYTE newHost = evt->u.lobbySettings.hostSlot;
            cs->lobbyHostSlot = newHost;
            if (cs->lobbyHostSlotKnown && cs->inLobby && newHost != oldHost) {
                const ClientLobbySlot *ns = clientSimGetLobbySlot(cs, newHost);
                if (ns != NULL && ns->connected && !ns->isBot &&
                    ns->playerName[0] != '\0') {
                    MessageArgs args;
                    memset(&args, 0, sizeof(args));
                    SDL_strlcpy(args.playerName, ns->playerName,
                                sizeof(args.playerName));
                    clientSimAppendLobbyChat(cs, "***",
                        langGetTextFmt(STR_DLGLOBBY_HOST_CHANGED_FMT, &args));
                }
            }
            cs->lobbyHostSlotKnown = true;
        }
        cs->lobbyAutoLockOnGameStart = evt->u.lobbySettings.lobbyAutoLockOnGameStart;
        cs->lobbyRanked          = evt->u.lobbySettings.lobbyRanked;
        cs->lobbyAllowNewPlayers = evt->u.lobbySettings.lobbyAllowNewPlayers;
        cs->lobbyWbnAvailable = evt->u.lobbySettings.lobbyWbnAvailable;
        cs->lobbyServerLocks         = evt->u.lobbySettings.lobbyServerLocks;
        cs->uploadPolicy             = evt->u.lobbySettings.uploadPolicy;
        /* The policy byte is stored raw, with no range check. This mirror
         * drives nothing the server does not enforce for itself, so a value
         * outside the enum can only make the local display wrong, never more
         * permissive: a server clamps the byte in serverSimApplyLobbySetting
         * before it reaches the wire, and each reader degrades safely on its
         * own — the overview's region test treats a policy it does not know as
         * off, while the view keys and buttons ask only whether the policy is
         * off, so they stay live and the server declines to send. */
        for (int vc = 0; vc < VIEW_CATEGORY_COUNT; vc++) {
            cs->viewPolicy[vc]    = evt->u.lobbySettings.viewPolicy[vc];
            cs->viewDecaySecs[vc] = evt->u.lobbySettings.viewDecaySecs[vc];
        }
        /* Adopt the server's authoritative game-timing settings. The
         * server's lobbyTimeLimit field carries its current remaining
         * gameLength (it decrements every running tick), so applying it
         * mid-game refreshes the client's local view rather than
         * clobbering it. The previous inLobby && !netRunning gate
         * prevented mid-game sync-replay arrivals from ever delivering
         * gmeLength to a late joiner; their default zero then fired
         * the gmeLength==0 game-over branch in clientUiOnTick the
         * first display tick after the snapshot landed. */
        clientSimSetGameType(cs,       evt->u.lobbySettings.lobbyGameType);
        clientSimSetHiddenMines(cs,    evt->u.lobbySettings.lobbyHiddenMines);
        clientSimSetAiType(cs,         (aiType)evt->u.lobbySettings.lobbyAiType);
        clientSimSetGmeStartDelay(cs,  evt->u.lobbySettings.lobbyStartDelay);
        clientSimSetGmeLength(cs,      evt->u.lobbySettings.lobbyTimeLimit);
        /* A fresh lobby snapshot supersedes any pending balance
         * proposal — the server only re-publishes if it still has a
         * live one. Otherwise the client's "Teams balanced" success
         * label would linger past its intended one-shot lifetime. */
        cs->balanceProposalActive = false;
        memset(cs->balanceProposal, 0, sizeof(cs->balanceProposal));
        break;

    case CTRL_LOBBY_TEAM_META: {
        uint8_t t = evt->u.lobbyTeamMeta.teamId;
        if (t == 0 || t >= MAX_TANKS) break;
        cs->lobbyTeamInUse[t] = evt->u.lobbyTeamMeta.in_use;
        cs->lobbyTeamColor[t] = evt->u.lobbyTeamMeta.color;
        cs->lobbyTeamPool[t]  = evt->u.lobbyTeamMeta.namingPool;
        strncpy(cs->lobbyTeamName[t], evt->u.lobbyTeamMeta.name,
                sizeof(cs->lobbyTeamName[t]) - 1);
        cs->lobbyTeamName[t][sizeof(cs->lobbyTeamName[t]) - 1] = '\0';
        break;
    }

    case CTRL_LOBBY_BOT_CONFIG: {
        uint8_t s = evt->u.lobbyBotConfig.slot;
        if (s >= MAX_TANKS) break;
        cs->lobbyBotDifficulty[s]  = evt->u.lobbyBotConfig.difficulty;
        cs->lobbyBotPersonality[s] = evt->u.lobbyBotConfig.personality;
        /* Bot display name flows through the lobbySlot path; the
         * name field on this event is informational and ignored here
         * to avoid stomping the slot's playerName on a partial mirror. */
        break;
    }

    case CTRL_LOBBY_BOT_BRAIN: {
        uint8_t s   = evt->u.lobbyBotBrain.slot;
        uint8_t idx = evt->u.lobbyBotBrain.brainIdx;
        if (s >= MAX_TANKS) break;
        /* Clamp out-of-range catalogue indices to the 0xFF sentinel so
         * downstream consumers never see a byte that points past the
         * brain catalogue. Matches the server-side write-time clamp in
         * serverSimSetBotBrainIdxFor. */
        if (idx != 0xFF && idx >= cs->lobbyBrainList.count) idx = 0xFF;
        cs->lobbyBotBrainIdx[s] = idx;
        break;
    }

    case CTRL_LOBBY_BRAIN_LIST:
        cs->lobbyBrainList = evt->u.lobbyBrainList.list;
        break;

    case CTRL_ROUND_STATS:
        cs->lastRoundStats = evt->u.roundStats;
        cs->lastRoundStatsValid = true;
        /* Nothing reads wbnLogKey yet, so this is the only way to see
         * whether the server published the finished round's key. A prefix
         * is enough to tell two rounds apart and to match the key in the
         * round's .wbv header; the whole key never goes to the log. */
        WB_LOG_INFO(WB_LOG_CAT_CLIENT,
                    "CTRL_ROUND_STATS wbnLogKey %s prefix='%.6s'",
                    cs->lastRoundStats.wbnLogKey[0] != '\0' ? "present"
                                                            : "empty",
                    cs->lastRoundStats.wbnLogKey);
        break;

    case CTRL_ROUND_RATING_POSTED: {
        const char *key = evt->u.ratingPosted.key;
        /* The poster's own client re-armed its fetch when its POST returned,
         * so a self-nudge would only make it read the round back twice. */
        if (evt->u.ratingPosted.fromPlayer == cs->myPlayerNum) break;
        /* A nudge for a round this client has already moved past must not
         * disturb the one its recap is showing. */
        if (key[0] == '\0' || cs->lastRoundStats.wbnLogKey[0] == '\0') break;
        if (strncmp(key, cs->lastRoundStats.wbnLogKey,
                    sizeof(cs->lastRoundStats.wbnLogKey)) != 0) {
            break;
        }
        cs->ratingPostedSeq++;
        WB_LOG_INFO(WB_LOG_CAT_CLIENT,
                    "CTRL_ROUND_RATING_POSTED from %u key prefix='%.6s'",
                    (unsigned)evt->u.ratingPosted.fromPlayer, key);
        break;
    }

    case CTRL_LOBBY_BOT_POOL_CHUNK: {
        /* Reassemble in-order fragments of the server's compressed
         * bot-pool catalog; install on the final fragment so the lobby
         * dropdown reflects the SERVER's pools. Fragments ride the
         * reliable, ordered control channel, so seq is monotonic; any
         * gap/mismatch aborts the in-progress reassembly. */
        uint8_t  seq   = evt->u.lobbyBotPoolChunk.seq;
        uint8_t  count = evt->u.lobbyBotPoolChunk.count;
        uint16_t fl    = evt->u.lobbyBotPoolChunk.fragLen;
        if (count == 0) break;
        if (seq == 0) {
            cs->lobbyPoolChunkExpected = count;
            cs->lobbyPoolNextSeq = 0;
            cs->lobbyPoolBlobLen = 0;
        }
        if (seq != cs->lobbyPoolNextSeq ||
            count != cs->lobbyPoolChunkExpected ||
            cs->lobbyPoolBlobLen + fl > sizeof(cs->lobbyPoolBlob)) {
            cs->lobbyPoolChunkExpected = 0;   /* abort */
            cs->lobbyPoolNextSeq = 0;
            cs->lobbyPoolBlobLen = 0;
            break;
        }
        if (fl > 0) {
            memcpy(cs->lobbyPoolBlob + cs->lobbyPoolBlobLen,
                   evt->u.lobbyBotPoolChunk.frag, fl);
            cs->lobbyPoolBlobLen += fl;
        }
        cs->lobbyPoolNextSeq++;
        if (cs->lobbyPoolNextSeq == count) {
            lobbyBotPoolsDeserializeInstall(cs->lobbyPoolBlob,
                                            (int)cs->lobbyPoolBlobLen, NULL);
            cs->lobbyPoolChunkExpected = 0;
            cs->lobbyPoolNextSeq = 0;
            cs->lobbyPoolBlobLen = 0;
        }
        break;
    }

    case CTRL_LOBBY_MAP_CHANGE:
        cs->mapDownloadComplete = false;
        memset(cs->mapSkipVotes, 0, sizeof(cs->mapSkipVotes));
        cs->mapSkipMyVote = false;
        if (!cs->isUdpTransport && cs->boundServerSim != NULL) {
            /* Local transport: the server is in-process. Pull the
             * freshly-compressed map directly and reinstall — there is
             * no MAP_DOWNLOAD wire path to wait on. */
            BYTE buf[MAP_DOWNLOAD_MAX_SIZE];
            int  len = serverSimGetCompressedMap(cs->boundServerSim, buf,
                                                 (int)sizeof(buf));
            if (len > 0) {
                installCompressedMap(cs, buf, len,
                                     serverSimGetMapName(cs->boundServerSim),
                                     /*initViewport=*/true);
                cs->mapDownloadComplete = true;
            }
        }
        break;

    case CTRL_MAP_DOWNLOAD_COMPLETE:
        cs->mapDownloadComplete = true;
        break;

    case CTRL_BALANCE_PROPOSAL: {
        bool anyNonZero = false;
        int i;
        memcpy(cs->balanceProposal, evt->u.balanceProposal.teamForSlot, MAX_TANKS);
        for (i = 0; i < MAX_TANKS; i++) {
            if (cs->balanceProposal[i] != 0) {
                anyNonZero = true;
                break;
            }
        }
        cs->balanceProposalActive = anyNonZero;
        /* Latch a one-shot timestamp the lobby UI reads to show a brief
         * "Teams balanced" success label next to the Balance-from-WBN
         * button. The server auto-applies and immediately clears the
         * proposal, so an in-process host's render thread would only
         * ever see balanceProposalActive=false; this timestamp survives
         * that race. Set only on a non-empty arrival so the matching
         * clear publish doesn't overwrite it. */
        if (anyNonZero) {
            cs->lastBalanceProposalArrivedMs = SDL_GetTicks();
        }
        break;
    }

    case CTRL_BALANCE_FAILED:
        /* Latched so the lobby UI's per-frame status row can flip the
         * "Asking WBN…" pill to the failure label as soon as we hear
         * back from the server — no need to wait out the 8 s NOREPLY
         * timeout. The control event is unicast to the host slot, so
         * remote clients won't see it. */
        cs->lastBalanceFailedMs     = SDL_GetTicks();
        cs->lastBalanceFailedReason = evt->u.balanceFailed.reasonCode;
        break;

    case CTRL_MAP_SKIP_STATE: {
        int i;
        for (i = 0; i < MAX_TANKS; i++) {
            cs->mapSkipVotes[i] = evt->u.mapSkipState.votes[i] ? true : false;
        }
        cs->mapSkipMyVote =
            cs->isSpectator ? false : cs->mapSkipVotes[cs->myPlayerNum];
        break;
    }

    case CTRL_GAME_VOTE_STATE: {
        uint8_t k = evt->u.gameVoteState.kind;
        int idx = (k == GAME_VOTE_KIND_BACK_TO_LOBBY) ? 0
                : (k == GAME_VOTE_KIND_SURRENDER)     ? 1 : -1;
        if (idx < 0) break;
        struct ClientGameVote *gv = &cs->gameVotes[idx];
        uint8_t prevActive = gv->active;
        bool wasRunning = (prevActive == GAME_VOTE_ACTIVE_RUNNING);
        gv->kind             = evt->u.gameVoteState.kind;
        gv->active           = evt->u.gameVoteState.active;
        gv->triggerSrc       = evt->u.gameVoteState.triggerSrc;
        gv->teamId           = evt->u.gameVoteState.teamId;
        gv->threshold        = evt->u.gameVoteState.threshold;
        gv->yesCount         = evt->u.gameVoteState.yesCount;
        gv->noCount          = evt->u.gameVoteState.noCount;
        gv->eligibleCount    = evt->u.gameVoteState.eligibleCount;
        gv->secondsRemaining = evt->u.gameVoteState.secondsRemaining;
        gv->votes            = evt->u.gameVoteState.votes;
        /* Auto-pop the widget when a vote starts — players can X to hide. */
        if (!wasRunning && gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            gv->widgetVisible  = true;
            gv->concludedAtMs  = 0;
        }
        /* Record conclusion time so the widget can auto-hide a few
         * seconds after a pass/fail/cancel. */
        if (wasRunning && gv->active != GAME_VOTE_ACTIVE_RUNNING) {
            gv->concludedAtMs = SDL_GetTicks();
        }
        /* Newswire notifications on state transitions so players who
         * have the widget closed still see what happened.
         *
         * Surrender votes are private to the surrendering team, so the
         * announcements are too: the opposing team learns of a surrender
         * only from the public "*** Team X has surrendered. ***" outcome,
         * and a failed surrender vote stays invisible to them. The server
         * already withholds the state event from non-members; this gate
         * covers the in-process bus (host's own ClientSim, which the
         * per-client wire filter never sees). The mirror above stays
         * unconditional — only the display is gated — so nothing depends
         * on the local team being known at sync-replay time. */
        if (!clientSimMayAnswerGameVote(cs, k, gv->teamId)) {
            break;
        }
        const char *kindLabel = (k == GAME_VOTE_KIND_BACK_TO_LOBBY)
                                ? "Return-to-lobby vote" : "Surrender vote";
        if (!wasRunning && gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            const char *trig =
                (gv->triggerSrc == GAME_VOTE_TRIGGER_BASE_MONOPOLY)
                    ? "started (one team controls every base)"
                : (gv->triggerSrc == GAME_VOTE_TRIGGER_POST_SURRENDER)
                    ? "started (following a surrender)"
                : "started";
            char body[128];
            snprintf(body, sizeof(body), "%s %s.", kindLabel, trig);
            clientMessageAdd(clientSimGetMessages(cs), newsWireMessage,
                             (char *)"Vote", body);
        } else if (wasRunning && gv->active != GAME_VOTE_ACTIVE_RUNNING) {
            const char *outcome =
                (gv->active == GAME_VOTE_ACTIVE_PASSED)    ? "passed"
              : (gv->active == GAME_VOTE_ACTIVE_FAILED)    ? "failed"
              : (gv->active == GAME_VOTE_ACTIVE_CANCELLED) ? "cancelled"
              :                                              "ended";
            char body[160];
            snprintf(body, sizeof(body), "%s %s (%u / %u).",
                     kindLabel, outcome,
                     (unsigned)gv->yesCount, (unsigned)gv->threshold);
            clientMessageAdd(clientSimGetMessages(cs), newsWireMessage,
                             (char *)"Vote", body);
        }
        break;
    }

    case CTRL_GAME_PHASE_LOBBY:
        cs->netStat = netLobby;
        cs->countdownSeconds = 0;
        /* Mirror of the RUNNING arm's inLobby flip. Without this the
         * client stays in the in-game UI after a back-to-lobby /
         * surrender vote passes — the server flips to lobby state and
         * publishes this event, but the frontend's clientSimIsInLobby
         * gate stays false until a follow-up CTRL_LOBBY_SETTINGS
         * arrives carrying inLobby=true, which only happens on hosts
         * with lobbyEnabled. */
        cs->inLobby = true;
        frontEndAudioReturningToLobby(false);
        /* Clear the predicted-tank latch so the next round's first
         * snapshot takes clientApplySnapshot's first-snapshot init
         * branch (sets stocks, jump-cuts the camera via
         * clientSimCenterTank) instead of the reconcile branch, which
         * only snaps position and leaves stocks + camera tracking
         * stale from the previous round. */
        cs->clientState.hasPredictedTank = FALSE;
        /* Drop the round that just ended. The world mirror (tanks, spent
         * shells, in-flight explosions) is otherwise only rewritten by
         * the next game's first snapshot, so without this the frozen
         * last round lingers behind the lobby — departed players' tanks
         * sitting where they stopped. Map/pill/base reset is owned by the
         * CTRL_LOBBY_MAP_CHANGE path, not here. Runs identically on SP,
         * host, and remote clients since it hangs off this one event. */
        clientSimResetWorld(cs);
        break;
    case CTRL_GAME_PHASE_COUNTDOWN:
        cs->netStat = netLobbyCountdown;
        cs->countdownSeconds = evt->u.gamePhase.countdownSeconds;
        /* Round-only scope: the previous round's stats panel clears when
         * the next round's countdown starts. */
        cs->lastRoundStatsValid = false;
        frontEndAudioReturningToLobby(false);
        break;
    case CTRL_GAME_PHASE_RUNNING:
        cs->netStat = netRunning;
        cs->countdownSeconds = 0;
        /* Frontends flip out of the lobby view on the running
         * transition; previously the SP finisher set this by hand
         * after StartGameInPlace, but now StartGameInPlace publishes
         * the RUNNING phase event and every subscriber should pick
         * up the lobby→game flip from this event. */
        cs->inLobby = false;
        /* Clear the lobby chat as the round starts so the lobby we
         * return to shows only this round's exit reason (and any new
         * lobby chat after). Keeping it across the game was confusing —
         * a "X has joined" line stayed visible after X left mid-round. */
        cs->lobbyChatHistory[0] = '\0';
        cs->lobbyTeamChatHistory[0] = '\0';
        cs->lobbyHostSlotKnown = false;
        /* Wipe per-game client state that a ClientSim surviving the lobby
         * cycle would otherwise carry into the new game. None of this is
         * refreshed wholesale by the snapshot apply, so without an explicit
         * reset it leaks across games on a lobby-enabled server. */
        /* Message scroller: queued-but-unshown chat would scroll in the
         * instant the new game's ticks resume. */
        messageReset(&cs->messages);
        /* Steam per-game achievement counters (consumed at CTRL_GAME_OVER).
         * Left un-reset, a death in any prior game permanently blocks the
         * flawless / no-LGM-loss achievements for the rest of the session. */
        cs->myDeathsThisGame = 0;
        cs->myLgmLossesThisGame = 0;
        cs->hasAnyBaseCaptured = false;
        cs->hasAnyPillCaptured = false;
        cs->maxPlayersSeenThisGame = 0;
        memset(cs->deathTimestamps, 0, sizeof(cs->deathTimestamps));
        cs->deathTimestampIdx = 0;
        /* A pending alliance dialog from the previous game is stale once a
         * new game (with fresh alliances) begins. */
        cs->pendingAllianceRequestFrom = 0xFF;
        /* In-game vote mirror (back-to-lobby / surrender). Zero is the
         * create-time "no vote" state. */
        memset(cs->gameVotes, 0, sizeof(cs->gameVotes));
        /* An unsent build request queued at the previous game's end would
         * otherwise fire on the new game's first input tick. */
        cs->pendingBuildAction = 0;
        cs->pendingBuildX = 0;
        cs->pendingBuildY = 0;
        /* Reseed the death-detection armour to "alive" (<= TANK_FULL_ARMOUR)
         * so the new game's first snapshot doesn't register a spurious
         * death or respawn edge against the previous game's last value. */
        cs->lastServerArmour = 0;
        frontEndAudioReturningToLobby(false);
        break;
    case CTRL_GAME_PHASE_GAME_OVER:
        /* Reset countdown unconditionally — used to be gated on
         * cs->inLobby, but that meant in-game game-over (vote pass /
         * map win / surrender chain) left netStat and countdown
         * untouched on the client, blocking the return-to-lobby UI
         * transition. The transport-internal joinState flip for the
         * non-lobby case is handled by the transport observer. */
        cs->netStat = netLobby;
        cs->countdownSeconds = 0;
        /* Silence in-flight playback so engine/shell/explosion
         * sounds don't keep draining behind the "Returning to
         * lobby" caption. Restored on the next phase event. */
        frontEndAudioReturningToLobby(true);
        break;

    case CTRL_GAME_OVER: {
        /* Win-detection + Steam achievements. Fires once per game-over
         * event on every audience — wire clients, SP host's in-process
         * ClientSim, bots — because the apply funnel is the single
         * convergence point for all three. Reads cs->sim.{bs,plyrs,game}
         * and the per-game counters set by client_snapshot during play. */
        BYTE numBases = basesGetNumBases(&cs->sim.bs);
        BYTE first    = NEUTRAL;
        bool allOwned = true;
        bool localWon = false;
        BYTE b;

        for (b = 1; b <= numBases && allOwned; b++) {
            BYTE owner = basesGetBaseOwner(&cs->sim.bs, b);
            BYTE shellsAmt, minesAmt, armourAmt;
            basesGetStats(&cs->sim.bs, b, &shellsAmt, &minesAmt, &armourAmt);
            if (owner == NEUTRAL || armourAmt <= MIN_ARMOUR_CAPTURE) {
                allOwned = false;
            } else if (b == 1) {
                first = owner;
            } else {
                allOwned = playersIsAllie(&cs->sim.plyrs, owner, first);
            }
        }

        /* Steam stats/achievements are for the local human only — bots run
         * this same game-over path with their own ClientSim and must not
         * credit wins/losses to the local user. */
        if (allOwned && numBases > 0 && !cs->isBot) {
            localWon = (cs->myPlayerNum == first) ||
                       playersIsAllie(&cs->sim.plyrs, cs->myPlayerNum, first);

            gameType gt = gameTypeGet(&cs->sim.game);
            BYTE numPlayers = playersGetNumPlayers(&cs->sim.plyrs);

            if (gt == gameTournament || gt == gameStrictTournament) {
                if (localWon) {
                    steam_increment_stat("STAT_TOURN_WINS", 1);
                    if (numPlayers == 2) {
                        steam_set_achievement("ACH_TOURN_WIN_1V1");
                    }
                } else {
                    steam_increment_stat("STAT_TOURN_LOSSES", 1);
                    if (numPlayers == 2) {
                        steam_set_achievement("ACH_TOURN_LOSE_1V1");
                    }
                }
            }

            if (localWon) {
                if (cs->myLgmLossesThisGame == 0) {
                    steam_set_achievement("ACH_WIN_NO_LGM_LOSS");
                }
                if (cs->myDeathsThisGame == 0 && numPlayers == 2) {
                    steam_set_achievement("ACH_1V1_FLAWLESS");
                }
            }

            steam_store_stats();
        }

        if (cs->inLobby) {
            cs->netStat = netLobby;
            cs->countdownSeconds = 0;
            /* Chat is cleared at game start (CTRL_GAME_PHASE_RUNNING),
             * not here — so the win/exit message published as the round
             * ends survives into the lobby we return to. */
        }
        break;
    }

    case CTRL_SERVER_TEXT: {
        const char *text = evt->u.serverText.text;
        if (text[0] == '\0') break;
        /* Team-scoped server text: only members of destTeam see it. */
        if (evt->u.serverText.destTeam != 0) {
            BYTE myPN = clientSimGetMyPlayerNum(cs);
            const ClientLobbySlot *ms = clientSimGetLobbySlot(cs, myPN);
            if (!ms || ms->teamNumber != evt->u.serverText.destTeam) break;
        }
        if (cs->inLobby) {
            clientSimAppendLobbyChat(cs, "Server", text);
            if (cs->lobbySyncSettled) frontEndPlaySound(cs, lobbyChatReceived);
        } else {
            /* In-game: route to newswire only. Server announcements
             * (vote countdown, surrender, etc.) belong on the same
             * channel as base captures / kills, and newswire is on by
             * default. */
            clientMessageAdd(clientSimGetMessages(cs), newsWireMessage,
                             (char *)"Server", (char *)text);
        }
        break;
    }

    case CTRL_SERVER_SHUTDOWN:
        /* No ClientSim field maps to UDP joinState; that field stays
         * transport-internal per the architectural commitment. */
        break;

    case CTRL_LOBBY_SYNC_COMPLETE:
        /* Final event of the join sync replay. The roster burst before it
         * (CTRL_PLAYER_JOIN / CTRL_LOBBY_SLOT per existing player) arrived
         * with inLobby already set; this marker tells us the burst is done,
         * so live lobby events may now play their sounds. */
        cs->lobbySyncSettled = true;
        break;

    case CTRL_CHAT: {
        /* In-process delivery for chat. Mirrors the UDP path in
         * transport_udp_client.c (PACKET_CHAT_BROADCAST), but for
         * subscribers that aren't UDP clients (host humanSim, bot
         * ClientSims). Without this, a bot's chat — submitted via
         * clientSimSubmitCommand and republished by the CMD_CHAT
         * arm as CTRL_CHAT — would never materialize in any
         * recipient's MessageState, so /info traffic between bots
         * would be invisible. */
        BYTE fromPlayer = evt->u.chat.fromPlayer;
        BYTE destPlayer = evt->u.chat.destPlayer;
        uint16_t bodyLen = evt->u.chat.bodyLen;
        BYTE myPN = clientSimGetMyPlayerNum(cs);
        const ClientLobbySlot *mySlot = clientSimGetLobbySlot(cs, myPN);
        BYTE myTeam = mySlot ? mySlot->teamNumber : 0;
        /* Only deliver if I'm the recipient or it's a broadcast.
         * Team-addressed chat reaches me when it carries my (non-zero)
         * team. Self-sends still surface in the sender's chat_log via
         * the Lua side (init.lua's outbound capture), so we don't need
         * a self-echo here. A spectator has no slot, so its myPN==0 would
         * spuriously match a unicast (destPlayer==0) or slot-0 team chat;
         * the server only forwards broadcast chat to spectators, so enforce
         * broadcast-only here too rather than rely on that alone. */
        bool for_me = cs->isSpectator
            ? (destPlayer == 0xFF)
            : ((destPlayer == 0xFF) || (destPlayer == myPN)
               || (CHAT_DEST_IS_TEAM(destPlayer) && myTeam != 0
                   && CHAT_DEST_TEAM_OF(destPlayer) == myTeam));
        if (for_me && fromPlayer < MAX_TANKS && bodyLen > 0
            && (cs->isSpectator || fromPlayer != myPN)) {
            char msg[PACKET_MAX_CHAT_MESSAGE + 1];
            uint16_t copyLen = bodyLen;
            if (copyLen > PACKET_MAX_CHAT_MESSAGE) copyLen = PACKET_MAX_CHAT_MESSAGE;
            memcpy(msg, evt->u.chat.body, copyLen);
            msg[copyLen] = '\0';
            if (CHAT_DEST_IS_TEAM(destPlayer) && clientSimIsInLobby(cs)) {
                clientSimAppendLobbyTeamChat(
                    cs, clientSimGetLobbySlot(cs, fromPlayer)->playerName, msg);
            } else {
                clientSimIncomingMessage(cs, fromPlayer, msg);
            }
            /* One play for both lobby sub-branches; in-game chat (not in
             * lobby) stays silent, and the join replay burst is gated out. */
            if (clientSimIsInLobby(cs) && cs->lobbySyncSettled) {
                frontEndPlaySound(cs, lobbyChatReceived);
            }
        }
        break;
    }

    case CTRL_PLAYER_LEAVE: {
        /* Reliable player removal. The snapshot's EVENT_PLAYER_LEAVE
         * (client_snapshot.c) also calls playersLeaveGame, but that
         * rides an unreliable, un-retransmitted snapshot — if the
         * snapshot carrying it was dropped, the departed player stayed
         * frozen in place forever (remote players render from
         * cs->sim.plyrs, not tanks[], and nothing else cleared them).
         * CTRL_PLAYER_LEAVE is on the reliable control queue, so do the
         * removal here too. playersLeaveGame is idempotent via its inUse
         * guard: whichever path arrives first removes the player, the
         * other is a no-op, so there is no duplicate quit newswire.
         * Self never reaches here — the early self-skip switch above
         * returned already. */
        BYTE pNum = evt->u.playerLeave.playerNum;
        char nameBuf[PACKET_MAX_PLAYER_NAME];
        memcpy(nameBuf, evt->u.playerLeave.name, sizeof(nameBuf));
        nameBuf[sizeof(nameBuf) - 1] = '\0';
        /* announce=false in the lobby: the in-game newswire is wrong there
         * (it would queue and pop at game start); the lobby chat line below
         * is the right surface. In-game, announce the leave on the newswire. */
        playersLeaveGame(cs, &cs->sim, &cs->sim.plyrs, cs->myPlayerNum,
                         pNum, FALSE, !cs->inLobby);
        if (cs->inLobby) {
            char leaveMsg[PACKET_MAX_PLAYER_NAME + 16];
            snprintf(leaveMsg, sizeof(leaveMsg), "%s has left.", nameBuf);
            clientSimAppendLobbyChat(cs, "***", leaveMsg);
            if (cs->lobbySyncSettled) frontEndPlaySound(cs, lobbyPlayerLeave);
        }
        break;
    }

    case CTRL_COMMAND_REJECTED:
        /* Only react to rejects attributed to our own slot. In-process
         * subscribers (SP-host, bots) receive every published reject;
         * the wire path is already filtered by udpClientDeliverControl.
         * lobbyLastRejectPacket stores origCmdType (the CMD_* enum
         * value) — its previous semantics were "non-zero = pending
         * reject" and the toast UI in imgui_lobby.cpp only checks for
         * non-zero, so storing a CMD_* there is compatible. */
        if (evt->u.commandRejected.origSlot == clientSimGetMyPlayerNum(cs)) {
            cs->lobbyLastRejectPacket = evt->u.commandRejected.origCmdType;
            cs->lobbyLastRejectReason = evt->u.commandRejected.reasonCode;
        }
        break;

    case CTRL_SHELL_DEATH: {
        /* Server closure for one of our predicted shells: cull the ghost so
         * it stops flying on past the server's impact at high ping. Owner-only
         * on the wire, but in-process subscribers (SP-host, bots) receive
         * every published event, so gate on owner == our slot (mirrors
         * CTRL_COMMAND_REJECTED). Match by fireTick and remove via the
         * swap-with-last predicted-shell cull idiom. */
        if (evt->u.shellDeath.owner != clientSimGetMyPlayerNum(cs)) {
            break;
        }
        int i;
        for (i = 0; i < cs->predictedShellCount; i++) {
            if (cs->predictedShells[i].fireTick == evt->u.shellDeath.fireTick) {
                cs->predictedShells[i] =
                    cs->predictedShells[cs->predictedShellCount - 1];
                cs->predictedShellCount--;
                break;  /* one fire per tick — at most one match */
            }
        }
        /* No impact drawn here: the owner already receives the authoritative
         * EVENT_EXPLOSION for this shell on the snapshot tail (drawn for every
         * client with no owner filter), so drawing one here would double up. */
        break;
    }
    }
}
