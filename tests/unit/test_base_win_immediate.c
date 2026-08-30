/*
 * All-bases win in the running tick (simRunHalfStep).
 *
 * One alliance holding every base, with none of those bases dead, IS the win
 * condition — so the round ends on the spot. "Dead" is armour <=
 * MIN_ARMOUR_CAPTURE, the same test basesGetStatusNum uses to draw the X on
 * the status pane, and a dead base is recapturable.
 *
 * There is deliberately no grace period on top of that. The grace period is
 * the condition itself: a base shelled to 0 keeps the sweep false for the
 * whole time it takes to regenerate past MIN_ARMOUR_CAPTURE, and that is the
 * losing side's window to retake it. An earlier version armed a further
 * 7-second abortable countdown here, which only bought the server the right
 * to announce a win and then retract it.
 *
 * Pinned here:
 *   1. The sweep announces once and ends the round immediately, with no
 *      countdown armed and no vote fabricated. The vote assertions are the
 *      regression guard for the older implementation, which faked a unanimous
 *      back-to-lobby vote at this point.
 *   2. A single dead base blocks the win, however the rest of the map is
 *      owned — this is the comeback window, so it must hold the round open.
 *   3. The threshold is exact: MIN_ARMOUR_CAPTURE is still dead, one point
 *      above it wins. This pins the `<=` in serverSimWinningOwner against the
 *      `<=` in basesGetStatusNum, which is what makes the win condition and
 *      the X the player sees the same rule.
 *   4. -quitonwin with a lobby ends the round the same way, but as a lobby
 *      return that names the winner rather than the silent no-lobby path.
 *   5. Without a lobby, -quitonwin ends the round with no announcement, and
 *      serverSimResolveGameOver leaves no pending lobby message (winners
 *      reach stdout only). This pins existing behaviour.
 *
 * Everything is observed off a bare ServerSim plus one counting subscriber;
 * publishServerMessage delivers CTRL_SERVER_TEXT straight to subscribers, so
 * no transport or loopback harness is involved. Base state is poked directly
 * on the GameSim (the unittests profile permits T2-internal access).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* returnToLobbyTicks / Reason */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / serverSimSetTeam */
#include "control_event.h"
#include "client_sim.h"            /* GAME_VOTE_KIND_BACK_TO_LOBBY */
#include "game_sim.h"              /* GameSim.bs */
#include "bases.h"                 /* basesGetNumBases, MIN_ARMOUR_CAPTURE */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* The sweeping owner sits in slot 0. playersIsAllie(p, p) is TRUE, so one
 * player owning every base is a valid sweep — no alliance setup needed. */
#define BW_WINNER_SLOT 0
#define BW_WINNER_NAME "Sweeper"

/* Armour comfortably above MIN_ARMOUR_CAPTURE (9) — a held base. */
#define BW_ARMOUR_HELD 50
/* Armour below MIN_ARMOUR_CAPTURE — a dead, recapturable base. */
#define BW_ARMOUR_DEAD 5

/* Long enough for a lingering countdown to have expired into game-over, short
 * enough to stay well clear of BASE_TICKS_BETWEEN_REFUEL (1000, drained once
 * per half-step) so a base cannot regenerate out from under a test. */
#define BW_HOLD_FRAMES 60

/* ----------------------------------------------------------------
 * Counting subscriber.
 * ---------------------------------------------------------------- */

typedef struct {
    int  serverTextCount;   /* CTRL_SERVER_TEXT seen */
    int  voteStateCount;    /* CTRL_GAME_VOTE_STATE seen */
    char lastText[512];     /* text of the most recent CTRL_SERVER_TEXT */
} BwCounter;

static void bw_count_events(void *ctx, const ControlEvent *evt) {
    BwCounter *c = (BwCounter *)ctx;
    if (evt->type == CTRL_SERVER_TEXT) {
        c->serverTextCount++;
        snprintf(c->lastText, sizeof(c->lastText), "%s", evt->u.serverText.text);
    } else if (evt->type == CTRL_GAME_VOTE_STATE) {
        c->voteStateCount++;
    }
}

/* ----------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------- */

