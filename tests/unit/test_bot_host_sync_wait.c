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
 *      clocks pause: that time does not count to the limits.
 *   3. bot_host_sync_stall: a host whose downloaded bytes do not go up for
 *      BOT_HOST_SYNC_STALL_TICKS is late at that tick, and its row stops
 *      holding Ready. A host whose bytes go up keeps syncing until the
 *      60 s limit. A host that has not reported yet does not stall.
 *   4. bot_host_sync_new_gen: a new manifest generation starts the stall
 *      clock again, and a report on another generation is ignored. A host
 *      that is late for this game stays late after a new generation, so
 *      Ready does not wait for it again.
 *   5. bot_host_sync_packed_big_file: a large packed file keeps the
 *      reported bytes flat while its pieces arrive. The host's acks on
 *      the bulk channel are progress, so it does not stall; when the acks
 *      stop it does.
 *   6. bot_host_sync_slots_flap: slots that go to 0 and back pause the
 *      clocks and do not start them again, so a host cannot hold Ready
 *      past the limits that way.
 *   7. bot_host_sync_server_step: the server's tick of the wait
 *      (serverBotHostSyncStep) and its row (serverBotHostSyncRow): the
 *      pause while not needed, no wait for a host that sits out the game,
 *      and the late mark on the row.
 */

#include <stdbool.h>
#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "bot_host_link.h"
#include "client_sim.h"
#include "control_event.h"
#include "transport_udp_server_internal.h" /* udpServer, serverBotHostSync* */
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
    for (t = 1010; t <= 1010 + 30000; t++) botHostSyncWatchHold(&w, t);
    UT_ASSERT(botHostSyncWatchCheck(&w, 31010) == BOT_HOST_SYNC_WAIT_ON);
    /* The player sets Max slots 2 at tick 31010: the clocks go on from
     * the pause, so the stall comes 10 s later and not at once. */
    botHostSyncWatchResume(&w, 31010);
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

    memset(&stuck, 0, sizeof(stuck));
    memset(&moving, 0, sizeof(moving));
    memset(&silent, 0, sizeof(silent));
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
        memset(&w, 0, sizeof(w));
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

    memset(&w, 0, sizeof(w));
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

    /* A report on a generation the watch does not know is ignored: it
     * does not start the clocks again and is no progress. */
    botHostSyncWatchStart(&w, 6, 100);
    botHostSyncWatchReport(&w, 6, 0, 110);
    botHostSyncWatchReport(&w, 7, 9000, 590);
    UT_ASSERT(w.gen == 6 && w.reported && w.startTick == 100);
    UT_ASSERT(w.lastDone == 0 && w.progressTick == 110);
    UT_ASSERT(botHostSyncWatchCheck(&w, 609) == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&w, 110 + BOT_HOST_SYNC_STALL_TICKS)
              == BOT_HOST_SYNC_WAIT_STALLED);
    /* A report on another generation before any report on this one does
     * not count as the first report either. */
    botHostSyncWatchStart(&w, 6, 100);
    botHostSyncWatchReport(&w, 5, 0, 110);
    UT_ASSERT(!w.reported);
    UT_ASSERT(botHostSyncWatchCheck(&w, 110 + BOT_HOST_SYNC_STALL_TICKS)
              == BOT_HOST_SYNC_WAIT_ON);

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

/* ================================================================
 * 5. A large packed file: the reported bytes stay flat
 * ================================================================ */
