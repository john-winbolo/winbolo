/*
 * Regression coverage for subscriber-arm side effects that were
 * orphaned when the wire layer migrated to the unified control bus.
 *
 * Each test dispatches a single ControlEvent through clientSimApplyControl
 * and asserts the matching ClientSim side effect — the dialog flag, the
 * inLobby flip, the lobby-chat append, etc. Pre-refactor the same side
 * effects lived in the now-dead PACKET_* handlers in transport_udp_client.c
 * and were never exercised by the codec or apply tests, so the bus
 * migration silently dropped them and CI stayed green.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_sim.h"
#include "client_sim_internal.h"
#include "client_sim_control.h"
#include "control_event.h"
#include "test_harness.h"

static ClientSim *fresh_client_sim(BYTE myPlayerNum, bool inLobby) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, myPlayerNum);
    cs->inLobby = inLobby;
    return cs;
}

/* ================================================================
 * CTRL_ALLIANCE_REQUEST — addressed slot flags pending; others ignore.
 *
 * Before the orphan fix the arm was a bare `break;` and the dialog
 * pop logic was unreachable on the wire path.
 * ================================================================ */
int run_alliance_request_flags_addressed(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/3, /*inLobby=*/false);
    UT_ASSERT(cs != NULL);
    UT_ASSERT(clientSimGetPendingAllianceRequest(cs) == 0xFF);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_REQUEST;
    evt.u.allianceRequest.fromPlayer = 5;
    evt.u.allianceRequest.toPlayer   = 3;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetPendingAllianceRequest(cs) == 5,
                  "addressed slot should record fromPlayer; got %u",
                  (unsigned)clientSimGetPendingAllianceRequest(cs));

    clientSimClearPendingAllianceRequest(cs);
    UT_ASSERT(clientSimGetPendingAllianceRequest(cs) == 0xFF);

    clientSimDestroy(cs);
    return 0;
}

int run_alliance_request_ignores_other_slot(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/3, /*inLobby=*/false);
    UT_ASSERT(cs != NULL);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_ALLIANCE_REQUEST;
    evt.u.allianceRequest.fromPlayer = 5;
    evt.u.allianceRequest.toPlayer   = 1;  /* not us */
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetPendingAllianceRequest(cs) == 0xFF,
                  "non-addressed slot must not flag; got %u",
                  (unsigned)clientSimGetPendingAllianceRequest(cs));

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_GAME_PHASE_LOBBY — flips cs->inLobby back to true.
 *
 * Pre-fix this arm only touched netStat/countdown. Vote-to-lobby and
 * surrender passes left the client stuck in the in-game UI because
 * clientSimIsInLobby kept returning false.
 * ================================================================ */
int run_phase_lobby_sets_inlobby(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/0, /*inLobby=*/false);
    UT_ASSERT(cs != NULL);

    /* Sanity: starting in-game. */
    UT_ASSERT(!clientSimIsInLobby(cs));

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_LOBBY;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimIsInLobby(cs),
                  "CTRL_GAME_PHASE_LOBBY must flip cs->inLobby=true");
    UT_ASSERT(clientSimGetNetStatus(cs) == netLobby);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_GAME_PHASE_GAME_OVER — resets state unconditionally.
 *
 * Pre-fix this arm was gated on cs->inLobby — meaning end-of-game
 * during a running game left netStat untouched on the client. The
 * unconditional reset lets the frontend transition out cleanly.
 * ================================================================ */
int run_phase_gameover_resets_in_game(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/0, /*inLobby=*/false);
    UT_ASSERT(cs != NULL);
    /* Force in-game state. */
    cs->netStat = netRunning;
    cs->countdownSeconds = 42;

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_GAME_PHASE_GAME_OVER;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(clientSimGetNetStatus(cs) == netLobby,
                  "game-over from in-game must drop netStat to netLobby");
    UT_ASSERT(cs->countdownSeconds == 0);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_PLAYER_JOIN — appends "X has joined." in lobby for non-self.
 * ================================================================ */
int run_player_join_appends_lobby_chat(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/0, /*inLobby=*/true);
    UT_ASSERT(cs != NULL);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_JOIN;
    evt.u.playerJoin.playerNum = 1;
    strncpy(evt.u.playerJoin.name, "Alice",
            sizeof(evt.u.playerJoin.name) - 1);
    evt.u.playerJoin.country[0] = 'A';
    evt.u.playerJoin.country[1] = 'U';
    clientSimApplyControl(cs, &evt);

    const char *hist = clientSimGetLobbyChatHistory(cs);
    UT_ASSERT_MSG(strstr(hist, "Alice has joined.") != NULL,
                  "lobby chat should contain join announcement; got '%s'",
                  hist);

    clientSimDestroy(cs);
    return 0;
}

int run_player_join_self_does_not_announce(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/1, /*inLobby=*/true);
    UT_ASSERT(cs != NULL);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_JOIN;
    evt.u.playerJoin.playerNum = 1;  /* self */
    strncpy(evt.u.playerJoin.name, "Me",
            sizeof(evt.u.playerJoin.name) - 1);
    clientSimApplyControl(cs, &evt);

    const char *hist = clientSimGetLobbyChatHistory(cs);
    UT_ASSERT_MSG(strstr(hist, "has joined") == NULL,
                  "self-join must not write to lobby chat; got '%s'",
                  hist);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_PLAYER_LEAVE — appends "X has left." in lobby for non-self.
 * ================================================================ */
int run_player_leave_appends_lobby_chat(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/0, /*inLobby=*/true);
    UT_ASSERT(cs != NULL);

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 2;
    strncpy(evt.u.playerLeave.name, "Bob",
            sizeof(evt.u.playerLeave.name) - 1);
    clientSimApplyControl(cs, &evt);

    const char *hist = clientSimGetLobbyChatHistory(cs);
    UT_ASSERT_MSG(strstr(hist, "Bob has left.") != NULL,
                  "lobby chat should contain leave announcement; got '%s'",
                  hist);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * CTRL_LOBBY_SETTINGS — clears stale balance proposal.
 *
 * The old PACKET_LOBBY_SETTINGS handler reset balanceProposalActive +
 * balanceProposal[] on every settings arrival ("fresh snapshot supersedes
 * any pending proposal"). After the bus migration that reset only happened
 * if the same packet was sent — but the packet is dead, so stale
 * "Teams balanced" labels could linger.
 * ================================================================ */
int run_lobby_settings_clears_balance_proposal(void) {
    ClientSim *cs = fresh_client_sim(/*myPlayerNum=*/0, /*inLobby=*/true);
    UT_ASSERT(cs != NULL);

    /* Seed a pending proposal that the arm should clear. */
    cs->balanceProposalActive = true;
    cs->balanceProposal[0] = 1;
    cs->balanceProposal[1] = 2;

    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_LOBBY_SETTINGS;
    strncpy(evt.u.lobbySettings.mapName, "TestMap",
            sizeof(evt.u.lobbySettings.mapName) - 1);
    evt.u.lobbySettings.hasLobby = true;
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(!cs->balanceProposalActive,
                  "fresh CTRL_LOBBY_SETTINGS must clear balanceProposalActive");
    for (int i = 0; i < MAX_TANKS; i++) {
        UT_ASSERT_MSG(cs->balanceProposal[i] == 0,
                      "balanceProposal[%d] should be zeroed", i);
    }

    clientSimDestroy(cs);
    return 0;
}
