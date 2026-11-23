/*
 * Copyright (c) 1998-2026 John Morrison.
 * SPDX-License-Identifier: GPL-3.0-or-later
 */

/*
 * The lobby's wait for a Bot Host's brain files. A player's host showed
 * "syncing 0%" and stayed there, and the lobby's Ready waited for it.
 *
 *   1. bot_host_sync_no_bots: with no bot in the game no host needs the
 *      files. Its row is not "syncing", so Ready never waits for it.
 *   2. bot_host_sync_zero_slots: a host that offers 0 slots never needs
 *      the files and never holds Ready. While nothing needs them its
 *      clocks do not run: the limits count from when they are needed.
 *   3. bot_host_sync_stall: a host whose downloaded bytes do not go up for
 *      BOT_HOST_SYNC_STALL_TICKS is late at that tick, and its row stops
 *      holding Ready. A host whose bytes go up keeps syncing until the
 *      60 s limit. A host that has not reported yet does not stall.
 *   4. bot_host_sync_new_gen: a new manifest generation starts the stall
 *      clock again. A host that is late for this game stays late after a
 *      new generation, so Ready does not wait for it again.
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

/* Host row idx in the client's lobby with the given sync state, then
 * whether the lobby's Ready waits on a host (clientSimBotHostSyncing). */
static bool syncRowHoldsReady(ClientSim *cs, uint8_t idx, int state,
                              uint8_t percent) {
    ControlEvent evt;
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_BOT_HOST_STATUS;
    evt.u.botHostStatus.idx = idx;
    evt.u.botHostStatus.connected = 1;
    evt.u.botHostStatus.state = (uint8_t)state;
    evt.u.botHostStatus.percent = percent;
    evt.u.botHostStatus.slots = 4;
    SDL_strlcpy(evt.u.botHostStatus.name, "Elvis",
                sizeof(evt.u.botHostStatus.name));
    clientSimApplyControl(cs, &evt);
    return clientSimBotHostSyncing(cs, NULL);
}

/* A host part way through the current generation: reported, not synced,
 * not late. */
static BotHostSyncRowIn syncRowDownloading(bool needed) {
    BotHostSyncRowIn in;
    memset(&in, 0, sizeof(in));
    in.needed = needed;
    in.reported = true;
    return in;
}

/* ================================================================
 * 1. No bot in the game
 * ================================================================ */
int run_bot_host_sync_no_bots(void) {
    ClientSim *cs = clientSimAlloc();
    BotHostSyncRowIn in;
    int st;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    UT_ASSERT(!botHostSyncNeeded(0, 4));
    UT_ASSERT(!botHostSyncNeeded(0, 16));
    UT_ASSERT(botHostSyncNeeded(1, 4));

    /* The case of the lobby: Elvis at 0% with no bot in the game. */
    in = syncRowDownloading(botHostSyncNeeded(0, 4));
    st = botHostSyncRowState(&in);
    UT_ASSERT_MSG(st == BOT_HOST_SYNC_WAITING, "state %d", st);
    UT_ASSERT(!syncRowHoldsReady(cs, 1, st, 0));

    /* Not reported yet, late, synced: none holds Ready either. */
    in.reported = false;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_WAITING);
    in.reported = true;
    in.late = true;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_LATE);
    UT_ASSERT(!syncRowHoldsReady(cs, 1, botHostSyncRowState(&in), 0));
    in.late = false;
    in.synced = true;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_READY);

    /* A bot is added: the same host now holds Ready while it syncs. */
    in = syncRowDownloading(botHostSyncNeeded(1, 4));
    st = botHostSyncRowState(&in);
    UT_ASSERT(st == BOT_HOST_SYNC_SYNCING);
    UT_ASSERT(syncRowHoldsReady(cs, 1, st, 0));
    /* The bot goes again: Ready is free at once. */
    in.needed = botHostSyncNeeded(0, 4);
    UT_ASSERT(!syncRowHoldsReady(cs, 1, botHostSyncRowState(&in), 0));

    /* A host on the server's computer is never "syncing". */
    in = syncRowDownloading(true);
    in.sameServer = true;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_WAITING);
    UT_ASSERT(botHostSyncRowState(NULL) == BOT_HOST_SYNC_WAITING);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 2. A host that offers 0 slots
 * ================================================================ */
