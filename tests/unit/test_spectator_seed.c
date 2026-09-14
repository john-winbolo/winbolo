/*
 * Spectator seed + forward-feed + cold-start tests (Phases 2d-c, 2d-e, 2d-f).
 *
 * t_seed_arm (2d-c): proves the server-side arm of a delayed-keyframe seed — a
 * connected spectator on a server whose spectator ring is recording gets the
 * keyframe at head - specDelayTicks copied into spectator-owned storage and
 * armed as a BULK_KIND_SPEC_SEED transfer on CHANNEL_BULK. The white-box gate
 * compares the armed seed blob byte-for-byte against an independent seek of the
 * same ring at the same delay, and checks the in-flight bulk kind. The ring is
 * frozen with logStop() before seating so the serve-path seek and the test's
 * cross-check seek read an identical static ring.
 *
 * t_feed_lag (2d-e): proves the anti-cheat lag of the forward feed — after the
 * seed completes, the feed streams ring records as BULK_KIND_SPEC_RECORD blobs,
 * tracking head - specDelayTicks. The ring keeps recording (no logStop freeze)
 * so the live head advances while the feed runs. A white-box per-pump ack hook
 * (transportUdpServerTestSpectatorAckBulk) simulates a peer that keeps up — it
 * completes the seed and keeps the bulk window draining without a real spectator
 * channel endpoint (the byte-level wire reconstruction is 3b). The gate, read
 * via transportUdpServerGetSpectatorFeedSeq: at delay > 0 the feed cursor equals
 * head - delay and stays strictly below head (never the live tick); at delay = 0
 * it equals head; the in-flight blob kind is BULK_KIND_SPEC_RECORD. The delay is
 * set (serverSimSetSpecDelayTicks) before serverInstanceCreateSpectatorRing so
 * the ring's retention is sized to cover it.
 *
 * t_cold_start (2d-f): proves the cold-start countdown — a spectator seated
 * before the delayed ring holds a full specDelayTicks of history gets a
 * CHANNEL_CONTROL countdown (remaining = delay - history) and no seed, the
 * remaining strictly decreases as ticks accrue, no seed ever arms during the
 * wait (no live-state leak), and once head - delay becomes seekable the
 * spectator leaves countdown and the normal seed arms — read via
 * transportUdpServerGetSpectatorCountdown / GetSpectatorSeed. A large delay with
 * only a few ticks pumped before seating makes the seek genuinely cold-start.
 *
 * The loopback harness stands up the real UDP server (and its background recv
 * thread); recording is added on top of it the way a real MP host gets it —
 * logStart() (so logWriteTick records ring ticks) plus
 * serverInstanceCreateSpectatorRing() once the spectator cap is non-zero. The
 * spectator JOIN rides the same hand-built raw-socket cookie handshake as
 * test_spectator_join.c.
 *
 * Each case is skipped (logged, test passes) when the distinct loopback source
 * IP a spectator needs for its own join rate-limit bucket isn't bindable
 * (Linux-only 127/8 aliases).
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"     /* JOIN_FLAG_SPECTATOR, MAX_SPECTATORS, spectator accessors */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "server_sim.h"        /* serverSimSetMaxSpectators, serverSimGetSpecDelayTicks */
#include "server_lifecycle.h"  /* serverInstanceCreateSpectatorRing, GetSpectatorRing */
#include "spectator_ring.h"    /* spectatorRingSeekDelayed, spectatorRingCursorKeyframe */
#include "bulk_transfer.h"     /* BULK_KIND_SPEC_SEED, BULK_KIND_SPEC_RECORD */
#include "log.h"               /* logCreate, logStart, logStop, logDestroy */
#include "test_harness.h"
#include "loopback_harness.h"

#define SS_REPLY_DEADLINE_MS 10000
#define SS_BOUNDARY_RETRIES  4
#define SS_COOKIE_WINDOW_SEC 16

static uint64_t ssCookieWindow(void) {
    return (SDL_GetTicks() / 1000ULL) / SS_COOKIE_WINDOW_SEC;
}

/* Build a JOIN_REQUEST with the server's version triple, a valid name, the
 * given flags byte, and a trailing cookie (given bytes, or zeros if NULL). */
static int ssBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
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

    buf[pos++] = flags;
    buf[pos++] = 0;   /* clientType */
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

