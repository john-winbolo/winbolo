/*
 * Rejoin on a locked game.
 *
 * While the game is locked (the server lock or "Allow New Players" off), a
 * JOIN with JOIN_FLAG_WANT_REJOIN, whose name has a live rejoin record, is
 * seated when the joiner's IPv4 address is the one the player had when
 * leaving. When the address is another one, and the player was signed in
 * to WinBolo.net when leaving, a joiner that sets JOIN_FLAG_WILL_AUTHENTICATE
 * and JOIN_FLAG_SIGN_IN_FIRST is checked with WinBolo.net before it is
 * seated: the server answers PACKET_JOIN_SIGN_IN, the joiner sends the JOIN
 * again with a player_key, and the verify runs on the WinBolo.net worker.
 * Nobody is seated while it runs. Only a key that WinBolo.net accepts under
 * the record's name gets the seat. Any other join gets the game-locked
 * reject, as before.
 *
 * Each test stands up the loopback harness with a running server, locks
 * the game, puts a rejoin record in the table, and drives hand-built
 * JOIN_REQUESTs from a raw UDP socket on 127.0.0.1, with the address-proof
 * cookie the server asks for. The WinBolo.net verify result is given by a
 * test seam, in place of the worker:
 *   join_locked_rejoin_address_seats       — same address: seated, record
 *                                            used, no WinBolo.net check
 *   join_locked_rejoin_no_match_rejects    — other address, and no
 *                                            WinBolo.net match (record not
 *                                            signed in, no will-auth, or a
 *                                            client without the sign-in
 *                                            flag): the game-locked reject
 *   join_locked_rejoin_wbn_sign_in_not_seated — signed-in record: the
 *                                            sign-in reply, no seat, and
 *                                            the check runs with no seat
 *   join_locked_rejoin_wbn_verified_seats  — WinBolo.net accepts the key
 *                                            under the record's name:
 *                                            seated, verified, record used,
 *                                            one keyed join announcement
 *   join_locked_rejoin_wbn_fail_rejects    — the verify fails: reject, no
 *                                            seat, record kept
 *   join_locked_rejoin_wbn_other_name_rejects — the key is another
 *                                            player's: reject, no seat,
 *                                            record kept
 *   join_locked_rejoin_wbn_timeout_rejects — no answer in time: reject, no
 *                                            seat, record kept, the owner
 *                                            rejoins
 *   join_locked_rejoin_ask_gets_question   — the ask flag and a match: the
 *                                            rejoin question; "Join as new"
 *                                            and no match: the reject
 *   join_locked_rejoin_unlocked_unchanged  — not locked: no address check
 *                                            and no WinBolo.net check
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE, MAX_TANKS */
#include "platform_net.h"      /* sockets, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_*, PACKET_HEADER_SIZE, BOLO_VERSION_* */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_*, test seams */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "game_sim.h"          /* GameSim */
#include "players.h"           /* playersGetClientFlags, PLAYER_FLAG_WBN_VERIFIED */
#include "server_sim.h"        /* serverSimIsPlayerConnected, serverSimGetGameSim */
#include "server_sim_lifecycle.h" /* serverSimSetAllowNewPlayers */
#include "playersrejoin.h"     /* playersRejoinAddPlayer, playersRejoinSetAddress */
#include "gui/lang.h"          /* STR_REJECT_GAME_LOCKED, STR_REJECT_CAN_REJOIN */
#include "threads.h"           /* threadsWaitForMutex, threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

/* The winbolonetAddEvent PLAYER_JOIN spy in test_stubs.c. */
extern int      wbnStubJoinEventCalls;
extern uint16_t wbnStubJoinEventMask;
extern char     wbnStubLastJoinKey[];

/* The reply to a JOIN comes on the next server tick, so this is ample. */
#define LR_PUMP_MAX 200

/* The name of the player the rejoin record is for. */
#define LR_NAME "Back"

