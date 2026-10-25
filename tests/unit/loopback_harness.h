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
 * A test that does want a delay= spec calls loopbackHarnessUseVirtualClock,
 * which puts both ends' impairment layers and round-trip measurements on a
 * counter the pump advances 20ms per tick — one serverInstanceTick, which is
 * one 20ms frame of two half-steps, so virtual time runs at the sim's rate.
 * The delay then costs a fixed number of pumps and no real time. The same
 * call runs the pump in lockstep: no sleeps, and each side waits for exactly
 * the datagrams the other has put on the wire (counted at both ends) before
 * it ticks, so neither the recv thread's scheduling nor the loopback path can
 * move a datagram to another pump. With one client, a seed and the same
 * script, such a run is the same run on every machine, tick for tick.
 *
 * ── Lifecycle ───────────────────────────────────────────────────────────
 *   loopbackHarnessStart(&h, name, lobbyMode, impairSpec, seed)
 *       Creates a ServerSim (running when lobbyMode is false, lobby when
 *       true) with no pre-added players, runs serverInstanceStartup with
 *       acceptRemoteClients=true and udpPort=0 — the OS picks an ephemeral
 *       localhost port and the harness reads back the one it bound — then
 *       connects a fresh ClientSim over UDP to it. Returns false on any
 *       setup failure.
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

/* SOCKET and struct sockaddr_in, for loopbackRecvFromServer below. Tests that
 * drive the harness server with a raw socket of their own need these anyway,
 * and the header is self-guarded, so pulling it in here costs them nothing. */
#include "platform_net.h"

struct ServerSim;
struct ClientSim;

