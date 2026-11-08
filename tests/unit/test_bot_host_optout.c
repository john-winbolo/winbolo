/*
 * The lobby's one-game opt-out from running the server's bots on this
 * computer ("Don't run bots on this computer for this game").
 *
 *   1. bot_host_optout_slots_codec: the opt-out is an existing message, a
 *      CTRL_BOT_HOST_SLOTS with 0 slots and no refusal. It keeps its
 *      fields through the wire form and its body has the same length as
 *      before, so an older server reads it as a host with no room.
 *   2. bot_host_optout_give_back: the server gives back every seat of a
 *      host that reports 0 slots (botHostSeatsToGiveBack) and no seat of a
 *      host that still has slots. Then, with the placement, the bots it
 *      gave back go to the server and to the other host, the 0-slot host
 *      gets no bot while it has 0 slots, and it takes bots again when it
 *      offers its slots again.
 *   3. bot_host_optout_client_reset: the client's opt-out state is on only
 *      for the game (ClientSim and ticket) it was turned on in. A new
 *      ticket (a new connection), another ClientSim or no game turns it
 *      off, and it stays off; turning it off and on again in the same game
 *      works.
 *   4. bot_host_same_server_codec: CTRL_BOT_HOST_SAME_SERVER is one byte,
 *      the kind; an empty body is refused; later bytes are skipped.
 *   5. bot_host_same_server_stop: the server tells a player's host on its
 *      own computer once a connection, and never a listed host there or a
 *      host that shares another computer. The client's built-in host
 *      stops on it (0 slots), only for the server's kind; a new join runs
 *      it again; the opt-out is not changed.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "bot_host_link.h"
#include "bot_host_machine.h"
#include "control_event.h"
#include "transport_control_codec.h"
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

    /* The opt-out: 0 slots, no seat turned down. */
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

    /* Turned off again: the host offers its slots again. */
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

    /* Host A opts out: 0 slots, and every bot on it comes back to the
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

    /* Host A turns the opt-out off: it offers 3 slots again and the
     * rebalance moves bots back onto it. */
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
 * 3. The client's opt-out state
 * ================================================================ */
int run_bot_host_optout_client_reset(void) {
    BotHostOptOut o;
    int gameA = 0, gameB = 0;           /* two ClientSims, by address */
    uint8_t t1[BOT_HOST_OPTOUT_TICKET_LEN], t2[BOT_HOST_OPTOUT_TICKET_LEN];

    memset(t1, 0x11, sizeof(t1));
    memset(t2, 0x11, sizeof(t2));
    t2[15] = 0x12;                      /* differs in the last byte only */

    /* Off at the start, for every game. */
    botHostOptOutInit(&o);
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t1));

    /* On in game A: on while the game and the ticket are the same. */
    botHostOptOutSet(&o, true, &gameA, t1);
    UT_ASSERT(botHostOptOutCheck(&o, &gameA, t1));
    UT_ASSERT(botHostOptOutCheck(&o, &gameA, t1));

    /* Turned off and on again in the same game: the slots go back, then
     * away again. */
    botHostOptOutSet(&o, false, &gameA, t1);
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t1));
    botHostOptOutSet(&o, true, &gameA, t1);
    UT_ASSERT(botHostOptOutCheck(&o, &gameA, t1));

    /* A new ticket (a rejoin, or the next round of a rotation): off, and
     * it stays off when the old ticket shows again. */
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t2));
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t1));
    UT_ASSERT(!o.on);

    /* Another ClientSim: off. */
    botHostOptOutSet(&o, true, &gameA, t1);
    UT_ASSERT(!botHostOptOutCheck(&o, &gameB, t1));
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t1));

    /* No game (the menu) or no ticket: off. */
    botHostOptOutSet(&o, true, &gameA, t1);
    UT_ASSERT(!botHostOptOutCheck(&o, NULL, t1));
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t1));
    botHostOptOutSet(&o, true, &gameA, t1);
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, NULL));
    UT_ASSERT(!botHostOptOutCheck(&o, &gameA, t1));

    /* On with no game or no ticket does not turn on. */
    botHostOptOutSet(&o, true, NULL, t1);
    UT_ASSERT(!o.on);
    botHostOptOutSet(&o, true, &gameA, NULL);
    UT_ASSERT(!o.on);

    /* A NULL state is safe. */
    botHostOptOutInit(NULL);
    botHostOptOutSet(NULL, true, &gameA, t1);
    UT_ASSERT(!botHostOptOutCheck(NULL, &gameA, t1));
    return 0;
}

/* ================================================================
 * 4. CTRL_BOT_HOST_SAME_SERVER on the wire
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
 * 5. The server tells; the client's host stops for this join
 * ================================================================ */
int run_bot_host_same_server_stop(void) {
    BotHostMachineIn in[4];
    BotHostMachineOut out[4];
    bool known[4] = { false, false, false, false };
    BotHostRun run;
    BotHostOptOut opt;
    uint8_t ticket[BOT_HOST_OPTOUT_TICKET_LEN];
    int game = 0, i;

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
    /* The player's opt-out is its own: the stop does not change it. */
    memset(ticket, 0x5A, sizeof(ticket));
    botHostOptOutInit(&opt);
    UT_ASSERT(!botHostOptOutCheck(&opt, &game, ticket));
    botHostOptOutSet(&opt, true, &game, ticket);
    UT_ASSERT(botHostRunStopped(&run));
    UT_ASSERT(botHostOptOutCheck(&opt, &game, ticket));
    /* The next join: it runs as normal again. */
    botHostRunJoin(&run);
    UT_ASSERT(!botHostRunStopped(&run) && botHostRunSlots(&run, 4) == 4);
    return 0;
}
