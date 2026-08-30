/*
 * Surrender votes are private to the surrendering team.
 *
 * A surrender vote belongs to one team: only its members see that one is
 * running, watch the tally, and learn how it ended. Everyone else learns
 * of a surrender solely from the public "*** Team X has surrendered. ***"
 * outcome — and a FAILED surrender vote stays invisible to them entirely.
 *
 * Three surfaces enforce that, and this file pins each:
 *
 *   wire filter  — udpClientDeliverControl withholds a team-scoped
 *                  CTRL_GAME_VOTE_STATE from non-members, so the tally
 *                  never reaches a modified client that ignores the UI.
 *                  Observed white-box via the client's CHANNEL_CONTROL
 *                  send sequence: it advances when the event passed the
 *                  per-recipient filters and holds when one dropped it.
 *   newswire gate — clientSimApplyControl suppresses the "Surrender vote
 *                  started / failed" lines for non-members. This is what
 *                  covers the in-process bus (the host's own ClientSim),
 *                  which the per-client wire filter never sees.
 *   the rule      — clientSimMayAnswerGameVote, shared by the newswire
 *                  gate and the GUI widget so the two cannot drift.
 *
 * The trap each layer has to avoid: the base-monopoly auto-vote sets a
 * NON-ZERO teamId on a BACK_TO_LOBBY vote, which is public. Filtering on
 * teamId alone (without checking kind) would silently hide that vote from
 * everyone but the monopolising team, so every layer is asserted against
 * a team-scoped back-to-lobby event too.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"             /* GAME_VOTE_*, clientSimMayAnswerGameVote */
#include "client_sim_internal.h"    /* cs->lobbySlots, cs->isSpectator */
#include "client_sim_control.h"     /* clientSimApplyControl */
#include "client_net.h"             /* clientSimGetConnectState */
#include "client_connect_state.h"   /* CLIENT_CONNECT_CONNECTED */
#include "control_event.h"
#include "messages.h"               /* MessageState::queueCount */
#include "server_sim.h"
#include "server_sim_lifecycle.h"   /* serverSimSetTeam */
#include "transport_udp.h"          /* transportUdpServerGetClientControlSeq */
#include "test_harness.h"
#include "loopback_harness.h"

#define VP_PLAYER_CONNECT 4000

/* ================================================================
 * Layer 1 — the wire filter (udpClientDeliverControl)
 * ================================================================ */

