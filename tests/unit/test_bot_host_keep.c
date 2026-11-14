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
 *   6. bot_host_place_keep_scaled_wait: the wait between two moves off a
 *      host is stepSecs / (rate / pct), never below stepMinSecs: 30 s at
 *      0.5%, 15 s at 1%, 6 s at 2.5%, 5 s at 3% and more. After a move the
 *      rate comes from the probes since the move, once they hold 250
 *      thinks.
 *   7. bot_host_place_keep_step_min_old: stepMinSecs = stepSecs gives the
 *      old fixed 30 s step and the full window after each move.
 *   8. bot_host_place_keep_free_batch: a place whose free CPU is short by
 *      3 bots gives 3 bots in one step, the server first; its limit drops
 *      to its bots - 3. With no room numbers each target takes one.
 *   9. bot_host_place_keep_batch_no_target: the batch stops when no good
 *      place is left.
 *  10. bot_host_place_keep_batch_off: -bothost-free-batch 0 (and stale
 *      free CPU numbers) give one bot by the usual rule.
 *  11. bot_host_place_keep_batch_server_room: the server, with free CPU
 *      room for 3 bots, takes 3 of a batch of 4; the 4th goes to a good
 *      Bot Host.
 *  12. bot_host_place_keep_batch_no_reading: a target with no free CPU
 *      reading (or no one-bot cost) takes 1.
 *  13. bot_host_place_keep_batch_slot_limit: a target takes no more than
 *      its free slots, however much free CPU room it has.
 *  14. bot_host_place_keep_batch_cpu_limit: a Bot Host target takes no
 *      more than its free CPU room; the full server takes none.
 *  15. bot_host_enforce_fall_back_holds: with -bothost-enforce-server-limit
 *      the bots that come back from a host past the server's slots are
 *      held, and no placement puts a bot on a full server.
 *  16. bot_host_enforce_host_resumes: held bots run again, the longest
 *      held first, on the server while it has a slot, then on a good Bot
 *      Host that connects.
 *  17. bot_host_enforce_leave_resumes: a bot that leaves the server lets
 *      one held bot run again there.
 *  18. bot_host_enforce_off_keeps_fall_back: -bothost-enforce-server-limit
 *      0 holds no bot; the server takes bots past its slots, as before.
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

/* The values the tests before the scaled wait were written for: a 1%
 * target rate, a fixed 30 s step and one bot a step for low free CPU. */
static void keepCfgOld(BotHostOverrunCfg *cfg) {
    botHostOverrunCfgDefaults(cfg);
    cfg->pct = 1.0;
    cfg->stepMinSecs = 30;
    cfg->freeBatch = false;
}

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
    keepCfgOld(&cfg);
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
    keepCfgOld(&cfg);
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

/* ================================================================
 * 6. The scaled wait
 * ================================================================ */

static bool keepNear(double a, double b) {
    return a > b - 0.0001 && a < b + 0.0001;
}

/* One probe of thinks thinks and overruns overruns on a host with bots
 * bots (8 slots, 40% free, 20% bot CPU). */
static BotHostOverrunAction keepProbe(BotHostOverrunState *st,
                                      const BotHostOverrunCfg *cfg,
                                      uint32_t thinks, uint32_t overruns,
                                      int bots, BotHostOverrunReport *rep) {
    BotHostOverrunProbe in;
    memset(&in, 0, sizeof(in));
    in.thinks = thinks;
    in.overruns = overruns;
    in.bots = bots;
    in.slots = 8;
    in.freePct = 40;
    in.botCpu = 20;
    return botHostOverrunProbe(st, cfg, &in, rep);
}

/* Probes at a steady rate from a fresh state; after the first move the
 * probes since it count. Returns the probe of the second move after the
 * first (0: none in 120 probes); *first gets the first move's probe. */
static int keepSecondMove(const BotHostOverrunCfg *cfg, uint32_t thinks,
                          uint32_t overruns, int *first,
                          BotHostOverrunReport *firstRep) {
    BotHostOverrunState st;
    BotHostOverrunReport rep;
    int k, bots = 6;
    botHostOverrunReset(&st);
    *first = 0;
    for (k = 1; k <= 120; k++) {
        if (keepProbe(&st, cfg, thinks, overruns, bots, &rep) !=
            BOT_HOST_OVR_OVER) {
            continue;
        }
        if (*first == 0) {
            *first = k;
            if (firstRep != NULL) *firstRep = rep;
            (void)botHostOverrunMoved(&st, bots);
            bots--;
            continue;
        }
        return k - *first;
    }
    return 0;
}

