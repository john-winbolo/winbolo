/*
 * Best-effort traffic survives the tick it was raised on.
 *
 * A running tick used to put exactly one channel frame on the wire per client:
 * the trailer riding the snapshot datagram. Reliable channels were never lost
 * to that — they wait, and a full window is backpressure — but the effect ring
 * is 64 deep and the voice ring 8, and neither waits: the next send over a full
 * ring drops the oldest entry. Everything a busy tick raised past the trailer's
 * share was thrown away on the tick that raised it. serverSendSnapshot now
 * emits up to SNAPSHOT_EXTRA_CHANNEL_FRAMES standalone frames after the
 * snapshot, and the two cases here are what prove the data arrives and is
 * applied, over the real transport, at a real client.
 *
 * send_drains_channels_multi_frame already counts the datagrams that one send
 * produces. These two go past the wire to the client's own state.
 *
 * ── run_best_effort_not_dropped ──────────────────────────────────────────
 * One client, clean path, running with the map installed. On one tick the test
 * queues BE_FILLER reliable game events, BE_EFFECTS best-effort effect events
 * and one voice frame, then lets the tick carry them.
 *
 * The filler is the point of the arithmetic. Every segment here is an
 * EVENT_PING: 7 bytes of event behind CHANNEL_SEG_HEADER_SIZE (7), so 14 bytes
 * on the wire. The snapshot trailer gets UDP_MAX_PAYLOAD minus the snapshot, so
 * its absolute ceiling — with nothing in the snapshot but its own header and
 * one tank entry — is (1400 - 8 - 17 - 2) / 14 = 98 segments, and against the
 * snapshot this fixture actually builds (one tank, no shells, Everard's bases
 * and pills) it is about 82. The effect ring is 64 deep, so the largest burst
 * that can be queued without the ring dropping its own oldest entry is 64 —
 * fewer than one frame holds. Without the filler the whole burst would ride the
 * trailer, the extra frames would carry nothing, and the case would pass with
 * the send loop reverted. BE_FILLER is 112, above the 98 ceiling, so the
 * trailer fills with reliable traffic (reliable channels are framed ahead of
 * best-effort in channelBuildFrame) and the effect burst has to ride an extra
 * frame.
 *
 * What that costs against the tick's budget: 112 + 48 + 1 = 161 segments,
 * 112*14 + 48*14 + 18 = 2482 bytes. One extra frame holds
 * (1400 - 8 - 2 - 36 acks) / 14 = 96 segments, so the whole burst needs the
 * trailer plus two extra frames at worst, against the four the tick has
 * (trailer + SNAPSHOT_EXTRA_CHANNEL_FRAMES). Nothing here sits near a frame
 * boundary.
 *
 * BE_EFFECTS is 48 rather than the ring's full 64 on purpose: the sim raises
 * its own effect events (base stock, explosions) during the carrying tick,
 * after the burst is queued, and a burst sized to the ring exactly would have
 * its oldest entry dropped by the first one of those.
 *
 * ── run_effect_burst_not_starved_by_reliable ─────────────────────────────
 * The same tick raises more reliable game events than one snapshot drain
 * takes, plus a burst of effects, and the tick after it raises a second burst.
 * The two channels used to share one drain cap, so the effects of a busy tick
 * were never reached and the second burst evicted them from the client's
 * receive ring. Both bursts have to be applied in full.
 *
 * ── run_quit_burst_all_delivered ─────────────────────────────────────────
 * Two clients. Client 2 holds every pillbox and base on the map and quits. Its
 * departure migrates all of them at once, which is the ownership burst the
 * original reports came from, and the case asserts client 1 gets every one of
 * them and the leave within two pumps of the tick that published them.
 *
 * That burst is mixed, and the two assertions cover different halves of it:
 * EVENT_BASE_UPDATE is reliable (CHANNEL_GAME) and EVENT_PILL_UPDATE is
 * best-effort (CHANNEL_GAME_EFFECT), so the channelGetBestEffortStats check
 * speaks for the pills only. The base half is proved by the per-event delivery
 * check, which is what makes that check the load-bearing one for both.
 *
 * ── How arrival is read ──────────────────────────────────────────────────
 * Off the client's brain-event buffer. clientSimApplyGameEvents buffers
 * EVENT_PING, EVENT_PILL_UPDATE and EVENT_BASE_UPDATE for every recipient
 * whatever the client type (client_snapshot.c), and nothing on this path drains
 * that buffer, so it is the record of what was applied. Every event is tagged
 * so it can be named individually — a count would pass on a burst that
 * delivered one event sixty times.
 *
 * ── How many pumps, and why the two cases differ ─────────────────────────
 * A pump is one client tick then one server tick, and the client's recv loop
 * takes the whole socket in one client tick, in send order.
 *
 * So for the tick that sends, pump P: on pump P+1 the client reads the snapshot
 * datagram first and runs its drain, which sees the mux as it stands at that
 * moment — the trailer, and nothing else, because the standalone frames are
 * later in the same socket read and have not been ingested yet. It reads them
 * next, and while running they ingest and drain nothing: the snapshot drain
 * owns the order the game, effect and map channels apply in
 * (transport_udp_client.c, the PACKET_CHANNEL case). What they carried
 * therefore waits for the next snapshot's drain, on pump P+2.
 *
 * That is structural, not a timing accident: anything that rides an extra frame
 * cannot be applied within one drain of its sending tick, so a two-pump bound
 * is unreachable for it however the harness is paced. The recv path can slip a
 * pump on top of that, which is why the bound below is a convergence bound —
 * pump until everything is in or it runs out — rather than a count.
 *
 * quit_burst_all_delivered is not subject to this. Its burst is about 33
 * segments, which the trailer carries whole, so its own tick's snapshot drain
 * applies it and two pumps is the right bound there.
 *
 * The client's per-snapshot drain is capped at MAX_SNAPSHOT_EVENTS (128) across
 * the reliable and best-effort channels together, reliable first. Drain one
 * takes the trailer's share of the filler (~96 here) and no effects, since none
 * have been ingested; drain two takes the filler the trailer could not hold
 * (~16) plus the whole 48-event burst, ~64 against the 128. The cap only starts
 * to bite if the trailer's share falls to about 32, which needs a snapshot four
 * times the size of this fixture's.
 *
 * ── Voice ────────────────────────────────────────────────────────────────
 * The frame is framed with voiceSegmentPackDown and queued straight onto the
 * slot's send-side voice channel, which is where serverPumpVoice would leave a
 * forwarded frame. Going through a second client's microphone would add the
 * talker-selection and mute policy to what this case is measuring.
 *
 * Clean path, no impairment and no virtual clock: nothing here measures a round
 * trip. Server state is read off udpServer and the ServerSim struct (the
 * unittests profile permits internal access).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"                        /* MAX_TANKS, BYTE, NEUTRAL */
