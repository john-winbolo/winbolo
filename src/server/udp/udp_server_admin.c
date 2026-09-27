/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-2.0-or-later
 */

/*********************************************************
 *Name:          UDP Server Admin
 *Filename:      udp_server_admin.c
 *Author:        John Morrison
 *Purpose:
 *  The operations a host or a console performs on a
 *  connected player, split out of transport_udp_server.c.
 *  Nothing here belongs to the per-tick flow.
 *    - Disconnecting one client, and draining the removals
 *      other paths defer to a safe point in the tick.
 *    - Kicking a named player or bot, and disconnecting
 *      everyone at once.
 *    - The server lock and the notice that announces it.
 *    - High-ping warning and kick enforcement.
 *    - Server-originated messages, localized and English.
 *    - Status reporting and bot naming.
 *    - The background thread that asks WinBolo.net for a
 *      team balance.
 *********************************************************/

#include <stdio.h>   /* fprintf, snprintf, stderr, stdout, FILE, fopen, fclose */
#include <stdlib.h>  /* malloc, free */
#include <string.h>  /* memcpy, memset, strncpy */

#include "transport_udp_internal.h"        /* packHeader, packGameEvent, and
                                            * SDL3/SDL.h for SDL_assert /
                                            * SDL_CreateThread / SDL_strlcpy */
#include "transport_udp_server_internal.h" /* udpServer, srvSendTo,
                                            * packLocalizedPayload,
                                            * serverCleanupMapDownload,
                                            * udpServerClearClientUploadState,
                                            * udpServerResetMapReaskLimit,
                                            * udpServerResetRoundLogLimits */
#include "transport_udp.h"   /* UdpServerClient, wbnJoinClear, PING_WARN_* /
                              * PING_KICK_* thresholds and counts */
#include "netpacks.h"        /* PACKET_KICKED, PACKET_HEADER_SIZE,
                              * PACKET_MAX_PLAYER_NAME, CLIENT_TIMEOUT_TICKS */
#include "global.h"          /* BYTE, MAX_TANKS, PlayerBitMap, PLAYER_NAME_LEN,
                              * TRUE, FALSE */
#include "input_packet.h"    /* GameEvent, GAME_EVENT_MAX_WIRE_SIZE,
                              * EVENT_SERVER_MSG, SERVER_MSG_GAME_LOCKED /
                              * SERVER_MSG_GAME_UNLOCKED */
#include "game_sim.h"        /* GameSim, and via lang.h the STR_ message ids */
#include "server_sim.h"      /* ServerSim, ServerState, BalanceProposal,
                              * SUBSCRIBER_HANDLE_INVALID and the sim accessors
                              * these paths read */
#include "server_sim_internal.h"  /* serverSimSetShadowCulled — sim co-owner */
#include "server_sim_lifecycle.h" /* lobbyAutoUnreadyOnChange, serverSimSetTeamBatch,
                                   * serverSimSetHostSlot,
                                   * serverSimSetBalanceRequestInFlight,
                                   * serverSimSetBalanceIncludeBots */
#include "server_sim_join.h"      /* serverSimReleaseIneligibleStartsAndBackfill — after a WBN balance moves players */
#include "control_event.h"   /* ControlEvent, CTRL_CHAT, CTRL_SERVER_TEXT,
                              * CTRL_SERVER_SHUTDOWN, CTRL_BALANCE_* */
#include "channel_mux.h"     /* channelMuxInit, channelSend, CHANNEL_GAME */
#include "bulk_transfer.h"   /* bulkSenderReset, bulkReceiverInit */
#include "playername_validate.h" /* playerNameCompare */
#include "log.h"             /* logAddEvent, log_BalanceApplied */
#include "../threads.h"      /* threadsWaitForMutex, threadsReleaseMutex,
                              * threadsContextActive, threadsCurrentlyHoldsMutex */
#include "../../common/wb_log.h"      /* WB_LOG_*, WB_LOG_CAT_NET */
#include "../../common/mp_diag_log.h" /* mpDiagLog */
#include "../sim/server_sim_shared.h"  /* serverSimAnnounce */
#include "../../winbolonet/winbolonet_core.h"   /* winbolonetAddEvent,
                                                 * WINBOLO_NET_EVENT_QUITTING,
                                                 * WINBOLO_NET_NO_PLAYER */
#include "../../winbolonet/winbolonet_server.h" /* winboloNetClientLeaveGame,
                                                 * winboloNetSendLock */

/* Forward declaration */
static bool serverClientsAllLocked(void);

