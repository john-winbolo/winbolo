/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The default bot placement: a bot stays where it runs well
 * (botHostPlaceRebalanceKeep), the stay rule that stops a quick way back
 * (BotHostTrail, and backTo in botHostOvrPickMove) and the free CPU test
 * of a place's own bots (botHostOverrunFreeLow), all in bot_host_link.c.
 *
 *   1. bot_host_place_keep_server_first: new bots stay on the server while
 *      it is good and has slots; only the rest go to Bot Hosts. A bad
 *      server gives one bot; a held one gives none.
 *   2. bot_host_place_keep_lopsided: a lopsided but good layout does not
 *      move (the even spread would move it). A host over its slots gives.
 *   3. bot_host_place_keep_bad_source: a bad place gives a bot to a good
 *      place, the server first; never to a bad place; a bad server and a
 *      bad host found by their free CPU numbers.
 *   4. bot_host_place_keep_no_return: a bot that left a place does not go
 *      back there within the stay time, by the spread or by the overrun
 *      move (only the emergency rule may).
 *   5. bot_host_place_keep_oil_rig: the lobby of
 *      20261108t111803_DH-Oil_Rig.wbv (436.6 s and 438.8 s): the even
 *      spread moved bot 0 off andrew-bsmt when a server bot left, then
 *      sent the same bot back 2.2 s later when a new bot put the server
 *      over its slots. Now neither move happens, and a forced move takes
 *      another bot.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bot_host_link.h"
#include "test_harness.h"

static const BotHostPlaceLag keepLag = {
    BOT_HOST_LAG_REF_MS_DEFAULT, BOT_HOST_LAG_MAX_MS_DEFAULT,
    BOT_HOST_LAG_GAIN_DEFAULT
};

/* The server itself (place 0: unlimited, lag 0) and up to 3 Bot Hosts. */
static void keepTable(BotHostPlaceHost *h, int n, int serverSlots,
                      int serverBots, const int *slots, const int *bots,
                      double lagMs) {
    int i;
    memset(h, 0, (size_t)n * sizeof(*h));
    h[0].slots = serverSlots;
    h[0].bots = serverBots;
    h[0].open = true;
    h[0].unlimited = true;
    h[0].movable = true;
    h[0].lagMs = 0.0;
    for (i = 1; i < n; i++) {
        h[i].slots = slots[i - 1];
        h[i].bots = bots[i - 1];
        h[i].open = true;
        h[i].movable = true;
        h[i].lagMs = lagMs;
    }
}

static bool keepStep(const BotHostPlaceHost *h, int n, bool even, int *from,
                     int *to, BotHostSpreadWhy *why) {
    return botHostPlaceRebalanceKeep(h, n, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag,
                                     even, from, to, why);
}

/* Moves by the rules until none is left; -1 when it does not stop. */
static int keepSettle(BotHostPlaceHost *h, int n, bool even) {
    int moves = 0, from = -1, to = -1;
    BotHostSpreadWhy why;
    while (keepStep(h, n, even, &from, &to, &why)) {
        h[from].bots--;
        h[to].bots++;
        if (++moves > 200) return -1;
    }
    return moves;
}

/* ================================================================
 * 1. The server first
 * ================================================================ */

