/*
 * Server side of the smart ping (test_ping_dispatch.c).
 *
 * CMD_PING is the only command a player can fire that puts something on
 * everyone else's screen at a place of the sender's choosing, so the arm in
 * server_command_dispatch.c has more gates than most:
 *
 *   - a running game (a lobby has no map to point at),
 *   - a sender that still occupies a player slot (dead is fine; empty is not),
 *   - a kind this build knows,
 *   - a position on the map,
 *   - and a rate limit, so a held key cannot machine-gun the team's view.
 *
 * Each of those is checked here, and so is the shape of the GameEvent the
 * accepted command turns into — the six bytes the client reads back out.
 *
 * Delivery is checked separately: serverSimPingReachesClient is the single
 * predicate both copies of the filter call (the per-client snapshot build and
 * the UDP drain), so pinning it pins both.
 */

#include <stdint.h>
#include <string.h>

#include "global.h"
#include "client_command.h"
#include "everard_map.h"
#include "input_packet.h"          /* EVENT_PING, PING_KIND_* */
#include "server_sim.h"
#include "server_sim_internal.h"   /* sim->tick, the rate-limit state, the reach predicate */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled, serverSimSetTeam */
#include "game_sim.h"
#include "players.h"
#include "threads.h"
#include "test_harness.h"

/* A world position comfortably inside Everard Island. World units are 256 to
 * a map square, so this is the middle of square (80, 90). */
#define PD_WORLD_X ((uint16_t)((80 << M_W_SHIFT_SIZE) + 128))
#define PD_WORLD_Y ((uint16_t)((90 << M_W_SHIFT_SIZE) + 128))

static CmdResult pd_send(ServerSim *sim, int senderSlot, uint8_t kind,
                         uint16_t wx, uint16_t wy) {
    ClientCommand cmd;
    CmdResult r;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PING;
    cmd.cmdSeq = 1;
    cmd.u.ping.kind   = kind;
    cmd.u.ping.worldX = wx;
    cmd.u.ping.worldY = wy;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* Move the sim's clock on without running a tick. The rate limit is measured
 * in sim ticks, and a unit test has no reason to spin a real game forward
 * hundreds of ticks to watch a cooldown expire. */
static void pd_advance(ServerSim *sim, uint32_t ticks) {
    sim->tick += ticks;
}

/* The last EVENT_PING the sim buffered, or NULL.
 *
 * The dispatch arm does not buffer the event itself — it records the ping in
 * pendingPing, and serverSimFlushPendingPings (called at the top of a running
 * serverSimTick, right after the per-frame event buffer is cleared) is the one
 * place an EVENT_PING enters sim->events. Running the flush here is what a real
 * frame does between receiving the command and draining events, so this reads
 * the buffer the transports actually see. The flush clears the pending flags,
 * so calling it twice cannot double-buffer a ping. */
static const GameEvent *pd_last_ping(ServerSim *sim) {
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

/* Both halves of the ping's path back to zero: the per-frame event buffer and
 * the pending records the flush feeds it from. */
static void pd_clear_events(ServerSim *sim) {
    sim->eventCount = 0;
    memset(sim->hasPendingPing, 0, sizeof(sim->hasPendingPing));
}

/* Did the arm record a pending ping for this slot? A refused ping must leave
 * this false — the record is the queue, so a stray one would still be sent. */
static bool pd_has_pending(ServerSim *sim, int slot) {
    return sim->hasPendingPing[slot];
}

int run_ping_dispatch_accepts_and_builds_event(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    const GameEvent *ev;
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    pd_clear_events(sim);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_ATTACK, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_OK,
                  "a running game with an occupied slot must accept a ping");

    /* The arm records, it does not buffer: the event must still be absent from
     * sim->events, and waiting in the pending slot. Buffering at dispatch time
     * as well would hand an in-process client, whose snapshot poll dedups per
     * serverTick, the same ping on two consecutive ticks. */
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 0,
                  "the dispatch arm buffered an event; the flush is the only "
                  "place a ping may enter sim->events");
    UT_ASSERT_MSG(pd_has_pending(sim, 0), "no ping was recorded as pending");
    UT_ASSERT_MSG(sim->pendingPing[0].type == EVENT_PING,
                  "pending record is not an EVENT_PING");
    UT_ASSERT_MSG(sim->pendingPing[0].data[1] == PING_KIND_ATTACK,
                  "pending record kind = %d", sim->pendingPing[0].data[1]);

    /* Now the tick's flush runs, and exactly one EVENT_PING appears. */
    ev = pd_last_ping(sim);
    UT_ASSERT_MSG(ev != NULL, "no EVENT_PING was buffered");
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 1,
                  "the flush buffered %d events, expected exactly 1",
                  (int)serverSimGetEventCount(sim));
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "the flush left the pending record set; the next tick would "
                  "send the same ping again");
    /* [sender, kind, xHi, xLo, yHi, yLo] — big-endian, the same order the
     * client's reader indexes. */
    UT_ASSERT_MSG(ev->data[0] == 0, "sender = %d", ev->data[0]);
    UT_ASSERT_MSG(ev->data[1] == PING_KIND_ATTACK, "kind = %d", ev->data[1]);
    UT_ASSERT_MSG(((ev->data[2] << 8) | ev->data[3]) == PD_WORLD_X,
                  "worldX round trip");
    UT_ASSERT_MSG(((ev->data[4] << 8) | ev->data[5]) == PD_WORLD_Y,
                  "worldY round trip");
    /* Nothing past the six bytes the size table declares. */
    UT_ASSERT(ev->data[6] == 0 && ev->data[7] == 0);

    /* A second frame with nothing new must add nothing: the flush clears what
     * it consumed, so a ping cannot be re-sent tick after tick. */
    serverSimFlushPendingPings(sim);
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 1,
                  "a second flush re-buffered the ping (count now %d)",
                  (int)serverSimGetEventCount(sim));

    /* A frame with no room left in the event buffer must postpone the ping,
     * not eat it: the sender has already been answered CMD_OK, and EVENT_PING
     * is reliable, so a dropped one is a marker the player watched themselves
     * place that nobody ever sees. Filling the buffer is a single assignment —
     * what it holds doesn't matter, only that serverSimAddEvent would refuse. */
    pd_clear_events(sim);
    pd_advance(sim, PING_RATE_WINDOW_TICKS);
    UT_ASSERT(pd_send(sim, 0, PING_KIND_ATTACK, PD_WORLD_X, PD_WORLD_Y)
                  == CMD_OK);
    sim->eventCount = MAX_SNAPSHOT_EVENTS;
    serverSimFlushPendingPings(sim);
    UT_ASSERT_MSG(pd_has_pending(sim, 0),
                  "a flush with no buffer room dropped the ping instead of "
                  "leaving it pending for the next tick");
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == MAX_SNAPSHOT_EVENTS,
                  "the full buffer grew past its cap");

    /* Next tick, with room again, it goes out. */
    sim->eventCount = 0;
    serverSimFlushPendingPings(sim);
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "the retried ping is still pending");
    UT_ASSERT_MSG(serverSimGetEventCount(sim) == 1,
                  "the retried ping buffered %d events, expected 1",
                  (int)serverSimGetEventCount(sim));

    serverSimDestroy(sim);
    return 0;
}

