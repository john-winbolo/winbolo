/*
 * Bot Host link (bot_host_link.c): how a Bot Host and a server find each
 * other, and the slot count a host reports.
 *
 *   1. bot_host_link_announce_codec: an ANNOUNCE keeps every field through
 *      the wire form; a short, long, wrong-proto or free-above-slots body is
 *      refused. QUERY and INVITE bodies are one proto byte.
 *   2. bot_host_link_addr_list: "host[:port]" lists split by commas, spaces
 *      and new lines, with '#' comments; bad ports, IPv6, empty lists and
 *      too many entries are refused. A file of entries reads the same.
 *   3. bot_host_link_allow: an entry with no port allows any port of its
 *      address; an entry with a port allows only that port.
 *   4. bot_host_slots_codec: CTRL_BOT_HOST_SLOTS keeps slots and the refusal
 *      through the wire form; too many slots or a bad seat is refused.
 *   5. bot_host_place_spread: bots added one at a time spread over the
 *      hosts in proportion to their slots, a host of 1 or 2 slots filling a
 *      little later; no host goes over its slots; a rebalance moves one bot
 *      at a time toward the same spread and never moves a bot that may not
 *      move.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bot_host_link.h"
#include "brain_sync.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* ================================================================
 * 1. ANNOUNCE codec
 * ================================================================ */
int run_bot_host_link_announce_codec(void) {
    BotHostAnnounce a, b;
    uint8_t buf[1024];
    int len, i;

    memset(&a, 0, sizeof(a));
    a.verMajor = 2;
    a.verMinor = 3;
    a.verRevision = 4;
    a.listenPort = 27510;
    a.slots = 6;
    a.freeSlots = 4;
    SDL_strlcpy(a.name, "Laptop", sizeof(a.name));
    a.brainCount = 2;
    SDL_strlcpy(a.brains[0].name, "GoalHunter", sizeof(a.brains[0].name));
    a.brains[0].version = 0x0123456789abcdefULL;
    SDL_strlcpy(a.brains[1].name, "Turtle", sizeof(a.brains[1].name));
    a.brains[1].version = 1;

    len = botHostAnnounceEncode(&a, buf, sizeof(buf));
    UT_ASSERT(len > 0);
    UT_ASSERT(botHostAnnounceDecode(buf, (size_t)len, &b));
    UT_ASSERT(b.verMajor == 2 && b.verMinor == 3 && b.verRevision == 4);
    UT_ASSERT(b.listenPort == 27510 && b.slots == 6 && b.freeSlots == 4);
    UT_ASSERT(strcmp(b.name, "Laptop") == 0);
    UT_ASSERT(b.brainCount == 2);
    for (i = 0; i < 2; i++) {
        UT_ASSERT(strcmp(b.brains[i].name, a.brains[i].name) == 0);
        UT_ASSERT(b.brains[i].version == a.brains[i].version);
    }

    /* Every shorter body is refused, and so is one extra byte. */
    for (i = 0; i < len; i++) {
        UT_ASSERT_MSG(!botHostAnnounceDecode(buf, (size_t)i, &b), "len %d", i);
    }
    buf[len] = 0;
    UT_ASSERT(!botHostAnnounceDecode(buf, (size_t)len + 1, &b));

    /* A wrong proto is refused. */
    buf[0] = BOT_HOST_LINK_PROTO + 1;
    UT_ASSERT(!botHostAnnounceDecode(buf, (size_t)len, &b));
    buf[0] = BOT_HOST_LINK_PROTO;

    /* The encoder holds free slots to the slots; a body with more free
     * slots than slots (byte 7) is refused. */
    a.freeSlots = 7;
    len = botHostAnnounceEncode(&a, buf, sizeof(buf));
    UT_ASSERT(len > 0 && botHostAnnounceDecode(buf, (size_t)len, &b));
    UT_ASSERT(b.freeSlots == 6);
    buf[7] = 7;
    UT_ASSERT(!botHostAnnounceDecode(buf, (size_t)len, &b));
    a.freeSlots = 4;

    /* No brains and no room. */
    a.brainCount = 0;
    len = botHostAnnounceEncode(&a, buf, sizeof(buf));
    UT_ASSERT(len > 0 && botHostAnnounceDecode(buf, (size_t)len, &b));
    UT_ASSERT(b.brainCount == 0);
    UT_ASSERT(botHostAnnounceEncode(&a, buf, 4) < 0);

    /* QUERY and INVITE: the proto byte and nothing else. */
    buf[0] = BOT_HOST_LINK_PROTO;
    UT_ASSERT(botHostLinkShortValid(buf, BOT_HOST_LINK_SHORT_LEN));
    UT_ASSERT(!botHostLinkShortValid(buf, 0));
    buf[0] = BOT_HOST_LINK_PROTO + 1;
    UT_ASSERT(!botHostLinkShortValid(buf, BOT_HOST_LINK_SHORT_LEN));
    return 0;
}

