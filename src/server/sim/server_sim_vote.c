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
 *Name:          Server Simulation Chat and Voting
 *Filename:      server_sim_vote.c
 *Author:        John Morrison
 *Purpose:
 *  The server-originated broadcasts and the chat intake,
 *  the map-skip vote, and the game-vote family —
 *  back-to-lobby and surrender.
 *********************************************************/

#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "server_sim_shared.h"      /* serverSimFillMapSkipStateEvent — the event body, encoded in server_sim_control.c */
#include "server_sim_internal.h"
#include "netpacks.h"               /* the GAME_VOTE_* kind, toggle, deadline and pass-percentage constants */
#include "wire_limits.h"            /* PACKET_MAX_CHAT_MESSAGE — the chat-body clamp */
#include "log.h"                    /* logAddEvent — the .wbv records for chat, map skip and the vote transitions */
#include "../../common/wb_log.h"    /* WB_LOG_INFO — the map-skip tally trace */

void publishMapSkipState(ServerSim *sim) {
    ControlEvent evt;
    if (sim == NULL) return;
    memset(&evt, 0, sizeof(evt));
    serverSimFillMapSkipStateEvent(sim, &evt);
    serverSimPublishControl(sim, &evt);
}

void serverSimMapSkipVoteToggle(ServerSim *sim, uint8_t playerNum) {
    int voteCount = 0;
    int connectedHumans = 0;
    BYTE i;

    if (sim->state != serverStateLobby || (sim->mapDirCount <= 1 && !sim->randomMapEnabled)) {
        return;
    }
    if (playerNum >= MAX_TANKS || !sim->playerConnected[playerNum]) {
        return;
    }
    if (sim->lobbyPlayers[playerNum].isBot) {
        return;
    }

    sim->mapSkipVotes[playerNum] = !sim->mapSkipVotes[playerNum];
    if (sim->mapSkipVotes[playerNum]) {
        logAddEvent(log_MapSkipVote, playerNum, 0, 0, 0, 0, NULL);
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || sim->lobbyPlayers[i].isBot) continue;
        connectedHumans++;
        if (sim->mapSkipVotes[i]) voteCount++;
    }

    WB_LOG_INFO(WB_LOG_CAT_SERVER, "Map skip: player %d voted %s (%d/%d)", playerNum,
            sim->mapSkipVotes[playerNum] ? "yes" : "no", voteCount, connectedHumans);

    if (connectedHumans > 0 && voteCount * 2 > connectedHumans) {
        WB_LOG_INFO(WB_LOG_CAT_SERVER, "Map skip: majority reached (%d/%d), skipping map", voteCount, connectedHumans);
        if (sim->randomMapEnabled) {
            serverSimRandomMapRegenerate(sim);
        } else {
            serverSimMapDirPickRandom(sim);
        }
        {
            char pstr[256];
            int nameLen = (int)strlen(sim->mapName);
            if (nameLen > 255) nameLen = 255;
            pstr[0] = (char)nameLen;
            memcpy(pstr + 1, sim->mapName, nameLen);
            logAddEvent(log_MapSkipApplied, 0, 0, 0, 0, 0, pstr);
        }
        serverSimMapSkipVotesReset(sim);
        serverSimWbnLobbyUpdate(sim, FALSE);
    }
    publishMapSkipState(sim);
}

void serverSimMapSkipVotesReset(ServerSim *sim) {
    memset(sim->mapSkipVotes, 0, sizeof(sim->mapSkipVotes));
}

/* ----------------------------------------------------------------------
 * In-game vote system (back-to-lobby + surrender). See docs/voting_plan.md.
 *
 * The state machine lives entirely on the server; clients are mirror-only.
 * Wire format: PACKET_GAME_VOTE_TOGGLE in, PACKET_GAME_VOTE_STATE out.
 *
 * NOTE: this is the data-model + helper layer. Wire serialisation lives
 * in transport_udp_server.c (broadcastGameVoteState). Tick wiring lives
 * in the transport tick path.
 * ---------------------------------------------------------------------- */

