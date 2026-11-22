/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * A Bot Host that joins a game in progress, or comes back to one.
 *
 *   1. bot_host_place_keep_lag_join: the Survival game of
 *      20261110t015635_Survival.wbv. andrew-lenovo (16 slots, about 0 ms)
 *      came back while Elvis2's own host (16 slots, about 240 ms) ran 6
 *      bots. The keep rules moved none: no host was over its slots or
 *      bad. Rule 3 of botHostPlaceRebalanceKeep (lag) now moves bots to
 *      andrew-lenovo by the lag score and its 33% gain, and stops there.
 *      It never moves a bot by counts alone, never to or from the server,
 *      never to a host with no lag reading, a bad host or a full host,
 *      and not with the lag rules off.
 *   2. bot_host_sysinfo_empty_hosts: System Info lists the hosts by their
 *      bots, so a connected host with no bot was not listed at all.
 *      clientSimBotHostRowsNoBots gives those rows (not the server's).
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bot_host_link.h"
#include "client_sim.h"
#include "control_event.h"
#include "test_harness.h"

static const BotHostPlaceLag midLag = {
    BOT_HOST_LAG_REF_MS_DEFAULT, BOT_HOST_LAG_MAX_MS_DEFAULT,
    BOT_HOST_LAG_GAIN_DEFAULT
};

/* The places of that game: the server (5 slots), andrew-laptop (1),
 * andrew-bsmt (2), andrew-lenovo (16), Canuck's own host (0 slots) and
 * Elvis2's own host (16). */
enum { MID_SERVER, MID_LAPTOP, MID_BSMT, MID_LENOVO, MID_CANUCK, MID_ELVIS,
       MID_PLACES };

static void midTable(BotHostPlaceHost *h, int lenovoBots, int elvisBots) {
    static const int slots[MID_PLACES] = { 5, 1, 2, 16, 0, 16 };
    static const double lag[MID_PLACES] = { 0.0, 3.0, 4.0, 1.0, 60.0,
                                            240.0 };
    int i;
    memset(h, 0, MID_PLACES * sizeof(*h));
    for (i = 0; i < MID_PLACES; i++) {
        h[i].slots = slots[i];
        h[i].open = true;
        h[i].movable = true;
        h[i].lagMs = lag[i];
    }
    h[MID_SERVER].unlimited = true;
    h[MID_SERVER].capped = true;
    h[MID_SERVER].bots = 5;
    h[MID_LAPTOP].bots = 1;
    h[MID_BSMT].bots = 2;
    h[MID_LENOVO].bots = lenovoBots;
    h[MID_ELVIS].bots = elvisBots;
    h[MID_CANUCK].movable = false;
}

static bool midStep(const BotHostPlaceHost *h, int n,
                    const BotHostPlaceLag *lag, int *from, int *to,
                    BotHostSpreadWhy *why) {
    return botHostPlaceRebalanceKeep(h, n, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT, lag, false,
                                     from, to, why);
}

int run_bot_host_place_keep_lag_join(void) {
    BotHostPlaceHost h[MID_PLACES];
    BotHostSpreadWhy why;
    BotHostPlaceLag off = midLag;
    int from = -1, to = -1, moves = 0;

    /* andrew-lenovo back with 16 free slots: a bot of Elvis2's host goes
     * to it, by the lag rule. */
    midTable(h, 0, 6);
    UT_ASSERT(midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    UT_ASSERT_MSG(from == MID_ELVIS && to == MID_LENOVO &&
                      why == BOT_HOST_SPREAD_LAG,
                  "from %d to %d why %d", from, to, (int)why);

    /* Step by step until no move is left. Elvis2 at 240 ms counts
     * b / 16 x 3.4; andrew-lenovo at 1 ms (k + 1) / 16 x 1.01. A move
     * needs the target at most 67% of the source: 4 moves, then 5 x 1.01
     * against 0.67 x 2 x 3.4 = 4.56 stops it. */
    while (midStep(h, MID_PLACES, &midLag, &from, &to, &why)) {
        UT_ASSERT(from == MID_ELVIS && to == MID_LENOVO &&
                  why == BOT_HOST_SPREAD_LAG);
        h[from].bots--;
        h[to].bots++;
        UT_ASSERT(++moves <= 16);
    }
    printf("  lag join: %d moves -> lenovo %d, Elvis2 %d\n", moves,
           h[MID_LENOVO].bots, h[MID_ELVIS].bots);
    UT_ASSERT(moves == 4);
    UT_ASSERT(h[MID_LENOVO].bots == 4 && h[MID_ELVIS].bots == 2);
    /* The server, andrew-laptop and andrew-bsmt keep their bots. */
    UT_ASSERT(h[MID_SERVER].bots == 5 && h[MID_LAPTOP].bots == 1 &&
              h[MID_BSMT].bots == 2);

    /* No lag reading for andrew-lenovo yet: no move. */
    midTable(h, 0, 6);
    h[MID_LENOVO].lagMs = -1.0;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    /* None for the source either: no move. */
    midTable(h, 0, 6);
    h[MID_ELVIS].lagMs = -1.0;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));

    /* The lag rules off (-bothost-lag-ref-ms 0): no move. */
    midTable(h, 0, 6);
    off.refMs = 0.0;
    UT_ASSERT(!midStep(h, MID_PLACES, &off, &from, &to, &why));

    /* A small lag difference does not move: 1 + 60 / 100 = 1.6 is more
     * than 0.67 x (1 + 80 / 100) = 1.21. */
    midTable(h, 0, 6);
    h[MID_LENOVO].lagMs = 60.0;
    h[MID_ELVIS].lagMs = 80.0;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    /* The same lag: the counts alone move nothing. */
    h[MID_LENOVO].lagMs = 240.0;
    h[MID_ELVIS].lagMs = 240.0;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));

    /* A bad target, a full target or a closed target: no move. */
    midTable(h, 0, 6);
    h[MID_LENOVO].bad = true;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    midTable(h, 16, 6);
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    midTable(h, 0, 6);
    h[MID_LENOVO].open = false;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    /* The source may not move its bots now (an offer waits, or
     * -bothost-nomove): no move. */
    midTable(h, 0, 6);
    h[MID_ELVIS].movable = false;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));

    /* The server is never a target of the lag rule: andrew-lenovo gone
     * and the server with room, Elvis2's bots stay. */
    midTable(h, 0, 6);
    h[MID_LENOVO].open = false;
    h[MID_SERVER].bots = 2;
    UT_ASSERT(!midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    /* A bad source is rule 2's: it goes there, not by lag. */
    midTable(h, 0, 6);
    h[MID_ELVIS].bad = true;
    UT_ASSERT(midStep(h, MID_PLACES, &midLag, &from, &to, &why));
    UT_ASSERT(from == MID_ELVIS && to == MID_LENOVO &&
              why == BOT_HOST_SPREAD_BAD);
    return 0;
}

