/*
 * The rejoin question at JOIN.
 *
 * A JOIN with JOIN_FLAG_ASK_REJOIN and no JOIN_FLAG_WANT_REJOIN, while a
 * live rejoin record has the joiner's name, is not seated. The server
 * answers with a JOIN_REJECT that carries STR_REJECT_CAN_REJOIN, the name,
 * the seconds since the player left and the seconds left on the record.
 *
 * Each test stands up the loopback harness with a running server, puts a
 * rejoin record in the table, and drives hand-built JOIN_REQUESTs from a
 * raw UDP socket of its own, with the address-proof cookie the server asks
 * for:
 *   join_ask_rejoin_live_record_asks   — the ask flag and a live record: the
 *                                        question, and no seat
 *   join_ask_rejoin_without_flag_joins — no ask flag: the join is seated, as
 *                                        before, and the record stays
 *   join_ask_rejoin_want_rejoin_joins  — the ask flag and WANT_REJOIN: the
 *                                        join is seated and uses the record
 *   join_ask_rejoin_no_record_joins    — the ask flag and no record: the
 *                                        join is seated
 *   join_ask_rejoin_timed_out_joins    — the ask flag and a record that has
 *                                        timed out: the join is seated
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
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_* */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "server_sim.h"        /* serverSimIsPlayerConnected */
#include "playersrejoin.h"     /* playersRejoinAddPlayer, playersRejoinGetAge */
#include "gui/lang.h"          /* STR_REJECT_CAN_REJOIN */
#include "test_harness.h"
#include "loopback_harness.h"

/* The reply to a JOIN comes on the next server tick, so this is ample. */
#define AR_PUMP_MAX 200

/* The name of the player the rejoin record is for. */
#define AR_NAME "Back"

/* What one JOIN drew from the server. */
typedef struct {
    bool     accept;
    bool     reject;
    bool     challenge;
    uint16_t langid;
    int      argCount;
    char     args[4][PLAYER_NAME_LEN];
    uint8_t  cookie[JOIN_COOKIE_LEN];
} ArReply;

/* A JOIN_REQUEST with the given flags byte and cookie (zeros when NULL),
 * laid out as transport_udp_client.c builds it. Returns its length. */