/* Server-originated English broadcast via CTRL_CHAT (fromPlayer=0xFE).
 * Inlined here (instead of calling transportUdpServerSendServerMessage)
 * so BrainTest / MapEditor — which link server_sim_static but not the
 * UDP transport — can still announce server messages to subscribers. */
void publishServerMessage(ServerSim *sim, const char *message) {
    ControlEvent evt;
    if (!sim || !message) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    evt.u.serverText.destPlayer = 0xFF;   /* every slot; 0 would be slot 0 alone */
    /* In-process subscribers display via client_sim_control.c's
     * CTRL_SERVER_TEXT handler (newswire / lobby chat); UDP clients
     * receive the codec-encoded PACKET_CHAT_BROADCAST(fromPlayer=0xFE)
     * via the encoder table. */
    serverSimPublishControl(sim, &evt);
}

/* Server-originated English broadcast for text that may be over the wire's
 * chat cap. Rather than let SDL_strlcpy lop the tail mid-character, a long
 * message is cut on a UTF-8 boundary and ends in an ellipsis, so the
 * overflow reads as an intentional truncation. */
void publishServerEnglishBroadcast(ServerSim *sim, const char *message) {
    ControlEvent evt;
    size_t maxChars = sizeof(evt.u.serverText.text) - 1; /* PACKET_MAX_CHAT_MESSAGE */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    evt.u.serverText.destPlayer = 0xFF;  /* everyone, not slot 0 */
    if (SDL_strlen(message) <= maxChars) {
        SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    } else {
        /* CTRL_SERVER_TEXT / PACKET_CHAT_BROADCAST cap the wire payload at
         * PACKET_MAX_CHAT_MESSAGE. Rather than let SDL_strlcpy lop the tail
         * mid-character (a long winners list overflows the cap), cut on a
         * UTF-8 boundary and append an ellipsis so the overflow reads as an
         * intentional truncation. */
        size_t cut = maxChars - 3; /* leave room for "..." */
        while (cut > 0 && ((unsigned char)message[cut] & 0xC0) == 0x80) {
            cut--; /* back up so a multi-byte sequence isn't split */
        }
        SDL_memcpy(evt.u.serverText.text, message, cut);
        SDL_strlcpy(evt.u.serverText.text + cut, "...",
                    sizeof(evt.u.serverText.text) - cut);
    }
    /* In-process subscribers (SP host bots + human) consume CTRL_SERVER_TEXT
     * directly; UDP clients receive the encoder-emitted
     * PACKET_CHAT_BROADCAST(fromPlayer=0xFE) via the codec. */
    serverSimPublishControl(sim, &evt);
}

/* Like publishServerMessage but delivered ONLY to members of teamId (1-15).
 * destTeam rides the ControlEvent and is filtered per-recipient in
 * udpClientDeliverControl + the in-process CTRL_SERVER_TEXT handler — used to
 * keep surrender-vote notices private to the surrendering team. */
void publishServerMessageToTeam(ServerSim *sim, const char *message,
                                BYTE teamId) {
    ControlEvent evt;
    if (!sim || !message) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SERVER_TEXT;
    SDL_strlcpy(evt.u.serverText.text, message, sizeof(evt.u.serverText.text));
    evt.u.serverText.destTeam = teamId;
    evt.u.serverText.destPlayer = 0xFF;   /* the team, not one slot within it */
    serverSimPublishControl(sim, &evt);
}