/* Data passed to the balance thread — snapshot of values needed for the
 * HTTP call so the thread doesn't read ServerSim without the mutex. */
typedef struct {
    ServerSim *sim;
    uint8_t    totalPlayers;
    uint8_t    teamSize;
    uint8_t    botSlots[MAX_TANKS];   /* bot slots to include; empty if !includeBots */
    uint8_t    numBotSlots;
    bool       includeBots;
} BalanceThreadData;

/* Background thread: calls WBN balance API (blocks on HTTP) then writes
 * results back under the game mutex so the timer can broadcast them. */
static int balanceThreadFunc(void *data) {
    BalanceThreadData *btd = (BalanceThreadData *)data;
    ServerSim *sim = btd->sim;
    bool includeBots = btd->includeBots;  /* captured before free(btd) below */

    /* This blocks on HTTP — runs outside the game mutex */
    serverSimRequestBalanceProposal(sim, btd->totalPlayers, btd->teamSize,
                                     btd->numBotSlots > 0 ? btd->botSlots : NULL,
                                     btd->numBotSlots);

    free(btd);

    /* If the server is shutting down, signal completion and exit without
     * acquiring the mutex (the main thread may have already torn it down). */
    if (serverSimBalanceShutdownRequested(sim)) {
        serverSimSetBalanceRequestInFlight(sim, false);
        return 0;
    }

    /* Write results back under the game mutex. No approval step — the
     * host already committed to the rebalance by confirming the popup,
     * so apply WBN's assignments directly and broadcast the new slots. */
    threadsWaitForMutex();
    serverSimSetBalanceRequestInFlight(sim, false);
    if (serverSimGetBalanceProposal(sim)->pending) {
        serverSimSetBalanceIncludeBots(sim, includeBots);
        int i;
        /* Publish the proposal data first so every client's dispatcher
         * latches lastBalanceProposalArrivedMs — gives the host's
         * "Teams balanced" status label a chance to fire even though
         * we'll clear right after applying. */
        {
            ControlEvent propEvt;
            memset(&propEvt, 0, sizeof(propEvt));
            propEvt.type = CTRL_BALANCE_PROPOSAL;
            memcpy(propEvt.u.balanceProposal.teamForSlot,
                   serverSimGetBalanceProposal(sim)->teamForSlot,
                   MAX_TANKS);
            serverSimPublishControl(sim, &propEvt);
        }
        /* "Humans only" kicks every bot before applying the human-only
         * team assignments. */
        if (!includeBots) {
            for (i = 0; i < MAX_TANKS; i++) {
                if (serverSimIsBot(sim, (BYTE)i)) {
                    serverSimRemoveBot(sim, (BYTE)i);
                }
            }
        }
        for (i = 0; i < MAX_TANKS; i++) {
            if (serverSimGetBalanceProposal(sim)->teamForSlot[i] != 0) {
                serverSimSetTeamBatch(sim, (BYTE)i,
                                      serverSimGetBalanceProposal(sim)->teamForSlot[i]);
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
        }
        serverSimReapplyTeamAlliances(sim);
        /* Re-pick the reservations the new teams' sides no longer allow —
         * the batch left each moved slot on the start it held for its old
         * team. A start still allowed is kept. Same as the
         * CMD_BALANCE_APPLY arm in server_command_dispatch.c. */
        serverSimReleaseIneligibleStartsAndBackfill(sim, 0xFF);
        serverSimClearBalanceProposal(sim);
        /* Publish the cleared proposal so balanceProposalActive flips
         * back to false on every client — keeps canBalance gating
         * from staying disabled on the Balance-from-WBN button. */
        {
            ControlEvent clrEvt;
            memset(&clrEvt, 0, sizeof(clrEvt));
            clrEvt.type = CTRL_BALANCE_PROPOSAL;
            serverSimPublishControl(sim, &clrEvt);
        }
        logAddEvent(log_BalanceApplied, 0, 0, 0, 0, 0, NULL);
        serverSimConsoleMessage("Team balance applied (WBN)");
    } else {
        /* WBN call returned without a usable proposal (non-200, null
         * body, or an "error" field — see winbolonet_server.c). Tell
         * the host so its "Asking WBN…" pill can flip immediately. */
        ControlEvent failEvt;
        memset(&failEvt, 0, sizeof(failEvt));
        failEvt.type = CTRL_BALANCE_FAILED;
        failEvt.u.balanceFailed.reasonCode = 1; /* http/transport */
        serverSimPublishControl(sim, &failEvt);
    }
    threadsReleaseMutex();
    return 0;
}

/* Disconnect a player by index.
 * graceful=TRUE means the player chose to quit (sends "is quitting" message).
 * graceful=FALSE means timeout or kick (no quit message). */
/* Check if all connected clients have voted to lock.
 * Returns TRUE only if there is at least one client and all are locked. */
static bool serverClientsAllLocked(void) {
    int i;
    bool anyConnected = false;
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            anyConnected = true;
            if (!udpServer.clientLocked[i]) return false;
        }
    }
    return anyConnected;
}

