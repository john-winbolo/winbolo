/*
 * In-process loopback transport harness — see loopback_harness.h for the
 * API contract, the impairment / determinism notes, and the lifecycle.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "platform_net.h"
#include "bolo_rand.h"
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_lifecycle.h"
#include "server_lifecycle.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "threads.h"
#include "transport_udp.h"  /* transportUdpServerRecvQueuePending, GetBoundPort */

#include "loopback_harness.h"

/* Longest a pump waits for the server's recv thread to deliver before it
 * ticks the server anyway. Only reached on a pump that carried nothing,
 * since the wait ends as soon as a datagram is queued. Generous enough to
 * cover a scheduling delay on a machine running the whole suite. */
#define LOOPBACK_DELIVER_MAX_MS 5

/* E_MAP compressed length — the same literal ut_make_running_sim passes to
 * serverSimCreateCompressed. */
#define LOOPBACK_EMAP_LEN 5097

/* Virtual impairment clock. The transport hands net_impair.c whatever clock
 * transportUdpClientSetVirtualClock installed, and the module is pure — it
 * takes nowMs as a parameter — so a counter the pump advances a fixed amount
 * per tick turns a delay= spec into an exact pump count. Off unless a test
 * asks for it: the bundled impaired tests keep the wall clock and the timing
 * they were written against. */

/* Game time in one pump. A pump runs serverInstanceTick once, which advances
 * the sim by one 20ms frame — serverSimTick runs both half-steps inside it —
 * so the step is the frame, not a half-step. Pace it off a half-step and the
 * clock runs at half the rate of the sim it is pacing, which costs a delay=
 * spec twice the game time it names. */
#define LOOPBACK_VIRTUAL_STEP_MS 20

#if WB_ENABLE_NETIMPAIR
static bool     loopbackVirtualClockOn = false;
static uint64_t loopbackVirtualNowMs   = 0;

static uint64_t loopbackVirtualNow(void) {
    return loopbackVirtualNowMs;
}
#endif

/* One pump's worth of virtual time. A no-op while the virtual clock is off,
 * which is what keeps loopbackHarnessPump's behaviour unchanged for every
 * test that does not ask for it. */
static void loopbackAdvanceVirtualClock(void) {
#if WB_ENABLE_NETIMPAIR
    if (loopbackVirtualClockOn) {
        loopbackVirtualNowMs += LOOPBACK_VIRTUAL_STEP_MS;
    }
#endif
}

void loopbackHarnessUseVirtualClock(LoopbackHarness *h, bool on) {
    (void)h;
#if WB_ENABLE_NETIMPAIR
    loopbackVirtualClockOn = on;
    if (on) {
        /* From zero, so a spec's delay is counted in pumps from the call
         * rather than from whenever the process started. Call it before the
         * first pump: anything already sitting in an impairment queue was
         * stamped off the wall clock and would not come due for hours of
         * virtual time. */
        loopbackVirtualNowMs = 0;
        transportUdpClientSetVirtualClock(loopbackVirtualNow);
    } else {
        transportUdpClientSetVirtualClock(NULL);
    }
#else
    (void)on;
#endif
}

/* Set (or clear) the WB_NETIMPAIR override the client reads at connect
 * time. Empty/NULL clears it so a clean-path harness sees no impairment. */
static void loopbackSetImpairEnv(const char *spec) {
#ifdef _WIN32
    _putenv_s("WB_NETIMPAIR", (spec != NULL && spec[0] != '\0') ? spec : "");
#else
    if (spec != NULL && spec[0] != '\0') {
        setenv("WB_NETIMPAIR", spec, 1);
    } else {
        unsetenv("WB_NETIMPAIR");
    }
#endif
}

/* Shared server bring-up: threads, an ephemeral localhost port, an
 * Everard-Island ServerSim (running unless lobbyMode), and the UDP server
 * instance accepting remote clients. Sets h->threadsUp / h->port / h->sim /
 * h->serverUp. The caller has already zeroed *h. Returns false on any failure
 * (the harness stays safe to Stop).
 *
 * Every step that fails, and the port on the way out, goes to stderr. CTest
 * keeps a case's output and shows it only when the case fails, so a green run
 * reads the same as before and a failed one says which port it was on. */
