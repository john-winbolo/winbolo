/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The overrun move of the bot placement (botHostOverrunProbe,
 * botHostOvrMoveRule, botHostOvrPickMove and botHostOverrunMayTake in
 * bot_host_link.c), the free CPU percent (machine_cpu_free.c),
 * CTRL_BOT_HOST_CPU_FREE and the log_BotHostData payload
 * (bothost_record.h).
 *
 * Each probe is one second of one host: its bots' new thinks and overruns.
 * The host's rate is the sum of its overruns over the sum of its thinks in
 * the window (30 probes by default). The tests feed probes of 100 thinks,
 * 0 overruns for a clean probe and 5 for a bad one (5% is over the 1%
 * default).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "bot_host_link.h"
#include "bothost_record.h"
#include "control_event.h"
#include "machine_cpu_free.h"
#include "transport_control_codec.h"
#include "test_harness.h"

/* One probe with the given thinks and overruns. */
static BotHostOverrunAction ovrFeedN(BotHostOverrunState *st,
                                     const BotHostOverrunCfg *cfg,
                                     uint32_t thinks, uint32_t overruns,
                                     int bots, int slots, uint8_t freePct,
                                     uint8_t botCpu,
                                     BotHostOverrunReport *rep) {
    BotHostOverrunProbe in;
    memset(&in, 0, sizeof(in));
    in.thinks = thinks;
    in.overruns = overruns;
    in.bots = bots;
    in.slots = slots;
    in.freePct = freePct;
    in.botCpu = botCpu;
    return botHostOverrunProbe(st, cfg, &in, rep);
}

/* One probe: bad (5%) or clean (0%), with the host's bots, slots, free CPU
 * and bot CPU. */
static BotHostOverrunAction ovrFeed(BotHostOverrunState *st,
                                    const BotHostOverrunCfg *cfg, bool bad,
                                    int bots, int slots, uint8_t freePct,
                                    uint8_t botCpu,
                                    BotHostOverrunReport *rep) {
    return ovrFeedN(st, cfg, 100, bad ? 5u : 0u, bots, slots, freePct, botCpu,
                    rep);
}

/* The values these tests were written for: a 1% target rate, a fixed
 * 30 s step (no scaled wait) and one bot a step for low free CPU. The
 * scaled wait and the batch have their own tests (test_bot_host_keep.c). */
static void ovrCfgOld(BotHostOverrunCfg *cfg) {
    botHostOverrunCfgDefaults(cfg);
    cfg->pct = 1.0;
    cfg->stepMinSecs = 30;
    cfg->freeBatch = false;
}

static void ovrStart(BotHostOverrunState *st, BotHostOverrunCfg *cfg) {
    ovrCfgOld(cfg);
    botHostOverrunReset(st);
}

/* A place for the move tests: a host with a known rate and bots, or (with
 * bots 0) a host with no rate. It may take a bot and has no lag. */
static BotHostOvrHost ovrHost(double ratePct, int bots) {
    BotHostOvrHost h;
    memset(&h, 0, sizeof(h));
    h.rateKnown = bots > 0;
    h.ratePct = bots > 0 ? ratePct : 0.0;
    h.bots = bots;
    h.mayTake = true;
    h.lagMs = 0.0;
    h.slots = 8;
    return h;
}

static BotHostOvrBot ovrBot(int seat, int moves, double staySecs) {
    BotHostOvrBot b;
    memset(&b, 0, sizeof(b));
    b.seat = seat;
    b.moves = moves;
    b.staySecs = staySecs;
    return b;
}

static bool ovrNear(double a, double b) {
    return a > b - 0.0001 && a < b + 0.0001;
}

/* The window rate: 30 probes at 1.5% (1 and 2 overruns in turn, of 100
 * thinks). The host is over the target only at the 30th probe, when the
 * window is full. The rate is a sum: one probe of 100% in 30 s of 1000
 * thinks each is far below the target. A probe with no bots makes the
 * window not known. */
int run_bot_host_overrun_window_rate(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a = BOT_HOST_OVR_NONE;
    double pct;
    int k;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) {
        a = ovrFeedN(&st, &cfg, 100, (k % 2) ? 1u : 2u, 2, 8, 40, 20, &rep);
        if (k < 30) {
            UT_ASSERT_MSG(a == BOT_HOST_OVR_NONE && !rep.windowKnown,
                          "probe %d: action %d", k, (int)a);
            UT_ASSERT(!botHostOverrunRate(&st, &cfg, &pct));
        }
    }
    UT_ASSERT_MSG(a == BOT_HOST_OVR_OVER, "probe 30: action %d", (int)a);
    UT_ASSERT(rep.windowKnown && ovrNear(rep.windowPct, 1.5));
    UT_ASSERT(botHostOverrunRate(&st, &cfg, &pct) && ovrNear(pct, 1.5));
    UT_ASSERT(rep.probes == 30);

    /* A short spike in a busy window: 29 s of 1000 clean thinks and 1 s of
     * 10 thinks, all overruns: 10 / 29010 = 0.03%. */
    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) {
        a = ovrFeedN(&st, &cfg, k == 15 ? 10u : 1000u, k == 15 ? 10u : 0u, 4,
                     8, 40, 20, &rep);
        UT_ASSERT(a == BOT_HOST_OVR_NONE);
    }
    UT_ASSERT(rep.windowKnown && rep.windowPct < 0.05);

    /* A second with no bots in the window: no rate. */
    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) {
        a = ovrFeed(&st, &cfg, true, k == 10 ? 0 : 2, 8, 40, 20, &rep);
        UT_ASSERT(a == BOT_HOST_OVR_NONE);
    }
    UT_ASSERT(!rep.windowKnown && !botHostOverrunRate(&st, &cfg, &pct));
    for (k = 1; k <= 20; k++) {
        a = ovrFeed(&st, &cfg, true, 2, 8, 40, 20, &rep);
    }
    UT_ASSERT_MSG(a == BOT_HOST_OVR_OVER, "after the no-bot second leaves: %d",
                  (int)a);
    return 0;
}

/* After a move the probes start again and the limit is bots - 1: the next
 * move needs a new full window (30 s). After a held move (-bothost-nomove)
 * the window stays, but the next decision waits 30 s. */