void serverDisconnectClient(ServerSim *sim, int idx, bool graceful) {
    char msg[128];
    if (!udpServer.clients[idx].connected) return;
    /* An actual disconnect supersedes any deferred overflow-disconnect for
     * this slot (see the queue-overflow branch in udpClientDeliverControl):
     * clear the flag so the drain can't later tear down a fresh occupant that
     * reused the slot after this teardown frees it. */
    udpServer.pendingSimRemove[idx] = false;

    {
        UdpServerClient *c = &udpServer.clients[idx];
        (void)c;
        WB_LOG_DEBUG(WB_LOG_CAT_NET,
            "disconnect slot=%d name='%s' addr=%s:%u graceful=%d "
            "tickCount=%u lastReceivedTick=%u tickDiff=%u (timeout=%d)",
            idx, c->playerName,
            inet_ntoa(c->addr.sin_addr), (unsigned)ntohs(c->addr.sin_port),
            (int)graceful,
            (unsigned)udpServer.tickCount,
            (unsigned)c->lastReceivedTick,
            (unsigned)(udpServer.tickCount - c->lastReceivedTick),
            (int)CLIENT_TIMEOUT_TICKS);
        mpDiagLog("[srv] DISCONNECT slot=%d name='%s' graceful=%d tickDiff=%u (timeout=%d)",
                  idx, c->playerName, (int)graceful,
                  (unsigned)(udpServer.tickCount - c->lastReceivedTick),
                  (int)CLIENT_TIMEOUT_TICKS);
    }

    if (graceful) {
        snprintf(msg, sizeof(msg), "%s is quitting.",
                 udpServer.clients[idx].playerName);
        winbolonetAddEvent(WINBOLO_NET_EVENT_QUITTING, TRUE,
                           (BYTE)idx, WINBOLO_NET_NO_PLAYER, FALSE, FALSE);
    } else {
        snprintf(msg, sizeof(msg), "%s timed out.",
                 udpServer.clients[idx].playerName);
    }
    WB_LOG_INFO(WB_LOG_CAT_NET, "%s", msg);
    fprintf(stderr, "[UDP SERVER] %s\n", msg);
    serverSimConsoleMessage(msg);

    /* Mirror the leave into every client's lobby chat panel via
     * CTRL_SERVER_TEXT. Console keeps the graceful-vs-timeout detail
     * (`msg` above); the chat line is the uniform "X has left." form
     * — players don't need the distinction and it matches what the
     * client-side wire-packet branch used to render. Done before the
     * subscriber/slot teardown below so the leaving client's
     * still-attached subscriber sees it if they're reachable, and
     * the formatted name is still in udpServer.clients[idx].playerName
     * (wiped below). */
    {
        char chatMsg[32 + PACKET_MAX_PLAYER_NAME];
        snprintf(chatMsg, sizeof(chatMsg), "%s has left.",
                 udpServer.clients[idx].playerName);
        /* Flip connected=false BEFORE the broadcast. Without this, the
         * "X has left." event re-enters udpClientDeliverControl for this
         * same slot; if the leaving slot's queue is what overflowed in
         * the first place (the path that brought us into this function
         * via the queue-overflow branch), the enqueue fails again and
         * recurses back into serverDisconnectClient — unbounded
         * recursion until the timer thread's stack blows. The deliver
         * callback's existing !connected short-circuit makes the
         * broadcast a no-op for this slot; the other slots still see
         * "X has left." normally. */
        udpServer.clients[idx].connected = false;
        if (serverSimAnnounce(sim, ANNOUNCE_KIND_LEFT, (BYTE)idx, (BYTE)idx)) {
            serverSendServerEnglishBroadcast(sim, chatMsg);
        }
    }

    /* Notify WinBolo.net that the player is leaving (must happen before
     * clearing the slot so the player key is still valid) */
    winboloNetClientLeaveGame((BYTE)idx,
                              serverSimGetNumPlayers(sim),
                              serverSimGetNumNeutralBases(sim),
                              serverSimGetNumNeutralPills(sim));

    /* PACKET_PLAYER_LEFT is fanned out by the codec when the outer caller
     * invokes serverSimRemovePlayer; no hand-built broadcast here. */
    serverSimUnregisterSubscriber(sim, udpServer.clients[idx].controlSub);
    udpServer.clients[idx].controlSub = SUBSCRIBER_HANDLE_INVALID;
    udpServer.clients[idx].nameStickySuffix = false;
    udpServer.clients[idx].claimPending = false;
    udpServer.clients[idx].claimDesiredName[0] = '\0';
    udpServer.clients[idx].inboundCmdSeq = 0;
    /* Slots are recycled: without this a new occupant would inherit the
     * previous player's mutes. */
    udpServer.clients[idx].voiceMuteMask = 0;
    /* The other direction of the same hazard, and the one that is invisible
     * to the player it hits: everyone who muted THIS player still holds the
     * bit for this slot, so the next occupant would arrive already muted for
     * them — no voice, and no chat either, since both read this one bit.
     * Sweeping the leaver's bit out of every mask is what makes a mute
     * belong to the player rather than to the slot they sat in. */
    {
        const PlayerBitMap leaving = ~((PlayerBitMap)1u << idx);
        int muter;
        for (muter = 0; muter < MAX_TANKS; muter++) {
            udpServer.clients[muter].voiceMuteMask &= leaving;
        }
    }
    /* Same reason: a recycled slot inheriting the previous player's onset
     * would hand a new joiner talker priority they did not earn. */
    udpServer.voiceLastFrameTick[idx] = 0;
    udpServer.voiceOnsetTick[idx] = 0;
    /* The diagnostic counters go with them, so a joiner does not inherit the
     * previous occupant's drops and read as a bad client, and the flood
     * credit starts empty rather than handing a joiner a free burst. */
    memset(&udpServer.voiceSlot[idx], 0, sizeof(udpServer.voiceSlot[idx]));
    udpServer.voiceCredits[idx] = 0;
    /* If the set every other client holds names this slot as talking, they go
     * on showing it until they are told otherwise, and clearing the tick above
     * is what stops the next pass computing a set that differs from the
     * published one. Ask for the send explicitly so the leaver goes out of that
     * set even when what remains is empty — which is exactly the case where
     * nothing else would send it. A leaver nobody was hearing is not in the
     * published set and costs no event. */
    if ((udpServer.voiceTalkingPublished & ((PlayerBitMap)1u << idx)) != 0) {
        udpServer.voiceTalkingResend = true;
    }
    memset(udpServer.clients[idx].playerName, 0, PACKET_MAX_PLAYER_NAME);
    udpServer.clientLocked[idx] = false;
    /* Drop any owed PLAYER_JOIN — the player left before it resolved, so
     * no orphan anonymous join (and no leave it would need to pair with). */
    wbnJoinClear(&udpServer.clients[idx].wbnJoin);
    /* And any verify still out for the leaver: its result is already dropped
     * on the connId check, and a live entry here would defer the next
     * occupant's announcement to a reply that is no longer for this slot. */
    udpServerClearReauthPending((BYTE)idx);
    /* Slot is free; a fresh occupant re-establishes WBN status at its join. */
    udpServer.clients[idx].wbnWasVerified = false;
    udpServer.clients[idx].wbnWebIdentityCached = false;
    udpServer.clients[idx].wbnWebIsLoggedIn = false;
    udpServer.clients[idx].wbnWebName[0] = '\0';
    udpServer.clients[idx].wbnWebCountry[0] = '\0';
    udpServer.clients[idx].wbnWebUserId = -1;

    /* Clear the high-ping enforcement tally. These count consecutive breaches
     * by ONE occupant, so leaving them set hands the next occupant of this slot
     * the departing player's strikes. A player kicked for high ping leaves with
     * pingKickStrikes at PING_KICK_COUNT, so on their reconnect the first
     * measurement over the threshold reaches PING_KICK_COUNT + 1 and kicks them
     * again immediately, with none of the PING_KICK_COUNT samples of grace the
     * enforcement is built around — and pingWarned still true, so they get no
     * warning first either. lastEnforcedPingMs goes too: it is the dedup for
     * "already counted this measurement", and a stale value silently skips the
     * next occupant's first matching sample. */
    udpServer.clients[idx].pingKickStrikes = 0;
    udpServer.clients[idx].pingWarnStrikes = 0;
    udpServer.clients[idx].pingWarned = false;
    udpServer.clients[idx].lastEnforcedPingMs = 0;

    /* Reset control-sync state so a re-using slot starts fresh. */
    udpServer.controlSyncInProgress[idx] = false;

    /* Hand the slot's copy of the terrain back to the tick: nothing is being
     * sent map events on this slot any more, and a bot or the host player can
     * take it next, which is a client that gets every change. */
    serverSimSetShadowCulled(sim, (BYTE)idx, false);

    /* Reset the channel mux so a re-using slot starts fresh. */
    channelMuxInit(&udpServer.channelMux[idx]);
    udpServer.channelFramesRx[idx] = 0;
    udpServer.mapGen[idx] = 0;
    bulkSenderReset(&udpServer.bulkSend[idx]);
    bulkReceiverInit(&udpServer.bulkRecvUp[idx]);
    udpServerResetRoundLogLimits(idx);
    udpServerResetMapReaskLimit(idx);

    /* Release any in-flight upload state. Without this, a client
     * who drops mid-upload would leave clientUploadActive set,
     * blocking every subsequent UPLOAD_BEGIN from a different
     * client with LOBBY_REJECT_UPLOAD_BUSY until server restart. */
    udpServerClearClientUploadState(idx);

    /* Unready any humans who were ready — a leaver changes the lobby
     * composition. A no-op in running state (no one is ready then).
     * Outer callers also fire serverSimRemovePlayer immediately after
     * this; running this here rather than in each caller keeps the
     * leave-side hook centralized alongside the chat broadcast above. */
    lobbyAutoUnreadyOnChange(sim);
}

