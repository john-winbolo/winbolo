/*
 * A bot's own smart ping (test_bot_ping.c).
 *
 * A brain can ask for a ping by returning ping_kind / ping_x / ping_y from
 * its think. What that request has to become is a CMD_PING from the BOT'S
 * OWN player slot, because everything a ping does downstream is keyed off
 * the sender: serverSimPingReachesClient filters the marker to the sender's
 * team, the marker the client draws names the sender, and the replay records
 * the sender. A ping placed under any other slot would go to the wrong team.
 *
 * The request cannot be applied where it is read. brain_data.c reads it on a
 * bot worker thread, inside the parallel brain-think stage, and
 * serverSimApplyCommand belongs to the producer thread — the same reason the
 * bot's chat is queued rather than sent. So botManagerQueuePing writes a
 * ClientCommand into the bot's own BotJobCtx, and Stage 3 of botManagerTick
 * drains it with serverSimApplyCommand(sim, slot, cmd).
 *
 * These tests do exactly what Stage 3 does: queue, then drain. That is the
 * whole path from "the brain asked" to "the sim has a ping", minus the
 * thread hop, which changes when the drain runs and not what it does.
 *
 * The rate limit is the bot's own, on top of the server's. A bot thinks
 * every tick and can decide to ping on any of them, so without a floor a
 * re-planning bot would mark the team's map fifty times a second.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_command.h"
#include "everard_map.h"
#include "input_packet.h"          /* EVENT_PING, PING_KIND_* */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->tick, sim->botMgr, the pending ping */
#include "server_sim_lifecycle.h"
#include "bot_manager.h"           /* botManagerQueuePing, BOT_PING_MIN_GAP_TICKS */
#include "game_sim.h"
#include "players.h"
#include "threads.h"
#include "test_harness.h"

/* The slot the bot sits in. Slot 0 is the human ut_make_running_sim seats. */
#define BP_BOT_SLOT 1

/* Middle of map square (70, 90). World units are 256 to a square. */
#define BP_WORLD_X ((uint16_t)((70 << M_W_SHIFT_SIZE) + 128))
#define BP_WORLD_Y ((uint16_t)((90 << M_W_SHIFT_SIZE) + 128))

/* A running sim with a human in slot 0 and a bot in BP_BOT_SLOT. */
static ServerSim *bp_make_sim(void) {
    ServerSim *sim = ut_make_running_sim("Human");
    if (sim == NULL) return NULL;
    serverSimAddPlayer(sim, BP_BOT_SLOT, "Bot1", false);
    return sim;
}

/* What Stage 3 of botManagerTick does with the queue: hand every pending
 * command to the dispatcher under the bot's own slot, then empty the queue.
 * Returns how many commands were drained. */
static int bp_drain(ServerSim *sim, BYTE slot) {
    BotJobCtx *j = &sim->botMgr.jobs[slot];
    int n = j->pendingCmdCount;
    int q;
    threadsWaitForMutex();
    for (q = 0; q < n; q++) {
        (void)serverSimApplyCommand(sim, slot, &j->pendingCmds[q]);
    }
    threadsReleaseMutex();
    j->pendingCmdCount = 0;
    return n;
}

/* The last EVENT_PING in the per-frame buffer, after running the flush that
 * a real tick runs. The dispatch arm records but does not buffer. */
static const GameEvent *bp_last_ping(ServerSim *sim) {
    const GameEvent *evs;
    int n, i;
    serverSimFlushPendingPings(sim);
    evs = serverSimGetEvents(sim);
    n = (int)serverSimGetEventCount(sim);
    for (i = n - 1; i >= 0; i--) {
        if (evs[i].type == EVENT_PING) return &evs[i];
    }
    return NULL;
}

static void bp_clear_events(ServerSim *sim) {
    sim->eventCount = 0;
    memset(sim->hasPendingPing, 0, sizeof(sim->hasPendingPing));
}

