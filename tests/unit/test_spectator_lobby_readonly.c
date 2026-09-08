/*
 * Spectator lobby read-only invariant — the client send-suppression backstop.
 *
 * A tankless spectator renders the live lobby but must never originate a lobby
 * or gameplay command. The lobby dialog hides/disables every mutating control
 * for a viewer, but that is ImGui rendering and cannot be exercised headlessly;
 * its read-only appearance is human-validated. What IS code-testable is the
 * lower backstop: each clientSimNetSend* mutation helper early-returns when
 * clientSimIsSpectator(cs) is set, so a missed UI guard — or any non-UI caller
 * — still cannot put a viewer command on the wire.
 *
 * This test connects a real spectator over loopback against a lobby server,
 * seats a real player in slot 0, then fires EVERY guarded send path from the
 * viewer and asserts the server state is wholly untouched: no player added or
 * removed, the seated player's team/ready/name unchanged, the spectator never
 * promoted, the roster counts steady. clientSimNetSendChat is intentionally
 * NOT fired here — it is deliberately left unguarded for the spectator chat
 * channel; the dialog disables its input instead.
 *
 * A positive control at the end mutates slot 0 server-side and confirms the
 * spectator's mirror DOES follow it — proving the "unchanged" assertions above
 * are observing a live channel, not a frozen one.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"              /* clientSimNetSend* family */
#include "client_connect_state.h"
#include "input_packet.h"            /* InputPacket */
#include "server_sim.h"             /* serverSimAddPlayer / serverSimGetNumPlayers */
#include "server_sim_lifecycle.h"    /* serverSimSetTeam / serverSimSetReady */
#include "transport_udp.h"           /* transportUdpServerGetSpectatorCount */
#include "threads.h"                 /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

#define SPEC_CONNECT_MAX 800   /* spectator join handshake, no map download */
#define DRAIN_MAX        600   /* a published control event reaching the sim */
#define SETTLE_PUMPS      60   /* let any errantly emitted command land + apply */

#define SLOT0_NAME "Slot0Player"

static bool pred_spectating(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

static bool pred_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimIsInLobby(h->cs);
}

static bool pred_slot0_ready_team(LoopbackHarness *h, void *user) {
    (void)user;
    const ClientLobbySlot *s = clientSimGetLobbySlot(h->cs, 0);
    return s != NULL && s->connected && !s->ready && s->teamNumber == 1 &&
           strcmp(s->playerName, SLOT0_NAME) == 0;
}

static bool pred_slot0_now_ready(LoopbackHarness *h, void *user) {
    (void)user;
    const ClientLobbySlot *s = clientSimGetLobbySlot(h->cs, 0);
    return s != NULL && s->connected && s->ready;
}

/* Fire every guarded mutation entry point once, from the viewer's ClientSim.
 * Each must early-return at the clientSimIsSpectator guard, so none reaches the
 * wire and the server state below stays put. Args are plausible-but-irrelevant
 * (the guard returns before they are read). */
static void fireAllSpectatorCommands(ClientSim *cs) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    static const uint8_t blob[4] = { 1, 2, 3, 4 };
    char md5[32];
    memset(md5, '0', sizeof(md5));

    clientSimNetSendInput(cs, &pkt);
    clientSimNetSendNameChange(cs, "NewName");
    clientSimNetSendAllianceRequest(cs, 0);
    clientSimNetSendAllianceAccept(cs, 0);
    clientSimNetSendAllianceLeave(cs);
    clientSimNetSendLockToggle(cs, false);
    clientSimNetSendTeamSet(cs, 0, 2);
    clientSimNetSendLobbyClaimStart(cs, 0, 1);
    clientSimNetSendReady(cs, true);
    clientSimNetSendAddBot(cs);
    clientSimNetSendAddBotConfigured(cs, 1, 0xFF, "Bot");
    clientSimNetSendRemoveBot(cs, 0);
    clientSimNetSendLobbyBotConfig(cs, 0, 0, 0, "Bot");
    clientSimNetSendLobbySetBotBrain(cs, 0, 0);
    clientSimNetSendLobbySetMap(cs, "maps/foo.map");
    clientSimNetSendLobbyPreviewCancel(cs);
    clientSimNetSendLobbyPreviewCommit(cs);
    clientSimNetSendLobbyPreviewRandom(cs, "seed");
    (void)clientSimNetSendLobbyMapUpload(cs, "nonexistent.map");
    (void)clientSimNetSendLobbyMapUploadBytes(cs, blob, sizeof(blob), "foo");
    clientSimNetSendLobbyMapUseLocal(cs, sizeof(blob), "foo", "maps/foo.map", md5);
    clientSimNetSendLobbyTeamMeta(cs, 1, 0, 0, 0, "Team");
    clientSimNetSendLobbyTeamClear(cs, 3);
    {
        uint8_t v = 1;
        clientSimNetSendLobbySetting(cs, 0, &v, 1);
    }
    clientSimNetSendLobbySetPassword(cs, "pw");
    clientSimNetSendLobbyOpenHost(cs, true);
    clientSimNetSendLobbyKick(cs, 0);
    clientSimNetSendLobbyTransferHost(cs, 0);
    clientSimNetSendMapSkipVote(cs);
    clientSimNetSendGameVoteToggle(cs, 0, 0);
    clientSimNetSendBalanceRequest(cs, 2, true);
    clientSimNetSendBalanceApply(cs);
    clientSimNetSendBalanceDismiss(cs);
}