int run_bot_host_sync_zero_slots(void) {
    ClientSim *cs = clientSimAlloc();
    BotHostSyncRowIn in;
    BotHostSyncWatch w;
    uint32_t t;
    int st;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);

    /* Max slots 0 with 6 bots in the game. */
    UT_ASSERT(!botHostSyncNeeded(6, 0));
    UT_ASSERT(!botHostSyncNeeded(6, -1));
    in = syncRowDownloading(botHostSyncNeeded(6, 0));
    st = botHostSyncRowState(&in);
    UT_ASSERT_MSG(st == BOT_HOST_SYNC_WAITING, "state %d", st);
    UT_ASSERT(!syncRowHoldsReady(cs, 2, st, 0));
    /* A second host that offers slots still holds Ready while it syncs,
     * and the 0-slot host adds nothing to the wait. */
    in = syncRowDownloading(botHostSyncNeeded(6, 4));
    UT_ASSERT(syncRowHoldsReady(cs, 3, botHostSyncRowState(&in), 40));
    {
        int pct = -1;
        UT_ASSERT(clientSimBotHostSyncing(cs, &pct));
        UT_ASSERT_MSG(pct == 40, "percent %d", pct);
    }

    /* Its clocks: the server holds them each tick while nothing needs the
     * files. Ten minutes at 0 slots, with no progress, is no stall and
     * no time limit. */
    memset(&w, 0, sizeof(w));
    botHostSyncWatchStart(&w, 7, 1000);
    botHostSyncWatchReport(&w, 7, 0, 1010);
    for (t = 1010; t <= 1010 + 30000; t++) botHostSyncWatchHold(&w, 0, t);
    UT_ASSERT(botHostSyncWatchCheck(&w, 31010) == BOT_HOST_SYNC_WAIT_ON);
    /* The player sets Max slots 2 at tick 31010: the clocks count from
     * there, so the stall comes 10 s later and not at once. */
    UT_ASSERT(botHostSyncWatchCheck(&w, 31010 + BOT_HOST_SYNC_STALL_TICKS - 1)
              == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&w, 31010 + BOT_HOST_SYNC_STALL_TICKS)
              == BOT_HOST_SYNC_WAIT_STALLED);
    /* The hold keeps the generation and the report. */
    UT_ASSERT(w.gen == 7 && w.reported);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 3. No progress for 10 s
 * ================================================================ */
int run_bot_host_sync_stall(void) {
    ClientSim *cs = clientSimAlloc();
    BotHostSyncWatch stuck, moving, silent;
    BotHostSyncRowIn in;
    const uint32_t t0 = 5000;
    uint32_t t, done = 0;
    uint32_t lateAt = 0;

    UT_ASSERT(cs != NULL);
    clientSimCreate(cs);
    UT_ASSERT(BOT_HOST_SYNC_STALL_TICKS == 500);
    UT_ASSERT(BOT_HOST_SYNC_LIMIT_TICKS == 3000);

    botHostSyncWatchStart(&stuck, 3, t0);
    botHostSyncWatchStart(&moving, 3, t0);
    botHostSyncWatchStart(&silent, 3, t0);
    /* Both report 0 bytes at t0 + 20. */
    botHostSyncWatchReport(&stuck, 3, 0, t0 + 20);
    botHostSyncWatchReport(&moving, 3, 0, t0 + 20);

    /* Each tick, as the server's loop: stuck reports 0 again now and
     * then, moving gets 1000 bytes more each 100 ticks. */
    for (t = t0 + 21; t <= t0 + 20 + BOT_HOST_SYNC_STALL_TICKS; t++) {
        if ((t - t0) % 50 == 0) botHostSyncWatchReport(&stuck, 3, 0, t);
        if ((t - t0) % 100 == 0) {
            done += 1000;
            botHostSyncWatchReport(&moving, 3, done, t);
        }
        if (lateAt == 0 &&
            botHostSyncWatchCheck(&stuck, t) == BOT_HOST_SYNC_WAIT_STALLED) {
            lateAt = t;
        }
        UT_ASSERT_MSG(botHostSyncWatchCheck(&moving, t) ==
                          BOT_HOST_SYNC_WAIT_ON, "moving at %u",
                      (unsigned)t);
    }
    printf("  stuck at 0 bytes: late at tick +%u from its first report\n",
           (unsigned)(lateAt - (t0 + 20)));
    UT_ASSERT_MSG(lateAt == t0 + 20 + BOT_HOST_SYNC_STALL_TICKS,
                  "late at %u", (unsigned)lateAt);

    /* The stuck host is late: its row stops holding Ready. The moving
     * host still holds it. */
    in = syncRowDownloading(true);
    UT_ASSERT(syncRowHoldsReady(cs, 1, botHostSyncRowState(&in), 0));
    UT_ASSERT(syncRowHoldsReady(cs, 2, botHostSyncRowState(&in), 30));
    in.late = true;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_LATE);
    (void)syncRowHoldsReady(cs, 1, botHostSyncRowState(&in), 0);
    {
        int pct = -1;
        UT_ASSERT(clientSimBotHostSyncing(cs, &pct));
        UT_ASSERT_MSG(pct == 30, "percent %d", pct);
    }
    in.late = false;
    in.synced = true;
    UT_ASSERT(!syncRowHoldsReady(cs, 2, botHostSyncRowState(&in), 100));

    /* A download that starts again from 0 is no progress; the bytes after
     * it are. */
    {
        BotHostSyncWatch w;
        botHostSyncWatchStart(&w, 4, 100);
        botHostSyncWatchReport(&w, 4, 8000, 110);
        botHostSyncWatchReport(&w, 4, 0, 400);
        UT_ASSERT(botHostSyncWatchCheck(&w, 110 + BOT_HOST_SYNC_STALL_TICKS)
                  == BOT_HOST_SYNC_WAIT_STALLED);
        botHostSyncWatchStart(&w, 4, 100);
        botHostSyncWatchReport(&w, 4, 8000, 110);
        botHostSyncWatchReport(&w, 4, 0, 400);
        botHostSyncWatchReport(&w, 4, 500, 450);
        UT_ASSERT(botHostSyncWatchCheck(&w, 110 + BOT_HOST_SYNC_STALL_TICKS)
                  == BOT_HOST_SYNC_WAIT_ON);
        UT_ASSERT(botHostSyncWatchCheck(&w, 450 + BOT_HOST_SYNC_STALL_TICKS)
                  == BOT_HOST_SYNC_WAIT_STALLED);
    }

    /* The 60 s limit stays: a host that keeps moving is late past it. */
    for (; t <= t0 + BOT_HOST_SYNC_LIMIT_TICKS; t++) {
        if ((t - t0) % 100 == 0) {
            done += 1000;
            botHostSyncWatchReport(&moving, 3, done, t);
        }
        UT_ASSERT(botHostSyncWatchCheck(&moving, t) == BOT_HOST_SYNC_WAIT_ON);
    }
    UT_ASSERT(botHostSyncWatchCheck(&moving, t0 + BOT_HOST_SYNC_LIMIT_TICKS + 1)
              == BOT_HOST_SYNC_WAIT_LATE);

    /* A host that has not reported on the generation is not "syncing" and
     * does not stall; the 60 s limit still applies to it. */
    UT_ASSERT(botHostSyncWatchCheck(&silent, t0 + BOT_HOST_SYNC_LIMIT_TICKS)
              == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&silent,
                                    t0 + BOT_HOST_SYNC_LIMIT_TICKS + 1)
              == BOT_HOST_SYNC_WAIT_LATE);
    /* No watch: nothing to stop. */
    UT_ASSERT(botHostSyncWatchCheck(NULL, t0) == BOT_HOST_SYNC_WAIT_ON);

    clientSimDestroy(cs);
    return 0;
}