void transportUdpServerDrainPendingRemovals(ServerSim *sim) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.pendingSimRemove[i]) continue;
        udpServer.pendingSimRemove[i] = false;
        /* The slot was flagged on a control-queue overflow detected inside a
         * publish (udpClientDeliverControl); the whole disconnect was deferred
         * to here so its leave broadcast + PLAYER_LEFT fan-out run outside any
         * publish. If the slot is no longer connected, another path already
         * disconnected it (and cleared this flag before any reuse), so there is
         * nothing to tear down. */
        if (!udpServer.clients[i].connected) continue;
        /* Same teardown order every other disconnect path uses — the download
         * buffer is caller-owned, serverDisconnectClient does not free it. */
        serverCleanupMapDownload(i);
        serverDisconnectClient(sim, i, false);
        serverSimRemovePlayer(sim, (BYTE)i);
    }
}

/* Send a localized server-originated message to all connected clients
 * via PACKET_CHAT_BROADCAST.  Uses fromPlayer=0xFF, destPlayer=0xFF to
 * mark the localized variant; client decodes langid + args and renders
 * via langGetTextFmt.  The packed langid+args payload rides as the
 * opaque body[] of CTRL_CHAT; the per-client codec encoder fans it out. */
void serverSendServerMessage(ServerSim *sim, langid id, int argCount,
                             const char *const args[]) {
    /* Scratch buffer mirrors the wire encoding so we can reuse
     * packLocalizedPayload; only the bytes after PACKET_HEADER_SIZE+2
     * become the CTRL_CHAT body. */
    uint8_t scratch[PACKET_HEADER_SIZE + 2 + 3 + 4 * (1 + PLAYER_NAME_LEN - 1)];
    int pos = PACKET_HEADER_SIZE + 2;
    ControlEvent evt;

    if (!packLocalizedPayload(scratch, &pos, sizeof(scratch), id, argCount, args)) {
        fprintf(stderr,
                "[UDP SERVER] serverSendServerMessage: pack failed id=%u argc=%d\n",
                (unsigned)id, argCount);
        return;
    }

    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_CHAT;
    evt.u.chat.fromPlayer = 0xFF; /* server localized */
    evt.u.chat.destPlayer = 0xFF; /* broadcast */
    evt.u.chat.bodyLen = (uint16_t)(pos - (PACKET_HEADER_SIZE + 2));
    if (evt.u.chat.bodyLen > 0) {
        memcpy(evt.u.chat.body, scratch + PACKET_HEADER_SIZE + 2,
               evt.u.chat.bodyLen);
    }
    serverSimPublishControl(sim, &evt);
}