/* An address that is not the test's 127.0.0.1. */
#define LR_OTHER_ADDR "10.1.2.3"

/* The team kept in the record, to show the record is not changed. */
#define LR_TEAM 3

/* The player_key the joiner sends after the sign-in reply. */
#define LR_KEY "0123456789abcdef0123456789abcdef"

/* The flags of a signed-in GUI client's rejoin. */
#define LR_WBN_FLAGS (JOIN_FLAG_WANT_REJOIN | JOIN_FLAG_WILL_AUTHENTICATE | \
                      JOIN_FLAG_SIGN_IN_FIRST)

/* What one JOIN drew from the server. */
typedef struct {
    bool     accept;
    bool     reject;
    bool     challenge;
    bool     signIn;
    uint16_t langid;
    uint8_t  cookie[JOIN_COOKIE_LEN];
} LrReply;

/* A JOIN_REQUEST with the given flags byte, key (empty when NULL) and
 * cookie (zeros when NULL), laid out as transport_udp_client.c builds it.
 * Returns its length. */
static int lrBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
                       const char *keyOrNull, const uint8_t *cookieOrNull) {
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
    memset(buf + pos, 0, WBN_JOIN_KEY_WIRE_LEN);
    if (keyOrNull != NULL) {
        strncpy((char *)(buf + pos), keyOrNull, WBN_JOIN_KEY_WIRE_LEN - 1);
    }
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

/* A non-blocking UDP socket on an ephemeral 127.0.0.1 port. */
static SOCKET lrOpenSocket(void) {
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

static void lrServerAddr(const LoopbackHarness *h, struct sockaddr_in *a) {
    memset(a, 0, sizeof(*a));
    a->sin_family      = AF_INET;
    a->sin_addr.s_addr = inet_addr("127.0.0.1");
    a->sin_port        = htons(h->port);
}

/* Reads what came to sock into r. */
static void lrRead(SOCKET sock, const struct sockaddr_in *serverAddr,
                   LrReply *r) {
    uint8_t in[1024];
    int n;
    while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                       serverAddr)) > 0) {
        uint8_t type = getPacketType(in, n);
        if (type == PACKET_JOIN_ACCEPT) {
            r->accept = true;
        } else if (type == PACKET_JOIN_REJECT &&
                   n >= PACKET_HEADER_SIZE + 2) {
            r->reject = true;
            r->langid = (uint16_t)((in[PACKET_HEADER_SIZE] << 8) |
                                   in[PACKET_HEADER_SIZE + 1]);
        } else if (type == PACKET_JOIN_CHALLENGE &&
                   n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
            r->challenge = true;
            memcpy(r->cookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
        } else if (type == PACKET_JOIN_SIGN_IN &&
                   n >= PACKET_HEADER_SIZE + WBN_JOIN_KEY_WIRE_LEN) {
            r->signIn = true;
        }
    }
}

/* Sends the JOIN (again every few pumps) and pumps the harness until the
 * server answers. */
