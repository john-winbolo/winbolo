/*
 * The snapshot trailer is not the only carrier a running tick has.
 *
 * serverSendSnapshot builds one datagram: the snapshot, then one channel frame
 * in whatever is left of UDP_MAX_PAYLOAD. A busy snapshot leaves a few dozen
 * bytes, so most of what the channel layer had queued stayed queued. Reliable
 * channels were never lost to that — they wait and a full window is
 * backpressure — but the best-effort effect ring is 64 deep and the voice ring
 * 8, and neither waits: the next send over a full ring drops the oldest entry.
 * A burst of sounds and explosions raised on one tick was thrown away on that
 * tick. Nothing in the design asked for one datagram per tick; the lobby and
 * map-download paths already emit up to MAP_DOWNLOAD_FRAMES_PER_TICK standalone
 * frames, and the running path simply never had the loop.
 *
 * This case pins the loop from the wire side. A raw UDP socket joins the
 * harness server for real — the cookie handshake, then the map stream acked
 * back on CHANNEL_BULK through a ChannelMux the test owns, so the slot reaches
 * download-complete the way any client does and snapshots start flowing to it.
 * The test then queues more reliable game events on that slot than one frame
 * can carry and runs exactly one transportUdpServerSend.
 *
 * Two things are asserted, because the first alone would pass on a change that
 * sent extra datagrams carrying nothing useful:
 *
 *   - the send produces more than one datagram, and no more than
 *     SNAPSHOT_EXTRA_CHANNEL_FRAMES past the snapshot;
 *   - every queued event is on the wire. The frames are fed back into the
 *     test's own mux and drained through channelReceive, which is in-order and
 *     stops dead at a gap — so recovering all MF_EVENTS markers is proof that
 *     the whole run of sequence numbers arrived, not just that the count
 *     matched.
 *
 * Before the send loop lands this reports one datagram and about ninety of the
 * events, which is what the trailer's share of the budget holds.
 *
 * The marker is carried in EVENT_PING's world coordinates: data[2..3] is the
 * event's index and data[4..5] a fixed pattern, so a real game event the
 * running sim happens to raise for this slot cannot be miscounted as one.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"                 /* MAP_STR_SIZE, MAX_TANKS, BYTE */
#include "platform_net.h"           /* sockets, struct sockaddr_in */
#include "wire_limits.h"            /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"               /* PACKET_* types, PACKET_HEADER_SIZE,
                                     * BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"          /* WBN_JOIN_KEY_WIRE_LEN, transportUdpServerSend,
                                     * the download-complete + game-event test hooks */
#include "transport_udp_internal.h" /* packHeader, packU32, unpackU32, getPacketType,
                                     * UDP_MAX_PAYLOAD, SNAPSHOT_HEADER_WIRE_SIZE,
                                     * the unpack*Snapshot helpers */
#include "transport_udp_server_internal.h" /* SNAPSHOT_EXTRA_CHANNEL_FRAMES */
#include "channel_mux.h"            /* ChannelMux and the channel primitives */
#include "input_packet.h"           /* GameEvent, EVENT_PING, PING_KIND_STANDARD,
                                     * the snapshot entry types */
#include "threads.h"                /* threadsWaitForMutex / threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

/* Pumps allowed for each phase to settle. Bounds to report a failure against,
 * not expected counts. */
#define MF_PUMP_MAX     200
#define MF_DOWNLOAD_MAX 4000

/* Polls, a millisecond apart, given to loopback to hand over the datagrams of
 * the one measured send. Nothing is pumped in that window, so a late arrival is
 * the only thing being waited for. */
#define MF_COLLECT_POLLS 200

/* Pumps run after the download completes so the last acks reach the server and
 * the reliable game channel is quiescent before the measured send. */
#define MF_SETTLE_PUMPS 8

/* Drain passes before the measured send, a millisecond apart, so a datagram
 * loopback is still delivering from the last settle pump is read out rather
 * than counted against the send. */
#define MF_DRAIN_POLLS 5

/* Reliable game events queued for the one measured send. One channel frame
 * holds at most (UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE - 2) / 14 = 99 of them
 * (7 bytes of segment header plus a 7-byte EVENT_PING), and the snapshot
 * trailer holds fewer still, so this always needs more than one frame. Four
 * frames hold at least 4 + 99 + 99 + 99 even against the largest snapshot
 * the server can build, so it also always fits inside the budget the loop
 * has. */
#define MF_EVENTS 220

/* The fixed pattern in the marker event's y coordinate. */
#define MF_MARK_HI 0xA5
#define MF_MARK_LO 0x5Au