/* ================================================================
 * 2. Address lists
 * ================================================================ */
int run_bot_host_link_addr_list(void) {
    BotHostAddrList l;
    char err[256];
    char text[2048];
    char dir[512], path[600];
    int i;

    UT_ASSERT(botHostAddrListParse("127.0.0.1:27650, lan-box 10.0.0.2:1\n"
                                   "# a comment, with 1.2.3.4\n"
                                   "laptop:27510\t# the end",
                                   &l, err, sizeof(err)));
    UT_ASSERT(l.count == 4);
    UT_ASSERT(strcmp(l.e[0].host, "127.0.0.1") == 0 && l.e[0].port == 27650);
    UT_ASSERT(strcmp(l.e[1].host, "lan-box") == 0 && l.e[1].port == 0);
    UT_ASSERT(strcmp(l.e[2].host, "10.0.0.2") == 0 && l.e[2].port == 1);
    UT_ASSERT(strcmp(l.e[3].host, "laptop") == 0 && l.e[3].port == 27510);

    UT_ASSERT(!botHostAddrListParse("", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse(" , # nothing", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse("a:0", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse("a:65536", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse("a:12x", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse("a:", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse(":27500", &l, err, sizeof(err)));
    UT_ASSERT(!botHostAddrListParse("::1", &l, err, sizeof(err)));
    UT_ASSERT(err[0] != '\0');

    text[0] = '\0';
    for (i = 0; i <= BOT_HOST_LINK_MAX_ADDRS; i++) {
        char one[32];
        SDL_snprintf(one, sizeof(one), "10.0.0.%d,", i + 1);
        SDL_strlcat(text, one, sizeof(text));
    }
    UT_ASSERT(!botHostAddrListParse(text, &l, err, sizeof(err)));

    /* A file of entries, by "@path" and by its plain name. */
    UT_ASSERT(utScratchPath(dir, sizeof(dir), "bh_link_list"));
    SDL_snprintf(path, sizeof(path), "%s/hosts.txt", dir);
    {
        const char *body = "# Bot Hosts\r\n127.0.0.1:27510\r\n10.1.1.1\r\n";
        UT_ASSERT(brainSyncWriteFile(path, body, strlen(body)));
    }
    SDL_snprintf(text, sizeof(text), "@%s", path);
    UT_ASSERT(botHostAddrListLoad(text, &l, err, sizeof(err)));
    UT_ASSERT(l.count == 2 && l.e[0].port == 27510 && l.e[1].port == 0);
    UT_ASSERT(botHostAddrListLoad(path, &l, err, sizeof(err)));
    UT_ASSERT(l.count == 2 && strcmp(l.e[1].host, "10.1.1.1") == 0);
    SDL_snprintf(text, sizeof(text), "@%s/missing.txt", dir);
    UT_ASSERT(!botHostAddrListLoad(text, &l, err, sizeof(err)));
    /* Not a file: the list itself. */
    UT_ASSERT(botHostAddrListLoad("127.0.0.1", &l, err, sizeof(err)));
    UT_ASSERT(l.count == 1);
    return 0;
}

/* ================================================================
 * 3. Allow list
 * ================================================================ */
int run_bot_host_link_allow(void) {
    BotHostAllow list[3];
    uint64_t v = 0;

    list[0].ip = 0x0100007fu;   /* 127.0.0.1 in network order on x86 */
    list[0].port = 27510;
    list[1].ip = 0x0200000au;   /* 10.0.0.2 */
    list[1].port = 0;
    list[2].ip = 0x0100007fu;
    list[2].port = 27511;

    UT_ASSERT(botHostAllowFind(list, 3, 0x0100007fu, 27510) == 0);
    UT_ASSERT(botHostAllowFind(list, 3, 0x0100007fu, 27511) == 2);
    UT_ASSERT(botHostAllowFind(list, 3, 0x0100007fu, 27512) == -1);
    UT_ASSERT(botHostAllowFind(list, 3, 0x0200000au, 1) == 1);
    UT_ASSERT(botHostAllowFind(list, 3, 0x0300000au, 27510) == -1);
    UT_ASSERT(botHostAllowFind(list, 0, 0x0100007fu, 27510) == -1);

    UT_ASSERT(botHostLinkParseVersion("0123456789abcdef", &v));
    UT_ASSERT(v == 0x0123456789abcdefULL);
    UT_ASSERT(!botHostLinkParseVersion("0123456789abcde", &v));
    UT_ASSERT(!botHostLinkParseVersion("0123456789abcdefa", &v));
    UT_ASSERT(!botHostLinkParseVersion("0123456789abcdeg", &v));
    return 0;
}

/* ================================================================
 * 4. CTRL_BOT_HOST_SLOTS
 * ================================================================ */
int run_bot_host_slots_codec(void) {
    ControlEvent in, out;
    uint8_t buf[64];
    size_t len = 0;
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_SLOTS);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_SLOTS);

    UT_ASSERT(enc != NULL && dec != NULL);
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_SLOTS;
    in.u.botHostSlots.slots = 3;
    in.u.botHostSlots.refuseSeat = BOT_HOST_SLOTS_NO_REFUSE;
    in.u.botHostSlots.refuseGen = 0;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.type == CTRL_BOT_HOST_SLOTS && out.u.botHostSlots.slots == 3);
    UT_ASSERT(out.u.botHostSlots.refuseSeat == BOT_HOST_SLOTS_NO_REFUSE);
    UT_ASSERT(!dec(buf, len - 1, &out));

    in.u.botHostSlots.slots = 0;
    in.u.botHostSlots.refuseSeat = 5;
    in.u.botHostSlots.refuseGen = 0xbeef;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.u.botHostSlots.slots == 0);
    UT_ASSERT(out.u.botHostSlots.refuseSeat == 5);
    UT_ASSERT(out.u.botHostSlots.refuseGen == 0xbeef);

    buf[0] = MAX_TANKS + 1;
    UT_ASSERT(!dec(buf, len, &out));
    buf[0] = 1;
    buf[1] = MAX_TANKS;
    UT_ASSERT(!dec(buf, len, &out));
    UT_ASSERT(enc(&in, NULL, buf, 2, &len) != ENCODE_OK);
    return 0;
}

/* ================================================================
 * 5. Placement: the proportional spread
 * ================================================================ */

/* Adds bots one at a time with botHostPlacePick and checks the spread
 * after each against want (count rows of n values). Prints the table. */
static int placeRun(const int *slots, int n, const int *want, int count,
                    const char *label) {
    BotHostPlaceHost h[8];
    int k, i;
    memset(h, 0, sizeof(h));
    for (i = 0; i < n; i++) {
        h[i].slots = slots[i];
        h[i].open = true;
    }
    printf("  placement %s (small <= %d slots, bias %.2f):\n", label,
           BOT_HOST_SMALL_SLOTS_DEFAULT, BOT_HOST_SMALL_BIAS_DEFAULT);
    for (k = 1; k <= count; k++) {
        int p = botHostPlacePick(h, n, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT);
        if (p >= 0) h[p].bots++;
        printf("    %2d bots:", k);
        for (i = 0; i < n; i++) printf(" %d/%d", h[i].bots, h[i].slots);
        printf("%s\n", p < 0 ? "  (no room)" : "");
        for (i = 0; i < n; i++) {
            UT_ASSERT(h[i].bots <= h[i].slots);
            UT_ASSERT(h[i].bots == want[(k - 1) * n + i]);
        }
    }
    return 0;
}

int run_bot_host_place_spread(void) {
    static const int s8421[] = { 8, 4, 2, 1 };
    static const int w8421[] = {
        1,0,0,0,  2,0,0,0,  2,1,0,0,  3,1,0,0,  4,1,0,0,
        4,2,0,0,  4,2,1,0,  5,2,1,0,  6,2,1,0,  6,3,1,0,
        7,3,1,0,  8,3,1,0,  8,4,1,0,  8,4,2,0,  8,4,2,1 };
    static const int s22[] = { 2, 2 };
    static const int w22[] = { 1,0,  1,1,  2,1,  2,2 };
    static const int s1[] = { 1 };
    static const int w1[] = { 1, 1 };
    BotHostPlaceHost h[3];
    int from = -1, to = -1, moves;

    if (placeRun(s8421, 4, w8421, 15, "hosts 8,4,2,1") != 0) return 1;
    if (placeRun(s22, 2, w22, 4, "hosts 2,2") != 0) return 1;
    if (placeRun(s1, 1, w1, 2, "host 1 alone") != 0) return 1;

    /* Effective slots: a small host counts the bias fewer. */
    UT_ASSERT(botHostPlaceEffSlots(8, 2, 0.25) == 8.0);
    UT_ASSERT(botHostPlaceEffSlots(2, 2, 0.25) == 1.75);
    UT_ASSERT(botHostPlaceEffSlots(1, 2, 0.25) == 0.75);
    UT_ASSERT(botHostPlaceEffSlots(1, 2, 5.0) == 0.05);
    UT_ASSERT(botHostPlaceEffSlots(2, 0, 0.25) == 2.0);

    /* Rebalance: a host of 4 joins a host of 8 that runs 4 bots. One move
     * takes the spread to 3 and 1, where a new bot would put it, and then
     * nothing moves. */
    memset(h, 0, sizeof(h));
    h[0].slots = 8; h[0].bots = 4; h[0].open = true; h[0].movable = true;
    h[1].slots = 4; h[1].bots = 0; h[1].open = true; h[1].movable = true;
    UT_ASSERT(botHostPlaceRebalance(h, 2, 2, 0.25, &from, &to));
    UT_ASSERT(from == 0 && to == 1);
    h[0].bots--; h[1].bots++;
    UT_ASSERT(!botHostPlaceRebalance(h, 2, 2, 0.25, &from, &to));

    /* The server itself (unlimited, capacity 3) runs 10 bots; a host of 4
     * joins. Moves go one at a time until the host is full. */
    memset(h, 0, sizeof(h));
    h[0].slots = 3; h[0].bots = 10; h[0].open = true; h[0].unlimited = true;
    h[0].movable = true;
    h[1].slots = 4; h[1].open = true; h[1].movable = true;
    moves = 0;
    while (botHostPlaceRebalance(h, 2, 2, 0.25, &from, &to) && moves < 20) {
        UT_ASSERT(from == 0 && to == 1);
        h[from].bots--; h[to].bots++;
        moves++;
    }
    UT_ASSERT(moves == 4 && h[0].bots == 6 && h[1].bots == 4);

    /* A bot that may not move (a pill carried, the builder out) stays,
     * even where it is crowded. */
    memset(h, 0, sizeof(h));
    h[0].slots = 8; h[0].bots = 8; h[0].open = true; h[0].movable = false;
    h[1].slots = 8; h[1].open = true; h[1].movable = true;
    UT_ASSERT(!botHostPlaceRebalance(h, 2, 2, 0.25, &from, &to));

    /* A host of 2 with 1 bot beside a host of 8 with 2: the small host is
     * the more crowded (1/1.75 against 2/8), and the host of 8 still has
     * room at 3/8, so its bot moves: the spread a new bot would make (3
     * bots on hosts 8 and 2 go 3 and 0). Then nothing moves. */
    memset(h, 0, sizeof(h));
    h[0].slots = 8; h[0].bots = 2; h[0].open = true; h[0].movable = true;
    h[1].slots = 2; h[1].bots = 1; h[1].open = true; h[1].movable = true;
    UT_ASSERT(botHostPlaceRebalance(h, 2, 2, 0.25, &from, &to));
    UT_ASSERT(from == 1 && to == 0);
    h[1].bots--; h[0].bots++;
    UT_ASSERT(!botHostPlaceRebalance(h, 2, 2, 0.25, &from, &to));
    return 0;
}