/* A running server with two players on opposing teams. lobbyEnabled and
 * quitOnWin are independent server flags, so both are callers' choice. */
static ServerSim *bw_make_running_sim(bool lobbyEnabled, bool quitOnWin) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, lobbyEnabled);
    serverSimSetQuitOnWin(sim, quitOnWin);
    /* StartGame forces the running phase regardless of the lobby flag; the
     * players join the running round afterwards, same as test_vote_surrender. */
    serverSimStartGame(sim);
    serverSimAddPlayer(sim, BW_WINNER_SLOT, BW_WINNER_NAME, false);
    serverSimSetTeam(sim, BW_WINNER_SLOT, 1);
    serverSimAddPlayer(sim, 1, "Defender", false);
    serverSimSetTeam(sim, 1, 2);
    return sim;
}

/* Give every base the same owner and armour. item[] is 0-based for the
 * 1-based base numbers the sweep check walks. */
static void bw_set_all_bases(ServerSim *sim, BYTE owner, BYTE armour) {
    GameSim *gs = serverSimGetGameSim(sim);
    BYTE n, i;
    if (gs == NULL) return;
    n = basesGetNumBases(&gs->bs);
    for (i = 0; i < n; i++) {
        (*gs->bs).item[i].owner  = owner;
        (*gs->bs).item[i].armour = armour;
    }
}

/* Register the counting subscriber and zero it. Registration replays current
 * server state to the new subscriber, so the counters are cleared afterwards
 * and measure only what happens next. */
static SubscriberHandle bw_subscribe(ServerSim *sim, BwCounter *c) {
    SubscriberHandle h;
    memset(c, 0, sizeof(*c));
    h = serverSimRegisterSubscriber(sim, bw_count_events, c);
    memset(c, 0, sizeof(*c));
    return h;
}

/* ================================================================
 * 1. The sweep announces and ends the round on the spot.
 * ================================================================ */
int run_base_win_ends_round_immediately(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ false);
    BwCounter c;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    h = bw_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the sweep must end the round on the tick it happens — "
                  "state %d after one tick", (int)serverSimGetState(sim));
    UT_ASSERT_MSG(sim->returnToLobbyTicks == 0,
                  "REGRESSION: the sweep armed a %d-tick countdown — a win is "
                  "immediate, the regen of a dead base is the only grace "
                  "period", (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "the round must end as a base win (%d) so the returning "
                  "lobby names the winner, got %u",
                  RETURN_REASON_BASE_WIN, (unsigned)sim->returnToLobbyReason);

    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the sweep must publish exactly one announcement, got %d",
                  c.serverTextCount);
    UT_ASSERT_MSG(strstr(c.lastText, BW_WINNER_NAME) != NULL,
                  "announcement must name the winning owner, got \"%s\"",
                  c.lastText);
    UT_ASSERT_MSG(strstr(c.lastText, "control every base") != NULL,
                  "announcement must say the side controls every base, got "
                  "\"%s\"", c.lastText);

    /* Regression guard: the all-bases win announces on its own. It must NOT
     * fabricate a vote — the old code faked a unanimous back-to-lobby vote
     * here, which showed players a vote they never cast and could not
     * answer. */
    UT_ASSERT_MSG(c.voteStateCount == 0,
                  "REGRESSION: the all-bases win published %d "
                  "CTRL_GAME_VOTE_STATE event(s) — it must end the round "
                  "without faking a vote", c.voteStateCount);
    UT_ASSERT_MSG(serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY)
                      == false,
                  "REGRESSION: the all-bases win started a back-to-lobby vote");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. One dead base blocks the win.
 *
 * This is the comeback window. A base shelled to or below
 * MIN_ARMOUR_CAPTURE shows the X and is recapturable, so the round must stay
 * open for however long that base takes to regenerate — no announcement, no
 * game over.
 * ================================================================ */