int run_bot_host_place_keep_scaled_wait(void) {
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int first, gap;

    botHostOverrunCfgDefaults(&cfg);
    UT_ASSERT(keepNear(cfg.pct, 0.5));
    UT_ASSERT(cfg.stepSecs == 30 && cfg.stepMinSecs == 5 && cfg.freeBatch);
    UT_ASSERT(botHostOverrunScaled(&cfg));
    /* The wait at each rate (severity = rate / 0.5%). */
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 0.5), 30.0));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 1.0), 15.0));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 2.5), 6.0));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 3.0), 5.0));
    /* The 5 s floor. */
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 6.0), 5.0));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 50.0), 5.0));
    /* A rate not over the target waits the step time. */
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 0.2), 30.0));
    printf("  wait: 0.5%% %.1f s, 1%% %.1f s, 2.5%% %.1f s, 3%% %.1f s, "
           "6%% %.1f s\n",
           botHostOverrunWaitSecs(&cfg, 0.5), botHostOverrunWaitSecs(&cfg, 1.0),
           botHostOverrunWaitSecs(&cfg, 2.5), botHostOverrunWaitSecs(&cfg, 3.0),
           botHostOverrunWaitSecs(&cfg, 6.0));

    /* The probes: the first move needs a full window (30 s) at the game
     * start; the next comes after the wait of the rate since the move. */
    gap = keepSecondMove(&cfg, 100, 1, &first, &rep);       /* 1% */
    UT_ASSERT_MSG(first == 30 && gap == 15, "1%%: %d then +%d", first, gap);
    UT_ASSERT(keepNear(rep.severity, 2.0) && keepNear(rep.waitSecs, 15.0));
    gap = keepSecondMove(&cfg, 200, 5, &first, &rep);       /* 2.5% */
    UT_ASSERT_MSG(first == 30 && gap == 6, "2.5%%: %d then +%d", first, gap);
    UT_ASSERT(keepNear(rep.severity, 5.0) && keepNear(rep.waitSecs, 6.0));
    gap = keepSecondMove(&cfg, 100, 3, &first, &rep);       /* 3% */
    UT_ASSERT_MSG(first == 30 && gap == 5, "3%%: %d then +%d", first, gap);
    UT_ASSERT(keepNear(rep.waitSecs, 5.0));
    gap = keepSecondMove(&cfg, 100, 10, &first, &rep);      /* 10% */
    UT_ASSERT_MSG(first == 30 && gap == 5, "10%%: %d then +%d", first, gap);
    /* 0.75%: 20 s. */
    gap = keepSecondMove(&cfg, 400, 3, &first, &rep);
    UT_ASSERT_MSG(first == 30 && gap == 20, "0.75%%: %d then +%d", first,
                  gap);
    /* The thinks floor: 20 thinks a second at 5% (wait 5 s) has 250 thinks
     * only after 13 probes. */
    gap = keepSecondMove(&cfg, 20, 1, &first, &rep);
    UT_ASSERT_MSG(first == 30 && gap == 13, "floor: %d then +%d", first, gap);
    /* At the target rate: never over it, no move. */
    gap = keepSecondMove(&cfg, 200, 1, &first, &rep);
    UT_ASSERT(first == 0 && gap == 0);
    return 0;
}

/* ================================================================
 * 7. stepMinSecs = stepSecs: the old step
 * ================================================================ */

