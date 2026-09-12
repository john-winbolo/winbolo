/*
 * Loopback integration test for the parallel reliable-ordered channel layer
 * (channel_mux.c) once it is wired into the live UDP transport.
 *
 * The channel runs empty and alongside the existing reliable-event queues: a
 * channel frame rides every snapshot (server→client) and every input
 * (client→server) as a trailer, and a standalone PACKET_CHANNEL carries it
 * when no such datagram flows.  These two cases drive the wiring end-to-end
 * over the real loopback transport.
 *
 *   1. Empty flow — a running session with no game/map traffic.  Frames are
 *      still exchanged (the 2-byte empty frame rides every trailer); the
 *      game (0), map (1) and bulk (3) channels carry no message, so their
 *      expectedSeq / ackedSeq must stay at 0 and neither side disconnects.
 *      CHANNEL_CONTROL (2) is excluded: control events now ride it, and the
 *      join sync-replay advances it.
 *
 *   2. Synthetic round-trip under loss + dup.  A message is injected
 *      on a server channel via the test-only hook; under impairment it must
 *      still cross the path (the client transport drains it, advancing
 *      expectedSeq) and the client's cumulative ack must make it back (the
 *      server's ackedSeq advances) — proving retransmit / cumulative-ack
 *      survive a hostile path.  It runs on CHANNEL_CONTROL; every message
 *      channel (0-2) is now consumed by the client transport, so the round-trip
 *      is observed via the channel's expectedSeq / ackedSeq bookkeeping rather
 *      than a raw receive peek (the drain owns the bytes and decode-skips a
 *      non-control payload harmlessly).
 *
 *   3. Real game events ride the best-effort channel.  A running session fires
 *      under loss; each shot expires into an EVENT_EXPLOSION the server routes
 *      onto CHANNEL_GAME_EFFECT (ephemeral events are best-effort).  The client
 *      must deliver it over that channel (expectedSeq advances) and apply it
 *      through the shared game-event path (brain event buffer grows) — the
 *      migration-parity check that the channel carries the same observable game
 *      events the per-client event queue used to.
 *
 *   6. Map-change recovery on channel 1.  A terrain change is staged under loss
 *      (server map mutated + map event held); it must recover over CHANNEL_MAP
 *      and apply on the client (the cell's terrain changes) WITHOUT a full map
 *      resync — the robustness win over the old "dropped change -> desync ->
 *      resync" path.
 *
 *   7. Map generation gate.  After the client installs a fresh map generation
 *      via a real resync, a map event tagged with the older generation (still
 *      arriving on CHANNEL_MAP) must be dropped, while a new-generation change
 *      is applied — proving a stale change can't land on top of a freshly
 *      installed map.
 *
 *   8. Previous-game straggler gate.  A game event staged on channel 0 and left
 *      in flight across game start must never reach the sim in the new game
 *      (the server drops its send tail and the client lifts its receive
 *      baseline via CTRL_CHANNEL_RESET), while a fresh post-flip game event
 *      still applies.
 *
 *   9. Map channel pacing.  A terrain burst larger than CHANNEL_MAP's send
 *      window is staged in one tick; the server must hold what will not fit
 *      and resume on later snapshots as acks free the window, never flagging
 *      the slot for removal.  Every square lands, the hold buffer empties, and
 *      no resync is involved — the changes arrive over the channel.
 *
 *   9b. A scenario's fill across the wire.  One fill-rect op covering 300
 *      squares, issued from the scenario hook the sim runs each frame.  The
 *      funnel applies a tick's budget and carries the rest, so the change
 *      leaves the server over two frames; every square must reach the client
 *      over CHANNEL_MAP, with no resync and no slot flagged for removal.
 *      The rectangle has to sit inside the client's viewports: terrain it
 *      cannot see is culled at the drain and never queued, so a rectangle
 *      chosen anywhere else would produce map events the server is right to
 *      drop and prove nothing about the pacing.
 *
 *  10. Map ack from the wire.  An InputPacket still carries a map-event ack
 *      field from the snapshot-tail era; the server's hold-buffer cursor is
 *      owned by its own drain now, so a client claiming an ack past the
 *      queue head must not move it.  Otherwise the space check wraps, every
 *      later change for that slot is dropped, and the stall flag sticks.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "client_enums.h"          /* netStatus / netRunning */
#include "client_sim_internal.h"   /* ClientSim::transport (Transport handle) */
#include "input_packet.h"
#include "channel_mux.h"           /* CHANNEL_GAME / CHANNEL_MAP / CHANNEL_COUNT / CHANNEL_MAX_SEG */
#include "transport_udp.h"         /* test-only channel hooks, spliceGameEventsBeforeTail */
#include "transport_udp_internal.h" /* packU32 / packGameEvent (map-channel payload) */
#include "server_sim.h"            /* serverSimAddEvent (overflow case) */
#include "server_sim_internal.h"   /* serverSimBuildViewports / inAnyViewport — the map-event cull's own predicate */
#include "server_sim_scenario.h"   /* the op funnel and the per-tick hook the fill case drives */
#include "test_harness.h"
#include "loopback_harness.h"

#define CONNECT_MAX     2000   /* join + map download */
#define EMPTY_PUMPS      300   /* steady state long enough for trailers to flow */
#define ROUNDTRIP_MAX   3000   /* delivery + ack convergence under impairment */
#define SETTLE_PUMPS     300   /* let the join control-replay drain before baselining */
#define RUNNING_MAX     2000   /* countdown (250) + RUNNING delivery under loss */
#define FLIP_SETTLE      150   /* flush any game-start transition events post-flip */
#define STRAGGLER_STAB   400   /* idle window the stale event must not break */
#define PACE_BURST       300   /* terrain changes staged in one tick; the map
                                * channel's window is CHANNEL_MAP_WINDOW (128),
                                * so the burst cannot fit in one drain */
#define PACE_DELIVER_MAX 2000  /* bounded convergence for the whole burst */

static bool pred_connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

/* Server-side readiness: the client reports CONNECTED once it has the full map,
 * but the server only flips downloadComplete a CHANNEL_BULK ack round-trip
 * later. The real game-event producer skips a slot until then, so a test that
 * drives it must wait on this after connect. */
