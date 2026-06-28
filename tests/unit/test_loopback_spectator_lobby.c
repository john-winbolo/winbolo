/*
 * End-to-end dual-mode spectator session — client side, over loopback.
 *
 * A spectator client alternates two views: the live read-only lobby while the
 * server is in lobby/countdown, and the delayed game once it starts. Which view
 * the session host shows is driven by one client bit — clientSimSpectatorIsLiveLobby
 * — seeded from the accept packet's mode byte and then flipped by which source is
 * feeding the connection. This test pins that bit's whole life cycle headlessly
 * (the modal view loop itself is not headless-testable):
 *
 *   connect-mode  — the accept byte. Connecting while the server is in the lobby
 *                   seeds live-lobby mode; connecting into a running game seeds
 *                   delayed mode. This is the wire contract the new accept byte
 *                   carries.
 *   game-start    — the lobby→game flip. At game start the server unsubscribes
 *                   the spectator from the live bus before the running phase is
 *                   published (so it never reaches netRunning) and begins the
 *                   delayed feed; the first delayed frame (cold-start countdown
 *                   or seed) leaves live-lobby mode. The host loop reads this as
 *                   "game started" instead of netRunning.
 *   return-flip   — the game→lobby flip. After the delayed game drains back to
 *                   the lobby the server re-subscribes the spectator, live lobby
 *                   control resumes, and the client re-enters live-lobby mode.
 *
 * The server-side cutover ordering (unsubscribe-before-running, drain-then-flip)
 * is proven separately in test_spectator_cutover.c; here the harness client is
 * the spectator, so the assertions are on the client bit tracking those events.
 *
 * The dual-mode loop seats a ready player directly in the sim to start the game
 * (the harness client is the tankless spectator and holds no slot) and runs at
 * -specdelay 0 so the delayed game drains quickly while still exercising the
 * real seek + drain-flip gate.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"             /* clientSimGetConnectState */
#include "client_connect_state.h"   /* CLIENT_CONNECT_SPECTATING */
#include "server_sim.h"             /* serverSim*, ServerState */
#include "server_sim_lifecycle.h"   /* serverSimSetReady / serverSimEnterGameOver */
#include "server_lifecycle.h"       /* serverInstanceCreateSpectatorRing */
#include "transport_udp.h"          /* transportUdpServerOnLobbyMapChange */
#include "threads.h"                /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

#define SPEC_CONNECT_MAX 800    /* spectator join handshake */
#define RUN_MAX          1000   /* lobby readied → countdown → running */
#define FEED_MAX         600    /* first delayed frame reaching the client */
#define FLIP_MAX         1200   /* gameover hold + delayed drain + return flip */
#define MAP_MAX          800    /* live-lobby map download over CHANNEL_BULK */

static bool pred_spectating(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_SPECTATING;
}

static bool pred_server_running(LoopbackHarness *h, void *u) {
    (void)u;
    return serverSimGetState(h->sim) == serverStateRunning;
}

static bool pred_client_delayed(LoopbackHarness *h, void *u) {
    (void)u;
    return !clientSimSpectatorIsLiveLobby(h->cs);
}

static bool pred_client_live(LoopbackHarness *h, void *u) {
    (void)u;
    return clientSimSpectatorIsLiveLobby(h->cs);
}

/* ── Leg 1: the accept mode byte seeds live when the server is in the lobby ── */

static int legConnectModeLobby(void) {
    LoopbackHarness h;
    int at;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "SpecLive", /*seed*/ 11u),
                  "spectator lobby harness start failed");
    at = loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }
    UT_ASSERT_MSG(clientSimSpectatorIsLiveLobby(h.cs),
                  "a lobby connect must seed live-lobby mode from the accept byte");
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 2: the accept mode byte seeds delayed when a game is running ──────── */

static int legConnectModeRunning(void) {
    LoopbackHarness h;
    int at;

    UT_ASSERT_MSG(loopbackHarnessStartSpectator(&h, "SpecDelay", /*seed*/ 12u),
                  "spectator running harness start failed");
    at = loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }
    UT_ASSERT_MSG(!clientSimSpectatorIsLiveLobby(h.cs),
                  "a running connect must seed delayed mode from the accept byte");
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 3: the bit follows the feed through lobby → game → lobby ──────────── */

