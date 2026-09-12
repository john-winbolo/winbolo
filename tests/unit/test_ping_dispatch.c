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
 *
 * The mute has a client half too — the bit the players panel draws from — and
 * the last test here is that one, because it only means anything against the
 * server sweep the test above it pins: both sides have to forget a departing
 * slot's mute or a recycled slot lies about who is muted.
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
#include "client_sim.h"            /* the client half of the mute mirror */
#include "client_sim_internal.h"   /* the render-limiter ring behind that mute */
#include "client_sim_control.h"    /* clientSimApplyControl */
#include "control_event.h"         /* CTRL_PLAYER_LEAVE */
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
    pd_advance(sim, PING_SPAM_WINDOW_30S_TICKS);
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

    /* Spend the 5s allowance in the round that is ending, so the ring holds
     * stamps whose oldest is the round's first tick. That is the state that
     * goes wrong: the rate limiter stamps sim->tick, and the next round
     * rewinds sim->tick to 0, so a stamp left behind sits in the new round's
     * future — the arm then measures the new round's first ping against a ping
     * sent in the last one and calls it a cooldown. */
    for (i = 0; i < PING_SPAM_MAX_5S; i++) {
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the previous round's allowance was refused", i);
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

    /* And the fresh allowance is a whole 5s window, not the remains of one. */
    pd_clear_events(sim);
    for (i = 1; i < PING_SPAM_MAX_5S; i++) {
        pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the new round's allowance was refused", i + 1);
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

/* The arm's map-range check, and why no ping can fail it today.
 *
 * CmdPing carries worldX/worldY as uint16_t, and a world unit is 1/256th of a
 * map square (M_W_SHIFT_SIZE), so the largest coordinate a client can express,
 * 0xFFFF, is square 255 — the last square of a 256x256 map (MAP_ARRAY_SIZE).
 * The whole u16 range is therefore on the map and the range check can never
 * fire: there is no off-map value to send. That is what this test pins. The
 * corner is accepted on both axes, and the arithmetic the guard performs is
 * asserted directly over the extremes, so if either constant moves — a wider
 * coordinate, a bigger world unit, a smaller map — this test fails and says
 * that an off-map ping has become reachable and now needs a reject case. */
int run_ping_dispatch_map_range_bound(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    UT_ASSERT(sim != NULL);
    pd_clear_events(sim);

    /* The guard's own expression, over the widest coordinate a CmdPing can
     * hold. Computed in unsigned int so a wider future field still overflows
     * the map rather than the type. */
    UT_ASSERT_MSG((((unsigned)UINT16_MAX) >> M_W_SHIFT_SIZE) < MAP_ARRAY_SIZE,
                  "the largest CmdPing coordinate now lands off the map: "
                  "square %u of %d. The dispatch arm's range check has become "
                  "reachable and needs a rejection test of its own.",
                  ((unsigned)UINT16_MAX) >> M_W_SHIFT_SIZE, MAP_ARRAY_SIZE);

    /* And the last valid square really is accepted, on X and on Y, so the
     * check is not quietly refusing the map's own edge. Each send needs its
     * own cooldown window. */
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, UINT16_MAX, PD_WORLD_Y)
                      == CMD_OK,
                  "the last valid square on X was refused");
    UT_ASSERT_MSG(pd_has_pending(sim, 0), "an accepted ping was not recorded");
    pd_clear_events(sim);

    pd_advance(sim, PING_SPAM_WINDOW_30S_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, UINT16_MAX)
                      == CMD_OK,
                  "the last valid square on Y was refused");
    UT_ASSERT_MSG(pd_has_pending(sim, 0), "an accepted ping was not recorded");
    pd_clear_events(sim);

    pd_advance(sim, PING_SPAM_WINDOW_30S_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, UINT16_MAX, UINT16_MAX)
                      == CMD_OK,
                  "the map's far corner was refused");
    UT_ASSERT_MSG(pd_has_pending(sim, 0), "an accepted ping was not recorded");

    /* Nothing off-map to reject, but the origin must not be mistaken for one
     * either — 0 is square 0, a real square, not a "no position" sentinel. */
    pd_clear_events(sim);
    pd_advance(sim, PING_SPAM_WINDOW_30S_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, 0, 0) == CMD_OK,
                  "the map's origin was refused");
    UT_ASSERT_MSG(pd_has_pending(sim, 0), "an accepted ping was not recorded");

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
            pd_advance(sim, PING_SPAM_WINDOW_30S_TICKS);
            UT_ASSERT_MSG(pd_send(sim, 0, (uint8_t)k, PD_WORLD_X, PD_WORLD_Y)
                              == CMD_OK, "kind %d refused", k);
        }
    }

    serverSimDestroy(sim);
    return 0;
}

