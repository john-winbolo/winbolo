/*
 * A Bot Host's slots: the lobby's Max slots of a player's own Bot Host
 * (Max 0 runs no bot on this computer) and the dynamic slots of each
 * place (start, ceiling and live limit).
 *
 *   1. bot_host_optout_slots_codec: Max 0 is an existing message, a
 *      CTRL_BOT_HOST_SLOTS with 0 slots and no refusal. It keeps its
 *      fields through the wire form and its body has the same length as
 *      before, so an older server reads it as a host with no room.
 *   2. bot_host_optout_give_back: the server gives back every seat of a
 *      host that reports 0 slots (botHostSeatsToGiveBack) and no seat of a
 *      host that still has slots. Then, with the placement, the bots it
 *      gave back go to the server and to the other host, the 0-slot host
 *      gets no bot while it has 0 slots, and it takes bots again when it
 *      offers its slots again.
 *   3. bot_host_same_server_codec: CTRL_BOT_HOST_SAME_SERVER is one byte,
 *      the kind; an empty body is refused; later bytes are skipped.
 *   4. bot_host_same_server_stop: the server tells a player's host on its
 *      own computer once a connection, and never a listed host there or a
 *      host that shares another computer. The client's built-in host
 *      stops on it (0 slots), only for the server's kind; a new join runs
 *      it again.
 *   5. bot_host_max_saved: Max slots from and to its saved text, the
 *      ceiling and the offer, and the client config keeps it in a section
 *      that does not go to WinBolo.net.
 *   6. bot_host_dyn_raise_rule: a live limit goes up by up to the raise
 *      step once each step time, never above the ceiling, and each of the
 *      rule's conditions a to e stops a raise.
 *   7. bot_host_dyn_under_reserve: a place under its reserve for a full
 *      window counts as bad and gives a bot to a place with room; a short
 *      dip that has ended, a window that is not full, no bots or no
 *      reserve does not count; after a move the next one needs a full
 *      window again; with no room the bots stay.
 *   8. bot_host_dyn_carry: a new game starts at the start, or with
 *      carryLimit at the limit the last game ended with, and says which.
 *   9. bot_host_dyn_keel: the KEEL values give fixed slots: no live limit,
 *      no raise past the slots, a ceiling that is the start.
 *  10. bot_host_dyn_determinism: a determinism run has no dynamic slots.
 *  11. bot_host_limits_codec: CTRL_BOT_HOST_LIMITS and _ROW_LIMITS on the
 *      wire, and the --cpu-reserve percent.
 *  12. bot_host_slot_owner_only: only the player at a player's computer
 *      may change that row's slots.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bot_host_link.h"
#include "bot_host_machine.h"
#include "control_event.h"
#include "transport_control_codec.h"
#include "bothost_record.h"
#include "common/prefs.h"
#include "common/prefs_doc.h"
#include "../../src/bothost/bothost_optout.h"
#include "test_harness.h"

/* ================================================================
 * 1. The 0-slot report
 * ================================================================ */
int run_bot_host_optout_slots_codec(void) {
    ControlEvent in, out;
    uint8_t buf[64];
    size_t len0 = 0, len3 = 0;
    ControlEncodeBodyFn enc = transportControlCodecBodyEncoder(CTRL_BOT_HOST_SLOTS);
    ControlDecodeBodyFn dec = transportControlCodecBodyDecoder(CTRL_BOT_HOST_SLOTS);

    UT_ASSERT(enc != NULL && dec != NULL);

    /* Max 0: 0 slots, no seat turned down. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_SLOTS;
    in.u.botHostSlots.slots = 0;
    in.u.botHostSlots.refuseSeat = BOT_HOST_SLOTS_NO_REFUSE;
    in.u.botHostSlots.refuseGen = 0;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len0) == ENCODE_OK);
    memset(&out, 0xAB, sizeof(out));
    UT_ASSERT(dec(buf, len0, &out));
    UT_ASSERT(out.type == CTRL_BOT_HOST_SLOTS);
    UT_ASSERT(out.u.botHostSlots.slots == 0);
    UT_ASSERT(out.u.botHostSlots.refuseSeat == BOT_HOST_SLOTS_NO_REFUSE);
    UT_ASSERT(out.u.botHostSlots.refuseGen == 0);

    /* Max back up: the host offers its slots again. */
    in.u.botHostSlots.slots = 3;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len3) == ENCODE_OK);
    UT_ASSERT(dec(buf, len3, &out));
    UT_ASSERT(out.u.botHostSlots.slots == 3);
    UT_ASSERT(out.u.botHostSlots.refuseSeat == BOT_HOST_SLOTS_NO_REFUSE);

    /* The body is the one an older server decodes: [slots 1][refuseSeat 1]
     * [refuseGen 2], 4 bytes either way. */
    UT_ASSERT(len0 == 4 && len3 == 4);
    printf("  CTRL_BOT_HOST_SLOTS 0 and 3: %u-byte bodies\n", (unsigned)len0);
    return 0;
}

/* ================================================================
 * 2. The server gives the bots back
 * ================================================================ */

/* Moves bots by botHostPlaceRebalanceLag until no move is left, as the
 * server does. Returns the moves, or -1 when it does not end. */