static int legLobbyGameLobby(void) {
    LoopbackHarness h;
    int specAt, runAt, delayedAt, liveAt;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "SpecCycle", /*seed*/ 13u),
                  "spectator lobby harness start failed");

    /* -specdelay 0 reads the ring at its head, collapsing the ring time the
     * return flip needs while still exercising the real segSpec==segHead gate.
     * The lobby-start harness creates no ring (a lobby spectator is bus-fed, not
     * ring-served), so create it now — the game-start cutover serves the
     * spectator from it. */
    serverSimSetSpecDelayTicks(h.sim, 0);
    serverInstanceCreateSpectatorRing(h.sim);

    specAt = loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL);
    if (specAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }
    UT_ASSERT_MSG(clientSimSpectatorIsLiveLobby(h.cs),
                  "spectator must start the cycle in live-lobby mode");

    /* Seat a ready player directly in the sim so the lobby can start a game (the
     * harness client is the tankless spectator and holds no slot of its own). */
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, 0, "Player", /*wantRejoin*/ false);
    serverSimSetReady(h.sim, 0, true);
    serverSimLobbyCheckAllReady(h.sim);
    threadsReleaseMutex();

    runAt = loopbackHarnessPumpUntil(&h, RUN_MAX, pred_server_running, NULL);
    UT_ASSERT_MSG(runAt > 0,
                  "server never reached running after the lobby readied");

    /* lobby→game: the client observes the delayed feed beginning (cold-start
     * countdown or seed) and leaves live-lobby mode — the signal the host loop
     * reads as game start in place of netRunning, which a spectator never sees. */
    delayedAt = loopbackHarnessPumpUntil(&h, FEED_MAX, pred_client_delayed, NULL);
    UT_ASSERT_MSG(delayedAt > 0,
                  "spectator client never left live-lobby mode at game start");

    /* End the game. The state machine holds in gameover then returns to the
     * lobby; the spectator drains the delayed game across the game→lobby
     * boundary, the server re-subscribes it, and live lobby control resumes. */
    threadsWaitForMutex();
    serverSimEnterGameOver(h.sim);
    threadsReleaseMutex();

    /* game→lobby: live lobby control reaching the client flips it back into
     * live-lobby mode — the signal spectatorRun returns on. */
    liveAt = loopbackHarnessPumpUntil(&h, FLIP_MAX, pred_client_live, NULL);
    UT_ASSERT_MSG(liveAt > 0,
                  "spectator client never returned to live-lobby mode after the "
                  "delayed game drained back to the lobby");
    UT_ASSERT_MSG(serverSimGetState(h.sim) == serverStateLobby,
                  "live state must be lobby when the spectator returns to live");

    fprintf(stderr, "  spectator lobby cycle: spec@%d run@%d delayed@%d live@%d\n",
            specAt, runAt, delayedAt, liveAt);

    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 4: the live-lobby map is delivered + installed (preview unblocked) ── */

static bool pred_map_complete(LoopbackHarness *h, void *u) {
    int len = 0;
    (void)u;
    /* clientSimGetServerMapData returns the bytes only once the lobby map has
     * installed (mapInstalled), so this is the same gate the preview/badge use. */
    return clientSimIsMapDownloadComplete(h->cs) &&
           clientSimGetServerMapData(h->cs, &len) != NULL && len > 0;
}

static bool pred_map_data_gone(LoopbackHarness *h, void *u) {
    int len = 0;
    (void)u;
    return clientSimGetServerMapData(h->cs, &len) == NULL;
}

static int legLobbyMapDelivered(void) {
    LoopbackHarness h;
    int len = 0;
    const BYTE *map;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "SpecMap", /*seed*/ 21u),
                  "spectator lobby harness start failed");
    if (loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }

    /* The live-lobby map streams over the spectator's CHANNEL_BULK and installs
     * without leaving spectator mode, so the preview/starts can render and the
     * "Downloading" badge clears. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, MAP_MAX, pred_map_complete, NULL) > 0,
                  "live-lobby spectator never completed its lobby map download");
    map = clientSimGetServerMapData(h.cs, &len);
    UT_ASSERT_MSG(map != NULL && len > 0,
                  "lobby map bytes unavailable to a seated live-lobby spectator");
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "spectator must stay SPECTATING after installing the lobby map");
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 5: a mid-lobby map change re-downloads to the live spectator ──────── */