/* Nothing on a clean path draws from the impairment stream; the seed keeps the
 * run reproducible regardless. */
#define MF_SEED 0x5E4Du

/* Build a JOIN_REQUEST into buf with the server's own version triple and a
 * trailing address-proof cookie (the given bytes, or zeros if NULL). Same wire
 * shape as test_map_reask_throttle.c's rtBuildJoin. */
static int mfBuildJoin(uint8_t *buf, const char *name,
                       const uint8_t *cookieOrNull) {
    int pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_JOIN_REQUEST, 0);

    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    strncpy((char *)(buf + pos), name, PACKET_MAX_PLAYER_NAME - 1);
    pos += PACKET_MAX_PLAYER_NAME;

    memset(buf + pos, 0, MAP_STR_SIZE);   /* empty password */
    pos += MAP_STR_SIZE;

    buf[pos++] = BOLO_VERSION_MAJOR;
    buf[pos++] = BOLO_VERSION_MINOR;
    buf[pos++] = BOLO_VERSION_REVISION;

    memset(buf + pos, 0, WBN_JOIN_KEY_WIRE_LEN);  /* anonymous join */
    pos += WBN_JOIN_KEY_WIRE_LEN;

    buf[pos++] = 0;   /* flags: no rejoin, no auth */
    buf[pos++] = 0;   /* clientType (unknown) */
    buf[pos++] = 0;   /* clientHints */
    buf[pos++] = 0;   /* fallbackCountry[0] */
    buf[pos++] = 0;   /* fallbackCountry[1] */

    if (cookieOrNull != NULL) {
        memcpy(buf + pos, cookieOrNull, JOIN_COOKIE_LEN);
    } else {
        memset(buf + pos, 0, JOIN_COOKIE_LEN);
    }
    pos += JOIN_COOKIE_LEN;
    return pos;
}

/* connId on the wire is two big-endian u32s, low half first (packConnId is
 * static to its translation unit, so the shape is spelled out here as
 * test_map_reask_throttle.c does). */
static uint64_t mfUnpackConnId(const uint8_t *buf) {
    uint32_t lo = unpackU32(buf);
    uint32_t hi = unpackU32(buf + 4);
    return ((uint64_t)hi << 32) | (uint64_t)lo;
}

static void mfPackConnId(uint8_t *buf, uint64_t connId) {
    packU32(buf,     (uint32_t)(connId & 0xffffffffULL));
    packU32(buf + 4, (uint32_t)(connId >> 32));
}

/* Open a non-blocking UDP socket bound to an ephemeral 127.0.0.1 port. */
static SOCKET mfOpenSocket(void) {
    struct sockaddr_in addr;
    unsigned long nonblock = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr("127.0.0.1");
    addr.sin_port        = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nonblock);
    return s;
}

/* Where the channel-frame trailer starts inside a PACKET_STATE_SNAPSHOT
 * datagram: parse the snapshot exactly as the client's PACKET_STATE_SNAPSHOT
 * arm does and whatever follows the last entry is the trailer. Returns len when
 * the snapshot filled the datagram (no trailer rode this one), or -1 if the
 * parse runs off the end. */
static int mfTrailerOffset(const uint8_t *buf, int len) {
    int pos = PACKET_HEADER_SIZE;
    int counts[5];
    int i, k;

    if (len < PACKET_HEADER_SIZE + SNAPSHOT_HEADER_WIRE_SIZE) return -1;
    pos += 4;                                /* serverTick         */
    pos += 4;                                /* lastProcessedInput */
    for (k = 0; k < 5; k++) counts[k] = buf[pos++];
    pos += 2;                                /* mapChecksum        */
    pos += 2;                                /* returnToLobbyTicks */

    for (i = 0; i < counts[0]; i++) {
        TankSnapshot ts;
        int n = unpackTankSnapshot(buf + pos, (size_t)(len - pos), &ts);
        if (n == 0) return -1;
        pos += n;
    }
    for (i = 0; i < counts[1]; i++) {
        ShellSnapshot ss;
        int n = unpackShellSnapshot(buf + pos, (size_t)(len - pos), &ss);
        if (n == 0) return -1;
        pos += n;
    }
    for (i = 0; i < counts[2]; i++) {
        TkExplosionSnapshot te;
        int n = unpackTkExplosionSnapshot(buf + pos, (size_t)(len - pos), &te);
        if (n == 0) return -1;
        pos += n;
    }
    for (i = 0; i < counts[3]; i++) {
        BaseSnapshot bs;
        int n = unpackBaseSnapshot(buf + pos, (size_t)(len - pos), &bs);
        if (n == 0) return -1;
        pos += n;
    }
    for (i = 0; i < counts[4]; i++) {
        PillSnapshot ps;
        int n = unpackPillSnapshot(buf + pos, (size_t)(len - pos), &ps);
        if (n == 0) return -1;
        pos += n;
    }
    return pos;
}

