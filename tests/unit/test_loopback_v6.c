/*
 * Joins over IPv6, both address families in one game, and the IPv4 fallback
 * socket.
 *
 * Every game socket opens dual stack: an AF_INET6 socket with IPV6_V6ONLY
 * cleared, so IPv4 peers reach it as mapped addresses (::ffff:a.b.c.d), and an
 * AF_INET socket when IPv6 is not available. Three cases over the loopback
 * harness, on a clean path:
 *
 *   loopback_v6_join_download_disconnect
 *       A client dials ::1. The join handshake and the map download complete,
 *       the server holds the slot under an IPv6 address and the client holds
 *       ::1 as the server address, then the client quits and the server frees
 *       the slot.
 *
 *   loopback_mixed_family_one_server
 *       One server, one client dialling ::1 and one dialling 127.0.0.1, both
 *       joined at the same time. Each gets a tank and each has the other in
 *       its roster, and the server shows the IPv4 client as plain 127.0.0.1
 *       (never ::ffff:127.0.0.1) beside the IPv6 one.
 *
 *   loopback_ipv4_fallback_socket
 *       With udpTestForceIPv4Socket on, the server and the client open AF_INET
 *       sockets, as on a machine with no IPv6 stack, and a join over 127.0.0.1
 *       still completes. The switch is turned off again on every path out.
 *
 * The two IPv6 cases need an IPv6 loopback. When creating an AF_INET6 socket
 * or binding ::1 fails, or the game's socket factory falls back to AF_INET,
 * they log that and pass as skipped (loopbackIpv6Usable).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "platform_net.h"
#include "bolo_addr.h"
#include "server_sim.h"
#include "client_sim.h"
#include "client_net.h"
#include "client_connect_state.h"
#include "game_sim.h"
#include "players.h"                        /* playersIsInUse */
#include "transport_udp.h"                  /* transportUdpServerGetClientAddrStr */
#include "transport_udp_internal.h"         /* udpTestForceIPv4Socket */
#include "transport_udp_server_internal.h"  /* udpServer.sockFamily */
#include "test_harness.h"
#include "loopback_harness.h"

/* Join plus the map download on a clean path. The other clean-path loopback
 * cases budget the same. */
#define LV6_CONNECT_MAX 2000

/* A tank for each client once joined, and the peer in each roster. */
#define LV6_ESTABLISH_MAX 600

/* The server reading the quit and freeing the slot. */
#define LV6_LEAVE_MAX 600

#define LV6_SEED 0x1F6u

static bool lv6Connected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs) == CLIENT_CONNECT_CONNECTED;
}

static bool lv6BothConnected(LoopbackHarness *h, void *user) {
    (void)user;
    return clientSimGetConnectState(h->cs)  == CLIENT_CONNECT_CONNECTED &&
           clientSimGetConnectState(h->cs2) == CLIENT_CONNECT_CONNECTED;
}

static bool lv6BothHaveTanks(LoopbackHarness *h, void *user) {
    BYTE mx = 0, my = 0;
    (void)user;
    return clientSimGetMyTankMapPos(h->cs,  &mx, &my) == TRUE &&
           clientSimGetMyTankMapPos(h->cs2, &mx, &my) == TRUE;
}

/* Is `slot` present in the roster this client keeps? */
static bool lv6RosterHas(ClientSim *cs, BYTE slot) {
    return playersIsInUse(&clientSimGetGameSim(cs)->plyrs, slot) == TRUE;
}

/* Each client has the other's slot in its roster. */
static bool lv6EachSeesTheOther(LoopbackHarness *h, void *user) {
    BYTE slot1 = clientSimGetMyPlayerNum(h->cs);
    BYTE slot2 = clientSimGetMyPlayerNum(h->cs2);
    (void)user;
    return lv6RosterHas(h->cs, slot2) && lv6RosterHas(h->cs2, slot1);
}

/* The server no longer holds a connected client in `slot`. */
static bool lv6SlotFree(LoopbackHarness *h, void *user) {
    char text[BOLO_ADDR_STR_LEN];
    BYTE slot = *(const BYTE *)user;
    (void)h;
    return !transportUdpServerGetClientAddrStr(slot, text, sizeof(text));
}

/* The address the server shows for `slot` begins with `prefix`. */
static bool lv6ServerSeesSlotAt(BYTE slot, const char *prefix, char *text,
                                size_t textLen) {
    if (!transportUdpServerGetClientAddrStr(slot, text, textLen)) {
        return false;
    }
    return strncmp(text, prefix, strlen(prefix)) == 0;
}

