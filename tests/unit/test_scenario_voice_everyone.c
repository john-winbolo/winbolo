/*
 * Copyright (c) 1998-2026 John Morrison.
 *
 * This program is free software; you can redistribute it and/or modify
 * it under the terms of the GNU General Public License as published by
 * the Free Software Foundation; either version 3 of the License, or
 * (at your option) any later version.
 *
 * This program is distributed in the hope that it will be useful,
 * but WITHOUT ANY WARRANTY; without even the implied warranty of
 * MERCHANTABILITY or FITNESS FOR A PARTICULAR PURPOSE.  See the
 * GNU General Public License for more details.
 *
 * You should have received a copy of the GNU General Public License
 * along with this program.  If not, see <https://www.gnu.org/licenses/>.
 *
 * Voice to everyone, the scenario op behind game.set_voice_everyone, and
 * serverSimVoiceSidesAllow, the rule the server's voice forward asks for
 * every talker and recipient:
 *
 *   run_scenario_voice_everyone_default  — with no script, a running round
 *       sends voice to allies only and the lobby sends it to everyone
 *   run_scenario_voice_everyone_on       — the op turns it on, two seats on
 *       different teams hear each other, and off puts it back
 *   run_scenario_voice_everyone_resets   — the return to the lobby, a new
 *       round's start and a scenario detach each turn it off
 *   run_scenario_voice_everyone_voice_off — a server with voice off refuses
 *       the op's on, takes its off, and the getter answers false
 *
 * The round is two seats on team 1 and one on team 2, so 0 and 1 are allies
 * and 2 is nobody's.
 */

#include <stdbool.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* lobbyPlayers[].teamNumber */
#include "server_sim_scenario.h"   /* serverSimApplyScenarioOp */
#include "server_sim_lifecycle.h"  /* serverSimSetLobbyEnabled */
#include "scenario_defs.h"         /* ScenarioOp, SCN_OP_SET_VOICE_EVERYONE */
#include "everard_map.h"           /* E_MAP */
#include "test_harness.h"

/* A lobby with three seats, 0 and 1 on team 1 and 2 on team 2. */
static ServerSim *veLobbySim(void) {
    BYTE emap[6000] = E_MAP;
    ServerSim *sim = serverSimCreateCompressed(emap, E_MAP_LEN, "Everard Island",
                                               gameOpen, false, 0, -1);
    if (sim == NULL) return NULL;
    serverSimSetLobbyEnabled(sim, true);
    serverSimAddPlayer(sim, 0, "Zero", false);
    serverSimAddPlayer(sim, 1, "One", false);
    serverSimAddPlayer(sim, 2, "Two", false);
    sim->lobbyPlayers[0].teamNumber = 1;
    sim->lobbyPlayers[1].teamNumber = 1;
    sim->lobbyPlayers[2].teamNumber = 2;
    return sim;
}

/* The same three, in a running round. */
static ServerSim *veRunningSim(void) {
    ServerSim *sim = veLobbySim();
    if (sim == NULL) return NULL;
    serverSimStartGame(sim);
    return sim;
}

static ScnOpResult veSet(ServerSim *sim, bool on) {
    ScenarioOp op;
    memset(&op, 0, sizeof(op));
    op.type                  = SCN_OP_SET_VOICE_EVERYONE;
    op.u.setVoiceEveryone.on = on;
    return serverSimApplyScenarioOp(sim, &op, NULL);
}

/* The round's sides as the fixture means them, checked before a case leans
 * on them. */
static int veSidesAreSet(ServerSim *sim) {
    UT_ASSERT_MSG(serverSimGetState(sim) == serverStateRunning,
                  "the round is not running (state %d)",
                  (int)serverSimGetState(sim));
    UT_ASSERT_MSG(serverSimIsAllied(sim, 0, 1),
                  "seats 0 and 1 share team 1 but are not allies");
    UT_ASSERT_MSG(!serverSimIsAllied(sim, 0, 2),
                  "seats 0 and 2 are on different teams but are allies");
    return 0;
}

