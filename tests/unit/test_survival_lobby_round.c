/*
 * Survival, started the way a host starts it: through the lobby.
 *
 * The shipped map and the script beside it, not a fixture. The lobby seats
 * the scenario's own template — ten HELD seats for the horde — and it seats
 * them from the first free slot upward, so the horde takes the LOW slots and
 * the defenders the host adds land above them. That is the opposite of a
 * headless run, where the -bots are seated first, and it is the whole of what
 * this case is here to hold: a script that reads a side off a slot number
 * passes headless and deals the keep to the wrong people in a lobby.
 *
 * run_survival_lobby_round
 *      — one human and two defender bots on team 1, started through
 *        serverSimLobbyCheckAllReady, then ticked past the grace:
 *          * the horde really does hold the low seats, so the case is
 *            testing what it says it is
 *          * every centre pill belongs to a seat on the defenders' team
 *          * each defender BOT's pill is built to full armour and stands on
 *            a road square
 *          * the human's pill is still dead on the ground, for them to place
 *          * the round opens with the "dig in" line
 *          * the first wave's line, and its first attacker, land 30 s in
 *            and not before
 *
 * Reads the ServerSim struct directly; the unittests profile permits it.
 */

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <SDL3/SDL.h>

#include "global.h"
#include "server_sim.h"
#include "server_sim_internal.h"   /* state, lobbyPlayers, the pill list */
#include "server_sim_lifecycle.h"  /* StartGame / the ready check / bot config */
#include "server_sim_scenario.h"   /* serverSimScenarioSeatLobby */
#include "control_event.h"
#include "scenario_host.h"
#include "game_sim.h"
#include "test_harness.h"

/* Where the shipped maps and their scripts live. */
#ifndef WB_DATA_MAPS_DIR
#define WB_DATA_MAPS_DIR "data/maps"
#endif

#define SLR_DEF_TEAM   1
#define SLR_WAVE_TEAM  2
#define SLR_CENTER_PILLS 6
#define SLR_BUILT_ARMOUR 15
#define SLR_TERRAIN_ROAD ROAD       /* global.h: 4 */
#define SLR_GRACE_TICKS  3000      /* 30 s at the 100-a-second tick */

/* Every line the whole game was told, with the tick it was told on. The
   script's own announcements come down this channel (game.message is server
   text), so this is where the round's clock is read off. */
typedef struct {
    int      count;
    uint32_t digInTick;
    uint32_t waveTick;
} SlrText;

static ServerSim *slrSim  = NULL;
static SlrText   *slrSeen = NULL;

#define SLR_NO_TICK 0xFFFFFFFFu

static void slrTextCb(void *ctx, const ControlEvent *evt) {
    (void)ctx;
    if (slrSeen == NULL || evt->type != CTRL_SERVER_TEXT) return;
    slrSeen->count++;
    if (strstr(evt->u.serverText.text, "SURVIVAL: dig in!") != NULL &&
        slrSeen->digInTick == SLR_NO_TICK) {
        slrSeen->digInTick = slrSim != NULL ? slrSim->tick : 0;
    }
    if (strstr(evt->u.serverText.text, "attackers inbound!") != NULL &&
        slrSeen->waveTick == SLR_NO_TICK) {
        slrSeen->waveTick = slrSim != NULL ? slrSim->tick : 0;
    }
}

/* The team a seat is on, or 0 for a seat nobody is in. */
static BYTE slrTeam(ServerSim *sim, int slot) {
    return sim->playerConnected[slot] ? sim->lobbyPlayers[slot].teamNumber : 0;
}

