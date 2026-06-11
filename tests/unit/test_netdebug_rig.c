/*
 * Deterministic server-side input repro rig (WB_NETDEBUG only).
 *
 * Drives ServerSim + ClientSim over the active local transport
 * (clientSimConnectLocal — clientSimNetTick advances the server) with
 * inputs scripted as explicit InputPackets, no sockets and no wall
 * clock. Measures the WB_NETDEBUG sim-executed counters
 * (serverSimNetdebugGet*) to:
 *
 *   1. prove commanded == executed on a clean input stream,
 *   2. reproduce executed > commanded under scripted input loss
 *      (today's stall-repeat + late-apply double-execute), and
 *   3. pin a single commanded mine lay executing exactly once under
 *      the same loss shape.
 *
 * The whole file is gated on WB_NETDEBUG; an unconditional build is an
 * empty translation unit rather than a link error.
 */

#ifdef WB_NETDEBUG

#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* serverSimGetGameSim feeds GameSim below */
#include "game_sim.h"              /* GameSim.tanks[] — raise the mine stock */
#include "tank.h"                  /* tankSetMines */
#include "client_sim.h"
#include "client_net.h"
#include "input_packet.h"
#include "bolo_rand.h"
#include "test_harness.h"

/* Fixed seed so the withhold-window placement is reproducible. */
#define NETDEBUG_SEED          0x5eed1234u
#define NETDEBUG_WARMUP_TICKS  4
#define NETDEBUG_PLAYER        0
#define NETDEBUG_HOLD_TICKS    100  /* even: feeds as whole pairs */
#define NETDEBUG_WITHHOLD_LEN  6

/* Left turn: translateInputToTankButton maps INPUT_BTN_LEFT to TLEFT,
 * which the server's netdebugButtonTurns() counts as a turn half-step. */
#define NETDEBUG_TURN_BTN      INPUT_BTN_LEFT
#define ND_BUTTONS_TURN(b)     (((b) & (INPUT_BTN_LEFT | INPUT_BTN_RIGHT)) != 0)

typedef struct {
    uint32_t holdTicks;      /* consecutive turn ticks, starting at startTick */
    uint32_t withholdStart;  /* first tick withheld then delivered late (0 = none) */
    uint32_t withholdLen;    /* length of the withhold window */
    uint32_t mineTick;       /* withheld tick also carrying LAY_MINE (0 = none) */
} NdScript;

/* Send one input tick. */
static void nd_send(ClientSim *cs, uint32_t tick, uint8_t buttons,
                    uint8_t actions) {
    InputPacket pkt;
    memset(&pkt, 0, sizeof(pkt));
    pkt.tick      = tick;
    pkt.playerNum = NETDEBUG_PLAYER;
    pkt.buttons   = buttons;
    pkt.actions   = actions;
    clientSimNetSendInput(cs, &pkt);
}

/* Feed the contiguous tick range [from, to) two inputs per
 * clientSimNetTick. The server runs two half-steps per tick, so feeding
 * two keeps a small backlog and the queue never stalls on a clean
 * stream. Ranges must be even-length or the trailing single-feed tick
 * leaves a half-step to stall. Returns the number of turn-carrying ticks
 * sent. */
static uint32_t nd_feed_pairs(ClientSim *cs, uint32_t from, uint32_t to,
                              uint8_t buttons) {
    uint32_t turns = 0;
    uint32_t t = from;
    while (t < to) {
        nd_send(cs, t, buttons, 0);
        if (ND_BUTTONS_TURN(buttons)) turns++;
        t++;
        if (t < to) {
            nd_send(cs, t, buttons, 0);
            if (ND_BUTTONS_TURN(buttons)) turns++;
            t++;
        }
        clientSimNetTick(cs);
    }
    return turns;
}

/* Run the scripted hold and return the commanded turn count = number of
 * scripted turn ticks delivered (withheld ticks count: they are
 * delivered late, not dropped). */