int run_spectator_lobby_readonly(void) {
    LoopbackHarness h;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "Spectator", /*seed*/ 7u),
                  "spectator lobby harness start failed");

    /* Reach the tankless spectating + lobby phase. */
    if (loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_in_lobby, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never entered the lobby phase within %d pumps",
                DRAIN_MAX);
    }
    UT_ASSERT_MSG(clientSimIsSpectator(h.cs),
                  "clientSimIsSpectator must report true for a spectator sim");

    /* Baseline: one tankless viewer, no players. */
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "setup: server must count exactly one spectator");
    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == 0,
                  "setup: spectator must consume no tank slot");

    /* Seat a real player in slot 0 (team 1, not ready) so commands targeting a
     * live slot have something to (fail to) move. */
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, 0, SLOT0_NAME, /*wantRejoin*/ false);
    serverSimSetTeam(h.sim, 0, 1);
    serverSimSetReady(h.sim, 0, false);
    serverSimPublishLobbySlot(h.sim, 0);
    threadsReleaseMutex();

    if (loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_slot0_ready_team, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot-0 player state never resolved on the spectator mirror");
    }

    /* Snapshot the state every command below would, if it leaked, disturb. */
    BYTE basePlayers = serverSimGetNumPlayers(h.sim);
    int  baseSpecs   = transportUdpServerGetSpectatorCount();

    /* Fire every guarded send from the viewer, then let anything errant land. */
    fireAllSpectatorCommands(h.cs);
    (void)loopbackHarnessPumpUntil(&h, SETTLE_PUMPS, NULL, NULL);

    /* === Invariant: nothing the viewer fired had any effect === */
    UT_ASSERT_MSG(serverSimGetNumPlayers(h.sim) == basePlayers,
                  "viewer commands must not add/remove a player (was %d, now %d)",
                  (int)basePlayers, (int)serverSimGetNumPlayers(h.sim));
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == baseSpecs,
                  "viewer commands must not change the spectator roster");
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "viewer must stay SPECTATING, never promote to a player state");
    UT_ASSERT_MSG(clientSimIsInLobby(h.cs),
                  "viewer must stay in the lobby phase");

    const ClientLobbySlot *s0 = clientSimGetLobbySlot(h.cs, 0);
    UT_ASSERT_MSG(s0 != NULL && s0->connected,
                  "slot 0 must still be connected after the command burst");
    UT_ASSERT_MSG(strcmp(s0->playerName, SLOT0_NAME) == 0,
                  "slot-0 name must be unchanged (not kicked), got '%s'",
                  s0->playerName);
    UT_ASSERT_MSG(s0->teamNumber == 1,
                  "slot-0 team must be unchanged (no team-set leaked), got %d",
                  (int)s0->teamNumber);
    UT_ASSERT_MSG(!s0->ready,
                  "slot-0 ready must be unchanged (no ready leaked)");
    UT_ASSERT_MSG(!clientSimIsMapSkipMyVote(h.cs),
                  "viewer must hold no map-skip vote of its own");

    /* Positive control: a real server-side change to slot 0 DOES reach the
     * mirror — so the unchanged assertions above were watching a live channel,
     * not a stuck one. */
    threadsWaitForMutex();
    serverSimSetReady(h.sim, 0, true);
    serverSimPublishLobbySlot(h.sim, 0);
    threadsReleaseMutex();
    if (loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_slot0_now_ready, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("positive control failed: a server-side slot-0 change never "
                "reached the spectator mirror (the invariant test would be vacuous)");
    }

    loopbackHarnessStop(&h);
    return 0;
}