/* Send a raw English server-originated message to all connected clients.
 * Uses fromPlayer=0xFE to mark the legacy English variant — used by
 * server-ops broadcasts (admin "say", lock toggle, ping enforcement)
 * that don't yet have dedicated langids.  As individual messages are
 * localized they should migrate to serverSendServerMessage above. */
void serverSendServerEnglishBroadcast(ServerSim *sim, const char *message) {
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

void transportUdpServerKickPlayer(ServerSim *sim, const char *playerName) {
    int i;
    char msg[128];

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            const char *kickArgs[1];
            snprintf(msg, sizeof(msg), "%s has been server kicked.",
                     udpServer.clients[i].playerName);
            WB_LOG_WARN(WB_LOG_CAT_NET, "admin kick slot=%d name='%s'",
                        i, udpServer.clients[i].playerName);
            fprintf(stderr, "[UDP SERVER] %s\n", msg);
            serverSimConsoleMessage(msg);
            /* Send kick message to all clients (including the kicked player) */
            kickArgs[0] = udpServer.clients[i].playerName;
            serverSendServerMessage(sim, STR_KICK_ANNOUNCE, 1, kickArgs);
            /* Hand the kicked client an immediate disconnect notification so
             * they don't sit waiting for the keepalive timeout. PACKET_KICKED
             * transitions the client's joinState to UDP_CLIENT_KICKED, which
             * surfaces a "you were kicked" dialog in the lobby. */
            {
                uint8_t kbuf[PACKET_HEADER_SIZE];
                packHeader(kbuf, PACKET_KICKED, 0);
                srvSendTo(kbuf, sizeof(kbuf), &udpServer.clients[i].addr);
            }
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
            serverSimRemovePlayer(sim, (BYTE)i);
            /* Broadcast lobby update if in lobby/countdown state */
            if (serverSimIsLobbyEnabled(sim) &&
                (serverSimGetState(sim) == serverStateLobby || serverSimGetState(sim) == serverStateCountdown)) {
                serverSimPublishLobbySlot(sim, (BYTE)i);
            }
            return;
        }
    }

    /* No human matched. Bots have no UDP client, so the connected check
     * above skipped them — but their display name is stored in the same
     * playerName slot via transportUdpServerSetBotName, so match on that
     * and fall back to bot removal. serverSimRemoveBot runs the same
     * teardown the crash-streak kick uses (botManagerRemoveBot ->
     * serverSimRemovePlayer, the human-leave path) and publishes the freed
     * lobby slot. It is safe mid-game: the caller already holds the sim
     * mutex, exactly as a human kick does. There is no client to hand a
     * PACKET_KICKED, so that step is simply absent. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsBot(sim, (BYTE)i)) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            const char *kickArgs[1];
            snprintf(msg, sizeof(msg), "%s has been server kicked.",
                     udpServer.clients[i].playerName);
            WB_LOG_WARN(WB_LOG_CAT_NET, "admin kick bot slot=%d name='%s'",
                        i, udpServer.clients[i].playerName);
            fprintf(stderr, "[UDP SERVER] %s\n", msg);
            serverSimConsoleMessage(msg);
            kickArgs[0] = udpServer.clients[i].playerName;
            serverSendServerMessage(sim, STR_KICK_ANNOUNCE, 1, kickArgs);
            serverSimRemoveBot(sim, (BYTE)i);
            return;
        }
    }

    serverSimConsoleMessage("Player not found.");
}

void transportUdpServerDisconnectAll(ServerSim *sim) {
    int i;
    bool anyConnected = false;

    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            anyConnected = true;
            break;
        }
    }

    /* Tell every connected client the round is over before tearing their
     * slots down, so they return to the server browser cleanly instead of
     * waiting out the keepalive timeout. The per-client codec subscriber
     * unicasts PACKET_SERVER_SHUTDOWN during this publish (same mechanism
     * transportUdpServerDestroy uses). */
    if (anyConnected) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_SERVER_SHUTDOWN;
        serverSimPublishControl(sim, &evt);
    }

    /* Boot every human client. serverDisconnectClient handles the
     * transport-side teardown for real UDP clients; serverSimRemovePlayer then
     * clears the sim-side player (gated on the sim's own connected flag, which
     * serverDisconnectClient leaves set). Bots are deliberately kept: they are
     * server configuration, not joined players, so they persist across a map
     * rotation exactly as they do across a normal lobby round (the caller
     * re-arms them for the new round via botManagerOnGameStart). A bot has no
     * udpServer.clients entry, so it is skipped by the connected check; the
     * removal is additionally gated on !serverSimIsBot so it survives. Removal
     * is index-based and doesn't compact the arrays, so a plain forward loop is
     * safe. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (udpServer.clients[i].connected) {
            serverCleanupMapDownload(i);
            serverDisconnectClient(sim, i, FALSE);
        }
        if (serverSimIsPlayerConnected(sim, (BYTE)i) &&
            !serverSimIsBot(sim, (BYTE)i)) {
            serverSimRemovePlayer(sim, (BYTE)i);
        }
    }
}

bool transportUdpServerSetHostByName(ServerSim *sim, const char *playerName) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        if (serverSimIsBot(sim, (BYTE)i)) continue;
        if (playerNameCompare(udpServer.clients[i].playerName, playerName) == 0) {
            serverSimSetHostSlot(sim, (BYTE)i);
            return true;
        }
    }
    return false;
}

void transportUdpServerEnforcePing(ServerSim *sim) {
    int i;
    if (serverSimGetState(sim) != serverStateRunning) return;

    for (i = 0; i < MAX_TANKS; i++) {
        UdpServerClient *client = &udpServer.clients[i];
        uint16_t ping;
        if (!client->connected) continue;

        /* Already queued for teardown by an earlier strike — the slot stays
         * connected until the drain runs, so skip it rather than re-striking
         * (and re-broadcasting) it on the intervening half-steps. */
        if (udpServer.pendingSimRemove[i]) continue;

        ping = client->pingMs;
        if (ping == 0) continue;  /* No measurement yet */
        if (ping == client->lastEnforcedPingMs) continue;  /* Same measurement, already checked */
        client->lastEnforcedPingMs = ping;

        /* Kick threshold */
        if (ping >= PING_KICK_THRESHOLD_MS) {
            client->pingKickStrikes++;
            if (client->pingKickStrikes >= PING_KICK_COUNT) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "%s kicked for high ping (%dms).",
                         client->playerName, ping);
                WB_LOG_WARN(WB_LOG_CAT_NET,
                    "ping-kick slot=%d name='%s' ping=%ums strikes=%d/%d threshold=%d",
                    i, client->playerName, (unsigned)ping,
                    (int)client->pingKickStrikes, (int)PING_KICK_COUNT,
                    (int)PING_KICK_THRESHOLD_MS);
                fprintf(stderr, "[UDP SERVER] %s\n", msg);
                serverSimConsoleMessage(msg);
                serverSendServerEnglishBroadcast(sim, msg);
                /* Teardown is DEFERRED, not run here. This function is called
                 * from simRunHalfStep, mid-sim-frame; serverSimRemovePlayer
                 * frees the slot's tank and lgm objects, and the half-step's
                 * world-update stage holds pointers into both. Freeing here
                 * left shellsUpdate dereferencing a NULLed lgm slot (fault at
                 * lgmObj::playerNum, offset 0x18) and walking freed tank
                 * pointers. Flag the slot instead and let
                 * transportUdpServerDrainPendingRemovals do the teardown at
                 * the top of the next tick — the same safe point the
                 * control-queue-overflow disconnect uses. The kick is
                 * announced now; only the free is delayed by one tick. */
                udpServer.pendingSimRemove[i] = true;
                continue;
            }
        } else {
            client->pingKickStrikes = 0;
        }

        /* Warn threshold */
        if (ping >= PING_WARN_THRESHOLD_MS) {
            client->pingWarnStrikes++;
            if (client->pingWarnStrikes >= PING_WARN_COUNT && !client->pingWarned) {
                char msg[128];
                snprintf(msg, sizeof(msg),
                         "%s has high ping (%dms) and may be kicked.",
                         client->playerName, ping);
                serverSendServerEnglishBroadcast(sim, msg);
                client->pingWarned = true;
            }
        } else {
            client->pingWarnStrikes = 0;
            client->pingWarned = false;
        }
    }
}

