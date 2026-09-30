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
 *Name:          Server Simulation Local Round Steps
 *Filename:      server_sim_lifecycle_local.c
 *Author:        John Morrison
 *Purpose:
 *  The lobby and round steps of the server's tick that a
 *  game with no remote clients still needs: the bot tick,
 *  the game-over, game-start and countdown publishes, the
 *  balance proposal, the lobby republish on the way back
 *  from a round, and the lobby-slot heartbeat. The server's
 *  tick (serverInstanceTick) calls each at its point in the
 *  tick, between the network work around it; the browser
 *  client's local tick calls the same functions.
 *
 *  None of these take a lock. They run under whatever the
 *  caller holds: the server's tick holds the tick mutex.
 *********************************************************/

#include <string.h>

#include "server_sim_shared.h"      /* publishServerEnglishBroadcast — the win message */
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"   /* the declarations of the functions below */
#include "control_event.h"          /* ControlEvent, the CTRL_* phase and lobby events */
#include "round_stats.h"            /* POSTGAME_STATS_ENABLED */
#include "bot_manager.h"            /* botManagerOnGameStart */

/* Adapter: serverSimEmitBrainAnnounces hands each event to a deliver
 * callback; the return-to-lobby path wants them on the broadcast bus. */
static void serverSimPublishBrainAnnounceCb(void *ctx, const ControlEvent *evt) {
    serverSimPublishControl((ServerSim *)ctx, evt);
}

/* Runs the brain bots for a running round, queueing two InputPackets per
 * bot (keys + game). The caller ticks the sim after it. Called by the
 * server's tick and the browser client's local tick. */
void serverSimLocalBotTick(ServerSim *sim) {
    if (serverSimGetNumBots(sim) > 0) {
        serverSimBotTick(sim, sim->botAiType);
    }
}

/* The running -> game-over edge: decides the win/exit message and WBN
 * crediting, publishes the game-over phase and event, and ships the
 * round's scoreboard and awards. Called by the server's tick and the
 * browser client's local tick, on the tick that ended the round. */
void serverSimLocalOnGameOver(ServerSim *sim) {
    /* Decide the win/exit message and WBN crediting. The policy lives in
     * the sim core (serverSimResolveGameOver) so the dedicated server and
     * the in-process SP/host both resolve a game over identically. */
    serverSimResolveGameOver(sim);
    {
        ControlEvent phaseEvt;
        ControlEvent overEvt;
        memset(&phaseEvt, 0, sizeof(phaseEvt));
        phaseEvt.type = CTRL_GAME_PHASE_GAME_OVER;
        serverSimPublishControl(sim, &phaseEvt);
        memset(&overEvt, 0, sizeof(overEvt));
        overEvt.type = CTRL_GAME_OVER;
        serverSimPublishControl(sim, &overEvt);
    }
    /* Ship the round's scoreboard + awards while the accumulator is still
     * intact (returnToLobby clears it later). This path runs only for a
     * round that actually reached game-over. */
#if POSTGAME_STATS_ENABLED
    {
        ControlEvent rsEvt;
        rsEvt.type = CTRL_ROUND_STATS;
        serverSimBuildRoundStatsSummary(sim, &rsEvt.u.roundStats);
        serverSimPublishControl(sim, &rsEvt);
    }
#endif
}

/* Publishes a team-balance proposal that has just completed, once. Called
 * by the server's tick and the browser client's local tick, after a lobby,
 * countdown or game-over sim tick. */
void serverSimLocalPublishBalanceProposal(ServerSim *sim) {
    if (sim->balanceProposal.broadcastNeeded) {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_BALANCE_PROPOSAL;
        memcpy(evt.u.balanceProposal.teamForSlot,
               sim->balanceProposal.teamForSlot, MAX_TANKS);
        serverSimPublishControl(sim, &evt);
        sim->balanceProposal.broadcastNeeded = false;
    }
}

/* The countdown -> running edge: publishes the running phase, wires the
 * bots into the round and re-asserts team alliances. The server's tick
 * resets its per-client transport queues before calling this. Called by
 * the server's tick and the browser client's local tick. */
void serverSimLocalOnGameStart(ServerSim *sim) {
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_GAME_PHASE_RUNNING;
        serverSimPublishControl(sim, &evt);
    }
    /* The table this round runs on has already been stated: the tick
     * that ended the countdown ran serverSimStartGame, which publishes
     * it at the end of every start. */
    if (serverSimGetNumBots(sim) > 0) {
        botManagerOnGameStart(sim);
    }
    /* Re-assert team alliances now that (a) the reliable queues were
     * reset — discarding the CTRL_ALLIANCE_RESET the start sequence
     * published, which left remote clients rendering their own teammates
     * as enemies — and (b) botManagerOnGameStart just rebuilt the bot
     * ClientSims, whose alliance matrices start empty. One republish +
     * direct bot sync fixes both sides. */
    serverSimReapplyTeamAlliances(sim);
}