#include "server_sim.h"
#include "server_sim_internal.h"           /* ServerSim::sim, for the pill/base setup */
#include "server_sim_lifecycle.h"          /* serverSimEnterGameOver */
#include "client_sim.h"
#include "client_net.h"                    /* clientSimNetReceiveVoice */
#include "client_connect_state.h"
#include "client_enums.h"                  /* netStatus */
#include "game_sim.h"
#include "players.h"                       /* playersIsInUse */
#include "pillbox.h"                       /* pillsGetNumPills, pillsSetPillOwner */
#include "bases.h"                         /* basesGetNumBases, basesSetBaseOwner */
#include "input_packet.h"                  /* GameEvent, EVENT_PING, EVENT_PILL_UPDATE,
                                            * EVENT_BASE_UPDATE, PING_KIND_STANDARD */
#include "transport_udp.h"                 /* the test hooks */
#include "transport_udp_server_internal.h" /* udpServer.channelMux */
#include "channel_mux.h"                   /* channelGetBestEffortStats,
                                            * channelSendBestEffort, CHANNEL_* */
#include "voice_segment.h"                 /* voiceSegmentPackDown */
#include "threads.h"                       /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

/* Convergence ceilings on a clean path. Bounds to report a failure against,
 * not expected counts. The other clean-path loopback cases budget the same. */
#define BE_CONNECT_MAX  4000
#define BE_READY_MAX    4000
#define BE_TANK_MAX     600

/* Pumps run before the measured tick so the reliable game channel is quiescent:
 * an unacked tail past its retransmit timeout would put a resend in the same
 * frames and muddy what the burst costs. */
#define BE_SETTLE       200

/* Pumps the quit case gives the client after the publishing tick. Its burst
 * rides the snapshot trailer, so its own tick's drain applies it. */
#define BE_DELIVER      2

/* Pumps the burst case gives the client after the sending tick. Two of them are
 * structural — one to ingest the extra frames, one whose snapshot drains what
 * they carried — and the rest is slack for the recv path slipping a pump under
 * load, and for a drain that hits MAX_SNAPSHOT_EVENTS and leaves a remainder
 * for the next one. A convergence bound: the loop stops the moment everything
 * is in and reports the pump it stopped at, so drift shows up in the log long
 * before it reaches the bound. It is deliberately not large enough to hide a
 * send loop that dribbles the burst out over later ticks — and the assertions
 * that rule that out are made on the sending tick itself, before this loop
 * runs. */
#define BE_DELIVER_MAX  60

/* Reliable game events queued to fill the snapshot trailer, so the effect burst
 * cannot ride it. Must stay above the trailer's 98-segment ceiling. */
#define BE_FILLER       112

/* Effect events in the burst. Below CHANNEL_GAME_EFFECT_WINDOW (64) with room
 * for what the sim raises on the same tick. */
#define BE_EFFECTS      48

/* The fixed pattern in a marker ping's data[4], and the two data[5] values that
 * separate the reliable filler from the best-effort burst. A real EVENT_PING
 * the sim happened to raise carries neither. */
#define BE_MARK         0xA5u
#define BE_MARK_FILLER  0x01u
#define BE_MARK_EFFECT  0x02u
#define BE_MARK_EFFECT2 0x03u
#define BE_MARK_POSTGAME 0x04u

/* ── post_game_segment_applied ───────────────────────────────────────────
 * Pumps run after game over before the segment is queued, so the client
 * has gone without a snapshot for longer than the drain waits before it
 * hands the order back (SNAPSHOT_ORDER_IDLE_TICKS, 20 local ticks, one per
 * pump). */
#define BE_POSTGAME_QUIET 30

/* Pumps the segment then gets. Far below the game-over hold
 * (GAMEOVER_HOLD_TICKS, 150 server ticks), so a pass cannot come from the
 * lobby phase event arriving and flipping the gate that way. */
#define BE_POSTGAME_MAX   15

/* ── effect_burst_not_starved_by_reliable ────────────────────────────────
 * Reliable game events queued on the measured tick. The client's snapshot
 * drain takes at most MAX_SNAPSHOT_EVENTS (128) reliable events per
 * snapshot, and this is well past that, so a drain that shared one cap
 * across both channels had nothing left for the effect channel. The server
 * frames all of these across the tick's four frames (about 96 each), so
 * they are all at the client by the drain that follows. */
#define BE_CAP_FILLER   240

/* Effect events in each of the two bursts. The client's best-effort receive
 * ring is CHANNEL_GAME_EFFECT_WINDOW (64) deep and evicts its oldest entry
 * rather than waiting, so a first burst left undrained is what the second
 * one pushes out. */
#define BE_CAP_EFFECTS  48

/* The injected voice frame: a payload short enough to leave the frame
 * arithmetic above uncluttered, and a sequence number no real frame would carry
 * here (nothing else produces voice in this fixture). */
#define BE_VOICE_SEQ    0x5Au
#define BE_VOICE_LEN    8
#define BE_VOICE_BYTE(i) ((uint8_t)(0xC0u + (i)))

/* Nothing on a clean path draws from the impairment stream; the seeds keep the
 * runs reproducible regardless. */
#define BE_SEED         0xB357Eu
#define QB_SEED         0x9017Bu

/* Pumps allowed for the quitting client's departure to reach the server, read
 * off the server's own migration of a base client 2 owned. */
#define QB_QUIT_MAX     600

/* ── Shared helpers ─────────────────────────────────────────────────────── */

/* One marker ping: data[2..3] is the index, data[4..5] the pattern. data[0] is
 * the recipient's own slot, so the client's ping-flood backstop takes its
 * "always see your own" path; the brain buffering the case reads happens ahead
 * of that either way. */