static bool pred_server_download_complete(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

/* Connected AND in the lobby phase. In lobby mode the phase is published over
 * CHANNEL_CONTROL, which lands a round-trip after the client reports CONNECTED,
 * so a test that needs the lobby must wait for both rather than asserting the
 * phase immediately on connect. */
static bool pred_in_lobby(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED &&
           clientSimIsInLobby(h->cs);
}

/* Send one (zeroed) input so the client transmits a datagram carrying its
 * channel trailer. Returns the next input tick. */
static uint32_t feed_input(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Send one fire input. The server creates a shell; when it expires it spawns
 * an EVENT_EXPLOSION near the tank, which the server routes onto CHANNEL_GAME.
 * Returns the next input tick. */
static uint32_t feed_fire(LoopbackHarness *h, uint32_t tick) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = clientSimGetMyPlayerNum(h->cs);
    pkt.actions   = INPUT_ACTION_FIRE;
    clientSimNetSendInput(h->cs, &pkt);
    return tick + 1;
}

/* Case 1: the empty channel is inert — frames flow but no sequence moves. */
static int run_empty_flow(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    int i;
    Transport *ct;
    int slot;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanEmpty", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x5EEDu),
                  "harness start (empty flow) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Steady state: drive inputs so both directions carry channel trailers. */
    for (i = 0; i < EMPTY_PUMPS; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
    }

    /* Frames must have been exchanged on both endpoints. */
    {
        uint32_t cliFrames = 0, srvFrames = 0;
        transportUdpClientChannelTestStats(ct, CHANNEL_GAME, NULL, NULL, &cliFrames);
        transportUdpServerChannelTestStats(slot, CHANNEL_GAME, NULL, NULL, &srvFrames);
        if (cliFrames == 0 || srvFrames == 0) {
            loopbackHarnessStop(&h);
            UT_FAIL("no channel frames exchanged (client=%u server=%u)",
                    (unsigned)cliFrames, (unsigned)srvFrames);
        }
    }

    /* Every channel with no traffic must be untouched on both sides.
     * CHANNEL_CONTROL is skipped: control events ride it and the join
     * sync-replay has advanced it. CHANNEL_BULK is skipped too: the join map
     * download now streams on it, so it has legitimately advanced by connect
     * time (the channels under test here are the no-traffic ones). */
    {
        uint8_t ch;
        for (ch = 0; ch < CHANNEL_COUNT; ch++) {
            uint32_t cExp = 0, cAck = 0, sExp = 0, sAck = 0;
            if (ch == CHANNEL_CONTROL || ch == CHANNEL_BULK) continue;
            transportUdpClientChannelTestStats(ct, ch, &cExp, &cAck, NULL);
            transportUdpServerChannelTestStats(slot, ch, &sExp, &sAck, NULL);
            if (cExp != 0 || cAck != 0 || sExp != 0 || sAck != 0) {
                loopbackHarnessStop(&h);
                UT_FAIL("ch %u moved on empty flow "
                        "(client exp=%u ack=%u, server exp=%u ack=%u)",
                        (unsigned)ch, (unsigned)cExp, (unsigned)cAck,
                        (unsigned)sExp, (unsigned)sAck);
            }
        }
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during empty-flow steady state");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 2: a message survives loss + dup and is acked back. */
static int run_roundtrip_impaired(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    int slot;
    Transport *ct;
    static const uint8_t kMsg[] = "channel-roundtrip!";
    const uint16_t kLen = (uint16_t)sizeof(kMsg);  /* includes NUL */
    bool converged = false;
    int i;

    /* Loss + dup only — no jitter/delay.  The loopback harness pumps with no
     * wall-clock pacing, so local-tick time outruns wall-clock; net_impair
     * delays delivery on a wall-clock basis while the join retry budget counts
     * local ticks, so a jitter/delay spec times out the join before a delayed
     * packet is ever delivered.  loss and dup deliver in the same tick, so they
     * exercise retransmit and dedup without that decoupling (matching the
     * loss-only specs every other loopback test uses). */
    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanRT", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10,dup=20",
                                       /*seed*/ 0xC0FFEEu),
                  "harness start (round-trip) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    slot = (int)clientSimGetMyPlayerNum(h.cs);
    ct   = &h.cs->transport;

    /* The join sync-replay has already put control events on CHANNEL_CONTROL,
     * so baseline the channel's seqs after letting it settle, then require the
     * injected message to advance BOTH past their baselines. */
    {
        int s;
        for (s = 0; s < SETTLE_PUMPS; s++) {
            inputTick = feed_input(&h, inputTick);
            loopbackHarnessPump(&h);
        }
    }
    uint32_t baseSrvAck = 0, baseCliExp = 0;
    transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &baseSrvAck, NULL);
    transportUdpClientChannelTestStats(ct, CHANNEL_CONTROL, &baseCliExp, NULL, NULL);

    /* Inject a single message on the server's control channel for this slot.
     * The client transport now consumes CHANNEL_CONTROL (decoding each message
     * as a control event), so the raw bytes can't be peeked back — the
     * round-trip is proven via the channel's own bookkeeping instead: the
     * message crosses the impaired path (client expectedSeq advances as the
     * transport drains it) and the cumulative ack returns (server ackedSeq
     * advances). The payload need not decode as a control event; the drain
     * skips it harmlessly while still advancing expectedSeq. */
    UT_ASSERT_MSG(transportUdpServerChannelTestSend(slot, CHANNEL_CONTROL,
                                                    kMsg, kLen),
                  "test channel send rejected");

    /* Pump (feeding input so the client's ack rides back) until the injected
     * message has been delivered (client expectedSeq advanced past baseline)
     * AND acked (server ackedSeq advanced past baseline). */
    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        uint32_t srvAck = 0, cliExp = 0;

        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);

        transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &srvAck, NULL);
        transportUdpClientChannelTestStats(ct, CHANNEL_CONTROL, &cliExp, NULL, NULL);
        if (cliExp > baseCliExp && srvAck > baseSrvAck) {
            converged = true;
            break;
        }
    }

    fprintf(stderr, "  channel round-trip (impaired): converged after %d pump(s) "
                    "(cap %d) delivered+acked=%d\n", i, ROUNDTRIP_MAX,
            (int)converged);

    if (!converged) {
        uint32_t srvAck = 0, cliExp = 0;
        transportUdpServerChannelTestStats(slot, CHANNEL_CONTROL, NULL, &srvAck, NULL);
        transportUdpClientChannelTestStats(ct, CHANNEL_CONTROL, &cliExp, NULL, NULL);
        loopbackHarnessStop(&h);
        UT_FAIL("injected control message never delivered+acked within %d pumps "
                "(client expectedSeq=%u/base %u server ackedSeq=%u/base %u)",
                ROUNDTRIP_MAX, (unsigned)cliExp, (unsigned)baseCliExp,
                (unsigned)srvAck, (unsigned)baseSrvAck);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 3: real game events produced by the running sim ride channel 0 and are
 * applied on the client, under loss. */
static int run_game_event_channel(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    bool gotEvent = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanGE", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10",
                                       /*seed*/ 0x9A11Eu),
                  "harness start (game event) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Fire repeatedly. Each shot's shell expires into an EVENT_EXPLOSION the
     * server distributes onto CHANNEL_GAME. The client must deliver it over the
     * channel (expectedSeq advances) AND apply it through the shared game-event
     * path (brainEventCount grows — the snapshot game tail is empty now, so the
     * only path that buffers a brain event is the channel drain). */
    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        uint32_t cliExp = 0;

        inputTick = feed_fire(&h, inputTick);
        loopbackHarnessPump(&h);

        transportUdpClientChannelTestStats(ct, CHANNEL_GAME_EFFECT, &cliExp, NULL, NULL);
        if (cliExp >= 1 && clientSimGetBrainEventCount(h.cs) > 0) {
            gotEvent = true;
            break;
        }
    }

    fprintf(stderr, "  channel game-event (impaired): converged after %d pump(s) "
                    "(cap %d) gotEvent=%d brainEvents=%d\n", i, ROUNDTRIP_MAX,
            (int)gotEvent, clientSimGetBrainEventCount(h.cs));

    if (!gotEvent) {
        uint32_t cliExp = 0, srvAck = 0;
        int brainEvents = clientSimGetBrainEventCount(h.cs);
        transportUdpClientChannelTestStats(ct, CHANNEL_GAME_EFFECT, &cliExp, NULL, NULL);
        transportUdpServerChannelTestStats(slot, CHANNEL_GAME_EFFECT, NULL, &srvAck, NULL);
        loopbackHarnessStop(&h);
        UT_FAIL("no game event delivered+applied via best-effort channel within %d pumps "
                "(client expectedSeq=%u server ackedSeq=%u brainEvents=%d)",
                ROUNDTRIP_MAX, (unsigned)cliExp, (unsigned)srvAck, brainEvents);
    }

    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while exchanging game events");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 4: the splice that the snapshot-trailer drain uses to merge
 * channel-delivered game events ahead of the staged map tail. Pure index math,
 * tested directly: order preserved, nothing lost or duplicated, clamps to cap. */