static int optoutSettle(BotHostPlaceHost *h, int n, const BotHostPlaceLag *lag,
                        int closed) {
    int moves = 0, from = -1, to = -1;
    while (botHostPlaceRebalanceLag(h, n, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                    BOT_HOST_SMALL_BIAS_DEFAULT, lag, &from,
                                    &to)) {
        if (to == closed) return -2;
        h[from].bots--;
        h[to].bots++;
        if (++moves > 500) return -1;
    }
    return moves;
}

int run_bot_host_optout_give_back(void) {
    static const BotHostPlaceLag lag = { 100.0, 250.0,
                                         BOT_HOST_LAG_GAIN_DEFAULT };
    /* Seats 0..7: seats 1 and 4 on host 0, seats 2, 5 and 6 on host 1, the
     * others in-process. */
    int seatHost[8] = { -1, 0, 1, -1, 0, 1, 1, -1 };
    BotHostPlaceHost h[3];
    uint32_t mask;
    int i, k, moves;

    /* Host 1 reports 0 slots: seats 2, 5, 6 come back; host 0 keeps its. */
    mask = botHostSeatsToGiveBack(seatHost, 8, 1, 0);
    UT_ASSERT(mask == ((1u << 2) | (1u << 5) | (1u << 6)));
    mask = botHostSeatsToGiveBack(seatHost, 8, 0, 0);
    UT_ASSERT(mask == ((1u << 1) | (1u << 4)));
    /* A host that still has slots gives nothing back at once. */
    UT_ASSERT(botHostSeatsToGiveBack(seatHost, 8, 1, 3) == 0);
    UT_ASSERT(botHostSeatsToGiveBack(seatHost, 8, 1, 1) == 0);
    /* No such host, no table. */
    UT_ASSERT(botHostSeatsToGiveBack(seatHost, 8, 2, 0) == 0);
    UT_ASSERT(botHostSeatsToGiveBack(seatHost, 8, -1, 0) == 0);
    UT_ASSERT(botHostSeatsToGiveBack(NULL, 8, 1, 0) == 0);

    /* The placement: index 0 the server (4 slots, unlimited, no lag),
     * 1 and 2 two players' hosts of 3 slots each. */
    memset(h, 0, sizeof(h));
    h[0].slots = 4; h[0].unlimited = true; h[0].lagMs = 0.0;
    h[1].slots = 3; h[1].lagMs = 20.0;
    h[2].slots = 3; h[2].lagMs = 20.0;
    for (i = 0; i < 3; i++) {
        h[i].open = true;
        h[i].movable = true;
    }
    for (k = 0; k < 8; k++) {
        int p = botHostPlacePickLag(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                    BOT_HOST_SMALL_BIAS_DEFAULT, &lag);
        UT_ASSERT(p >= 0);
        h[p].bots++;
    }
    printf("  8 bots placed: server %d, host A %d/3, host B %d/3\n",
           h[0].bots, h[1].bots, h[2].bots);
    UT_ASSERT(h[1].bots > 0);

    /* Host A sets Max 0: 0 slots, and every bot on it comes back to the
     * server at once. */
    h[1].slots = 0;
    h[0].bots += h[1].bots;
    h[1].bots = 0;
    moves = optoutSettle(h, 3, &lag, 1);
    UT_ASSERT(moves >= 0);
    printf("  host A at 0 slots: server %d, host A %d/0, host B %d/3 "
           "(%d moves)\n", h[0].bots, h[1].bots, h[2].bots, moves);
    UT_ASSERT(h[1].bots == 0);
    UT_ASSERT(h[2].bots <= 3);
    UT_ASSERT(h[0].bots + h[2].bots == 8);
    /* A new bot never goes to host A while it has 0 slots, even when
     * host B is full. */
    {
        int p = botHostPlacePickLag(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                    BOT_HOST_SMALL_BIAS_DEFAULT, &lag);
        UT_ASSERT(p != 1);
        h[2].bots += 3 - h[2].bots;
        h[0].bots = 8 - h[2].bots;
        p = botHostPlacePickLag(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                BOT_HOST_SMALL_BIAS_DEFAULT, &lag);
        UT_ASSERT(p == 0);
    }

    /* Host A sets Max 3 again: it offers 3 slots again and the rebalance
     * moves bots back onto it. */
    h[1].slots = 3;
    moves = optoutSettle(h, 3, &lag, -1);
    UT_ASSERT(moves > 0);
    printf("  host A at 3 slots again: server %d, host A %d/3, host B %d/3 "
           "(%d moves)\n", h[0].bots, h[1].bots, h[2].bots, moves);
    UT_ASSERT(h[1].bots > 0 && h[1].bots <= 3);
    UT_ASSERT(h[2].bots <= 3);
    return 0;
}

/* ================================================================
 * 3. CTRL_BOT_HOST_SAME_SERVER on the wire
 * ================================================================ */