void transportUdpServerSetBotName(BYTE playerNum, const char *name) {
    if (playerNum >= MAX_TANKS) return;
    strncpy(udpServer.clients[playerNum].playerName, name,
            PACKET_MAX_PLAYER_NAME - 1);
    udpServer.clients[playerNum].playerName[PACKET_MAX_PLAYER_NAME - 1] = '\0';
}

bool transportUdpServerStartBalanceRequest(ServerSim *sim,
                                           uint8_t teamSize,
                                           bool includeBots) {
    BalanceThreadData *btd = malloc(sizeof(BalanceThreadData));
    if (!btd) {
        return false;
    }
    SDL_Thread *t;
    int i;
    memset(btd, 0, sizeof(*btd));
    btd->sim = sim;
    btd->teamSize = teamSize;
    btd->includeBots = includeBots;
    btd->totalPlayers = 0;
    btd->numBotSlots = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!serverSimIsPlayerConnected(sim, i)) continue;
        btd->totalPlayers++;
        /* Include bot slots in the WBN request only when the caller
         * asked for "Bots included". When !includeBots the bots are
         * kicked at APPLY time and don't need to be skill-placed. */
        if (btd->includeBots && serverSimIsBot(sim, (BYTE)i)) {
            btd->botSlots[btd->numBotSlots++] = (uint8_t)i;
        }
    }
    serverSimSetBalanceRequestInFlight(sim, true);
    t = SDL_CreateThread(balanceThreadFunc, "WbnBalance", btd);
    if (!t) {
        serverSimSetBalanceRequestInFlight(sim, false);
        free(btd);
        serverSimConsoleMessage("Failed to start balance thread");
        return false;
    }
    SDL_DetachThread(t);
    return true;
}

