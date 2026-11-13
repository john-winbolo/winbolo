/*
 * Lobby bots on a team the scenario holds seats for (fielded = false).
 *
 * A team the lobby template marks fielded = false has no ordinary bots: its
 * seats sit in the roster with no tank until a spawn fields them. These
 * cases pin that a host's Add Bot onto such a team, and a bot the host moves
 * onto it, become one of those held seats with the team's own brain and init
 * table, as the template's seating makes them. A held seat moved onto any
 * other team becomes an ordinary bot on the field. A scenario whose teams are
 * all fielded, and a lobby with no scenario, keep the ordinary rule.
 *
 * The template is built by hand and handed to the sim, as
 * test_lobby_template.c does, and the commands go through
 * serverSimApplyCommand under the threads mutex, as the host's clicks do.
 * The unit binary has no Lua brain, so the fixture brain in test_stubs.c is
 * armed for the ordinary bots.
 *
 * run_lobby_held_add_bot_seats_held  — Add Bot onto a held team that the
 *                                      host emptied makes a held seat with
 *                                      the team's brain and init, named or
 *                                      not, even where the scenario refuses
 *                                      extra teams, and writes the seat's
 *                                      name into the transport's name table
 * run_lobby_held_move_on_and_off     — a bot moved onto a held team is held;
 *                                      moved back off, it is an ordinary bot
 *                                      on the field under the same name,
 *                                      the transport's name table set both
 *                                      ways
 * run_lobby_held_all_fielded_unchanged
 *                                    — with every team fielded, and with no
 *                                      scenario, Add Bot and a move make
 *                                      ordinary bots as before
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "client_command.h"        /* CMD_LOBBY_ADD_BOT, CMD_TEAM_SET */
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers, seatBrain, seatInit,
                                    * scenarioPolicy */
#include "server_sim_lifecycle.h"  /* serverSimSetBotAiType / BrainPath */
#include "server_sim_scenario.h"   /* the template entry points */
#include "bot_manager.h"           /* botManagerIsBot */
#include "players.h"               /* playersGetPlayerName */
#include "transport_udp.h"         /* transportUdpServerSetBotName / GetPlayerName */
#include "server/sim/server_sim_shared.h" /* serverSimSetActive */
#include "everard_map.h"
#include "threads.h"
#include "test_harness.h"

#define HT_HELD    3     /* fielded = false, the Survival shape */
#define HT_FIELD   4     /* fielded */
#define HT_OTHER   5     /* in no template */

static char htBrain[512];
static const char kHtHeldBrain[] = "brains/ut_held_team/init.lua";

static bool htSetUp(void) {
    FILE *f;
    if (!utScratchPath(htBrain, sizeof(htBrain), "held_team_brain.lua")) {
        return false;
    }
    f = fopen(htBrain, "wb");
    if (f == NULL) return false;
    fputs("-- fixture\n", f);
    fclose(f);
    return true;
}

/* The host in slot 0, not ready, and a server that runs bots. */
static ServerSim *htSim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN,
                                               "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Host", false);
    sim->lobbyPlayers[0].ready = false;
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, htBrain);
    serverSimSetState(sim, serverStateLobby);
    return sim;
}

/* One held team of `held` seats with its own brain and init, and one
   fielded team with none seated. */
static void htTemplate(ScnLobbyTemplate *t, BYTE held, bool heldFielded) {
    memset(t, 0, sizeof(*t));
    t->numTeams = 2;
    t->teams[0].id      = HT_HELD;
    t->teams[0].bots    = held;
    t->teams[0].maxBots = 10;
    t->teams[0].fielded = heldFielded;
    SDL_strlcpy(t->teams[0].brain, heldFielded ? htBrain : kHtHeldBrain,
                sizeof(t->teams[0].brain));
    (void)scnTableSet(&t->teams[0].init, "role", "horde");
    t->teams[1].id      = HT_FIELD;
    t->teams[1].bots    = 0;
    t->teams[1].maxBots = 6;
    t->teams[1].fielded = true;
}