typedef struct LoopbackHarness {
    struct ServerSim *sim;
    struct ClientSim *cs;
    struct ClientSim *cs2;      /* second client, or NULL — see AddClient */
    unsigned short    port;     /* ephemeral localhost port the server bound */
    bool              threadsUp;
    bool              serverUp;
    bool              clientUp;
    bool              client2Up;
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

/* Called once by loopbackHarnessStartPrepared, with the server sim built and
 * h->sim set but no client connected yet. `user` is passed through. */
typedef void (*LoopbackSimSetup)(LoopbackHarness *h, void *user);

/* Same as loopbackHarnessStart, but runs `setup` on the fresh ServerSim
 * between bringing the server up and connecting the client. A test that has
 * to shape the world it plays in — carve terrain, move a pillbox — wants the
 * shaping to be in the map the joiner downloads, not a pile of map events
 * chasing it afterwards. setup runs under the tick mutex; a NULL setup makes
 * this exactly loopbackHarnessStart. */
bool loopbackHarnessStartPrepared(LoopbackHarness *h, const char *playerName,
                                  bool lobbyMode, const char *impairSpec,
                                  uint64_t seed, LoopbackSimSetup setup,
                                  void *user);

/* Join a second player client to a harness that is already up, so a test can
 * observe what one client's departure does to the other's server-side state
 * (a mask keyed by player, a roster entry) — the single-client harness can
 * only ever watch a slot it is itself sitting in. Connects on a clean path
 * (the first client keeps whatever impairment it was constructed with) and
 * pumps in the same phase as the first client. Returns false if the harness
 * has no server, already has a second client, or the connect failed; the
 * harness stays safe to Stop either way. */
bool loopbackHarnessAddClient(LoopbackHarness *h, const char *playerName);

/* Bring up server + a tankless SPECTATOR client (clientSimConnectUdp with the
 * spectator flag set), on a clean path with no peer players. The server is
 * started running with viewer slots opened so its spectator-accept path admits
 * the connect; the client reaches CLIENT_CONNECT_SPECTATING with no tank slot
 * or map download. seed seeds bolo_srand for reproducibility. Returns false
 * (and leaves the harness safe to Stop) on failure. */
bool loopbackHarnessStartSpectator(LoopbackHarness *h, const char *playerName,
                                   uint64_t seed);

/* Bring up server + a tankless SPECTATOR client against a server in the LOBBY
 * (lobbyMode=true) rather than a running game. Viewer slots are opened so the
 * accept path admits the connect; in lobby/countdown the server registers the
 * viewer as a live control-bus subscriber, so allowlisted lobby control flows
 * to the spectator's ClientSim over CHANNEL_CONTROL with no delayed ring. seed
 * seeds bolo_srand. Returns false (and leaves the harness safe to Stop) on
 * failure. */
bool loopbackHarnessStartSpectatorLobby(LoopbackHarness *h,
                                        const char *playerName, uint64_t seed);

/* Advance both endpoints by exactly one tick: client tick (sends queued
 * input/commands, receives + impairs inbound, applies control/snapshots),
 * then server tick (drains recv queue, ticks the sim, sends snapshots /
 * control). */
void loopbackHarnessPump(LoopbackHarness *h);

/* Pump only the client endpoints `n` times, without ticking the server. The
 * server's recv thread still queues arriving datagrams; nothing drains them
 * until the next loopbackHarnessPump or a direct serverInstanceTick. Models
 * a server-side hitch: the client keeps producing while the server stops. */
void loopbackHarnessPumpClientOnly(LoopbackHarness *h, int n);

/* Drive both transports' impairment layers and ping measurements from a
 * counter the harness advances 20ms per pump instead of the wall clock, so a
 * delay= spec is deterministic and costs no real time, and the ping the
 * server measures is the delay in frames. It also puts the pump in lockstep
 * (see Determinism above); the server->client half of that needs a single
 * client and is left off once loopbackHarnessAddClient has added a second.
 * Off by default: the loopback tests that already run impaired keep the wall
 * clock and their existing timing. Call after Start and before the first
 * pump. No effect unless WB_ENABLE_NETIMPAIR is on. */
void loopbackHarnessUseVirtualClock(LoopbackHarness *h, bool on);

/* Drop the next `count` server->client datagrams of packetType that reach
 * client `cs` (h->cs or h->cs2), as if they were lost on the wire. Other
 * packet types and the other client are not touched. count 0 turns it off.
 * DropNextLeft returns how many drops are still pending (0 once all have
 * happened). No effect, and Left returns 0, unless WB_ENABLE_NETIMPAIR is
 * on. */
void loopbackHarnessDropNextToClient(LoopbackHarness *h, struct ClientSim *cs,
                                     uint8_t packetType, int count);
int  loopbackHarnessDropNextLeft(LoopbackHarness *h, struct ClientSim *cs);

/* The same for client->server datagrams: drop the next `count` of
 * packetType that client `cs` sends, as if they were lost on the way to the
 * server. */
void loopbackHarnessDropNextFromClient(LoopbackHarness *h, struct ClientSim *cs,
                                       uint8_t packetType, int count);
int  loopbackHarnessDropNextFromClientLeft(LoopbackHarness *h,
                                           struct ClientSim *cs);

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

/* Receive the next datagram on `s` that actually came from `server`, into buf
 * (at most cap bytes). Returns the byte count, or <= 0 when nothing the server
 * sent is waiting — so a caller polling a non-blocking socket can write
 * `while ((n = loopbackRecvFromServer(s, in, sizeof(in), &addr)) > 0)` exactly
 * where it used to write the bare recvfrom.
 *
 * Tests that speak to the harness server over a raw socket bind that socket to
 * an ephemeral 127.0.0.1 port, and a full `ctest -j` run has dozens of other
 * loopback servers on the same interface sending JOIN_CHALLENGEs, JOIN_ACCEPTs
 * and the rest at ephemeral ports of their own. One addressed to a port this
 * test happens to hold arrives looking exactly like the reply it asked for,
 * and counting it means asserting on evidence our server never produced — a
 * false pass for a test proving a reply came, a false failure for one proving
 * none did.
 *
 * A datagram from anyone else is consumed and dropped here rather than
 * returned, so a stray neither reaches the caller nor ends its drain loop
 * early: the loop keeps reading until the socket is genuinely empty, which is
 * also what keeps a socket buffer from filling for callers that only want to
 * discard. `server` must not be NULL. */
int loopbackRecvFromServer(SOCKET s, uint8_t *buf, int cap,
                           const struct sockaddr_in *server);

/* Tear down client, server (stops the recv thread), sim and threads, clear
 * the WB_NETIMPAIR environment override, and put the impairment layer back on
 * the wall clock so one test's virtual clock cannot leak into the next. Safe
 * on a zeroed or partially-started harness. */
void loopbackHarnessStop(LoopbackHarness *h);

#endif /* WINBOLO_TEST_LOOPBACK_HARNESS_H */