static int legLobbyMapChange(void) {
    LoopbackHarness h;
    int len = 0;
    const BYTE *map;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "SpecChg", /*seed*/ 22u),
                  "spectator lobby harness start failed");
    if (loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, MAP_MAX, pred_map_complete, NULL) > 0,
                  "lobby map never completed before the change");

    /* Host changes the map: the transport re-sends the spectator accept and
     * re-arms its lobby-map download (resetting the bulk channel via
     * CTRL_CHANNEL_RESET), so the viewer abandons the old map and re-downloads. */
    threadsWaitForMutex();
    transportUdpServerOnLobbyMapChange(h.sim);
    threadsReleaseMutex();

    /* The re-accept drops the install (the bytes go unavailable) until the new
     * map lands — proof the change actually triggered a fresh download — then it
     * re-completes. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, MAP_MAX, pred_map_data_gone, NULL) > 0,
                  "map change never invalidated the spectator's installed lobby map");
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, MAP_MAX, pred_map_complete, NULL) > 0,
                  "spectator never re-completed its lobby map after a map change");
    map = clientSimGetServerMapData(h.cs, &len);
    UT_ASSERT_MSG(map != NULL && len > 0,
                  "lobby map bytes unavailable after a map-change re-download");
    UT_ASSERT_MSG(clientSimGetConnectState(h.cs) == CLIENT_CONNECT_SPECTATING,
                  "spectator must stay SPECTATING across a lobby map change");
    loopbackHarnessStop(&h);
    return 0;
}

/* ── Leg 6: a cutover with a lobby map in flight still seeds the delayed game ── */

static int legCutoverPendingMap(void) {
    LoopbackHarness h;

    UT_ASSERT_MSG(loopbackHarnessStartSpectatorLobby(&h, "SpecCut", /*seed*/ 23u),
                  "spectator lobby harness start failed");
    /* -specdelay 0 so the seed (not a countdown) is the first delayed frame; the
     * lobby start creates no ring, so create it for the game-start cutover. */
    serverSimSetSpecDelayTicks(h.sim, 0);
    serverInstanceCreateSpectatorRing(h.sim);

    if (loopbackHarnessPumpUntil(&h, SPEC_CONNECT_MAX, pred_spectating, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("spectator never reached SPECTATING within %d pumps",
                SPEC_CONNECT_MAX);
    }

    /* Start the game promptly — the lobby map download may still be in flight, so
     * this drives the let-drain cutover: the delayed seed arms only after the
     * in-flight lobby map fully drains and acks (same bulk idle gate). Seat a
     * ready player to take the lobby into the countdown→running transition. */
    threadsWaitForMutex();
    serverSimAddPlayer(h.sim, 0, "Player", /*wantRejoin*/ false);
    serverSimSetReady(h.sim, 0, true);
    serverSimLobbyCheckAllReady(h.sim);
    threadsReleaseMutex();

    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, RUN_MAX, pred_server_running, NULL) > 0,
                  "server never reached running");
    /* The seed feed reaches the client despite the pending lobby map — it leaves
     * live-lobby mode. Proof the cutover's seed arms after the lobby map drained. */
    UT_ASSERT_MSG(loopbackHarnessPumpUntil(&h, FEED_MAX, pred_client_delayed, NULL) > 0,
                  "spectator never received the delayed feed after a cutover with a "
                  "lobby map in flight");
    loopbackHarnessStop(&h);
    return 0;
}

int run_loopback_spectator_lobby(void) {
    int rc;
    if ((rc = legConnectModeLobby())   != 0) return rc;
    if ((rc = legConnectModeRunning()) != 0) return rc;
    if ((rc = legLobbyGameLobby())     != 0) return rc;
    if ((rc = legLobbyMapDelivered())  != 0) return rc;
    if ((rc = legLobbyMapChange())     != 0) return rc;
    if ((rc = legCutoverPendingMap())  != 0) return rc;
    return 0;
}
