/*
 * Server JOIN version-gate test.
 *
 * The server requires an exact protocol-version triple match in every
 * JOIN_REQUEST: a client whose three version bytes don't match the version
 * the server was compiled with is rejected at JOIN with langid
 * STR_REJECT_VERSION_MISMATCH (1389), before any slot allocation or
 * JOIN_ACCEPT.  A matching triple proceeds as normal.
 *
 * This stands up the loopback harness (whose built-in UDP client joins with
 * correct version bytes — the positive control end-to-end) and then drives
 * two hand-built JOIN_REQUESTs from raw UDP sockets the test owns:
 *   1. valid in every field except a version triple one revision low — must
 *      draw a JOIN_REJECT carrying langid 1389, and never a JOIN_ACCEPT;
 *   2. identical but with the server's own version triple — must NOT draw a
 *      1389 reject (a JOIN_ACCEPT, or any non-1389 response, proves the gate
 *      discriminates on version rather than blanket-rejecting crafted joins).
 *
 * The crafted body mirrors the client's JOIN_REQUEST layout exactly so it
 * clears the server's length gate and reaches the version check; the player
 * name is a valid ASCII name so the earlier name-validation step (which runs
 * before the version check) doesn't reject it first.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, select, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_* */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_* */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "gui/lang.h"          /* STR_REJECT_VERSION_MISMATCH */
#include "test_harness.h"
#include "loopback_harness.h"

/* Generous bound: the crafted JOIN's reply is produced on the very next
 * server tick after it's drained, so a few dozen pumps is ample. */
#define VG_PUMP_MAX 200

/* Build a JOIN_REQUEST into buf with the given version triple and a trailing
 * address-proof cookie (the given bytes, or zeros if NULL). Returns the total
 * length. Layout mirrors transport_udp_client.c's builder: header + name +
 * pass + 3 version bytes + WBN token + flags + clientType + clientHints +
 * 2-byte fallbackCountry + JOIN_COOKIE_LEN cookie. */