int run_bot_host_sync_packed_big_file(void) {
    BotHostSyncWatch w, noAcks;
    const uint32_t t0 = 8000;
    uint32_t t, acked = 0, lateAt = 0;
    const uint32_t done = 40000;   /* the small files before the big one */

    /* shield_stamp_cache.bin: about 1.1 MB packed. At about 30 KB/s for
     * this host it takes about 37 s; the host reports each piece of 128
     * KiB (about every 4.4 s) with the same done, since a packed file
     * counts only when it is whole. Its acks go up each tick a segment
     * arrives. */
    memset(&w, 0, sizeof(w));
    memset(&noAcks, 0, sizeof(noAcks));
    botHostSyncWatchStart(&w, 9, t0);
    botHostSyncWatchStart(&noAcks, 9, t0);
    botHostSyncWatchReport(&w, 9, done, t0 + 20);
    botHostSyncWatchReport(&noAcks, 9, done, t0 + 20);
    for (t = t0 + 21; t <= t0 + 20 + 1850; t++) {
        if (t % 2 == 0) acked++;   /* one 640-byte segment per 2 ticks */
        botHostSyncWatchAcked(&w, acked, t);
        if ((t - t0) % 220 == 0) {
            botHostSyncWatchReport(&w, 9, done, t);
            botHostSyncWatchReport(&noAcks, 9, done, t);
        }
        UT_ASSERT_MSG(botHostSyncWatchCheck(&w, t) == BOT_HOST_SYNC_WAIT_ON,
                      "a moving packed file stalled at +%u",
                      (unsigned)(t - t0));
        if (lateAt == 0 && botHostSyncWatchCheck(&noAcks, t) ==
                               BOT_HOST_SYNC_WAIT_STALLED) {
            lateAt = t;
        }
    }
    /* With the same reports and no acks the host stalls at 10 s. */
    UT_ASSERT_MSG(lateAt == t0 + 20 + BOT_HOST_SYNC_STALL_TICKS,
                  "no acks: late at %u", (unsigned)lateAt);
    printf("  packed file, flat done, acks moving: no stall for %u ticks\n",
           1850u);

    /* The acks stop (the link died): the stall comes 10 s after the
     * last one, though the server may still send. */
    {
        uint32_t last = t - 1;
        for (; t < last + BOT_HOST_SYNC_STALL_TICKS; t++) {
            botHostSyncWatchAcked(&w, acked, t);
            UT_ASSERT(botHostSyncWatchCheck(&w, t) == BOT_HOST_SYNC_WAIT_ON);
        }
        botHostSyncWatchAcked(&w, acked, t);
        UT_ASSERT_MSG(botHostSyncWatchCheck(&w, last + BOT_HOST_SYNC_STALL_TICKS)
                      == BOT_HOST_SYNC_WAIT_STALLED, "no stall after acks stop");
    }

    /* The acks do not move the 60 s limit. */
    botHostSyncWatchStart(&w, 9, t0);
    botHostSyncWatchReport(&w, 9, done, t0 + 20);
    for (t = t0 + 21, acked = 0; t <= t0 + BOT_HOST_SYNC_LIMIT_TICKS; t++) {
        botHostSyncWatchAcked(&w, ++acked, t);
        UT_ASSERT(botHostSyncWatchCheck(&w, t) == BOT_HOST_SYNC_WAIT_ON);
    }
    botHostSyncWatchAcked(&w, ++acked, t);
    UT_ASSERT(botHostSyncWatchCheck(&w, t) == BOT_HOST_SYNC_WAIT_LATE);
    return 0;
}

/* ================================================================
 * 6. Slots that go to 0 and back
 * ================================================================ */