static void beMakePing(GameEvent *ev, BYTE slot, int idx, uint8_t which) {
    memset(ev, 0, sizeof(*ev));
    ev->type    = EVENT_PING;
    ev->data[0] = (uint8_t)slot;
    ev->data[1] = PING_KIND_STANDARD;
    ev->data[2] = (uint8_t)(idx >> 8);
    ev->data[3] = (uint8_t)(idx & 0xff);
    ev->data[4] = BE_MARK;
    ev->data[5] = which;
}

/* Mark every marker ping of one pattern the client has applied. */
static void beCollectPings(ClientSim *cs, uint8_t which, bool *seen, int cap) {
    const GameEvent *ev = clientSimGetBrainEvents(cs);
    int n = clientSimGetBrainEventCount(cs);
    int i;

    if (ev == NULL) return;
    for (i = 0; i < n; i++) {
        int idx;
        if (ev[i].type != EVENT_PING) continue;
        if (ev[i].data[4] != BE_MARK || ev[i].data[5] != which) continue;
        idx = ((int)ev[i].data[2] << 8) | (int)ev[i].data[3];
        if (idx >= 0 && idx < cap) seen[idx] = true;
    }
}

/* Index of the first entry not set, or -1 when they all are. */
static int beFirstMissing(const bool *seen, int cap) {
    int i;
    for (i = 0; i < cap; i++) {
        if (!seen[i]) return i;
    }
    return -1;
}

static int beCountMissing(const bool *seen, int cap) {
    int i, n = 0;
    for (i = 0; i < cap; i++) {
        if (!seen[i]) n++;
    }
    return n;
}

/* Has the client applied an event of this type naming `idx` in data[0] and
 * carrying `owner` in data[ownerAt]? */
static bool beSawOwnerEvent(ClientSim *cs, uint8_t type, uint8_t idx,
                            int ownerAt, BYTE owner) {
    const GameEvent *ev = clientSimGetBrainEvents(cs);
    int n = clientSimGetBrainEventCount(cs);
    int i;

    if (ev == NULL) return false;
    for (i = 0; i < n; i++) {
        if (ev[i].type != type) continue;
        if (ev[i].data[0] != idx) continue;
        if (ev[i].data[ownerAt] == owner) return true;
    }
    return false;
}

/* ── Predicates ─────────────────────────────────────────────────────────── */

static bool beConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool beBothConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs)  == CLIENT_CONNECT_CONNECTED &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

/* The server flips download-complete only once the map's bytes are acked back
 * on CHANNEL_BULK, and snapshots are gated until then — so the extra-frame loop
 * this case measures does not run before it. */
static bool beServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
        (int)clientSimGetMyPlayerNum(h->cs));
}

static bool beBothServerReady(LoopbackHarness *h, void *user) {
    (void)user;
    return transportUdpServerTestDownloadComplete(
               (int)clientSimGetMyPlayerNum(h->cs)) &&
           transportUdpServerTestDownloadComplete(
               (int)clientSimGetMyPlayerNum(h->cs2));
}

/* The server is running the round. */
static bool beRunning(LoopbackHarness *h, void *user) {
    (void)user;
    return serverSimGetState(h->sim) == serverStateRunning;
}

/* And the client has been told: CTRL_GAME_PHASE_RUNNING clears inLobby,
 * which is one of the three tests the snapshot-order gate makes. */
static bool beClientInGame(LoopbackHarness *h, void *user) {
    (void)user;
    return !clientSimIsInLobby(h->cs);
}

static bool beHaveTank(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs, &mx, &my) == TRUE;
}

static bool beBothHaveTanks(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs,  &mx, &my) == TRUE &&
           clientSimGetMyTankMapPos(h->cs2, &mx, &my) == TRUE;
}

/* ── best_effort_not_dropped ────────────────────────────────────────────── */

