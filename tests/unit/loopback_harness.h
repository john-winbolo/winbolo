/*
 * In-process loopback transport harness.
 *
 * Stands up the real UDP server transport (via serverInstanceStartup /
 * serverInstanceTick) and the real UDP client transport (via
 * clientSimConnectUdp / clientSimNetTick) inside one test process, talking
 * over actual localhost sockets. A pump steps both endpoints one tick at a
 * time so wire-level reliability behaviour (join handshake, map download,
 * reliable control events, lobby→running transition) can be driven from
 * CTest under seeded network impairment.
 *
 * ── Impairment ──────────────────────────────────────────────────────────
 * Impairment runs on the CLIENT endpoint, which impairs both directions:
 * impairOut on the client→server leg and impairIn on the server→client leg.
 * It is gated by WB_ENABLE_NETIMPAIR, which the WinBoloUnitTests target
 * defines to 1 only for its own compilation of transport_udp_client.c —
 * server_static (and every shipping target) keeps WB_ENABLE_NETIMPAIR=0.
 * The per-test spec is handed to the client through the WB_NETIMPAIR
 * environment variable (the same hook the dedicated server's -netimpair
 * flag drives), so the start call sets it before connecting and clears it
 * otherwise. The spec grammar is netImpairParseConfig's:
 * "delay=75,jitter=30,loss=5,burst=2"; pass NULL/"" for a clean path.
 *
 * ── Determinism ─────────────────────────────────────────────────────────
 * Impairment decisions come from the process-global bolo_rand stream, which
 * the start call seeds via bolo_srand(seed); every impairment draw and every
 * server-sim draw happens on the pumping thread in a fixed per-pump order
 * (client tick, then server tick), so the impairment sequence is
 * reproducible. The server still runs its normal background recv thread
 * (started by transportUdpServerCreate); that thread only buffers raw
 * datagrams into an SPSC queue and draws no randomness, but it does add OS
 * scheduling variance to when a datagram becomes drainable. Tests therefore
 * assert "converges within N pump iterations", never an exact packet trace.
 * Specs used by the bundled tests are loss-only (delay=0/jitter=0) so a
 * surviving datagram is due immediately regardless of wall-clock — the tight
 * pump loop never needs SDL_GetTicks() to advance for a packet to deliver.
 *
 * ── Lifecycle ───────────────────────────────────────────────────────────
 *   loopbackHarnessStart(&h, name, lobbyMode, impairSpec, seed)
 *       Picks an ephemeral localhost port, creates a ServerSim (running when
 *       lobbyMode is false, lobby when true) with no pre-added players, runs
 *       serverInstanceStartup with acceptRemoteClients=true, then connects a
 *       fresh ClientSim over UDP. Returns false on any setup failure.
 *   loopbackHarnessPump(&h)              one client tick + one server tick.
 *   loopbackHarnessPumpUntil(...)        pump until a predicate holds.
 *   loopbackHarnessTriggerGameStart(&h)  mark the joined client ready and
 *       enter the lobby countdown (lobby mode only).
 *   loopbackHarnessStop(&h)              tear everything down.
 *
 * The caller owns nothing inside the harness; Stop frees it all. The API is
 * deliberately small — the upcoming wire-change tests (conn migration, view
 * tick, map resync) are its intended consumers.
 */

#ifndef WINBOLO_TEST_LOOPBACK_HARNESS_H
#define WINBOLO_TEST_LOOPBACK_HARNESS_H

#include <stdbool.h>
#include <stdint.h>

struct ServerSim;
struct ClientSim;

typedef struct LoopbackHarness {
    struct ServerSim *sim;
    struct ClientSim *cs;
    unsigned short    port;     /* ephemeral localhost port the server bound */
    bool              threadsUp;
    bool              serverUp;
    bool              clientUp;
} LoopbackHarness;

/* Predicate evaluated after each pump by loopbackHarnessPumpUntil. Returns
 * true to stop pumping. `user` is passed through untouched. */
typedef bool (*LoopbackPredicate)(LoopbackHarness *h, void *user);

/* Bring up server + client. playerName is the joiner's name; lobbyMode
 * selects a lobby (true) or already-running (false) server. impairSpec is a
 * netImpairParseConfig spec applied to the client endpoint, or NULL/"" for a
 * clean path. seed seeds bolo_srand for reproducible impairment. Returns
 * false (and leaves the harness safe to Stop) on failure. */
bool loopbackHarnessStart(LoopbackHarness *h, const char *playerName,
                          bool lobbyMode, const char *impairSpec,
                          uint64_t seed);

/* Bring up server + a tankless SPECTATOR client (clientSimConnectUdp with the
 * spectator flag set), on a clean path with no peer players. The server is
 * started running with viewer slots opened so its spectator-accept path admits
 * the connect; the client reaches CLIENT_CONNECT_SPECTATING with no tank slot
 * or map download. seed seeds bolo_srand for reproducibility. Returns false
 * (and leaves the harness safe to Stop) on failure. */
bool loopbackHarnessStartSpectator(LoopbackHarness *h, const char *playerName,
                                   uint64_t seed);

/* Advance both endpoints by exactly one tick: client tick (sends queued
 * input/commands, receives + impairs inbound, applies control/snapshots),
 * then server tick (drains recv queue, ticks the sim, sends snapshots /
 * control). */
void loopbackHarnessPump(LoopbackHarness *h);

/* Pump up to maxIters times, evaluating pred after each pump. Returns the
 * 1-based pump count at which pred first held, or -1 if it never held within
 * maxIters. A NULL pred pumps exactly maxIters times and returns maxIters. */
int loopbackHarnessPumpUntil(LoopbackHarness *h, int maxIters,
                             LoopbackPredicate pred, void *user);

/* Lobby mode only: mark the joined client's slot ready on the server and run
 * the all-ready check, which (with a wire client present) enters the lobby
 * countdown. The countdown→running transition then happens inside
 * serverInstanceTick as the harness keeps pumping. Returns false if the
 * client has not yet been assigned a slot. */
bool loopbackHarnessTriggerGameStart(LoopbackHarness *h);

/* Tear down client, server (stops the recv thread), sim and threads, and
 * clear the WB_NETIMPAIR environment override. Safe on a zeroed or
 * partially-started harness. */
void loopbackHarnessStop(LoopbackHarness *h);

#endif /* WINBOLO_TEST_LOOPBACK_HARNESS_H */