int run_bot_host_same_server_codec(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_BOT_HOST_SAME_SERVER);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_BOT_HOST_SAME_SERVER);
    ControlEvent in, out;
    uint8_t buf[16];
    size_t len = 0;
    UT_ASSERT(enc != NULL && dec != NULL);
    UT_ASSERT(strcmp(controlEventTypeName(CTRL_BOT_HOST_SAME_SERVER),
                     "CTRL_BOT_HOST_SAME_SERVER") == 0);
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_SAME_SERVER;
    in.u.botHostSameServer.kind = BOT_HOST_MACHINE_SERVER;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 1 && buf[0] == BOT_HOST_MACHINE_SERVER);
    UT_ASSERT(enc(&in, NULL, buf, 0, &len) != ENCODE_OK);
    memset(&out, 0xEE, sizeof(out));
    UT_ASSERT(dec(buf, 1, &out));
    UT_ASSERT(out.type == CTRL_BOT_HOST_SAME_SERVER);
    UT_ASSERT(out.u.botHostSameServer.kind == BOT_HOST_MACHINE_SERVER);
    UT_ASSERT(!dec(buf, 0, &out));
    /* A later field from a newer server is skipped. */
    buf[1] = 0x7F;
    UT_ASSERT(dec(buf, 2, &out));
    UT_ASSERT(out.u.botHostSameServer.kind == BOT_HOST_MACHINE_SERVER);
    return 0;
}

/* ================================================================
 * 4. The server tells; the client's host stops for this join
 * ================================================================ */
int run_bot_host_same_server_stop(void) {
    BotHostMachineIn in[4];
    BotHostMachineOut out[4];
    bool known[4] = { false, false, false, false };
    BotHostRun run;
    int i;

    UT_ASSERT(BOT_HOST_RUN_SAME_SERVER_KIND == BOT_HOST_MACHINE_SERVER);
    memset(in, 0, sizeof(in));
    memset(out, 0, sizeof(out));
    /* 0: a player's host from 127.0.0.1; 1: a listed host on the server's
     * computer; 2 and 3: two players' hosts on one other computer. */
    in[0].present = true; in[0].playerHost = true; in[0].addrServer = true;
    in[0].joinSeq = 1; in[0].offered = 4;
    in[1].present = true; in[1].addrServer = true;
    in[1].joinSeq = 2; in[1].offered = 4;
    for (i = 2; i < 4; i++) {
        in[i].present = true;
        in[i].playerHost = true;
        in[i].hashHave = true;
        memset(in[i].hash, 0x33, sizeof(in[i].hash));
        in[i].joinSeq = (uint32_t)(i + 1);
        in[i].offered = 4;
    }
    /* Not known yet: nobody is told. */
    for (i = 0; i < 4; i++) {
        UT_ASSERT(!botHostMachineTellSameServer(known[i], &out[i], false));
    }
    botHostMachineStep(in, 4, NULL, false, out, known);
    UT_ASSERT(botHostMachineTellSameServer(known[0], &out[0], false));
    /* Once a connection. */
    UT_ASSERT(!botHostMachineTellSameServer(known[0], &out[0], true));
    /* A listed host there keeps its slots and is not told. */
    UT_ASSERT(!out[1].zero);
    UT_ASSERT(!botHostMachineTellSameServer(known[1], &out[1], false));
    /* Two clients on one computer: one of them gets 0, neither is told. */
    UT_ASSERT(out[2].zero != out[3].zero);
    UT_ASSERT(!botHostMachineTellSameServer(known[2], &out[2], false));
    UT_ASSERT(!botHostMachineTellSameServer(known[3], &out[3], false));

    /* The client's host: runs at the join, stops on the event. */
    botHostRunJoin(&run);
    UT_ASSERT(!botHostRunStopped(&run) && botHostRunSlots(&run, 4) == 4);
    /* Another kind is not the server's computer: it keeps running. */
    UT_ASSERT(!botHostRunSameServer(&run, BOT_HOST_MACHINE_HOST));
    UT_ASSERT(!botHostRunStopped(&run));
    UT_ASSERT(botHostRunSameServer(&run, BOT_HOST_MACHINE_SERVER));
    UT_ASSERT(botHostRunStopped(&run) && botHostRunSlots(&run, 4) == 0);
    /* A second event changes nothing. */
    UT_ASSERT(!botHostRunSameServer(&run, BOT_HOST_MACHINE_SERVER));
    UT_ASSERT(botHostRunStopped(&run));
    /* The next join: it runs as normal again. */
    botHostRunJoin(&run);
    UT_ASSERT(!botHostRunStopped(&run) && botHostRunSlots(&run, 4) == 4);
    return 0;
}

/* ================================================================
 * 5. Max slots, saved
 * ================================================================ */