/* A queued request becomes one CMD_PING carrying the bot's own slot. */
int run_bot_ping_queue_becomes_cmd_ping_from_bot_slot(void) {
    ServerSim       *sim = bp_make_sim();
    const GameEvent *ev;
    UT_ASSERT_MSG(sim != NULL, "bp_make_sim returned NULL");
    bp_clear_events(sim);

    botManagerQueuePing(sim, BP_BOT_SLOT, PING_KIND_ATTACK,
                        BP_WORLD_X, BP_WORLD_Y);
    UT_ASSERT_MSG(sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount == 1,
                  "the request queued %d commands, expected 1",
                  sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount);
    UT_ASSERT_MSG(sim->botMgr.jobs[BP_BOT_SLOT].pendingCmds[0].type == CMD_PING,
                  "the queued command is not a CMD_PING");

    /* Nothing is queued under any other slot: a bot writes only its own. */
    UT_ASSERT_MSG(sim->botMgr.jobs[0].pendingCmdCount == 0,
                  "the bot's ping was queued under slot 0 as well");

    UT_ASSERT_MSG(bp_drain(sim, BP_BOT_SLOT) == 1, "nothing was drained");

    ev = bp_last_ping(sim);
    UT_ASSERT_MSG(ev != NULL, "no EVENT_PING reached the event buffer");
    UT_ASSERT_MSG(ev->data[0] == BP_BOT_SLOT,
                  "the ping's sender is %d, expected the bot's own slot %d",
                  ev->data[0], BP_BOT_SLOT);
    UT_ASSERT_MSG(ev->data[1] == PING_KIND_ATTACK, "kind = %d", ev->data[1]);
    UT_ASSERT_MSG(((ev->data[2] << 8) | ev->data[3]) == BP_WORLD_X,
                  "worldX did not survive the trip");
    UT_ASSERT_MSG(((ev->data[4] << 8) | ev->data[5]) == BP_WORLD_Y,
                  "worldY did not survive the trip");

    serverSimDestroy(sim);
    return 0;
}

/* One ping per bot per BOT_PING_MIN_GAP_TICKS; the extras are dropped where
 * they are asked for, so they never reach the queue at all. */
int run_bot_ping_rate_limit_drops_extras(void) {
    ServerSim *sim = bp_make_sim();
    int        i;
    UT_ASSERT_MSG(sim != NULL, "bp_make_sim returned NULL");
    bp_clear_events(sim);

    /* Ten requests on one tick — a brain re-planning every think. */
    for (i = 0; i < 10; i++) {
        botManagerQueuePing(sim, BP_BOT_SLOT, PING_KIND_ON_MY_WAY,
                            BP_WORLD_X, BP_WORLD_Y);
    }
    UT_ASSERT_MSG(sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount == 1,
                  "ten requests on one tick queued %d pings, expected 1",
                  sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount);
    (void)bp_drain(sim, BP_BOT_SLOT);

    /* One tick short of the gap is still inside it. */
    sim->tick += BOT_PING_MIN_GAP_TICKS - 1;
    botManagerQueuePing(sim, BP_BOT_SLOT, PING_KIND_ATTACK,
                        BP_WORLD_X, BP_WORLD_Y);
    UT_ASSERT_MSG(sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount == 0,
                  "a ping %d ticks after the last one was let through",
                  BOT_PING_MIN_GAP_TICKS - 1);

    /* One more tick and the gap is served. */
    sim->tick += 1;
    botManagerQueuePing(sim, BP_BOT_SLOT, PING_KIND_ATTACK,
                        BP_WORLD_X, BP_WORLD_Y);
    UT_ASSERT_MSG(sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount == 1,
                  "a ping a full gap after the last one was refused");

    /* The limit is per bot, not shared: another slot is untouched by it. */
    botManagerQueuePing(sim, 0, PING_KIND_ATTACK, BP_WORLD_X, BP_WORLD_Y);
    UT_ASSERT_MSG(sim->botMgr.jobs[0].pendingCmdCount == 1,
                  "one bot's ping gap silenced another slot");

    serverSimDestroy(sim);
    return 0;
}

/* A slot number the bot table does not have must be refused before it is
 * used as an index. */
int run_bot_ping_ignores_bad_slot(void) {
    ServerSim *sim = bp_make_sim();
    UT_ASSERT_MSG(sim != NULL, "bp_make_sim returned NULL");

    botManagerQueuePing(sim, (BYTE)MAX_TANKS, PING_KIND_ATTACK,
                        BP_WORLD_X, BP_WORLD_Y);
    botManagerQueuePing(NULL, BP_BOT_SLOT, PING_KIND_ATTACK,
                        BP_WORLD_X, BP_WORLD_Y);
    UT_ASSERT_MSG(sim->botMgr.jobs[BP_BOT_SLOT].pendingCmdCount == 0,
                  "an out-of-range slot queued a ping anyway");

    serverSimDestroy(sim);
    return 0;
}