/* Set or clear the server's "locked to new players" state and announce the
 * change to every connected, download-complete client with a reliable
 * EVENT_SERVER_MSG on CHANNEL_GAME — the same path the per-tick game-event
 * producer uses (transportUdpServerDrainEvents).
 *
 * Precondition: the caller must hold threadsMutex.  The notice goes out via
 * channelSend, which mutates per-client channelMux state that is otherwise only
 * touched on the tick; the mutex is what serializes the two.  All callers
 * comply — the servermain console wraps each call (lock/unlock/alarm), and the
 * server_sim auto-lock/unlock callers run inside serverInstanceTick.  The
 * assert no-ops in bare logic unit tests that never start the threading system
 * (no tick thread to race, and no client is connected there to send to). */
void transportUdpServerSetLock(ServerSim *sim, bool locked) {
    (void)sim;
    SDL_assert(!threadsContextActive() || threadsCurrentlyHoldsMutex());
    if (udpServer.gameLocked == locked) return;
    udpServer.gameLocked = locked;
    serverSimConsoleMessage(locked
        ? "This game is now locked to new players (server lock)"
        : "This game is now unlocked to new players (server unlock)");
    winboloNetSendLock(locked);
    /* Announce on the reliable game channel.  serverSimAddEvent() won't do —
     * the sim's per-tick event buffer is cleared at the start of each tick and
     * this runs between ticks — so pack the event once and channelSend it onto
     * CHANNEL_GAME per connected, download-complete client, exactly as the
     * per-tick producer does. */
    {
        GameEvent ev;
        uint8_t evBuf[GAME_EVENT_MAX_WIRE_SIZE];
        int evLen;
        int c;
        ev.type = EVENT_SERVER_MSG;
        memset(ev.data, 0, sizeof(ev.data));
        ev.data[0] = locked ? SERVER_MSG_GAME_LOCKED : SERVER_MSG_GAME_UNLOCKED;
        evLen = packGameEvent(evBuf, &ev);
        for (c = 0; c < MAX_TANKS; c++) {
            if (!udpServer.clients[c].connected) continue;
            if (!udpServer.mapDownload[c].downloadComplete) continue;
            if (!channelSend(&udpServer.channelMux[c], CHANNEL_GAME,
                             evBuf, (uint16_t)evLen)) {
                /* Window full — defer the disconnect off this path, mirroring
                 * the per-tick game-channel overflow handling.  The flag guard
                 * keeps a re-hit from spamming the log. */
                if (!udpServer.pendingSimRemove[c]) {
                    WB_LOG_ERROR(WB_LOG_CAT_NET,
                                 "game channel overflow for slot %d, deferring disconnect",
                                 c);
                    udpServer.pendingSimRemove[c] = true;
                }
            }
        }
    }
}