static bool loopbackBringUpServer(LoopbackHarness *h, bool lobbyMode) {
    BYTE emap[6000] = E_MAP;
    ServerInstanceConfig cfg;

    bolo_net_init();

    if (!threadsCreate(TRUE)) {
        fprintf(stderr, "  loopback bring-up failed: threadsCreate\n");
        return false;
    }
    h->threadsUp = true;

    h->sim = serverSimCreateCompressed(emap, LOOPBACK_EMAP_LEN,
                                       "Everard Island", gameOpen,
                                       false, 0, -1);
    if (h->sim == NULL) {
        fprintf(stderr, "  loopback bring-up failed: serverSimCreateCompressed\n");
        return false;
    }
    /* The wire joiner claims its own slot — keep the lobby open. */
    serverSimSetAllowNewPlayers(h->sim, true);

    memset(&cfg, 0, sizeof(cfg));
    cfg.bindAddr            = "127.0.0.1";
    cfg.password            = "";
    cfg.maxPlayers          = MAX_TANKS;
    cfg.acceptRemoteClients = true;
    cfg.useWbn              = false;
    cfg.compTanks           = aiNone;
    cfg.useTracker          = false;
    cfg.trackerAddr         = "";
    cfg.trackerPort         = 0;
    cfg.useNatKeepalive     = false;
    cfg.useNatPortmap       = false;
    if (lobbyMode) {
        cfg.lobbyEnabled = true;
    } else {
        cfg.skipLobby = true;   /* enter running immediately */
    }
    /* Ask the OS for a port rather than naming one. The harness used to bind
     * a throwaway socket to 127.0.0.1:0, read the kernel's choice back and
     * hand that number to the server, which left a close→rebind window in
     * which a parallel ctest run could be given the same port — so the bind
     * failed and the bring-up retried on a fresh pick. Requesting 0 hands the
     * port straight from the kernel to the socket the server keeps, with no
     * window for anyone to take it in, and transportUdpServerGetBoundPort
     * reads back (via getsockname) what it got. The race the retry loop
     * existed for cannot happen, so neither exists any more.
     *
     * The read-back is only meaningful because cfg.acceptRemoteClients is
     * true above: that is what makes serverInstanceStartup create the UDP
     * server socket in the first place. */
    cfg.udpPort = 0;
    if (!serverInstanceStartup(h->sim, &cfg)) {
        fprintf(stderr, "  loopback bring-up failed: serverInstanceStartup\n");
        return false;
    }
    h->serverUp = true;
    h->port     = transportUdpServerGetBoundPort();
    fprintf(stderr, "  loopback server: port=%u mode=%s\n",
            (unsigned)h->port, lobbyMode ? "lobby" : "running");
    return true;
}

/* Connect a fresh ClientSim over UDP to the harness server. spectator selects a
 * tankless spectator connect (no tank slot, no map download) over a normal
 * player join. impairSpec is applied to the client endpoint before connect.
 * Sets h->cs / h->clientUp. */
static bool loopbackConnectClientInto(LoopbackHarness *h, const char *playerName,
                                      const char *impairSpec, bool spectator,
                                      struct ClientSim **outCs, bool *outUp) {
    /* Set before connect — transportUdpClientCreate reads WB_NETIMPAIR
     * once at construction. */
    loopbackSetImpairEnv(impairSpec);

    *outCs = clientSimAlloc();
    if (*outCs == NULL) {
        return false;
    }
    clientSimCreate(*outCs);
    if (!clientSimConnectUdp(*outCs, "127.0.0.1", h->port, playerName,
                             /*fallbackCountry*/ "", /*password*/ "",
                             /*wbnApiToken*/ NULL, /*wbnServerKey*/ NULL,
                             /*wantRejoin*/ false, /*trackerAddr*/ "",
                             /*trackerPort*/ 0, spectator)) {
        return false;
    }
    *outUp = true;
    return true;
}

static bool loopbackConnectClient(LoopbackHarness *h, const char *playerName,
                                  const char *impairSpec, bool spectator) {
    return loopbackConnectClientInto(h, playerName, impairSpec, spectator,
                                     &h->cs, &h->clientUp);
}

bool loopbackHarnessAddClient(LoopbackHarness *h, const char *playerName) {
    if (h == NULL || !h->serverUp || h->client2Up) {
        return false;
    }
    /* Clean path only, and the env is cleared for it: the first client read
     * WB_NETIMPAIR at its own construction and keeps whatever it was given,
     * so a second client never inherits the first one's impairment. */
    return loopbackConnectClientInto(h, playerName, /*impairSpec*/ NULL,
                                     /*spectator*/ false, &h->cs2,
                                     &h->client2Up);
}