int run_bot_host_overrun_step_spacing(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a;
    int k, first = 0, second = 0, bots = 4;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 120 && second == 0; k++) {
        a = ovrFeed(&st, &cfg, true, bots, 8, 40, 20, &rep);
        if (a != BOT_HOST_OVR_OVER) continue;
        if (first == 0) {
            first = k;
            UT_ASSERT(botHostOverrunMoved(&st, bots) == 3);
            bots--;
            UT_ASSERT(st.probeCount == 0);
        } else {
            second = k;
        }
    }
    UT_ASSERT_MSG(first == 30, "first move at %d", first);
    UT_ASSERT_MSG(second == first + 30, "second move at %d", second);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);

    /* Held: the next decision is 30 s later, with the window kept. */
    ovrStart(&st, &cfg);
    first = second = 0;
    for (k = 1; k <= 120 && second == 0; k++) {
        a = ovrFeed(&st, &cfg, true, 4, 8, 40, 20, &rep);
        if (a != BOT_HOST_OVR_OVER) continue;
        if (first == 0) {
            first = k;
            botHostOverrunHeld(&st);
        } else {
            second = k;
        }
    }
    UT_ASSERT_MSG(first == 30 && second == 60, "held: %d then %d", first,
                  second);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8);
    return 0;
}

/* A steady 1.5% on a host with 2 bots and a host at 0.2%: one bot moves
 * to the 0.2% host (normal rule), and the source's limit becomes 1. */
int run_bot_host_overrun_steady_moves_one(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a = BOT_HOST_OVR_NONE;
    BotHostOvrHost h[3];
    BotHostOvrBot b[2];
    BotHostOvrMove mv;
    int k;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) {
        a = ovrFeedN(&st, &cfg, 200, 3, 2, 8, 40, 20, &rep);
    }
    UT_ASSERT(a == BOT_HOST_OVR_OVER && ovrNear(rep.windowPct, 1.5));

    h[0] = ovrHost(0.0, 0);
    h[0].mayTake = false;            /* the server: no free CPU room */
    h[1] = ovrHost(rep.windowPct, 2);
    h[2] = ovrHost(0.2, 3);
    b[0] = ovrBot(4, 0, 30.0);
    b[1] = ovrBot(5, 0, 30.0);
    UT_ASSERT(botHostOvrPickMove(&cfg, h, 3, 1, b, 2, NULL, &mv));
    UT_ASSERT_MSG(mv.to == 2 && mv.seat == 4, "to %d seat %d", mv.to,
                  mv.seat);
    UT_ASSERT(mv.rule == BOT_HOST_OVR_RULE_NORMAL);
    UT_ASSERT(ovrNear(mv.srcPct, 1.5) && mv.dstKnown && ovrNear(mv.dstPct, 0.2));
    UT_ASSERT(mv.moves == 0 && ovrNear(mv.needStaySecs, 0.0));
    UT_ASSERT(botHostOverrunMoved(&st, 2) == 1);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 1);
    return 0;
}

/* Every host at 2%: no move, "no better host". */
int run_bot_host_overrun_all_same_no_move(void) {
    BotHostOverrunCfg cfg;
    BotHostOvrHost h[3];
    BotHostOvrBot b[2];
    BotHostOvrMove mv;

    ovrCfgOld(&cfg);
    h[0] = ovrHost(2.0, 2);
    h[1] = ovrHost(2.0, 2);
    h[2] = ovrHost(2.0, 2);
    b[0] = ovrBot(1, 0, 100.0);
    b[1] = ovrBot(2, 0, 100.0);
    UT_ASSERT(!botHostOvrPickMove(&cfg, h, 3, 0, b, 2, NULL, &mv));
    UT_ASSERT(mv.seat == -1 && mv.to == -1);
    UT_ASSERT_MSG(strcmp(mv.why, "no better host") == 0, "%s", mv.why);
    /* No bot may move: its own reason. */
    UT_ASSERT(!botHostOvrPickMove(&cfg, h, 3, 0, b, 0, NULL, &mv));
    UT_ASSERT(strcmp(mv.why, "no bot may move now") == 0);
    return 0;
}

/* A 3% source and a 1.6% target: no move, the target is not below the
 * 1% target rate (rule 4b). */