int run_bot_host_max_saved(void) {
    char path[1024], buf[32];
    char *body;
    PrefsDoc *up;

    /* From its saved text. */
    UT_ASSERT(botHostMaxParse(NULL) == BOT_HOST_MAX_AUTO);
    UT_ASSERT(botHostMaxParse("") == BOT_HOST_MAX_AUTO);
    UT_ASSERT(botHostMaxParse("Auto") == BOT_HOST_MAX_AUTO);
    UT_ASSERT(botHostMaxParse("-3") == BOT_HOST_MAX_AUTO);
    UT_ASSERT(botHostMaxParse("4x") == BOT_HOST_MAX_AUTO);
    UT_ASSERT(botHostMaxParse("0") == 0);
    UT_ASSERT(botHostMaxParse("7") == 7);
    UT_ASSERT(botHostMaxParse("16") == 16);
    UT_ASSERT(botHostMaxParse("99") == BOT_HOST_MAX_TOP);
    /* To its saved text, and back. */
    botHostMaxFormat(BOT_HOST_MAX_AUTO, buf, sizeof(buf));
    UT_ASSERT(strcmp(buf, "Auto") == 0);
    botHostMaxFormat(0, buf, sizeof(buf));
    UT_ASSERT(strcmp(buf, "0") == 0 && botHostMaxParse(buf) == 0);
    botHostMaxFormat(40, buf, sizeof(buf));
    UT_ASSERT(strcmp(buf, "16") == 0);

    /* The ceiling: Max, or the brain's answer for Auto. */
    UT_ASSERT(botHostMaxCeiling(BOT_HOST_MAX_AUTO, 5) == 5);
    UT_ASSERT(botHostMaxCeiling(BOT_HOST_MAX_AUTO, -1) == 0);
    UT_ASSERT(botHostMaxCeiling(BOT_HOST_MAX_AUTO, 40) == BOT_HOST_MAX_TOP);
    UT_ASSERT(botHostMaxCeiling(0, 5) == 0);
    UT_ASSERT(botHostMaxCeiling(9, 5) == 9);
    /* The offer: the ceiling to a server that sent the hello; at most the
     * start to an older server, so it never counts the ceiling. */
    UT_ASSERT(botHostMaxOffer(9, 2, true) == 9);
    UT_ASSERT(botHostMaxOffer(9, 2, false) == 2);
    UT_ASSERT(botHostMaxOffer(1, 2, false) == 1);
    UT_ASSERT(botHostMaxOffer(0, 2, true) == 0);
    UT_ASSERT(botHostMaxOffer(0, 2, false) == 0);
    /* The Bot Host program's ceiling: --slots-max, else the larger of
     * --slots and CPUs - 1, at most 16; --keel: --slots. */
    UT_ASSERT(botHostSlotsMaxDefault(6, 2, 12, false) == 6);
    UT_ASSERT(botHostSlotsMaxDefault(0, 2, 12, false) == 11);
    UT_ASSERT(botHostSlotsMaxDefault(0, 4, 2, false) == 4);
    UT_ASSERT(botHostSlotsMaxDefault(0, 2, 64, false) == BOT_HOST_MAX_TOP);
    UT_ASSERT(botHostSlotsMaxDefault(0, 3, 12, true) == 3);
    UT_ASSERT(botHostSlotsMaxDefault(6, 3, 12, true) == 6);

    /* The client config keeps it for later games. */
    UT_ASSERT(utScratchPath(path, sizeof(path), "bothost_max_prefs.json"));
    remove(path);
    prefsInit(path);
    botHostMaxFormat(3, buf, sizeof(buf));
    UT_ASSERT(prefsSetString(BOT_HOST_MAX_PREFS_SECTION,
                             BOT_HOST_MAX_PREFS_KEY, buf));
    /* It fits this computer only: it does not go to WinBolo.net. */
    body = prefsSerializeForUpload();
    UT_ASSERT(body != NULL);
    up = prefsDocParseJson(body);
    free(body);
    UT_ASSERT(up != NULL);
    prefsDocGetString(up, BOT_HOST_MAX_PREFS_SECTION, BOT_HOST_MAX_PREFS_KEY,
                      "<absent>", buf, sizeof(buf));
    prefsDocFree(up);
    UT_ASSERT_MSG(strcmp(buf, "<absent>") == 0,
                  "Max slots went into the upload body: '%s'", buf);
    prefsShutdown();
    prefsInit(path);
    prefsGetString(BOT_HOST_MAX_PREFS_SECTION, BOT_HOST_MAX_PREFS_KEY, "Auto",
                   buf, sizeof(buf));
    prefsShutdown();
    remove(path);
    UT_ASSERT_MSG(botHostMaxParse(buf) == 3, "saved Max slots read '%s'",
                  buf);
    printf("  Max slots 3 saved and read back\n");
    return 0;
}

/* ================================================================
 * 6. to 9. Dynamic slots
 * ================================================================ */

/* One kind of second for dynRun. */
typedef struct {
    int    freePct;     /* the free CPU reading */
    int    cost;        /* one bot's CPU percent */
    double lagMs;
    bool   full;        /* the bots are at the limit; else one below */
    bool   running;
    int    permille;    /* overruns per 1000 thinks */
} DynSecond;

static const DynSecond dynGood = { 80, 5, 50.0, true, true, 0 };

/* A place: start 2, ceiling 8, reserve 25 that refuses new bots too, at a
 * new game. */
static void dynSetUp(BotHostOverrunState *st, BotHostOverrunCfg *cfg) {
    botHostOverrunCfgDefaults(cfg);
    cfg->dynamic = true;
    botHostOverrunReset(st);
    botHostOverrunSetStart(st, 2, 8, 25.0, true);
    (void)botHostOverrunNewGame(st, cfg);
}