/* serverSimReceiveChat — authoritative entry for any chat the server
 * accepts, regardless of which transport delivered the input.
 *
 * Per docs/ARCHITECTURE.md "Worked example — adding a chat message":
 * every audience (in-process subscribers + UDP-connected clients) must
 * see the same event. We achieve that by routing both inputs (the UDP
 * server's PACKET_CHAT_MESSAGE handler and the bot pool's chat-send
 * callback) through here, then publishing a single CTRL_CHAT — the
 * per-client subscriber in transport_udp_server.c fans it back out on
 * the wire (via the codec encoder) and the in-process CTRL_CHAT
 * handler in client_sim_control.c materializes it into recipient
 * MessageStates.
 *
 * fromPlayer must be a real slot (0..MAX_TANKS-1); destPlayer is the
 * single recipient or 0xFF for broadcast. body/bodyLen is the raw chat
 * payload (no length prefix). Caller is responsible for keeping
 * bodyLen <= PACKET_MAX_CHAT_MESSAGE. */
void serverSimReceiveChat(ServerSim *sim, BYTE fromPlayer, BYTE destPlayer,
                          const void *body, size_t bodyLen) {
    ControlEvent evt;
    if (sim == NULL || body == NULL || fromPlayer >= MAX_TANKS) return;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = fromPlayer;
    evt.u.chat.destPlayer = destPlayer;
    evt.u.chat.bodyLen    = (uint16_t)bodyLen;
    if (bodyLen > 0) {
        memcpy(evt.u.chat.body, body, bodyLen);
    }

    /* The lobby-chat catch-up capture lives in serverSimPublishControl, the one
     * chokepoint this and the wire CMD_CHAT path both publish through. */
    serverSimPublishControl(sim, &evt);
}

/* serverSimReceiveSpectatorChat — authoritative entry for a lobby chat line
 * typed by a tankless spectator. A spectator has no player slot, so it cannot
 * route through serverSimReceiveChat; instead the message is stamped with the
 * sender's specIdx and published as CTRL_SPECTATOR_CHAT, which the bus fans to
 * players and to spectators (the spectator deliver allowlist passes it). The
 * line is recorded into the .wbv as log_SpectatorChat so the log viewer can
 * attribute it. */
void serverSimReceiveSpectatorChat(ServerSim *sim, uint8_t specIdx,
                                   const void *body, size_t bodyLen) {
    ControlEvent evt;
    if (sim == NULL || body == NULL || specIdx >= MAX_SPECTATORS) return;
    if (bodyLen > PACKET_MAX_CHAT_MESSAGE) bodyLen = PACKET_MAX_CHAT_MESSAGE;

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_SPECTATOR_CHAT;
    evt.u.spectatorChat.specIdx = specIdx;
    evt.u.spectatorChat.bodyLen = (uint16_t)bodyLen;
    if (bodyLen > 0) {
        memcpy(evt.u.spectatorChat.body, body, bodyLen);
    }

    /* Capture for the drain-flip catch-up happens in serverSimPublishControl. */
    serverSimPublishControl(sim, &evt);

    {
        char pstr[256];
        int pLen = (int)bodyLen;
        if (pLen > 255) pLen = 255;
        pstr[0] = (char)pLen;
        if (pLen > 0) memcpy(pstr + 1, body, pLen);
        logAddEvent(log_SpectatorChat, specIdx, 0, 0, 0, 0, pstr);
    }
}

/* Publish current vote state through the control-event dispatcher.
 * In-process subscribers see it directly; remote UDP clients receive
 * the wire-encoded PACKET_GAME_VOTE_STATE via the codec encoder. */
static void publishGameVoteState(ServerSim *sim, uint8_t kind) {
    ServerGameVoteSnapshot snap;
    ControlEvent evt;
    if (!serverSimGetGameVoteSnapshot(sim, kind, &snap)) return;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_VOTE_STATE;
    evt.u.gameVoteState.kind             = snap.kind;
    evt.u.gameVoteState.active           = snap.active;
    evt.u.gameVoteState.triggerSrc       = snap.triggerSrc;
    evt.u.gameVoteState.teamId           = snap.teamId;
    evt.u.gameVoteState.threshold        = snap.threshold;
    evt.u.gameVoteState.yesCount         = snap.yesCount;
    evt.u.gameVoteState.noCount          = snap.noCount;
    evt.u.gameVoteState.eligibleCount    = snap.eligibleCount;
    evt.u.gameVoteState.secondsRemaining = snap.secondsRemaining;
    evt.u.gameVoteState.votes            = snap.votes;
    serverSimPublishControl(sim, &evt);
}