int run_survival_lobby_round(void) {
    char          mapPath[512];
    char          err[512];
    ServerSim    *sim;
    ScenarioHost *host;
    SlrText       seen;
    int           i;
    int           hordeLow  = 0;    /* horde seats below the first defender */
    int           humanSlot = 0;
    int           botSlot[2];
    int           built     = 0;
    BYTE          fieldedAtStart;

    memset(&seen, 0, sizeof(seen));
    seen.digInTick = SLR_NO_TICK;
    seen.waveTick  = SLR_NO_TICK;

    snprintf(mapPath, sizeof(mapPath), "%s/Survival.map", WB_DATA_MAPS_DIR);
    sim = serverSimCreate(mapPath, gameOpen, false, 0, -1);
    if (sim == NULL) UT_FAIL("the map is missing or will not load: %s", mapPath);
    slrSim = sim;
    serverSimSetLobbyEnabled(sim, true);
    /* A server that runs brains at all: the scenario's spawn arm refuses on
       a server with no bot AI, exactly as the lobby's Add Bot does, and the
       waves reach the field through that arm. The brain the template names
       ships beside the binary, so this is the path a host really runs. */
    serverSimSetBotAiType(sim, aiFull);
    serverSimSetBotBrainPath(sim, "brains/GoalHunter_1.7/init.lua");

    err[0] = '\0';
    host = scenarioHostAttach(sim, mapPath, err, sizeof(err));
    if (host == NULL) {
        serverSimDestroy(sim);
        UT_FAIL("the script beside %s was refused: %s", mapPath, err);
    }

    /* The host, in slot 0. gamefront puts him there before it seats the
       template, because the lobby's host role lives in that slot. */
    serverSimAddPlayer(sim, (BYTE)humanSlot, "Host", false);
    sim->lobbyPlayers[humanSlot].ready      = true;
    sim->lobbyPlayers[humanSlot].teamNumber = SLR_DEF_TEAM;

    /* The lobby the scenario asks for. */
    serverSimScenarioSeatLobby(sim);

    /* Two defender bots, taking the seats a host's Add Bot would take. */
    ut_brain_stub_arm(true);
    for (i = 0; i < 2; i++) {
        int slot = serverSimFindFreeSlot(sim, true);
        if (slot < 0) UT_FAIL("no free slot for defender bot %d", i);
        if (!serverSimCreateBot(sim, (BYTE)slot,
                                "brains/GoalHunter_1.7/init.lua", "Def",
                                serverSimGetBotAiType(sim), gameOpen,
                                sim->sim.hiddenMines, SLR_DEF_TEAM, NULL)) {
            UT_FAIL("defender bot %d was refused at slot %d", i, slot);
        }
        sim->lobbyPlayers[slot].ready = true;
        botSlot[i] = slot;
    }

    /* The premise. Every horde seat below the lowest defender bot is a slot
       the old "defenders are 0..5" rule would have read as a defender. */
    for (i = 0; i < botSlot[0]; i++) {
        if (slrTeam(sim, i) == SLR_WAVE_TEAM) hordeLow++;
    }
    UT_ASSERT_MSG(hordeLow >= SLR_CENTER_PILLS,
                  "only %d horde seat(s) sit below the defenders; this case "
                  "is not exercising the lobby's seating order", hordeLow);
    UT_ASSERT_MSG(botSlot[0] > 5 && botSlot[1] > 5,
                  "the defender bots landed in slots %d and %d, inside the "
                  "range the old rule read as the defenders",
                  botSlot[0], botSlot[1]);

    (void)serverSimRegisterSubscriber(sim, slrTextCb, NULL);
    slrSeen = &seen;

    /* Start it the way the lobby does. */
    serverSimLobbyCheckAllReady(sim);
    while (sim->state == serverStateCountdown) serverSimTick(sim);
    UT_ASSERT_MSG(sim->state == serverStateRunning,
                  "the round did not start: state %d", (int)sim->state);

    /* One tick, which is the round's first running one: on_start runs there
       and so does the first on_tick. */
    serverSimTick(sim);

    UT_ASSERT_MSG(scenarioHostLastError(host)[0] == '\0',
                  "the round's setup complained: %s",
                  scenarioHostLastError(host));

    /* (a) Every centre pill belongs to a seat on the defenders' team. */
    for (i = 1; i <= SLR_CENTER_PILLS; i++) {
        const pillbox *p = &(*sim->sim.pb).item[i - 1];
        UT_ASSERT_MSG(p->owner < MAX_TANKS,
                      "centre pill %d is nobody's (owner %d)", i, (int)p->owner);
        UT_ASSERT_MSG(slrTeam(sim, p->owner) == SLR_DEF_TEAM,
                      "centre pill %d belongs to slot %d, which is on team %d "
                      "and not the defenders'", i, (int)p->owner,
                      (int)slrTeam(sim, p->owner));
    }

    /* (b) Each defender BOT has exactly one pill dug in: at full armour and
           standing on the ring road, which is where the pre-build slides it.
           Exactly one, because with three defenders sharing six positions a
           bot is dealt two pills and is only meant to be manning one. */
    for (i = 0; i < 2; i++) {
        int n;
        int mineBuilt = -1;
        int mineCount = 0;
        for (n = 0; n < SLR_CENTER_PILLS; n++) {
            const pillbox *p = &(*sim->sim.pb).item[n];
            if (p->owner != (BYTE)botSlot[i]) continue;
            if (p->armour == 0) continue;
            mineCount++;
            if (mineBuilt < 0) mineBuilt = n;
        }
        UT_ASSERT_MSG(mineCount == 1,
                      "the defender bot in slot %d has %d pill(s) dug in, "
                      "expected 1", botSlot[i], mineCount);
        {
            const pillbox *p = &(*sim->sim.pb).item[mineBuilt];
            BYTE tile = (*sim->sim.mp).mapItem[p->x][p->y];
            UT_ASSERT_MSG(p->armour == SLR_BUILT_ARMOUR,
                          "pill %d, the bot in slot %d's, has armour %d and "
                          "is not at full strength", mineBuilt + 1, botSlot[i],
                          (int)p->armour);
            UT_ASSERT_MSG(tile == SLR_TERRAIN_ROAD,
                          "pill %d, the bot in slot %d's, stands on terrain "
                          "%d and not on the ring road", mineBuilt + 1,
                          botSlot[i], (int)tile);
            built++;
        }
    }
    UT_ASSERT(built == 2);

    /* (c) Every pill the human was dealt is still dead on the ground: a
           person carries it and builds it where they choose, and the
           pre-build skips them on purpose. */
    {
        int n;
        int mine = 0;
        for (n = 0; n < SLR_CENTER_PILLS; n++) {
            const pillbox *p = &(*sim->sim.pb).item[n];
            if (p->owner != (BYTE)humanSlot) continue;
            mine++;
            UT_ASSERT_MSG(p->armour == 0,
                          "pill %d, the human's, was built for them "
                          "(armour %d)", n + 1, (int)p->armour);
        }
        UT_ASSERT_MSG(mine > 0, "the human in slot %d was dealt no centre pill",
                      humanSlot);
    }

    /* (e) The round opened with the line that tells the players to dig in. */
    UT_ASSERT_MSG(seen.digInTick != SLR_NO_TICK,
                  "the round opened without the \"dig in\" line");

    /* (d) The grace is a real 30 seconds: no attacker on the field and no
           wave line before it, and both just after. Ticked in two stretches
           so a wave that landed early is caught where it happened rather
           than at the end. */
    fieldedAtStart = serverSimGetNumFielded(sim);
    while (sim->tick < SLR_GRACE_TICKS) {
        serverSimTick(sim);
        UT_ASSERT_MSG(seen.waveTick == SLR_NO_TICK,
                      "wave 1 was called on tick %u, inside the 30 s grace",
                      (unsigned)seen.waveTick);
        UT_ASSERT_MSG(serverSimGetNumFielded(sim) == fieldedAtStart,
                      "an attacker took the field on tick %u, inside the "
                      "30 s grace", (unsigned)sim->tick);
        UT_ASSERT_MSG(sim->state == serverStateRunning,
                      "the round ended on tick %u, inside the grace",
                      (unsigned)sim->tick);
    }
    while (sim->tick < SLR_GRACE_TICKS + 400 &&
           (seen.waveTick == SLR_NO_TICK ||
            serverSimGetNumFielded(sim) == fieldedAtStart)) {
        serverSimTick(sim);
    }
    UT_ASSERT_MSG(seen.waveTick != SLR_NO_TICK,
                  "no wave was called by tick %u, 4 s past the grace",
                  (unsigned)sim->tick);
    UT_ASSERT_MSG(seen.waveTick >= SLR_GRACE_TICKS,
                  "wave 1 was called on tick %u, before the grace was out",
                  (unsigned)seen.waveTick);
    UT_ASSERT_MSG(serverSimGetNumFielded(sim) > fieldedAtStart,
                  "no attacker had taken the field by tick %u, 4 s past the "
                  "grace", (unsigned)sim->tick);

    slrSeen = NULL;
    slrSim  = NULL;
    serverSimDestroy(sim);
    return 0;
}
