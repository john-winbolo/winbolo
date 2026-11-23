/*
 * Rejoin on a locked game.
 *
 * While the game is locked (the server lock or "Allow New Players" off), a
 * JOIN with JOIN_FLAG_WANT_REJOIN, whose name has a live rejoin record, is
 * seated when the joiner's IPv4 address is the one the player had when
 * leaving, or when the player was signed in to WinBolo.net when leaving and
 * the joiner sets JOIN_FLAG_WILL_AUTHENTICATE. In the second case the seat
 * is provisional: WinBolo.net must confirm it under the record's name, or
 * it is removed with the game-locked reason and the record stays for its
 * owner. Any other join gets the game-locked reject, as before.
 *
 * Each test stands up the loopback harness with a running server, locks
 * the game, puts a rejoin record in the table, and drives hand-built
 * JOIN_REQUESTs from a raw UDP socket on 127.0.0.1, with the address-proof
 * cookie the server asks for. The WinBolo.net verify is given by a test
 * seam that takes the same path the re-authentication result takes:
 *   join_locked_rejoin_address_seats      — same address: seated, record used
 *   join_locked_rejoin_no_match_rejects   — other address, and no WinBolo.net
 *                                           match: the game-locked reject
 *   join_locked_rejoin_wbn_confirm_keeps  — signed-in record, will-auth
 *                                           joiner: seated, waits, and stays
 *                                           when WinBolo.net confirms
 *   join_locked_rejoin_wbn_mismatch_removes — WinBolo.net confirms another
 *                                           name: removed, record kept
 *   join_locked_rejoin_wbn_fail_removes   — WinBolo.net turns it down:
 *                                           removed, record kept
 *   join_locked_rejoin_wbn_timeout_removes — no confirm in time: removed,
 *                                           record kept, the owner rejoins
 *   join_locked_rejoin_ask_gets_question  — the ask flag and a match: the
 *                                           rejoin question; "Join as new"
 *                                           and no match: the reject
 *   join_locked_rejoin_unlocked_unchanged — not locked: no address check and
 *                                           no wait, as before
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
#include "server_sim.h"        /* serverSimIsPlayerConnected */
#include "server_sim_lifecycle.h" /* serverSimSetAllowNewPlayers */
#include "playersrejoin.h"     /* playersRejoinAddPlayer, playersRejoinSetAddress */
#include "gui/lang.h"          /* STR_REJECT_GAME_LOCKED, STR_REJECT_CAN_REJOIN */
#include "threads.h"           /* threadsWaitForMutex, threadsReleaseMutex */
#include "test_harness.h"
#include "loopback_harness.h"

/* The reply to a JOIN comes on the next server tick, so this is ample. */
#define LR_PUMP_MAX 200

/* The name of the player the rejoin record is for. */
#define LR_NAME "Back"

/* An address that is not the test's 127.0.0.1. */
#define LR_OTHER_ADDR "10.1.2.3"

/* The team kept in the record, to show the record is not changed. */
#define LR_TEAM 3

/* What one JOIN drew from the server. */
typedef struct {
    bool     accept;
    bool     reject;
    bool     challenge;
    uint16_t langid;
    uint8_t  cookie[JOIN_COOKIE_LEN];
} LrReply;

/* A JOIN_REQUEST with the given flags byte and cookie (zeros when NULL),
 * laid out as transport_udp_client.c builds it. Returns its length. */
static int lrBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
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
    memset(buf + pos, 0, WBN_JOIN_KEY_WIRE_LEN);  /* the key rides empty */
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
        }
    }
}

/* Sends the JOIN (again every few pumps) and pumps the harness until the
 * server answers with an accept, a reject or a challenge. */
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
        if (r->accept || r->reject || r->challenge) return;
    }
}

/* One full JOIN with the given flags: the first JOIN draws the cookie
 * challenge, and the second echoes the cookie. r holds the answer to the
 * second. The socket stays open in *sockOut, for later replies. Returns
 * false when no challenge came. */
static bool lrJoinOpen(LoopbackHarness *h, uint8_t flags, LrReply *r,
                       SOCKET *sockOut) {
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    int joinLen;
    SOCKET sock;

    *sockOut = INVALID_SOCKET;
    lrServerAddr(h, &serverAddr);
    sock = lrOpenSocket();
    if (sock == INVALID_SOCKET) return false;
    joinLen = lrBuildJoin(joinBuf, LR_NAME, flags, NULL);
    lrDrive(h, sock, joinBuf, joinLen, &serverAddr, r);
    if (!r->challenge) {
        closesocket(sock);
        return false;
    }
    memcpy(cookie, r->cookie, JOIN_COOKIE_LEN);
    joinLen = lrBuildJoin(joinBuf, LR_NAME, flags, cookie);
    lrDrive(h, sock, joinBuf, joinLen, &serverAddr, r);
    fprintf(stderr, "  join flags=0x%02x: accept=%d reject=%d langid=%u\n",
            (unsigned)flags, (int)r->accept, (int)r->reject,
            (unsigned)r->langid);
    *sockOut = sock;
    return true;
}

