/*
 * Server side of the smart ping (test_ping_dispatch.c).
 *
 * CMD_PING is the only command a player can fire that puts something on
 * everyone else's screen at a place of the sender's choosing, so the arm in
 * server_command_dispatch.c has more gates than most:
 *
 *   - a running game (a lobby has no map to point at),
 *   - a sender that actually holds a tank,
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

/* The last EVENT_PING the sim buffered this tick, or NULL. */
static const GameEvent *pd_last_ping(ServerSim *sim) {
    const GameEvent *evs = serverSimGetEvents(sim);
    int n = (int)serverSimGetEventCount(sim);
    int i;
    for (i = n - 1; i >= 0; i--) {
        if (evs[i].type == EVENT_PING) return &evs[i];
    }
    return NULL;
}

static void pd_clear_events(ServerSim *sim) {
    sim->eventCount = 0;
}

int run_ping_dispatch_accepts_and_builds_event(void) {
    ServerSim *sim = ut_make_running_sim("Pinger");
    const GameEvent *ev;
    UT_ASSERT_MSG(sim != NULL, "ut_make_running_sim returned NULL");

    pd_clear_events(sim);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_ATTACK, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_OK,
                  "a running game with a live tank must accept a ping");

    ev = pd_last_ping(sim);
    UT_ASSERT_MSG(ev != NULL, "no EVENT_PING was buffered");
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
    UT_ASSERT_MSG(pd_last_ping(sim) == NULL, "a refused ping emitted an event");

    serverSimDestroy(sim);
    return 0;
}

int run_ping_dispatch_rejects_tankless_and_spectator(void) {
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

    /* First one lands. */
    UT_ASSERT(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                  == CMD_OK);

    /* A second one on the same tick, and anywhere inside the minimum gap, is
     * a cooldown — a held key must not machine-gun the team's view. */
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "two pings on one tick must not both land");
    pd_advance(sim, PING_RATE_MIN_GAP_TICKS - 1);
    UT_ASSERT_MSG(pd_send(sim, 0, PING_KIND_STANDARD, PD_WORLD_X, PD_WORLD_Y)
                      == CMD_REJECT_COOLDOWN,
                  "just inside the minimum gap must still be a cooldown");

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
