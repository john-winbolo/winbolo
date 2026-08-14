/*
 * Game-over resolution: what the returning lobby is told, and who
 * WinBolo.net credits with the win.
 *
 * serverSimResolveGameOver is the single policy point for both. It runs at
 * the running->gameOver transition and switches on returnToLobbyReason, the
 * field whoever armed the countdown set:
 *
 *   BASE_WIN / NONE  -> the base-sweep winner line + a WBN win event per
 *                       player in the winning alliance.
 *   SURRENDER        -> the surrender line plus the opposing side's names,
 *                       and a WBN win event per opposing-team player.
 *   MANUAL_VOTE      -> "*** Players voted to return to the lobby. ***" and
 *                       no WBN crediting: a round ended by vote has no
 *                       winner, whatever the map looked like at the end.
 *   ABANDONED        -> no lobby line and no crediting at all.
 *
 * The tick does NOT call this. The dedicated-server lifecycle calls it when
 * it observes the state change (server_lifecycle.c), so every test here
 * ticks the round into game-over and then calls serverSimResolveGameOver
 * itself — exactly the sequence production runs.
 *
 * The BASE_WIN arm is the regression guard: a swept round used to reach the
 * lobby with a blank winner line because the reason was not mapped back to
 * the base-sweep message.
 *
 * Crediting is observed through the WIN-event spy in test_stubs.c
 * (wbnStubWinEventCalls / wbnStubWinEventMask). Those globals live for the
 * whole test-binary run, so every test zeroes them immediately before the
 * resolve it measures. Base state is poked directly on the GameSim (the
 * unittests profile permits T2-internal access).
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* returnToLobbyTicks / Reason / TeamId,
                                    * pendingWinMessage, botMgr,
                                    * serverSimGameVoteToggle */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled / serverSimSetTeam */
#include "control_event.h"
#include "client_sim.h"            /* GAME_VOTE_KIND_* / GAME_VOTE_TOGGLE_* */
#include "game_sim.h"              /* GameSim.bs */
#include "bases.h"                 /* basesGetNumBases, MIN_ARMOUR_CAPTURE */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* WIN-event spy from test_stubs.c. */
extern int      wbnStubWinEventCalls;
extern uint16_t wbnStubWinEventMask;

/* playersIsAllie(p, p) is TRUE, so one player owning every base is a valid
 * sweep — no alliance setup needed. */
#define RG_WINNER_SLOT 0
#define RG_WINNER_NAME "Sweeper"
#define RG_LOSER_SLOT  1
#define RG_LOSER_NAME  "Defender"

/* Armour comfortably above MIN_ARMOUR_CAPTURE (9) — a held base. */
#define RG_ARMOUR_HELD 50
/* Armour at or below MIN_ARMOUR_CAPTURE — a dead, recapturable base. */
#define RG_ARMOUR_DEAD 5

/* The return-to-lobby countdown is 700 game ticks and serverSimTick runs two
 * half-steps while running, so ~350 calls expire it. The guard is generous
 * enough that a real expiry always lands inside it and a countdown that
 * never expires still terminates the loop instead of hanging the suite. */
#define RG_TICK_GUARD 1200

/* ----------------------------------------------------------------
 * Counting subscriber.
 * ---------------------------------------------------------------- */

typedef struct {
    int  serverTextCount;   /* CTRL_SERVER_TEXT seen */
    char lastText[512];     /* text of the most recent CTRL_SERVER_TEXT */
} RgCounter;

static void rg_count_events(void *ctx, const ControlEvent *evt) {
    RgCounter *c = (RgCounter *)ctx;
    if (evt->type == CTRL_SERVER_TEXT) {
        c->serverTextCount++;
        snprintf(c->lastText, sizeof(c->lastText), "%s", evt->u.serverText.text);
    }
}

/* ----------------------------------------------------------------
 * Fixtures.
 * ---------------------------------------------------------------- */

/* A running, lobby-enabled server with no players yet — each test builds the
 * roster it needs. StartGame forces the running phase regardless of the
 * lobby flag; players join the running round afterwards. */
static ServerSim *rg_make_running_sim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, 5097, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimSetQuitOnWin(sim, false);
    serverSimStartGame(sim);
    return sim;
}

/* Give every base the same owner and armour. item[] is 0-based for the
 * 1-based base numbers the sweep check walks. */