/* Probes secs seconds of d. Returns the raises. */
static int dynRun(BotHostOverrunState *st, const BotHostOverrunCfg *cfg,
                  const DynSecond *d, int secs) {
    int s, raises = 0;
    for (s = 0; s < secs; s++) {
        BotHostOverrunProbe p;
        int bots = d->full ? st->limit : st->limit - 1;
        if (bots < 0) bots = 0;
        memset(&p, 0, sizeof(p));
        p.thinks = (uint32_t)(50 * bots);
        p.overruns = p.thinks * (uint32_t)d->permille / 1000u;
        p.bots = bots;
        p.slots = 8;
        p.botCpu = (uint8_t)(bots * d->cost > 100 ? 100 : bots * d->cost);
        p.freePct = (uint8_t)d->freePct;
        p.lagMs = d->lagMs;
        p.running = d->running;
        if (botHostOverrunProbe(st, cfg, &p, NULL) == BOT_HOST_OVR_RAISE) {
            raises++;
        }
    }
    return raises;
}

int run_bot_host_dyn_raise_rule(void) {
    BotHostOverrunState *st = (BotHostOverrunState *)calloc(1, sizeof(*st));
    BotHostOverrunCfg cfg;
    DynSecond d;

    UT_ASSERT(st != NULL);
    UT_ASSERT(BOT_HOST_DYN_SLOTS_DEFAULT && BOT_HOST_RAISE_STEP_DEFAULT == 2);
    UT_ASSERT(BOT_HOST_RAISE_RESERVE_PCT_DEFAULT == 25);
    UT_ASSERT(BOT_HOST_CLIENT_RESERVE_PCT_DEFAULT == 25);
    UT_ASSERT(BOT_HOST_RAISE_LAG_MAX_MS_DEFAULT == 200);
    UT_ASSERT(BOT_HOST_RAISE_AFTER_CUT_DEFAULT == 120);
    UT_ASSERT(BOT_HOST_CARRY_LIMIT_DEFAULT && BOT_HOST_CLIENT_START_DEFAULT == 2);

    /* All good: up by 2 after a full window, once each 30 s, to the
     * ceiling and no further. */
    dynSetUp(st, &cfg);
    UT_ASSERT(botHostOverrunDynamic(st, &cfg) && st->limit == 2);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 29) == 0 && st->limit == 2);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 1) == 1 && st->limit == 4);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 29) == 0 && st->limit == 4);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 1) == 1 && st->limit == 6);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 300) == 1 && st->limit == 8);
    UT_ASSERT(botHostOverrunLimit(st, 8) == 8);
    printf("  all good: 2, 4 at 30 s, 6 at 60 s, 8 (the ceiling) after\n");

    /* a. Not full: no raise. */
    dynSetUp(st, &cfg);
    d = dynGood; d.full = false;
    UT_ASSERT(dynRun(st, &cfg, &d, 300) == 0 && st->limit == 2);
    /* The game does not run: no raise. */
    dynSetUp(st, &cfg);
    d = dynGood; d.running = false;
    UT_ASSERT(dynRun(st, &cfg, &d, 300) == 0 && st->limit == 2);
    /* b. Overruns in the window: no raise. */
    dynSetUp(st, &cfg);
    d = dynGood; d.permille = 10;
    UT_ASSERT(dynRun(st, &cfg, &d, 300) == 0 && st->limit == 2);
    /* c. The predicted free CPU under the reserve: 28 - 5 < 25. */
    dynSetUp(st, &cfg);
    d = dynGood; d.freePct = 28;
    UT_ASSERT(dynRun(st, &cfg, &d, 300) == 0 && st->limit == 2);
    /* Room for one bot, not two: 33 - 5 > 25, 33 - 10 < 25. */
    dynSetUp(st, &cfg);
    d = dynGood; d.freePct = 33;
    UT_ASSERT(dynRun(st, &cfg, &d, 30) == 1 && st->limit == 3);
    /* d. Lag above the most, or not known: no raise. */
    dynSetUp(st, &cfg);
    d = dynGood; d.lagMs = 300.0;
    UT_ASSERT(dynRun(st, &cfg, &d, 300) == 0 && st->limit == 2);
    dynSetUp(st, &cfg);
    d = dynGood; d.lagMs = -1.0;
    UT_ASSERT(dynRun(st, &cfg, &d, 300) == 0 && st->limit == 2);
    /* With no lag check (0) the same lag raises. */
    dynSetUp(st, &cfg);
    cfg.raiseLagMaxMs = 0;
    d = dynGood; d.lagMs = 300.0;
    UT_ASSERT(dynRun(st, &cfg, &d, 30) == 1 && st->limit == 4);
    /* e. A step down: no raise for raiseAfterCutSecs. */
    dynSetUp(st, &cfg);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 30) == 1 && st->limit == 4);
    UT_ASSERT(botHostOverrunMovedN(st, 4, 1) == 3);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 119) == 0 && st->limit == 3);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 1) == 1 && st->limit == 5);
    printf("  each of a to e stops the raise; a cut holds it 120 s\n");

    /* A raise step of 1 adds one bot a time. */
    dynSetUp(st, &cfg);
    cfg.raiseStep = 1;
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 30) == 1 && st->limit == 3);
    free(st);
    return 0;
}

/* Probes secs seconds of d, the free CPU dip (dipFree) from second
 * dipFrom (counted from 1) to dipTo. */
static void dynRunDip(BotHostOverrunState *st, const BotHostOverrunCfg *cfg,
                      const DynSecond *d, int secs, int dipFrom, int dipTo,
                      int dipFree) {
    int s;
    for (s = 1; s <= secs; s++) {
        DynSecond one = *d;
        if (s >= dipFrom && s <= dipTo) one.freePct = dipFree;
        (void)dynRun(st, cfg, &one, 1);
    }
}