int run_bot_host_overrun_target_not_below(void) {
    BotHostOverrunCfg cfg;
    BotHostOvrHost h[2];
    BotHostOvrBot b[1];
    BotHostOvrMove mv;
    const char *why = NULL;

    ovrCfgOld(&cfg);
    h[0] = ovrHost(3.0, 2);
    h[1] = ovrHost(1.6, 2);
    b[0] = ovrBot(1, 0, 100.0);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 3.0, &h[1], 0, 100.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    UT_ASSERT_MSG(strcmp(why, "the target is not below the target rate") == 0,
                  "%s", why);
    UT_ASSERT(!botHostOvrPickMove(&cfg, h, 2, 0, b, 1, NULL, &mv));
    /* The source not over the target rate: rule 4a. */
    UT_ASSERT(botHostOvrMoveRule(&cfg, 1.0, &h[1], 0, 100.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    UT_ASSERT(strcmp(why, "the source is not over the target rate") == 0);
    return 0;
}

/* m = 1 needs a target at most a quarter of the source rate (0.5^2): with
 * a 2% source, 0.6% is refused and 0.5% is taken; for m = 0, 0.6% is
 * taken (at most half). */
int run_bot_host_overrun_gain_per_move(void) {
    BotHostOverrunCfg cfg;
    BotHostOvrHost t;
    const char *why = NULL;

    ovrCfgOld(&cfg);
    t = ovrHost(0.6, 2);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 2.0, &t, 1, 60.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    UT_ASSERT_MSG(strcmp(why, "the target is not low enough for this bot's "
                              "moves") == 0, "%s", why);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 2.0, &t, 0, 0.0, &why) ==
              BOT_HOST_OVR_RULE_NORMAL);
    t = ovrHost(0.5, 2);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 2.0, &t, 1, 60.0, &why) ==
              BOT_HOST_OVR_RULE_NORMAL);
    /* m = 0 at 0.9% against 1.5%: above half, refused. */
    t = ovrHost(0.9, 2);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 1.5, &t, 0, 0.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    /* A host with no bots: a trial, with no rate rule. */
    t = ovrHost(0.0, 0);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 1.5, &t, 1, 60.0, &why) ==
              BOT_HOST_OVR_RULE_TRIAL);
    /* A host with bots but no rate yet: refused. */
    t = ovrHost(0.2, 2);
    t.rateKnown = false;
    UT_ASSERT(botHostOvrMoveRule(&cfg, 1.5, &t, 0, 0.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    /* A target that may not take a bot (free CPU or slots): rule 4e. */
    t = ovrHost(0.2, 2);
    t.mayTake = false;
    UT_ASSERT(botHostOvrMoveRule(&cfg, 1.5, &t, 0, 0.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    UT_ASSERT(strcmp(why, "the target may not take a bot") == 0);
    return 0;
}

/* The stay times: none for m = 0, then 30, 60 and 120 s. */
int run_bot_host_overrun_stay_times(void) {
    BotHostOverrunCfg cfg;
    BotHostOvrHost t;
    const char *why = NULL;

    ovrCfgOld(&cfg);
    UT_ASSERT(ovrNear(botHostOvrStayNeed(&cfg, 0), 0.0));
    UT_ASSERT(ovrNear(botHostOvrStayNeed(&cfg, 1), 30.0));
    UT_ASSERT(ovrNear(botHostOvrStayNeed(&cfg, 2), 60.0));
    UT_ASSERT(ovrNear(botHostOvrStayNeed(&cfg, 3), 120.0));
    /* m = 2 with a 4% source and a 0.4% target (below 4 x 0.125): 59 s is
     * too short, 60 s moves. */
    t = ovrHost(0.4, 2);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 4.0, &t, 2, 59.0, &why) ==
              BOT_HOST_OVR_RULE_NONE);
    UT_ASSERT(strcmp(why, "the bot has not stayed long enough") == 0);
    UT_ASSERT(botHostOvrMoveRule(&cfg, 4.0, &t, 2, 60.0, &why) ==
              BOT_HOST_OVR_RULE_NORMAL);
    return 0;
}

/* The emergency rule: a 6% source and a 2.9% target (at most half) moves,
 * with no gain or stay rule, even for a bot with 3 moves that just moved.
 * Every host at 6%: no move. */
int run_bot_host_overrun_emergency(void) {
    BotHostOverrunCfg cfg;
    BotHostOvrHost h[3];
    BotHostOvrBot b[1];
    BotHostOvrMove mv;

    ovrCfgOld(&cfg);
    h[0] = ovrHost(6.0, 3);
    h[1] = ovrHost(2.9, 2);
    h[2] = ovrHost(6.0, 2);
    b[0] = ovrBot(7, 3, 0.0);
    UT_ASSERT(botHostOvrPickMove(&cfg, h, 3, 0, b, 1, NULL, &mv));
    UT_ASSERT(mv.to == 1 && mv.seat == 7);
    UT_ASSERT(mv.rule == BOT_HOST_OVR_RULE_EMERGENCY);
    UT_ASSERT(mv.moves == 3 && ovrNear(mv.needStaySecs, 0.0));
    /* 3.1% is above half: refused. */
    h[1] = ovrHost(3.1, 2);
    UT_ASSERT(!botHostOvrPickMove(&cfg, h, 3, 0, b, 1, NULL, &mv));
    /* Every host at 6%. */
    h[1] = ovrHost(6.0, 2);
    UT_ASSERT(!botHostOvrPickMove(&cfg, h, 3, 0, b, 1, NULL, &mv));
    UT_ASSERT(strcmp(mv.why, "no better host") == 0);
    /* 4.9% is not an emergency: the normal rules refuse 2.4% (not below
     * 1%). */
    h[0] = ovrHost(4.9, 3);
    h[1] = ovrHost(2.4, 2);
    b[0] = ovrBot(7, 0, 100.0);
    UT_ASSERT(!botHostOvrPickMove(&cfg, h, 3, 0, b, 1, NULL, &mv));
    return 0;
}

/* The order of the targets: the lowest rate first (lag only breaks a tie
 * within 0.1%), a host with no bots (a trial) after every host with a
 * rate. Bots: the fewest moves first. */
int run_bot_host_overrun_target_order(void) {
    BotHostOverrunCfg cfg;
    BotHostPlaceLag lag;
    BotHostOvrHost h[4];
    BotHostOvrBot b[2];
    BotHostOvrMove mv;

    ovrCfgOld(&cfg);
    memset(&lag, 0, sizeof(lag));
    lag.refMs = 100.0;
    lag.maxMs = 250.0;
    h[0] = ovrHost(2.0, 3);
    h[1] = ovrHost(0.0, 0);           /* a trial */
    h[2] = ovrHost(0.5, 2);
    h[2].lagMs = 400.0;               /* over the lag ceiling: still taken */
    h[3] = ovrHost(0.8, 2);
    b[0] = ovrBot(1, 1, 300.0);
    b[1] = ovrBot(2, 0, 10.0);
    UT_ASSERT(botHostOvrPickMove(&cfg, h, 4, 0, b, 2, &lag, &mv));
    UT_ASSERT_MSG(mv.to == 2 && mv.seat == 2, "to %d seat %d", mv.to,
                  mv.seat);
    /* A tie within 0.1%: the lower lag. */
    h[3] = ovrHost(0.55, 2);
    UT_ASSERT(botHostOvrPickMove(&cfg, h, 4, 0, b, 2, &lag, &mv));
    UT_ASSERT_MSG(mv.to == 3, "to %d", mv.to);
    /* Only the trial left: it takes the bot. */
    h[2].mayTake = false;
    h[3].mayTake = false;
    UT_ASSERT(botHostOvrPickMove(&cfg, h, 4, 0, b, 2, &lag, &mv));
    UT_ASSERT(mv.to == 1 && mv.rule == BOT_HOST_OVR_RULE_TRIAL &&
              !mv.dstKnown);
    return 0;
}

/* m per bot: a voluntary move adds 1 and starts the stay again; a forced
 * fall-back adds nothing and keeps the stay; the lobby clears it; a seat
 * that is no longer a bot clears its count. */
int run_bot_host_overrun_forced_no_m(void) {
    BotHostOvrMoves mv;

    botHostOvrMovesReset(&mv);
    botHostOvrMovesSeat(&mv, 3, true, 1000);
    UT_ASSERT(botHostOvrMovesCount(&mv, 3) == 0);
    UT_ASSERT(ovrNear(botHostOvrMovesStay(&mv, 3, 2500, 50), 30.0));
    botHostOvrMovesNote(&mv, 3, BOT_HOST_MOVE_FORCED, 2500);
    UT_ASSERT(botHostOvrMovesCount(&mv, 3) == 0);
    UT_ASSERT(ovrNear(botHostOvrMovesStay(&mv, 3, 3000, 50), 40.0));
    botHostOvrMovesNote(&mv, 3, BOT_HOST_MOVE_VOLUNTARY, 3000);
    UT_ASSERT(botHostOvrMovesCount(&mv, 3) == 1);
    UT_ASSERT(ovrNear(botHostOvrMovesStay(&mv, 3, 4500, 50), 30.0));
    botHostOvrMovesNote(&mv, 3, BOT_HOST_MOVE_FORCED, 4500);
    UT_ASSERT(botHostOvrMovesCount(&mv, 3) == 1);
    UT_ASSERT(ovrNear(botHostOvrMovesStay(&mv, 3, 4500, 50), 30.0));
    botHostOvrMovesSeat(&mv, 3, false, 5000);
    UT_ASSERT(botHostOvrMovesCount(&mv, 3) == 0);
    botHostOvrMovesNote(&mv, 3, BOT_HOST_MOVE_VOLUNTARY, 6000);
    botHostOvrMovesReset(&mv);
    UT_ASSERT(botHostOvrMovesCount(&mv, 3) == 0);
    return 0;
}

/* -bothost-nomove: the overrun move becomes "would move", the source is
 * held (its limit stays, the next decision waits 30 s), and m does not
 * change. Without the flag the move goes ahead. */
int run_bot_host_overrun_nomove_blocks(void) {
    BotHostNoMove nm, off;
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOvrMoves moves;
    BotHostOverrunAction a = BOT_HOST_OVR_NONE;
    int k;

    botHostNoMoveInit(&nm, true);
    botHostNoMoveInit(&off, false);
    botHostNoMoveNewGame(&nm);
    botHostOvrMovesReset(&moves);
    botHostOvrMovesSeat(&moves, 2, true, 0);
    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) a = ovrFeedN(&st, &cfg, 200, 3, 2, 8, 40, 20, &rep);
    UT_ASSERT(a == BOT_HOST_OVR_OVER);
    UT_ASSERT(botHostNoMoveOverrunDecision(&nm, true) ==
              BOTHOST_DECISION_WOULD_MOVE);
    /* What the server does for a held move. */
    botHostOverrunHeld(&st);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8 && !st.cut);
    UT_ASSERT(botHostOvrMovesCount(&moves, 2) == 0);
    for (k = 1; k < 30; k++) {
        a = ovrFeedN(&st, &cfg, 200, 3, 2, 8, 40, 20, &rep);
        UT_ASSERT(a == BOT_HOST_OVR_NONE);
    }
    UT_ASSERT(ovrFeedN(&st, &cfg, 200, 3, 2, 8, 40, 20, &rep) ==
              BOT_HOST_OVR_OVER);
    UT_ASSERT(botHostNoMoveOverrunDecision(&off, true) ==
              BOTHOST_DECISION_MOVE);
    return 0;
}