int run_bot_host_place_keep_step_min_old(void) {
    BotHostOverrunCfg cfg;
    BotHostOverrunState st;
    BotHostOverrunReport rep;
    double pct;
    int first, gap, k;

    botHostOverrunCfgDefaults(&cfg);
    cfg.stepMinSecs = 30;
    UT_ASSERT(!botHostOverrunScaled(&cfg));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 1.0), 30.0));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 3.0), 30.0));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 50.0), 30.0));
    /* Above the step time: the same. */
    cfg.stepMinSecs = 60;
    UT_ASSERT(!botHostOverrunScaled(&cfg));
    UT_ASSERT(keepNear(botHostOverrunWaitSecs(&cfg, 3.0), 30.0));
    cfg.stepMinSecs = 30;
    /* Each move 30 s after the last, at any rate. */
    gap = keepSecondMove(&cfg, 100, 1, &first, &rep);
    UT_ASSERT_MSG(first == 30 && gap == 30, "1%%: %d then +%d", first, gap);
    gap = keepSecondMove(&cfg, 100, 10, &first, &rep);
    UT_ASSERT_MSG(first == 30 && gap == 30, "10%%: %d then +%d", first, gap);
    /* After a move no rate until a full window. */
    botHostOverrunReset(&st);
    for (k = 1; k <= 30; k++) (void)keepProbe(&st, &cfg, 100, 3, 4, &rep);
    (void)botHostOverrunMoved(&st, 4);
    for (k = 1; k < 30; k++) {
        (void)keepProbe(&st, &cfg, 100, 3, 3, &rep);
        UT_ASSERT(!botHostOverrunRate(&st, &cfg, &pct));
    }
    (void)keepProbe(&st, &cfg, 100, 3, 3, &rep);
    UT_ASSERT(botHostOverrunRate(&st, &cfg, &pct) && keepNear(pct, 3.0));
    /* With the scaled wait the rate since the move is known after 5. */
    cfg.stepMinSecs = 5;
    botHostOverrunReset(&st);
    for (k = 1; k <= 30; k++) (void)keepProbe(&st, &cfg, 100, 3, 4, &rep);
    (void)botHostOverrunMoved(&st, 4);
    for (k = 1; k < 5; k++) {
        (void)keepProbe(&st, &cfg, 100, 3, 3, &rep);
        UT_ASSERT(!botHostOverrunRate(&st, &cfg, &pct));
    }
    (void)keepProbe(&st, &cfg, 100, 3, 3, &rep);
    UT_ASSERT(botHostOverrunRate(&st, &cfg, &pct) && keepNear(pct, 3.0));
    /* A new game starts again with the full window. */
    (void)botHostOverrunNewGame(&st, &cfg);
    for (k = 1; k < 30; k++) {
        (void)keepProbe(&st, &cfg, 100, 3, 3, &rep);
        UT_ASSERT(!botHostOverrunRate(&st, &cfg, &pct));
    }
    return 0;
}

/* ================================================================
 * 8 to 10. The free CPU batch
 * ================================================================ */

/* One probe with the given numbers (100 thinks, 8 slots). */
static void keepFeedFree(BotHostOverrunState *st, const BotHostOverrunCfg *cfg,
                         int bots, uint32_t overruns, uint8_t freePct,
                         uint8_t botCpu) {
    BotHostOverrunProbe in;
    memset(&in, 0, sizeof(in));
    in.thinks = 100;
    in.overruns = overruns;
    in.bots = bots;
    in.slots = 8;
    in.freePct = freePct;
    in.botCpu = botCpu;
    (void)botHostOverrunProbe(st, cfg, &in, NULL);
}

/* A place with 5 bots whose free CPU is short by 3: 30 s clean with 3 bots
 * at 40% free, then 30 s bad with 5 bots at 5% free and 100% bot CPU (20%
 * a bot). The check needs a predicted free CPU of at least 40% (the
 * lowest clean reading): 5 + 20 k - 20 >= 40, so k = 3. */
static void keepShortBy3(BotHostOverrunState *st,
                         const BotHostOverrunCfg *cfg) {
    int k;
    botHostOverrunReset(st);
    for (k = 0; k < 30; k++) keepFeedFree(st, cfg, 3, 0, 40, 60);
    for (k = 0; k < 30; k++) keepFeedFree(st, cfg, 5, 5, 5, 100);
}