int run_bot_host_dyn_under_reserve(void) {
    static const BotHostPlaceLag lag = { 100.0, 250.0,
                                         BOT_HOST_LAG_GAIN_DEFAULT };
    BotHostOverrunState *st = (BotHostOverrunState *)calloc(1, sizeof(*st));
    BotHostOverrunCfg cfg;
    BotHostPlaceHost h[3];
    DynSecond d;
    int i, from = -1, to = -1;
    BotHostSpreadWhy why = BOT_HOST_SPREAD_NONE;

    UT_ASSERT(st != NULL);
    UT_ASSERT(BOT_HOST_RESERVE_RECENT_SECS_DEFAULT == 10);
    /* No reading yet: not known, not under. */
    dynSetUp(st, &cfg);
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 2));
    UT_ASSERT(botHostOverrunFits(st, &cfg, 2) == -1);
    /* Free CPU 20, under 25, but the window is not full yet. */
    d = dynGood; d.freePct = 20;
    (void)dynRun(st, &cfg, &d, 29);
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 2));
    /* A full window under it: under, while it has bots. */
    (void)dynRun(st, &cfg, &d, 1);
    UT_ASSERT(botHostOverrunUnderReserve(st, &cfg, 2));
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 0));
    /* The rule does not change the limit by itself. */
    UT_ASSERT(st->limit == 2);
    /* New bots: (20 + 10 - 25) / 5 = 1 fits, fewer than its 2. */
    UT_ASSERT(botHostOverrunFits(st, &cfg, 2) == 1);
    UT_ASSERT(!botHostOverrunMayTake(st, &cfg, 2, 20, NULL));
    /* A bot left: the next move needs a full window with it gone. */
    botHostOverrunGave(st);
    (void)dynRun(st, &cfg, &d, 29);
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 2));
    (void)dynRun(st, &cfg, &d, 1);
    UT_ASSERT(botHostOverrunUnderReserve(st, &cfg, 2));
    /* With no reserve the same place is not under it. */
    botHostOverrunSetStart(st, 2, 8, 0.0, false);
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 2));

    /* At 28 free with a 5 s dip to 0 at 11..15 s: the window average is
     * 23.3, under 25, but the last 10 s are back at 28, so it is not
     * under. */
    dynSetUp(st, &cfg);
    d = dynGood; d.freePct = 28;
    dynRunDip(st, &cfg, &d, 30, 11, 15, 0);
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 2));
    /* With the recent check off the window average alone says under. */
    cfg.reserveRecentSecs = 0;
    UT_ASSERT(botHostOverrunUnderReserve(st, &cfg, 2));
    /* The dip still on in the last 10 s: under. */
    dynSetUp(st, &cfg);
    dynRunDip(st, &cfg, &d, 30, 26, 30, 0);
    UT_ASSERT(botHostOverrunUnderReserve(st, &cfg, 2));
    /* At 28 with no dip: not under. */
    dynSetUp(st, &cfg);
    (void)dynRun(st, &cfg, &d, 30);
    UT_ASSERT(!botHostOverrunUnderReserve(st, &cfg, 2));
    printf("  under for a full window: under; a 5 s dip that ended: not\n");

    /* The move: place 1 (under its reserve) gives a bot only to a place
     * with room. 0 the server at its slots, 2 another host. */
    memset(h, 0, sizeof(h));
    h[0].slots = 4; h[0].bots = 4; h[0].unlimited = true; h[0].lagMs = 0.0;
    h[1].slots = 3; h[1].bots = 3; h[1].lagMs = 20.0; h[1].bad = true;
    h[2].slots = 3; h[2].bots = 3; h[2].lagMs = 20.0;
    for (i = 0; i < 3; i++) {
        h[i].open = true;
        h[i].movable = true;
    }
    /* No room: the bots stay. */
    UT_ASSERT(!botHostPlaceRebalanceKeep(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                         BOT_HOST_SMALL_BIAS_DEFAULT, &lag,
                                         false, &from, &to, &why));
    /* Room on place 2: one bot goes there. */
    h[2].bots = 2;
    UT_ASSERT(botHostPlaceRebalanceKeep(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                        BOT_HOST_SMALL_BIAS_DEFAULT, &lag,
                                        false, &from, &to, &why));
    UT_ASSERT(from == 1 && to == 2 && why == BOT_HOST_SPREAD_BAD);
    /* Held for its step time after that move: no second bot yet. */
    h[1].bots = 2; h[2].bots = 2; h[1].badHold = true;
    UT_ASSERT(!botHostPlaceRebalanceKeep(h, 3, BOT_HOST_SMALL_SLOTS_DEFAULT,
                                         BOT_HOST_SMALL_BIAS_DEFAULT, &lag,
                                         false, &from, &to, &why));
    free(st);
    return 0;
}