int run_bot_host_place_keep_server_first(void) {
    static const int slots[2] = { 2, 4 };
    int bots[2] = { 0, 0 };
    BotHostPlaceHost h[3];
    BotHostSpreadWhy why;
    int k, from = -1, to = -1;

    /* Bots added one at a time on the server (each new bot starts there):
     * 8 stay on its 8 slots, the rest go to the Bot Hosts by their score
     * until both are full. */
    keepTable(h, 3, 8, 0, slots, bots, 2.0);
    for (k = 1; k <= 14; k++) {
        h[0].bots++;
        UT_ASSERT(keepSettle(h, 3, false) >= 0);
        printf("  keep: %2d bots -> server %d, host(2) %d, host(4) %d\n", k,
               h[0].bots, h[1].bots, h[2].bots);
        UT_ASSERT(h[0].bots + h[1].bots + h[2].bots == k);
        if (k <= 8) {
            UT_ASSERT(h[0].bots == k && h[1].bots == 0 && h[2].bots == 0);
        }
        UT_ASSERT(h[1].bots <= 2 && h[2].bots <= 4);
    }
    UT_ASSERT(h[0].bots == 8 && h[1].bots == 2 && h[2].bots == 4);
    /* The 9th bot alone: a forced move, to the 4-slot host. */
    keepTable(h, 3, 8, 9, slots, bots, 2.0);
    UT_ASSERT(keepStep(h, 3, false, &from, &to, &why));
    UT_ASSERT(from == 0 && to == 2 && why == BOT_HOST_SPREAD_OVER_SLOTS);

    /* The even spread puts bots on the hosts while the server has room. */
    keepTable(h, 3, 8, 8, slots, bots, 2.0);
    UT_ASSERT(keepSettle(h, 3, true) > 0);
    UT_ASSERT(h[0].bots < 8);

    /* A bad server gives one bot, to the best good host. */
    keepTable(h, 3, 8, 5, slots, bots, 2.0);
    h[0].bad = true;
    UT_ASSERT(keepStep(h, 3, false, &from, &to, &why));
    UT_ASSERT(from == 0 && to == 2 && why == BOT_HOST_SPREAD_BAD);
    /* Held (it gave one within the step time): none. */
    h[0].badHold = true;
    UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    UT_ASSERT(why == BOT_HOST_SPREAD_NONE);
    /* Good again: none. */
    h[0].bad = false;
    h[0].badHold = false;
    UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    /* A server bot that may not move now (a pill, its builder out): none. */
    h[0].bad = true;
    h[0].movable = false;
    UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    return 0;
}

/* ================================================================
 * 2. A lopsided good layout stays
 * ================================================================ */

int run_bot_host_place_keep_lopsided(void) {
    static const int slots[2] = { 2, 4 };
    BotHostPlaceHost h[3];
    BotHostSpreadWhy why;
    int from = -1, to = -1;

    /* The server with 1 of 8, the 4-slot host full, 30 ms away. */
    {
        int bots[2] = { 0, 4 };
        keepTable(h, 3, 8, 1, slots, bots, 30.0);
        UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
        UT_ASSERT(keepSettle(h, 3, false) == 0);
        UT_ASSERT(h[0].bots == 1 && h[2].bots == 4);
        /* The even spread would move bots to the server. */
        UT_ASSERT(keepStep(h, 3, true, &from, &to, &why));
        UT_ASSERT(from == 2 && to == 0 && why == BOT_HOST_SPREAD_EVEN);
    }
    /* The server full, both hosts empty. */
    {
        int bots[2] = { 0, 0 };
        keepTable(h, 3, 8, 8, slots, bots, 2.0);
        UT_ASSERT(keepSettle(h, 3, false) == 0);
        UT_ASSERT(h[0].bots == 8);
    }
    /* The server empty, both hosts full. */
    {
        int bots[2] = { 2, 4 };
        keepTable(h, 3, 8, 0, slots, bots, 2.0);
        UT_ASSERT(keepSettle(h, 3, false) == 0);
    }
    /* A host over its slots (a lowered limit, a lower slot count) gives:
     * to the server first. */
    {
        int bots[2] = { 0, 5 };
        keepTable(h, 3, 8, 3, slots, bots, 2.0);
        UT_ASSERT(keepStep(h, 3, false, &from, &to, &why));
        UT_ASSERT(from == 2 && to == 0 && why == BOT_HOST_SPREAD_OVER_SLOTS);
        UT_ASSERT(keepSettle(h, 3, false) == 1);
        UT_ASSERT(h[0].bots == 4 && h[2].bots == 4);
    }
    /* A host with bots and no slots gives them all. */
    {
        int bots[2] = { 2, 0 };
        keepTable(h, 3, 8, 3, slots, bots, 2.0);
        h[1].slots = 0;
        UT_ASSERT(keepSettle(h, 3, false) == 2);
        UT_ASSERT(h[0].bots == 5 && h[1].bots == 0);
    }
    /* The server over its slots with no host room: it keeps them. */
    {
        int bots[2] = { 2, 4 };
        keepTable(h, 3, 8, 10, slots, bots, 2.0);
        UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    }
    return 0;
}