/* ================================================================
 * 4. A new generation
 * ================================================================ */
int run_bot_host_sync_new_gen(void) {
    BotHostSyncWatch w;
    BotHostSyncRowIn in;
    const uint32_t t0 = 2000;
    uint32_t t1;

    botHostSyncWatchStart(&w, 5, t0);
    botHostSyncWatchReport(&w, 5, 0, t0 + 10);
    /* 400 ticks with no progress, then the brain files change. */
    t1 = t0 + 410;
    UT_ASSERT(botHostSyncWatchCheck(&w, t1) == BOT_HOST_SYNC_WAIT_ON);
    botHostSyncWatchStart(&w, 6, t1);
    UT_ASSERT(w.gen == 6 && !w.reported && w.lastDone == 0);
    /* The old stall clock is gone: no stall 500 ticks after the old
     * report. */
    UT_ASSERT(botHostSyncWatchCheck(&w, t0 + 10 + BOT_HOST_SYNC_STALL_TICKS)
              == BOT_HOST_SYNC_WAIT_ON);
    /* The first report on generation 6 starts the stall clock. */
    botHostSyncWatchReport(&w, 6, 0, t1 + 30);
    UT_ASSERT(botHostSyncWatchCheck(&w,
                                    t1 + 30 + BOT_HOST_SYNC_STALL_TICKS - 1)
              == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&w, t1 + 30 + BOT_HOST_SYNC_STALL_TICKS)
              == BOT_HOST_SYNC_WAIT_STALLED);
    /* And the 60 s limit counts from the new list. */
    botHostSyncWatchReport(&w, 6, 100, t1 + 2990);
    UT_ASSERT(botHostSyncWatchCheck(&w, t1 + BOT_HOST_SYNC_LIMIT_TICKS)
              == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&w, t1 + BOT_HOST_SYNC_LIMIT_TICKS + 1)
              == BOT_HOST_SYNC_WAIT_LATE);

    /* A report on a generation the watch does not know starts it too. */
    botHostSyncWatchStart(&w, 6, 100);
    botHostSyncWatchReport(&w, 6, 0, 110);
    botHostSyncWatchReport(&w, 7, 0, 590);
    UT_ASSERT(w.gen == 7 && w.reported && w.startTick == 590);
    UT_ASSERT(botHostSyncWatchCheck(&w, 610) == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&w, 590 + BOT_HOST_SYNC_STALL_TICKS)
              == BOT_HOST_SYNC_WAIT_STALLED);

    /* A host late for this game, then a new generation: the server keeps
     * it out of the game (late), so its row does not go back to
     * "syncing" and Ready does not wait for it again. */
    in = syncRowDownloading(true);
    in.late = true;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_LATE);
    in.reported = false;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_LATE);
    /* It finishes the new files: ready (it still sits out the game). */
    in.synced = true;
    UT_ASSERT(botHostSyncRowState(&in) == BOT_HOST_SYNC_READY);
    return 0;
}