int run_best_effort_not_dropped(void) {
    LoopbackHarness h;
    bool fillerSeen[BE_FILLER];
    bool effectSeen[BE_EFFECTS];
    uint8_t voiceSeg[CHANNEL_VOICE_SEG];
    uint8_t voiceOpus[BE_VOICE_LEN];
    uint8_t voiceOut[CHANNEL_VOICE_SEG];
    uint32_t fxSent0 = 0, fxDrop0 = 0, fxSkip0 = 0;
    uint32_t vxSent0 = 0, vxDrop0 = 0, vxSkip0 = 0;
    uint32_t fxSent = 0, fxDrop = 0, fxSkip = 0;
    uint32_t vxSent = 0, vxDrop = 0, vxSkip = 0;
    BYTE slot;
    int voiceLen, i, at;
    int voiceFrames = 0, voiceBad = 0;
    int deliveredAt = -1;
    bool voiceGood = false;

    memset(fillerSeen, 0, sizeof(fillerSeen));
    memset(effectSeen, 0, sizeof(effectSeen));
    memset(&h, 0, sizeof(h));

    if (!loopbackHarnessStart(&h, "BurstHost", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, BE_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (best effort not dropped) failed");
    }

    at = loopbackHarnessPumpUntil(&h, BE_CONNECT_MAX, beConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps (state=%d)",
                BE_CONNECT_MAX, (int)clientSimGetConnectState(h.cs));
    }
    at = loopbackHarnessPumpUntil(&h, BE_READY_MAX, beServerReady, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client's map download never completed within %d pumps — "
                "no snapshot flows to it, so no tick carries the burst",
                BE_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, BE_TANK_MAX, beHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", BE_TANK_MAX);
    }

    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client holds no slot, so there is no mux to queue onto");
    }

    loopbackHarnessPumpUntil(&h, BE_SETTLE, NULL, NULL);
    clientSimSetBrainEventCount(h.cs, 0);

    /* Baselines. The skip count is asserted absolutely below — nothing charges
     * it before the first snapshot goes out, and a quiet running tick carries
     * everything it raises — but the ring-drop count is read as a delta: the
     * map-download window has no snapshot to carry effects, so events raised
     * while it ran can overflow the ring before the measured tick, which is not
     * what this case is about. */
    channelGetBestEffortStats(&udpServer.channelMux[slot], CHANNEL_GAME_EFFECT,
                              &fxSent0, &fxDrop0, &fxSkip0);
    channelGetBestEffortStats(&udpServer.channelMux[slot], CHANNEL_VOICE,
                              &vxSent0, &vxDrop0, &vxSkip0);
    if (fxSkip0 != 0 || vxSkip0 != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the fixture arrived with best-effort traffic already left "
                "behind (effect %u, voice %u) before the burst was queued — "
                "some tick between the join and here could not carry what it "
                "raised, so the assertion after the burst would be reading "
                "that rather than the burst",
                (unsigned)fxSkip0, (unsigned)vxSkip0);
    }

    /* ---- The burst, all on one tick. ---- */
    for (i = 0; i < BE_VOICE_LEN; i++) {
        voiceOpus[i] = BE_VOICE_BYTE(i);
    }
    voiceLen = voiceSegmentPackDown(voiceSeg, (int)sizeof(voiceSeg),
                                    (uint8_t)slot, BE_VOICE_SEQ, /*flags*/ 0,
                                    voiceOpus, BE_VOICE_LEN);
    if (voiceLen <= 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the voice frame would not pack into a %d-byte segment",
                (int)sizeof(voiceSeg));
    }

    threadsWaitForMutex();
    for (i = 0; i < BE_FILLER; i++) {
        GameEvent ev;
        beMakePing(&ev, slot, i, BE_MARK_FILLER);
        if (!transportUdpServerTestAddGameEvent((int)slot, &ev)) {
            threadsReleaseMutex();
            loopbackHarnessStop(&h);
            UT_FAIL("the reliable game channel refused filler event %d of %d — "
                    "the window is %d deep, so the fixture is wrong, not the "
                    "server", i, BE_FILLER, CHANNEL_GAME_WINDOW);
        }
    }
    for (i = 0; i < BE_EFFECTS; i++) {
        GameEvent ev;
        beMakePing(&ev, slot, i, BE_MARK_EFFECT);
        if (!transportUdpServerTestAddEffectEvent((int)slot, &ev)) {
            threadsReleaseMutex();
            loopbackHarnessStop(&h);
            UT_FAIL("the effect channel refused event %d of %d — a best-effort "
                    "enqueue only reports a usage error, so the event or the "
                    "slot is wrong", i, BE_EFFECTS);
        }
    }
    if (!channelSendBestEffort(&udpServer.channelMux[slot], CHANNEL_VOICE,
                               voiceSeg, (uint16_t)voiceLen)) {
        threadsReleaseMutex();
        loopbackHarnessStop(&h);
        UT_FAIL("the voice channel refused the %d-byte frame", voiceLen);
    }
    threadsReleaseMutex();

    /* The tick that carries it. */
    loopbackHarnessPump(&h);

    channelGetBestEffortStats(&udpServer.channelMux[slot], CHANNEL_GAME_EFFECT,
                              &fxSent, &fxDrop, &fxSkip);
    channelGetBestEffortStats(&udpServer.channelMux[slot], CHANNEL_VOICE,
                              &vxSent, &vxDrop, &vxSkip);
    fprintf(stderr, "  best effort burst: slot %u, %d filler + %d effect + 1 "
                    "voice; effect sent %u->%u drops %u->%u skips %u->%u; "
                    "voice sent %u->%u drops %u->%u skips %u->%u\n",
            (unsigned)slot, BE_FILLER, BE_EFFECTS,
            (unsigned)fxSent0, (unsigned)fxSent, (unsigned)fxDrop0,
            (unsigned)fxDrop, (unsigned)fxSkip0, (unsigned)fxSkip,
            (unsigned)vxSent0, (unsigned)vxSent, (unsigned)vxDrop0,
            (unsigned)vxDrop, (unsigned)vxSkip0, (unsigned)vxSkip);

    if (fxSkip != 0 || vxSkip != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the tick that carried the burst left best-effort traffic "
                "behind: effect skips %u, voice skips %u (both were %u / %u "
                "before it). A burst sized to fit the tick's frames leaves "
                "nothing pending once the last one is built",
                (unsigned)fxSkip, (unsigned)vxSkip, (unsigned)fxSkip0,
                (unsigned)vxSkip0);
    }
    if (fxDrop != fxDrop0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the burst cost %u effect ring drop(s) — %d events went into a "
                "%d-deep ring, so either the fixture is too big for the ring or "
                "CHANNEL_GAME_EFFECT_WINDOW is too small for one tick's burst",
                (unsigned)(fxDrop - fxDrop0), BE_EFFECTS,
                CHANNEL_GAME_EFFECT_WINDOW);
    }
    if (vxDrop != vxDrop0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the burst cost %u voice ring drop(s)",
                (unsigned)(vxDrop - vxDrop0));
    }
    if (fxSent - fxSent0 < (uint32_t)BE_EFFECTS) {
        loopbackHarnessStop(&h);
        UT_FAIL("only %u of the %d effect event(s) were framed onto the wire by "
                "the tick that raised them",
                (unsigned)(fxSent - fxSent0), BE_EFFECTS);
    }
    if (vxSent == vxSent0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the voice frame was never framed onto the wire");
    }

    /* Pump until it has all arrived. One pump ingests the extra frames, the
     * next one's snapshot drains them; the rest of the bound is slack. */
    for (i = 1; i <= BE_DELIVER_MAX; i++) {
        uint8_t from = 0, seq = 0, flags = 0;
        int n;
        loopbackHarnessPump(&h);
        while ((n = clientSimNetReceiveVoice(h.cs, &from, &seq, &flags,
                                             voiceOut,
                                             (int)sizeof(voiceOut))) > 0) {
            voiceFrames++;
            if (n == BE_VOICE_LEN && seq == BE_VOICE_SEQ &&
                from == (uint8_t)slot &&
                memcmp(voiceOut, voiceOpus, BE_VOICE_LEN) == 0) {
                voiceGood = true;
            } else {
                voiceBad++;
            }
        }
        beCollectPings(h.cs, BE_MARK_FILLER, fillerSeen, BE_FILLER);
        beCollectPings(h.cs, BE_MARK_EFFECT, effectSeen, BE_EFFECTS);
        /* The seen arrays are the record now, so the buffer can go. It holds
         * MAX_BRAIN_EVENTS and drops what arrives once full, and nothing on
         * this path drains it — over a bound this wide the sim's own base-stock
         * and pill traffic would otherwise fill it and a marker still in flight
         * would be discarded on arrival. */
        clientSimSetBrainEventCount(h.cs, 0);
        if (voiceGood && beCountMissing(fillerSeen, BE_FILLER) == 0 &&
            beCountMissing(effectSeen, BE_EFFECTS) == 0) {
            deliveredAt = i;
            break;
        }
    }

    fprintf(stderr, "  best effort burst: client applied %d/%d filler, %d/%d "
                    "effect, %d voice frame(s) after %d pump(s) of %d\n",
            BE_FILLER - beCountMissing(fillerSeen, BE_FILLER), BE_FILLER,
            BE_EFFECTS - beCountMissing(effectSeen, BE_EFFECTS), BE_EFFECTS,
            voiceFrames, deliveredAt < 0 ? BE_DELIVER_MAX : deliveredAt,
            BE_DELIVER_MAX);

    if (beCountMissing(fillerSeen, BE_FILLER) != 0) {
        int miss = beCountMissing(fillerSeen, BE_FILLER);
        int first = beFirstMissing(fillerSeen, BE_FILLER);
        loopbackHarnessStop(&h);
        UT_FAIL("%d of the %d reliable filler event(s) had not reached the "
                "client %d pump(s) after the sending tick (first missing index "
                "%d) — the filler is the premise of this case, not its subject: "
                "if it did not all arrive, the trailer was not full and the "
                "effect burst was never pushed onto an extra frame",
                miss, BE_FILLER, BE_DELIVER_MAX, first);
    }
    if (beCountMissing(effectSeen, BE_EFFECTS) != 0) {
        int miss = beCountMissing(effectSeen, BE_EFFECTS);
        int first = beFirstMissing(effectSeen, BE_EFFECTS);
        loopbackHarnessStop(&h);
        UT_FAIL("%d of the %d effect event(s) had not reached the client %d "
                "pump(s) after the sending tick (first missing index %d) — the "
                "burst was raised on one tick and the extra frames after that "
                "tick's snapshot are what carry what the trailer had no room "
                "for", miss, BE_EFFECTS, BE_DELIVER_MAX, first);
    }
    if (!voiceGood) {
        loopbackHarnessStop(&h);
        UT_FAIL("the voice frame did not reach the client intact within %d "
                "pump(s): %d frame(s) arrived, %d of them not the one that was "
                "queued", BE_DELIVER_MAX, voiceFrames, voiceBad);
    }

    /* Every delivery pump built another tick's worth of frames; none of them
     * may have left anything behind either. */
    channelGetBestEffortStats(&udpServer.channelMux[slot], CHANNEL_GAME_EFFECT,
                              &fxSent, &fxDrop, &fxSkip);
    channelGetBestEffortStats(&udpServer.channelMux[slot], CHANNEL_VOICE,
                              &vxSent, &vxDrop, &vxSkip);
    if (fxSkip != 0 || vxSkip != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("best-effort traffic was left behind after the burst was "
                "delivered: effect skips %u, voice skips %u",
                (unsigned)fxSkip, (unsigned)vxSkip);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ── effect_burst_not_starved_by_reliable ───────────────────────────────
 *
 * The snapshot drain used to take the reliable game channel and the
 * best-effort effect channel into one array with one cap of
 * MAX_SNAPSHOT_EVENTS (128). A tick that sends more reliable events than
 * that - which the multi-frame send loop made possible - filled the array
 * before the effect channel was reached, and nothing holds a best-effort
 * segment for the next snapshot: the client's 64-deep receive ring drops
 * its oldest entry when the next tick's burst arrives over it. The effects
 * of a busy tick were silently lost at the client, having survived the
 * whole way across the wire.
 *
 * The effect channel is drained separately now, after the snapshot is
 * applied, with the ring as its only bound.
 *
 * The fixture: one tick queues BE_CAP_FILLER reliable events and a first
 * burst of BE_CAP_EFFECTS effects, the next tick queues a second burst of
 * the same size, and every event of both bursts has to reach the client.
 * Before the split the first burst is the one that loses events, because
 * the second burst is what evicts it.
 */
int run_effect_burst_not_starved_by_reliable(void) {
    LoopbackHarness h;
    bool firstSeen[BE_CAP_EFFECTS];
    bool secondSeen[BE_CAP_EFFECTS];
    BYTE slot;
    int at, i;
    int deliveredAt = -1;

    memset(firstSeen, 0, sizeof(firstSeen));
    memset(secondSeen, 0, sizeof(secondSeen));
    memset(&h, 0, sizeof(h));

    if (!loopbackHarnessStart(&h, "EffectCapHost", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, BE_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (effect burst not starved) failed");
    }

    at = loopbackHarnessPumpUntil(&h, BE_CONNECT_MAX, beConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps (state=%d)",
                BE_CONNECT_MAX, (int)clientSimGetConnectState(h.cs));
    }
    at = loopbackHarnessPumpUntil(&h, BE_READY_MAX, beServerReady, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client's map download never completed within %d pumps",
                BE_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, BE_TANK_MAX, beHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", BE_TANK_MAX);
    }

    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client holds no slot, so there is no mux to queue onto");
    }

    loopbackHarnessPumpUntil(&h, BE_SETTLE, NULL, NULL);
    clientSimSetBrainEventCount(h.cs, 0);

    /* Tick one: more reliable events than one drain takes, and the first
     * effect burst behind them. */
    threadsWaitForMutex();
    for (i = 0; i < BE_CAP_FILLER; i++) {
        GameEvent ev;
        beMakePing(&ev, slot, i, BE_MARK_FILLER);
        if (!transportUdpServerTestAddGameEvent((int)slot, &ev)) {
            threadsReleaseMutex();
            loopbackHarnessStop(&h);
            UT_FAIL("the reliable game channel refused filler event %d of %d - "
                    "the window is %d deep, so the fixture is wrong, not the "
                    "server", i, BE_CAP_FILLER, CHANNEL_GAME_WINDOW);
        }
    }
    for (i = 0; i < BE_CAP_EFFECTS; i++) {
        GameEvent ev;
        beMakePing(&ev, slot, i, BE_MARK_EFFECT);
        if (!transportUdpServerTestAddEffectEvent((int)slot, &ev)) {
            threadsReleaseMutex();
            loopbackHarnessStop(&h);
            UT_FAIL("the effect channel refused event %d of the first burst",
                    i);
        }
    }
    threadsReleaseMutex();
    loopbackHarnessPump(&h);

    /* Tick two: the burst that evicts whatever of the first one the client
     * has not drained. */
    threadsWaitForMutex();
    for (i = 0; i < BE_CAP_EFFECTS; i++) {
        GameEvent ev;
        beMakePing(&ev, slot, i, BE_MARK_EFFECT2);
        if (!transportUdpServerTestAddEffectEvent((int)slot, &ev)) {
            threadsReleaseMutex();
            loopbackHarnessStop(&h);
            UT_FAIL("the effect channel refused event %d of the second burst",
                    i);
        }
    }
    threadsReleaseMutex();
    loopbackHarnessPump(&h);

    for (i = 1; i <= BE_DELIVER_MAX; i++) {
        loopbackHarnessPump(&h);
        beCollectPings(h.cs, BE_MARK_EFFECT, firstSeen, BE_CAP_EFFECTS);
        beCollectPings(h.cs, BE_MARK_EFFECT2, secondSeen, BE_CAP_EFFECTS);
        clientSimSetBrainEventCount(h.cs, 0);
        if (beCountMissing(firstSeen, BE_CAP_EFFECTS) == 0 &&
            beCountMissing(secondSeen, BE_CAP_EFFECTS) == 0) {
            deliveredAt = i;
            break;
        }
    }

    fprintf(stderr, "  effect cap: %d reliable + 2 x %d effect; client applied "
                    "%d/%d then %d/%d after %d pump(s) of %d\n",
            BE_CAP_FILLER, BE_CAP_EFFECTS,
            BE_CAP_EFFECTS - beCountMissing(firstSeen, BE_CAP_EFFECTS),
            BE_CAP_EFFECTS,
            BE_CAP_EFFECTS - beCountMissing(secondSeen, BE_CAP_EFFECTS),
            BE_CAP_EFFECTS, deliveredAt < 0 ? BE_DELIVER_MAX : deliveredAt,
            BE_DELIVER_MAX);

    if (beCountMissing(firstSeen, BE_CAP_EFFECTS) != 0) {
        int miss = beCountMissing(firstSeen, BE_CAP_EFFECTS);
        int first = beFirstMissing(firstSeen, BE_CAP_EFFECTS);
        loopbackHarnessStop(&h);
        UT_FAIL("%d of the %d effect event(s) sent alongside %d reliable "
                "event(s) never reached the client (first missing index %d) - "
                "the snapshot drain takes at most %d events, and an effect "
                "segment it has no room for is evicted from the receive ring "
                "by the next tick's burst rather than waiting",
                miss, BE_CAP_EFFECTS, BE_CAP_FILLER, first,
                MAX_SNAPSHOT_EVENTS);
    }
    if (beCountMissing(secondSeen, BE_CAP_EFFECTS) != 0) {
        int miss = beCountMissing(secondSeen, BE_CAP_EFFECTS);
        int first = beFirstMissing(secondSeen, BE_CAP_EFFECTS);
        loopbackHarnessStop(&h);
        UT_FAIL("%d of the %d effect event(s) in the second burst never "
                "reached the client (first missing index %d)",
                miss, BE_CAP_EFFECTS, first);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ── post_game_segment_applied ──────────────────────────────────────────
 *
 * The snapshot drain owns the order the game, effect and map channels apply
 * in while a round is running. The three tests that said so - joined, not in
 * the lobby, map installed - all stay true through the whole post-game
 * window, and the client's inLobby flips only on CTRL_GAME_PHASE_LOBBY,
 * which arrives at the end of it. The server stops sending snapshots the
 * moment the round ends, so for the length of the game-over hold the client
 * ingested standalone frames and drained nothing from those three channels:
 * the server resent the unacked tail over and over into a client that was
 * holding it for a drain that had stopped.
 *
 * The gate now also asks whether a snapshot has been applied recently. This
 * ends a round, lets the quiet settle, sends one reliable game event and
 * requires it to be applied while the server is still in game over.
 */
int run_post_game_segment_applied(void) {
    LoopbackHarness h;
    bool seen[1];
    BYTE slot;
    int at, i;
    int appliedAt = -1;

    memset(seen, 0, sizeof(seen));
    memset(&h, 0, sizeof(h));

    if (!loopbackHarnessStart(&h, "PostGameHost", /*lobbyMode*/ true,
                              /*impairSpec*/ NULL, BE_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (post game segment) failed");
    }

    at = loopbackHarnessPumpUntil(&h, BE_CONNECT_MAX, beConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never reached CONNECTED within %d pumps (state=%d)",
                BE_CONNECT_MAX, (int)clientSimGetConnectState(h.cs));
    }
    if (!loopbackHarnessTriggerGameStart(&h)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client's slot was never ready for game start");
    }
    at = loopbackHarnessPumpUntil(&h, BE_READY_MAX, beRunning, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the game never reached the running state within %d pumps",
                BE_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, BE_READY_MAX, beClientInGame, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never left the lobby within %d pumps",
                BE_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, BE_READY_MAX, beServerReady, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client's map download never completed within %d pumps",
                BE_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, BE_TANK_MAX, beHaveTank, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client never got a tank within %d pumps", BE_TANK_MAX);
    }

    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client holds no slot, so there is no mux to queue onto");
    }
    loopbackHarnessPumpUntil(&h, BE_SETTLE, NULL, NULL);

    /* End the round. The server stops sending snapshots here and holds in
     * game over for GAMEOVER_HOLD_TICKS before the lobby phase event. */
    threadsWaitForMutex();
    serverSimEnterGameOver(h.sim);
    threadsReleaseMutex();
    for (i = 0; i < BE_POSTGAME_QUIET; i++) {
        loopbackHarnessPump(&h);
    }
    if (serverSimGetState(h.sim) != serverStateGameOver) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server left game over after %d pump(s) (state %d), so the "
                "window this case measures was never open", BE_POSTGAME_QUIET,
                (int)serverSimGetState(h.sim));
    }

    /* The premise: the client still believes it is in the round, so the
     * three original tests in the gate all hold and only the quiet is left
     * to break the tie. The lobby phase event is what flips this, and it is
     * still ahead of us. */
    if (clientSimIsInLobby(h.cs)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client is already in the lobby %d pump(s) after game "
                "over, so the gate this case measures was already open",
                BE_POSTGAME_QUIET);
    }

    clientSimSetBrainEventCount(h.cs, 0);
    {
        GameEvent ev;
        beMakePing(&ev, slot, 0, BE_MARK_POSTGAME);
        threadsWaitForMutex();
        if (!transportUdpServerTestAddGameEvent((int)slot, &ev)) {
            threadsReleaseMutex();
            loopbackHarnessStop(&h);
            UT_FAIL("the reliable game channel refused the post-game event");
        }
        threadsReleaseMutex();
    }

    for (i = 1; i <= BE_POSTGAME_MAX; i++) {
        loopbackHarnessPump(&h);
        beCollectPings(h.cs, BE_MARK_POSTGAME, seen, 1);
        clientSimSetBrainEventCount(h.cs, 0);
        if (seen[0]) {
            appliedAt = i;
            break;
        }
    }

    fprintf(stderr, "  post game: server state %d, event applied at pump %d of "
                    "%d\n", (int)serverSimGetState(h.sim), appliedAt,
            BE_POSTGAME_MAX);

    if (!seen[0]) {
        int state = (int)serverSimGetState(h.sim);
        loopbackHarnessStop(&h);
        UT_FAIL("a reliable game event sent after game over was not applied "
                "within %d pump(s) (server state %d) - with no snapshot "
                "flowing there is no snapshot drain to hold it for, and the "
                "server goes on resending the tail until the round ends",
                BE_POSTGAME_MAX, state);
    }

    loopbackHarnessStop(&h);
    return 0;
}