static uint32_t nd_run_script(ClientSim *cs, uint32_t startTick,
                              const NdScript *s) {
    uint32_t commanded = 0;
    uint32_t endTick = startTick + s->holdTicks;  /* exclusive */
    uint8_t  turn = (uint8_t)NETDEBUG_TURN_BTN;
    uint32_t t;

    if (s->withholdLen == 0) {
        commanded += nd_feed_pairs(cs, startTick, endTick, turn);
    } else {
        uint32_t wStart = s->withholdStart;
        uint32_t wEnd   = wStart + s->withholdLen;

        /* Pre-window: clean pairs up to the window. */
        commanded += nd_feed_pairs(cs, startTick, wStart, turn);

        /* Window: withhold these ticks (send nothing) and tick the
         * client empty. The queue drains its small backlog and then the
         * stall branch repeats the held turn through the window —
         * lastProcessedInput freezes because no fresh input arrives. */
        for (t = 0; t < s->withholdLen; t++) {
            clientSimNetTick(cs);
        }

        /* Late delivery: burst the withheld ticks. Their tick numbers
         * are still > the frozen lastProcessedInput, so the dequeue
         * applies them rather than dropping them as stale. */
        for (t = wStart; t < wEnd; t++) {
            uint8_t act = (s->mineTick != 0 && t == s->mineTick)
                              ? (uint8_t)INPUT_ACTION_LAY_MINE
                              : 0;
            nd_send(cs, t, turn, act);
            commanded++;
        }
        for (t = 0; t < (s->withholdLen / 2) + 2; t++) {
            clientSimNetTick(cs);
        }

        /* Post-window: resume clean pairs, contiguous with wEnd. */
        commanded += nd_feed_pairs(cs, wEnd, endTick, turn);
    }

    /* Release + tail: TNONE inputs so the held buttons stop turning,
     * then spin to drain. Drain-time stalls now repeat TNONE and do not
     * count as turns. */
    nd_feed_pairs(cs, endTick, endTick + 8, 0);
    for (t = 0; t < 32; t++) {
        clientSimNetTick(cs);
    }
    return commanded;
}

/* Warmup mirrors test_active_local_input_to_shot: one TNONE input per
 * net-tick so the jitter buffer fills and lastProcessedInput advances
 * past 0 before the caller resets the netdebug counters. Returns the
 * next unused input tick. */
static uint32_t nd_warmup(ClientSim *cs) {
    uint32_t t;
    for (t = 1; t <= NETDEBUG_WARMUP_TICKS; t++) {
        nd_send(cs, t, 0, 0);
        clientSimNetTick(cs);
    }
    return t;  /* NETDEBUG_WARMUP_TICKS + 1 */
}

/* Odd-aligned mid-hold window start so every pair-fed sub-range stays
 * even-length. Seeded draw keeps placement reproducible. */
static uint32_t nd_pick_withhold_start(uint32_t startTick) {
    bolo_srand(NETDEBUG_SEED);
    return startTick + 36u + 2u * bolo_rand_below(10);
}

static ClientSim *nd_connect(ServerSim *sim, const char *name) {
    ClientSim *cs = clientSimAlloc();
    if (cs == NULL) {
        return NULL;
    }
    clientSimCreate(cs);
    if (!clientSimConnectLocal(cs, sim, name, "", 0, 0)) {
        clientSimDestroy(cs);
        return NULL;
    }
    return cs;
}

