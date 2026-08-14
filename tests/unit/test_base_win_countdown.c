/*
 * All-bases win countdown in the running tick (simRunHalfStep).
 *
 * When one side holds every base on a lobby-enabled server the round does
 * not end on the spot: the server announces the sweep and arms a 7-second
 * return-to-lobby countdown (returnToLobbyTicks, reason
 * RETURN_REASON_BASE_WIN). The countdown is abortable on purpose — a base
 * at or below MIN_ARMOUR_CAPTURE is dead and recapturable, so shelling one
 * base down (or losing it outright) puts the sweep back in doubt. When that
 * happens the countdown is cancelled, "*** The round continues. ***" goes
 * out and a BASE_WIN_REARM_TICKS cooldown blocks a re-announce, so an owner
 * oscillating around the capture threshold cannot flap the newswire and the
 * clients' 3/2/1.
 *
 * Pinned here:
 *   1. The announce arms the countdown and starts NO vote. This is the
 *      regression guard for the whole change — the previous implementation
 *      faked a unanimous back-to-lobby vote at this point, which is why the
 *      test asserts both zero CTRL_GAME_VOTE_STATE events and no running
 *      back-to-lobby vote.
 *   2. Breaking the sweep aborts the countdown mid-flight and resumes the
 *      round rather than ending it.
 *   3. The re-arm cooldown suppresses a second announcement until it expires.
 *   4. -quitonwin does NOT bypass the countdown when a lobby is configured:
 *      the two flags are independent and the lobby branch is tested first.
 *   5. Without a lobby, -quitonwin still ends the round instantly, with no
 *      countdown and no announcement, and serverSimResolveGameOver leaves no
 *      pending lobby message (winners reach stdout only).
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
#include "server_sim_internal.h"   /* returnToLobbyTicks / Reason, baseWinRearmTicks */
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
/* Armour at or below MIN_ARMOUR_CAPTURE — a dead, recapturable base. */
#define BW_ARMOUR_DEAD 5

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
 * 1. The announce arms the countdown and starts no vote.
 * ================================================================ */
int run_base_win_announces_and_publishes_no_vote_state(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ false);
    BwCounter c;
    SubscriberHandle h;

    UT_ASSERT(sim != NULL);
    h = bw_subscribe(sim, &c);
    UT_ASSERT(h != SUBSCRIBER_HANDLE_INVALID);

    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);

    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the sweep must publish exactly one announcement, got %d",
                  c.serverTextCount);
    UT_ASSERT_MSG(strstr(c.lastText, BW_WINNER_NAME) != NULL,
                  "announcement must name the winning owner, got \"%s\"",
                  c.lastText);
    UT_ASSERT_MSG(strstr(c.lastText, "control every base") != NULL,
                  "announcement must say the side controls every base, got "
                  "\"%s\"", c.lastText);

    /* serverSimTick runs two half-steps while running: the first arms the
     * countdown at 700, the later drain in that same half-step and the
     * second half-step take it down — so this is 698 here, not 700. The
     * contract is "armed", not an exact tick value. */
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0 && sim->returnToLobbyTicks <= 700,
                  "sweep must arm the return-to-lobby countdown, got %d",
                  (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "countdown reason must be RETURN_REASON_BASE_WIN (%d), got %u",
                  RETURN_REASON_BASE_WIN, (unsigned)sim->returnToLobbyReason);

    /* Regression guard for this change: the all-bases win announces and
     * counts down on its own. It must NOT fabricate a vote — the old code
     * faked a unanimous back-to-lobby vote here, which showed players a
     * vote they never cast and could not answer. */
    UT_ASSERT_MSG(c.voteStateCount == 0,
                  "REGRESSION: the all-bases win published %d "
                  "CTRL_GAME_VOTE_STATE event(s) — it must announce and count "
                  "down without faking a vote", c.voteStateCount);
    UT_ASSERT_MSG(serverSimGameVoteIsRunning(sim, GAME_VOTE_KIND_BACK_TO_LOBBY)
                      == false,
                  "REGRESSION: the all-bases win started a back-to-lobby vote "
                  "— the countdown replaced the auto-vote entirely");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. Breaking the sweep aborts the countdown and resumes the round.
 * ================================================================ */