int run_bot_host_place_keep_free_batch(void) {
    static const int slots[3] = { 4, 4, 8 };
    int bots[3] = { 0, 1, 5 };
    BotHostPlaceHost h[4];
    BotHostOverrunCfg cfg;
    BotHostOverrunState st;
    BotHostOverrunReport rep;
    BotHostSpreadWhy why;
    int to[8], n, given, from = -1, dst = -1;

    botHostOverrunCfgDefaults(&cfg);
    keepShortBy3(&st, &cfg);
    UT_ASSERT(botHostOverrunFreeLow(&st, &cfg, 5, 5, &rep));
    n = botHostOverrunFreeShort(&st, &cfg, 5, 5, &rep);
    printf("  batch: short by %d (free %.1f%%, one-bot cost %.1f%%, lowest "
           "clean %d%%, highest bad %d%%)\n", n, rep.freeAvg,
           rep.oneBotCost, rep.lowestClean, rep.highestBad);
    UT_ASSERT_MSG(n == 3, "short by %d", n);
    /* The batch is on and the numbers are fresh: 3 bots. */
    UT_ASSERT(botHostOverrunBatchSize(&cfg, n, true) == 3);

    /* The server (8 slots, 2 bots), hosts of 4 slots with 0 and 1 bots,
     * and the bad place (8 slots, 5 bots). */
    keepTable(h, 4, 8, 2, slots, bots, 2.0);
    h[3].bad = true;
    /* Rule 2 of the rebalance picks the same place and first target. */
    UT_ASSERT(keepStep(h, 4, false, &from, &dst, &why));
    UT_ASSERT(from == 3 && dst == 0 && why == BOT_HOST_SPREAD_BAD);
    given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3, n,
                                 NULL, to);
    UT_ASSERT_MSG(given == 3, "given %d", given);
    /* The server first, then the emptier host, then the other. */
    UT_ASSERT_MSG(to[0] == 0 && to[1] == 1 && to[2] == 2,
                  "to %d %d %d", to[0], to[1], to[2]);
    UT_ASSERT(h[3].bots == 2 && h[0].bots == 3 && h[1].bots == 1 &&
              h[2].bots == 2);
    /* The limit drops to bots - 3. */
    UT_ASSERT(botHostOverrunMovedN(&st, 5, given) == 2);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 2);
    UT_ASSERT(st.probeCount == 0 && st.sinceChange == 0);

    /* The count is at most bots - 1: with 2 bots it is 1 (its first bot
     * alone always passes). With 1 bot it is not short at all. */
    keepShortBy3(&st, &cfg);
    UT_ASSERT(botHostOverrunFreeShort(&st, &cfg, 2, 5, &rep) == 1);
    UT_ASSERT(botHostOverrunFreeShort(&st, &cfg, 1, 5, &rep) == 0);
    return 0;
}

int run_bot_host_place_keep_batch_no_target(void) {
    static const int slots[3] = { 4, 4, 8 };
    BotHostPlaceHost h[4];
    int to[8], given;

    /* The server full and one host bad: only the other host is good. */
    {
        int bots[3] = { 0, 1, 5 };
        keepTable(h, 4, 8, 8, slots, bots, 2.0);
        h[3].bad = true;
        h[2].bad = true;
        given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3,
                                     3, NULL, to);
        UT_ASSERT_MSG(given == 1 && to[0] == 1, "given %d to %d", given,
                      to[0]);
        UT_ASSERT(h[3].bots == 4 && h[1].bots == 1);
    }
    /* No good place at all: none. */
    {
        int bots[3] = { 4, 4, 5 };
        keepTable(h, 4, 8, 8, slots, bots, 2.0);
        h[3].bad = true;
        given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3,
                                     3, NULL, to);
        UT_ASSERT(given == 0 && h[3].bots == 5);
    }
    /* A good place that may not take a bot now (its free CPU check): not
     * a target. */
    {
        int bots[3] = { 0, 1, 5 };
        keepTable(h, 4, 8, 8, slots, bots, 2.0);
        h[3].bad = true;
        h[1].open = false;
        given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3,
                                     3, NULL, to);
        UT_ASSERT_MSG(given == 1 && to[0] == 2, "given %d to %d", given,
                      to[0]);
    }
    return 0;
}

int run_bot_host_place_keep_batch_off(void) {
    static const int slots[3] = { 4, 4, 8 };
    int bots[3] = { 0, 1, 5 };
    BotHostPlaceHost h[4];
    BotHostOverrunCfg cfg;
    BotHostOverrunState st;
    BotHostSpreadWhy why;
    int n, from = -1, to = -1;

    botHostOverrunCfgDefaults(&cfg);
    cfg.freeBatch = false;
    keepShortBy3(&st, &cfg);
    n = botHostOverrunFreeShort(&st, &cfg, 5, 5, NULL);
    UT_ASSERT(n == 3);
    /* Off: no batch, so the usual rule gives one bot. */
    UT_ASSERT(botHostOverrunBatchSize(&cfg, n, true) == 0);
    keepTable(h, 4, 8, 2, slots, bots, 2.0);
    h[3].bad = true;
    UT_ASSERT(keepStep(h, 4, false, &from, &to, &why));
    UT_ASSERT(from == 3 && to == 0 && why == BOT_HOST_SPREAD_BAD);
    /* On, but a bot left within the step time (stale numbers): none. */
    cfg.freeBatch = true;
    UT_ASSERT(botHostOverrunBatchSize(&cfg, n, false) == 0);
    /* On with no shortage: none. */
    UT_ASSERT(botHostOverrunBatchSize(&cfg, 0, true) == 0);
    /* A bad reading and no clean one: the numbers cannot count, 1. */
    {
        int k;
        botHostOverrunReset(&st);
        for (k = 0; k < 30; k++) keepFeedFree(&st, &cfg, 5, 5, 5, 100);
        UT_ASSERT(botHostOverrunFreeShort(&st, &cfg, 5, 5, NULL) == 1);
    }
    return 0;
}