static void lrDrive(LoopbackHarness *h, SOCKET sock, const uint8_t *joinBuf,
                    int joinLen, const struct sockaddr_in *serverAddr,
                    LrReply *r) {
    int i;
    memset(r, 0, sizeof(*r));
    for (i = 0; i < LR_PUMP_MAX; i++) {
        if (i % 8 == 0) {
            sendto(sock, (const char *)joinBuf, joinLen, 0,
                   (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        }
        loopbackHarnessPump(h);
        lrRead(sock, serverAddr, r);
        if (r->accept || r->reject || r->challenge || r->signIn) return;
    }
}

/* A joiner on its own socket, with the cookie the server gave it. */
typedef struct {
    SOCKET  sock;
    uint8_t cookie[JOIN_COOKIE_LEN];
} LrJoiner;

/* Sends one JOIN from j, with the cookie, and reads the answer into r. */
static void lrSend(LoopbackHarness *h, LrJoiner *j, uint8_t flags,
                   const char *keyOrNull, LrReply *r) {
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    int joinLen;
    lrServerAddr(h, &serverAddr);
    joinLen = lrBuildJoin(joinBuf, LR_NAME, flags, keyOrNull, j->cookie);
    lrDrive(h, j->sock, joinBuf, joinLen, &serverAddr, r);
    fprintf(stderr, "  join flags=0x%02x key=%d: accept=%d reject=%d "
            "langid=%u signIn=%d\n", (unsigned)flags,
            keyOrNull != NULL ? 1 : 0, (int)r->accept, (int)r->reject,
            (unsigned)r->langid, (int)r->signIn);
}

/* One full JOIN with the given flags: the first JOIN draws the cookie
 * challenge, and the second echoes the cookie. r holds the answer to the
 * second. The joiner stays open in j, for later JOINs. Returns false when
 * no challenge came. */
static bool lrJoinOpen(LoopbackHarness *h, uint8_t flags, LrReply *r,
                       LrJoiner *j) {
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    int joinLen;

    memset(j, 0, sizeof(*j));
    lrServerAddr(h, &serverAddr);
    j->sock = lrOpenSocket();
    if (j->sock == INVALID_SOCKET) return false;
    joinLen = lrBuildJoin(joinBuf, LR_NAME, flags, NULL, NULL);
    lrDrive(h, j->sock, joinBuf, joinLen, &serverAddr, r);
    if (!r->challenge) {
        closesocket(j->sock);
        j->sock = INVALID_SOCKET;
        return false;
    }
    memcpy(j->cookie, r->cookie, JOIN_COOKIE_LEN);
    lrSend(h, j, flags, NULL, r);
    return true;
}

/* lrJoinOpen, with the socket closed afterwards. */
static bool lrJoin(LoopbackHarness *h, uint8_t flags, LrReply *r) {
    LrJoiner j;
    if (!lrJoinOpen(h, flags, r, &j)) return false;
    closesocket(j.sock);
    return true;
}

/* How many seats are connected. */
static int lrSeats(LoopbackHarness *h) {
    int i, n = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(h->sim, (BYTE)i)) n++;
    }
    return n;
}

/* True once the harness client has a seat. */
static bool lrClientSeated(LoopbackHarness *h, void *user) {
    (void)user;
    return lrSeats(h) >= 1;
}

/* Lock the game the way real callers do: under the server mutex. */
static void lrSetLock(LoopbackHarness *h, bool locked) {
    threadsWaitForMutex();
    transportUdpServerSetLock(h->sim, locked);
    threadsReleaseMutex();
}

/* Starts a running server with its own client seated, an empty rejoin
 * table, and the game locked by the server lock when lockIt. */
static bool lrStart(LoopbackHarness *h, bool lockIt) {
    if (!loopbackHarnessStart(h, "Joiner", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, /*seed*/ 1u)) {
        return false;
    }
    if (loopbackHarnessPumpUntil(h, LR_PUMP_MAX, lrClientSeated, NULL) < 0) {
        loopbackHarnessStop(h);
        return false;
    }
    playersRejoinCreate();
    if (lockIt) lrSetLock(h, true);
    return true;
}

static void lrStop(LoopbackHarness *h) {
    transportUdpServerSetLockedRejoinWbnForTest(false);
    lrSetLock(h, false);
    loopbackHarnessStop(h);
    playersRejoinCreate();
}

/* A record for LR_NAME with team LR_TEAM, signed in or not, and the given
 * address. */
static void lrAddRecord(bool signedIn, const char *addr) {
    playersRejoinAddPlayer(LR_NAME, 0, 0, LR_TEAM, signedIn);
    playersRejoinSetAddress(LR_NAME, signedIn, (uint32_t)inet_addr(addr));
}