static bool vpPlayerConnected(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static void vpFillVoteState(ControlEvent *evt, uint8_t kind, uint8_t teamId,
                            uint8_t active) {
    memset(evt, 0, sizeof(*evt));
    evt->type = CTRL_GAME_VOTE_STATE;
    evt->u.gameVoteState.kind             = kind;
    evt->u.gameVoteState.active           = active;
    evt->u.gameVoteState.triggerSrc       = GAME_VOTE_TRIGGER_MANUAL;
    evt->u.gameVoteState.teamId           = teamId;
    evt->u.gameVoteState.threshold        = 2;
    evt->u.gameVoteState.yesCount         = 1;
    evt->u.gameVoteState.noCount          = 0;
    evt->u.gameVoteState.eligibleCount    = 2;
    evt->u.gameVoteState.secondsRemaining = 30;
    evt->u.gameVoteState.votes            = 0x0002;
}

/* The harness's one real UDP client sits on a known team; publish
 * synthetic vote states and watch whether each reached its wire. */
int run_vote_surrender_state_withheld_from_other_team(void) {
    LoopbackHarness h;
    ControlEvent evt;
    BYTE slot;
    uint32_t before;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Alice", /*lobbyMode*/ true,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, VP_PLAYER_CONNECT,
                                           vpPlayerConnected, NULL) > 0,
                  "harness player never reached CONNECTED");

    slot = clientSimGetMyPlayerNum(h.cs);
    UT_ASSERT_MSG(slot < MAX_TANKS, "bad player slot %u", (unsigned)slot);
    serverSimSetTeam(h.sim, slot, 3);

    /* No pump from here on — serverSimPublishControl delivers synchronously,
     * so each seq delta is solely the publish under test. */

    /* (1) Another team's surrender vote must not reach this client at all. */
    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, /*teamId*/ 4,
                    GAME_VOTE_ACTIVE_RUNNING);
    before = transportUdpServerGetClientControlSeq(slot);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetClientControlSeq(slot) == before,
                  "team 4's surrender vote leaked to a team 3 client "
                  "(control seq %u -> %u)",
                  before, transportUdpServerGetClientControlSeq(slot));

    /* (2) ...nor may its conclusion. A failed surrender stays invisible. */
    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, /*teamId*/ 4,
                    GAME_VOTE_ACTIVE_FAILED);
    before = transportUdpServerGetClientControlSeq(slot);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetClientControlSeq(slot) == before,
                  "team 4's surrender conclusion leaked to a team 3 client");

    /* (3) The client's OWN team's surrender vote must still arrive. */
    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, /*teamId*/ 3,
                    GAME_VOTE_ACTIVE_RUNNING);
    before = transportUdpServerGetClientControlSeq(slot);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetClientControlSeq(slot) == before + 1,
                  "team 3 client did not receive its own surrender vote "
                  "(control seq %u -> %u)",
                  before, transportUdpServerGetClientControlSeq(slot));

    /* (4) The trap: a base-monopoly BACK_TO_LOBBY vote carries a non-zero
     * teamId but is public. It must reach a client on a different team. */
    vpFillVoteState(&evt, GAME_VOTE_KIND_BACK_TO_LOBBY, /*teamId*/ 4,
                    GAME_VOTE_ACTIVE_RUNNING);
    evt.u.gameVoteState.triggerSrc = GAME_VOTE_TRIGGER_BASE_MONOPOLY;
    before = transportUdpServerGetClientControlSeq(slot);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetClientControlSeq(slot) == before + 1,
                  "base-monopoly back-to-lobby vote (teamId=4) was wrongly "
                  "filtered from a team 3 client — the filter must check kind");

    /* (5) The idle/never-run surrender snapshot (teamId=0) the sync replay
     * emits carries no vote to hide and must still reach everyone. */
    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, /*teamId*/ 0,
                    GAME_VOTE_ACTIVE_NONE);
    before = transportUdpServerGetClientControlSeq(slot);
    serverSimPublishControl(h.sim, &evt);
    UT_ASSERT_MSG(transportUdpServerGetClientControlSeq(slot) == before + 1,
                  "idle surrender snapshot (teamId=0) was wrongly filtered");

    loopbackHarnessStop(&h);
    return 0;
}

/* ================================================================
 * Layer 2 — the newswire gate (clientSimApplyControl)
 * ================================================================ */

/* A ClientSim seated in slot 1 on `team`, with newswire display on. */
static ClientSim *vpClientOnTeam(uint8_t team) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) return NULL;
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 1);
    cs->lobbySlots[1].connected  = true;
    cs->lobbySlots[1].teamNumber = team;
    messageSetNewswire(clientSimGetMessages(cs), true);
    return cs;
}

/* Newswire lines land in the MessageState ring, which messageAddItem fills
 * one entry per visual column — so a line's arrival shows up as the queue
 * growing by its width, not by one. The assertions below therefore test
 * "grew" vs "held", which is the distinction that matters here. */
static int vpNewswireCount(ClientSim *cs) {
    return clientSimGetMessages(cs)->queueCount;
}

int run_vote_surrender_newswire_hidden_from_other_team(void) {
    ClientSim *cs = vpClientOnTeam(3);
    ControlEvent evt;
    int before;

    UT_ASSERT(cs != NULL);
    before = vpNewswireCount(cs);

    /* Team 4's vote starts, then fails. A team 3 player hears neither. */
    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, 4, GAME_VOTE_ACTIVE_RUNNING);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(vpNewswireCount(cs) == before,
                  "\"Surrender vote started\" leaked to a non-member "
                  "(newswire %d -> %d)", before, vpNewswireCount(cs));

    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, 4, GAME_VOTE_ACTIVE_FAILED);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(vpNewswireCount(cs) == before,
                  "\"Surrender vote failed\" leaked to a non-member "
                  "(newswire %d -> %d)", before, vpNewswireCount(cs));

    clientSimDestroy(cs);
    return 0;
}