int run_bot_host_sync_slots_flap(void) {
    BotHostSyncWatch stuck, moving;
    const uint32_t t0 = 3000;
    uint32_t t, needed = 0, stuckAt = 0, lateAt = 0, done = 0;

    /* The host offers 0 slots for one tick each 9 s (450 ticks). Before,
     * each such tick started both clocks again, so a host stuck at 0%
     * held Ready with no end. */
    memset(&stuck, 0, sizeof(stuck));
    memset(&moving, 0, sizeof(moving));
    botHostSyncWatchStart(&stuck, 2, t0);
    botHostSyncWatchStart(&moving, 2, t0);
    botHostSyncWatchReport(&stuck, 2, 0, t0 + 10);
    botHostSyncWatchReport(&moving, 2, 0, t0 + 10);
    for (t = t0 + 11; t <= t0 + 20000; t++) {
        bool zeroSlots = (t - t0) % 450 == 0;
        if ((t - t0) % 100 == 0) {
            done += 1000;
            botHostSyncWatchReport(&moving, 2, done, t);
        }
        if (zeroSlots) {
            botHostSyncWatchHold(&stuck, t);
            botHostSyncWatchHold(&moving, t);
            continue;
        }
        botHostSyncWatchResume(&stuck, t);
        botHostSyncWatchResume(&moving, t);
        needed++;
        if (stuckAt == 0 &&
            botHostSyncWatchCheck(&stuck, t) == BOT_HOST_SYNC_WAIT_STALLED) {
            stuckAt = t;
        }
        if (lateAt == 0 &&
            botHostSyncWatchCheck(&moving, t) == BOT_HOST_SYNC_WAIT_LATE) {
            lateAt = t;
        }
    }
    printf("  slots flap: stuck host stalls at +%u, moving host late at "
           "+%u\n", (unsigned)(stuckAt - t0), (unsigned)(lateAt - t0));
    /* The stuck host stalls after 500 needed ticks from its report: the
     * one paused tick in that run adds one. */
    UT_ASSERT_MSG(stuckAt != 0, "the stuck host never stalled");
    UT_ASSERT_MSG(stuckAt == t0 + 10 + BOT_HOST_SYNC_STALL_TICKS + 1,
                  "stuck host late at +%u", (unsigned)(stuckAt - t0));
    /* The moving host is late after 60 s of needed time: the paused
     * ticks (one each 450) are not counted, and nothing else is added. */
    UT_ASSERT_MSG(lateAt != 0, "the moving host never got late");
    {
        uint32_t paused = (lateAt - t0) / 450;
        UT_ASSERT_MSG(lateAt == t0 + BOT_HOST_SYNC_LIMIT_TICKS + 1 + paused,
                      "moving host late at +%u (paused %u)",
                      (unsigned)(lateAt - t0), (unsigned)paused);
    }

    /* Time before a pause still counts: 400 ticks of no progress, a long
     * pause, then 100 more ticks stall it. */
    botHostSyncWatchStart(&stuck, 2, t0);
    botHostSyncWatchReport(&stuck, 2, 0, t0);
    for (t = t0 + 400; t < t0 + 400 + 5000; t++) botHostSyncWatchHold(&stuck, t);
    UT_ASSERT(botHostSyncWatchCheck(&stuck, t) == BOT_HOST_SYNC_WAIT_ON);
    botHostSyncWatchResume(&stuck, t);
    UT_ASSERT(botHostSyncWatchCheck(&stuck, t + 99) == BOT_HOST_SYNC_WAIT_ON);
    UT_ASSERT(botHostSyncWatchCheck(&stuck, t + 100) ==
              BOT_HOST_SYNC_WAIT_STALLED);

    /* Progress while paused counts at the pause, so the clocks never
     * stand past now when the pause ends. */
    botHostSyncWatchStart(&stuck, 2, t0);
    botHostSyncWatchReport(&stuck, 2, 0, t0);
    botHostSyncWatchHold(&stuck, t0 + 300);
    botHostSyncWatchReport(&stuck, 2, 5000, t0 + 2000);
    botHostSyncWatchAcked(&stuck, 77, t0 + 2500);
    botHostSyncWatchResume(&stuck, t0 + 4000);
    UT_ASSERT(stuck.progressTick == t0 + 4000);
    UT_ASSERT(stuck.startTick == t0 + 3700);
    UT_ASSERT(botHostSyncWatchCheck(&stuck, t0 + 4000 +
                                    BOT_HOST_SYNC_STALL_TICKS - 1) ==
              BOT_HOST_SYNC_WAIT_ON);
    /* A new generation while paused starts its clocks at the pause's end. */
    botHostSyncWatchHold(&stuck, t0 + 5000);
    botHostSyncWatchStart(&stuck, 3, t0 + 6000);
    botHostSyncWatchResume(&stuck, t0 + 9000);
    UT_ASSERT(stuck.startTick == t0 + 9000);
    UT_ASSERT(botHostSyncWatchCheck(&stuck, t0 + 9000 +
                                    BOT_HOST_SYNC_LIMIT_TICKS) ==
              BOT_HOST_SYNC_WAIT_ON);
    return 0;
}

/* ================================================================
 * 7. The server's tick of the wait and its row
 * ================================================================ */