/* Feed one datagram the server sent us into the test's mux. A standalone
 * PACKET_CHANNEL is one frame straight after the header; a snapshot carries one
 * as its trailer. Anything else is ignored. Returns false only when a snapshot
 * would not parse, which means the trailer could not be found rather than that
 * it was not there. */
static bool mfIngest(ChannelMux *m, const uint8_t *buf, int n) {
    uint8_t type = getPacketType(buf, n);
    if (type == PACKET_CHANNEL) {
        if (n > PACKET_HEADER_SIZE) {
            channelRecvFrame(m, buf + PACKET_HEADER_SIZE, n - PACKET_HEADER_SIZE);
        }
        return true;
    }
    if (type == PACKET_STATE_SNAPSHOT) {
        int off = mfTrailerOffset(buf, n);
        if (off < 0) return false;
        if (off < n) {
            channelRecvFrame(m, buf + off, n - off);
        }
    }
    return true;
}

/* Pop everything the mux can deliver and throw it away, except that a game
 * event carrying the test's marker sets its slot in `seen`. Draining is what
 * advances each channel's delivery cursor, which is the ack the test sends
 * back — without it the map download never completes. */
static void mfDrain(ChannelMux *m, bool *seen, int seenCap) {
    uint8_t seg[CHANNEL_MAX_SEG];
    uint16_t slen;

    while (channelReceive(m, CHANNEL_GAME, seg, &slen)) {
        GameEvent ev;
        int idx;
        if (unpackGameEvent(seg, slen, &ev) <= 0) continue;
        if (ev.type != EVENT_PING) continue;
        if (ev.data[4] != MF_MARK_HI || ev.data[5] != MF_MARK_LO) continue;
        idx = ((int)ev.data[2] << 8) | (int)ev.data[3];
        if (seen != NULL && idx >= 0 && idx < seenCap) seen[idx] = true;
    }
    while (channelReceive(m, CHANNEL_CONTROL, seg, &slen)) { /* discarded */ }
    while (channelReceive(m, CHANNEL_MAP, seg, &slen))     { /* discarded */ }
    while (channelReceive(m, CHANNEL_BULK, seg, &slen))    { /* discarded */ }
    while (channelReceiveBestEffort(m, CHANNEL_GAME_EFFECT, seg, &slen)) { }
    while (channelReceiveBestEffort(m, CHANNEL_VOICE, seg, &slen))       { }
}

/* Send the mux's current frame back to the server as a standalone
 * PACKET_CHANNEL. Sent even when it is the empty two-byte frame: it is the only
 * traffic this raw client produces, and it is what keeps the slot's
 * lastReceivedTick fresh so the timeout sweep does not drop it. */