/* ================================================================
 * 3. A bad source gives
 * ================================================================ */

/* One probe of 100 thinks and no overruns. */
static void keepFeed(BotHostOverrunState *st, const BotHostOverrunCfg *cfg,
                     int bots, uint8_t freePct, uint8_t botCpu) {
    BotHostOverrunProbe in;
    memset(&in, 0, sizeof(in));
    in.thinks = 100;
    in.bots = bots;
    in.slots = 8;
    in.freePct = freePct;
    in.botCpu = botCpu;
    (void)botHostOverrunProbe(st, cfg, &in, NULL);
}

int run_bot_host_place_keep_bad_source(void) {
    static const int slots[2] = { 2, 4 };
    BotHostPlaceHost h[3];
    BotHostSpreadWhy why;
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int from = -1, to = -1, k;

    /* The 4-slot host bad, the server with room: to the server. */
    {
        int bots[2] = { 0, 4 };
        keepTable(h, 3, 8, 3, slots, bots, 2.0);
        h[2].bad = true;
        UT_ASSERT(keepStep(h, 3, false, &from, &to, &why));
        UT_ASSERT(from == 2 && to == 0 && why == BOT_HOST_SPREAD_BAD);
    }
    /* The server full: to the other good host; the server takes no bot
     * past its slots for a bad host. */
    {
        int bots[2] = { 0, 4 };
        keepTable(h, 3, 8, 8, slots, bots, 2.0);
        h[2].bad = true;
        UT_ASSERT(keepStep(h, 3, false, &from, &to, &why));
        UT_ASSERT(from == 2 && to == 1 && why == BOT_HOST_SPREAD_BAD);
        /* That host bad too: no good place, no move. */
        h[1].bad = true;
        UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
        /* That host full: no move either. */
        h[1].bad = false;
        h[1].bots = 2;
        UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    }
    /* A bad server is not a target. */
    {
        int bots[2] = { 2, 4 };
        keepTable(h, 3, 8, 3, slots, bots, 2.0);
        h[0].bad = true;
        h[2].bad = true;
        UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    }

    /* The free CPU of a place's own bots. Unknown numbers count as good. */
    botHostOverrunCfgDefaults(&cfg);
    botHostOverrunReset(&st);
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 0, 60, &rep));
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 1, 60, &rep));
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 3, 60, &rep));
    UT_ASSERT(strcmp(rep.why, "no reading yet") == 0);
    /* 2 bots at 90% free, 20% bot CPU (10% a bot): good. */
    for (k = 0; k < 30; k++) keepFeed(&st, &cfg, 2, 90, 20);
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 2, 90, &rep));
    /* 2 bots at 30% free, 80% bot CPU (40% a bot): the check for the
     * second bot says no (predicted -10%, under one bot's 40%). */
    botHostOverrunReset(&st);
    for (k = 0; k < 30; k++) keepFeed(&st, &cfg, 2, 30, 80);
    UT_ASSERT(botHostOverrunFreeLow(&st, &cfg, 2, 30, &rep));
    UT_ASSERT_MSG(strcmp(rep.why, "too little free CPU") == 0, "%s", rep.why);
    /* Its first bot alone is always good. */
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 1, 30, &rep));
    /* A 3rd bot not in a reading yet: not known, good. */
    botHostOverrunReset(&st);
    for (k = 0; k < 30; k++) keepFeed(&st, &cfg, 1, 30, 40);
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 3, 30, &rep));
    /* The check off: good. */
    botHostOverrunReset(&st);
    for (k = 0; k < 30; k++) keepFeed(&st, &cfg, 2, 30, 80);
    cfg.pct = 0.0;
    UT_ASSERT(!botHostOverrunFreeLow(&st, &cfg, 2, 30, &rep));
    return 0;
}

/* ================================================================
 * 4. No quick way back
 * ================================================================ */

static BotHostOvrHost keepOvrHost(double ratePct, int bots) {
    BotHostOvrHost h;
    memset(&h, 0, sizeof(h));
    h.rateKnown = bots > 0;
    h.ratePct = bots > 0 ? ratePct : 0.0;
    h.bots = bots;
    h.mayTake = true;
    h.slots = 8;
    return h;
}