/* ── quit_burst_all_delivered ───────────────────────────────────────────── */

int run_quit_burst_all_delivered(void) {
    LoopbackHarness h;
    BYTE slot1, slot2;
    BYTE numPills = 0, numBases = 0;
    BYTE pillOwner[MAX_PILLS];
    BYTE baseOwner[MAX_BASES];
    bool heldPill[MAX_PILLS];
    bool heldBase[MAX_BASES];
    int heldPills = 0, heldBases = 0;
    int watchBase = -1, watchPill = -1;
    uint32_t fxDrop0 = 0, fxSkip0 = 0, fxSent0 = 0;
    uint32_t fxDrop = 0, fxSkip = 0, fxSent = 0;
    int at, i;
    int quitAt = -1, leftAt = -1;
    int missingPills = 0, missingBases = 0;
    int firstMissingPill = -1, firstMissingBase = -1;

    memset(&h, 0, sizeof(h));
    memset(pillOwner, 0, sizeof(pillOwner));
    memset(baseOwner, 0, sizeof(baseOwner));
    memset(heldPill, 0, sizeof(heldPill));
    memset(heldBase, 0, sizeof(heldBase));

    if (!loopbackHarnessStart(&h, "QuitBurstStays", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, QB_SEED)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start (quit burst) failed");
    }
    if (!loopbackHarnessAddClient(&h, "QuitBurstLeaves")) {
        loopbackHarnessStop(&h);
        UT_FAIL("the second client failed to connect");
    }

    at = loopbackHarnessPumpUntil(&h, BE_CONNECT_MAX, beBothConnected, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never reached CONNECTED within %d pumps "
                "(client 1 state=%d, client 2 state=%d)", BE_CONNECT_MAX,
                (int)clientSimGetConnectState(h.cs),
                (int)clientSimGetConnectState(h.cs2));
    }
    at = loopbackHarnessPumpUntil(&h, BE_READY_MAX, beBothServerReady, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never finished the map download within %d pumps",
                BE_READY_MAX);
    }
    at = loopbackHarnessPumpUntil(&h, BE_TANK_MAX, beBothHaveTanks, NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never got a tank within %d pumps", BE_TANK_MAX);
    }

    slot1 = clientSimGetMyPlayerNum(h.cs);
    slot2 = clientSimGetMyPlayerNum(h.cs2);
    if (slot1 >= MAX_TANKS || slot2 >= MAX_TANKS || slot1 == slot2) {
        loopbackHarnessStop(&h);
        UT_FAIL("the two clients hold slots %u and %u — quitting one of them "
                "would prove nothing", (unsigned)slot1, (unsigned)slot2);
    }

    /* Client 2 takes every pillbox and base, so its departure migrates the lot
     * in one tick. Under the server's mutex, as the harness's own start does:
     * the server's tick runs on this thread, but its recv thread does not.
     * These are the same two calls serverSimRemovePlayer migrates with. */
    threadsWaitForMutex();
    numPills = pillsGetNumPills(&h.sim->sim.pb);
    numBases = basesGetNumBases(&h.sim->sim.bs);
    if (numPills > MAX_PILLS) numPills = MAX_PILLS;
    if (numBases > MAX_BASES) numBases = MAX_BASES;
    for (i = 1; i <= (int)numPills; i++) {
        pillsSetPillOwner(&h.sim->sim, &h.sim->sim.pb, (BYTE)i, slot2,
                          /*migrate*/ TRUE);
    }
    for (i = 1; i <= (int)numBases; i++) {
        basesSetBaseOwner(&h.sim->sim, (BYTE)i, slot2, /*migrate*/ TRUE,
                          /*keepStock*/ TRUE);
    }
    threadsReleaseMutex();

    /* Let those changes publish and settle before the window opens, so what is
     * counted below is the departure's burst and not the setup's. */
    loopbackHarnessPumpUntil(&h, BE_SETTLE, NULL, NULL);

    /* What client 2 actually ended up holding is what its departure will
     * migrate, so that is what the assertions below are drawn from — an
     * inactive pill or base takes no owner, and asserting on one it never held
     * would report a missing event that was never raised. One of them is
     * singled out as the watch: the pump on which the server moves it off slot
     * 2 is the pump that published the whole burst. */
    for (i = 1; i <= (int)numPills; i++) {
        if (pillsGetPillOwner(&h.sim->sim.pb, (BYTE)i) == slot2) {
            heldPill[i - 1] = true;
            heldPills++;
            if (watchPill < 0) watchPill = i;
        }
    }
    for (i = 1; i <= (int)numBases; i++) {
        if (basesGetBaseOwner(&h.sim->sim.bs, (BYTE)i) == slot2) {
            heldBase[i - 1] = true;
            heldBases++;
            if (watchBase < 0) watchBase = i;
        }
    }
    if (heldPills == 0 && heldBases == 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 2 holds no pillbox and no base on this map (%u pill(s), "
                "%u base(s) on it), so its departure publishes no ownership "
                "burst to measure", (unsigned)numPills, (unsigned)numBases);
    }

    clientSimSetBrainEventCount(h.cs, 0);
    channelGetBestEffortStats(&udpServer.channelMux[slot1], CHANNEL_GAME_EFFECT,
                              &fxSent0, &fxDrop0, &fxSkip0);
    if (fxSkip0 != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's mux arrived with %u best-effort segment(s) already "
                "left behind, before the quit — the assertion after it would be "
                "reading that rather than the burst", (unsigned)fxSkip0);
    }

    if (clientSimGetNetStatus(h.cs) != netRunning) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1's net status is %d, not netRunning, before the quit",
                (int)clientSimGetNetStatus(h.cs));
    }

    /* The quit. clientSimDisconnect tears the transport down, and
     * transportUdpClientDestroy sends PACKET_QUIT on the way out because the
     * join state is still CONNECTED. Nothing is said to the server here. */
    clientSimDisconnect(h.cs2);

    /* The publishing tick, found from the server's own state: the migration
     * runs in serverSimRemovePlayer while the recv queue is drained, and the
     * per-tick pill/base diff that turns it into events runs later in the same
     * serverInstanceTick, so the pump that shows the new owner is the pump that
     * put the burst on the wire. The recv thread can lag a pump, which is why
     * this is a search and not a fixed count. */
    for (i = 1; i <= QB_QUIT_MAX; i++) {
        loopbackHarnessPump(&h);
        if (watchBase > 0) {
            if (basesGetBaseOwner(&h.sim->sim.bs, (BYTE)watchBase) != slot2) {
                quitAt = i;
                break;
            }
        } else if (pillsGetPillOwner(&h.sim->sim.pb, (BYTE)watchPill) != slot2) {
            quitAt = i;
            break;
        }
    }
    if (quitAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server never migrated slot %u's holdings within %d pumps "
                "of the quit — there is no burst to observe", (unsigned)slot2,
                QB_QUIT_MAX);
    }

    /* Whoever inherited: NEUTRAL with no connected ally, client 1 if the two
     * happen to be allied. Read it off the server rather than assume, and
     * assert the client was told that. */
    for (i = 1; i <= (int)numPills; i++) {
        pillOwner[i - 1] = pillsGetPillOwner(&h.sim->sim.pb, (BYTE)i);
    }
    for (i = 1; i <= (int)numBases; i++) {
        baseOwner[i - 1] = basesGetBaseOwner(&h.sim->sim.bs, (BYTE)i);
    }

    /* Two pumps, and everything the burst published has to be in. */
    for (i = 0; i < BE_DELIVER; i++) {
        loopbackHarnessPump(&h);
        if (leftAt < 0 &&
            playersIsInUse(&clientSimGetGameSim(h.cs)->plyrs, slot2) == FALSE) {
            leftAt = i + 1;
        }
    }

    for (i = 0; i < (int)numPills; i++) {
        /* EVENT_PILL_UPDATE: data[0] is the 0-based index, data[3] the owner. */
        if (!heldPill[i]) continue;
        if (!beSawOwnerEvent(h.cs, EVENT_PILL_UPDATE, (uint8_t)i, 3,
                             pillOwner[i])) {
            if (firstMissingPill < 0) firstMissingPill = i;
            missingPills++;
        }
    }
    for (i = 0; i < (int)numBases; i++) {
        /* EVENT_BASE_UPDATE: data[0] is the 0-based index, data[1] the owner. */
        if (!heldBase[i]) continue;
        if (!beSawOwnerEvent(h.cs, EVENT_BASE_UPDATE, (uint8_t)i, 1,
                             baseOwner[i])) {
            if (firstMissingBase < 0) firstMissingBase = i;
            missingBases++;
        }
    }

    channelGetBestEffortStats(&udpServer.channelMux[slot1], CHANNEL_GAME_EFFECT,
                              &fxSent, &fxDrop, &fxSkip);
    fprintf(stderr, "  quit burst: slot %u left at pump %d, %d pill(s) and %d "
                    "base(s) migrated; client 1 missing %d pill / %d base "
                    "event(s), leave seen at pump %d; effect sent %u->%u drops "
                    "%u->%u skips %u->%u\n",
            (unsigned)slot2, quitAt, heldPills, heldBases,
            missingPills, missingBases, leftAt,
            (unsigned)fxSent0, (unsigned)fxSent, (unsigned)fxDrop0,
            (unsigned)fxDrop, (unsigned)fxSkip0, (unsigned)fxSkip);

    if (missingPills != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1 was never told the new owner of %d of the %d "
                "pillbox(es) the quit migrated (first missing index %d, owner "
                "%u) within %d pump(s) of the publishing tick — the burst is "
                "raised on one tick and has to leave on it",
                missingPills, heldPills, firstMissingPill,
                (unsigned)pillOwner[firstMissingPill < 0 ? 0 : firstMissingPill],
                BE_DELIVER);
    }
    if (missingBases != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("client 1 was never told the new owner of %d of the %d base(s) "
                "the quit migrated (first missing index %d, owner %u) within "
                "%d pump(s) of the publishing tick",
                missingBases, heldBases, firstMissingBase,
                (unsigned)baseOwner[firstMissingBase < 0 ? 0 : firstMissingBase],
                BE_DELIVER);
    }
    if (leftAt < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("slot %u was still in client 1's roster %d pump(s) after the "
                "publishing tick — the leave did not ride out with the burst",
                (unsigned)slot2, BE_DELIVER);
    }
    /* Best-effort loss covers the pill half of the burst only: EVENT_PILL_UPDATE
     * is best-effort and EVENT_BASE_UPDATE is reliable. The base half is proved
     * by the per-event check above. */
    if (fxDrop != fxDrop0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the quit's pill-ownership burst cost %u effect ring drop(s) on "
                "client 1's mux", (unsigned)(fxDrop - fxDrop0));
    }
    if (fxSkip != 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the quit's pill-ownership burst left %u best-effort segment(s) "
                "behind on client 1's mux (it was %u before the quit)",
                (unsigned)fxSkip, (unsigned)fxSkip0);
    }

    loopbackHarnessStop(&h);
    return 0;
}