/* ================================================================
 * 2. System Info's hosts with no bot
 * ================================================================ */

static void midRow(ClientSim *cs, uint8_t idx, bool connected,
                   const char *name, uint16_t seats) {
    ControlEvent evt;
    uint16_t m = seats;
    uint8_t used = 0;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_STATUS;
    evt.u.botHostStatus.idx = idx;
    evt.u.botHostStatus.connected = connected ? 1 : 0;
    if (connected) {
        while (m != 0) {
            used = (uint8_t)(used + (m & 1u));
            m >>= 1;
        }
        evt.u.botHostStatus.state = BOT_HOST_SYNC_READY;
        evt.u.botHostStatus.percent = 100;
        evt.u.botHostStatus.slots = 16;
        evt.u.botHostStatus.used = used;
        evt.u.botHostStatus.seats = seats;
        SDL_strlcpy(evt.u.botHostStatus.name, name,
                    sizeof(evt.u.botHostStatus.name));
    }
    clientSimApplyControl(cs, &evt);
}

int run_bot_host_sysinfo_empty_hosts(void) {
    ClientSim *cs = clientSimAlloc();
    uint8_t idx[CLIENT_BOT_HOST_ROWS];
    int n;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(clientSimBotHostRowsNoBots(cs, idx, CLIENT_BOT_HOST_ROWS) == 0);
    UT_ASSERT(clientSimBotHostRowsNoBots(NULL, idx, CLIENT_BOT_HOST_ROWS) == 0);
    UT_ASSERT(clientSimBotHostRowsNoBots(cs, NULL, CLIENT_BOT_HOST_ROWS) == 0);

    /* Elvis2 with 6 bots, andrew-lenovo with none, the server's own row
     * with none. */
    midRow(cs, 1, true, "Elvis2", 0x0FA0u);
    midRow(cs, 3, true, "andrew-lenovo", 0);
    midRow(cs, CLIENT_BOT_HOST_SERVER_ROW, true, "Server", 0);
    n = clientSimBotHostRowsNoBots(cs, idx, CLIENT_BOT_HOST_ROWS);
    UT_ASSERT_MSG(n == 1 && idx[0] == 3, "n %d first %d", n,
                  n > 0 ? (int)idx[0] : -1);

    /* A second host with none, in row order; cap honoured. */
    midRow(cs, 0, true, "andrew-laptop", 0);
    n = clientSimBotHostRowsNoBots(cs, idx, CLIENT_BOT_HOST_ROWS);
    UT_ASSERT(n == 2 && idx[0] == 0 && idx[1] == 3);
    UT_ASSERT(clientSimBotHostRowsNoBots(cs, idx, 1) == 1 && idx[0] == 0);
    UT_ASSERT(clientSimBotHostRowsNoBots(cs, idx, 0) == 0);

    /* andrew-lenovo gets a bot: the bot list shows it from then on. */
    midRow(cs, 3, true, "andrew-lenovo", 0x0001u);
    n = clientSimBotHostRowsNoBots(cs, idx, CLIENT_BOT_HOST_ROWS);
    UT_ASSERT(n == 1 && idx[0] == 0);

    /* A host that goes is not listed. */
    midRow(cs, 0, false, "", 0);
    UT_ASSERT(clientSimBotHostRowsNoBots(cs, idx, CLIENT_BOT_HOST_ROWS) == 0);

    clientSimDestroy(cs);
    return 0;
}