/* The server stays the last fall-back:
 *   - a host over the target with no bot that may move keeps its limit;
 *   - a Bot Host's bot finds no other place with room, so the caller
 *     sends it to the server;
 *   - a lowered server limit moves a server bot only to a host with room. */
int run_bot_host_overrun_server_fallback(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOvrHost oh[2];
    BotHostOvrMove mv;
    BotHostPlaceHost h[3];
    BotHostPlaceLag lag;
    int k, from = -1, to = -1;
    BotHostOverrunAction a = BOT_HOST_OVR_NONE;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) {
        a = ovrFeed(&st, &cfg, true, 4, 4, 30, 40, &rep);
    }
    UT_ASSERT_MSG(a == BOT_HOST_OVR_OVER, "action %d", (int)a);
    oh[0] = ovrHost(0.2, 1);
    oh[1] = ovrHost(rep.windowPct, 4);
    UT_ASSERT(!botHostOvrPickMove(&cfg, oh, 2, 1, NULL, 0, NULL, &mv));
    UT_ASSERT(strcmp(mv.why, "no bot may move now") == 0);
    UT_ASSERT(botHostOverrunLimit(&st, 4) == 4);
    UT_ASSERT(!st.cut);

    memset(&lag, 0, sizeof(lag));
    memset(h, 0, sizeof(h));
    /* The server (0) takes no new bot (its free CPU check), host 2 is
     * full: no place for host 1's bot, so it falls back to the server. */
    h[0].slots = 4; h[0].bots = 4; h[0].open = false; h[0].unlimited = true;
    h[1].slots = 3; h[1].bots = 4; h[1].open = true; h[1].movable = true;
    h[2].slots = 2; h[2].bots = 2; h[2].open = true;
    UT_ASSERT(botHostPlacePickOther(h, 3, 2, 0.25, &lag, 1) == -1);
    /* The server open: it takes the bot (it has no limit). */
    h[0].open = true;
    UT_ASSERT(botHostPlacePickOther(h, 3, 2, 0.25, &lag, 1) == 0);

    /* The server's limit lowered to 2 with 4 bots: a bot goes to host 2
     * when it has room, and stays when it has none. */
    memset(h, 0, sizeof(h));
    h[0].slots = 2; h[0].bots = 4; h[0].open = true; h[0].movable = true;
    h[1].slots = 4; h[1].bots = 4; h[1].open = true;
    h[2].slots = 4; h[2].bots = 1; h[2].open = true;
    UT_ASSERT(botHostPlaceRebalanceLag(h, 3, 2, 0.25, &lag, &from, &to));
    UT_ASSERT_MSG(from == 0 && to == 2, "from %d to %d", from, to);
    h[2].bots = 4;
    UT_ASSERT(!botHostPlaceRebalanceLag(h, 3, 2, 0.25, &lag, &from, &to));
    /* A host whose limit went to 0 with a bot on it gives it first. */
    memset(h, 0, sizeof(h));
    h[0].slots = 8; h[0].bots = 1; h[0].open = true; h[0].unlimited = true;
    h[1].slots = 0; h[1].bots = 1; h[1].open = true; h[1].movable = true;
    UT_ASSERT(botHostPlaceRebalanceLag(h, 2, 2, 0.25, &lag, &from, &to));
    UT_ASSERT_MSG(from == 1 && to == 0, "from %d to %d", from, to);
    return 0;
}

/* A host cut by overruns: phase 1 clean with free CPU c1, phase 2 bad with
 * free CPU b until the move, phase 3 clean with free CPU r and one bot
 * fewer. Each bot costs 5% (bot CPU 20% over 4 bots, 15% over 3). Returns
 * the first raise or refusal and its report. */