bool loopbackHarnessStart(LoopbackHarness *h, const char *playerName,
                          bool lobbyMode, const char *impairSpec,
                          uint64_t seed) {
    if (h == NULL) return false;
    memset(h, 0, sizeof(*h));

    if (!loopbackBringUpServer(h, lobbyMode)) {
        return false;
    }
    if (!loopbackConnectClient(h, playerName, impairSpec, /*spectator*/ false)) {
        return false;
    }

    /* Seed last so the pump phase's impairment + sim draws are reproducible
     * regardless of any randomness consumed during setup above. */
    bolo_srand(seed);
    return true;
}

bool loopbackHarnessStartSpectator(LoopbackHarness *h, const char *playerName,
                                   uint64_t seed) {
    if (h == NULL) return false;
    memset(h, 0, sizeof(*h));

    /* A running game with no players is still watchable — the spectator's seed
     * comes off the server's spectator ring, not a peer. */
    if (!loopbackBringUpServer(h, /*lobbyMode*/ false)) {
        return false;
    }
    /* Spectating is operator-gated and off by default; open viewer slots so the
     * server's spectator-accept path admits the connect. */
    serverSimSetMaxSpectators(h->sim, 4);

    /* serverInstanceStartup creates the spectator ring only when maxSpectators
     * was already > 0; here the slots are opened just above (after startup), so
     * create the ring now — it is the production registration call and the seed
     * a connecting spectator receives comes off this ring. Without it the running
     * world records nothing and the spectator hangs awaiting a seed.
     * serverInstanceShutdown (via loopbackHarnessStop) frees it. */
    serverInstanceCreateSpectatorRing(h->sim);

    if (!loopbackConnectClient(h, playerName, /*impairSpec*/ NULL,
                               /*spectator*/ true)) {
        return false;
    }

    bolo_srand(seed);
    return true;
}

bool loopbackHarnessStartSpectatorLobby(LoopbackHarness *h,
                                        const char *playerName, uint64_t seed) {
    if (h == NULL) return false;
    memset(h, 0, sizeof(*h));

    /* Lobby (not running) server: a spectator that connects while the server
     * is in lobby/countdown is registered as a live control-bus subscriber and
     * fed allowlisted lobby control directly — no spectator ring is consulted
     * on this path, so (unlike loopbackHarnessStartSpectator) none is created. */
    if (!loopbackBringUpServer(h, /*lobbyMode*/ true)) {
        return false;
    }
    /* Spectating is operator-gated and off by default; open viewer slots so the
     * server's spectator-accept path admits the connect. */
    serverSimSetMaxSpectators(h->sim, 4);

    if (!loopbackConnectClient(h, playerName, /*impairSpec*/ NULL,
                               /*spectator*/ true)) {
        return false;
    }

    bolo_srand(seed);
    return true;
}

/* True while a client is still working through the join handshake. That
 * phase gives up after JOIN_RETRY_INTERVAL * JOIN_MAX_RETRIES client ticks,
 * so a datagram the recv thread delivers a pump late costs part of a budget
 * nothing else in the harness measures in ticks. Everything after it is
 * driven by wall clock and does not care. */
static bool loopbackPumpHandshaking(LoopbackHarness *h) {
    ClientConnectState st;
    if (h->clientUp) {
        st = clientSimGetConnectState(h->cs);
        if (st == CLIENT_CONNECT_JOINING ||
            st == CLIENT_CONNECT_DOWNLOADING_MAP) return true;
    }
    if (h->client2Up) {
        st = clientSimGetConnectState(h->cs2);
        if (st == CLIENT_CONNECT_JOINING ||
            st == CLIENT_CONNECT_DOWNLOADING_MAP) return true;
    }
    return false;
}

void loopbackHarnessPump(LoopbackHarness *h) {
    if (h == NULL) return;
    loopbackAdvanceVirtualClock();
    if (h->clientUp) clientSimNetTick(h->cs);
    /* Second client (when one was added) ticks in the same phase as the
     * first, before the server: both endpoints' datagrams are then drained
     * by the one server tick below, keeping the pump a single step. */
    if (h->client2Up) clientSimNetTick(h->cs2);
    /* The server's datagrams are ingested by its background recv thread into an
     * SPSC queue that serverInstanceTick drains. In a tight pump loop that
     * thread can lag a pump, so the just-sent client datagram isn't drained
     * until a later tick — and that scheduling slack shifts ack/retransmit
     * timing relative to the seeded impairment, which made connect/lobby
     * convergence under loss nondeterministic.
     *
     * Ask the queue whether the thread has delivered rather than sleeping a
     * fixed interval and assuming it has. A flat 1ms was long enough on an
     * idle machine and not on a busy one, so a full-suite run could slip a
     * datagram by enough pumps to burn the client's whole join budget
     * (JOIN_RETRY_INTERVAL * JOIN_MAX_RETRIES ticks) while the same test
     * passed on its own. Returns the moment something is queued.
     *
     * Only while a client is still handshaking. That is where the budget is
     * tight and where all but a handful of pumps carry a datagram, so the
     * wait ends almost at once. Once connected the flat yield stands: a
     * steady-state pump is usually carrying nothing, so waiting the cap on
     * every one of them buys nothing and slows the quiet scenario tests
     * enough to move the timers they are measuring. */
    if (h->serverUp && loopbackPumpHandshaking(h)) {
        int waited = 0;
        while (waited < LOOPBACK_DELIVER_MAX_MS &&
               transportUdpServerRecvQueuePending() == 0) {
            SDL_Delay(1);
            waited++;
        }
    } else {
        SDL_Delay(1);
    }
    if (h->serverUp) serverInstanceTick(h->sim);
}