/* Forward declarations for the in-TU helpers — gameVoteThreshold is
 * called from the public snapshot accessor which sits above the
 * helper's definition. */
static uint8_t gameVoteThreshold(const ServerSim *sim, const struct ServerGameVote *gv);

static struct ServerGameVote *gameVoteSlot(ServerSim *sim, uint8_t kind) {
    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) return &sim->gameVotes[0];
    if (kind == GAME_VOTE_KIND_SURRENDER)     return &sim->gameVotes[1];
    return NULL;
}

static const struct ServerGameVote *gameVoteSlotConst(const ServerSim *sim, uint8_t kind) {
    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) return &sim->gameVotes[0];
    if (kind == GAME_VOTE_KIND_SURRENDER)     return &sim->gameVotes[1];
    return NULL;
}

/* Returns the bitmask of slots eligible to vote on this kind. For
 * back-to-lobby that's every connected human; for surrender it's the
 * connected humans on `teamId`. */
static uint16_t gameVoteEligibleMask(const ServerSim *sim,
                                     uint8_t kind, uint8_t teamId) {
    uint16_t mask = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i]) continue;
        if (sim->lobbyPlayers[i].isBot) continue;
        if (kind == GAME_VOTE_KIND_SURRENDER &&
            sim->lobbyPlayers[i].teamNumber != teamId) continue;
        mask |= (uint16_t)(1u << i);
    }
    return mask;
}

static uint8_t popcount16(uint16_t v) {
    uint8_t n = 0;
    while (v) { n += (uint8_t)(v & 1u); v >>= 1; }
    return n;
}

uint8_t serverSimCountActiveTeams(const ServerSim *sim) {
    /* Counts distinct teamNumbers across teams with at least one
     * connected human. Bots don't count — surrender needs a human
     * on each side. */
    bool seen[MAX_TANKS] = {0};
    uint8_t count = 0;
    BYTE i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!sim->playerConnected[i] || sim->lobbyPlayers[i].isBot) continue;
        uint8_t t = sim->lobbyPlayers[i].teamNumber;
        if (t == 0 || t >= MAX_TANKS) continue;
        if (!seen[t]) { seen[t] = true; count++; }
    }
    return count;
}

bool serverSimGameVoteIsRunning(const ServerSim *sim, uint8_t kind) {
    const struct ServerGameVote *gv = gameVoteSlotConst(sim, kind);
    return gv && gv->active == GAME_VOTE_ACTIVE_RUNNING;
}

bool serverSimGetGameVoteSnapshot(const ServerSim *sim, uint8_t kind,
                                  ServerGameVoteSnapshot *out) {
    const struct ServerGameVote *gv = gameVoteSlotConst(sim, kind);
    if (!gv || !out) return false;
    memset(out, 0, sizeof(*out));
    out->kind       = gv->kind;
    out->active     = gv->active;
    out->triggerSrc = gv->triggerSrc;
    out->teamId     = gv->teamId;
    uint16_t eligibleMask = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
    out->eligibleCount = popcount16(eligibleMask);
    out->threshold  = gameVoteThreshold(sim, gv);
    out->yesCount   = popcount16(gv->votesMask);
    out->noCount    = popcount16(gv->answeredMask & ~gv->votesMask);
    out->votes      = gv->votesMask;
    if (gv->active == GAME_VOTE_ACTIVE_RUNNING) {
        uint64_t now = sim->gameVoteWallMs;
        /* During the pre-pass grace window, surface that grace's
         * remaining seconds so the client widget shows the short
         * "Passing in N..." countdown instead of the long 60-s
         * timeout. The client infers the state from
         * (yesCount == threshold) + small secondsRemaining. */
        uint64_t until = (gv->pendingPassUntilMs != 0)
                         ? gv->pendingPassUntilMs
                         : gv->deadlineMs;
        uint64_t rem = (until > now) ? (until - now) : 0;
        uint32_t secs = (uint32_t)((rem + 999) / 1000);
        if (secs > 0xFFu) secs = 0xFFu;
        out->secondsRemaining = (uint8_t)secs;
    }
    return true;
}

