/*
 * Tankless spectator live-lobby control intake — client side.
 *
 * A spectator's ClientSim claims no tank, so its myPlayerNum stays at the
 * create-time 0 — an in-bounds value that ALIASES real player slot 0. The
 * server feeds a lobby/countdown spectator the allowlisted lobby control bus
 * over CHANNEL_CONTROL, and the client applies every event through
 * clientSimApplyControl. Before the cs->isSpectator guards, each branch keyed
 * on "is this my slot / from me" mis-fired for slot 0: the slot-0 player's name
 * was swallowed by playersSetPlayer's self-branch, its broadcast chat was
 * dropped as a self-echo, its map-skip vote was mirrored as the viewer's own,
 * and a player joining slot-0's (aliased) team produced a team-scoped chat line
 * a viewer must never see.
 *
 * This test drives a real spectator client over loopback against a lobby-mode
 * server, publishes the allowlisted lobby control from the in-process server,
 * and asserts the spectator's ClientSim populates correctly with no slot-0
 * mis-attribution. The two load-bearing regression assertions:
 *   - clientSimGetLobbySlot(cs, 0)->playerName resolves to the real name
 *     (never empty / placeholder) — proves the intake no longer treats slot 0
 *     as the viewer's own non-refreshing self.
 *   - the slot-0 broadcast chat reaches lobbyChatHistory — proves the self-echo
 *     drop no longer eats a real player's broadcast.
 *
 * Note: myPlayerNum stays 0 (in-bounds), so none of these are out-of-bounds —
 * the liveness assertion (every driven event applies and the run completes) is
 * a behavioural check, not a sanitizer trap.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"    /* serverSimSetTeam / serverSimSetReady */
#include "transport_udp.h"           /* MAX_SPECTATORS */
#include "threads.h"                 /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

#define SPEC_CONNECT_MAX 800   /* spectator join handshake, no map download */
#define DRAIN_MAX        600   /* a published control event reaching the sim */

#define SLOT0_NAME   "Slot0Player"
#define SLOT1_NAME   "Slot1Player"
#define BROADCAST_TX "spectator sees this broadcast"

static bool pred_spectating(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

static bool pred_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimIsInLobby(h->cs);
}

/* Slot `idx` is connected on the spectator's mirror and carries `name`. */
static bool slot_named(LoopbackHarness *h, BYTE idx, const char *name) {
    const ClientLobbySlot *s = clientSimGetLobbySlot(h->cs, idx);
    return s != NULL && s->connected && strcmp(s->playerName, name) == 0;
}

static bool pred_slot0_named(LoopbackHarness *h, void *user) {
    return slot_named(h, 0, (const char *)user);
}

static bool pred_slot1_named(LoopbackHarness *h, void *user) {
    return slot_named(h, 1, (const char *)user);
}

static bool pred_chat_contains(LoopbackHarness *h, void *user) {
    const char *hist = clientSimGetLobbyChatHistory(h->cs);
    return hist != NULL && strstr(hist, (const char *)user) != NULL;
}

/* The map-skip state for slot 0 arrived (votes[] is filled unconditionally),
 * which is what makes the "my vote stays false" assertion meaningful. */
static bool pred_slot0_skipvote(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimIsMapSkipVote(h->cs, 0);
}

/* The spectator must see itself on the spectator roster (CTRL_SPECTATOR_SLOT
 * from the registration replay). */
static bool spectator_self_present(LoopbackHarness *h, const char *name) {
    uint8_t i;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        const ClientSpectatorSlot *s = clientSimGetSpectatorSlot(h->cs, i);
        if (s != NULL && s->connected && strcmp(s->playerName, name) == 0) {
            return true;
        }
    }
    return false;
}