int run_bot_host_place_keep_no_return(void) {
    BotHostTrail t;
    BotHostOverrunCfg cfg;
    BotHostOvrHost oh[3];
    BotHostOvrBot b;
    BotHostOvrMove mv;
    uint32_t stay = 30u * 50u;   /* the default stay time, 30 s */
    int seats2[2] = { 3, 5 };
    int seats1[1] = { 3 };

    botHostTrailReset(&t);
    UT_ASSERT(botHostTrailBackTo(&t, 3, 1000, stay) == -1);
    /* Bot 3 left place 1 at tick 1000. */
    botHostTrailNote(&t, 3, 1, 1000);
    UT_ASSERT(botHostTrailBackTo(&t, 3, 1000 + 110, stay) == 1);
    UT_ASSERT(botHostTrailBackTo(&t, 3, 1000 + stay - 1, stay) == 1);
    UT_ASSERT(botHostTrailBackTo(&t, 3, 1000 + stay, stay) == -1);
    UT_ASSERT(botHostTrailBackTo(&t, 3, 1000 + 110, 0) == -1);
    /* To place 1 within the stay time: another bot goes. */
    UT_ASSERT(botHostTrailPickSeat(&t, seats2, 2, 1, 1110, stay, false) == 5);
    UT_ASSERT(botHostTrailPickSeat(&t, seats2, 2, 1, 1110, stay, true) == 5);
    /* Only that bot: none, unless forced. */
    UT_ASSERT(botHostTrailPickSeat(&t, seats1, 1, 1, 1110, stay, false) == -1);
    UT_ASSERT(botHostTrailPickSeat(&t, seats1, 1, 1, 1110, stay, true) == 3);
    /* To another place, or after the stay time: it may go. */
    UT_ASSERT(botHostTrailPickSeat(&t, seats1, 1, 2, 1110, stay, false) == 3);
    UT_ASSERT(botHostTrailPickSeat(&t, seats1, 1, 1, 1000 + stay, stay,
                                   false) == 3);
    /* Its seat empties: it starts again. */
    botHostTrailClear(&t, 3);
    UT_ASSERT(botHostTrailBackTo(&t, 3, 1110, stay) == -1);

    /* The overrun move: the server at 3%, host 1 at 0.2% (where the bot
     * came from), host 2 at 0.5%. Without the rule host 1 is best. */
    botHostOverrunCfgDefaults(&cfg);
    oh[0] = keepOvrHost(3.0, 4);
    oh[1] = keepOvrHost(0.2, 1);
    oh[2] = keepOvrHost(0.5, 1);
    memset(&b, 0, sizeof(b));
    b.seat = 0;
    UT_ASSERT(botHostOvrPickMove(&cfg, oh, 3, 0, &b, 1, NULL, &mv));
    UT_ASSERT(mv.to == 1);
    /* It left host 1 within the stay time: host 2. */
    b.back = true;
    b.backTo = 1;
    UT_ASSERT(botHostOvrPickMove(&cfg, oh, 3, 0, &b, 1, NULL, &mv));
    UT_ASSERT(mv.to == 2 && mv.rule == BOT_HOST_OVR_RULE_NORMAL);
    /* Host 1 the only target: no move. */
    oh[2].mayTake = false;
    UT_ASSERT(!botHostOvrPickMove(&cfg, oh, 3, 0, &b, 1, NULL, &mv));
    /* The emergency rule (5% or more) may still send it back. */
    oh[0].ratePct = 6.0;
    UT_ASSERT(botHostOvrPickMove(&cfg, oh, 3, 0, &b, 1, NULL, &mv));
    UT_ASSERT(mv.to == 1 && mv.rule == BOT_HOST_OVR_RULE_EMERGENCY);
    return 0;
}

/* ================================================================
 * 5. The Oil Rig lobby (436.6 s, 438.8 s)
 * ================================================================ */

/* The places of that lobby: the server (8 slots), andrew-bsmt (2 slots,
 * lag lagMs) and Canuck's own host (0 slots). */