int run_base_win_countdown_aborts_when_sweep_breaks(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ false);
    GameSim *gs;
    BwCounter c;
    int i;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "setup: the sweep did not arm the countdown");

    /* Countdown is long (7 seconds); a few frames in it is still running. */
    for (i = 0; i < 3; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "setup: countdown expired far too early (%d left)",
                  (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "setup: countdown reason changed while it was running");

    /* Shell one base at or below MIN_ARMOUR_CAPTURE — it is dead and
     * recapturable, so the sweep no longer holds. */
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    (*gs->bs).item[0].armour = BW_ARMOUR_DEAD;

    memset(&c, 0, sizeof(c));
    serverSimTick(sim);

    UT_ASSERT_MSG(sim->returnToLobbyTicks == 0,
                  "a broken sweep must cancel the countdown, %d ticks left",
                  (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_NONE,
                  "cancelled countdown must clear the reason, got %u",
                  (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the abort must publish exactly one announcement, got %d",
                  c.serverTextCount);
    UT_ASSERT_MSG(strcmp(c.lastText, "*** The round continues. ***") == 0,
                  "abort announcement text changed, got \"%s\"", c.lastText);
    UT_ASSERT_MSG(sim->baseWinRearmTicks > 0,
                  "the abort must arm the re-announce cooldown, got %d",
                  (int)sim->baseWinRearmTicks);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "an aborted countdown resumes the round — it must not end it");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. The re-arm cooldown stops announcement flapping.
 * ================================================================ */
int run_base_win_rearm_cooldown_limits_announcements(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ false);
    GameSim *gs;
    BwCounter c;
    int i;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    /* Arm, then break the sweep — the abort starts the cooldown. */
    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "setup: the sweep did not arm the countdown");
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    (*gs->bs).item[0].armour = BW_ARMOUR_DEAD;
    serverSimTick(sim);
    UT_ASSERT_MSG(sim->baseWinRearmTicks > 0,
                  "setup: the abort did not arm the cooldown");

    /* The owner immediately holds every base again — the flapping case. */
    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    memset(&c, 0, sizeof(c));

    /* Well inside the cooldown: the cooldown drains once per half-step, i.e.
     * twice per serverSimTick, so 100 frames burn 200 of BASE_WIN_REARM_TICKS. */
    for (i = 0; i < 100; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "the cooldown must suppress a re-announce, got %d "
                  "announcement(s) with %d cooldown ticks left",
                  c.serverTextCount, (int)sim->baseWinRearmTicks);
    UT_ASSERT_MSG(sim->returnToLobbyTicks == 0,
                  "no countdown may re-arm during the cooldown, got %d",
                  (int)sim->returnToLobbyTicks);

    /* Past the cooldown the sweep announces again — exactly once. Bounded by
     * BASE_WIN_REARM_TICKS frames, which is more than the cooldown can need
     * (it drains two per frame) and stops well short of the 7-second
     * countdown expiring into game-over. */
    for (i = 0; i < BASE_WIN_REARM_TICKS && c.serverTextCount == 0; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "the sweep must re-announce exactly once after the cooldown, "
                  "got %d", c.serverTextCount);
    UT_ASSERT_MSG(strstr(c.lastText, "control every base") != NULL,
                  "re-announcement must be the all-bases line, got \"%s\"",
                  c.lastText);
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "the countdown must re-arm once the cooldown expires");
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "re-armed countdown reason must be RETURN_REASON_BASE_WIN, "
                  "got %u", (unsigned)sim->returnToLobbyReason);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. -quitonwin with a lobby still counts down.
 *
 * lobbyEnabled and quitOnWin are independent flags — -quitonwin does not
 * clear the lobby — so the real configuration reaches the tick with both
 * set. Testing quitOnWin first would end the round instantly here; the
 * lobby branch has to win.
 * ================================================================ */
int run_base_win_quitonwin_with_lobby_counts_down(void) {
    ServerSim *sim = bw_make_running_sim(/*lobbyEnabled*/ true,
                                         /*quitOnWin*/ true);
    BwCounter c;

    UT_ASSERT(sim != NULL);
    UT_ASSERT(bw_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    bw_set_all_bases(sim, BW_WINNER_SLOT, BW_ARMOUR_HELD);
    serverSimTick(sim);

    UT_ASSERT_MSG(c.serverTextCount == 1,
                  "-quitonwin with a lobby must still announce the sweep, "
                  "got %d announcement(s)", c.serverTextCount);
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "-quitonwin with a lobby must arm the countdown, got %d",
                  (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "countdown reason must be RETURN_REASON_BASE_WIN, got %u",
                  (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(serverSimGetState(sim) != serverStateGameOver,
                  "-quitonwin must not end a lobby round on the spot — the "
                  "lobby countdown owns the win");

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. -quitonwin without a lobby is instant.
 *
 * A no-lobby round has nowhere to count down to, so the win ends it
 * immediately with no announcement. serverSimResolveGameOver is a no-op
 * without a lobby, so no pending message is left behind either — winners
 * reach stdout only. This pins existing behaviour.
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