/* True when the record for LR_NAME is still as lrAddRecord made it. */
static bool lrRecordKept(bool signedIn, const char *addr) {
    BYTE team;
    bool recSignedIn;
    if (!playersRejoinFind(LR_NAME, &team, &recSignedIn)) return false;
    return team == LR_TEAM && recSignedIn == signedIn &&
           playersRejoinGetAddress(LR_NAME) == (uint32_t)inet_addr(addr);
}

/* Pumps a few ticks. */
static void lrPumpSome(LoopbackHarness *h, int n) {
    int i;
    for (i = 0; i < n; i++) loopbackHarnessPump(h);
}

/* A bit for each connected seat, so a test can find the seat a JOIN
 * added. */
static uint32_t lrSeatMask(LoopbackHarness *h) {
    int i;
    uint32_t mask = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(h->sim, (BYTE)i)) mask |= 1u << i;
    }
    return mask;
}

/* A record with the joiner's address: the rejoin is seated past the lock
 * and uses the record. A signed-in GUI client gets no WinBolo.net check. */
int run_join_locked_rejoin_address_seats(void) {
    LoopbackHarness h;
    LrReply r;
    int seats, now;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    transportUdpServerSetLockedRejoinWbnForTest(true);
    lrAddRecord(FALSE, "127.0.0.1");
    seats = lrSeats(&h);
    if (!lrJoin(&h, LR_WBN_FLAGS, &r)) {
        lrStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.accept || r.signIn) {
        lrStop(&h);
        UT_FAIL("the rejoin was not accepted (reject=%d langid=%u "
                "signIn=%d)", (int)r.reject, (unsigned)r.langid,
                (int)r.signIn);
    }
    now = lrSeats(&h);
    if (now != seats + 1) {
        lrStop(&h);
        UT_FAIL("%d seats after the rejoin, was %d", now, seats);
    }
    if (transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
        lrStop(&h);
        UT_FAIL("an address match is checked with WinBolo.net");
    }
    if (playersRejoinGetAge(LR_NAME) >= 0) {
        lrStop(&h);
        UT_FAIL("the rejoin did not use the record");
    }
    lrStop(&h);
    return 0;
}

/* Another address, with "Allow New Players" off as the lock: an unsigned
 * record with a will-auth joiner, a signed-in record with a joiner that will
 * not authenticate, and a signed-in record with a will-auth joiner that
 * cannot answer the sign-in reply (an older build, headless, Bot Host), all
 * get the game-locked reject, no seat, and the record stays. */
int run_join_locked_rejoin_no_match_rejects(void) {
    static const struct {
        bool    signedIn;
        uint8_t flags;
    } passes[3] = {
        { false, LR_WBN_FLAGS },
        { true,  JOIN_FLAG_WANT_REJOIN | JOIN_FLAG_SIGN_IN_FIRST },
        { true,  JOIN_FLAG_WANT_REJOIN | JOIN_FLAG_WILL_AUTHENTICATE },
    };
    LoopbackHarness h;
    LrReply r;
    int seats, now, pass;

    UT_ASSERT_MSG(lrStart(&h, false), "harness start failed");
    threadsWaitForMutex();
    serverSimSetAllowNewPlayers(h.sim, false);
    threadsReleaseMutex();
    transportUdpServerSetLockedRejoinWbnForTest(true);
    for (pass = 0; pass < 3; pass++) {
        bool signedIn = passes[pass].signedIn;
        playersRejoinCreate();
        lrAddRecord(signedIn, LR_OTHER_ADDR);
        seats = lrSeats(&h);
        if (!lrJoin(&h, passes[pass].flags, &r)) {
            lrStop(&h);
            UT_FAIL("pass %d: the JOIN drew no cookie challenge", pass);
        }
        if (!r.reject || r.langid != STR_REJECT_GAME_LOCKED || r.accept ||
            r.signIn) {
            lrStop(&h);
            UT_FAIL("pass %d: expected the game-locked reject, got accept=%d "
                    "reject=%d langid=%u signIn=%d", pass, (int)r.accept,
                    (int)r.reject, (unsigned)r.langid, (int)r.signIn);
        }
        now = lrSeats(&h);
        if (now != seats) {
            lrStop(&h);
            UT_FAIL("pass %d: %d seats after the reject, was %d", pass, now,
                    seats);
        }
        if (!lrRecordKept(signedIn, LR_OTHER_ADDR)) {
            lrStop(&h);
            UT_FAIL("pass %d: the record changed", pass);
        }
    }
    threadsWaitForMutex();
    serverSimSetAllowNewPlayers(h.sim, true);
    threadsReleaseMutex();
    lrStop(&h);
    return 0;
}