static int run_game_event_splice(void) {
    GameEvent ev[8];
    GameEvent chan[3];
    int n, i;

    /* Two map-tail events staged at [0,2); splice three channel game events in
     * front. Tag each by data[0] so position is verifiable. */
    memset(ev, 0, sizeof(ev));
    memset(chan, 0, sizeof(chan));
    ev[0].type = EVENT_MAP_CHANGE;    ev[0].data[0] = 0xA0;
    ev[1].type = EVENT_MAP_CHANGE;    ev[1].data[0] = 0xA1;
    chan[0].type = EVENT_EXPLOSION;   chan[0].data[0] = 0x10;
    chan[1].type = EVENT_EXPLOSION;   chan[1].data[0] = 0x11;
    chan[2].type = EVENT_TANK_KILLED; chan[2].data[0] = 0x12;

    n = spliceGameEventsBeforeTail(ev, /*tailStart*/ 0, /*tailCount*/ 2,
                                   chan, /*chanCount*/ 3, /*cap*/ 8);
    if (n != 5) UT_FAIL("splice count = %d, want 5", n);
    if (ev[0].data[0] != 0x10 || ev[1].data[0] != 0x11 || ev[2].data[0] != 0x12)
        UT_FAIL("channel events not spliced in order at the front");
    if (ev[3].data[0] != 0xA0 || ev[4].data[0] != 0xA1)
        UT_FAIL("map tail not preserved behind the channel events");

    /* chanCount == 0 is a no-op that returns the existing count. */
    memset(ev, 0, sizeof(ev));
    ev[0].data[0] = 0xA0;
    ev[1].data[0] = 0xA1;
    n = spliceGameEventsBeforeTail(ev, 0, 2, chan, 0, 8);
    if (n != 2 || ev[0].data[0] != 0xA0 || ev[1].data[0] != 0xA1)
        UT_FAIL("zero-channel splice mutated the tail (n=%d)", n);

    /* Overflow: more channel events than cap. They clamp to cap, the tail is
     * dropped for lack of room, and nothing is written past ev[cap]. */
    {
        GameEvent big[6];
        GameEvent many[10];
        memset(big, 0, sizeof(big));
        memset(many, 0, sizeof(many));
        for (i = 0; i < 10; i++) many[i].data[0] = (uint8_t)(0x20 + i);
        big[4].type = 0x7F;  /* sentinels just past cap=4 */
        big[5].type = 0x7F;
        n = spliceGameEventsBeforeTail(big, 0, 2, many, 10, 4);
        if (n != 4) UT_FAIL("overflow splice count = %d, want 4", n);
        if (big[4].type != 0x7F || big[5].type != 0x7F)
            UT_FAIL("splice wrote past cap");
        if (big[0].data[0] != 0x20 || big[3].data[0] != 0x23)
            UT_FAIL("overflow splice did not place clamped channel events");
    }

    return 0;
}

/* Case 5: a game-event channel send that overflows the window mid-tick defers
 * the disconnect (sets the pending-removal flag) instead of acting inline, and
 * the removal drain then clears it. Drives the real producer, not a stub. */