void serverSimGameVoteResetAll(ServerSim *sim) {
    memset(sim->gameVotes, 0, sizeof(sim->gameVotes));
    sim->gameVotes[0].kind = GAME_VOTE_KIND_BACK_TO_LOBBY;
    sim->gameVotes[1].kind = GAME_VOTE_KIND_SURRENDER;
    sim->returnToLobbyTicks = 0;
    sim->returnToLobbyReason = RETURN_REASON_NONE;
    sim->returnToLobbyTeamId = 0;
}

static void gameVoteStart(ServerSim *sim, uint8_t kind, uint8_t triggerSrc,
                          uint8_t teamId, uint64_t nowMs, uint8_t initiator) {
    struct ServerGameVote *gv = gameVoteSlot(sim, kind);
    if (!gv) return;
    memset(gv, 0, sizeof(*gv));
    gv->kind        = kind;
    gv->active      = GAME_VOTE_ACTIVE_RUNNING;
    gv->triggerSrc  = triggerSrc;
    gv->teamId      = teamId;
    gv->startMs     = nowMs;
    gv->deadlineMs  = nowMs + (uint64_t)GAME_VOTE_DEADLINE_SECONDS * 1000ULL;
    gv->lastHeartbeatMs = nowMs;
    logAddEvent(log_GameVoteStart, kind, initiator, teamId, 0, 0, NULL);
}

static void gameVoteConclude(ServerSim *sim, struct ServerGameVote *gv,
                             uint8_t finalState, uint64_t nowMs) {
    gv->active = finalState;
    gv->concludedAtMs = nowMs;
    publishGameVoteState(sim, gv->kind);
    logAddEvent(log_GameVoteEnd, gv->kind,
                finalState == GAME_VOTE_ACTIVE_PASSED ? 1 : 0,
                0, 0, 0, NULL);
}

/* Fire the actual pass effects (countdown for back-to-lobby,
 * announcement + chained vote for surrender). Called from the tick
 * once the pending-pass grace expires. */
static void gameVoteFirePass(ServerSim *sim, struct ServerGameVote *gv,
                             uint64_t nowMs) {
    uint8_t kind = gv->kind;
    gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_PASSED, nowMs);

    if (kind == GAME_VOTE_KIND_BACK_TO_LOBBY) {
        /* Game keeps running — players can still move, shoot,
         * etc. — but each tick decrements sim->returnToLobbyTicks
         * and at 0 the running tick transitions to gameOver. The
         * snapshot header carries the remaining ticks every frame
         * so clients can render their own 3/2/1 countdown.
         *
         * Budget: 7 seconds at 100Hz (each serverSimTick call). */
        /* The vote leaves a line in the returning lobby explaining why the
         * round ended (no in-game newswire line — clients already render
         * the 3/2/1 countdown). Every back-to-lobby vote is player-started
         * since the base-monopoly auto-vote was removed. */
        sim->returnToLobbyReason = RETURN_REASON_MANUAL_VOTE;
        sim->returnToLobbyTicks = 700;
    } else if (kind == GAME_VOTE_KIND_SURRENDER) {
        char buf[160];
        const char *tname = sim->teams[gv->teamId].name[0]
                            ? sim->teams[gv->teamId].name : "?";
        snprintf(buf, sizeof(buf),
                 "*** Team %s has surrendered. ***", tname);
        /* The one kind of newswire fact the server writes as text. There is
           no quiet byte to stamp and no event to hold: a line the policy
           turns down is simply not sent. The surrender itself goes ahead. */
        if (serverSimAnnounce(sim, ANNOUNCE_KIND_VOTE, gv->teamId, NEUTRAL)) {
            publishServerMessage(sim, buf);
        }

        /* A surrender ends the round immediately — no chained
         * back-to-lobby vote. Record the team that gave up so the
         * game-over handler credits the opposing team with the win
         * (WBN events + lobby winner line), then return to the lobby on
         * the same countdown a back-to-lobby vote uses. */
        sim->returnToLobbyReason = RETURN_REASON_SURRENDER;
        sim->returnToLobbyTeamId = gv->teamId;
        sim->returnToLobbyTicks = 700;
    }
}