static BotHostOverrunAction ovrCutThenRecover(BotHostOverrunState *st,
                                              BotHostOverrunCfg *cfg,
                                              uint8_t c1, uint8_t b,
                                              uint8_t r,
                                              BotHostOverrunReport *out) {
    BotHostOverrunReport rep;
    int k;
    bool cut = false;
    ovrStart(st, cfg);
    for (k = 0; k < 30; k++) ovrFeed(st, cfg, false, 4, 8, c1, 20, &rep);
    for (k = 0; k < 60 && !cut; k++) {
        cut = ovrFeed(st, cfg, true, 4, 8, b, 20, &rep) == BOT_HOST_OVR_OVER;
    }
    if (!cut) return BOT_HOST_OVR_NONE;
    (void)botHostOverrunMoved(st, 4);
    for (k = 0; k < 120; k++) {
        BotHostOverrunAction a = ovrFeed(st, cfg, false, 3, 8, r, 15, &rep);
        if (a == BOT_HOST_OVR_RAISE || a == BOT_HOST_OVR_REFUSE) {
            *out = rep;
            return a;
        }
    }
    return BOT_HOST_OVR_NONE;
}

/* Clean at 40% free, bad at 10% free, then 50% free: predicted 50 - 5 = 45;
 * a clean reading had 40% (at or below 45) and the bad ones had 10% (below
 * 45): the limit goes up by one, 30 s after the move. */
int run_bot_host_overrun_raise_clean_evidence(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a = ovrCutThenRecover(&st, &cfg, 40, 10, 50, &rep);
    UT_ASSERT_MSG(a == BOT_HOST_OVR_RAISE, "action %d (%s)", (int)a, rep.why);
    UT_ASSERT(rep.limitBefore == 3 && rep.newLimit == 4);
    UT_ASSERT(rep.costKnown && rep.oneBotCost > 4.99 && rep.oneBotCost < 5.01);
    UT_ASSERT(rep.predicted > 44.9 && rep.predicted < 45.1);
    UT_ASSERT(rep.lowestClean == 40 && rep.highestBad == 10);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 4);
    return 0;
}

/* Clean at 60% free, bad at 10%, then 50% free: predicted 45, but the
 * clean readings all had more free CPU (60 and 50): no raise. */
int run_bot_host_overrun_no_raise_clean_too_free(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a = ovrCutThenRecover(&st, &cfg, 60, 10, 50, &rep);
    UT_ASSERT_MSG(a == BOT_HOST_OVR_REFUSE, "action %d", (int)a);
    UT_ASSERT(rep.lowestClean == 50);
    UT_ASSERT(strcmp(rep.why, "no clean reading with that little free CPU") == 0);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    return 0;
}

/* Clean at 40% free, bad at 50% free (overruns with free CPU), then 50%:
 * predicted 45 is at or below a bad reading's 50: no raise. */
int run_bot_host_overrun_no_raise_bad_reading(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a = ovrCutThenRecover(&st, &cfg, 40, 50, 50, &rep);
    UT_ASSERT_MSG(a == BOT_HOST_OVR_REFUSE, "action %d", (int)a);
    UT_ASSERT(rep.highestBad == 50);
    UT_ASSERT(strcmp(rep.why, "a bad reading had as much free CPU") == 0);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    return 0;
}

/* No free CPU data (an older Bot Host): the limit stays lowered for the
 * rest of the game, and a refusal comes at most each 30 s. */
int run_bot_host_overrun_no_raise_no_free_data(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int k, refusals = 0;
    BotHostOverrunAction a =
        ovrCutThenRecover(&st, &cfg, BOT_HOST_FREE_UNKNOWN,
                          BOT_HOST_FREE_UNKNOWN, BOT_HOST_FREE_UNKNOWN, &rep);
    UT_ASSERT_MSG(a == BOT_HOST_OVR_REFUSE, "action %d", (int)a);
    UT_ASSERT(!rep.freeKnown);
    UT_ASSERT(strcmp(rep.why, "no free CPU data") == 0);
    for (k = 0; k < 300; k++) {
        a = ovrFeed(&st, &cfg, false, 3, 8, BOT_HOST_FREE_UNKNOWN, 15, &rep);
        UT_ASSERT(a != BOT_HOST_OVR_RAISE);
        if (a == BOT_HOST_OVR_REFUSE) refusals++;
    }
    UT_ASSERT_MSG(refusals == 10, "%d refusals in 300 s", refusals);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    return 0;
}

/* A new game clears the limit and the probes but keeps the free CPU
 * history and the one-bot cost; a reset (a rejoin) clears them too. The
 * check off (P = 0) clears the limit at the next probe. */
int run_bot_host_overrun_new_game_reset(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int k, readings;
    bool over = false;

    ovrStart(&st, &cfg);
    for (k = 0; k < 60 && !over; k++) {
        over = ovrFeed(&st, &cfg, true, 4, 8, 30, 20, &rep) ==
               BOT_HOST_OVR_OVER;
    }
    UT_ASSERT(over);
    (void)botHostOverrunMoved(&st, 4);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    for (k = 0; k < 10; k++) ovrFeed(&st, &cfg, true, 3, 8, 30, 15, &rep);
    readings = st.readingCount;
    UT_ASSERT(readings > 0 && st.costKnown);

    botHostOverrunNewGame(&st);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8);
    UT_ASSERT(!st.cut && st.probeCount == 0);
    UT_ASSERT(st.readingCount == readings && st.costKnown);
    /* The first probe of the new game sees an empty window. */
    ovrFeed(&st, &cfg, false, 3, 8, 30, 15, &rep);
    UT_ASSERT(rep.probes == 1);

    botHostOverrunReset(&st);
    UT_ASSERT(st.readingCount == 0 && !st.costKnown && st.limit == -1);

    /* The check off. */
    ovrStart(&st, &cfg);
    for (k = 0; k < 30; k++) ovrFeed(&st, &cfg, true, 4, 8, 30, 20, &rep);
    (void)botHostOverrunMoved(&st, 4);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    cfg.pct = 0.0;
    UT_ASSERT(ovrFeed(&st, &cfg, true, 3, 8, 30, 15, &rep) ==
              BOT_HOST_OVR_NONE);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8);
    return 0;
}