static int run_game_channel_overflow(void) {
    LoopbackHarness h;
    int connectedAt;
    int slot;
    int i;
    bool windowFull = false;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanOvf", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x0FFEEu),
                  "harness start (overflow) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    /* Let the server finish the join download (its downloadComplete lags the
     * client's CONNECTED by a CHANNEL_BULK ack round-trip).  Until it does, the
     * game-event producer skips this slot as still-downloading, so the injected
     * event below would never reach channelSend and never defer.  This pump runs
     * before the window fill, so it can't drain the filled window. */
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX,
                                 pred_server_download_complete, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server never completed the join download within %d pumps",
                CONNECT_MAX);
    }

    slot = (int)clientSimGetMyPlayerNum(h.cs);

    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d already flagged for removal before overflow", slot);
    }

    /* Fill the slot's game-channel send window without pumping, so no client
     * ack drains it, until channelSend rejects the next message. */
    {
        uint8_t msg[GAME_EVENT_MAX_WIRE_SIZE];
        memset(msg, 0, sizeof(msg));
        for (i = 0; i < CHANNEL_GAME_WINDOW + 8; i++) {
            if (!transportUdpServerChannelTestSend(slot, CHANNEL_GAME, msg,
                                                   (uint16_t)sizeof(msg))) {
                windowFull = true;
                break;
            }
        }
    }
    if (!windowFull) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not fill the game channel window for slot %d", slot);
    }

    /* Inject one real game event and run the real producer. Its channelSend
     * must hit the full window and defer — not perform — the disconnect.
     * EVENT_TANK_KILLED is non-sound and not distance-culled, so it reaches
     * the channelSend unconditionally. */
    {
        GameEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type = EVENT_TANK_KILLED;
        serverSimAddEvent(h.sim, &ev);
        transportUdpServerDrainEvents(h.sim);
    }

    if (!transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("channel overflow did not defer a disconnect for slot %d", slot);
    }

    /* The deferred removal is resolved at the safe point and the flag clears. */
    transportUdpServerDrainPendingRemovals(h.sim);
    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("pending removal not cleared by the drain for slot %d", slot);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Find a land cell whose current terrain differs from `target` (so applying
 * `target` is an observable change) and is not the avoided cell. Scans the map
 * interior, which is solid land on the Everard map the harness loads. Returns
 * false if none found in the scanned span. */
static bool find_changeable_cell(ClientSim *cs, uint8_t target,
                                 uint8_t avoidX, uint8_t avoidY,
                                 uint8_t *outX, uint8_t *outY) {
    int x, y;
    for (y = 96; y < 160; y++) {
        for (x = 96; x < 160; x++) {
            uint8_t t;
            if ((uint8_t)x == avoidX && (uint8_t)y == avoidY) continue;
            t = clientSimGetMapTerrain(cs, (uint8_t)x, (uint8_t)y);
            if (t != DEEP_SEA && t != RIVER && t != target) {
                *outX = (uint8_t)x;
                *outY = (uint8_t)y;
                return true;
            }
        }
    }
    return false;
}

/* Case 6: a staged terrain change recovers over channel 1 under loss and is
 * applied on the client without forcing a full map resync. */