/* The minimum gap and the 5s window: a held key cannot machine-gun the view,
 * and no more than PING_SPAM_MAX_5S pings land inside any 5-second window. */
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

    /* Past the gap it lands again — up to the 5s cap. The first ping above
     * used one of the PING_SPAM_MAX_5S slots, so PING_SPAM_MAX_5S-1 more fit
     * before the 5s window closes. The min gap alone lets them through; the
     * window cap is what stops the next one. */
    for (i = 1; i < PING_SPAM_MAX_5S; i++) {
        pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the 5s allowance was refused", i + 1);
    }

    /* One past the 5s cap, still inside the window: refused even though the
     * minimum gap has passed. */
    pd_advance(sim, PING_RATE_MIN_GAP_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "the 5s cap must hold once the allowance is spent");
    pd_clear_events(sim);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "the 5s cap must hold on a retry too");
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "a capped ping was still queued for sending");

    /* Once the window has rolled past the oldest of the allowance, it comes
     * back. Stepping the whole 5s window clears every stamp out of it. */
    pd_advance(sim, PING_SPAM_WINDOW_5S_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_OK,
                  "the allowance must return once the 5s window has passed");

    serverSimDestroy(sim);
    return 0;
}

/* The 30s window: a spammer can keep the ping rate under the 5s cap by
 * spacing pings out, so the wider window is the backstop — no more than
 * PING_SPAM_MAX_30S land in any 30 seconds. Pings are spaced far enough apart
 * (PING_SPAM_30S_SPACING) that the 5s cap never fires, so the only thing that
 * can stop the run is the 30s cap itself. */
#define PING_SPAM_30S_SPACING  (PING_SPAM_WINDOW_5S_TICKS / PING_SPAM_MAX_5S + 1)

int run_ping_dispatch_spam_30s_window(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    int i;
    UT_ASSERT(sim != NULL);
    pd_clear_events(sim);

    /* PING_SPAM_MAX_30S land, one every PING_SPAM_30S_SPACING ticks. The
     * spacing keeps at most PING_SPAM_MAX_5S-1 inside any 5s window, so the
     * 5s cap never trips; the whole run fits inside the 30s window. */
    for (i = 0; i < PING_SPAM_MAX_30S; i++) {
        UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD,
                              PD_WORLD_X, PD_WORLD_Y) == CMD_OK,
                      "ping %d of the 30s allowance was refused", i);
        pd_clear_events(sim);
        pd_advance(sim, PING_SPAM_30S_SPACING);
    }

    /* The whole run so far spans (PING_SPAM_MAX_30S-1)*spacing ticks, well
     * inside the 30s window, so all PING_SPAM_MAX_30S stamps are still live —
     * one more is refused by the 30s cap even though the 5s window is clear. */
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "the 30s cap must hold once %d pings are inside the window",
                  PING_SPAM_MAX_30S);
    UT_ASSERT_MSG(!pd_has_pending(sim, 0),
                  "a 30s-capped ping was still queued for sending");

    /* Step the whole 30s window past the run and the allowance returns. */
    pd_advance(sim, PING_SPAM_WINDOW_30S_TICKS);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_OK,
                  "the allowance must return once the 30s window has passed");

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

/* Send one CMD_PLAYER_PING_MUTE through the real dispatcher, as pd_send does
 * for CMD_PING, so the arm's own guards (self-mute reject, range) are exercised
 * rather than calling serverSimSetPingMute directly. */
static CmdResult pd_send_ping_mute(ServerSim *sim, int senderSlot,
                                   uint8_t target, bool muted) {
    ClientCommand cmd;
    CmdResult r;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_PLAYER_PING_MUTE;
    cmd.cmdSeq = 1;
    cmd.u.playerPingMute.targetPlayer = target;
    cmd.u.playerPingMute.muted        = muted ? 1 : 0;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* The ping-mute relay skip: a recipient who has muted a sender does not
 * receive that sender's ping, through the SAME serverSimPingReachesClient the
 * team/ally filter uses, while the sender's own copy and the reverse direction
 * are untouched. Mirrors the voice mute, but sim-level (its mask lives on the
 * ServerSim, not the UDP transport). */
int run_ping_mute_relay_skip(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 1, "Mate", false);

    /* Both on one team so the baseline is "reaches". */
    threadsWaitForMutex();
    serverSimSetTeamBatch(sim, 0, 1);
    serverSimSetTeamBatch(sim, 1, 1);
    serverSimReapplyTeamAlliances(sim);
    threadsReleaseMutex();
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 1, 0),
                  "teammates must reach each other before any mute");

    /* Slot 1 mutes slot 0's pings. */
    UT_ASSERT_MSG(pd_send_ping_mute(sim, 1, 0, true) == CMD_OK,
                  "muting another player's pings must be accepted");
    UT_ASSERT_MSG((sim->pingMuteMask[1] & ((PlayerBitMap)1u << 0)) != 0,
                  "the mute bit did not set");
    UT_ASSERT_MSG(!serverSimPingReachesClient(sim, 1, 0),
                  "a muted sender's ping must not reach the muting recipient");
    /* The sender still sees its own, and the mute is one-directional. */
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 0, 0),
                  "the sender must still see its own ping while muted by others");
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 0, 1),
                  "the reverse direction must be unaffected by the mute");

    /* Unmute restores delivery. */
    UT_ASSERT_MSG(pd_send_ping_mute(sim, 1, 0, false) == CMD_OK,
                  "unmuting must be accepted");
    UT_ASSERT_MSG((sim->pingMuteMask[1] & ((PlayerBitMap)1u << 0)) == 0,
                  "the mute bit did not clear");
    UT_ASSERT_MSG(serverSimPingReachesClient(sim, 1, 0),
                  "delivery must return after an unmute");

    /* Muting yourself is meaningless and rejected — the bit must never set. */
    UT_ASSERT_MSG(pd_send_ping_mute(sim, 0, 0, true) == CMD_REJECT_INVALID,
                  "a self ping-mute must be rejected");
    UT_ASSERT_MSG((sim->pingMuteMask[0] & ((PlayerBitMap)1u << 0)) == 0,
                  "a rejected self-mute still set a bit");
    /* An out-of-range target is rejected before it can touch the mask. */
    UT_ASSERT_MSG(pd_send_ping_mute(sim, 0, MAX_TANKS, true) == CMD_REJECT_INVALID,
                  "an out-of-range ping-mute target must be rejected");

    serverSimDestroy(sim);
    return 0;
}