void loopbackHarnessPumpClientOnly(LoopbackHarness *h, int n) {
    int i;
    if (h == NULL || n <= 0) return;
    for (i = 0; i < n; i++) {
        loopbackAdvanceVirtualClock();
        if (h->clientUp) clientSimNetTick(h->cs);
        if (h->client2Up) clientSimNetTick(h->cs2);
        /* No serverInstanceTick, and no SDL_Delay either: the yield in
         * loopbackHarnessPump exists to give the server's recv thread a
         * chance to deliver before the server ticks, and nothing here ticks
         * the server. Datagrams the client sends still reach the server's
         * socket and its recv thread still queues them; they sit in that
         * queue until something drains it. */
    }
}

int loopbackHarnessPumpUntil(LoopbackHarness *h, int maxIters,
                             LoopbackPredicate pred, void *user) {
    int i;
    if (h == NULL || maxIters <= 0) return -1;
    for (i = 1; i <= maxIters; i++) {
        loopbackHarnessPump(h);
        if (pred != NULL && pred(h, user)) {
            return i;
        }
    }
    return (pred == NULL) ? maxIters : -1;
}

bool loopbackHarnessTriggerGameStart(LoopbackHarness *h) {
    BYTE slot;
    if (h == NULL || !h->clientUp || !h->serverUp) return false;
    slot = clientSimGetMyPlayerNum(h->cs);
    if (slot >= MAX_TANKS) return false;
    threadsWaitForMutex();
    serverSimSetReady(h->sim, slot, true);
    serverSimLobbyCheckAllReady(h->sim);
    threadsReleaseMutex();
    return true;
}

int loopbackRecvFromServer(SOCKET s, uint8_t *buf, int cap,
                           const struct sockaddr_in *server) {
    for (;;) {
        struct sockaddr_in from;
        socklen_t fromLen = (socklen_t)sizeof(from);
        int n;

        memset(&from, 0, sizeof(from));
        n = (int)recvfrom(s, (char *)buf, cap, 0,
                          (struct sockaddr *)&from, &fromLen);
        if (n <= 0) {
            return n;   /* nothing waiting, or the socket gave up */
        }
        if (from.sin_addr.s_addr == server->sin_addr.s_addr &&
            from.sin_port == server->sin_port) {
            return n;
        }
        /* Somebody else's datagram. It has been read out of the socket
         * buffer, and going round again is what makes that a drop rather
         * than a short return that would strand the rest of the queue. */
    }
}

void loopbackHarnessStop(LoopbackHarness *h) {
    if (h == NULL) return;
    /* Second client first: it leaves while the server is still up, so its
     * slot goes through the same disconnect path a real leave takes. */
    if (h->cs2 != NULL) {
        if (h->client2Up) {
            clientSimDisconnect(h->cs2);
        }
        clientSimDestroy(h->cs2);
        h->cs2 = NULL;
        h->client2Up = false;
    }
    if (h->cs != NULL) {
        if (h->clientUp) {
            clientSimDisconnect(h->cs);
        }
        clientSimDestroy(h->cs);
        h->cs = NULL;
    }
    if (h->sim != NULL) {
        if (h->serverUp) {
            serverInstanceShutdown(h->sim);
        }
        serverSimDestroy(h->sim);
        h->sim = NULL;
    }
    if (h->threadsUp) {
        threadsDestroy();
        h->threadsUp = false;
    }
    loopbackSetImpairEnv(NULL);
    /* Process-wide state, so it outlives the harness unless it is cleared
     * here: the next test in this binary would otherwise inherit a counter
     * this one stopped advancing, and its impaired packets would never come
     * due. */
    loopbackHarnessUseVirtualClock(h, false);
    h->clientUp = false;
    h->serverUp = false;
}