/* lrJoinOpen, with the socket closed afterwards. */
static bool lrJoin(LoopbackHarness *h, uint8_t flags, LrReply *r) {
    SOCKET sock;
    if (!lrJoinOpen(h, flags, r, &sock)) return false;
    closesocket(sock);
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

/* The slot of the locked-game rejoin that waits for WinBolo.net, or -1. */
static int lrPendingSlot(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        if (transportUdpServerLockedRejoinPendingForTest((BYTE)i)) return i;
    }
    return -1;
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
    playersRejoinSetAddress(LR_NAME, (uint32_t)inet_addr(addr));
}

/* True when the record for LR_NAME is still as lrAddRecord made it. */
static bool lrRecordKept(bool signedIn, const char *addr) {
    BYTE team;
    bool recSignedIn;
    if (!playersRejoinFind(LR_NAME, &team, &recSignedIn)) return false;
    return team == LR_TEAM && recSignedIn == signedIn &&
           playersRejoinGetAddress(LR_NAME) == (uint32_t)inet_addr(addr);
}

/* Pumps a few ticks, so a deferred removal is drained. */
static void lrPumpSome(LoopbackHarness *h, int n) {
    int i;
    for (i = 0; i < n; i++) loopbackHarnessPump(h);
}

/* A record with the joiner's address: the rejoin is seated past the lock
 * and uses the record. */
int run_join_locked_rejoin_address_seats(void) {
    LoopbackHarness h;
    LrReply r;
    int seats, now;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    lrAddRecord(FALSE, "127.0.0.1");
    seats = lrSeats(&h);
    if (!lrJoin(&h, JOIN_FLAG_WANT_REJOIN, &r)) {
        lrStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.accept) {
        lrStop(&h);
        UT_FAIL("the rejoin was not accepted (reject=%d langid=%u)",
                (int)r.reject, (unsigned)r.langid);
    }
    now = lrSeats(&h);
    if (now != seats + 1) {
        lrStop(&h);
        UT_FAIL("%d seats after the rejoin, was %d", now, seats);
    }
    if (lrPendingSlot() >= 0) {
        lrStop(&h);
        UT_FAIL("an address match waits for WinBolo.net");
    }
    if (playersRejoinGetAge(LR_NAME) >= 0) {
        lrStop(&h);
        UT_FAIL("the rejoin did not use the record");
    }
    lrStop(&h);
    return 0;
}

/* Another address, with "Allow New Players" off as the lock: an unsigned
 * record with a will-auth joiner, and a signed-in record with a joiner that
 * will not authenticate, both get the game-locked reject, no seat, and the
 * record stays. */