static CmdResult htApply(ServerSim *sim, const ClientCommand *cmd) {
    CmdResult r;
    threadsWaitForMutex();
    r = serverSimApplyCommand(sim, 0, cmd);
    threadsReleaseMutex();
    return r;
}

/* Add Bot with a name ("" for none) and a team; the slot it landed in, or
   -1. */
static int htAddBot(ServerSim *sim, const char *name, BYTE team) {
    ClientCommand cmd;
    bool          before[MAX_TANKS];
    int           i;
    size_t        n = strlen(name);

    for (i = 0; i < MAX_TANKS; i++) {
        before[i] = sim->playerConnected[i] ? true : false;
    }
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_LOBBY_ADD_BOT;
    cmd.cmdSeq = 1;
    cmd.u.lobbyAddBot.teamNumber = team;
    cmd.u.lobbyAddBot.nameLen    = (uint8_t)n;
    memcpy(cmd.u.lobbyAddBot.name, name, n);
    if (htApply(sim, &cmd) != CMD_OK) return -1;
    for (i = 0; i < MAX_TANKS; i++) {
        if (!before[i] && sim->playerConnected[i]) return i;
    }
    return -1;
}

static CmdResult htSetTeam(ServerSim *sim, BYTE slot, BYTE team) {
    ClientCommand cmd;
    memset(&cmd, 0, sizeof(cmd));
    cmd.type   = CMD_TEAM_SET;
    cmd.cmdSeq = 1;
    cmd.u.teamSet.slot = slot;
    cmd.u.teamSet.team = team;
    return htApply(sim, &cmd);
}

static void htName(ServerSim *sim, int slot, char *out, size_t cap) {
    out[0] = '\0';
    playersGetPlayerName(&sim->sim.plyrs, (BYTE)slot, out, cap, TRUE);
}

/* Put a stale name in every seat's row of the transport's name table, so a
   seat whose add does not write the table is caught. */
static void htStaleTransportNames(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        transportUdpServerSetBotName((BYTE)i, "Stale");
    }
}

/* Empty every row again, so the next test starts from a clean table. */
static void htClearTransportNames(void) {
    int i;
    for (i = 0; i < MAX_TANKS; i++) {
        transportUdpServerSetBotName((BYTE)i, "");
    }
}

/* The transport's name for a seat, read as the duplicate-name checks read
   it. */
static int htCheckTransportName(ServerSim *sim, int slot, const char *want,
                                const char *why) {
    const char *got;
    serverSimSetActive(sim);
    got = transportUdpServerGetPlayerName((BYTE)slot);
    serverSimSetActive(NULL);
    UT_ASSERT_MSG(got != NULL && strcmp(got, want) == 0,
                  "%s: transport name of seat %d is '%s', expected '%s'",
                  why, slot, (got != NULL) ? got : "(null)", want);
    return 0;
}

/* A held seat of the held team, as the template's seating makes one. */
static int htCheckHeld(ServerSim *sim, int slot, const char *why) {
    const char *v;
    UT_ASSERT_MSG(slot >= 0 && sim->playerConnected[slot],
                  "%s: no seat", why);
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].isBot,
                  "%s: seat %d is not a bot seat", why, slot);
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == HT_HELD,
                  "%s: seat %d is on team %d", why, slot,
                  (int)sim->lobbyPlayers[slot].teamNumber);
    UT_ASSERT_MSG(!sim->lobbyPlayers[slot].fielded,
                  "%s: seat %d is on the field", why, slot);
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].keepSeat,
                  "%s: seat %d is not one of the scenario's", why, slot);
    UT_ASSERT_MSG(!botManagerIsBot(sim, (BYTE)slot),
                  "%s: seat %d has a bot running in it", why, slot);
    UT_ASSERT_MSG(sim->sim.tanks[slot] == NULL,
                  "%s: seat %d has a tank", why, slot);
    UT_ASSERT_MSG(strcmp(sim->seatBrain[slot], kHtHeldBrain) == 0,
                  "%s: seat %d has brain '%s', expected the team's", why,
                  slot, sim->seatBrain[slot]);
    v = scnTableGet(&sim->seatInit[slot], "role");
    UT_ASSERT_MSG(sim->seatInit[slot].count == 1 && v != NULL &&
                  strcmp(v, "horde") == 0,
                  "%s: seat %d init has %u pairs, role '%s'", why, slot,
                  (unsigned)sim->seatInit[slot].count,
                  (v != NULL) ? v : "(absent)");
    return 0;
}