int run_spectator_client_lobby_intake(void) {
    LoopbackHarness h;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "Spectator", /*seed*/ 9u),
                  "spectator lobby harness start failed");

    /* 1. Reach the tankless spectating state. */
    int specAt = loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX,
                                          pred_spectating, NULL);
    if (specAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }

    /* 2. The registration sync replay flips the viewer into the lobby phase.
     *    Chat only routes to lobbyChatHistory once inLobby is set, so assert
     *    the phase landed BEFORE any chat is driven rather than relying on it. */
    int lobbyAt = loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_in_lobby, NULL);
    if (lobbyAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never entered the lobby phase within %d pumps",
                DRAIN_MAX);
    }
    UT_ASSERT_MSG(clientSimIsInLobby(h.cs),
                  "spectator must be in lobby before chat is driven");
    UT_ASSERT_MSG(clientSimIsSpectator(h.cs),
                  "clientSimIsSpectator must report true for a spectator sim");
    UT_ASSERT_MSG(clientSimGetMyPlayerNum(h.cs) == 0,
                  "spectator myPlayerNum must stay 0 (in-bounds slot-0 alias)");

    /* 3. Put a real player in slot 0 and publish its lobby row. CTRL_PLAYER_JOIN
     *    (from AddPlayer) and CTRL_LOBBY_SLOT (from the publish) both reach the
     *    live subscriber. */
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, 0, SLOT0_NAME, /*wantRejoin*/ false);
    serverSimSetTeam(h.sim, 0, 1);
    serverSimPublishLobbySlot(h.sim, 0);
    threadsReleaseMutex();

    int s0At = loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_slot0_named,
                                        (void *)SLOT0_NAME);
    if (s0At < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot-0 player name never resolved on the spectator mirror");
    }

    /* 4. A second player joins slot-0's (aliased) team. Pre-fix, the viewer's
     *    myTeam aliased slot 0, so this produced a spurious team-scoped chat
     *    line; the team-chat-empty assertion below is the regression. This
     *    unready second player also keeps the lobby from ever satisfying
     *    all-ready when slot 0 readies below. */
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, 1, SLOT1_NAME, /*wantRejoin*/ false);
    serverSimSetTeam(h.sim, 1, 1);
    serverSimPublishLobbySlot(h.sim, 1);
    threadsReleaseMutex();

    int s1At = loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_slot1_named,
                                        (void *)SLOT1_NAME);
    if (s1At < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot-1 player name never resolved on the spectator mirror");
    }

    /* 5. Toggle slot-0 ready and republish — exercises the ready-cue branch for
     *    the aliased slot. Slot 1 stays unready, so the lobby never auto-starts
     *    (and we never call the all-ready check ourselves). */
    threadsWaitForMutex();
    serverSimSetReady(h.sim, 0, true);
    serverSimPublishLobbySlot(h.sim, 0);
    threadsReleaseMutex();
    /* A short settle for the ready republish; control is reliable+ordered, so
     * later publishes still arrive after it even if it is mid-flight here. */
    (void)loopbackHarnessPumpUntil(&h, 20, NULL, NULL);

    /* 6. Broadcast chat FROM slot 0 — pre-fix this was dropped as a self-echo. */
    threadsWaitForMutex();
    serverSimReceiveChat(h.sim, 0, 0xFF, BROADCAST_TX, strlen(BROADCAST_TX));
    threadsReleaseMutex();

    int chatAt = loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_chat_contains,
                                          (void *)BROADCAST_TX);
    if (chatAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot-0 broadcast chat never reached the spectator's "
                "lobby chat history");
    }

    /* 7. Slot-0 map-skip vote — votes[] fills, but the viewer's own vote must
     *    not mirror it. The toggle is gated on a rotation of >1 map, so install
     *    a two-entry rotation list first (malloc'd: the sim takes ownership and
     *    frees each entry with free() on teardown). A lone slot-0 yes vote is
     *    short of the 2-human majority, so no map is actually skipped. */
    {
        char **rotation = (char **)malloc(2 * sizeof(char *));
        if (rotation == NULL) {
            loopbackHarnessStop(&h);
            UT_FAIL("rotation list allocation failed");
        }
        rotation[0] = (char *)malloc(16);
        rotation[1] = (char *)malloc(16);
        if (rotation[0] == NULL || rotation[1] == NULL) {
            loopbackHarnessStop(&h);
            UT_FAIL("rotation entry allocation failed");
        }
        strcpy(rotation[0], "alpha.map");
        strcpy(rotation[1], "bravo.map");
        threadsWaitForMutex();
        serverSimInstallMapDirList(h.sim, rotation, 2, NULL);
        serverSimMapSkipVoteToggle(h.sim, 0);
        threadsReleaseMutex();
    }

    int skipAt = loopbackHarnessPumpUntil(&h, DRAIN_MAX, pred_slot0_skipvote,
                                          NULL);
    if (skipAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot-0 map-skip state never reached the spectator");
    }

    /* === Assertions === */

    /* Regression: the real slot-0 name resolved (never empty / placeholder). */
    const ClientLobbySlot *s0 = clientSimGetLobbySlot(h.cs, 0);
    UT_ASSERT_MSG(s0 != NULL && s0->connected,
                  "slot 0 must be connected on the spectator mirror");
    UT_ASSERT_MSG(strcmp(s0->playerName, SLOT0_NAME) == 0,
                  "slot-0 name must resolve to the real player name, got '%s'",
                  s0->playerName);

    /* Spectator sees itself on the roster. */
    UT_ASSERT_MSG(spectator_self_present(&h, "Spectator"),
                  "spectator must appear on its own spectator roster");

    /* Regression: slot-0 broadcast chat is in the lobby history. */
    UT_ASSERT_MSG(pred_chat_contains(&h, (void *)BROADCAST_TX),
                  "slot-0 broadcast chat must be in the spectator's lobby chat");

    /* Map-skip arrived for slot 0, but the viewer holds no vote of its own. */
    UT_ASSERT_MSG(clientSimIsMapSkipVote(h.cs, 0),
                  "slot-0 map-skip vote must be recorded on the spectator");
    UT_ASSERT_MSG(!clientSimIsMapSkipMyVote(h.cs),
                  "spectator must not mirror slot-0's vote as its own");

    /* No team-scoped chatter attributed to the teamless viewer. */
    const char *teamChat = clientSimGetLobbyTeamChatHistory(h.cs);
    UT_ASSERT_MSG(teamChat == NULL || teamChat[0] == '\0',
                  "spectator must see no team-scoped lobby chat, got '%s'",
                  teamChat ? teamChat : "");

    /* Liveness: still a tankless spectator in the lobby after the whole run. */
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "spectator must still rest in SPECTATING");
    UT_ASSERT_MSG(clientSimIsInLobby(h.cs),
                  "spectator must still be in the lobby phase");

    fprintf(stderr, "  spectator lobby intake: spec@%d lobby@%d s0@%d s1@%d "
                    "chat@%d skip@%d\n", specAt, lobbyAt, s0At, s1At,
                    chatAt, skipAt);

    loopbackHarnessStop(&h);
    return 0;
}