int run_bot_host_sync_server_step(void) {
    static BotHostConn saved;
    BotHostConn *hc = &udpServer.botHosts[0];
    const uint32_t savedTick = udpServer.tickCount;
    const uint32_t savedGen = udpServer.brainManifestGen;
    const uint32_t t0 = 60000;
    uint32_t t;
    int rowNotNeeded, rowSkips, rowSyncing, rowLate, rowSkipsLate;
    BotHostSyncWait stepSkips = BOT_HOST_SYNC_WAIT_ON, stepPaused;
    BotHostSyncWait stepAcked = BOT_HOST_SYNC_WAIT_ON, stepStall;
    bool markedLate, markedSkip, outLate, outSkip;
    uint32_t stallAfter = 0;

    saved = *hc;
    memset(hc, 0, sizeof(*hc));
    SDL_strlcpy(hc->name, "ut652host", sizeof(hc->name));
    hc->connected = true;
    udpServer.brainManifestGen = 12;
    udpServer.tickCount = t0;
    botHostSyncWatchStart(&hc->syncWatch, 12, t0);
    hc->syncGen = 12;
    hc->syncDone = 1000;
    hc->syncTotal = 2000000;
    botHostSyncWatchReport(&hc->syncWatch, 12, hc->syncDone, t0);

    /* Not needed (no bot, or 0 slots): the row waits, Ready is free, and
     * ten minutes of it are no stall. */
    rowNotNeeded = serverBotHostSyncRow(0, false, false);
    for (t = t0; t < t0 + 30000; t++) {
        udpServer.tickCount = t;
        (void)serverBotHostSyncStep(0, false, false);
    }
    stepPaused = serverBotHostSyncStep(0, true, false);
    rowSyncing = serverBotHostSyncRow(0, true, false);

    /* Sits out this game (botHostSkipsGame): never checked, never marked,
     * however long; its row shows late. */
    for (t = udpServer.tickCount; t < t0 + 30000 + 4000; t++) {
        BotHostSyncWait w;
        udpServer.tickCount = t;
        w = serverBotHostSyncStep(0, true, true);
        if (w != BOT_HOST_SYNC_WAIT_ON) stepSkips = w;
    }
    rowSkips = serverBotHostSyncRow(0, true, true);
    outLate = hc->syncLate;
    outSkip = hc->skipGame;

    /* Needed again with the bulk acks moving and done flat: no stall. */
    t = udpServer.tickCount;
    botHostSyncWatchStart(&hc->syncWatch, 12, t);
    botHostSyncWatchReport(&hc->syncWatch, 12, hc->syncDone, t);
    {
        const uint32_t end = t + 1500;
        for (; t < end; t++) {
            BotHostSyncWait w;
            hc->channelMux.ch[CHANNEL_BULK].ackedSeq++;
            udpServer.tickCount = t;
            w = serverBotHostSyncStep(0, true, false);
            if (w != BOT_HOST_SYNC_WAIT_ON) stepAcked = w;
        }
    }
    /* The acks stop: STALLED 10 s after the last one, and the host is
     * marked late. */
    stepStall = BOT_HOST_SYNC_WAIT_ON;
    {
        const uint32_t lastAck = t - 1;
        for (; t <= lastAck + BOT_HOST_SYNC_STALL_TICKS; t++) {
            udpServer.tickCount = t;
            stepStall = serverBotHostSyncStep(0, true, false);
            if (stepStall != BOT_HOST_SYNC_WAIT_ON) break;
        }
        stallAfter = t - lastAck;
    }
    markedLate = hc->syncLate;
    markedSkip = hc->skipGame;
    rowLate = serverBotHostSyncRow(0, true, false);
    /* A late host's row stays late with the files not needed. */
    rowSkipsLate = serverBotHostSyncRow(0, false, false);

    *hc = saved;
    udpServer.tickCount = savedTick;
    udpServer.brainManifestGen = savedGen;

    UT_ASSERT_MSG(rowNotNeeded == BOT_HOST_SYNC_ROW_WAITING, "row %d",
                  rowNotNeeded);
    UT_ASSERT_MSG(stepPaused == BOT_HOST_SYNC_WAIT_ON, "after pause %d",
                  (int)stepPaused);
    UT_ASSERT_MSG(rowSyncing == BOT_HOST_SYNC_ROW_SYNCING, "row %d",
                  rowSyncing);
    UT_ASSERT_MSG(stepSkips == BOT_HOST_SYNC_WAIT_ON, "skips %d",
                  (int)stepSkips);
    UT_ASSERT(!outLate && !outSkip);
    UT_ASSERT_MSG(rowSkips == BOT_HOST_SYNC_ROW_LATE, "row %d", rowSkips);
    UT_ASSERT_MSG(stepAcked == BOT_HOST_SYNC_WAIT_ON, "acked %d",
                  (int)stepAcked);
    UT_ASSERT_MSG(stepStall == BOT_HOST_SYNC_WAIT_STALLED, "stall %d",
                  (int)stepStall);
    UT_ASSERT_MSG(stallAfter == BOT_HOST_SYNC_STALL_TICKS,
                  "stalled %u ticks after the last ack", (unsigned)stallAfter);
    UT_ASSERT(markedLate && markedSkip);
    UT_ASSERT_MSG(rowLate == BOT_HOST_SYNC_ROW_LATE, "row %d", rowLate);
    UT_ASSERT_MSG(rowSkipsLate == BOT_HOST_SYNC_ROW_LATE, "row %d",
                  rowSkipsLate);
    return 0;
}