/* ================================================================
 * 11 to 14. How many bots each target of a batch takes
 * ================================================================ */

/* A target with bots bots that has run 30 s clean at freePct free, each
 * bot using cost% of the CPU. With no bad reading and the lowest clean
 * reading at freePct, the check for the k-th bot after the first passes
 * while freePct - k cost - cost >= cost, so the room is
 * floor(freePct / cost) - 1. */
static void keepTargetFree(BotHostOverrunState *st,
                           const BotHostOverrunCfg *cfg, int bots,
                           uint8_t freePct, int cost) {
    int k;
    botHostOverrunReset(st);
    for (k = 0; k < 30; k++) {
        keepFeedFree(st, cfg, bots, 0, freePct, (uint8_t)(cost * bots));
    }
}

int run_bot_host_place_keep_batch_server_room(void) {
    static const int slots[3] = { 4, 4, 8 };
    int bots[3] = { 0, 1, 5 };
    BotHostPlaceHost h[4];
    BotHostOverrunCfg cfg;
    BotHostOverrunState srv;
    int room[4] = { 0, 1, 1, 0 };
    int to[8], given;

    botHostOverrunCfgDefaults(&cfg);
    /* The server with 1 bot at 80% free, 20% a bot: 80 / 20 - 1 = 3. */
    keepTargetFree(&srv, &cfg, 1, 80, 20);
    room[0] = botHostOverrunFreeRoom(&srv, &cfg, 1, 80, 4);
    UT_ASSERT_MSG(room[0] == 3, "room %d", room[0]);
    /* At most max. */
    UT_ASSERT(botHostOverrunFreeRoom(&srv, &cfg, 1, 80, 2) == 2);

    keepTable(h, 4, 8, 1, slots, bots, 2.0);
    h[3].bad = true;
    given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3, 4,
                                 room, to);
    UT_ASSERT_MSG(given == 4, "given %d", given);
    UT_ASSERT_MSG(to[0] == 0 && to[1] == 0 && to[2] == 0 && to[3] == 1,
                  "to %d %d %d %d", to[0], to[1], to[2], to[3]);
    UT_ASSERT(h[0].bots == 4 && h[1].bots == 1 && h[2].bots == 1 &&
              h[3].bots == 1);
    return 0;
}

int run_bot_host_place_keep_batch_no_reading(void) {
    static const int slots[3] = { 4, 4, 8 };
    int bots[3] = { 0, 1, 5 };
    BotHostPlaceHost h[4];
    BotHostOverrunCfg cfg;
    BotHostOverrunState st;
    int room[4], to[8], given, k;

    botHostOverrunCfgDefaults(&cfg);
    /* No reading at all, no bots: its first bot, 1. */
    botHostOverrunReset(&st);
    UT_ASSERT(botHostOverrunFreeRoom(&st, &cfg, 0, 80, 4) == 1);
    /* Bots but no free CPU data from the host: 1. */
    UT_ASSERT(botHostOverrunFreeRoom(&st, &cfg, 2, 0xFF, 4) == 1);
    /* Readings of 80% free with no bots, so no one-bot cost: 1. */
    for (k = 0; k < 30; k++) keepFeedFree(&st, &cfg, 0, 0, 80, 0);
    UT_ASSERT(!st.costKnown);
    UT_ASSERT(botHostOverrunFreeRoom(&st, &cfg, 0, 80, 4) == 1);

    /* In a batch: room 1 at each target, one bot each, as before. */
    room[0] = room[1] = room[2] = 1;
    room[3] = 0;
    keepTable(h, 4, 8, 2, slots, bots, 2.0);
    h[3].bad = true;
    given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3, 3,
                                 room, to);
    UT_ASSERT_MSG(given == 3 && to[0] == 0 && to[1] == 1 && to[2] == 2,
                  "given %d to %d %d %d", given, to[0], to[1], to[2]);
    /* A room of 0 (the check refuses now) on a place still open: 1. */
    room[0] = 0;
    keepTable(h, 4, 8, 2, slots, bots, 2.0);
    h[3].bad = true;
    given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3, 3,
                                 room, to);
    UT_ASSERT(given == 3 && to[0] == 0 && h[0].bots == 3);
    return 0;
}