int run_scenario_voice_everyone_default(void) {
    ServerSim *sim = veLobbySim();
    UT_ASSERT(sim != NULL);

    /* The lobby: everyone hears everyone, as it always has. */
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 2),
                  "the lobby kept seat 2's voice from seat 0");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 2, 0),
                  "the lobby kept seat 0's voice from seat 2");

    /* The round: allies only. */
    serverSimStartGame(sim);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 1),
                  "allies 0 and 1 did not hear each other");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 1, 0),
                  "allies 1 and 0 did not hear each other");
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 0, 2),
                  "seat 0 heard seat 2, who is not an ally");
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 2, 1),
                  "seat 2 heard seat 1, who is not an ally");

    /* Out of range and no sim are no. */
    UT_ASSERT(!serverSimVoiceSidesAllow(sim, MAX_TANKS, 0));
    UT_ASSERT(!serverSimVoiceSidesAllow(sim, 0, MAX_TANKS));
    UT_ASSERT(!serverSimVoiceSidesAllow(NULL, 0, 1));
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(NULL));

    serverSimDestroy(sim);
    return 0;
}

int run_scenario_voice_everyone_on(void) {
    ServerSim  *sim = veRunningSim();
    ScnOpResult r;

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    r = veSet(sim, true);
    UT_ASSERT_MSG(r == SCN_OP_OK, "on answered %d", (int)r);
    UT_ASSERT(serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 2),
                  "with voice to everyone on, seat 0 did not hear seat 2");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 2, 1),
                  "with voice to everyone on, seat 2 did not hear seat 1");
    UT_ASSERT_MSG(serverSimVoiceSidesAllow(sim, 0, 1),
                  "with voice to everyone on, allies stopped hearing each "
                  "other");
    /* The sides themselves are untouched: this is voice, not alliance. */
    UT_ASSERT(!serverSimIsAllied(sim, 0, 2));

    r = veSet(sim, false);
    UT_ASSERT_MSG(r == SCN_OP_OK, "off answered %d", (int)r);
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 0, 2),
                  "after off, seat 0 still heard seat 2");
    UT_ASSERT(serverSimVoiceSidesAllow(sim, 0, 1));

    serverSimDestroy(sim);
    return 0;
}

int run_scenario_voice_everyone_resets(void) {
    ServerSim *sim = veRunningSim();

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    /* The round ends and the server goes back to the lobby. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimReturnToLobby(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateLobby);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone outlived the return to the lobby");

    /* Set in the lobby, then a new round starts. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimStartGame(sim);
    UT_ASSERT(serverSimGetState(sim) == serverStateRunning);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone set in the lobby reached the round");
    UT_ASSERT_MSG(!serverSimVoiceSidesAllow(sim, 0, 2),
                  "the new round sent seat 2's voice to seat 0");

    /* Set in the round, then the scenario is taken off the server. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimSetScenarioIdentity(sim, lobbyScenarioNone, NULL, NULL, NULL,
                                 false, false, false, false, false);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone outlived the scenario's detach");

    /* Set in the round, then a new round starts straight from it. */
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    serverSimStartGame(sim);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "voice to everyone outlived the start of the next round");

    serverSimDestroy(sim);
    return 0;
}

int run_scenario_voice_everyone_voice_off(void) {
    ServerSim  *sim = veRunningSim();
    ScnOpResult r;

    UT_ASSERT(sim != NULL);
    if (veSidesAreSet(sim) != 0) {
        serverSimDestroy(sim);
        return 1;
    }

    serverSimSetVoiceMode(sim, serverVoiceOff);
    r = veSet(sim, true);
    UT_ASSERT_MSG(r == SCN_OP_WRONG_STATE,
                  "on with server voice off answered %d, expected %d",
                  (int)r, (int)SCN_OP_WRONG_STATE);
    UT_ASSERT(!serverSimGetScenarioVoiceEveryone(sim));
    UT_ASSERT(!serverSimVoiceSidesAllow(sim, 0, 2));

    r = veSet(sim, false);
    UT_ASSERT_MSG(r == SCN_OP_OK, "off with server voice off answered %d",
                  (int)r);

    /* Turned on while voice was on, and then the server's voice goes off:
       the getter answers false, because nobody hears anybody. */
    serverSimSetVoiceMode(sim, serverVoiceOn);
    UT_ASSERT(veSet(sim, true) == SCN_OP_OK);
    UT_ASSERT(serverSimGetScenarioVoiceEveryone(sim));
    serverSimSetVoiceMode(sim, serverVoiceOff);
    UT_ASSERT_MSG(!serverSimGetScenarioVoiceEveryone(sim),
                  "the getter said voice goes to everyone on a server with "
                  "voice off");

    serverSimDestroy(sim);
    return 0;
}