int run_bot_host_dyn_carry(void) {
    BotHostOverrunState *st = (BotHostOverrunState *)calloc(1, sizeof(*st));
    BotHostOverrunCfg cfg;

    UT_ASSERT(st != NULL);
    /* Carry on: the next game starts at 4, the limit the last one ended
     * with. */
    dynSetUp(st, &cfg);
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 30) == 1 && st->limit == 4);
    UT_ASSERT(botHostOverrunNewGame(st, &cfg) == BOTHOST_DECISION_LIMIT_CARRY);
    UT_ASSERT(st->limit == 4);
    /* The placement step sets the same start again: the limit stays. */
    botHostOverrunSetStart(st, 2, 8, 25.0, true);
    UT_ASSERT(st->limit == 4);
    /* A lower ceiling takes the limit down with it. */
    botHostOverrunSetStart(st, 2, 3, 25.0, true);
    UT_ASSERT(st->limit == 3);
    /* A new start puts the limit at it. */
    botHostOverrunSetStart(st, 1, 8, 25.0, true);
    UT_ASSERT(st->limit == 1);

    /* Carry off: the next game starts at the start. */
    dynSetUp(st, &cfg);
    cfg.carryLimit = false;
    UT_ASSERT(dynRun(st, &cfg, &dynGood, 30) == 1 && st->limit == 4);
    UT_ASSERT(botHostOverrunNewGame(st, &cfg) == BOTHOST_DECISION_LIMIT_START);
    UT_ASSERT(st->limit == 2);

    /* No start: no live limit, and no decision to log. */
    botHostOverrunReset(st);
    UT_ASSERT(botHostOverrunNewGame(st, &cfg) == 0);
    UT_ASSERT(st->limit == -1);
    printf("  carry: 4 into the next game; no carry: 2\n");
    free(st);
    return 0;
}

int run_bot_host_dyn_keel(void) {
    BotHostOverrunState *st = (BotHostOverrunState *)calloc(1, sizeof(*st));
    BotHostOverrunCfg cfg;
    BotHostDynamicFlags f;

    UT_ASSERT(st != NULL);
    botHostDynamicFlagsInit(&f);
    UT_ASSERT(f.reserveRecentSecs == -1);
    UT_ASSERT(f.dynamic == -1 && f.serverMax == -1 && f.clientStart == -1);
    UT_ASSERT(f.raiseReservePct == -1 && f.clientReservePct == -1);
    UT_ASSERT(f.raiseLagMaxMs == -1 && f.raiseStep == -1);
    UT_ASSERT(f.raiseAfterCutSecs == -1 && f.carryLimit == -1);
    /* The KEEL values of every dynamic slot flag. */
    botHostDynamicFlagsKeel(&f);
    UT_ASSERT(f.dynamic == 0);
    UT_ASSERT(f.serverMax == 0);
    UT_ASSERT(f.clientStart == 0);
    UT_ASSERT(f.raiseReservePct == 0);
    UT_ASSERT(f.clientReservePct == 0);
    UT_ASSERT(f.raiseLagMaxMs == 0);
    UT_ASSERT(f.raiseStep == 1);
    UT_ASSERT(f.raiseAfterCutSecs == 0);
    UT_ASSERT(f.carryLimit == 0);
    UT_ASSERT(f.reserveRecentSecs == 0);

    /* With them a place has no live limit: it counts its slots, as
     * before dynamic slots. */
    dynSetUp(st, &cfg);
    cfg.dynamic = f.dynamic != 0;
    UT_ASSERT(botHostOverrunNewGame(st, &cfg) == 0);
    UT_ASSERT(!botHostOverrunDynamic(st, &cfg));
    UT_ASSERT(st->limit == -1 && botHostOverrunLimit(st, 8) == 8);
    (void)dynRun(st, &cfg, &dynGood, 120);
    UT_ASSERT(st->limit == -1 && botHostOverrunLimit(st, 8) == 8);
    /* The Bot Host program's --keel: its ceiling is --slots, and an older
     * server or this one gets the same slots. */
    UT_ASSERT(botHostMaxOffer(botHostSlotsMaxDefault(0, 4, 12, true), 4,
                              true) == 4);
    UT_ASSERT(botHostMaxOffer(botHostSlotsMaxDefault(0, 4, 12, true), 4,
                              false) == 4);
    free(st);
    return 0;
}

int run_bot_host_dyn_determinism(void) {
    /* All three flags: dynamic slots off, whatever was asked. */
    UT_ASSERT(botHostDynamicForRun(1, true, true, true) == 0);
    UT_ASSERT(botHostDynamicForRun(-1, true, true, true) == 0);
    UT_ASSERT(botHostDynamicForRun(0, true, true, true) == 0);
    /* Any one missing: as asked. */
    UT_ASSERT(botHostDynamicForRun(1, false, true, true) == 1);
    UT_ASSERT(botHostDynamicForRun(1, true, false, true) == 1);
    UT_ASSERT(botHostDynamicForRun(1, true, true, false) == 1);
    UT_ASSERT(botHostDynamicForRun(-1, true, true, false) == -1);
    UT_ASSERT(botHostDynamicForRun(0, false, false, false) == 0);
    return 0;
}

/* ================================================================
 * 11. CTRL_BOT_HOST_LIMITS and _ROW_LIMITS
 * ================================================================ */