/* The free CPU check for each new bot on a host (part A). */
int run_bot_host_overrun_may_take(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int k;

    ovrStart(&st, &cfg);
    /* The first bot: always. */
    UT_ASSERT(botHostOverrunMayTake(&st, &cfg, 0, 60, &rep));
    /* No free CPU data at all: the slots only. */
    UT_ASSERT(botHostOverrunMayTake(&st, &cfg, 3, BOT_HOST_FREE_UNKNOWN, &rep));
    /* Free CPU known but no reading with the bot yet. */
    UT_ASSERT(!botHostOverrunMayTake(&st, &cfg, 1, 60, &rep));
    UT_ASSERT(strcmp(rep.why, "no reading yet") == 0);

    /* One bot ran for 5 s at 60% free and cost 10%: predicted 50 >= 10. */
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 1, 8, 60, 10, &rep);
    UT_ASSERT(st.readingCount == 1);
    UT_ASSERT(botHostOverrunMayTake(&st, &cfg, 1, 60, &rep));
    UT_ASSERT(strcmp(rep.why, "room for one more bot") == 0);
    /* The second bot is placed (2 bots) but no reading counts it yet. */
    UT_ASSERT(!botHostOverrunMayTake(&st, &cfg, 2, 60, &rep));
    UT_ASSERT(strcmp(rep.why, "no reading counts its newest bot yet") == 0);
    /* A reading with 2 bots at 30% free, 80% bot CPU (40% a bot): predicted
     * free = (60 + 30) / 2 - 40 = 5 < 40, and no clean reading had 5% or
     * less: no third bot. */
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 2, 8, 30, 80, &rep);
    UT_ASSERT(!botHostOverrunMayTake(&st, &cfg, 2, 30, &rep));
    UT_ASSERT_MSG(strcmp(rep.why, "too little free CPU") == 0, "%s", rep.why);

    /* With a bad reading: the raise rule. Clean at 30% free with 2 bots
     * (cost 10% a bot), bad at 20%; now 50% free: predicted 40, clean 30
     * <= 40, bad 20 < 40: yes. */
    ovrStart(&st, &cfg);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 2, 8, 30, 20, &rep);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, true, 2, 8, 20, 20, &rep);
    for (k = 0; k < 30; k++) ovrFeed(&st, &cfg, false, 2, 8, 50, 20, &rep);
    UT_ASSERT_MSG(botHostOverrunMayTake(&st, &cfg, 2, 50, &rep), "%s", rep.why);
    UT_ASSERT(rep.highestBad == 20 && rep.lowestClean == 30);
    /* The same with the bad reading at 45% free: predicted 40 <= 45. */
    ovrStart(&st, &cfg);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 2, 8, 30, 20, &rep);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, true, 2, 8, 45, 20, &rep);
    for (k = 0; k < 30; k++) ovrFeed(&st, &cfg, false, 2, 8, 50, 20, &rep);
    UT_ASSERT(!botHostOverrunMayTake(&st, &cfg, 2, 50, &rep));
    UT_ASSERT(strcmp(rep.why, "a bad reading had as much free CPU") == 0);

    /* The check off: always. */
    cfg.pct = 0.0;
    UT_ASSERT(botHostOverrunMayTake(&st, &cfg, 2, 50, &rep));
    return 0;
}

/* The free CPU percent from two counter readings, and the one-byte
 * CTRL_BOT_HOST_CPU_FREE body. */
int run_bot_host_cpu_free_codec(void) {
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_CPU_FREE);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_CPU_FREE);
    ControlEvent evt, back;
    uint8_t buf[16];
    MachineCpuFree m;
    int n;

    UT_ASSERT(machineCpuFreePercent(100, 1000, 350, 2000) == 25);
    UT_ASSERT(machineCpuFreePercent(0, 0, 1000, 1000) == 100);
    UT_ASSERT(machineCpuFreePercent(0, 1000, 0, 1000) == MACHINE_CPU_FREE_UNKNOWN);
    UT_ASSERT(machineCpuFreePercent(500, 1000, 400, 2000) == MACHINE_CPU_FREE_UNKNOWN);
    /* This machine: the first sample is unknown; a later one is 0..100 on
     * a platform that has the counters, else unknown. */
    machineCpuFreeInit(&m);
    UT_ASSERT(machineCpuFreeSample(&m) == MACHINE_CPU_FREE_UNKNOWN);
    {
        uint8_t v = machineCpuFreeSample(&m);
        UT_ASSERT(v <= 100 || v == MACHINE_CPU_FREE_UNKNOWN);
    }

    UT_ASSERT(enc != NULL && dec != NULL);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_CPU_FREE;
    evt.u.botHostCpuFree.free = 37;
    {
        size_t outLen = 0;
        UT_ASSERT(enc(&evt, NULL, buf, sizeof(buf), &outLen) == ENCODE_OK);
        n = (int)outLen;
    }
    UT_ASSERT_MSG(n == 1 && buf[0] == 37, "encoded %d bytes", n);
    memset(&back, 0, sizeof(back));
    UT_ASSERT(dec(buf, (uint16_t)n, &back));
    UT_ASSERT(back.type == CTRL_BOT_HOST_CPU_FREE && back.u.botHostCpuFree.free == 37);
    /* Above 100 reads as unknown; a short body is refused; a longer one
     * (a later version) keeps the first byte. */
    buf[0] = 150;
    UT_ASSERT(dec(buf, 1, &back) && back.u.botHostCpuFree.free == 0xFF);
    UT_ASSERT(!dec(buf, 0, &back));
    buf[0] = 12; buf[1] = 99;
    UT_ASSERT(dec(buf, 2, &back) && back.u.botHostCpuFree.free == 12);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_BOT_HOST_CPU_FREE),
                     "CTRL_BOT_HOST_CPU_FREE") == 0);
    return 0;
}

/* The log_BotHostData payload: each kind keeps its fields through the
 * bytes; a short payload or an unknown kind is refused; bytes after the
 * fields (a later version) are skipped. */