bool transportUdpServerGetLock(void) {
    return udpServer.gameLocked;
}

void transportUdpServerSendServerMessage(const char *message) {
    /* Public API: external callers (servermain console "say", saveMap
     * announcement, server_lifecycle pendingWinMessage) pass arbitrary
     * pre-rendered English strings.  These don't have dedicated langids
     * yet, so route through the legacy English passthrough wire format.
     * Sim handle comes from serverSimGetActive() — the public signature
     * doesn't carry it, matching the pattern other public entry points
     * in this TU use when they need the active sim. */
    ServerSim *sim = serverSimGetActive();
    if (sim == NULL) return;
    serverSendServerEnglishBroadcast(sim, message);
}

void transportUdpServerPrintStatus(bool toFile) {
    int i;
    FILE *fp = NULL;

    if (toFile) {
        fp = fopen("status.txt", "w");
    }

    for (i = 0; i < MAX_TANKS; i++) {
        if (!udpServer.clients[i].connected) continue;
        fprintf(stdout, "%s (slot %d, ping %dms)\n",
                udpServer.clients[i].playerName, i,
                udpServer.clients[i].pingMs);
        if (fp != NULL) {
            fprintf(fp, "%s (slot %d, ping %dms)\n",
                    udpServer.clients[i].playerName, i,
                    udpServer.clients[i].pingMs);
        }
    }

    if (fp != NULL) {
        fclose(fp);
    }
}