/* YES-vote count needed for the vote to pass, given the current
 * eligible voter pool and the configured pass percentage.
 *
 *   threshold = ceil(eligible * NUM / DENOM)
 *
 * For NUM/DENOM = 100/100 that's exact unanimity (== eligible). */
static uint8_t gameVoteThreshold(const ServerSim *sim, const struct ServerGameVote *gv) {
    uint32_t eligible = popcount16(gameVoteEligibleMask(sim, gv->kind, gv->teamId));
    if (eligible == 0) return 0;
    uint32_t num = (uint32_t)GAME_VOTE_PASS_PCT_NUM;
    uint32_t den = (uint32_t)GAME_VOTE_PASS_PCT_DENOM;
    /* ceil(eligible * num / den) */
    uint32_t thr = (eligible * num + (den - 1)) / den;
    if (thr > 0xFFu) thr = 0xFFu;
    return (uint8_t)thr;
}

/* Drop bits from votes/answered for slots that disappeared. */
static void gameVotePruneVotes(const ServerSim *sim, struct ServerGameVote *gv) {
    uint16_t elig = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
    gv->votesMask    &= elig;
    gv->answeredMask &= elig;
}

void serverSimGameVoteToggle(ServerSim *sim, uint8_t playerNum,
                             uint8_t kind, uint8_t toggleMode) {
    /* No lobby means no place to return to: a passed back-to-lobby /
     * surrender vote would only terminate (or, under map rotation,
     * blindly rotate) the server. Disable voting entirely in that mode. */
    if (!sim->lobbyEnabled) return;
    if (playerNum >= MAX_TANKS) return;
    if (!sim->playerConnected[playerNum]) return;
    if (sim->lobbyPlayers[playerNum].isBot) return;
    struct ServerGameVote *gv = gameVoteSlot(sim, kind);
    if (!gv) return;

    /* Only allow during running game. */
    if (sim->state != serverStateRunning) return;

    /* Reject malformed toggleMode bytes from the wire before any
     * state-mutating branch can react to them. The historical else-fall
     * treated anything that wasn't YES as NO, so 0xFF would be recorded
     * as a NO vote. */
    if (toggleMode != GAME_VOTE_TOGGLE_NO &&
        toggleMode != GAME_VOTE_TOGGLE_YES &&
        toggleMode != GAME_VOTE_TOGGLE_OPEN_ONLY) {
        return;
    }

    /* Surrender precondition: exactly two teams in play, and the
     * caller must be on a real team — an Unassigned (team 0) player
     * surrendering "team 0" would broadcast a fake side and chain a
     * back-to-lobby vote against two unrelated playing teams. */
    if (kind == GAME_VOTE_KIND_SURRENDER) {
        if (serverSimCountActiveTeams(sim) != 2) return;
        if (sim->lobbyPlayers[playerNum].teamNumber == 0) return;
    }

    uint64_t nowMs = sim->gameVoteWallMs;
    uint8_t teamId = (kind == GAME_VOTE_KIND_SURRENDER)
                     ? sim->lobbyPlayers[playerNum].teamNumber
                     : 0;

    /* A standalone NO has no effect when no vote is running. The
     * vote-start branch below would otherwise open a fresh vote and
     * record the caller as NO+answered, which is meaningless. Only
     * YES or OPEN_ONLY may open a vote. */
    if (gv->active != GAME_VOTE_ACTIVE_RUNNING &&
        toggleMode == GAME_VOTE_TOGGLE_NO) {
        return;
    }

    /* Open-only re-press: if a vote is running, just rebroadcast (so the
     * client can pop the widget back up); if no vote is running, start one
     * as if the caller voted yes. */
    if (toggleMode == GAME_VOTE_TOGGLE_OPEN_ONLY) {
        if (gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            publishGameVoteState(sim, kind);
            return;
        }
        gameVoteStart(sim, kind, GAME_VOTE_TRIGGER_MANUAL, teamId, nowMs, playerNum);
        gv->votesMask    |= (uint16_t)(1u << playerNum);
        gv->answeredMask |= (uint16_t)(1u << playerNum);
        logAddEvent(log_GameVoteCast, kind, playerNum, 1, 0, 0, NULL);

        /* Check whether opening + auto-YES already constitutes a pass.
         * Solo (threshold==1) starts the 5-s grace immediately so the
         * very first broadcast carries secondsRemaining=5 instead of
         * the 60-s timeout (otherwise the widget flashes "60s" before
         * the next heartbeat brings it down). Multi-voter unanimity
         * fires the pass right away. */
        uint8_t thr = gameVoteThreshold(sim, gv);
        uint8_t yes = popcount16(gv->votesMask);
        if (thr > 0 && yes >= thr) {
            if (thr == 1) {
                gv->pendingPassUntilMs = nowMs +
                    (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL;
                publishGameVoteState(sim, kind);
            } else {
                publishGameVoteState(sim, kind);
                gameVoteFirePass(sim, gv, nowMs);
            }
            return;
        }

        publishGameVoteState(sim, kind);
        return;
    }

    if (gv->active != GAME_VOTE_ACTIVE_RUNNING) {
        /* First voter starts the vote. */
        gameVoteStart(sim, kind, GAME_VOTE_TRIGGER_MANUAL, teamId, nowMs, playerNum);
    } else if (kind == GAME_VOTE_KIND_SURRENDER &&
               gv->teamId != sim->lobbyPlayers[playerNum].teamNumber) {
        /* Different-team player can't vote on a team's surrender. */
        return;
    }

    gv->answeredMask |= (uint16_t)(1u << playerNum);
    if (toggleMode == GAME_VOTE_TOGGLE_YES) {
        gv->votesMask |= (uint16_t)(1u << playerNum);
    } else {
        gv->votesMask &= (uint16_t)~(1u << playerNum);
    }
    logAddEvent(log_GameVoteCast, kind, playerNum,
                toggleMode == GAME_VOTE_TOGGLE_YES ? 1 : 0, 0, 0, NULL);

    gameVotePruneVotes(sim, gv);

    uint8_t thr = gameVoteThreshold(sim, gv);
    uint8_t yes = popcount16(gv->votesMask);

    if (thr > 0 && yes >= thr) {
        /* Solo voter ("am I sure?") path: one-human votes get a
         * 5-second grace before the effect applies so a misclick
         * is reversible. Multi-human votes fire instantly — by the
         * time everyone has agreed there's nothing to second-guess. */
        if (thr == 1) {
            if (gv->pendingPassUntilMs == 0) {
                gv->pendingPassUntilMs = nowMs +
                    (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL;
            }
            publishGameVoteState(sim, kind);
        } else {
            publishGameVoteState(sim, kind);
            gameVoteFirePass(sim, gv, nowMs);
        }
        return;
    }

    /* Lost unanimity during a solo grace — cancel the pending pass. */
    if (gv->pendingPassUntilMs != 0) {
        gv->pendingPassUntilMs = 0;
    }

    publishGameVoteState(sim, kind);

    /* Everyone answered but yes count didn't reach the pass
     * threshold → fail now instead of waiting for the timeout. */
    {
        uint16_t eligibleMask = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
        uint8_t eligible = popcount16(eligibleMask);
        uint8_t answered = popcount16(gv->answeredMask & eligibleMask);
        if (eligible > 0 && answered >= eligible && yes < thr) {
            gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_FAILED, nowMs);
        }
    }
}

void serverSimGameVoteTick(ServerSim *sim, uint64_t nowMs) {
    /* Voting only exists on lobby-enabled servers (see
     * serverSimGameVoteToggle). Skip the whole vote machinery when there
     * is no lobby. */
    if (!sim->lobbyEnabled) return;

    sim->gameVoteWallMs = nowMs;

    int k;
    for (k = 0; k < 2; k++) {
        struct ServerGameVote *gv = &sim->gameVotes[k];

        if (gv->active == GAME_VOTE_ACTIVE_RUNNING) {
            /* Prune in case a voter disconnected. */
            gameVotePruneVotes(sim, gv);

            /* 60s timeout. */
            if (nowMs >= gv->deadlineMs) {
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_FAILED, nowMs);
                continue;
            }

            /* Surrender invalidation: team count must remain == 2. */
            if (gv->kind == GAME_VOTE_KIND_SURRENDER &&
                serverSimCountActiveTeams(sim) != 2) {
                if (serverSimAnnounce(sim, ANNOUNCE_KIND_VOTE, gv->teamId,
                                      NEUTRAL)) {
                    publishServerMessageToTeam(sim, "Surrender vote cancelled — team count changed.", gv->teamId);
                }
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_CANCELLED, nowMs);
                continue;
            }

            /* If the surrendering team or the voter pool has emptied
             * entirely (everyone disconnected), the vote is moot. */
            if (popcount16(gameVoteEligibleMask(sim, gv->kind, gv->teamId)) == 0) {
                if (gv->kind == GAME_VOTE_KIND_SURRENDER &&
                    serverSimAnnounce(sim, ANNOUNCE_KIND_VOTE, gv->teamId,
                                      NEUTRAL)) {
                    publishServerMessageToTeam(sim,
                        "Surrender vote cancelled — surrendering team is empty.", gv->teamId);
                }
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_CANCELLED, nowMs);
                continue;
            }

            /* 1Hz heartbeat broadcast (drives client countdown display). */
            if (nowMs - gv->lastHeartbeatMs >= 1000ULL) {
                gv->lastHeartbeatMs = nowMs;
                publishGameVoteState(sim, gv->kind);
            }

            /* Re-check pass under the live eligible-mask. Eligibility
             * may have shrunk (disconnect) so unanimity can land here
             * without a fresh toggle. Solo (threshold==1) votes use
             * a 5-s grace; everything else fires immediately. */
            uint16_t eligibleMask = gameVoteEligibleMask(sim, gv->kind, gv->teamId);
            uint8_t eligible = popcount16(eligibleMask);
            uint8_t answered = popcount16(gv->answeredMask & eligibleMask);
            uint8_t thr = gameVoteThreshold(sim, gv);
            uint8_t yes = popcount16(gv->votesMask);

            if (thr > 0 && yes >= thr) {
                if (thr == 1) {
                    if (gv->pendingPassUntilMs == 0) {
                        gv->pendingPassUntilMs = nowMs +
                            (uint64_t)GAME_VOTE_PASS_GRACE_SECONDS * 1000ULL;
                    }
                } else {
                    gameVoteFirePass(sim, gv, nowMs);
                    continue;
                }
            } else if (gv->pendingPassUntilMs != 0) {
                gv->pendingPassUntilMs = 0;
            }

            /* Solo pre-pass grace expired → fire for real. */
            if (gv->pendingPassUntilMs != 0 &&
                nowMs >= gv->pendingPassUntilMs) {
                gv->pendingPassUntilMs = 0;
                gameVoteFirePass(sim, gv, nowMs);
                continue;
            }

            /* Everyone eligible has answered but the yes side fell
             * short of the pass threshold → vote fails immediately,
             * no point waiting on the 60-s timeout. */
            if (eligible > 0 && answered >= eligible &&
                yes < thr && gv->pendingPassUntilMs == 0) {
                gameVoteConclude(sim, gv, GAME_VOTE_ACTIVE_FAILED, nowMs);
                continue;
            }
        }

    }
}