static int arBuildJoin(uint8_t *buf, const char *name, uint8_t flags,
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
    memset(buf + pos, 0, WBN_JOIN_KEY_WIRE_LEN);  /* not signed in */
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
static SOCKET arOpenSocket(void) {
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

/* Reads the langid and the args of a JOIN_REJECT into r. */
static void arDecodeReject(const uint8_t *in, int n, ArReply *r) {
    int pos = PACKET_HEADER_SIZE;
    int i;
    r->langid = (uint16_t)((in[pos] << 8) | in[pos + 1]);
    pos += 2;
    r->argCount = 0;
    if (pos >= n) return;
    r->argCount = in[pos++];
    if (r->argCount > 4) r->argCount = 4;
    for (i = 0; i < r->argCount; i++) {
        int aLen;
        if (pos >= n) { r->argCount = i; return; }
        aLen = in[pos++];
        if (aLen > PLAYER_NAME_LEN - 1 || pos + aLen > n) {
            r->argCount = i;
            return;
        }
        memcpy(r->args[i], in + pos, (size_t)aLen);
        r->args[i][aLen] = '\0';
        pos += aLen;
    }
}

/* Sends the JOIN (again every few pumps) and pumps the harness until the
 * server answers with an accept, a reject or a challenge. */
static void arDrive(LoopbackHarness *h, SOCKET sock, const uint8_t *joinBuf,
                    int joinLen, const struct sockaddr_in *serverAddr,
                    ArReply *r) {
    int i;
    BoloAddr server;
    boloAddrFromSockaddr((const struct sockaddr *)serverAddr,
                         (socklen_t)sizeof(*serverAddr), &server);
    memset(r, 0, sizeof(*r));
    for (i = 0; i < AR_PUMP_MAX; i++) {
        uint8_t in[1024];
        int n;
        if (i % 8 == 0) {
            sendto(sock, (const char *)joinBuf, joinLen, 0,
                   (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        }
        loopbackHarnessPump(h);
        while ((n = loopbackRecvFromServer(sock, in, sizeof(in),
                                           &server)) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT) {
                r->accept = true;
            } else if (type == PACKET_JOIN_REJECT &&
                       n >= PACKET_HEADER_SIZE + 2) {
                r->reject = true;
                arDecodeReject(in, n, r);
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                r->challenge = true;
                memcpy(r->cookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
            }
        }
        if (r->accept || r->reject || r->challenge) return;
    }
}

/* One full JOIN with the given flags: the first JOIN draws the cookie
 * challenge, and the second echoes the cookie. r holds the answer to the
 * second. Returns false when no challenge came. */
static bool arJoin(LoopbackHarness *h, uint8_t flags, ArReply *r) {
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    int joinLen;
    SOCKET sock;

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h->port);

    sock = arOpenSocket();
    if (sock == INVALID_SOCKET) return false;
    joinLen = arBuildJoin(joinBuf, AR_NAME, flags, NULL);
    arDrive(h, sock, joinBuf, joinLen, &serverAddr, r);
    if (!r->challenge) {
        closesocket(sock);
        return false;
    }
    memcpy(cookie, r->cookie, JOIN_COOKIE_LEN);
    joinLen = arBuildJoin(joinBuf, AR_NAME, flags, cookie);
    arDrive(h, sock, joinBuf, joinLen, &serverAddr, r);
    closesocket(sock);
    fprintf(stderr, "  join flags=0x%02x: accept=%d reject=%d langid=%u\n",
            (unsigned)flags, (int)r->accept, (int)r->reject,
            (unsigned)r->langid);
    return true;
}

/* How many seats are connected. */
static int arSeats(LoopbackHarness *h) {
    int i, n = 0;
    for (i = 0; i < MAX_TANKS; i++) {
        if (serverSimIsPlayerConnected(h->sim, (BYTE)i)) n++;
    }
    return n;
}

/* True once the harness client has a seat. */
static bool arClientSeated(LoopbackHarness *h, void *user) {
    (void)user;
    return arSeats(h) >= 1;
}

/* Starts a running server with its own client seated, and an empty rejoin
 * table. The client takes its seat over a few pumps, so this waits for it.
 * Then the seat count before a JOIN does not move when that client sits. */
static bool arStart(LoopbackHarness *h) {
    if (!loopbackHarnessStart(h, "Joiner", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, /*seed*/ 1u)) {
        return false;
    }
    if (loopbackHarnessPumpUntil(h, AR_PUMP_MAX, arClientSeated, NULL) < 0) {
        loopbackHarnessStop(h);
        return false;
    }
    playersRejoinCreate();
    return true;
}

static void arStop(LoopbackHarness *h) {
    loopbackHarnessStop(h);
    playersRejoinCreate();
}

/* A record 15 seconds old. The ask flag gets the question: the name, about
 * 15 seconds since the player left and about 285 left of the 5 minutes, and
 * no seat. The record stays. */
int run_join_ask_rejoin_live_record_asks(void) {
    LoopbackHarness h;
    ArReply r;
    int seats, now, i, ago, left;

    UT_ASSERT_MSG(arStart(&h), "harness start failed");
    playersRejoinAddPlayer(AR_NAME, 0, 0, 0, FALSE);
    for (i = 0; i < 15 * REJOIN_TICKS_PER_SECOND; i++) playersRejoinUpdate();
    seats = arSeats(&h);

    if (!arJoin(&h, JOIN_FLAG_ASK_REJOIN, &r)) {
        arStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.reject || r.langid != STR_REJECT_CAN_REJOIN || r.accept) {
        arStop(&h);
        UT_FAIL("expected the rejoin question (langid %u), got accept=%d "
                "reject=%d langid=%u", (unsigned)STR_REJECT_CAN_REJOIN,
                (int)r.accept, (int)r.reject, (unsigned)r.langid);
    }
    if (r.argCount != 4 || strcmp(r.args[0], AR_NAME) != 0) {
        arStop(&h);
        UT_FAIL("the question has %d args, name '%s'", r.argCount,
                r.argCount > 0 ? r.args[0] : "");
    }
    ago = atoi(r.args[2]);
    left = atoi(r.args[3]);
    fprintf(stderr, "  ago=%d s left=%d s\n", ago, left);
    if (ago < 15 || ago > 25 || ago + left < 299 || ago + left > 301) {
        arStop(&h);
        UT_FAIL("the question says %d s ago and %d s left", ago, left);
    }
    now = arSeats(&h);
    if (now != seats) {
        arStop(&h);
        UT_FAIL("the asked JOIN was seated (%d seats, was %d)", now, seats);
    }
    if (playersRejoinGetAge(AR_NAME) < 0) {
        arStop(&h);
        UT_FAIL("the record is gone after the question");
    }
    arStop(&h);
    return 0;
}

/* A record, and a JOIN with no ask flag, as an older client or "Join as
 * new" sends it: the join is seated, and the record stays for its owner. */
int run_join_ask_rejoin_without_flag_joins(void) {
    LoopbackHarness h;
    ArReply r;
    int seats, now;

    UT_ASSERT_MSG(arStart(&h), "harness start failed");
    playersRejoinAddPlayer(AR_NAME, 0, 0, 0, FALSE);
    seats = arSeats(&h);
    if (!arJoin(&h, 0, &r)) {
        arStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.accept) {
        arStop(&h);
        UT_FAIL("the JOIN was not accepted (reject=%d langid=%u)",
                (int)r.reject, (unsigned)r.langid);
    }
    now = arSeats(&h);
    if (now != seats + 1) {
        arStop(&h);
        UT_FAIL("%d seats after the join, was %d", now, seats);
    }
    if (playersRejoinGetAge(AR_NAME) < 0) {
        arStop(&h);
        UT_FAIL("a plain join used the record");
    }
    arStop(&h);
    return 0;
}

/* A record, and a JOIN with the ask flag and WANT_REJOIN, as "Rejoin"
 * sends it: the join is seated and uses the record. */
int run_join_ask_rejoin_want_rejoin_joins(void) {
    LoopbackHarness h;
    ArReply r;
    int seats, now;

    UT_ASSERT_MSG(arStart(&h), "harness start failed");
    playersRejoinAddPlayer(AR_NAME, 0, 0, 0, FALSE);
    seats = arSeats(&h);
    if (!arJoin(&h, JOIN_FLAG_ASK_REJOIN | JOIN_FLAG_WANT_REJOIN, &r)) {
        arStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.accept) {
        arStop(&h);
        UT_FAIL("the rejoin was not accepted (reject=%d langid=%u)",
                (int)r.reject, (unsigned)r.langid);
    }
    now = arSeats(&h);
    if (now != seats + 1) {
        arStop(&h);
        UT_FAIL("%d seats after the rejoin, was %d", now, seats);
    }
    if (playersRejoinGetAge(AR_NAME) >= 0) {
        arStop(&h);
        UT_FAIL("the rejoin did not use the record");
    }
    arStop(&h);
    return 0;
}

/* No record, and a JOIN with the ask flag: the join is seated. */
int run_join_ask_rejoin_no_record_joins(void) {
    LoopbackHarness h;
    ArReply r;
    int seats, now;

    UT_ASSERT_MSG(arStart(&h), "harness start failed");
    seats = arSeats(&h);
    if (!arJoin(&h, JOIN_FLAG_ASK_REJOIN, &r)) {
        arStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.accept) {
        arStop(&h);
        UT_FAIL("the JOIN was not accepted (reject=%d langid=%u)",
                (int)r.reject, (unsigned)r.langid);
    }
    now = arSeats(&h);
    if (now != seats + 1) {
        arStop(&h);
        UT_FAIL("%d seats after the join, was %d", now, seats);
    }
    arStop(&h);
    return 0;
}

/* A record that has timed out, and a JOIN with the ask flag: there is
 * nothing to rejoin, so the join is seated. */
int run_join_ask_rejoin_timed_out_joins(void) {
    LoopbackHarness h;
    ArReply r;
    int seats, now, i;

    UT_ASSERT_MSG(arStart(&h), "harness start failed");
    playersRejoinAddPlayer(AR_NAME, 0, 0, 0, FALSE);
    for (i = 0; i < MAX_REJOIN_TIME; i++) playersRejoinUpdate();
    if (playersRejoinGetAge(AR_NAME) >= 0) {
        arStop(&h);
        UT_FAIL("the record did not time out");
    }
    seats = arSeats(&h);
    if (!arJoin(&h, JOIN_FLAG_ASK_REJOIN, &r)) {
        arStop(&h);
        UT_FAIL("the JOIN drew no cookie challenge");
    }
    if (!r.accept) {
        arStop(&h);
        UT_FAIL("the JOIN was not accepted (reject=%d langid=%u)",
                (int)r.reject, (unsigned)r.langid);
    }
    now = arSeats(&h);
    if (now != seats + 1) {
        arStop(&h);
        UT_FAIL("%d seats after the join, was %d", now, seats);
    }
    arStop(&h);
    return 0;
}
