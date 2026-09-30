/*
 * Coverage for the CMD_LOBBY_RELOAD_SCENARIO case in
 * server_command_dispatch.c — the lobby's own version of the dedicated
 * server console's `reload`, for a host with no console.
 *
 * The sim cannot re-read a script itself: that belongs to the scenario
 * library and the dependency points one way. So the handler asks through a
 * callback the host registers, and these cases register one of their own
 * rather than standing up a Lua VM. What they pin is the handler's contract:
 * who may ask, when, what a lobby with no scenario answers, and that a
 * refusal never reaches the callback at all.
 *
 * run_lobby_reload_scenario_needs_host    — a non-host is refused
 * run_lobby_reload_scenario_needs_lobby   — outside a lobby it is refused
 * run_lobby_reload_scenario_no_scenario   — a plain map answers rather than
 *                                           crashing, and says so
 * run_lobby_reload_scenario_calls_back    — the host's callback runs, and
 *                                           what it answers is what the
 *                                           player is told
 * run_lobby_reload_scenario_cooldown      — one a second, refused before the
 *                                           callback
 *
 * A reload that fails answers CMD_OK: the line it sends carries the reason,
 * and a reject code on top of it would put a second, contentless toast in
 * front of the sender. The reject codes left are the ones with no line —
 * the authority and state refusals, and the cooldown.
 *
 * Drives serverSimApplyCommand directly, holding the threads mutex the
 * dispatcher asserts.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_RELOAD_SCENARIO, CmdResult */
#include "control_event.h"         /* CTRL_SERVER_TEXT — the line the sender
                                    * is sent */
#include "wire_limits.h"           /* PACKET_MAX_CHAT_MESSAGE — its length */
#include "everard_map.h"
#include "server_sim.h"
#include "server_sim_internal.h"
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "server_sim_scenario.h"   /* serverSimSetScenarioReload */
#include "threads.h"
#include "test_harness.h"

/* What the fixture callback was asked and what it answers back. */
typedef struct {
    int  calls;
    bool answer;
    char err[128];
} RsCtx;

/* Lines the sender was sent, counted where a client would read them: the
 * control bus, which both the wire path and the in-process one deliver
 * from. */
typedef struct {
    int  count;
    char last[PACKET_MAX_CHAT_MESSAGE + 1];
} RsText;

static void rsTextCb(void *ctx, const ControlEvent *evt) {
    RsText *t = (RsText *)ctx;

    if (evt->type != CTRL_SERVER_TEXT) return;
    t->count++;
    snprintf(t->last, sizeof(t->last), "%s", evt->u.serverText.text);
}

/* Registration replays the server's state to the new subscriber, so the
 * record is cleared afterwards and counts only what happens next. */
static void rsWatchText(ServerSim *sim, RsText *t) {
    memset(t, 0, sizeof(*t));
    (void)serverSimRegisterSubscriber(sim, rsTextCb, t);
    memset(t, 0, sizeof(*t));
}

/* Past the reload cooldown. The lobby advances ticks like any other state,
 * so a host waiting a second is exactly this many of them; a case that wants
 * to ask twice moves the clock rather than sleeping. */
static void rsPastCooldown(ServerSim *sim) {
    sim->tick += SCENARIO_RELOAD_GAP_TICKS;
}

static bool rsReload(void *ctx, char *err, size_t errLen) {
    RsCtx *c = (RsCtx *)ctx;

    c->calls++;
    if (!c->answer && err != NULL && errLen > 0) {
        snprintf(err, errLen, "%s", c->err);
    }
    return c->answer;
}

/* Host in slot 0 — lobbyClientMayEdit(sim, 0) is true — and a non-host
 * human in slot 1. */
static ServerSim *rsLobby(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    serverSimAddPlayer(sim, 1, "Bob", false);
    return sim;
}

static CmdResult rsApply(ServerSim *sim, int senderSlot) {
    ClientCommand cmd;
    CmdResult r;

    memset(&cmd, 0, sizeof(cmd));
    cmd.type = CMD_LOBBY_RELOAD_SCENARIO;
    cmd.cmdSeq = 1;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, senderSlot, &cmd);
    threadsReleaseMutex();
    return r;
}

/* ── Only the host may ask ────────────────────────────────────────── */

int run_lobby_reload_scenario_needs_host(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);

    UT_ASSERT_MSG(rsApply(sim, 1) == CMD_REJECT_NOT_HOST,
                  "a non-host's reload was not refused");
    UT_ASSERT_MSG(c.calls == 0,
                  "a refused reload reached the callback %d times", c.calls);

    /* And the host's does go through, so the refusal above is the authority
       check and not the command being inert. */
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK, "the host's reload was refused");
    UT_ASSERT_MSG(c.calls == 1,
                  "the host's reload reached the callback %d times, wanted 1",
                  c.calls);

    serverSimDestroy(sim);
    return 0;
}

/* ── And only in a lobby ──────────────────────────────────────────── */

int run_lobby_reload_scenario_needs_lobby(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);

    /* A server that runs no lobby at all has nowhere to ask from. */
    serverSimSetLobbyEnabled(sim, false);
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_BAD_STATE,
                  "a reload outside a lobby was not refused");
    UT_ASSERT_MSG(c.calls == 0,
                  "a reload refused for state reached the callback %d times",
                  c.calls);

    serverSimDestroy(sim);
    return 0;
}

/* ── A lobby on a plain map answers ───────────────────────────────── */