static int run_map_event_channel_recovery(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    uint8_t cx = 0, cy = 0;
    uint32_t resync0 = 0, resync1 = 0;
    bool applied = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanMap", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10", /*seed*/ 0x3A9Fu),
                  "harness start (map recovery) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    if (!find_changeable_cell(h.cs, CRATER, 0xFF, 0xFF, &cx, &cy)) {
        loopbackHarnessStop(&h);
        UT_FAIL("no changeable land cell found in the scanned span");
    }
    transportUdpClientTestMapState(ct, NULL, &resync0);

    /* Stage a real terrain change (server map mutated + map event held), then
     * let CHANNEL_MAP retransmit recover it under loss. */
    UT_ASSERT_MSG(transportUdpServerTestAddMapEvent(h.sim, slot, cx, cy, CRATER),
                  "map event injection rejected");

    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (clientSimGetMapTerrain(h.cs, cx, cy) == CRATER) {
            applied = true;
            break;
        }
    }

    fprintf(stderr, "  map-event recovery (impaired): converged after %d pump(s) "
                    "(cap %d) applied=%d\n", i, ROUNDTRIP_MAX, (int)applied);

    if (!applied) {
        loopbackHarnessStop(&h);
        UT_FAIL("map change never recovered+applied via channel 1 within %d pumps",
                ROUNDTRIP_MAX);
    }

    /* The robustness win: it recovered over the channel, no full resync. */
    transportUdpClientTestMapState(ct, NULL, &resync1);
    if (resync1 != resync0) {
        loopbackHarnessStop(&h);
        UT_FAIL("map change forced a resync (count %u -> %u)",
                (unsigned)resync0, (unsigned)resync1);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while recovering a map change");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 7: after a real resync installs a fresh map generation, an
 * older-generation map event on channel 1 is dropped while a current-generation
 * change is applied. */
static int run_map_event_generation_gate(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    uint8_t ax = 0, ay = 0, bx = 0, by = 0;
    uint8_t origA;
    uint32_t installedGen = 0, resyncCount = 0;
    bool installed = false;
    bool applied = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanGate", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=10", /*seed*/ 0x6B1Du),
                  "harness start (generation gate) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Drive a real resync end-to-end so the client installs generation >= 1. */
    UT_ASSERT_MSG(transportUdpClientTestBeginResync(ct), "begin resync rejected");
    for (i = 1; i <= CONNECT_MAX; i++) {
        loopbackHarnessPump(&h);
        transportUdpClientTestMapState(ct, &installedGen, &resyncCount);
        if (resyncCount >= 1) {
            installed = true;
            break;
        }
    }
    if (!installed) {
        loopbackHarnessStop(&h);
        UT_FAIL("resync never installed within %d pumps", CONNECT_MAX);
    }
    if (installedGen < 1) {
        loopbackHarnessStop(&h);
        UT_FAIL("installedMapGen did not advance (=%u)", (unsigned)installedGen);
    }

    /* Two distinct changeable cells: A for the stale event, B for the fresh. */
    if (!find_changeable_cell(h.cs, CRATER, 0xFF, 0xFF, &ax, &ay) ||
        !find_changeable_cell(h.cs, CRATER, ax, ay, &bx, &by)) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not find two changeable land cells");
    }
    origA = clientSimGetMapTerrain(h.cs, ax, ay);

    /* Stale: a map event tagged with the pre-resync generation 0, injected
     * straight onto channel 1. installedMapGen is now >= 1, so it must be
     * dropped — cell A stays unchanged. */
    {
        uint8_t payload[4 + GAME_EVENT_MAX_WIRE_SIZE];
        GameEvent ev;
        int evLen;
        memset(&ev, 0, sizeof(ev));
        ev.type = EVENT_MAP_CHANGE;
        ev.data[0] = ax;
        ev.data[1] = ay;
        ev.data[2] = CRATER;
        packU32(payload, 0u);                  /* stale generation */
        evLen = packGameEvent(payload + 4, &ev);
        UT_ASSERT_MSG(transportUdpServerChannelTestSend(slot, CHANNEL_MAP, payload,
                                                        (uint16_t)(4 + evLen)),
                      "stale map-channel send rejected");
    }

    /* Fresh: a real change tagged with the current generation via the
     * hold -> drain path. The channel delivers in order, so cell B becoming
     * CRATER proves the stale event ahead of it was delivered and dropped —
     * not merely still in flight. */
    UT_ASSERT_MSG(transportUdpServerTestAddMapEvent(h.sim, slot, bx, by, CRATER),
                  "fresh map event injection rejected");

    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (clientSimGetMapTerrain(h.cs, bx, by) == CRATER) {
            applied = true;
            break;
        }
    }

    fprintf(stderr, "  map generation gate: resyncCount=%u installedGen=%u "
                    "freshApplied=%d after %d pump(s)\n",
            (unsigned)resyncCount, (unsigned)installedGen, (int)applied, i);

    if (!applied) {
        loopbackHarnessStop(&h);
        UT_FAIL("fresh-generation map change never applied (cell B) within %d pumps",
                ROUNDTRIP_MAX);
    }
    /* The stale event was delivered before B (in-order channel) but dropped. */
    if (clientSimGetMapTerrain(h.cs, ax, ay) != origA) {
        loopbackHarnessStop(&h);
        UT_FAIL("stale-generation map event was applied (cell A changed from %u)",
                (unsigned)origA);
    }
    /* No extra resync was triggered by any of this. */
    {
        uint32_t resyncNow = 0;
        transportUdpClientTestMapState(ct, NULL, &resyncNow);
        if (resyncNow != resyncCount) {
            loopbackHarnessStop(&h);
            UT_FAIL("unexpected extra resync (count %u -> %u)",
                    (unsigned)resyncCount, (unsigned)resyncNow);
        }
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during the generation-gate exchange");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 8: previous-game straggler gate. A distinguishable game event is staged
 * on the slot's reliable game channel (channel 0) and left in flight under loss
 * across game start; after the lobby→running flip it must never reach the sim,
 * while a fresh post-flip game event does. The server drops the stale tail at
 * game start (channelResetSend) and sends a CTRL_CHANNEL_RESET; the client lifts
 * its game receive baseline, so the straggler dedup-drops.
 *
 * Observable: clientSimGetBrainEventCount — every game event the client applies
 * buffers a brain event (run_game_event_channel relies on the same path), and an
 * idle running tank produces no channel-0 traffic (run_empty_flow proves
 * expectedSeq stays 0 across a long idle window). So once the count is zeroed
 * after the flip has settled, the stale straggler — which the server no longer
 * sends and the client's lifted baseline would drop anyway — cannot move it,
 * while injecting a fresh event does. */
static int run_straggler_gate(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    int slot;
    int runningAt = -1;
    bool freshApplied = false;
    int i;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Straggler", /*lobbyMode*/ true,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0x57A661u),
                  "harness start (straggler gate) failed");

    /* Wait for both CONNECTED and the lobby phase — the phase arrives over
     * CHANNEL_CONTROL a round-trip after connect. */
    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_in_lobby, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached the lobby phase within %d pumps",
                CONNECT_MAX);
    }
    slot = (int)clientSimGetMyPlayerNum(h.cs);

    /* Stage a previous-game channel-0 event left in flight across game start.
     * EVENT_TANK_KILLED is a non-sound, non-culled game event that buffers a
     * brain event when applied. */
    {
        GameEvent stale;
        memset(&stale, 0, sizeof(stale));
        stale.type = EVENT_TANK_KILLED;
        stale.data[0] = 0xAA;  /* distinguishing marker */
        UT_ASSERT_MSG(transportUdpServerTestAddGameEvent(slot, &stale),
                      "stale game-event injection rejected");
    }

    /* Drive all-ready → countdown → running. */
    UT_ASSERT_MSG(loopbackHarnessTriggerGameStart(&h),
                  "trigger game start failed (no slot assigned?)");
    for (i = 1; i <= RUNNING_MAX; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (clientSimGetNetStatus(h.cs) == netRunning) {
            runningAt = i;
            break;
        }
    }
    if (runningAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached running phase within %d pumps", RUNNING_MAX);
    }

    /* Let any game-start transition events flush, then zero the observable. The
     * stale straggler can only have been delivered pre-flip (the server dropped
     * it at game start and never resends it), so anything before this point is
     * neutralised here. */
    for (i = 1; i <= FLIP_SETTLE; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
    }
    clientSimSetBrainEventCount(h.cs, 0);

    /* Stability window: the count must stay 0 — the stale straggler must never
     * reach the sim in the new game, and an idle tank generates no game events. */
    for (i = 1; i <= STRAGGLER_STAB; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
    }
    if (clientSimGetBrainEventCount(h.cs) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("a game event reached the sim after game start with none injected "
                "(brainEvents=%d) — previous-game straggler leaked",
                clientSimGetBrainEventCount(h.cs));
    }

    /* A fresh post-flip game event DOES apply — the gate drops only the stale
     * tail, not new-game traffic. */
    {
        GameEvent fresh;
        memset(&fresh, 0, sizeof(fresh));
        fresh.type = EVENT_TANK_KILLED;
        fresh.data[0] = 0xBB;
        UT_ASSERT_MSG(transportUdpServerTestAddGameEvent(slot, &fresh),
                      "fresh game-event injection rejected");
    }
    for (i = 1; i <= ROUNDTRIP_MAX; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (clientSimGetBrainEventCount(h.cs) > 0) {
            freshApplied = true;
            break;
        }
    }

    fprintf(stderr, "  straggler gate: running@%d freshApplied=%d after %d pump(s)\n",
            runningAt, (int)freshApplied, i);

    if (!freshApplied) {
        loopbackHarnessStop(&h);
        UT_FAIL("fresh post-flip game event never applied within %d pumps",
                ROUNDTRIP_MAX);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped during the straggler-gate exchange");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 9: a terrain burst larger than the map channel's send window is paced,
 * not fatal. PACE_BURST changes are staged for one slot in a single tick, so
 * the first snapshot drain fills CHANNEL_MAP's window and is refused partway
 * through. The server must leave the remainder in the hold buffer and resume
 * on later snapshots as acks free the window — never mark the slot for
 * removal. Every square must land on the client, the hold buffer must empty,
 * and no map resync may be involved: the changes have to arrive over the
 * channel, not be papered over by a fresh copy of the map. */
static bool collect_changeable_cells(ClientSim *cs, uint8_t target,
                                     uint8_t *xs, uint8_t *ys, int want) {
    int x, y, n = 0;
    for (y = 96; y < 160 && n < want; y++) {
        for (x = 96; x < 160 && n < want; x++) {
            uint8_t t = clientSimGetMapTerrain(cs, (uint8_t)x, (uint8_t)y);
            if (t == DEEP_SEA || t == RIVER || t == target) continue;
            /* Leave squares carrying a pill or a base alone — those objects
             * own their tile and the sim may write it back. */
            if (clientSimPillExistsAt(cs, (uint8_t)x, (uint8_t)y)) continue;
            if (clientSimBaseExistsAt(cs, (uint8_t)x, (uint8_t)y)) continue;
            xs[n] = (uint8_t)x;
            ys[n] = (uint8_t)y;
            n++;
        }
    }
    return n == want;
}

static int run_map_channel_pacing(void) {
    LoopbackHarness h;
    int connectedAt;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    static uint8_t xs[PACE_BURST];
    static uint8_t ys[PACE_BURST];
    static bool landed[PACE_BURST];
    int staged = 0;
    int delivered = 0;
    uint32_t resync0 = 0, resync1 = 0;
    int i, k;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ChanPace", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0x9ACE1u),
                  "harness start (map channel pacing) failed");

    connectedAt = loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL);
    if (connectedAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    /* The drain is held off until the join download is acked through, so wait
     * for the server's own view of it before staging anything. */
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX,
                                 pred_server_download_complete, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server never completed the join download within %d pumps",
                CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);
    transportUdpClientTestMapState(ct, NULL, &resync0);

    if (!collect_changeable_cells(h.cs, CRATER, xs, ys, PACE_BURST)) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not find %d changeable land squares in the scanned span",
                PACE_BURST);
    }
    memset(landed, 0, sizeof(landed));

    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d already flagged for removal before the burst", slot);
    }

    /* Stage the whole burst between ticks, so the next snapshot meets more
     * changes than one window can carry. */
    for (k = 0; k < PACE_BURST; k++) {
        if (!transportUdpServerTestAddMapEvent(h.sim, slot, xs[k], ys[k],
                                               CRATER)) {
            break;
        }
        staged++;
    }
    if (staged != PACE_BURST) {
        loopbackHarnessStop(&h);
        UT_FAIL("hold buffer accepted only %d of %d changes — the burst never "
                "exceeded the send window, so nothing was paced",
                staged, PACE_BURST);
    }

    /* Carry it. The slot must never be flagged for removal at any point. */
    for (i = 1; i <= PACE_DELIVER_MAX && delivered < PACE_BURST; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (transportUdpServerTestPendingRemove(slot)) {
            loopbackHarnessStop(&h);
            UT_FAIL("slot %d flagged for removal on a full map channel after "
                    "%d pump(s) — the burst was treated as fatal instead of "
                    "being held", slot, i);
        }
        for (k = 0; k < PACE_BURST; k++) {
            if (landed[k]) continue;
            if (clientSimGetMapTerrain(h.cs, xs[k], ys[k]) == CRATER) {
                landed[k] = true;
                delivered++;
            }
        }
    }

    fprintf(stderr, "  map channel pacing: %d/%d squares after %d pump(s) "
                    "(cap %d), held=%u\n",
            delivered, PACE_BURST, i, PACE_DELIVER_MAX,
            (unsigned)transportUdpServerTestMapQueueOutstanding(slot));

    if (delivered != PACE_BURST) {
        loopbackHarnessStop(&h);
        UT_FAIL("only %d of %d staged changes reached the client within %d "
                "pumps", delivered, PACE_BURST, PACE_DELIVER_MAX);
    }

    /* The hold buffer must have been handed over in full, not merely
     * partially drained with the rest abandoned. */
    if (transportUdpServerTestMapQueueOutstanding(slot) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("hold buffer still owes %u event(s) after the burst landed",
                (unsigned)transportUdpServerTestMapQueueOutstanding(slot));
    }

    /* It has to have arrived over the channel. A resync would have installed a
     * fresh copy of the map carrying every change, passing the checks above
     * without the pacing path doing any of the work. */
    transportUdpClientTestMapState(ct, NULL, &resync1);
    if (resync1 != resync0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the burst forced a map resync (count %u -> %u) instead of "
                "being paced over the channel",
                (unsigned)resync0, (unsigned)resync1);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while the burst was being paced");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 9b: a scenario's fill reaches a remote client whole. The op covers more
 * squares than one tick's tile budget, so the funnel paints what it can, keeps
 * the rest and answers SCN_OP_QUEUED; the sim drains the remainder on the next
 * frame. Both slices go out as ordinary map events, so what this case holds is
 * that the pacing inside the sim and the pacing on the map channel compose:
 * every square lands on the client, over the channel rather than through a
 * fresh copy of the map, and the burst is never treated as fatal.
 *
 * The op is issued from the scenario hook, which is where a scenario's writes
 * come from and the only place inside the frame where the map-change callback
 * is installed. */
