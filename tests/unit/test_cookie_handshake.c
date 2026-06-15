/*
 * JOIN address-proof cookie handshake test.
 *
 * The server gates slot allocation on a retry cookie: an HMAC of
 * (source address ‖ port ‖ time-window) under a per-process secret. A JOIN
 * that doesn't echo a valid cookie draws a PACKET_JOIN_CHALLENGE carrying a
 * fresh cookie and allocates nothing; the joiner must resend the JOIN echoing
 * those bytes. This defeats blind/IP-spoofed joins: an attacker who can't
 * receive at the forged address can't learn the cookie.
 *
 * Like test_join_version_gate.c / test_join_rate_limit.c this stands up the
 * loopback harness and drives hand-built JOINs from raw UDP sockets the test
 * owns, each JOIN carrying a trailing JOIN_COOKIE_LEN cookie field. Three
 * assertions:
 *
 *   (a) No pre-proof allocation + real round-trip: a JOIN with an all-zero
 *       cookie draws a PACKET_JOIN_CHALLENGE and no JOIN_ACCEPT; echoing the
 *       challenge's cookie bytes then draws a JOIN_ACCEPT.
 *   (b) Blind/wrong cookie can't complete: a JOIN with a garbage cookie draws
 *       a challenge but never a JOIN_ACCEPT within the pump budget.
 *   (c) Cookie expires: a cookie acquired at window W is rejected once the
 *       server's window has advanced to W+2 (challenge, no accept), while a
 *       fresh challenge/echo at the shifted window still succeeds. The window
 *       is advanced deterministically via the WB_COOKIE_WINDOW_OFFSET clock
 *       seam — no real-time wait.
 *
 * Each assertion uses a distinct loopback source IP (127.0.0.2/.3/.4) so it
 * gets its own per-source-IP join rate-limit bucket, independent of the
 * harness's own 127.0.0.1 client and of the other assertions. That makes the
 * cases Linux-only (127/8 aliases); each skips with a log if its source can't
 * be bound, rather than failing.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"            /* MAP_STR_SIZE */
#include "platform_net.h"      /* sockets, select, struct sockaddr_in */
#include "wire_limits.h"       /* PACKET_MAX_PLAYER_NAME */
#include "netpacks.h"          /* PACKET_* types, PACKET_HEADER_SIZE, BOLO_VERSION_*, JOIN_COOKIE_LEN */
#include "transport_udp.h"     /* WBN_JOIN_KEY_WIRE_LEN, JOIN_FLAG_* */
#include "transport_udp_internal.h" /* packHeader, getPacketType */
#include "test_harness.h"
#include "loopback_harness.h"

/* Generous: a reply lands on the next server tick after the JOIN is drained.
 * Each ckDriveOnce sends exactly one datagram (one rate-limit token), so the
 * budget can be large without risking the per-source burst cap. */
#define CK_PUMP_MAX 400

/* Build a JOIN_REQUEST with the server's own version triple, a valid name, and
 * a trailing JOIN_COOKIE_LEN cookie (the given bytes, or zeros if NULL).
 * Layout mirrors transport_udp_client.c's builder. Returns the total length. */