int run_bot_host_place_keep_batch_slot_limit(void) {
    static const int slots[3] = { 4, 4, 8 };
    int bots[3] = { 0, 1, 5 };
    BotHostPlaceHost h[4];
    BotHostOverrunCfg cfg;
    BotHostOverrunState srv;
    int room[4] = { 0, 1, 1, 0 };
    int to[8], given;

    botHostOverrunCfgDefaults(&cfg);
    /* The server with 6 bots at 90% free, 5% a bot: room 17, at most 4. */
    keepTargetFree(&srv, &cfg, 6, 90, 5);
    room[0] = botHostOverrunFreeRoom(&srv, &cfg, 6, 90, 4);
    UT_ASSERT_MSG(room[0] == 4, "room %d", room[0]);

    /* Its 8 slots leave room for 2: the other 2 go to the hosts. */
    keepTable(h, 4, 8, 6, slots, bots, 2.0);
    h[3].bad = true;
    given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3, 4,
                                 room, to);
    UT_ASSERT_MSG(given == 4, "given %d", given);
    UT_ASSERT_MSG(to[0] == 0 && to[1] == 0 && to[2] == 1 && to[3] == 2,
                  "to %d %d %d %d", to[0], to[1], to[2], to[3]);
    UT_ASSERT(h[0].bots == 8 && h[1].bots == 1 && h[2].bots == 2);

    /* A Bot Host with 3 of 4 slots used and room 4 takes 1. */
    {
        static const int slots2[3] = { 4, 4, 8 };
        int bots2[3] = { 3, 4, 5 };
        int room2[4] = { 0, 4, 4, 0 };
        keepTable(h, 4, 8, 8, slots2, bots2, 2.0);
        h[3].bad = true;
        given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3,
                                     4, room2, to);
        UT_ASSERT_MSG(given == 1 && to[0] == 1, "given %d to %d", given,
                      to[0]);
        UT_ASSERT(h[0].bots == 8 && h[1].bots == 4);
    }
    return 0;
}

int run_bot_host_place_keep_batch_cpu_limit(void) {
    static const int slots[3] = { 8, 4, 8 };
    int bots[3] = { 1, 0, 5 };
    BotHostPlaceHost h[4];
    BotHostOverrunCfg cfg;
    BotHostOverrunState host;
    int room[4] = { 4, 0, 1, 0 };
    int to[8], given;

    botHostOverrunCfgDefaults(&cfg);
    /* Bot Host 1 with 1 bot at 60% free, 20% a bot: 60 / 20 - 1 = 2. */
    keepTargetFree(&host, &cfg, 1, 60, 20);
    room[1] = botHostOverrunFreeRoom(&host, &cfg, 1, 60, 4);
    UT_ASSERT_MSG(room[1] == 2, "room %d", room[1]);
    /* The room comes from its free CPU, not from the bots it has: 2 bots
     * at 60% free also give 2, and 40% free gives 1. */
    keepTargetFree(&host, &cfg, 2, 60, 20);
    UT_ASSERT(botHostOverrunFreeRoom(&host, &cfg, 2, 60, 4) == 2);
    keepTargetFree(&host, &cfg, 2, 40, 20);
    UT_ASSERT(botHostOverrunFreeRoom(&host, &cfg, 2, 40, 4) == 1);

    /* The server full (8 of 8): host 1 takes its 2, host 2 its 1, and the
     * server takes none past its slots. */
    keepTable(h, 4, 8, 8, slots, bots, 2.0);
    h[3].bad = true;
    given = botHostPlaceBadBatch(h, 4, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag, 3, 4,
                                 room, to);
    UT_ASSERT_MSG(given == 3, "given %d", given);
    UT_ASSERT_MSG(to[0] == 1 && to[1] == 1 && to[2] == 2,
                  "to %d %d %d", to[0], to[1], to[2]);
    UT_ASSERT(h[0].bots == 8 && h[1].bots == 3 && h[2].bots == 1 &&
              h[3].bots == 2);
    return 0;
}