#define SCN_FILL_W        20
#define SCN_FILL_H        15
#define SCN_FILL_N        (SCN_FILL_W * SCN_FILL_H)
#define SCN_FILL_DELIVER_MAX 3000  /* bounded convergence for the whole fill */

typedef struct {
    ServerSim *sim;
    uint8_t    x0, y0, x1, y1;
    int        calls;
    int        result;
} ScnFillHook;

static void scn_fill_hook(void *ctx) {
    ScnFillHook *hk = (ScnFillHook *)ctx;
    ScenarioOp op;
    hk->calls++;
    if (hk->calls != 1) return;
    memset(&op, 0, sizeof(op));
    op.type = SCN_OP_MAP_FILL_RECT;
    op.u.mapFillRect.x0 = hk->x0;
    op.u.mapFillRect.y0 = hk->y0;
    op.u.mapFillRect.x1 = hk->x1;
    op.u.mapFillRect.y1 = hk->y1;
    op.u.mapFillRect.terrain = CRATER;
    hk->result = (int)serverSimApplyScenarioOp(hk->sim, &op, NULL);
}

/* The top-left of a rectangle the fill will change every square of, and that
 * the client will be sent every square of.
 *
 * The second half is the one that is easy to miss. A wire client is sent only
 * the terrain changes inside its viewports; a change outside them is not
 * queued and is not written into the server's copy of that client's map
 * either, so it is not a desync and no resync ever repairs it — the square
 * simply never arrives. A rectangle picked off the far side of the map
 * therefore produces map events that are all correctly dropped. So every
 * square is tested against inAnyViewport, the same predicate
 * transportUdpServerDrainEvents culls on, rather than against a distance this
 * test would have to keep in step with the viewport constants.
 *
 * The rest is as before: no square already holding the target terrain, and
 * none a pill or a base owns and writes back. Terrain is read off the client's
 * copy, which is the map both ends agree on at this point. */
