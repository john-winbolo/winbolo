/*
 * In-process loopback transport harness — see loopback_harness.h for the
 * API contract, the impairment / determinism notes, and the lifecycle.
 */

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

#include "loopback_harness.h"

/* E_MAP compressed length — the same literal ut_make_running_sim passes to
 * serverSimCreateCompressed. */
#define LOOPBACK_EMAP_LEN 5097

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

/* Bind a throwaway UDP socket to 127.0.0.1:0 and read back the kernel-
 * assigned port. The server rebinds it (with SO_REUSEADDR) immediately
 * after; the close→rebind window is small enough to ignore for a test. */
static unsigned short loopbackPickEphemeralPort(void) {
    SOCKET probe;
    struct sockaddr_in bindAddr;
    struct sockaddr_in gotAddr;
    socklen_t gotLen;
    unsigned short port;

    probe = socket(AF_INET, SOCK_DGRAM, 0);
    if (probe == INVALID_SOCKET) {
        return 0;
    }
    memset(&bindAddr, 0, sizeof(bindAddr));
    bindAddr.sin_family      = AF_INET;
    bindAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    bindAddr.sin_port        = 0;
    if (bind(probe, (struct sockaddr *)&bindAddr, sizeof(bindAddr)) != 0) {
        closesocket(probe);
        return 0;
    }
    memset(&gotAddr, 0, sizeof(gotAddr));
    gotLen = (socklen_t)sizeof(gotAddr);
    if (getsockname(probe, (struct sockaddr *)&gotAddr, &gotLen) != 0) {
        closesocket(probe);
        return 0;
    }
    port = ntohs(gotAddr.sin_port);
    closesocket(probe);
    return port;
}

/* Shared server bring-up: threads, an ephemeral localhost port, an
 * Everard-Island ServerSim (running unless lobbyMode), and the UDP server
 * instance accepting remote clients. Sets h->threadsUp / h->port / h->sim /
 * h->serverUp. The caller has already zeroed *h. Returns false on any failure
 * (the harness stays safe to Stop). */
static bool loopbackBringUpServer(LoopbackHarness *h, bool lobbyMode) {
    BYTE emap[6000] = E_MAP;
    ServerInstanceConfig cfg;

    bolo_net_init();

    if (!threadsCreate(TRUE)) {
        return false;
    }
    h->threadsUp = true;

    h->port = loopbackPickEphemeralPort();
    if (h->port == 0) {
        return false;
    }

    h->sim = serverSimCreateCompressed(emap, LOOPBACK_EMAP_LEN,
                                       "Everard Island", gameOpen,
                                       false, 0, -1);
    if (h->sim == NULL) {
        return false;
    }
    /* The wire joiner claims its own slot — keep the lobby open. */
    serverSimSetAllowNewPlayers(h->sim, true);

    memset(&cfg, 0, sizeof(cfg));
    cfg.udpPort             = h->port;
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
    if (!serverInstanceStartup(h->sim, &cfg)) {
        return false;
    }
    h->serverUp = true;
    return true;
}

/* Connect a fresh ClientSim over UDP to the harness server. spectator selects a
 * tankless spectator connect (no tank slot, no map download) over a normal
 * player join. impairSpec is applied to the client endpoint before connect.
 * Sets h->cs / h->clientUp. */
static bool loopbackConnectClient(LoopbackHarness *h, const char *playerName,
                                  const char *impairSpec, bool spectator) {
    /* Set before connect — transportUdpClientCreate reads WB_NETIMPAIR
     * once at construction. */
    loopbackSetImpairEnv(impairSpec);

    h->cs = clientSimAlloc();
    if (h->cs == NULL) {
        return false;
    }
    clientSimCreate(h->cs);
    if (!clientSimConnectUdp(h->cs, "127.0.0.1", h->port, playerName,
                             /*fallbackCountry*/ "", /*password*/ "",
                             /*wbnApiToken*/ NULL, /*wbnServerKey*/ NULL,
                             /*wantRejoin*/ false, /*trackerAddr*/ "",
                             /*trackerPort*/ 0, spectator)) {
        return false;
    }
    h->clientUp = true;
    return true;
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

void loopbackHarnessPump(LoopbackHarness *h) {
    if (h == NULL) return;
    if (h->clientUp) clientSimNetTick(h->cs);
    /* The server's datagrams are ingested by its background recv thread into an
     * SPSC queue that serverInstanceTick drains. In a tight pump loop that
     * thread can lag a pump, so the just-sent client datagram isn't drained
     * until a later tick — and that scheduling slack shifts ack/retransmit
     * timing relative to the seeded impairment, which made connect/lobby
     * convergence under loss nondeterministic. Yield ~1ms so the recv thread
     * delivers this pump's datagram before the server tick consumes the queue,
     * making the loopback tests deterministic. */
    SDL_Delay(1);
    if (h->serverUp) serverInstanceTick(h->sim);
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

void loopbackHarnessStop(LoopbackHarness *h) {
    if (h == NULL) return;
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
    h->clientUp = false;
    h->serverUp = false;
}