int run_bot_host_limits_codec(void) {
    ControlEncodeBodyFn enc =
        transportControlCodecBodyEncoder(CTRL_BOT_HOST_LIMITS);
    ControlDecodeBodyFn dec =
        transportControlCodecBodyDecoder(CTRL_BOT_HOST_LIMITS);
    ControlEncodeBodyFn renc =
        transportControlCodecBodyEncoder(CTRL_BOT_HOST_ROW_LIMITS);
    ControlDecodeBodyFn rdec =
        transportControlCodecBodyDecoder(CTRL_BOT_HOST_ROW_LIMITS);
    ControlEvent in, out;
    uint8_t buf[16];
    size_t len = 0;
    int pct = -1;

    UT_ASSERT(enc != NULL && dec != NULL && renc != NULL && rdec != NULL);
    /* The server's hello: every byte none. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_LIMITS;
    in.u.botHostLimits.start = BOT_HOST_LIMIT_NONE;
    in.u.botHostLimits.ceiling = BOT_HOST_LIMIT_NONE;
    in.u.botHostLimits.reserve = BOT_HOST_LIMIT_NONE;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 3);
    memset(&out, 0xAB, sizeof(out));
    UT_ASSERT(dec(buf, len, &out) && out.type == CTRL_BOT_HOST_LIMITS);
    UT_ASSERT(out.u.botHostLimits.start == BOT_HOST_LIMIT_NONE);
    UT_ASSERT(out.u.botHostLimits.ceiling == BOT_HOST_LIMIT_NONE);
    UT_ASSERT(out.u.botHostLimits.reserve == BOT_HOST_LIMIT_NONE);
    /* The host's answer, with its --cpu-reserve. */
    in.u.botHostLimits.start = 2;
    in.u.botHostLimits.ceiling = 11;
    in.u.botHostLimits.reserve = 30;
    UT_ASSERT(enc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(dec(buf, len, &out));
    UT_ASSERT(out.u.botHostLimits.start == 2);
    UT_ASSERT(out.u.botHostLimits.ceiling == 11);
    UT_ASSERT(out.u.botHostLimits.reserve == 30);
    /* Short, a ceiling above 16, a reserve above 100: refused. A later
     * byte from a newer host is skipped. */
    UT_ASSERT(!dec(buf, 2, &out));
    buf[1] = 17;
    UT_ASSERT(!dec(buf, 3, &out));
    buf[1] = 11; buf[2] = 101;
    UT_ASSERT(!dec(buf, 3, &out));
    buf[2] = 30; buf[3] = 0x7F;
    UT_ASSERT(dec(buf, 4, &out) && out.u.botHostLimits.reserve == 30);
    UT_ASSERT(enc(&in, NULL, buf, 2, &len) != ENCODE_OK);

    /* A lobby row's limits. */
    memset(&in, 0, sizeof(in));
    in.type = CTRL_BOT_HOST_ROW_LIMITS;
    in.u.botHostRowLimits.idx = 1;
    in.u.botHostRowLimits.ceiling = 6;
    in.u.botHostRowLimits.reserve = 25;
    in.u.botHostRowLimits.fits = BOT_HOST_LIMIT_NONE;
    UT_ASSERT(renc(&in, NULL, buf, sizeof(buf), &len) == ENCODE_OK);
    UT_ASSERT(len == 4);
    memset(&out, 0xAB, sizeof(out));
    UT_ASSERT(rdec(buf, len, &out) && out.type == CTRL_BOT_HOST_ROW_LIMITS);
    UT_ASSERT(out.u.botHostRowLimits.idx == 1);
    UT_ASSERT(out.u.botHostRowLimits.ceiling == 6);
    UT_ASSERT(out.u.botHostRowLimits.reserve == 25);
    UT_ASSERT(out.u.botHostRowLimits.fits == BOT_HOST_LIMIT_NONE);
    UT_ASSERT(!rdec(buf, 3, &out));
    buf[1] = 17;
    UT_ASSERT(!rdec(buf, 4, &out));

    /* --cpu-reserve: 0 to 100 only. */
    UT_ASSERT(botHostParsePercent("0", &pct) && pct == 0);
    UT_ASSERT(botHostParsePercent("25", &pct) && pct == 25);
    UT_ASSERT(botHostParsePercent("100", &pct) && pct == 100);
    pct = 7;
    UT_ASSERT(!botHostParsePercent("101", &pct) && pct == 7);
    UT_ASSERT(!botHostParsePercent("-1", &pct));
    UT_ASSERT(!botHostParsePercent("", &pct));
    UT_ASSERT(!botHostParsePercent(NULL, &pct));
    UT_ASSERT(!botHostParsePercent("25%", &pct));
    UT_ASSERT(!botHostParsePercent("x", &pct) && pct == 7);
    return 0;
}

/* ================================================================
 * 12. Only the owner changes a player's row
 * ================================================================ */
int run_bot_host_slot_owner_only(void) {
    /* Player 3's own computer: only player 3. */
    UT_ASSERT(botHostSlotSetAllowed(true, 3, 3));
    UT_ASSERT(!botHostSlotSetAllowed(true, 3, 0));
    UT_ASSERT(!botHostSlotSetAllowed(true, 3, 4));
    /* No owner known: nobody. */
    UT_ASSERT(!botHostSlotSetAllowed(true, -1, -1));
    UT_ASSERT(!botHostSlotSetAllowed(true, -1, 0));
    /* A Bot Host program's row: nobody, the host owner included. */
    UT_ASSERT(!botHostSlotSetAllowed(false, 0, 0));
    UT_ASSERT(!botHostSlotSetAllowed(false, -1, 0));
    return 0;
}
