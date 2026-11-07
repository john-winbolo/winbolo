/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The overrun check of the bot placement (botHostOverrunProbe and
 * botHostOverrunMayTake in bot_host_link.c), the free CPU percent
 * (machine_cpu_free.c), CTRL_BOT_HOST_CPU_FREE and the log_BotHostData
 * payload (bothost_record.h).
 *
 * Each probe is one second of one host: its bots' new thinks and overruns.
 * The tests feed probes of 100 thinks, 0 overruns for a clean probe and 5
 * for a bad one (5% is over the 1% default).
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

/* One probe: bad or clean, with the host's bots, slots, free CPU and bot
 * CPU. */
static BotHostOverrunAction ovrFeed(BotHostOverrunState *st,
                                    const BotHostOverrunCfg *cfg, bool bad,
                                    int bots, int slots, uint8_t freePct,
                                    uint8_t botCpu, bool canMove,
                                    BotHostOverrunReport *rep) {
    BotHostOverrunProbe in;
    memset(&in, 0, sizeof(in));
    in.thinks = 100;
    in.overruns = bad ? 5u : 0u;
    in.bots = bots;
    in.slots = slots;
    in.freePct = freePct;
    in.botCpu = botCpu;
    in.canMove = canMove;
    return botHostOverrunProbe(st, cfg, &in, rep);
}

static void ovrStart(BotHostOverrunState *st, BotHostOverrunCfg *cfg) {
    botHostOverrunCfgDefaults(cfg);
    botHostOverrunReset(st);
}

/* 6 clean probes, then 24 bad: the 30th probe has 24 of 30 bad (80%) and
 * the host steps down by one bot. No probe before it steps. */
int run_bot_host_overrun_trigger_24_of_30(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunAction a;
    int k;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 30; k++) {
        a = ovrFeed(&st, &cfg, k > 6, 4, 8, 40, 20, true, &rep);
        if (k < 30) {
            UT_ASSERT_MSG(a == BOT_HOST_OVR_NONE, "probe %d: action %d", k,
                          (int)a);
        }
    }
    UT_ASSERT_MSG(a == BOT_HOST_OVR_LOWER, "probe 30: action %d", (int)a);
    UT_ASSERT(rep.badProbes == 24 && rep.probes == 30);
    UT_ASSERT(rep.limitBefore == -1 && rep.newLimit == 3);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    UT_ASSERT(st.cut);
    return 0;
}

/* 7 clean probes, then 23 bad, then clean: never more than 23 of 30 bad,
 * so no step. */
int run_bot_host_overrun_no_trigger_23_of_30(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int k;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 60; k++) {
        BotHostOverrunAction a =
            ovrFeed(&st, &cfg, k > 7 && k <= 30, 4, 8, 40, 20, true, &rep);
        UT_ASSERT_MSG(a == BOT_HOST_OVR_NONE, "probe %d: action %d", k,
                      (int)a);
        if (k == 30) UT_ASSERT(rep.badProbes == 23 && rep.probes == 30);
    }
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8);
    return 0;
}

/* A short spike: 10 bad seconds in a clean minute, then 20 bad seconds
 * after it. Neither reaches 24 of 30. Probes with no thinks are never bad. */
int run_bot_host_overrun_short_spike(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostOverrunProbe idle;
    int k;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 120; k++) {
        bool bad = (k > 20 && k <= 30) || (k > 70 && k <= 90);
        BotHostOverrunAction a =
            ovrFeed(&st, &cfg, bad, 4, 8, 40, 20, true, &rep);
        UT_ASSERT_MSG(a == BOT_HOST_OVR_NONE, "probe %d: action %d", k,
                      (int)a);
    }
    /* 30 seconds with no thinks: not bad. */
    memset(&idle, 0, sizeof(idle));
    idle.bots = 4;
    idle.slots = 8;
    idle.overruns = 3;
    idle.freePct = 40;
    idle.botCpu = 20;
    idle.canMove = true;
    for (k = 0; k < 30; k++) {
        UT_ASSERT(botHostOverrunProbe(&st, &cfg, &idle, &rep) ==
                  BOT_HOST_OVR_NONE);
    }
    UT_ASSERT(rep.badProbes == 0);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8);
    return 0;
}

/* All probes bad: the first step at 24 bad probes; the window starts again
 * after it; the next step waits for 30 s after the first even though 24
 * bad probes come at 24 s. */