/* A signed-in GUI rejoin for a signed-in record from another address on the
 * locked game, up to the point where the check runs: the first JOIN gets
 * the sign-in reply, and the JOIN with the key gets it again while the
 * check runs. Nobody is seated, and the record is in place. Returns NULL,
 * or what is wrong. The joiner stays open in j. */
static const char *lrStartCheck(LoopbackHarness *h, LrJoiner *j) {
    LrReply r;
    int seats;

    transportUdpServerSetLockedRejoinWbnForTest(true);
    lrAddRecord(TRUE, LR_OTHER_ADDR);
    seats = lrSeats(h);
    if (!lrJoinOpen(h, LR_WBN_FLAGS, &r, j)) {
        return "the JOIN drew no cookie challenge";
    }
    if (!r.signIn || r.accept || r.reject) {
        return "the JOIN without a key did not get the sign-in reply";
    }
    if (transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
        return "a check runs with no key";
    }
    if (lrSeats(h) != seats) return "the JOIN without a key was seated";
    lrSend(h, j, LR_WBN_FLAGS, LR_KEY, &r);
    if (!r.signIn || r.accept || r.reject) {
        return "the JOIN with the key did not get the sign-in reply";
    }
    if (!transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
        return "the JOIN with the key started no check";
    }
    if (lrSeats(h) != seats) return "a seat was given while the check runs";
    if (!lrRecordKept(TRUE, LR_OTHER_ADDR)) {
        return "the record changed while the check runs";
    }
    return NULL;
}

/* After a refused check: the JOIN with the key gets the game-locked reject,
 * nobody is seated, no check runs, and the record is as it was. Returns
 * NULL, or what is wrong. */
static const char *lrCheckRefused(LoopbackHarness *h, LrJoiner *j,
                                  int seatsBefore) {
    LrReply r;
    lrSend(h, j, LR_WBN_FLAGS, LR_KEY, &r);
    if (!r.reject || r.langid != STR_REJECT_GAME_LOCKED || r.accept) {
        return "the JOIN did not get the game-locked reject";
    }
    if (lrSeats(h) != seatsBefore) return "a seat was given";
    if (transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
        return "a check still runs";
    }
    if (!lrRecordKept(TRUE, LR_OTHER_ADDR)) return "the record changed";
    return NULL;
}

/* The sign-in reply and the running check give no seat, for as long as
 * the check runs. */