static SOCKET ssOpenSocketOnIp(const char *ip) {
    struct sockaddr_in addr;
    unsigned long nonblock = 1;
    SOCKET s = socket(AF_INET, SOCK_DGRAM, 0);
    if (s == INVALID_SOCKET) return INVALID_SOCKET;
    memset(&addr, 0, sizeof(addr));
    addr.sin_family      = AF_INET;
    addr.sin_addr.s_addr = inet_addr(ip);
    addr.sin_port        = 0;
    if (bind(s, (struct sockaddr *)&addr, sizeof(addr)) != 0) {
        closesocket(s);
        return INVALID_SOCKET;
    }
    ioctlsocket(s, FIONBIO, &nonblock);
    return s;
}

/* Send one JOIN and pump, capturing whether an ACCEPT and/or CHALLENGE arrived
 * and (on a challenge) its cookie. */
static void ssDriveOnce(LoopbackHarness *h, SOCKET sock,
                        const uint8_t *join, int joinLen,
                        const struct sockaddr_in *server,
                        bool *gotAccept, bool *gotChallenge, bool *gotReject,
                        uint8_t *outCookie) {
    Uint64 deadline;
    *gotAccept    = false;
    *gotChallenge = false;
    *gotReject    = false;
    sendto(sock, (const char *)join, joinLen, 0,
           (const struct sockaddr *)server, sizeof(*server));
    deadline = SDL_GetTicks() + SS_REPLY_DEADLINE_MS;
    while (SDL_GetTicks() < deadline) {
        uint8_t in[1024];
        int n;
        bool drained = false;
        loopbackHarnessPump(h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in), server)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT && n > PACKET_HEADER_SIZE) {
                *gotAccept = true;
            } else if (type == PACKET_JOIN_REJECT) {
                *gotReject = true;
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                *gotChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
            drained = true;
        }
        if (*gotAccept || *gotReject) return;
        if (*gotChallenge) return;
        if (!drained) SDL_Delay(1);
    }
}

/* Drive a full spectator handshake (cookieless -> challenge -> echo). Returns
 * true on JOIN_ACCEPT. Retries only when a cookie ages out of its window. */
static bool ssHandshakeAccepts(LoopbackHarness *h, SOCKET s,
                               const struct sockaddr_in *server,
                               const char *name) {
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    bool gotAccept, gotChallenge, gotReject;
    int attempt;

    for (attempt = 0; attempt < SS_BOUNDARY_RETRIES; attempt++) {
        uint64_t w0 = ssCookieWindow();
        int joinLen = ssBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, NULL);
        ssDriveOnce(h, s, joinBuf, joinLen, server,
                    &gotAccept, &gotChallenge, &gotReject, cookie);
        if (gotAccept) return true;          /* accepted with no cookie?? */
        if (gotReject) return false;
        if (!gotChallenge) return false;

        joinLen = ssBuildJoin(joinBuf, name, JOIN_FLAG_SPECTATOR, cookie);
        ssDriveOnce(h, s, joinBuf, joinLen, server,
                    &gotAccept, &gotChallenge, &gotReject, NULL);
        if (gotAccept) return true;
        if (gotReject) return false;
        if (ssCookieWindow() - w0 >= 2) continue;  /* cookie aged out — retry */
        return false;
    }
    return false;
}

/* The spectator slot is up and its seed has been armed. */
static bool ssSeedArmed(LoopbackHarness *h, void *user) {
    int i;
    (void)h;
    (void)user;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (transportUdpServerGetSpectatorSeed(i, NULL, NULL) != NULL) {
            return true;
        }
    }
    return false;
}