static int ckBuildJoin(uint8_t *buf, const char *name,
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

    buf[pos++] = 0;   /* flags */
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

/* Open a non-blocking UDP socket bound to an ephemeral port on the given
 * loopback IP. Returns INVALID_SOCKET if the bind isn't possible (non-Linux
 * loopback alias). */
static SOCKET ckOpenSocketOnIp(const char *ip) {
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

static int ckTryRecv(SOCKET s, uint8_t *buf, int cap) {
    return (int)recvfrom(s, (char *)buf, cap, 0, NULL, NULL);
}

/* Test-only window seam toggle (pairs with the server's getenv on
 * WB_COOKIE_WINDOW_OFFSET). Portable across the unit-test platforms; the CRT
 * getenv the server uses observes both forms. */
static void ckSetWindowOffset(const char *val) {
#ifdef _WIN32
    _putenv_s("WB_COOKIE_WINDOW_OFFSET", val != NULL ? val : "");
#else
    if (val != NULL) setenv("WB_COOKIE_WINDOW_OFFSET", val, 1);
    else unsetenv("WB_COOKIE_WINDOW_OFFSET");
#endif
}

/* Send one JOIN from sock and pump. Records whether a JOIN_ACCEPT and/or a
 * JOIN_CHALLENGE was seen; on a challenge, copies its cookie bytes into
 * outCookie (if non-NULL). When wantNoAccept is true the loop runs the full
 * budget to prove no accept ever arrives; otherwise it returns as soon as it
 * has a verdict. Exactly one datagram is sent (one rate-limit token). */
static void ckDriveOnce(LoopbackHarness *h, SOCKET sock,
                        const uint8_t *join, int joinLen,
                        const struct sockaddr_in *server,
                        bool wantNoAccept,
                        bool *gotAccept, bool *gotChallenge,
                        uint8_t *outCookie) {
    int i;
    *gotAccept    = false;
    *gotChallenge = false;
    sendto(sock, (const char *)join, joinLen, 0,
           (const struct sockaddr *)server, sizeof(*server));
    for (i = 0; i < CK_PUMP_MAX; i++) {
        uint8_t in[1024];
        int n;
        loopbackHarnessPump(h);
        while ((n = ckTryRecv(sock, in, sizeof(in))) > 0) {
            uint8_t type = getPacketType(in, n);
            if (type == PACKET_JOIN_ACCEPT) {
                *gotAccept = true;
            } else if (type == PACKET_JOIN_CHALLENGE &&
                       n >= PACKET_HEADER_SIZE + JOIN_COOKIE_LEN) {
                *gotChallenge = true;
                if (outCookie != NULL) {
                    memcpy(outCookie, in + PACKET_HEADER_SIZE, JOIN_COOKIE_LEN);
                }
            }
        }
        if (*gotAccept) return;                       /* accept is terminal */
        if (!wantNoAccept && *gotChallenge) return;   /* verdict reached */
    }
}

int run_cookie_handshake(void) {
    LoopbackHarness h;
    struct sockaddr_in serverAddr;
    uint8_t joinBuf[1024];
    uint8_t cookie[JOIN_COOKIE_LEN];
    bool gotAccept, gotChallenge;
    int joinLen;

    UT_ASSERT_MSG(loopbackHarnessStart(&h, "Joiner", /*lobbyMode*/ false,
                                       /*impairSpec*/ NULL, /*seed*/ 1u),
                  "harness start failed");

    /* A stray offset from a prior run/test must not skew the windows. */
    ckSetWindowOffset(NULL);

    memset(&serverAddr, 0, sizeof(serverAddr));
    serverAddr.sin_family      = AF_INET;
    serverAddr.sin_addr.s_addr = inet_addr("127.0.0.1");
    serverAddr.sin_port        = htons(h.port);

    /* ---- (a) cookie-less JOIN draws a challenge; echo completes ---- */
    {
        SOCKET s = ckOpenSocketOnIp("127.0.0.2");
        if (s == INVALID_SOCKET) {
            SDL_Log("  cookie handshake: skipping (a) — 127.0.0.2 not bindable");
        } else {
            joinLen = ckBuildJoin(joinBuf, "CkA", NULL);  /* zero cookie */
            ckDriveOnce(&h, s, joinBuf, joinLen, &serverAddr,
                        /*wantNoAccept*/ false, &gotAccept, &gotChallenge,
                        cookie);
            fprintf(stderr, "  cookie (a) cookieless: challenge=%d accept=%d\n",
                    (int)gotChallenge, (int)gotAccept);
            if (gotAccept) {
                closesocket(s);
                loopbackHarnessStop(&h);
                UT_FAIL("cookie-less JOIN was accepted (no address proof)");
            }
            if (!gotChallenge) {
                closesocket(s);
                loopbackHarnessStop(&h);
                UT_FAIL("cookie-less JOIN drew no challenge within %d pumps",
                        CK_PUMP_MAX);
            }
            joinLen = ckBuildJoin(joinBuf, "CkA", cookie);  /* echo cookie */
            ckDriveOnce(&h, s, joinBuf, joinLen, &serverAddr,
                        /*wantNoAccept*/ false, &gotAccept, &gotChallenge, NULL);
            fprintf(stderr, "  cookie (a) echoed:     challenge=%d accept=%d\n",
                    (int)gotChallenge, (int)gotAccept);
            closesocket(s);
            if (!gotAccept) {
                loopbackHarnessStop(&h);
                UT_FAIL("valid-cookie JOIN drew no JOIN_ACCEPT within %d pumps",
                        CK_PUMP_MAX);
            }
        }
    }

    /* ---- (b) garbage cookie draws a challenge, never an accept ---- */
    {
        SOCKET s = ckOpenSocketOnIp("127.0.0.3");
        if (s == INVALID_SOCKET) {
            SDL_Log("  cookie handshake: skipping (b) — 127.0.0.3 not bindable");
        } else {
            uint8_t garbage[JOIN_COOKIE_LEN];
            int i;
            for (i = 0; i < JOIN_COOKIE_LEN; i++) garbage[i] = (uint8_t)(0xA5 ^ i);
            joinLen = ckBuildJoin(joinBuf, "CkB", garbage);
            ckDriveOnce(&h, s, joinBuf, joinLen, &serverAddr,
                        /*wantNoAccept*/ true, &gotAccept, &gotChallenge, NULL);
            fprintf(stderr, "  cookie (b) garbage:    challenge=%d accept=%d\n",
                    (int)gotChallenge, (int)gotAccept);
            closesocket(s);
            if (gotAccept) {
                loopbackHarnessStop(&h);
                UT_FAIL("garbage-cookie JOIN was accepted");
            }
            if (!gotChallenge) {
                loopbackHarnessStop(&h);
                UT_FAIL("garbage-cookie JOIN drew no challenge within %d pumps",
                        CK_PUMP_MAX);
            }
        }
    }

    /* ---- (c) cookie expires across the window seam ---- */
    {
        SOCKET s = ckOpenSocketOnIp("127.0.0.4");
        if (s == INVALID_SOCKET) {
            SDL_Log("  cookie handshake: skipping (c) — 127.0.0.4 not bindable");
        } else {
            uint8_t freshCookie[JOIN_COOKIE_LEN];
            bool staleAccept, staleChallenge;
            bool freshAccept, freshChallenge;

            /* Acquire a cookie at the current window W (offset unset). Do not
             * complete the handshake, so this source stays unconnected and the
             * cookie gate keeps applying to it. */
            ckSetWindowOffset(NULL);
            joinLen = ckBuildJoin(joinBuf, "CkC", NULL);
            ckDriveOnce(&h, s, joinBuf, joinLen, &serverAddr,
                        /*wantNoAccept*/ false, &gotAccept, &gotChallenge,
                        cookie);
            if (!gotChallenge || gotAccept) {
                closesocket(s);
                loopbackHarnessStop(&h);
                UT_FAIL("(c) setup: expected a challenge at W (challenge=%d "
                        "accept=%d)", (int)gotChallenge, (int)gotAccept);
            }

            /* Advance the server's window to W+2: the cookie for W now falls
             * outside the accepted [W+2, W+1] pair. */
            ckSetWindowOffset("2");
            joinLen = ckBuildJoin(joinBuf, "CkC", cookie);  /* stale */
            ckDriveOnce(&h, s, joinBuf, joinLen, &serverAddr,
                        /*wantNoAccept*/ true, &staleAccept, &staleChallenge,
                        freshCookie);

            /* Positive control at the shifted clock: echo the just-issued
             * (W+2) cookie and complete the handshake. */
            joinLen = ckBuildJoin(joinBuf, "CkC", freshCookie);
            ckDriveOnce(&h, s, joinBuf, joinLen, &serverAddr,
                        /*wantNoAccept*/ false, &freshAccept, &freshChallenge,
                        NULL);

            ckSetWindowOffset(NULL);   /* restore before any assert can return */
            closesocket(s);

            fprintf(stderr, "  cookie (c) stale@W+2:  challenge=%d accept=%d ; "
                    "fresh@W+2: accept=%d\n",
                    (int)staleChallenge, (int)staleAccept, (int)freshAccept);

            if (staleAccept) {
                loopbackHarnessStop(&h);
                UT_FAIL("(c) stale cookie from window W was accepted at W+2");
            }
            if (!staleChallenge) {
                loopbackHarnessStop(&h);
                UT_FAIL("(c) stale-cookie JOIN drew no challenge at W+2");
            }
            if (!freshAccept) {
                loopbackHarnessStop(&h);
                UT_FAIL("(c) fresh cookie at W+2 drew no JOIN_ACCEPT");
            }
        }
    }

    loopbackHarnessStop(&h);
    return 0;
}