static bool scn_find_fill_rect(LoopbackHarness *h, uint8_t target,
                               uint8_t *ox, uint8_t *oy) {
    ViewportRect vps[MAX_VIEWPORTS];
    int nvp;
    int x, y, dx, dy;
    BYTE slot = clientSimGetMyPlayerNum(h->cs);

    if (slot >= MAX_TANKS) return false;
    nvp = serverSimBuildViewports(h->sim, slot, vps, MAX_VIEWPORTS);
    if (nvp <= 0) return false;

    for (y = MAP_MINE_EDGE_TOP + 1; y + SCN_FILL_H <= MAP_MINE_EDGE_BOTTOM; y++) {
        for (x = MAP_MINE_EDGE_LEFT + 1; x + SCN_FILL_W <= MAP_MINE_EDGE_RIGHT; x++) {
            bool clear = true;
            for (dy = 0; dy < SCN_FILL_H && clear; dy++) {
                for (dx = 0; dx < SCN_FILL_W && clear; dx++) {
                    uint8_t cx = (uint8_t)(x + dx);
                    uint8_t cy = (uint8_t)(y + dy);
                    if (clientSimGetMapTerrain(h->cs, cx, cy) == target ||
                        clientSimPillExistsAt(h->cs, cx, cy) ||
                        clientSimBaseExistsAt(h->cs, cx, cy) ||
                        !inAnyViewport(vps, nvp, (int)cx, (int)cy)) {
                        clear = false;
                    }
                }
            }
            if (clear) {
                *ox = (uint8_t)x;
                *oy = (uint8_t)y;
                return true;
            }
        }
    }
    return false;
}