static void mfSendFrame(ChannelMux *m, SOCKET sock,
                        const struct sockaddr_in *serverAddr, uint32_t tick,
                        uint32_t *seq) {
    uint8_t cbuf[UDP_MAX_PAYLOAD];
    int frameLen;

    channelTick(m, tick, 0);
    frameLen = channelBuildFrame(m, cbuf + PACKET_HEADER_SIZE,
                                 UDP_MAX_PAYLOAD - PACKET_HEADER_SIZE);
    if (frameLen < 2) return;
    packHeader(cbuf, PACKET_CHANNEL, (*seq)++);
    sendto(sock, (const char *)cbuf, PACKET_HEADER_SIZE + frameLen, 0,
           (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
}

int run_send_drains_channels_multi_frame(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    ChannelMux *peer;
    bool *seen;
    uint8_t joinBuf[1024];
    uint8_t readyBuf[PACKET_HEADER_SIZE + 8];
    uint8_t cookie[JOIN_COOKIE_LEN];
    uint8_t datagrams[1 + SNAPSHOT_EXTRA_CHANNEL_FRAMES + 4][UDP_MAX_PAYLOAD];
    int     datagramLen[1 + SNAPSHOT_EXTRA_CHANNEL_FRAMES + 4];
    int joinLen, slot = -1, i, n, got = 0, snapshots = 0, missing = 0;
    uint64_t connId = 0;
    uint32_t peerTick = 0, peerSeq = 1;
    SOCKET sock;
    bool gotAccept = false, gotChallenge = false, complete = false;

    peer = (ChannelMux *)malloc(sizeof(ChannelMux));
    seen = (bool *)calloc((size_t)MF_EVENTS, sizeof(bool));
    if (peer == NULL || seen == NULL) {
        free(peer);
        free(seen);
        UT_FAIL("out of memory allocating the test's channel mux");
    }
    channelMuxInit(peer);

    if (!loopbackHarnessStart(&h, "MultiFrameHost", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, MF_SEED)) {
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("harness start (send multi frame) failed");
    }

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    sock = mfOpenSocket();
    if (sock == INVALID_SOCKET) {
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("could not open the raw UDP socket for the crafted JOIN");
    }

    /* ---- Join for real: challenge, then the cookie echoed back. ---- */
    joinLen = mfBuildJoin(joinBuf, "MultiFrame", NULL);
    for (i = 0; i < MF_PUMP_MAX && !gotChallenge; i++) {
        uint8_t in[UDP_MAX_PAYLOAD];
        if (i == 0) {
            sendto(sock, (const char *)joinBuf, joinLen, 0,
                   (const struct sockaddr *)&serverAddr, sizeof(serverAddr));
        }
        loopbackHarnessPump(&h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &serverAddr)) > 0) {
            if (getPacketType(in, n) == PACKET_JOIN_CHALLENGE &&
                n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                memcpy(cookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                gotChallenge = true;
            }
        }
    }
    if (!gotChallenge) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("crafted JOIN drew no PACKET_JOIN_CHALLENGE within %d pumps",
                MF_PUMP_MAX);
    }

    joinLen = mfBuildJoin(joinBuf, "MultiFrame", cookie);
    for (i = 0; i < MF_PUMP_MAX && !gotAccept; i++) {
        uint8_t in[UDP_MAX_PAYLOAD];
        if (i == 0) {
            sendto(sock, (const char *)joinBuf, joinLen, 0,
                   (const struct sockaddr *)&serverAddr, sizeof(serverAddr));
        }
        loopbackHarnessPump(&h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &serverAddr)) > 0) {
            if (getPacketType(in, n) == PACKET_JOIN_ACCEPT &&
                n >= PACKET_HEADER_SIZE + 1 + 4 + 4 + 8) {
                slot   = in[PACKET_HEADER_SIZE];
                connId = mfUnpackConnId(in + PACKET_HEADER_SIZE + 9);
                gotAccept = true;
            } else {
                mfIngest(peer, in, n);
            }
        }
        mfDrain(peer, NULL, 0);
    }
    if (!gotAccept || slot < 0 || slot >= MAX_TANKS) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("cookie-echoing JOIN drew no usable PACKET_JOIN_ACCEPT within "
                "%d pumps (slot %d)", MF_PUMP_MAX, slot);
    }

    /* ---- Take the map download the whole way, so snapshots start flowing.
     * READY arms the stream; every pump after it feeds the arriving bulk
     * segments into the test's mux, drains them (which advances the delivery
     * cursor) and sends the cursor back as an ack. ---- */
    packHeader(readyBuf, PACKET_MAP_DL_READY, 1u);
    mfPackConnId(readyBuf + PACKET_HEADER_SIZE, connId);
    sendto(sock, (const char *)readyBuf, sizeof(readyBuf), 0,
           (const struct sockaddr *)&serverAddr, sizeof(serverAddr));

    for (i = 0; i < MF_DOWNLOAD_MAX && !complete; i++) {
        uint8_t in[UDP_MAX_PAYLOAD];
        loopbackHarnessPump(&h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &serverAddr)) > 0) {
            mfIngest(peer, in, n);
        }
        mfDrain(peer, NULL, 0);
        mfSendFrame(peer, sock, &serverAddr, ++peerTick, &peerSeq);
        complete = transportUdpServerTestDownloadComplete(slot);
    }
    if (!complete) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("slot %d never reached map-download complete within %d pumps",
                slot, MF_DOWNLOAD_MAX);
    }

    /* Let the last acks land so the reliable game channel is caught up before
     * the measured send — an unacked tail past its retransmit timeout would put
     * a resend in the same frames and muddy what the burst costs. */
    for (i = 0; i < MF_SETTLE_PUMPS; i++) {
        uint8_t in[UDP_MAX_PAYLOAD];
        loopbackHarnessPump(&h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &serverAddr)) > 0) {
            mfIngest(peer, in, n);
        }
        mfDrain(peer, NULL, 0);
        mfSendFrame(peer, sock, &serverAddr, ++peerTick, &peerSeq);
    }

    /* Empty the socket so everything counted below came from the one send.
     * Polled, not a single pass: loopback does not hand a datagram over
     * inside sendto, so the last settle pump's snapshot can still be on its
     * way when the first read finds the socket empty. Without the wait it
     * lands inside the count below as a second snapshot. */
    for (i = 0; i < MF_DRAIN_POLLS; i++) {
        uint8_t in[UDP_MAX_PAYLOAD];
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &serverAddr)) > 0) {
            mfIngest(peer, in, n);
        }
        mfDrain(peer, NULL, 0);
        SDL_Delay(1);
    }

    /* ---- Queue more than one frame holds, then run exactly one send. ---- */
    for (i = 0; i < MF_EVENTS; i++) {
        GameEvent ev;
        memset(&ev, 0, sizeof(ev));
        ev.type    = EVENT_PING;
        ev.data[0] = (uint8_t)slot;
        ev.data[1] = PING_KIND_STANDARD;
        ev.data[2] = (uint8_t)(i >> 8);
        ev.data[3] = (uint8_t)(i & 0xff);
        ev.data[4] = MF_MARK_HI;
        ev.data[5] = MF_MARK_LO;
        if (!transportUdpServerTestAddGameEvent(slot, &ev)) {
            closesocket(sock);
            loopbackHarnessStop(&h);
            free(peer);
            free(seen);
            UT_FAIL("the reliable game channel refused event %d of %d — the "
                    "window is %d deep, so the fixture is wrong, not the "
                    "server", i, MF_EVENTS, CHANNEL_GAME_WINDOW);
        }
    }

    threadsWaitForMutex();
    transportUdpServerSend(h.sim);
    threadsReleaseMutex();

    /* Collect what that one send put on the wire. No pump runs here, so a
     * second send cannot add to it; the loop only gives loopback time to
     * deliver. */
    memset(datagramLen, 0, sizeof(datagramLen));
    for (i = 0; i < MF_COLLECT_POLLS; i++) {
        uint8_t in[UDP_MAX_PAYLOAD];
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &serverAddr)) > 0) {
            if (got < (int)(sizeof(datagramLen) / sizeof(datagramLen[0]))) {
                memcpy(datagrams[got], in, (size_t)n);
                datagramLen[got] = n;
            }
            got++;
            if (getPacketType(in, n) == PACKET_STATE_SNAPSHOT) snapshots++;
        }
        SDL_Delay(1);
    }

    fprintf(stderr, "  send multi frame: %d event(s) queued -> %d datagram(s), "
                    "%d snapshot(s)\n", MF_EVENTS, got, snapshots);

    if (snapshots != 1) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("one transportUdpServerSend produced %d snapshot datagram(s) "
                "for slot %d, expected exactly 1", snapshots, slot);
    }
    if (got < 2) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("%d queued game event(s) went out in %d datagram(s) — the "
                "snapshot trailer carries what is left of UDP_MAX_PAYLOAD and "
                "nothing follows it, so the rest of the burst waited a tick",
                MF_EVENTS, got);
    }
    if (got > 1 + SNAPSHOT_EXTRA_CHANNEL_FRAMES) {
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("one send produced %d datagram(s), more than the snapshot plus "
                "SNAPSHOT_EXTRA_CHANNEL_FRAMES (%d)",
                got, 1 + SNAPSHOT_EXTRA_CHANNEL_FRAMES);
    }

    /* Every queued event on the wire. channelReceive is in-order and stops at
     * a gap, so a single missing segment leaves every event above it
     * undelivered — recovering all MF_EVENTS markers is the whole run. */
    for (i = 0; i < got; i++) {
        if (datagramLen[i] <= 0) continue;
        if (!mfIngest(peer, datagrams[i], datagramLen[i])) {
            closesocket(sock);
            loopbackHarnessStop(&h);
            free(peer);
            free(seen);
            UT_FAIL("datagram %d (%d bytes) did not parse as a snapshot, so "
                    "its channel trailer could not be located — the snapshot "
                    "layout this case mirrors has moved", i, datagramLen[i]);
        }
    }
    mfDrain(peer, seen, MF_EVENTS);
    for (i = 0; i < MF_EVENTS; i++) {
        if (!seen[i]) missing++;
    }
    if (missing != 0) {
        int firstMissing = -1;
        for (i = 0; i < MF_EVENTS; i++) {
            if (!seen[i]) { firstMissing = i; break; }
        }
        closesocket(sock);
        loopbackHarnessStop(&h);
        free(peer);
        free(seen);
        UT_FAIL("%d of %d queued game event(s) never reached the wire in the "
                "one send (first missing index %d) — the extra frames are not "
                "carrying what the trailer left behind",
                missing, MF_EVENTS, firstMissing);
    }

    closesocket(sock);
    loopbackHarnessStop(&h);
    free(peer);
    free(seen);
    return 0;
}