int run_bothost_record_roundtrip(void) {
    BotHostRecord r, b;
    uint8_t buf[BOTHOST_RECORD_MAX_BYTES + 8];
    int n;

    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_HOST;
    r.host = 2;
    r.player = 5;
    strcpy(r.name, "Andrew's laptop");
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == 3 + 1 + 1 + (int)strlen(r.name));
    UT_ASSERT(buf[0] == BOTHOST_RECORD_VERSION && buf[1] == BOTHOST_RECORD_HOST);
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.host == 2 && b.player == 5 && strcmp(b.name, r.name) == 0);
    UT_ASSERT(!botHostRecordDecode(buf, n - 1, &b));

    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_PROBE;
    r.host = 0;
    r.bots = 6; r.slots = 7; r.limit = BOTHOST_RECORD_NONE;
    r.thinks = 300; r.overruns = 4; r.flags = BOTHOST_RECORD_PROBE_BAD;
    r.badProbes = 17; r.probes = 30; r.windowRate = 152;
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == 15);
    UT_ASSERT(buf[6] == 0x01 && buf[7] == 0x2C);  /* thinks 300 big-endian */
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.bots == 6 && b.slots == 7 && b.limit == BOTHOST_RECORD_NONE &&
              b.thinks == 300 && b.overruns == 4 && b.flags == 1 &&
              b.badProbes == 17 && b.probes == 30 && b.windowRate == 152);
    /* A version 1 probe (13 bytes): no window rate. */
    UT_ASSERT(botHostRecordDecode(buf, 13, &b) &&
              b.windowRate == BOTHOST_RECORD_RATE_NONE && b.probes == 30);
    UT_ASSERT(!botHostRecordDecode(buf, 12, &b));
    /* A later version's extra bytes are skipped. */
    buf[n] = 0xAA; buf[n + 1] = 0xBB;
    UT_ASSERT(botHostRecordDecode(buf, n + 2, &b) && b.probes == 30);

    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_READING;
    r.host = 1; r.freePct = 42; r.botCpu = 0xFE; r.bots = 3;
    r.flags = BOTHOST_RECORD_READING_RAN | BOTHOST_RECORD_READING_BAD;
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == 7);
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.freePct == 42 && b.botCpu == 0xFE && b.bots == 3 && b.flags == 3);

    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_DECISION;
    r.host = 3; r.decision = BOTHOST_DECISION_RAISE_REFUSED;
    r.limitBefore = 3; r.limitAfter = 3; r.badProbes = 2; r.probes = 30;
    r.flags = BOTHOST_RECORD_DEC_COST | BOTHOST_RECORD_DEC_FREE;
    r.predictedTenths = botHostRecordTenthsS(-2.46);
    r.costTenths = botHostRecordTenthsU(5.0);
    r.freeAvgTenths = botHostRecordTenthsU(2.54);
    r.lowestClean = 40; r.highestBad = BOTHOST_RECORD_NONE;
    r.seat = BOTHOST_RECORD_NONE; r.toHost = BOTHOST_RECORD_NONE;
    memset(r.reason, 'x', sizeof(r.reason) - 1);
    r.reason[sizeof(r.reason) - 1] = '\0';
    r.srcRate = botHostRecordHundredthsU(1.52);
    r.dstRate = BOTHOST_RECORD_RATE_NONE;
    r.moves = 2; r.rule = BOTHOST_RULE_TRIAL;
    r.staySecs = botHostRecordSecs(61.4); r.needStaySecs = 60;
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == BOTHOST_RECORD_MAX_BYTES);
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.srcRate == 152 && b.dstRate == BOTHOST_RECORD_RATE_NONE &&
              b.moves == 2 && b.rule == BOTHOST_RULE_TRIAL &&
              b.staySecs == 61 && b.needStaySecs == 60);
    /* A version 1 decision (no move fields): they read as none. */
    UT_ASSERT(botHostRecordDecode(buf, n - 10, &b));
    UT_ASSERT(b.srcRate == BOTHOST_RECORD_RATE_NONE &&
              b.rule == BOTHOST_RULE_NONE &&
              b.staySecs == BOTHOST_RECORD_SECS_NONE);
    UT_ASSERT(b.predictedTenths == -25 && b.costTenths == 50 &&
              b.freeAvgTenths == 25);
    UT_ASSERT(b.decision == BOTHOST_DECISION_RAISE_REFUSED &&
              b.lowestClean == 40 && b.highestBad == BOTHOST_RECORD_NONE);
    UT_ASSERT(strlen(b.reason) == sizeof(b.reason) - 1);
    UT_ASSERT(botHostRecordEncode(&r, buf, n - 1) == 0);
    /* A decision with no reason bytes still reads. */
    UT_ASSERT(botHostRecordDecode(buf, 19, &b) && b.reason[0] == '\0');
    UT_ASSERT(!botHostRecordDecode(buf, 18, &b));

    /* A CPU record: the shown value, its source and the full budget. */
    memset(&r, 0, sizeof(r));
    r.kind = BOTHOST_RECORD_CPU;
    r.host = 2; r.shownMilli = 3412; r.source = 3;
    r.budgetMilli = BOTHOST_RECORD_MILLI_NONE; r.botCpu = 0xFE; r.bots = 4;
    r.overrunTenths = BOTHOST_RECORD_RATE_NONE; r.freePct = 61; r.slots = 5;
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == 18);
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.kind == BOTHOST_RECORD_CPU && b.host == 2);
    UT_ASSERT(b.shownMilli == 3412 && b.source == 3);
    UT_ASSERT(b.budgetMilli == BOTHOST_RECORD_MILLI_NONE);
    UT_ASSERT(b.botCpu == 0xFE && b.bots == 4 && b.freePct == 61 &&
              b.slots == 5);
    UT_ASSERT(b.overrunTenths == BOTHOST_RECORD_RATE_NONE);
    UT_ASSERT(!botHostRecordDecode(buf, 17, &b));

    /* An unknown kind or version 0. */
    buf[0] = 1; buf[1] = 99; buf[2] = 0;
    UT_ASSERT(!botHostRecordDecode(buf, 3, &b));
    buf[0] = 0; buf[1] = BOTHOST_RECORD_READING;
    UT_ASSERT(!botHostRecordDecode(buf, 7, &b));
    r.kind = 99;
    UT_ASSERT(botHostRecordEncode(&r, buf, (int)sizeof(buf)) == 0);
    return 0;
}

/* ================================================================
 * -bothost-nomove (BotHostNoMove in bot_host_link.h)
 * ================================================================ */

/* While the game runs with the flag, no bot moves: not from a host (the
 * spread, the lag rule, the overrun check), and not from the server after
 * its placement time. A bot there at the start, or one added later, is
 * placed once in its first ticks. */