static int t_seed_arm(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    char fname[64];
    SpectatorRing *ring;
    SpectatorRingCursor cur;
    const uint8_t *seed, *kf;
    uint32_t seedLen = 0;
    uint8_t seedKind = 0;
    int kfLen = 0;
    int i, specSlot, armed;

    spec = ssOpenSocketOnIp("127.0.0.20");
    if (spec == INVALID_SOCKET) {
        SDL_Log("  spectator seed: skipping — 127.0.0.20 not bindable");
        return 0;
    }

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* Turn on spectating and stand up a recording ring on top of the running
     * server. specDelayTicks stays at its default 0, so the seed target is the
     * head keyframe — available as soon as the ring holds a keyframe, without
     * driving thousands of ticks. */
    serverSimSetMaxSpectators(h.sim, 4);
    snprintf(fname, sizeof(fname), "test_spectator_seed.wbv");
    remove(fname);
    logCreate();
    UT_ASSERT_MSG(logStart(fname, h.sim, 0, MAX_TANKS, FALSE) == TRUE,
                  "logStart failed");
    serverInstanceCreateSpectatorRing(h.sim);
    ring = serverInstanceGetSpectatorRing();
    UT_ASSERT_MSG(ring != NULL, "ring not created for maxSpectators > 0");

    /* Accumulate ring history (each serverInstanceTick records one tick; the
     * first is a keyframe), then freeze the ring so the serve-path seek and the
     * cross-check seek below read an identical, static ring. */
    loopbackHarnessPumpUntil(&h, 60, NULL, NULL);
    UT_ASSERT_MSG(spectatorRingHeadSeq(ring) > 0, "ring recorded no ticks");
    logStop();

    /* Seat the spectator over the raw-socket cookie handshake. */
    if (!ssHandshakeAccepts(&h, spec, &serverAddr, "SpecSeed")) {
        closesocket(spec);
        logDestroy();
        loopbackHarnessStop(&h);
        remove(fname);
        UT_FAIL("spectator JOIN drew no accept");
    }
    UT_ASSERT_MSG(transportUdpServerGetSpectatorCount() == 1,
                  "accepted spectator must be counted once");

    /* Let the serve path seek + copy + arm the seed (the raw socket never acks
     * CHANNEL_BULK, so the seed stays armed — it never completes/frees). */
    armed = loopbackHarnessPumpUntil(&h, 120, ssSeedArmed, NULL);
    UT_ASSERT_MSG(armed > 0, "spectator seed never armed");

    specSlot = -1;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (transportUdpServerGetSpectatorSeed(i, NULL, NULL) != NULL) {
            specSlot = i;
            break;
        }
    }
    UT_ASSERT_MSG(specSlot >= 0, "no spectator slot carries an armed seed");

    seed = transportUdpServerGetSpectatorSeed(specSlot, &seedLen, &seedKind);
    UT_ASSERT_MSG(seed != NULL && seedLen > 0, "armed seed blob is empty");
    UT_ASSERT_MSG(seedKind == BULK_KIND_SPEC_SEED,
                  "armed transfer kind != BULK_KIND_SPEC_SEED (%u)",
                  (unsigned)seedKind);

    /* Cross-check: an independent seek of the (frozen) ring at the same delay
     * yields the byte-identical keyframe the serve path armed. */
    UT_ASSERT_MSG(spectatorRingSeekDelayed(ring,
                      serverSimGetSpecDelayTicks(h.sim), &cur)
                  == SPECTATOR_RING_OK,
                  "ring seek at the configured delay was not OK");
    kf = spectatorRingCursorKeyframe(&cur, &kfLen, NULL);
    UT_ASSERT_MSG(kf != NULL && kfLen > 0, "ring keyframe at delay is empty");
    UT_ASSERT_MSG((uint32_t)kfLen == seedLen,
                  "seed length %u != ring keyframe length %d", seedLen, kfLen);
    UT_ASSERT_MSG(memcmp(seed, kf, (size_t)kfLen) == 0,
                  "armed seed blob != ring keyframe at head - delay");

    closesocket(spec);
    logDestroy();           /* tap already idle (logStop); shutdown unregisters it */
    loopbackHarnessStop(&h);
    remove(fname);
    return 0;
}

/* Forward-feed anti-cheat lag (2d-e). At delay > 0 the feed cursor settles at
 * head - delay and never reaches the live head; at delay = 0 it equals head.
 * ip selects the spectator's loopback rate-limit bucket; delay is in recordSeq. */