/* An ordinary bot on the field, as the lobby's Add Bot makes one. */
static int htCheckOrdinary(ServerSim *sim, int slot, BYTE team,
                           const char *why) {
    UT_ASSERT_MSG(slot >= 0 && sim->playerConnected[slot],
                  "%s: no seat", why);
    UT_ASSERT_MSG(botManagerIsBot(sim, (BYTE)slot),
                  "%s: seat %d has no bot running in it", why, slot);
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].fielded,
                  "%s: seat %d is not on the field", why, slot);
    UT_ASSERT_MSG(!sim->lobbyPlayers[slot].keepSeat,
                  "%s: seat %d is marked as the scenario's", why, slot);
    UT_ASSERT_MSG(sim->lobbyPlayers[slot].teamNumber == team,
                  "%s: seat %d is on team %d, expected %d", why, slot,
                  (int)sim->lobbyPlayers[slot].teamNumber, (int)team);
    UT_ASSERT_MSG(sim->seatBrain[slot][0] == '\0' &&
                  sim->seatInit[slot].count == 0,
                  "%s: seat %d kept a held seat's brain or init", why, slot);
    return 0;
}

/* The one decision a policy is asked here: no team nobody is on. */
static bool htNoExtraTeams(void *ctx) {
    (void)ctx;
    return false;
}

int run_lobby_held_add_bot_seats_held(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    ScenarioPolicy   pol;
    char             name[64];
    int              i, slot;

    UT_ASSERT(htSetUp());
    ut_brain_stub_arm(true);
    sim = htSim();
    UT_ASSERT(sim != NULL);
    htTemplate(&t, 3, false);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);

    /* The host takes every held seat out, as tonight's host did with the
       team's X button. */
    for (i = 0; i < MAX_TANKS; i++) {
        if (sim->playerConnected[i] && sim->lobbyPlayers[i].keepSeat) {
            serverSimRemoveBot(sim, (BYTE)i);
        }
    }

    /* And a scenario that keeps the round to its own teams, as Survival
       does: the held team is the scenario's own, so it is not refused. */
    memset(&pol, 0, sizeof(pol));
    pol.allowExtraTeams = htNoExtraTeams;
    sim->scenarioPolicy = &pol;
    htStaleTransportNames();

    slot = htAddBot(sim, "Extra", HT_HELD);
    UT_ASSERT_MSG(slot >= 0, "Add Bot onto the emptied held team failed");
    if (htCheckHeld(sim, slot, "named add") != 0) return 1;
    UT_ASSERT_MSG(!ut_brain_stub_made(slot),
                  "a brain was built for held seat %d", slot);
    htName(sim, slot, name, sizeof(name));
    UT_ASSERT_MSG(strcmp(name, "Extra") == 0,
                  "the held seat is named '%s', expected 'Extra'", name);
    if (htCheckTransportName(sim, slot, "Extra", "named add") != 0) return 1;

    /* No name: the template's own style, "Bot N". */
    slot = htAddBot(sim, "", HT_HELD);
    UT_ASSERT_MSG(slot >= 0, "unnamed Add Bot onto the held team failed");
    if (htCheckHeld(sim, slot, "unnamed add") != 0) return 1;
    {
        char want[16];
        SDL_snprintf(want, sizeof(want), "Bot %d", slot + 1);
        htName(sim, slot, name, sizeof(name));
        UT_ASSERT_MSG(strcmp(name, want) == 0,
                      "the held seat is named '%s', expected '%s'", name,
                      want);
        if (htCheckTransportName(sim, slot, want, "unnamed add") != 0) {
            return 1;
        }
    }

    htClearTransportNames();
    sim->scenarioPolicy = NULL;
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    return 0;
}