/* ================================================================
 * 15 to 18. The server's own bot limit (-bothost-enforce-server-limit)
 * ================================================================ */

/* The seats of the held tables below (MAX_TANKS in the game). */
#define ENFORCE_SEATS 16

/* Bots that come back from a host one at a time to a server with serverBots
 * of serverSlots: how many run on the server and how many are held. */
static void enforceFallBack(bool enforce, int *serverBots, int serverSlots,
                            int back, int *held) {
    int k;
    *held = 0;
    for (k = 0; k < back; k++) {
        if (botHostServerFallBackHolds(enforce, *serverBots, serverSlots)) {
            (*held)++;
        } else {
            (*serverBots)++;
        }
    }
}

int run_bot_host_enforce_fall_back_holds(void) {
    static const int slots[1] = { 4 };
    int bots[1] = { 4 };
    BotHostPlaceHost h[2];
    int serverBots = 2, held = 0;

    /* The server runs 2 of its 3 slots; its Bot Host with 4 bots goes. */
    enforceFallBack(true, &serverBots, 3, 4, &held);
    UT_ASSERT_MSG(serverBots == 3 && held == 3, "server %d held %d",
                  serverBots, held);
    /* A full server holds the first one too. */
    serverBots = 3;
    enforceFallBack(true, &serverBots, 3, 2, &held);
    UT_ASSERT(serverBots == 3 && held == 2);
    /* A server with 0 slots runs none. */
    serverBots = 0;
    enforceFallBack(true, &serverBots, 0, 1, &held);
    UT_ASSERT(serverBots == 0 && held == 1);

    /* No placement puts a bot on the full server: with the host full too,
     * there is no place for a new bot and no place for a held one. */
    keepTable(h, 2, 3, 3, slots, bots, 2.0);
    h[0].capped = true;
    UT_ASSERT(botHostPlacePickLag(h, 2, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                  BOT_HOST_SMALL_BIAS_DEFAULT,
                                  &keepLag) == -1);
    UT_ASSERT(botHostPlaceHeldTarget(h, 2, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT,
                                     &keepLag) == -1);
    /* A host over its slots gives no bot to the full server either. */
    {
        int from = -1, to = -1;
        BotHostSpreadWhy why;
        bots[0] = 5;
        keepTable(h, 2, 3, 3, slots, bots, 2.0);
        h[0].capped = true;
        UT_ASSERT(!keepStep(h, 2, false, &from, &to, &why) || to != 0);
    }
    return 0;
}

int run_bot_host_enforce_host_resumes(void) {
    static const int slots[2] = { 2, 4 };
    int bots[2] = { 0, 4 };
    BotHostPlaceHost h[3];
    bool held[ENFORCE_SEATS];
    uint32_t since[ENFORCE_SEATS];
    int order[3], where[3], k, s;

    memset(held, 0, sizeof(held));
    memset(since, 0, sizeof(since));
    /* Seats 2, 5 and 6 held at ticks 300, 100 and 200. */
    held[2] = true;
    since[2] = 300;
    held[5] = true;
    since[5] = 100;
    held[6] = true;
    since[6] = 200;

    /* The server runs 2 of its 3 slots, host 1 (2 slots) has just
     * connected, host 2 is full. */
    keepTable(h, 3, 3, 2, slots, bots, 2.0);
    h[0].capped = true;
    for (k = 0; k < 3; k++) {
        int tgt = botHostPlaceHeldTarget(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                         BOT_HOST_SMALL_BIAS_DEFAULT,
                                         &keepLag);
        s = botHostHeldOldest(held, since, ENFORCE_SEATS, 400);
        UT_ASSERT_MSG(tgt >= 0 && s >= 0, "step %d tgt %d seat %d", k, tgt,
                      s);
        order[k] = s;
        where[k] = tgt;
        held[s] = false;
        h[tgt].bots++;
    }
    /* The longest held first: 5, 6, 2. The server first, then host 1. */
    UT_ASSERT_MSG(order[0] == 5 && order[1] == 6 && order[2] == 2,
                  "order %d %d %d", order[0], order[1], order[2]);
    UT_ASSERT_MSG(where[0] == 0 && where[1] == 1 && where[2] == 1,
                  "where %d %d %d", where[0], where[1], where[2]);
    UT_ASSERT(h[0].bots == 3 && h[1].bots == 2 && h[2].bots == 4);
    UT_ASSERT(botHostHeldOldest(held, since, ENFORCE_SEATS, 400) == -1);

    /* A bad host, or one that takes no bots, is no place for a held bot;
     * the server is, while it has a slot, even when it is bad. */
    bots[0] = 0;
    keepTable(h, 3, 3, 3, slots, bots, 2.0);
    h[0].capped = true;
    h[1].bad = true;
    UT_ASSERT(botHostPlaceHeldTarget(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT,
                                     &keepLag) == -1);
    h[1].bad = false;
    h[1].open = false;
    UT_ASSERT(botHostPlaceHeldTarget(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT,
                                     &keepLag) == -1);
    h[0].bots = 2;
    h[0].bad = true;
    UT_ASSERT(botHostPlaceHeldTarget(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT,
                                     &keepLag) == 0);
    /* Ties of the held time go to the lower seat; the count wraps. */
    memset(held, 0, sizeof(held));
    held[3] = held[1] = true;
    since[3] = since[1] = 0xFFFFFFF0u;
    UT_ASSERT(botHostHeldOldest(held, since, ENFORCE_SEATS, 16) == 1);
    since[3] = 0xFFFFFFE0u;
    UT_ASSERT(botHostHeldOldest(held, since, ENFORCE_SEATS, 16) == 3);
    return 0;
}