int run_bot_host_nomove_no_move_running(void) {
    BotHostNoMove nm, off;
    uint32_t t0 = 1000;

    botHostNoMoveInit(&nm, true);
    botHostNoMoveInit(&off, false);
    botHostNoMoveNewGame(&nm);
    UT_ASSERT(nm.on);
    /* No move from where a bot is, all game long. */
    UT_ASSERT(!botHostNoMoveMayMove(&nm, true));
    UT_ASSERT(botHostNoMoveMayMove(&off, true));
    /* Player 0 on the server at the game start: it may go to a host in its
     * first 500 ticks only. */
    botHostNoMoveSeat(&nm, 0, true, false, t0);
    UT_ASSERT(botHostNoMoveMayPlace(&nm, 0, true, t0 + 499, 500));
    UT_ASSERT(!botHostNoMoveMayPlace(&nm, 0, true, t0 + 500, 500));
    /* Player 1 on a host at the start: it never moves from there, and it is
     * not offered again. */
    botHostNoMoveSeat(&nm, 1, true, true, t0);
    UT_ASSERT(!botHostNoMoveMayPlace(&nm, 1, true, t0 + 1, 500));
    /* Player 2 added at t0 + 3000: it is placed once, in its own first 500
     * ticks. */
    botHostNoMoveSeat(&nm, 2, false, false, t0);
    UT_ASSERT(!botHostNoMoveMayPlace(&nm, 2, true, t0, 500));
    botHostNoMoveSeat(&nm, 2, true, false, t0 + 3000);
    UT_ASSERT(botHostNoMoveMayPlace(&nm, 2, true, t0 + 3100, 500));
    botHostNoMoveSeat(&nm, 2, true, true, t0 + 3200);   /* it went */
    UT_ASSERT(!botHostNoMoveMayPlace(&nm, 2, true, t0 + 3300, 500));
    /* The overrun check records "would move" in place of a move. */
    UT_ASSERT(botHostNoMoveOverrunDecision(&nm, true) ==
              BOTHOST_DECISION_WOULD_MOVE);
    UT_ASSERT(botHostNoMoveOverrunDecision(&off, true) ==
              BOTHOST_DECISION_MOVE);
    /* Without the flag every seat may be offered at any time. */
    botHostNoMoveSeat(&off, 0, true, true, t0);
    UT_ASSERT(botHostNoMoveMayPlace(&off, 0, true, t0 + 100000, 500));
    return 0;
}

/* The fall-backs still happen with the flag: a host that offers 0 slots
 * (the opt-out) gives every bot back, a host with fewer slots than bots
 * gives one back, and a bot that came back stays on the server. */
int run_bot_host_nomove_fallback(void) {
    BotHostNoMove nm;
    int seatHost[8] = {-1, 1, 1, 0, -1, -1, -1, -1};
    uint32_t mask;

    botHostNoMoveInit(&nm, true);
    botHostNoMoveNewGame(&nm);
    mask = botHostSeatsToGiveBack(seatHost, 8, 1, 0);
    UT_ASSERT_MSG(mask == ((1u << 1) | (1u << 2)), "mask 0x%x", (unsigned)mask);
    /* Fewer slots than bots: one goes back while the game runs. */
    UT_ASSERT(botHostNoMoveGiveBack(&nm, true, 3, 2));
    UT_ASSERT(botHostNoMoveGiveBack(&nm, true, 1, 0));
    UT_ASSERT(!botHostNoMoveGiveBack(&nm, true, 2, 2));
    UT_ASSERT(!botHostNoMoveGiveBack(&nm, true, 0, 0));
    /* Outside the game (or without the flag) the spread does it. */
    UT_ASSERT(!botHostNoMoveGiveBack(&nm, false, 3, 2));
    /* Player 1 was on host 1 and fell back: it stays on the server. */
    botHostNoMoveSeat(&nm, 1, true, true, 100);
    botHostNoMoveSeat(&nm, 1, true, false, 150);
    UT_ASSERT(!botHostNoMoveMayPlace(&nm, 1, true, 160, 500));
    return 0;
}

/* The lobby places as without the flag, and the next lobby or round starts
 * every seat again. */
int run_bot_host_nomove_lobby_places(void) {
    BotHostNoMove nm;

    botHostNoMoveInit(&nm, true);
    UT_ASSERT(botHostNoMoveMayMove(&nm, false));
    botHostNoMoveSeat(&nm, 0, true, true, 10);
    botHostNoMoveSeat(&nm, 0, true, false, 20);
    UT_ASSERT(botHostNoMoveMayPlace(&nm, 0, false, 100000, 500));
    UT_ASSERT(botHostNoMoveOverrunDecision(&nm, false) ==
              BOTHOST_DECISION_MOVE);
    UT_ASSERT(!botHostNoMoveGiveBack(&nm, false, 4, 0));
    /* The game ran and player 0 was on a host; the next game starts again. */
    UT_ASSERT(!botHostNoMoveMayPlace(&nm, 0, true, 30, 500));
    botHostNoMoveNewGame(&nm);
    UT_ASSERT(nm.on);
    botHostNoMoveSeat(&nm, 0, true, false, 40);
    UT_ASSERT(botHostNoMoveMayPlace(&nm, 0, true, 50, 500));
    return 0;
}

/* The Linux free CPU reading: steal (the 8th value) is busy, free is idle
 * + iowait only, and an older kernel's shorter line reads steal as 0. */
int run_machine_cpu_proc_stat_steal(void) {
    uint64_t i0, t0, i1, t1;
    /* user nice system idle iowait irq softirq steal guest guest_nice */
    UT_ASSERT(machineCpuParseProcStat(
        "cpu  1000 0 500 8000 200 0 0 300 0 0\n", &i0, &t0));
    UT_ASSERT(i0 == 8200 && t0 == 10000);
    /* 1000 ticks later: 200 user, 100 system, 300 idle, 100 iowait,
     * 300 steal. Free = (300 + 100) / 1000 = 40%; without steal as busy it
     * would read 400 / 700 = 57%. */
    UT_ASSERT(machineCpuParseProcStat(
        "cpu  1200 0 600 8300 300 0 0 600 0 0\n", &i1, &t1));
    UT_ASSERT(i1 == 8600 && t1 == 11000);
    UT_ASSERT_MSG(machineCpuFreePercent(i0, t0, i1, t1) == 40, "free %u",
                  (unsigned)machineCpuFreePercent(i0, t0, i1, t1));

    /* An older kernel: four values (no iowait, no steal). */
    UT_ASSERT(machineCpuParseProcStat("cpu 100 0 100 800\n", &i0, &t0));
    UT_ASSERT(i0 == 800 && t0 == 1000);
    /* Seven values (no steal). */
    UT_ASSERT(machineCpuParseProcStat("cpu 100 0 100 800 50 0 0\n", &i0, &t0));
    UT_ASSERT(i0 == 850 && t0 == 1050);
    /* Not a cpu line, or too short. */
    UT_ASSERT(!machineCpuParseProcStat("intr 1 2 3 4\n", &i0, &t0));
    UT_ASSERT(!machineCpuParseProcStat("cpu 1 2 3\n", &i0, &t0));
    UT_ASSERT(!machineCpuParseProcStat(NULL, &i0, &t0));
    return 0;
}