int run_join_locked_rejoin_wbn_sign_in_not_seated(void) {
    LoopbackHarness h;
    LrJoiner j;
    LrReply r;
    const char *why;
    int seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    why = lrStartCheck(&h, &j);
    if (why == NULL) {
        /* More JOINs while the check runs get the same answer. */
        lrPumpSome(&h, 5);
        lrSend(&h, &j, LR_WBN_FLAGS, LR_KEY, &r);
        if (!r.signIn || r.accept || r.reject) {
            why = "a later JOIN did not get the sign-in reply";
        } else if (lrSeats(&h) != seats) {
            why = "a later JOIN was seated";
        } else if (!transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
            why = "the check stopped";
        }
    }
    if (j.sock != INVALID_SOCKET) closesocket(j.sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    lrStop(&h);
    return 0;
}

/* WinBolo.net accepts the key under the record's name: the next JOIN is
 * seated as a WinBolo.net-verified player, the record is used, and the join
 * is announced to WinBolo.net once, with the key. */
int run_join_locked_rejoin_wbn_verified_seats(void) {
    LoopbackHarness h;
    LrJoiner j;
    LrReply r;
    const char *why;
    uint32_t before, added;
    int slot = -1, i;
    uint8_t flags;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    before = lrSeatMask(&h);
    why = lrStartCheck(&h, &j);
    if (why == NULL) {
        threadsWaitForMutex();
        if (!transportUdpServerLockedSignInResultForTest(LR_NAME, LR_NAME)) {
            why = "the result found no check";
        }
        threadsReleaseMutex();
    }
    wbnStubJoinEventCalls = 0;
    wbnStubJoinEventMask = 0;
    wbnStubLastJoinKey[0] = '\0';
    if (why == NULL) {
        lrSend(&h, &j, LR_WBN_FLAGS, LR_KEY, &r);
        if (!r.accept) why = "the verified rejoin was not accepted";
    }
    if (why == NULL) lrPumpSome(&h, 20);
    if (j.sock != INVALID_SOCKET) closesocket(j.sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    added = lrSeatMask(&h) & ~before;
    for (i = 0; i < MAX_TANKS; i++) {
        if (added == (1u << i)) slot = i;
    }
    if (slot < 0) {
        lrStop(&h);
        UT_FAIL("the verified rejoin did not add exactly one seat "
                "(mask 0x%x)", (unsigned)added);
    }
    flags = playersGetClientFlags(&serverSimGetGameSim(h.sim)->plyrs,
                                  (BYTE)slot);
    if ((flags & PLAYER_FLAG_WBN_VERIFIED) == 0) {
        lrStop(&h);
        UT_FAIL("the seat is not WinBolo.net verified (flags 0x%02x)",
                (unsigned)flags);
    }
    if (transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
        lrStop(&h);
        UT_FAIL("the check still runs after the seat");
    }
    if (wbnStubJoinEventCalls != 1 ||
        wbnStubJoinEventMask != (uint16_t)(1u << slot) ||
        strcmp(wbnStubLastJoinKey, LR_KEY) != 0) {
        lrStop(&h);
        UT_FAIL("expected one keyed join announcement for slot %d, got %d "
                "(mask 0x%x, key '%s')", slot, wbnStubJoinEventCalls,
                (unsigned)wbnStubJoinEventMask, wbnStubLastJoinKey);
    }
    if (playersRejoinGetAge(LR_NAME) >= 0) {
        lrStop(&h);
        UT_FAIL("the verified rejoin did not get the record");
    }
    lrStop(&h);
    return 0;
}

/* The verify fails: reject, no seat, record kept. */
int run_join_locked_rejoin_wbn_fail_rejects(void) {
    LoopbackHarness h;
    LrJoiner j;
    const char *why;
    int seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    why = lrStartCheck(&h, &j);
    if (why == NULL) {
        threadsWaitForMutex();
        if (!transportUdpServerLockedSignInResultForTest(LR_NAME, NULL)) {
            why = "the result found no check";
        }
        threadsReleaseMutex();
    }
    if (why == NULL) why = lrCheckRefused(&h, &j, seats);
    if (j.sock != INVALID_SOCKET) closesocket(j.sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    lrStop(&h);
    return 0;
}

/* The key belongs to another player, so WinBolo.net does not accept it
 * under the record's name: reject, no seat, record kept. */
int run_join_locked_rejoin_wbn_other_name_rejects(void) {
    LoopbackHarness h;
    LrJoiner j;
    const char *why;
    int seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    why = lrStartCheck(&h, &j);
    if (why == NULL) {
        threadsWaitForMutex();
        if (!transportUdpServerLockedSignInResultForTest(LR_NAME,
                                                         "Somebody")) {
            why = "the result found no check";
        }
        threadsReleaseMutex();
    }
    if (why == NULL) why = lrCheckRefused(&h, &j, seats);
    if (j.sock != INVALID_SOCKET) closesocket(j.sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    lrStop(&h);
    return 0;
}

/* No answer before the check lapses: reject, no seat, record kept. Then the
 * owner, from the address the record has, rejoins. */
int run_join_locked_rejoin_wbn_timeout_rejects(void) {
    LoopbackHarness h;
    LrJoiner j;
    LrReply r;
    const char *why;
    int seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    why = lrStartCheck(&h, &j);
    if (why == NULL) {
        threadsWaitForMutex();
        transportUdpServerExpireLockedSignInForTest(LR_NAME);
        threadsReleaseMutex();
        why = lrCheckRefused(&h, &j, seats);
    }
    if (j.sock != INVALID_SOCKET) closesocket(j.sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    /* The owner comes back from the address the record has. */
    playersRejoinSetAddress(LR_NAME, TRUE, (uint32_t)inet_addr("127.0.0.1"));
    if (!lrJoin(&h, LR_WBN_FLAGS, &r) || !r.accept) {
        lrStop(&h);
        UT_FAIL("the owner could not rejoin after the timeout "
                "(reject=%d langid=%u signIn=%d)", (int)r.reject,
                (unsigned)r.langid, (int)r.signIn);
    }
    /* The record is signed in, so as for any signed-in rejoin it stays held
     * for the owner's WinBolo.net sign-in. */
    if (!lrRecordKept(TRUE, "127.0.0.1")) {
        lrStop(&h);
        UT_FAIL("the record is not held for the owner's sign-in");
    }
    lrStop(&h);
    return 0;
}

/* The ask flag, on the locked game. With an address match, and with a
 * signed-in record and a signed-in GUI joiner, the Join gets the rejoin
 * question and no seat. "Join as new" (no ask flag) then gets the
 * game-locked reject. With no match the ask flag gets the reject too. */
int run_join_locked_rejoin_ask_gets_question(void) {
    LoopbackHarness h;
    LrReply r;
    int seats;
    const uint8_t wbnNew = JOIN_FLAG_WILL_AUTHENTICATE | JOIN_FLAG_SIGN_IN_FIRST;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);

    /* Address match. */
    lrAddRecord(FALSE, "127.0.0.1");
    if (!lrJoin(&h, JOIN_FLAG_ASK_REJOIN, &r) || !r.reject ||
        r.langid != STR_REJECT_CAN_REJOIN) {
        lrStop(&h);
        UT_FAIL("address match: no rejoin question (accept=%d reject=%d "
                "langid=%u)", (int)r.accept, (int)r.reject,
                (unsigned)r.langid);
    }
    if (!lrJoin(&h, 0, &r) || !r.reject ||
        r.langid != STR_REJECT_GAME_LOCKED) {
        lrStop(&h);
        UT_FAIL("address match: \"Join as new\" was not rejected as locked "
                "(accept=%d reject=%d langid=%u)", (int)r.accept,
                (int)r.reject, (unsigned)r.langid);
    }

    /* WinBolo.net match. */
    playersRejoinCreate();
    transportUdpServerSetLockedRejoinWbnForTest(true);
    lrAddRecord(TRUE, LR_OTHER_ADDR);
    if (!lrJoin(&h, (uint8_t)(JOIN_FLAG_ASK_REJOIN | wbnNew), &r) ||
        !r.reject || r.langid != STR_REJECT_CAN_REJOIN) {
        lrStop(&h);
        UT_FAIL("WinBolo.net match: no rejoin question (accept=%d reject=%d "
                "langid=%u signIn=%d)", (int)r.accept, (int)r.reject,
                (unsigned)r.langid, (int)r.signIn);
    }
    if (!lrJoin(&h, wbnNew, &r) || !r.reject ||
        r.langid != STR_REJECT_GAME_LOCKED) {
        lrStop(&h);
        UT_FAIL("WinBolo.net match: \"Join as new\" was not rejected as "
                "locked (accept=%d reject=%d langid=%u signIn=%d)",
                (int)r.accept, (int)r.reject, (unsigned)r.langid,
                (int)r.signIn);
    }

    if (lrSeats(&h) != seats) {
        lrStop(&h);
        UT_FAIL("a JOIN was seated (%d seats, was %d)", lrSeats(&h), seats);
    }
    lrStop(&h);

    /* No match: the joiner will not authenticate. This runs on a new
     * server, as the four JOINs above use up the per-address join rate. */
    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    transportUdpServerSetLockedRejoinWbnForTest(true);
    lrAddRecord(TRUE, LR_OTHER_ADDR);
    if (!lrJoin(&h, JOIN_FLAG_ASK_REJOIN | JOIN_FLAG_SIGN_IN_FIRST, &r) ||
        !r.reject || r.langid != STR_REJECT_GAME_LOCKED) {
        lrStop(&h);
        UT_FAIL("no match: the ask flag was not rejected as locked "
                "(accept=%d reject=%d langid=%u)", (int)r.accept,
                (int)r.reject, (unsigned)r.langid);
    }
    if (lrSeats(&h) != seats) {
        lrStop(&h);
        UT_FAIL("a JOIN was seated (%d seats, was %d)", lrSeats(&h), seats);
    }
    if (!lrRecordKept(TRUE, LR_OTHER_ADDR)) {
        lrStop(&h);
        UT_FAIL("the record changed");
    }
    lrStop(&h);
    return 0;
}

/* Not locked. A rejoin from another address is seated and uses an unsigned
 * record: there is no address check. A signed-in GUI rejoin for a signed-in
 * record is seated at once, with no WinBolo.net check, and the record waits
 * for the re-authentication as before. */
int run_join_locked_rejoin_unlocked_unchanged(void) {
    LoopbackHarness h;
    LrReply r;
    int seats;

    UT_ASSERT_MSG(lrStart(&h, false), "harness start failed");
    seats = lrSeats(&h);
    lrAddRecord(FALSE, LR_OTHER_ADDR);
    if (!lrJoin(&h, JOIN_FLAG_WANT_REJOIN, &r) || !r.accept) {
        lrStop(&h);
        UT_FAIL("unlocked: the rejoin was not accepted (reject=%d "
                "langid=%u)", (int)r.reject, (unsigned)r.langid);
    }
    if (lrSeats(&h) != seats + 1) {
        lrStop(&h);
        UT_FAIL("unlocked: the rejoin has no seat");
    }
    if (playersRejoinGetAge(LR_NAME) >= 0) {
        lrStop(&h);
        UT_FAIL("unlocked: the rejoin did not use the record");
    }
    lrStop(&h);

    UT_ASSERT_MSG(lrStart(&h, false), "harness start failed");
    transportUdpServerSetLockedRejoinWbnForTest(true);
    lrAddRecord(TRUE, LR_OTHER_ADDR);
    if (!lrJoin(&h, LR_WBN_FLAGS, &r) || !r.accept || r.signIn) {
        lrStop(&h);
        UT_FAIL("unlocked: the signed-in rejoin was not accepted at once "
                "(reject=%d langid=%u signIn=%d)", (int)r.reject,
                (unsigned)r.langid, (int)r.signIn);
    }
    if (transportUdpServerLockedSignInPendingForTest(LR_NAME)) {
        lrStop(&h);
        UT_FAIL("unlocked: the rejoin is checked with WinBolo.net");
    }
    /* The signed-in record waits for WinBolo.net as before, in place. */
    if (playersRejoinGetAge(LR_NAME) < 0) {
        lrStop(&h);
        UT_FAIL("unlocked: the signed-in record was used at the join");
    }
    lrStop(&h);
    return 0;
}