int run_netdebug_commanded_vs_executed(void) {
    ServerSim *sim = ut_make_running_sim("Turner");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    ClientSim *cs = nd_connect(sim, "Turner");
    UT_ASSERT(cs != NULL);

    uint32_t startTick = nd_warmup(cs);
    serverSimNetdebugResetCounters(sim);

    NdScript s;
    memset(&s, 0, sizeof(s));
    s.holdTicks = NETDEBUG_HOLD_TICKS;
    uint32_t commanded = nd_run_script(cs, startTick, &s);

    uint32_t executed = serverSimNetdebugGetExecTurnTicks(sim, NETDEBUG_PLAYER);
    fprintf(stderr, "  netdebug clean: commanded=%u executed=%u\n",
            commanded, executed);
    UT_ASSERT_MSG(executed == commanded,
                  "clean stream: executed %u != commanded %u",
                  executed, commanded);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

int run_netdebug_overshoot_under_loss(void) {
    ServerSim *sim = ut_make_running_sim("Turner");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    ClientSim *cs = nd_connect(sim, "Turner");
    UT_ASSERT(cs != NULL);

    uint32_t startTick = nd_warmup(cs);
    serverSimNetdebugResetCounters(sim);

    NdScript s;
    memset(&s, 0, sizeof(s));
    s.holdTicks     = NETDEBUG_HOLD_TICKS;
    s.withholdLen   = NETDEBUG_WITHHOLD_LEN;
    s.withholdStart = nd_pick_withhold_start(startTick);
    uint32_t commanded = nd_run_script(cs, startTick, &s);

    uint32_t executed = serverSimNetdebugGetExecTurnTicks(sim, NETDEBUG_PLAYER);

    /* Today the stall branch repeats the held turn through the withhold
     * window AND the late inputs are applied when they finally arrive,
     * so those ticks execute twice — executed exceeds commanded. When
     * the stall-advance change lands (the stall stops re-running held
     * buttons), this gate inverts to equality (± the ticks where a
     * release falls inside a stall window): whoever lands that change
     * must flip this assertion to ==, not delete it. */
    fprintf(stderr,
            "  netdebug overshoot: commanded=%u executed=%u delta=%d\n",
            commanded, executed, (int)executed - (int)commanded);
    UT_ASSERT_MSG(executed > commanded,
                  "expected overshoot: executed %u <= commanded %u",
                  executed, commanded);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

int run_netdebug_mine_once_under_loss(void) {
    ServerSim *sim = ut_make_running_sim("Miner");
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    ClientSim *cs = nd_connect(sim, "Miner");
    UT_ASSERT(cs != NULL);

    /* Give the tank a known mine stock so the commanded lay actually
     * fires. The counter sits in the same branch as tankLayMine, so it
     * would read 1 even at zero mines, but a real lay keeps the test
     * honest. Reached through the unittests-profile internal access
     * (serverSimGetGameSim -> GameSim.tanks[], tankSetMines). */
    GameSim *gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    UT_ASSERT(gs->tanks[NETDEBUG_PLAYER] != NULL);
    tankSetMines(&gs->tanks[NETDEBUG_PLAYER], 40);

    uint32_t startTick = nd_warmup(cs);
    serverSimNetdebugResetCounters(sim);

    NdScript s;
    memset(&s, 0, sizeof(s));
    s.holdTicks     = NETDEBUG_HOLD_TICKS;
    s.withholdLen   = NETDEBUG_WITHHOLD_LEN;
    s.withholdStart = nd_pick_withhold_start(startTick);
    /* Even tick → game-arm apply, where the LAY_MINE action is acted on.
     * Inside the withhold window, so it is delivered late after the
     * stall window covering its tick. */
    s.mineTick      = s.withholdStart + 1;
    (void)nd_run_script(cs, startTick, &s);

    uint32_t mines = serverSimNetdebugGetMineLays(sim, NETDEBUG_PLAYER);
    fprintf(stderr, "  netdebug mine-once: lays=%u\n", mines);
    UT_ASSERT_MSG(mines == 1, "expected exactly one mine lay, got %u", mines);

    clientSimDestroy(cs);
    serverSimDestroy(sim);
    return 0;
}

#else /* !WB_NETDEBUG — empty TU so an accidental build links cleanly. */
typedef int netdebug_rig_translation_unit_not_empty;
#endif /* WB_NETDEBUG */