int run_base_win_dead_base_blocks_the_win(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ false);
    GameSim *gs;
    BwCounter c;
    int i;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    /* Whole map owned by one side, but one base is dead. */
    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    (*gs->bs).item[0].armour = BW_ARMOUR_DEAD;

    for (i = 0; i < BW_HOLD_FRAMES; i++) {
        serverSimTick(sim);
    }

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "a dead base is recapturable — the round must stay open, "
                  "state %d after %d ticks",
                  (int)serverSimGetState(sim), BW_HOLD_FRAMES);
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "no win may be announced while a base is dead, got %d "
                  "announcement(s) (\"%s\")", c.serverTextCount, c.lastText);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_NONE,
                  "no return-to-lobby reason may be set, got %u",
                  (unsigned)sim->returnToLobbyReason);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. The dead/held boundary is exactly MIN_ARMOUR_CAPTURE.
 *
 * serverSimWinningOwner treats armour <= MIN_ARMOUR_CAPTURE as no winner and
 * basesGetStatusNum draws the X on the same comparison. Pinning both sides of
 * the boundary here is what keeps the win condition and the status the player
 * is looking at from drifting apart.
 * ================================================================ */
int run_base_win_fires_when_dead_base_regenerates(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ false);
    GameSim *gs;
    BwCounter c;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    /* Exactly at the threshold: still dead, still no win. */
    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    (*gs->bs).item[0].armour = MIN_ARMOUR_CAPTURE;
    serverSimTick(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "armour == MIN_ARMOUR_CAPTURE (%d) is still dead — the round "
                  "must not end", MIN_ARMOUR_CAPTURE);
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "armour == MIN_ARMOUR_CAPTURE must not announce a win, got "
                  "%d announcement(s)", c.serverTextCount);

    /* One point of regen past it and the sweep holds. */
    (*gs->bs).item[0].armour = MIN_ARMOUR_CAPTURE + 1;
    serverSimTick(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "armour == MIN_ARMOUR_CAPTURE + 1 completes the sweep — the "
                  "round must end, state %d", (int)serverSimGetState(sim));
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the completed sweep must announce exactly once, got %d",
                  c.serverTextCount);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "the round must end as a base win (%d), got %u",
                  RETURN_REASON_BASE_WIN, (unsigned)sim->returnToLobbyReason);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. -quitonwin with a lobby still returns to the lobby.
 *
 * lobbyEnabled and quitOnWin are independent flags — -quitonwin does not
 * clear the lobby — so the real configuration reaches the tick with both set.
 * The lobby branch has to win: the round ends either way, but only the lobby
 * branch announces the sweep and sets the reason that names the winner.
 * ================================================================ */
int run_base_win_quitonwin_with_lobby_returns_to_lobby(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ true);
    BwCounter c;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "-quitonwin with a lobby must still end the round at once, "
                  "state %d", (int)serverSimGetState(sim));
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "-quitonwin with a lobby must still announce the sweep, "
                  "got %d announcement(s)", c.serverTextCount);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "the lobby branch owns the win — reason must be "
                  "RETURN_REASON_BASE_WIN (%d), got %u",
                  RETURN_REASON_BASE_WIN, (unsigned)sim->returnToLobbyReason);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. -quitonwin without a lobby is instant and silent.
 *
 * A no-lobby round has nowhere to return to, so the win ends it with no
 * announcement. serverSimResolveGameOver is a no-op without a lobby, so no
 * pending message is left behind either — winners reach stdout only. This
 * pins existing behaviour.
 * ================================================================ */
int run_base_win_nolobby_quitonwin_is_instant(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ false,
                                         /*quitOnWin*/ true);
    BwCounter c;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "a no-lobby -quitonwin win must end the round immediately");
    UT_ASSERT_MSG(sim->returnToLobbyTicks == 0,
                  "a no-lobby win must not arm a countdown, got %d",
                  (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "a no-lobby win must not announce a return to lobby, got "
                  "%d announcement(s) (\"%s\")", c.serverTextCount, c.lastText);

    serverSimResolveGameOver(sim);
    UT_ASSERT_MSG(sim->pendingWinMessage[0] == '\0',
                  "resolveGameOver is a no-op without a lobby — no pending "
                  "message may be built, got \"%s\"", sim->pendingWinMessage);

    serverSimDestroy(sim);
    return 0;
}