int run_lobby_reload_scenario_no_scenario(void) {
    ServerSim *sim = rsLobby();

    UT_ASSERT(sim != NULL);
    /* Nothing registered, which is every lobby on a map with no script. */
    UT_ASSERT_MSG(sim->scenarioReload == NULL,
                  "setup: this case wants a sim with no reload registered");

    /* CMD_OK, not a refusal: the reload did fail, and the sender is told
       why by the line the handler sends — a reject code on top of it would
       add a second toast saying only that something was wrong. */
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "a reload with no scenario attached answered a reject code "
                  "as well as the line that says why");

    /* And the sim is still usable afterwards — the point of the case is
       that asking is answered rather than fatal. */
    rsPastCooldown(sim);
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "asking twice did not answer the same way");
    UT_ASSERT_MSG(serverSimGetNumPlayers(sim) == 2,
                  "the roster changed across a failed reload");

    /* The helper the handler calls says why, for the line the player gets. */
    {
        char err[128];
        err[0] = 'x';
        UT_ASSERT_MSG(!serverSimScenarioReload(sim, err, sizeof(err)),
                      "a reload with nothing registered answered true");
        UT_ASSERT_MSG(err[0] != '\0' && err[0] != 'x',
                      "a reload with nothing registered left no reason");
    }

    serverSimDestroy(sim);
    return 0;
}

/* ── What the callback answers is what comes back ─────────────────── */

int run_lobby_reload_scenario_calls_back(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;
    RsText     text;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);
    rsWatchText(sim, &text);

    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "a reload the host's callback accepted was refused");
    UT_ASSERT(c.calls == 1);
    UT_ASSERT_MSG(text.count == 1,
                  "an accepted reload sent %d lines, wanted the one that says "
                  "what it changed", text.count);

    /* A script that will not read answers false with a reason. The handler
       sends that reason and answers CMD_OK: the sender has been told what
       went wrong, and a reject code would put a contentless toast on top of
       it. */
    c.answer = false;
    snprintf(c.err, sizeof(c.err), "scenario: wave.lua:3: unexpected symbol");
    rsPastCooldown(sim);
    memset(&text, 0, sizeof(text));
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "a reload the callback turned down answered a reject code "
                  "as well as the line that says why");
    UT_ASSERT_MSG(c.calls == 2,
                  "the callback ran %d times over two reloads", c.calls);
    UT_ASSERT_MSG(text.count == 1,
                  "a failed reload sent %d lines, wanted exactly one",
                  text.count);
    UT_ASSERT_MSG(strcmp(text.last, c.err) == 0,
                  "the line the sender got was \"%s\", wanted the callback's "
                  "own reason", text.last);

    /* Cleared the way a detach clears it: back to answering for itself, and
       still saying so rather than reaching a callback that is gone. */
    serverSimSetScenarioReload(sim, NULL, NULL);
    rsPastCooldown(sim);
    memset(&text, 0, sizeof(text));
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "a cleared reload answered a reject code");
    UT_ASSERT_MSG(c.calls == 2,
                  "a cleared reload still reached the callback");
    UT_ASSERT_MSG(text.count == 1,
                  "a cleared reload sent %d lines, wanted one saying there is "
                  "no scenario", text.count);

    serverSimDestroy(sim);
    return 0;
}

/* ── One reload a second ──────────────────────────────────────────── */

/* The read, the parse and the check behind a reload are disk and Lua work on
 * the thread the command arrives on, so a taken one holds the next off for a
 * second. The refusal happens before the callback, which is what says the
 * file was never opened. */
int run_lobby_reload_scenario_cooldown(void) {
    ServerSim *sim = rsLobby();
    RsCtx      c;
    RsText     text;

    UT_ASSERT(sim != NULL);
    memset(&c, 0, sizeof(c));
    c.answer = true;
    serverSimSetScenarioReload(sim, rsReload, &c);
    rsWatchText(sim, &text);

    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK, "the first reload was refused");
    UT_ASSERT(c.calls == 1);

    /* Straight away again, on the same tick. */
    {
        CmdResult r = rsApply(sim, 0);
        UT_ASSERT_MSG(r == CMD_REJECT_COOLDOWN,
                      "a second reload inside the cooldown answered %d, "
                      "wanted CMD_REJECT_COOLDOWN (%d)",
                      (int)r, (int)CMD_REJECT_COOLDOWN);
    }
    UT_ASSERT_MSG(c.calls == 1,
                  "a reload refused for the cooldown reached the callback: "
                  "%d calls, wanted the one from the first", c.calls);

    /* A tick short of the gap is still inside it. */
    sim->tick += SCENARIO_RELOAD_GAP_TICKS - 1;
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_REJECT_COOLDOWN,
                  "a reload one tick short of the gap was accepted");
    UT_ASSERT(c.calls == 1);

    /* And the tick the gap is up on is not. */
    sim->tick += 1;
    UT_ASSERT_MSG(rsApply(sim, 0) == CMD_OK,
                  "a reload after the cooldown was refused");
    UT_ASSERT_MSG(c.calls == 2,
                  "the reload after the cooldown reached the callback %d "
                  "times, wanted 2 in total", c.calls);

    /* Two accepted reloads, two lines; the refused ones sent none, because
       the toast the reject code raises is what tells the sender about
       those. */
    UT_ASSERT_MSG(text.count == 2,
                  "the sender was sent %d lines over two accepted and two "
                  "refused reloads, wanted 2", text.count);

    serverSimDestroy(sim);
    return 0;
}