static int t_feed_lag(const char *ip, uint32_t delay) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    char fname[64];
    SpectatorRing *ring;
    int i, specSlot, reached;
    uint32_t led = 0, head = 0, tgt = 0;
    uint8_t kind = 0;

    spec = ssOpenSocketOnIp(ip);
    if (spec == INVALID_SOCKET) {
        SDL_Log("  spectator feed: skipping (delay=%u) — %s not bindable",
                (unsigned)delay, ip);
        return 0;
    }

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* Enable spectating and set the delay BEFORE creating the ring, so the
     * ring's retention is sized from this delay (2d-b sizes it at create). */
    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, delay);
    snprintf(fname, sizeof(fname), "test_spectator_feed_%u.wbv", (unsigned)delay);
    remove(fname);
    logCreate();
    UT_ASSERT_MSG(logStart(fname, h.sim, 0, MAX_TANKS, FALSE) == TRUE,
                  "logStart failed");
    serverInstanceCreateSpectatorRing(h.sim);
    ring = serverInstanceGetSpectatorRing();
    UT_ASSERT_MSG(ring != NULL, "ring not created for maxSpectators > 0");

    /* Build ring history so head - delay is seekable; the ring keeps recording
     * (no freeze) for the rest of the test so the head advances under the feed. */
    loopbackHarnessPumpUntil(&h, 80 + (int)delay, NULL, NULL);
    UT_ASSERT_MSG(spectatorRingHeadSeq(ring) > delay,
                  "ring head did not pass the delay");

    /* Seat the spectator. */
    if (!ssHandshakeAccepts(&h, spec, &serverAddr, "SpecFeed")) {
        closesocket(spec);
        logStop();
        logDestroy();
        loopbackHarnessStop(&h);
        remove(fname);
        UT_FAIL("spectator JOIN drew no accept (delay=%u)", (unsigned)delay);
    }
    specSlot = -1;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (transportUdpServerGetSpectatorFeedSeq(i, NULL, NULL)) {
            specSlot = i;
            break;
        }
    }
    UT_ASSERT_MSG(specSlot >= 0, "no connected spectator slot found");

    /* Pump while acking all bulk each tick (a keeping-up peer): the seed
     * completes, then the feed catches up. Stop once the feed cursor reaches
     * head - delay (and has advanced past the seed at recordSeq 0). */
    reached = 0;
    for (i = 0; i < 600; i++) {
        loopbackHarnessPump(&h);
        transportUdpServerTestSpectatorAckBulk(specSlot);
        if (!transportUdpServerGetSpectatorFeedSeq(specSlot, &led, &kind)) {
            continue;
        }
        head = spectatorRingHeadSeq(ring);
        tgt = (head > delay) ? head - delay : 0;
        if (led == tgt && led > 0) {
            reached = i + 1;
            break;
        }
    }
    UT_ASSERT_MSG(reached > 0,
                  "feed never caught up to head-delay (delay=%u, led=%u, head=%u)",
                  (unsigned)delay, (unsigned)led, (unsigned)head);

    /* Re-read the invariant on the now-static cursor (no pump since the match). */
    UT_ASSERT(transportUdpServerGetSpectatorFeedSeq(specSlot, &led, &kind));
    head = spectatorRingHeadSeq(ring);
    if (delay > 0) {
        UT_ASSERT_MSG(led == head - delay,
                      "feed lag != delay: led=%u head=%u delay=%u",
                      (unsigned)led, (unsigned)head, (unsigned)delay);
        UT_ASSERT_MSG(led < head,
                      "feed reached the live head (anti-cheat broken): led=%u head=%u",
                      (unsigned)led, (unsigned)head);
    } else {
        UT_ASSERT_MSG(led == head,
                      "delay=0 feed not at head: led=%u head=%u",
                      (unsigned)led, (unsigned)head);
    }
    UT_ASSERT_MSG(kind == BULK_KIND_SPEC_RECORD,
                  "forward feed in-flight kind != BULK_KIND_SPEC_RECORD (%u)",
                  (unsigned)kind);

    closesocket(spec);
    logStop();
    logDestroy();
    loopbackHarnessStop(&h);
    remove(fname);
    return 0;
}

/* Cold-start countdown (2d-f). A spectator that joins before the delayed ring
 * holds a full specDelayTicks of history is put in a CHANNEL_CONTROL countdown
 * (remaining = delay - history) with no seed armed, until head - delay becomes
 * seekable — at which point it leaves countdown and the normal seed arms. The
 * delay is large and only a few ticks are pumped before seating, so the seek
 * genuinely cold-starts rather than finding enough history already recorded.
 * ip selects the spectator's loopback rate-limit bucket. */