int run_bot_host_overrun_step_spacing(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    int k, first = 0, second = 0, bots = 4;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 120 && second == 0; k++) {
        BotHostOverrunAction a =
            ovrFeed(&st, &cfg, true, bots, 8, 40, 20, true, &rep);
        if (a == BOT_HOST_OVR_LOWER) {
            if (first == 0) {
                first = k;
                bots--;  /* the bot moved off */
            } else {
                second = k;
            }
        } else if (first != 0 && k == first + 1) {
            /* The window was cleared by the step. */
            UT_ASSERT_MSG(rep.probes == 1 && rep.badProbes == 1,
                          "after the step: %d of %d", rep.badProbes,
                          rep.probes);
        }
    }
    UT_ASSERT_MSG(first == 24, "first step at %d", first);
    UT_ASSERT_MSG(second == first + 30, "second step at %d", second);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 2);
    return 0;
}

/* The server stays the last fall-back:
 *   - a trigger with no bot that may move waits, and the limit stays;
 *   - a Bot Host's bot finds no other place with room, so the caller
 *     sends it to the server;
 *   - a lowered server limit moves a server bot only to a host with room. */
int run_bot_host_overrun_server_fallback(void) {
    BotHostOverrunState st;
    BotHostOverrunCfg cfg;
    BotHostOverrunReport rep;
    BotHostPlaceHost h[3];
    BotHostPlaceLag lag;
    int k, from = -1, to = -1;
    BotHostOverrunAction a = BOT_HOST_OVR_NONE;

    ovrStart(&st, &cfg);
    for (k = 1; k <= 24; k++) {
        a = ovrFeed(&st, &cfg, true, 4, 4, 30, 40, false, &rep);
    }
    UT_ASSERT_MSG(a == BOT_HOST_OVR_WAIT, "action %d", (int)a);
    UT_ASSERT(botHostOverrunLimit(&st, 4) == 4);
    UT_ASSERT(!st.cut);
    /* A bot that may move now: the step happens. */
    a = ovrFeed(&st, &cfg, true, 4, 4, 30, 40, true, &rep);
    UT_ASSERT(a == BOT_HOST_OVR_LOWER && rep.newLimit == 3);

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
 * free CPU b until the step, phase 3 clean with free CPU r and one bot
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
    for (k = 0; k < 30; k++) ovrFeed(st, cfg, false, 4, 8, c1, 20, true, &rep);
    for (k = 0; k < 60 && !cut; k++) {
        cut = ovrFeed(st, cfg, true, 4, 8, b, 20, true, &rep) ==
              BOT_HOST_OVR_LOWER;
    }
    if (!cut) return BOT_HOST_OVR_NONE;
    for (k = 0; k < 120; k++) {
        BotHostOverrunAction a =
            ovrFeed(st, cfg, false, 3, 8, r, 15, true, &rep);
        if (a == BOT_HOST_OVR_RAISE || a == BOT_HOST_OVR_REFUSE) {
            *out = rep;
            return a;
        }
    }
    return BOT_HOST_OVR_NONE;
}