int run_join_locked_rejoin_no_match_rejects(void) {
    LoopbackHarness h;
    LrReply r;
    int seats, now, pass;

    UT_ASSERT_MSG(lrStart(&h, false), "harness start failed");
    threadsWaitForMutex();
    serverSimSetAllowNewPlayers(h.sim, false);
    threadsReleaseMutex();
    transportUdpServerSetLockedRejoinWbnForTest(true);
    for (pass = 0; pass < 2; pass++) {
        bool signedIn = (pass == 1);
        uint8_t flags = (uint8_t)(JOIN_FLAG_WANT_REJOIN |
                                  (signedIn ? 0 : JOIN_FLAG_WILL_AUTHENTICATE));
        playersRejoinCreate();
        lrAddRecord(signedIn, LR_OTHER_ADDR);
        seats = lrSeats(&h);
        if (!lrJoin(&h, flags, &r)) {
            lrStop(&h);
            UT_FAIL("pass %d: the JOIN drew no cookie challenge", pass);
        }
        if (!r.reject || r.langid != STR_REJECT_GAME_LOCKED || r.accept) {
            lrStop(&h);
            UT_FAIL("pass %d: expected the game-locked reject, got accept=%d "
                    "reject=%d langid=%u", pass, (int)r.accept,
                    (int)r.reject, (unsigned)r.langid);
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

/* Seats a will-auth rejoin for a signed-in record from another address on
 * the locked game, and checks it waits with the record in place. Returns
 * the slot, or -1 with *why set. The socket stays open in *sockOut. */
static int lrSeatProvisional(LoopbackHarness *h, SOCKET *sockOut,
                             const char **why) {
    LrReply r;
    int seats, slot;

    transportUdpServerSetLockedRejoinWbnForTest(true);
    lrAddRecord(TRUE, LR_OTHER_ADDR);
    seats = lrSeats(h);
    if (!lrJoinOpen(h, JOIN_FLAG_WANT_REJOIN | JOIN_FLAG_WILL_AUTHENTICATE,
                    &r, sockOut)) {
        *why = "the JOIN drew no cookie challenge";
        return -1;
    }
    if (!r.accept) {
        *why = "the will-auth rejoin was not accepted";
        return -1;
    }
    if (lrSeats(h) != seats + 1) {
        *why = "the will-auth rejoin has no seat";
        return -1;
    }
    slot = lrPendingSlot();
    if (slot < 0) {
        *why = "the seat does not wait for WinBolo.net";
        return -1;
    }
    if (!lrRecordKept(TRUE, LR_OTHER_ADDR)) {
        *why = "the record was used before WinBolo.net confirmed the seat";
        return -1;
    }
    return slot;
}

/* After a removal: the seat is gone, the client was told the game is
 * locked, and the record is as it was. Returns NULL, or what is wrong. */
static const char *lrCheckRemoved(LoopbackHarness *h, SOCKET sock, int slot,
                                  int seatsBefore) {
    struct sockaddr_in serverAddr;
    LrReply r;
    memset(&r, 0, sizeof(r));
    lrServerAddr(h, &serverAddr);
    lrPumpSome(h, 5);
    lrRead(sock, &serverAddr, &r);
    if (serverSimIsPlayerConnected(h->sim, (BYTE)slot)) {
        return "the seat was not removed";
    }
    if (lrSeats(h) != seatsBefore) return "the seat count is wrong";
    if (!r.reject || r.langid != STR_REJECT_GAME_LOCKED) {
        return "the client was not told the game is locked";
    }
    if (lrPendingSlot() >= 0) return "a seat still waits";
    if (!lrRecordKept(TRUE, LR_OTHER_ADDR)) {
        return "the removal changed the record";
    }
    return NULL;
}

/* WinBolo.net confirms the seat under the record's name: it stays, stops
 * waiting, and gets the record. */
int run_join_locked_rejoin_wbn_confirm_keeps(void) {
    LoopbackHarness h;
    SOCKET sock;
    const char *why = NULL;
    int slot;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    slot = lrSeatProvisional(&h, &sock, &why);
    if (slot < 0) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    threadsWaitForMutex();
    transportUdpServerLockedRejoinVerifyForTest(h.sim, (BYTE)slot, true,
                                                LR_NAME);
    threadsReleaseMutex();
    lrPumpSome(&h, 5);
    closesocket(sock);
    if (!serverSimIsPlayerConnected(h.sim, (BYTE)slot)) {
        lrStop(&h);
        UT_FAIL("the confirmed seat was removed");
    }
    if (lrPendingSlot() >= 0) {
        lrStop(&h);
        UT_FAIL("the confirmed seat still waits");
    }
    if (playersRejoinGetAge(LR_NAME) >= 0) {
        lrStop(&h);
        UT_FAIL("the confirmed seat did not get the record");
    }
    lrStop(&h);
    return 0;
}

/* WinBolo.net confirms another name: the seat is removed with the
 * game-locked reason, and the record stays. */
int run_join_locked_rejoin_wbn_mismatch_removes(void) {
    LoopbackHarness h;
    SOCKET sock;
    const char *why = NULL;
    int slot, seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    slot = lrSeatProvisional(&h, &sock, &why);
    if (slot < 0) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    threadsWaitForMutex();
    transportUdpServerLockedRejoinVerifyForTest(h.sim, (BYTE)slot, true,
                                                "Somebody");
    threadsReleaseMutex();
    why = lrCheckRemoved(&h, sock, slot, seats);
    closesocket(sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    lrStop(&h);
    return 0;
}

/* WinBolo.net turns the seat down: it is removed with the game-locked
 * reason, and the record stays. */
int run_join_locked_rejoin_wbn_fail_removes(void) {
    LoopbackHarness h;
    SOCKET sock;
    const char *why = NULL;
    int slot, seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    slot = lrSeatProvisional(&h, &sock, &why);
    if (slot < 0) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    threadsWaitForMutex();
    transportUdpServerLockedRejoinVerifyForTest(h.sim, (BYTE)slot, false,
                                                NULL);
    threadsReleaseMutex();
    why = lrCheckRemoved(&h, sock, slot, seats);
    closesocket(sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    lrStop(&h);
    return 0;
}

/* No confirm before the deadline: the seat is removed with the game-locked
 * reason and the record stays. Then the owner, from the address the record
 * has, rejoins and gets it. */
int run_join_locked_rejoin_wbn_timeout_removes(void) {
    LoopbackHarness h;
    LrReply r;
    SOCKET sock;
    const char *why = NULL;
    int slot, seats;

    UT_ASSERT_MSG(lrStart(&h, true), "harness start failed");
    seats = lrSeats(&h);
    slot = lrSeatProvisional(&h, &sock, &why);
    if (slot < 0) {
        if (sock != INVALID_SOCKET) closesocket(sock);
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    /* Not yet: a few ticks in, the seat still waits. */
    lrPumpSome(&h, 5);
    if (lrPendingSlot() != slot) {
        closesocket(sock);
        lrStop(&h);
        UT_FAIL("the seat stopped waiting before its deadline");
    }
    transportUdpServerExpireLockedRejoinForTest((BYTE)slot);
    why = lrCheckRemoved(&h, sock, slot, seats);
    closesocket(sock);
    if (why != NULL) {
        lrStop(&h);
        UT_FAIL("%s", why);
    }
    /* The owner comes back from the address the record has. */
    playersRejoinSetAddress(LR_NAME, (uint32_t)inet_addr("127.0.0.1"));
    if (!lrJoin(&h, JOIN_FLAG_WANT_REJOIN, &r) || !r.accept) {
        lrStop(&h);
        UT_FAIL("the owner could not rejoin after the removal "
                "(reject=%d langid=%u)", (int)r.reject, (unsigned)r.langid);
    }
    /* The address match seats the owner with no wait. The record is
     * signed in, so as for any signed-in rejoin it stays held for the
     * owner's WinBolo.net sign-in. */
    if (lrPendingSlot() >= 0) {
        lrStop(&h);
        UT_FAIL("the owner's address match waits for WinBolo.net");
    }
    if (!lrRecordKept(TRUE, "127.0.0.1")) {
        lrStop(&h);
        UT_FAIL("the record is not held for the owner's sign-in");
    }
    lrStop(&h);
    return 0;
}

/* The ask flag, on the locked game. With an address match, and with a
 * signed-in record and a will-auth joiner, the Join gets the rejoin
 * question and no seat. "Join as new" (no ask flag) then gets the
 * game-locked reject. With no match the ask flag gets the reject too. */
int run_join_locked_rejoin_ask_gets_question(void) {
    LoopbackHarness h;
    LrReply r;
    int seats;

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
    if (!lrJoin(&h, JOIN_FLAG_ASK_REJOIN | JOIN_FLAG_WILL_AUTHENTICATE, &r) ||
        !r.reject || r.langid != STR_REJECT_CAN_REJOIN) {
        lrStop(&h);
        UT_FAIL("WinBolo.net match: no rejoin question (accept=%d reject=%d "
                "langid=%u)", (int)r.accept, (int)r.reject,
                (unsigned)r.langid);
    }
    if (!lrJoin(&h, JOIN_FLAG_WILL_AUTHENTICATE, &r) || !r.reject ||
        r.langid != STR_REJECT_GAME_LOCKED) {
        lrStop(&h);
        UT_FAIL("WinBolo.net match: \"Join as new\" was not rejected as "
                "locked (accept=%d reject=%d langid=%u)", (int)r.accept,
                (int)r.reject, (unsigned)r.langid);
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
    if (!lrJoin(&h, JOIN_FLAG_ASK_REJOIN, &r) || !r.reject ||
        r.langid != STR_REJECT_GAME_LOCKED) {
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
 * record: there is no address check. A will-auth rejoin for a signed-in
 * record is seated and does not wait for a locked-game confirm. */
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
    if (!lrJoin(&h, JOIN_FLAG_WANT_REJOIN | JOIN_FLAG_WILL_AUTHENTICATE, &r) ||
        !r.accept) {
        lrStop(&h);
        UT_FAIL("unlocked: the will-auth rejoin was not accepted "
                "(reject=%d langid=%u)", (int)r.reject, (unsigned)r.langid);
    }
    if (lrPendingSlot() >= 0) {
        lrStop(&h);
        UT_FAIL("unlocked: the seat waits for a locked-game confirm");
    }
    /* The signed-in record waits for WinBolo.net as before, in place. */
    if (playersRejoinGetAge(LR_NAME) < 0) {
        lrStop(&h);
        UT_FAIL("unlocked: the signed-in record was used at the join");
    }
    lrStop(&h);
    return 0;
}