static int t_cold_start(const char *ip, uint32_t delay) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    SOCKET spec;
    char fname[64];
    SpectatorRing *ring;
    int i, specSlot, armed;
    uint32_t rem0 = 0, rem = 0, remPrev;
    bool sawDecrease = false, leftCountdown = false;

    spec = ssOpenSocketOnIp(ip);
    if (spec == INVALID_SOCKET) {
        SDL_Log("  spectator cold-start: skipping — %s not bindable", ip);
        return 0;
    }

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Player", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* Set the delay BEFORE creating the ring so retention is sized to cover it
     * (2d-b). A large delay keeps the ring short of a full delay of history
     * through the seat, so the spectator's first seek cold-starts. */
    serverSimSetMaxSpectators(h.sim, 4);
    serverSimSetSpecDelayTicks(h.sim, delay);
    snprintf(fname, sizeof(fname), "test_spectator_cold_%u.wbv", (unsigned)delay);
    remove(fname);
    logCreate();
    UT_ASSERT_MSG(logStart(fname, h.sim, 0, MAX_TANKS, FALSE) == TRUE,
                  "logStart failed");
    serverInstanceCreateSpectatorRing(h.sim);
    ring = serverInstanceGetSpectatorRing();
    UT_ASSERT_MSG(ring != NULL, "ring not created for maxSpectators > 0");

    /* Only a few ticks of history — far short of the delay — before seating. */
    loopbackHarnessPumpUntil(&h, 10, NULL, NULL);
    UT_ASSERT_MSG(spectatorRingHeadSeq(ring) < delay,
                  "ring already holds a full delay before seating");

    if (!ssHandshakeAccepts(&h, spec, &serverAddr, "SpecCold")) {
        closesocket(spec);
        logStop();
        logDestroy();
        loopbackHarnessStop(&h);
        remove(fname);
        UT_FAIL("spectator JOIN drew no accept (delay=%u)", (unsigned)delay);
    }

    /* The seek runs each tick once connected: find the slot in countdown. */
    specSlot = -1;
    for (i = 0; i < MAX_SPECTATORS; i++) {
        if (transportUdpServerGetSpectatorCountdown(i, &rem0)) {
            specSlot = i;
            break;
        }
    }
    UT_ASSERT_MSG(specSlot >= 0,
                  "spectator not in cold-start countdown (delay=%u head=%u)",
                  (unsigned)delay, (unsigned)spectatorRingHeadSeq(ring));
    UT_ASSERT_MSG(rem0 > 0 && rem0 <= delay,
                  "countdown remaining out of range: rem0=%u delay=%u",
                  (unsigned)rem0, (unsigned)delay);
    UT_ASSERT_MSG(transportUdpServerGetSpectatorSeed(specSlot, NULL, NULL) == NULL,
                  "seed armed during countdown (live-state leak)");

    /* Pump: remaining must strictly decrease and no seed may arm while waiting,
     * until the seek flips to OK and the spectator leaves countdown. */
    remPrev = rem0;
    for (i = 0; i < 800; i++) {
        loopbackHarnessPump(&h);
        if (transportUdpServerGetSpectatorCountdown(specSlot, &rem)) {
            UT_ASSERT_MSG(
                transportUdpServerGetSpectatorSeed(specSlot, NULL, NULL) == NULL,
                "seed armed mid-countdown (live-state leak): rem=%u",
                (unsigned)rem);
            if (rem < remPrev) sawDecrease = true;
            remPrev = rem;
        } else {
            leftCountdown = true;
            break;
        }
    }
    UT_ASSERT_MSG(sawDecrease,
                  "countdown remaining never decreased (rem0=%u last=%u)",
                  (unsigned)rem0, (unsigned)remPrev);
    UT_ASSERT_MSG(leftCountdown,
                  "spectator never left countdown (last rem=%u delay=%u)",
                  (unsigned)remPrev, (unsigned)delay);

    /* Transition: a seed now arms. seedBlob is copied the tick the seek returns
     * OK; allow one carrier pump to settle if the read just missed it. */
    armed = ssSeedArmed(&h, NULL) ? 1 : 0;
    if (!armed) {
        armed = loopbackHarnessPumpUntil(&h, 5, ssSeedArmed, NULL);
    }
    UT_ASSERT_MSG(armed > 0, "no seed armed after leaving countdown");
    {
        uint32_t seedLen = 0;
        const uint8_t *seed =
            transportUdpServerGetSpectatorSeed(specSlot, &seedLen, NULL);
        UT_ASSERT_MSG(seed != NULL && seedLen > 0,
                      "seed empty after leaving countdown (seedLen=%u)",
                      (unsigned)seedLen);
    }
    UT_ASSERT_MSG(!transportUdpServerGetSpectatorCountdown(specSlot, NULL),
                  "still in countdown after the seed armed");

    closesocket(spec);
    logStop();
    logDestroy();
    loopbackHarnessStop(&h);
    remove(fname);
    return 0;
}

int run_spectator_seed(void) {
    if (t_seed_arm()) {
        return 1;
    }
    if (t_feed_lag("127.0.0.21", /*delay*/ 20)) {
        return 1;
    }
    if (t_feed_lag("127.0.0.22", /*delay*/ 0)) {
        return 1;
    }
    if (t_cold_start("127.0.0.23", /*delay*/ 400)) {
        return 1;
    }
    return 0;
}