/* During the countdown, publishes the seconds left once per second. Does
 * nothing in any other state. Called by the server's tick and the browser
 * client's local tick, after a countdown sim tick that did not start the
 * round. */
void serverSimLocalCountdownTick(ServerSim *sim) {
    if (sim->state == serverStateCountdown &&
        sim->countdownTicks > 0 &&
        sim->countdownTicks % 50 == 0) {
        /* Broadcast countdown tick (once per second) */
        uint8_t secs = (uint8_t)((sim->countdownTicks + 49) / 50);
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        evt.type = CTRL_GAME_PHASE_COUNTDOWN;
        evt.u.gamePhase.countdownSeconds = secs;
        serverSimPublishControl(sim, &evt);
    }
}

/* The game-over -> lobby edge: republishes the brain catalogue and its
 * announce lines, the lobby settings and every slot, then sends the
 * pending win message. Called by the server's tick, after its WinBolo.net
 * session rotation, and by the browser client's local tick. */
void serverSimLocalOnReturnToLobby(ServerSim *sim) {
    /* Republish the bot brain catalogue.  Mid-game joiners were gated
     * out of the BrainList during their sync replay (see
     * serverSimSyncSubscriber), so they need it now before the lobby
     * UI's AiConfig combobox appears.  In-lobby clients get it as a
     * (cheap) refresh. */
    {
        ControlEvent evt;
        memset(&evt, 0, sizeof(evt));
        serverSimFillLobbyBrainListEvent(sim, &evt);
        serverSimPublishControl(sim, &evt);
        /* ... and the brains' announce lines that go with it, so the
         * returning lobby can announce a bot's brain the same way a fresh
         * join does. The refresh first: this seam between rounds is where an
         * operator would have edited a brain's texts, and it is off the tick
         * path, so a re-read costs nothing anybody feels. Docs that changed
         * get a new generation here, which tells each client to drop the
         * copy it holds. */
        serverSimRefreshBrainDocs(sim);
        serverSimEmitBrainAnnounces(sim, serverSimPublishBrainAnnounceCb, sim);
    }
    /* Republish lobby state so every client's mirror reflects the
     * fresh lobby. serverSimReturnToLobby's contract says the caller
     * does this fan-out; CTRL_GAME_PHASE_LOBBY alone doesn't carry
     * the inLobby flag or per-slot data, so without these the host's
     * own UDP loopback ClientSim leaves cs->inLobby false and never
     * opens the lobby dialog — the window looks frozen because there
     * is no game view either. */
    serverSimPublishLobbySettings(sim);
    {
        BYTE pi;
        /* Republish EVERY slot, not just connected ones. A player who
         * left mid-round had their CTRL_LOBBY_SLOT suppressed — the
         * leave-time publish is gated to lobby/countdown state
         * (transport_udp_server.c PACKET_QUIT), so a running-state quit
         * never told clients to clear that slot. The client's lobbySlots
         * mirror is only mutated by CTRL_LOBBY_SLOT (CTRL_PLAYER_LEAVE is
         * chat-only), so without this the departed player lingers as a
         * ghost in the returning lobby. A vacant slot fills as
         * connected=false (serverSimFillLobbySlotEvent), which clears it. */
        for (pi = 0; pi < MAX_TANKS; pi++) {
            serverSimPublishLobbySlot(sim, pi);
        }
    }
    /* Send the win message now that players are back in the lobby */
    if (sim->pendingWinMessage[0] != '\0') {
        publishServerEnglishBroadcast(sim, sim->pendingWinMessage);
        sim->pendingWinMessage[0] = '\0';
    }
}

/* Republishes every connected slot once every 250 ticks (~5 s) in the
 * lobby or countdown, so the lobby's ping column tracks live values
 * between unrelated slot changes. tickCount is the caller's tick counter:
 * the server's tick passes its transport's, the browser client's local
 * tick its own. Skipped in running state — snapshots already carry pingMs
 * per tick there. */
void serverSimLocalLobbySlotHeartbeat(ServerSim *sim, uint32_t tickCount) {
    if (tickCount % 250 == 0 &&
        (sim->state == serverStateLobby ||
         sim->state == serverStateCountdown)) {
        BYTE pi;
        for (pi = 0; pi < MAX_TANKS; pi++) {
            if (sim->playerConnected[pi]) {
                serverSimPublishLobbySlot(sim, pi);
            }
        }
    }
}