static int vgBuildJoin(uint8_t *buf, const char *name,
                       uint8_t major, uint8_t minor, uint8_t rev,
                       const uint8_t *cookieOrNull) {
    int pos = PACKET_HEADER_SIZE;
    packHeader(buf, PACKET_JOIN_REQUEST, 0);

    memset(buf + pos, 0, PACKET_MAX_PLAYER_NAME);
    strncpy((char *)(buf + pos), name, PACKET_MAX_PLAYER_NAME - 1);
    pos += PACKET_MAX_PLAYER_NAME;

    memset(buf + pos, 0, MAP_STR_SIZE);   /* empty password */
    pos += MAP_STR_SIZE;

    buf[pos++] = major;
    buf[pos++] = minor;
    buf[pos++] = rev;

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

/* Open a non-blocking UDP socket bound to an ephemeral 127.0.0.1 port. */
static SOCKET vgOpenSocket(void) {
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
    /* Non-blocking so the pump loop can poll without stalling when no reply
     * is waiting (FIONBIO maps to ioctl on POSIX via platform_net.h). */
    ioctlsocket(s, FIONBIO, &nonblock);
    return s;
}

/* Non-blocking poll: returns the byte count of a datagram now waiting on s,
 * or <= 0 if none is currently available. */
static int vgTryRecv(SOCKET s, uint8_t *buf, int cap) {
    return (int)recvfrom(s, (char *)buf, cap, 0, NULL, NULL);
}

/* Drive the crafted JOIN at sock and pump the harness, collecting the first
 * JOIN_ACCEPT / JOIN_REJECT / JOIN_CHALLENGE seen. On a reject,
 * *outRejectLangid receives the langid; on a challenge, *outCookie (if
 * non-NULL) receives the 16 cookie bytes. Sets the matching *outGot* flag.
 * Resends the JOIN periodically since UDP delivery (and the server's
 * recv-thread drain) is best-effort. */
static void vgDriveJoin(LoopbackHarness *h, SOCKET sock,
                        const uint8_t *joinBuf, int joinLen,
                        const struct sockaddr_in *serverAddr,
                        bool *outGotAccept, bool *outGotReject,
                        uint16_t *outRejectLangid,
                        bool *outGotChallenge, uint8_t *outCookie) {
    int i;
    *outGotAccept    = false;
    *outGotReject    = false;
    *outRejectLangid = 0;
    *outGotChallenge = false;

    for (i = 0; i < VG_PUMP_MAX; i++) {
        uint8_t in[1024];
        int n;
        /* Resend every few pumps to ride out best-effort delivery. */
        if (i % 8 == 0) {
            sendto(sock, (const char *)joinBuf, joinLen, 0,
                   (const struct sockaddr *)serverAddr, sizeof(*serverAddr));
        }
        loopbackHarnessPump(h);
        while ((n = vgTryRecv(sock, in, sizeof(in))) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT) {
                *outGotAccept = true;
            } else if (type == PACKET_JOIN_REJECT &&
                       n >= PACKET_HEADER_SIZE + 2) {
                *outGotReject = true;
                *outRejectLangid =
                    (uint16_t)((in[PACKET_HEADER_SIZE] << 8) |
                               in[PACKET_HEADER_SIZE + 1]);
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                *outGotChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
        }
        if (*outGotReject || *outGotAccept || *outGotChallenge) return;
    }
}

int run_join_version_gate(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    int joinLen;
    SOCKET badSock, goodSock;
    bool gotAccept, gotReject, gotChallenge;
    uint16_t rejectLangid;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Joiner", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* ---- Negative: wrong version triple (one revision low) ---- */
    badSock = vgOpenSocket();
    if (badSock == INVALID_SOCKET) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not open raw UDP socket for wrong-version JOIN");
    }
    joinLen = vgBuildJoin(joinBuf, "VerGate",
                          BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR,
                          (uint8_t)(BOLO_VERSION_REVISION - 1), NULL);
    vgDriveJoin(&h, badSock, joinBuf, joinLen, &serverAddr,
                &gotAccept, &gotReject, &rejectLangid, &gotChallenge, NULL);
    fprintf(stderr, "  version gate (wrong): reject=%d langid=%u accept=%d\n",
            (int)gotReject, (unsigned)rejectLangid, (int)gotAccept);
    closesocket(badSock);

    if (!gotReject) {
        loopbackHarnessStop(&h);
        UT_FAIL("wrong-version JOIN drew no JOIN_REJECT within %d pumps",
                VG_PUMP_MAX);
    }
    if (rejectLangid != STR_REJECT_VERSION_MISMATCH) {
        loopbackHarnessStop(&h);
        UT_FAIL("wrong-version JOIN rejected with langid %u, expected %u",
                (unsigned)rejectLangid, (unsigned)STR_REJECT_VERSION_MISMATCH);
    }
    if (gotAccept) {
        loopbackHarnessStop(&h);
        UT_FAIL("wrong-version JOIN was accepted");
    }

    /* ---- Positive: correct version triple discriminates ----
     * The version gate runs before the address-proof cookie gate, so a
     * correct-version JOIN clears the version check and is challenged for a
     * cookie rather than version-rejected. Confirm it draws a challenge (not a
     * 1389 reject, not an accept yet), then complete the handshake by echoing
     * the cookie and confirm the accept. */
    goodSock = vgOpenSocket();
    if (goodSock == INVALID_SOCKET) {
        loopbackHarnessStop(&h);
        UT_FAIL("could not open raw UDP socket for correct-version JOIN");
    }
    joinLen = vgBuildJoin(joinBuf, "VerGate2",
                          BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR,
                          BOLO_VERSION_REVISION, NULL);
    vgDriveJoin(&h, goodSock, joinBuf, joinLen, &serverAddr,
                &gotAccept, &gotReject, &rejectLangid, &gotChallenge, cookie);
    fprintf(stderr, "  version gate (correct): reject=%d langid=%u accept=%d "
            "challenge=%d\n", (int)gotReject, (unsigned)rejectLangid,
            (int)gotAccept, (int)gotChallenge);

    if (gotReject && rejectLangid == STR_REJECT_VERSION_MISMATCH) {
        closesocket(goodSock);
        loopbackHarnessStop(&h);
        UT_FAIL("correct-version JOIN was version-rejected (langid %u)",
                (unsigned)rejectLangid);
    }
    if (!gotChallenge) {
        closesocket(goodSock);
        loopbackHarnessStop(&h);
        UT_FAIL("correct-version JOIN drew no challenge (so it did not clear "
                "the version gate) within %d pumps", VG_PUMP_MAX);
    }
    if (gotAccept) {
        closesocket(goodSock);
        loopbackHarnessStop(&h);
        UT_FAIL("correct-version JOIN was accepted without echoing a cookie");
    }

    /* Echo the cookie — the join now completes. */
    joinLen = vgBuildJoin(joinBuf, "VerGate2",
                          BOLO_VERSION_MAJOR, BOLO_VERSION_MINOR,
                          BOLO_VERSION_REVISION, cookie);
    vgDriveJoin(&h, goodSock, joinBuf, joinLen, &serverAddr,
                &gotAccept, &gotReject, &rejectLangid, &gotChallenge, NULL);
    fprintf(stderr, "  version gate (cookie echoed): accept=%d reject=%d "
            "langid=%u\n", (int)gotAccept, (int)gotReject,
            (unsigned)rejectLangid);
    closesocket(goodSock);

    if (!gotAccept) {
        loopbackHarnessStop(&h);
        UT_FAIL("correct-version JOIN with a valid cookie drew no JOIN_ACCEPT "
                "within %d pumps", VG_PUMP_MAX);
    }

    loopbackHarnessStop(&h);
    return 0;
}