int run_loopback_v6_join_download_disconnect(void) {
    LoopbackHarness h;
    BoloAddr srv;
    char text[BOLO_ADDR_STR_LEN];
    BYTE slot;
    int at;

    if (!loopbackIpv6Usable("loopback_v6_join_download_disconnect")) {
        return 0;
    }

    if (!loopbackHarnessStartDial(&h, "V6Joiner", /*lobbyMode*/ false,
                                  /*impairSpec*/ NULL, LV6_SEED,
                                  LOOPBACK_DIAL_IPV6)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start dialling ::1 failed");
    }

    at = loopbackHarnessPumpUntil(&h, LV6_CONNECT_MAX, lv6Connected, NULL);
    if (at < 0) {
        int st = (int)clientSimGetConnectState(h.cs);
        loopbackHarnessStop(&h);
        UT_FAIL("the ::1 client never reached CONNECTED within %d pumps "
                "(state=%d)", LV6_CONNECT_MAX, st);
    }
    fprintf(stderr, "  loopback v6: connected over ::1 after %d pump(s)\n", at);

    if (!clientSimIsMapDownloadComplete(h.cs)) {
        loopbackHarnessStop(&h);
        UT_FAIL("CONNECTED over ::1 with the map download not complete");
    }

    /* The transport stored the address it dialled. */
    if (!clientSimGetServerAddr(h.cs, &srv) ||
        boloAddrFamily(&srv) != AF_INET6 || boloAddrIsV4(&srv) ||
        !boloAddrIsLoopback(&srv) || boloAddrPort(&srv) != h.port) {
        loopbackHarnessStop(&h);
        UT_FAIL("the client does not hold [::1]:%u as the server address",
                (unsigned)h.port);
    }

    /* The server holds the slot under the IPv6 address it came from. */
    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS || !lv6ServerSeesSlotAt(slot, "[::1]:", text,
                                                  sizeof(text))) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server does not show slot %u at [::1] (shows \"%s\")",
                (unsigned)slot, slot < MAX_TANKS ? text : "");
    }
    fprintf(stderr, "  loopback v6: server holds slot %u at %s\n",
            (unsigned)slot, text);

    /* A clean quit: clientSimDisconnect sends PACKET_QUIT, and the server
     * frees the slot when it reads it. */
    clientSimDisconnect(h.cs);
    at = loopbackHarnessPumpUntil(&h, LV6_LEAVE_MAX, lv6SlotFree, &slot);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server still holds slot %u %d pumps after the ::1 "
                "client quit", (unsigned)slot, LV6_LEAVE_MAX);
    }
    if (serverSimGetNumPlayers(h.sim) != 0) {
        BYTE players = serverSimGetNumPlayers(h.sim);
        loopbackHarnessStop(&h);
        UT_FAIL("%u player(s) left on the server after the only one quit",
                (unsigned)players);
    }
    fprintf(stderr, "  loopback v6: slot freed %d pump(s) after the quit\n", at);

    loopbackHarnessStop(&h);
    return 0;
}