/* The client's own copy of the mute (cs->pingMutedByMe, read by the players
 * panel through clientSimIsPingMuted) has to be swept when a slot is released
 * exactly as the server sweeps pingMuteMask above. Slots are recycled, and a
 * mute belongs to the player who was in the slot: a bit left set would draw
 * the next occupant as "pings hidden" while the server — which cleared its
 * half on the leave — delivered their pings anyway, and the first click on
 * them would go out as a no-op unmute. CTRL_PLAYER_LEAVE is where the client
 * learns the slot is free (reliable, unlike the snapshot's EVENT_PLAYER_LEAVE),
 * so it is where the forgetting happens. */
int run_ping_mute_client_mirror_cleared_on_leave(void) {
    ClientSim *cs = clientSimAlloc();
    ControlEvent evt;
    int i;

    UT_ASSERT_MSG(cs != NULL, "clientSimAlloc returned NULL");
    clientSimCreate(cs);
    clientSimSetPlayerNum(cs, 0);

    /* Slot 3's pings are muted here, and slot 4's are not. */
    clientSimSetPingMuted(cs, 3, true);
    UT_ASSERT_MSG(clientSimIsPingMuted(cs, 3),
                  "the client-side mute bit did not set");

    /* Fill slot 3's render-limiter ring too: a slot vacated by a spammer must
     * not start its next occupant off already over the cap. */
    for (i = 0; i < PING_SPAM_MAX_30S; i++) {
        cs->pingRenderMs[3][i] = 1000u + (uint32_t)i;
    }
    cs->pingRenderIdx[3] = 2;

    /* Slot 3 leaves. */
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 3;
    evt.u.playerLeave.quiet = 1;   /* no newswire or lobby line wanted here */
    clientSimApplyControl(cs, &evt);

    UT_ASSERT_MSG(!clientSimIsPingMuted(cs, 3),
                  "a released slot kept its ping mute: the next occupant would "
                  "show as muted while their pings were delivered");
    for (i = 0; i < PING_SPAM_MAX_30S; i++) {
        UT_ASSERT_MSG(cs->pingRenderMs[3][i] == 0,
                      "a released slot kept its render-limiter timestamps");
    }
    UT_ASSERT_MSG(cs->pingRenderIdx[3] == 0,
                  "a released slot kept its render-limiter cursor");

    /* Nothing else moved: the leave forgets one slot, not the table. */
    clientSimSetPingMuted(cs, 4, true);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 3;
    evt.u.playerLeave.quiet = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimIsPingMuted(cs, 4),
                  "one slot leaving cleared another slot's mute");

    /* Self is skipped before the arm runs (a player cannot mute itself, so
     * there is nothing to clear), and an out-of-range slot is rejected at the
     * trust boundary — neither must touch the mask or crash. */
    clientSimSetPingMuted(cs, 4, true);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = 0;   /* == myPlayerNum */
    evt.u.playerLeave.quiet = 1;
    clientSimApplyControl(cs, &evt);
    memset(&evt, 0, sizeof(evt));
    evt.type = CTRL_PLAYER_LEAVE;
    evt.u.playerLeave.playerNum = MAX_TANKS;
    evt.u.playerLeave.quiet = 1;
    clientSimApplyControl(cs, &evt);
    UT_ASSERT_MSG(clientSimIsPingMuted(cs, 4),
                  "a self or out-of-range leave disturbed the mask");

    clientSimDestroy(cs);   /* frees cs itself */
    return 0;
}