static void keepOilRig(BotHostPlaceHost *h, int serverBots, int bsmtBots,
                       double lagMs) {
    static const int slots[2] = { 2, 0 };
    int bots[2];
    bots[0] = bsmtBots;
    bots[1] = 0;
    keepTable(h, 3, 8, serverBots, slots, bots, lagMs);
    h[2].movable = false;
}

int run_bot_host_place_keep_oil_rig(void) {
    BotHostPlaceHost h[3];
    BotHostSpreadWhy why;
    BotHostTrail t;
    uint32_t stay = 30u * 50u;
    uint32_t t436 = 21828;            /* 436.56 s at 50 entries a second */
    uint32_t t438 = 21939;            /* 438.78 s */
    /* The server's own bots after bot 0 came back, in seat order (player
     * 3 is Canuck; bot 1 stayed on andrew-bsmt). */
    int local[8] = { 0, 2, 4, 5, 6, 7, 8, 9 };
    int from = -1, to = -1;

    /* 436.54 s: Chipper (player 9, a server bot) leaves: the server 7 of
     * 8, andrew-bsmt 2 of 2. */
    keepOilRig(h, 7, 2, 40.0);
    /* The old even spread moved a bot off andrew-bsmt: its score 2 / 1.75
     * x (1 + 40 / 100) = 1.6 against the server's 8 / 8 = 1.0, a gain of
     * more than 33%. That needs a lag of 31 ms or more (or one not known);
     * at 2 ms it would not move. */
    UT_ASSERT(keepStep(h, 3, true, &from, &to, &why));
    UT_ASSERT(from == 1 && to == 0 && why == BOT_HOST_SPREAD_EVEN);
    keepOilRig(h, 7, 2, 2.0);
    UT_ASSERT(!keepStep(h, 3, true, &from, &to, &why));
    /* Now: andrew-bsmt is good, so no move at any lag. */
    keepOilRig(h, 7, 2, 40.0);
    UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));
    keepOilRig(h, 7, 2, 250.0);
    h[1].lagMs = -1.0;                /* a lag not known */
    UT_ASSERT(!keepStep(h, 3, false, &from, &to, &why));

    /* The old path on: bot 0 back on the server (8 of 8, andrew-bsmt 1).
     * Rooster leaves (7), Lennon joins (8): no move either way. */
    botHostTrailReset(&t);
    botHostTrailNote(&t, 0, 1, t436);
    keepOilRig(h, 7, 1, 40.0);
    UT_ASSERT(!keepStep(h, 3, true, &from, &to, &why));
    keepOilRig(h, 8, 1, 40.0);
    UT_ASSERT(!keepStep(h, 3, true, &from, &to, &why));
    /* 438.76 s: Mozart joins: the server 9 of 8 while andrew-bsmt has
     * room: a forced move to it, in both modes. */
    keepOilRig(h, 9, 1, 40.0);
    UT_ASSERT(keepStep(h, 3, true, &from, &to, &why));
    UT_ASSERT(from == 0 && to == 1 && why == BOT_HOST_SPREAD_OVER_SLOTS);
    UT_ASSERT(keepStep(h, 3, false, &from, &to, &why));
    UT_ASSERT(from == 0 && to == 1 && why == BOT_HOST_SPREAD_OVER_SLOTS);
    /* The old pick was the first server bot, bot 0, which had left
     * andrew-bsmt 2.2 s before. Now another bot goes. */
    UT_ASSERT(botHostTrailBackTo(&t, 0, t438, stay) == 1);
    UT_ASSERT(botHostTrailPickSeat(&t, local, 8, to, t438, stay, true) == 2);

    /* The whole lobby with the new rules: the server 8, 7, 6, 7, 8 bots
     * and andrew-bsmt 2 throughout: no move at all, the layout the game
     * started with (482.8 s: the server 8 of 8, andrew-bsmt 2 of 2). */
    {
        static const int serverBots[6] = { 8, 7, 6, 7, 8, 8 };
        int k;
        for (k = 0; k < 6; k++) {
            keepOilRig(h, serverBots[k], 2, 40.0);
            UT_ASSERT(keepSettle(h, 3, false) == 0);
        }
    }
    return 0;
}