int run_ping_dispatch_new_round_clears_rate_limit(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    int i;
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");
    pd_clear_events(sim);

    /* Spend the whole burst allowance in the round that is ending, so the
     * ring is full and its oldest entry is the round's first tick. That is
     * the state that goes wrong: the rate limiter stamps sim->tick, and the
     * next round rewinds sim->tick to 0, so a stamp left behind sits in the
     * new round's future — the arm then measures the new round's first ping
     * against a ping sent in the last one and calls it a cooldown. */
    for (i = 0; i < PING_RATE_BURST; i++) {
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the previous round's burst was refused", i);
        pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
        pd_clear_events(sim);
    }
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "the previous round's allowance should now be spent");

    /* A new round, the same way the harness started the first one. */
    serverSimStartGame(sim);
    UT_ASSERT_MSG(sim->tick == 0, "a new round did not rewind the clock");
    UT_ASSERT_MSG(playersIsInUse(&sim->sim.plyrs, 0),
                  "the player did not survive the round change");
    pd_clear_events(sim);

    /* First ping of the new round, at its first ticks, must land. */
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_OK,
                  "the new round's first ping was refused as a cooldown for a "
                  "ping sent in the previous round");
    UT_ASSERT_MSG(pd_has_pending(sim, 0),
                  "the accepted ping left no pending record");

    /* And the fresh allowance is a whole burst, not the remains of one. */
    pd_clear_events(sim);
    for (i = 1; i < PING_RATE_BURST; i++) {
        pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the new round's burst was refused", i + 1);
        pd_clear_events(sim);
    }

    serverSimDestroy(sim);
    return 0;
}

int run_ping_dispatch_rejects_lobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    UT_ASSERT_MSG(sim != NULL, "serverSimCreateCompressed returned NULL");
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);

    /* No map is being played, so there is nothing to point at. */
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_BAD_STATE,
                  "a lobby ping must be refused");
    UT_ASSERT_MSG(!sim->hasPendingPing[0],
                  "a refused ping was recorded as pending");
    UT_ASSERT_MSG(pd_last_ping(sim) == NULL, "a refused ping emitted an event");

    serverSimDestroy(sim);
    return 0;
}