/* Clean at 40% free, bad at 10% free, then 50% free: predicted 50 - 5 = 45;
 * a clean reading had 40% (at or below 45) and the bad ones had 10% (below
 * 45): the limit goes up by one, 30 s after the step. */
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
        a = ovrFeed(&st, &cfg, false, 3, 8, BOT_HOST_FREE_UNKNOWN, 15, true,
                    &rep);
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
    bool cut = false;

    ovrStart(&st, &cfg);
    for (k = 0; k < 60 && !cut; k++) {
        cut = ovrFeed(&st, &cfg, true, 4, 8, 30, 20, true, &rep) ==
              BOT_HOST_OVR_LOWER;
    }
    UT_ASSERT(cut && botHostOverrunLimit(&st, 8) == 3);
    for (k = 0; k < 10; k++) ovrFeed(&st, &cfg, true, 3, 8, 30, 15, true, &rep);
    readings = st.readingCount;
    UT_ASSERT(readings > 0 && st.costKnown);

    botHostOverrunNewGame(&st);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 8);
    UT_ASSERT(!st.cut && st.probeCount == 0);
    UT_ASSERT(st.readingCount == readings && st.costKnown);
    /* The first probe of the new game sees an empty window, and a step
     * may come at once (no 30 s wait from the old game's step). */
    ovrFeed(&st, &cfg, false, 3, 8, 30, 15, true, &rep);
    UT_ASSERT(rep.probes == 1);

    botHostOverrunReset(&st);
    UT_ASSERT(st.readingCount == 0 && !st.costKnown && st.limit == -1);

    /* The check off. */
    ovrStart(&st, &cfg);
    for (k = 0; k < 24; k++) ovrFeed(&st, &cfg, true, 4, 8, 30, 20, true, &rep);
    UT_ASSERT(botHostOverrunLimit(&st, 8) == 3);
    cfg.pct = 0.0;
    UT_ASSERT(ovrFeed(&st, &cfg, true, 3, 8, 30, 15, true, &rep) ==
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
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 1, 8, 60, 10, true, &rep);
    UT_ASSERT(st.readingCount == 1);
    UT_ASSERT(botHostOverrunMayTake(&st, &cfg, 1, 60, &rep));
    UT_ASSERT(strcmp(rep.why, "room for one more bot") == 0);
    /* The second bot is placed (2 bots) but no reading counts it yet. */
    UT_ASSERT(!botHostOverrunMayTake(&st, &cfg, 2, 60, &rep));
    UT_ASSERT(strcmp(rep.why, "no reading counts its newest bot yet") == 0);
    /* A reading with 2 bots at 30% free, 80% bot CPU (40% a bot): predicted
     * free = (60 + 30) / 2 - 40 = 5 < 40, and no clean reading had 5% or
     * less: no third bot. */
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 2, 8, 30, 80, true, &rep);
    UT_ASSERT(!botHostOverrunMayTake(&st, &cfg, 2, 30, &rep));
    UT_ASSERT_MSG(strcmp(rep.why, "too little free CPU") == 0, "%s", rep.why);

    /* With a bad reading: the raise rule. Clean at 30% free with 2 bots
     * (cost 10% a bot), bad at 20%; now 50% free: predicted 40, clean 30
     * <= 40, bad 20 < 40: yes. */
    ovrStart(&st, &cfg);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 2, 8, 30, 20, true, &rep);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, true, 2, 8, 20, 20, true, &rep);
    for (k = 0; k < 30; k++) ovrFeed(&st, &cfg, false, 2, 8, 50, 20, true, &rep);
    UT_ASSERT_MSG(botHostOverrunMayTake(&st, &cfg, 2, 50, &rep), "%s", rep.why);
    UT_ASSERT(rep.highestBad == 20 && rep.lowestClean == 30);
    /* The same with the bad reading at 45% free: predicted 40 <= 45. */
    ovrStart(&st, &cfg);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, false, 2, 8, 30, 20, true, &rep);
    for (k = 0; k < 5; k++) ovrFeed(&st, &cfg, true, 2, 8, 45, 20, true, &rep);
    for (k = 0; k < 30; k++) ovrFeed(&st, &cfg, false, 2, 8, 50, 20, true, &rep);
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
    r.badProbes = 17; r.probes = 30;
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == 13);
    UT_ASSERT(buf[6] == 0x01 && buf[7] == 0x2C);  /* thinks 300 big-endian */
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.bots == 6 && b.slots == 7 && b.limit == BOTHOST_RECORD_NONE &&
              b.thinks == 300 && b.overruns == 4 && b.flags == 1 &&
              b.badProbes == 17 && b.probes == 30);
    UT_ASSERT(!botHostRecordDecode(buf, n - 1, &b));
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
    n = botHostRecordEncode(&r, buf, (int)sizeof(buf));
    UT_ASSERT(n == BOTHOST_RECORD_MAX_BYTES);
    UT_ASSERT(botHostRecordDecode(buf, n, &b));
    UT_ASSERT(b.predictedTenths == -25 && b.costTenths == 50 &&
              b.freeAvgTenths == 25);
    UT_ASSERT(b.decision == BOTHOST_DECISION_RAISE_REFUSED &&
              b.lowestClean == 40 && b.highestBad == BOTHOST_RECORD_NONE);
    UT_ASSERT(strlen(b.reason) == sizeof(b.reason) - 1);
    UT_ASSERT(botHostRecordEncode(&r, buf, n - 1) == 0);
    /* A decision with no reason bytes still reads. */
    UT_ASSERT(botHostRecordDecode(buf, 19, &b) && b.reason[0] == '\0');
    UT_ASSERT(!botHostRecordDecode(buf, 18, &b));

    /* An unknown kind or version 0. */
    buf[0] = 1; buf[1] = 99; buf[2] = 0;
    UT_ASSERT(!botHostRecordDecode(buf, 3, &b));
    buf[0] = 0; buf[1] = BOTHOST_RECORD_READING;
    UT_ASSERT(!botHostRecordDecode(buf, 7, &b));
    r.kind = 99;
    UT_ASSERT(botHostRecordEncode(&r, buf, (int)sizeof(buf)) == 0);
    return 0;
}