static void rg_set_all_bases(ServerSim *sim, BYTE owner, BYTE armour) {
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
static SubscriberHandle rg_subscribe(ServerSim *sim, RgCounter *c) {
    SubscriberHandle h;
    memset(c, 0, sizeof(*c));
    h = serverSimRegisterSubscriber(sim, rg_count_events, c);
    memset(c, 0, sizeof(*c));
    return h;
}

/* Tick the running round until it leaves the running state. Returns FALSE if
 * the guard ran out first, so callers can fail loudly instead of asserting
 * against a round that never ended. */
static bool rg_tick_to_game_over(ServerSim *sim) {
    int i;
    for (i = 0; i < RG_TICK_GUARD && serverSimGetState(sim) == serverStateRunning;
         i++) {
        serverSimTick(sim);
    }
    return serverSimGetState(sim) != serverStateRunning;
}

/* Both spy globals are process-wide; clear them right before the resolve
 * (or tick) whose crediting is under test. */
static void rg_reset_win_spy(void) {
    wbnStubWinEventCalls = 0;
    wbnStubWinEventMask  = 0;
}

/* ================================================================
 * 1. A base-win countdown that expires reports the winner.
 *
 * The direct regression guard: RETURN_REASON_BASE_WIN must resolve through
 * the base-sweep message and crediting, not fall out with an empty line.
 * ================================================================ */
int run_base_win_expiry_reports_the_winner(void) {
    ServerSim *sim = rg_make_running_sim();

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, RG_WINNER_SLOT, RG_WINNER_NAME, false);
    serverSimSetTeam(sim, RG_WINNER_SLOT, 1);
    serverSimAddPlayer(sim, RG_LOSER_SLOT, RG_LOSER_NAME, false);
    serverSimSetTeam(sim, RG_LOSER_SLOT, 2);

    rg_set_all_bases(sim, RG_WINNER_SLOT, RG_ARMOUR_HELD);

    UT_ASSERT_MSG(rg_tick_to_game_over(sim),
                  "the sweep's countdown never expired within %d ticks "
                  "(%d countdown ticks left, reason %u)",
                  RG_TICK_GUARD, (int)sim->returnToLobbyTicks,
                  (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "an expired countdown must land in game-over, state %d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_BASE_WIN,
                  "the round must reach game-over as a base win (%d), got %u",
                  RETURN_REASON_BASE_WIN, (unsigned)sim->returnToLobbyReason);

    rg_reset_win_spy();
    serverSimResolveGameOver(sim);

    UT_ASSERT_MSG(sim->pendingWinMessage[0] != '\0',
                  "REGRESSION: a swept round left the returning lobby a BLANK "
                  "winner line — RETURN_REASON_BASE_WIN must resolve through "
                  "the base-sweep win message");
    UT_ASSERT_MSG(strstr(sim->pendingWinMessage, "Game Won!") != NULL,
                  "the lobby line must announce the win, got \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG(strstr(sim->pendingWinMessage, RG_WINNER_NAME) != NULL,
                  "the lobby line must name the winner, got \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG((wbnStubWinEventMask & (1u << RG_WINNER_SLOT)) != 0,
                  "the sweeping owner must be credited with a WinBolo.net "
                  "win, mask 0x%04x after %d win event(s)",
                  (unsigned)wbnStubWinEventMask, wbnStubWinEventCalls);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 2. A passed manual vote is irrevocable — losing a base doesn't abort it.
 *
 * Only a win-driven countdown is abortable (the sweep can be put back in
 * doubt). A back-to-lobby vote has already passed, so the round is going
 * back to the lobby whatever happens to the bases in the meantime.
 * ================================================================ */
int run_manual_vote_countdown_survives_lost_base(void) {
    ServerSim *sim = rg_make_running_sim();
    GameSim *gs;
    RgCounter c;
    int i;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, RG_WINNER_SLOT, RG_WINNER_NAME, false);
    serverSimSetTeam(sim, RG_WINNER_SLOT, 1);
    serverSimAddPlayer(sim, RG_LOSER_SLOT, RG_LOSER_NAME, false);
    serverSimSetTeam(sim, RG_LOSER_SLOT, 2);
    UT_ASSERT(rg_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    /* Stage the state a passed back-to-lobby vote leaves behind, rather than
     * driving the vote — the countdown fields are what this test is about. */
    rg_set_all_bases(sim, RG_WINNER_SLOT, RG_ARMOUR_HELD);
    sim->returnToLobbyReason = RETURN_REASON_MANUAL_VOTE;
    sim->returnToLobbyTicks  = 700;

    for (i = 0; i < 3; i++) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "setup: the staged countdown expired far too early (%d left)",
                  (int)sim->returnToLobbyTicks);

    /* Shell one base at or below MIN_ARMOUR_CAPTURE — the sweep no longer
     * holds. A BASE_WIN countdown would abort here. */
    gs = serverSimGetGameSim(sim);
    UT_ASSERT(gs != NULL);
    (*gs->bs).item[0].armour = RG_ARMOUR_DEAD;

    memset(&c, 0, sizeof(c));
    rg_reset_win_spy();
    serverSimTick(sim);

    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "a passed vote's countdown must keep running when the sweep "
                  "breaks, %d ticks left", (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_MANUAL_VOTE,
                  "the countdown reason must stay RETURN_REASON_MANUAL_VOTE "
                  "(%d), got %u",
                  RETURN_REASON_MANUAL_VOTE, (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "a lost base must not announce anything during a vote "
                  "countdown, got %d announcement(s) (\"%s\")",
                  c.serverTextCount, c.lastText);
    UT_ASSERT_MSG(strcmp(c.lastText, "*** The round continues. ***") != 0,
                  "the abort announcement belongs to a base-win countdown "
                  "only — a passed vote cannot be called off");

    /* The countdown still ends the round, and it ends it as a vote. */
    UT_ASSERT_MSG(rg_tick_to_game_over(sim),
                  "the vote countdown never expired within %d ticks (%d left)",
                  RG_TICK_GUARD, (int)sim->returnToLobbyTicks);
    rg_reset_win_spy();
    serverSimResolveGameOver(sim);
    UT_ASSERT_MSG(strcmp(sim->pendingWinMessage,
                         "*** Players voted to return to the lobby. ***") == 0,
                  "the lobby must be told the vote ended the round, got \"%s\"",
                  sim->pendingWinMessage);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 3. A surrender credits the opposing team.
 *
 * Driven through the real vote so returnToLobbyTeamId is set by production
 * code. Two humans on the surrendering team make the pass threshold 2, which
 * fires immediately (the 5-second "am I sure?" grace is solo-voter only).
 * ================================================================ */
int run_surrender_credits_the_opposing_team(void) {
    ServerSim *sim = rg_make_running_sim();

    UT_ASSERT(sim != NULL);
    /* Slots 0 and 1 give up; slot 2 is the whole opposing side. */
    serverSimAddPlayer(sim, 0, "Quitter", false);
    serverSimSetTeam(sim, 0, 1);
    serverSimAddPlayer(sim, 1, "Sidekick", false);
    serverSimSetTeam(sim, 1, 1);
    serverSimAddPlayer(sim, 2, "Holdout", false);
    serverSimSetTeam(sim, 2, 2);
    UT_ASSERT_MSG(serverSimCountActiveTeams(sim) == 2,
                  "setup: a surrender needs exactly two active teams, got %u",
                  (unsigned)serverSimCountActiveTeams(sim));

    serverSimGameVoteToggle(sim, 0, GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_OPEN_ONLY);
    serverSimGameVoteToggle(sim, 1, GAME_VOTE_KIND_SURRENDER,
                            GAME_VOTE_TOGGLE_YES);

    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_SURRENDER,
                  "the passed surrender must arm a SURRENDER countdown (%d), "
                  "got %u",
                  RETURN_REASON_SURRENDER, (unsigned)sim->returnToLobbyReason);
    UT_ASSERT_MSG(sim->returnToLobbyTeamId == 1,
                  "the surrendering team must be recorded as 1, got %u",
                  (unsigned)sim->returnToLobbyTeamId);
    UT_ASSERT_MSG(sim->returnToLobbyTicks > 0,
                  "the passed surrender must arm the countdown, got %d",
                  (int)sim->returnToLobbyTicks);

    UT_ASSERT_MSG(rg_tick_to_game_over(sim),
                  "the surrender countdown never expired within %d ticks "
                  "(%d left)", RG_TICK_GUARD, (int)sim->returnToLobbyTicks);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_SURRENDER,
                  "the reason must survive to game-over, got %u",
                  (unsigned)sim->returnToLobbyReason);

    rg_reset_win_spy();
    serverSimResolveGameOver(sim);

    UT_ASSERT_MSG(strstr(sim->pendingWinMessage, "surrendered") != NULL,
                  "the lobby line must explain the surrender, got \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG(strstr(sim->pendingWinMessage, "Holdout") != NULL,
                  "the lobby line must name the surviving side, got \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG((wbnStubWinEventMask & (1u << 2)) != 0,
                  "the opposing team must be credited with the win, mask "
                  "0x%04x", (unsigned)wbnStubWinEventMask);
    UT_ASSERT_MSG((wbnStubWinEventMask & ((1u << 0) | (1u << 1))) == 0,
                  "the surrendering team must NOT be credited, mask 0x%04x",
                  (unsigned)wbnStubWinEventMask);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 4. A sweep during a vote countdown still resolves as a vote.
 *
 * The vote owns the countdown already, so the sweep neither announces nor
 * arms a second one — and the round it ends has no winner to credit, even
 * though one side held the map when the clock ran out.
 * ================================================================ */
int run_win_during_manual_countdown_resolves_as_vote(void) {
    ServerSim *sim = rg_make_running_sim();
    RgCounter c;
    int32_t prevTicks;
    int i;

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, RG_WINNER_SLOT, RG_WINNER_NAME, false);
    serverSimSetTeam(sim, RG_WINNER_SLOT, 1);
    serverSimAddPlayer(sim, RG_LOSER_SLOT, RG_LOSER_NAME, false);
    serverSimSetTeam(sim, RG_LOSER_SLOT, 2);
    UT_ASSERT(rg_subscribe(sim, &c) != SUBSCRIBER_HANDLE_INVALID);

    sim->returnToLobbyReason = RETURN_REASON_MANUAL_VOTE;
    sim->returnToLobbyTicks  = 700;

    /* The win lands while the vote's clock is already running. */
    rg_set_all_bases(sim, RG_WINNER_SLOT, RG_ARMOUR_HELD);

    memset(&c, 0, sizeof(c));
    prevTicks = sim->returnToLobbyTicks;
    for (i = 0; i < 5; i++) {
        serverSimTick(sim);
        UT_ASSERT_MSG(sim->returnToLobbyTicks < prevTicks,
                      "the vote countdown must keep draining (tick %d: %d -> "
                      "%d) — a sweep must not re-arm it",
                      i, (int)prevTicks, (int)sim->returnToLobbyTicks);
        prevTicks = sim->returnToLobbyTicks;
    }
    UT_ASSERT_MSG(c.serverTextCount == 0,
                  "a sweep under a running vote countdown must not announce, "
                  "got %d announcement(s) (\"%s\")",
                  c.serverTextCount, c.lastText);
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_MANUAL_VOTE,
                  "the sweep must not take the countdown over, reason %u",
                  (unsigned)sim->returnToLobbyReason);

    UT_ASSERT_MSG(rg_tick_to_game_over(sim),
                  "the vote countdown never expired within %d ticks (%d left)",
                  RG_TICK_GUARD, (int)sim->returnToLobbyTicks);

    rg_reset_win_spy();
    serverSimResolveGameOver(sim);

    UT_ASSERT_MSG(strcmp(sim->pendingWinMessage,
                         "*** Players voted to return to the lobby. ***") == 0,
                  "a round ended by vote must report the vote, got \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG(wbnStubWinEventCalls == 0,
                  "a round ended by vote credits nobody even with the map "
                  "swept, got %d win event(s) (mask 0x%04x)",
                  wbnStubWinEventCalls, (unsigned)wbnStubWinEventMask);

    serverSimDestroy(sim);
    return 0;
}