int run_loopback_mixed_family_one_server(void) {
    LoopbackHarness h;
    BoloAddr srv1, srv2;
    char text1[BOLO_ADDR_STR_LEN];
    char text2[BOLO_ADDR_STR_LEN];
    BYTE slot1, slot2;
    int at;

    if (!loopbackIpv6Usable("loopback_mixed_family_one_server")) {
        return 0;
    }

    /* Started with the IPv6 dial, so the server binds the any address and
     * both clients can reach it. */
    if (!loopbackHarnessStartDial(&h, "SixJoiner", /*lobbyMode*/ false,
                                  /*impairSpec*/ NULL, LV6_SEED,
                                  LOOPBACK_DIAL_IPV6)) {
        loopbackHarnessStop(&h);
        UT_FAIL("harness start dialling ::1 failed");
    }
    if (!loopbackHarnessAddClientDial(&h, "FourJoiner", LOOPBACK_DIAL_IPV4)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the 127.0.0.1 client failed to connect");
    }

    at = loopbackHarnessPumpUntil(&h, LV6_CONNECT_MAX, lv6BothConnected, NULL);
    if (at < 0) {
        int st1 = (int)clientSimGetConnectState(h.cs);
        int st2 = (int)clientSimGetConnectState(h.cs2);
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never reached CONNECTED within %d pumps "
                "(::1 state=%d, 127.0.0.1 state=%d)", LV6_CONNECT_MAX,
                st1, st2);
    }
    fprintf(stderr, "  loopback mixed: both connected after %d pump(s)\n", at);

    at = loopbackHarnessPumpUntil(&h, LV6_ESTABLISH_MAX, lv6BothHaveTanks,
                                  NULL);
    if (at < 0) {
        loopbackHarnessStop(&h);
        UT_FAIL("both clients never got a tank within %d pumps",
                LV6_ESTABLISH_MAX);
    }

    slot1 = clientSimGetMyPlayerNum(h.cs);
    slot2 = clientSimGetMyPlayerNum(h.cs2);
    if (slot1 >= MAX_TANKS || slot2 >= MAX_TANKS || slot1 == slot2) {
        loopbackHarnessStop(&h);
        UT_FAIL("the two clients hold slots %u and %u", (unsigned)slot1,
                (unsigned)slot2);
    }

    /* Full state on both: each has the other in its roster. */
    at = loopbackHarnessPumpUntil(&h, LV6_ESTABLISH_MAX, lv6EachSeesTheOther,
                                  NULL);
    if (at < 0) {
        bool oneSeesTwo = lv6RosterHas(h.cs, slot2);
        bool twoSeesOne = lv6RosterHas(h.cs2, slot1);
        loopbackHarnessStop(&h);
        UT_FAIL("the clients never both had the other in their roster within "
                "%d pumps (::1 has the IPv4 client: %d, 127.0.0.1 has the IPv6 "
                "client: %d)", LV6_ESTABLISH_MAX, (int)oneSeesTwo,
                (int)twoSeesOne);
    }

    /* Each client holds the server address of its own family. */
    if (!clientSimGetServerAddr(h.cs, &srv1) || boloAddrIsV4(&srv1) ||
        !boloAddrIsLoopback(&srv1)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the ::1 client does not hold an IPv6 server address");
    }
    if (!clientSimGetServerAddr(h.cs2, &srv2) || !boloAddrIsV4(&srv2) ||
        !boloAddrIsLoopback(&srv2)) {
        loopbackHarnessStop(&h);
        UT_FAIL("the 127.0.0.1 client does not hold an IPv4 server address");
    }

    /* One server, one slot of each family. The IPv4 client arrived on the
     * dual-stack socket as ::ffff:127.0.0.1 and is shown as plain IPv4. */
    if (!lv6ServerSeesSlotAt(slot1, "[::1]:", text1, sizeof(text1))) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server does not show slot %u at [::1] (shows \"%s\")",
                (unsigned)slot1, text1);
    }
    if (!lv6ServerSeesSlotAt(slot2, "127.0.0.1:", text2, sizeof(text2))) {
        loopbackHarnessStop(&h);
        UT_FAIL("the server does not show slot %u at 127.0.0.1 (shows "
                "\"%s\")", (unsigned)slot2, text2);
    }
    fprintf(stderr, "  loopback mixed: slot %u at %s, slot %u at %s\n",
            (unsigned)slot1, text1, (unsigned)slot2, text2);

    loopbackHarnessStop(&h);
    return 0;
}

int run_loopback_ipv4_fallback_socket(void) {
    LoopbackHarness h;
    char text[BOLO_ADDR_STR_LEN];
    BYTE slot;
    int at;
    int serverFamily;

    /* Every socket opened from here to the end of the case is AF_INET. */
    udpTestForceIPv4Socket(true);

    if (!loopbackHarnessStart(&h, "V4Fallback", /*lobbyMode*/ false,
                              /*impairSpec*/ NULL, LV6_SEED)) {
        loopbackHarnessStop(&h);
        udpTestForceIPv4Socket(false);
        UT_FAIL("harness start on the IPv4 fallback socket failed");
    }

    serverFamily = udpServer.sockFamily;
    if (serverFamily != AF_INET) {
        loopbackHarnessStop(&h);
        udpTestForceIPv4Socket(false);
        UT_FAIL("the server socket is family %d, not the AF_INET fallback",
                serverFamily);
    }

    at = loopbackHarnessPumpUntil(&h, LV6_CONNECT_MAX, lv6Connected, NULL);
    if (at < 0) {
        int st = (int)clientSimGetConnectState(h.cs);
        loopbackHarnessStop(&h);
        udpTestForceIPv4Socket(false);
        UT_FAIL("the client never reached CONNECTED on the IPv4 fallback "
                "socket within %d pumps (state=%d)", LV6_CONNECT_MAX, st);
    }
    fprintf(stderr, "  loopback v4 fallback: connected after %d pump(s)\n", at);

    slot = clientSimGetMyPlayerNum(h.cs);
    if (slot >= MAX_TANKS || !lv6ServerSeesSlotAt(slot, "127.0.0.1:", text,
                                                  sizeof(text))) {
        loopbackHarnessStop(&h);
        udpTestForceIPv4Socket(false);
        UT_FAIL("the server does not show slot %u at 127.0.0.1",
                (unsigned)slot);
    }

    loopbackHarnessStop(&h);
    udpTestForceIPv4Socket(false);
    return 0;
}