int run_ping_dispatch_rejects_empty_slot_and_out_of_range(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    UT_ASSERT(sim != NULL);
    pd_clear_events(sim);

    /* Slot 1 has nobody in it: the sender holds no tank in this game. */
    UT_ASSERT_MSG(pd_send(sim, 1, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_BAD_STATE,
                  "a slot with no player must be refused");

    /* A spectator holds no player-table slot at all, so the only identity it
     * can ever present to the dispatcher is an out-of-range one. The entry
     * guard refuses that before the arm sees it. */
    UT_ASSERT_MSG(pd_send(sim, MAX_TANKS, PING_KIND_STANDARD,
                          PD_WORLD_X, PD_WORLD_Y) == CMD_REJECT_INVALID,
                  "an out-of-range sender must be refused");
    UT_ASSERT_MSG(pd_send(sim, -1, PING_KIND_STANDARD,
                          PD_WORLD_X, PD_WORLD_Y) == CMD_REJECT_INVALID,
                  "a negative sender must be refused");
    UT_ASSERT_MSG(!pd_has_pending(sim, 0) && !pd_has_pending(sim, 1),
                  "a refused ping was recorded as pending");
    UT_ASSERT(pd_last_ping(sim) == NULL);

    serverSimDestroy(sim);
    return 0;
}

int run_ping_dispatch_rejects_bad_kind(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    UT_ASSERT(sim != NULL);
    pd_clear_events(sim);

    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_COUNT, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_INVALID,
                  "one past the last kind must be refused");
    UT_ASSERT_MSG(pd_send(sim, 0, 255, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_INVALID,
                  "a nonsense kind must be refused");
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "a refused ping was recorded as pending");
    UT_ASSERT_MSG(pd_last_ping(sim) == NULL, "a refused ping emitted an event");

    /* Every kind this build does know is accepted, one per cooldown window. */
    {
        int k;
        for (k = 0; k < PING_KIND_COUNT; k++) {
            pd_advance(sim, PING_RATE_WINDOW_TICKS);
            UT_ASSERT_MSG(pd_send(sim, 0, (uint8_t)k, PD_WORLD_X, PD_WORLD_Y)
                              == CMD_OK, "kind %d refused", k);
        }
    }

    serverSimDestroy(sim);
    return 0;
}

int run_ping_dispatch_rate_limit(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    int i;
    UT_ASSERT(sim != NULL);
    pd_clear_events(sim);

    /* First one lands, and is recorded for the tick's flush to buffer. */
    UT_ASSERT(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                  == CMD_OK);
    UT_ASSERT_MSG(pd_has_pending(sim, 0),
                  "an accepted ping left no pending record");

    /* Consume it the way the top of a running tick would, so what follows is
     * measured against an empty queue: a cooldown rejection must add nothing
     * back. The pending record IS the send queue, so a rejected ping that set
     * it would still go out. */
    pd_clear_events(sim);

    /* A second one on the same tick, and anywhere inside the minimum gap, is
     * a cooldown — a held key must not machine-gun the team's view. */
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "two pings on one tick must not both land");
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "a rate-limited ping was still queued for sending");
    pd_advance(sim, PING_RATE_MIN_GAP_TICKS - 1);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "just inside the minimum gap must still be a cooldown");
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "a rate-limited ping was still queued for sending");

    /* Past the gap it lands again — up to the burst cap. The first ping
     * above used one of the PING_RATE_BURST slots, so PING_RATE_BURST-1
     * more fit before the window closes. */
    for (i = 1; i < PING_RATE_BURST; i++) {
        pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the burst was refused", i + 1);
    }

    /* One past the burst, still inside the window: refused even though the
     * minimum gap has passed. */
    pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "the burst cap must hold once the allowance is spent");
    pd_clear_events(sim);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "the burst cap must hold on a retry too");
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "a burst-capped ping was still queued for sending");

    /* Once the window has rolled past the oldest of the burst, the
     * allowance comes back. */
    pd_advance(sim, PING_RATE_WINDOW_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_OK,
                  "the allowance must return once the window has passed");

    serverSimDestroy(sim);
    return 0;
}

int run_ping_reaches_team_only(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 1, "Mate", false);
    serverSimAddPlayer(sim, 2, "Enemy", false);

    /* A sender with no team pings for itself alone. Slots arrive on a
     * default team, so this has to be arranged rather than assumed. */
    threadsWaitForMutex();
    serverSimSetTeamBatch(sim, 0, 0);
    serverSimSetTeamBatch(sim, 1, 0);
    serverSimSetTeamBatch(sim, 2, 0);
    threadsReleaseMutex();
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 0, 0),
                  "the sender always sees its own ping");
    UT_ASSERT_MSG(!serverSimPingReachesClient(sim, 1, 0),
                  "a teamless sender pings for itself alone");

    /* Two on the same team, one on another. Set in a batch and reconciled
     * once: serverSimSetTeam reapplies alliances after every single call,
     * and the reapply only ever ADDS pairs — so moving the slots one at a
     * time would ally two of them off the default team assignment on the way
     * through and never take it back. The batch form is what the lobby's own
     * multi-slot paths use for the same reason. */
    threadsWaitForMutex();
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimSetTeamBatch(sim, 2, 2);
    serverSimReapplyTeamAlliances(sim);
    threadsReleaseMutex();

    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 1, 0),
                  "a teammate must receive the ping");
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 0, 1),
                  "and the other way round");
    UT_ASSERT_MSG(!serverSimPingReachesClient(sim, 2, 0),
                  "the other team must not");

    /* Out-of-range slots are not a team. */
    UT_ASSERT(!serverSimPingReachesClient(sim, MAX_TANKS, 0));
    UT_ASSERT(!serverSimPingReachesClient(sim, 0, MAX_TANKS));
    UT_ASSERT(!serverSimPingReachesClient(NULL, 0, 0));

    serverSimDestroy(sim);
    return 0;
}