/* ================================================================
 * 5. An abandoned round reports nothing.
 *
 * The last human leaving ends the round with RETURN_REASON_ABANDONED. Even
 * with a connected player holding every base — a bot here, so the round
 * counts as humanless while the win predicate would still fire — nobody won
 * it, so there is no lobby line and no crediting.
 * ================================================================ */
int run_abandoned_round_reports_nothing(void) {
    ServerSim *sim = rg_make_running_sim();

    UT_ASSERT(sim != NULL);
    serverSimAddPlayer(sim, 0, "Lastout", false);
    serverSimSetTeam(sim, 0, 1);
    /* Latch roundHadHuman — without it the humanless check never fires. */
    serverSimTick(sim);

    /* A bot holding the whole map: connected (so the sweep and its crediting
     * would find it) but invisible to serverSimGetNumHumans. */
    serverSimAddPlayer(sim, 1, "Botty", false);
    serverSimSetTeam(sim, 1, 2);
    sim->botMgr.bots[1].active = true;
    rg_set_all_bases(sim, 1, RG_ARMOUR_HELD);

    serverSimRemovePlayer(sim, 0);
    UT_ASSERT_MSG(serverSimGetNumHumans(sim) == 0,
                  "setup: the round must be humanless with the bot still "
                  "connected, got %u human(s)",
                  (unsigned)serverSimGetNumHumans(sim));

    rg_reset_win_spy();
    serverSimTick(sim);

    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateGameOver,
                  "the last human leaving must end the round, state %d",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(sim->returnToLobbyReason == RETURN_REASON_ABANDONED,
                  "an emptied round must be RETURN_REASON_ABANDONED (%d), "
                  "got %u",
                  RETURN_REASON_ABANDONED, (unsigned)sim->returnToLobbyReason);

    serverSimResolveGameOver(sim);

    UT_ASSERT_MSG(sim->pendingWinMessage[0] == '\0',
                  "an abandoned round leaves the lobby no line, got \"%s\"",
                  sim->pendingWinMessage);
    UT_ASSERT_MSG(wbnStubWinEventCalls == 0,
                  "an abandoned round credits nobody, got %d win event(s) "
                  "(mask 0x%04x)",
                  wbnStubWinEventCalls, (unsigned)wbnStubWinEventMask);

    serverSimDestroy(sim);
    return 0;
}
