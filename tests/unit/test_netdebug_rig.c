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

/* Run the scripted hold and return the commanded turn count = the number
 * of turn-carrying tick numbers actually delivered to the server (the
 * withheld ones included — they are delivered late, not dropped).
 *
 * Pacing invariant: every clientSimNetTick is preceded by exactly two new
 * tick numbers (t, t+1). A "withheld" number is one whose send we skip
 * (delivered late or never) — never a pause in the numbering. Once the
 * stream is established the server consumes one tick number per half-step
 * (substituting from the held buttons when starved), so a frame that fails
 * to produce two new numbers would skew the server permanently ahead and
 * make every later input arrive stale. Holding the numbering at exactly
 * two per net tick keeps client and server in lockstep.
 *
 * Why executed == commanded: each held tick number in [startTick, endTick)
 * is executed exactly once — by a real apply when its input is fresh, or
 * by a same-button substitute when the window starved that number — and
 * never twice, because the substitute advances lastProcessedInput so the
 * late arrival drops as stale. With one button held throughout, a
 * substitute produces the same turn the real input would have, so the
 * total turn count is conserved regardless of how the post-window re-fill
 * gate batches a few applies behind substitutes. */
static uint32_t nd_run_script(ClientSim *cs, uint32_t startTick,
                              const NdScript *s) {
    uint32_t commanded = 0;
    uint32_t endTick = startTick + s->holdTicks;  /* exclusive */
    uint8_t  turn = (uint8_t)NETDEBUG_TURN_BTN;
    uint32_t wStart = s->withholdStart;
    uint32_t wEnd   = (s->withholdLen != 0) ? wStart + s->withholdLen : 0;
    bool     lateDone = (s->withholdLen == 0);
    uint32_t t;

    /* Hold: one frame owns the tick pair (t, t+1). Each number is sent
     * fresh unless it lies in the withhold window [wStart, wEnd). */
    for (t = startTick; t < endTick; t += 2) {
        uint32_t a = t, b = t + 1;

        /* Once we are past the window, deliver the withheld numbers as a
         * late burst right before this frame's net tick (no extra net
         * ticks for them). The window already substituted past these
         * numbers, so they arrive stale: movement drops, the mine on
         * mineTick is harvested onto the next real game input. */
        if (!lateDone && t >= wEnd) {
            uint32_t w;
            for (w = wStart; w < wEnd; w++) {
                uint8_t act = (s->mineTick != 0 && w == s->mineTick)
                                  ? (uint8_t)INPUT_ACTION_LAY_MINE : 0;
                nd_send(cs, w, turn, act);
                commanded++;  /* withheld-then-late turn tick */
            }
            lateDone = true;
        }

        if (s->withholdLen == 0 || a < wStart || a >= wEnd) {
            nd_send(cs, a, turn, 0);
            commanded++;
        }
        if (b < endTick && (s->withholdLen == 0 || b < wStart || b >= wEnd)) {
            nd_send(cs, b, turn, 0);
            commanded++;
        }
        clientSimNetTick(cs);
    }

    /* If the window butted right up against endTick we may exit the loop
     * before any post-window frame ran the late burst — flush it now. */
    if (!lateDone) {
        uint32_t w;
        for (w = wStart; w < wEnd; w++) {
            uint8_t act = (s->mineTick != 0 && w == s->mineTick)
                              ? (uint8_t)INPUT_ACTION_LAY_MINE : 0;
            nd_send(cs, w, turn, act);
            commanded++;
        }
        lateDone = true;
    }

    /* Release: TNONE pairs, still two per net tick so they apply fresh and
     * clear lastInputButtons. After them, bare drain spins substitute the
     * now-TNONE buttons — no turn counts. */
    for (t = endTick; t < endTick + 8; t += 2) {
        nd_send(cs, t, 0, 0);
        nd_send(cs, t + 1, 0, 0);
        clientSimNetTick(cs);
    }
    for (t = 0; t < 32; t++) {
        clientSimNetTick(cs);
    }
    return commanded;
}

/* Warmup: TNONE two per net tick so the server consumes exactly what we
 * send (zero skew) and ends established — jitter buffer filled,
 * lastProcessedInput advanced past 0 — before the caller resets the
 * netdebug counters. Returns the next unused input tick. */
static uint32_t nd_warmup(ClientSim *cs) {
    uint32_t t = 1;
    while (t <= NETDEBUG_WARMUP_TICKS) {  /* even count → whole pairs */
        nd_send(cs, t, 0, 0);
        nd_send(cs, t + 1, 0, 0);
        clientSimNetTick(cs);
        t += 2;
    }
    return t;  /* NETDEBUG_WARMUP_TICKS + 1 */
}

/* Odd-aligned mid-hold window start (startTick is odd, +even keeps it odd)
 * so the window covers whole (t, t+1) frames cleanly. Seeded draw keeps
 * placement reproducible. */
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

    /* Stall-advance contract: the withhold-window stall substitutes each
     * covered tick with the held turn AND advances lastProcessedInput past
     * it, so the late-delivered inputs for those ticks arrive stale and
     * drop instead of executing the turn a second time. The script holds
     * one turn button throughout, so a substituted tick executes the same
     * turn the late-dropped tick would have — exact equality for this
     * script (no release falls inside the window). */
    fprintf(stderr,
            "  netdebug overshoot: commanded=%u executed=%u delta=%d\n",
            commanded, executed, (int)executed - (int)commanded);
    UT_ASSERT_MSG(executed == commanded,
                  "stall-advance: executed %u != commanded %u",
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