int run_vote_surrender_newswire_shown_to_own_team(void) {
    ClientSim *cs = vpClientOnTeam(3);
    ControlEvent evt;
    int before, afterStart;

    UT_ASSERT(cs != NULL);
    before = vpNewswireCount(cs);

    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, 3, GAME_VOTE_ACTIVE_RUNNING);
    clientSimApplyControl(cs, &evt);
    afterStart = vpNewswireCount(cs);
    UT_ASSERT_MSG(afterStart > before,
                  "own team's \"Surrender vote started\" was not announced "
                  "(newswire %d -> %d)", before, afterStart);

    vpFillVoteState(&evt, GAME_VOTE_KIND_SURRENDER, 3, GAME_VOTE_ACTIVE_FAILED);
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(vpNewswireCount(cs) > afterStart,
                  "own team's \"Surrender vote failed\" was not announced "
                  "(newswire %d -> %d)", afterStart, vpNewswireCount(cs));

    clientSimDestroy(cs);
    return 0;
}

/* The trap again, one layer up: a team-scoped BACK_TO_LOBBY vote is
 * public and must still be announced to a player on another team. */
int run_vote_back_to_lobby_newswire_stays_public(void) {
    ClientSim *cs = vpClientOnTeam(3);
    ControlEvent evt;
    int before;

    UT_ASSERT(cs != NULL);
    before = vpNewswireCount(cs);

    vpFillVoteState(&evt, GAME_VOTE_KIND_BACK_TO_LOBBY, 4,
                    GAME_VOTE_ACTIVE_RUNNING);
    evt.u.gameVoteState.triggerSrc = GAME_VOTE_TRIGGER_BASE_MONOPOLY;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(vpNewswireCount(cs) > before,
                  "base-monopoly back-to-lobby vote (teamId=4) was wrongly "
                  "hidden from a team 3 player (newswire %d -> %d)",
                  before, vpNewswireCount(cs));

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * Layer 3 — the shared eligibility rule
 * ================================================================ */

int run_vote_surrender_may_answer_rule(void) {
    ClientSim *cs = vpClientOnTeam(3);
    UT_ASSERT(cs != NULL);

    UT_ASSERT_MSG(clientSimMayAnswerGameVote(cs, GAME_VOTE_KIND_SURRENDER, 3),
                  "a team 3 player must be able to answer team 3's surrender");
    UT_ASSERT_MSG(!clientSimMayAnswerGameVote(cs, GAME_VOTE_KIND_SURRENDER, 4),
                  "a team 3 player must not answer team 4's surrender");
    UT_ASSERT_MSG(clientSimMayAnswerGameVote(cs, GAME_VOTE_KIND_BACK_TO_LOBBY, 4),
                  "back-to-lobby is open to every player regardless of teamId");

    /* An Unassigned player is on no team, so no surrender vote is theirs —
     * including the teamId=0 idle snapshot (which is never a real vote:
     * serverSimGameVoteToggle rejects a team-0 caller). */
    cs->lobbySlots[1].teamNumber = 0;
    UT_ASSERT_MSG(!clientSimMayAnswerGameVote(cs, GAME_VOTE_KIND_SURRENDER, 0),
                  "an Unassigned player must not answer a team-0 surrender");

    /* A spectator has no slot and no team. myPlayerNum is a stale index for
     * a spectator rather than an empty one, so this needs its own guard. */
    cs->lobbySlots[1].teamNumber = 3;
    cs->isSpectator = true;
    UT_ASSERT_MSG(!clientSimMayAnswerGameVote(cs, GAME_VOTE_KIND_SURRENDER, 3),
                  "a spectator is never part of a surrender electorate");

    clientSimDestroy(cs);
    return 0;
}