int run_lobby_held_move_on_and_off(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    char             name[64];
    int              slot;

    UT_ASSERT(htSetUp());
    ut_brain_stub_arm(true);
    sim = htSim();
    UT_ASSERT(sim != NULL);
    htTemplate(&t, 0, false);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);

    /* An ordinary bot on the fielded team: the humans' side. */
    slot = htAddBot(sim, "Mover", HT_FIELD);
    if (htCheckOrdinary(sim, slot, HT_FIELD, "add to fielded team") != 0) {
        return 1;
    }

    /* Onto the held team: one of its held seats. */
    htStaleTransportNames();
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_HELD) == CMD_OK);
    if (htCheckHeld(sim, slot, "moved onto held team") != 0) return 1;
    htName(sim, slot, name, sizeof(name));
    UT_ASSERT_MSG(strcmp(name, "Mover") == 0,
                  "the moved seat is named '%s', expected 'Mover'", name);
    if (htCheckTransportName(sim, slot, "Mover", "moved onto held team") !=
        0) {
        return 1;
    }

    /* The same move again changes nothing. */
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_HELD) == CMD_OK);
    if (htCheckHeld(sim, slot, "moved onto held team again") != 0) return 1;

    /* Back to the fielded team: an ordinary bot on the field. */
    htStaleTransportNames();
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_FIELD) == CMD_OK);
    if (htCheckOrdinary(sim, slot, HT_FIELD, "moved off held team") != 0) {
        return 1;
    }
    htName(sim, slot, name, sizeof(name));
    UT_ASSERT_MSG(strcmp(name, "Mover") == 0,
                  "the moved bot is named '%s', expected 'Mover'", name);
    if (htCheckTransportName(sim, slot, "Mover", "moved off held team") != 0) {
        return 1;
    }

    /* A held seat moved onto a team the template does not list is an
       ordinary bot too. */
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_HELD) == CMD_OK);
    if (htCheckHeld(sim, slot, "moved onto held team third time") != 0) {
        return 1;
    }
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_OTHER) == CMD_OK);
    if (htCheckOrdinary(sim, slot, HT_OTHER, "moved to unlisted team") != 0) {
        return 1;
    }

    /* A human moved onto the held team stays a human. */
    UT_ASSERT(htSetTeam(sim, 0, HT_HELD) == CMD_OK);
    UT_ASSERT_MSG(sim->playerConnected[0] && !sim->lobbyPlayers[0].isBot &&
                  sim->lobbyPlayers[0].teamNumber == HT_HELD,
                  "the host's seat changed when it moved onto the held team");

    htClearTransportNames();
    serverSimDestroy(sim);
    ut_brain_stub_arm(false);
    return 0;
}

int run_lobby_held_all_fielded_unchanged(void) {
    ServerSim       *sim;
    ScnLobbyTemplate t;
    int              slot;

    UT_ASSERT(htSetUp());
    ut_brain_stub_arm(true);

    /* Every team fielded: team 3 plays from the start. */
    sim = htSim();
    UT_ASSERT(sim != NULL);
    htTemplate(&t, 0, true);
    serverSimSetScenarioLobbyTemplate(sim, &t);
    serverSimScenarioSeatLobby(sim);
    slot = htAddBot(sim, "Plain", HT_HELD);
    if (htCheckOrdinary(sim, slot, HT_HELD, "fielded template add") != 0) {
        return 1;
    }
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_FIELD) == CMD_OK);
    if (htCheckOrdinary(sim, slot, HT_FIELD, "fielded template move") != 0) {
        return 1;
    }
    serverSimDestroy(sim);

    /* No scenario at all. */
    sim = htSim();
    UT_ASSERT(sim != NULL);
    slot = htAddBot(sim, "Plain", HT_HELD);
    if (htCheckOrdinary(sim, slot, HT_HELD, "no scenario add") != 0) {
        return 1;
    }
    UT_ASSERT(htSetTeam(sim, (BYTE)slot, HT_FIELD) == CMD_OK);
    if (htCheckOrdinary(sim, slot, HT_FIELD, "no scenario move") != 0) {
        return 1;
    }
    serverSimDestroy(sim);

    ut_brain_stub_arm(false);
    return 0;
}