int run_bot_host_enforce_leave_resumes(void) {
    static const int slots[1] = { 2 };
    int bots[1] = { 2 };
    BotHostPlaceHost h[2];
    bool held[ENFORCE_SEATS];
    uint32_t since[ENFORCE_SEATS];
    int tgt, s;

    memset(held, 0, sizeof(held));
    memset(since, 0, sizeof(since));
    held[4] = true;
    since[4] = 500;
    held[7] = true;
    since[7] = 450;

    /* The server is full (3 of 3), its one host too: both stay held. */
    keepTable(h, 2, 3, 3, slots, bots, 2.0);
    h[0].capped = true;
    UT_ASSERT(botHostPlaceHeldTarget(h, 2, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT,
                                     &keepLag) == -1);

    /* A bot leaves the server: the one held longest (seat 7) runs there. */
    h[0].bots = 2;
    tgt = botHostPlaceHeldTarget(h, 2, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                 BOT_HOST_SMALL_BIAS_DEFAULT, &keepLag);
    s = botHostHeldOldest(held, since, ENFORCE_SEATS, 600);
    UT_ASSERT_MSG(tgt == 0 && s == 7, "tgt %d seat %d", tgt, s);
    held[s] = false;
    h[0].bots++;

    /* Only one: the server is full again and seat 4 stays held. */
    UT_ASSERT(botHostPlaceHeldTarget(h, 2, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                     BOT_HOST_SMALL_BIAS_DEFAULT,
                                     &keepLag) == -1);
    UT_ASSERT(botHostHeldOldest(held, since, ENFORCE_SEATS, 600) == 4);
    return 0;
}

int run_bot_host_enforce_off_keeps_fall_back(void) {
    static const int slots[1] = { 4 };
    int bots[1] = { 4 };
    BotHostPlaceHost h[2];
    int serverBots = 2, held = 0;

    /* All 4 bots of the host that went run on the server, 6 of 3 slots. */
    enforceFallBack(false, &serverBots, 3, 4, &held);
    UT_ASSERT_MSG(serverBots == 6 && held == 0, "server %d held %d",
                  serverBots, held);
    UT_ASSERT(!botHostServerFallBackHolds(false, 16, 0));

    /* Not capped: with every Bot Host full, the full server still takes a
     * new bot, as before. */
    keepTable(h, 2, 3, 3, slots, bots, 2.0);
    UT_ASSERT(botHostPlacePickLag(h, 2, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                  BOT_HOST_SMALL_BIAS_DEFAULT,
                                  &keepLag) == 0);
    /* And a host over its slots gives one to it. */
    {
        int from = -1, to = -1;
        BotHostSpreadWhy why;
        bots[0] = 5;
        keepTable(h, 2, 3, 3, slots, bots, 2.0);
        UT_ASSERT(keepStep(h, 2, false, &from, &to, &why));
        UT_ASSERT_MSG(from == 1 && to == 0, "from %d to %d", from, to);
    }
    return 0;
}