static int run_scenario_fill_over_the_wire(void) {
    LoopbackHarness h;
    ScnFillHook hk;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    uint8_t rx = 0, ry = 0;
    uint32_t resync0 = 0, resync1 = 0;
    int i, dx, dy;
    int landed = 0;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "ScnFill", /*lobbyMode*/ false,
                                       /*impairSpec*/ "loss=5,burst=2",
                                       /*seed*/ 0x5CF111u),
                  "harness start (scenario fill) failed");

    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX,
                                 pred_server_download_complete, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server never completed the join download within %d pumps",
                CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);
    transportUdpClientTestMapState(ct, NULL, &resync0);

    if (!scn_find_fill_rect(&h, CRATER, &rx, &ry)) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not find a %dx%d rectangle inside slot %d's viewports "
                "that the fill would change every square of", SCN_FILL_W,
                SCN_FILL_H, slot);
    }
    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d already flagged for removal before the fill", slot);
    }

    /* One op, more squares than a tick's budget, issued the way a scenario
       issues one. */
    memset(&hk, 0, sizeof(hk));
    hk.sim = h.sim;
    hk.x0 = rx;
    hk.y0 = ry;
    hk.x1 = (uint8_t)(rx + SCN_FILL_W - 1);
    hk.y1 = (uint8_t)(ry + SCN_FILL_H - 1);
    hk.result = -1;
    serverSimSetScenarioTick(h.sim, scn_fill_hook, &hk);

    for (i = 1; i <= SCN_FILL_DELIVER_MAX && landed < SCN_FILL_N; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (transportUdpServerTestPendingRemove(slot)) {
            serverSimSetScenarioTick(h.sim, NULL, NULL);
            loopbackHarnessStop(&h);
            UT_FAIL("slot %d flagged for removal on a scenario fill after %d "
                    "pump(s) — the burst was treated as fatal instead of "
                    "being paced", slot, i);
        }
        landed = 0;
        for (dy = 0; dy < SCN_FILL_H; dy++) {
            for (dx = 0; dx < SCN_FILL_W; dx++) {
                if (clientSimGetMapTerrain(h.cs, (uint8_t)(rx + dx),
                                           (uint8_t)(ry + dy)) == CRATER) {
                    landed++;
                }
            }
        }
    }
    serverSimSetScenarioTick(h.sim, NULL, NULL);

    fprintf(stderr, "  scenario fill: %d/%d squares after %d pump(s) (cap %d), "
                    "held=%u\n",
            landed, SCN_FILL_N, i, SCN_FILL_DELIVER_MAX,
            (unsigned)transportUdpServerTestMapQueueOutstanding(slot));

    if (hk.calls == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the scenario hook never ran, so no op was issued");
    }
    if (hk.result != (int)SCN_OP_QUEUED) {
        loopbackHarnessStop(&h);
        UT_FAIL("a %d-square fill answered %d, wanted SCN_OP_QUEUED",
                SCN_FILL_N, hk.result);
    }
    if (landed != SCN_FILL_N) {
        loopbackHarnessStop(&h);
        UT_FAIL("only %d of %d filled squares reached the client within %d "
                "pumps", landed, SCN_FILL_N, SCN_FILL_DELIVER_MAX);
    }
    if (transportUdpServerTestMapQueueOutstanding(slot) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("hold buffer still owes %u event(s) after the fill landed",
                (unsigned)transportUdpServerTestMapQueueOutstanding(slot));
    }

    /* Over the channel, not through a fresh copy of the map. */
    transportUdpClientTestMapState(ct, NULL, &resync1);
    if (resync1 != resync0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the fill forced a map resync (count %u -> %u) instead of "
                "arriving over the channel",
                (unsigned)resync0, (unsigned)resync1);
    }
    if (clientSimGetConnectState(h.cs) != CLIENT_CONNECT_CONNECTED) {
        loopbackHarnessStop(&h);
        UT_FAIL("client dropped while the fill was being paced");
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* Case 10: the map-event ack an InputPacket carries cannot move the server's
 * hold-buffer cursor. The field is left over from when map events rode the
 * snapshot tail; they ride CHANNEL_MAP now, with the channel's own acks, and
 * the cursor means "handed to the channel", which only the drain knows. A
 * real client sends 1 for the life of the connection, so the seam below is
 * the only way any other value reaches the wire. The claim is placed far past
 * the queue head: a server that honours it puts the cursor beyond the head,
 * the space check's subtraction wraps, and the buffer refuses every change
 * from then on — so the assertion is simply that one change staged after the
 * claim is accepted, and lands. */
#define WIRE_ACK_SETTLE       20   /* pumps for the claiming input to be consumed */
#define WIRE_ACK_DELIVER_MAX 500   /* bounded delivery of the change that follows */

static int run_map_ack_from_wire_ignored(void) {
    LoopbackHarness h;
    uint32_t inputTick = 1;
    Transport *ct;
    int slot;
    uint8_t x = 0, y = 0;
    uint32_t resync0 = 0, resync1 = 0;
    int i;
    bool landed = false;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "WireAck", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 0x0ACC1u),
                  "harness start (map ack from wire) failed");

    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX, pred_connected, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client never reached CONNECTED within %d pumps", CONNECT_MAX);
    }
    if (loopbackHarnessPumpUntil(&h, CONNECT_MAX,
                                 pred_server_download_complete, NULL) < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("server never completed the join download within %d pumps",
                CONNECT_MAX);
    }

    ct   = &h.cs->transport;
    slot = (int)clientSimGetMyPlayerNum(h.cs);
    transportUdpClientTestMapState(ct, NULL, &resync0);

    if (!collect_changeable_cells(h.cs, CRATER, &x, &y, 1)) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not find a changeable land square in the scanned span");
    }

    /* Put the claim on the wire on real inputs, and let the server consume
     * them. */
    transportUdpClientTestSetMapEventAck(ct, 0xFFFFFFF0u);
    for (i = 0; i < WIRE_ACK_SETTLE; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
    }

    /* Nothing has been staged, so nothing is owed. A cursor pushed past the
     * head reads as an enormous outstanding count once the subtraction
     * wraps. */
    if (transportUdpServerTestMapQueueOutstanding(slot) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the wire ack moved slot %d's hold-buffer cursor: %u event(s) "
                "outstanding on a queue nothing was staged on",
                slot, (unsigned)transportUdpServerTestMapQueueOutstanding(slot));
    }

    /* One change after the claim. The buffer must take it. */
    if (!transportUdpServerTestAddMapEvent(h.sim, slot, x, y, CRATER)) {
        loopbackHarnessStop(&h);
        UT_FAIL("hold buffer refused a single change for slot %d after the "
                "wire ack — the space check wrapped, so every later change "
                "for this slot would be dropped", slot);
    }

    /* And it must land over the channel, with the inputs still carrying the
     * claim the whole way. */
    for (i = 1; i <= WIRE_ACK_DELIVER_MAX && !landed; i++) {
        inputTick = feed_input(&h, inputTick);
        loopbackHarnessPump(&h);
        if (clientSimGetMapTerrain(h.cs, x, y) == CRATER) landed = true;
    }

    fprintf(stderr, "  map ack from wire: change landed=%d after %d pump(s) "
                    "(cap %d), held=%u\n",
            landed ? 1 : 0, i, WIRE_ACK_DELIVER_MAX,
            (unsigned)transportUdpServerTestMapQueueOutstanding(slot));

    if (!landed) {
        loopbackHarnessStop(&h);
        UT_FAIL("the change staged after the wire ack never reached the "
                "client within %d pumps", WIRE_ACK_DELIVER_MAX);
    }
    if (transportUdpServerTestMapQueueOutstanding(slot) != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("hold buffer still owes %u event(s) after the change landed",
                (unsigned)transportUdpServerTestMapQueueOutstanding(slot));
    }
    transportUdpClientTestMapState(ct, NULL, &resync1);
    if (resync1 != resync0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the change arrived by a map resync (count %u -> %u), not "
                "over the channel", (unsigned)resync0, (unsigned)resync1);
    }
    if (transportUdpServerTestPendingRemove(slot)) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %d was flagged for removal", slot);
    }

    loopbackHarnessStop(&h);
    return 0;
}

int run_loopback_channel(void) {
    int rc = run_empty_flow();
    if (rc != 0) return rc;
    rc = run_roundtrip_impaired();
    if (rc != 0) return rc;
    rc = run_game_event_channel();
    if (rc != 0) return rc;
    rc = run_game_event_splice();
    if (rc != 0) return rc;
    rc = run_game_channel_overflow();
    if (rc != 0) return rc;
    rc = run_map_event_channel_recovery();
    if (rc != 0) return rc;
    rc = run_map_event_generation_gate();
    if (rc != 0) return rc;
    rc = run_straggler_gate();
    if (rc != 0) return rc;
    rc = run_map_channel_pacing();
    if (rc != 0) return rc;
    rc = run_scenario_fill_over_the_wire();
    if (rc != 0) return rc;
    return run_map_ack_from_wire_ignored();
}
